// WWE SmackDown vs. Raw 2011 - native Direct3D 12 renderer (prototype).
//
// Instead of emulating the Xenos GPU, the game's own D3D calls are hooked
// (native_hooks.cpp) and drawn directly with D3D12, using the game's shaders
// converted ahead of time (XenosRecomp -> DXIL, tools/convert_shaders.py).
// The game state at each draw is read from the D3D device's register mirror
// (guest_d3d.h).
//
// native_renderer = main    (default) draws the game; the emulated renderer
//                           is the backup
//                 = off     not used (the emulated renderer draws)
//                 = shadow  runs beside the emulated renderer, drawing into a
//                           second window "SvR 2011 - native renderer" so the
//                           two can be compared
// The window title shows the native frame rate and draw statistics.
// Shaders are loaded from SVR2011_NATIVE_SHADERS (default <exe>\native_shaders).

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <cstdint>

struct PPCContext;

namespace rex::memory {
class Memory;
}

namespace svr2011::native {

bool Enabled();

// Why the native renderer couldn't start or stopped, or "" while it works.
// The game draws only with it (no emulated fallback): the overlay shows this
// over a black screen (fps_overlay.cpp).
std::string FailureReason();

// Main mode: switch between this renderer and the emulated one while the
// game runs (the GRAPHICS page). CanSwitch: started as the main renderer
// and still working.
bool CanSwitch();

// Whether an upscaler can run here (the GRAPHICS page's UPSCALING choices): "fsr2" - FSR 2, Direct3D 12 / Vulkan
// on Windows; "dlss" - NVIDIA DLSS (not in this build yet). `why`: the reason when not (logged once each).
bool UpscalerAvailable(const std::string& kind, std::string* why = nullptr);
void SetNativeActive(bool active);
bool NativeActive();

// What draws the game right now, for the FPS counter: "Native 2x",
// "Emulated", or "Emulated (native failed)".
std::string RendererLabel();
// The renderer's own costs per frame over about the last second (the performance overlay): recording the frame's
// draws on the CPU, waiting for the GPU to finish the frame before (high: the GPU is the limit), submitting, and the
// draws; the textures' host memory. Zero before the first frames.
struct FrameCosts {
  double draw_ms = 0, gpu_wait_ms = 0, submit_ms = 0, draws = 0, texture_mb = 0;
  double targets_mb = 0;  // the render targets standing in for the Xbox 360's (colour and depth; scale and wide screen)
  uint32_t textures = 0;  // the game's textures on the GPU
};
FrameCosts LastFrameCosts();
// The draws of the last frame shown, and how many frames so far (any thread; model_mem.cpp's load watch: a loading
// screen draws a handful).
uint32_t LastFrameDraws();
uint64_t FramesShown();

// Game state (any thread): a match (entrances on) or the menus - on a wide
// window, matches fill it (native_widescreen), menus stay 16:9.
void SetMatchScene(bool in_match);

// REPORT A PROBLEM's screenshot (report_page.cpp): the next shown frame's image (the game's, without the port's
// overlays) kept in memory - previous: the frame shown before the request instead (a pause menu drawn in the frame
// that read START) - TakeFrameCapture hands it over once (RGB, 3 bytes a pixel; false: none yet).
void RequestFrameCapture(bool previous = false);
bool TakeFrameCapture(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height);

// 30 fps with the world at 60 Hz (frame_rate.cpp): every other frame is
// drawn without its draws and clears and not shown. HalfFrames: on, with
// this renderer drawing.
void SetHalfFrames(bool on);
bool HalfFrames();
bool LastFrameHidden();  // (the frame OnPresent just ended wasn't shown)
// While the world can't keep 60 Hz (frames of two updates), every frame is
// shown again (frame_rate.cpp).
void SetHalfSuspended(bool on);

// The known pipelines being built ahead in the background (any thread):
// false when it isn't running; `paused` while a match, entrance or cutscene
// is on (it waits for the menus).
bool PreparingPipelines(uint32_t* done, uint32_t* total, bool* paused);

// Screens narrower than 16:9 (matches): the factor the camera's tan(vertical
// field of view / 2) is scaled by (1: unchanged) - native_hooks.cpp applies
// it in the projection setup (sub_825818B8).
float CameraFovScale();

// The main window's client size in pixels (main mode: the frames follow it).
void SetWindowSizeSource(std::function<std::pair<uint32_t, uint32_t>()> source);

// Called once guest memory exists (before the game runs).
void Attach(rex::memory::Memory* memory);

// Hook entry points (guest render thread).
// front_buffer: the texture object Swap shows (guest pointer).
void OnPresent(uint32_t front_buffer);
void OnDrawIndexed(const PPCContext& ctx);
void OnDrawUP(const PPCContext& args, uint32_t data);  // data: the space it returned
void OnEndVertices();
void OnClear(const PPCContext& ctx);
void OnResolve(const PPCContext& ctx);
// The port rewrote guest texture data (guest virtual address, bytes; the
// arena select pages' banners): textures cached from it are checked again at
// their next use - the cache re-hashes a texture in constant use only every
// 120 frames, so page 2's banners came in one by one, some half written.
void GuestWritten(uint32_t address, uint32_t size);
void OnSetRenderTarget(uint32_t index, uint32_t surface);
void OnSetStreamSource(uint32_t stream, uint32_t buffer, uint32_t offset, uint32_t stride);
void OnShaderCreated(uint32_t container, uint32_t object, bool pixel);

// Shader constants, tracked as the GPU's constant file sees them:
//   SetVertex/PixelShaderConstantF write the device mirror (marked dirty),
//   the D3D flush copies dirty mirror ranges to the GPU,
//   and per-draw blocks are loaded straight from memory (LOAD_ALU_CONSTANT).
void OnSetShaderConstants(bool pixel, uint32_t start, uint32_t count);
// The D3D constant flush: `mask` has a bit per 4 dirty constants of the stage
// whose GPU registers start at `reg` (0x4000 vertex, 0x4400 pixel).
void OnFlushShaderConstants(uint64_t mask, uint32_t reg);
void OnLoadShaderConstants(uint32_t shader_object, uint32_t base);

}  // namespace svr2011::native
