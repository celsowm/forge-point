#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace forge {

class ICommand {
 public:
  virtual ~ICommand() = default;
  virtual void Execute(const std::vector<std::string>& args) = 0;
};

class CommandHandler {
 public:
  void Register(const std::string& name, std::function<void(const std::vector<std::string>&)> handler);
  bool Execute(const std::string& name, const std::vector<std::string>& args);

 private:
  std::unordered_map<std::string, std::function<void(const std::vector<std::string>&)>> handlers_;
};

}  // namespace forge
