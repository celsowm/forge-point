#include "core/llama_downloader.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"
#include "app/debug_overlay.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace forge {

LlamaDownloader::LlamaDownloader(IHttpClient& http) : http_(http) {}

std::optional<LlamaRelease> LlamaDownloader::GetLatestRelease(std::string& error) const {
  auto resp = http_.Get("https://api.github.com/repos/ggml-org/llama.cpp/releases/latest");
  if (!resp.error.empty()) {
    error = resp.error;
    return std::nullopt;
  }
  if (resp.status != 200) {
    error = "HTTP " + std::to_string(resp.status);
    return std::nullopt;
  }
  auto payload = json::parse(resp.body, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    error = "invalid JSON";
    return std::nullopt;
  }
  LlamaRelease release;
  release.tag = payload.value("tag_name", "");
  for (const auto& asset : payload.value("assets", json::array())) {
    release.assets.push_back(asset.value("name", ""));
  }
  return release;
}

std::optional<std::string> LlamaDownloader::FindAssetForPlatform(
    const LlamaRelease& release, const GpuInfo& gpu) const {
  std::vector<std::string> candidates;
  
  // Debug logging
  auto& debug = DebugOverlay::Instance();
  debug.Debug("FindAssetForPlatform called. Backend: " + std::to_string(static_cast<int>(gpu.backend)) + 
              ", Name: " + gpu.name, "Downloader");
  
#ifdef _WIN32
  // Windows binaries
  if (gpu.backend == GpuBackend::Cuda) {
    debug.Debug("CUDA backend detected, using CUDA 12.4 (pre-compiled binary)", "Downloader");
    // For pre-compiled binaries, always use CUDA 12.4 (most stable)
    // The CUDA runtime is included in the llama.cpp release
    std::string cuda = "12.4";
    candidates = {
        "llama-" + release.tag.substr(1) + "-bin-win-cuda-" + cuda + "-x64.zip",
        "llama-b8580-bin-win-cuda-" + cuda + "-x64.zip",
        "llama-b8565-bin-win-cuda-" + cuda + "-x64.zip"};
    debug.Debug("Generated CUDA candidates: " + candidates[0] + ", " + candidates[1], "Downloader");
  } else if (gpu.backend == GpuBackend::Vulkan || gpu.backend == GpuBackend::None) {
    debug.Debug("Vulkan/None backend detected", "Downloader");
    candidates = {"llama-" + release.tag.substr(1) + "-bin-win-vulkan-x64.zip",
                  "llama-b8580-bin-win-vulkan-x64.zip",
                  "llama-b8565-bin-win-vulkan-x64.zip"};
  } else if (gpu.backend == GpuBackend::Cpu) {
    debug.Debug("CPU backend detected", "Downloader");
    candidates = {"llama-" + release.tag.substr(1) + "-bin-win-cpu-x64.zip",
                  "llama-b8580-bin-win-cpu-x64.zip",
                  "llama-b8565-bin-win-cpu-x64.zip"};
  } else if (gpu.backend == GpuBackend::Sycl) {
    debug.Debug("SYCL backend detected", "Downloader");
    candidates = {"llama-" + release.tag.substr(1) + "-bin-win-sycl-x64.zip",
                  "llama-b8580-bin-win-sycl-x64.zip",
                  "llama-b8565-bin-win-sycl-x64.zip"};
  } else if (gpu.backend == GpuBackend::Rocm) {
    debug.Debug("ROCm backend detected", "Downloader");
    candidates = {"llama-" + release.tag.substr(1) + "-bin-win-hip-radeon-x64.zip",
                  "llama-b8580-bin-win-hip-radeon-x64.zip",
                  "llama-b8565-bin-win-hip-radeon-x64.zip"};
  } else {
    debug.Debug("Unknown backend: " + std::to_string(static_cast<int>(gpu.backend)), "Downloader");
  }
  
  debug.Debug("Total candidates: " + std::to_string(candidates.size()), "Downloader");
  for (size_t i = 0; i < candidates.size() && i < 3; ++i) {
    debug.Debug("  Candidate " + std::to_string(i) + ": " + candidates[i], "Downloader");
  }
  
#elif defined(__APPLE__)
  // macOS binaries
  if (gpu.backend == GpuBackend::Metal || gpu.backend == GpuBackend::None) {
    // Check architecture
    auto arch_probe = util::RunCommandCapture({"uname", "-m"});
    bool is_arm = (arch_probe.exit_code == 0 && 
                   util::Trim(arch_probe.output).find("arm") != std::string::npos);
    
    if (is_arm) {
      // Apple Silicon
      candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-arm64.tar.gz",
                    "llama-b8580-bin-macos-arm64.tar.gz",
                    "llama-b8565-bin-macos-arm64.tar.gz"};
    } else {
      // Intel Mac
      candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-x64.tar.gz",
                    "llama-b8580-bin-macos-x64.tar.gz",
                    "llama-b8565-bin-macos-x64.tar.gz"};
    }
  } else {
    // Fallback to ARM for Metal
    candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-arm64.tar.gz",
                  "llama-b8580-bin-macos-arm64.tar.gz"};
  }
  
