#pragma once

#include "interfaces.hpp"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace forge {

class HfCacheScanner : public ILocalModelScanner {
 public:
  explicit HfCacheScanner(fs::path app_models_dir);

  fs::path CacheRoot() const override;
  std::vector<LocalModel> Scan() const override;

 private:
  fs::path app_models_dir_;
};

}  // namespace forge
