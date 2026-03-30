#include "core/llama_server_manager.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"

namespace forge {

LlamaServerManager::LlamaServerManager(IProcess& process, IHttpClient& http)
    : process_(process), http_(http) {}

std::optional<fs::path> LlamaServerManager::FindBundledBinary(const fs::path& runtime_dir) {
#ifdef _WIN32
  const std::vector<std::string> names = {"llama-server.exe", "server.exe"};
#else
  const std::vector<std::string> names = {"llama-server", "server"};
#endif
  for (const auto& name : names) {
    const fs::path direct = runtime_dir / name;
    if (fs::exists(direct)) return direct;
    const fs::path bin_path = runtime_dir / "bin" / name;
    if (fs::exists(bin_path)) return bin_path;
  }

  if (fs::exists(runtime_dir)) {
    fs::directory_options options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(runtime_dir, options), end; it != end; ++it) {
      std::error_code ec;
      if (!it->is_regular_file(ec)) continue;
      const auto filename = util::ToLower(it->path().filename().string());
      for (const auto& candidate : names) {
        if (filename == util::ToLower(candidate)) {
          return it->path();
        }
      }
    }
  }

#ifdef _WIN32
  const auto probe = util::RunCommandCapture({"llama-server.exe", "--version"});
  if (probe.exit_code == 0) return fs::path("llama-server.exe");
#else
  const auto probe = util::RunCommandCapture({"llama-server", "--version"});
  if (probe.exit_code == 0) return fs::path("llama-server");
#endif
  return std::nullopt;
}

bool LlamaServerManager::StartWithLocalModel(const fs::path& binary,
                                              const fs::path& model,
                                              const std::string& host,
                                              const std::string& port,
                                              const std::string& extra_args,
                                              const IProcess::LogFn& log_fn,
                                              std::string& command_preview) {
  std::vector<std::string> argv = {binary.string(), "-m", model.string(),
                                    "--host", host, "--port", port};
  const auto extra = util::SplitArgs(extra_args);
  argv.insert(argv.end(), extra.begin(), extra.end());
  command_preview = util::JoinCommand(argv);
  return process_.Start(argv, log_fn);
}

bool LlamaServerManager::StartWithHfRepo(const fs::path& binary,
                                          const std::string& repo,
                                          const std::string& filename,
                                          const std::string& host,
                                          const std::string& port,
                                          const std::string& extra_args,
                                          const IProcess::LogFn& log_fn,
                                          std::string& command_preview) {
  std::vector<std::string> argv = {
      binary.string(), "--hf-repo", repo, "--hf-file", filename,
      "--host", host, "--port", port,
  };
  const auto extra = util::SplitArgs(extra_args);
  argv.insert(argv.end(), extra.begin(), extra.end());
  command_preview = util::JoinCommand(argv);
  return process_.Start(argv, log_fn);
}

void LlamaServerManager::Stop() { process_.Stop(); }

bool LlamaServerManager::Running() const { return process_.Running(); }

std::string LlamaServerManager::Health(const std::string& host,
                                        const std::string& port) {
  const std::string url = "http://" + host + ":" + port + "/health";
  const auto response = http_.Get(url);

  if (!response.error.empty()) {
    return "unreachable (error: " + response.error + ")";
  }
  if (response.status == 0) {
    return "unreachable (no response)";
  }
  if (response.status < 200 || response.status >= 300) {
    return "unreachable (HTTP " + std::to_string(response.status) + ")";
  }
  auto body = util::Trim(response.body);
  return body.empty() ? "healthy" : body;
}

}  // namespace forge
