#pragma once

#include <string>
#include <cstdint>

namespace forge {

struct CpuInfo {
  std::string brand;
  std::string instruction_set;  // avx512, avx2, avx, generic
  uint64_t l1_cache_size = 0;
  uint64_t l2_cache_size = 0;
  uint64_t l3_cache_size = 0;
  int num_cores = 0;
  int num_threads = 0;
};

class CpuDetector {
 public:
  static CpuInfo Detect();
  static std::string GetInstructionSet();
  static uint64_t GetL3CacheSize();
};

}  // namespace forge
