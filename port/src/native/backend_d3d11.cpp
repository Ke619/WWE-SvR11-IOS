// WWE SmackDown vs. Raw 2011 - the native renderer's Direct3D 11 backend (gpu.h).
//
// For GPUs / drivers that run neither Direct3D 12 nor Vulkan: the renderer
// draws through plume's Direct3D 11 backend (plume_d3d11.h) on the device and
// immediate context of the emulator's D3D11 presenter, which shows the frames
// (no fences: the context runs everything in order).

#include "native/gpu.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include <d3d11_1.h>
#include <fmt/format.h>
#include <wrl/client.h>

#include <plume_d3d11.h>

#include <rex/cvar.h>
#include <rex/external_frame.h>
#include <rex/logging.h>

#include "frame_rate.h"
#include "../util.h"

REXCVAR_DEFINE_INT32(native_d3d11_flush_draws, 1000, "GPU",
                     "Direct3D 11: submissions with fewer draws than this start on the GPU at once (0: none)");
REXCVAR_DEFINE_BOOL(native_d3d11_replay_thread, true, "GPU",
                    "Direct3D 11: the frame's GPU commands are issued on a thread of their own");
REXCVAR_DEFINE_STRING(native_d3d11_constants, "auto", "GPU",
                      "Direct3D 11: how the game's constants reach the GPU - auto, offsets (D3D11.1 windows into one "
                      "buffer, rings written in place), offsets_discard (windows, rings renamed at each upload), copy "
                      "(a copy a draw: slowest, works on every driver; auto picks it on GeForce/Quadro 400-500 (Fermi))");

namespace {

// NVIDIA Fermi (GeForce / Quadro 400-500 series, GF1xx): the last drivers (391/390) give feature level 11_0 and the
// D3D11.1 constant buffer offsets through the 11.1 runtime; wrestlers' textures flickered and went white on a Quadro 600
// (report 20261031-024628) - constants copied per draw there (native_d3d11_constants auto).
bool IsNvidiaFermi(uint32_t vendor, uint32_t device) {
  if (vendor != 0x10DE) return false;
  return (device >= 0x06C0 && device <= 0x06DF) ||  // GF100
         (device >= 0x0DC0 && device <= 0x0DFF) ||  // GF106 / GF108
         (device >= 0x0E20 && device <= 0x0E3F) ||  // GF104
         (device >= 0x1040 && device <= 0x109F) ||  // GF119 / GF110
         (device >= 0x1200 && device <= 0x125F);    // GF114 / GF116
}

}  // namespace

namespace svr2011::native::backend {

namespace {

using Microsoft::WRL::ComPtr;

// The frame shown, and the ones published since: each is shown once its
// submission has been replayed on the context (plume replays on its own
// thread). The newest replayed one wins - with a slow GPU the replay is always
// behind the newest frame, which alone would never be shown (a black screen).
struct FrameImage {
  ComPtr<ID3D11Texture2D> texture;
  uint32_t w = 0, h = 0;
  uint64_t submission = 0;
};
std::mutex g_frame_mutex;
FrameImage g_frame;
std::deque<FrameImage> g_published;
constexpr size_t kMaxPublished = 8;
plume::D3D11CommandQueue* g_queue = nullptr;

ID3D11Device* Native(plume::RenderDevice* device) {
  return device ? static_cast<plume::D3D11Device*>(device)->d3d.Get() : nullptr;
}

using svr2011::EnvFlag;

class D3D11Backend final : public Backend {
 public:
  Api api() const override { return Api::kD3D11; }

