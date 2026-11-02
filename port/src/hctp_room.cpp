// HCTP backstage rooms (branch hctp-backstage): SmackDown! Here Comes the
// Pain's backstage areas played as backstage mods with their own row (see
// docs/BACKSTAGE_MODS.md). The room, its props, its climb spots (T21/T22 on
// 0xDF walls) and its converted HCTP motions are data (the mod's arena.pac
// and gimmick.pac); what 2011 has no data for is done here.
//
// Test aid (SVR2011_TEST_HCTP_LOG=1): the wall-attribute switch
// (sub_82306B10, fighter's touched attribute at +455), the walk-into-wall
// gimmick search (sub_824B6A70, T22) and the motion-key lookups it makes
// (sub_82394520(bank, key, x, y)) are logged.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

#include "arena_mods.h"
#include "match_hud.h"
#include "match_types.h"
#include "p2p.h"
#include "pad_types.h"

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/bytes.h"

namespace {

const bool g_log = std::getenv("SVR2011_TEST_HCTP_LOG") != nullptr;
// (tests: SVR2011_TEST_HCTP_ROOF=1 the roof breaks at the first knock-down,
// =now 5 seconds into the match)
const char* const g_roof_env = std::getenv("SVR2011_TEST_HCTP_ROOF");
const bool g_roof_once = g_roof_env != nullptr;
const bool g_roof_now = g_roof_env && std::strcmp(g_roof_env, "now") == 0;
// (=crate: fighter 1 is shut in the equipment crate 5 seconds in)
const bool g_crate_now = g_roof_env && std::strcmp(g_roof_env, "crate") == 0;
// (=steam: both valves opened 5 seconds in)
const bool g_steam_now = g_roof_env && std::strcmp(g_roof_env, "steam") == 0;
// (=puff: a steam puff shown over fighter 0's head, to see one)
const bool g_puff_now = g_roof_env && std::strcmp(g_roof_env, "puff") == 0;
// (=props: the railings and the lead pipes knocked over 5 seconds in)
const bool g_props_now = g_roof_env && std::strcmp(g_roof_env, "props") == 0;
// (=tour: the world mod's doors in turn, every 6 seconds from 6 seconds in - each room's first door,
// then its next on the next visit)
const bool g_tour_now = g_roof_env && std::strcmp(g_roof_env, "tour") == 0;
// (=fork: room 31, fighter 0 - the test's pad - gets on the forklift 5 seconds in; the script drives)
const bool g_fork_now = g_roof_env && std::strcmp(g_roof_env, "fork") == 0;
// (=walkin: wrestlers go through a door by walking into it, as the CPU does - not with Y)
const bool g_walk_in = g_roof_env && std::strcmp(g_roof_env, "walkin") == 0;
// (=door: the world mod's current room's first door - fighter 0 put on its trigger 5 seconds in, fighter 1
// 15 seconds in (a LAN pair: each side's player presses Y there))
const bool g_door_now = g_roof_env && std::strcmp(g_roof_env, "door") == 0;
// (=nograb: no cage-wall grabs or climbs, so strikes reach the fence)
const bool g_no_grab = g_roof_env && std::strcmp(g_roof_env, "nograb") == 0;
// (=cart: the button cycle starts 5 seconds in)
const bool g_cart_now = g_roof_env && std::strcmp(g_roof_env, "cart") == 0;

using svrfmt::Rd32, svrfmt::RdF, svrfmt::Wr32, svrfmt::WrF;
// (at most n lines a second per site)
bool Allow(int& budget, std::chrono::steady_clock::time_point& t0, int n = 4) {
  const auto now = std::chrono::steady_clock::now();
  if (now - t0 > std::chrono::seconds(1)) {
    t0 = now;
    budget = n;
  }
  return budget-- > 0;
}

}  // namespace

// -- Climbing ----------------------------------------------------------------
//
// In HCTP a wrestler who walks into a climbable wall (collision attribute
// 0xFB crates, 0xFD crate top, 0xF4 ladder, 0xDF cage) climbs it. The room's
// converter gives those walls 0xDF and puts a T22 walk-into-wall spot (the
// HCTP climb motion as its key) in front of each. 2011 reaches its T22
// search (sub_824B6A70) only from the ring-style walk handler; backstage
// fighters walk in motion 20500, which never gets there. So: after a
// fighter's collision (sub_821E5910), when he has pressed into a 0xDF wall
// for a few frames while walking, the T22 search is asked here (it checks
// the spot's range, snaps and turns him and plays the key).
namespace {

constexpr uint32_t kChars = 0x82E3CC50;
constexpr uint32_t kWalkBackstage = 20500;

struct Climber {
  uint32_t fighter = 0;
  int pressing = 0;    // frames in a row against a 0xDF wall
  int cooldown = 0;    // frames before another try
  uint32_t frame = 0;  // (the match frame last counted)
  uint32_t hit = 0;    // (the last match frame he walked into a climb wall, + 1)
  float x[8] = {}, z[8] = {};  // (where he was the last 8 counted frames: the way he walks in)
};
Climber g_climbers[8];
// Touched attributes that climb besides 0xDF: the standing cage panels (their
// collision carries the panel's own tag, 0xA0 + prop, in place of 0xDF).
bool g_climb_attr[256];

bool HctpRoomActive() {
  const int r = svr2011::g_active_row;
  return r >= 0 && size_t(r) < svr2011::BackstageRows().size() && svr2011::BackstageRows()[size_t(r)].hctp > 0;
}

// The HCTP room number (hctp=30 the Back Entrance) - its cage rules use room 30's places.
int HctpRoom();

void WrF(uint8_t* p, float f);

void Climb(PPCContext& ctx, uint8_t* base, uint32_t f) {
  int slot = -1;
  for (int i = 0; i < 8; ++i)
    if (Rd32(base + kChars + i * 4) == f) slot = i;
  if (slot < 0) return;
  Climber& c = g_climbers[slot];
  if (c.fighter != f) c = {f};
  // (walking, or running: +1286; the touched attribute is there every other frame while he pushes: the
  // collision pushes him out, the next frame he walks back in)
  const bool walking = Rd32(base + f + 212) == kWalkBackstage || base[f + 1286];
  const uint8_t touched = base[f + 455];
  const uint32_t frame = Rd32(base + 0x82E3CD0C);  // (the match frames: kMatchFrames, below)
  if (walking && (touched == 0xDF || g_climb_attr[touched])) c.hit = frame + 1;
  // (counted once a frame: this runs for each of his collision parts, and only one of them meets the wall)
  if (frame == c.frame) return;
  c.frame = frame;
  c.x[frame % 8] = RdF(base + f + 288), c.z[frame % 8] = RdF(base + f + 296);
  if (c.cooldown > 0) --c.cooldown;
  if (c.hit && c.hit + 1 >= frame)  // (the wall this frame or the one before)
    ++c.pressing;
  else if (c.pressing > 0)
    --c.pressing;
  if (g_log && c.pressing && c.pressing % 4 == 0)
    REXLOG_INFO("hctp: fighter {} presses into a climb wall ({} frames)", slot, c.pressing);
  if (c.pressing < 12 || c.cooldown > 0 || g_no_grab) return;
  const float yaw_was = RdF(base + f + 276);
  const float wx = RdF(base + f + 288) - c.x[(frame + 1) % 8], wz = RdF(base + f + 296) - c.z[(frame + 1) % 8];
  const auto saved = ctx;
  ctx.r3.u64 = f;
  sub_824B6A70(ctx, base);
  const uint32_t started = ctx.r3.u32;
  ctx = saved;
  c.cooldown = 30;
  c.pressing = 0;
  // (a spot's yaw comes from its wall's normal, and HCTP's collision winds some faces - the ladder, the crate
  // stack - the other way: the climb then played with his back to the wall. He walks into the wall, so the
  // climb faces against his walk - a fighter in a climb at yaw y faces (-sin y, -cos y))
  if (started && wx * wx + wz * wz > 1.0f) {
    const float y = RdF(base + f + 276);
    if (std::sin(y) * wx + std::cos(y) * wz > 0.0f) WrF(base + f + 276, y > 0.0f ? y - 3.14159265f : y + 3.14159265f);
  }
  REXLOG_INFO("hctp: fighter {} pressed into a climb wall at ({:.1f} {:.1f} {:.1f}): {} (yaw {:.2f} -> {:.2f})", slot,
              RdF(base + f + 288), RdF(base + f + 292), RdF(base + f + 296),
              started ? fmt::format("climbs (motion {})", Rd32(base + f + 212)) : std::string("no spot in range"),
              yaw_was, RdF(base + f + 276));
}

}  // namespace

// A fighter's stage collision, once a frame (sub_821E5910(collision
// component); the fighter at component +32).
REX_EXTERN(__imp__sub_821E5910);
REX_HOOK_RAW(sub_821E5910) {
  const uint32_t comp = ctx.r3.u32;
  const uint32_t f = comp ? Rd32(base + comp + 32) : 0;
  __imp__sub_821E5910(ctx, base);
  if (f && HctpRoomActive()) Climb(ctx, base, f);
}

REX_EXTERN(__imp__sub_82306B10);
REX_HOOK_RAW(sub_82306B10) {
  const uint32_t f = ctx.r3.u32;
  if (g_log) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t))
      REXLOG_INFO("hctp: wall switch: fighter {:08X} attr {:02X} motion {} running {}", f, base[f + 455],
                  Rd32(base + f + 212), base[f + 1286]);
  }
  __imp__sub_82306B10(ctx, base);
}

REX_EXTERN(__imp__sub_824B6A70);
REX_HOOK_RAW(sub_824B6A70) {
  const uint32_t f = ctx.r3.u32;
  __imp__sub_824B6A70(ctx, base);
  if (g_log) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t))
      REXLOG_INFO("hctp: T22 search: fighter {:08X} -> {}", f, ctx.r3.u32);
  }
}

REX_EXTERN(__imp__sub_82394520);
REX_HOOK_RAW(sub_82394520) {
  const uint32_t key = ctx.r4.u32, x = ctx.r5.u32, y = ctx.r6.u32;
  __imp__sub_82394520(ctx, base);
  if (g_log && key >= 12500 && key < 13600) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t)) REXLOG_INFO("hctp: motion key {} ({}, {}) -> {:08X}", key, x, y, ctx.r3.u32);
  }
}