#else
  // Linux binaries
  auto arch_probe = util::RunCommandCapture({"uname", "-m"});
  bool is_arm = (arch_probe.exit_code == 0 && 
                 util::Trim(arch_probe.output).find("aarch64") != std::string::npos);
  
  if (is_arm) {
    // ARM64 Linux
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-arm64.tar.gz",
                  "llama-b8580-bin-ubuntu-arm64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Cuda) {
    // Linux CUDA - determine version from driver
    std::string cuda = "12.4";  // default for Linux
    if (!gpu.cuda_version.empty()) {
      cuda = gpu.cuda_version;
    } else if (!gpu.driver_version.empty()) {
      // Parse driver version (Linux drivers use same versioning)
      int major_version = 0;
      sscanf(gpu.driver_version.c_str(), "%d", &major_version);
      if (major_version >= 560) {
        cuda = "13.1";
      } else if (major_version >= 552) {
        cuda = "12.4";
      } else {
        cuda = "11.8";
      }
    }
    candidates = {
        "llama-" + release.tag.substr(1) + "-bin-ubuntu-cuda-" + cuda + "-x64.tar.gz",
        "llama-b8580-bin-ubuntu-cuda-" + cuda + "-x64.tar.gz",
        "llama-b8565-bin-ubuntu-cuda-" + cuda + "-x64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Rocm) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-rocm-7.2-x64.tar.gz",
                  "llama-b8580-bin-ubuntu-rocm-7.2-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-rocm-7.2-x64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Vulkan) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-vulkan-x64.tar.gz",
                  "llama-b8580-bin-ubuntu-vulkan-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-vulkan-x64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Cpu) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                  "llama-b8580-bin-ubuntu-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-x64.tar.gz"};
  } else {
    // Default to CPU
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                  "llama-b8580-bin-ubuntu-x64.tar.gz"};
  }
  
  if (candidates.empty()) {
    // Ultimate fallback
#ifdef _WIN32
    candidates = {"llama-b8580-bin-win-cpu-x64.zip"};
#elif defined(__APPLE__)
    candidates = {"llama-b8580-bin-macos-arm64.tar.gz"};
#else
    candidates = {"llama-b8580-bin-ubuntu-x64.tar.gz"};
#endif
  }
#endif
  auto sys_probe = util::RunCommandCapture({"uname", "-m"});
  bool is_arm = sys_probe.exit_code == 0 &&
                util::Trim(sys_probe.output).find("arm") != std::string::npos;
  if (is_arm) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-arm64.tar.gz",
                  "llama-b8565-bin-macos-arm64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Metal) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-macos-arm64.tar.gz",
                  "llama-b8565-bin-macos-arm64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Cuda) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-x64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Vulkan) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-vulkan-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-vulkan-x64.tar.gz"};
  } else if (gpu.backend == GpuBackend::Rocm) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-rocm-7.2-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-rocm-7.2-x64.tar.gz"};
  } else {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-x64.tar.gz"};
  }
  
  if (candidates.empty()) {
    candidates = {"llama-" + release.tag.substr(1) + "-bin-ubuntu-x64.tar.gz",
                  "llama-b8565-bin-ubuntu-x64.tar.gz"};
  }
  for (const auto& candidate : candidates) {
    for (const auto& asset : release.assets) {
      if (asset == candidate) {
        return candidate;
      }
    }
  }
  return std::nullopt;
}

std::string LlamaDownloader::GetDownloadUrl(const std::string& tag,
                                             const std::string& asset) const {
  return "https://github.com/ggml-org/llama.cpp/releases/download/" + tag + "/" + asset;
}

bool LlamaDownloader::DownloadAndExtract(const std::string& url,
                                          const fs::path& target_dir,
                                          std::string& error,
                                          const std::function<void(std::string)>& progress,
                                          ProgressFn on_dl_progress) {
  fs::path temp = fs::temp_directory_path() / "llama-download";
  std::error_code ec;
  fs::remove_all(temp, ec);
  fs::create_directories(temp, ec);
  fs::path archive = temp / "llama.zip";
  progress("Downloading...");
  if (!http_.DownloadToFile(url, archive, error, {}, std::move(on_dl_progress))) {
    return false;
  }
  progress("Extracting...");
  fs::create_directories(target_dir, ec);
#ifdef _WIN32
  std::string cmd = "powershell -Command \"Expand-Archive -Path '" +
                    archive.string() + "' -DestinationPath '" +
                    target_dir.string() + "' -Force\"";
#else
  std::string cmd = "tar -xzf " + util::ShellQuote(archive.string()) +
                    " -C " + util::ShellQuote(target_dir.string());
#endif
  int result = std::system(cmd.c_str());
  if (result != 0) {
    error = "extraction failed";
    return false;
  }
  progress("Done!");
  return true;
}

}  // namespace forge
