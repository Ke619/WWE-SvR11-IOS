// WWE SmackDown vs. Raw 2011 - HD entrance movies (see hd_movies.h).
//
// The game's movies are Bink (recompiled guest code, 0x82D4xxxx): BinkOpen
// (sub_82D49B80(name, flags)) reads the header, the movie's textures are made
// at its size (one 8-bit texture per Y / cR / cB plane, the colour in a
// shader; titantron movies are 320 x 320, the game's own go to 1024 x 1024),
// and Bink decodes each frame straight into one of two frame buffer sets. A
// movie object (sub_82592B90(movie) steps it: BinkDoFrame) has at +12 the
// Bink (+8 frames, +12 the frame number, from 1) and at +32 the frame buffer
// sets: +132 / +136 the Y plane's width / height, +140 / +144 the cR / cB
// planes', +148 the set just decoded, then per set (48 bytes from +152) the
// Y, cR, cB planes' {allocate, buffer, pitch} (sub_825930F8 copies that set
// to the textures' own at +36 for the screens).
//
// An HD movie is an .mp4: the game opens a stub .bik of its size and length
// (every frame "all skip": nearly free to decode) and after each step the
// decoder's frame for that time is written over the set Bink just decoded.
// Size: 320 for a 320 .bik fallback or a square .mp4 up to 320, else 1024 (640's
// luma drew shifted: not used) - the
// game's own movie layout at that size (the big screen the top size*220/320
// rows, shown at 16:9, the stage strip below): a square .mp4 is taken as
// laid out so, any other fills the big screen (stretched to it) over a black
// strip.
//
// Windows: Media Foundation's source reader (advanced video processing: it
// scales to the size and gives NV12) on a thread per open movie, a frame
// ahead. Where it can't decode (no H.264: a Windows "N" edition, Proton on
// the Steam Deck) RegisterHdMovie says no and the fallback .bik plays.
// Android: the NDK's MediaExtractor + MediaCodec (the phone's own decoder),
// its output buffers scaled to the frame size on the movie's thread; no
// decoder for it on the phone: the fallback .bik.
//
// Test aid: SVR2011_TEST_HD_MOVIES=1 logs the decoders.

#include "hd_movies.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/thread.h>

#include "generated/default/svr2011_init.h"
#include "user_movies.h"
#include "util.h"

extern "C" {
#include "bink_encode.h"
}

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <strmif.h>
#include <wrl/client.h>
#include <initguid.h>  // (codecapi.h's GUIDs, defined here)
#include <codecapi.h>
using Microsoft::WRL::ComPtr;
#elif defined(__ANDROID__)
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kFps = 30;
constexpr const char* kHdFolder = ".hd";

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool LogOn() {
  static const bool on = [] {
    const char* e = std::getenv("SVR2011_TEST_HD_MOVIES");
    return e && *e == '1';
  }();
  return on;
}

struct HdMovie {
  fs::path mp4;
  int size = 1024;  // the stub's (square)
  bool square = false;  // the mp4 is laid out like the game's (else: the big screen only)
};
std::mutex g_mutex;
std::map<std::string, HdMovie> g_movies;  // by the stub's file name (lower case)

using svr2011::Lower;

// A .bik's size from its header (0: not one).
int BinkSize(const fs::path& bik) {
  uint8_t h[28] = {};
  FILE* f = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&f, bik.wstring().c_str(), L"rb")) f = nullptr;
#else
  f = std::fopen(bik.c_str(), "rb");
#endif
  if (!f) return 0;
  const size_t n = std::fread(h, 1, sizeof h, f);
  std::fclose(f);
  if (n < sizeof h || std::memcmp(h, "BIK", 3)) return 0;
  const int w = h[20] | h[21] << 8, hh = h[24] | h[25] << 8;
  return w == hh && (w == 320 || w == 640 || w == 1024) ? w : 0;
}

// The stub: `frames` frames at size x size, black, then the same.
bool WriteStub(const fs::path& out, int size, int frames) {
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  const fs::path part = out.string() + ".part";
  FILE* f = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&f, part.wstring().c_str(), L"wb")) f = nullptr;
#else
  f = std::fopen(part.c_str(), "wb");
#endif
  if (!f) return false;
  BinkWriter* w = bink_writer_open_size(f, frames, size, size);
  bool ok = w != nullptr;
  if (ok) {
    std::vector<uint8_t> black(size_t(size) * size * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
    ok = bink_writer_frame(w, black.data()) == 1;
    for (int k = 1; ok && k < frames; ++k) ok = bink_writer_repeat(w) == 1;
    if (ok) ok = bink_writer_close(w) != 0;
    else bink_writer_free(w);
  }
  std::fclose(f);
  if (ok) {
    fs::rename(part, out, ec);
    ok = !ec;
  }
  if (!ok) fs::remove(part, ec);
  return ok;
}

