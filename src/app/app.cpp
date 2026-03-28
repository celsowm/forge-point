#include "app/app.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"
#include "ui/main_layout.hpp"
#include "ui/welcome_screen.hpp"

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace forge {

static constexpr const char* kAppTitle = "Forge-Point";
static constexpr const char* kTagline = "Local-first GGUF cockpit for llama.cpp";

App::App()
    : screen_(ScreenInteractive::FullscreenAlternateScreen()),
      project_models_dir_(fs::current_path() / "models"),
      runtime_dir_(fs::current_path() / "runtime" / "llama.cpp"),
      scanner_(project_models_dir_),
      hf_client_(http_client_),
      server_manager_(process_, http_client_),
      downloader_(http_client_) {
  fs::create_directories(project_models_dir_);
  fs::create_directories(runtime_dir_);

  search_query_ = "Qwen GGUF";
  host_input_ = "127.0.0.1";
  port_input_ = "8080";
  extra_args_input_ = "-c 4096";

  source_toggle_entries_ = {"Local GGUF", "HF Repo/File"};
  repo_entries_ = {"(none)"};
  file_entries_ = {"(none)"};

  RegisterCommands();
  RefreshBinaryStatus();
  RefreshLocalModels();
  BuildUi();
  UpdateCommandSuggestions();

  logger_.Log("Forge-Point booted.");
  logger_.Log("Drop a prebuilt llama.cpp release inside runtime/llama.cpp/ and Forge-Point will auto-detect llama-server.");
}

void App::Run() { screen_.Loop(root_); }

// ─── Threading ──────────────────────────────────────────────────────────────

void App::PostToUi(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(ui_queue_mutex_);
    ui_queue_.push_back(std::move(fn));
  }
  screen_.PostEvent(Event::Custom);
}

void App::DrainUiQueue() {
  std::deque<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(ui_queue_mutex_);
    batch.swap(ui_queue_);
  }
  for (auto& fn : batch) fn();
}

void App::CleanupFinishedWorkers() {
  workers_.erase(
      std::remove_if(workers_.begin(), workers_.end(),
                     [](const std::jthread& t) { return !t.joinable(); }),
      workers_.end());
}

void App::SpawnWorker(std::function<void()> fn) {
  CleanupFinishedWorkers();
  workers_.emplace_back([f = std::move(fn)] { f(); });
}

// ─── Business logic ─────────────────────────────────────────────────────────

void App::RefreshBinaryStatus() {
  gpu_info_ = gpu_detector_.Detect();
  binary_path_ = LlamaServerManager::FindBundledBinary(runtime_dir_);
  if (binary_path_) {
    binary_status_ = "llama-server: " + binary_path_->string();
  } else {
    std::string gpu_name = gpu_info_.name.empty() ? "CPU" : gpu_info_.name;
    binary_status_ = "No llama-server. GPU: " + gpu_name;
  }
}

void App::RefreshLocalModels() {
  local_models_ = scanner_.Scan();
  local_model_entries_.clear();
  for (const auto& model : local_models_) {
    local_model_entries_.push_back(model.name + " [" + util::HumanBytes(model.size) + "]");
  }
  if (local_model_entries_.empty()) {
    local_model_entries_.push_back("(none)");
  }
  if (local_selected_ >= static_cast<int>(local_model_entries_.size())) {
    local_selected_ = std::max(0, static_cast<int>(local_model_entries_.size()) - 1);
  }
  logger_.Log("Local scan complete. Found " + std::to_string(local_models_.size()) + " GGUF file(s).");
  screen_.PostEvent(Event::Custom);
}

