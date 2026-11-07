// WWE SmackDown vs. Raw 2011 - the native renderer's command recording on a
// second thread (see recording_list.h).

#include "native/recording_list.h"

#include "android_perf.h"

#include <cstdlib>
#include <cstring>
#include <type_traits>

#include <rex/logging.h>

namespace svr2011::native {

namespace {
constexpr size_t kChunkBytes = 1 << 20;
constexpr size_t kMaxChunks = 256;  // (reserved: the worker reads the list while it grows)
constexpr size_t kPublishEvery = 32;

template <typename T>
constexpr bool Pod = std::is_trivially_copyable_v<T>;
static_assert(Pod<plume::RenderBufferBarrier> && Pod<plume::RenderTextureBarrier> &&
              Pod<plume::RenderIndexBufferView> && Pod<plume::RenderVertexBufferView> &&
              Pod<plume::RenderInputSlot> && Pod<plume::RenderViewport> && Pod<plume::RenderRect> &&
              Pod<plume::RenderColor> && Pod<plume::RenderTextureCopyLocation> && Pod<plume::RenderBox> &&
              Pod<plume::RenderBufferReference>);

struct Header {
  uint16_t op;
  uint16_t pad;
  uint32_t size;  // (header included, 8-byte multiple)
};

inline size_t Align8(size_t n) { return (n + 7) & ~size_t(7); }
}  // namespace

enum class RecordingList::Op : uint16_t {
  kNextChunk, kBegin, kEnd, kBarriers, kDraw, kDrawIndexed, kPipeline, kGraphicsLayout, kPushConstants,
  kDescriptorSet, kRootDescriptor, kIndexBuffer, kVertexBuffers, kViewports, kScissors, kFramebuffer,
  kDepthBias, kStencilReference, kBlendFactor, kClearColor, kClearDepthStencil, kCopyTextureRegion,
  kCopyTexture, kDescriptorSetDynamic,
};

struct RecordingList::Chunk {
  alignas(16) uint8_t data[kChunkBytes];
};

// Reading and writing the ops' payloads in order.
namespace {
struct Out {
  uint8_t* p;
  template <typename T>
  void put(const T& v) {
    std::memcpy(p, &v, sizeof(T));
    p += sizeof(T);
  }
  void put_bytes(const void* src, size_t n) {
    if (n) std::memcpy(p, src, n);
    p += Align8(n);
  }
};
struct In {
  const uint8_t* p;
  template <typename T>
  T get() {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  template <typename T>
  const T* array(size_t n) {  // (in place: the arena is 16-aligned and ops 8-aligned)
    const T* a = reinterpret_cast<const T*>(p);
    p += Align8(sizeof(T) * n);
    return n ? a : nullptr;
  }
};
}  // namespace

RecordingList::RecordingList() {
  chunks_.reserve(kMaxChunks);
  chunks_.push_back(std::make_unique<Chunk>());
  thread_ = std::thread([this] { Worker(); });
}

RecordingList::~RecordingList() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  work_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void RecordingList::Unsupported() {
  REXLOG_ERROR("native renderer: unsupported command recorded");
  std::abort();
}

uint8_t* RecordingList::Reserve(Op op, size_t bytes) {
  // (Published here, not after reserving: the caller has written the ops so
  // far, but not yet the payload of the one being reserved.)
  if (ops_since_publish_ >= kPublishEvery) Publish();
  ++ops_since_publish_;
  bytes = Align8(bytes + sizeof(Header));
  if (write_offset_ + bytes + sizeof(Header) > kChunkBytes) {
    // The rest of this chunk is skipped: the next op starts a new one.
    Header next{uint16_t(Op::kNextChunk), 0, uint32_t(sizeof(Header))};
    std::memcpy(chunks_[write_chunk_]->data + write_offset_, &next, sizeof(next));
    if (++write_chunk_ == chunks_.size()) {
      if (chunks_.size() == kMaxChunks) {
        REXLOG_ERROR("native renderer: recorded commands exceed {} MB in a frame", kMaxChunks);
        std::abort();
      }
      chunks_.push_back(std::make_unique<Chunk>());
    }
    write_offset_ = 0;
  }
  uint8_t* p = chunks_[write_chunk_]->data + write_offset_;
  const Header h{uint16_t(op), 0, uint32_t(bytes)};
  std::memcpy(p, &h, sizeof(h));
  write_offset_ += bytes;
  return p + sizeof(Header);
}

void RecordingList::Publish() {
  ops_since_publish_ = 0;
  published_.store((uint64_t(write_chunk_) << 32) | write_offset_, std::memory_order_seq_cst);
  // Woken only when asleep (a busy worker picks the new ops up itself):
  // signalling at every publish cost the render thread ~8%.
  if (sleeping_.load(std::memory_order_seq_cst)) {
    { std::lock_guard lock(mutex_); }  // (it is either not yet checking, or waiting)
    work_.notify_one();
  }
}

void RecordingList::Finish() {
  Publish();
  const uint64_t end = published_.load(std::memory_order_relaxed);
  // The worker is usually only a few ops behind: yield briefly first.
  for (int i = 0; i < 2000 && replayed_.load(std::memory_order_acquire) != end; ++i) std::this_thread::yield();
  if (replayed_.load(std::memory_order_acquire) != end) {
    waiting_finish_.store(true, std::memory_order_seq_cst);
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return replayed_.load(std::memory_order_seq_cst) == end; });
    waiting_finish_.store(false, std::memory_order_relaxed);
  }
  // Both at the start again, then a new epoch (so the worker restarts at 0:
  // seeing the new epoch, it sees the reset too).
  published_.store(0, std::memory_order_seq_cst);
  replayed_.store(0, std::memory_order_seq_cst);
  epoch_.fetch_add(1, std::memory_order_seq_cst);
  write_chunk_ = write_offset_ = 0;
}