// One open HD movie: the platform's thread (Run) decoding a frame ahead of
// the game into a ring of three NV12 frames at w_ x h_; Write copies the one
// due into Bink's planes.
class DecoderBase {
 public:
  explicit DecoderBase(HdMovie movie) : movie_(std::move(movie)) {
    w_ = uint32_t(movie_.size);
    h_ = movie_.square ? w_ : w_ * 220 / 320;
    h_ &= ~1u;
  }
  virtual ~DecoderBase() = default;

  // Copies the frame for `seconds` (the newest at or before it; none yet:
  // nothing) into Bink's planes: Y (pitch ypitch, the top h_ rows), cR / cB
  // (half size). The strip below stays the stub's black.
  void Write(double seconds, uint8_t* y, uint32_t ypitch, uint8_t* cr, uint8_t* cb, uint32_t cpitch) {
    want_ = seconds;
    std::lock_guard lock(mutex_);
    const Frame* best = nullptr;
    for (const Frame& f : frames_)
      if (f.ready && f.time <= seconds + 0.5 / kFps && (!best || f.time > best->time)) best = &f;
    // (the game showing the movie 3 s with no frame from the decoder: said once - the
    // screen is the stub's black)
    if (!best && !shown_ && !told_ && seconds >= 3.0) {
      told_ = true;
      REXLOG_WARN("[svr2011] hd movies: {} has shown 3 s with no decoded frame (black)",
                  movie_.mp4.filename().string());
    }
    if (!best) return;
    shown_ = true;
    for (uint32_t r = 0; r < h_; ++r) std::memcpy(y + size_t(r) * ypitch, best->nv12.data() + size_t(r) * w_, w_);
    // (cR / cB rows go `down` rows below their luma rows: the game's
    // Bink planes have them so - its own decodes measured: 12 rows at 320, 6 at
    // 1024 - and the screens draw them back up; level, the colour sat that far
    // above the picture: pink and green edges)
    // (test aid SVR2011_TEST_CHROMA_DOWN=<rows>: that many instead - an A/B of the colour's place)
    static const int test_down = [] { const char* v = std::getenv("SVR2011_TEST_CHROMA_DOWN"); return v ? std::atoi(v) : -1; }();
    const uint32_t down = test_down >= 0 ? uint32_t(test_down) : w_ == 320 ? 12 : 6;
    const uint8_t* uv = best->nv12.data() + size_t(w_) * h_;
    for (uint32_t r = 0; r < h_ / 2 && r + down < w_ / 2; ++r) {
      uint8_t* u = cb + size_t(r + down) * cpitch;
      uint8_t* v = cr + size_t(r + down) * cpitch;
      const uint8_t* s = uv + size_t(r) * w_;
      for (uint32_t x = 0; x < w_ / 2; ++x) u[x] = s[2 * x], v[x] = s[2 * x + 1];
    }
  }

 protected:
  struct Frame {
    std::vector<uint8_t> nv12;
    double time = -1;
    bool ready = false;
  };

  // (the subclass starts its thread when constructed and stops it in its
  // destructor, while its own members still exist)
  void Start() { thread_ = std::thread([this] { rex::thread::set_current_thread_name("hd movie"); Run(); }); }
  void Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  virtual void Run() = 0;

  // The game looped (it wants a time well before the last frame): true -
  // the decoder starts again from the beginning.
  bool Looped(double last) const { return last >= 0 && want_.load() + 1.0 < last; }
  // Far enough ahead of the game (or at the end): wait.
  bool Ahead(double last) const { return last >= 0 && last > want_.load() + 2.0 / kFps; }
  void Clear() {
    std::lock_guard lock(mutex_);
    for (Frame& f : frames_) f.ready = false;
  }
  void Push(Frame&& f) {
    std::lock_guard lock(mutex_);
    // (the oldest of three goes)
    auto oldest = std::min_element(frames_, frames_ + 3, [](const Frame& a, const Frame& b) {
      return !a.ready ? true : !b.ready ? false : a.time < b.time;
    });
    *oldest = std::move(f);
  }

  HdMovie movie_;
  uint32_t w_ = 0, h_ = 0;
  std::atomic<bool> stop_{false};

 private:
  std::thread thread_;
  std::atomic<double> want_{0.0};
  bool shown_ = false, told_ = false;  // (Write's: a frame shown; the no-frame warning given)
  std::mutex mutex_;
  Frame frames_[3];
};

#if defined(_WIN32)

// Media Foundation on this thread (COM, multithreaded) - once per thread.
bool MfOn() {
  thread_local int state = 0;  // 0 not tried, 1 on, 2 failed
  if (!state) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    state = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)) ? 1 : 2;
  }
  return state == 1;
}

