#pragma once

#include "interfaces.hpp"

#include <functional>

namespace forge {

class LlamaDownloader {
 public:
  explicit LlamaDownloader(IHttpClient& http);

  std::optional<LlamaRelease> GetLatestRelease(std::string& error) const;
  std::optional<std::string> FindAssetForPlatform(const LlamaRelease& release,
                                                   const GpuInfo& gpu) const;
  std::string GetDownloadUrl(const std::string& tag, const std::string& asset) const;
  bool DownloadAndExtract(const std::string& url, const fs::path& target_dir,
                          std::string& error,
                          const std::function<void(std::string)>& progress,
                          ProgressFn on_dl_progress = nullptr);

 private:
  IHttpClient& http_;
};

}  // namespace forge
