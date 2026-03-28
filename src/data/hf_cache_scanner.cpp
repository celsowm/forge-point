#include "data/hf_cache_scanner.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"

#include <set>

namespace forge {

HfCacheScanner::HfCacheScanner(fs::path app_models_dir)
    : app_models_dir_(std::move(app_models_dir)) {}

fs::path HfCacheScanner::CacheRoot() const {
  return util::DefaultHfCacheRoot();
}

std::vector<LocalModel> HfCacheScanner::Scan() const {
  std::vector<LocalModel> models;
  std::set<std::string> seen;

  auto add_model = [&](const fs::path& path, const std::string& origin) {
    std::error_code ec;
    const auto extension = util::ToLower(path.extension().string());
    if (extension != ".gguf") return;

    const auto canonical_path = fs::weakly_canonical(path, ec);
    const std::string stable_path = ec ? path.lexically_normal().string()
                                       : canonical_path.string();
    if (!seen.insert(stable_path).second) return;

    LocalModel model;
    model.name = path.filename().string();
    model.path = path;
    model.origin = origin;
    model.size = fs::file_size(path, ec);
    models.push_back(std::move(model));
  };

  auto scan_tree = [&](const fs::path& root, const std::string& origin) {
    if (!fs::exists(root)) return;
    fs::directory_options options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(root, options), end; it != end; ++it) {
      std::error_code ec;
      const auto status = it->symlink_status(ec);
      if (ec) continue;
      if (!fs::is_regular_file(status) && !fs::is_symlink(status)) continue;
      add_model(it->path(), origin);
    }
  };

  auto scan_hf_cache = [&](const fs::path& cache_root) {
    if (!fs::exists(cache_root)) return;
    fs::directory_options options = fs::directory_options::skip_permission_denied;
    for (fs::directory_iterator it(cache_root, options), end; it != end; ++it) {
      std::error_code ec;
      if (!it->is_directory(ec)) continue;
      const std::string name = it->path().filename().string();
      if (name.rfind("models--", 0) != 0) continue;
      scan_tree(it->path() / "snapshots", "huggingface-cache");
    }
  };

  scan_hf_cache(util::DefaultHfCacheRoot());
  scan_tree(app_models_dir_, "project-models");

  std::sort(models.begin(), models.end(),
            [](const LocalModel& a, const LocalModel& b) {
              return a.name < b.name;
            });
  return models;
}

}  // namespace forge
