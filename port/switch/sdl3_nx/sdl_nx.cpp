// SDL3 on Horizon: SDL has no Switch backend, and the SDK and the game use about
// 110 of its functions. This file implements those, against SDL's own headers,
// with libnx: one window (the NWindow), up to 8 pads, one audio stream (audout),
// timers and the event queue. Anything a console has no use for (mouse capture,
// clipboard, fullscreen modes) answers quietly.
#include <SDL3/SDL.h>

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ---- events ----
std::mutex g_event_lock;
std::condition_variable g_event_cv;
std::deque<SDL_Event> g_events;
struct Watch {
  SDL_EventFilter fn;
  void* data;
};
std::vector<Watch> g_watches;
std::atomic<Uint32> g_next_user{SDL_EVENT_USER};
std::thread::id g_main_thread;
SDL_InitFlags g_init = 0;

void Post(SDL_Event& e) {
  std::vector<Watch> watches;
  {
    std::lock_guard<std::mutex> lock(g_event_lock);
    watches = g_watches;
  }
  for (const Watch& w : watches) {
    w.fn(w.data, &e);
  }
  std::lock_guard<std::mutex> lock(g_event_lock);
  g_events.push_back(e);
  g_event_cv.notify_all();
}

// ---- window ----
struct Window {
  int unused;
} g_window;
constexpr SDL_WindowID kWindowId = 1;
constexpr SDL_PropertiesID kWindowProps = 1;
constexpr SDL_PropertiesID kGamepadProps = 2;
bool g_window_open = false;

void WindowEvent(Uint32 type) {
  SDL_Event e{};
  e.type = type;
  e.window.windowID = kWindowId;
  Post(e);
}

// ---- pads ----
constexpr int kPads = 8;
struct Pad {
  PadState state;
  HidNpadIdType id;
  bool connected = false;
  u64 buttons = 0;
  Sint16 axes[6] = {};
};
Pad g_pads[kPads];
bool g_pads_ready = false;

