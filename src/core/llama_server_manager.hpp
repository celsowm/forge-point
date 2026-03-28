#pragma once

#include "interfaces.hpp"

#include <string>

namespace forge {

class LlamaServerManager {
 public:
  LlamaServerManager(IProcess& process, IHttpClient& http);

  static std::optional<fs::path> FindBundledBinary(const fs::path& runtime_dir);

  bool StartWithLocalModel(const fs::path& binary, const fs::path& model,
                           const std::string& host, const std::string& port,
                           const std::string& extra_args,
                           const IProcess::LogFn& log_fn,
                           std::string& command_preview);

  bool StartWithHfRepo(const fs::path& binary, const std::string& repo,
                        const std::string& filename,
                        const std::string& host, const std::string& port,
                        const std::string& extra_args,
                        const IProcess::LogFn& log_fn,
                        std::string& command_preview);

  void Stop();
  bool Running() const;
  std::string Health(const std::string& host, const std::string& port);

 private:
  IProcess& process_;
  IHttpClient& http_;
};

}  // namespace forge