void RecordingList::Worker() {
  svr2011::PerfHintThread();  // (android_perf.h: in the phone's hint session with the frame's thread)
  uint64_t pos = 0, epoch = 0;
  while (true) {
    const uint64_t e = epoch_.load(std::memory_order_seq_cst);
    if (e != epoch) epoch = e, pos = 0;
    uint64_t target = published_.load(std::memory_order_seq_cst);
    if (epoch_.load(std::memory_order_seq_cst) != e) continue;  // (a frame ended meanwhile)
    if (target < pos) {  // (reset by Finish, the epoch not yet advanced)
      std::this_thread::yield();
      continue;
    }
    if (target == pos) {
      // Caught up: spin a little (the next ops usually come within
      // microseconds), then sleep until woken.
      for (int i = 0; i < 256 && target == pos; ++i) {  // (a reset shows as target < pos)
        std::this_thread::yield();
        target = published_.load(std::memory_order_seq_cst);
      }
      if (target <= pos || epoch_.load(std::memory_order_seq_cst) != e) {
        std::unique_lock lock(mutex_);
        sleeping_.store(true, std::memory_order_seq_cst);
        work_.wait(lock, [&] {
          return stop_ || epoch_.load(std::memory_order_seq_cst) != e ||
                 published_.load(std::memory_order_seq_cst) != pos;
        });
        sleeping_.store(false, std::memory_order_relaxed);
        if (stop_) return;
        continue;
      }
    }
    while (pos != target) {
      const size_t chunk = size_t(pos >> 32), offset = size_t(pos & 0xFFFFFFFFu);
      const uint8_t* p = chunks_[chunk]->data + offset;
      Header h;
      std::memcpy(&h, p, sizeof(h));
      if (Op(h.op) == Op::kNextChunk) {
        pos = uint64_t(chunk + 1) << 32;
        continue;
      }
      Replay(p);
      pos += h.size;
    }
    replayed_.store(pos, std::memory_order_seq_cst);
    if (waiting_finish_.load(std::memory_order_seq_cst)) {
      { std::lock_guard lock(mutex_); }  // (Finish is either not yet checking, or waiting)
      done_.notify_all();
    }
  }
}

