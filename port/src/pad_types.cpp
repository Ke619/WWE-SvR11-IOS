// WWE SmackDown vs. Raw 2011 - which controller each player uses (see
// pad_types.h).

#include "pad_types.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <SDL3/SDL_hints.h>
#if defined(__ANDROID__)
#include <set>

#include <jni.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_system.h>
#endif

#include "generated/default/svr2011_init.h"
#include "fast_loads.h"
#include "fast_start.h"

#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(vibration, true, "Input", "Controller vibration (rumble); OPTIONS -> CONTROLS -> VIBRATION");
REXCVAR_DEFINE_BOOL(device_vibration, true, "Input",
                    "Android: when player 1's controller can't rumble (a handheld's built-in pad), the phone / "
                    "handheld vibrates instead");

#if defined(__ANDROID__)
// The device's own vibrator (GameActivity.deviceRumble(low, high), 0-65535 each: VibratorManager's default
// vibrator). Handhelds' built-in pads (AYN Thor / Odin, Retroid ...) have no vibrator of their own on their
// InputDevice - SDL can't rumble them - their motor is the device's.
namespace {
void DeviceRumble(uint32_t low, uint32_t high) {
  static uint32_t last = 0;  // (low << 16 | high: only changes go to Java)
  const uint32_t now = low << 16 | high;
  if (now == last) return;
  last = now;
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return;
  jclass cls = env->GetObjectClass(activity);
  static jmethodID method = env->GetMethodID(cls, "deviceRumble", "(II)V");
  if (method) env->CallVoidMethod(activity, method, jint(low), jint(high));
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}

// Once per controller: can SDL rumble it (its InputDevice has a vibrator)?
void LogRumbleSupport() {
  static std::set<SDL_JoystickID> told;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (!ids) return;
  for (int i = 0; i < count; ++i) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
    if (!pad || !told.insert(ids[i]).second) continue;
    const bool can = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    const char* name = SDL_GetGamepadName(pad);
    REXLOG_INFO("[svr2011] rumble: {} {}", name ? name : "controller",
                can ? "supported"
                    : REXCVAR_GET(device_vibration) ? "not supported -> device vibrator (when it plays player 1)"
                                                    : "not supported (device_vibration off)");
  }
  SDL_free(ids);
}
}  // namespace
#endif

// The game's XamInputSetState(user, vibration) wrapper: sub_82905068 (r4: the
// two motor speeds, u16 each). With vibration off the motors get 0 - so a
// rumble already running stops too.
REX_EXTERN(__imp__sub_82905068);
REX_HOOK_RAW(sub_82905068) {
  if (!REXCVAR_GET(vibration) && ctx.r4.u32) {
    static bool told = false;
    if (!told && (base[ctx.r4.u32] | base[ctx.r4.u32 + 1] | base[ctx.r4.u32 + 2] | base[ctx.r4.u32 + 3]))
    {
      told = true;
      REXLOG_INFO("[svr2011] controls: vibration off - the game's rumble not sent");
    }
    std::memset(base + ctx.r4.u32, 0, 4);
  }
#if defined(__ANDROID__)
  const uint32_t user = (ctx.r3.u32 & 0xFF) == 0xFF ? 0 : ctx.r3.u32;
  const uint32_t low = ctx.r4.u32 ? uint32_t(base[ctx.r4.u32] << 8 | base[ctx.r4.u32 + 1]) : 0;
  const uint32_t high = ctx.r4.u32 ? uint32_t(base[ctx.r4.u32 + 2] << 8 | base[ctx.r4.u32 + 3]) : 0;
#endif
  __imp__sub_82905068(ctx, base);
#if defined(__ANDROID__)
  // (the result: the pads' - SDK InputSystem::SetState - X_ERROR_SUCCESS when one rumbled, an error when
  // the player's pads can't; touch controls alone count as success)
  constexpr uint32_t kNotConnected = 0x48F;
  const uint32_t result = ctx.r3.u32;
  if (low | high) LogRumbleSupport();
  if (user == 0) {
    const bool fallback = REXCVAR_GET(device_vibration) && result != 0 && result != kNotConnected;
    DeviceRumble(fallback ? low : 0, fallback ? high : 0);
  }
#endif
}