// -- The room's gimmicks -------------------------------------------------------
//
// HCTP's Arena Back Entrance gimmicks (HCTP's own code: CGmControl and its
// object classes), from the mod's hctp.txt (written by the converter):
//   prop <name> <hctp id> <kind> <attr> <x> <y> <z> <ry> recs=<GMPD records> tris=<HMD 1004 triangles>
//        box=<x0,x1,y0,y1,z0,z1> (its collision's extent)
//   weapon <hctp id> <model> <x> <y> <z>
//   key <key> <frames>
// Each prop's wall triangles carry the low attribute byte <attr>, so a
// fighter's touched attribute (+455) names the prop he hit.
//  - stack (cardboard boxes, barrels; HCTP gmAttackObjAndWeapon) and throw
//    (railings, lead pipes; CGmThrowObject): a fighter thrown / whipped /
//    running into it (touching it while not in the walk motion) knocks it
//    over: its fall motion, his hit reaction, its collision gone, then its
//    weapon (2011 stand-ins: trash can 63, tire iron 120); railings vanish.
//  - roof (two halves, CGmComeOffObject): a knock-down on the roof (motion
//    1000 starting there) shakes it, the second breaks both halves - their
//    collision goes and whoever stands there falls into the cage.
//  - wall (the cage's chain-link walls, CGmWireNetting): the action button (X)
//    at a wall grabs it (12507); about 11 presses tear it and its partner
//    down (12508); they vanish and stop blocking.
namespace {

constexpr uint32_t kGimmickMgr = 0x82E3DBCC, kGimmickBank = 0x82E3CC40, kStage = 0x82E35530;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;
constexpr int kParkingTrip = 4;  // stage TRIP slot of rule 0x1B (HMD 1004)
// converted HCTP motion keys (hctp2ymks: HCTP act -> ours)
constexpr uint32_t kHitObject = 12515;  // HCTP 12014 hit-object reactions, variants 0-5
constexpr uint32_t kWallGrab = 12507, kWallBreak = 12508, kWallRelease = 12545;
constexpr uint32_t kLying = 1000;
constexpr uint32_t kButtonX = 0x80;  // (input +52: pressed this frame)
constexpr uint32_t kButtonB = 0x20, kButtonY = 0x10, kButtonR1 = 0x08;

struct Prop {
  std::string name, kind;
  int hid = 0;
  uint8_t attr = 0;
  float x = 0, y = 0, z = 0, ry = 0;
  std::vector<int> recs, tris;
  float box[6] = {};
  bool has_box = false;
  float zone[6] = {};  // switch / valve: where the button press counts
  bool has_zone = false;
  // run time
  int state = 0;  // 0 at rest, 1 falling / broken, 2 done
  int timer = 0;
  int count = 0;  // roof knock-downs
  bool held = false;
  int cooldown = 0;
  std::vector<std::pair<float, float>> saved_y;  // collision y ranges, for RestoreCollision
  // rooms 31-36: the HCTP tag's params (p=), the record models in order (m=), the HMD shapes the
  // params name as 2011 boxes (h<id>=x0,x1,y0,y1,z0,z1)
  std::vector<float> params;
  std::vector<int> models;
  std::map<int, std::array<float, 6>> shapes;
  int bits = 0;  // (car: 1 door off, 2 bonnet off, 4 crushed)
  int body = 0;  // (car: the body model shown, 0 whole / 1 dented)
  uint32_t who = 0;  // (the wrestler it holds: locker, toilet, treadmill)
};
// A steam jet (HCTP particle 32 / 34, toggled by a valve): from (x, y, z)
// along (dx, dz) for len, half width hw, half height hh.
struct Steam {
  int pid = 0, valve = 0;
  float x = 0, y = 0, z = 0, dx = 0, dz = 0, len = 0, hw = 0, hh = 0;
  bool on = false;
  int t = 0;
};
struct WeaponSpot {
  int hid, model;
  float x, y, z;
};
// A door of the HCTP backstage as one stage (world hctp.txt): where it is, the
// door it leads to, and where a fighter coming in through it stands (x, z, yaw).
// HCTP's gates (build_world.py's gate lines) also carry the act the one going
// through plays first and the way he faces for it, and a spot for each of the 8
// fighters coming in (HCTP's own); older lines have one entry spot (ex, ez).
struct Door {
  int id = 0, to = 0;
  float x = 0, y = 0, z = 0, ex = 0, ez = 0, eyaw = 0;
  uint32_t act = 0;
  float oyaw = 0;
  std::vector<std::array<float, 2>> spots;
};
struct Room {
  std::string dir;
  // (world: the HCTP room number, its stage model ids and gimmick object ids
  // (shown only while it is the current room), its object acts' first key and
  // its floor's extent x0, x1, z0, z1)
  int number = 0;
  int model_lo = 0, model_hi = -1, obj_lo = 0, obj_hi = -1;
  uint32_t obj_act = 12700;
  float area[4] = {};
  std::vector<Door> doors;
  std::map<int, bool> rec_shown;  // (records ShowRec showed / hid: kept while the room is away)
  // (world: its triangles in HMD 1004 and its climb-spot records, first..last; the weapons put down on
  // its first visit - model, x, y, z)
  int tri_lo = 0, tri_hi = -1, spot_lo = 0, spot_hi = -1;
  std::vector<std::array<float, 4>> spawns;
  bool visited = false;
  std::vector<std::pair<float, float>> coll_saved;  // (its triangles' y while it is away)
  std::vector<std::pair<uint32_t, std::array<float, 3>>> parked;  // (its weapons lying there while it is away)
  std::vector<Prop> props;
  std::vector<WeaponSpot> weapons;
  std::map<uint32_t, int> frames;
  bool loaded = false;
  uint32_t last_frame = 0;
  int drop = 0;  // frames left of the roof's fall-in nudge
  std::vector<Steam> steam;
  int set_up_frames = 0;  // frames of object setup done (SetUp)
  // the switch cycle: 0 armed, 1 shutter opening, 2 cart rolling, 3 cart hit
  // something / arrived, 4 waiting, 5 shutter closing
  int cycle = 0, cycle_t = 0;
  float cart_z = 0;
  // the equipment crate: 0 ready, 1 victim shut in, 2 climbing out, 3 resting
  int crate = 0, crate_t = 0;
  uint32_t crate_victim = 0;
  std::vector<std::array<float, 3>> poles;  // (room 33: the climbable poles' tops)
  int heli_t = 0;                            // (room 33: frames into the helicopter's flight)
  std::vector<std::array<float, 5>> heli;    // (room 33: its flight keys - HCTP frame, x, y, z, yaw)
};
// The rooms of the mod's hctp.txt: one, or (the HCTP backstage as one stage,
// 'room' lines) several, of which the fighters' is the current one.
std::vector<Room> g_rooms(1);
size_t g_cur = 0;
Room& Cur() { return g_rooms[g_cur]; }
bool World() { return g_rooms.size() > 1; }

// The rooms' own random numbers (hit-reaction variants, knock-downs, traffic timing): one sequence,
// restarted when the room data loads at the match's start, so both sides of inputs-only netplay draw the
// same ones (the C library's rand is shared with other code: its sequence can differ between the games).
uint32_t g_room_rng = 1;
int RoomRand() {
  g_room_rng = g_room_rng * 1103515245u + 12345u;
  return int((g_room_rng >> 16) & 0x7FFF);
}
int HctpRoom() {
  if (!HctpRoomActive()) return 0;
  return World() ? Cur().number : svr2011::BackstageRows()[size_t(svr2011::g_active_row)].hctp;
}

struct FighterState {
  uint32_t fighter = 0, motion = 0;
  int wall = -1;  // the wall prop he holds
  int presses = 0;
  int idle = 0;   // frames since his last press
  int away = 0;   // frames the stick has pointed away from the wall he holds
  int burn_cd = 0;  // frames before steam can burn him again
};
FighterState g_fs[8];


std::vector<int> Ints(const std::string& s) {
  std::vector<int> out;
  size_t at = 0;
  while (at < s.size()) {
    const size_t end = std::min(s.find(',', at), s.size());
    out.push_back(std::atoi(s.substr(at, end - at).c_str()));
    at = end + 1;
  }
  return out;
}

// The climbable cage panels of the current room.
void ClimbAttrs() {
  std::fill(std::begin(g_climb_attr), std::end(g_climb_attr), false);
  for (const Prop& p : Cur().props)
    if (p.kind == "wall") g_climb_attr[p.attr] = true;
}

bool LoadRoom(const std::string& dir) {
  g_rooms.assign(1, Room{});
  g_cur = 0;
  Cur().dir = dir;
  bool first_room = true;
  std::ifstream in(std::filesystem::path(dir) / "hctp.txt");
  if (!in) {
    REXLOG_WARN("hctp: no hctp.txt in {}", dir);
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string what;
    ls >> what;
    if (what == "room") {  // (world: a new room starts)
      if (!first_room) g_rooms.push_back(Room{}), g_cur = g_rooms.size() - 1, Cur().dir = dir;
      first_room = false;
      Room& r = Cur();
      ls >> r.number >> r.model_lo >> r.model_hi >> r.obj_lo >> r.obj_hi >> r.obj_act >> r.area[0] >> r.area[1] >>
          r.area[2] >> r.area[3] >> r.tri_lo >> r.tri_hi >> r.spot_lo >> r.spot_hi;
    } else if (what == "spawn") {
      std::array<float, 4> w{};
      ls >> w[0] >> w[1] >> w[2] >> w[3];
      Cur().spawns.push_back(w);
    } else if (what == "door") {
      Door d;
      ls >> d.id >> d.to >> d.x >> d.y >> d.z;
      if (line.find(';') != std::string::npos) {  // (a gate: act, way out, 8 'x,z' spots, way in)
        std::string spots;
        ls >> d.act >> d.oyaw >> spots >> d.eyaw;
        std::istringstream ss(spots);
        std::array<float, 2> s{};
        char c = 0;
        while (ss >> s[0] >> c >> s[1]) {
          d.spots.push_back(s);
          ss >> c;  // (';')
        }
        if (!d.spots.empty()) d.ex = d.spots[0][0], d.ez = d.spots[0][1];
      } else {
        ls >> d.ex >> d.ez >> d.eyaw;
      }
      Cur().doors.push_back(d);
    } else if (what == "prop") {
      Prop p;
      int attr = 0;
      ls >> p.name >> p.hid >> p.kind >> attr >> p.x >> p.y >> p.z >> p.ry;
      p.attr = uint8_t(attr);
      std::string kv;
      while (ls >> kv) {
        if (kv.rfind("recs=", 0) == 0) p.recs = Ints(kv.substr(5));
        if (kv.rfind("tris=", 0) == 0) p.tris = Ints(kv.substr(5));
        if (kv.rfind("zone=", 0) == 0)
          p.has_zone = std::sscanf(kv.c_str() + 5, "%f,%f,%f,%f,%f,%f", &p.zone[0], &p.zone[1], &p.zone[2],
                                   &p.zone[3], &p.zone[4], &p.zone[5]) == 6;
        if (kv.rfind("box=", 0) == 0)
          p.has_box = std::sscanf(kv.c_str() + 4, "%f,%f,%f,%f,%f,%f", &p.box[0], &p.box[1], &p.box[2], &p.box[3],
                                  &p.box[4], &p.box[5]) == 6;
        if (kv.rfind("p=", 0) == 0) {
          std::istringstream ps(kv.substr(2));
          std::string v;
          while (std::getline(ps, v, ',')) p.params.push_back(float(std::atof(v.c_str())));
        }
        if (kv.rfind("m=", 0) == 0) p.models = Ints(kv.substr(2));
        if (kv.size() > 2 && kv[0] == 'h' && std::isdigit(uint8_t(kv[1]))) {
          std::array<float, 6> b{};
          const int id = std::atoi(kv.c_str() + 1);
          const size_t eq = kv.find('=');
          if (eq != std::string::npos && std::sscanf(kv.c_str() + eq + 1, "%f,%f,%f,%f,%f,%f", &b[0], &b[1], &b[2],
                                                     &b[3], &b[4], &b[5]) == 6)
            p.shapes[id] = b;
        }
      }
      Cur().props.push_back(std::move(p));
    } else if (what == "weapon") {
      WeaponSpot w{};
      ls >> w.hid >> w.model >> w.x >> w.y >> w.z;
      Cur().weapons.push_back(w);
    } else if (what == "steam") {
      Steam st;
      ls >> st.pid >> st.valve >> st.x >> st.y >> st.z >> st.dx >> st.dz >> st.len >> st.hw >> st.hh;
      Cur().steam.push_back(st);
    } else if (what == "heli") {
      std::array<float, 5> k{};
      ls >> k[0] >> k[1] >> k[2] >> k[3] >> k[4];
      Cur().heli.push_back(k);
    } else if (what == "pole") {
      int id = 0;
      std::array<float, 3> q{};
      ls >> id >> q[0] >> q[1] >> q[2];
      Cur().poles.push_back(q);
    } else if (what == "key") {
      uint32_t k = 0;
      int n = 0;
      ls >> k >> n;
      Cur().frames[k] = n;
    }
  }
  // (the converted motions' lengths are one list: every room knows them all)
  std::map<uint32_t, int> frames;
  for (const Room& r : g_rooms) frames.insert(r.frames.begin(), r.frames.end());
  for (Room& r : g_rooms) r.frames = frames, r.loaded = true;
  g_cur = 0;
  ClimbAttrs();
  for (const Room& r : g_rooms)
    REXLOG_INFO("hctp: room data (room {}): {} props, {} weapon spots, {} doors, {} motions", r.number,
                r.props.size(), r.weapons.size(), r.doors.size(), r.frames.size());
  return true;
}

// The T23 element of a GMPD record id (0 if none).
uint32_t Element(PPCContext& ctx, uint8_t* base, int rec) {
  const uint32_t mgr = Rd32(base + kGimmickMgr);
  if (!mgr) return 0;
  const auto saved = ctx;
  ctx.r3.u64 = mgr;
  ctx.r4.u64 = uint32_t(rec);
  ctx.r5.u64 = 0;
  sub_824B0DC8(ctx, base);
  const uint32_t e = ctx.r3.u32;
  ctx = saved;
  return e && Rd32(base + e + 68) == 23 ? e : 0;
}

bool HasKey(PPCContext& ctx, uint8_t* base, uint32_t key, uint32_t x, uint32_t y) {
  const auto saved = ctx;
  ctx.r3.u64 = Rd32(base + kGimmickBank);
  ctx.r4.u64 = key, ctx.r5.u64 = x, ctx.r6.u64 = y;
  sub_82394520(ctx, base);
  const bool ok = ctx.r3.u32 != 0;
  ctx = saved;
  return ok;
}

// One record's objects play (key, x, y) from the room bank.
void PlayRec(PPCContext& ctx, uint8_t* base, int rec, uint32_t key, uint32_t x = 0, uint32_t y = 0) {
  const uint32_t e = Element(ctx, base, rec);
  if (!e) return;
  for (uint32_t at : {100u, 104u})  // (a moved object: its motion drives it again)
    if (const uint32_t obj = Rd32(base + e + at)) Wr32(base + obj + 1140, 0);
  const auto saved = ctx;
  ctx.r3.u64 = e;
  ctx.r4.u64 = key, ctx.r5.u64 = x, ctx.r6.u64 = y;
  sub_824B91C0(ctx, base);
  ctx = saved;
}

// A prop's objects play (key, x, y) from the room bank.
void PlayProp(PPCContext& ctx, uint8_t* base, const Prop& p, uint32_t key, uint32_t x = 0, uint32_t y = 0) {
  if (!HasKey(ctx, base, key, x, y)) {
    REXLOG_WARN("hctp: {}: no motion ({}, {}, {}) in the room bank", p.name, key, x, y);
    return;
  }
  for (int rec : p.recs) PlayRec(ctx, base, rec, key, x, y);
}

// One record's element placed at (x, y, z), turned (rx, ry, rz): its
// position and rotation, then sub_824B9C40 copies them to both objects and
// rebuilds their nodes. The objects' pose copy (sub_824B46F8, each frame)
// would put back the matrices their last motion made at the old place, so
// their motion component ('S3MW') is marked not running (+12 = 0; the next
// key sets it again) - 46F8 then only commits the rebuilt node. (Not +1140:
// that skips the commit too and the object isn't drawn where it is.)
void StopPoseCopy(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  const auto saved = ctx;
  ctx.r3.u64 = obj + 1088;
  ctx.r4.u64 = 0x53334D57;  // 'S3MW'
  sub_8269DFB0(ctx, base);
  const uint32_t c = ctx.r3.u32;
  ctx = saved;
  if (const uint32_t m = c ? Rd32(base + c + 24) : 0) Wr32(base + m + 12, 0);
  Wr32(base + obj + 1140, 0);
}

void MoveRec(PPCContext& ctx, uint8_t* base, int rec, float x, float y, float z, float rx = 0, float ry = 0,
             float rz = 0) {
  const uint32_t e = Element(ctx, base, rec);
  if (!e) return;
  WrF(base + e + 32, x), WrF(base + e + 36, y), WrF(base + e + 40, z);
  WrF(base + e + 48, rx), WrF(base + e + 52, ry), WrF(base + e + 56, rz);
  for (uint32_t at : {100u, 104u})
    if (const uint32_t obj = Rd32(base + e + at)) StopPoseCopy(ctx, base, obj);
  const auto saved = ctx;
  ctx.r3.u64 = e;
  sub_824B9C40(ctx, base);
  ctx = saved;
}

// A prop's objects freeze on their current pose (the end of a fall).
void HoldProp(PPCContext& ctx, uint8_t* base, const Prop& p) {
  for (int rec : p.recs)
    if (const uint32_t e = Element(ctx, base, rec))
      for (uint32_t at : {100u, 104u})
        if (const uint32_t obj = Rd32(base + e + at)) Wr32(base + obj + 1140, 1);
}

void ShowRecRaw(PPCContext& ctx, uint8_t* base, int rec, bool show);
void ShowRec(PPCContext& ctx, uint8_t* base, int rec, bool show) {
  Cur().rec_shown[rec] = show;
  ShowRecRaw(ctx, base, rec, show);
}
void ShowRecRaw(PPCContext& ctx, uint8_t* base, int rec, bool show) {
  const uint32_t e = Element(ctx, base, rec);
  if (!e) return;
  // (hidden for good: the element inactive too - a playing key shows its
  // objects again otherwise)
  const auto saved_e = ctx;
  ctx.r3.u64 = e, ctx.r4.u64 = show ? 1 : 0, ctx.r5.u64 = show ? 1 : 0;
  sub_824B8FE0(ctx, base);
  ctx = saved_e;
  for (uint32_t at : {100u, 104u})
    if (const uint32_t obj = Rd32(base + e + at)) {
      const auto saved = ctx;
      ctx.r3.u64 = obj, ctx.r4.u64 = show ? 1 : 0;
      sub_824B4198(ctx, base);
      ctx.r3.u64 = obj, ctx.r4.u64 = show ? 1 : 0;
      sub_824B4150(ctx, base);
      ctx = saved;
    }
}

void ShowProp(PPCContext& ctx, uint8_t* base, const Prop& p, bool show) {
  for (int rec : p.recs) ShowRec(ctx, base, rec, show);
  if (g_log) REXLOG_INFO("hctp: {} {}", p.name, show ? "shown" : "hidden");
}

// The prop's triangles out of the parking room's collision (their y range
// moved far away: the broad phase never marks them near).
void DropCollision(uint8_t* base, Prop& p) {
  const uint32_t stage = Rd32(base + kStage);
  const uint32_t trip = stage ? Rd32(base + stage + 22516 + 4 * kParkingTrip) : 0;
  if (!trip) return;
  const uint32_t n = Rd32(base + trip + 176), recs = Rd32(base + trip + 180);
  int dropped = 0;
  for (int t : p.tris) {
    if (t < 0 || uint32_t(t) >= n) continue;
    const uint32_t r = Rd32(base + recs + 4 * uint32_t(t));
    if (!r) continue;
    if (RdF(base + r + 12) < 1.0e5f) p.saved_y.push_back({RdF(base + r + 12), RdF(base + r + 28)});
    WrF(base + r + 12, 1.0e6f);
    WrF(base + r + 28, 1.0e6f);
    ++dropped;
  }
  REXLOG_INFO("hctp: {}: {} collision triangles out", p.name, dropped);
}

// The prop's triangles back (what DropCollision took out).
void RestoreCollision(uint8_t* base, Prop& p) {
  const uint32_t stage = Rd32(base + kStage);
  const uint32_t trip = stage ? Rd32(base + stage + 22516 + 4 * kParkingTrip) : 0;
  if (!trip || p.saved_y.empty()) return;
  const uint32_t n = Rd32(base + trip + 176), recs = Rd32(base + trip + 180);
  size_t k = 0;
  for (int t : p.tris) {
    if (t < 0 || uint32_t(t) >= n || k >= p.saved_y.size()) continue;
    const uint32_t r = Rd32(base + recs + 4 * uint32_t(t));
    if (!r) continue;
    WrF(base + r + 12, p.saved_y[k].first);
    WrF(base + r + 28, p.saved_y[k].second);
    ++k;
  }
  p.saved_y.clear();
}

// A fighter plays (key, x, y) from the room bank, from frame 'start'.
void JumpAct(PPCContext& ctx, uint8_t* base, uint32_t f, uint32_t key, uint32_t x = 0, uint32_t y = 0,
             float start = 0.0f) {
  if (!HasKey(ctx, base, key, x, y)) return;
  const auto saved = ctx;
  ctx.r3.u64 = f;
  ctx.r4.u64 = key, ctx.r5.u64 = x, ctx.r6.u64 = y;
  ctx.f1.f64 = start;
  sub_82396E80(ctx, base);
  ctx = saved;
}

// A weapon lying at (x, y, z): sub_821D2070(any free slot, position,
// rotation, model; stack: motion 15010 lying, place 1 floor, flag 1).
void SpawnWeapon(PPCContext& ctx, uint8_t* base, int model, float x, float y, float z) {
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  Wr32(base + ctx.r1.u32 + 84, 15010);
  Wr32(base + ctx.r1.u32 + 92, 1);
  Wr32(base + ctx.r1.u32 + 100, 1);
  ctx.r3.u64 = 0xFFFFFFFFu;
  ctx.f1.f64 = x, ctx.f2.f64 = y, ctx.f3.f64 = z;
  ctx.f4.f64 = 0, ctx.f5.f64 = 0, ctx.f6.f64 = 0;
  ctx.r7.u64 = uint32_t(model);
  sub_821D2070(ctx, base);
  const uint32_t w = ctx.r3.u32;
  ctx = saved;
  REXLOG_INFO("hctp: weapon {} at ({:.0f} {:.0f} {:.0f}): {}", model, x, y, z,
              w ? "made" : "not made (no free slot, or the model isn't loaded)");
}

int Frames(uint32_t key, int fallback) {
  const auto it = Cur().frames.find(key);
  return it == Cur().frames.end() ? fallback : it->second;
}

// Rooms 31-36: HCTP backstage acts 12000 + n are converted as 12600 + n, the room's object
// acts n (its bank 50102) as 12700 + n (build_room.sh).
constexpr uint32_t kRoomAct = 12600;
uint32_t ObjAct() { return Cur().obj_act; }  // (12700; the world: each room its own)

float Param(const Prop& p, size_t i, float fallback = 0.0f) { return i < p.params.size() ? p.params[i] : fallback; }

// A prop's own fall motion: room 30's converted keys; other rooms: its tag's object act (param 3).
uint32_t FallKey(const Prop& p) {
  switch (p.hid) {
    case 202: return 12533;  // cardboard boxes collapse
    case 218: return 12526;  // barrels fall
    case 209: return 12524;  // lead pipes fall
    case 206: return 12528;  // a railing falls
    default: return Param(p, 3) > 0 ? ObjAct() + uint32_t(Param(p, 3)) : 0;
  }
}

void KnockOver(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f) {
  p.state = 1;
  p.timer = 0;
  if (const uint32_t key = FallKey(p)) PlayProp(ctx, base, p, key);
  DropCollision(base, p);
  if (f) JumpAct(ctx, base, f, kHitObject, uint32_t(1 + RoomRand() % 5));
  REXLOG_INFO("hctp: {} knocked over", p.name);
}

// The frame a prop's first object is on (its motion's +24), -1 if none.
float PropFrame(PPCContext& ctx, uint8_t* base, const Prop& p) {
  for (int rec : p.recs)
    if (const uint32_t e = Element(ctx, base, rec))
      for (uint32_t at : {100u, 104u})
        if (const uint32_t obj = Rd32(base + e + at)) return RdF(base + obj + 24);
  return -1.0f;
}

void UpdateFall(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state != 1) return;
  ++p.timer;
  const int n = Frames(FallKey(p), 40);
  // (motions run at 30 frames a second, the room at 60: hold the last pose
  // when the object's own frame gets there)
  if (!p.held && (PropFrame(ctx, base, p) >= float(n - 1) || p.timer > 3 * n)) {
    HoldProp(ctx, base, p);
    p.held = true;
  }
  // its weapon, when the stack has come down (pipes: HCTP's 60 frames)
  if (p.timer == 2 * std::max(n, p.hid == 209 ? 60 : 0))
    for (const WeaponSpot& w : Cur().weapons)
      if (w.hid == p.hid) SpawnWeapon(ctx, base, w.model, w.x, w.y, w.z);
  // a railing: gone after HCTP's 42 + 30 frames; other rooms' throw objects at their vanish start
  // (param 4; it vanishes over param 5 frames)
  if (p.hid == 206 && p.timer == 2 * 72) ShowProp(ctx, base, p, false);
  if (!p.params.empty() && Param(p, 4, -1) >= 0 && p.timer == 2 * int(Param(p, 4) + Param(p, 5) / 2))
    ShowProp(ctx, base, p, false);
  if (p.held && p.timer > 2 * std::max(n, 72) + 10) p.state = 2;
}

// Inside a prop's collision extent (x, z) grown by m.
bool InBox(const Prop& p, float x, float z, float m) {
  return p.has_box && x > p.box[0] - m && x < p.box[1] + m && z > p.box[4] - m && z < p.box[5] + m;
}

// On a roof half: over its panel, on the roof (y < -25).
bool OnRoofHalf(const Prop& p, float x, float y, float z) {
  return y < -25.0f && InBox(p, x, z, 1.5f);
}

void BreakRoof(PPCContext& ctx, uint8_t* base) {
  for (Prop& r : Cur().props)
    if (r.kind == "roof" && r.state == 0) {
      r.state = 1;
      DropCollision(base, r);
      ShowProp(ctx, base, r, false);
    }
  Cur().drop = 20;
  REXLOG_INFO("hctp: the cage roof breaks");
}

// Whoever stands over the hole falls into the cage (HCTP CellCheck_BackOpenSpace:
// nudged toward the middle, ~0.4 a frame, and gravity takes him to the floor).
void DropIn(uint8_t* base) {
  if (Cur().drop <= 0) return;
  --Cur().drop;
  for (const Prop& r : Cur().props) {
    if (r.kind != "roof" || !r.has_box) continue;
    const float cx = (r.box[0] + r.box[1]) / 2, cz = (r.box[4] + r.box[5]) / 2;
    for (int i = 0; i < 8; ++i) {
      const uint32_t f = Rd32(base + kChars + i * 4);
      if (!f) continue;
      const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
      if (!OnRoofHalf(r, x, y, z)) continue;
      WrF(base + f + 288, x + std::clamp(cx - x, -0.4f, 0.4f));
      WrF(base + f + 296, z + std::clamp(cz - z, -0.4f, 0.4f));
      if (g_log && Cur().drop % 5 == 0)
        REXLOG_INFO("hctp: fighter {} over the hole at ({:.1f} {:.1f} {:.1f})", i, x, y, z);
    }
  }
}

// The wall's partner: HCTP tears down both panels of a side together
// (walls in placement order: 0, 1 one side; 2, 3 the other).
int WallPartner(int i) {
  std::vector<int> walls;
  for (size_t j = 0; j < Cur().props.size(); ++j)
    if (Cur().props[j].kind == "wall") walls.push_back(int(j));
  for (size_t k = 0; k < walls.size(); ++k)
    if (walls[k] == i && (k ^ 1) < walls.size()) return walls[k ^ 1];
  return -1;
}

void BreakWall(PPCContext& ctx, uint8_t* base, int i) {
  for (int j : {i, WallPartner(i)}) {
    if (j < 0) continue;
    Prop& w = Cur().props[size_t(j)];
    if (w.state) continue;
    w.state = 1;
    w.timer = 0;
    g_climb_attr[w.attr] = false;
    PlayProp(ctx, base, w, kWallBreak, 0, 1);
  }
}

