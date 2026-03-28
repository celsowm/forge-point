#pragma once

#include <functional>
#include <string>
#include <vector>

namespace forge {

struct SlashCommand {
  std::string name;
  std::string usage;
  std::string description;
};

class CommandRegistry {
 public:
  CommandRegistry();

  const std::vector<SlashCommand>& GetAll() const;
  std::vector<SlashCommand> Match(const std::string& input) const;

 private:
  std::vector<SlashCommand> commands_;
};

}  // namespace forge
