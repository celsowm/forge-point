#include "core/platform_utils.hpp"

#include "core/string_utils.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdio>
#include <cstdlib>

namespace forge::util {

std::string GetEnv(const char* key) {
  if (const char* value = std::getenv(key)) {
    return value;
  }
  return {};
}

std::optional<fs::path> HomeDirectory() {
#ifdef _WIN32
  const std::string user_profile = GetEnv("USERPROFILE");
  if (!user_profile.empty()) {
    return fs::path(user_profile);
  }
  const std::string home_drive = GetEnv("HOMEDRIVE");
  const std::string home_path = GetEnv("HOMEPATH");
  if (!home_drive.empty() && !home_path.empty()) {
    return fs::path(home_drive + home_path);
  }
  return std::nullopt;
#else
  const std::string home = GetEnv("HOME");
  if (!home.empty()) {
    return fs::path(home);
  }
  return std::nullopt;
#endif
}

fs::path DefaultHfCacheRoot() {
  const std::string hf_hub_cache = GetEnv("HF_HUB_CACHE");
  if (!hf_hub_cache.empty()) {
    return fs::path(hf_hub_cache);
  }
  const std::string hf_home = GetEnv("HF_HOME");
  if (!hf_home.empty()) {
    return fs::path(hf_home) / "hub";
  }
#ifdef _WIN32
  const std::string local_app_data = GetEnv("LOCALAPPDATA");
  if (!local_app_data.empty()) {
    const fs::path win32_cache = fs::path(local_app_data) / "huggingface" / "hub";
    if (fs::exists(win32_cache)) {
      return win32_cache;
    }
  }
  if (const auto home = HomeDirectory()) {
    const fs::path win_cache = *home / ".cache" / "huggingface" / "hub";
    if (fs::exists(win_cache)) {
      return win_cache;
    }
  }
#endif
  const std::string xdg_cache_home = GetEnv("XDG_CACHE_HOME");
  if (!xdg_cache_home.empty()) {
    return fs::path(xdg_cache_home) / "huggingface" / "hub";
  }
  if (const auto home = HomeDirectory()) {
    return *home / ".cache" / "huggingface" / "hub";
  }
  return fs::current_path() / ".cache" / "huggingface" / "hub";
}

CommandResult RunCommandCapture(const std::vector<std::string>& argv) {
  const std::string command = JoinCommand(argv) + " 2>&1";
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "r");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) {
    return {-1, "failed to spawn command"};
  }

  std::array<char, 4096> buffer{};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#ifdef _WIN32
  const int status = _pclose(pipe);
  return {status, output};
#else
  const int status = pclose(pipe);
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
  return {exit_code, output};
#endif
}

}  // namespace forge::util
