#pragma once

#include "interfaces.hpp"

namespace forge {

class GpuDetector : public IGpuDetector {
 public:
  GpuInfo Detect() override;

 private:
  static bool DetectNvidia(GpuInfo& info);
  static bool DetectAmdWindows(GpuInfo& info);
  static bool DetectRocm(GpuInfo& info);
  static bool DetectMetal(GpuInfo& info);
};

}  // namespace forge