size_t RecordingList::Replay(const uint8_t* p) {
  Header h;
  std::memcpy(&h, p, sizeof(h));
  In in{p + sizeof(Header)};
  plume::RenderCommandList* t = target_;
  switch (Op(h.op)) {
    case Op::kBegin:
      t->begin();
      break;
    case Op::kEnd:
      t->end();
      break;
    case Op::kBarriers: {
      const auto stages = in.get<plume::RenderBarrierStages>();
      in.get<uint32_t>();  // (pad)
      const auto nb = in.get<uint32_t>(), nt = in.get<uint32_t>();
      const auto* b = in.array<plume::RenderBufferBarrier>(nb);
      const auto* tx = in.array<plume::RenderTextureBarrier>(nt);
      t->barriers(stages, b, nb, tx, nt);
      break;
    }
    case Op::kDraw: {
      const auto a = in.get<uint32_t>(), b = in.get<uint32_t>(), c = in.get<uint32_t>(), d = in.get<uint32_t>();
      t->drawInstanced(a, b, c, d);
      break;
    }
    case Op::kDrawIndexed: {
      const auto a = in.get<uint32_t>(), b = in.get<uint32_t>(), c = in.get<uint32_t>();
      const auto d = in.get<int32_t>();
      const auto e = in.get<uint32_t>();
      t->drawIndexedInstanced(a, b, c, d, e);
      break;
    }
    case Op::kPipeline:
      t->setPipeline(in.get<const plume::RenderPipeline*>());
      break;
    case Op::kGraphicsLayout:
      t->setGraphicsPipelineLayout(in.get<const plume::RenderPipelineLayout*>());
      break;
    case Op::kPushConstants: {
      const auto range = in.get<uint32_t>(), offset = in.get<uint32_t>(), size = in.get<uint32_t>();
      in.get<uint32_t>();  // (pad)
      t->setGraphicsPushConstants(range, in.array<uint8_t>(size), offset, size);
      break;
    }
    case Op::kDescriptorSet: {
      auto* set = in.get<plume::RenderDescriptorSet*>();
      t->setGraphicsDescriptorSet(set, in.get<uint32_t>());
      break;
    }
    case Op::kDescriptorSetDynamic: {
      auto* set = in.get<plume::RenderDescriptorSet*>();
      const auto index = in.get<uint32_t>(), count = in.get<uint32_t>();
      t->setGraphicsDescriptorSetDynamic(set, index, in.array<uint32_t>(count), count);
      break;
    }
    case Op::kRootDescriptor: {
      const auto ref = in.get<plume::RenderBufferReference>();
      t->setGraphicsRootDescriptor(ref, in.get<uint32_t>());
      break;
    }
    case Op::kIndexBuffer: {
      const auto view = in.get<plume::RenderIndexBufferView>();
      t->setIndexBuffer(&view);
      break;
    }
    case Op::kVertexBuffers: {
      const auto start = in.get<uint32_t>(), n = in.get<uint32_t>();
      const auto* views = in.array<plume::RenderVertexBufferView>(n);
      const auto* slots = in.array<plume::RenderInputSlot>(n);
      t->setVertexBuffers(start, views, n, slots);
      break;
    }
    case Op::kViewports: {
      const auto n = in.get<uint32_t>();
      in.get<uint32_t>();
      t->setViewports(in.array<plume::RenderViewport>(n), n);
      break;
    }
    case Op::kScissors: {
      const auto n = in.get<uint32_t>();
      in.get<uint32_t>();
      t->setScissors(in.array<plume::RenderRect>(n), n);
      break;
    }
    case Op::kFramebuffer:
      t->setFramebuffer(in.get<const plume::RenderFramebuffer*>());
      break;
    case Op::kDepthBias: {
      const auto a = in.get<float>(), b = in.get<float>(), c = in.get<float>();
      t->setDepthBias(a, b, c);
      break;
    }
    case Op::kStencilReference:
      t->setStencilReference(in.get<uint32_t>());
      break;
    case Op::kBlendFactor: {
      const auto* f = in.array<float>(4);
      t->setBlendFactor(f);
      break;
    }
    case Op::kClearColor: {
      const auto index = in.get<uint32_t>(), n = in.get<uint32_t>();
      const auto color = in.get<plume::RenderColor>();
      t->clearColor(index, color, in.array<plume::RenderRect>(n), n);
      break;
    }
    case Op::kClearDepthStencil: {
      const auto flags = in.get<uint32_t>(), stencil = in.get<uint32_t>();
      const float depth = in.get<float>();
      const uint32_t n = in.get<uint32_t>();
      t->clearDepthStencil(flags & 1, flags & 2, depth, stencil, in.array<plume::RenderRect>(n), n);
      break;
    }
    case Op::kCopyTextureRegion: {
      const auto dst = in.get<plume::RenderTextureCopyLocation>();
      const auto src = in.get<plume::RenderTextureCopyLocation>();
      const auto x = in.get<uint32_t>(), y = in.get<uint32_t>(), z = in.get<uint32_t>(),
                 has_box = in.get<uint32_t>();
      const auto box = in.get<plume::RenderBox>();
      t->copyTextureRegion(dst, src, x, y, z, has_box ? &box : nullptr);
      break;
    }
    case Op::kCopyTexture: {
      const auto* dst = in.get<const plume::RenderTexture*>();
      t->copyTexture(dst, in.get<const plume::RenderTexture*>());
      break;
    }
    case Op::kNextChunk:
      break;
  }
  return h.size;
}