// The GPU's video decoder for the readers: a D3D11 device (video support,
// shared by the movies' threads) in a DXGI device manager - without one
// Media Foundation decodes H.264 and scales on the CPU, next to the game's
// own threads. nullptr: none (no such GPU / driver): the CPU then. A device
// the driver lost (removed: a driver reset, the GPU's power state) is made
// again - every reader on the lost one gives no pictures.
ComPtr<IMFDXGIDeviceManager> GpuManager() {
  static std::mutex mutex;
  static ComPtr<ID3D11Device> device;
  static ComPtr<IMFDXGIDeviceManager> manager;
  static bool failed = false;
  std::lock_guard lock(mutex);
  if (device && device->GetDeviceRemovedReason() != S_OK) {
    REXLOG_WARN("[svr2011] hd movies: the GPU video device was lost (0x{:08X}) - made again",
                uint32_t(device->GetDeviceRemovedReason()));
    device.Reset(), manager.Reset();
  }
  if (manager || failed) return manager;
  D3D_FEATURE_LEVEL level;
  ComPtr<ID3D11Device> d;
  ComPtr<IMFDXGIDeviceManager> m;
  UINT token = 0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr,
                               0, D3D11_SDK_VERSION, &d, &level, nullptr)) ||
      FAILED(MFCreateDXGIDeviceManager(&token, &m))) {
    failed = true;
    return nullptr;
  }
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(d.As(&mt))) mt->SetMultithreadProtected(TRUE);
  if (FAILED(m->ResetDevice(d.Get(), token))) {
    failed = true;
    return nullptr;
  }
  device = d, manager = m;
  return manager;
}

// A source reader for `mp4` giving NV12 at w x h (scaled by the reader; on
// the GPU when `gpu` and there is one).
ComPtr<IMFSourceReader> OpenReader(const fs::path& mp4, uint32_t w, uint32_t h, bool gpu = false) {
  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFSourceReader> reader;
  ComPtr<IMFDXGIDeviceManager> manager = gpu ? GpuManager() : nullptr;
  if (gpu && !manager) return nullptr;
  if (FAILED(MFCreateAttributes(&attrs, 3)) ||
      (manager && FAILED(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.Get()))) ||
      FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)) ||
      FAILED(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE)) ||  // (the GPU's decoder where it has one)
      FAILED(MFCreateSourceReaderFromURL(mp4.wstring().c_str(), attrs.Get(), &reader)))
    return nullptr;
  const DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(video, TRUE);
  ComPtr<IMFMediaType> type;
  if (FAILED(MFCreateMediaType(&type)) || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
      FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h)) ||
      FAILED(reader->SetCurrentMediaType(video, nullptr, type.Get())))
    return nullptr;
  return reader;
}

// The mp4's length (s) and source size, and whether a frame decodes here.
bool Probe(const fs::path& mp4, double* seconds, uint32_t* sw, uint32_t* sh) {
  if (!MfOn()) return false;
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromURL(mp4.wstring().c_str(), nullptr, &reader))) return false;
  PROPVARIANT v;
  PropVariantInit(&v);
  if (FAILED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v))) return false;
  *seconds = double(v.uhVal.QuadPart) / 1e7;
  PropVariantClear(&v);
  ComPtr<IMFMediaType> native;
  if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native)) ||
      FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, sw, sh)) || !*sw || !*sh)
    return false;
  // (a frame: the decoder exists - not so without H.264)
  ComPtr<IMFSourceReader> test = OpenReader(mp4, 320, 320);
  if (!test) return false;
  DWORD flags = 0;
  LONGLONG t = 0;
  ComPtr<IMFSample> sample;
  for (int k = 0; k < 8 && !sample; ++k)
    if (FAILED(test->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &t, &sample)) ||
        (flags & MF_SOURCE_READERF_ENDOFSTREAM))
      break;
  return sample != nullptr;
}

// Windows: Media Foundation's source reader (GPU decode and scale where it can).
class Decoder : public DecoderBase {
 public:
  explicit Decoder(HdMovie movie) : DecoderBase(std::move(movie)) { Start(); }
  ~Decoder() override { Stop(); }

