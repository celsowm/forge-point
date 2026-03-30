#include "app/app.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"
#include "ui/home_menu.hpp"
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

  // Rotating file logger (5 MB per file, keep 5 files = ~25 MB max)
  auto logs_dir = fs::current_path() / "logs";
  logger_.SetFileLogger(
      std::make_unique<RotatingFileLogger>(logs_dir, "forge-point", 5 * 1024 * 1024, 5));

  search_query_ = "Qwen GGUF";

  repo_entries_ = {"(none)"};
  file_entries_ = {"(none)"};

  home_items_ = {
      ui::MenuItem{"Search GGUF models", "Browse and download models from Hugging Face", "/hub"},
      ui::MenuItem{"Download llama.cpp", "Get llama-server binary for your system", "/download-binary"},
      ui::MenuItem{"Manage server", "Start, stop, and monitor llama-server", "/start"},
      ui::MenuItem{"View local models", "See models in your models/ directory", "/models"},
  };

  server_items_ = {
      ui::MenuItem{"Start server", "Launch llama-server with selected model", "/start"},
      ui::MenuItem{"Stop server", "Stop the running llama-server", "/stop"},
      ui::MenuItem{"Health check", "Check server status", "/health"},
      ui::MenuItem{"Back", "Return to home menu", ""},
  };

  model_items_ = {};

  RegisterCommands();
  RefreshBinaryStatus();
  RefreshLocalModels();
  BuildUi();
  UpdateCommandSuggestions();

  logger_.Log("Welcome to Forge-Point");
  logger_.Log("Navigate with arrow keys, press Enter to select");
  logger_.Log("Logs: " + logs_dir.string() + "\\forge-point.log");
  logger_.Log("Runtime: " + runtime_dir_.string());
  logger_.Log("GPU: " + gpu_info_.name + " (backend=" +
              std::to_string(static_cast<int>(gpu_info_.backend)) + ")");
  logger_.Log("Host: " + default_host_ + ":" + default_port_);
}

void App::Run() { screen_.Loop(root_); }

void App::NavigateTo(MenuState state) {
  previous_state_ = current_state_;
  current_state_ = state;
  home_selected_ = 0;
  screen_.PostEvent(Event::Custom);
}

void App::GoBack() {
  switch (current_state_) {
    case MenuState::Home:
      // Esc on Home = quit
      QuitApp();
      return;
    case MenuState::Files:
      // Files -> Search (repos list)
      current_state_ = MenuState::Search;
      break;
    default:
      // Everything else -> Home
      current_state_ = MenuState::Home;
      break;
  }
  home_selected_ = 0;
  screen_.PostEvent(Event::Custom);
}

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
  
  model_items_.clear();
  for (size_t i = 0; i < local_models_.size(); ++i) {
    model_items_.push_back(ui::MenuItem{
        local_models_[i].name,
        util::HumanBytes(local_models_[i].size),
        ""
    });
  }
  if (model_items_.empty()) {
    model_items_.push_back(ui::MenuItem{"No models found", "Run /rescan after adding files", ""});
  }
  model_items_.push_back(ui::MenuItem{"Back", "Return to home menu", ""});
  
  logger_.Log("Local scan complete. Found " + std::to_string(local_models_.size()) + " GGUF file(s).");
  screen_.PostEvent(Event::Custom);
}

