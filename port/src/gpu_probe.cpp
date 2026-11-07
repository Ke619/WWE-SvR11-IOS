// WWE SmackDown vs. Raw 2011 - which graphics API this PC can draw the game
// with (gpu_probe.h).
//
// The native renderer needs, on Direct3D 12: Windows 10 2004 (build 19041,
// ID3D12Device8), feature level 11_0, resource binding tier 2 (its texture
// tables are 16384 descriptors; tier 1 GPUs - GeForce 400 / 500, the first
// D3D12 Intel ones - hold 128) and Shader Model 6.0 (DXIL). On Vulkan: a 1.1
// device (SPIR-V 1.3). Anything else draws with Direct3D 11 (feature level
// 11_0: Shader Model 5.0; 10_0 / 10_1: Shader Model 4.0).

#include "gpu_probe.h"

#include "platform.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include <fstream>

#include <fmt/format.h>

namespace svr2011 {

#if defined(_WIN32)

namespace {

using Microsoft::WRL::ComPtr;

// Empty if Direct3D 12 can run the renderer, else why not.
std::string D3D12Problem() {
  if (const uint32_t build = WindowsBuild(); build && build < 19041)
    return fmt::format("Windows build {} is older than 19041", build);
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  if (!d3d12) return "no d3d12.dll";
  std::string problem;
  auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  if (!create_device) {
    problem = "no D3D12CreateDevice";
  } else if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
             factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(&adapter)) != S_OK) {
    problem = "no DXGI adapter";
  } else {
    ComPtr<ID3D12Device> device;
    if (FAILED(create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
      problem = "no device at feature level 11_0";
    } else {
      ComPtr<ID3D12Device8> device8;
      D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
      D3D12_FEATURE_DATA_SHADER_MODEL model = {D3D_SHADER_MODEL_6_0};
      if (FAILED(device.As(&device8))) {
        problem = "no ID3D12Device8";
      } else if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
                 options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) {
        problem = "resource binding tier 1";
      } else if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
                 model.HighestShaderModel < D3D_SHADER_MODEL_6_0) {
        problem = "no Shader Model 6.0";
      }
    }
  }
  FreeLibrary(d3d12);
  return problem;
}

// Empty if a Vulkan 1.1 GPU is there, else why not.
std::string VulkanProblem() {
  HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (!loader) return "no vulkan-1.dll";
  std::string problem;
  auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
  auto create_instance =
      get_proc ? reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr, "vkCreateInstance")) : nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "svr2011 probe";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  if (!create_instance || create_instance(&info, nullptr, &instance) != VK_SUCCESS) {
    problem = "no Vulkan instance";
  } else {
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_proc(instance, "vkEnumeratePhysicalDevices"));
    auto properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_proc(instance, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
    uint32_t count = 0;
    VkPhysicalDevice devices[16];
    problem = "no Vulkan 1.1 GPU";
    if (enumerate && properties && enumerate(instance, &count, nullptr) == VK_SUCCESS) {
      count = count > 16 ? 16 : count;
      if (enumerate(instance, &count, devices) >= VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) {
          VkPhysicalDeviceProperties p = {};
          properties(devices[i], &p);
          if (p.apiVersion >= VK_API_VERSION_1_1 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            problem.clear();
            break;
          }
        }
      }
    }
    if (destroy) destroy(instance, nullptr);
  }
  FreeLibrary(loader);
  return problem;
}

// The GPU and driver the answer holds for: the high-performance adapter's
// ids and user-mode driver version, and the Windows build (cheap: no device).
std::string AdapterKey() {
  ComPtr<IDXGIFactory1> factory;
  ComPtr<IDXGIFactory6> factory6;
  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  if (SUCCEEDED(factory.As(&factory6))) {
    factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  }
  if (!adapter && factory->EnumAdapters1(0, &adapter) != S_OK) return {};
  DXGI_ADAPTER_DESC1 desc = {};
  LARGE_INTEGER umd = {};
  adapter->GetDesc1(&desc);
  adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
  return fmt::format("{:04X}:{:04X}:{:08X}:{}:{:016X}:{}", desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
                     uint64_t(umd.QuadPart), WindowsBuild());
}

std::filesystem::path g_cache_file;  // (PickGraphicsApi's - "any" was used)
std::string g_cache_key, g_picked;

}  // namespace

std::string PickGraphicsApi(const std::filesystem::path& cache_file, std::string* why, bool* cached) {
  why->clear();
  *cached = false;
  const std::string key = AdapterKey();
  g_cache_file = cache_file, g_cache_key = key;  // (and g_picked: for AvoidPickedGraphicsApi)
  if (!key.empty()) {
    std::ifstream in(cache_file);
    std::string k, api;
    if (in && std::getline(in, k) && std::getline(in, api) && k == key &&
        (api == "d3d12" || api == "vulkan" || api == "d3d11")) {
      std::getline(in, *why);
      *cached = true;
      g_picked = api;
      return api;
    }
  }
  std::string api = "d3d12";
  if (const std::string d3d12 = D3D12Problem(); !d3d12.empty()) {
    const std::string vulkan = VulkanProblem();
    *why = fmt::format("Direct3D 12 not usable ({})", d3d12);
    if (vulkan.empty()) {
      api = "vulkan";
    } else {
      *why += fmt::format(", Vulkan not usable ({})", vulkan);
      api = "d3d11";
    }
  }
  if (!key.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(cache_file.parent_path(), ec);
    std::ofstream(cache_file, std::ios::trunc) << key << '\n' << api << '\n' << *why << '\n';
  }
  g_picked = api;
  return api;
}

std::string AvoidPickedGraphicsApi(const std::string& reason) {
  if (g_cache_file.empty() || g_cache_key.empty() || (g_picked != "d3d12" && g_picked != "vulkan")) return {};
  std::string next = "d3d11", why;
  if (g_picked == "d3d12") {
    why = fmt::format("Direct3D 12 failed on this GPU and driver ({})", reason);
    if (const std::string vulkan = VulkanProblem(); vulkan.empty())
      next = "vulkan";
    else
      why += fmt::format(", Vulkan not usable ({})", vulkan);
  } else {
    why = fmt::format("Vulkan failed on this GPU and driver ({})", reason);
  }
  std::ofstream(g_cache_file, std::ios::trunc) << g_cache_key << '\n' << next << '\n' << why << '\n';
  g_picked = next;
  return next == "vulkan" ? "Vulkan" : "Direct3D 11";
}

#else

std::string PickGraphicsApi(const std::filesystem::path&, std::string* why, bool* cached) {
  why->clear();
  *cached = false;
  return "vulkan";
}

std::string AvoidPickedGraphicsApi(const std::string&) { return {}; }

#endif

}  // namespace svr2011