void App::DownloadLlamaCpp() {
  std::string error;
  logger_.Log("Fetching latest llama.cpp release...");
  screen_.PostEvent(Event::Custom);
  auto release = downloader_.GetLatestRelease(error);
  if (!release) {
    logger_.Log("Failed to fetch release: " + error);
    screen_.PostEvent(Event::Custom);
    return;
  }
  logger_.Log("Release: " + release->tag);
  auto asset = downloader_.FindAssetForPlatform(*release, gpu_info_);
  if (!asset) {
    logger_.Log("No compatible binary found for this platform.");
    screen_.PostEvent(Event::Custom);
    return;
  }
  logger_.Log("Downloading: " + *asset);
  auto url = downloader_.GetDownloadUrl(release->tag, *asset);
  size_t tid = transfers_.Add("llama.cpp: " + *asset);
  auto dl_progress = [this, tid](uint64_t dl, uint64_t total) {
    transfers_.Update(tid, dl, total);
    screen_.PostEvent(Event::Custom);
  };
  bool ok = downloader_.DownloadAndExtract(url, runtime_dir_, error,
                                            [this](const std::string& msg) {
                                              logger_.Log(msg);
                                              screen_.PostEvent(Event::Custom);
                                            },
                                            dl_progress);
  if (ok) {
    transfers_.Finish(tid);
    logger_.Log("Download complete!");
    PostToUi([this] { RefreshBinaryStatus(); });
  } else {
    transfers_.Finish(tid, true);
    logger_.Log("Download failed: " + error);
  }
  screen_.PostEvent(Event::Custom);
}

void App::SearchRepos() {
  std::string error;
  hf_repos_ = hf_client_.SearchRepos(search_query_, error);
  repo_entries_.clear();
  repo_files_.clear();
  file_entries_.clear();
  repo_selected_ = 0;
  file_selected_ = 0;
  active_repo_sha_.clear();

  if (!error.empty()) {
    logger_.Log("HF search failed: " + error);
    screen_.PostEvent(Event::Custom);
    return;
  }

  for (const auto& repo : hf_repos_) {
    repo_entries_.push_back(repo.id + " (↓" + std::to_string(repo.downloads) +
                            ", ♥" + std::to_string(repo.likes) + ")");
  }
  if (repo_entries_.empty()) {
    repo_entries_.push_back("(none)");
  }
  logger_.Log("HF search returned " + std::to_string(hf_repos_.size()) + " repos.");
  screen_.PostEvent(Event::Custom);
}

void App::LoadRepoFiles() {
  if (hf_repos_.empty() || repo_selected_ < 0 ||
      repo_selected_ >= static_cast<int>(hf_repos_.size())) {
    logger_.Log("Choose a repo first.");
    screen_.PostEvent(Event::Custom);
    return;
  }
  std::string error;
  active_repo_sha_ = hf_repos_[repo_selected_].sha;
  repo_files_ = hf_client_.ListGgufFiles(hf_repos_[repo_selected_].id,
                                          active_repo_sha_, error);
  file_entries_.clear();
  file_selected_ = 0;

  if (!error.empty()) {
    logger_.Log("HF file listing failed: " + error);
    screen_.PostEvent(Event::Custom);
    return;
  }

  for (const auto& file : repo_files_) {
    std::string label = file.filename;
    if (file.size > 0) label += " [" + util::HumanBytes(file.size) + "]";
    file_entries_.push_back(label);
  }
  if (file_entries_.empty()) {
    file_entries_.push_back("(none)");
  }
  logger_.Log("Loaded " + std::to_string(repo_files_.size()) + " GGUF file(s) from the selected repo.");
  screen_.PostEvent(Event::Custom);
}

