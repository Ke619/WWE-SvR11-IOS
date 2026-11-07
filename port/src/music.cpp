// WWE SmackDown vs. Raw 2011 - user music for entrances (see music.h).

#include "music.h"
#include "media_mods.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <xaudio2.h>
#elif defined(__ANDROID__)
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <fmt/format.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "jukebox.h"

namespace svr2011 {

namespace {

// Songs' gains (SetSongGain): by lower-case file name, as factors. A mod's theme copy is in here even at 0 dB.
std::mutex g_gain_mutex;
std::map<std::u16string, float> g_gains;
// The player's own songs' levels: <Music>/.levels.txt, "<file name>\t<dB>" lines the launcher writes (its Music
// tab's Level volume: the Mod Maker's -24 LUFS). Read again when the file changes.
std::filesystem::path g_levels_path;
std::filesystem::file_time_type g_levels_time;
std::map<std::u16string, float> g_levels;

std::u16string GainKey(const std::filesystem::path& file) {
  std::u16string k = file.filename().u16string();
  for (char16_t& c : k)
    if (c >= u'A' && c <= u'Z') c = char16_t(c - u'A' + u'a');
  return k;
}

void ReadLevels() {  // (under g_gain_mutex)
  std::error_code ec;
  if (g_levels_path.empty()) return;
  const auto t = std::filesystem::last_write_time(g_levels_path, ec);
  if (ec) {
    g_levels.clear();
    return;
  }
  if (t == g_levels_time && !g_levels.empty()) return;
  g_levels_time = t;
  g_levels.clear();
  std::ifstream in(g_levels_path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || !tab) continue;
    const std::string name = line.substr(0, tab);
    const float db = std::clamp(float(std::atof(line.c_str() + tab + 1)), -20.0f, 6.0f);
    g_levels[GainKey(std::filesystem::path(std::u8string(name.begin(), name.end())))] = std::pow(10.0f, db / 20.0f);
  }
}

float SongGain(const std::filesystem::path& file) {
  std::lock_guard lock(g_gain_mutex);
  const auto key = GainKey(file);
  if (const auto g = g_gains.find(key); g != g_gains.end()) return g->second;
  ReadLevels();
  const auto l = g_levels.find(key);
  return l == g_levels.end() ? 1.0f : l->second;
}

using rex::kernel::xam::apps::XmpApp;

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
constexpr const wchar_t* kExtensions[] = {L".mp3", L".wma", L".m4a", L".aac", L".wav", L".flac"};
constexpr uint32_t kSlots = 6;   // buffers owned by the player
constexpr uint32_t kQueued = 4;  // at most this many queued on the voice
constexpr double kBufferSeconds = 0.1;
#else
// (Android's decoders: no WMA)
constexpr const wchar_t* kExtensions[] = {L".mp3", L".m4a", L".aac", L".wav", L".flac", L".ogg"};
#endif

std::filesystem::path g_folder;

bool IsSong(const std::filesystem::path& path) {
  std::wstring ext = path.extension().wstring();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
  return std::find(std::begin(kExtensions), std::end(kExtensions), ext) != std::end(kExtensions);
}

// The playlists: each subfolder (its first song, by name) and each song
// directly in the folder, sorted by name.
std::vector<std::pair<std::u16string, std::filesystem::path>> Playlists() {
  std::vector<std::pair<std::u16string, std::filesystem::path>> lists;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(g_folder, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const std::filesystem::path& path = it->path();
    if (it->is_directory(ec)) {
      std::vector<std::filesystem::path> songs;
      std::error_code ec2;
      for (auto s = std::filesystem::recursive_directory_iterator(
               path, std::filesystem::directory_options::skip_permission_denied, ec2);
           !ec2 && s != std::filesystem::recursive_directory_iterator(); s.increment(ec2)) {
        if (s->is_regular_file(ec2) && IsSong(s->path())) songs.push_back(s->path());
      }
      if (!songs.empty()) {
        std::sort(songs.begin(), songs.end());
        lists.emplace_back(path.filename().u16string(), songs.front());
      }
    } else if (it->is_regular_file(ec) && IsSong(path)) {
      lists.emplace_back(path.stem().u16string(), path);
    }
  }
  std::sort(lists.begin(), lists.end());
  return lists;
}

#if defined(_WIN32)
// Plays one song on a loop on its own XAudio2 voice, on a thread of its own.
class Player {
 public:
  explicit Player(bool once = false) : once_(once) {}
  void LogVolume() { log_volume_ = true; }
  void Play(std::filesystem::path path, bool once) {
    once_ = once;
    Play(std::move(path));
  }
  // A song is playing or paused (not yet at its end, for a clip).
  bool Active() const { return active_; }
  // As Play, at `gain` (a factor) for this file only - a clip's own level, whatever its name (every mod's
  // announcer call is voice.mp3).
  void PlayAt(std::filesystem::path path, float gain, bool once) {  // (gain 0: the file's own, SetSongGain)
    once_ = once;
    PlayAt(std::move(path), gain);
  }
  void PlayAt(std::filesystem::path path, float gain) {
    {
      std::lock_guard lock(mutex_);
      pending_gain_ = gain;
    }
    Play(std::move(path));
  }
  void Play(std::filesystem::path path) {
    active_ = !path.empty();
    std::lock_guard lock(mutex_);
    request_ = std::move(path);
    has_request_ = true;
    paused_ = false;
    wake_.notify_one();
  }
  void Stop() {
    active_ = false;
    std::lock_guard lock(mutex_);
    request_.clear();
    has_request_ = true;
    wake_.notify_one();
  }
  void Pause(bool paused) {
    std::lock_guard lock(mutex_);
    paused_ = paused;
    wake_.notify_one();
  }
  void SetVolume(float volume) {
    std::lock_guard lock(mutex_);
    game_volume_ = std::clamp(volume, 0.0f, 1.0f);
  }

  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // (mfplat / mfreadwrite are delay-loaded: Windows "N" without the Media Feature Pack has neither)
    if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
        !LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
        FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      REXLOG_WARN("user music: Media Foundation unavailable (Windows N: the Media Feature Pack)");
      return;
    }
    if (!Engine()) {
      REXLOG_WARN("user music: no audio output");
      return;
    }
    for (;;) {
      std::filesystem::path request;
      bool has_request, paused;
      {
        std::unique_lock lock(mutex_);
        // Idle: sleep until asked; playing: top the queue up every 20 ms.
        wake_.wait_for(lock, voice_ ? std::chrono::milliseconds(20) : std::chrono::hours(1),
                       [&] { return has_request_ || (voice_ && paused_ != voice_paused_); });
        has_request = std::exchange(has_request_, false);
        request = request_;
        clip_gain_ = std::exchange(pending_gain_, 0.0f);
        paused = paused_;
      }
      if (has_request) {
        StopVoice();
        if (engine_lost_) {  // (the audio device went away or changed: this engine plays nothing any more)
          REXLOG_WARN("user music: the audio output stopped (XAudio2 error {:08X}: the audio device changed or "
                      "was lost) - opened again", uint32_t(engine_error_.load()));
          if (master_) master_->DestroyVoice();
          master_ = nullptr;
          xaudio2_.Reset();
          if (!Engine()) REXLOG_WARN("user music: no audio output");
        }
        song_ = request;
        if (!song_.empty() && !Open(song_)) song_.clear();
        if (song_.empty()) active_ = false;
      }
      if (!voice_) continue;
      if (paused != voice_paused_) {
        paused ? voice_->Stop() : voice_->Start();
        voice_paused_ = paused;
      }
      UpdateVolume();
      if (voice_paused_) continue;
      Feed();
      CheckRendering();
      if (ended_) {
        XAUDIO2_VOICE_STATE state = {};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) {  // from the top again, until stopped (a clip: once)
          StopVoice();
          if (once_) {
            song_.clear();
            active_ = false;
          } else if (Open(song_)) {
            voice_->Start();
            voice_paused_ = false;
          } else {
            song_.clear();
          }
        }
      }
    }
  }

 private:
  // The XAudio2 engine and its output (again after the device went away: OnCriticalError).
  struct EngineEvents : IXAudio2EngineCallback {
    Player* player = nullptr;
    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override {
      player->engine_error_ = error;
      player->engine_lost_ = true;
    }
  };
  bool Engine() {
    engine_lost_ = false;
    if (FAILED(XAudio2Create(&xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !xaudio2_ ||
        FAILED(xaudio2_->CreateMasteringVoice(&master_)))
      return false;
    events_.player = this;
    xaudio2_->RegisterForCallbacks(&events_);
    static const bool test_lost = std::getenv("SVR2011_TEST_AUDIO_LOST") != nullptr;  // (test: the first song's engine "lost")
    if (test_lost && log_volume_ && !test_lost_done_) test_lost_done_ = true, engine_error_ = 0x88890004, engine_lost_ = true;
    return true;
  }
  // The user-music player: a song that rendered no sound in its first 2 s is noted (once a song).
  void CheckRendering() {
    if (!log_volume_ || checked_) return;
    if (std::chrono::steady_clock::now() - opened_at_ < std::chrono::seconds(2)) return;
    checked_ = true;
    XAUDIO2_VOICE_STATE state = {};
    voice_->GetState(&state);
    XAUDIO2_PERFORMANCE_DATA perf = {};
    xaudio2_->GetPerformanceData(&perf);
    if (state.SamplesPlayed == 0 || engine_lost_)
      REXLOG_WARN("user music: no sound after 2 s ({} samples played, {} buffers queued, volume {:.2f}, {} voice(s) "
                  "active{})", state.SamplesPlayed, state.BuffersQueued, volume_, perf.ActiveSourceVoiceCount,
                  engine_lost_ ? ", the audio device was lost" : "");
    else
      REXLOG_INFO("user music: playing ({} samples in 2 s, volume {:.2f})", state.SamplesPlayed, volume_);
  }

  bool Open(const std::filesystem::path& path) {
    song_gain_ = clip_gain_ > 0.0f ? clip_gain_ : SongGain(path);
    if (song_gain_ != 1.0f)
      REXLOG_INFO("user music: {} at {:.1f} dB (theme_gain)", path.filename().string(), 20.0f * std::log10(song_gain_));
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
      REXLOG_WARN("user music: cannot open {}", path.string());
      return false;
    }
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(stream, TRUE);
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    ComPtr<IMFMediaType> got;
    WAVEFORMATEX* format = nullptr;
    UINT32 format_size = 0;
    if (FAILED(reader->SetCurrentMediaType(stream, nullptr, want.Get())) ||
        FAILED(reader->GetCurrentMediaType(stream, &got)) ||
        FAILED(MFCreateWaveFormatExFromMFMediaType(got.Get(), &format, &format_size))) {
      REXLOG_WARN("user music: cannot decode {}", path.string());
      return false;
    }
    IXAudio2SourceVoice* voice = nullptr;
    const HRESULT hr = xaudio2_->CreateSourceVoice(&voice, format);
    block_align_ = std::max<uint32_t>(format->nBlockAlign, 1);
    bytes_per_buffer_ = uint32_t(format->nAvgBytesPerSec * kBufferSeconds);
    bytes_per_buffer_ -= bytes_per_buffer_ % block_align_;
    CoTaskMemFree(format);
    if (FAILED(hr)) {
      REXLOG_WARN("user music: no voice for {}", path.string());
      return false;
    }
    reader_ = reader;
    voice_ = voice;
    ended_ = false;
    voice_paused_ = true;  // started by the loop (unless paused)
    volume_ = -1.0f;
    opened_at_ = std::chrono::steady_clock::now();
    checked_ = false;
    return true;
  }

  void StopVoice() {
    if (voice_) {
      voice_->DestroyVoice();  // stops it and drops its queue
      voice_ = nullptr;
    }
    reader_.Reset();
  }

  void UpdateVolume() {
    float game;
    {
      std::lock_guard lock(mutex_);
      game = game_volume_;
    }
    float v = game * song_gain_;  // (the song's own: a mod's theme_gain=)
    const bool muted = rex::cvar::GetFlagByName("audio_mute") == "true";
    if (muted) v = 0.0f;
    if (v != volume_) {
      if (log_volume_)  // (as a song starts and when it changes: a silent song shows here)
        REXLOG_INFO("user music: volume {:.2f} (the game's {:.2f}, theme_gain {:.2f}{})", v, game, song_gain_,
                    muted ? ", audio_mute" : "");
      volume_ = v;
      voice_->SetVolume(v);
    }
  }

  // Tops the voice's queue up with decoded audio.
  void Feed() {
    XAUDIO2_VOICE_STATE state = {};
    voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    uint32_t queued = state.BuffersQueued;
    while (!ended_ && queued < kQueued) {
      std::vector<uint8_t>& slot = slots_[next_slot_];
      slot.clear();
      while (slot.size() < bytes_per_buffer_) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                                       &flags, nullptr, &sample)) ||
            (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
          ended_ = true;
          break;
        }
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
          slot.insert(slot.end(), data, data + length);
          buffer->Unlock();
        }
      }
      slot.resize(slot.size() - slot.size() % block_align_);
      if (slot.empty()) break;
      XAUDIO2_BUFFER xb = {};
      xb.AudioBytes = uint32_t(slot.size());
      xb.pAudioData = slot.data();
      if (ended_) xb.Flags = XAUDIO2_END_OF_STREAM;
      if (FAILED(voice_->SubmitSourceBuffer(&xb))) {
        ended_ = true;
        break;
      }
      // Never more than kQueued < kSlots queued, so the slot written next is
      // one XAudio2 has finished with.
      next_slot_ = (next_slot_ + 1) % kSlots;
      ++queued;
    }
  }

  std::atomic<bool> once_;
  std::atomic<bool> active_{false};
  // Requests (any thread).
  std::mutex mutex_;
  std::condition_variable wake_;
  std::filesystem::path request_;
  bool has_request_ = false;
  bool paused_ = false;
  float game_volume_ = 1.0f;
  float song_gain_ = 1.0f;
  float pending_gain_ = 0.0f;  // (PlayAt's, until the player thread takes the request)
  float clip_gain_ = 0.0f;     // (the player thread's: the request's PlayAt gain, 0 = SongGain)
  std::atomic<bool> log_volume_{false};  // (the user-music player: LogVolume)

  // The player thread's own.
  ComPtr<IXAudio2> xaudio2_;
  IXAudio2MasteringVoice* master_ = nullptr;
  EngineEvents events_;
  std::atomic<bool> engine_lost_{false};
  std::atomic<long> engine_error_{0};
  bool test_lost_done_ = false;
  std::chrono::steady_clock::time_point opened_at_{};
  bool checked_ = true;
  IXAudio2SourceVoice* voice_ = nullptr;
  ComPtr<IMFSourceReader> reader_;
  std::filesystem::path song_;
  bool ended_ = false;
  bool voice_paused_ = true;
  float volume_ = -1.0f;
  uint32_t block_align_ = 1;
  uint32_t bytes_per_buffer_ = 0;
  std::array<std::vector<uint8_t>, kSlots> slots_;
  uint32_t next_slot_ = 0;
};