 private:
  void Run() override {
    // (below the game's own threads: a frame late on the titantron is better
    // than a frame late in the match)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!MfOn()) {
      REXLOG_WARN("[svr2011] hd movies: no Media Foundation on the decoder's thread - {} stays black",
                  movie_.mp4.filename().string());
      return;
    }
    // (the GPU's decoder where it has one, else the CPU's; a GPU reader that
    // gives no picture - no frame in kGpuWait, a failed read or copy before
    // the first - goes over to the CPU's: a black screen otherwise)
    constexpr auto kGpuWait = std::chrono::milliseconds(1500);
    bool gpu = true;
    ComPtr<IMFSourceReader> reader = OpenReader(movie_.mp4, w_, h_, true);
    if (!reader) gpu = false, reader = OpenCpuReader();
    if (!reader) {
      REXLOG_WARN("[svr2011] hd movies: can't decode {}", movie_.mp4.string());
      return;
    }
    if (LogOn()) REXLOG_INFO("[svr2011] hd movies: decoding {} at {}x{} ({})", movie_.mp4.string(), w_, h_, gpu ? "GPU" : "CPU");
    auto opened = std::chrono::steady_clock::now();
    bool pictured = false;  // (a frame decoded and copied)
    const auto to_cpu = [&](const char* why) {
      REXLOG_WARN("[svr2011] hd movies: {} gave no picture on the GPU ({}) - decoding on the CPU",
                  movie_.mp4.filename().string(), why);
      reader = OpenCpuReader();
      gpu = false;
      opened = std::chrono::steady_clock::now();
      return reader != nullptr;
    };
    double last = -1;
    bool ended = false;
    while (!stop_) {
      if (gpu && !pictured && std::chrono::steady_clock::now() - opened > kGpuWait) {
        if (!to_cpu("nothing in 1.5 s")) break;
        last = -1, ended = false;
        Clear();
      }
      if (Looped(last)) {
        PROPVARIANT p;
        PropVariantInit(&p);
        p.vt = VT_I8;
        p.hVal.QuadPart = 0;
        reader->SetCurrentPosition(GUID_NULL, p);
        last = -1;
        ended = false;
        Clear();
      }
      if (ended || Ahead(last)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }
      DWORD flags = 0;
      LONGLONG t = 0;
      ComPtr<IMFSample> sample;
      const HRESULT read =
          reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &t, &sample);
      if (FAILED(read) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        if (!pictured && gpu) {
          if (!to_cpu(FAILED(read) ? "a failed read" : "no frame before the end")) break;
          last = -1;
          continue;
        }
        if (!pictured && !ended)
          REXLOG_WARN("[svr2011] hd movies: {}: no frame decoded (0x{:08X}) - it stays black",
                      movie_.mp4.filename().string(), uint32_t(read));
        ended = true;  // (the end: the last frame stays until the game loops or closes)
        continue;
      }
      if (!sample) continue;
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
        if (!pictured && gpu && !to_cpu("a frame it couldn't copy")) break;
        continue;
      }
      Frame f;
      f.nv12.resize(size_t(w_) * h_ * 3 / 2);
      ComPtr<IMF2DBuffer> b2;
      BYTE* data = nullptr;
      LONG pitch = 0;
      bool copied = false;
      if (SUCCEEDED(buffer.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) {
        for (uint32_t r = 0; r < h_; ++r) std::memcpy(f.nv12.data() + size_t(r) * w_, data + LONG_PTR(pitch) * r, w_);
        const BYTE* uv = data + LONG_PTR(pitch) * h_;
        for (uint32_t r = 0; r < h_ / 2; ++r)
          std::memcpy(f.nv12.data() + size_t(w_) * h_ + size_t(r) * w_, uv + LONG_PTR(pitch) * r, w_);
        b2->Unlock2D();
        copied = true;
      } else {
        DWORD len = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &len))) {
          std::memcpy(f.nv12.data(), data, std::min<size_t>(len, f.nv12.size()));
          buffer->Unlock();
          copied = true;
        }
      }
      // (test aid: SVR2011_TEST_HD_NO_GPU_FRAMES=1 - the GPU's frames dropped, as if it gave none)
      static const bool no_gpu_frames = std::getenv("SVR2011_TEST_HD_NO_GPU_FRAMES") != nullptr;
      if (gpu && no_gpu_frames) copied = false;
      if (!copied) {
        if (!pictured && gpu && !to_cpu("a frame it couldn't copy")) break;
        continue;
      }
      if (!pictured && (gpu ? LogOn() : true))
        REXLOG_INFO("[svr2011] hd movies: {}: first frame after {} ms ({})", movie_.mp4.filename().string(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened)
                        .count(),
                    gpu ? "GPU" : "CPU");
      pictured = true;
      f.time = double(t) / 1e7;
      f.ready = true;
      last = f.time;
      Push(std::move(f));
    }
  }

  // The CPU's reader: its decoder on 2 threads (its own work queue took half
  // a core next to the game's threads).
  ComPtr<IMFSourceReader> OpenCpuReader() {
    ComPtr<IMFSourceReader> reader = OpenReader(movie_.mp4, w_, h_);
    ComPtr<IMFSourceReaderEx> ex;
    ComPtr<IMFTransform> decoder;
    ComPtr<ICodecAPI> codec;
    GUID category = {};
    if (reader && SUCCEEDED(reader.As(&ex)) &&
        SUCCEEDED(ex->GetTransformForStream(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &category, &decoder)) &&
        SUCCEEDED(decoder.As(&codec))) {
      VARIANT v;
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = 2;
      codec->SetValue(&CODECAPI_AVDecNumWorkerThreads, &v);
    }
    return reader;
  }
};