void App::DownloadSelectedFile() {
  if (hf_repos_.empty() || repo_files_.empty() ||
      repo_selected_ >= static_cast<int>(hf_repos_.size()) ||
      file_selected_ >= static_cast<int>(repo_files_.size())) {
    logger_.Log("Choose a repo and GGUF file before downloading.");
    screen_.PostEvent(Event::Custom);
    return;
  }

  const auto repo = hf_repos_[repo_selected_];
  const auto file = repo_files_[file_selected_];
  const fs::path target_dir = project_models_dir_ /
                               util::ReplaceAll(repo.id, "/", "__");
  const fs::path target_file = target_dir / fs::path(file.filename).filename();

  guard_.Request("Download GGUF",
                 "Download " + file.filename + " into\n" + target_file.string(),
                 true,
                 [this, repo, file, target_file] {
                   logger_.Log("Downloading " + file.filename + "...");
                   size_t tid = transfers_.Add(file.filename);
                   SpawnWorker([this, url = file.download_url, target_file, tid] {
                     std::string error;
                     auto progress = [this, tid](uint64_t dl, uint64_t total) {
                       transfers_.Update(tid, dl, total);
                       screen_.PostEvent(Event::Custom);
                     };
                     if (hf_client_.DownloadFile(url, target_file, error, progress)) {
                       transfers_.Finish(tid);
                       logger_.Log("Download finished: " + target_file.string());
                       PostToUi([this] { RefreshLocalModels(); });
                     } else {
                       transfers_.Finish(tid, true);
                       logger_.Log("Download failed: " + error);
                     }
                     screen_.PostEvent(Event::Custom);
                   });
                   screen_.PostEvent(Event::Custom);
                 });
  screen_.PostEvent(Event::Custom);
}

void App::StartServer() {
  RefreshBinaryStatus();
  if (!binary_path_) {
    logger_.Log("Cannot start server: llama-server not found.");
    screen_.PostEvent(Event::Custom);
    return;
  }

  guard_.Request("Start llama-server", DescribeServerLaunch(), true, [this] {
    auto log_fn = [this](const std::string& chunk) {
      logger_.Log(chunk);
      screen_.PostEvent(Event::Custom);
    };
    bool ok = false;
    if (source_mode_ == 0) {
      if (local_models_.empty() ||
          local_selected_ >= static_cast<int>(local_models_.size())) {
        logger_.Log("Choose a local GGUF first.");
        screen_.PostEvent(Event::Custom);
        return;
      }
      ok = server_manager_.StartWithLocalModel(*binary_path_,
                                                local_models_[local_selected_].path,
                                                host_input_, port_input_,
                                                extra_args_input_, log_fn,
                                                last_command_preview_);
    } else {
      if (hf_repos_.empty() || repo_files_.empty() ||
          repo_selected_ >= static_cast<int>(hf_repos_.size()) ||
          file_selected_ >= static_cast<int>(repo_files_.size())) {
        logger_.Log("Choose a repo and GGUF file first.");
        screen_.PostEvent(Event::Custom);
        return;
      }
      ok = server_manager_.StartWithHfRepo(*binary_path_,
                                            hf_repos_[repo_selected_].id,
                                            repo_files_[file_selected_].filename,
                                            host_input_, port_input_,
                                            extra_args_input_, log_fn,
                                            last_command_preview_);
    }
    if (ok) {
      logger_.Log("Server starting...");
      logger_.Log(last_command_preview_);
    } else {
      logger_.Log("Failed to launch llama-server.");
    }
    screen_.PostEvent(Event::Custom);
  });
  screen_.PostEvent(Event::Custom);
}

std::string App::DescribeServerLaunch() const {
  std::ostringstream oss;
  oss << "Host: " << host_input_ << '\n';
  oss << "Port: " << port_input_ << '\n';
  oss << "Args: " << extra_args_input_ << '\n';
  if (source_mode_ == 0 && !local_models_.empty() &&
      local_selected_ < static_cast<int>(local_models_.size())) {
    oss << "Mode: Local GGUF\n";
    oss << "Model: " << local_models_[local_selected_].path.string();
  } else if (source_mode_ == 1 && !hf_repos_.empty() && !repo_files_.empty() &&
             repo_selected_ < static_cast<int>(hf_repos_.size()) &&
             file_selected_ < static_cast<int>(repo_files_.size())) {
    oss << "Mode: HF Repo/File\n";
    oss << "Repo: " << hf_repos_[repo_selected_].id << '\n';
    oss << "File: " << repo_files_[file_selected_].filename;
  } else {
    oss << "Mode: (target not fully selected)";
  }
  return oss.str();
}

