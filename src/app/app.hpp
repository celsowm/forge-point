#pragma once

#include "interfaces.hpp"

#include "app/command_handler.hpp"
#include "app/command_registry.hpp"
#include "app/confirmation_guard.hpp"
#include "app/logger.hpp"
#include "app/transfer_tracker.hpp"
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

  std::string DescribeServerLaunch() const;
  void ShowWelcome();
  void HideWelcome();
  void CyclePanel(int delta);
  void FocusActivePanel();

  void BuildUi();
  void OpenCommandPalette();
  void MoveSuggestion(int delta);
  void AcceptSuggestion();
  void UpdateCommandSuggestions();
  void ExecuteCommandInput();

  Element BuildMainScreen();
  std::optional<Element> BuildTransfers();
  Element BuildHeader() const;
  Element BuildLocalPanel() const;
  Element BuildHubPanel() const;
  Element BuildServerPanel() const;
  Element BuildCommandPalette();
  Element BuildConfirmOverlay() const;
  Element BuildWelcomeScreen() const;

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

  bool show_welcome_ = true;

  std::string search_query_;
  std::string host_input_;
  std::string port_input_;
  std::string extra_args_input_;
  std::string command_input_;

  std::vector<LocalModel> local_models_;
  std::vector<HfRepo> hf_repos_;
  std::vector<HfFile> repo_files_;
  std::vector<std::jthread> workers_;

  std::vector<std::string> local_model_entries_;
  std::vector<std::string> repo_entries_;
  std::vector<std::string> file_entries_;
  std::vector<std::string> source_toggle_entries_;
  std::vector<SlashCommand> command_suggestions_;

  int local_selected_ = 0;
  int repo_selected_ = 0;
  int file_selected_ = 0;
  int source_mode_ = 0;
  int command_selected_ = 0;

  enum class Panel { Local = 0, Hub, Server, Command, COUNT };
  Panel active_panel_ = Panel::Command;

  // ─── FTXUI components ───────────────────────────────────────────────
  Component root_container_;
  Component root_;
  Component search_input_;
  Component host_input_component_;
  Component port_input_component_;
  Component extra_args_component_;
  Component command_input_component_;
  Component local_menu_;
  Component repo_menu_;
  Component file_menu_;
  Component source_toggle_;

  // ─── UI queue ────────────────────────────────────────────────────────
  std::mutex ui_queue_mutex_;
  std::deque<std::function<void()>> ui_queue_;
};

}  // namespace forge
