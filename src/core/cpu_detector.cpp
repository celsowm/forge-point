#include "core/cpu_detector.hpp"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <unistd.h>
#include <sys/sysctl.h>
#endif

namespace forge {

#ifdef _WIN32
struct CpuIdResult {
  int eax, ebx, ecx, edx;
};

static CpuIdResult cpuid(int func, int subfunc = 0) {
  CpuIdResult result;
  int regs[4];
  __cpuidex(regs, func, subfunc);
  result.eax = regs[0];
  result.ebx = regs[1];
  result.ecx = regs[2];
  result.edx = regs[3];
  return result;
}
#endif

CpuInfo CpuDetector::Detect() {
  CpuInfo info;
  
#ifdef _WIN32
  // Get brand string
  int cpu_info[4];
  __cpuid(cpu_info, 0x80000000);
  if (cpu_info[0] >= 0x80000004) {
    char brand[49] = {0};
    __cpuid(cpu_info, 0x80000002);
    memcpy(brand, cpu_info, 16);
    __cpuid(cpu_info, 0x80000003);
    memcpy(brand + 16, cpu_info, 16);
    __cpuid(cpu_info, 0x80000004);
    memcpy(brand + 32, cpu_info, 16);
    info.brand = brand;
  }
  
  // Get CPU features
  auto features = cpuid(1);
  auto extended_features = cpuid(7, 0);
  
  // Determine instruction set support
  bool has_avx = (features.ecx & (1 << 28)) != 0;
  bool has_avx2 = (extended_features.ebx & (1 << 5)) != 0;
  bool has_avx512f = (extended_features.ebx & (1 << 16)) != 0;
  bool has_avx512bw = (extended_features.ebx & (1 << 30)) != 0;
  bool has_avx512vl = (extended_features.ebx & (1 << 31)) != 0;
  
  if (has_avx512f && has_avx512bw && has_avx512vl) {
    info.instruction_set = "avx512";
  } else if (has_avx2) {
    info.instruction_set = "avx2";
  } else if (has_avx) {
    info.instruction_set = "avx";
  } else {
    info.instruction_set = "generic";
  }
  
  // Get cache info via CPUID
  auto cache_info = cpuid(4, 0);
  if ((cache_info.eax & 0x1F) >= 3) {
    int cache_type = cache_info.eax & 0x1F;
    if (cache_type == 3) {
      int cache_sets = ((cache_info.eax >> 22) & 0x3FF) + 1;
      int cache_associativity = ((cache_info.ebx >> 22) & 0x3FF) + 1;
      int cache_line_size = (cache_info.ebx & 0xFFF) + 1;
      int cache_partitions = ((cache_info.ebx >> 12) & 0x3FF) + 1;
      info.l3_cache_size = static_cast<uint64_t>(cache_sets) * 
                           cache_associativity * 
                           cache_line_size * 
                           cache_partitions;
    }
  }
  
  // Get core/thread count
  auto topology = cpuid(1);
  info.num_threads = ((topology.ebx >> 16) & 0xFF);
  
  auto extended_topology = cpuid(0x0B, 1);
  if (extended_topology.eax > 0) {
    info.num_cores = extended_topology.ebx & 0xFFFF;
  } else {
    info.num_cores = info.num_threads / 2;
  }
  
#elif defined(__APPLE__)
  // macOS/Apple Silicon detection
  char buffer[256];
  size_t buffer_size = sizeof(buffer);
  
  // Get brand
  if (sysctlbyname("machdep.cpu.brand_string", &buffer, &buffer_size, nullptr, 0) == 0) {
    info.brand = buffer;
  } else {
    info.brand = "Apple Silicon";
  }
  
  // Check for ARM features (Apple Silicon supports NEON which is equivalent to AVX)
  info.instruction_set = "neon";  // ARM equivalent
  
  // Get L3 cache
  buffer_size = sizeof(buffer);
  if (sysctlbyname("hw.l3cachesize", &buffer, &buffer_size, nullptr, 0) == 0) {
    info.l3_cache_size = *reinterpret_cast<uint64_t*>(buffer);
  }
  
  // Get core count
  buffer_size = sizeof(buffer);
  if (sysctlbyname("hw.physicalcpu", &buffer, &buffer_size, nullptr, 0) == 0) {
    info.num_cores = *reinterpret_cast<int*>(buffer);
  }
  
  buffer_size = sizeof(buffer);
  if (sysctlbyname("hw.logicalcpu", &buffer, &buffer_size, nullptr, 0) == 0) {
    info.num_threads = *reinterpret_cast<int*>(buffer);
  }
#else
  // Linux detection
  info.instruction_set = "generic";
  
  // Read /proc/cpuinfo
  FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
  if (cpuinfo) {
    char line[256];
    while (fgets(line, sizeof(line), cpuinfo)) {
      if (strncmp(line, "model name", 10) == 0) {
        char* sep = strchr(line, ':');
        if (sep) {
          info.brand = std::string(sep + 2);
          // Remove newline
          if (!info.brand.empty() && info.brand.back() == '\n') {
            info.brand.pop_back();
          }
        }
      }
      if (strncmp(line, "flags", 5) == 0) {
        std::string flags(line);
        if (flags.find("avx512f") != std::string::npos && 
            flags.find("avx512bw") != std::string::npos) {
          info.instruction_set = "avx512";
        } else if (flags.find("avx2") != std::string::npos) {
          info.instruction_set = "avx2";
        } else if (flags.find("avx") != std::string::npos) {
          info.instruction_set = "avx";
        }
      }
    }
    fclose(cpuinfo);
  }
  
  // Get core count
  info.num_cores = sysconf(_SC_NPROCESSORS_ONLN);
  info.num_threads = info.num_cores;
#endif
  
  return info;
}

std::string CpuDetector::GetInstructionSet() {
#ifdef _WIN32
  auto features = cpuid(1);
  auto extended_features = cpuid(7, 0);
  
  bool has_avx = (features.ecx & (1 << 28)) != 0;
  bool has_avx2 = (extended_features.ebx & (1 << 5)) != 0;
  bool has_avx512f = (extended_features.ebx & (1 << 16)) != 0;
  bool has_avx512bw = (extended_features.ebx & (1 << 30)) != 0;
  bool has_avx512vl = (extended_features.ebx & (1 << 31)) != 0;
  
  if (has_avx512f && has_avx512bw && has_avx512vl) {
    return "avx512";
  } else if (has_avx2) {
    return "avx2";
  } else if (has_avx) {
    return "avx";
  }
#elif defined(__APPLE__)
  return "neon";
#else
  // Linux - check /proc/cpuinfo
  FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
  if (cpuinfo) {
    char line[256];
    while (fgets(line, sizeof(line), cpuinfo)) {
      if (strncmp(line, "flags", 5) == 0) {
        std::string flags(line);
        if (flags.find("avx512f") != std::string::npos) {
          fclose(cpuinfo);
          return "avx512";
        } else if (flags.find("avx2") != std::string::npos) {
          fclose(cpuinfo);
          return "avx2";
        } else if (flags.find("avx") != std::string::npos) {
          fclose(cpuinfo);
          return "avx";
        }
      }
    }
    fclose(cpuinfo);
  }
#endif
  return "generic";
}

uint64_t CpuDetector::GetL3CacheSize() {
#ifdef _WIN32
  auto cache_info = cpuid(4, 3);
  if ((cache_info.eax & 0x1F) == 3) {
    int cache_sets = ((cache_info.eax >> 22) & 0x3FF) + 1;
    int cache_associativity = ((cache_info.ebx >> 22) & 0x3FF) + 1;
    int cache_line_size = (cache_info.ebx & 0xFFF) + 1;
    int cache_partitions = ((cache_info.ebx >> 12) & 0x3FF) + 1;
    return static_cast<uint64_t>(cache_sets) * 
           cache_associativity * 
           cache_line_size * 
           cache_partitions;
  }
#elif defined(__APPLE__)
  char buffer[256];
  size_t buffer_size = sizeof(buffer);
  if (sysctlbyname("hw.l3cachesize", &buffer, &buffer_size, nullptr, 0) == 0) {
    return *reinterpret_cast<uint64_t*>(buffer);
  }
#endif
  return 0;
}

}  // namespace forge