void UpdateWall(PPCContext& ctx, uint8_t* base, Prop& w) {
  if (w.state != 1) return;
  if (++w.timer == 100) {  // (HCTP: it fades out from frame 50 - 30 a second)
    ShowProp(ctx, base, w, false);
    DropCollision(base, w);
    w.state = 2;
  }
}

// HCTP motion keys of the switch, valves, steam and crate (hctp2ymks)
constexpr uint32_t kPressButton = 12510;                     // HCTP 12035
constexpr uint32_t kValveOpen = 12511, kValveClose = 12512;  // 12036 / 12037 (the valve on track 1)
constexpr uint32_t kSteamBurn = 12509;                       // 12047
constexpr uint32_t kHitByObject = 12516;                     // 340
constexpr uint32_t kCrateIn = 12513, kCrateOut = 12514;      // 12034 (attacker 0, victim 1, crate 2) / 12043
constexpr uint32_t kShutterOpen = 12525, kShutterClose = 12530, kShutterRest = 12555;
constexpr uint32_t kCratesFall = 12529, kCratesIdle = 12531;
constexpr uint32_t kJumpDown = 12505;   // HCTP 12572
constexpr uint32_t kStepIn = 12506;     // HCTP 12092

Prop* FindKind(const char* kind, int nth = 0) {
  for (Prop& p : Cur().props)
    if (p.kind == kind && nth-- == 0) return &p;
  return nullptr;
}

bool InZone(const Prop& p, float x, float y, float z) {
  return p.has_zone && x > p.zone[0] - 1 && x < p.zone[1] + 1 && z > p.zone[4] - 1 && z < p.zone[5] + 1 &&
         std::fabs(y - p.zone[2]) < 5.0f;
}

// A fighter put at (x, y, z) facing yaw (his position and his physics body's).
// (After his act starts, as the game's own T22 snap does - sub_824B67D0: the
// act's root motion counts from where he is then.)
// A wrestler's yaw facing the way a prop at object yaw a faces (turned by 'turn' as the HCTP code turns
// its own): -a + turn. An object's local +z at yaw a points (-sin a, cos a), a wrestler's yaw y faces
// (sin y, cos y) - the prop's own a put him the mirrored way on props turned 90 (room 35's lions).)
float FighterYaw(float object_yaw, float turn = 0.0f) { return -object_yaw + turn; }

void PlaceFighter(uint8_t* base, uint32_t f, float x, float y, float z, float yaw) {
  WrF(base + f + 288, x), WrF(base + f + 292, y), WrF(base + f + 296, z);
  WrF(base + f + 276, yaw);
  if (const uint32_t body = Rd32(base + f + 304)) {
    WrF(base + body, x), WrF(base + body + 4, y), WrF(base + body + 8, z);
  }
}

// A wrestler put somewhere for good (through a door, the door test): placed now and again at the start of
// the next kHeldFrames world frames, before the game's update - with the body's other two position copies
// (+704, +768). Online the game's update puts him back on his physics body and the body then eases toward
// him over several frames (seen with svrtest lan: placed at the door, back at his old place the next frame,
// then 150 -> 121 -> 108 ...); placed this way the body has nothing left to ease.
constexpr int kHeldFrames = 10;
struct Held {
  uint32_t f = 0;
  float x = 0, y = 0, z = 0, yaw = 0;
  int frames = 0;
};
Held g_held[8];

void PlaceBody(uint8_t* base, uint32_t f, float x, float y, float z, float yaw) {
  PlaceFighter(base, f, x, y, z, yaw);
  if (const uint32_t body = Rd32(base + f + 304))
    for (uint32_t at : {704u, 768u}) WrF(base + body + at, x), WrF(base + body + at + 4, y), WrF(base + body + at + 8, z);
}

void PlaceFighterHeld(uint8_t* base, uint32_t f, float x, float y, float z, float yaw) {
  PlaceBody(base, f, x, y, z, yaw);
  Held* slot = nullptr;
  for (Held& h : g_held)
    if (h.f == f || (!slot && !h.frames)) slot = &h;
  if (slot) *slot = {f, x, y, z, yaw, kHeldFrames};
}

// First frame: the closing shutter (HCTP syattar2) and the steam puffs hidden.
void ShowModel(PPCContext& ctx, uint8_t* base, const Prop& p, int model, bool show);

void SetUp(PPCContext& ctx, uint8_t* base) {
  // (again for the room's first frames: the elements may not be there yet
  // on the first one)
  if (Cur().set_up_frames >= 30) return;
  ++Cur().set_up_frames;
  if (Prop* sh = FindKind("shutter"); sh && sh->recs.size() > 1) ShowRec(ctx, base, sh->recs[1], false);
  for (Prop& p : Cur().props) {
    if (p.kind == "car" && p.models.size() > 1 && !p.body) ShowModel(ctx, base, p, p.models[1], false);
    if ((p.kind == "window" || p.kind == "traffic") && p.state == 0) ShowProp(ctx, base, p, false);
    if (p.kind == "brokenwall" && p.state == 0)
      for (size_t k = 1; k < p.models.size(); ++k) ShowModel(ctx, base, p, p.models[k], false);
    if (p.kind == "movecar")
      for (int mdl : p.models) ShowModel(ctx, base, p, mdl, false);
  }
  // (the steam puffs wait under the floor: an object hidden and shown again
  // isn't drawn while no key plays on it)
  for (Prop& p : Cur().props)
    if (p.kind == "puff")
      for (int rec : p.recs) MoveRec(ctx, base, rec, p.x, 1000.0f, p.z);
  // (the cart has no rest key: until a move rebuilds its node it's drawn
  // off its spot, a grey plank by the cage - put it in place now)
  if (Prop* cart = FindKind("cart")) {
    Cur().cart_z = cart->z;
    for (int rec : cart->recs) MoveRec(ctx, base, rec, cart->x, cart->y, cart->z, 0, cart->ry, 0);
  }
}

// -- The green buttons, the shutter and the cart (HCTP CGmSwitch 208,
// CGmShutter 201, CGmCrateAndWinch 220): a button opens the garage shutter
// and the crate cart rolls out into the room; whoever it runs into is hit
// (340) and the crates fall off; 240 HCTP frames later the shutter closes
// and everything is set again.
void PressSwitch(PPCContext& ctx, uint8_t* base, uint32_t f, int i, const Prop& sw) {
  JumpAct(ctx, base, f, kPressButton);
  REXLOG_INFO("hctp: fighter {} presses {}", i, sw.name);
  if (Cur().cycle != 0) return;
  Cur().cycle = 1;
  Cur().cycle_t = 0;
  if (Prop* sh = FindKind("shutter")) PlayRec(ctx, base, sh->recs[0], kShutterOpen);
}

void UpdateCycle(PPCContext& ctx, uint8_t* base) {
  Prop* sh = FindKind("shutter");
  Prop* cart = FindKind("cart");
  if (!sh || !cart || Cur().cycle == 0) return;
  const int t = ++Cur().cycle_t;
  constexpr float kCartEnd = -429.0f;  // HCTP z -11
  switch (Cur().cycle) {
    case 1:  // the shutter rolls up (26 frames at 30 a second), then it's gone
      if (t == 2 * 26) {
        ShowRec(ctx, base, sh->recs[0], false);
        DropCollision(base, *cart);
        Cur().cycle = 2, Cur().cycle_t = 0;
        REXLOG_INFO("hctp: the shutter is up - the cart rolls");
      }
      break;
    case 2: {  // the cart rolls out (HCTP CObjMove: 5% of the way a frame at 30)
      const float step = std::clamp((kCartEnd - Cur().cart_z) * 0.025f, 0.3f, 1.6f);
      Cur().cart_z = std::min(Cur().cart_z + step, kCartEnd);
      for (int rec : cart->recs) MoveRec(ctx, base, rec, cart->x, cart->y, Cur().cart_z, 0, cart->ry, 0);
      const float dz = Cur().cart_z - cart->z;
      bool hit = false;
      for (int i = 0; i < 8; ++i) {
        const uint32_t f = Rd32(base + kChars + i * 4);
        if (!f) continue;
        const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
        if (y < -6.0f || x < cart->box[0] - 1 || x > cart->box[1] + 1 || z < cart->box[4] + dz - 1 ||
            z > cart->box[5] + dz + 1)
          continue;
        JumpAct(ctx, base, f, kHitByObject);
        REXLOG_INFO("hctp: the cart runs into fighter {}", i);
        hit = true;
      }
      if (hit || Cur().cart_z >= kCartEnd - 0.01f) {
        PlayRec(ctx, base, cart->recs[0], kCratesFall);
        Cur().cycle = 3, Cur().cycle_t = 0;
      }
      break;
    }
    case 3:  // the crates fall off; cart, shadow and crates fade (HCTP 60..160)
      if (t == 2 * 60) {
        ShowRec(ctx, base, cart->recs[1], false);
        if (cart->recs.size() > 2) ShowRec(ctx, base, cart->recs[2], false);
      }
      if (t == 2 * 130) {
        ShowRec(ctx, base, cart->recs[0], false);
        Cur().cycle = 4, Cur().cycle_t = 0;
      }
      break;
    case 4:  // 240 HCTP frames, then the shutter closes (syattar2, act 30)
      if (t == 2 * 240) {
        if (sh->recs.size() > 1) {
          ShowRec(ctx, base, sh->recs[1], true);
          PlayRec(ctx, base, sh->recs[1], kShutterClose);
        }
        Cur().cycle = 5, Cur().cycle_t = 0;
      }
      break;
    case 5:  // closed: the shutter and the cart back as they were
      if (t == 2 * 31) {
        if (sh->recs.size() > 1) ShowRec(ctx, base, sh->recs[1], false);
        ShowRec(ctx, base, sh->recs[0], true);
        PlayRec(ctx, base, sh->recs[0], kShutterRest);
        Cur().cart_z = cart->z;
        for (int rec : cart->recs) {
          ShowRec(ctx, base, rec, true);
          MoveRec(ctx, base, rec, cart->x, cart->y, cart->z, 0, cart->ry, 0);
        }
        PlayRec(ctx, base, cart->recs[0], kCratesIdle);
        RestoreCollision(base, *cart);
        Cur().cycle = 0;
        REXLOG_INFO("hctp: the shutter is down - the button works again");
      }
      break;
  }
}

// -- Valves and steam (HCTP CGmSwitchOnOffParticle 210 / 215, particles
// 32 / 34): the action button at a valve opens it (12036) and its pipe lets
// out steam, again closes it (12037); at most a toggle every 121 frames,
// off by itself after 250 (HCTP frames). Whoever is in the jet is burnt
// (12047). Valve 210 also brings the barrel stack down.
Steam* SteamOf(const Prop& valve) {
  for (Steam& st : Cur().steam)
    if (st.valve == valve.hid) return &st;
  return nullptr;
}

void SetSteam(PPCContext& ctx, uint8_t* base, Steam& st, bool on) {
  st.on = on;
  st.t = 0;
  if (!on)
    for (Prop& p : Cur().props)
      if (p.kind == "puff" && p.hid == st.pid)
        for (int rec : p.recs) MoveRec(ctx, base, rec, p.x, 1000.0f, p.z);
  REXLOG_INFO("hctp: steam {} {}", st.pid, on ? "on" : "off");
}

void TurnValve(PPCContext& ctx, uint8_t* base, uint32_t f, int i, Prop& v) {
  if (v.cooldown > 0) return;
  v.cooldown = 2 * 121;
  Steam* st = SteamOf(v);
  const bool open = v.state == 0;
  if (f) JumpAct(ctx, base, f, open ? kValveOpen : kValveClose);
  PlayProp(ctx, base, v, open ? kValveOpen : kValveClose, 0, 1);
  REXLOG_INFO("hctp: fighter {} {} {}", i, open ? "opens" : "closes", v.name);
  v.state = open ? 1 : 0;
  if (st) {
    if (open) {
      st->on = false;
      st->t = -2 * 20;  // (the steam comes as the valve turns)
    } else if (st->on) {
      SetSteam(ctx, base, *st, false);
    }
  }
  if (open && v.hid == 210)
    for (Prop& p : Cur().props)
      if (p.hid == 218 && p.state == 0) KnockOver(ctx, base, p, 0);
}

void UpdateSteam(PPCContext& ctx, uint8_t* base) {
  for (Prop& v : Cur().props)
    if (v.kind == "valve" && v.cooldown > 0) --v.cooldown;
  for (Steam& st : Cur().steam) {
    Prop* v = nullptr;
    for (Prop& p : Cur().props)
      if (p.kind == "valve" && p.hid == st.valve) v = &p;
    if (!v || v->state == 0) continue;
    if (!st.on) {
      if (++st.t >= 0) SetSteam(ctx, base, st, true);
      continue;
    }
    if (++st.t > 2 * 250) {  // off by itself
      SetSteam(ctx, base, st, false);
      v->state = 0;
      continue;
    }
    // the puffs: along the jet, each its own way along, turning
    if (g_log && st.t % 60 == 1)
      for (const Prop& p : Cur().props)
        if (p.kind == "puff" && p.hid == st.pid && !p.recs.empty())
          if (const uint32_t e = Element(ctx, base, p.recs[0])) {
            const uint32_t o = Rd32(base + e + 100);
            REXLOG_INFO("hctp: {} elem at ({:.1f} {:.1f} {:.1f}) obj {:08X} shown {} active {} freeze {} draw {} {}",
                        p.name, RdF(base + e + 32), RdF(base + e + 36), RdF(base + e + 40), o,
                        o ? Rd32(base + o + 1124) : 0, o ? Rd32(base + o + 1120) : 0, o ? Rd32(base + o + 1140) : 0,
                        o ? Rd32(base + o + 476 + 16) : 0, o ? Rd32(base + o + 780 + 16) : 0);
          }
    int k = 0;
    for (Prop& p : Cur().props) {
      if (p.kind != "puff" || p.hid != st.pid) continue;
      const float u = std::fmod(float(st.t) / 40.0f + float(k) / 5.0f, 1.0f);
      const float d = u * st.len;
      for (int rec : p.recs)
        MoveRec(ctx, base, rec, st.x + st.dx * d, st.y - 1.5f * u, st.z + st.dz * d, 0,
                float(st.t) * 0.02f + float(k), float(st.t) * 0.07f + float(k) * 1.3f);
      ++k;
    }
    // burns
    for (int i = 0; i < 8; ++i) {
      const uint32_t f = Rd32(base + kChars + i * 4);
      if (!f) continue;
      FighterState& s = g_fs[i];
      if (s.burn_cd > 0) continue;
      const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
      const float rx = x - st.x, rz = z - st.z;
      const float along = rx * st.dx + rz * st.dz, across = std::fabs(-rx * st.dz + rz * st.dx);
      // (his body: from his feet y up to y - 18)
      if (along < 0 || along > st.len || across > st.hw + 2 || y < st.y - st.hh || y - 18.0f > st.y + st.hh) continue;
      JumpAct(ctx, base, f, kSteamBurn);
      s.burn_cd = 2 * 103;
      REXLOG_INFO("hctp: steam {} burns fighter {}", st.pid, i);
    }
  }
  for (FighterState& s : g_fs)
    if (s.burn_cd > 0) --s.burn_cd;
}

// -- The equipment crate (HCTP CGmShutObject 207): Irish-whip a wrestler
// into it and he is shut in (12034: attacker, victim and the crate on one
// spot); he climbs out at HCTP frame 420 (12043); ready again at 901.
constexpr uint32_t kWhipRun = 59;  // 2011: the whipped wrestler's run

void ShutIn(PPCContext& ctx, uint8_t* base, Prop& cr, uint32_t victim, int vi) {
  // the attacker: the nearest other wrestler
  uint32_t att = 0;
  float best = 1e9f;
  for (int i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + i * 4);
    if (!f || f == victim) continue;
    const float dx = RdF(base + f + 288) - RdF(base + victim + 288);
    const float dz = RdF(base + f + 296) - RdF(base + victim + 296);
    if (dx * dx + dz * dz < best) best = dx * dx + dz * dz, att = f;
  }
  if (!att || best > 60.0f * 60.0f) return;
  JumpAct(ctx, base, att, kCrateIn, 0, 0);
  JumpAct(ctx, base, victim, kCrateIn, 0, 1);
  PlaceFighter(base, att, cr.x, cr.y, cr.z, FighterYaw(cr.ry));
  PlaceFighter(base, victim, cr.x, cr.y, cr.z, FighterYaw(cr.ry));
  PlayProp(ctx, base, cr, kCrateIn, 0, 2);
  Cur().crate = 1, Cur().crate_t = 0, Cur().crate_victim = victim;
  REXLOG_INFO("hctp: fighter {} is shut in {}", vi, cr.name);
}

void UpdateCrate(PPCContext& ctx, uint8_t* base) {
  Prop* cr = FindKind("crate");
  if (!cr || Cur().crate == 0) return;
  const int t = ++Cur().crate_t;
  const uint32_t v = Cur().crate_victim;
  if (Cur().crate == 1) {
    // inside: he stays on the act's last pose until he climbs out
    const int n = Frames(kCrateIn, 170);
    if (t > 20 && (Rd32(base + v + 212) != kCrateIn || RdF(base + v + 24) >= float(n - 2))) {
      JumpAct(ctx, base, v, kCrateIn, 0, 1, float(n - 2));
      PlaceFighter(base, v, cr->x, cr->y, cr->z, FighterYaw(cr->ry));
    }
    if (t >= 2 * 420) {
      JumpAct(ctx, base, v, kCrateOut, 0, 0);
      PlaceFighter(base, v, cr->x, cr->y, cr->z, FighterYaw(cr->ry));
      PlayProp(ctx, base, *cr, kCrateOut, 0, 1);
      Cur().crate = 2;
      REXLOG_INFO("hctp: the crate opens - he climbs out");
    }
  } else if (t >= 2 * 901) {
    Cur().crate = 0;
  }
}

// -- Rooms 31-36 (re_rooms31_36.md) ---------------------------------------
// Props carry their HCTP tag params (p=), their record models in order (m=)
// and the HMD shapes the params name (h<id>=); object acts n play as
// ObjAct() + n, HCTP backstage acts 12000 + n as kRoomAct + n.

int RecOfModel(const Prop& p, int model) {
  for (size_t i = 0; i < p.models.size() && i < p.recs.size(); ++i)
    if (p.models[i] == model) return p.recs[i];
  return -1;
}

// One of a prop's models plays (key, x, y) / is shown or hidden.
void PlayModel(PPCContext& ctx, uint8_t* base, const Prop& p, int model, uint32_t key, uint32_t x = 0,
               uint32_t y = 0) {
  const int rec = RecOfModel(p, model);
  if (rec < 0) return;
  if (!HasKey(ctx, base, key, x, y)) {
    REXLOG_WARN("hctp: {}: no motion ({}, {}, {}) for model {}", p.name, key, x, y, model);
    return;
  }
  PlayRec(ctx, base, rec, key, x, y);
}

void ShowModel(PPCContext& ctx, uint8_t* base, const Prop& p, int model, bool show) {
  if (const int rec = RecOfModel(p, model); rec >= 0) ShowRec(ctx, base, rec, show);
}

// Later: a model shown / hidden some frames from now (HCTP's vanish frames).
struct LaterShow {
  int frames;
  size_t prop;
  int model;
  bool show;
};
std::vector<LaterShow> g_later;

