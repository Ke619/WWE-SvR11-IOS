// WWE SmackDown vs. Raw 2011 - the native renderer's Direct3D 12 backend (gpu.h).

#include "native/gpu.h"
#include "frame_rate.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <plume_d3d12.h>
#include "../util.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();  // (plume_d3d12.cpp; no header declares it)
}

#include <rex/external_frame.h>
#include <rex/logging.h>

namespace svr2011::native::backend {

namespace d3d12 {

namespace {

using Microsoft::WRL::ComPtr;

// The frame handed to the emulator's presentation (it holds references, so a
// frame image replaced by a window resize stays valid until the next one).
std::mutex g_frame_mutex;
ComPtr<ID3D12Resource> g_frame;
ComPtr<ID3D12Fence> g_frame_fence;
uint64_t g_frame_fence_value = 0;
uint32_t g_frame_w = 0, g_frame_h = 0;

// Every buffer created (GPU VA, size), live or since freed: the fault report
// names the ones near the faulting address.
struct BufferLogEntry {
  D3D12_GPU_VIRTUAL_ADDRESS va;
  uint64_t size;
};
std::mutex g_buffer_log_mutex;
std::vector<BufferLogEntry> g_buffer_log;

ID3D12Device* Native(plume::RenderDevice* device) {
  return device ? static_cast<plume::D3D12Device*>(device)->d3d : nullptr;
}

using svr2011::EnvFlag;

}  // namespace

std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) {
  device_name->clear();
  // Device Removed Extended Data: if the GPU faults, the log names the
  // allocation (ReportDeviceLost). Set before the device exists.
  {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
      dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
      dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
  }
  // (debug: SVR2011_NATIVE_D3D_DEBUG - the debug layer, its messages logged by DrainDebugMessages; needs Windows'
  // Graphics Tools)
  if (EnvFlag("SVR2011_NATIVE_D3D_DEBUG")) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
  }
  if (auto* emulator = static_cast<ID3D12Device*>(rex::external_frame::Device())) {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(emulator->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
      DXGI_ADAPTER_DESC1 desc = {};
      adapter->GetDesc1(&desc);
      // plume names adapters by their description, in UTF-8.
      const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        device_name->resize(size_t(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, device_name->data(), n, nullptr, nullptr);
      }
    }
  }
  return plume::CreateD3D12Interface();
}

plume::RenderShaderFormat ShaderFormat() { return plume::RenderShaderFormat::DXIL; }
const char* ShaderExtension() { return ".dxil"; }

void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                  uint32_t height, plume::RenderCommandFence* fence) {
  // (the emulator AddRefs the resource while it uses it)
  auto* texture = static_cast<plume::D3D12Texture*>(image.get());
  auto* f = static_cast<plume::D3D12CommandFence*>(fence);
  svr2011::LatencyOnPublish();  // (frame_rate.h: test aid)
  std::lock_guard lock(g_frame_mutex);
  g_frame = texture->d3d;
  g_frame_w = width;
  g_frame_h = height;
  g_frame_fence = f->d3d;
  g_frame_fence_value = f->fenceValue - 1;  // (plume counts on past the value it just signaled)
}

bool GetFrame(rex::external_frame::Frame& frame) {
  std::lock_guard lock(g_frame_mutex);
  if (!g_frame) return false;
  frame.resource = g_frame.Get();
  frame.srv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  frame.component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  frame.width = g_frame_w;
  frame.height = g_frame_h;
  frame.fence = g_frame_fence.Get();
  frame.fence_value = g_frame_fence_value;
  return true;
}

void ClearFrame() {
  std::lock_guard lock(g_frame_mutex);
  g_frame.Reset();
  g_frame_fence.Reset();
}

bool DeviceLost(plume::RenderDevice* device) {
  ID3D12Device* d = Native(device);
  return d && d->GetDeviceRemovedReason() != S_OK;
}

