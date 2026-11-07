// WWE SmackDown vs. Raw 2011 - the native renderer's graphics API.
//
// The renderer draws through plume (github.com/renderbag/plume), a thin layer
// over Direct3D 12 and Vulkan. What plume doesn't cover - which GPU to use,
// handing finished frames to the emulator's presentation, GPU fault reports -
// is here, one implementation per API (backend_d3d12.cpp, backend_vulkan.cpp).
// The renderer draws on the emulator's own device with the emulator's API:
// its presentation shows the frames, which only works on the same device.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <plume_render_interface.h>

namespace rex::external_frame {
struct Frame;
}

namespace svr2011::native::backend {

enum class Api { kNone, kD3D12, kVulkan, kD3D11 };

class Backend {
 public:
  virtual ~Backend() = default;
  virtual Api api() const = 0;
  virtual std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) = 0;
  virtual plume::RenderShaderFormat ShaderFormat() const = 0;
  virtual const char* ShaderExtension() const = 0;
  virtual bool CompactTables() const { return false; }
  virtual plume::RenderFormat DepthFormat() const { return plume::RenderFormat::D24_UNORM_S8_UINT; }
  virtual void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                            uint32_t height, plume::RenderCommandFence* fence) = 0;
  virtual bool GetFrame(rex::external_frame::Frame& frame) = 0;
  virtual void ClearFrame() = 0;
  virtual bool DeviceLost(plume::RenderDevice* device) = 0;
  virtual void ReportDeviceLost(plume::RenderDevice* device) = 0;
  virtual void ReleaseFence(plume::RenderCommandFence* fence) = 0;
  virtual void DrainDebugMessages(plume::RenderDevice* device) = 0;
  virtual void LogBuffer(plume::RenderBuffer* buffer, uint64_t size) = 0;
  virtual bool PipelineCreated(plume::RenderPipeline* pipeline) = 0;
  virtual void StallQueue(plume::RenderCommandQueue* queue) = 0;
  // A pipeline cache kept on disk (Vulkan: a VkPipelineCache; D3D12: a
  // pipeline library).
  virtual void LoadPipelineCache(plume::RenderDevice*, const std::filesystem::path&) {}
  virtual void SavePipelineCache(plume::RenderDevice*, const std::filesystem::path&) {}
};

std::unique_ptr<Backend> CreateD3D12Backend();   // null where D3D12 isn't built
std::unique_ptr<Backend> CreateVulkanBackend();
std::unique_ptr<Backend> CreateD3D11Backend();   // null where D3D11 isn't built

// Picks the emulator's API (Vulkan when it runs on Vulkan, else D3D12) and
// creates the interface and the name of the GPU to draw on.
std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name);
Api ActiveApi();
// Why the backend couldn't make its interface, for the player (empty: no
// particular reason known).
std::string FailReason();
void SetFailReason(std::string reason);

// The converted shaders this backend loads: "<hash>.<vs|ps><variant><extension>".
plume::RenderShaderFormat ShaderFormat();
const char* ShaderExtension();
// Vulkan GPUs without descriptor indexing (old Mali drivers): no big texture
// tables - each draw binds small sets of its own textures (native_renderer.cpp,
// compact tables) and the shaders are the .spvc build (fixed-size tables).
bool CompactTables();
// The host depth buffers' format: D24S8, or D32F S8 where the GPU has no D24S8
// depth target (AMD's Windows Vulkan driver). The game's depth values (0..1)
// are the same in either.
plume::RenderFormat DepthFormat();

// Main mode: `image` (shader-readable, RGBA8) is this frame, finished when
// `fence` - just signaled by the frame's submission - is. The emulator's
// presentation shows it (rex/external_frame.h) until the next one; the
// backend keeps the image alive while the emulator may still read it.
void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                  uint32_t height, plume::RenderCommandFence* fence);
bool GetFrame(rex::external_frame::Frame& frame);
void ClearFrame();

// The GPU stopped: whether the device is lost, and a report of why (with
// the faulting allocation when the driver tells).
bool DeviceLost(plume::RenderDevice* device);
void ReportDeviceLost(plume::RenderDevice* device);
// Sets `fence` from the CPU, so work waiting on it (the emulator's queue,
// for a frame) runs on after the GPU stopped answering.
void ReleaseFence(plume::RenderCommandFence* fence);
// Debug layer messages (SVR2011_NATIVE_D3D_DEBUG), logged and cleared.
void DrainDebugMessages(plume::RenderDevice* device);
// Names a buffer's GPU range for the fault report.
void LogBuffer(plume::RenderBuffer* buffer, uint64_t size);
// Whether the driver built the pipeline (plume returns one either way).
bool PipelineCreated(plume::RenderPipeline* pipeline);
// The pipeline cache file (per API): read before the first pipeline, written
// when new pipelines were built. Compiled pipelines then load from it the
// next time instead of being compiled again (phones' drivers keep none).
void LoadPipelineCache(plume::RenderDevice* device, const std::filesystem::path& file);
void SavePipelineCache(plume::RenderDevice* device, const std::filesystem::path& file);
// Debug: makes `queue` wait for good before its next work (a stuck GPU job).
void StallQueue(plume::RenderCommandQueue* queue);
// The GPU's own memory in use and the budget the OS / driver gives the game (bytes; the performance overlay):
// DXGI QueryVideoMemoryInfo (D3D12 / D3D11), VK_EXT_memory_budget's device-local heaps (Vulkan). False where
// the API can't tell.
bool VideoMemory(uint64_t* used, uint64_t* budget);
bool DxgiVideoMemory(uint64_t* used, uint64_t* budget);    // (backend_d3d12.cpp; Windows)
bool VulkanVideoMemory(uint64_t* used, uint64_t* budget);  // (backend_vulkan.cpp)
// Vulkan: the GPU's largest 2D image side (maxImageDimension2D; 0: not Vulkan / not known yet).
uint32_t VulkanMaxImageSide();

}  // namespace svr2011::native::backend