#elif defined(__ANDROID__)

// Android: the phone's own decoders (NDK AMediaExtractor + AMediaCodec: mp3,
// m4a / aac, flac, wav...) into an SDL audio stream, on a thread of its own.
class Player {
 public:
  explicit Player(bool once = false) : once_(once) {}
  void LogVolume() { log_volume_ = true; }
  void Play(std::filesystem::path path, bool once) {
    once_ = once;
    Play(std::move(path));
  }
  // A song is playing or paused (not yet at its end, for a clip).
  bool Active() const { return active_; }
  // As Play, at `gain` (a factor) for this file only - a clip's own level, whatever its name (every mod's
  // announcer call is voice.mp3).
  void PlayAt(std::filesystem::path path, float gain, bool once) {  // (gain 0: the file's own, SetSongGain)
    once_ = once;
    PlayAt(std::move(path), gain);
  }
  void PlayAt(std::filesystem::path path, float gain) {
    {
      std::lock_guard lock(mutex_);
      pending_gain_ = gain;
    }
    Play(std::move(path));
  }
  void Play(std::filesystem::path path) {
    active_ = !path.empty();
    std::lock_guard lock(mutex_);
    request_ = std::move(path);
    has_request_ = true;
    paused_ = false;
    wake_.notify_one();
  }
  void Stop() {
    active_ = false;
    std::lock_guard lock(mutex_);
    request_.clear();
    has_request_ = true;
    wake_.notify_one();
  }
  void Pause(bool paused) {
    std::lock_guard lock(mutex_);
    paused_ = paused;
    wake_.notify_one();
  }
  void SetVolume(float volume) {
    std::lock_guard lock(mutex_);
    game_volume_ = std::clamp(volume, 0.0f, 1.0f);
  }