void Later(const Prop& p, int hctp_frames, int model, bool show) {
  g_later.push_back({2 * hctp_frames, size_t(&p - Cur().props.data()), model, show});
}

void UpdateLater(PPCContext& ctx, uint8_t* base) {
  for (size_t k = 0; k < g_later.size();) {
    if (--g_later[k].frames > 0) {
      ++k;
      continue;
    }
    if (g_later[k].prop < Cur().props.size())
      ShowModel(ctx, base, Cur().props[g_later[k].prop], g_later[k].model, g_later[k].show);
    g_later.erase(g_later.begin() + std::ptrdiff_t(k));
  }
}

// Inside one of a prop's HMD shapes grown by m (y: from m above its top to its foot).
bool InShape(const Prop& p, int id, float x, float y, float z, float m) {
  const auto it = p.shapes.find(id);
  if (it == p.shapes.end()) return false;
  const auto& b = it->second;
  return x > b[0] - m && x < b[1] + m && z > b[4] - m && z < b[5] + m && y > b[2] - m && y < b[3] + m;
}

// The wrestler nearest to f (another one), and the squared distance.
uint32_t Nearest(uint8_t* base, uint32_t f, float* d2) {
  uint32_t best_f = 0;
  float best = 1e18f;
  for (int i = 0; i < 8; ++i) {
    const uint32_t o = Rd32(base + kChars + i * 4);
    if (!o || o == f) continue;
    const float dx = RdF(base + o + 288) - RdF(base + f + 288), dz = RdF(base + o + 296) - RdF(base + f + 296);
    if (dx * dx + dz * dz < best) best = dx * dx + dz * dz, best_f = o;
  }
  *d2 = best;
  return best_f;
}

// The hit-object reaction (HCTP 12014: variants 0-5) for a wrestler driven into a gimmick.
void HitReaction(PPCContext& ctx, uint8_t* base, uint32_t f) { JumpAct(ctx, base, f, kHitObject, uint32_t(RoomRand() % 6)); }

// CGmBrokenCar (310 / 318): models body whole, body dented, tyres, door, bonnet, windows (params 1-6),
// collision 7, door zone 8, bonnet zone 9, acts 10 bonnet hit / 11 door hit / 12 crush / 13 bump.
// Driven into the door zone: the door flies off; into the bonnet zone: the bonnet flies off, the body
// shows dented; then a wrestler going down on its roof crushes it (windows shatter, tyres fly).
void CarHit(PPCContext& ctx, uint8_t* base, Prop& c, uint32_t f, int i, float x, float y, float z, bool reacts,
            int force = 0) {
  if (c.models.size() < 6 || c.cooldown > 0) return;
  const int body0 = c.models[0], body1 = c.models[1], tyres = c.models[2], door = c.models[3],
            bonnet = c.models[4], windows = c.models[5];
  // (force: the test switch - 1 the door zone, 2 the bonnet zone)
  const bool in_door = force ? force == 1 : InShape(c, int(Param(c, 8)), x, y, z, 4.0f);
  const bool in_bonnet = force ? force == 2 : InShape(c, int(Param(c, 9)), x, y, z, 4.0f);
  if (c.bits & 4) {  // crushed: it rocks
    PlayModel(ctx, base, c, body1, ObjAct() + uint32_t(Param(c, 13)));
    if (reacts) HitReaction(ctx, base, f);
    c.cooldown = 2 * 60;
    return;
  }
  if (in_door && !(c.bits & 1)) {
    const uint32_t k = ObjAct() + uint32_t(Param(c, 11));
    PlayModel(ctx, base, c, c.body ? body1 : body0, k);
    PlayModel(ctx, base, c, windows, ObjAct() + 39);
    PlayModel(ctx, base, c, door, k, 1, 0);
    PlayModel(ctx, base, c, tyres, k);
    if (!(c.bits & 2)) PlayModel(ctx, base, c, bonnet, k);
    Later(c, 30, door, false);
    c.bits |= 1;
    REXLOG_INFO("hctp: fighter {} knocks {}'s door off", i, c.name);
  } else if (in_bonnet && !(c.bits & 2)) {
    const uint32_t k = ObjAct() + uint32_t(Param(c, 10));
    c.body = 1;
    ShowModel(ctx, base, c, body0, false);
    ShowModel(ctx, base, c, body1, true);
    PlayModel(ctx, base, c, body1, k);
    PlayModel(ctx, base, c, windows, ObjAct() + 38);
    PlayModel(ctx, base, c, bonnet, k, 1, 0);
    PlayModel(ctx, base, c, tyres, k);
    if (!(c.bits & 1)) PlayModel(ctx, base, c, door, k);
    Later(c, 30, bonnet, false);
    c.bits |= 2;
    REXLOG_INFO("hctp: fighter {} knocks {}'s bonnet off", i, c.name);
  } else {
    return;
  }
  if (reacts) HitReaction(ctx, base, f);
  c.cooldown = 2 * 60;
}

void CarCrush(PPCContext& ctx, uint8_t* base, Prop& c, int i) {
  if (c.models.size() < 6 || (c.bits & 3) != 3 || (c.bits & 4)) return;
  const uint32_t k = ObjAct() + uint32_t(Param(c, 12));
  PlayModel(ctx, base, c, c.models[1], k);
  PlayModel(ctx, base, c, c.models[2], k, 1, 0);
  Later(c, 30, c.models[2], false);
  Later(c, 30, c.models[5], false);  // (the glass shatters)
  c.bits |= 4;
  c.cooldown = 2 * 60;
  REXLOG_INFO("hctp: fighter {} crushes {}", i, c.name);
}

// CGmThrowObjectRepeat (341, the equipment box): driven into it, it jolts (param 3) and he reacts;
// again after its re-arm time (param 13) - its collision always stands.
void RepeatHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f, int i) {
  if (p.cooldown > 0) return;
  PlayProp(ctx, base, p, ObjAct() + uint32_t(Param(p, 3)));
  HitReaction(ctx, base, f);
  p.cooldown = 2 * int(Param(p, 13, 60)) + 2 * 60;
  REXLOG_INFO("hctp: fighter {} is driven into {}", i, p.name);
}

// CGmPowerSupply (311, the generator - room geometry): driven into its hit plane (param 1) he is
// electrocuted (param 2, HCTP 12079); again after param 13 frames.
void PowerHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f, int i) {
  if (p.cooldown > 0) return;
  JumpAct(ctx, base, f, kRoomAct + uint32_t(Param(p, 2, 12079)) - 12000);
  p.cooldown = 2 * int(Param(p, 13, 300));
  REXLOG_INFO("hctp: fighter {} is electrocuted by {}", i, p.name);
}

// gmHammerThrowAtk (317): an Irish whip into its box (the trailer side) - attacker and victim play
// the sync act param 7 (HCTP 12078) together.
void HammerThrow(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t victim, int vi) {
  if (p.cooldown > 0) return;
  float d2 = 0;
  const uint32_t att = Nearest(base, victim, &d2);
  if (!att || d2 > 80.0f * 80.0f) return;
  const uint32_t k = kRoomAct + uint32_t(Param(p, 7, 12078)) - 12000;
  JumpAct(ctx, base, att, k, 0, 0);
  JumpAct(ctx, base, victim, k, 0, 1);
  p.cooldown = 2 * 120;
  REXLOG_INFO("hctp: fighter {} is thrown into the trailer ({})", vi, p.name);
}

// CGmMoveCar (312): a background car driving the path 312 -> 313 -> 314 -> 315 (param 3-5 ids),
// the two car models (params 1, 2) taking turns, a wait of up to param 6 frames between runs.
void UpdateMoveCar(PPCContext& ctx, uint8_t* base) {
  for (Prop& c : Cur().props) {
    if (c.kind != "movecar" || c.models.size() < 2) continue;
    std::vector<std::array<float, 3>> path{{c.x, c.y, c.z}};
    for (size_t k = 3; k < 6; ++k)
      for (const Prop& q : Cur().props)
        if (q.hid == int(Param(c, k))) path.push_back({q.x, q.y, q.z});
    if (path.size() < 2) continue;
    // count: frames into the run (negative: waiting); bits: which model drives
    if (c.count < 0) {
      ++c.count;
      continue;
    }
    const float speed = 1.6f;  // (2011 units a frame)
    float t = float(c.count) * speed;
    size_t seg = 0;
    for (; seg + 1 < path.size(); ++seg) {
      const float dx = path[seg + 1][0] - path[seg][0], dz = path[seg + 1][2] - path[seg][2];
      const float len = std::sqrt(dx * dx + dz * dz);
      if (t <= len) break;
      t -= len;
    }
    const int model = c.models[size_t(c.bits & 1)];
    if (seg + 1 >= path.size()) {  // arrived: hide it, wait, the other car next
      ShowModel(ctx, base, c, model, false);
      c.bits ^= 1;
      c.count = -(60 + RoomRand() % std::max(1, 2 * int(Param(c, 6, 200))));
      continue;
    }
    if (c.count == 0) ShowModel(ctx, base, c, model, true);
    const auto& a = path[seg];
    const auto& b = path[seg + 1];
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float len = std::max(0.01f, std::sqrt(dx * dx + dz * dz));
    const float u = t / len;
    // (an object's front - its local +z - at yaw a points (-sin a, cos a): objects turn the other way to
    // fighters, whose yaw y faces (sin y, cos y))
    MoveRec(ctx, base, RecOfModel(c, model), a[0] + dx * u, a[1] + dy * u, a[2] + dz * u, 0, std::atan2(-dx, dz), 0);
    ++c.count;
  }
}

// Room 31's per-wrestler checks (from Fighters): m his motion now, prev last frame's, touched the
// attribute of the collision he touches.
bool Room31Fighter(PPCContext& ctx, uint8_t* base, uint32_t f, int i, uint32_t m, uint32_t prev, uint8_t touched,
                   float x, float y, float z) {
  // (lying: 0 or 1000 - backstage standing / walking is 20500)
  const bool walking = m == kWalkBackstage;
  const bool whipped = m == kWhipRun;
  const bool down = (m == 0 || m == kLying) && !(prev == 0 || prev == kLying);
  for (Prop& p : Cur().props) {
    if (p.kind == "car") {
      // driven into it (whipped, or not walking against its body), or down on its roof
      if (!walking && (whipped || touched == p.attr)) CarHit(ctx, base, p, f, i, x, y, z, true);
      if (down && y < -3.9f && InShape(p, int(Param(p, 7)), x, y, z, 2.0f)) CarCrush(ctx, base, p, i);
    } else if (p.kind == "repeat") {
      if (!walking && touched == p.attr) RepeatHit(ctx, base, p, f, i);
    } else if (p.kind == "power") {
      if (whipped && InShape(p, int(Param(p, 1)), x, y, z, 6.0f)) PowerHit(ctx, base, p, f, i);
    } else if (p.kind == "hammer") {
      if (whipped && InZone(p, x, y, z)) HammerThrow(ctx, base, p, f, i);
    }
  }
  return false;
}

// -- Room 32 (training room) -------------------------------------------------
// Nearly everything triggers on a wrestler whipped into it (HCTP attack mask 4): the victim in the
// whip run inside the object's hit shape (param named per class, grown by 6).

uint32_t RoomKey(float hctp_act) { return kRoomAct + uint32_t(hctp_act) - 12000; }

// On his feet for an action-button gimmick: walking (20500), or standing still on the ground - motion 0
// is standing as well as lying (a wrestler placed or idle since the bell).
bool Afoot(uint32_t m, float y) { return m == kWalkBackstage || (m == 0 && std::fabs(y) < 1.0f); }

// A victim held on a gimmick's act until 'until' (the act's last pose replayed, at the gimmick's place).
void HoldVictim(PPCContext& ctx, uint8_t* base, const Prop& p, uint32_t key) {
  const uint32_t v = p.who;
  if (!v) return;
  const int n = Frames(key, 60);
  if (Rd32(base + v + 212) != key || RdF(base + v + 24) >= float(n - 2)) JumpAct(ctx, base, v, key, 0, 0, float(n - 2));
  PlaceFighter(base, v, p.x, p.y, p.z, FighterYaw(p.ry));
}

// GmLocker (400): whipped in, he is shut inside (door 69 / body 70, him 12071) for 120 HCTP frames, then
// comes out (door 71 / body 72, him 12072) and the door falls off (no 2011 locker-door weapon: it's hidden).
void LockerHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t v, int i) {
  if (p.state || p.models.size() < 2) return;
  p.state = 1, p.timer = 0, p.who = v;
  PlayModel(ctx, base, p, p.models[1], ObjAct() + uint32_t(Param(p, 4)));
  PlayModel(ctx, base, p, p.models[0], ObjAct() + uint32_t(Param(p, 3)));
  JumpAct(ctx, base, v, RoomKey(Param(p, 2)));
  PlaceFighter(base, v, p.x, p.y, p.z, FighterYaw(p.ry));
  REXLOG_INFO("hctp: fighter {} is shut in {}", i, p.name);
}

void UpdateLocker(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state == 0 || p.state == 3) return;
  ++p.timer;
  if (p.state == 1) {
    if (p.timer > 20) HoldVictim(ctx, base, p, RoomKey(Param(p, 2)));
    if (p.timer >= 2 * (Frames(ObjAct() + uint32_t(Param(p, 4)), 37) + 120)) {
      PlayModel(ctx, base, p, p.models[1], ObjAct() + uint32_t(Param(p, 9)));
      PlayModel(ctx, base, p, p.models[0], ObjAct() + uint32_t(Param(p, 8)));
      JumpAct(ctx, base, p.who, RoomKey(Param(p, 7)));
      PlaceFighter(base, p.who, p.x, p.y, p.z, FighterYaw(p.ry));
      p.state = 2, p.timer = 0;
      REXLOG_INFO("hctp: {} opens - he comes out", p.name);
    }
  } else if (p.timer >= 2 * Frames(ObjAct() + uint32_t(Param(p, 9)), 35)) {
    ShowModel(ctx, base, p, p.models[1], false);  // (HCTP: the door falls off as weapon 55)
    p.state = 3;
  }
}

// gmToilet (405): whipped in, he sits in the stall (12076) and comes out (12077); the door (procedural in
// HCTP) is out of the way meanwhile. Repeatable.
void ToiletHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t v, int i) {
  if (p.state || p.cooldown > 0) return;
  p.state = 1, p.timer = 0, p.who = v;
  if (!p.models.empty()) ShowModel(ctx, base, p, p.models[0], false);
  JumpAct(ctx, base, v, RoomKey(Param(p, 5)));
  PlaceFighter(base, v, p.x, p.y, p.z, FighterYaw(p.ry));
  REXLOG_INFO("hctp: fighter {} is thrown into {}", i, p.name);
}

void UpdateToilet(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state != 1) return;
  ++p.timer;
  // (HCTP: door frames 101..253 he's held in the stall)
  if (p.timer > 2 * 101 && p.timer < 2 * 254) HoldVictim(ctx, base, p, RoomKey(Param(p, 5)));
  if (p.timer == 2 * 254) {
    JumpAct(ctx, base, p.who, RoomKey(Param(p, 7)));
    PlaceFighter(base, p.who, p.x, p.y, p.z, FighterYaw(p.ry));
    if (!p.models.empty()) ShowModel(ctx, base, p, p.models[0], true);
    p.state = 0, p.cooldown = 2 * 60;
  }
}

// gmShower (420): whipped in, door and glass swing (92), he stands under the water (12086). 300 frames.
void ShowerHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t v, int i) {
  if (p.cooldown > 0) return;
  for (int m : p.models) PlayModel(ctx, base, p, m, ObjAct() + uint32_t(Param(p, 7)));
  JumpAct(ctx, base, v, RoomKey(Param(p, 1)));
  PlaceFighter(base, v, p.x, p.y, p.z, 0.0f);
  p.cooldown = 2 * 300;
  REXLOG_INFO("hctp: fighter {} is thrown into {}", i, p.name);
}

// gmRackWithObj (440 / 445 / 467): whipped in, the shelf shakes and he reacts; again after 60 frames.
// The items on it (gmAttackObjFade 460, gmAttackObjWeapon 465 / 466 / 468) stand at the same place: the
// same whip knocks them down - one fades out, the others become weapons.
void RackHit(PPCContext& ctx, uint8_t* base, Prop& r, uint32_t v, int i) {
  if (r.cooldown > 0) return;
  PlayProp(ctx, base, r, ObjAct() + uint32_t(Param(r, 1)));
  HitReaction(ctx, base, v);
  r.cooldown = 2 * 60;
  for (Prop& q : Cur().props)
    if ((q.kind == "fade" || q.kind == "rackweapon") && q.state == 0 && std::fabs(q.x - r.x) < 3.0f &&
        std::fabs(q.z - r.z) < 3.0f) {
      PlayProp(ctx, base, q, ObjAct() + uint32_t(Param(q, 3)));
      q.state = 1, q.timer = 0;
    }
  REXLOG_INFO("hctp: fighter {} is thrown into {}", i, r.name);
}

void UpdateShelfItem(PPCContext& ctx, uint8_t* base, Prop& q) {
  if (q.state != 1) return;
  ++q.timer;
  const int n = Frames(ObjAct() + uint32_t(Param(q, 3)), 30);
  if (q.kind == "fade" && q.timer == 2 * (n + int(Param(q, 5, 30)))) ShowProp(ctx, base, q, false), q.state = 2;
  if (q.kind == "rackweapon" && q.timer == 2 * n) {
    ShowProp(ctx, base, q, false);
    for (const WeaponSpot& w : Cur().weapons)
      if (w.hid == q.hid && std::fabs(w.x - q.x) < 20.0f && std::fabs(w.z - q.z) < 20.0f)
        SpawnWeapon(ctx, base, w.model, w.x, w.y, w.z);
    q.state = 2;
  }
}

// gmTVClash (435): whipped into the stand, or the wall behind it broken: TV 80 / stand 81 fall, then the
// TV is gone (HCTP: weapon 53, which 2011 lacks) and the stand fades; its collision goes.
void TVFall(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t v) {
  if (p.state || p.models.size() < 2) return;
  PlayModel(ctx, base, p, p.models[0], ObjAct() + uint32_t(Param(p, 2)));
  PlayModel(ctx, base, p, p.models[1], ObjAct() + uint32_t(Param(p, 3)));
  if (v) HitReaction(ctx, base, v);
  DropCollision(base, p);
  p.state = 1, p.timer = 0;
  REXLOG_INFO("hctp: {} falls", p.name);
}

void UpdateTV(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state != 1) return;
  ++p.timer;
  const int n = Frames(ObjAct() + uint32_t(Param(p, 3)), 45);
  if (p.timer == 2 * n) ShowModel(ctx, base, p, p.models[0], false);
  if (p.timer == 2 * (n + 31)) ShowModel(ctx, base, p, p.models[1], false), p.state = 2;
}

// gmBrokenWall (430): models whole, broken, falling piece, cracked. The first whip / running hit cracks it,
// the second breaks it open (83 on the broken wall, (83, 1) on the piece) and knocks the TV over; both
// fade and the wall's collision goes - the passage is open.
void WallHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t v, int i) {
  if (p.cooldown > 0 || p.state >= 2 || p.models.size() < 4) return;
  if (v) HitReaction(ctx, base, v);
  p.cooldown = 2 * 60;
  if (p.state == 0) {
    ShowModel(ctx, base, p, p.models[0], false);
    ShowModel(ctx, base, p, p.models[3], true);
    p.state = 1;
    REXLOG_INFO("hctp: fighter {} cracks {}", i, p.name);
    return;
  }
  ShowModel(ctx, base, p, p.models[3], false);
  ShowModel(ctx, base, p, p.models[1], true);
  ShowModel(ctx, base, p, p.models[2], true);
  const uint32_t k = ObjAct() + uint32_t(Param(p, 6));
  PlayModel(ctx, base, p, p.models[1], k);
  PlayModel(ctx, base, p, p.models[2], k, 1, 0);
  p.state = 2, p.timer = 0;
  for (Prop& t : Cur().props)
    if (t.kind == "tv") TVFall(ctx, base, t, 0);
  REXLOG_INFO("hctp: fighter {} breaks {} open", i, p.name);
}

