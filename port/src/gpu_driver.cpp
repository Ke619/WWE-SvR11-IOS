// WWE SmackDown vs. Raw 2011 - custom GPU drivers on Android (gpu_driver.h).
//
// The launcher keeps driver packages (Mesa Turnip and the like: a zip with
// meta.json and the driver's .so) in the app's own storage and writes the
// chosen one's path to gpu_driver. When the SDK creates its Vulkan instance
// it asks this opener for the loader: libadrenotools loads the phone's
// libvulkan in a namespace where it loads that driver instead of the phone's
// (Android's loader keeps doing the swapchain). The hook libraries are in the
// app's native library folder (GameActivity passes it as
// SVR2011_NATIVE_LIB_DIR). Any failure: the phone's own driver - and a note
// (gpu_driver_failed.txt in the app's cache) the launcher shows the player.
//
// A driver that loads but never gets the first frame out (Turnip on an Adreno
// 610 stopped at the first pipelines, with nothing in the log): after 60 s
// without a finished frame the log says so and gpu_stuck.txt (cache) tells
// the launcher, which offers the phone's own driver or says the GPU may be
// unsupported. A frame after all takes the note back.
//
// gpu_driver_env: driver variables set before the driver loads, "NAME=value"
// separated by ';' - e.g. Turnip's FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1
// (graphical glitches on HyperOS 3). A gpu_driver outside the app's storage
// (a .so in Download, set by hand) is copied into the app's cache first:
// Android only loads code from there.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <rex/external_frame.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if SVR2011_ADRENOTOOLS
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <adrenotools/driver.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#endif

REXCVAR_DEFINE_STRING(gpu_driver, "", "GPU",
                      "Android: a custom Vulkan driver's .so (the launcher's Settings set it); empty: the phone's own");
REXCVAR_DEFINE_STRING(gpu_driver_env, "", "GPU",
                      "Android: driver variables set before the GPU driver loads, NAME=value separated by ';' "
                      "(e.g. FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1 for HyperOS 3)");

// (rex/ui/vulkan/instance.h, declared here: that header needs RenderDoc's)
namespace rex::ui::vulkan {
using LoaderOpener = void* (*)();
void SetLoaderOpener(LoaderOpener opener);
}  // namespace rex::ui::vulkan

namespace svr2011 {
namespace {

#if SVR2011_ADRENOTOOLS
// Can the driver make a Vulkan instance? (Some load and then fail there - a
// Qualcomm driver for another GPU: ErrorOutOfHostMemory. The game would be
// left without a picture.)
VkResult ProbeDriver(void* handle) {
  auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
  if (!get_proc) return VK_ERROR_INITIALIZATION_FAILED;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr, "vkCreateInstance"));
  if (!create) return VK_ERROR_INITIALIZATION_FAILED;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult result = create(&info, nullptr, &instance);
  if (result != VK_SUCCESS) return result;
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
  if (destroy) destroy(instance, nullptr);
  return VK_SUCCESS;
}

// The launcher's note: this driver didn't work (it offers the phone's own back).
void NoteFailure(const std::string& path, const char* why) {
  const char* cache = std::getenv("SVR2011_CACHE_DIR");
  if (!cache) return;
  if (FILE* f = std::fopen((std::string(cache) + "/gpu_driver_failed.txt").c_str(), "w")) {
    std::fprintf(f, "%s\n%s\n", path.c_str(), why);
    std::fclose(f);
  }
}
#endif

#if SVR2011_ADRENOTOOLS
// gpu_driver_env: each NAME=value (split at the first '='), before any driver loads.
void ApplyDriverEnv() {
  const std::string env = REXCVAR_GET(gpu_driver_env);
  size_t pos = 0;
  while (pos <= env.size()) {
    size_t end = env.find_first_of(";\n", pos);
    if (end == std::string::npos) end = env.size();
    std::string item = env.substr(pos, end - pos);
    const size_t a = item.find_first_not_of(" \t"), z = item.find_last_not_of(" \t");
    item = a == std::string::npos ? std::string() : item.substr(a, z - a + 1);
    const size_t eq = item.find('=');
    if (eq != std::string::npos && eq > 0) {
      const std::string name = item.substr(0, eq), value = item.substr(eq + 1);
      setenv(name.c_str(), value.c_str(), 1);
      REXLOG_INFO("GPU driver variable: {}={}", name, value);
    }
    pos = end + 1;
  }
}