// ---- recording ----------------------------------------------------------

void RecordingList::begin() { Reserve(Op::kBegin, 0); }

void RecordingList::end() {
  Reserve(Op::kEnd, 0);
  Publish();
}

void RecordingList::barriers(plume::RenderBarrierStages stages, const plume::RenderBufferBarrier* b, uint32_t nb,
                             const plume::RenderTextureBarrier* t, uint32_t nt) {
  if (!b) nb = 0;
  if (!t) nt = 0;
  Out o{Reserve(Op::kBarriers, 16 + Align8(sizeof(*b) * nb) + Align8(sizeof(*t) * nt))};
  static_assert(sizeof(stages) == 4);
  o.put(stages);
  o.put(uint32_t(0));  // (pad: the arrays stay 8-aligned)
  o.put(nb);
  o.put(nt);
  o.put_bytes(b, sizeof(*b) * nb);
  o.put_bytes(t, sizeof(*t) * nt);
}

void RecordingList::drawInstanced(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  Out o{Reserve(Op::kDraw, 16)};
  o.put(a), o.put(b), o.put(c), o.put(d);
}

void RecordingList::drawIndexedInstanced(uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) {
  Out o{Reserve(Op::kDrawIndexed, 20)};
  o.put(a), o.put(b), o.put(c), o.put(d), o.put(e);
}

void RecordingList::setPipeline(const plume::RenderPipeline* pipeline) {
  Out o{Reserve(Op::kPipeline, sizeof(pipeline))};
  o.put(pipeline);
}

void RecordingList::setGraphicsPipelineLayout(const plume::RenderPipelineLayout* layout) {
  Out o{Reserve(Op::kGraphicsLayout, sizeof(layout))};
  o.put(layout);
}

void RecordingList::setGraphicsPushConstants(uint32_t range, const void* data, uint32_t offset, uint32_t size) {
  Out o{Reserve(Op::kPushConstants, 16 + Align8(size))};
  o.put(range), o.put(offset), o.put(size), o.put(uint32_t(0));
  o.put_bytes(data, size);
}

void RecordingList::setGraphicsDescriptorSet(plume::RenderDescriptorSet* set, uint32_t index) {
  Out o{Reserve(Op::kDescriptorSet, sizeof(set) + 4)};
  o.put(set), o.put(index);
}

void RecordingList::setGraphicsDescriptorSetDynamic(plume::RenderDescriptorSet* set, uint32_t index,
                                                    const uint32_t* offsets, uint32_t count) {
  Out o{Reserve(Op::kDescriptorSetDynamic, sizeof(set) + 8 + Align8(count * 4))};
  o.put(set), o.put(index), o.put(count);
  o.put_bytes(offsets, count * 4);
}