struct ButtonMap {
  u64 nx;
  SDL_GamepadButton sdl;
};
// By label: the button marked A is the game's A.
const ButtonMap kButtons[] = {
    {HidNpadButton_A, SDL_GAMEPAD_BUTTON_SOUTH},
    {HidNpadButton_B, SDL_GAMEPAD_BUTTON_EAST},
    {HidNpadButton_X, SDL_GAMEPAD_BUTTON_WEST},
    {HidNpadButton_Y, SDL_GAMEPAD_BUTTON_NORTH},
    {HidNpadButton_Minus, SDL_GAMEPAD_BUTTON_BACK},
    {HidNpadButton_Plus, SDL_GAMEPAD_BUTTON_START},
    {HidNpadButton_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {HidNpadButton_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {HidNpadButton_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {HidNpadButton_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {HidNpadButton_Up, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {HidNpadButton_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {HidNpadButton_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {HidNpadButton_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

void InitPads() {
  if (g_pads_ready) {
    return;
  }
  padConfigureInput(kPads, HidNpadStyleSet_NpadStandard);
  for (int i = 0; i < kPads; i++) {
    g_pads[i].id = static_cast<HidNpadIdType>(HidNpadIdType_No1 + i);
    if (i == 0) {
      // The first player is also the console itself in handheld mode.
      padInitializeWithMask(&g_pads[i].state, (1ULL << HidNpadIdType_No1) | (1ULL << HidNpadIdType_Handheld));
    } else {
      padInitializeWithMask(&g_pads[i].state, 1ULL << g_pads[i].id);
    }
  }
  g_pads_ready = true;
}

void PumpPads() {
  if (!g_pads_ready) {
    return;
  }
  for (int i = 0; i < kPads; i++) {
    Pad& p = g_pads[i];
    padUpdate(&p.state);
    bool connected = padIsConnected(&p.state);
    SDL_JoystickID which = i + 1;
    if (connected != p.connected) {
      p.connected = connected;
      SDL_Event e{};
      e.type = connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
      e.gdevice.which = which;
      Post(e);
      if (!connected) {
        p.buttons = 0;
        std::memset(p.axes, 0, sizeof(p.axes));
      }
    }
    if (!connected) {
      continue;
    }
    u64 now = padGetButtons(&p.state);
    u64 changed = now ^ p.buttons;
    if (changed) {
      for (const ButtonMap& b : kButtons) {
        if (changed & b.nx) {
          SDL_Event e{};
          bool down = (now & b.nx) != 0;
          e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
          e.gbutton.which = which;
          e.gbutton.button = static_cast<Uint8>(b.sdl);
          e.gbutton.down = down;
          Post(e);
        }
      }
      p.buttons = now;
    }
    HidAnalogStickState l = padGetStickPos(&p.state, 0), r = padGetStickPos(&p.state, 1);
    auto y = [](s32 v) { return static_cast<Sint16>(std::clamp<int>(-v, -32768, 32767)); };
    auto x = [](s32 v) { return static_cast<Sint16>(std::clamp<int>(v, -32768, 32767)); };
    const Sint16 axes[6] = {x(l.x), y(l.y), x(r.x), y(r.y),
                            static_cast<Sint16>((now & HidNpadButton_ZL) ? 32767 : 0),
                            static_cast<Sint16>((now & HidNpadButton_ZR) ? 32767 : 0)};
    static const SDL_GamepadAxis kAxis[6] = {SDL_GAMEPAD_AXIS_LEFTX,  SDL_GAMEPAD_AXIS_LEFTY,
                                             SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                             SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};
    for (int a = 0; a < 6; a++) {
      if (axes[a] != p.axes[a]) {
        p.axes[a] = axes[a];
        SDL_Event e{};
        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.which = which;
        e.gaxis.axis = static_cast<Uint8>(kAxis[a]);
        e.gaxis.value = axes[a];
        Post(e);
      }
    }
  }
}

void PumpApplet() {
  static bool s_quit = false;
  if (!s_quit && !appletMainLoop()) {
    s_quit = true;
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    Post(e);
  }
}

// ---- timers ----
struct Timer {
  SDL_TimerID id;
  std::atomic<bool> stop{false};
};
std::mutex g_timer_lock;
std::vector<Timer*> g_timers;
std::atomic<SDL_TimerID> g_next_timer{1};

// ---- audio ----
constexpr int kAudioRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioFrames = 1024;
constexpr int kAudioBuffers = 4;
}  // namespace

struct SDL_AudioStream {
  SDL_AudioStreamCallback callback = nullptr;
  void* userdata = nullptr;
  std::mutex lock;
  std::deque<float> samples;
  float gain = 1.0f;
  bool paused = true;
};

namespace {
SDL_AudioStream* g_stream = nullptr;
std::thread g_audio_thread;
std::atomic<bool> g_audio_run{false};
AudioOutBuffer g_audio_buffers[kAudioBuffers];

void AudioThread() {
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
  const size_t bytes = (kAudioFrames * kAudioChannels * sizeof(s16) + 0xFFF) & ~size_t(0xFFF);
  for (auto& b : g_audio_buffers) {
    b.next = nullptr;
    b.buffer = memalign(0x1000, bytes);
    b.buffer_size = bytes;
    b.data_size = kAudioFrames * kAudioChannels * sizeof(s16);
    b.data_offset = 0;
    std::memset(b.buffer, 0, bytes);
    audoutAppendAudioOutBuffer(&b);
  }
  std::vector<float> floats(kAudioFrames * kAudioChannels);
  while (g_audio_run.load()) {
    AudioOutBuffer* done = nullptr;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&done, &count, 100000000ull)) || !done) {
      continue;
    }
    SDL_AudioStream* s = g_stream;
    size_t need = floats.size();
    std::fill(floats.begin(), floats.end(), 0.0f);
    if (s && !s->paused) {
      size_t have;
      {
        std::lock_guard<std::mutex> lock(s->lock);
        have = s->samples.size();
      }
      if (have < need && s->callback) {
        int want = static_cast<int>((need - have) * sizeof(float));
        s->callback(s->userdata, s, want, want);
      }
      std::lock_guard<std::mutex> lock(s->lock);
      size_t n = std::min(need, s->samples.size());
      for (size_t i = 0; i < n; i++) {
        floats[i] = s->samples.front() * s->gain;
        s->samples.pop_front();
      }
    }
    s16* out = static_cast<s16*>(done->buffer);
    for (size_t i = 0; i < need; i++) {
      out[i] = static_cast<s16>(std::clamp(floats[i], -1.0f, 1.0f) * 32767.0f);
    }
    armDCacheFlush(done->buffer, done->data_size);
    audoutAppendAudioOutBuffer(done);
  }
}

}  // namespace

extern "C" {

const char* SDL_GetError(void) { return "SDL on Switch: not available"; }
bool SDL_SetHint(const char*, const char*) { return true; }
bool SDL_SetAppMetadataProperty(const char*, const char*) { return true; }
bool SDL_IsMainThread(void) { return std::this_thread::get_id() == g_main_thread; }

bool SDL_InitSubSystem(SDL_InitFlags flags) {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_main_thread == std::thread::id()) {
      g_main_thread = std::this_thread::get_id();
    }
  });
  if (flags & (SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
    InitPads();
  }
  if ((flags & SDL_INIT_AUDIO) && !(g_init & SDL_INIT_AUDIO)) {
    if (R_FAILED(audoutInitialize()) || R_FAILED(audoutStartAudioOut())) {
      return false;
    }
  }
  g_init |= flags;
  return true;
}
void SDL_QuitSubSystem(SDL_InitFlags flags) {
  if ((flags & SDL_INIT_AUDIO) && (g_init & SDL_INIT_AUDIO)) {
    audoutStopAudioOut();
    audoutExit();
  }
  g_init &= ~flags;
}
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return flags ? (g_init & flags) : g_init; }

// ---- events ----
void SDL_PumpEvents(void) {
  PumpApplet();
  PumpPads();
}
bool SDL_PushEvent(SDL_Event* event) {
  Post(*event);
  return true;
}
bool SDL_WaitEvent(SDL_Event* event) {
  for (;;) {
    SDL_PumpEvents();
    std::unique_lock<std::mutex> lock(g_event_lock);
    if (!g_events.empty()) {
      if (event) {
        *event = g_events.front();
        g_events.pop_front();
      }
      return true;
    }
    // Pads are read at about 250 Hz while the queue is empty.
    g_event_cv.wait_for(lock, std::chrono::milliseconds(4));
    if (!g_events.empty()) {
      if (event) {
        *event = g_events.front();
        g_events.pop_front();
      }
      return true;
    }
  }
}
int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_EventAction action, Uint32 minType, Uint32 maxType) {
  std::lock_guard<std::mutex> lock(g_event_lock);
  int found = 0;
  if (action == SDL_ADDEVENT) {
    for (int i = 0; i < numevents; i++) {
      g_events.push_back(events[i]);
    }
    return numevents;
  }
  for (auto it = g_events.begin(); it != g_events.end();) {
    if (it->type >= minType && it->type <= maxType) {
      if (events && found < numevents) {
        events[found] = *it;
      }
      found++;
      if (action == SDL_GETEVENT) {
        it = g_events.erase(it);
        if (found >= numevents) {
          break;
        }
        continue;
      }
    }
    ++it;
  }
  return found;
}
void SDL_FlushEvents(Uint32 minType, Uint32 maxType) {
  std::lock_guard<std::mutex> lock(g_event_lock);
  g_events.erase(std::remove_if(g_events.begin(), g_events.end(),
                                [&](const SDL_Event& e) { return e.type >= minType && e.type <= maxType; }),
                 g_events.end());
}
Uint32 SDL_RegisterEvents(int numevents) { return g_next_user.fetch_add(numevents); }
bool SDL_AddEventWatch(SDL_EventFilter filter, void* userdata) {
  std::lock_guard<std::mutex> lock(g_event_lock);
  g_watches.push_back({filter, userdata});
  return true;
}
void SDL_RemoveEventWatch(SDL_EventFilter filter, void* userdata) {
  std::lock_guard<std::mutex> lock(g_event_lock);
  g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(),
                                 [&](const Watch& w) { return w.fn == filter && w.data == userdata; }),
                  g_watches.end());
}

