#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace forge::util {

std::string GetEnv(const char* key);
std::optional<fs::path> HomeDirectory();
fs::path DefaultHfCacheRoot();

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

CommandResult RunCommandCapture(const std::vector<std::string>& argv);

}  // namespace forge::util