  void Run() {
    for (;;) {
      std::filesystem::path request;
      bool has_request, paused;
      {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, codec_ ? std::chrono::milliseconds(20) : std::chrono::hours(1),
                       [&] { return has_request_ || (codec_ && paused_ != stream_paused_); });
        has_request = std::exchange(has_request_, false);
        request = request_;
        clip_gain_ = std::exchange(pending_gain_, 0.0f);
        paused = paused_;
      }
      if (has_request) {
        Close();
        song_ = request;
        if (!song_.empty() && !Open(song_)) song_.clear();
        if (song_.empty()) active_ = false;
      }
      if (!codec_) continue;
      if (paused != stream_paused_) {
        paused ? SDL_PauseAudioStreamDevice(stream_) : SDL_ResumeAudioStreamDevice(stream_);
        stream_paused_ = paused;
        const auto now = std::chrono::steady_clock::now();
        if (paused) paused_at_ = now;
        else if (paused_at_ != std::chrono::steady_clock::time_point{}) started_ += now - paused_at_;  // (a pause isn't played time)
      }
      UpdateVolume();
      if (stream_paused_) continue;
      Feed();
      // (a sound played once is over at its length + 2 s whatever the decoder
      // and the stream say: on some phones neither reported the end)
      const auto played = std::chrono::steady_clock::now() - started_;
      const bool overdue = once_ && duration_us_ > 0 &&
                           played > std::chrono::microseconds(duration_us_) + std::chrono::seconds(2);
      if (overdue) REXLOG_INFO("user music: {} over by its length ({} s)", song_.filename().string(), duration_us_ / 1000000);
      // (decoded to the end: over once the stream has at most ~20 ms left or
      // stops draining - on some phones it never reached 0)
      bool drained = false;
      if (ended_) {
        const int queued = SDL_GetAudioStreamQueued(stream_);
        const auto now = std::chrono::steady_clock::now();
        if (queued != last_queued_) last_queued_ = queued, queued_since_ = now;
        drained = queued <= bytes_per_second_ / 50 || now - queued_since_ > std::chrono::milliseconds(250);
      }
      if (drained || overdue) {  // from the top again, until stopped (a clip: once)
        Close();
        if (once_ || !Open(song_)) song_.clear(), active_ = false;
      }
    }
  }

 private:
  bool Open(const std::filesystem::path& path) {
    song_gain_ = clip_gain_ > 0.0f ? clip_gain_ : SongGain(path);
    if (song_gain_ != 1.0f)
      REXLOG_INFO("user music: {} at {:.1f} dB (theme_gain)", path.filename().string(), 20.0f * std::log10(song_gain_));
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      REXLOG_WARN("user music: cannot open {}", path.string());
      return false;
    }
    const off64_t size = lseek64(fd_, 0, SEEK_END);
    extractor_ = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, size) != AMEDIA_OK) {
      REXLOG_WARN("user music: cannot read {}", path.string());
      Close();
      return false;
    }
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(extractor_) && !codec_; ++i) {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, i);
      const char* mime = nullptr;
      if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime &&
          std::strncmp(mime, "audio/", 6) == 0) {
        AMediaExtractor_selectTrack(extractor_, i);
        codec_ = AMediaCodec_createDecoderByType(mime);
        if (codec_ && (AMediaCodec_configure(codec_, format, nullptr, nullptr, 0) != AMEDIA_OK ||
                       AMediaCodec_start(codec_) != AMEDIA_OK)) {
          AMediaCodec_delete(codec_);
          codec_ = nullptr;
        }
        int32_t rate = 0, channels = 0;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
        SetFormat(rate, channels);
        int64_t duration = 0;
        duration_us_ = AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration) ? duration : 0;
      }
      AMediaFormat_delete(format);
    }
    if (!codec_ || !stream_) {
      REXLOG_WARN("user music: cannot decode {}", path.string());
      Close();
      return false;
    }
    ended_ = input_done_ = false;
    drain_rounds_ = 0;
    last_queued_ = -1;
    started_ = std::chrono::steady_clock::now();
    paused_at_ = {};
    stream_paused_ = true;  // started by the loop (unless paused)
    volume_ = -1.0f;
    return true;
  }

  // The decoder's output (16-bit PCM): the stream's input format.
  void SetFormat(int32_t rate, int32_t channels) {
    if (rate <= 0 || channels <= 0) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, rate};
    if (!stream_) {
      stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      if (!stream_) REXLOG_WARN("user music: no audio output ({})", SDL_GetError());
    } else {
      SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    }
    bytes_per_second_ = rate * channels * 2;
  }

  void Close() {
    if (codec_) {
      AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (extractor_) {
      AMediaExtractor_delete(extractor_);
      extractor_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (stream_) SDL_ClearAudioStream(stream_);
  }

  void UpdateVolume() {
    float game;
    {
      std::lock_guard lock(mutex_);
      game = game_volume_;
    }
    float v = game * song_gain_;  // (the song's own: a mod's theme_gain=)
    const bool muted = rex::cvar::GetFlagByName("audio_mute") == "true";
    if (muted) v = 0.0f;
    if (v != volume_) {
      if (log_volume_)  // (as a song starts and when it changes: a silent song shows here)
        REXLOG_INFO("user music: volume {:.2f} (the game's {:.2f}, theme_gain {:.2f}{})", v, game, song_gain_,
                    muted ? ", audio_mute" : "");
      volume_ = v;
      SDL_SetAudioStreamGain(stream_, v);
    }
  }

  // Keeps ~0.3 s decoded audio queued on the stream.
  void Feed() {
    while (!ended_ && SDL_GetAudioStreamQueued(stream_) < int(bytes_per_second_ * 0.3)) {
      if (!input_done_) {
        const ssize_t in = AMediaCodec_dequeueInputBuffer(codec_, 2000);
        if (in >= 0) {
          size_t capacity = 0;
          uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, size_t(in), &capacity);
          const ssize_t n = AMediaExtractor_readSampleData(extractor_, buffer, capacity);
          if (n < 0) {
            AMediaCodec_queueInputBuffer(codec_, size_t(in), 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            input_done_ = true;
            REXLOG_INFO("user music: {}: all read", song_.filename().string());
          } else {
            AMediaCodec_queueInputBuffer(codec_, size_t(in), 0, size_t(n),
                                         uint64_t(AMediaExtractor_getSampleTime(extractor_)), 0);
            AMediaExtractor_advance(extractor_);
          }
        }
      }
      AMediaCodecBufferInfo info = {};
      const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
      if (out >= 0) {
        drain_rounds_ = 0;
        size_t capacity = 0;
        const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, size_t(out), &capacity);
        if (data && info.size > 0) SDL_PutAudioStreamData(stream_, data + info.offset, info.size);
        AMediaCodec_releaseOutputBuffer(codec_, size_t(out), false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
          ended_ = true;
          REXLOG_INFO("user music: {}: decoded to the end", song_.filename().string());
        }
      } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
        int32_t rate = 0, channels = 0;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
        AMediaFormat_delete(format);
        SetFormat(rate, channels);
      } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER && input_done_) {
        // (draining: wait for the next round. Some decoders never hand back
        // a buffer flagged END_OF_STREAM: nothing more for ~0.5 s after the
        // last input is the end too - else a song played once never ends)
        if (++drain_rounds_ > 25 && !ended_) {
          ended_ = true;
          REXLOG_INFO("user music: {}: decoder done (no end flag)", song_.filename().string());
        }
        break;
      }
    }
  }

  // Requests (any thread).
  std::atomic<bool> once_;
  std::atomic<bool> active_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::filesystem::path request_;
  bool has_request_ = false;
  bool paused_ = false;
  float game_volume_ = 1.0f;
  float song_gain_ = 1.0f;
  float pending_gain_ = 0.0f;  // (PlayAt's, until the player thread takes the request)
  float clip_gain_ = 0.0f;     // (the player thread's: the request's PlayAt gain, 0 = SongGain)
  std::atomic<bool> log_volume_{false};  // (the user-music player: LogVolume)

  // The player thread's own.
  std::filesystem::path song_;
  int fd_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* codec_ = nullptr;
  SDL_AudioStream* stream_ = nullptr;
  int bytes_per_second_ = 1;
  bool ended_ = false, input_done_ = false;
  int drain_rounds_ = 0;  // rounds without output since the last input
  int64_t duration_us_ = 0;  // the song's length (0: unknown)
  int last_queued_ = -1;     // the stream's queued bytes, and since when
  std::chrono::steady_clock::time_point queued_since_{};
  std::chrono::steady_clock::time_point started_{}, paused_at_{};
  bool stream_paused_ = true;
  float volume_ = -1.0f;
};

