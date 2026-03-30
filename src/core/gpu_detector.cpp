#include "core/gpu_detector.hpp"

#include "core/platform_utils.hpp"
#include "core/string_utils.hpp"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")

#ifdef HAS_VULKAN
#include <vulkan/vulkan.h>
#pragma comment(lib, "vulkan-1.lib")
#endif

// NVML (NVIDIA Management Library) - dynamically loaded
typedef enum {
  NVML_SUCCESS = 0,
  NVML_ERROR_FUNCTION_NOT_FOUND = 14,
  NVML_ERROR_UNKNOWN = 999
} nvmlReturn_t;

typedef struct nvmlDevice_st* nvmlDevice_t;
typedef struct nvmlMemory_st {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
} nvmlMemory_t;

#elif defined(__APPLE__)
// macOS - Check for Metal/Apple Silicon
#else
// Linux
#ifdef HAS_VULKAN
#include <vulkan/vulkan.h>
#endif
#endif

namespace forge {

GpuInfo GpuDetector::Detect() {
  GpuInfo info;
  
#ifdef _WIN32
  // Windows detection stack
  
  // Layer 1: NVML (most accurate for NVIDIA)
  if (DetectNvidiaNVML(info)) {
    return info;
  }
  
  // Layer 2: DXGI (Windows native)
  if (DetectDxgi(info)) {
    return info;
  }
  
  // Layer 3: Vulkan fallback
#ifdef HAS_VULKAN
  if (DetectVulkanGpu(info)) {
    return info;
  }
#endif
  
  // Layer 4: Legacy methods
  if (DetectNvidiaSmi(info)) {
    return info;
  }
  
  if (DetectAmdWindows(info)) {
    return info;
  }
  
#elif defined(__APPLE__)
  // macOS detection
  
  // Check for Apple Silicon
  if (DetectMetal(info)) {
    return info;
  }
  
  // Intel Mac with AMD GPU
  if (DetectAmdMacOS(info)) {
    return info;
  }
  
#else
  // Linux detection stack (CUDA is king on Linux!)
  
  // Layer 1: NVML for NVIDIA (best performance on Linux)
  if (DetectNvidiaNVML(info)) {
    return info;
  }
  
  // Layer 2: nvidia-smi CLI (also NVIDIA)
  if (DetectNvidiaSmi(info)) {
    return info;
  }
  
  // Layer 3: ROCm for AMD (good alternative)
  if (DetectRocm(info)) {
    return info;
  }
  
  // Layer 4: Vulkan fallback (universal but slower than CUDA)
#ifdef HAS_VULKAN
  if (DetectVulkanGpu(info)) {
    return info;
  }
#endif
#endif
  
  // No GPU detected
  info.backend = GpuBackend::Cpu;
  info.name = "CPU Only";
  return info;
}

#ifdef _WIN32
bool GpuDetector::DetectNvidiaNVML(GpuInfo& info) {
  // Try to load NVML dynamically
  HMODULE nvml = LoadLibraryA("nvml.dll");
  if (!nvml) {
    // Try alternative path
    nvml = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!nvml) {
      return false;
    }
  }
  