void App::StopServer() {
  guard_.Request("Stop llama-server",
                 "Stop the managed llama-server process?",
                 true,
                 [this] {
                   if (!server_manager_.Running()) {
                     logger_.Log("Server is not running.");
                     screen_.PostEvent(Event::Custom);
                     return;
                   }
                   server_manager_.Stop();
                   logger_.Log("Server stopped.");
                   screen_.PostEvent(Event::Custom);
                 });
  screen_.PostEvent(Event::Custom);
}

void App::CheckHealth() {
  health_status_ = server_manager_.Health(host_input_, port_input_);
  logger_.Log("Health: " + health_status_);
  screen_.PostEvent(Event::Custom);
}

void App::ShowWelcome() {
  show_welcome_ = true;
  logger_.Log("Welcome screen opened.");
  screen_.PostEvent(Event::Custom);
}

void App::HideWelcome() {
  if (show_welcome_) {
    show_welcome_ = false;
    logger_.Log("Welcome screen dismissed.");
    screen_.PostEvent(Event::Custom);
  }
}

void App::CyclePanel(int delta) {
  const int count = static_cast<int>(Panel::COUNT);
  active_panel_ = static_cast<Panel>((static_cast<int>(active_panel_) + delta + count) % count);
  FocusActivePanel();
}

void App::FocusActivePanel() {
  switch (active_panel_) {
    case Panel::Local:   local_menu_->TakeFocus(); break;
    case Panel::Hub:     search_input_->TakeFocus(); break;
    case Panel::Server:  host_input_component_->TakeFocus(); break;
    case Panel::Command: command_input_component_->TakeFocus(); break;
    default: break;
  }
}

// ─── Commands ────────────────────────────────────────────────────────────────

void App::RegisterCommands() {
  command_handler_.Register("/help", [this](const auto&) {
    logger_.Log("Slash commands:");
    for (const auto& item : command_registry_.GetAll()) {
      logger_.Log("  " + item.usage + " — " + item.description);
    }
  });

  command_handler_.Register("/search", [this](const auto& args) {
    if (!args.empty()) {
      search_query_ = args[0];
      for (size_t i = 1; i < args.size(); ++i) search_query_ += " " + args[i];
    }
    SearchRepos();
  });

  command_handler_.Register("/files", [this](const auto&) { LoadRepoFiles(); });
  command_handler_.Register("/download", [this](const auto&) { DownloadSelectedFile(); });
  command_handler_.Register("/start", [this](const auto&) { StartServer(); });
  command_handler_.Register("/stop", [this](const auto&) { StopServer(); });
  command_handler_.Register("/health", [this](const auto&) { CheckHealth(); });
  command_handler_.Register("/rescan", [this](const auto&) { RefreshLocalModels(); });

  command_handler_.Register("/refresh-binary", [this](const auto&) {
    RefreshBinaryStatus();
    logger_.Log(binary_status_);
    screen_.PostEvent(Event::Custom);
  });

  command_handler_.Register("/download-binary", [this](const auto&) {
    if (binary_path_) {
      logger_.Log("llama-server already present. Use /refresh-binary to rescan.");
    } else {
      logger_.Log("Starting download in background...");
      SpawnWorker([this] { DownloadLlamaCpp(); });
    }
    screen_.PostEvent(Event::Custom);
  });

  command_handler_.Register("/welcome", [this](const auto&) { ShowWelcome(); });

  command_handler_.Register("/yolo", [this](const auto& args) {
    if (!args.empty()) {
      const std::string value = util::ToLower(args[0]);
      if (value == "on") {
        guard_.SetYolo(true);
      } else if (value == "off") {
        guard_.SetYolo(false);
      } else {
        guard_.ToggleYolo();
      }
    } else {
      guard_.ToggleYolo();
    }
    logger_.Log(std::string("YOLO mode ") + (guard_.YoloMode() ? "enabled." : "disabled."));
    screen_.PostEvent(Event::Custom);
  });

  command_handler_.Register("/focus", [this](const auto& args) {
    const std::string target = args.empty() ? "" : util::ToLower(args[0]);
    if (target == "search" || target == "hub") {
      active_panel_ = Panel::Hub;
    } else if (target == "models" || target == "local") {
      active_panel_ = Panel::Local;
    } else if (target == "server") {
      active_panel_ = Panel::Server;
    } else {
      active_panel_ = Panel::Command;
    }
    FocusActivePanel();
    logger_.Log("Focus moved to " + (target.empty() ? std::string("command") : target) + ".");
    screen_.PostEvent(Event::Custom);
  });
}