void UpdateBrokenWall(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state != 2) return;
  ++p.timer;
  if (p.timer == 2 * 64) DropCollision(base, p);
  if (p.timer == 2 * (64 + 80)) {
    ShowModel(ctx, base, p, p.models[1], false);
    ShowModel(ctx, base, p, p.models[2], false);
    p.state = 3;
  }
}

// gmSandBag (415): hanging from the ceiling; a hit pushes it along the attacker's facing (strike p3,
// whipped into it p5), two pendulums swing it (decay 0.998, +3 degrees a frame), a hard swing (> 18)
// knocks standing wrestlers near its foot down (340).
struct Swing {
  float ax = 0, az = 0, phase = 0;
};
Swing g_bag;

void SandbagHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f, float push) {
  if (p.cooldown > 2 * 50) return;
  const float yaw = RdF(base + f + 276);
  g_bag.ax += push * 4.0f * std::sin(yaw);
  g_bag.az += push * 4.0f * std::cos(yaw);
  g_bag.ax = std::clamp(g_bag.ax, -28.0f, 28.0f), g_bag.az = std::clamp(g_bag.az, -28.0f, 28.0f);
  p.cooldown = 2 * 60;
}

void UpdateSandbag(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.recs.empty()) return;
  g_bag.ax *= 0.998f, g_bag.az *= 0.998f;
  g_bag.phase += 3.0f * 3.14159265f / 180.0f / 2.0f;
  const float s = std::sin(g_bag.phase);
  constexpr float kRad = 3.14159265f / 180.0f;
  MoveRec(ctx, base, p.recs[0], p.x, p.y, p.z, g_bag.az * s * kRad, p.ry, -g_bag.ax * s * kRad);
  if (std::fabs(g_bag.ax) + std::fabs(g_bag.az) <= 18.0f || p.cooldown > 2 * 50) return;
  // (its foot: the bag hangs 50 down from its pivot - y -50 is its top)
  for (int i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + i * 4);
    if (!f || Rd32(base + f + 212) != kWalkBackstage) continue;
    const float dx = RdF(base + f + 288) - (p.x + g_bag.az * s * 0.6f), dz = RdF(base + f + 296) - (p.z - g_bag.ax * s * 0.6f);
    if (dx * dx + dz * dz < 6.0f * 6.0f) {
      JumpAct(ctx, base, f, kHitByObject, uint32_t(RoomRand() & 1));
      REXLOG_INFO("hctp: the sandbag knocks fighter {} down", i);
    }
  }
}

// gmTrngMch (425): the action button (X) next to it puts him on the treadmill (12073, the belt 75); the
// second button (A) mashed raises the speed (levels 0-2: him 12073 / 12074 / 12075, belt 75 / 76 / 77);
// no press for 121 frames lowers it, at level 0 he steps off; X again steps off.
constexpr uint32_t kButtonA = 0x40;  // (input +52)

void TreadmillOn(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f, int i) {
  p.who = f, p.state = 1, p.count = 0, p.timer = 0;
  JumpAct(ctx, base, f, RoomKey(12073));
  PlaceFighter(base, f, p.x, p.y, p.z, FighterYaw(p.ry, -1.5707963f));
  PlayProp(ctx, base, p, ObjAct() + uint32_t(Param(p, 6)));
  REXLOG_INFO("hctp: fighter {} gets on {}", i, p.name);
}

void TreadmillOff(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.who) JumpAct(ctx, base, p.who, RoomKey(12059));
  p.who = 0, p.state = 0, p.cooldown = 2 * 30;
}

void UpdateTreadmill(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state != 1 || !p.who) return;
  const uint32_t f = p.who, m = Rd32(base + f + 212);
  const uint32_t want = RoomKey(12073.0f + float(p.count));
  if (m != RoomKey(12073) && m != RoomKey(12074) && m != RoomKey(12075)) {  // knocked off
    p.who = 0, p.state = 0;
    return;
  }
  PlaceFighter(base, f, p.x, p.y, p.z, FighterYaw(p.ry, -1.5707963f));
  const uint32_t input = svr2011::FighterInput(f);
  const uint32_t pressed = input ? Rd32(base + input + 52) : 0;
  ++p.timer;
  if (pressed & kButtonX) {
    TreadmillOff(ctx, base, p);
    return;
  }
  int lv = p.count;
  if (pressed & kButtonA) {
    lv = std::min(2, lv + (p.timer < 2 * 10 ? 1 : 0));
    p.timer = 0;
  } else if (p.timer > 2 * 121) {
    if (lv == 0) {
      TreadmillOff(ctx, base, p);
      return;
    }
    --lv, p.timer = 0;
  }
  if (lv != p.count || (m != want && RdF(base + f + 24) >= float(Frames(want, 60) - 1))) {
    p.count = lv;
    JumpAct(ctx, base, f, RoomKey(12073.0f + float(lv)));
    PlayProp(ctx, base, p, ObjAct() + uint32_t(Param(p, size_t(6 + lv))));
  }
}

bool Room32Fighter(PPCContext& ctx, uint8_t* base, uint32_t f, int i, uint32_t m, uint32_t prev, uint8_t touched,
                   float x, float y, float z, bool x_pressed) {
  const bool whipped = m == kWhipRun;
  const bool walking = m == kWalkBackstage;
  for (Prop& p : Cur().props) {
    const auto hit = [&](size_t shape_param, float grow) {
      return InShape(p, int(Param(p, shape_param)), x, y, z, grow);
    };
    if (p.kind == "locker" && whipped && hit(6, 16.0f)) LockerHit(ctx, base, p, f, i);
    if (p.kind == "toilet" && whipped && hit(1, 8.0f)) ToiletHit(ctx, base, p, f, i);
    if (p.kind == "shower" && whipped && hit(4, 8.0f)) ShowerHit(ctx, base, p, f, i);
    if (p.kind == "rack" && whipped && hit(3, 8.0f)) RackHit(ctx, base, p, f, i);
    if (p.kind == "tv" && !walking && (whipped || touched == p.attr)) TVFall(ctx, base, p, f);
    if (p.kind == "brokenwall" && !walking && (whipped || base[f + 1286] || touched == p.attr) &&
        (touched == p.attr || hit(2, 6.0f)))
      WallHit(ctx, base, p, f, i);
    if (p.kind == "sandbag") {
      const float dx = x - p.x, dz = z - p.z;
      if (dx * dx + dz * dz < 12.0f * 12.0f && !walking && m != prev && m != 0 && m != kLying)
        SandbagHit(ctx, base, p, f, whipped ? Param(p, 5, 1.5f) : Param(p, 3, 2.0f));
    }
    if (p.kind == "treadmill" && p.state == 0 && p.cooldown == 0 && x_pressed && Afoot(prev, y)) {
      const float dx = x - p.x, dz = z - p.z;
      if (dx * dx + dz * dz < Param(p, 2, 6.0f) * Param(p, 2, 6.0f) * 4.0f) {
        TreadmillOn(ctx, base, p, f, i);
        return true;
      }
    }
  }
  return false;
}

void UpdateLion(PPCContext& ctx, uint8_t* base, Prop& p);

void UpdateRoom32(PPCContext& ctx, uint8_t* base) {
  for (Prop& p : Cur().props) {
    if (p.kind == "locker") UpdateLocker(ctx, base, p);
    if (p.kind == "toilet") UpdateToilet(ctx, base, p);
    if (p.kind == "fade" || p.kind == "rackweapon") UpdateShelfItem(ctx, base, p);
    if (p.kind == "tv") UpdateTV(ctx, base, p);
    if (p.kind == "brokenwall") UpdateBrokenWall(ctx, base, p);
    if (p.kind == "sandbag") UpdateSandbag(ctx, base, p);
    if (p.kind == "treadmill") UpdateTreadmill(ctx, base, p);
    if (p.kind == "lion") UpdateLion(ctx, base, p);
  }
}

// -- Rooms 35 / 36 (outside arena B) ----------------------------------------
// (HCTP's flood and tower collapse are dead code there - not ported.)

// CGmRion (700 / 701), the lion statues: running straight at one of its three climb areas (params 2 / 4 / 6)
// climbs it (acts params 3 / 5 / 7 = 12006 / 12007 / 12005) onto its top (12008 / 12011 / 12012); there the
// action button (X), or 601 HCTP frames, dives off (HCTP 3550 - here the jump down, 12505).
bool OnLion(PPCContext& ctx, uint8_t* base, uint32_t m) {
  return m == RoomKey(12005) || m == RoomKey(12006) || m == RoomKey(12007) || m == RoomKey(12008) ||
         m == RoomKey(12011) || m == RoomKey(12012);
}

void LionClimb(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f, int i, float x, float y, float z) {
  if (p.who) return;
  const float yaw = RdF(base + f + 276), fx = std::sin(yaw), fz = std::cos(yaw);
  const float dx = p.x - x, dz = p.z - z, len = std::max(0.01f, std::sqrt(dx * dx + dz * dz));
  if ((dx * fx + dz * fz) / len < 0.8f) return;  // (HCTP: straight at the face, within 6 degrees)
  for (size_t k : {2u, 4u, 6u}) {
    if (!InShape(p, int(Param(p, k)), x, y, z, 3.0f)) continue;
    const float to = std::atan2(dx, dz);
    PlaceFighter(base, f, x, y, z, to);
    WrF(base + f + 276, to);
    JumpAct(ctx, base, f, RoomKey(Param(p, k + 1)));
    p.who = f, p.timer = 0;
    REXLOG_INFO("hctp: fighter {} climbs {}", i, p.name);
    return;
  }
}

void UpdateLion(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (!p.who) return;
  const uint32_t f = p.who, m = Rd32(base + f + 212);
  if (!OnLion(ctx, base, m)) {  // (he left it some other way)
    p.who = 0;
    return;
  }
  const bool on_top = m == RoomKey(12008) || m == RoomKey(12011) || m == RoomKey(12012);
  if (!on_top) return;
  ++p.timer;
  const uint32_t input = svr2011::FighterInput(f);
  if ((input && (Rd32(base + input + 52) & kButtonX)) || p.timer >= 2 * 601) {
    JumpAct(ctx, base, f, kJumpDown);
    p.who = 0;
    REXLOG_INFO("hctp: he dives off {}", p.name);
  }
}

// GmFountainThrow (710 / 711, room 35): an Irish whip next to a fountain (radius sqrt 600 of its centre) -
// attacker and victim play 12027 together: thrown into the water.
void FountainThrow(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t victim, int vi) {
  if (p.cooldown > 0) return;
  float d2 = 0;
  const uint32_t att = Nearest(base, victim, &d2);
  if (!att || d2 > 60.0f * 60.0f) return;
  JumpAct(ctx, base, att, RoomKey(12027), 0, 0);
  JumpAct(ctx, base, victim, RoomKey(12027), 0, 1);
  p.cooldown = 2 * 140;
  REXLOG_INFO("hctp: fighter {} is thrown into the fountain", vi);
}

void Room35Fighter(PPCContext& ctx, uint8_t* base, uint32_t f, int i, uint32_t m, float x, float y, float z) {
  const bool running = base[f + 1286] != 0;
  for (Prop& p : Cur().props) {
    if (p.kind == "lion" && running && m != kWhipRun) LionClimb(ctx, base, p, f, i, x, y, z);
    if (p.kind == "fountainthrow" && m == kWhipRun) {
      const float cx = (p.zone[0] + p.zone[1]) / 2, cz = (p.zone[4] + p.zone[5]) / 2;
      if (p.has_zone && (x - cx) * (x - cx) + (z - cz) * (z - cz) <= 600.0f && std::fabs(y) < 2.0f)
        FountainThrow(ctx, base, p, f, i);
    }
  }
}

// -- Room 33 (Times Square) -------------------------------------------------
// HCTP's high-ground acts 12500-12599 are converted as 12800 + (n - 12500).
uint32_t HighKey(int hctp_act) { return 12800u + uint32_t(hctp_act - 12500); }

// GmCarMany / GmCarRun: a taxi drives its lane from its placement to its anchor (param 1) and starts again
// after a delay (param 4 at first); it brakes for a wrestler in front of it, runs over one it reaches at
// speed (12014), and blows up when a wrestler goes down on its roof (everyone near: 12015), coming back after.
struct Taxi {
  float speed = 0, top = 1.0f, along = 0;
  int wait = 0;  // frames before it (re)starts
  bool exploded = false;
};
std::map<size_t, Taxi> g_taxis;

void UpdateTraffic(PPCContext& ctx, uint8_t* base) {
  for (size_t k = 0; k < Cur().props.size(); ++k) {
    Prop& c = Cur().props[k];
    if (c.kind != "traffic" || c.recs.empty()) continue;
    const Prop* end = nullptr;
    for (const Prop& q : Cur().props)
      if (q.hid == int(Param(c, 1))) end = &q;
    if (!end) continue;
    auto [it, fresh] = g_taxis.try_emplace(k);
    Taxi& t = it->second;
    if (fresh) t.wait = 2 * int(Param(c, 4, 300)), t.top = 0.8f + float(RoomRand() % 64) / 100.0f;
    const float dx = end->x - c.x, dz = end->z - c.z, len = std::max(1.0f, std::sqrt(dx * dx + dz * dz));
    const float ux = dx / len, uz = dz / len;
    if (t.wait > 0) {
      if (--t.wait == 0) ShowProp(ctx, base, c, true), t.along = 0, t.speed = 0, t.exploded = false;
      continue;
    }
    const float px = c.x + ux * t.along, pz = c.z + uz * t.along;
    // brake for a wrestler 15..100 ahead in its lane, else speed up (HCTP per-frame speeds halved: 60 Hz)
    bool blocked = false;
    for (int i = 0; i < 8; ++i) {
      const uint32_t f = Rd32(base + kChars + i * 4);
      if (!f) continue;
      const float wx = RdF(base + f + 288) - px, wz = RdF(base + f + 296) - pz, wy = RdF(base + f + 292);
      const float ahead = wx * ux + wz * uz, side = std::fabs(-wx * uz + wz * ux);
      if (wy < -1.0f) {
        // on its roof: going down there blows it up
        const uint32_t m = Rd32(base + f + 212);
        if (wy > -25.0f && wy < -5.0f && std::fabs(ahead) < 25.0f && side < 12.0f && (m == 0 || m == kLying)) {
          t.exploded = true;
          ShowProp(ctx, base, c, false);
          for (int j = 0; j < 8; ++j)
            if (const uint32_t o = Rd32(base + kChars + j * 4); o && o != f) {
              const float ox = RdF(base + o + 288) - px, oz = RdF(base + o + 296) - pz;
              if (ox * ox + oz * oz < 70.0f * 70.0f) JumpAct(ctx, base, o, RoomKey(12015));
            }
          t.wait = 2 * (200 + RoomRand() % 100);
          REXLOG_INFO("hctp: fighter {} blows {} up", i, c.name);
        }
        continue;
      }
      if (side < 12.0f && ahead > 10.0f && ahead < 100.0f) blocked = true;
      // run over (only at speed)
      if (side < 9.0f && std::fabs(ahead) < 22.0f && t.speed > t.top / 3.0f) {
        const uint32_t m = Rd32(base + f + 212);
        if (m == kWalkBackstage || base[f + 1286]) {
          HitReaction(ctx, base, f);
          t.speed = 0;
          REXLOG_INFO("hctp: {} runs fighter {} over", c.name, i);
        }
      }
    }
    if (t.exploded) continue;
    t.speed = blocked ? std::max(0.0f, t.speed - 0.05f) : std::min(t.top / 2.0f, t.speed + 0.01f);
    t.along += t.speed;
    if (t.along >= len) {  // at its anchor: back to the start after a delay
      ShowProp(ctx, base, c, false);
      t.wait = 2 * (200 + RoomRand() % 100);
      continue;
    }
    for (int rec : c.recs) MoveRec(ctx, base, rec, c.x + ux * t.along, c.y, c.z + uz * t.along, 0, c.ry, 0);
  }
}

// GmTimesPole: X at the foot of one of the four poles (within 10) climbs it (12009); on top X jumps down (12510).
struct Pole {
  float x, y, z;
  uint32_t who = 0;
  int timer = 0;
};
std::vector<Pole> g_poles;

bool PoleClimb(PPCContext& ctx, uint8_t* base, uint32_t f, int i, float x, float y, float z) {
  for (Pole& p : g_poles) {
    if (p.who) continue;
    const float dx = p.x - x, dz = p.z - z;
    if (std::fabs(y) > 0.5f || dx * dx + dz * dz > 10.0f * 10.0f) continue;
    WrF(base + f + 276, std::atan2(dx, dz));
    JumpAct(ctx, base, f, RoomKey(12009));
    p.who = f, p.timer = 0;
    REXLOG_INFO("hctp: fighter {} climbs a pole", i);
    return true;
  }
  return false;
}

void UpdatePoles(PPCContext& ctx, uint8_t* base) {
  for (Pole& p : g_poles) {
    if (!p.who) continue;
    const uint32_t f = p.who;
    if (Rd32(base + f + 212) != RoomKey(12009)) {
      p.who = 0;
      continue;
    }
    ++p.timer;
    const uint32_t input = svr2011::FighterInput(f);
    if (p.timer > 2 * 30 && ((input && (Rd32(base + input + 52) & kButtonX)) || p.timer >= 2 * 600)) {
      JumpAct(ctx, base, f, HighKey(12510));
      p.who = 0;
      REXLOG_INFO("hctp: he jumps off the pole");
    }
  }
}

// GmShowWindow (550): whipped into it, the pane breaks (glass02, param 4, shown from then on) and he reacts.
void WindowHit(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t v, int i) {
  if (p.state) return;
  ShowProp(ctx, base, p, true);
  HitReaction(ctx, base, v);
  p.state = 1;
  REXLOG_INFO("hctp: fighter {} goes through {}", i, p.name);
}

bool HeliGrab(PPCContext& ctx, uint8_t* base, uint32_t f, int i, float x, float z);

void Room33Fighter(PPCContext& ctx, uint8_t* base, uint32_t f, int i, uint32_t m, uint32_t prev, uint8_t touched,
                   float x, float y, float z, bool x_pressed) {
  for (Prop& p : Cur().props)
    if (p.kind == "window" && m == kWhipRun && InShape(p, int(Param(p, 5)), x, y, z, 6.0f)) WindowHit(ctx, base, p, f, i);
  if (x_pressed && Afoot(prev, y) && !Cur().heli.empty() && HeliGrab(ctx, base, f, i, x, z)) return;
  if (x_pressed && Afoot(prev, y) && !g_poles.empty()) PoleClimb(ctx, base, f, i, x, y, z);
  // the ladder (attribute 0xE0) and the wall up to the top level (0xE6): walking / running into them climbs
  const bool moving = m == kWalkBackstage || base[f + 1286];
  if (moving && touched == 0xE0 && m != RoomKey(12069)) {
    bool busy = false;
    for (int j = 0; j < 8; ++j)
      if (const uint32_t o = Rd32(base + kChars + j * 4); o && o != f && Rd32(base + o + 212) == RoomKey(12069)) busy = true;
    if (!busy) {
      JumpAct(ctx, base, f, RoomKey(12069));
      REXLOG_INFO("hctp: fighter {} climbs the ladder", i);
    }
  }
  if (moving && touched == 0xE6) {
    JumpAct(ctx, base, f, HighKey(12517));
    REXLOG_INFO("hctp: fighter {} climbs up", i);
  }
}