namespace svr2011 {

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using namespace rex::input;  // (X_INPUT_* types)

std::array<std::atomic<PadType>, kMaxPlayers> g_types{};
std::atomic<uint32_t> g_version{0};

constexpr const char* kKeyboardName = "Keyboard and Mouse";  // (the SDK's MnK driver)

// The USB vendor in an SDL GUID (bytes 4-5, little-endian: hex chars 8-11).
uint32_t GuidVendor(const std::string& guid) {
  if (guid.size() < 12) return 0;
  auto hex = [&](size_t i) { return std::stoul(guid.substr(i, 2), nullptr, 16); };
  try {
    return uint32_t(hex(8) | hex(10) << 8);
  } catch (...) {
    return 0;
  }
}

PadType TypeOf(const DeviceInfo& d) {
  if (d.name == kKeyboardName) return PadType::kKeyboard;
  constexpr uint32_t kSony = 0x054C;
  if (GuidVendor(d.guid) == kSony) return PadType::kPlayStation;
  std::string name = d.name;
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  for (const char* ps : {"ps3", "ps4", "ps5", "dualshock", "dualsense", "playstation", "sony"})
    if (name.find(ps) != std::string::npos) return PadType::kPlayStation;
  return PadType::kXbox;  // (XInput pads, and others with an Xbox-style layout)
}

const char* TypeName(PadType t) {
  switch (t) {
    case PadType::kXbox: return "Xbox";
    case PadType::kPlayStation: return "PlayStation";
    case PadType::kKeyboard: return "keyboard";
    default: return "none";
  }
}

// The SDL driver for the PlayStation controllers next to XInput: SDL sees
// only HIDAPI devices other than Xbox ones (no XInput, raw input, DirectInput,
// WGI or GameInput), so no pad is seen twice.
class PlayStationDriver final : public rex::input::InputDriver {
 public:
  PlayStationDriver() : InputDriver(nullptr, 0), sdl_(nullptr, 0) {}

  X_STATUS Setup() override {
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");  // (a DualShock 3 with the DsHidMini driver)
    return sdl_.Setup();
  }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override { sdl_.EnumerateDevices(out); }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* s) override { return sdl_.GetDeviceState(id, s); }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* c) override {
    return sdl_.GetDeviceCapabilities(id, flags, c);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* v) override {
    return sdl_.SetDeviceVibration(id, v);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* k) override {
    return sdl_.GetDeviceKeystroke(id, flags, k);
  }

 private:
  rex::input::sdl::SDLInputDriver sdl_;
};

}  // namespace

// The players' order: player u gets the devices (and pad type) of slot
// g_order[u] - the identity, but for the player who pressed START at the
// press-START screen and player 1, swapped (MakeLeadPlayer).
static_assert(kMaxPlayers == 4);
static std::array<std::atomic<uint8_t>, kMaxPlayers> g_order = {0, 1, 2, 3};

static uint32_t Slot(uint32_t user_index) { return g_order[user_index].load(std::memory_order_relaxed); }

// The player who pressed START keeps player 1 (LockLeadPlayer): a controller that connects later - a pad
// switched on, Steam / DS4Windows' virtual pad - went before the keyboard and took player 1 from a keyboard
// player (report 2026-10-15: the keyboard player became player 2, an idle "XInput Controller" player 1).
// g_primary: each slot's own device (a controller or the keyboard; not the shared touch controls).
static std::mutex g_lead_mutex;
static rex::input::DeviceId g_lead = rex::input::DeviceId::kInvalid;
static std::string g_lead_name;
static std::array<rex::input::DeviceId, kMaxPlayers> g_primary{};
static std::array<std::string, kMaxPlayers> g_primary_name;
static std::vector<rex::input::DeviceId> g_known;  // (the devices seen at the last change)