// ─── UI building ─────────────────────────────────────────────────────────────

void App::BuildUi() {
  search_input_ = Input(&search_query_, "Qwen GGUF / org/model");
  host_input_component_ = Input(&host_input_, "127.0.0.1");
  port_input_component_ = Input(&port_input_, "8080");
  extra_args_component_ = Input(&extra_args_input_, "-c 4096");
  command_input_component_ = Input(&command_input_, "/help, /search, /start...");

  local_menu_ = Menu(&local_model_entries_, &local_selected_);
  repo_menu_ = Menu(&repo_entries_, &repo_selected_);
  file_menu_ = Menu(&file_entries_, &file_selected_);
  source_toggle_ = Toggle(&source_toggle_entries_, &source_mode_);

  auto local_panel = Container::Vertical({local_menu_, source_toggle_});
  auto hub_panel = Container::Vertical({search_input_, repo_menu_, file_menu_});
  auto server_panel = Container::Vertical({host_input_component_, port_input_component_, extra_args_component_});

  root_container_ = Container::Vertical({
      local_panel,
      hub_panel,
      server_panel,
      command_input_component_,
  });

  root_ = Renderer(root_container_, [this] {
    DrainUiQueue();
    Element body = show_welcome_ ? BuildWelcomeScreen() : BuildMainScreen();
    if (guard_.IsShowing()) {
      body = dbox({body, BuildConfirmOverlay()});
    }
    return body;
  });

  root_ = CatchEvent(root_, [this](Event event) {
    if (show_welcome_) {
      if (event == Event::Return || event == Event::Escape || event == Event::Character(' ')) {
        HideWelcome();
        active_panel_ = Panel::Command;
        FocusActivePanel();
        return true;
      }
      return true;
    }

    if (guard_.IsShowing()) {
      if (event == Event::Return || event == Event::Character('y') || event == Event::Character('Y')) {
        guard_.Confirm();
        return true;
      }
      if (event == Event::Escape || event == Event::Character('n') || event == Event::Character('N')) {
        guard_.Cancel();
        logger_.Log("Action cancelled.");
        screen_.PostEvent(Event::Custom);
        return true;
      }
      return true;
    }

    if (event == Event::Tab) {
      if (command_input_component_->Focused() && !command_input_.empty() && command_input_.front() == '/') {
        AcceptSuggestion();
        return true;
      }
      CyclePanel(1);
      return true;
    }

    if (event == Event::TabReverse) {
      CyclePanel(-1);
      return true;
    }

    if (event == Event::Special({25})) {
      guard_.ToggleYolo();
      logger_.Log(std::string("YOLO mode ") + (guard_.YoloMode() ? "enabled." : "disabled."));
      screen_.PostEvent(Event::Custom);
      return true;
    }

    if (event == Event::Character('q') || event == Event::CtrlC) {
      if (command_input_component_->Focused() || search_input_->Focused() ||
          host_input_component_->Focused() || port_input_component_->Focused() ||
          extra_args_component_->Focused()) {
        if (event == Event::Character('q')) return false;
      }
      screen_.ExitLoopClosure()();
      return true;
    }

    if (event == Event::Character('/')) {
      OpenCommandPalette();
      return true;
    }

    if (event == Event::Escape) {
      active_panel_ = Panel::Command;
      FocusActivePanel();
      return true;
    }

    if (event == Event::Return) {
      if (command_input_component_->Focused()) {
        ExecuteCommandInput();
        return true;
      }
      if (search_input_->Focused()) {
        SearchRepos();
        return true;
      }
      if (repo_menu_->Focused()) {
        LoadRepoFiles();
        return true;
      }
      if (file_menu_->Focused()) {
        DownloadSelectedFile();
        return true;
      }
      if (local_menu_->Focused()) {
        if (!local_models_.empty() && local_selected_ >= 0 &&
            local_selected_ < static_cast<int>(local_models_.size())) {
          source_mode_ = 0;
          logger_.Log("Selected: " + local_models_[local_selected_].name);
          screen_.PostEvent(Event::Custom);
        }
        return true;
      }
    }

    if (command_input_component_->Focused()) {
      if (event == Event::ArrowDown) { MoveSuggestion(1); return true; }
      if (event == Event::ArrowUp) { MoveSuggestion(-1); return true; }
    }

    return false;
  });

  command_input_component_ = CatchEvent(command_input_component_, [this](Event event) {
    if (event.is_character()) {
      screen_.PostEvent(Event::Custom);
    }
    return false;
  });
}

