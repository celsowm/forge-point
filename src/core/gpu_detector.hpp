#pragma once

#include "interfaces.hpp"

namespace forge {

class GpuDetector : public IGpuDetector {
 public:
  GpuInfo Detect() override;

 private:
  // Layer 1: NVML (NVIDIA Management Library) - most accurate for NVIDIA
  static bool DetectNvidiaNVML(GpuInfo& info);
  
  // Layer 2: DXGI (DirectX Graphics Infrastructure) - Windows native
  static bool DetectDxgi(GpuInfo& info);
  
  // Layer 3: Vulkan - cross-platform fallback
#ifdef HAS_VULKAN
  static bool DetectVulkanGpu(GpuInfo& info);
#endif
  
  // Layer 4: Legacy methods (nvidia-smi CLI, WMI, etc.)
  static bool DetectNvidiaSmi(GpuInfo& info);
  static bool DetectAmdWindows(GpuInfo& info);
  static bool DetectAmdMacOS(GpuInfo& info);
  static bool DetectRocm(GpuInfo& info);
  static bool DetectMetal(GpuInfo& info);
};

}  // namespace forge