void App::DownloadLlamaCpp() {
  auto& debug = DebugOverlay::Instance();
  debug.Info("=== DownloadLlamaCpp STARTED ===", "Downloader");
  debug.Debug("GPU Info: " + gpu_info_.name + " (Backend: " + std::to_string(static_cast<int>(gpu_info_.backend)) + ")", "Downloader");
  
  std::string error;
  debug.Debug("Fetching latest release...", "Downloader");
  auto release = downloader_.GetLatestRelease(error);
  if (!release) {
    debug.Error("Failed to fetch release: " + error, "Downloader");
    logger_.Log("ERROR: Failed to fetch release: " + error);
    screen_.PostEvent(Event::Custom);
    return;
  }
  debug.Info("Found release: " + release->tag, "Downloader");
  logger_.Log("Found release: " + release->tag);
  
  debug.Debug("Finding asset for platform...", "Downloader");
  auto asset = downloader_.FindAssetForPlatform(*release, gpu_info_);
  if (!asset) {
    debug.Error("No compatible binary found! GPU Backend: " + std::to_string(static_cast<int>(gpu_info_.backend)), "Downloader");
    logger_.Log("ERROR: No compatible binary found for this platform.");
    logger_.Log("GPU Backend: " + std::to_string(static_cast<int>(gpu_info_.backend)));
    screen_.PostEvent(Event::Custom);
    return;
  }
  debug.Info("Selected asset: " + *asset, "Downloader");
  logger_.Log("Selected asset: " + *asset);
  
  auto download_url = downloader_.GetDownloadUrl(release->tag, *asset);
  debug.Debug("Download URL: " + download_url, "Downloader");
  
  size_t tid = transfers_.Add(*asset);
  debug.Debug("Transfer ID: " + std::to_string(tid), "Downloader");
  
  auto dl_progress = [this, tid, &debug](uint64_t dl, uint64_t total) {
    transfers_.Update(tid, dl, total);
    if (total > 0 && dl > 0) {
      double pct = static_cast<double>(dl) / static_cast<double>(total) * 100.0;
      debug.Debug("Download progress: " + std::to_string(static_cast<int>(pct)) + "%", "Downloader");
    }
    screen_.PostEvent(Event::Custom);
  };
  
  debug.Info("Starting download...", "Downloader");
  logger_.Log("Starting download...");
  bool ok = downloader_.DownloadAndExtract(download_url, runtime_dir_, error,
                                            [this, &debug](const std::string& msg) {
                                              debug.Debug("Extract: " + msg, "Downloader");
                                              logger_.Log("[Extract] " + msg);
                                              screen_.PostEvent(Event::Custom);
                                            },
                                            dl_progress);
  
  if (ok) {
    transfers_.Finish(tid);
    debug.Info("SUCCESS: Download complete!", "Downloader");
    logger_.Log("SUCCESS: Download complete!");
    PostToUi([this, &debug] { 
      RefreshBinaryStatus();
      debug.Info("Binary status: " + binary_status_, "Downloader");
    });
  } else {
    transfers_.Finish(tid, true);
    debug.Error("Download failed: " + error, "Downloader");
    logger_.Log("ERROR: Download failed: " + error);
  }
  screen_.PostEvent(Event::Custom);
  debug.Info("=== DownloadLlamaCpp FINISHED ===", "Downloader");
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
    repo_entries_.push_back(repo.id + " (" + std::to_string(repo.downloads) + " downloads)");
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
                 "Download " + file.filename + " to " + target_file.string(),
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
        logger_.Log("No model selected. Select a model first.");
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
  oss << "Host: " << default_host_ << "\n";
  oss << "Port: " << default_port_ << "\n";
  oss << "Args: " << default_extra_args_ << "\n";

  bool has_hf = !hf_repos_.empty() && !repo_files_.empty() &&
                repo_selected_ < static_cast<int>(hf_repos_.size()) &&
                file_selected_ < static_cast<int>(repo_files_.size());
  bool has_local = !local_models_.empty() &&
                   local_selected_ < static_cast<int>(local_models_.size());

  if (active_view_ == View::Hub && has_hf) {
    oss << "Mode: HF Repo/File\n";
    oss << "Repo: " << hf_repos_[repo_selected_].id << "\n";
    oss << "File: " << repo_files_[file_selected_].filename;
  } else if (has_local) {
    oss << "Mode: Local GGUF\n";
    oss << "Model: " << local_models_[local_selected_].path.string();
  } else {
    oss << "Mode: (no model selected)";
  }
  return oss.str();
}