  auto nvmlInit = (nvmlReturn_t (*)())GetProcAddress(nvml, "nvmlInit");
  auto nvmlShutdown = (nvmlReturn_t (*)())GetProcAddress(nvml, "nvmlShutdown");
  auto nvmlDeviceGetHandleByIndex = (nvmlReturn_t (*)(int, nvmlDevice_t*))GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex");
  auto nvmlDeviceGetMemoryInfo = (nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*))GetProcAddress(nvml, "nvmlDeviceGetMemoryInfo");
  auto nvmlDeviceGetName = (nvmlReturn_t (*)(nvmlDevice_t, char*, int))GetProcAddress(nvml, "nvmlDeviceGetName");
  auto nvmlSystemGetDriverVersion = (nvmlReturn_t (*)(char*, int))GetProcAddress(nvml, "nvmlSystemGetDriverVersion");
  
  if (!nvmlInit || !nvmlShutdown || !nvmlDeviceGetHandleByIndex || 
      !nvmlDeviceGetMemoryInfo || !nvmlDeviceGetName || !nvmlSystemGetDriverVersion) {
    FreeLibrary(nvml);
    return false;
  }
  
  if (nvmlInit() != NVML_SUCCESS) {
    FreeLibrary(nvml);
    return false;
  }
  
  nvmlDevice_t device;
  if (nvmlDeviceGetHandleByIndex(0, &device) != NVML_SUCCESS) {
    nvmlShutdown();
    FreeLibrary(nvml);
    return false;
  }
  
  // Get GPU name
  char name[256];
  if (nvmlDeviceGetName(device, name, sizeof(name)) == NVML_SUCCESS) {
    info.name = name;
  }
  
  // Get VRAM
  nvmlMemory_t memory;
  if (nvmlDeviceGetMemoryInfo(device, &memory) == NVML_SUCCESS) {
    info.vram_bytes = memory.total;
  }
  
  // Get driver version
  char driver_version[256];
  if (nvmlSystemGetDriverVersion(driver_version, sizeof(driver_version)) == NVML_SUCCESS) {
    info.driver_version = driver_version;
    // Parse CUDA version from driver
    // Driver 552.22+ supports CUDA 12.4
    // Driver 560+ supports CUDA 13.1
    int major_version = 0;
    sscanf(driver_version, "%d", &major_version);
    if (major_version >= 560) {
      info.cuda_version = "13.1";
    } else if (major_version >= 552) {
      info.cuda_version = "12.4";
    } else {
      info.cuda_version = "11.8";
    }
  }
  
  info.backend = GpuBackend::Cuda;
  
  nvmlShutdown();
  FreeLibrary(nvml);
  return true;
}

bool GpuDetector::DetectDxgi(GpuInfo& info) {
  IDXGIFactory1* factory = nullptr;
  
  if (CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory) != S_OK) {
    return false;
  }
  
  IDXGIAdapter1* adapter = nullptr;
  bool found = false;
  
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    if (adapter->GetDesc1(&desc) != S_OK) {
      adapter->Release();
      continue;
    }
    
    // Skip software adapters (Microsoft Basic Render)
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      adapter->Release();
      continue;
    }
    
    // Prefer dedicated GPUs
    if (desc.DedicatedVideoMemory > 0) {
      std::wstring wname(desc.Description);
      info.name = std::string(wname.begin(), wname.end());
      info.vram_bytes = desc.DedicatedVideoMemory;
      
      // Determine vendor from VendorId
      if (desc.VendorId == 0x10DE) {
        info.backend = GpuBackend::Cuda;  // NVIDIA
        info.vendor_id = 0x10DE;
      } else if (desc.VendorId == 0x1002) {
        info.backend = GpuBackend::Vulkan;  // AMD
        info.vendor_id = 0x1002;
      } else if (desc.VendorId == 0x8086) {
        info.backend = GpuBackend::Vulkan;  // Intel
        info.vendor_id = 0x8086;
      } else {
        info.backend = GpuBackend::Vulkan;  // Unknown, use Vulkan
        info.vendor_id = desc.VendorId;
      }
      
      found = true;
      adapter->Release();
      break;
    }
    
    adapter->Release();
  }
  
  factory->Release();
  return found;
}

#ifdef HAS_VULKAN
bool GpuDetector::DetectVulkanGpu(GpuInfo& info) {
  VkInstance instance;
  VkInstanceCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  
  if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
    return false;
  }
  
  uint32_t device_count = 0;
  if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
    vkDestroyInstance(instance, nullptr);
    return false;
  }
  
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
  
  // Get first discrete GPU if available
  VkPhysicalDevice selected_device = devices[0];
  for (const auto& device : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      selected_device = device;
      break;
    }
  }
  
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(selected_device, &props);
  
  info.name = props.deviceName;
  info.vram_bytes = props.limits.maxMemoryAllocationSize;
  
  if (props.vendorID == 0x10DE) {
    info.backend = GpuBackend::Vulkan;
    info.vendor_id = 0x10DE;
  } else if (props.vendorID == 0x1002) {
    info.backend = GpuBackend::Vulkan;
    info.vendor_id = 0x1002;
  } else if (props.vendorID == 0x8086) {
    info.backend = GpuBackend::Vulkan;
    info.vendor_id = 0x8086;
  }
  
  vkDestroyInstance(instance, nullptr);
  return true;
}
#endif