// ---- timers ----
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void* userdata) {
  auto* t = new Timer();
  t->id = g_next_timer.fetch_add(1);
  SDL_TimerID id = t->id;
  {
    std::lock_guard<std::mutex> lock(g_timer_lock);
    g_timers.push_back(t);
  }
  std::thread([t, interval, callback, userdata]() mutable {
    Uint32 wait = interval;
    while (wait && !t->stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait));
      if (t->stop.load()) {
        break;
      }
      wait = callback(userdata, t->id, wait);
    }
    std::lock_guard<std::mutex> lock(g_timer_lock);
    g_timers.erase(std::remove(g_timers.begin(), g_timers.end(), t), g_timers.end());
    delete t;
  }).detach();
  return id;
}
bool SDL_RemoveTimer(SDL_TimerID id) {
  std::lock_guard<std::mutex> lock(g_timer_lock);
  for (Timer* t : g_timers) {
    if (t->id == id) {
      t->stop.store(true);
      return true;
    }
  }
  return false;
}

// ---- window ----
const char* SDL_GetCurrentVideoDriver(void) { return "switch"; }
SDL_Window* SDL_CreateWindow(const char*, int, int, SDL_WindowFlags) {
  g_window_open = true;
  return reinterpret_cast<SDL_Window*>(&g_window);
}
void SDL_DestroyWindow(SDL_Window*) { g_window_open = false; }
SDL_WindowID SDL_GetWindowID(SDL_Window*) { return kWindowId; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window*) { return kWindowProps; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window*) { return SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS; }
bool SDL_GetWindowSize(SDL_Window*, int* w, int* h) {
  u32 ww = 1280, hh = 720;
  nwindowGetDimensions(nwindowGetDefault(), &ww, &hh);
  if (w) *w = ww;
  if (h) *h = hh;
  return true;
}
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) { return SDL_GetWindowSize(window, w, h); }
float SDL_GetWindowPixelDensity(SDL_Window*) { return 1.0f; }
float SDL_GetWindowDisplayScale(SDL_Window*) { return 1.0f; }
bool SDL_SetWindowTitle(SDL_Window*, const char*) { return true; }
bool SDL_SetWindowPosition(SDL_Window*, int, int) { return true; }
bool SDL_SetWindowFullscreen(SDL_Window*, bool) { return true; }
bool SDL_SetWindowFullscreenMode(SDL_Window*, const SDL_DisplayMode*) { return true; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window*, bool) { return true; }
bool SDL_RaiseWindow(SDL_Window*) { return true; }
bool SDL_ShowWindow(SDL_Window*) {
  WindowEvent(SDL_EVENT_WINDOW_SHOWN);
  WindowEvent(SDL_EVENT_WINDOW_EXPOSED);
  return true;
}
bool SDL_HideCursor(void) { return true; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_CaptureMouse(bool) { return true; }
void SDL_WarpMouseInWindow(SDL_Window*, float, float) {}
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
  if (x) *x = 0;
  if (y) *y = 0;
  return 0;
}
bool SDL_StartTextInput(SDL_Window*) { return true; }
bool SDL_StopTextInput(SDL_Window*) { return true; }
bool SDL_SetClipboardText(const char*) { return true; }
char* SDL_GetClipboardText(void) {
  char* s = static_cast<char*>(std::malloc(1));
  s[0] = 0;
  return s;
}