void App::OpenCommandPalette() {
  if (command_input_.empty() || command_input_.front() != '/') command_input_ = "/";
  command_input_component_->TakeFocus();
  UpdateCommandSuggestions();
}

void App::MoveSuggestion(int delta) {
  UpdateCommandSuggestions();
  if (command_suggestions_.empty()) return;
  const int count = static_cast<int>(command_suggestions_.size());
  command_selected_ = (command_selected_ + delta + count) % count;
}

void App::AcceptSuggestion() {
  UpdateCommandSuggestions();
  if (command_suggestions_.empty()) return;
  const std::string replacement = command_suggestions_[command_selected_].usage;
  const auto first_space = command_input_.find(' ');
  if (first_space == std::string::npos) {
    command_input_ = replacement;
    if (replacement.find(' ') == std::string::npos) command_input_ += ' ';
  } else {
    command_input_ = replacement.substr(0, replacement.find(' ')) +
                     command_input_.substr(first_space);
  }
  UpdateCommandSuggestions();
}

void App::UpdateCommandSuggestions() {
  command_suggestions_ = command_registry_.Match(command_input_);
  if (command_selected_ >= static_cast<int>(command_suggestions_.size()))
    command_selected_ = 0;
}

void App::ExecuteCommandInput() {
  const std::string raw = util::Trim(command_input_);
  if (raw.empty()) return;
  logger_.Log(std::string("$ ") + raw);
  command_input_.clear();
  UpdateCommandSuggestions();

  const auto words = util::SplitWords(raw);
  if (words.empty()) return;

  const std::string cmd = util::ToLower(words[0]);
  std::vector<std::string> args(words.begin() + 1, words.end());

  if (!command_handler_.Execute(cmd, args)) {
    logger_.Log("Unknown command. Try /help.");
  }
  screen_.PostEvent(Event::Custom);
}

// ─── Rendering ───────────────────────────────────────────────────────────────

Element App::BuildMainScreen() {
  UpdateCommandSuggestions();

  auto logs_copy = logger_.GetLogs();
  const size_t start = logs_copy.size() > 80 ? logs_copy.size() - 80 : 0;
  std::ostringstream log_stream;
  for (size_t i = start; i < logs_copy.size(); ++i) {
    log_stream << logs_copy[i];
    if (i + 1 < logs_copy.size()) log_stream << '\n';
  }

  Elements layout;
  layout.push_back(BuildHeader());
  layout.push_back(separator());
  layout.push_back(hbox({BuildLocalPanel() | flex, BuildHubPanel() | flex, BuildServerPanel() | flex}));
  layout.push_back(separator());

  auto transfers_el = BuildTransfers();
  if (transfers_el) {
    layout.push_back(*transfers_el);
    layout.push_back(separator());
  }

  layout.push_back(window(text("Logs"), paragraph(log_stream.str().empty() ? "(no logs yet)" : log_stream.str()) | frame | size(HEIGHT, GREATER_THAN, 12)));
  layout.push_back(separator());
  layout.push_back(BuildCommandPalette());
  layout.push_back(separator());
  layout.push_back(text("q quit · / commands · Tab cycle panels · Esc focus command · Ctrl+Y yolo · Enter act") | dim);

  return vbox(std::move(layout)) | border;
}

