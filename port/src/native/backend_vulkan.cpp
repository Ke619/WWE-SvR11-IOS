// WWE SmackDown vs. Raw 2011 - the native renderer's Vulkan backend (gpu.h).
//
// plume adopts the emulator's own Vulkan device and queue (external_frame
// VulkanDevice; see plume's VulkanExistingDevice): the frame images are
// shown by the emulator's presentation directly, and as both submit to one
// queue, submission order and a barrier on the emulator's side are all the
// synchronization the handoff needs.

#include "native/gpu.h"
#include "frame_rate.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_vulkan.h>

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/logging.h>

#include "native/textures.h"

REXCVAR_DEFINE_BOOL(mali_alpha, false, "GPU",
                    "Android: Mali GPUs (alpha) - the launcher turns it on with the emulator's geometry shader "
                    "and line fill requirements off (Mali has neither)");

REXCVAR_DEFINE_BOOL(native_compact_tables, false, "GPU",
                    "Vulkan: compact tables (each draw binds its own textures; shader constants from uniform "
                    "buffers) even with descriptor indexing - as GPUs without it (Mali) always do. A test "
                    "switch for drivers that read uniform buffers faster than storage buffers (Adreno)");

namespace svr2011::native::backend {

namespace {

// The depth buffers' format (DepthFormat): ReportDevice picks it.
plume::RenderFormat g_depth_format = plume::RenderFormat::D24_UNORM_S8_UINT;
// The GPU's largest 2D image side (VkPhysicalDeviceLimits; 0 until ReportDevice): the render scale's limit.
std::atomic<uint32_t> g_max_image_side{0};

// What a player's log needs to tell why the native renderer does or doesn't
// run on their GPU (Mali, old Adreno drivers): the device, the features it
// asks for and the formats it uses. Sets the texture cache's BC fallback;
// false: the device can't run it.
bool ReportDevice(const rex::external_frame::VulkanDevice& d) {
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(d.get_instance_proc_addr);
  auto instance = static_cast<VkInstance>(d.instance);
  auto physical = static_cast<VkPhysicalDevice>(d.physical_device);
  auto get_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  auto get_features =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(gipa(instance, "vkGetPhysicalDeviceFeatures"));
  auto get_format = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      gipa(instance, "vkGetPhysicalDeviceFormatProperties"));
  if (!get_properties || !get_features || !get_format) return true;
  VkPhysicalDeviceProperties props;
  get_properties(physical, &props);
  VkPhysicalDeviceFeatures features;
  get_features(physical, &features);
  g_max_image_side = props.limits.maxImageDimension2D;
  // (vulkan_simulate_mali: the SDK hides Mali's missing features - here the
  // formats it can't filter or read too, so their paths run on another GPU)
  const bool simulated = rex::cvar::GetFlagByName("vulkan_simulate_mali") == "true";
  const bool mali = props.vendorID == 0x13B5 || simulated;  // ARM
  if (mali && !REXCVAR_GET(mali_alpha)) {
    // (started without the launcher's Mali alpha mode - a home screen that skipped the launcher, settings reset:
    // what it decides in the game is turned on here; the SDK has relaxed the device requirements already)
    REXLOG_WARN("GPU report: a Mali GPU without mali_alpha - turned on");
    REXCVAR_SET(mali_alpha, true);
  }
  REXLOG_INFO("GPU report: '{}', vendor 0x{:04X} device 0x{:08X}, Vulkan {}.{}.{}, driver 0x{:08X}{}",
              props.deviceName, props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
              VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
              props.driverVersion,
              mali ? (REXCVAR_GET(mali_alpha) ? " - Mali (alpha mode)" : " - Mali (alpha mode off)") : "");
  REXLOG_INFO("GPU report: app renderer features {}, shaderInt64 {}, geometryShader {}, fillModeNonSolid {}, "
              "textureCompressionBC {}, ETC2 {}, ASTC {}, independentBlend {}, depthClamp {}, "
              "fragmentStoresAndAtomics {}, vertexPipelineStoresAndAtomics {}, maxImageDimension2D {}",
              d.app_renderer_features, bool(features.shaderInt64), bool(features.geometryShader),
              bool(features.fillModeNonSolid), bool(features.textureCompressionBC),
              bool(features.textureCompressionETC2), bool(features.textureCompressionASTC_LDR),
              bool(features.independentBlend), bool(features.depthClamp), bool(features.fragmentStoresAndAtomics),
              bool(features.vertexPipelineStoresAndAtomics), props.limits.maxImageDimension2D);