static uint32_t PlayerOfSlot(uint32_t slot) {
  for (uint32_t p = 0; p < kMaxPlayers; ++p)
    if (Slot(p) == slot) return p;
  return slot;
}

PadType PlayerPadType(uint32_t user_index) {
  return user_index < kMaxPlayers ? g_types[Slot(user_index)].load(std::memory_order_relaxed) : PadType::kNone;
}

void SetTestPadType(uint32_t player, const std::string& type) {
  if (player >= kMaxPlayers) return;
  g_types[Slot(player)] = type == "ps" ? PadType::kPlayStation : type == "kb" ? PadType::kKeyboard
                        : type == "xbox" ? PadType::kXbox : PadType::kNone;
  ++g_version;
  REXLOG_INFO("[svr2011] players: test - player {} is now '{}'", player + 1, type);
}

void MakeLeadPlayer(uint32_t user_index) {
  if (user_index == 0 || user_index >= kMaxPlayers) return;
  const uint8_t lead = g_order[user_index].load(), first = g_order[0].load();
  g_order[0] = lead;
  g_order[user_index] = first;
  ++g_version;  // (the button pictures follow)
  REXLOG_INFO("[svr2011] players: player {} pressed START - now player 1 (the profile's), player 1 now player {}",
              user_index + 1, user_index + 1);
  std::lock_guard lock(g_lead_mutex);
  if (g_lead != rex::input::DeviceId::kInvalid) g_lead = g_primary[lead], g_lead_name = g_primary_name[lead];
}

void LockLeadPlayer() {
  std::lock_guard lock(g_lead_mutex);
  const uint32_t slot = Slot(0);
  if (g_primary[slot] == rex::input::DeviceId::kInvalid) return;  // (touch controls alone: nothing to keep)
  if (g_lead != g_primary[slot])
    REXLOG_INFO("[svr2011] players: START pressed - player 1 stays '{}' when controllers connect later",
                g_primary_name[slot]);
  g_lead = g_primary[slot], g_lead_name = g_primary_name[slot];
}

uint32_t PadTypesInUse() {
  uint32_t mask = 0;
  for (const auto& t : g_types) mask |= 1u << uint32_t(t.load(std::memory_order_relaxed));
  return mask & ~(1u << uint32_t(PadType::kNone));
}

uint32_t PadTypesVersion() { return g_version.load(std::memory_order_relaxed); }

// Android lists phone parts that report joystick / D-pad input as game
// controllers on some phones (fingerprint readers - uinput-fpc, goodix -
// power keys, touch panels): one of those as player 1's pad, merged
// with the touch controls, can hold a stick or a button and the game plays
// by itself (B18). Those are left out by name.
static bool NotAController(const std::string& name) {
#if defined(__ANDROID__)
  std::string n = name;
  for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  static const char* kParts[] = {"uinput", "fingerprint", "fpc", "goodix", "fortsense", "silead", "egistec",
                                 "chipone", "cdfinger", "gpio", "pwrkey", "qpnp", "sensor", "touch",
                                 "stylus", "e-pen"};
  for (const char* p : kParts)
    if (n.find(p) != std::string::npos) return true;
#else
  (void)name;
#endif
  return false;
}