// GmHeli (512): flies its path (its act-10 root keys, interpolated); X under it grabs the rope ladder
// (12000, then hanging 12001) - it climbs to ride height and the stick steers it within 60 of its path;
// X lets go (12503), Y drops (12502); hit off the ladder: 12503.
struct Heli {
  uint32_t rider = 0;
  float ox = 0, oz = 0, vx = 0, vz = 0, lift = 0;
  int t = 0;
};
Heli g_heli;

bool HeliPos(float* x, float* y, float* z, float* yaw) {
  const auto& K = Cur().heli;
  if (K.size() < 2) return false;
  const float len = K.back()[0];
  const float f = std::fmod(float(Cur().heli_t) / 2.0f, std::max(1.0f, len));
  size_t k = 0;
  while (k + 2 < K.size() && K[k + 1][0] <= f) ++k;
  const float span = std::max(1.0f, K[k + 1][0] - K[k][0]), u = std::clamp((f - K[k][0]) / span, 0.0f, 1.0f);
  *x = K[k][1] + (K[k + 1][1] - K[k][1]) * u + g_heli.ox;
  *y = K[k][2] + (K[k + 1][2] - K[k][2]) * u + g_heli.lift;
  *z = K[k][3] + (K[k + 1][3] - K[k][3]) * u + g_heli.oz;
  *yaw = K[k][4] + std::remainder(K[k + 1][4] - K[k][4], 6.2831853f) * u;
  return true;
}

bool HeliGrab(PPCContext& ctx, uint8_t* base, uint32_t f, int i, float x, float z) {
  float hx, hy, hz, hyaw;
  if (g_heli.rider || !HeliPos(&hx, &hy, &hz, &hyaw)) return false;
  if (g_log) REXLOG_INFO("hctp: fighter {} X at ({:.0f} {:.0f}), the helicopter at ({:.0f} {:.0f} {:.0f})", i, x, z, hx, hy, hz);
  // (=props test: the scripted pad's fighter grabs it from anywhere)
  if ((x - hx) * (x - hx) + (z - hz) * (z - hz) > 120.0f && !(g_props_now && i == 0)) return false;
  JumpAct(ctx, base, f, RoomKey(12000));
  g_heli.rider = f, g_heli.t = 0;
  REXLOG_INFO("hctp: fighter {} grabs the helicopter's ladder", i);
  return true;
}

void UpdateHeli(PPCContext& ctx, uint8_t* base) {
  Prop* h = FindKind("heli");
  if (!h || Cur().heli.empty() || Cur().set_up_frames < 30) return;
  ++Cur().heli_t;
  const uint32_t f = g_heli.rider;
  if (f) {
    const uint32_t m = Rd32(base + f + 212);
    const uint32_t input = svr2011::FighterInput(f);
    const uint32_t pressed = input ? Rd32(base + input + 52) : 0;
    const int32_t stick = input ? int32_t(Rd32(base + input + 56)) : -1;
    ++g_heli.t;
    g_heli.lift = std::max(-50.0f, g_heli.lift - 0.15f);  // (up to HCTP's ride height, -110)
    if (stick >= 0 && stick < 8) {
      const float a = float(stick - 4) * 0.785398f;
      g_heli.vx = std::clamp(g_heli.vx - std::sin(a) * 0.025f, -2.0f, 2.0f);
      g_heli.vz = std::clamp(g_heli.vz - std::cos(a) * 0.025f, -1.0f, 1.0f);
    } else {
      g_heli.vx *= 0.97f, g_heli.vz *= 0.97f;
    }
    g_heli.ox = std::clamp(g_heli.ox + g_heli.vx, -60.0f, 60.0f);
    g_heli.oz = std::clamp(g_heli.oz + g_heli.vz, -60.0f, 60.0f);
    const bool hanging = m == RoomKey(12000) || m == RoomKey(12001);
    uint32_t drop = 0;
    if (!hanging) drop = HighKey(12503);
    else if ((pressed & kButtonX) && g_heli.t > 2 * 20) drop = HighKey(12503);
    else if ((pressed & kButtonY) && g_heli.t > 2 * 20) drop = HighKey(12502);
    if (drop) {
      JumpAct(ctx, base, f, drop);
      g_heli.rider = 0;
      REXLOG_INFO("hctp: he lets go of the helicopter");
    } else {
      if (m == RoomKey(12000) ? g_heli.t >= 2 * Frames(RoomKey(12000), 40) : RdF(base + f + 24) >= float(Frames(m, 60) - 1))
        JumpAct(ctx, base, f, RoomKey(12001));
    }
  } else {
    g_heli.lift = std::min(0.0f, g_heli.lift + 0.15f);
    g_heli.ox *= 0.99f, g_heli.oz *= 0.99f;
  }
  float x, y, z, yaw;
  if (!HeliPos(&x, &y, &z, &yaw)) return;
  for (int rec : h->recs) MoveRec(ctx, base, rec, x, y, z, 0, yaw, 0);
  if (g_heli.rider) PlaceFighter(base, g_heli.rider, x, y + 38.0f, z + 4.0f, yaw);
}

void UpdateRoom33(PPCContext& ctx, uint8_t* base) {
  UpdateTraffic(ctx, base);
  UpdatePoles(ctx, base);
}

// -- The forklift (room 31, CVehicleForkLift 320 + CForkliftBoxSupply 321) ------------------------
// X next to either side gets on (12080 / 12081), seated 12085. Held buttons: A accelerates, R1 brakes,
// Y reverses; the stick steers (12082 / 12083 while turning). B lifts the crate - it tips off the forks
// (86) onto whoever is in front (340). Driving into the reload zone (321) puts a new crate on (85).
// X gets off (12084); knocked off: 12091. (input +48: buttons held, +52: pressed this frame; +56: the
// stick's 8-way direction, -1 none - logged by the test to check)

// heading h: the way it drives (and its forks point), (-sin h, -cos h) - 0 toward -z, as the stick's
// directions; its model's yaw is pi - h (an object's local +z at yaw a points (-sin a, cos a)), the
// driver's h + pi (a fighter's yaw y faces (sin y, cos y)). (Driving with the model at yaw h turned it
// the mirrored way: the driver ended up facing away from the forks.)
struct Forklift {
  uint32_t rider = 0;
  int state = 0;  // 0 free, 1 getting on, 2 driving, 3 getting off
  int t = 0;
  float x = 0, z = 0, heading = 0, speed = 0;
  bool moved = false;
  int crate = 0, crate_t = 0;  // 0 on the forks, 1 lifting / tipping, 2 gone
};
Forklift g_fork;

void ForkliftPlace(PPCContext& ctx, uint8_t* base, Prop& p) {
  // (its static collision out once it moves: a solid box would stay at its old place)
  if (!g_fork.moved) DropCollision(base, p), g_fork.moved = true;
  for (size_t k = 0; k < p.recs.size(); ++k)
    if (!(k == 1 && g_fork.crate == 2)) MoveRec(ctx, base, p.recs[k], g_fork.x, p.y, g_fork.z, 0, 3.14159265f - g_fork.heading, 0);
}

bool ForkliftGetOn(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f, int i, float x, float z) {
  if (g_fork.rider || g_fork.state) return false;
  if (g_fork.x == 0 && g_fork.z == 0) g_fork.x = p.x, g_fork.z = p.z, g_fork.heading = 3.14159265f - p.ry;
  for (int side = 0; side < 2; ++side) {
    const float s = side ? -10.0f : 10.0f;
    const float sx = g_fork.x + s * std::cos(g_fork.heading), sz = g_fork.z - s * std::sin(g_fork.heading);
    if ((x - sx) * (x - sx) + (z - sz) * (z - sz) > 10.0f * 10.0f) continue;
    g_fork.rider = f, g_fork.state = 1, g_fork.t = 0;
    JumpAct(ctx, base, f, RoomKey(side ? 12081 : 12080));
    REXLOG_INFO("hctp: fighter {} gets on {}", i, p.name);
    return true;
  }
  return false;
}

void UpdateForklift(PPCContext& ctx, uint8_t* base) {
  Prop* p = FindKind("forklift");
  if (!p) return;
  if (g_fork.x == 0 && g_fork.z == 0) g_fork.x = p->x, g_fork.z = p->z, g_fork.heading = 3.14159265f - p->ry;
  // the crate
  if (g_fork.crate == 1) {
    ++g_fork.crate_t;
    const float fx = std::sin(g_fork.heading), fz = std::cos(g_fork.heading);
    if (g_fork.crate_t == 2 * 30) {  // it lands: whoever stands in front of the forks is hit
      const float cx = g_fork.x - fx * 18.0f, cz = g_fork.z - fz * 18.0f;
      for (int i = 0; i < 8; ++i)
        if (const uint32_t o = Rd32(base + kChars + i * 4); o && o != g_fork.rider) {
          const float dx = RdF(base + o + 288) - cx, dz = RdF(base + o + 296) - cz;
          if (dx * dx + dz * dz < 14.0f * 14.0f) JumpAct(ctx, base, o, kHitByObject);
        }
    }
    if (g_fork.crate_t == 2 * 60) {
      if (p->recs.size() > 1) ShowRec(ctx, base, p->recs[1], false);
      g_fork.crate = 2;
    }
  } else if (g_fork.crate == 2) {
    for (const Prop& q : Cur().props)
      if (q.kind == "boxsupply" && InShape(q, int(Param(q, 1)), g_fork.x, 0.0f, g_fork.z, 4.0f)) {
        g_fork.crate = 0;
        if (p->recs.size() > 1) {
          ShowRec(ctx, base, p->recs[1], true);
          PlayRec(ctx, base, p->recs[1], ObjAct() + uint32_t(Param(*p, 4, 85)));
        }
        ForkliftPlace(ctx, base, *p);
        REXLOG_INFO("hctp: a new crate on {}", p->name);
      }
  }
  const uint32_t f = g_fork.rider;
  if (!f) return;
  const uint32_t m = Rd32(base + f + 212);
  ++g_fork.t;
  const uint32_t input = svr2011::FighterInput(f);
  const uint32_t held = input ? Rd32(base + input + 48) : 0, pressed = input ? Rd32(base + input + 52) : 0;
  const int32_t stick = input ? int32_t(Rd32(base + input + 56)) : -1;
  if (g_log && g_fork.t % 60 == 0) REXLOG_INFO("hctp: forklift input held {:08X} pressed {:08X} stick {}", held, pressed, stick);
  const bool riding_act = m == RoomKey(12080) || m == RoomKey(12081) || m == RoomKey(12082) || m == RoomKey(12083) ||
                          m == RoomKey(12085) || m == RoomKey(12084);
  if (!riding_act) {  // knocked off
    JumpAct(ctx, base, f, RoomKey(12091));
    g_fork = {0, 0, 0, g_fork.x, g_fork.z, g_fork.heading, 0, g_fork.moved, g_fork.crate, g_fork.crate_t};
    return;
  }
  if (g_fork.state == 1) {
    if (g_fork.t >= 2 * 27) g_fork.state = 2, JumpAct(ctx, base, f, RoomKey(12085));
    return;
  }
  if (g_fork.state == 3) {
    if (g_fork.t >= 2 * 36) g_fork.rider = 0, g_fork.state = 0;
    return;
  }
  // driving
  if ((pressed & kButtonX) && g_fork.t > 2 * 10) {
    JumpAct(ctx, base, f, RoomKey(12084));
    g_fork.state = 3, g_fork.t = 0, g_fork.speed = 0;
    REXLOG_INFO("hctp: he gets off {}", p->name);
    return;
  }
  if ((pressed & kButtonB) && g_fork.crate == 0 && p->recs.size() > 1) {
    PlayRec(ctx, base, p->recs[1], ObjAct() + uint32_t(Param(*p, 5, 86)));
    g_fork.crate = 1, g_fork.crate_t = 0;
    REXLOG_INFO("hctp: the crate tips off {}", p->name);
  }
  float& v = g_fork.speed;
  if (held & 0x40) v = std::min(1.0f, v + 0.015f);
  else if (held & kButtonR1) v = std::fabs(v) < 0.1f ? 0.0f : v - std::copysign(0.05f, v);
  else if (held & kButtonY) v = std::max(-1.2f, v - 0.03f);
  else v = v > 0 ? std::max(0.0f, v - 0.0125f) : std::min(0.0f, v + 0.0075f);
  // steering: the stick's way (8 directions, 45 degrees apart: 4 up the screen = -z, 6 right = -x,
  // 0 down) as a world heading (0 = toward -z), turned toward
  bool turning = false;
  if (stick >= 0 && stick < 8 && std::fabs(v) > 0.01f) {
    float want = float(stick - 4) * 0.785398f;
    if (v < 0) want += 3.14159265f;
    float d = std::remainder(want - g_fork.heading, 6.2831853f);
    const float step = 0.0245f * std::fabs(v);
    turning = std::fabs(d) > step;
    g_fork.heading += std::clamp(d, -step, step);
  }
  // (60 Hz: HCTP's per-frame move halved)
  g_fork.x -= std::sin(g_fork.heading) * 0.375f * v;
  g_fork.z -= std::cos(g_fork.heading) * 0.375f * v;
  ForkliftPlace(ctx, base, *p);
  PlaceFighter(base, f, g_fork.x, p->y, g_fork.z, g_fork.heading + 3.14159265f);
  const uint32_t want_act = turning ? RoomKey(12082) : RoomKey(12085);
  if (m != want_act && (m == RoomKey(12085) || m == RoomKey(12082) || m == RoomKey(12083)) &&
      (m != want_act || RdF(base + f + 24) >= float(Frames(want_act, 24) - 1)))
    JumpAct(ctx, base, f, want_act);
  else if (RdF(base + f + 24) >= float(Frames(m, 24) - 1))
    JumpAct(ctx, base, f, m);  // (the seated loop again)
}

// -- Off the cage top (HCTP GapIn_BackOpenSpace, from the strike button):
// up on the cage (y <= -28) near its front or east edge, X while walking
// toward the edge puts him on the edge (HCTP SetGapPos) and he jumps down
// (12572). After the roof broke, X toward the hole steps in (12092) and he
// drops into the cage. 2011's facing: an act's local +z (and the way he
// faces) is (sin yaw, cos yaw) in the world.
bool EdgeJump(PPCContext& ctx, uint8_t* base, uint32_t f, int i, float x, float y, float z) {
  if (y > -28.0f || x < 100.0f || x > 166.0f || z < -505.0f || z > -452.0f) return false;
  const float yaw = RdF(base + f + 276), fx = std::sin(yaw), fz = std::cos(yaw);
  // the roof hole (both halves gone): step in
  for (const Prop& r : Cur().props)
    if (r.kind == "roof" && r.state && InBox(r, x, z, 6.0f) && !InBox(r, x, z, 0.0f)) {
      const float cx = (r.box[0] + r.box[1]) / 2 - x, cz = (r.box[4] + r.box[5]) / 2 - z;
      if ((cx * fx + cz * fz) / std::max(0.01f, std::sqrt(cx * cx + cz * cz)) < 0.5f) continue;
      JumpAct(ctx, base, f, kStepIn);
      PlaceFighter(base, f, std::clamp(x, r.box[0] + 1, r.box[1] - 1), y, std::clamp(z, r.box[4] + 1, r.box[5] - 1), yaw);
      Cur().drop = 60;
      REXLOG_INFO("hctp: fighter {} steps into the hole", i);
      return true;
    }
  // the front edge (z -460, jump toward +z) and the east edge (x 160, toward +x)
  // (his facing goes in before the act - its root motion turns with it -
  // his place after it, as the T22 snap does)
  if (z > -466.0f && fz > 0.7f) {
    WrF(base + f + 276, 0.0f);
    JumpAct(ctx, base, f, kJumpDown);
    PlaceFighter(base, f, x, y, -463.0f, 0.0f);
  } else if (x > 154.0f && fx > 0.7f) {
    WrF(base + f + 276, 1.5707963f);
    JumpAct(ctx, base, f, kJumpDown);
    PlaceFighter(base, f, 157.0f, y, z, 1.5707963f);
  } else {
    return false;
  }
  REXLOG_INFO("hctp: fighter {} jumps down off the cage (yaw before {:.2f})", i, yaw);
  return true;
}