#endif

Player* g_player = nullptr;  // lives for the whole run
Player* g_clips = nullptr;   // short clips, played once (PlayClip)

}  // namespace

std::filesystem::path UserMusicFolder() { return g_folder; }

void PlayClip(const std::filesystem::path& file, float db) {
  if (g_clips) g_clips->PlayAt(file, std::pow(10.0f, std::clamp(db, -40.0f, 12.0f) / 20.0f));
}

// Host sounds (media mods): a pool of players, each one sound at a time.
namespace {
std::mutex g_pool_mutex;
std::vector<Player*> g_pool;
}  // namespace

int HostSoundStart(const std::filesystem::path& file, bool loop, float volume, float gain_db) {
  std::lock_guard lock(g_pool_mutex);
  size_t i = 0;
  while (i < g_pool.size() && g_pool[i]->Active()) ++i;
  if (i == g_pool.size()) {
    if (g_pool.size() >= 16) return -1;  // (enough at once)
    Player* p = new Player(true);
    g_pool.push_back(p);
    std::thread([p] {
#if defined(_WIN32)
      rex::thread::set_current_thread_name("Host sound");
#endif
      p->Run();
    }).detach();
  }
  g_pool[i]->SetVolume(volume);
  g_pool[i]->PlayAt(file, gain_db ? std::pow(10.0f, gain_db / 20.0f) : 0.0f, !loop);
  return int(i);
}
void HostSoundStop(int h) {
  std::lock_guard lock(g_pool_mutex);
  if (h >= 0 && size_t(h) < g_pool.size()) g_pool[h]->Stop();
}
void HostSoundPause(int h, bool paused) {
  std::lock_guard lock(g_pool_mutex);
  if (h >= 0 && size_t(h) < g_pool.size()) g_pool[h]->Pause(paused);
}
void HostSoundVolume(int h, float volume) {
  std::lock_guard lock(g_pool_mutex);
  if (h >= 0 && size_t(h) < g_pool.size()) g_pool[h]->SetVolume(volume);
}
bool HostSoundActive(int h) {
  std::lock_guard lock(g_pool_mutex);
  return h >= 0 && size_t(h) < g_pool.size() && g_pool[h]->Active();
}