void PlayerSlots::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& u : users_) u.clear();
  std::array<PadType, kMaxPlayers> types{};
  // Controllers by connection order (ordinal N -> player N, as SlotAssignment:
  // unplugging pad one doesn't make pad two player one).
  const DeviceInfo* keyboard = nullptr;
  std::vector<const DeviceInfo*> shared;  // (the touch controller: player 1's)
  std::array<const DeviceInfo*, kMaxPlayers> primary{};  // (each slot's own device)
  uint32_t next_free = 0;
  // Test aid: SVR2011_TEST_SCRIPT_PLAYER=<n> - the scripted controller as
  // player n's pad (a second pad, say, pressing START at the title; the
  // keyboard is then player 1, the other pad).
  static const uint32_t script_player = [] {
    const char* v = std::getenv("SVR2011_TEST_SCRIPT_PLAYER");
    return v ? uint32_t(std::atoi(v)) : 0u;
  }();
  // (a left-out phone part doesn't take a player's number: the controllers
  // after it move up)
  auto ordinal = [&devices](const DeviceInfo& d) {
    uint32_t skipped = 0;
    for (const DeviceInfo& o : devices)
      if (!o.synthetic && o.ordinal < d.ordinal && NotAController(o.name)) ++skipped;
    return d.ordinal - skipped;
  };
  for (const DeviceInfo& d : devices) {
    if (script_player >= 1 && script_player <= kMaxPlayers && d.name == "Script") {
      users_[script_player - 1].push_back(d.id);
      primary[script_player - 1] = &d;
      types[script_player - 1] = PadType::kXbox;
      next_free = std::max(next_free, script_player);
    } else if (d.name == kKeyboardName) {
      keyboard = &d;
    } else if (d.synthetic) {
      shared.push_back(&d);
    } else if (NotAController(d.name)) {
      static std::string told;  // (once per name)
      if (told.find("|" + d.name + "|") == std::string::npos) {
        told += "|" + d.name + "|";
        REXLOG_WARN("[svr2011] players: '{}' left out - a phone part, not a game controller", d.name);
      }
    } else if (const uint32_t o = ordinal(d); o < kMaxPlayers) {
      users_[o].push_back(d.id);
      if (!primary[o]) primary[o] = &d;
      if (types[o] == PadType::kNone) types[o] = TypeOf(d);
      next_free = std::max(next_free, o + 1);
    }
  }
  // The keyboard: the player after the controllers (player 1 with none; the
  // scripted player's test: player 1, as another pad already there).
  if (keyboard) {
    const uint32_t user = script_player >= 2 && users_[0].empty() ? 0 : std::min(next_free, kMaxPlayers - 1);
    users_[user].push_back(keyboard->id);
    if (types[user] == PadType::kNone) types[user] = PadType::kKeyboard;
    if (!primary[user]) primary[user] = keyboard;
  }
  {
    // The player who pressed START stays player 1: his device's slot back first in the order.
    std::lock_guard lock(g_lead_mutex);
    std::vector<rex::input::DeviceId> now;
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
      g_primary[i] = primary[i] ? primary[i]->id : rex::input::DeviceId::kInvalid;
      g_primary_name[i] = primary[i] ? primary[i]->name : std::string();
      if (primary[i]) now.push_back(primary[i]->id);
    }
    if (g_lead != rex::input::DeviceId::kInvalid) {
      int at = -1;
      for (uint32_t i = 0; i < kMaxPlayers && at < 0; ++i)
        if (g_primary[i] == g_lead) at = int(i);
      for (uint32_t i = 0; i < kMaxPlayers && at < 0; ++i)  // (reconnected: a new id, the same name)
        if (primary[i] && g_primary_name[i] == g_lead_name) at = int(i), g_lead = g_primary[i];
      if (at >= 0 && Slot(0) != uint32_t(at)) {
        const uint32_t p = PlayerOfSlot(uint32_t(at));
        const uint8_t first = g_order[0].load();
        g_order[0] = uint8_t(at);
        g_order[p] = first;
        ++g_version;
      }
    }
    for (uint32_t i = 0; i < kMaxPlayers; ++i) {
      if (!primary[i] || std::find(g_known.begin(), g_known.end(), primary[i]->id) != g_known.end()) continue;
      if (g_known.empty()) continue;  // (the start: the devices there from the beginning)
      if (g_lead != rex::input::DeviceId::kInvalid && primary[i]->id != g_lead)
        REXLOG_INFO("[svr2011] players: {} connected - player {} (player 1 stays {})", primary[i]->name,
                    PlayerOfSlot(i) + 1, g_lead_name);
      else
        REXLOG_INFO("[svr2011] players: {} connected - player {}", primary[i]->name, PlayerOfSlot(i) + 1);
    }
    g_known = now;
  }
  for (const DeviceInfo* d : shared) users_[Slot(0)].push_back(d->id);  // (player 1's, wherever he is)
  // Test aid: SVR2011_TEST_PAD_TYPES=xbox,ps,kb,none - the players' types as
  // if those controllers were connected (button pictures without the pads).
  if (const char* v = std::getenv("SVR2011_TEST_PAD_TYPES"); v && *v) {
    std::string list = v;
    for (uint32_t i = 0; i < kMaxPlayers && !list.empty(); ++i) {
      const std::string t = list.substr(0, list.find(','));
      list = list.find(',') == std::string::npos ? "" : list.substr(list.find(',') + 1);
      types[i] = t == "ps" ? PadType::kPlayStation : t == "kb" ? PadType::kKeyboard
               : t == "xbox" ? PadType::kXbox : PadType::kNone;
    }
  }

  std::string log;
  bool changed = false;
  static std::array<uint8_t, kMaxPlayers> logged_order{0, 1, 2, 3};
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    changed |= g_types[i].exchange(types[i]) != types[i];
    changed |= logged_order[i] != Slot(i);
    logged_order[i] = uint8_t(Slot(i));
    const PadType t = types[Slot(i)];  // (by player: player i plays slot Slot(i))
    if (t != PadType::kNone) log += fmt::format("{}P{} {}", log.empty() ? "" : ", ", i + 1, TypeName(t));
  }
  if (changed) {
    ++g_version;
    std::string names;
    for (const DeviceInfo& d : devices)
      if (!d.synthetic || d.name == kKeyboardName) names += (names.empty() ? "" : "; ") + d.name;
    REXLOG_INFO("[svr2011] players: {} ({})", log.empty() ? "none" : log, names);
  }
}

