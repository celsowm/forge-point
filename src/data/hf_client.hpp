#pragma once

#include "interfaces.hpp"

#include <string>

namespace forge {

class HfClient : public IModelRepository {
 public:
  explicit HfClient(IHttpClient& http);

  std::vector<HfRepo> SearchRepos(const std::string& query,
                                  std::string& error) override;
  std::vector<HfFile> ListGgufFiles(const std::string& repo_id,
                                    std::string& repo_sha,
                                    std::string& error) override;
  bool DownloadFile(const std::string& url, const fs::path& target,
                    std::string& error,
                    ProgressFn on_progress = nullptr) override;

 private:
  std::vector<std::string> AuthHeaders() const;
  HttpResponse GetWithAuth(const std::string& url) const;

  IHttpClient& http_;
};

}  // namespace forge