bool GpuDetector::DetectNvidiaSmi(GpuInfo& info) {
  // Try multiple paths for nvidia-smi
  std::vector<std::string> paths = {
    "nvidia-smi",
    "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe",
    "C:\\Windows\\System32\\nvidia-smi.exe"
  };
  
  for (const auto& path : paths) {
    auto probe = util::RunCommandCapture({path, "--query-gpu=name", "--format=csv,noheader"});
    if (probe.exit_code == 0 && !util::Trim(probe.output).empty()) {
      info.name = util::Trim(probe.output);
      info.backend = GpuBackend::Cuda;
      
      // Get driver version
      auto driver_probe = util::RunCommandCapture({path, "--query-gpu=driver_version", "--format=csv,noheader"});
      if (driver_probe.exit_code == 0) {
        info.driver_version = util::Trim(driver_probe.output);
        int major_version = 0;
        sscanf(info.driver_version.c_str(), "%d", &major_version);
        if (major_version >= 560) {
          info.cuda_version = "13.1";
        } else if (major_version >= 552) {
          info.cuda_version = "12.4";
        } else {
          info.cuda_version = "11.8";
        }
      }
      
      return true;
    }
  }
  
  return false;
}
#endif

bool GpuDetector::DetectAmdWindows(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"wmic", "path", "win32_VideoController", "get", "name", "/format:csv"});
  if (probe.exit_code == 0 && probe.output.find("Radeon") != std::string::npos) {
    info.name = "AMD GPU";
    info.backend = GpuBackend::Vulkan;
    return true;
  }
  return false;
}

bool GpuDetector::DetectAmdMacOS(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"system_profiler", "SPDisplaysDataType"});
  if (probe.exit_code == 0 && probe.output.find("Radeon") != std::string::npos) {
    info.name = "AMD Radeon";
    info.backend = GpuBackend::Metal;
    return true;
  }
  return false;
}

bool GpuDetector::DetectRocm(GpuInfo& info) {
  // Try rocm-smi first
  auto probe = util::RunCommandCapture({"rocm-smi", "--showproductname"});
  if (probe.exit_code == 0 && !util::Trim(probe.output).empty()) {
    info.name = util::Trim(probe.output);
    info.backend = GpuBackend::Rocm;
    return true;
  }
  
  // Fallback: check /sys/class/drm for AMD GPUs
  if (util::RunCommandCapture({"test", "-d", "/sys/class/drm"}).exit_code == 0) {
    auto card_probe = util::RunCommandCapture({"ls", "/sys/class/drm/"});
    if (card_probe.exit_code == 0 && card_probe.output.find("card") != std::string::npos) {
      auto vendor_probe = util::RunCommandCapture({"cat", "/sys/class/drm/card0/device/vendor"});
      if (vendor_probe.exit_code == 0 && vendor_probe.output.find("0x1002") != std::string::npos) {
        info.name = "AMD GPU (Linux)";
        info.backend = GpuBackend::Rocm;
        return true;
      }
    }
  }
  
  return false;
}

bool GpuDetector::DetectMetal(GpuInfo& info) {
  auto probe = util::RunCommandCapture({"system_profiler", "SPDisplaysDataType"});
  if (probe.exit_code == 0) {
    std::string output = probe.output;
    // Check for Apple Silicon
    if (output.find("Apple M1") != std::string::npos ||
        output.find("Apple M2") != std::string::npos ||
        output.find("Apple M3") != std::string::npos ||
        output.find("Apple M4") != std::string::npos) {
      info.name = "Apple Silicon";
      info.backend = GpuBackend::Metal;
      return true;
    }
  }
  return false;
}

}  // namespace forge
