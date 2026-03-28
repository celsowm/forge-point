#include "app/command_handler.hpp"

namespace forge {

void CommandHandler::Register(
    const std::string& name,
    std::function<void(const std::vector<std::string>&)> handler) {
  handlers_[name] = std::move(handler);
}

bool CommandHandler::Execute(const std::string& name,
                              const std::vector<std::string>& args) {
  auto it = handlers_.find(name);
  if (it == handlers_.end()) return false;
  it->second(args);
  return true;
}

}  // namespace forge
