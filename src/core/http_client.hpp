#pragma once

#include "interfaces.hpp"

#include <filesystem>

namespace forge {

class HttpClient : public IHttpClient {
 public:
  HttpClient();
  ~HttpClient() override;

  HttpResponse Get(const std::string& url,
                   const std::vector<std::string>& headers = {}) const override;
  bool DownloadToFile(const std::string& url, const fs::path& target,
                      std::string& error,
                      const std::vector<std::string>& headers = {},
                      ProgressFn on_progress = nullptr) const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace forge