// A driver outside the app's own storage (shared storage: Android won't load
// code from there): copied into the app's cache. The path to load, or "".
std::string LocalCopy(const std::string& path) {
  const char* cache = std::getenv("SVR2011_CACHE_DIR");
  if (!cache || (path.rfind("/storage/", 0) != 0 && path.rfind("/sdcard/", 0) != 0)) return path;
  const std::string dir = std::string(cache) + "/imported_driver/";
  mkdir(dir.c_str(), 0700);
  const std::string out = dir + path.substr(path.rfind('/') + 1);
  FILE* in = std::fopen(path.c_str(), "rb");
  FILE* to = in ? std::fopen(out.c_str(), "wb") : nullptr;
  bool ok = in && to;
  char buffer[1 << 16];
  size_t n;
  while (ok && (n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) ok = std::fwrite(buffer, 1, n, to) == n;
  if (in) std::fclose(in);
  if (to) std::fclose(to);
  if (!ok) return "";
  REXLOG_INFO("custom GPU driver {} copied to {} (Android loads drivers only from the app's storage)", path, out);
  return out;
}
#endif

void StartStuckWatch(const std::string& driver) {
  static bool started = false;
  if (started) return;
  started = true;
  std::thread([driver] {
    rex::thread::set_current_thread_name("gpu driver check");
    constexpr int kSeconds = 60;
    const auto t0 = std::chrono::steady_clock::now();
    while (rex::external_frame::SwapCount() == 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(kSeconds)) continue;
      REXLOG_ERROR("GPU: no frame {} s after the start - the graphics driver ({}) seems stuck; try another "
                   "driver (launcher: Settings, Graphics driver)",
                   kSeconds, driver.empty() ? "the phone's own" : driver);
      const char* cache = std::getenv("SVR2011_CACHE_DIR");
      const std::string note = cache ? std::string(cache) + "/gpu_stuck.txt" : std::string();
      if (!note.empty()) {
        if (FILE* f = std::fopen(note.c_str(), "w")) {
          std::fprintf(f, "%s\n%d\n", driver.c_str(), kSeconds);
          std::fclose(f);
        }
      }
      while (rex::external_frame::SwapCount() == 0) std::this_thread::sleep_for(std::chrono::seconds(1));
      REXLOG_INFO("GPU: the first frame came after all - not stuck");
      if (!note.empty()) std::remove(note.c_str());
      return;
    }
  }).detach();
}

void* OpenVulkanLoader() {
  // (before the log's settings lines: the GPU device is made first)
  REXLOG_INFO("GPU settings: mali_alpha {}, vulkan_require_geometry_shader {}, vulkan_require_fill_mode_non_solid {}",
              rex::cvar::GetFlagByName("mali_alpha"), rex::cvar::GetFlagByName("vulkan_require_geometry_shader"),
              rex::cvar::GetFlagByName("vulkan_require_fill_mode_non_solid"));
#if SVR2011_ADRENOTOOLS
  ApplyDriverEnv();
  const std::string chosen = REXCVAR_GET(gpu_driver);
  StartStuckWatch(chosen);
  if (chosen.empty()) return nullptr;
  const std::string path = access(chosen.c_str(), R_OK) == 0 ? LocalCopy(chosen) : chosen;
  if (path.empty()) {
    REXLOG_WARN("custom GPU driver {} couldn't be copied into the app's storage - the phone's own driver", chosen);
    NoteFailure(chosen, "couldn't be copied");
    return nullptr;
  }
  const size_t slash = path.rfind('/');
  const char* hooks = std::getenv("SVR2011_NATIVE_LIB_DIR");
  const char* tmp = std::getenv("SVR2011_CACHE_DIR");
  if (slash == std::string::npos || !hooks || access(path.c_str(), R_OK) != 0) {
    REXLOG_WARN("custom GPU driver {} can't be used (missing?) - the phone's own driver", path);
    NoteFailure(path, "missing");
    return nullptr;
  }
  const std::string dir = path.substr(0, slash + 1), name = path.substr(slash + 1);
  std::string hook_dir = hooks;
  if (!hook_dir.empty() && hook_dir.back() != '/') hook_dir += '/';
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp, hook_dir.c_str(), dir.c_str(),
                                            name.c_str(), nullptr, nullptr);
  if (!handle) {
    const char* error = dlerror();
    REXLOG_WARN("custom GPU driver {} didn't load: {} - the phone's own driver", name, error ? error : "?");
    NoteFailure(path, "didn't load");
    return nullptr;
  }
  if (const VkResult result = ProbeDriver(handle); result != VK_SUCCESS) {
    // (left loaded: unloading a driver half set up isn't safe; it is unused)
    REXLOG_WARN("custom GPU driver {} can't start Vulkan (VkResult {}) - the phone's own driver", name,
                static_cast<int>(result));
    NoteFailure(path, "can't start Vulkan");
    return nullptr;
  }
  REXLOG_INFO("custom GPU driver: {}", path);
  return handle;
#else
  return nullptr;
#endif
}

// (registered when the library loads: before the SDK creates its Vulkan instance)
const bool g_registered = (rex::ui::vulkan::SetLoaderOpener(&OpenVulkanLoader), true);

}  // namespace
}  // namespace svr2011
