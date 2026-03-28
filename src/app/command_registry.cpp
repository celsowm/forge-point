#include "app/command_registry.hpp"

#include "core/string_utils.hpp"

namespace forge {

CommandRegistry::CommandRegistry()
    : commands_({
          {"/help", "/help", "Show available commands."},
          {"/vibecode", "/vibecode", "Switch to log/output view (default)."},
          {"/models", "/models", "Browse local GGUF models."},
          {"/hub", "/hub [query]", "Search Hugging Face repos."},
          {"/search", "/search <query>", "Search Hugging Face repos."},
          {"/files", "/files", "List GGUF files for the selected repo."},
          {"/select", "/select <index>", "Select a model/repo/file by index."},
          {"/download", "/download", "Download the selected GGUF file."},
          {"/start", "/start [--host H] [--port P] [--extra-args ...]", "Launch llama-server."},
          {"/stop", "/stop", "Stop llama-server."},
          {"/health", "/health", "Check the /health endpoint."},
          {"/rescan", "/rescan", "Rescan local GGUFs and cache."},
          {"/refresh-binary", "/refresh-binary", "Rescan runtime/ for llama-server."},
          {"/download-binary", "/download-binary", "Download llama.cpp for your GPU."},
          {"/yolo", "/yolo [on|off|toggle]", "Toggle confirmation-free mode."},
          {"/welcome", "/welcome", "Show the splash screen."},
          {"/quit", "/quit", "Quit Forge-Point."},
      }) {}

const std::vector<SlashCommand>& CommandRegistry::GetAll() const {
  return commands_;
}

std::vector<SlashCommand> CommandRegistry::Match(const std::string& input) const {
  std::vector<SlashCommand> results;
  const std::string trimmed = util::Trim(input);
  if (trimmed.empty() || trimmed.front() != '/') return results;

  std::string prefix = trimmed;
  const auto space = trimmed.find(' ');
  if (space != std::string::npos) {
    prefix = trimmed.substr(0, space);
  }

  for (const auto& cmd : commands_) {
    if (prefix == "/" ||
        util::StartsWithIgnoreCase(cmd.name, prefix) ||
        util::StartsWithIgnoreCase(cmd.usage, prefix)) {
      results.push_back(cmd);
    }
  }
  return results;
}

}  // namespace forge
