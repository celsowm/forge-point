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

// Build a set of candidate asset names for the current platform + GPU backend.
// Actual naming convention from llama.cpp releases:
//   Windows:  llama-{tag}-bin-win-{variant}-{arch}.zip
//   macOS:    llama-{tag}-bin-macos-{arch}.tar.gz
//   Linux:    llama-{tag}-bin-ubuntu-{variant}-{arch}.tar.gz
//   CUDA DLLs (Windows only): cudart-llama-bin-win-cuda-{ver}-x64.zip
std::vector<std::string> LlamaDownloader::BuildCandidates(
    const std::string& tag, const GpuInfo& gpu) const {
  std::vector<std::string> c;

#ifdef _WIN32
  // ── Windows ──────────────────────────────────────────────────────────────
  const std::string ext = ".zip";

  switch (gpu.backend) {
    case GpuBackend::Cuda: {
      std::string cuda = "12.4";
      if (!gpu.cuda_version.empty()) {
        cuda = gpu.cuda_version;
      } else if (!gpu.driver_version.empty()) {
        int major = 0;
        sscanf(gpu.driver_version.c_str(), "%d", &major);
        if (major >= 560)
          cuda = "13.1";
        else if (major >= 552)
          cuda = "12.4";
      }
      c.push_back("llama-" + tag + "-bin-win-cuda-" + cuda + "-x64" + ext);
      // Also offer the CUDA runtime DLLs as a candidate (separate download)
      c.push_back("cudart-llama-bin-win-cuda-" + cuda + "-x64" + ext);
      break;
    }
    case GpuBackend::Vulkan:
      c.push_back("llama-" + tag + "-bin-win-vulkan-x64" + ext);
      break;
    case GpuBackend::Sycl:
      c.push_back("llama-" + tag + "-bin-win-sycl-x64" + ext);
      break;
    case GpuBackend::Rocm:
      c.push_back("llama-" + tag + "-bin-win-hip-radeon-x64" + ext);
      break;
    case GpuBackend::OpenCL:
      c.push_back("llama-" + tag + "-bin-win-opencl-adreno-arm64" + ext);
      break;
    case GpuBackend::Cpu:
    case GpuBackend::None:
    default:
      c.push_back("llama-" + tag + "-bin-win-cpu-x64" + ext);
      c.push_back("llama-" + tag + "-bin-win-cpu-arm64" + ext);
      break;
  }

#elif defined(__APPLE__)
  // ── macOS ────────────────────────────────────────────────────────────────
  const std::string ext = ".tar.gz";

  auto arch_probe = util::RunCommandCapture({"uname", "-m"});
  bool is_arm = (arch_probe.exit_code == 0 &&
                 util::Trim(arch_probe.output).find("arm") != std::string::npos);

  if (is_arm) {
    c.push_back("llama-" + tag + "-bin-macos-arm64" + ext);
  } else {
    c.push_back("llama-" + tag + "-bin-macos-x64" + ext);
  }

#else
  // ── Linux ────────────────────────────────────────────────────────────────
  const std::string ext = ".tar.gz";

  auto arch_probe = util::RunCommandCapture({"uname", "-m"});
  bool is_arm = (arch_probe.exit_code == 0 &&
                 util::Trim(arch_probe.output).find("aarch64") != std::string::npos);

  if (is_arm) {
    // No pre-built ARM64 Linux binaries currently offered
    c.push_back("llama-" + tag + "-bin-ubuntu-x64" + ext);
  } else if (gpu.backend == GpuBackend::Cuda) {
    std::string cuda = "12.4";
    if (!gpu.cuda_version.empty()) {
      cuda = gpu.cuda_version;
    } else if (!gpu.driver_version.empty()) {
      int major = 0;
      sscanf(gpu.driver_version.c_str(), "%d", &major);
      if (major >= 560)
        cuda = "13.1";
      else if (major >= 552)
        cuda = "12.4";
    }
    c.push_back("llama-" + tag + "-bin-ubuntu-cuda-" + cuda + "-x64" + ext);
  } else if (gpu.backend == GpuBackend::Rocm) {
    c.push_back("llama-" + tag + "-bin-ubuntu-rocm-7.2-x64" + ext);
  } else if (gpu.backend == GpuBackend::Vulkan) {
    c.push_back("llama-" + tag + "-bin-ubuntu-vulkan-x64" + ext);
  } else {
    // CPU or unknown
    c.push_back("llama-" + tag + "-bin-ubuntu-x64" + ext);
  }
#endif

  return c;
}

std::optional<std::string> LlamaDownloader::FindAssetForPlatform(
    const LlamaRelease& release, const GpuInfo& gpu) const {
  auto& debug = DebugOverlay::Instance();
  debug.Debug("FindAssetForPlatform backend=" +
                  std::to_string(static_cast<int>(gpu.backend)) +
                  " gpu=" + gpu.name,
              "Downloader");

  auto candidates = BuildCandidates(release.tag, gpu);

  debug.Debug("Candidates: " + std::to_string(candidates.size()), "Downloader");
  for (size_t i = 0; i < candidates.size(); ++i) {
    debug.Debug("  [" + std::to_string(i) + "] " + candidates[i], "Downloader");
  }

  // Match against actual assets in the release
  for (const auto& candidate : candidates) {
    for (const auto& asset : release.assets) {
      if (asset == candidate) {
        debug.Debug("Matched: " + candidate, "Downloader");
        return candidate;
      }
    }
  }

  debug.Debug("No matching asset found", "Downloader");
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