void ReportDeviceLost(plume::RenderDevice* device) {
  ID3D12Device* d = Native(device);
  if (!d) return;
  const HRESULT reason = d->GetDeviceRemovedReason();
  REXLOG_ERROR("native renderer: GPU device removed, reason {:08X} - native renderer disabled",
               uint32_t(reason));
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(d->QueryInterface(IID_PPV_ARGS(&dred)))) return;
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
    for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
      const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      if (done >= n->BreadcrumbCount) continue;  // finished list
      REXLOG_ERROR("native renderer: list '{}' stopped after {} of {} ops",
                   n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?", done,
                   n->BreadcrumbCount);
      for (uint32_t i = done > 6 ? done - 6 : 0; i < std::min(done + 3, n->BreadcrumbCount); ++i)
        REXLOG_ERROR("native renderer:   [{}] op {}{}", i, int(n->pCommandHistory[i]),
                     i == done ? "  <- faulting" : "");
    }
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
    REXLOG_ERROR("native renderer: page fault at GPU VA {:016X}", fault.PageFaultVA);
    {
      std::lock_guard lock(g_buffer_log_mutex);
      uint32_t shown = 0;
      for (const BufferLogEntry& b : g_buffer_log) {
        if (fault.PageFaultVA + (64ull << 20) >= b.va && fault.PageFaultVA < b.va + b.size + (64ull << 20) &&
            shown++ < 16)
          REXLOG_ERROR("native renderer:   {} native buffer {:016X} size {:X}",
                       fault.PageFaultVA >= b.va && fault.PageFaultVA < b.va + b.size ? "IN" : "near",
                       b.va, b.size);
      }
      REXLOG_ERROR("native renderer:   ({} native buffers created)", g_buffer_log.size());
    }
    for (auto* n = fault.pHeadExistingAllocationNode; n; n = n->pNext)
      REXLOG_ERROR("native renderer:   existing allocation '{}' type {}",
                   n->ObjectNameA ? n->ObjectNameA : "?", int(n->AllocationType));
    for (auto* n = fault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
      REXLOG_ERROR("native renderer:   recently freed allocation '{}' type {}",
                   n->ObjectNameA ? n->ObjectNameA : "?", int(n->AllocationType));
  }
}

void ReleaseFence(plume::RenderCommandFence* fence) {
  if (auto* f = static_cast<plume::D3D12CommandFence*>(fence); f && f->d3d) f->d3d->Signal(UINT64_MAX);
}

void DrainDebugMessages(plume::RenderDevice* device) {
  static const bool on = EnvFlag("SVR2011_NATIVE_D3D_DEBUG");
  if (!on) return;
  ID3D12Device* d = Native(device);
  ComPtr<ID3D12InfoQueue> q;
  if (!d || FAILED(d->QueryInterface(IID_PPV_ARGS(&q)))) return;
  static uint32_t logged = 0;
  const UINT64 n = q->GetNumStoredMessages();
  for (UINT64 i = 0; i < n && logged < 200; ++i) {
    SIZE_T len = 0;
    q->GetMessage(i, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
    if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
      REXLOG_WARN("native renderer: D3D12: {}", m->pDescription);
      ++logged;
    }
  }
  q->ClearStoredMessages();
}

void LogBuffer(plume::RenderBuffer* buffer, uint64_t size) {
  if (!buffer) return;
  std::lock_guard lock(g_buffer_log_mutex);
  if (g_buffer_log.size() < 65536) g_buffer_log.push_back({buffer->getDeviceAddress(), size});
}

bool PipelineCreated(plume::RenderPipeline* pipeline) {
  return pipeline && static_cast<plume::D3D12GraphicsPipeline*>(pipeline)->d3d != nullptr;
}

void StallQueue(plume::RenderCommandQueue* queue) {
  static ComPtr<ID3D12Fence> never;
  auto* q = static_cast<plume::D3D12CommandQueue*>(queue);
  if (!never) Native(q->device)->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&never));
  if (never) q->d3d->Wait(never.Get(), 1);
}

}  // namespace d3d12