#elif defined(__ANDROID__)

// Android: the NDK's MediaExtractor + MediaCodec (the phone's own decoder),
// scaled here to the frame size.

// The mp4's video track.
struct Track {
  int fd = -1;
  AMediaExtractor* extractor = nullptr;
  AMediaFormat* format = nullptr;
  const char* mime = nullptr;
  int32_t w = 0, h = 0;
  int64_t duration = 0;  // (microseconds)
  ~Track() {
    if (format) AMediaFormat_delete(format);
    if (extractor) AMediaExtractor_delete(extractor);
    if (fd >= 0) close(fd);
  }
};

bool OpenTrack(const fs::path& mp4, Track* t) {
  t->fd = open(mp4.c_str(), O_RDONLY);
  if (t->fd < 0) return false;
  std::error_code ec;
  const auto size = fs::file_size(mp4, ec);
  t->extractor = AMediaExtractor_new();
  if (!t->extractor || AMediaExtractor_setDataSourceFd(t->extractor, t->fd, 0, off64_t(size)) != AMEDIA_OK) return false;
  for (size_t i = 0; i < AMediaExtractor_getTrackCount(t->extractor) && !t->format; ++i) {
    AMediaFormat* f = AMediaExtractor_getTrackFormat(t->extractor, i);
    const char* m = nullptr;
    if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && std::strncmp(m, "video/", 6) == 0) {
      t->format = f, t->mime = m;
      AMediaExtractor_selectTrack(t->extractor, i);
    } else {
      AMediaFormat_delete(f);
    }
  }
  if (!t->format) return false;
  AMediaFormat_getInt32(t->format, AMEDIAFORMAT_KEY_WIDTH, &t->w);
  AMediaFormat_getInt32(t->format, AMEDIAFORMAT_KEY_HEIGHT, &t->h);
  AMediaFormat_getInt64(t->format, AMEDIAFORMAT_KEY_DURATION, &t->duration);
  return t->w > 0 && t->h > 0;
}

// The mp4's length (s) and source size, and whether this phone has a decoder for it.
bool Probe(const fs::path& mp4, double* seconds, uint32_t* sw, uint32_t* sh) {
  Track t;
  if (!OpenTrack(mp4, &t)) return false;
  *seconds = double(t.duration) / 1e6;
  *sw = uint32_t(t.w), *sh = uint32_t(t.h);
  AMediaCodec* codec = AMediaCodec_createDecoderByType(t.mime);
  if (!codec) return false;
  AMediaCodec_delete(codec);
  return true;
}

// The decoder's own output buffers, asked for as semi-planar (NV12), else
// planar (I420), the planes `stride` bytes a row and `slice` rows apart. (Not
// a surface into an image reader: on the Galaxy Z Fold7 that gave no picture
// at all - every mod's tron black; and not "flexible": its buffers then say
// only that - no telling NV12 from I420.)
constexpr int32_t kColourPlanar = 19, kColourPackedPlanar = 20, kColourSemiPlanar = 21,
                  kColourPackedSemiPlanar = 39, kColourQcom32m = 0x7FA30C04,  // (Qualcomm's NV12, 128 / 32 aligned)
    kColourFlexible = 0x7F420888;

class Decoder : public DecoderBase {
 public:
  explicit Decoder(HdMovie movie) : DecoderBase(std::move(movie)) { Start(); }
  ~Decoder() override { Stop(); }

 private:
  // The output buffers' layout (the decoder's output format).
  struct Layout {
    int32_t w = 0, h = 0;          // (the picture: the crop's size)
    int32_t left = 0, top = 0;     // (its corner in the buffer)
    int32_t stride = 0, slice = 0;
    int32_t colour = 0;
    bool semi = false, known = false;
  };

