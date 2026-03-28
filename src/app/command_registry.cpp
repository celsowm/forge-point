#include "app/command_registry.hpp"

#include "core/string_utils.hpp"

namespace forge {

CommandRegistry::CommandRegistry()
    : commands_({
          {"/help", "/help", "Show available slash commands."},
          {"/search", "/search <query>", "Search Hugging Face repos."},
          {"/files", "/files", "List GGUF files for the selected repo."},
          {"/download", "/download", "Download the selected GGUF into models/."},
          {"/start", "/start", "Launch llama-server with the selected target."},
          {"/stop", "/stop", "Stop llama-server."},
          {"/health", "/health", "Check the /health endpoint."},
          {"/rescan", "/rescan", "Rescan local GGUFs and cache."},
          {"/refresh-binary", "/refresh-binary", "Rescan runtime/llama.cpp for llama-server."},
          {"/download-binary", "/download-binary", "Download llama.cpp for your GPU."},
          {"/yolo", "/yolo [on|off|toggle]", "Toggle confirmation-free mode."},
          {"/welcome", "/welcome", "Show the splash screen again."},
          {"/focus", "/focus <search|models|server|command>", "Move focus to a specific control."},
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