std::optional<Element> App::BuildTransfers() {
  auto active = transfers_.GetActive();
  if (active.empty()) return std::nullopt;

  Elements rows;
  for (const auto& t : active) {
    std::string status_text;
    if (t.total > 0) {
      double pct = static_cast<double>(t.downloaded) / static_cast<double>(t.total);
      status_text = util::HumanBytes(t.downloaded) + " / " + util::HumanBytes(t.total) +
                    " (" + std::to_string(static_cast<int>(pct * 100.0)) + "%)";
      rows.push_back(hbox({
          text(t.label + " ") | bold,
          gauge(static_cast<float>(pct)) | flex,
          text(" " + status_text),
      }));
    } else {
      status_text = util::HumanBytes(t.downloaded) + " downloaded...";
      rows.push_back(hbox({
          text(t.label + " ") | bold,
          spinner(4, static_cast<int>(t.downloaded / 65536) % 4),
          text(" " + status_text) | dim,
      }));
    }
  }
  return window(text("Transfers"), vbox(std::move(rows)));
}

Element App::BuildHeader() const {
  Element yolo_chip = text(guard_.YoloMode() ? " YOLO ON " : " YOLO OFF ") |
                      bold |
                      color(guard_.YoloMode() ? Color::Black : Color::White) |
                      bgcolor(guard_.YoloMode() ? Color::YellowLight : Color::GrayDark);

  Element server_chip = text(server_manager_.Running() ? " SERVER RUNNING " : " SERVER STOPPED ") |
                        bold |
                        color(server_manager_.Running() ? Color::Black : Color::White) |
                        bgcolor(server_manager_.Running() ? Color::GreenLight : Color::RedLight);

  return hbox({
      vbox({
          text(kAppTitle) | bold | color(Color::CyanLight),
          text(kTagline) | dim,
      }) | flex,
      hbox({server_chip, text(" "), yolo_chip}),
  });
}

Element App::BuildLocalPanel() const {
  std::string details = "No local model selected.";
  if (!local_models_.empty() && local_selected_ >= 0 &&
      local_selected_ < static_cast<int>(local_models_.size())) {
    const auto& m = local_models_[local_selected_];
    details = m.path.string() + "\norigin: " + m.origin + "\nsize: " + util::HumanBytes(m.size);
  }

  bool active = (active_panel_ == Panel::Local);
  Element title = active ? (text("▸ Local GGUFs") | color(Color::CyanLight)) : text("Local GGUFs");

  return window(title,
                vbox({
                    hbox({text("HF cache: "), text(scanner_.CacheRoot().string()) | dim}) | flex,
                    separator(),
                    local_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 12),
                    separator(),
                    paragraph(details),
                    separator(),
                    text("/rescan · /download-binary · Enter select") | dim,
                }));
}