  Layout ReadLayout(AMediaFormat* f) const {
    Layout l;
    int32_t bw = 0, bh = 0;
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &bw);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &bh);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &l.stride);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &l.slice);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &l.colour);
    if (l.stride < bw) l.stride = bw;
    if (l.slice < bh) l.slice = bh;
    l.w = bw, l.h = bh;
    int32_t cl = 0, ct = 0, cr = 0, cb = 0;
    if (AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &cl, &ct, &cr, &cb) && cr >= cl && cb >= ct &&
        cr < l.stride && cb < l.slice)
      l.left = cl & ~1, l.top = ct & ~1, l.w = cr - cl + 1, l.h = cb - ct + 1;
    // (a decoder that still says only "flexible": the layout it took when asked)
    const int32_t c = l.colour == kColourFlexible ? asked_ : l.colour;
    l.semi = c == kColourSemiPlanar || c == kColourPackedSemiPlanar || c == kColourQcom32m;
    l.known = l.w > 0 && l.h > 0 && (l.semi || c == kColourPlanar || c == kColourPackedPlanar);
    return l;
  }

  int32_t asked_ = kColourFlexible;  // (the colour format the decoder took)

  void Run() override {
    setpriority(PRIO_PROCESS, 0, 10);  // (this thread: below the game's own)
    Track t;
    AMediaCodec* codec = nullptr;
    bool ok = OpenTrack(movie_.mp4, &t);
    if (ok) {
      codec = AMediaCodec_createDecoderByType(t.mime);
      ok = false;
      for (const int32_t colour : {kColourSemiPlanar, kColourPlanar, kColourFlexible}) {
        if (!codec) break;
        AMediaFormat_setInt32(t.format, AMEDIAFORMAT_KEY_COLOR_FORMAT, colour);
        if (AMediaCodec_configure(codec, t.format, nullptr, nullptr, 0) == AMEDIA_OK) {
          asked_ = colour;
          ok = AMediaCodec_start(codec) == AMEDIA_OK;
          break;
        }
        // (refused: a fresh decoder for the next ask - a failed configure can leave it unusable)
        AMediaCodec_delete(codec);
        codec = AMediaCodec_createDecoderByType(t.mime);
      }
    }
    if (!ok) {
      REXLOG_WARN("[svr2011] hd movies: can't decode {}", movie_.mp4.string());
    } else {
      if (LogOn())
        REXLOG_INFO("[svr2011] hd movies: decoding {} ({}x{}) at {}x{} (MediaCodec)", movie_.mp4.string(), t.w, t.h, w_, h_);
      Decode(t, codec);
      AMediaCodec_stop(codec);
    }
    if (codec) AMediaCodec_delete(codec);
  }

  void Decode(Track& t, AMediaCodec* codec) {
    constexpr int64_t kWaitUs = 2000;
    const auto opened = std::chrono::steady_clock::now();
    Layout layout;
    bool have_layout = false, pictured = false, told = false;
    double last = -1;
    bool input_done = false, ended = false;
    while (!stop_) {
      if (Looped(last)) {
        AMediaExtractor_seekTo(t.extractor, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        AMediaCodec_flush(codec);
        last = -1;
        input_done = ended = false;
        Clear();
      }
      if (ended || Ahead(last)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }
      if (!input_done) {
        const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, kWaitUs);
        if (in >= 0) {
          size_t size = 0;
          uint8_t* data = AMediaCodec_getInputBuffer(codec, size_t(in), &size);
          const ssize_t n = data ? AMediaExtractor_readSampleData(t.extractor, data, size) : -1;
          if (n < 0) {
            AMediaCodec_queueInputBuffer(codec, size_t(in), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            input_done = true;
          } else {
            AMediaCodec_queueInputBuffer(codec, size_t(in), 0, size_t(n), uint64_t(AMediaExtractor_getSampleTime(t.extractor)),
                                         0);
            AMediaExtractor_advance(t.extractor);
          }
        }
      }
      AMediaCodecBufferInfo info;
      const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, kWaitUs);
      if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || (out >= 0 && !have_layout)) {
        AMediaFormat* f = AMediaCodec_getOutputFormat(codec);
        if (f) {
          layout = ReadLayout(f);
          AMediaFormat_delete(f);
          have_layout = true;
          if (LogOn() || !layout.known)
            REXLOG_INFO("[svr2011] hd movies: {}: the decoder gives {}x{} (crop at {},{}), stride {}, {} rows, colour 0x{:X} (asked {}){}",
                        movie_.mp4.filename().string(), layout.w, layout.h, layout.left, layout.top, layout.stride,
                        layout.slice, uint32_t(layout.colour), asked_, layout.known ? "" : " - a layout not read here: black");
        }
      }
      if (out < 0) continue;
      const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      size_t cap = 0;
      const uint8_t* buffer = info.size > 0 ? AMediaCodec_getOutputBuffer(codec, size_t(out), &cap) : nullptr;
      bool pushed = false;
      if (buffer && layout.known && size_t(info.offset) < cap) {
        Frame f;
        f.nv12.resize(size_t(w_) * h_ * 3 / 2);
        if (Scale(layout, buffer + info.offset, std::min(cap - size_t(info.offset), size_t(info.size)), f.nv12.data())) {
          f.time = double(info.presentationTimeUs) / 1e6;
          f.ready = true;
          last = f.time;
          Push(std::move(f));
          pushed = true;
        }
      }
      AMediaCodec_releaseOutputBuffer(codec, size_t(out), false);
      if (eos) ended = true;  // (the end: the last frame stays until the game loops or closes)
      if (pushed && !pictured) {
        pictured = true;
        if (LogOn())
          REXLOG_INFO("[svr2011] hd movies: {}: first frame after {} ms", movie_.mp4.filename().string(),
                      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - opened).count());
      } else if (!pushed && info.size > 0 && layout.known && !told) {
        told = true;
        REXLOG_WARN("[svr2011] hd movies: {}: a frame of {} bytes too small for its layout - skipped",
                    movie_.mp4.filename().string(), info.size);
      }
    }
  }

  // The decoder's picture (planar or semi-planar, `l`) to the frame's NV12 at
  // w_ x h_ (nearest pixel - the titantron's size is near the source's).
  // False: the buffer is shorter than its layout says.
  bool Scale(const Layout& l, const uint8_t* data, size_t len, uint8_t* nv12) {
    const size_t luma = size_t(l.stride) * l.slice;
    const uint8_t* y0 = data + size_t(l.top) * l.stride + l.left;
    const uint8_t *u0, *v0;
    size_t crow, cpix;
    if (l.semi) {
      crow = size_t(l.stride), cpix = 2;
      u0 = data + luma + size_t(l.top / 2) * crow + size_t(l.left / 2) * 2;
      v0 = u0 + 1;
    } else {
      crow = size_t(l.stride / 2), cpix = 1;
      u0 = data + luma + size_t(l.top / 2) * crow + l.left / 2;
      v0 = u0 + crow * size_t(l.slice / 2);
    }
    // (the last byte read: the bottom right of the last chroma plane)
    const uint8_t* end = std::max(u0, v0) + size_t((l.h + 1) / 2 - 1) * crow + size_t((l.w + 1) / 2 - 1) * cpix;
    if (end >= data + len) return false;
    for (uint32_t y = 0; y < h_; ++y) {
      const uint8_t* src = y0 + size_t(int64_t(y) * l.h / h_) * l.stride;
      uint8_t* dst = nv12 + size_t(y) * w_;
      for (uint32_t x = 0; x < w_; ++x) dst[x] = src[size_t(int64_t(x) * l.w / w_)];
    }
    uint8_t* uv = nv12 + size_t(w_) * h_;
    for (uint32_t y = 0; y < h_ / 2; ++y) {
      const size_t sy = size_t(int64_t(y) * l.h / h_);
      const uint8_t* u = u0 + sy * crow;
      const uint8_t* v = v0 + sy * crow;
      uint8_t* dst = uv + size_t(y) * w_;
      for (uint32_t x = 0; x < w_ / 2; ++x) {
        const size_t sx = size_t(int64_t(x) * l.w / w_);
        dst[2 * x] = u[sx * cpix], dst[2 * x + 1] = v[sx * cpix];
      }
    }
    return true;
  }
};