void RecordingList::setGraphicsRootDescriptor(plume::RenderBufferReference ref, uint32_t index) {
  Out o{Reserve(Op::kRootDescriptor, sizeof(ref) + 4)};
  o.put(ref), o.put(index);
}

void RecordingList::setIndexBuffer(const plume::RenderIndexBufferView* view) {
  Out o{Reserve(Op::kIndexBuffer, sizeof(*view))};
  o.put(*view);
}

void RecordingList::setVertexBuffers(uint32_t start, const plume::RenderVertexBufferView* views, uint32_t n,
                                     const plume::RenderInputSlot* slots) {
  Out o{Reserve(Op::kVertexBuffers, 8 + Align8(sizeof(*views) * n) + Align8(sizeof(*slots) * n))};
  o.put(start), o.put(n);
  o.put_bytes(views, sizeof(*views) * n);
  o.put_bytes(slots, sizeof(*slots) * n);
}

void RecordingList::setViewports(const plume::RenderViewport* viewports, uint32_t n) {
  Out o{Reserve(Op::kViewports, 8 + Align8(sizeof(*viewports) * n))};
  o.put(n), o.put(uint32_t(0));
  o.put_bytes(viewports, sizeof(*viewports) * n);
}

void RecordingList::setScissors(const plume::RenderRect* rects, uint32_t n) {
  Out o{Reserve(Op::kScissors, 8 + Align8(sizeof(*rects) * n))};
  o.put(n), o.put(uint32_t(0));
  o.put_bytes(rects, sizeof(*rects) * n);
}

void RecordingList::setFramebuffer(const plume::RenderFramebuffer* framebuffer) {
  Out o{Reserve(Op::kFramebuffer, sizeof(framebuffer))};
  o.put(framebuffer);
}

void RecordingList::setDepthBias(float a, float b, float c) {
  Out o{Reserve(Op::kDepthBias, 12)};
  o.put(a), o.put(b), o.put(c);
}

void RecordingList::setStencilReference(uint32_t reference) {
  Out o{Reserve(Op::kStencilReference, 4)};
  o.put(reference);
}

void RecordingList::setBlendFactor(const float factor[4]) {
  Out o{Reserve(Op::kBlendFactor, 16)};
  o.put_bytes(factor, 16);
}

void RecordingList::clearColor(uint32_t index, plume::RenderColor color, const plume::RenderRect* rects,
                               uint32_t n) {
  if (!rects) n = 0;
  Out o{Reserve(Op::kClearColor, 8 + Align8(sizeof(color)) + Align8(sizeof(*rects) * n))};
  o.put(index), o.put(n);
  o.put(color);
  o.p = o.p + (Align8(sizeof(color)) - sizeof(color));
  o.put_bytes(rects, sizeof(*rects) * n);
}

void RecordingList::clearDepthStencil(bool depth, bool stencil, float value, uint32_t stencil_value,
                                      const plume::RenderRect* rects, uint32_t n) {
  if (!rects) n = 0;
  Out o{Reserve(Op::kClearDepthStencil, 16 + Align8(sizeof(*rects) * n))};
  o.put(uint32_t((depth ? 1 : 0) | (stencil ? 2 : 0))), o.put(stencil_value), o.put(value), o.put(n);
  o.put_bytes(rects, sizeof(*rects) * n);
}

void RecordingList::copyTextureRegion(const plume::RenderTextureCopyLocation& dst,
                                      const plume::RenderTextureCopyLocation& src, uint32_t x, uint32_t y,
                                      uint32_t z, const plume::RenderBox* box) {
  Out o{Reserve(Op::kCopyTextureRegion, sizeof(dst) * 2 + 16 + sizeof(plume::RenderBox))};
  o.put(dst), o.put(src);
  o.put(x), o.put(y), o.put(z), o.put(uint32_t(box ? 1 : 0));
  o.put(box ? *box : plume::RenderBox());
}

void RecordingList::copyTexture(const plume::RenderTexture* dst, const plume::RenderTexture* src) {
  Out o{Reserve(Op::kCopyTexture, sizeof(dst) * 2)};
  o.put(dst), o.put(src);
}

}  // namespace svr2011::native