std::vector<std::filesystem::path> UserMusicSongs() {
  std::vector<std::filesystem::path> songs;
  std::error_code ec;
  for (auto s = std::filesystem::recursive_directory_iterator(
           g_folder, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && s != std::filesystem::recursive_directory_iterator(); s.increment(ec)) {
    std::error_code ec2;
    if (s->is_regular_file(ec2) && IsSong(s->path())) songs.push_back(s->path().lexically_relative(g_folder));
  }
  std::sort(songs.begin(), songs.end());
  return songs;
}

void SetSongGain(const std::filesystem::path& file, float db) {
  db = std::clamp(db, -20.0f, 6.0f);
  std::lock_guard lock(g_gain_mutex);
  g_gains[GainKey(file)] = std::pow(10.0f, db / 20.0f);   // (0 dB kept too: a mod's theme copy isn't levelled again)
}

std::filesystem::path UserMusicSong(const std::string& name) {
  const std::u16string wanted = std::filesystem::path(std::u8string(name.begin(), name.end())).u16string();
  for (auto& [list, song] : Playlists())
    if (list == wanted) return song;
  return {};
}

void InstallUserMusic(const std::filesystem::path& folder) {
  g_folder = folder;
  {
    std::lock_guard lock(g_gain_mutex);
    g_levels_path = folder / ".levels.txt";
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);  // so players can find it
  g_player = new Player();
  g_player->LogVolume();
  std::thread([] {
#if defined(_WIN32)
    rex::thread::set_current_thread_name("User music");
#endif
    g_player->Run();
  }).detach();
  g_clips = new Player(true);
  std::thread([] {
#if defined(_WIN32)
    rex::thread::set_current_thread_name("Clips");
#endif
    g_clips->Run();
  }).detach();

  XmpApp::HostMusic music;
  music.list_playlists = [] {
    std::vector<std::u16string> names;
    for (auto& [name, song] : Playlists()) names.push_back(name);
    REXLOG_INFO("user music: {} playlist(s) in {}", names.size(), g_folder.string());
    return names;
  };
  music.play = [](const std::u16string& name) {
    // The win (docs/VICTORY_SCENES.md 5): the game starts the song at the result, and the scene starts it again.
    // The song goes on through that; the owner's own track, if any, in its place.
    static std::u16string win_name;
    if (!WinMusicNow()) {
      win_name.clear();
    } else {
      if (win_name == name && g_player->Active()) {
        REXLOG_INFO("user music: the win's song started again: it goes on");
        return true;
      }
      win_name = name;
      std::filesystem::path track;
      float db = 0.0f;
      if (VictoryTrackOfPlaylist(name, &track, &db)) {
        REXLOG_INFO("user music: playing {} (the win's, {:+.1f} dB)", track.string(), db);
        JukeboxStopMyMusic();
        g_player->PlayAt(track, db ? std::pow(10.0f, db / 20.0f) : 1.0f);
        return true;
      }
    }
    for (auto& [list, song] : Playlists()) {
      if (list == name) {
        REXLOG_INFO("user music: playing {}", song.string());
        JukeboxStopMyMusic();  // (the jukebox's MY MUSIC in the menus: jukebox.h)
        g_player->Play(song);
        return true;
      }
    }
    return false;
  };
  music.stop = [] {
    REXLOG_INFO("user music: stop");
    g_player->Stop();
  };
  music.pause = [] {
    REXLOG_INFO("user music: pause");
    g_player->Pause(true);
  };
  music.resume = [] {
    REXLOG_INFO("user music: resume");
    g_player->Pause(false);
  };
  music.set_volume = [](float volume) { g_player->SetVolume(volume); };
  XmpApp::SetHostMusic(std::move(music));
}

}  // namespace svr2011