void App::StopServer() {
  guard_.Request("Stop llama-server",
                 "Stop the running llama-server process?",
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
  logger_.Log("[health] Checking " + default_host_ + ":" + default_port_ + " ...");
  SpawnWorker([this] {
    std::string status = server_manager_.Health(default_host_, default_port_);
    PostToUi([this, status] {
      health_status_ = status;
      logger_.Log("[health] Result: " + health_status_);
      screen_.PostEvent(Event::Custom);
    });
  });
}

void App::QuitApp() {
  if (server_manager_.Running()) {
    logger_.Log("Stopping llama-server...");
    server_manager_.Stop();
    logger_.Log("Server stopped.");
  }
  logger_.Log("Bye!");
  screen_.ExitLoopClosure()();
}

// ─── Commands ────────────────────────────────────────────────────────────────

void App::RegisterCommands() {
  command_handler_.Register("/help", [this](const auto&) {
    logger_.Log("Available commands:");
    for (const auto& item : command_registry_.GetAll()) {
      logger_.Log("  " + item.usage + "  " + item.description);
    }
  });

  command_handler_.Register("/search", [this](const auto& args) {
    if (!args.empty()) {
      search_query_ = args[0];
      for (size_t i = 1; i < args.size(); ++i) search_query_ += " " + args[i];
    }
    SearchRepos();
    NavigateTo(MenuState::Search);
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
    if (idx >= 0 && idx < static_cast<int>(local_models_.size())) {
      local_selected_ = idx;
      logger_.Log("Selected model: " + local_models_[idx].name);
    } else {
      logger_.Log("Model index out of range.");
    }
    screen_.PostEvent(Event::Custom);
  });

  command_handler_.Register("/start", [this](const auto& args) {
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
    QuitApp();
  });
  
  command_handler_.Register("/home", [this](const auto&) { NavigateTo(MenuState::Home); });
  command_handler_.Register("/back", [this](const auto&) { GoBack(); });
}

// ─── UI building ─────────────────────────────────────────────────────────────

void App::BuildUi() {
  command_input_component_ = Input(&command_input_, "Type / for commands...");

  local_menu_ = Menu(&local_model_entries_, &local_selected_);
  repo_menu_ = Menu(&repo_entries_, &repo_selected_);
  file_menu_ = Menu(&file_entries_, &file_selected_);

  root_container_ = Container::Vertical({
      command_input_component_,
  });

  root_ = Renderer(root_container_, [this] {
    DrainUiQueue();
    Element body = BuildMainScreen();
    if (guard_.IsShowing()) {
      body = dbox({body, BuildConfirmOverlay()});
    }
    // Add debug overlay on top
    body = dbox({body, DebugOverlay::Instance().Render()});
    return body;
  });

  root_ = CatchEvent(root_, [this](Event event) {
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

    if (event == Event::Escape) {
      if (command_input_component_->Focused()) {
        command_input_.clear();
        screen_.PostEvent(Event::Custom);
        return true;
      }
      GoBack();
      return true;
    }

    if (event == Event::Tab) {
      if (command_input_component_->Focused() && !command_input_.empty() && command_input_.front() == '/') {
        AcceptSuggestion();
        return true;
      }
      if (current_state_ == MenuState::Search && !hf_repos_.empty() && !repo_files_.empty()) {
        hub_focus_on_files_ = !hub_focus_on_files_;
        screen_.PostEvent(Event::Custom);
        return true;
      }
      return true;
    }

    if (event == Event::Special({25})) {
      guard_.ToggleYolo();
      logger_.Log(std::string("YOLO mode ") + (guard_.YoloMode() ? "enabled." : "disabled."));
      screen_.PostEvent(Event::Custom);
      return true;
    }

    if (event == Event::F12) {
      DebugOverlay::Instance().Toggle();
      screen_.PostEvent(Event::Custom);
      return true;
    }

    if (event == Event::CtrlC) {
      QuitApp();
      return true;
    }

    if (event == Event::Character('q')) {
      if (command_input_component_->Focused() && command_input_.empty()) {
        QuitApp();
        return true;
      }
      if (!command_input_component_->Focused()) {
        command_input_component_->TakeFocus();
        return true;
      }
      return false;
    }

    if (event == Event::Character('/')) {
      if (command_input_.empty()) command_input_ = "/";
      command_input_component_->TakeFocus();
      UpdateCommandSuggestions();
      return true;
    }

    if (event == Event::Return) {
      if (command_input_component_->Focused() && !command_input_.empty()) {
        ExecuteCommandInput();
        return true;
      }
      HandleEnterKey();
      return true;
    }

    if (event == Event::ArrowUp || event == Event::ArrowDown) {
      if (command_input_component_->Focused()) return false;
      HandleArrowKeys(event);
      return true;
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

void App::HandleEnterKey() {
  auto& debug = DebugOverlay::Instance();
  debug.Info("HandleEnterKey - State: " + std::to_string(static_cast<int>(current_state_)) + ", Selected: " + std::to_string(home_selected_), "UI");
  
  switch (current_state_) {
    case MenuState::Home:
      debug.Info("Home selected: " + std::to_string(home_selected_), "UI");
      if (home_selected_ == 0) NavigateTo(MenuState::Search);
      else if (home_selected_ == 1) NavigateTo(MenuState::DownloadBinary);
      else if (home_selected_ == 2) NavigateTo(MenuState::Server);
      else if (home_selected_ == 3) NavigateTo(MenuState::Models);
      break;
      
    case MenuState::Search:
      if (hf_repos_.empty()) {
        debug.Info("Triggering repo search", "UI");
        SearchRepos();
      } else {
        debug.Info("Loading repo files", "UI");
        LoadRepoFiles();
        NavigateTo(MenuState::Files);
      }
      break;
      
    case MenuState::Files:
      debug.Info("Downloading selected file", "UI");
      DownloadSelectedFile();
      break;
      
    case MenuState::DownloadBinary:
      debug.Info("DownloadBinary state entered", "UI");
      if (!binary_path_) {
        debug.Info("No binary found, starting download", "UI");
        logger_.Log("Starting download... Check status bar for progress");
        SpawnWorker([this] { DownloadLlamaCpp(); });
      } else {
        debug.Info("Binary already exists: " + binary_path_->string(), "UI");
        logger_.Log("llama-server already installed: " + binary_path_->string());
        GoBack();
      }
      break;
      
    case MenuState::Server:
      if (server_selected_ == 0) StartServer();
      else if (server_selected_ == 1) StopServer();
      else if (server_selected_ == 2) CheckHealth();
      else GoBack();
      break;
      
    case MenuState::Models:
      if (local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
        logger_.Log("Selected model: " + local_models_[local_selected_].name);
      } else {
        GoBack();
      }
      break;
  }
}

void App::HandleArrowKeys(Event event) {
  switch (current_state_) {
    case MenuState::Home:
      if (event == Event::ArrowUp) {
        home_selected_ = std::max(0, home_selected_ - 1);
      } else {
        home_selected_ = std::min(static_cast<int>(home_items_.size()) - 1, home_selected_ + 1);
      }
      break;
      
    case MenuState::Search:
      if (event == Event::ArrowUp) {
        repo_selected_ = std::max(0, repo_selected_ - 1);
      } else {
        repo_selected_ = std::min(static_cast<int>(hf_repos_.size()) - 1, repo_selected_ + 1);
      }
      break;
      
    case MenuState::Files:
      if (event == Event::ArrowUp) {
        file_selected_ = std::max(0, file_selected_ - 1);
      } else {
        file_selected_ = std::min(static_cast<int>(repo_files_.size()) - 1, file_selected_ + 1);
      }
      break;
      
    case MenuState::Server:
      if (event == Event::ArrowUp) {
        server_selected_ = std::max(0, server_selected_ - 1);
      } else {
        server_selected_ = std::min(static_cast<int>(server_items_.size()) - 1, server_selected_ + 1);
      }
      break;
      
    case MenuState::Models:
      if (event == Event::ArrowUp) {
        local_selected_ = std::max(0, local_selected_ - 1);
      } else {
        local_selected_ = std::min(static_cast<int>(model_items_.size()) - 1, local_selected_ + 1);
      }
      break;
  }
  screen_.PostEvent(Event::Custom);
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

  Element view_content;
  switch (current_state_) {
    case MenuState::Home:
      view_content = ui::BuildHomeMenu(home_items_, home_selected_);
      break;
    case MenuState::Search:
      view_content = BuildSearchView();
      break;
    case MenuState::Files:
      view_content = BuildFilesView();
      break;
    case MenuState::DownloadBinary:
      view_content = BuildDownloadBinaryView();
      break;
    case MenuState::Server:
      view_content = BuildServerView();
      break;
    case MenuState::Models:
      view_content = BuildModelsView();
      break;
  }
  
  layout.push_back(view_content | flex);

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
  bool running = server_manager_.Running();
  Element server_chip = text(running ? " RUNNING " : " STOPPED ") |
                        bold |
                        color(Color::Black) |
                        bgcolor(running ? Color::GreenLight : Color::RedLight);

  Element yolo_chip = text("");
  if (guard_.YoloMode()) {
    yolo_chip = hbox({
        text(" "),
        text(" YOLO ") | bold | color(Color::Black) | bgcolor(Color::YellowLight),
    });
  }

  auto active = transfers_.GetActive();
  Element transfers_chip = text("");
  if (!active.empty()) {
    transfers_chip = hbox({
        text(" "),
        text(" downloading ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
    });
  }

  std::string state_name;
  switch (current_state_) {
    case MenuState::Home: state_name = "Home"; break;
    case MenuState::Search: state_name = "Search"; break;
    case MenuState::Files: state_name = "Files"; break;
    case MenuState::DownloadBinary: state_name = "Download"; break;
    case MenuState::Server: state_name = "Server"; break;
    case MenuState::Models: state_name = "Models"; break;
  }

  return hbox({
      text(kAppTitle) | bold | color(Color::CyanLight),
      text(" "),
      server_chip,
      yolo_chip,
      transfers_chip,
      filler(),
      text("[") | dim,
      text(state_name) | color(Color::CyanLight),
      text("]") | dim,
      text("  Esc: Back"),
  });
}

Element App::BuildSearchView() {
  Elements rows;

  rows.push_back(hbox({
      text("Search: ") | dim,
      text(search_query_) | color(Color::CyanLight) | bold,
      text("  (Type /search <query> to change)") | dim,
  }));
  rows.push_back(text(""));

  if (hf_repos_.empty() || (repo_entries_.size() == 1 && repo_entries_[0] == "(none)")) {
    rows.push_back(text("No results yet. Press Enter to search or use /search <query>") | dim);
  } else {
    rows.push_back(text("Repositories") | bold | color(Color::CyanLight));
    rows.push_back(text("Press Enter to list files") | dim);
    rows.push_back(text(""));

    for (size_t i = 0; i < hf_repos_.size(); ++i) {
      const auto& repo = hf_repos_[i];
      bool selected = (static_cast<int>(i) == repo_selected_);
      Element row = hbox({
          text(selected ? "> " : "  ") | color(Color::CyanLight),
          text(repo.id) | (selected ? bold : nothing),
          text(" (" + std::to_string(repo.downloads) + " downloads)"),
      });
      if (selected) row = row | bgcolor(Color::GrayDark);
      rows.push_back(row);
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("Enter: Select  Esc: Back") | dim);

  return vbox(std::move(rows));
}

Element App::BuildFilesView() {
  Elements rows;

  if (repo_selected_ >= 0 && repo_selected_ < static_cast<int>(hf_repos_.size())) {
    rows.push_back(hbox({
        text("Files from: ") | dim,
        text(hf_repos_[repo_selected_].id) | bold | color(Color::CyanLight),
    }));
  }
  rows.push_back(text(""));

  if (repo_files_.empty() || (file_entries_.size() == 1 && file_entries_[0] == "(none)")) {
    rows.push_back(text("No GGUF files found") | dim);
  } else {
    rows.push_back(text("GGUF Files") | bold | color(Color::CyanLight));
    rows.push_back(text("Press Enter to download") | dim);
    rows.push_back(text(""));

    for (size_t i = 0; i < repo_files_.size(); ++i) {
      const auto& file = repo_files_[i];
      bool selected = (static_cast<int>(i) == file_selected_);
      std::string size_str = file.size > 0 ? util::HumanBytes(file.size) : "?";
      Element row = hbox({
          text(selected ? "> " : "  ") | color(Color::CyanLight),
          text(file.filename) | (selected ? bold : nothing),
          text(" [" + size_str + "]"),
      });
      if (selected) row = row | bgcolor(Color::GrayDark);
      rows.push_back(row);
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("Enter: Download  Esc: Back") | dim);

  return vbox(std::move(rows));
}

Element App::BuildDownloadBinaryView() {
  Elements rows;

  rows.push_back(text("Download llama.cpp") | bold | color(Color::CyanLight));
  rows.push_back(text(""));

  if (binary_path_) {
    rows.push_back(text("llama-server already installed:") | color(Color::GreenLight));
    rows.push_back(text(binary_path_->string()) | dim);
    rows.push_back(text(""));
    rows.push_back(text("Press Enter to go back") | dim);
  } else {
    std::string gpu_name = gpu_info_.name.empty() ? "CPU" : gpu_info_.name;
    rows.push_back(hbox({text("Detected: ") | dim, text(gpu_name) | bold}));
    rows.push_back(text(""));
    
    // Check if download is in progress
    auto active_transfers = transfers_.GetActive();
    if (!active_transfers.empty()) {
      rows.push_back(text("Download in progress...") | color(Color::YellowLight) | bold);
      rows.push_back(text(""));
      for (const auto& transfer : active_transfers) {
        std::string status;
        if (transfer.total > 0) {
          double pct = static_cast<double>(transfer.downloaded) / static_cast<double>(transfer.total) * 100.0;
          status = util::HumanBytes(transfer.downloaded) + " / " + util::HumanBytes(transfer.total) + 
                   " (" + std::to_string(static_cast<int>(pct)) + "%)";
        } else {
          status = util::HumanBytes(transfer.downloaded) + " downloaded...";
        }
        rows.push_back(hbox({
            text("  ") | color(Color::CyanLight),
            text(transfer.label) | bold,
            text(" - ") | dim,
            text(status) | color(Color::GreenLight),
        }));
      }
      rows.push_back(text(""));
      rows.push_back(text("Wait for download to complete...") | dim);
    } else {
      rows.push_back(text("Press Enter to download latest release") | color(Color::CyanLight) | bold);
      rows.push_back(text(""));
      rows.push_back(text("This will download and extract llama-server to runtime/llama.cpp/") | dim);
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("Enter: Download  Esc: Back") | dim);

  return vbox(std::move(rows));
}

Element App::BuildServerView() {
  Elements rows;

  rows.push_back(text("Server Controls") | bold | color(Color::CyanLight));
  rows.push_back(text(""));

  bool running = server_manager_.Running();
  rows.push_back(hbox({
      text("Status: ") | dim,
      text(running ? "RUNNING" : "STOPPED") | (running ? color(Color::GreenLight) : color(Color::RedLight)) | bold,
  }));

  if (health_status_ != "not checked") {
    bool healthy = health_status_.find("ok") != std::string::npos ||
                   health_status_ == "healthy";
    rows.push_back(hbox({
        text("Health: ") | dim,
        text(health_status_) | (healthy ? color(Color::GreenLight) : color(Color::RedLight)) | bold,
    }));
  }

  rows.push_back(hbox({text("Host: ") | dim, text(default_host_)}));
  rows.push_back(hbox({text("Port: ") | dim, text(default_port_)}));
  rows.push_back(text(""));

  if (!binary_path_) {
    rows.push_back(text("llama-server not found. Download first.") | color(Color::YellowLight));
    rows.push_back(text(""));
    rows.push_back(text("Press Enter on 'Download llama.cpp' from home menu") | dim);
  } else {
    for (size_t i = 0; i < server_items_.size(); ++i) {
      const auto& item = server_items_[i];
      bool selected = (static_cast<int>(i) == server_selected_);
      Element row = hbox({
          text(selected ? "> " : "  ") | color(Color::CyanLight),
          text(item.label) | (selected ? bold : nothing),
      });
      if (selected) row = row | bgcolor(Color::GrayDark);
      rows.push_back(row);
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("Enter: Execute  Esc: Back") | dim);

  return vbox(std::move(rows));
}

Element App::BuildModelsView() {
  Elements rows;

  rows.push_back(text("Local Models") | bold | color(Color::CyanLight));
  rows.push_back(text(""));

  if (model_items_.empty()) {
    rows.push_back(text("No local GGUF models found.") | dim);
    rows.push_back(text(""));
    rows.push_back(text("Try:") | dim);
    rows.push_back(text("  /download-binary   - get llama.cpp") | color(Color::CyanLight));
    rows.push_back(text("  /search <query>    - search Hugging Face") | color(Color::CyanLight));
    rows.push_back(text("  /rescan            - rescan models/ directory") | color(Color::CyanLight));
  } else {
    for (size_t i = 0; i < model_items_.size(); ++i) {
      const auto& item = model_items_[i];
      bool selected = (static_cast<int>(i) == local_selected_);
      Element row = hbox({
          text(selected ? "> " : "  ") | color(Color::CyanLight),
          text(item.label) | (selected ? bold : nothing),
      });
      if (selected && !item.description.empty()) {
        row = vbox({
            row,
            text("  " + item.description) | dim,
        });
      }
      if (selected) row = row | bgcolor(Color::GrayDark);
      rows.push_back(row);
    }

    if (local_selected_ >= 0 && local_selected_ < static_cast<int>(local_models_.size())) {
      const auto& m = local_models_[local_selected_];
      rows.push_back(text(""));
      rows.push_back(hbox({text("Path:   ") | dim, text(m.path.string()) | color(Color::White)}));
      rows.push_back(hbox({text("Size:   ") | dim, text(util::HumanBytes(m.size)) | color(Color::White)}));
    }
  }

  rows.push_back(text(""));
  rows.push_back(text("Enter: Select  Esc: Back") | dim);

  return vbox(std::move(rows));
}

Element App::BuildCommandInput() {
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
          text(" downloading ") | color(Color::CyanLight),
          text(t.label) | bold,
          text(" "),
          gauge(static_cast<float>(pct)) | flex | color(Color::CyanLight),
          text(" " + status),
      }));
    } else {
      std::string status = util::HumanBytes(t.downloaded) + "...";
      rows.push_back(hbox({
          text(" downloading ") | color(Color::CyanLight),
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
                             text(guard_.YoloMode() ? "YOLO mode is on." : "Enter/Y confirm - Esc/N cancel") | dim,
                         })) | size(WIDTH, LESS_THAN, 72),
                  filler(),
              }),
              filler(),
          }) |
          bgcolor(Color::Black);
}

}  // namespace forge