// ---- displays ----
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window*) { return 1; }
float SDL_GetDisplayContentScale(SDL_DisplayID) { return 1.0f; }
SDL_DisplayID* SDL_GetDisplays(int* count) {
  auto* d = static_cast<SDL_DisplayID*>(std::calloc(2, sizeof(SDL_DisplayID)));
  d[0] = 1;
  if (count) *count = 1;
  return d;
}
const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID) {
  static SDL_DisplayMode mode;
  u32 w = 1280, h = 720;
  nwindowGetDimensions(nwindowGetDefault(), &w, &h);
  mode.displayID = 1;
  mode.format = SDL_PIXELFORMAT_RGBA32;
  mode.w = w;
  mode.h = h;
  mode.pixel_density = 1.0f;
  mode.refresh_rate = 60.0f;
  return &mode;
}

// ---- properties ----
void* SDL_GetPointerProperty(SDL_PropertiesID props, const char* name, void* def) {
  if (props == kWindowProps && !std::strcmp(name, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER)) {
    return nwindowGetDefault();
  }
  return def;
}
Sint64 SDL_GetNumberProperty(SDL_PropertiesID, const char*, Sint64 def) { return def; }
bool SDL_GetBooleanProperty(SDL_PropertiesID, const char*, bool def) { return def; }