  // The bindless tables' limits (update-after-bind: the texture set is written
  // while bound).
  if (auto get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          gipa(instance, d.api_version >= VK_API_VERSION_1_1 ? "vkGetPhysicalDeviceProperties2"
                                                             : "vkGetPhysicalDeviceProperties2KHR"))) {
    VkPhysicalDeviceDescriptorIndexingProperties di = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &di;
    get_properties2(physical, &p2);
    REXLOG_INFO("GPU report: update-after-bind limits - sampled images per stage {} / per set {}, samplers per "
                "stage {} / per set {}, resources per stage {}, descriptors in all pools {}; per stage (plain) "
                "sampled images {}, samplers {}, storage buffers {}, sets {}",
                di.maxPerStageDescriptorUpdateAfterBindSampledImages, di.maxDescriptorSetUpdateAfterBindSampledImages,
                di.maxPerStageDescriptorUpdateAfterBindSamplers, di.maxDescriptorSetUpdateAfterBindSamplers,
                di.maxPerStageUpdateAfterBindResources, di.maxUpdateAfterBindDescriptorsInAllPools,
                props.limits.maxPerStageDescriptorSampledImages, props.limits.maxPerStageDescriptorSamplers,
                props.limits.maxPerStageDescriptorStorageBuffers, props.limits.maxBoundDescriptorSets);
  }