#else

bool Probe(const fs::path&, double*, uint32_t*, uint32_t*) { return false; }
class Decoder : public DecoderBase {
 public:
  explicit Decoder(HdMovie movie) : DecoderBase(std::move(movie)) {}

 private:
  void Run() override {}
};

#endif

std::mutex g_open_mutex;
std::map<uint32_t, std::unique_ptr<Decoder>> g_open;  // by the guest Bink

}  // namespace

namespace svr2011 {

bool PrepareHdMovie(const fs::path& movies, const std::string& file, const fs::path& mp4) {
  if (movies.empty()) return false;
  {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_movies.find(Lower(file)); it != g_movies.end() && it->second.mp4 == mp4) return true;
  }
  double seconds = 0;
  uint32_t sw = 0, sh = 0;
  bool decodes = false;
  // (on a thread of its own: Media Foundation's COM set-up stays off the caller's)
  std::thread([&] { rex::thread::set_current_thread_name("hd movie probe"); decodes = Probe(mp4, &seconds, &sw, &sh); }).join();
  if (!decodes || seconds <= 0) {
    REXLOG_INFO("[svr2011] hd movies: {} doesn't play here (no decoder) - its .bik does", mp4.filename().string());
    return false;
  }
  HdMovie m;
  m.mp4 = mp4;
  const int fallback = BinkSize(movies / file);
  m.square = sw * 10 / sh >= 9 && sw * 10 / sh <= 11;
  // (no .bik: a square .mp4 - a shrunk 320 x 320 entrance .bik, movie_shrink.py - at the
  // first of the game's sizes it fits, not 1024: ten times the pixels to decode for nothing)
  // (640: its luma drew a third of a screen to the right - 1024 instead)
  m.size = (fallback ? fallback == 320 : m.square && sw <= 320) ? 320 : 1024;
  const int frames = std::max(1, int(seconds * kFps + 0.5));
  const fs::path stub = movies / kHdFolder / file;
  // (made again when the mp4 or the size changed: the stub's own size and
  // length say; same: kept)
  std::error_code ec;
  if (BinkSize(stub) != m.size || fs::last_write_time(stub, ec) < fs::last_write_time(mp4, ec)) {
    if (!WriteStub(stub, m.size, frames)) {
      REXLOG_WARN("[svr2011] hd movies: can't write {}", stub.string());
      return false;
    }
  }
  if (!fs::exists(movies / file, ec)) fs::copy_file(stub, movies / file, ec);  // (no fallback: the stub is the user movie)
  REXLOG_INFO("[svr2011] hd movies: {} plays {} ({}x{}, {:.1f} s, {})", file, mp4.filename().string(), m.size, m.size,
              seconds, m.square ? "laid out" : "big screen");
  std::lock_guard lock(g_mutex);
  g_movies[Lower(file)] = std::move(m);
  return true;
}

