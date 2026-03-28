#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace forge {

// ─── Shared types ───────────────────────────────────────────────────────────

struct HttpResponse {
  long status = 0;
  std::string body;
  std::string error;
};

using ProgressFn = std::function<void(uint64_t downloaded, uint64_t total)>;

struct HfRepo {
  std::string id;
  std::string sha;
  int downloads = 0;
  int likes = 0;
};

struct HfFile {
  std::string filename;
  std::uintmax_t size = 0;
  std::string download_url;
};

struct LocalModel {
  std::string name;
  fs::path path;
  std::uintmax_t size = 0;
  std::string origin;
};

enum class GpuBackend { None, Cpu, Cuda, Vulkan, Rocm, Sycl, Metal, OpenCL };

struct GpuInfo {
  GpuBackend backend = GpuBackend::None;
  std::string name;
  std::string cuda_version;
};

struct LlamaRelease {
  std::string tag;
  std::vector<std::string> assets;
};

// ─── Interfaces ─────────────────────────────────────────────────────────────

class IHttpClient {
 public:
  virtual ~IHttpClient() = default;
  virtual HttpResponse Get(const std::string& url,
                           const std::vector<std::string>& headers = {}) const = 0;
  virtual bool DownloadToFile(const std::string& url, const fs::path& target,
                              std::string& error,
                              const std::vector<std::string>& headers = {},
                              ProgressFn on_progress = nullptr) const = 0;
};

class IProcess {
 public:
  using LogFn = std::function<void(const std::string&)>;
  virtual ~IProcess() = default;
  virtual bool Start(const std::vector<std::string>& argv, LogFn log_fn) = 0;
  virtual void Stop() = 0;
  virtual bool Running() const = 0;
};

class IModelRepository {
 public:
  virtual ~IModelRepository() = default;
  virtual std::vector<HfRepo> SearchRepos(const std::string& query,
                                          std::string& error) = 0;
  virtual std::vector<HfFile> ListGgufFiles(const std::string& repo_id,
                                            std::string& repo_sha,
                                            std::string& error) = 0;
  virtual bool DownloadFile(const std::string& url, const fs::path& target,
                            std::string& error,
                            ProgressFn on_progress = nullptr) = 0;
};

class ILocalModelScanner {
 public:
  virtual ~ILocalModelScanner() = default;
  virtual fs::path CacheRoot() const = 0;
  virtual std::vector<LocalModel> Scan() const = 0;
};

class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual void Log(const std::string& message) = 0;
};

class IGpuDetector {
 public:
  virtual ~IGpuDetector() = default;
  virtual GpuInfo Detect() = 0;
};

}  // namespace forge
