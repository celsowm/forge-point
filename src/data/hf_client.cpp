#include "data/hf_client.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace forge {

HfClient::HfClient(IHttpClient& http) : http_(http) {}

std::vector<HfRepo> HfClient::SearchRepos(const std::string& query,
                                            std::string& error) {
  const std::string url =
      "https://huggingface.co/api/models?search=" + util::UrlEncode(query) +
      "&limit=20&sort=downloads&direction=-1";
  auto response = GetWithAuth(url);
  if (!response.error.empty()) {
    error = response.error;
    return {};
  }
  if (response.status < 200 || response.status >= 300) {
    error = "HTTP " + std::to_string(response.status);
    return {};
  }

  const auto payload = json::parse(response.body, nullptr, false);
  if (payload.is_discarded() || !payload.is_array()) {
    error = "invalid JSON from Hugging Face search API";
    return {};
  }

  std::vector<HfRepo> repos;
  for (const auto& item : payload) {
    HfRepo repo;
    repo.id = item.value("id", "");
    repo.sha = item.value("sha", "");
    repo.downloads = item.value("downloads", 0);
    repo.likes = item.value("likes", 0);
    if (!repo.id.empty()) {
      repos.push_back(std::move(repo));
    }
  }
  return repos;
}

std::vector<HfFile> HfClient::ListGgufFiles(const std::string& repo_id,
                                              std::string& repo_sha,
                                              std::string& error) {
  const std::string url = "https://huggingface.co/api/models/" +
                          util::UrlEncode(repo_id);
  auto response = GetWithAuth(url);
  if (!response.error.empty()) {
    error = response.error;
    return {};
  }
  if (response.status < 200 || response.status >= 300) {
    error = "HTTP " + std::to_string(response.status);
    return {};
  }

  const auto payload = json::parse(response.body, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    error = "invalid JSON from Hugging Face model API";
    return {};
  }

  repo_sha = payload.value("sha", repo_sha);
  std::vector<HfFile> files;
  for (const auto& sibling : payload.value("siblings", json::array())) {
    const std::string name = sibling.value("rfilename", "");
    if (name.size() < 5 ||
        !util::StartsWithIgnoreCase(name.substr(name.size() - 5), ".gguf")) {
      continue;
    }
    HfFile file;
    file.filename = name;
    file.size = sibling.value("size", 0ULL);
    const std::string ref = repo_sha.empty() ? "main" : repo_sha;
    file.download_url = "https://huggingface.co/" + repo_id + "/resolve/" +
                        ref + "/" + util::UrlEncode(name) + "?download=true";
    files.push_back(std::move(file));
  }

  std::sort(files.begin(), files.end(), [](const HfFile& a, const HfFile& b) {
    return a.filename < b.filename;
  });
  return files;
}

bool HfClient::DownloadFile(const std::string& url, const fs::path& target,
                              std::string& error, ProgressFn on_progress) {
  return http_.DownloadToFile(url, target, error, AuthHeaders(),
                              std::move(on_progress));
}

std::vector<std::string> HfClient::AuthHeaders() const {
  const std::string token = util::GetEnv("HF_TOKEN");
  if (token.empty()) return {};
  return {"Authorization: Bearer " + token};
}

HttpResponse HfClient::GetWithAuth(const std::string& url) const {
  return http_.Get(url, AuthHeaders());
}

}  // namespace forge
