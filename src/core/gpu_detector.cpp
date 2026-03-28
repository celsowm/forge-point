#include "core/gpu_detector.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"

namespace forge {

GpuInfo GpuDetector::Detect() {
  GpuInfo info;
#ifdef _WIN32
  if (DetectNvidia(info)) return info;
  if (DetectAmdWindows(info)) return info;
  info.backend = GpuBackend::Cpu;
  return info;
#else
  if (DetectNvidia(info)) return info;
  if (DetectRocm(info)) return info;
  if (DetectMetal(info)) return info;
  info.backend = GpuBackend::Cpu;
  return info;
#endif
}

bool GpuDetector::DetectNvidia(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"nvidia-smi", "--query-gpu=name", "--format=csv,noheader"});
  if (probe.exit_code == 0 && !util::Trim(probe.output).empty()) {
    info.name = util::Trim(probe.output);
    info.backend = GpuBackend::Cuda;
    auto ver_probe = util::RunCommandCapture({"nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"});
    if (ver_probe.exit_code == 0) {
      std::string ver = util::Trim(ver_probe.output);
      if (ver.rfind("13.", 0) == 0) {
        info.cuda_version = "13.1";
      } else if (ver.rfind("12.", 0) == 0) {
        info.cuda_version = "12.4";
      }
    }
    return true;
  }
  return false;
}

bool GpuDetector::DetectAmdWindows(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"wmic", "path", "win32_VideoController", "get", "name", "/format:csv"});
  if (probe.exit_code == 0 && probe.output.find("Radeon") != std::string::npos) {
    info.name = "AMD GPU";
    info.backend = GpuBackend::Vulkan;
    return true;
  }
  return false;
}

bool GpuDetector::DetectRocm(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"rocm-smi", "--showproductname"});
  if (probe.exit_code == 0 && !util::Trim(probe.output).empty()) {
    info.name = util::Trim(probe.output);
    info.backend = GpuBackend::Rocm;
    return true;
  }
  return false;
}

bool GpuDetector::DetectMetal(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"system_profiler", "SPDisplaysDataType"});
  if (probe.exit_code == 0 && probe.output.find("Apple") != std::string::npos) {
    info.name = "Apple Silicon";
    info.backend = GpuBackend::Metal;
    return true;
  }
  return false;
}

}  // namespace forge