namespace {

class D3D12Backend final : public Backend {
 public:
  Api api() const override { return Api::kD3D12; }
  std::unique_ptr<plume::RenderInterface> CreateInterface(std::string* device_name) override {
    return d3d12::CreateInterface(device_name);
  }
  plume::RenderShaderFormat ShaderFormat() const override { return d3d12::ShaderFormat(); }
  const char* ShaderExtension() const override { return d3d12::ShaderExtension(); }
  void PublishFrame(const std::shared_ptr<plume::RenderTexture>& image, uint32_t width,
                    uint32_t height, plume::RenderCommandFence* fence) override {
    d3d12::PublishFrame(image, width, height, fence);
  }
  bool GetFrame(rex::external_frame::Frame& frame) override { return d3d12::GetFrame(frame); }
  void ClearFrame() override { d3d12::ClearFrame(); }
  bool DeviceLost(plume::RenderDevice* device) override { return d3d12::DeviceLost(device); }
  void ReportDeviceLost(plume::RenderDevice* device) override { d3d12::ReportDeviceLost(device); }
  void ReleaseFence(plume::RenderCommandFence* fence) override { d3d12::ReleaseFence(fence); }
  void DrainDebugMessages(plume::RenderDevice* device) override {
    d3d12::DrainDebugMessages(device);
  }
  void LogBuffer(plume::RenderBuffer* buffer, uint64_t size) override {
    d3d12::LogBuffer(buffer, size);
  }
  bool PipelineCreated(plume::RenderPipeline* pipeline) override {
    return d3d12::PipelineCreated(pipeline);
  }
  void StallQueue(plume::RenderCommandQueue* queue) override { d3d12::StallQueue(queue); }

  // The compiled pipelines (an ID3D12PipelineLibrary): without it every
  // pipeline was compiled again at each start - hitches at each new scene on
  // slow CPUs/drivers.
  void LoadPipelineCache(plume::RenderDevice* device, const std::filesystem::path& file) override {
    std::vector<uint8_t> data;
    if (std::ifstream in{file, std::ios::binary}) {
      data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto* d3d = static_cast<plume::D3D12Device*>(device);
    const bool loaded = d3d->setPipelineCacheData(data.empty() ? nullptr : data.data(), data.size());
    saved_stores_ = 0;
    REXLOG_INFO("native renderer: pipeline library {} ({} KB)",
                loaded ? "loaded" : data.empty() ? "new" : "new (the saved one is from another driver)",
                loaded ? data.size() / 1024 : 0);
  }

  void SavePipelineCache(plume::RenderDevice* device, const std::filesystem::path& file) override {
    auto* d3d = static_cast<plume::D3D12Device*>(device);
    if (d3d->pipelineLibraryStores == saved_stores_) return;  // (nothing new)
    const uint32_t stores = d3d->pipelineLibraryStores;
    const std::vector<uint8_t> data = d3d->getPipelineCacheData();
    if (data.empty()) return;
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
      saved_stores_ = stores;
      REXLOG_INFO("native renderer: pipeline library saved ({} KB; {} loaded, {} compiled this run)",
                  data.size() / 1024, d3d->pipelineLibraryLoads, stores);
    }
  }

 private:
  uint32_t saved_stores_ = 0;
};

}  // namespace

std::unique_ptr<Backend> CreateD3D12Backend() { return std::make_unique<D3D12Backend>(); }


// The adapter the emulator's device is on (by its LUID: the D3D12 device's, or the D3D11 device's DXGI adapter), its
// local (dedicated) memory use and budget.
bool DxgiVideoMemory(uint64_t* used, uint64_t* budget) {
  static std::mutex mutex;
  static Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
  static bool tried = false;
  std::lock_guard lock(mutex);
  if (!tried) {
    tried = true;
    LUID luid = {};
    bool have = false;
    if (ActiveApi() == Api::kD3D11) {
      if (const rex::external_frame::D3D11Device* d = rex::external_frame::GetD3D11Device()) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dx;
        Microsoft::WRL::ComPtr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(static_cast<ID3D11Device*>(d->device)->QueryInterface(IID_PPV_ARGS(&dx))) && SUCCEEDED(dx->GetAdapter(&a)) &&
            SUCCEEDED(a->GetDesc(&desc)))
          luid = desc.AdapterLuid, have = true;
      }
    } else if (auto* d12 = static_cast<ID3D12Device*>(rex::external_frame::Device())) {
      luid = d12->GetAdapterLuid(), have = true;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (have && SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return false;
  *used = info.CurrentUsage, *budget = info.Budget;
  return true;
}

}  // namespace svr2011::native::backend