bool RegisterHdMovie(const std::string& file, const fs::path& mp4) {
  {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_movies.find(Lower(file)); it != g_movies.end() && it->second.mp4 == mp4) return true;
  }
  // (not prepared at start: a stub written now may not open until the next
  // start - the umovie: device knows the files it found at its mount)
  REXLOG_WARN("[svr2011] hd movies: {} wasn't prepared at start", file);
  return PrepareHdMovie(UserMoviesFolder(), file, mp4);
}

std::string HdMovieFile(const std::string& file) {
  std::lock_guard lock(g_mutex);
  return g_movies.count(Lower(file)) ? std::string(kHdFolder) + "\\" + file : file;
}

fs::path HdMovieMp4(const std::string& file) {
  std::lock_guard lock(g_mutex);
  const auto it = g_movies.find(Lower(file));
  return it != g_movies.end() ? it->second.mp4 : fs::path();
}

}  // namespace svr2011

// BinkOpen(name, flags) -> the Bink: an HD stub's gets its decoder.
REX_EXTERN(__imp__sub_82D49B80);
REX_HOOK_RAW(sub_82D49B80) {
  const uint32_t name = ctx.r3.u32;
  std::string n;
  if (name)
    for (uint32_t k = 0; k < 260 && base[name + k]; ++k) n.push_back(char(base[name + k]));
  __imp__sub_82D49B80(ctx, base);
  const uint32_t bink = ctx.r3.u32;
  if (LogOn()) REXLOG_INFO("[svr2011] hd movies: open {} -> {:08X}", n, bink);
  const std::string hd = std::string("\\") + kHdFolder + "\\";
  const size_t at = n.find(hd);
  if (!bink || at == std::string::npos) return;
  HdMovie m;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_movies.find(Lower(n.substr(at + hd.size())));
    if (it == g_movies.end()) return;
    m = it->second;
  }
  std::lock_guard lock(g_open_mutex);
  g_open[bink] = std::make_unique<Decoder>(std::move(m));
}

// BinkClose(bink).
REX_EXTERN(__imp__sub_82D4BEF8);
REX_HOOK_RAW(sub_82D4BEF8) {
  const uint32_t bink = ctx.r3.u32;
  {
    std::unique_ptr<Decoder> d;
    std::lock_guard lock(g_open_mutex);
    if (auto it = g_open.find(bink); it != g_open.end()) {
      d = std::move(it->second);
      g_open.erase(it);
    }
  }
  __imp__sub_82D4BEF8(ctx, base);
}

// A movie's step (sub_82592B90(movie): BinkDoFrame): an HD movie's frame
// over the set just decoded.
REX_EXTERN(__imp__sub_82592B90);
REX_HOOK_RAW(sub_82592B90) {
  const uint32_t movie = ctx.r3.u32;
  __imp__sub_82592B90(ctx, base);
  if (!movie) return;
  const uint32_t bink = Rd32(base + movie + 12), sets = Rd32(base + movie + 32);
  if (!bink || !sets) return;
  std::lock_guard lock(g_open_mutex);
  const auto it = g_open.find(bink);
  if (it == g_open.end()) return;
  const uint32_t frame = Rd32(base + bink + 12), i = Rd32(base + sets + 148);
  if (!frame || i > 1) return;
  const uint32_t set = sets + 152 + i * 48;
  const uint32_t y = Rd32(base + set + 4), yp = Rd32(base + set + 8);
  const uint32_t cr = Rd32(base + set + 16), cp = Rd32(base + set + 20), cb = Rd32(base + set + 28);
  if (!y || !cr || !cb || !yp || !cp) return;
  static int told = 0;
  if (LogOn() && told < 3) {
    ++told;
    REXLOG_INFO("[svr2011] hd movies: frame {} set {} Y {:08X}/{} cR {:08X} cB {:08X}/{} planes {}x{} chroma {}x{}", frame, i, y, yp, cr,
                cb, cp, Rd32(base + sets + 132), Rd32(base + sets + 136), Rd32(base + sets + 140), Rd32(base + sets + 144));
  }
  it->second->Write(double(frame - 1) / kFps, base + y, yp, base + cr, base + cb, cp);
}