Element App::BuildHubPanel() const {
  std::string repo_detail = "No repo selected.";
  if (!hf_repos_.empty() && repo_selected_ >= 0 &&
      repo_selected_ < static_cast<int>(hf_repos_.size())) {
    const auto& repo = hf_repos_[repo_selected_];
    repo_detail = repo.id + "\ndownloads: " + std::to_string(repo.downloads) +
                  "\nlikes: " + std::to_string(repo.likes) +
                  "\nsha: " + (repo.sha.empty() ? "(unknown)" : repo.sha);
  }

  std::string file_detail = "No GGUF file selected.";
  if (!repo_files_.empty() && file_selected_ >= 0 &&
      file_selected_ < static_cast<int>(repo_files_.size())) {
    const auto& file = repo_files_[file_selected_];
    file_detail = file.filename + "\nsize: " + util::HumanBytes(file.size) + "\n" + file.download_url;
  }

  bool active = (active_panel_ == Panel::Hub);
  Element title = active ? (text("▸ Hugging Face") | color(Color::CyanLight)) : text("Hugging Face");

  return window(title,
                vbox({
                    search_input_->Render(),
                    separator(),
                    text("Repos (Enter → list files)") | bold,
                    repo_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                    separator(),
                    paragraph(repo_detail),
                    separator(),
                    text("GGUF files (Enter → download)") | bold,
                    file_menu_->Render() | frame | size(HEIGHT, LESS_THAN, 8),
                    separator(),
                    paragraph(file_detail),
                    separator(),
                    text("/search · /files · /download") | dim,
                }));
}

Element App::BuildServerPanel() const {
  std::string target = source_mode_ == 0 ? "Local GGUF" : "HF Repo/File";
  if (source_mode_ == 0 && !local_models_.empty() && local_selected_ >= 0 &&
      local_selected_ < static_cast<int>(local_models_.size())) {
    target += "\n" + local_models_[local_selected_].path.string();
  }
  if (source_mode_ == 1 && !hf_repos_.empty() && !repo_files_.empty() &&
      repo_selected_ >= 0 && repo_selected_ < static_cast<int>(hf_repos_.size()) &&
      file_selected_ >= 0 && file_selected_ < static_cast<int>(repo_files_.size())) {
    target += "\nrepo: " + hf_repos_[repo_selected_].id +
              "\nfile: " + repo_files_[file_selected_].filename;
  }

  bool active = (active_panel_ == Panel::Server);
  Element title = active ? (text("▸ Server") | color(Color::CyanLight)) : text("Server");

  return window(title,
                vbox({
                    text(binary_status_),
                    separator(),
                    source_toggle_->Render(),
                    separator(),
                    text("Target") | bold,
                    paragraph(target),
                    separator(),
                    text("Host"), host_input_component_->Render(),
                    text("Port"), port_input_component_->Render(),
                    text("Extra args"), extra_args_component_->Render(),
                    separator(),
                    text("Health: " + health_status_),
                    separator(),
                    text("/start · /stop · /health") | dim,
                }));
}

Element App::BuildCommandPalette() {
  Elements suggestion_rows;
  if (command_input_.empty()) {
    suggestion_rows.push_back(text("Type / to open commands.") | dim);
  } else if (command_suggestions_.empty()) {
    suggestion_rows.push_back(text("No matching slash commands.") | dim);
  } else {
    for (size_t i = 0; i < command_suggestions_.size() && i < 6; ++i) {
      const auto& cmd = command_suggestions_[i];
      Element row = hbox({text(cmd.usage) | bold, text("  "), text(cmd.description) | dim});
      if (static_cast<int>(i) == command_selected_) {
        row = row | bgcolor(Color::Blue) | color(Color::White);
      }
      suggestion_rows.push_back(row);
    }
  }

  return window(text("Slash command palette"),
                vbox({
                    command_input_component_->Render(),
                    separator(),
                    vbox(std::move(suggestion_rows)),
                }));
}

Element App::BuildConfirmOverlay() const {
  return vbox({
             filler(),
             hbox({
                 filler(),
                 window(text(guard_.Title()),
                        vbox({
                            paragraph(guard_.Detail()),
                            separator(),
                            text(guard_.YoloMode() ? "YOLO mode is on." : "Press Enter/Y to confirm, Esc/N to cancel.") | dim,
                        })) | size(WIDTH, LESS_THAN, 72),
                 filler(),
             }),
             filler(),
         }) |
         bgcolor(Color::Black);
}

Element App::BuildWelcomeScreen() const {
  return ui::BuildWelcomeScreen();
}

}  // namespace forge