void PlayerSlots::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index < kMaxPlayers) out = users_[Slot(user_index)];
}

std::unique_ptr<rex::input::InputDriver> CreatePlayStationDriver() { return std::make_unique<PlayStationDriver>(); }

}  // namespace svr2011

// The press-START screen takes the pad that pressed START as the active user
// (sub_8258B5F8(screen): screen+252 -> the game data's +0, the user whose
// profile, saves and achievements are used). Only user 0 is signed in, so
// any other pad got "You are not currently signed in". That pad becomes
// player 1 instead (MakeLeadPlayer) and the game's choice user 0.
namespace {
constexpr uint32_t kGameData = 0x82E3DDFC;  // -> +0: the active user
constexpr uint32_t kScreenPad = 252;        // the press-START screen's pad
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void SetBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
}  // namespace

REX_EXTERN(__imp__sub_8258B5F8);
REX_HOOK_RAW(sub_8258B5F8) {
  static const bool off = std::getenv("SVR2011_TEST_NO_LEAD") != nullptr;  // (tests: the game's own choice)
  const uint32_t screen = ctx.r3.u32;
  const uint32_t pad = screen && !off ? Be32(base + screen + kScreenPad) : 0;
  const uint32_t state_before = screen ? Be32(base + screen + 2752) : 0;
  svr2011::FastStartScreenFrame(base, screen);
  __imp__sub_8258B5F8(ctx, base);
  const bool taken = screen && state_before != 3 && Be32(base + screen + 2752) == 3;
  if (screen) svr2011::FastLoadsStartScreen(taken);  // (fast_loads.h)
  if (taken)  // (START taken: fast_start.h)
    svr2011::FastStartPressStart(base, screen, Be32(base + screen + kScreenPad));
  const uint32_t data = Be32(base + kGameData);
  if (data && pad != 0 && pad < svr2011::kMaxPlayers && Be32(base + data) == pad) {
    svr2011::MakeLeadPlayer(pad);
    SetBe32(base + data, 0);
    SetBe32(base + screen + kScreenPad, 0);
  }
  if (taken) svr2011::LockLeadPlayer();  // (player 1 - the presser - stays player 1 when pads connect later)
}