void Fighters(PPCContext& ctx, uint8_t* base) {
  for (int i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + i * 4);
    if (!f) continue;
    FighterState& s = g_fs[i];
    if (s.fighter != f) s = {f, Rd32(base + f + 212), -1, 0, 0};
    const uint32_t m = Rd32(base + f + 212);
    const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
    const uint8_t touched = base[f + 455];
    // thrown / whipped / running into a stack, railing or the lead pipes
    if (m != kWalkBackstage)
      for (Prop& p : Cur().props)
        if ((p.kind == "stack" || p.kind == "throw" || p.kind == "drums") && p.state == 0 && touched == p.attr)
          KnockOver(ctx, base, p, f);
    // a knock-down on the roof
    // (lying: 0 here - backstage standing is 20500 - or 1000)
    const bool down = m == 0 || m == kLying, was_down = s.motion == 0 || s.motion == kLying;
    if (down && !was_down)
      for (Prop& r : Cur().props)
        if (r.kind == "roof" && r.state == 0 && OnRoofHalf(r, x, y, z)) {
          if (++r.count >= (g_roof_once ? 1 : 2))
            BreakRoof(ctx, base);
          else
            REXLOG_INFO("hctp: the cage roof shakes (fighter {} down at {:.0f} {:.0f} {:.0f})", i, x, y, z);
          break;
        }
    // the cage walls: X next to a wall grabs it, mashing X tears it down
    // (the input object's +52 holds the buttons pressed this frame - X is 0x80;
    // +68 gets them two frames later, when the strike X starts is already on)
    const uint32_t input = svr2011::FighterInput(f);
    const bool x_pressed = input && (Rd32(base + input + 52) & kButtonX);
    if (HctpRoom() != 30) {
      Room31Fighter(ctx, base, f, i, m, s.motion, touched, x, y, z);
      Room35Fighter(ctx, base, f, i, m, x, y, z);
      Room33Fighter(ctx, base, f, i, m, s.motion, touched, x, y, z, x_pressed);
      if (x_pressed && Afoot(s.motion, y))
        if (Prop* fl = FindKind("forklift"); fl && ForkliftGetOn(ctx, base, *fl, f, i, x, z)) {
          s.motion = Rd32(base + f + 212);
          continue;
        }
      if (Room32Fighter(ctx, base, f, i, m, s.motion, touched, x, y, z, x_pressed)) {
        s.motion = Rd32(base + f + 212);
        continue;
      }
    }
    if (g_log && i == 0 && input) {
      static std::string last;
      std::string hex;
      for (uint32_t at = 48; at < 112; ++at) hex += fmt::format("{}{:02X}", at % 4 ? "" : " ", base[input + at]);
      if (hex != last && (Rd32(base + input + 52) & 0xFFFF)) REXLOG_INFO("hctp: fighter 0 input +48:{} motion {}", hex, m);
      last = hex;
    }
    // (X is the strike button: the press has already started a strike this
    // frame, so it's last frame's motion that must be the walk; the grab
    // replaces the strike)
    bool used = false;
    if (s.wall < 0 && x_pressed && s.motion == kWalkBackstage) {
      // a green button or a valve (before the cage walls: the ground button
      // is within reach of the east wall)
      for (Prop& p : Cur().props) {
        if (used || !InZone(p, x, y, z)) continue;
        if (p.kind == "switch") PressSwitch(ctx, base, f, i, p), used = true;
        if (p.kind == "valve") TurnValve(ctx, base, f, i, p), used = true;
      }
      for (size_t j = 0; j < Cur().props.size() && !used && !g_no_grab; ++j) {
        const Prop& w = Cur().props[j];
        if (w.kind != "wall" || w.state || std::fabs(y) > 5.0f) continue;
        // (next to the panel: within 8 of its collision)
        if (InBox(w, x, z, 8.0f)) {
          s.wall = int(j);
          s.presses = 0;
          s.idle = 0;
          s.away = 0;
          JumpAct(ctx, base, f, kWallGrab);
          PlayProp(ctx, base, w, kWallGrab, 0, 1);
          REXLOG_INFO("hctp: fighter {} grabs {}", i, w.name);
          used = true;
        }
      }
      if (!used && HctpRoom() == 30) used = EdgeJump(ctx, base, f, i, x, y, z);
    } else if (s.wall >= 0) {
      if (g_log && s.idle % 60 == 0)
        REXLOG_INFO("hctp: fighter {} holds {}: motion {} presses {} idle {}", i, Cur().props[size_t(s.wall)].name, m,
                    s.presses, s.idle);
      // holding the wall (HCTP: shaking it while the button is mashed; he
      // lets go 180 frames after the last press; ~11 presses tear it down)
      ++s.idle;
      // (the stick away from the wall - 8 ways, 4 up the screen = -z, 6 right = -x - or B lets go: a
      // player can always walk off; before, only 180 frames without a press did)
      const Prop& held = Cur().props[size_t(s.wall)];
      const int32_t stick = input ? int32_t(Rd32(base + input + 56)) : -1;
      bool off = input && (Rd32(base + input + 52) & kButtonB);
      if (stick >= 0 && stick < 8 && held.has_box) {
        const float a = float(stick - 4) * 0.785398f;
        const float wx = x - (held.box[0] + held.box[1]) * 0.5f, wz = z - (held.box[4] + held.box[5]) * 0.5f;
        s.away = -std::sin(a) * wx - std::cos(a) * wz > 0.0f ? s.away + 1 : 0;
      } else
        s.away = 0;
      off = off || s.away >= 8;
      if (x_pressed) {
        ++s.presses;
        s.idle = 0;
      }
      if (m != kWallGrab && m != kWalkBackstage) {
        s.wall = -1;  // knocked off
      } else if (s.presses >= 11) {
        JumpAct(ctx, base, f, kWallBreak);
        BreakWall(ctx, base, s.wall);
        REXLOG_INFO("hctp: fighter {} tears {} down", i, Cur().props[size_t(s.wall)].name);
        s.wall = -1;
      } else if (s.idle > 180 || off) {
        JumpAct(ctx, base, f, kWallRelease);  // let go (HCTP 12060)
        REXLOG_INFO("hctp: fighter {} lets go of {}", i, Cur().props[size_t(s.wall)].name);
        s.wall = -1;
      } else if (m != kWallGrab) {
        JumpAct(ctx, base, f, kWallGrab);  // (the shake again: he keeps hold)
        PlayProp(ctx, base, Cur().props[size_t(s.wall)], kWallGrab, 0, 1);
      }
    }
    // whipped into the equipment crate
    if (m == kWhipRun && Cur().crate == 0)
      if (Prop* cr = FindKind("crate"); cr && InBox(*cr, x, z, 2.0f) && std::fabs(y) < 5.0f) ShutIn(ctx, base, *cr, f, i);
    // (his motion now: an act started here this frame counts as last frame's)
    s.motion = Rd32(base + f + 212);
  }
}

// -- Hits through the cage fence (HCTP CGmWireNetting::CheckChar +
// GmAtariCheck): a wrestler inside the cage and one outside it can't hit
// or grab each other while the wall between them stands. Room 30's cage:
// x 100..160, z -500..-460; the east (x 160) and front (z -460) sides are
// the two panel pairs; west and back are solid walls. Wrestlers up high
// (y < -20: the roof, the walkway) are not in either cell.
int CageCell(uint32_t f, uint8_t* base, int* side) {
  const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
  if (y < -20.0f) return 0;
  if (x > 100 && x < 160 && z > -500 && z < -460) return 1;
  if (x < 75 || x > 185 || z < -525 || z > -435) return 0;
  *side = x >= 160 ? 0 : z >= -460 ? 1 : 2;  // east panels, front panels, a solid side
  return 2;
}

bool SideOpen(int side) {
  if (side == 2) return false;
  for (const Prop& w : Cur().props)
    if (w.kind == "wall" && w.has_box && (side == 0) == (w.box[0] > 155.0f) && w.state == 0) return false;
  return true;
}

// The fighter a hit collider belongs to (as sub_82238F28 finds it).
uint32_t ColliderFighter(PPCContext& ctx, uint8_t* base, uint32_t collider) {
  const auto saved = ctx;
  ctx.r3.u64 = collider;
  sub_8269DF80(ctx, base);
  ctx.r4.u64 = 0x43484152;  // 'CHAR'
  sub_8269DFB0(ctx, base);
  const uint32_t c = ctx.r3.u32;
  ctx = saved;
  return c ? Rd32(base + c + 24) : 0;
}

// -- The HCTP backstage as one stage ----------------------------------------
//
// A world mod (build_world.py) has every HCTP room in one bg78, all in the
// parking room's place one over the other (the match camera's target stays
// inside the stock backstage), each in its own model ids and gimmick objects.
// Its hctp.txt has a 'room' line per room (number, model ids, object ids, its
// object acts' first key, its floor x0 x1 z0 z1, its HMD 1004 triangles and
// climb-spot records first..last), 'door' lines (HCTP's 50000.txt move points
// 201-210 and where HCTP's gates send them: id, the door it leads to, where it
// is, HCTP's exit act and the way out, where those coming in stand and face)
// and 'spawn' lines (weapons put down on its first visit). Only the current
// room is shown, collides, has its climb spots and runs its props. As in HCTP
// (CGate4 / CMoveStage) a player within 10 of a gate on each axis presses Y
// (the CPU walks into it for half a second): he plays the exit act there, then
// every fighter stands at the other gate's spots, the fight box goes with them
// and the camera cuts.
struct Stash {  // (the per-room state kept outside Room while a room is away)
  std::vector<LaterShow> later;
  std::map<size_t, Taxi> taxis;
  std::vector<Pole> poles;
  bool poles_set = false;
  Heli heli;
  Forklift fork;
};
std::vector<Stash> g_stash;
bool g_box_set = false;
bool g_world_set = false;  // (the other rooms' collision and climb spots taken out)
int g_door_cd = 0;        // frames before a door takes anyone again
int g_door_hold[8] = {};  // frames each fighter has walked into a door
// (a gate's exit act under way: the door, the one going through and the frames since; at most 200)
struct Going {
  const Door* door = nullptr;
  int who = -1, frames = 0;
  std::array<std::array<float, 4>, 8> wait{};  // (where the others stand meanwhile: x, y, z, yaw)
};
Going g_going;
// The screen's fade at a gate (HCTP's exit acts fade to black - their event op 18, dropped: 2011 never
// faded back in): black over the act's last kFadeFrames, back in over kFadeFrames in the new room.
// Set by the game's frame, drawn by DoorPromptOverlay.
constexpr int kFadeFrames = 20;
std::atomic<float> g_fade{0.0f};
int g_fade_in = 0;

void SetPoles() {
  g_poles.clear();
  for (const auto& q : Cur().poles) g_poles.push_back({q[0], 0.0f, q[2]});
}

void RoomBox(uint8_t* base, const Room& r) {
  svr2011::SetFightBox(base, (r.area[0] + r.area[1]) / 2, (r.area[2] + r.area[3]) / 2, (r.area[1] - r.area[0]) / 2,
                       (r.area[3] - r.area[2]) / 2);
}

// A room's collision in or out (all rooms lie one over the other: only the current one's triangles stay;
// the others' are lifted far away, as DropCollision does, and put back as they were).
void RoomCollision(uint8_t* base, Room& r, bool in) {
  const uint32_t stage = Rd32(base + kStage);
  const uint32_t trip = stage ? Rd32(base + stage + 22516 + 4 * kParkingTrip) : 0;
  if (!trip || r.tri_hi < r.tri_lo) return;
  const uint32_t n = Rd32(base + trip + 176), recs = Rd32(base + trip + 180);
  if (!in) r.coll_saved.clear();
  size_t k = 0;
  for (int t = r.tri_lo; t <= r.tri_hi && uint32_t(t) < n; ++t) {
    const uint32_t rec = Rd32(base + recs + 4 * uint32_t(t));
    if (!rec) continue;
    if (!in) {
      r.coll_saved.push_back({RdF(base + rec + 12), RdF(base + rec + 28)});
      WrF(base + rec + 12, 1.0e6f);
      WrF(base + rec + 28, 1.0e6f);
    } else if (k < r.coll_saved.size()) {
      WrF(base + rec + 12, r.coll_saved[k].first);
      WrF(base + rec + 28, r.coll_saved[k].second);
      ++k;
    }
  }
}

// A room's climb spots (T21 / T22 records) on or off.
void RoomSpots(PPCContext& ctx, uint8_t* base, const Room& r, bool on) {
  const uint32_t mgr = Rd32(base + kGimmickMgr);
  if (!mgr) return;
  const auto saved = ctx;
  for (int rec = r.spot_lo; rec <= r.spot_hi; ++rec) {
    ctx.r3.u64 = mgr, ctx.r4.u64 = uint32_t(rec), ctx.r5.u64 = 0;
    sub_824B0DC8(ctx, base);
    if (const uint32_t e = ctx.r3.u32) {
      ctx.r3.u64 = e, ctx.r4.u64 = on ? 1 : 0, ctx.r5.u64 = on ? 1 : 0;
      sub_824B8FE0(ctx, base);
    }
  }
  ctx = saved;
}

// First visit: the room's weapons put down.
void Visit(PPCContext& ctx, uint8_t* base, Room& r) {
  if (r.visited) return;
  r.visited = true;
  for (const auto& w : r.spawns) SpawnWeapon(ctx, base, int(w[0]), w[1], w[2], w[3]);
}

// The weapons lying in a room (the match's weapon objects, 72 at 0x82DE0220, position +288..+296 as a
// wrestler's): parked far below the floor while the room is away, back where they lay when it is the
// current one again - unless someone took one meanwhile. One within 14 of a wrestler is left alone: in
// his hands it goes with him (one only lying by him shows in the next room).
constexpr uint32_t kWeaponObjects = 0x82DE0220;
constexpr float kParkDrop = 5000.0f;

void PlaceObject(uint8_t* base, uint32_t o, float x, float y, float z) {
  WrF(base + o + 288, x), WrF(base + o + 292, y), WrF(base + o + 296, z);
  const uint32_t body = Rd32(base + o + 304);
  if (body >= 0x40000000u && body < 0xA0000000u) WrF(base + body, x), WrF(base + body + 4, y), WrF(base + body + 8, z);
}

void ParkWeapons(uint8_t* base, Room& r) {
  r.parked.clear();
  for (uint32_t i = 0; i < 72; ++i) {
    const uint32_t w = Rd32(base + kWeaponObjects + 4 * i);
    if (!w) continue;
    const float x = RdF(base + w + 288), y = RdF(base + w + 292), z = RdF(base + w + 296);
    if (y > kParkDrop / 2) continue;
    bool by_someone = false;
    for (uint32_t j = 0; j < 8 && !by_someone; ++j)
      if (const uint32_t f = Rd32(base + kChars + 4 * j)) {
        const float dx = RdF(base + f + 288) - x, dz = RdF(base + f + 296) - z;
        by_someone = dx * dx + dz * dz < 14.0f * 14.0f;
      }
    if (by_someone) continue;
    r.parked.push_back({w, {x, y, z}});
    PlaceObject(base, w, x, y + kParkDrop, z);
  }
}

void UnparkWeapons(uint8_t* base, Room& r) {
  for (const auto& [w, p] : r.parked) {
    bool still = false;
    for (uint32_t i = 0; i < 72 && !still; ++i) still = Rd32(base + kWeaponObjects + 4 * i) == w;
    if (still && RdF(base + w + 292) > kParkDrop / 2) PlaceObject(base, w, p[0], p[1], p[2]);
  }
  r.parked.clear();
}

void SwitchRoom(PPCContext& ctx, uint8_t* base, size_t k) {
  if (k == g_cur || k >= g_rooms.size()) return;
  RoomCollision(base, Cur(), false);
  RoomSpots(ctx, base, Cur(), false);
  Stash& old = g_stash[g_cur];
  old.later = std::move(g_later), old.taxis = std::move(g_taxis), old.poles = std::move(g_poles);
  old.heli = g_heli, old.fork = g_fork, old.poles_set = true;
  // (the room left: its records inactive, so none of its keys show its objects again)
  for (const Prop& p : Cur().props)
    for (int rec : p.recs) ShowRecRaw(ctx, base, rec, false);
  g_cur = k;
  Stash& now = g_stash[k];
  g_later = std::move(now.later), g_taxis = std::move(now.taxis), g_heli = now.heli, g_fork = now.fork;
  if (now.poles_set)
    g_poles = std::move(now.poles);
  else
    SetPoles();
  for (const Prop& p : Cur().props)
    for (int rec : p.recs) {
      const auto it = Cur().rec_shown.find(rec);
      ShowRecRaw(ctx, base, rec, it == Cur().rec_shown.end() || it->second);
    }
  RoomCollision(base, Cur(), true);
  RoomSpots(ctx, base, Cur(), true);
  UnparkWeapons(base, Cur());
  Visit(ctx, base, Cur());
  Cur().set_up_frames = 0;  // (SetUp hides again what waits hidden)
  for (auto& s : g_fs) s = {};
  ClimbAttrs();
  RoomBox(base, Cur());
  REXLOG_INFO("hctp: now in room {} ({} props)", Cur().number, Cur().props.size());
}

void GoThrough(PPCContext& ctx, uint8_t* base, const Door& d, int who) {
  for (size_t k = 0; k < g_rooms.size(); ++k)
    for (const Door& t : g_rooms[k].doors) {
      if (t.id != d.to) continue;
      ParkWeapons(base, Cur());  // (before anyone moves: what lies by a wrestler goes with him)
      // (side by side across the way in; the one who went through in front)
      const float fx = std::sin(t.eyaw), fz = std::cos(t.eyaw);
      int n = 0;
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t f = Rd32(base + kChars + 4 * i);
        if (!f || !Rd32(base + f + 304)) continue;
        const float side = (n % 2 ? -1.0f : 1.0f) * float((n + 1) / 2) * 14.0f;
        const float back = i == uint32_t(who) ? 0.0f : -12.0f;
        // (an act first - standing, 0 - then the place: a walk or run under way puts him back where
        // he was, the game's own snap does it in this order too)
        JumpAct(ctx, base, f, 0);
        if (t.spots.size() == 8)  // (a gate: fighter i at HCTP's spot i - CMoveStage's Rear)
          PlaceFighterHeld(base, f, t.spots[i][0], t.y, t.spots[i][1], t.eyaw);
        else
          PlaceFighterHeld(base, f, t.ex + side * fz + back * fx, t.y, t.ez - side * fx + back * fz, t.eyaw);
        ++n;
      }
      svr2011::SnapMatchCamera();
      REXLOG_INFO("hctp: door {} (room {}) -> door {} (room {})", d.id, Cur().number, t.id, g_rooms[k].number);
      SwitchRoom(ctx, base, k);
      g_door_cd = 90;
      g_going = {};
      std::fill(std::begin(g_door_hold), std::end(g_door_hold), 0);
      return;
    }
}

// The door prompt (drawn by DoorPromptOverlay on the UI thread): which player stands at a door and
// where it leads. HCTP's room names.
struct DoorPrompt {
  uint32_t port = 0;
  int to = 0;  // (the door it leads to)
};
std::mutex g_prompt_mutex;
DoorPrompt g_prompt;
std::string g_prompt_room;  // (empty: no prompt)
std::chrono::steady_clock::time_point g_prompt_at;

const char* RoomName(int number) {
  switch (number) {
    case 30: return "BACK ENTRANCE";
    case 31: return "PARKING LOT";
    case 32: return "TRAINING ROOM";
    case 33: return "TIMES SQUARE";
    case 34: return "OUTSIDE THE ARENA";
    case 35: case 36: return "FOUNTAIN SQUARE";
    default: return "THE NEXT ROOM";
  }
}

void PublishPrompt(const DoorPrompt* p) {
  std::string room;
  if (p)
    for (const Room& r : g_rooms)
      for (const Door& d : r.doors)
        if (d.id == p->to) room = RoomName(r.number);
  std::lock_guard<std::mutex> lock(g_prompt_mutex);
  if (p) g_prompt = *p;
  g_prompt_room = room;
  g_prompt_at = std::chrono::steady_clock::now();
}

// The button as the player's controller names it (pad_types.h: PlayStation / keyboard players).
std::string YLabel(uint32_t port) {
  switch (svr2011::PlayerPadType(port)) {
    case svr2011::PadType::kPlayStation: return "TRIANGLE";
    case svr2011::PadType::kKeyboard: {
      std::string k = rex::cvar::Query<std::string>("keybind_y");
      k = k.substr(0, k.find(','));
      std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::toupper(c)); });
      return k.empty() ? "Y" : k;
    }
    default: return "Y";
  }
}

class DoorPromptOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit DoorPromptOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (const float a = g_fade.load(); a > 0.0f && svr2011::MatchHudVisible())
      ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, int(a * 255.0f)));
    std::string room;
    uint32_t port = 0;
    {
      std::lock_guard<std::mutex> lock(g_prompt_mutex);
      if (std::chrono::steady_clock::now() - g_prompt_at > std::chrono::milliseconds(300)) return;
      room = g_prompt_room, port = g_prompt.port;
    }
    if (room.empty() || !svr2011::MatchHudVisible()) return;
    svr2011::DrawHudPanel(io.DisplaySize.x, io.DisplaySize.y, YLabel(port), "GO TO " + room);
  }
};

