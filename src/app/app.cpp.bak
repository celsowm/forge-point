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

  repo_entries_ = {"(none)"};
  file_entries_ = {"(none)"};

  RegisterCommands();
  RefreshBinaryStatus();
  RefreshLocalModels();
  BuildUi();
  UpdateCommandSuggestions();

  logger_.Log("Forge-Point booted.");
  logger_.Log("Type /help for available commands.");
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
    logger_.Log("Cannot start server: llama-server not found. Try /download-binary.");
    screen_.PostEvent(Event::Custom);
    return;
  }

  std::string host = default_host_;
  std::string port = default_port_;
  std::string extra = default_extra_args_;

  // Parse args: --host, --port, --extra-args
  // These are already parsed by the command handler before calling StartServer
  // We store them temporarily
  // Actually, StartServer is called directly. Let's parse from last_command_args_.
  // Better approach: the /start handler parses args and we read them here.
  // For now we use defaults. The /start command handler will set them before calling.

  guard_.Request("Start llama-server", DescribeServerLaunch(), true, [this] {
    auto log_fn = [this](const std::string& chunk) {
      logger_.Log(chunk);
      screen_.PostEvent(Event::Custom);
    };
    bool ok = false;
    if (active_view_ == View::Hub || (!hf_repos_.empty() && !repo_files_.empty() &&
        repo_selected_ < static_cast<int>(hf_repos_.size()) &&
        file_selected_ < static_cast<int>(repo_files_.size()))) {
      ok = server_manager_.StartWithHfRepo(*binary_path_,
                                            hf_repos_[repo_selected_].id,
                                            repo_files_[file_selected_].filename,
                                            default_host_, default_port_,
                                            default_extra_args_, log_fn,
                                            last_command_preview_);
    } else {
      if (local_models_.empty() ||
          local_selected_ >= static_cast<int>(local_models_.size())) {
        logger_.Log("No model selected. Use /models to browse, /select N to pick one.");
        screen_.PostEvent(Event::Custom);
        return;
      }
      ok = server_manager_.StartWithLocalModel(*binary_path_,
                                                local_models_[local_selected_].path,
                                                default_host_, default_port_,
                                                default_extra_args_, log_fn,
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
  oss << "Host: " << default_host_ << '\n';
  oss << "Port: " << default_port_ << '\n';
  oss << "Args: " << default_extra_args_ << '\n';

  bool has_hf = !hf_repos_.empty() && !repo_files_.empty() &&
                repo_selected_ < static_cast<int>(hf_repos_.size()) &&
                file_selected_ < static_cast<int>(repo_files_.size());
  bool has_local = !local_models_.empty() &&
                   local_selected_ < static_cast<int>(local_models_.size());

  if (active_view_ == View::Hub && has_hf) {
    oss << "Mode: HF Repo/File\n";
    oss << "Repo: " << hf_repos_[repo_selected_].id << '\n';
    oss << "File: " << repo_files_[file_selected_].filename;
  } else if (has_local) {
    oss << "Mode: Local GGUF\n";
    oss << "Model: " << local_models_[local_selected_].path.string();
  } else {
    oss << "Mode: (no model selected — use /select N)";
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
  health_status_ = server_manager_.Health(default_host_, default_port_);
  logger_.Log("Health: " + health_status_);
  screen_.PostEvent(Event::Custom);
}

void App::ShowWelcome() {
  show_welcome_ = true;
  screen_.PostEvent(Event::Custom);
}

void App::HideWelcome() {
  if (show_welcome_) {
    show_welcome_ = false;
    screen_.PostEvent(Event::Custom);
  }
}

void App::SwitchView(std::string_view name) {
  if (name == "vibecode" || name == "logs") {
    active_view_ = View::Vibecode;
    logger_.Log("[view] vibecode");
  } else if (name == "models" || name == "local") {
    active_view_ = View::Models;
    logger_.Log("[view] models");
  } else if (name == "hub") {
    active_view_ = View::Hub;
    logger_.Log("[view] hub");
  } else {
    logger_.Log("Unknown view: " + std::string(name) + ". Try: vibecode, models, hub");
  }
  FocusActiveView();
  screen_.PostEvent(Event::Custom);
}

void App::FocusActiveView() {
  // In the new design, arrow keys are handled globally based on active_view_.
  // Focus stays on command_input_ for typing, but arrows work in any view.
  command_input_component_->TakeFocus();
}

// ─── Commands ────────────────────────────────────────────────────────────────

void App::RegisterCommands() {
  command_handler_.Register("/help", [this](const auto&) {
    logger_.Log("Commands:");
    for (const auto& item : command_registry_.GetAll()) {
      logger_.Log("  " + item.usage + "  " + item.description);
    }
  });

  command_handler_.Register("/vibecode", [this](const auto&) { SwitchView("vibecode"); });
  command_handler_.Register("/logs", [this](const auto&) { SwitchView("vibecode"); });

  command_handler_.Register("/models", [this](const auto&) {
    RefreshLocalModels();
    SwitchView("models");
  });

  command_handler_.Register("/hub", [this](const auto& args) {
    if (!args.empty()) {
      search_query_ = args[0];
      for (size_t i = 1; i < args.size(); ++i) search_query_ += " " + args[i];
      SearchRepos();
    }
    SwitchView("hub");
  });

  command_handler_.Register("/search", [this](const auto& args) {
    if (!args.empty()) {
      search_query_ = args[0];
      for (size_t i = 1; i < args.size(); ++i) search_query_ += " " + args[i];
    }
    SearchRepos();
    if (active_view_ != View::Hub) SwitchView("hub");
  });

  command_handler_.Register("/files", [this](const auto&) { LoadRepoFiles(); });
  command_handler_.Register("/download", [this](const auto&) { DownloadSelectedFile(); });

  command_handler_.Register("/select", [this](const auto& args) {
    if (args.empty()) {
      logger_.Log("Usage: /select <index>  (0-based)");
      screen_.PostEvent(Event::Custom);
      return;
    }
    int idx = 0;
    try { idx = std::stoi(args[0]); } catch (...) {
      logger_.Log("Invalid index: " + args[0]);
      screen_.PostEvent(Event::Custom);
      return;
    }
    if (active_view_ == View::Models || active_view_ == View::Vibecode) {
      if (idx >= 0 && idx < static_cast<int>(local_models_.size())) {
        local_selected_ = idx;
        logger_.Log("Selected model: " + local_models_[idx].name);
      } else {
        logger_.Log("Model index out of range. Use /models to see the list.");
      }
    } else if (active_view_ == View::Hub) {
      if (!repo_entries_.empty() && repo_entries_[0] != "(none)") {
        if (idx >= 0 && idx < static_cast<int>(hf_repos_.size())) {
          repo_selected_ = idx;
          logger_.Log("Selected repo: " + hf_repos_[idx].id);
        } else {
          logger_.Log("Repo index out of range.");
        }
      }
    }
    screen_.PostEvent(Event::Custom);
  });

  command_handler_.Register("/start", [this](const auto& args) {
    // Parse --host, --port, --extra-args from args
    std::string host = default_host_;
    std::string port = default_port_;
    std::string extra = default_extra_args_;

    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "--host" && i + 1 < args.size()) {
        host = args[++i];
      } else if (args[i] == "--port" && i + 1 < args.size()) {
        port = args[++i];
      } else if (args[i] == "--extra-args" && i + 1 < args.size()) {
        extra = args[++i];
      }
    }
    default_host_ = host;
    default_port_ = port;
    default_extra_args_ = extra;
    StartServer();
  });

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

  command_handler_.Register("/quit", [this](const auto&) {
    screen_.ExitLoopClosure()();
  });
}

// ─── UI building ─────────────────────────────────────────────────────────────

void App::BuildUi() {
  command_input_component_ = Input(&command_input_, "/help, /search, /start...");

  local_menu_ = Menu(&local_model_entries_, &local_selected_);
  repo_menu_ = Menu(&repo_entries_, &repo_selected_);
  file_menu_ = Menu(&file_entries_, &file_selected_);

  root_container_ = Container::Vertical({
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
        command_input_component_->TakeFocus();
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

    // Escape focuses command input
    if (event == Event::Escape) {
      command_input_component_->TakeFocus();
      return true;
    }

    // Tab: accept autocomplete or toggle hub list focus
    if (event == Event::Tab) {
      if (command_input_component_->Focused() && !command_input_.empty() && command_input_.front() == '/') {
        AcceptSuggestion();
        return true;
      }
      // In Hub view, Tab toggles between repo and file lists
      if (active_view_ == View::Hub && !hf_repos_.empty() && !repo_files_.empty()) {
        hub_focus_on_files_ = !hub_focus_on_files_;
        screen_.PostEvent(Event::Custom);
        return true;
      }
      return true;
    }

    // Ctrl+Y toggles YOLO
    if (event == Event::Special({25})) {
      guard_.ToggleYolo();
      logger_.Log(std::string("YOLO mode ") + (guard_.YoloMode() ? "enabled." : "disabled."));
      screen_.PostEvent(Event::Custom);
      return true;
    }

    // Ctrl+C always quits
    if (event == Event::CtrlC) {
      screen_.ExitLoopClosure()();
      return true;
    }

    // q quits only when command input is focused and empty
    if (event == Event::Character('q')) {
      if (command_input_component_->Focused() && command_input_.empty()) {
        screen_.ExitLoopClosure()();
        return true;
      }
      if (!command_input_component_->Focused()) {
        // q in a menu → focus command input, don't quit
        command_input_component_->TakeFocus();
        return true;
      }
      return false;  // let 'q' be typed into input
    }

    // / starts typing a command (from anywhere)
    if (event == Event::Character('/')) {
      if (command_input_.empty()) command_input_ = "/";
      command_input_component_->TakeFocus();
      UpdateCommandSuggestions();
      return true;
    }

    // Enter: act on selected item in Models/Hub views FIRST, then execute command
    if (event == Event::Return) {
      // In Models view, Enter selects the highlighted model
      if (active_view_ == View::Models && !local_models_.empty() &&
          local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
        logger_.Log("Selected model: " + local_models_[local_selected_].name);
        screen_.PostEvent(Event::Custom);
        return true;
      }
      // In Hub view, Enter on repos loads files, Enter on files downloads
      if (active_view_ == View::Hub) {
        if (!hub_focus_on_files_ && !hf_repos_.empty()) {
          LoadRepoFiles();
          return true;
        }
        if (hub_focus_on_files_ && !repo_files_.empty()) {
          DownloadSelectedFile();
          return true;
        }
      }
      // Otherwise execute command input
      if (command_input_component_->Focused()) {
        if (!command_input_.empty() && command_input_.front() == '/') {
          AcceptSuggestion();
        }
        ExecuteCommandInput();
        return true;
      }
      return true;
    }

    // Arrow up/down: navigate menus in Models/Hub views FIRST, then command input
    if (event == Event::ArrowUp || event == Event::ArrowDown) {
      // In Models view, always navigate local models list
      if (active_view_ == View::Models && !local_models_.empty()) {
        if (event == Event::ArrowUp) {
          local_selected_ = std::max(0, local_selected_ - 1);
        } else {
          local_selected_ = std::min(static_cast<int>(local_models_.size()) - 1, local_selected_ + 1);
        }
        screen_.PostEvent(Event::Custom);
        return true;
      }
      // In Hub view, always navigate repo or file list based on Tab toggle
      if (active_view_ == View::Hub && !hf_repos_.empty()) {
        if (!hub_focus_on_files_) {
          // Navigate repos
          if (event == Event::ArrowUp) {
            repo_selected_ = std::max(0, repo_selected_ - 1);
          } else {
            repo_selected_ = std::min(static_cast<int>(hf_repos_.size()) - 1, repo_selected_ + 1);
          }
          screen_.PostEvent(Event::Custom);
          return true;
        }
        // Navigate files (if loaded)
        if (!repo_files_.empty()) {
          if (event == Event::ArrowUp) {
            file_selected_ = std::max(0, file_selected_ - 1);
          } else {
            file_selected_ = std::min(static_cast<int>(repo_files_.size()) - 1, file_selected_ + 1);
          }
          screen_.PostEvent(Event::Custom);
          return true;
        }
      }
      // In Vibecode view or empty lists, use arrows for command input autocomplete
      if (command_input_component_->Focused()) {
        if (!command_input_.empty() && command_input_.front() == '/') {
          MoveSuggestion(event == Event::ArrowDown ? 1 : -1);
          return true;
        }
        return false;  // let input handle arrow keys for cursor movement
      }
      return false;
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

  Elements layout;
  layout.push_back(BuildStatusBar());
  layout.push_back(separator());

  // View content
  Element view_content;
  switch (active_view_) {
    case View::Vibecode: view_content = BuildVibecodeView(); break;
    case View::Models:   view_content = BuildModelsView(); break;
    case View::Hub:      view_content = BuildHubView(); break;
  }
  layout.push_back(view_content | flex);

  // Transfers (inline if active)
  auto transfers_el = BuildTransfers();
  if (transfers_el) {
    layout.push_back(separator());
    layout.push_back(*transfers_el);
  }

  layout.push_back(separator());
  layout.push_back(BuildCommandInput());

  return vbox(std::move(layout)) | border;
}

Element App::BuildStatusBar() const {
  // Server chip
  bool running = server_manager_.Running();
  Element server_chip = text(running ? " ● RUNNING " : " ○ STOPPED ") |
                        bold |
                        color(Color::Black) |
                        bgcolor(running ? Color::GreenLight : Color::RedLight);

  // YOLO chip (only shown when ON)
  Element yolo_chip = text("");
  if (guard_.YoloMode()) {
    yolo_chip = hbox({
        text(" "),
        text(" YOLO ") | bold | color(Color::Black) | bgcolor(Color::YellowLight),
    });
  }

  // Transfers summary
  Element transfers_chip = text("");
  auto active = transfers_.GetActive();
  if (!active.empty()) {
    transfers_chip = hbox({
        text(" "),
        text(" ↓" + std::to_string(active.size()) + " ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
    });
  }

  // View name
  std::string view_name;
  switch (active_view_) {
    case View::Vibecode: view_name = "vibecode"; break;
    case View::Models:   view_name = "models"; break;
    case View::Hub:      view_name = "hub"; break;
  }

  return hbox({
      text(kAppTitle) | bold | color(Color::CyanLight),
      text(" "),
      server_chip,
      yolo_chip,
      transfers_chip,
      filler(),
      text("[") | dim,
      text(view_name) | color(Color::CyanLight),
      text("]") | dim,
  });
}

Element App::BuildVibecodeView() const {
  auto logs_copy = logger_.GetLogs();
  const size_t start = logs_copy.size() > 200 ? logs_copy.size() - 200 : 0;
  Elements log_lines;
  for (size_t i = start; i < logs_copy.size(); ++i) {
    Color c = Color::GrayLight;
    const std::string& line = logs_copy[i];
    if (line.rfind("$ ", 0) == 0) {
      c = Color::CyanLight;
    } else if (line.find("error") != std::string::npos || line.find("failed") != std::string::npos || line.find("Error") != std::string::npos) {
      c = Color::RedLight;
    } else if (line.find("[view]") != std::string::npos) {
      c = Color::YellowLight;
    } else if (line.rfind("Server", 0) == 0 || line.find("starting") != std::string::npos) {
      c = Color::GreenLight;
    }
    log_lines.push_back(text(line) | color(c));
  }
  if (log_lines.empty()) {
    log_lines.push_back(text("Ready. Type /help to get started.") | dim);
  }
  return vbox(std::move(log_lines)) | frame;
}

Element App::BuildModelsView() {
  Elements rows;

  if (local_models_.empty()) {
    rows.push_back(text("No local GGUF models found.") | dim);
    rows.push_back(text(""));
    rows.push_back(text("Try:") | dim);
    rows.push_back(text("  /download-binary   — get llama.cpp") | color(Color::CyanLight));
    rows.push_back(text("  /hub <query>       — search Hugging Face") | color(Color::CyanLight));
    rows.push_back(text("  /rescan            — rescan models/ directory") | color(Color::CyanLight));
  } else {
    rows.push_back(text("Models ") | bold | color(Color::CyanLight));
    rows.push_back(text("← [navigate]") | color(Color::YellowLight));
    rows.push_back(text("  (Enter: select)") | dim);
    rows.push_back(text(""));

    for (int i = 0; i < static_cast<int>(local_models_.size()); ++i) {
      const auto& m = local_models_[i];
      bool selected = (i == local_selected_);
      Element row = hbox({
          text(selected ? "▸ " : "  ") | color(Color::CyanLight),
          text(m.name) | (selected ? bold : nothing),
          text(" ["),
          text(util::HumanBytes(m.size)) | color(Color::GreenLight),
          text("]"),
      });
      if (selected) row = row | bgcolor(Color::GrayDark);
      rows.push_back(row);
    }

    if (local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
      const auto& m = local_models_[local_selected_];
      rows.push_back(text(""));
      rows.push_back(hbox({text("path:   ") | dim, text(m.path.string()) | color(Color::White)}));
      rows.push_back(hbox({text("origin: ") | dim, text(m.origin) | color(Color::White)}));
      rows.push_back(hbox({text("size:   ") | dim, text(util::HumanBytes(m.size)) | color(Color::White)}));
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("/select N · /start · /rescan · /hub") | dim);

  return vbox(std::move(rows));
}

Element App::BuildHubView() {
  Elements rows;

  // Search header
  rows.push_back(hbox({
      text("search: ") | dim,
      text(search_query_) | color(Color::CyanLight) | bold,
      text("  (/search <query> to change)") | dim,
  }));
  rows.push_back(text(""));

  if (hf_repos_.empty() || (repo_entries_.size() == 1 && repo_entries_[0] == "(none)")) {
    rows.push_back(text("No search results yet.") | dim);
    rows.push_back(text(""));
    rows.push_back(text("Try:  /search <query>") | color(Color::CyanLight));
  } else {
    // Repos
    bool repos_focused = !hub_focus_on_files_;
    rows.push_back(text("Repos ") | bold | color(repos_focused ? Color::CyanLight : Color::White));
    if (repos_focused) rows.push_back(text("← [navigate]") | color(Color::YellowLight));
    rows.push_back(text("  (Enter: list files)") | dim);
    rows.push_back(text(""));

    for (int i = 0; i < static_cast<int>(hf_repos_.size()); ++i) {
      const auto& repo = hf_repos_[i];
      bool selected = (i == repo_selected_);
      Element row = hbox({
          text(selected ? "▸ " : "  ") | color(Color::CyanLight),
          text(repo.id) | (selected ? bold : nothing),
          text(" ↓"),
          text(std::to_string(repo.downloads)) | color(Color::GreenLight),
          text(" ♥"),
          text(std::to_string(repo.likes)) | color(Color::RedLight),
      });
      if (selected) row = row | bgcolor(Color::GrayDark);
      rows.push_back(row);
    }

    if (repo_selected_ >= 0 && repo_selected_ < static_cast<int>(hf_repos_.size())) {
      const auto& repo = hf_repos_[repo_selected_];
      rows.push_back(text(""));
      rows.push_back(hbox({text("  id:   ") | dim, text(repo.id) | color(Color::White)}));
      rows.push_back(hbox({text("  sha:  ") | dim, text(repo.sha.empty() ? "(unknown)" : repo.sha) | color(Color::White)}));
    }

    // Files
    if (!repo_files_.empty() && !(repo_files_.size() == 1 && file_entries_[0] == "(none)")) {
      rows.push_back(text(""));
      bool files_focused = hub_focus_on_files_;
      rows.push_back(text("GGUF Files ") | bold | color(files_focused ? Color::CyanLight : Color::White));
      if (files_focused) rows.push_back(text("← [navigate]") | color(Color::YellowLight));
      rows.push_back(text("  (Enter: download)") | dim);
      rows.push_back(text(""));

      for (int i = 0; i < static_cast<int>(repo_files_.size()); ++i) {
        const auto& file = repo_files_[i];
        bool selected = (i == file_selected_);
        std::string size_str = file.size > 0 ? util::HumanBytes(file.size) : "?";
        Element row = hbox({
            text(selected ? "▸ " : "  ") | color(Color::CyanLight),
            text(file.filename) | (selected ? bold : nothing),
            text(" ["),
            text(size_str) | color(Color::GreenLight),
            text("]"),
        });
        if (selected) row = row | bgcolor(Color::GrayDark);
        rows.push_back(row);
      }
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("/search · /files · /download · /start") | dim);

  return vbox(std::move(rows));
}

Element App::BuildCommandInput() {
  // Autocomplete suggestions
  Elements suggestion_rows;
  if (!command_input_.empty() && command_input_.front() == '/') {
    UpdateCommandSuggestions();
    for (size_t i = 0; i < command_suggestions_.size() && i < 5; ++i) {
      const auto& cmd = command_suggestions_[i];
      bool selected = (static_cast<int>(i) == command_selected_);
      Element row = hbox({
          text("  "),
          text(cmd.usage) | bold | (selected ? color(Color::CyanLight) : color(Color::White)),
          text("  ") | dim,
          text(cmd.description) | dim,
      });
      if (selected) row = row | bgcolor(Color::GrayDark);
      suggestion_rows.push_back(row);
    }
  }

  Element input_line = hbox({
      text(" $ ") | color(Color::CyanLight) | bold,
      command_input_component_->Render(),
  });

  if (suggestion_rows.empty()) {
    return input_line;
  }

  return vbox({
      vbox(std::move(suggestion_rows)),
      separator(),
      input_line,
  });
}

std::optional<Element> App::BuildTransfers() {
  auto active = transfers_.GetActive();
  if (active.empty()) return std::nullopt;

  Elements rows;
  for (const auto& t : active) {
    if (t.total > 0) {
      double pct = static_cast<double>(t.downloaded) / static_cast<double>(t.total);
      std::string status = util::HumanBytes(t.downloaded) + " / " + util::HumanBytes(t.total) +
                           " (" + std::to_string(static_cast<int>(pct * 100.0)) + "%)";
      rows.push_back(hbox({
          text("↓ ") | color(Color::CyanLight),
          text(t.label) | bold,
          text(" "),
          gauge(static_cast<float>(pct)) | flex | color(Color::CyanLight),
          text(" " + status),
      }));
    } else {
      std::string status = util::HumanBytes(t.downloaded) + "...";
      rows.push_back(hbox({
          text("↓ ") | color(Color::CyanLight),
          text(t.label) | bold,
          text(" "),
          spinner(4, static_cast<int>(t.downloaded / 65536) % 4),
          text(" " + status) | dim,
      }));
    }
  }
  return vbox(std::move(rows));
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
                            text(guard_.YoloMode() ? "YOLO mode is on." : "Enter/Y confirm · Esc/N cancel") | dim,
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