  std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) override {
    device_name->clear();
    const rex::external_frame::D3D11Device* d = rex::external_frame::GetD3D11Device();
    if (!d) return nullptr;
    if (d->feature_level < D3D_FEATURE_LEVEL_10_0) {
      REXLOG_ERROR("native renderer: the GPU's Direct3D feature level is {}_{} - 10_0 is needed",
                   (d->feature_level >> 12) & 0xF, (d->feature_level >> 8) & 0xF);
      SetFailReason(fmt::format("the GPU supports Direct3D feature level {}_{}; the game needs 10_0 (GeForce 8, "
                                "Radeon HD 2000, Intel HD Graphics or newer)",
                                (d->feature_level >> 12) & 0xF, (d->feature_level >> 8) & 0xF));
      return nullptr;
    }
    // Feature level 10_x: the Shader Model 4.0 shaders (.dxbc4: 16 interpolators).
    sm4_ = d->feature_level < D3D_FEATURE_LEVEL_11_0;
    REXLOG_INFO("native renderer: Direct3D 11, feature level {}_{}, constant buffer offsets {}{}",
                (d->feature_level >> 12) & 0xF, (d->feature_level >> 8) & 0xF,
                d->constant_buffer_offsetting ? "yes" : "no (constants copied per draw)",
                d->warp ? ", WARP (software)" : "");
    // The constants' path: the driver's D3D11.1 caps, or the native_d3d11_constants setting.
    bool offsets = d->constant_buffer_offsetting, no_overwrite = false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
    if (SUCCEEDED(static_cast<ID3D11Device*>(d->device)->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                                                             sizeof(options)))) {
      no_overwrite = options.MapNoOverwriteOnDynamicConstantBuffer;
    }
    // (the adapter: its ids for the Fermi check, its driver version for the log)
    uint32_t vendor = 0, device = 0;
    std::string driver = "?";
    {
      ComPtr<IDXGIDevice> dxgi;
      ComPtr<IDXGIAdapter> adapter;
      DXGI_ADAPTER_DESC desc = {};
      LARGE_INTEGER umd = {};
      if (SUCCEEDED(static_cast<ID3D11Device*>(d->device)->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
          SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        vendor = desc.VendorId, device = desc.DeviceId;
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
          driver = fmt::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                               LOWORD(umd.LowPart));
        }
      }
    }
    // (test aid SVR2011_TEST_D3D11_DEVICE_ID=<hex>: another GPU's id, e.g. 0DF8 - a Quadro 600)
    if (const char* id = std::getenv("SVR2011_TEST_D3D11_DEVICE_ID")) device = uint32_t(std::strtoul(id, nullptr, 16));
    std::string mode = REXCVAR_GET(native_d3d11_constants);
    const bool fermi = IsNvidiaFermi(vendor, device);
    if (mode == "auto" && fermi) mode = "copy";
    if (mode == "copy") {
      offsets = false;
    } else if (mode == "offsets_discard") {
      no_overwrite = false;
    }
    REXLOG_INFO("native renderer: D3D11 GPU {:04X}:{:04X}, driver {}; constants: driver offsets {}, no-overwrite "
                "constant maps {}; using {}{}",
                vendor, device, driver, d->constant_buffer_offsetting ? "yes" : "no",
                options.MapNoOverwriteOnDynamicConstantBuffer ? "yes" : "no",
                !offsets ? "a copy a draw" : no_overwrite ? "offsets, rings written in place" : "offsets, rings renamed",
                fermi && REXCVAR_GET(native_d3d11_constants) == "auto" ? " (NVIDIA Fermi)"
                : mode == "auto"                                        ? ""
                                                                         : fmt::format(" (native_d3d11_constants {})", mode));
    plume::D3D11NoOverwriteConstantsOff = !no_overwrite;
    plume::D3D11LogFunction = [](const char* message) { REXLOG_WARN("native renderer: {}", message); };
    plume::D3D11FlushDrawLimit = uint32_t(std::max(0, REXCVAR_GET(native_d3d11_flush_draws)));
    plume::D3D11AsyncReplay = REXCVAR_GET(native_d3d11_replay_thread);
    return plume::CreateD3D11Interface(static_cast<ID3D11Device*>(d->device),
                                       static_cast<ID3D11DeviceContext*>(d->context), d->context_mutex, offsets);
  }

  plume::RenderShaderFormat ShaderFormat() const override { return plume::RenderShaderFormat::DXBC; }
  const char* ShaderExtension() const override { return sm4_ ? ".dxbc4" : ".dxbc"; }

  void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width, uint32_t height,
                    plume::RenderCommandFence* /*fence*/) override {
    auto* texture = static_cast<plume::D3D11Texture*>(image.get());
    FrameImage f;
    texture->d3d.As(&f.texture);
    f.w = width;
    f.h = height;
    g_queue = texture->device->queue;
    f.submission = g_queue ? g_queue->submitted.load() : 0;
    svr2011::LatencyOnPublish();  // (frame_rate.h: test aid)
    std::lock_guard lock(g_frame_mutex);
    g_published.push_back(std::move(f));
    if (g_published.size() > kMaxPublished) g_published.pop_front();
  }

  bool GetFrame(rex::external_frame::Frame& frame) override {
    std::lock_guard lock(g_frame_mutex);
    const uint64_t replayed = g_queue ? g_queue->replayed.load() : ~0ull;
    while (!g_published.empty() && g_published.front().submission <= replayed) {
      g_frame = std::move(g_published.front());
      g_published.pop_front();
    }
    if (!g_frame.texture) return false;
    frame.d3d11_texture = g_frame.texture.Get();
    frame.width = g_frame.w;
    frame.height = g_frame.h;
    return true;
  }

  void ClearFrame() override {
    std::lock_guard lock(g_frame_mutex);
    g_frame = {};
    g_published.clear();
  }

  bool DeviceLost(plume::RenderDevice* device) override {
    ID3D11Device* d = Native(device);
    return d && d->GetDeviceRemovedReason() != S_OK;
  }

  void ReportDeviceLost(plume::RenderDevice* device) override {
    if (ID3D11Device* d = Native(device)) {
      REXLOG_ERROR("native renderer: GPU device removed, reason {:08X} - native renderer disabled",
                   uint32_t(d->GetDeviceRemovedReason()));
    }
  }

  void ReleaseFence(plume::RenderCommandFence*) override {}  // (event queries end on device loss)

  void DrainDebugMessages(plume::RenderDevice* device) override {
    static const bool on = EnvFlag("SVR2011_NATIVE_D3D_DEBUG");
    if (!on) return;
    ID3D11Device* d = Native(device);
    ComPtr<ID3D11InfoQueue> q;
    if (!d || FAILED(d->QueryInterface(IID_PPV_ARGS(&q)))) return;
    static uint32_t logged = 0;
    const UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && logged < 200; ++i) {
      SIZE_T len = 0;
      q->GetMessage(i, nullptr, &len);
      std::vector<uint8_t> buf(len);
      auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
      if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
        REXLOG_WARN("native renderer: D3D11: {}", m->pDescription);
        ++logged;
      }
    }
    q->ClearStoredMessages();
  }

  void LogBuffer(plume::RenderBuffer*, uint64_t) override {}

  bool PipelineCreated(plume::RenderPipeline* pipeline) override {
    return pipeline && static_cast<plume::D3D11GraphicsPipeline*>(pipeline)->valid;
  }

  void StallQueue(plume::RenderCommandQueue*) override {}

 private:
  bool sm4_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateD3D11Backend() { return std::make_unique<D3D11Backend>(); }

}  // namespace svr2011::native::backend