void UpdateWorld(PPCContext& ctx, uint8_t* base) {
  if (!World()) return;
  if (g_fade_in > 0) g_fade.store(float(--g_fade_in) / kFadeFrames);
  // (once the stage and the package are there: only the first room's collision and climb spots)
  if (!g_world_set && Rd32(base + kGimmickMgr)) {
    g_world_set = true;
    for (size_t k = 0; k < g_rooms.size(); ++k)
      if (k != g_cur) RoomCollision(base, g_rooms[k], false), RoomSpots(ctx, base, g_rooms[k], false);
    Cur().visited = true;  // (the first room's weapons are the manifest's)
  }
  DoorPrompt prompt;
  bool prompt_set = false;
  if (g_door_cd > 0) {
    --g_door_cd;
    PublishPrompt(nullptr);
    return;
  }
  // (a gate's exit act: through once it has played - 200 frames at most; the others wait meanwhile where
  // they stood, as CMoveStage has them - act 0, the pads stopped)
  if (g_going.door) {
    const uint32_t f = Rd32(base + kChars + 4 * uint32_t(g_going.who));
    ++g_going.frames;
    for (uint32_t i = 0; i < 8; ++i)
      if (const uint32_t o = Rd32(base + kChars + 4 * i); o && i != uint32_t(g_going.who)) {
        const auto& w = g_going.wait[i];
        PlaceBody(base, o, w[0], w[1], w[2], w[3]);
      }
    const int n = std::min(Frames(g_going.door->act, 60), 200);
    g_fade.store(std::clamp(float(g_going.frames - (n - kFadeFrames)) / kFadeFrames, 0.0f, 1.0f));
    // (the act holds its last frame: through once its frames have played, not at the 200 cap - a second black)
    if (!f || (g_going.frames > 2 && Rd32(base + f + 212) != g_going.door->act) || g_going.frames >= n + 2) {
      const int frames = g_going.frames;
      GoThrough(ctx, base, *g_going.door, g_going.who);
      g_fade_in = g_fade.load() > 0.0f ? int(g_fade.load() * kFadeFrames) : 0;
      if (g_fade_in == 0) g_fade.store(0.0f);
      REXLOG_INFO("hctp: through after {} of the act's {} frames - black {:.2f}, back in over {} frames", frames, n, g_fade.load(), g_fade_in);
    }
    PublishPrompt(nullptr);
    return;
  }
  // (HCTP's exit act at the gate, facing out - the act first, then the place)
  const auto go = [&](const Door& d, uint32_t f, int i) {
    if (!d.act || !HasKey(ctx, base, d.act, 0, 0)) return GoThrough(ctx, base, d, i);
    JumpAct(ctx, base, f, d.act);
    PlaceFighterHeld(base, f, d.x, d.y, d.z, d.oyaw);
    g_going = {&d, i, 0};
    for (uint32_t k = 0; k < 8; ++k)
      if (const uint32_t o = Rd32(base + kChars + 4 * k); o && k != uint32_t(i)) {
        JumpAct(ctx, base, o, 0);
        g_going.wait[k] = {RdF(base + o + 288), RdF(base + o + 292), RdF(base + o + 296), RdF(base + o + 276)};
      }
    REXLOG_INFO("hctp: fighter {} goes through door {} (act {})", i, d.id, d.act);
  };
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + 4 * i);
    int& hold = g_door_hold[i];
    if (!f) {
      hold = 0;
      continue;
    }
    const uint32_t m = Rd32(base + f + 212);
    const bool moving = m == kWalkBackstage || base[f + 1286];
    const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
    const Door* door = nullptr;
    // (a gate: CGate4::Action's 10 on each axis, 16 across here - 2011's walls keep a wrestler further from
    // some of HCTP's gate marks: room 30's roof gate 202 stopped him 12 away, so the gate seemed missing;
    // an older door: 22 around, 4 up or down)
    for (const Door& d : Cur().doors)
      if (d.spots.empty() ? (x - d.x) * (x - d.x) + (z - d.z) * (z - d.z) < 22.0f * 22.0f && std::fabs(y - d.y) <= 4.0f
                          : std::fabs(x - d.x) < 16.0f && std::fabs(y - d.y) < 10.0f && std::fabs(z - d.z) < 16.0f)
        door = &d;
    // a player: Y at the door (HCTP's own way; the prompt shows meanwhile); the CPU: walking into it for
    // half a second (its AI walks after its opponent - CPU-vs-CPU and chases keep moving)
    // (online every wrestler is a player: a port is local - the other side's player has one of 4 or
    // more - and both games must decide alike)
    const uint32_t port = Rd32(base + f + 1152);
    const bool player = (port < 4 || svr2011::P2POnlineMode()) && !g_walk_in;
    if (player) {
      hold = 0;
      // (on his feet: standing 0, fighting stance 20, walking or running - not lying or in a move)
      const bool ready = m == 0 || m == 20 || m == kWalkBackstage || base[f + 1286];
      if (!door || !ready) continue;
      if (!prompt_set && port < 4) {  // (only this PC's own players see it)
        prompt_set = true;
        prompt.port = port, prompt.to = door->to;
      }
      const uint32_t input = svr2011::FighterInput(f);
      if (input && (Rd32(base + input + 52) & kButtonY)) {
        go(*door, f, int(i));
        break;
      }
      continue;
    }
    hold = door && moving ? hold + 1 : 0;
    if (hold >= 30) {
      go(*door, f, int(i));
      break;
    }
  }
  PublishPrompt(prompt_set ? &prompt : nullptr);
}

bool FenceBlocks(PPCContext& ctx, uint8_t* base, uint32_t comp, uint32_t target) {
  if (HctpRoom() != 30 || !Cur().loaded || !comp || !target) return false;
  const uint32_t att = Rd32(base + comp + 448), vic = ColliderFighter(ctx, base, target);
  if (!att || !vic || att == vic) return false;
  int sa = 2, sv = 2;
  const int ca = CageCell(att, base, &sa), cv = CageCell(vic, base, &sv);
  if (g_log && ca && cv && ca != cv) REXLOG_INFO("hctp: a hit across the fence (cells {} -> {}, side {})", ca, cv, ca == 2 ? sa : sv);
  if (!ca || !cv || ca == cv) return false;
  if (SideOpen(ca == 2 ? sa : sv)) return false;
  if (g_log) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t)) REXLOG_INFO("hctp: the fence stops a hit ({:08X} -> {:08X})", att, vic);
  }
  return true;
}

}  // namespace

// A fighter's attack collider meets a target (the hit broker sub_823AC758
// calls vt[8] = sub_82238F28, then vt[10] = sub_8222F678); a hit or grab
// through the standing cage fence is dropped here.
REX_EXTERN(__imp__sub_82238F28);
REX_HOOK_RAW(sub_82238F28) {
  if (FenceBlocks(ctx, base, ctx.r3.u32, ctx.r5.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82238F28(ctx, base);
}

REX_EXTERN(__imp__sub_8222F678);
REX_HOOK_RAW(sub_8222F678) {
  if (FenceBlocks(ctx, base, ctx.r3.u32, ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8222F678(ctx, base);
}

namespace svr2011 {

void HctpRoomUpdate(PPCContext& ctx, uint8_t* base) {
  if (!HctpRoomActive()) {
    if (Cur().loaded) {
      for (Held& h : g_held) h = {};
      REXLOG_INFO("hctp: room unloaded (held placements, door hold cleared)");
      g_going = {};
      g_fade.store(0.0f), g_fade_in = 0;
      std::fill(std::begin(g_door_hold), std::end(g_door_hold), 0);
    }
    Cur().loaded = false;
    return;
  }
  const uint32_t frame = Rd32(base + kMatchFrames);
  const auto& row = BackstageRows()[size_t(g_active_row)];
  if (!Cur().loaded || Cur().dir != row.dir || frame < Cur().last_frame) {
    if (!LoadRoom(row.dir)) return;
    g_room_rng = 1;
    for (auto& s : g_fs) s = {};
    g_later.clear();
    g_taxis.clear();
    g_fork = {};
    g_heli = {};
    SetPoles();
    g_stash.assign(g_rooms.size(), Stash{});
    g_door_cd = 0;
    g_going = {};  // (its door was in the rooms just replaced)
    g_fade.store(0.0f), g_fade_in = 0;
    g_box_set = false;
    g_world_set = false;
    for (Held& h : g_held) h = {};
    std::fill(std::begin(g_door_hold), std::end(g_door_hold), 0);
    REXLOG_INFO("hctp: room loaded ({} rooms) - reset: wrestler states, later shows, taxis, forklift, heli, poles, "
                "room stashes, held placements, door hold / cooldown, fight box, rng", g_rooms.size());
  }
  Cur().last_frame = frame;
  if (!Rd32(base + kGimmickMgr) || !Rd32(base + kStage)) return;
  if (World() && !g_box_set) g_box_set = true, RoomBox(base, Cur());
  static uint32_t tour_step = 0;
  if (g_tour_now && World() && frame >= 360 && frame / 360 != tour_step && !Cur().doors.empty()) {
    tour_step = frame / 360;
    static std::map<int, size_t> next;
    size_t& n = next[Cur().number];
    GoThrough(ctx, base, Cur().doors[n++ % Cur().doors.size()], 0);
  }
  if (g_door_now && World() && !Cur().doors.empty()) {
    static bool done[2] = {};
    for (uint32_t i = 0; i < 2; ++i)
      if (!done[i] && frame >= (i ? 900u : 300u) && Rd32(base + kChars + 4 * i)) {
        done[i] = true;
        const Door& d = Cur().doors[0];
        JumpAct(ctx, base, Rd32(base + kChars + 4 * i), 0);  // (an act first, as GoThrough: then the place holds)
        PlaceFighterHeld(base, Rd32(base + kChars + 4 * i), d.x, d.y, d.z, d.eyaw + 3.14159265f);
        REXLOG_INFO("hctp: fighter {} put at door {} (test)", i, d.id);
      }
  }
  UpdateWorld(ctx, base);
  SetUp(ctx, base);
  Fighters(ctx, base);
  UpdateCycle(ctx, base);
  UpdateSteam(ctx, base);
  if (g_crate_now && frame >= 300 && frame < 400 && Cur().crate == 0)
    if (Prop* cr = FindKind("crate"); cr && Rd32(base + kChars + 4)) ShutIn(ctx, base, *cr, Rd32(base + kChars + 4), 1);
  UpdateCrate(ctx, base);
  if (g_cart_now && frame >= 300 && Cur().cycle == 0 && Cur().cycle_t == 0)
    if (Prop* sw = FindKind("switch")) PressSwitch(ctx, base, Rd32(base + kChars + 4), 1, *sw);
  if (g_puff_now && frame >= 300)
    if (Prop* p = FindKind("puff"); p && Rd32(base + kChars)) {
      const uint32_t f0 = Rd32(base + kChars);
      for (int rec : p->recs)
        MoveRec(ctx, base, rec, RdF(base + f0 + 288), RdF(base + f0 + 292) - 24.0f, RdF(base + f0 + 296), 0,
                float(frame) * 0.03f, 0);
    }
  if (g_props_now && frame >= 300)
    for (Prop& p : Cur().props)
      if ((p.kind == "throw" || p.kind == "stack" || p.kind == "drums") && p.state == 0) KnockOver(ctx, base, p, 0);
  // (=props in rooms 31-36: each car loses its door at 300, its bonnet at 420, is crushed at 540; the
  // equipment box jolts at 300; fighter 1 is electrocuted at 360 and thrown into the trailer at 600)
  // (room 31: fighter 0 - the test's pad - stands by the forklift at 1100, after the trailer slam)
  if (g_props_now && HctpRoom() == 31 && frame >= 1100 && frame < 1102)
    if (Prop* fl = FindKind("forklift"); fl && Rd32(base + kChars))
      PlaceFighter(base, Rd32(base + kChars), fl->x - 10.0f * std::cos(fl->ry), 0.0f, fl->z - 10.0f * std::sin(fl->ry), FighterYaw(fl->ry));
  if (g_fork_now && HctpRoom() == 31 && frame >= 300 && !g_fork.rider && g_fork.state == 0 && Rd32(base + kChars))
    if (Prop* fl = FindKind("forklift")) {
      static bool done = false;
      if (!done) {
        done = true;
        const uint32_t f0 = Rd32(base + kChars);
        const float x = fl->x - 10.0f * std::cos(fl->ry), z = fl->z - 10.0f * std::sin(fl->ry);
        PlaceFighter(base, f0, x, 0.0f, z, FighterYaw(fl->ry));
        ForkliftGetOn(ctx, base, *fl, f0, 0, x, z);
      }
    }
  // (room 33: fighter 0 - the test's pad - stands under the helicopter from 600 to 1200)
  if (g_props_now && HctpRoom() == 33 && frame >= 600 && frame < 1200 && !g_heli.rider && Rd32(base + kChars)) {
    float hx, hy, hz, hyaw;
    if (HeliPos(&hx, &hy, &hz, &hyaw)) PlaceFighter(base, Rd32(base + kChars), hx, 0.0f, hz, 0.0f);
  }
  if (g_props_now && HctpRoom() == 33 && frame >= 420 && frame < 422 && !g_poles.empty() && Rd32(base + kChars + 4)) {
    const uint32_t f1 = Rd32(base + kChars + 4);
    PlaceFighter(base, f1, g_poles[0].x, 0.0f, g_poles[0].z + 6.0f, 3.14159265f);
    PoleClimb(ctx, base, f1, 1, g_poles[0].x, 0.0f, g_poles[0].z + 6.0f);
  }
  if (g_props_now && HctpRoom() != 30 && Rd32(base + kChars + 4)) {
    const uint32_t f1 = Rd32(base + kChars + 4);
    const auto at = [frame](uint32_t n) { return frame >= n && frame < n + 2; };
    for (Prop& p : Cur().props) {
      if (p.kind == "car" && (at(300) || at(420)))
        p.cooldown = 0, CarHit(ctx, base, p, f1, 1, 0, 0, 0, false, frame < 400 ? 1 : 2);
      if (p.kind == "car" && at(540)) CarCrush(ctx, base, p, 1);
      if (p.kind == "repeat" && at(300)) RepeatHit(ctx, base, p, f1, 1);
      if (p.kind == "power" && at(360)) PowerHit(ctx, base, p, f1, 1);
      if (p.kind == "hammer" && at(600)) HammerThrow(ctx, base, p, f1, 1);
      // (room 32: a rack 300, the sandbag 300, the wall 420 / 480 (and the TV), a locker 600, the toilet
      // 1100, a shower 1700)
      if (p.kind == "rack" && at(300) && p.hid == 467) RackHit(ctx, base, p, f1, 1);
      if (p.kind == "sandbag" && at(300)) SandbagHit(ctx, base, p, Rd32(base + kChars), 6.0f);
      // (fighter 0 - the test's pad - on the first treadmill at 800: the script mashes A)
      if (p.kind == "treadmill" && at(800) && &p == FindKind("treadmill") && Rd32(base + kChars))
        TreadmillOn(ctx, base, p, Rd32(base + kChars), 0);
      if (p.kind == "brokenwall" && (at(420) || at(480))) p.cooldown = 0, WallHit(ctx, base, p, 0, 1);
      if (p.kind == "locker" && at(600) && &p == FindKind("locker")) LockerHit(ctx, base, p, f1, 1);
      if (p.kind == "toilet" && at(1100)) ToiletHit(ctx, base, p, f1, 1);
      if (p.kind == "shower" && at(1700) && &p == FindKind("shower")) ShowerHit(ctx, base, p, f1, 1);
      // (rooms 35 / 36: fighter 1 climbs the first lion at 300; the fountain throw at 1100)
      if (p.kind == "lion" && at(300) && &p == FindKind("lion") && !p.who) {
        JumpAct(ctx, base, f1, RoomKey(Param(p, 3)));
        PlaceFighter(base, f1, p.x, p.y, p.z, FighterYaw(p.ry));
        p.who = f1, p.timer = 0;
        REXLOG_INFO("hctp: fighter 1 climbs {} (test)", p.name);
      }
      if (p.kind == "fountainthrow" && at(1100) && &p == FindKind("fountainthrow") && p.has_zone) {
        // (both by the basin first: the throw wants its thrower within 60)
        const float cx = (p.zone[0] + p.zone[1]) / 2, cz = (p.zone[4] + p.zone[5]) / 2;
        PlaceFighter(base, f1, cx, 0.0f, cz + 20.0f, 0.0f);
        if (const uint32_t f0 = Rd32(base + kChars)) PlaceFighter(base, f0, cx, 0.0f, cz + 34.0f, 3.14159265f);
        FountainThrow(ctx, base, p, f1, 1);
      }
      // (room 33: fighter 1 goes through the first shop window at 300, climbs the first pole at 420)
      if (p.kind == "window" && at(300) && &p == FindKind("window")) WindowHit(ctx, base, p, f1, 1);
    }
  }
  if (g_steam_now && frame >= 300 && frame < 302)
    for (Prop& p : Cur().props)
      if (p.kind == "valve" && p.state == 0) TurnValve(ctx, base, 0, -1, p);
  if (g_roof_now && frame >= 300 &&
      std::any_of(Cur().props.begin(), Cur().props.end(), [](const Prop& p) { return p.kind == "roof" && !p.state; }))
    BreakRoof(ctx, base);
  DropIn(base);
  for (Prop& p : Cur().props) {
    if (p.kind == "stack" || p.kind == "throw" || p.kind == "drums") UpdateFall(ctx, base, p);
    if (p.kind == "wall") UpdateWall(ctx, base, p);
    if (p.cooldown > 0 && p.kind != "wall" && p.kind != "valve") --p.cooldown;
  }
  UpdateLater(ctx, base);
  UpdateMoveCar(ctx, base);
  UpdateRoom32(ctx, base);
  UpdateRoom33(ctx, base);
  UpdateForklift(ctx, base);
  UpdateHeli(ctx, base);
}

void InstallHctpOverlay(rex::ui::ImGuiDrawer* drawer) { new DoorPromptOverlay(drawer); }

// A world frame's start (frame_rate.cpp, before the game's update): the held placements.
void HctpBeforeFrame(uint8_t* base) {
  // (nothing outside an HCTP row's match: a held slot from the last match must not write into the next
  // one's wrestlers - they are other objects then)
  if (!HctpRoomActive() || !Cur().loaded) {
    for (Held& h : g_held) h = {};
    return;
  }
  for (Held& h : g_held)
    if (h.frames > 0 && h.f) {
      PlaceBody(base, h.f, h.x, h.y, h.z, h.yaw);
      if (--h.frames == 0) h.f = 0;
    }
}

void HctpWorldVisibility(PPCContext& ctx, uint8_t* base) {
  // (test aid SVR2011_TEST_HCTP_NOVIS=1 / models / objects: the stock visibility only, for all or that part)
  static const char* off = std::getenv("SVR2011_TEST_HCTP_NOVIS");
  static const bool no_models = off && std::strcmp(off, "objects") != 0, no_objects = off && std::strcmp(off, "models") != 0;
  if ((no_models && no_objects) || !HctpRoomActive() || !World() || !Cur().loaded || !Rd32(base + kGimmickMgr)) return;
  const auto saved = ctx;
  const auto models = [&](uint32_t lo, uint32_t hi, bool show) {
    ctx.r3.u64 = lo, ctx.r4.u64 = hi, ctx.r5.u64 = show ? 1 : 0;
    sub_8252FF40(ctx, base);
  };
  if (!no_models) {
    models(0, 199, false);
    models(uint32_t(Cur().model_lo), uint32_t(Cur().model_hi), true);
  }
  // (objects: the stock update shows / hides id ranges by its own rooms - the current room's are
  // as ShowRec left them, the others' hidden)
  for (size_t k = 0; k < g_rooms.size() && !no_objects; ++k)
    for (const Prop& p : g_rooms[k].props)
      for (int rec : p.recs) {
        bool show = k == g_cur;
        if (show)
          if (const auto it = Cur().rec_shown.find(rec); it != Cur().rec_shown.end()) show = it->second;
        const uint32_t e = Element(ctx, base, rec);
        if (!e) continue;
        for (uint32_t at : {100u, 104u})
          if (const uint32_t obj = Rd32(base + e + at)) {
            ctx.r3.u64 = obj, ctx.r4.u64 = show ? 1 : 0;
            sub_824B4198(ctx, base);
          }
      }
  ctx = saved;
}

}  // namespace svr2011