// Saving an entrance whose music is USER PLAYLIST (CREATE AN ENTRANCE ->
// FINALIZE -> SAVE -> YES) waits in sub_8287DDB0 until the music manager is
// done (sub_8286FE30): the user song stopped and each channel of the game's
// music engine stopped - sub_8286F928 accepts channel flags 0, 0x01 or 0x20.
// The entrance-music channel the game paused (0x08) for the user song while
// the saved entrance already had one is never resumed, so the save waited
// forever on a frozen frame. A channel that is only paused counts as stopped
// here (the manager has already asked the channels to stop).
namespace {
std::atomic<uint32_t> g_channel_flags[3] = {0, 0, 0};  // last read, per channel
}

// Channel status: sub_825F31E8(engine, channel, &flags).
REX_EXTERN(__imp__sub_825F31E8);
REX_HOOK_RAW(sub_825F31E8) {
  const uint32_t channel = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_825F31E8(ctx, base);
  if (channel < 3 && out) {
    g_channel_flags[channel] = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + out));
  }
}

// "Channels stopped?" of the music manager's player (sub_8286F928).
REX_EXTERN(__imp__sub_8286F928);
REX_HOOK_RAW(sub_8286F928) {
  for (auto& f : g_channel_flags) f = 0;
  __imp__sub_8286F928(ctx, base);
  if (ctx.r3.u32) return;
  bool paused = false;
  for (auto& f : g_channel_flags) {
    const uint32_t flags = f;
    if (flags == 0x08) {
      paused = true;
    } else if (flags && !(flags & 0x21)) {
      return;  // really still playing
    }
  }
  if (paused) ctx.r3.u64 = 1;
}