  // The formats the renderer creates: s = sampled, f = filtered, c = color
  // attachment, b = blended, d = depth attachment (optimal tiling).
  struct Format {
    VkFormat format;
    const char* name;
  };
  static const Format kFormats[] = {
      {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"},       {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1 sRGB"},
      {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"},            {VK_FORMAT_BC2_SRGB_BLOCK, "BC2 sRGB"},
      {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"},            {VK_FORMAT_BC3_SRGB_BLOCK, "BC3 sRGB"},
      {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"},            {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"},
      {VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},           {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
      {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2"}, {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
      {VK_FORMAT_R16G16B16A16_UNORM, "RGBA16"},      {VK_FORMAT_R16G16B16A16_SNORM, "RGBA16 snorm"},
      {VK_FORMAT_R16G16_SNORM, "RG16 snorm"},        {VK_FORMAT_R16G16_UNORM, "RG16"},
      {VK_FORMAT_R16_UNORM, "R16"},
      {VK_FORMAT_R32_SFLOAT, "R32F"},                {VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F"},
      {VK_FORMAT_D24_UNORM_S8_UINT, "D24S8"},        {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32S8"},
      {VK_FORMAT_D32_SFLOAT, "D32F"},
  };
  std::string line;
  bool bc = true, d24s8 = false, d32s8 = false, unorm16_filter = true;
  for (const Format& f : kFormats) {
    VkFormatProperties fp;
    get_format(physical, f.format, &fp);
    const VkFormatFeatureFlags o = fp.optimalTilingFeatures;
    std::string bits;
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) bits += 's';
    if (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) bits += 'f';
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) bits += 'c';
    if (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) bits += 'b';
    if (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) bits += 'd';
    line += fmt::format("{}{} {}", line.empty() ? "" : ", ", f.name, bits.empty() ? "-" : bits);
    if (f.format <= VK_FORMAT_BC5_UNORM_BLOCK && f.format >= VK_FORMAT_BC1_RGBA_UNORM_BLOCK &&
        !(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      bc = false;
    if (f.format == VK_FORMAT_D24_UNORM_S8_UINT) d24s8 = o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (f.format == VK_FORMAT_D32_SFLOAT_S8_UINT) d32s8 = o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((f.format == VK_FORMAT_R16_UNORM || f.format == VK_FORMAT_R16G16_UNORM ||
         f.format == VK_FORMAT_R16G16B16A16_UNORM) &&
        !(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
      unorm16_filter = false;
  }
  REXLOG_INFO("GPU report: formats (s sampled, f filtered, c color target, b blended, d depth target): {}", line);

  bc = bc && features.textureCompressionBC && !simulated;
  textures::SetBlockCompressionSupported(bc);
  unorm16_filter = unorm16_filter && !simulated;  // (as Mali-G57 / G68 / G78 / G720 / G925)
  textures::SetUnorm16Filterable(unorm16_filter);
  if (!unorm16_filter)
    REXLOG_WARN("native renderer: 16-bit UNORM textures can't be filtered on this GPU - converted to 16-bit float");
  if (!bc) REXLOG_WARN("native renderer: no BC (DXT) textures on this GPU - they are decoded on the CPU (slower loads)");
  // Depth buffers: D24S8, else D32F S8 (AMD's Windows Vulkan driver has no
  // D24S8 depth target). SVR2011_NATIVE_DEPTH_D32=1: D32F S8 anyway (tests
  // that path on a GPU with both).
  static const bool force_d32 = std::getenv("SVR2011_NATIVE_DEPTH_D32") != nullptr;
  if (d24s8 && !(force_d32 && d32s8)) {
    g_depth_format = plume::RenderFormat::D24_UNORM_S8_UINT;
    REXLOG_INFO("native renderer: depth buffers D24S8");
  } else if (d32s8) {
    g_depth_format = plume::RenderFormat::D32_FLOAT_S8_UINT;
    REXLOG_INFO("native renderer: depth buffers D32F S8{}", d24s8 ? " (SVR2011_NATIVE_DEPTH_D32)" : " (no D24S8 depth target)");
  } else {
    REXLOG_ERROR("native renderer: the GPU has no D24S8 or D32F S8 depth buffers");
    return false;
  }
  return true;
}

class VulkanBackend final : public Backend {
 public:
  Api api() const override { return Api::kVulkan; }

  std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) override {
    device_name->clear();
    const rex::external_frame::VulkanDevice* d = rex::external_frame::GetVulkanDevice();
    if (!d) return nullptr;
    if (!ReportDevice(*d)) return nullptr;
    // (Vulkan 1.0 / 1.1 too: descriptor indexing through its extension; the
    // shaders need no buffer device addresses or 64-bit integers)
    // Without descriptor indexing (runtime arrays, partially bound / update-
    // after-bind sampled images - old Mali drivers): compact tables, a small
    // set of each draw's own textures.
    compact_ = !d->app_renderer_features || REXCVAR_GET(native_compact_tables);
    if (compact_ && d->app_renderer_features)
      REXLOG_INFO("native renderer: compact tables chosen (native_compact_tables) - shaders .spvc");
    else if (compact_)
      REXLOG_WARN("native renderer: no descriptor indexing on this GPU - each draw binds its own textures "
                  "(compact tables, shaders .spvc)");
    plume::VulkanExistingDevice existing;
    existing.instance = static_cast<VkInstance>(d->instance);
    existing.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(d->get_instance_proc_addr);
    existing.apiVersion = d->api_version;
    existing.physicalDevice = static_cast<VkPhysicalDevice>(d->physical_device);
    existing.device = static_cast<VkDevice>(d->device);
    existing.queueFamilyIndex = d->queue_family;
    existing.queue = static_cast<VkQueue>(d->queue);
    existing.queueMutex = d->queue_mutex;
    existing.descriptorIndexing = !compact_;
    existing.bufferDeviceAddress = false;  // (constants come from a storage buffer: shader_common.h)
    existing.scalarBlockLayout = d->scalar_block_layout;
    existing.nullDescriptor = d->null_descriptor;
    existing.samplerMirrorClampToEdge = d->sampler_mirror_clamp_to_edge;
    existing.geometryShader = d->geometry_shader;
    return plume::CreateVulkanInterface(existing);
  }

  plume::RenderShaderFormat ShaderFormat() const override { return plume::RenderShaderFormat::SPIRV; }

  void LoadPipelineCache(plume::RenderDevice* device, const std::filesystem::path& file) override {
    std::vector<uint8_t> data;
    if (std::ifstream in{file, std::ios::binary}) {
      data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto* vk = static_cast<plume::VulkanDevice*>(device);
    saved_size_ = data.size();
    if (vk->setPipelineCacheData(data.empty() ? nullptr : data.data(), data.size())) {
      REXLOG_INFO("native renderer: pipeline cache {} ({} KB)", data.empty() ? "new" : "loaded",
                  data.size() / 1024);
    }
  }

  void SavePipelineCache(plume::RenderDevice* device, const std::filesystem::path& file) override {
    const std::vector<uint8_t> data = static_cast<plume::VulkanDevice*>(device)->getPipelineCacheData();
    if (data.empty() || data.size() == saved_size_) return;  // (only cache hits: nothing new)
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      if (!out) return;
    }
    std::filesystem::rename(tmp, file, ec);
    if (!ec) {
      saved_size_ = data.size();
      REXLOG_INFO("native renderer: pipeline cache saved ({} KB)", data.size() / 1024);
    }
  }
  const char* ShaderExtension() const override { return compact_ ? ".spvc" : ".spv"; }
  bool CompactTables() const override { return compact_; }
  plume::RenderFormat DepthFormat() const override { return g_depth_format; }

  void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                    uint32_t height, plume::RenderCommandFence*) override {
    std::lock_guard lock(mutex_);
    ++serial_;
    svr2011::LatencyOnPublish();  // (frame_rate.h: test aid)
    Held& held = held_[image.get()];
    if (!held.texture) {
      held.texture = image;
      held.view = image->createTextureView(
          plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    }
    held.serial = serial_;
    // Images not shown for a while (replaced by a window resize) go: the
    // emulator's presentation is long done with them.
    for (auto it = held_.begin(); it != held_.end();) {
      it = it->second.serial + kHoldFrames < serial_ ? held_.erase(it) : std::next(it);
    }
    frame_image_ = uint64_t(static_cast<plume::VulkanTexture*>(image.get())->vk);
    frame_view_ = uint64_t(static_cast<plume::VulkanTextureView*>(held.view.get())->vk);
    frame_w_ = width;
    frame_h_ = height;
    // (now and then: the frames handed to the emulator's presentation)
    if (serial_ == 1 || serial_ % 1800 == 0) {
      REXLOG_INFO("native renderer: {} frames published ({}x{}, image {:X})", serial_, width, height, frame_image_);
    }
  }

  bool GetFrame(rex::external_frame::Frame& frame) override {
    std::lock_guard lock(mutex_);
    if (!frame_view_) return false;
    frame.vk_image = frame_image_;
    frame.vk_image_view = frame_view_;
    frame.width = frame_w_;
    frame.height = frame_h_;
    return true;
  }

  void ClearFrame() override {
    std::lock_guard lock(mutex_);
    frame_image_ = frame_view_ = 0;
  }

  // A lost device shows up in the emulator too (the same device), which
  // reports it.
  bool DeviceLost(plume::RenderDevice*) override { return false; }
  void ReportDeviceLost(plume::RenderDevice*) override {}
  // The emulator doesn't wait for the renderer's fences (one queue).
  void ReleaseFence(plume::RenderCommandFence*) override {}
  void DrainDebugMessages(plume::RenderDevice*) override {}
  void LogBuffer(plume::RenderBuffer*, uint64_t) override {}

  bool PipelineCreated(plume::RenderPipeline* pipeline) override {
    return pipeline && static_cast<plume::VulkanGraphicsPipeline*>(pipeline)->vk != VK_NULL_HANDLE;
  }

  void StallQueue(plume::RenderCommandQueue*) override {
    REXLOG_WARN("native renderer: the test stall is D3D12 only");
  }

 private:
  size_t saved_size_ = 0;  // (the pipeline cache file's)
  bool compact_ = false;   // (no descriptor indexing: CompactTables)
  static constexpr uint64_t kHoldFrames = 16;
  struct Held {
    std::shared_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint64_t serial = 0;
  };
  std::mutex mutex_;
  std::unordered_map<const plume::RenderTexture*, Held> held_;
  uint64_t serial_ = 0;
  uint64_t frame_image_ = 0, frame_view_ = 0;
  uint32_t frame_w_ = 0, frame_h_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }


uint32_t VulkanMaxImageSide() { return g_max_image_side.load(); }

// VK_EXT_memory_budget on the emulator's physical device: the device-local heaps' use and budget (phones: the shared
// memory the driver gives the GPU).
bool VulkanVideoMemory(uint64_t* used, uint64_t* budget) {
  const rex::external_frame::VulkanDevice* d = rex::external_frame::GetVulkanDevice();
  if (!d || !d->get_instance_proc_addr || !d->physical_device) return false;
  static std::mutex mutex;
  static int supported = -1;
  static PFN_vkGetPhysicalDeviceMemoryProperties2 props2 = nullptr;
  std::lock_guard lock(mutex);
  const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(d->get_instance_proc_addr);
  const auto instance = static_cast<VkInstance>(d->instance);
  const auto pd = static_cast<VkPhysicalDevice>(d->physical_device);
  if (supported < 0) {
    supported = 0;
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties2"));
    uint32_t n = 0;
    if (enumerate && props2 && enumerate(pd, nullptr, &n, nullptr) == VK_SUCCESS) {
      std::vector<VkExtensionProperties> ext(n);
      if (enumerate(pd, nullptr, &n, ext.data()) == VK_SUCCESS)
        for (const auto& e : ext)
          if (!std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) supported = 1;
    }
  }
  if (!supported) return false;
  VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {};
  b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  VkPhysicalDeviceMemoryProperties2 p = {};
  p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  p.pNext = &b;
  props2(pd, &p);
  uint64_t u = 0, g = 0;
  for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; ++i)
    if (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) u += b.heapUsage[i], g += b.heapBudget[i];
  if (!g) return false;
  *used = u, *budget = g;
  return true;
}

}  // namespace svr2011::native::backend
