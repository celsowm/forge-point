#pragma once

#include "interfaces.hpp"

#include "app/command_handler.hpp"
#include "app/command_registry.hpp"
#include "app/confirmation_guard.hpp"
#include "app/logger.hpp"
#include "app/transfer_tracker.hpp"
#include "app/debug_overlay.hpp"
#include "ui/home_menu.hpp"
#include "core/gpu_detector.hpp"
#include "core/http_client.hpp"
#include "core/llama_downloader.hpp"
#include "core/llama_server_manager.hpp"
#include "core/process_supervisor.hpp"
#include "data/hf_cache_scanner.hpp"
#include "data/hf_client.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace ftxui;

namespace forge {

class App {
 public:
  enum class MenuState { Home = 0, Search, Files, DownloadBinary, Server, Models };

  App();
  void Run();

 private:
  void PostToUi(std::function<void()> fn);
  void DrainUiQueue();
  void CleanupFinishedWorkers();
  void SpawnWorker(std::function<void()> fn);

  void RefreshBinaryStatus();
  void RefreshLocalModels();
  void DownloadLlamaCpp();
  void SearchRepos();
  void LoadRepoFiles();
  void DownloadSelectedFile();
  void StartServer();
  void StopServer();
  void CheckHealth();
  void QuitApp();

  std::string DescribeServerLaunch() const;

  void BuildUi();
  void NavigateTo(MenuState state);
  void GoBack();
  void HandleEnterKey();
  void HandleArrowKeys(Event event);
  void MoveSuggestion(int delta);
  void AcceptSuggestion();
  void UpdateCommandSuggestions();
  void ExecuteCommandInput();

  Element BuildMainScreen();
  Element BuildStatusBar() const;
  Element BuildSearchView();
  Element BuildFilesView();
  Element BuildDownloadBinaryView();
  Element BuildServerView();
  Element BuildModelsView();
  Element BuildCommandInput();
  std::optional<Element> BuildTransfers();
  Element BuildConfirmOverlay() const;
  Element BuildVibecodeView() const;
  Element BuildHubView();

  void RegisterCommands();

  // ─── Infrastructure ──────────────────────────────────────────────────
  ScreenInteractive screen_;
  fs::path project_models_dir_;
  fs::path runtime_dir_;

  // ─── Dependencies (interface-based) ──────────────────────────────────
  HttpClient http_client_;
  GpuDetector gpu_detector_;
  HfCacheScanner scanner_;
  HfClient hf_client_;
  ProcessSupervisor process_;
  LlamaServerManager server_manager_;
  LlamaDownloader downloader_;

  // ─── Focused collaborators ───────────────────────────────────────────
  Logger logger_;
  TransferTracker transfers_;
  CommandRegistry command_registry_;
  CommandHandler command_handler_;
  ConfirmationGuard guard_;

  // ─── State ───────────────────────────────────────────────────────────
  GpuInfo gpu_info_;
  std::optional<fs::path> binary_path_;
  std::string binary_status_ = "unknown";
  std::string health_status_ = "not checked";
  std::string last_command_preview_;
  std::string active_repo_sha_;

  std::string search_query_;
  std::string command_input_;

  std::string default_host_ = "127.0.0.1";
  std::string default_port_ = "8080";
  std::string default_extra_args_ = "-c 4096";

  std::vector<LocalModel> local_models_;
  std::vector<HfRepo> hf_repos_;
  std::vector<HfFile> repo_files_;
  std::vector<std::jthread> workers_;

  std::vector<std::string> local_model_entries_;
  std::vector<std::string> repo_entries_;
  std::vector<std::string> file_entries_;
  std::vector<SlashCommand> command_suggestions_;

  int local_selected_ = 0;
  int repo_selected_ = 0;
  int file_selected_ = 0;
  int command_selected_ = 0;
  int home_selected_ = 0;
  int server_selected_ = 0;
  bool hub_focus_on_files_ = false;

  std::vector<ui::MenuItem> home_items_;
  std::vector<ui::MenuItem> server_items_;
  std::vector<ui::MenuItem> model_items_;

  MenuState current_state_ = MenuState::Home;
  MenuState previous_state_ = MenuState::Home;
  
  enum class View { Vibecode = 0, Models, Hub };
  View active_view_ = View::Vibecode;

  // ─── FTXUI components ───────────────────────────────────────────────
  Component root_container_;
  Component root_;
  Component command_input_component_;
  Component local_menu_;
  Component repo_menu_;
  Component file_menu_;

  // ─── UI queue ────────────────────────────────────────────────────────
  std::mutex ui_queue_mutex_;
  std::deque<std::function<void()>> ui_queue_;
};

}  // namespace forge