// ---- gamepads ----
bool SDL_HasGamepad(void) {
  for (const Pad& p : g_pads) {
    if (p.connected) return true;
  }
  return false;
}
SDL_JoystickID* SDL_GetGamepads(int* count) {
  auto* ids = static_cast<SDL_JoystickID*>(std::calloc(kPads + 1, sizeof(SDL_JoystickID)));
  int n = 0;
  for (int i = 0; i < kPads; i++) {
    if (g_pads[i].connected) ids[n++] = i + 1;
  }
  if (count) *count = n;
  return ids;
}
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id) {
  if (id < 1 || id > kPads) return nullptr;
  return reinterpret_cast<SDL_Gamepad*>(&g_pads[id - 1]);
}
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID id) { return SDL_OpenGamepad(id); }
void SDL_CloseGamepad(SDL_Gamepad*) {}
SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad* g) { return reinterpret_cast<SDL_Joystick*>(g); }
SDL_JoystickID SDL_GetJoystickID(SDL_Joystick* j) {
  return static_cast<SDL_JoystickID>(reinterpret_cast<Pad*>(j) - g_pads) + 1;
}
const char* SDL_GetGamepadName(SDL_Gamepad*) { return "Nintendo Switch controller"; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO; }
SDL_JoystickType SDL_GetJoystickType(SDL_Joystick*) { return SDL_JOYSTICK_TYPE_GAMEPAD; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad*) { return 0x057E; }
Uint16 SDL_GetGamepadProduct(SDL_Gamepad*) { return 0x2009; }
SDL_GUID SDL_GetJoystickGUID(SDL_Joystick*) {
  SDL_GUID g{};
  return g;
}
void SDL_GUIDToString(SDL_GUID, char* out, int size) {
  if (size > 0) out[0] = 0;
}
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad*) { return kGamepadProps; }
int SDL_GetGamepadPlayerIndex(SDL_Gamepad* g) { return static_cast<int>(reinterpret_cast<Pad*>(g) - g_pads); }
bool SDL_SetGamepadPlayerIndex(SDL_Gamepad*, int) { return true; }
bool SDL_GamepadHasButton(SDL_Gamepad*, SDL_GamepadButton) { return true; }
SDL_JoystickConnectionState SDL_GetJoystickConnectionState(SDL_Joystick*) { return SDL_JOYSTICK_CONNECTION_WIRED; }
bool SDL_RumbleGamepad(SDL_Gamepad*, Uint16, Uint16, Uint32) { return true; }  // (not yet)
void SDL_UpdateJoysticks(void) { PumpPads(); }
int SDL_AddGamepadMappingsFromFile(const char*) { return 0; }

// ---- audio ----
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec*, SDL_AudioStreamCallback callback,
                                           void* userdata) {
  auto* s = new SDL_AudioStream();
  s->callback = callback;
  s->userdata = userdata;
  g_stream = s;
  if (!g_audio_run.exchange(true)) {
    g_audio_thread = std::thread(AudioThread);
  }
  return s;
}
void SDL_DestroyAudioStream(SDL_AudioStream* s) {
  if (g_stream == s) {
    g_stream = nullptr;
  }
  if (g_audio_run.exchange(false) && g_audio_thread.joinable()) {
    g_audio_thread.join();
  }
  delete s;
}
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream*) { return 1; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID, SDL_AudioSpec* spec, int* frames) {
  if (spec) {
    spec->format = SDL_AUDIO_F32LE;
    spec->channels = kAudioChannels;
    spec->freq = kAudioRate;
  }
  if (frames) *frames = kAudioFrames;
  return true;
}
const char* SDL_GetAudioDeviceName(SDL_AudioDeviceID) { return "audout"; }
bool SDL_ResumeAudioDevice(SDL_AudioDeviceID) {
  if (g_stream) g_stream->paused = false;
  return true;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* s) {
  s->paused = false;
  return true;
}
bool SDL_PauseAudioStreamDevice(SDL_AudioStream* s) {
  s->paused = true;
  return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* data, int len) {
  auto* f = static_cast<const float*>(data);
  std::lock_guard<std::mutex> lock(s->lock);
  s->samples.insert(s->samples.end(), f, f + len / sizeof(float));
  return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* s) {
  std::lock_guard<std::mutex> lock(s->lock);
  return static_cast<int>(s->samples.size() * sizeof(float));
}
bool SDL_ClearAudioStream(SDL_AudioStream* s) {
  std::lock_guard<std::mutex> lock(s->lock);
  s->samples.clear();
  return true;
}
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain) {
  s->gain = gain;
  return true;
}
bool SDL_SetAudioStreamFormat(SDL_AudioStream*, const SDL_AudioSpec*, const SDL_AudioSpec*) { return true; }

void SDL_free(void* p) { std::free(p); }
void* SDL_malloc(size_t n) { return std::malloc(n); }

}  // extern "C"
