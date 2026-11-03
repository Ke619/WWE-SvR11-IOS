// WWE SmackDown vs. Raw 2011 - native renderer textures and samplers.
//
// Guest textures are described by Xenos texture fetch constants (the device
// register mirror, 6 dwords per slot). Each distinct texture is uploaded once
// into a GPU texture: the guest data is detiled, endian-swapped and, for the
// few formats the host lacks, converted on the CPU; the layout of the mips
// and the packed mip tail comes from the SDK's Xenia texture utilities, so it
// matches the emulated renderer. Textures are re-checked against guest memory
// every so often (streamed textures reuse memory).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <plume_render_interface.h>

namespace rex::memory {
class Memory;
}

namespace svr2011::native::textures {

struct Context {
  plume::RenderDevice* device = nullptr;
  plume::RenderCommandList* list = nullptr;
  // The shaders' texture tables: 2D, 3D and cube textures (register spaces
  // 0-2), and samplers (space 3). A texture's index is the same in each.
  plume::RenderDescriptorSet* texture_sets[3] = {};
  plume::RenderDescriptorSet* sampler_set = nullptr;
  // Where each table starts in its set: Vulkan has the three tables in one
  // set (bindings 0-2, kSrvHeapSize apart), D3D12 one set each (all 0).
  uint32_t texture_base[3] = {};
  uint64_t frame = 0;
  // Keeps an object alive until the GPU has finished the current frame.
  std::function<void(std::shared_ptr<void>)> retire;
  // Space in the frame's upload ring (cpu pointer, the ring buffer, offset in
  // it), or {nullptr, nullptr, 0} when it's full.
  struct UploadSpace {
    uint8_t* cpu;
    plume::RenderBuffer* buffer;
    uint64_t offset;
  };
  std::function<UploadSpace(uint64_t size, uint64_t align)> allocate;
};

// Descriptors [srv_first, srv_first + srv_count) of the texture tables and
// [sampler_first, sampler_first + sampler_count) of the sampler table are the
// texture cache's.
void Initialize(rex::memory::Memory* memory, uint32_t srv_first, uint32_t srv_count,
                uint32_t sampler_first, uint32_t sampler_count);

// Texture table index of the texture a fetch constant (6 host-order dwords)
// describes, viewed as `dimension` (0 2D, 1 3D, 2 cube, as the shader samples
// it), uploading it if needed; UINT32_MAX when it can't be provided (the
// caller binds a placeholder).
uint32_t Texture(const Context& ctx, const uint32_t fetch[6], uint32_t dimension);

// Sampler table index for the fetch constant's filtering and addressing.
uint32_t Sampler(const Context& ctx, const uint32_t fetch[6]);

// Moves on whenever a Texture or Sampler answer could change: a texture uploaded, changed, replaced or dropped, a
// resolve registered or forgotten, guest memory written, the quality or the check mode set. Within one frame,
// while it stays, the same fetch constants get the same answers (the renderer keeps each slot's last one).
uint64_t Generation();

// false: the GPU can't sample BC (DXT) textures (Mali): they are decoded on
// the CPU to RGBA8 / RG8 / R8 instead (before any texture is uploaded).
void SetBlockCompressionSupported(bool supported);
// false: 16-bit UNORM textures can't be linearly filtered (most Mali GPUs):
// they are converted to 16-bit float on the CPU instead.
void SetUnorm16Filterable(bool filterable);

// Texture quality (native_texture_quality): mip levels left out of big
// mipmapped 2D textures - 0 high (none), 1 medium, 2 low; their host textures
// start at the game's own smaller mipmap. A change drops the uploaded
// textures (call with the GPU idle); they're uploaded again as they're used.
void SetQuality(uint32_t skip);

// Drops every render target copy (the renderer rebuilt its targets).
void ForgetResolved();
// Drops the render target copy at one address: its image is in guest memory
// now (a resolve written back), so textures there come from memory again -
// the game may put something else there later (after Superstar Threads,
// Tyson Kidd's roster picture sat where its bake had been).
void ForgetResolved(uint32_t base_address);
// The renderer wrote guest memory (a resolve written back): textures cached
// from it are checked again at their next use, even in the same frame - the
// attire bake draws each mip from the level it just wrote back, and its
// cached copy could still be the previous bake's at that address.
void GuestWritten(uint32_t address, uint32_t size);
// true while the renderer looks up the textures of a Superstar Threads attire
// bake draw: each is checked against guest memory at every use, even in the
// same frame. The game builds each attire part's paint layer on the CPU in
// the same memory before its draw; on a slow PC two parts can come in one
// frame, and the second part was drawn with the first one's layer (colours
// on the wrong parts). Bake lookups are logged (threads bake: ...).
void SetCheckEveryUse(bool on);

// A resolve wrote the guest texture at `base_address` (physical): the image
// is in `texture` (a copy of the render target, owned by the renderer), not
// in guest memory, so fetches of that address sample the texture.
// `format` / `gamma_format` are the view formats (the latter for fetches that
// ask for gamma), `components` the channels the format has; `swap_rb`: the
// resolve stored red and blue swapped (RB_COPY_DEST_INFO copy_dest_swap),
// which the fetch swizzle of such textures undoes. `bytes`: the guest span the
// resolve covers (a resolve doesn't write it, so when it changes the game put
// something else there and the copy is forgotten).
void RegisterResolved(uint32_t base_address, plume::RenderTexture* texture, plume::RenderFormat format,
                      plume::RenderFormat gamma_format, uint32_t components, bool swap_rb,
                      uint32_t bytes);
// The renderer is about to free `texture` (a resolve copy it replaced): its
// views go with it (after the frame, through ctx.retire).
void ReleaseResolved(const Context& ctx, plume::RenderTexture* texture);

// The component mapping for a Xenos fetch swizzle (0-3 xyzw, 4 zero, 5 one);
// components the format lacks repeat its last one (as Xenia does).
plume::RenderComponentMapping ComponentMapping(uint32_t swizzle, uint32_t components,
                                               bool swap_rb = false);

struct Stats {
  uint32_t textures = 0;     // resident
  uint32_t uploads = 0;      // this frame
  uint32_t unsupported = 0;  // lookups that got the placeholder, this frame
  // Accumulated until TakePerf (performance log).
  uint64_t hash_bytes = 0, upload_bytes = 0;  // upload: host bytes copied
  uint64_t resident_bytes = 0;  // all the textures' host bytes (TakePerf)
  uint32_t reduced = 0;         // textures with levels left out (texture quality)
  uint32_t hashes = 0, uploads_total = 0;
  double hash_ms = 0, upload_ms = 0, convert_ms = 0;  // convert: of upload_ms
};
// The accumulated performance counters (and resets them).
Stats TakePerf();
// The game's textures on the GPU now: their host bytes and how many (the performance overlay).
uint64_t ResidentBytes();
uint32_t ResidentCount();
Stats FrameStats();  // and resets the per-frame counters

}  // namespace svr2011::native::textures
