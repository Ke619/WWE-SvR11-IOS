// The Arena Editor (Mod Maker, arenas branch). See editor.h.
//
// Game space: 1 unit = 10 cm, y points down (floor ~0, ring mat -12). The
// view keeps game space and looks with "up" = -y. Objects are the arena's
// models (its PACH entries); an object added in the editor is a group of
// meshes appended to a model the game always draws (the floor), as the
// Blender import does. Movable objects are static (one bone): their edit
// transform (offset, turn about the vertical, uniform scale, around a pivot
// at the bottom centre) is baked into the vertices whenever it changes.
// Ring parts are placed and drawn by the game's code (heights 3.4 above the
// mat, then 4.2 apart; ropes 28.5 from the centre), so the editor previews
// them the same way and edits them only through the Ring Kit.
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <d3dcompiler.h>
#include <shobjidl.h>

#include "editor.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "imgui.h"
#include "svrfmt/arena_build.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/model_objects.h"
#include "svrfmt/png.h"

namespace fs = std::filesystem;
using namespace svrfmt;

namespace editor {

namespace {

// ---------------------------------------------------------------- math

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) {
  const float l = std::sqrt(Dot(a, a));
  return l > 1e-9f ? a * (1.0f / l) : a;
}

// row-major, row vectors (HLSL row_major + mul(v, M))
struct M4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
      r.m[i * 4 + j] = s;
    }
  return r;
}
M4 LookAt(V3 eye, V3 at, V3 up) {
  const V3 z = Norm(at - eye), x = Norm(Cross(up, z)), y = Cross(z, x);
  M4 r;
  const float v[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  std::memcpy(r.m, v, sizeof v);
  return r;
}
// x flipped: the game's view is left-handed - with the plain LookAt basis the writing on barriers and the stage
// read mirrored next to the game's own shots (as the Full preview's did, 165a6fe). `mirror`
// shows it the other way round. Only the view flips: positions are the game's either way.
M4 Perspective(float fovy, float aspect, float zn, float zf, bool mirror) {
  const float ys = 1.0f / std::tan(fovy / 2), xs = ys / aspect * (mirror ? 1.0f : -1.0f);
  M4 r;
  const float v[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  std::memcpy(r.m, v, sizeof v);
  return r;
}
// yaw about the vertical (game y), then a move
M4 Place(V3 t, float yaw) {
  M4 r;
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float v[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, t.x, t.y, t.z, 1};
  std::memcpy(r.m, v, sizeof v);
  return r;
}
// A corner's turnbuckle link (ar_tb1, 952): the model is one link along x from its post (-32, -32) - x -33.3 ..
// -26.6 at z -32. In game each corner's three run from the post to the corner pad (ar_cover10-13 at +-28, +-28):
// turned 45 degrees about the post, then the corner by quarter turns about the ring's centre (only the quarter turns
// left the links lying along a side, pointing away from the pads - "turnbuckles in the wrong place").
M4 TurnbuckleAt(int corner, float y) {
  constexpr float kPost = -32.0f;
  const float d = -0.78539816f;  // (+x -> the diagonal into the ring)
  const float cd = std::cos(d), sd = std::sin(d);
  const float ox = kPost - (kPost * cd + kPost * sd), oz = kPost - (-kPost * sd + kPost * cd);  // post - R(d) post
  const float a = float(corner) * 1.57079633f, ca = std::cos(a), sa = std::sin(a);
  return Place({ox * ca + oz * sa, y, -ox * sa + oz * ca}, a + d);
}

bool Invert(const M4& a, M4& out) {
  const float* m = a.m;
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) return false;
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) out.m[i] = inv[i] * det;
  return true;
}
// row vector * M, with the divide
V3 Project(V3 p, const M4& m, float* w_out = nullptr) {
  const float x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12];
  const float y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13];
  const float z = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14];
  const float w = p.x * m.m[3] + p.y * m.m[7] + p.z * m.m[11] + m.m[15];
  if (w_out) *w_out = w;
  return {x / w, y / w, z / w};
}

constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------- ring

constexpr float kMatY = -12.0f, kGameBase = 3.4f, kGameGap = 4.2f, kRopeOut = 28.5f, kApronOut = 31.9f;
constexpr uint32_t kSideRope[4] = {962, 961, 960, 963};

enum class Role { kNormal, kRopeSide, kRopePerRope, kPad, kTurnbuckle, kApron, kNotDrawn };

// rope model -> its side's offset from the centre (the model lies along one axis through 0)
V3 RopeOffset(uint32_t side_model) {
  switch (side_model) {
    case 960: return {0, 0, kRopeOut};
    case 961: return {-kRopeOut, 0, 0};
    case 962: return {0, 0, -kRopeOut};
    default: return {kRopeOut, 0, 0};  // 963
  }
}

// ---------------------------------------------------------------- state

struct Obj {
  int model = -1;
  std::vector<int> meshes;
  std::string label;
  Zone zone = Zone::kFree;
  Role role = Role::kNormal;
  bool movable = false, added = false, hidden = false;
  bool glow = false;  // light glows / beams: drawn additive
  // moved by the arena's own animation (its flag table, entry 50001: "m(<n>)"):
  // light rigs, beams, glows. Their meshes sit at the origin until the game
  // animates them into place, so the view doesn't draw them (they'd pile up
  // in the ring); they stay in the arena as they are.
  bool effect = false;
  // drawn twice by the game: as it is and mirrored across x = 0 (flag "r";
  // half an arena's fences, stands and truss are stored once)
  bool mirrored = false;
  // another area of a multi-area file (backstage: bg78 holds seven rooms):
  // not shown, not listed, kept as it is
  bool outside = false;
  bool ceiling = false;  // a room's ceiling / lights rig (backstage): hidden in the view unless "Show ceilings"
  // its flag table line (stage lights): l(<n>) the light set it is lit by (100 without), m(<n>) the
  // object motion that places it, "e" entrance only, "g" match only, "b" billboard
  int light_set = 100, motion = -1;
  bool entrance_only = false, match_only = false, billboard = false;
  V3 pos;           // edit offset
  float yaw = 0;    // radians (turn about the up axis)
  float pitch = 0, roll = 0;  // radians: tilted forward / sideways (applied before the turn)
  float scale = 1;
  V3 pivot;         // bottom centre of the original geometry
  std::vector<std::vector<Vertex>> base;
  std::vector<std::vector<Strip>> strips;
};

struct Snapshot {
  std::vector<std::array<float, 8>> xf;  // pos, yaw, scale, hidden, pitch, roll
  std::vector<std::string> tex;          // each object's texture (first mesh)
};

struct GpuMesh {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT count = 0;
  std::string tex;
};

struct GpuVertex {
  float pos[3], n[3];
  uint8_t rgba[4];
  float uv[2];
};

struct Cb {
  float world[16], viewproj[16];
  float tint[4], flat[4];
};

Hooks g_hooks;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11Buffer* g_cb_mode = nullptr;   // (b1: mode.x 1 = self-lit, the textures in g_self_lit)
ID3D11Buffer* g_cb_light = nullptr;  // (b2: the arena's light set for the next draws - SetStageLight)
int g_cb_light_now = -2;
int g_cb_mode_now = -1;
bool g_keep_blend = false;  // (the glow pass: its additive blend stays, whatever the model's own kind)
std::set<std::string> g_self_lit;    // (SetSelfLit: screens drawn at their own brightness)
std::set<std::string> g_led_mask;    // (SetSelfLit's masks: a screen's dot grid over it, its holes the lit dots)
ID3D11BlendState* g_led_blend = nullptr;
// The game's see-through rules, from each model's C351 flags (on in every view; env SVR2011_MM_NO_GAME_ALPHA: the old
// cut-everything look, for A/B shots): s / k / i / j blended, a / o added, c cut out, none opaque. Every texture's
// alpha cut at 0.25 turned the cage trusses (s) of Hell in a Cell into blotchy holes, and WWE barriers (no flag, a
// gloss mask in the alpha) see-through.
bool g_game_alpha = !std::getenv("SVR2011_MM_NO_GAME_ALPHA");
const svrfmt::Arena* g_cut_arena = nullptr;
std::vector<char> g_cut;             // (per model: 0 opaque, 1 cut out, 2 blended, 3 added)
std::map<uint32_t, std::string> ModelFlagsOf(const svrfmt::Arena& arena);
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_solid = nullptr;
ID3D11RasterizerState* g_wire = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11DepthStencilState* g_depth_ro = nullptr;  // (glows: test, no write)
ID3D11BlendState* g_add = nullptr;               // (glows: additive)
ID3D11ShaderResourceView* g_white = nullptr;
ID3D11ShaderResourceView* g_screen_off = nullptr;  // (MovieSurface: a screen that is off)
// the viewport's target
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_rt_srv = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;
int g_rt_w = 0, g_rt_h = 0;

Arena* g_arena = nullptr;
std::string g_title;
std::vector<Obj> g_objs;
std::map<std::pair<int, int>, GpuMesh> g_gpu;  // (model, mesh)
std::map<std::string, ID3D11ShaderResourceView*> g_tex;
std::vector<bool> g_model_dirty;
int g_sel = -1;
std::vector<int> g_multi;  // the other selected objects (Ctrl+click), besides g_sel
RingSpec g_ring;
Lighting g_light;
std::vector<Snapshot> g_undo, g_redo;
bool g_grid = true;
float g_snap_step = 5;  // units (1 = 10 cm)
bool g_frame_all = false;
int g_test_view = -1;  // (TestStart)
std::string g_test_select;
int g_file_from = 0;  // Add object from a file: 0 any 3D program in metres, 1 in centimetres, 2 Export to Blender
// (test aids on the --select object: --test-add-object <file>[,<from>[,<x>,<up>,<z> metres]], --test-export-object
// <dir>, --test-remove-part <n> - run instead of the test edit by --test-edit-save)
std::string g_test_add_object, g_test_export_dir;
int g_test_remove_part = -1;
std::string g_test_picture;  // (test aid --test-picture: put on the --select object as "Use a picture..." does)

// camera
V3 g_target{0, -12, 0};
int g_area_lo = -1;  // SetArea: >= 0 an area is shown (its model ids: g_area)
std::vector<std::pair<int, int>> g_area;
bool InArea(uint32_t id) {
  for (const auto& [lo, hi] : g_area)
    if (id >= uint32_t(lo) && id <= uint32_t(hi)) return true;
  return false;
}
bool g_show_ceilings = false;
// SetArea: a spot in the room the game's cameras see (TestEdit puts its box
// there); half x 0: none
float g_spot[5] = {};
float g_yaw = 0.6f, g_pitch = 0.5f, g_dist = 180;
bool g_mirror = false;
// +1 / -1: which way the camera's x shows on screen (drags and arrow keys follow the screen, mirrored or not)
float ScreenX() { return g_mirror ? 1.0f : -1.0f; }
bool g_show_ring = true;
M4 g_viewproj;
ImVec2 g_vp_min, g_vp_size;

enum class Tool { kSelect, kMove, kRotate, kScale };
Tool g_tool = Tool::kMove;
bool g_snap = true;
float g_turn_snap = kPi / 12;  // (radians; 0: free)
constexpr float kMaxMove = 10000.0f;  // (game units, 10 cm: 1 km - a sanity bound, not the game's)
// a drag in the viewport
int g_drag_axis = -1;  // -1 none, 0..2 axis, 3 plane
V3 g_drag_start_pos;
float g_drag_start_yaw = 0, g_drag_start_scale = 1, g_drag_start_pitch = 0, g_drag_start_roll = 0;
V3 g_drag_hit;
ImVec2 g_drag_mouse;

// budget
std::atomic<int> g_edits{0};
int g_budget_edits = -1;
std::chrono::steady_clock::time_point g_last_edit;
std::thread g_budget_thread;
std::atomic<bool> g_budget_busy{false};
std::mutex g_budget_mutex;
size_t g_budget_file = 0, g_budget_unpacked = 0, g_budget_file_max = 0, g_budget_unpacked_max = 0;
size_t g_budget_biggest = 0;
uint32_t g_budget_biggest_id = 0;

bool g_readonly = false;  // (the view scene: camera only)
bool g_quiet = false;  // (budget builds)
bool g_glow_pass = false;

void Log(const std::string& s) {
  if (g_hooks.log && !g_quiet) g_hooks.log(s);
}

std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

void Edited() {
  ++g_edits;
  g_last_edit = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------- GPU

const char* kShader = R"(
cbuffer C : register(b0) {
  row_major float4x4 world;
  row_major float4x4 viewproj;
  float4 tint;
  float4 flat;
};
cbuffer D : register(b1) { float4 mode; };
cbuffer L : register(b2) { float4 ldir[6]; float4 lcol[6]; float4 lon; };  // (the arena's light set: 2 fills, 4 directional)
struct VI { float3 p : POSITION; float3 n : NORMAL; float4 c : COLOR; float2 uv : TEXCOORD; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float4 c : COLOR; float2 uv : TEXCOORD; };
VO vs(VI i) {
  VO o;
  float4 w = mul(float4(i.p, 1), world);
  o.p = mul(w, viewproj);
  o.n = mul(i.n, (float3x3)world);
  o.c = i.c;
  o.uv = i.uv;
  return o;
}
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 ps(VO i) : SV_Target {
  if (flat.w > 0.5) return float4(flat.rgb, 1);
  float4 tx = t.Sample(s, i.uv);
  if (flat.w < -0.5) return float4(tx.rgb * tx.a * i.c.rgb * tint.rgb, 0);  // (glows and beams, added at full strength as the game - 0.6 left them faint)
  if (mode.x > 1.5) return float4(tint.rgb * 0.55, 1 - tx.a);  // (an LED mask: its holes are lit dots, blended)
  // mode.z: the model's see-through kind (its C351 flags): 0 opaque or cut-out, 1 blended (s k i j), 2 added (a o)
  if (mode.z < 0.5) { if (tx.a < 0.25 && mode.y < 0.5) discard; }  // (mode.y: opaque - the game's alpha there isn't see-through)
  else if (tx.a < 0.004) discard;
  float3 c;
  if (mode.x > 0.5) c = tx.rgb * tint.rgb;  // (self-lit: a screen)
  else {
    float3 n = normalize(i.n + 1e-5);
    if (lon.x > 0.5) {  // (the arena's own light set: fills half-lit from behind, directional ones as they face)
      float3 l = lcol[0].rgb * (0.5 + 0.5 * dot(n, ldir[0].xyz)) + lcol[1].rgb * (0.5 + 0.5 * dot(n, ldir[1].xyz));
      [unroll] for (int k = 2; k < 6; ++k) l += lcol[k].rgb * max(0, dot(n, ldir[k].xyz));
      c = tx.rgb * i.c.rgb * tint.rgb * l * lon.y;
    } else {
      float d = abs(dot(n, normalize(float3(0.35, -1.0, 0.25))));
      c = tx.rgb * i.c.rgb * tint.rgb * (0.55 + 0.45 * d);
    }
  }
  if (mode.z > 1.5) return float4(c * tx.a, 0);
  return float4(c, mode.z > 0.5 ? tx.a : 1);
}
)";

bool CreatePipeline() {
  ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), "editor", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kShader, std::strlen(kShader), "editor", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
    if (err) Log(std::string("editor shader: ") + static_cast<const char*>(err->GetBufferPointer()));
    return false;
  }
  g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
  g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
  const D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  g_dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
  vsb->Release();
  psb->Release();
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(Cb);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_dev->CreateBuffer(&bd, nullptr, &g_cb);
  bd.ByteWidth = 16;
  g_dev->CreateBuffer(&bd, nullptr, &g_cb_mode);
  bd.ByteWidth = 13 * 16;
  g_dev->CreateBuffer(&bd, nullptr, &g_cb_light);
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 8;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  g_dev->CreateSamplerState(&sd, &g_sampler);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  g_dev->CreateRasterizerState(&rd, &g_solid);
  rd.FillMode = D3D11_FILL_WIREFRAME;
  rd.DepthBias = -100;
  rd.SlopeScaledDepthBias = -1.0f;
  g_dev->CreateRasterizerState(&rd, &g_wire);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  g_dev->CreateDepthStencilState(&dd, &g_depth);
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  g_dev->CreateDepthStencilState(&dd, &g_depth_ro);
  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  g_dev->CreateBlendState(&bl, &g_add);
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;  // (LED masks: dest x (1 - a) + dot colour x a)
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  g_dev->CreateBlendState(&bl, &g_led_blend);
  // a white texture for meshes without one
  const uint32_t white = 0xFFFFFFFFu;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 1;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd = {&white, 4, 0};
  ID3D11Texture2D* tex = nullptr;
  g_dev->CreateTexture2D(&td, &srd, &tex);
  g_dev->CreateShaderResourceView(tex, nullptr, &g_white);
  tex->Release();
  const uint32_t off = 0xFF1A1816u;  // (a movie screen that is off: near black)
  srd.pSysMem = &off;
  g_dev->CreateTexture2D(&td, &srd, &tex);
  g_dev->CreateShaderResourceView(tex, nullptr, &g_screen_off);
  tex->Release();
  return true;
}

void ResizeTarget(int w, int h) {
  if (w == g_rt_w && h == g_rt_h && g_rt) return;
  if (g_rt_srv) g_rt_srv->Release(), g_rt_srv = nullptr;
  if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
  if (g_rt) g_rt->Release(), g_rt = nullptr;
  if (g_dsv) g_dsv->Release(), g_dsv = nullptr;
  g_rt_w = w, g_rt_h = h;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  g_dev->CreateTexture2D(&td, nullptr, &g_rt);
  g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv);
  g_dev->CreateShaderResourceView(g_rt, nullptr, &g_rt_srv);
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth = nullptr;
  g_dev->CreateTexture2D(&td, nullptr, &depth);
  g_dev->CreateDepthStencilView(depth, nullptr, &g_dsv);
  depth->Release();
}

std::map<std::string, ID3D11ShaderResourceView*> g_tex_override;  // (SetTextureOverride: the Full preview's titantron)

// The texture a model names, in the arena's sets: as written, else whatever the case - the game finds them so
// (the practice arena's Sheat_A seats ask for st_sheata / st_sheata_blue, its set has st_sheatA / st_sheatA_Blue;
// RAW's billboards rawtest02 for RAWtest02) - drawn white they were the "white stands".
const BundleTexture* FindTextureAnyCase(const std::string& name) {
  if (!g_arena) return nullptr;
  if (const BundleTexture* t = g_arena->FindTexture(name)) return t;
  auto low = [](std::string x) {
    for (char& c : x) c = char(std::tolower(uint8_t(c)));
    return x;
  };
  const std::string want = low(name);
  for (const auto& b : g_arena->bundles)
    for (const auto& t : b.textures)
      if (low(t.name) == want) return &t;
  return nullptr;
}

// Surfaces the game fills with a movie at run time (no such texture in any pac): RAW's screens titantron_movie and
// arena_movie (the superstar's movies	itantron\<id>.bik: 320 x 320, the titantron above, a name strip below).
// Without a movie set on them (SetTextureOverride) they are drawn as a screen that is off, not white.
bool MovieSurface(const std::string& name) { return name == "titantron_movie" || name == "arena_movie"; }

ID3D11ShaderResourceView* TextureFor(const std::string& name) {
  if (auto o = g_tex_override.find(name); o != g_tex_override.end() && o->second) return o->second;
  if (name.empty()) return g_white;
  if (MovieSurface(name) && g_screen_off) return g_screen_off;
  auto it = g_tex.find(name);
  if (it != g_tex.end()) return it->second ? it->second : g_white;
  ID3D11ShaderResourceView* srv = nullptr;
  if (const BundleTexture* t = FindTextureAnyCase(name)) {
    DdsInfo info;
    if (DdsInfoOf(t->data, info)) {
      DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
      int block = 0;
      switch (info.format) {
        case DxtFormat::kDxt1: f = DXGI_FORMAT_BC1_UNORM, block = 8; break;
        case DxtFormat::kDxt3: f = DXGI_FORMAT_BC2_UNORM, block = 16; break;
        case DxtFormat::kDxt5: f = DXGI_FORMAT_BC3_UNORM, block = 16; break;
        case DxtFormat::kArgb: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        default: break;
      }
      std::vector<D3D11_SUBRESOURCE_DATA> levels;
      size_t off = 128;
      int w = info.w, h = info.h;
      for (int l = 0; l < info.mips && f != DXGI_FORMAT_UNKNOWN; ++l) {
        const size_t pitch = block ? size_t(std::max(1, (w + 3) / 4)) * block : size_t(w) * 4;
        const size_t rows = block ? size_t(std::max(1, (h + 3) / 4)) : size_t(h);
        if (off + pitch * rows > t->data.size()) break;
        levels.push_back({t->data.data() + off, UINT(pitch), 0});
        off += pitch * rows;
        w = std::max(1, w / 2), h = std::max(1, h / 2);
      }
      if (!levels.empty()) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(info.w);
        td.Height = UINT(info.h);
        td.MipLevels = UINT(levels.size());
        td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, levels.data(), &tex))) {
          g_dev->CreateShaderResourceView(tex, nullptr, &srv);
          tex->Release();
        }
      }
    }
  }
  g_tex[name] = srv;
  return srv ? srv : g_white;
}

void ForgetTextures() {
  for (auto& [n, s] : g_tex)
    if (s) s->Release();
  g_tex.clear();
}

void ForgetGpu(int model = -1) {
  for (auto it = g_gpu.begin(); it != g_gpu.end();) {
    if (model >= 0 && it->first.first != model) { ++it; continue; }
    if (it->second.vb) it->second.vb->Release();
    if (it->second.ib) it->second.ib->Release();
    it = g_gpu.erase(it);
  }
}

std::string ObjTexture(const Obj& o) {
  if (o.meshes.empty() || !g_arena) return "";
  const Model& m = g_arena->models[o.model].model;
  const int slot = DiffuseSlot(m.meshes[o.meshes[0]]);
  return slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "";
}
void SetObjTexture(Obj& o, const std::string& name);
bool Selected(int i) { return i == g_sel || std::find(g_multi.begin(), g_multi.end(), i) != g_multi.end(); }
// every selected object (the primary first)
std::vector<int> Selection() {
  std::vector<int> v;
  if (g_sel >= 0 && g_sel < int(g_objs.size())) v.push_back(g_sel);
  for (int i : g_multi)
    if (i >= 0 && i < int(g_objs.size()) && i != g_sel) v.push_back(i);
  return v;
}
void SelectOne(int i) { g_sel = i, g_multi.clear(); }
void ToggleSelect(int i) {
  if (i < 0) return;
  if (g_sel < 0) { g_sel = i; return; }
  if (i == g_sel) {  // the primary goes: the next one becomes primary
    g_sel = g_multi.empty() ? -1 : g_multi.front();
    if (!g_multi.empty()) g_multi.erase(g_multi.begin());
    return;
  }
  auto it = std::find(g_multi.begin(), g_multi.end(), i);
  if (it == g_multi.end()) g_multi.push_back(i);
  else g_multi.erase(it);
}

GpuMesh* Gpu(int model, int mesh) {
  auto key = std::make_pair(model, mesh);
  auto it = g_gpu.find(key);
  if (it != g_gpu.end()) return &it->second;
  const Model& m = g_arena->models[model].model;
  const Mesh& s = m.meshes[mesh];
  GpuMesh g;
  const int slot = DiffuseSlot(s);
  g.tex = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : (m.textures.empty() ? "" : m.textures[0]);
  std::vector<GpuVertex> vs(s.verts.size());
  for (size_t i = 0; i < s.verts.size(); ++i) {
    const Vertex& v = s.verts[i];
    GpuVertex& o = vs[i];
    std::memcpy(o.pos, v.pos, 12);
    std::memcpy(o.n, v.normal, 12);
    o.rgba[0] = uint8_t(v.color >> 16), o.rgba[1] = uint8_t(v.color >> 8), o.rgba[2] = uint8_t(v.color);
    o.rgba[3] = uint8_t(v.color >> 24);
    o.uv[0] = i < s.uvs.size() ? s.uvs[i][0] : 0;
    o.uv[1] = i < s.uvs.size() ? s.uvs[i][1] : 0;
  }
  std::vector<uint16_t> idx;
  for (const auto& st : s.strips)
    for (const auto& t : StripToTriangles(st.indices))
      if (t[0] < vs.size() && t[1] < vs.size() && t[2] < vs.size()) idx.insert(idx.end(), t.begin(), t.end());
  g.count = UINT(idx.size());
  if (!vs.empty() && !idx.empty()) {
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = UINT(vs.size() * sizeof(GpuVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {vs.data(), 0, 0};
    g_dev->CreateBuffer(&bd, &sd, &g.vb);
    bd.ByteWidth = UINT(idx.size() * 2);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idx.data();
    g_dev->CreateBuffer(&bd, &sd, &g.ib);
  }
  return &(g_gpu[key] = g);
}

void SetCb(const M4& world, const float tint[3], const float* flat = nullptr) {
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
  Cb* c = static_cast<Cb*>(ms.pData);
  std::memcpy(c->world, world.m, 64);
  std::memcpy(c->viewproj, g_viewproj.m, 64);
  c->tint[0] = tint[0], c->tint[1] = tint[1], c->tint[2] = tint[2], c->tint[3] = 1;
  if (flat) c->flat[0] = flat[0], c->flat[1] = flat[1], c->flat[2] = flat[2], c->flat[3] = 1;
  else c->flat[3] = g_glow_pass ? -1.0f : 0.0f;
  g_ctx->Unmap(g_cb, 0);
}

// A field's width that leaves room for `label` (the widest of a group, so the fields line up) at any font size.
float LabelRoom(const char* label) {
  return ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x + 2;
}

int AlphaKind(int model) {
  if (!g_arena || model < 0) return 1;
  if (!g_game_alpha) return 1;
  if (g_cut_arena != g_arena) {
    g_cut_arena = g_arena;
    g_cut.assign(g_arena->models.size(), 0);
    const auto flags = ModelFlagsOf(*g_arena);
    for (size_t i = 0; i < g_arena->models.size(); ++i) {
      const auto f = flags.find(g_arena->models[i].id);
      if (f == flags.end()) {
        g_cut[i] = 1;  // (no line - an added object, a ring part: as before)
        continue;
      }
      for (const char* w : {" c "}) if (f->second.find(w) != std::string::npos) g_cut[i] = 1;
      for (const char* w : {" s ", " k ", " i ", " j "}) if (f->second.find(w) != std::string::npos) g_cut[i] = 2;
      for (const char* w : {" a ", " o "}) if (f->second.find(w) != std::string::npos) g_cut[i] = 3;
    }
  }
  return size_t(model) < g_cut.size() ? g_cut[size_t(model)] : 1;
}

void DrawMesh(int model, int mesh) {
  GpuMesh* g = Gpu(model, mesh);
  if (!g->vb || !g->count) return;
  const UINT stride = sizeof(GpuVertex), offset = 0;
  g_ctx->IASetVertexBuffers(0, 1, &g->vb, &stride, &offset);
  g_ctx->IASetIndexBuffer(g->ib, DXGI_FORMAT_R16_UINT, 0);
  ID3D11ShaderResourceView* srv = TextureFor(g->tex);
  g_ctx->PSSetShaderResources(0, 1, &srv);
  const int lit = g_led_mask.count(g->tex) ? 2 : g_self_lit.count(g->tex) ? 1 : 0;
  const int kind = lit == 2 ? 1 : AlphaKind(model);  // (0 opaque, 1 cut out, 2 blended, 3 added)
  const int mode = lit * 4 + kind;
  if (mode != g_cb_mode_now && g_cb_mode && !g_glow_pass) {
    const auto blend = [](int m) -> ID3D11BlendState* {
      return m / 4 == 2 || m % 4 == 2 ? g_led_blend : m % 4 == 3 ? g_add : nullptr;
    };
    if (!g_keep_blend && (g_cb_mode_now < 0 || blend(mode) != blend(g_cb_mode_now)))
      g_ctx->OMSetBlendState(blend(mode), nullptr, 0xFFFFFFFF);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_cb_mode, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      const float v[4] = {float(lit), kind == 1 ? 0.0f : 1.0f, kind == 2 ? 1.0f : kind == 3 ? 2.0f : 0.0f, 0};
      std::memcpy(ms.pData, v, sizeof v);
      g_ctx->Unmap(g_cb_mode, 0);
      g_cb_mode_now = mode;
    }
  }
  g_ctx->DrawIndexed(g->count, 0, 0);
}

// ---------------------------------------------------------------- objects

float RopeBase() { return g_ring.rope_base > 0 ? g_ring.rope_base : kGameBase; }
float RopeGap() { return g_ring.rope_gap > 0 ? g_ring.rope_gap : kGameGap; }
float RopeY(int k) { return kMatY - RopeBase() - k * RopeGap(); }

void LightTint(float out[3]) {
  for (int k = 0; k < 3; ++k) out[k] = g_light.color[k] * g_light.strength;
}

bool IsStatic(const Mesh& s) { return s.palette.size() <= 1 && s.weights.size() <= 1; }

void ComputePivot(Obj& o) {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& vs : o.base)
    for (const auto& v : vs)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  if (lo[0] > hi[0]) return;
  o.pivot = {(lo[0] + hi[0]) / 2, hi[1], (lo[2] + hi[2]) / 2};  // y down: hi = bottom
}

// The edit transform into the model's vertices (and strips, for hidden).
void ApplyObj(Obj& o) {
  Model& m = g_arena->models[o.model].model;
  // R = yaw (about the up axis) * pitch (about x) * roll (about z): game axes (y down)
  const float cy = std::cos(o.yaw), sy = std::sin(o.yaw), cp = std::cos(o.pitch), sp = std::sin(o.pitch),
              cr = std::cos(o.roll), sr = std::sin(o.roll);
  auto rot = [&](float x, float y, float z, float* out) {
    const float x1 = x * cr - y * sr, y1 = x * sr + y * cr;
    const float y2 = y1 * cp - z * sp, z2 = y1 * sp + z * cp;
    out[0] = x1 * cy + z2 * sy, out[1] = y2, out[2] = -x1 * sy + z2 * cy;
  };
  for (size_t k = 0; k < o.meshes.size(); ++k) {
    Mesh& mesh = m.meshes[o.meshes[k]];
    if (o.movable) {
      const auto& base = o.base[k];
      mesh.verts.resize(base.size());
      for (size_t i = 0; i < base.size(); ++i) {
        Vertex v = base[i];
        float d[3], n[3];
        rot((v.pos[0] - o.pivot.x) * o.scale, (v.pos[1] - o.pivot.y) * o.scale, (v.pos[2] - o.pivot.z) * o.scale, d);
        v.pos[0] = o.pivot.x + o.pos.x + d[0];
        v.pos[1] = o.pivot.y + o.pos.y + d[1];
        v.pos[2] = o.pivot.z + o.pos.z + d[2];
        rot(v.normal[0], v.normal[1], v.normal[2], n);
        v.normal[0] = n[0], v.normal[1] = n[1], v.normal[2] = n[2];
        mesh.verts[i] = v;
      }
      // bounding sphere
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (const auto& v : mesh.verts)
        for (int j = 0; j < 3; ++j) lo[j] = std::min(lo[j], v.pos[j]), hi[j] = std::max(hi[j], v.pos[j]);
      if (!mesh.verts.empty()) {
        const float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
        float r = 0;
        for (const auto& v : mesh.verts)
          r = std::max(r, (v.pos[0] - cx) * (v.pos[0] - cx) + (v.pos[1] - cy) * (v.pos[1] - cy) +
                              (v.pos[2] - cz) * (v.pos[2] - cz));
        mesh.sphere = {cx, cy, cz, std::sqrt(r)};
      }
    }
    if (o.hidden) {
      for (auto& st : mesh.strips) st.indices.clear();
    } else {
      mesh.strips = o.strips[k];
    }
  }
  g_arena->models[o.model].changed = true;
  ForgetGpu(o.model);
  Edited();
  if (o.movable) FitNodeSpheres(m);  // (the game culls by the node spheres: svrfmt/arena.h)
}

Role RoleOf(uint32_t id) {
  if (id >= 960 && id <= 963) return Role::kRopeSide;
  if (id >= 900 && id <= 911) return Role::kRopePerRope;
  if (id >= 964 && id <= 967) return Role::kPad;
  if (id == 952) return Role::kTurnbuckle;
  if (id == 973 || id == 974) return Role::kApron;
  if ((id >= 956 && id <= 959) || id == 40200) return Role::kNotDrawn;
  return Role::kNormal;
}

std::string Label(const ArenaModel& am) {
  char b[64];
  std::snprintf(b, sizeof b, "%s  (%X)", am.model.name.c_str(), am.id);
  return b;
}

// The arena's per-model flag table (entry 50001, text: "<model id> <flags>"):
// "m(<n>)" placed by animation track n, "r" also drawn mirrored (x -> -x).
std::map<uint32_t, std::string> ModelFlagsOf(const svrfmt::Arena& arena) {
  std::map<uint32_t, std::string> flags;
  for (const auto& e : arena.entries) {
    if (e.id != 50001) continue;
    const Bytes t = Unpack(e.data);
    std::string text(t.begin(), t.end()), line;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t nl = text.find('\n', pos);
      line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? text.size() : nl + 1;
      if (line.empty() || !std::isdigit(uint8_t(line[0]))) continue;
      const uint32_t id = uint32_t(std::atoi(line.c_str()));
      flags[id] = " " + line.substr(line.find_first_not_of("0123456789")) + " ";
    }
  }
  return flags;
}
std::map<uint32_t, std::string> ModelFlags() { return g_arena ? ModelFlagsOf(*g_arena) : std::map<uint32_t, std::string>(); }

// ---------------------------------------------------------------- stage lights
// What the game lights an arena with, from the arena's own entries:
// - 0x7530 (30000): a YMKs bank of light sets. Per set id, channels (the YMKs y) 0 and 1 = two
//   opposite fill lights (the ground / sky colours), 10-13 = up to four directional lights, 50-59 =
//   the moving crowd lights (set 20 only). Keys, 25 bytes: u8 frames to the next key, f32x3 position
//   (x 0.01; the light comes from there towards the centre), s16x3 rotation, u16x3 colour (/ 4096).
// - 0xC351 l(<n>): the set a model is lit by; a model without one gets set 100.
// - 0x7531 (30001): object motions (YMKs, no header), one per m(<n>) of 0xC351, a 29-byte record a
//   frame: u8, f32x3 position (x 0.01), f32x3 rotation (radians, x then y then z), f32 scale. They
//   put the beams, glows and light rigs (modelled at the centre) in place, 60 frames a second.
struct StageKey {
  int frame = 0;
  float v[9] = {};  // lights: position, rotation, colour; motions: position, rotation, scale
};
struct StageTrack {
  int frames = 1;
  std::vector<StageKey> keys;
};
using StageBank = std::map<std::pair<int, int>, StageTrack>;  // (id, channel) -> track
StageBank g_light_bank, g_motions;
const svrfmt::Arena* g_stage_of = nullptr;
float g_stage_time = 0;          // seconds (SetStageTime: the Full preview's clock; the editor: still)
bool g_stage_entrance = false;   // the entrance's look: "e" models shown, "g" ones not
bool g_stage_lights = !std::getenv("SVR2011_MM_NO_STAGE_LIGHTS");  // (the lighting panel's; the env: an A/B test shot)

float LeF32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}
uint32_t LeU32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t LeU16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// A YMKs bank (the 16-byte "YMKs" head optional: the 2007-2009 banks and 0x7531 have none) -> its
// tracks. motion: 0x7531's 29-byte records, else 0x7530's 25-byte light keys.
StageBank ReadStageBank(const Bytes& raw, bool motion) {
  StageBank out;
  size_t p = raw.size() >= 16 && !std::memcmp(raw.data(), "YMKs", 4) ? 16 : 0;
  if (raw.size() < p + 0x104) return out;
  const uint32_t n = LeU32(raw.data() + p + 0x100);
  const size_t dir = p + 0x104, base = dir + 16 * size_t(n);
  if (n > 4096 || base > raw.size()) return out;
  const size_t pose = motion ? 29 : 25;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* d = raw.data() + dir + 16 * i;
    const int y = d[0], id = LeU16(d + 2);
    const size_t off = LeU32(d + 4);
    StageTrack t;
    t.frames = int(std::max<uint32_t>(1, LeU32(d + 8)));
    size_t q = base + off;
    for (int f = 0; q < raw.size() && f < t.frames;) {
      const uint8_t b = raw[q];
      if (b < 0x60) {
        f += b, ++q;
        continue;
      }
      size_t sz = b - 0x60;
      if (b == 0x60) {
        if (q + 3 > raw.size()) break;
        sz = LeU16(raw.data() + q + 1), q += 3;
      } else {
        ++q;
      }
      if (q + sz > raw.size()) break;
      const uint8_t* r = raw.data() + q;
      size_t rs = sz;
      if (rs && (r[0] & 0x80)) {  // (an event block first)
        const size_t ev = 1 + (r[0] & 0x7f);
        r += std::min(ev, rs), rs -= std::min(ev, rs);
      }
      if (rs >= pose) {
        StageKey k;
        k.frame = f;
        for (int a = 0; a < 3; ++a) k.v[a] = LeF32(r + 1 + 4 * a) * 0.01f;
        if (motion) {
          for (int a = 0; a < 3; ++a) k.v[3 + a] = LeF32(r + 13 + 4 * a);
          k.v[6] = LeF32(r + 25);
        } else {
          for (int a = 0; a < 3; ++a) k.v[3 + a] = int16_t(LeU16(r + 13 + 2 * a)) * (2 * kPi / 65536.0f);
          for (int a = 0; a < 3; ++a) k.v[6 + a] = LeU16(r + 19 + 2 * a) / 4096.0f;
        }
        t.keys.push_back(k);
      }
      q += sz, ++f;
    }
    if (!t.keys.empty()) out[{id, y}] = std::move(t);
  }
  return out;
}

// a track's values at the stage clock (looped; linear between keys)
void SampleTrack(const StageTrack& t, float out[9]) {
  const float f = std::fmod(g_stage_time * 60.0f, float(t.frames));
  size_t k = 0;
  while (k + 1 < t.keys.size() && float(t.keys[k + 1].frame) <= f) ++k;
  const StageKey& a = t.keys[k];
  if (k + 1 >= t.keys.size() || t.keys[k + 1].frame <= a.frame) {
    std::memcpy(out, a.v, sizeof a.v);
    return;
  }
  const StageKey& b = t.keys[k + 1];
  const float u = std::clamp((f - float(a.frame)) / float(b.frame - a.frame), 0.0f, 1.0f);
  for (int i = 0; i < 9; ++i) out[i] = a.v[i] + (b.v[i] - a.v[i]) * u;
}

void LoadStage() {
  if (g_stage_of == g_arena) return;
  g_stage_of = g_arena;
  g_light_bank.clear(), g_motions.clear();
  if (!g_arena) return;
  for (const auto& e : g_arena->entries) {
    if (e.id == 30000) g_light_bank = ReadStageBank(Unpack(e.data), false);
    if (e.id == 30001) g_motions = ReadStageBank(Unpack(e.data), true);
  }
}

// l(<n>), m(<n>), e / g / b of a flag table line (" ... ")
void ReadStageFlags(const std::string& f, Obj& o) {
  if (const size_t l = f.find(" l("); l != std::string::npos) o.light_set = std::atoi(f.c_str() + l + 3);
  if (const size_t m = f.find(" m("); m != std::string::npos) o.motion = std::atoi(f.c_str() + m + 3);
  auto word = [&](const char* w) {
    for (size_t at = f.find(w); at != std::string::npos; at = f.find(w, at + 1))
      if (at > 0 && std::isspace(uint8_t(f[at - 1])) && at + 1 < f.size() && std::isspace(uint8_t(f[at + 1]))) return true;
    return false;
  };
  o.entrance_only = word("e"), o.match_only = word("g"), o.billboard = word("b");
}

// the light set's values now: two fills and four directional lights (direction to the light, colour)
struct StageLight {
  float dir[6][4] = {};  // xyz (towards the light), w unused
  float col[6][4] = {};  // rgb
};
bool StageLightOf(int set, StageLight& out) {
  static const int kCh[6] = {0, 1, 10, 11, 12, 13};
  bool any = false;
  for (int c = 0; c < 6; ++c) {
    const auto t = g_light_bank.find({set, kCh[c]});
    if (t == g_light_bank.end()) continue;
    float v[9];
    SampleTrack(t->second, v);
    const V3 d = Norm({v[0], v[1], v[2]});
    out.dir[c][0] = d.x, out.dir[c][1] = d.y, out.dir[c][2] = d.z;
    out.col[c][0] = v[6], out.col[c][1] = v[7], out.col[c][2] = v[8];
    any = true;
  }
  return any;
}

// an effect model's place now (its m(<n>) motion): scale, rotation x then y then z, the move
bool MotionPlace(int motion, M4& out) {
  const auto t = g_motions.find({motion, 0});
  if (t == g_motions.end()) return false;
  float v[9];
  SampleTrack(t->second, v);
  const float s = v[6] != 0 ? v[6] : 1.0f;
  const float cx = std::cos(v[3]), sx = std::sin(v[3]), cy = std::cos(v[4]), sy = std::sin(v[4]), cz = std::cos(v[5]),
              sz = std::sin(v[5]);
  // row vectors: p * Rx * Ry * Rz (x first)
  M4 rx, ry, rz, sc, tr;
  rx.m[5] = cx, rx.m[6] = sx, rx.m[9] = -sx, rx.m[10] = cx;
  ry.m[0] = cy, ry.m[2] = -sy, ry.m[8] = sy, ry.m[10] = cy;
  rz.m[0] = cz, rz.m[1] = sz, rz.m[4] = -sz, rz.m[5] = cz;
  sc.m[0] = sc.m[5] = sc.m[10] = s;
  tr.m[12] = v[0], tr.m[13] = v[1], tr.m[14] = v[2];
  out = Mul(Mul(Mul(Mul(sc, rx), ry), rz), tr);
  return true;
}

// the light set the next draws are lit by (-1, no bank or the lights off: the editor's own light)
void SetStageLight(int set) {
  if (!g_cb_light) return;
  if (!g_stage_lights || g_light_bank.empty()) set = -1;
  if (set == g_cb_light_now) return;  // (the clock moves between frames only)
  StageLight sl;
  // (0.75: matched against game shots of RAW and the practice arena from the game's own cameras - at 1 the fills and
  // directional lights together washed stage floors and stands out; test aid SVR2011_MM_STAGE_GAIN)
  static const float gain = std::getenv("SVR2011_MM_STAGE_GAIN") ? std::strtof(std::getenv("SVR2011_MM_STAGE_GAIN"), nullptr) : 0.75f;
  float on[4] = {0, gain, 0, 0};
  if (set >= 0 && (StageLightOf(set, sl) || StageLightOf(100, sl))) on[0] = 1;
  D3D11_MAPPED_SUBRESOURCE ms;
  if (FAILED(g_ctx->Map(g_cb_light, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
  auto* f = static_cast<float*>(ms.pData);
  std::memcpy(f, sl.dir, sizeof sl.dir);
  std::memcpy(f + 24, sl.col, sizeof sl.col);
  std::memcpy(f + 48, on, sizeof on);
  g_ctx->Unmap(g_cb_light, 0);
  g_cb_light_now = set;
}

V3 g_eye;  // the camera's position this frame (RenderScene): "b" models turn to it

// A "b" (billboard) model turned about its own length to face the camera, as the game draws its beams: every "b"
// model of the stock arenas (RAW's ar_ten_beam / raw_glow_mo*, the practice arena's tf_ / stand_glow_mo*) runs along
// its local x from the lamp, its quads lying in local x / y and facing local z. at: its place (scale, rotation, move;
// row vectors), the turn about local x goes first.
M4 FaceCamera(const M4& at) {
  const V3 ax = Norm({at.m[0], at.m[1], at.m[2]}), yw = Norm({at.m[4], at.m[5], at.m[6]}),
           zw = Norm({at.m[8], at.m[9], at.m[10]});
  V3 v = g_eye - V3{at.m[12], at.m[13], at.m[14]};
  v = v - ax * Dot(v, ax);  // (towards the camera, across the beam)
  if (Dot(v, v) < 1e-6f) return at;
  v = Norm(v);
  // p * Rx(t): local z -> (0, -sin t, cos t), so the face's normal becomes -sin t yw + cos t zw = v
  const float t = std::atan2(-Dot(v, yw), Dot(v, zw)), c = std::cos(t), sn = std::sin(t);
  M4 rx;
  rx.m[5] = c, rx.m[6] = sn, rx.m[9] = -sn, rx.m[10] = c;
  return Mul(rx, at);
}

// an effect model (m(<n>)) is drawn where its motion puts it, in the look it belongs to
bool ShowEffect(const Obj& o) {
  if (!g_stage_lights || o.motion < 0 || !g_motions.count({o.motion, 0})) return false;
  return g_stage_entrance ? !o.match_only : !o.entrance_only;
}

void BuildObjects() {
  g_objs.clear();
  g_undo.clear(), g_redo.clear();
  g_sel = -1, g_multi.clear();
  ForgetGpu();
  ForgetTextures();
  if (!g_arena) return;
  const std::map<uint32_t, std::string> flags = ModelFlags();
  for (size_t mi = 0; mi < g_arena->models.size(); ++mi) {
    const ArenaModel& am = g_arena->models[mi];
    Obj o;
    if (const auto f = flags.find(am.id); f != flags.end() && !am.added) {
      o.effect = f->second.find("m(") != std::string::npos;
      for (char sep : {' ', '\t', '\r'})  // (a standalone "r")
        for (char sep2 : {' ', '\t', '\r'}) o.mirrored |= f->second.find(std::string(1, sep) + "r" + sep2) != std::string::npos;
      ReadStageFlags(f->second, o);
    }
    o.model = int(mi);
    o.label = Label(am);
    o.zone = am.zone;
    o.role = RoleOf(am.id);
    bool all_static = !am.model.meshes.empty();
    for (size_t k = 0; k < am.model.meshes.size(); ++k) {
      o.meshes.push_back(int(k));
      o.base.push_back(am.model.meshes[k].verts);
      o.strips.push_back(am.model.meshes[k].strips);
      all_static &= IsStatic(am.model.meshes[k]);
    }
    o.outside = g_area_lo >= 0 && !am.added && !InArea(am.id);
    if (g_area_lo >= 0) {
      const std::string& n = am.model.name;
      o.ceiling = n.find("_top") != std::string::npos || n.find("_ten") != std::string::npos ||
                  n.find("roof") != std::string::npos || n.find("_lig") != std::string::npos;
    }
    if (g_area_lo >= 0) o.zone = Zone::kFree, o.role = am.added ? o.role : Role::kNormal;  // (no ring here)
    o.movable = all_static && o.zone != Zone::kRing && o.role == Role::kNormal && !o.effect;
    o.glow = am.model.name.find("glow") != std::string::npos || am.model.name.find("beam") != std::string::npos ||
             am.model.name.find("_ev") != std::string::npos;
    ComputePivot(o);
    g_objs.push_back(std::move(o));
  }
}

Snapshot Snap() {
  Snapshot s;
  for (const auto& o : g_objs) {
    s.xf.push_back({o.pos.x, o.pos.y, o.pos.z, o.yaw, o.scale, o.hidden ? 1.f : 0.f, o.pitch, o.roll});
    s.tex.push_back(ObjTexture(o));
  }
  return s;
}

void PushUndo() {
  g_undo.push_back(Snap());
  if (g_undo.size() > 64) g_undo.erase(g_undo.begin());
  g_redo.clear();
}

void Restore(const Snapshot& s) {
  for (size_t i = 0; i < g_objs.size() && i < s.xf.size(); ++i) {
    Obj& o = g_objs[i];
    const auto& x = s.xf[i];
    if (i < s.tex.size() && !s.tex[i].empty() && s.tex[i] != ObjTexture(o)) SetObjTexture(o, s.tex[i]);
    if (o.pos.x == x[0] && o.pos.y == x[1] && o.pos.z == x[2] && o.yaw == x[3] && o.scale == x[4] &&
        o.hidden == (x[5] > 0.5f) && o.pitch == x[6] && o.roll == x[7])
      continue;
    o.pos = {x[0], x[1], x[2]}, o.yaw = x[3], o.scale = x[4], o.hidden = x[5] > 0.5f, o.pitch = x[6], o.roll = x[7];
    ApplyObj(o);
  }
}

void Undo() {
  if (g_undo.empty()) return;
  g_redo.push_back(Snap());
  const Snapshot s = g_undo.back();
  g_undo.pop_back();
  Restore(s);
}

void Redo() {
  if (g_redo.empty()) return;
  g_undo.push_back(Snap());
  const Snapshot s = g_redo.back();
  g_redo.pop_back();
  Restore(s);
}

// Anywhere, any way round (the edit is baked into the vertices; save grows the visibility tree's boxes to
// match - svrfmt GrowVisibility). Ringside / entrance parts too: the game's collision and the spots its moves
// aim at stay where they were (the inspector says so). Only sanity bounds remain.
void Constrain(Obj& o) {
  for (float* v : {&o.pos.x, &o.pos.y, &o.pos.z}) *v = std::clamp(*v, -kMaxMove, kMaxMove);
  o.scale = std::clamp(o.scale, 0.01f, 100.0f);
}

int HostModel() {
  int best = -1;
  size_t best_n = 0;
  for (size_t i = 0; i < g_arena->models.size(); ++i) {
    const auto& am = g_arena->models[i];
    if (g_area_lo >= 0) {  // (backstage: a model of the shown room, else the game hides it)
      if (am.added || !InArea(am.id) || am.id >= 1000) continue;  // (1000+: the room's movable objects)
      if (am.model.name.find("floor") != std::string::npos) return int(i);
    } else if (am.model.name.rfind("ar_ground", 0) == 0) {
      return int(i);
    }
    if ((g_area_lo < 0 && am.zone != Zone::kFree) || am.model.meshes.empty() || !IsStatic(am.model.meshes[0])) continue;
    size_t n = 0;
    for (const auto& s : am.model.meshes) n += s.verts.size();
    if (n > best_n) best = int(i), best_n = n;
  }
  return best;
}

// New meshes on a model (from an import) -> one added object
int AdoptNewMeshes(int model, size_t first, const std::string& label) {
  Obj o;
  o.model = model;
  o.label = label;
  o.zone = Zone::kFree;
  o.added = true;
  o.movable = true;
  const Model& m = g_arena->models[model].model;
  for (size_t k = first; k < m.meshes.size(); ++k) {
    o.meshes.push_back(int(k));
    o.base.push_back(m.meshes[k].verts);
    o.strips.push_back(m.meshes[k].strips);
  }
  if (o.meshes.empty()) return -1;
  ComputePivot(o);
  g_objs.push_back(std::move(o));
  ForgetGpu(model);
  ForgetTextures();
  Edited();
  return int(g_objs.size() - 1);
}

void Duplicate(int i) {
  if (i < 0 || !g_objs[i].movable) return;
  const Obj src = g_objs[i];
  Model& m = g_arena->models[src.model].model;
  const size_t first = m.meshes.size();
  for (size_t k = 0; k < src.meshes.size(); ++k) {
    Mesh s = m.meshes[src.meshes[k]];
    s.verts = src.base[k];
    s.strips = src.strips[k];
    m.meshes.push_back(std::move(s));
  }
  const int n = AdoptNewMeshes(src.model, first, src.label + " copy");
  if (n < 0) return;
  Obj& o = g_objs[n];
  o.pivot = src.pivot;
  o.pos = src.pos + V3{6, 0, 6};
  o.yaw = src.yaw;
  o.pitch = src.pitch, o.roll = src.roll;
  o.scale = src.scale;
  ApplyObj(o);
  g_sel = n;
  Log("Duplicated " + src.label + ".");
}

void DuplicateSelection() {
  const std::vector<int> sel = Selection();
  if (sel.size() <= 1) { Duplicate(g_sel); return; }
  std::vector<int> made;
  for (int i : sel) {
    Duplicate(i);
    if (g_sel >= 0 && g_sel != i) made.push_back(g_sel);
  }
  g_sel = made.empty() ? -1 : made.front();
  g_multi.assign(made.begin() + (made.empty() ? 0 : 1), made.end());
}

void Delete(int i) {
  if (i < 0) return;
  Obj& o = g_objs[i];
  if (!o.added) {
    PushUndo();
    o.hidden = true;
    ApplyObj(o);
    return;
  }
  // remove its meshes from the model; later mesh indices move down
  Model& m = g_arena->models[o.model].model;
  std::vector<int> gone = o.meshes;
  std::sort(gone.rbegin(), gone.rend());
  for (int k : gone) m.meshes.erase(m.meshes.begin() + k);
  for (auto& other : g_objs) {
    if (&other == &o || other.model != o.model) continue;
    for (int& k : other.meshes) {
      int below = 0;
      for (int g : gone) below += g < k;
      k -= below;
    }
  }
  g_arena->models[o.model].changed = true;
  ForgetGpu(o.model);
  g_objs.erase(g_objs.begin() + i);
  g_sel = -1;
  g_multi.clear();
  g_undo.clear(), g_redo.clear();  // (structure changed)
  Edited();
}

// The selection's objects: hidden (game objects) or removed (added ones), highest index first.
void DeleteSelection() {
  std::vector<int> sel = Selection();
  std::sort(sel.rbegin(), sel.rend());
  for (int i : sel) Delete(i);
  g_sel = -1, g_multi.clear();
}

void HideSelection(bool hide) {
  PushUndo();
  for (int i : Selection()) {
    g_objs[i].hidden = hide;
    ApplyObj(g_objs[i]);
  }
}

void DuplicateSelection();

void SetObjTexture(Obj& o, const std::string& name) {
  Model& m = g_arena->models[o.model].model;
  if (ObjTexture(o) == name) return;
  int slot = -1;
  for (size_t i = 0; i < m.textures.size(); ++i)
    if (m.textures[i] == name) slot = int(i);
  if (slot < 0) {
    m.textures.push_back(name);
    slot = int(m.textures.size() - 1);
  }
  for (int k : o.meshes)
    for (auto& p : m.meshes[k].params)
      if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) PutBe32(p.value.data(), uint32_t(slot));
  g_arena->models[o.model].changed = true;
  ForgetGpu(o.model);
  Edited();
}

// A picture as a new texture in the arena's first set (DXT5 when it has alpha)
std::string AddPicture(const std::string& path) {
  Image img;
  if (!LoadImageFile(path, img) || g_arena->bundles.empty()) return "";
  bool alpha = false;
  for (size_t i = 3; i < img.rgba.size() && !alpha; i += 4) alpha = img.rgba[i] < 250;
  // power-of-two, at most 512
  auto p2 = [](int v) { int p = 16; while (p < v && p < 512) p *= 2; return p; };
  img = Resize(img, p2(img.w), p2(img.h));
  static int n = 0;
  std::string name;
  do {
    char b[16];
    std::snprintf(b, sizeof b, "ed_tex%02d", n++);
    name = b;
  } while (g_arena->FindTexture(name));
  g_arena->bundles[0].textures.push_back({name, "dds", DdsEncode(img, alpha ? DxtFormat::kDxt5 : DxtFormat::kDxt1, true)});
  g_arena->bundles[0].changed = true;
  ForgetTextures();
  Edited();
  return name;
}

// An OBJ/FBX file's objects -> one added object on the host model
int ImportObject(const std::string& path, const std::string& label) {
  const int host = HostModel();
  if (host < 0) { Log("This arena has no model to add objects to."); return -1; }
  const size_t first = g_arena->models[host].model.meshes.size();
  ImportOptions opt;
  opt.host_model = g_arena->models[host].model.name;
  ImportReport rep;
  const bool ok = ImportFbx(*g_arena, path, opt, rep);
  for (const auto& w : rep.warnings) Log("  " + w);
  for (const auto& e : rep.errors) Log("  error: " + e);
  if (!ok) return -1;
  return AdoptNewMeshes(host, first, label);
}

// A box (w x h x d units) as an OBJ in the import's space (Y up, metres-ish),
// then sized and placed exactly on the camera target.
void AddBox(float w, float h, float d) {
  const fs::path tmp = fs::temp_directory_path() / "svr2011_modmaker_box.obj";
  if (FILE* f = std::fopen(tmp.string().c_str(), "wb")) {
    std::fprintf(f, "o box\n");
    const float x[2] = {-0.5f, 0.5f}, y[2] = {0, 1}, z[2] = {-0.5f, 0.5f};
    for (int i = 0; i < 8; ++i) std::fprintf(f, "v %g %g %g\n", x[i & 1], y[(i >> 1) & 1], z[(i >> 2) & 1]);
    std::fprintf(f, "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n");
    const int faces[6][4] = {{1, 2, 4, 3}, {5, 7, 8, 6}, {1, 5, 6, 2}, {3, 4, 8, 7}, {1, 3, 7, 5}, {2, 6, 8, 4}};
    for (const auto& q : faces) std::fprintf(f, "f %d/1 %d/2 %d/3 %d/4\n", q[0], q[1], q[2], q[3]);
    std::fclose(f);
  }
  const int i = ImportObject(tmp.string(), "box");
  if (i < 0) return;
  Obj& o = g_objs[i];
  // normalise to the wanted size, bottom centre on the floor below the camera target
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& vs : o.base)
    for (const auto& v : vs)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  const float size[3] = {w, h, d};
  for (auto& vs : o.base)
    for (auto& v : vs)
      for (int k = 0; k < 3; ++k) {
        const float t = hi[k] > lo[k] ? (v.pos[k] - lo[k]) / (hi[k] - lo[k]) : 0.5f;  // 0..1
        const float c = k == 0 ? g_target.x : k == 2 ? g_target.z : 0.0f;
        // y down: the box stands on the floor (y = 0; an area's: its lowest point) and rises to -h
        const float floor = g_area_lo >= 0 ? g_target.y : 0.0f;
        v.pos[k] = k == 1 ? floor - (1.0f - t) * size[1] : c + (t - 0.5f) * size[k];
      }
  ComputePivot(o);
  ApplyObj(o);
  g_sel = i;
  Log("Added a box. Pick its texture on the right.");
}

// An OBJ / FBX from any 3D program (svrfmt/model_objects.h: metres or centimetres, Y up, counter-clockwise front
// faces, textures from its materials) -> one added object standing on the floor at the view centre. from 2: a file
// Export to Blender wrote (game units, the game's winding) - the round-trip importer, as before.
int AddFileObject(const std::string& path, int from) {
  const std::string label = U8(fs::u8path(path).stem());
  if (from == 2) return ImportObject(path, label);
  const int host = HostModel();
  if (host < 0) { Log("This arena has no model to add objects to."); return -1; }
  AddOptions opt;
  opt.obj_unit_meters = from == 1 ? 0.01f : 1.0f;
  AddReport rep;
  const bool ok = AddObjectFile(*g_arena, host, path, opt, rep);
  for (const auto& w : rep.warnings) Log("  " + w);
  for (const auto& e : rep.errors) Log("  error: " + e);
  if (!ok) {
    if (rep.first_mesh >= 0) {  // (added, then refused - too much detail: taken out again)
      auto& ms = g_arena->models[host].model.meshes;
      ms.erase(ms.begin() + rep.first_mesh, ms.end());
      ForgetGpu(host);
    }
    return -1;
  }
  const int i = AdoptNewMeshes(host, size_t(rep.first_mesh), label);
  if (i < 0) return -1;
  Obj& o = g_objs[i];
  const float floor = g_area_lo >= 0 ? g_target.y : 0.0f;  // (y down: the floor at 0; an area's: below the view)
  o.pos = {g_target.x - o.pivot.x, floor - o.pivot.y, g_target.z - o.pivot.z};
  ApplyObj(o);
  Log("Added " + label + ": " + std::to_string(rep.meshes_added) + " part(s), " + std::to_string(rep.textures_added) +
      " new texture(s).");
  return i;
}

// A folder (the system's folder picker), "" if none.
std::string PickFolderU8(const wchar_t* title) {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS);
    dlg->SetTitle(title);
    IShellItem* item = nullptr;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      out = U8(fs::path(path));
      CoTaskMemFree(path);
    }
    if (item) item->Release();
    dlg->Release();
  }
  return out;
}

// The object as it stands now (moved, turned, scaled) -> <dir>/<label>.obj + .mtl + its textures as PNG.
void ExportObject(const Obj& o, const std::string& dir) {
  std::string path, err;
  const std::string name = o.label.substr(0, o.label.find("  ("));  // (not the "  (id)" the list shows)
  if (ExportObjectObj(*g_arena, o.model, o.meshes, dir, &path, &err, name)) Log("Exported " + o.label + " to " + path + ".");
  else Log("Could not export " + o.label + ": " + err);
}

// One part (mesh) of an object taken out: emptied in the arena as the game expects (svrfmt/model_objects.h); the
// object's own copies follow, so later moves keep it out. Not on the undo list.
bool RemovePart(Obj& o, size_t k) {
  if (k >= o.meshes.size()) return false;
  std::string err;
  if (!RemoveObject(*g_arena, o.model, o.meshes[k], &err)) {
    Log("Could not remove that part: " + err);
    return false;
  }
  const Mesh& m = g_arena->models[o.model].model.meshes[size_t(o.meshes[k])];
  o.base[k] = m.verts;
  o.strips[k] = m.strips;
  ApplyObj(o);
  Log("Removed part " + std::to_string(k + 1) + " of " + o.label + ".");
  return true;
}

void PartsPanel(Obj& o) {
  if (o.meshes.size() < 2) return;
  const std::string head = "Parts (" + std::to_string(o.meshes.size()) + ")";
  if (!ImGui::CollapsingHeader(head.c_str())) return;
  static int pick = -1, pick_of = -1;
  if (pick_of != g_sel) pick = -1, pick_of = g_sel;
  const Model& m = g_arena->models[o.model].model;
  const float scale = ImGui::GetFontSize() / 17.0f;
  ImGui::BeginChild("parts", ImVec2(0, std::min(160.0f, 22.0f * float(o.meshes.size()) + 8) * scale), true);
  for (size_t k = 0; k < o.meshes.size(); ++k) {
    const Mesh& s = m.meshes[size_t(o.meshes[k])];
    size_t tris = 0;
    for (const auto& st : s.strips) tris += StripToTriangles(st.indices).size();
    const int slot = DiffuseSlot(s);
    const std::string tex = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "-";
    char b[128];
    std::snprintf(b, sizeof b, "%zu  %s  %zu triangles%s##part%zu", k + 1, tex.c_str(), tris, tris ? "" : " (removed)", k);
    if (ImGui::Selectable(b, pick == int(k))) pick = int(k);
  }
  ImGui::EndChild();
  ImGui::BeginDisabled(pick < 0);
  if (ImGui::Button("Remove part") && RemovePart(o, size_t(pick))) pick = -1;
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("(can't be undone)");
}

// ---------------------------------------------------------------- view

V3 Eye() {
  return g_target + V3{std::cos(g_pitch) * std::sin(g_yaw), -std::sin(g_pitch), std::cos(g_pitch) * std::cos(g_yaw)} * g_dist;
}

void Ray(ImVec2 mouse, V3& origin, V3& dir) {
  M4 inv;
  Invert(g_viewproj, inv);
  const float nx = (mouse.x - g_vp_min.x) / g_vp_size.x * 2 - 1, ny = 1 - (mouse.y - g_vp_min.y) / g_vp_size.y * 2;
  const V3 a = Project({nx, ny, 0}, inv), b = Project({nx, ny, 1}, inv);
  origin = a;
  dir = Norm(b - a);
}

bool ToScreen(V3 p, ImVec2& out) {
  float w;
  const V3 c = Project(p, g_viewproj, &w);
  if (w <= 0) return false;
  out = {g_vp_min.x + (c.x + 1) * 0.5f * g_vp_size.x, g_vp_min.y + (1 - c.y) * 0.5f * g_vp_size.y};
  return true;
}

// nearest object hit by the ray (Moller-Trumbore), -1 if none
int Pick(V3 o, V3 d, float* t_out = nullptr) {
  int best = -1;
  float best_t = 1e30f;
  for (size_t i = 0; i < g_objs.size(); ++i) {
    const Obj& ob = g_objs[i];
    if (ob.hidden || ob.effect || ob.outside || (ob.ceiling && !g_show_ceilings) || ob.role != Role::kNormal) continue;
    const Model& m = g_arena->models[ob.model].model;
    for (int k : ob.meshes) {
      const Mesh& s = m.meshes[k];
      // sphere test first
      const V3 c{s.sphere[0], s.sphere[1], s.sphere[2]};
      const V3 oc = c - o;
      const float along = Dot(oc, d);
      if (Dot(oc, oc) - along * along > s.sphere[3] * s.sphere[3] + 1) continue;
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices)) {
          if (t[0] >= s.verts.size() || t[1] >= s.verts.size() || t[2] >= s.verts.size()) continue;
          const V3 p0{s.verts[t[0]].pos[0], s.verts[t[0]].pos[1], s.verts[t[0]].pos[2]};
          const V3 p1{s.verts[t[1]].pos[0], s.verts[t[1]].pos[1], s.verts[t[1]].pos[2]};
          const V3 p2{s.verts[t[2]].pos[0], s.verts[t[2]].pos[1], s.verts[t[2]].pos[2]};
          const V3 e1 = p1 - p0, e2 = p2 - p0, pv = Cross(d, e2);
          const float det = Dot(e1, pv);
          if (std::fabs(det) < 1e-8f) continue;
          const float inv = 1 / det;
          const V3 tv = o - p0;
          const float u = Dot(tv, pv) * inv;
          if (u < 0 || u > 1) continue;
          const V3 qv = Cross(tv, e1);
          const float v = Dot(d, qv) * inv;
          if (v < 0 || u + v > 1) continue;
          const float tt = Dot(e2, qv) * inv;
          if (tt > 0.01f && tt < best_t) best_t = tt, best = int(i);
        }
    }
  }
  if (t_out) *t_out = best_t;
  return best;
}

// the read-only view's camera and extra drawing (SetViewCamera / SetViewExtra: the Full preview)
bool g_vcam_set = false;
M4 g_vcam_view, g_vcam_proj;
ViewExtraFn g_view_extra;
bool g_view_hint = true;

void RenderScene() {
  const float clear[4] = {0.05f, 0.055f, 0.065f, 1};
  g_ctx->ClearRenderTargetView(g_rtv, clear);
  g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
  if (!g_arena) return;
  D3D11_VIEWPORT vp = {0, 0, float(g_rt_w), float(g_rt_h), 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_ctx->RSSetState(g_solid);
  g_ctx->IASetInputLayout(g_layout);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps, nullptr, 0);
  g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(1, 1, &g_cb_mode);
  g_ctx->PSSetConstantBuffers(2, 1, &g_cb_light);
  g_cb_mode_now = -1;  // (its value: set again by the first mesh)
  g_cb_light_now = -2;
  LoadStage();
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);  // (an LED mask drawn last mustn't leave its blend on)
  g_ctx->PSSetSamplers(0, 1, &g_sampler);
  const bool given = g_readonly && g_vcam_set;  // (the view's camera from the caller)
  M4 view = given ? g_vcam_view : LookAt(Eye(), g_target, {0, -1, 0});
  M4 proj = given ? g_vcam_proj : Perspective(0.9f, float(g_rt_w) / float(std::max(1, g_rt_h)), 1.0f, 5000.0f, g_mirror);
  // test aid SVR2011_MM_EDITOR_VIEW "<16 floats: the game's row-vector view>,<fov>": the game's own camera (its camera
  // dump), the projection as the Full preview's --full-preview-view - for side-by-sides with game shots
  static float test_view[17];
  static const bool test_on = [] {
    const char* e = std::getenv("SVR2011_MM_EDITOR_VIEW");
    int n = 0;
    for (const char* p = e; p && *p && n < 17;) {
      char* end = nullptr;
      test_view[n] = std::strtof(p, &end);
      if (end == p) break;
      ++n;
      for (p = end; *p == ',' || *p == ' ';) ++p;
    }
    return n == 17;
  }();
  if (const char* t = std::getenv("SVR2011_MM_STAGE_TIME"); t && !g_readonly)  // (test aid "<seconds>[,e]": the clock / look)
    g_stage_time = std::strtof(t, nullptr), g_stage_entrance = std::strchr(t, 'e') != nullptr;
  if (test_on) {
    std::memcpy(view.m, test_view, 64);
    const float xs = 1 / std::tan(test_view[16] * kPi / 360.0f), zn = 1, zf = 5000;
    proj = M4();
    proj.m[0] = xs, proj.m[5] = -xs * float(g_rt_w) / float(std::max(1, g_rt_h)), proj.m[10] = zf / (zf - zn), proj.m[11] = 1;
    proj.m[14] = -zn * zf / (zf - zn), proj.m[15] = 0;
  }
  g_viewproj = Mul(view, proj);
  if (M4 inv; Invert(view, inv)) g_eye = {inv.m[12], inv.m[13], inv.m[14]};
  float light[3];
  LightTint(light);
  const bool per_rope = [] {
    for (const auto& o : g_objs)
      if (o.role == Role::kRopePerRope) return true;
    return false;
  }();
  const M4 ident;
  for (const Obj& o : g_objs) {
    if (o.hidden || o.outside || (o.ceiling && !g_show_ceilings)) continue;
    SetStageLight(o.light_set);
    if (o.effect) {  // (placed by its motion)
      M4 at;
      if (o.glow || !ShowEffect(o) || !MotionPlace(o.motion, at)) continue;
      if (o.billboard) at = FaceCamera(at);
      SetCb(at, light);
      for (int k : o.meshes) DrawMesh(o.model, k);
      continue;
    }
    switch (o.role) {
      case Role::kNotDrawn:
        break;
      case Role::kNormal:
        if (o.glow || AlphaKind(o.model) >= 2) break;  // (after the solid ones)
        SetCb(ident, light);
        for (int k : o.meshes) DrawMesh(o.model, k);
        if (o.mirrored) {  // (the game's mirrored copy)
          M4 mirror;
          mirror.m[0] = -1;
          SetCb(mirror, light);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      case Role::kRopeSide:
      case Role::kRopePerRope: {
        if (!g_show_ring) break;
        const uint32_t id = g_arena->models[o.model].id;
        if (o.role == Role::kRopeSide && per_rope) break;  // the game draws the twelve instead
        for (int r = 0; r < 3; ++r) {
          int side_model = int(id);
          if (o.role == Role::kRopePerRope) {
            if ((int(id) - 900) / 4 != r) continue;
            side_model = int(kSideRope[(id - 900) % 4]);
          }
          if (!g_ring.ropes[r].visible) continue;
          const uint32_t c = g_ring.ropes[r].tint;
          const float tint[3] = {light[0] * ((c >> 16) & 255) / 255.f, light[1] * ((c >> 8) & 255) / 255.f,
                                 light[2] * (c & 255) / 255.f};
          SetCb(Place(RopeOffset(uint32_t(side_model)) + V3{0, RopeY(r), 0}, 0), tint);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      }
      case Role::kPad:
        if (!g_show_ring || !g_ring.pads) break;
        for (int r = 0; r < 3; ++r) {
          SetCb(Place({0, RopeY(r), 0}, 0), light);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      case Role::kTurnbuckle:
        if (!g_show_ring || !g_ring.turnbuckles) break;
        for (int corner = 0; corner < 4; ++corner)
          for (int r = 0; r < 3; ++r) {
            SetCb(TurnbuckleAt(corner, RopeY(r)), light);
            for (int k : o.meshes) DrawMesh(o.model, k);
          }
        break;
      case Role::kApron: {
        if (!g_show_ring) break;
        // The game makes four panels, 974 / 973 / 974 / 973 round the ring (the ring object's +152..+164), so
        // bs_ar_epron2 (974) is on the two z sides - the ramp's, with the arena's second picture (Superstars:
        // WWE.COM) - and bs_ar_epron (973) on the two x sides. A panel's front (its triangles' winding; the
        // normals say +z, for the light) faces -z at z = 0: each goes out along its front, so the picture
        // reads from outside the ring.
        const bool z_sides = g_arena->models[o.model].id == 974;
        for (int side = 0; side < 2; ++side) {
          const float yaw = (z_sides ? 0 : kPi / 2) + side * kPi;
          const float c = std::cos(yaw), s = std::sin(yaw);
          SetCb(Place({-kApronOut * s, 0, -kApronOut * c}, yaw), light);
          for (int k : o.meshes) DrawMesh(o.model, k);
        }
        break;
      }
    }
  }
  // the see-through models (C351 s k i j: blended, a o: added), after the solid ones, depth read only
  g_ctx->OMSetDepthStencilState(g_depth_ro, 0);
  for (const Obj& o : g_objs) {
    if (o.hidden || o.effect || o.outside || (o.ceiling && !g_show_ceilings) || o.glow || o.role != Role::kNormal ||
        AlphaKind(o.model) < 2)
      continue;
    SetStageLight(o.light_set);
    SetCb(ident, light);
    for (int k : o.meshes) DrawMesh(o.model, k);
    if (o.mirrored) {
      M4 mirror;
      mirror.m[0] = -1;
      SetCb(mirror, light);
      for (int k : o.meshes) DrawMesh(o.model, k);
    }
  }
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  // light glows and beams, added on top
  g_ctx->OMSetBlendState(g_add, nullptr, 0xFFFFFFFF);
  g_keep_blend = true;
  g_ctx->OMSetDepthStencilState(g_depth_ro, 0);
  g_glow_pass = true;
  SetCb(ident, light);
  g_glow_pass = false;
  for (const Obj& o : g_objs)
    if (o.glow && !o.hidden && !o.effect && !o.outside && !(o.ceiling && !g_show_ceilings) && o.role == Role::kNormal)
      for (int k : o.meshes) DrawMesh(o.model, k);
  for (const Obj& o : g_objs) {  // (the beams and glows the arena's motions put in place)
    M4 at;
    if (!o.glow || !o.effect || o.hidden || o.outside || !ShowEffect(o) || !MotionPlace(o.motion, at)) continue;
    if (o.billboard) at = FaceCamera(at);
    g_glow_pass = true;
    SetCb(at, light);
    g_glow_pass = false;
    for (int k : o.meshes) DrawMesh(o.model, k);
  }
  {  // (the mirrored copies)
    M4 mirror;
    mirror.m[0] = -1;
    g_glow_pass = true;
    SetCb(mirror, light);
    g_glow_pass = false;
    for (const Obj& o : g_objs)
      if (o.glow && o.mirrored && !o.hidden && !o.effect && !o.outside && o.role == Role::kNormal)
        for (int k : o.meshes) DrawMesh(o.model, k);
  }
  g_keep_blend = false;
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_cb_mode_now = -1;
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  // the selection, as a wireframe on top
  g_ctx->RSSetState(g_wire);
  for (int i : Selection()) {
    if (g_objs[i].role != Role::kNormal || g_objs[i].hidden) continue;
    const float orange[3] = {1.0f, 0.55f, 0.1f}, yellow[3] = {1.0f, 0.9f, 0.3f};
    SetCb(ident, light, i == g_sel ? orange : yellow);
    for (int k : g_objs[i].meshes) DrawMesh(g_objs[i].model, k);
  }
  g_ctx->RSSetState(g_solid);
  if (g_readonly && g_view_extra) g_view_extra(g_rtv, g_dsv, g_rt_w, g_rt_h, view.m, proj.m);
  ID3D11RenderTargetView* none = nullptr;
  g_ctx->OMSetRenderTargets(1, &none, nullptr);
}

// ---------------------------------------------------------------- budget

void UpdateBudget() {
  if (!g_arena || g_budget_busy || g_budget_edits == g_edits) return;
  if (std::chrono::steady_clock::now() - g_last_edit < std::chrono::milliseconds(1200) && g_budget_edits >= 0) return;
  if (g_budget_thread.joinable()) g_budget_thread.join();
  g_budget_edits = g_edits;
  g_budget_busy = true;
  auto copy = std::make_shared<Arena>(*g_arena);
  g_quiet = true;
  ApplyBuild(*copy);
  g_quiet = false;
  g_budget_thread = std::thread([copy] {
    size_t unpacked = 0, biggest = 0;
    uint32_t biggest_id = 0;
    auto note = [&](const ArenaModel& m, size_t size) {
      if (size > biggest) biggest = size, biggest_id = m.id;
    };
    for (const auto& e : copy->entries) {
      bool done = false;
      for (const auto& m : copy->models)
        if (m.id == e.id && m.changed) {
          const size_t n = JboyWrite(m.model).size();
          unpacked += n, done = true;
          note(m, n);
        }
      for (const auto& b : copy->bundles)
        if (b.id == e.id && b.changed) unpacked += BundleWrite(b.textures).size(), done = true;
      if (!done) unpacked += Unpack(e.data).size();
    }
    for (const auto& m : copy->models)
      if (m.added) {
        const size_t n = JboyWrite(m.model).size();
        unpacked += n;
        note(m, n);
      }
    const size_t file = copy->Save().size();
    std::lock_guard lock(g_budget_mutex);
    g_budget_file = file, g_budget_unpacked = unpacked;
    g_budget_biggest = biggest, g_budget_biggest_id = biggest_id;
    g_budget_file_max = copy->original_file, g_budget_unpacked_max = copy->original_unpacked;
    g_budget_busy = false;
  });
}


// ---------------------------------------------------------------- lighting

struct Preset {
  const char* name;
  float color[3], strength;
};
const Preset kPresets[] = {
    {"As made", {1, 1, 1}, 1.0f},          {"Brighter", {1, 1, 1}, 1.25f},
    {"Darker", {1, 1, 1}, 0.7f},           {"Night show", {0.75f, 0.8f, 1.0f}, 0.6f},
    {"Warm", {1.1f, 0.95f, 0.8f}, 1.0f},   {"Cool", {0.85f, 0.95f, 1.15f}, 1.0f},
    {"Blood red", {1.25f, 0.55f, 0.55f}, 1.0f}, {"Toxic green", {0.7f, 1.2f, 0.7f}, 1.0f},
    {"Royal purple", {0.95f, 0.7f, 1.2f}, 1.0f}, {"Custom", {1, 1, 1}, 1.0f},
};

// ---------------------------------------------------------------- UI parts

const char* ZoneTitle(Zone z) {
  switch (z) {
    case Zone::kRing: return "Ring kit";
    case Zone::kRingside: return "Ringside";
    case Zone::kEntrance: return "Entrance";
    default: return "Arena";
  }
}

void Outliner() {
  static char filter[64] = "";
  if (g_area_lo >= 0) ImGui::Checkbox("Show ceilings", &g_show_ceilings);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", "Find an object", filter, sizeof filter);
  const Zone zones[] = {Zone::kRing, Zone::kRingside, Zone::kEntrance, Zone::kFree};
  for (int pass = 0; pass < 6; ++pass) {
    const bool added_pass = pass == 4, effect_pass = pass == 5;
    const char* title = effect_pass ? "Effects (placed by the game)" : added_pass ? "Added"
                        : g_area_lo >= 0                                    ? "Area"
                                                                            : ZoneTitle(zones[pass]);
    auto in_pass = [&](const Obj& o) {
      if (o.outside) return false;
      return effect_pass ? o.effect : added_pass ? o.added : (!o.added && !o.effect && o.zone == zones[pass]);
    };
    int count = 0;
    for (const auto& o : g_objs) count += in_pass(o);
    if (!count) continue;
    char head[64];
    std::snprintf(head, sizeof head, "%s (%d)###z%d", title, count, pass);
    ImGui::SetNextItemOpen(pass == 0 || pass == 4, ImGuiCond_Once);
    const bool open = ImGui::TreeNode(head);
    if (effect_pass && ImGui::IsItemHovered())
      ImGui::SetTooltip("Light rigs, beams and glows the arena's own animation moves into place during the show.\n"
                        "Not drawn here (they would all sit in the ring); untick one to remove it from the arena.");
    if (!open) continue;
    for (size_t i = 0; i < g_objs.size(); ++i) {
      Obj& o = g_objs[i];
      if (!in_pass(o)) continue;
      if (filter[0] && o.label.find(filter) == std::string::npos) continue;
      ImGui::PushID(int(i));
      bool vis = !o.hidden;
      if (ImGui::Checkbox("##v", &vis)) {
        PushUndo();
        o.hidden = !vis;
        ApplyObj(o);
      }
      ImGui::SameLine();
      if (o.hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (ImGui::Selectable(o.label.c_str(), Selected(int(i)))) {
        if (ImGui::GetIO().KeyCtrl) ToggleSelect(int(i));
        else SelectOne(int(i));
      }
      if (o.hidden) ImGui::PopStyleColor();
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
}

void RingKitPanel() {
  bool changed = false;
  ImGui::TextWrapped("The ring's ropes, turnbuckles and corner pads. Bottom rope = 0.");
  for (int r = 2; r >= 0; --r) {
    ImGui::PushID(r);
    const char* names[] = {"Bottom rope", "Middle rope", "Top rope"};
    changed |= ImGui::Checkbox(names[r], &g_ring.ropes[r].visible);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
    float col[3] = {((g_ring.ropes[r].tint >> 16) & 255) / 255.f, ((g_ring.ropes[r].tint >> 8) & 255) / 255.f,
                    (g_ring.ropes[r].tint & 255) / 255.f};
    if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs)) {
      g_ring.ropes[r].tint = uint32_t(col[0] * 255 + 0.5f) << 16 | uint32_t(col[1] * 255 + 0.5f) << 8 |
                             uint32_t(col[2] * 255 + 0.5f);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(g_ring.ropes[r].texture.empty() ? "Picture..." : "Picture*")) {
      if (g_hooks.pick_picture) {
        const std::string f = g_hooks.pick_picture("A picture wrapped along the rope (a wide strip works best)");
        if (!f.empty()) g_ring.ropes[r].texture = f, changed = true;
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", g_ring.ropes[r].texture.empty() ? "A picture of your own along this rope (tape, a logo, a pattern)."
                                                               : g_ring.ropes[r].texture.c_str());
    if (!g_ring.ropes[r].texture.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) g_ring.ropes[r].texture.clear(), changed = true;
    }
    ImGui::PopID();
  }
  changed |= ImGui::Checkbox("Turnbuckles", &g_ring.turnbuckles);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Corner pads", &g_ring.pads);
  float base = RopeBase() * 10, gap = RopeGap() * 10;  // cm
  ImGui::SetNextItemWidth(-LabelRoom("Low rope (cm)"));
  if (ImGui::SliderFloat("Low rope (cm)", &base, 15, 70, "%.0f")) g_ring.rope_base = base / 10, changed = true;
  ImGui::SetNextItemWidth(-LabelRoom("Low rope (cm)"));
  if (ImGui::SliderFloat("Rope gap (cm)", &gap, 25, 55, "%.0f")) g_ring.rope_gap = gap / 10, changed = true;
  if (RopeBase() + 2 * RopeGap() > 12.5f)
    ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "The top rope would be above the posts.");
  if (ImGui::Button("Reset ring")) g_ring = RingSpec(), changed = true;
  if (g_ring.VisibleRopes() == 0)
    ImGui::TextWrapped("No ropes: runners and whipped wrestlers stop at the edge instead of rebounding, and there "
                       "are no rope breaks.");
  else if (g_ring.VisibleRopes() < 3) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.7f, 0.2f, 1));
    ImGui::TextWrapped("Ropes left out are not drawn, but wrestlers still use all three rope heights (rebounds, "
                       "rope moves). Leave out all three for a ring without rope gameplay.");
    ImGui::PopStyleColor();
  }
  if (changed) Edited();
}

void LightingPanel() {
  int& p = g_light.preset;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##preset", kPresets[p].name)) {
    for (int i = 0; i < int(std::size(kPresets)); ++i)
      if (ImGui::Selectable(kPresets[i].name, p == i)) {
        p = i;
        if (i != int(std::size(kPresets)) - 1) {
          std::memcpy(g_light.color, kPresets[i].color, sizeof g_light.color);
          g_light.strength = kPresets[i].strength;
        }
        Edited();
      }
    ImGui::EndCombo();
  }
  if (ImGui::ColorEdit3("Colour", g_light.color)) p = int(std::size(kPresets)) - 1, Edited();
  if (ImGui::SliderFloat("Strength", &g_light.strength, 0.3f, 1.6f, "%.2f")) p = int(std::size(kPresets)) - 1, Edited();
  ImGui::TextDisabled("Every arena material's colour (not the crowd).");
  if (ImGui::Checkbox("Crowd in the seats", &g_light.crowd)) Edited();
  ImGui::SeparatorText("In the game");
  {
    static const char* const kMatch[] = {"The game's (match lighting)", "The entrance's lighting",
                                         "The entrance's, with its coloured spotlight"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##match_light", &g_light.match_light, kMatch, 3)) Edited();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How the wrestlers and the ring are lit during the match. The entrance's: as on the stage\n"
                        "and ramp (brighter in arenas whose match lighting is dark).");
    if (ImGui::SliderFloat("Game light", &g_light.game_scale, 0.5f, 2.0f, "x%.2f")) Edited();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The arena's lights in the game - wrestlers, ring and hall - times this (and the player's\n"
                        "GRAPHICS -> ARENA BRIGHTNESS). Not shown in this view.");
    static const char* const kTron[] = {"Titantron: the arena's screens", "Titantron: the live camera"};
    int tron = g_light.tron_live ? 1 : 0;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##titantron", &tron, kTron, 2)) g_light.tron_live = tron == 1, Edited();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("What the video screens (models 800-899) show during the match. The arena's own screens:\n"
                        "the screen art it was made with. The live camera: the match itself, as in the game's own\n"
                        "arenas. The entrance video plays on them either way.");
  }
  ImGui::Checkbox("The arena's own stage lights", &g_stage_lights);  // (the view only: the game always uses them)
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The view lit as the game lights this arena: each object by its own light set, and the\n"
                      "light beams and glows put where the arena's motions place them.\nOff: the editor's plain light.");
}

void Inspector() {
  if (g_sel < 0 || g_sel >= int(g_objs.size())) {
    ImGui::TextDisabled("Click an object in the view or the list. Ctrl+click adds to the selection.");
    return;
  }
  if (!g_multi.empty()) {
    const std::vector<int> sel = Selection();
    ImGui::Text("%zu objects selected", sel.size());
    ImGui::TextDisabled("Move, Turn and Scale in the view act on all of them; so do these:");
    if (ImGui::Button("Duplicate all")) DuplicateSelection();
    ImGui::SameLine();
    if (ImGui::Button("Hide all")) HideSelection(true);
    ImGui::SameLine();
    if (ImGui::Button("Show all")) HideSelection(false);
    if (ImGui::Button("Delete all")) { DeleteSelection(); return; }
    ImGui::SameLine();
    if (ImGui::Button("Select one")) g_multi.clear();
    ImGui::Separator();
    ImGui::TextDisabled("The first one:");
  }
  Obj& o = g_objs[g_sel];
  const ArenaModel& am = g_arena->models[o.model];
  ImGui::TextWrapped("%s", o.label.c_str());
  ImGui::TextDisabled("%s, %zu meshes%s", ZoneTitle(o.zone), o.meshes.size(), o.added ? ", added" : "");
  if (o.zone == Zone::kRing) {
    ImGui::TextWrapped("Ring parts are placed by the game: change them with the Ring Kit, or retexture them.");
  } else if (!o.movable) {
    ImGui::TextWrapped("This object is rigged (moves with bones): it can be hidden or retextured, not moved.");
  } else {
    if (o.zone == Zone::kRingside || o.zone == Zone::kEntrance) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.75f, 0.3f, 1));
      ImGui::TextWrapped("Only its look moves; wrestlers still use the old spot.");
      ImGui::PopStyleColor();
    }
    if (o.mirrored) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("The game draws it twice, mirrored across the middle: its mirror image moves too.");
      ImGui::PopStyleColor();
    }
    // (typed or dragged: Ctrl+click or double-click a field to type a value; no range but a 1 km sanity bound)
    float p[3] = {o.pos.x / 10, -o.pos.y / 10, o.pos.z / 10};  // metres, up positive
    bool ch = false;
    ImGui::SetNextItemWidth(-LabelRoom("Turn (deg)"));
    ch |= ImGui::DragFloat3("Move (m)", p, 0.01f, -kMaxMove / 10, kMaxMove / 10, "%.3f");
    if (ImGui::IsItemActivated()) PushUndo();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Right, up, forward in metres. Drag, or Ctrl+click to type.");
    float deg[3] = {o.yaw * 180 / kPi, o.pitch * 180 / kPi, o.roll * 180 / kPi};
    ImGui::SetNextItemWidth(-LabelRoom("Turn (deg)"));
    ch |= ImGui::DragFloat3("Turn (deg)", deg, 0.25f, 0, 0, "%.2f");
    if (ImGui::IsItemActivated()) PushUndo();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn (about the up axis), tilt forward, tilt sideways - any angle.");
    ImGui::SetNextItemWidth(-LabelRoom("Turn (deg)"));
    ch |= ImGui::DragFloat("Scale", &o.scale, 0.002f, 0.01f, 100, "%.3f");
    if (ImGui::IsItemActivated()) PushUndo();
    if (ch) {
      o.pos = {p[0] * 10, -p[1] * 10, p[2] * 10};
      o.yaw = deg[0] * kPi / 180, o.pitch = deg[1] * kPi / 180, o.roll = deg[2] * kPi / 180;
      Constrain(o);
      ApplyObj(o);
    }
    if (ImGui::Button("Duplicate")) DuplicateSelection();
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
      PushUndo();
      o.pos = {}, o.yaw = 0, o.pitch = 0, o.roll = 0, o.scale = 1;
      ApplyObj(o);
    }
  }
  if (ImGui::Button(o.hidden ? "Show" : "Hide")) {
    PushUndo();
    o.hidden = !o.hidden;
    ApplyObj(o);
  }
  if (o.added) {
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
      Delete(g_sel);
      return;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Export OBJ...")) {
    const std::string dir = PickFolderU8(L"A folder for the object (.obj, .mtl and its textures)");
    if (!dir.empty()) ExportObject(o, dir);
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("This object as it stands (moved, turned, scaled) as an OBJ with its textures: metres, Y up - "
                      "for Blender or any 3D program, or to add it to another arena.");
  PartsPanel(o);
  // texture
  if (!o.meshes.empty()) {
    const Model& m = am.model;
    const int slot = DiffuseSlot(m.meshes[o.meshes[0]]);
    const std::string cur = slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "";
    ImGui::SetNextItemWidth(-LabelRoom("Texture"));
    if (ImGui::BeginCombo("Texture", cur.c_str())) {
      for (const auto& b : g_arena->bundles)
        for (const auto& t : b.textures)
          if (ImGui::Selectable(t.name.c_str(), t.name == cur)) PushUndo(), SetObjTexture(o, t.name);
      ImGui::EndCombo();
    }
    const bool test = !g_test_picture.empty();  // (test aid --test-picture: as if picked with this button)
    if (ImGui::Button("Use a picture...") || test) {
      wchar_t file[MAX_PATH] = L"";
      OPENFILENAMEW ofn = {sizeof ofn};
      ofn.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0";
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST;
      std::string picked;
      if (test) picked = g_test_picture, g_test_picture.clear(), Log("test: a picture on " + o.label);
      else if (GetOpenFileNameW(&ofn)) picked = U8(fs::path(file));
      if (!picked.empty()) {
        const std::string path = picked;
        const std::string name = AddPicture(path);
        if (name.empty()) Log("That picture could not be read (PNG, JPG, BMP or TGA): nothing changed.");
        else PushUndo(), SetObjTexture(o, name), Log("New texture " + name + " on " + o.label + ".");
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(this object only)");
  }
}

// -- The prop library: models from any of the game's arenas ------------------

struct Library {
  int arena = -1;                  // index in g_hooks.library (loaded)
  std::unique_ptr<Arena> src;
  std::vector<int> models;         // LibraryModels(src)
  std::thread loader;
  std::atomic<bool> loading{false};
  std::unique_ptr<Arena> pending;  // (from the loader)
  int pending_arena = -1;
  std::mutex mutex;
  int pick = -1;
  char filter[64] = "";
};
Library g_lib;

void LibraryLoad(int i) {
  if (g_lib.loading || i < 0 || i >= int(g_hooks.library.size())) return;
  if (g_lib.loader.joinable()) g_lib.loader.join();
  g_lib.loading = true;
  const std::string path = g_hooks.library[i].second;
  g_lib.loader = std::thread([i, path] {
    auto a = std::make_unique<Arena>();
    std::string err;
    const bool ok = a->Load(path, &err);
    std::lock_guard lock(g_lib.mutex);
    if (ok) g_lib.pending = std::move(a), g_lib.pending_arena = i;
    g_lib.loading = false;
  });
}

void AddFromLibrary(int src_model, bool at_view) {
  const int host = HostModel();
  if (host < 0) { Log("This arena has no floor model to add objects to."); return; }
  std::string err;
  const int first = CopyModelInto(*g_arena, host, *g_lib.src, src_model, &err);
  if (first < 0) { Log("Could not add it: " + err); return; }
  const std::string label = g_lib.src->models[src_model].model.name + " (" + g_hooks.library[g_lib.arena].first + ")";
  const int i = AdoptNewMeshes(host, size_t(first), label);
  if (i < 0) return;
  Obj& o = g_objs[i];
  if (at_view) {
    o.pos = {g_target.x - o.pivot.x, 0, g_target.z - o.pivot.z};
    ApplyObj(o);
  }
  g_sel = i;
  Log("Added " + label + (at_view ? " at the view centre." : " in its place."));
}

void LibraryPanel() {
  {
    std::lock_guard lock(g_lib.mutex);
    if (g_lib.pending) {
      g_lib.src = std::move(g_lib.pending);
      g_lib.arena = g_lib.pending_arena;
      g_lib.models = LibraryModels(*g_lib.src);
      g_lib.pick = -1;
    }
  }
  if (g_hooks.library.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  ImGui::SetNextItemWidth(-1);
  const char* cur = g_lib.arena >= 0 ? g_hooks.library[g_lib.arena].first.c_str() : "Take models from...";
  if (ImGui::BeginCombo("##libarena", cur)) {
    for (int i = 0; i < int(g_hooks.library.size()); ++i)
      if (ImGui::Selectable(g_hooks.library[i].first.c_str(), i == g_lib.arena)) LibraryLoad(i);
    ImGui::EndCombo();
  }
  if (g_lib.loading) {
    ImGui::TextDisabled("Loading...");
    return;
  }
  if (!g_lib.src) {
    ImGui::TextWrapped("Any object of any arena: stages, titantrons, trusses, lights, signs, barriers.");
    return;
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##libfilter", "Find", g_lib.filter, sizeof g_lib.filter);
  const float scale = ImGui::GetFontSize() / 17.0f;
  ImGui::BeginChild("liblist", ImVec2(0, 180 * scale), true);
  for (int k : g_lib.models) {
    const auto& am = g_lib.src->models[k];
    if (g_lib.filter[0] && am.model.name.find(g_lib.filter) == std::string::npos) continue;
    size_t tris = 0;
    for (const auto& sm : am.model.meshes)
      for (const auto& st : sm.strips) tris += st.indices.size();
    char b[96];
    std::snprintf(b, sizeof b, "%s  (%s, %zuk)##%d", am.model.name.c_str(), ZoneName(am.zone), (tris + 999) / 1000, k);
    if (ImGui::Selectable(b, g_lib.pick == k)) g_lib.pick = k;
  }
  ImGui::EndChild();
  ImGui::BeginDisabled(g_lib.pick < 0);
  if (ImGui::Button("Add in its place")) AddFromLibrary(g_lib.pick, false);
  ImGui::SameLine();
  if (ImGui::Button("Add at the view centre")) AddFromLibrary(g_lib.pick, true);
  ImGui::EndDisabled();
}

// Pictures from a folder onto the arena's textures of the same name (<name>.png / .dds).
void RetextureFromFolder(const std::string& folder) {
  int done = 0;
  std::error_code ec;
  std::vector<std::string> keep;
  for (auto& b : g_arena->bundles)
    for (auto& t : b.textures) {
      fs::path f;
      for (const char* ext : {".png", ".dds", ".jpg", ".tga", ".bmp"})
        if (fs::exists(fs::u8path(folder) / (t.name + ext), ec)) { f = fs::u8path(folder) / (t.name + ext); break; }
      if (f.empty()) continue;
      Image img;
      DdsInfo info;
      if (!DdsInfoOf(t.data, info)) continue;
      if (f.extension() == ".dds") {
        Bytes d;
        if (!ReadFile(U8(f), d) || !DdsDecode(d, img)) continue;
      } else if (!LoadImageFile(U8(f), img)) {
        continue;
      }
      const DxtFormat fmt = info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format;
      t.data = DdsEncode(Resize(img, info.w, info.h), fmt, info.mips > 1);
      b.changed = true;
      keep.push_back(t.name);
      ++done;
    }
  ForgetTextures();
  Edited();
  Log(std::to_string(done) + " textures replaced from " + folder + " (same size and format as the arena's).");
}

void AddPanel() {
  static float box[3] = {1.0f, 1.0f, 1.0f};  // metres
  ImGui::SetNextItemWidth(-LabelRoom("Box (m)"));
  ImGui::DragFloat3("Box (m)", box, 0.05f, 0.1f, 50, "%.2f");
  if (ImGui::Button("Add box at the view centre")) AddBox(box[0] * 10, box[1] * 10, box[2] * 10);
  if (ImGui::Button("Pictures from a folder...")) {
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
      DWORD opt = 0;
      dlg->GetOptions(&opt);
      dlg->SetOptions(opt | FOS_PICKFOLDERS);
      dlg->SetTitle(L"A folder of pictures named like the arena's textures");
      IShellItem* item = nullptr;
      PWSTR path = nullptr;
      if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        RetextureFromFolder(U8(fs::path(path)));
        CoTaskMemFree(path);
      }
      if (item) item->Release();
      dlg->Release();
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Every picture in the folder whose name matches one of the arena's textures replaces it (fitted to "
                      "the same size and format): a whole retexture at once, e.g. textures exported from another game.");
  static const char* kFrom[] = {"Any 3D program (metres)", "Any 3D program (centimetres)", "Export to Blender (10 cm)"};
  ImGui::SetNextItemWidth(-LabelRoom("File from"));
  ImGui::Combo("File from", &g_file_from, kFrom, 3);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Blender, 3ds Max, SketchUp...: metres (or centimetres), Y up, textures from the file's "
                      "materials. Export to Blender: a file this editor wrote, in its own units.");
  if (ImGui::Button("Add object from a file (.obj / .fbx)...")) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.lpstrFilter = L"3D objects\0*.obj;*.fbx\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
      const int i = AddFileObject(U8(fs::path(file)), g_file_from);
      if (i >= 0) g_sel = i;
    }
  }
  ImGui::TextDisabled("Stands on the floor at the view centre. Textures: the file's materials (an OBJ's .mtl).");
}

void BudgetPanel() {
  UpdateBudget();
  std::lock_guard lock(g_budget_mutex);
  if (!g_budget_file_max) {
    ImGui::TextDisabled("Measuring...");
    return;
  }
  auto bar = [](const char* what, size_t v, size_t max) {
    const float f = float(double(v) / double(max));
    char b[96];
    std::snprintf(b, sizeof b, "%s %.1f / %.1f MB", what, v / 1048576.0, max / 1048576.0);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, f > 1 ? ImVec4(0.85f, 0.45f, 0.1f, 1) : ImVec4(0.2f, 0.65f, 0.35f, 1));
    ImGui::ProgressBar(std::min(f, 1.0f), ImVec2(-1, 0), b);
    ImGui::PopStyleColor();
  };
  bar("File", g_budget_file, g_budget_file_max);
  bar("Memory", g_budget_unpacked, g_budget_unpacked_max);
  if (g_budget_file > g_budget_file_max || g_budget_unpacked > g_budget_unpacked_max)
    ImGui::TextWrapped("Over the game's room for this arena: saving halves the biggest textures until it fits.");
  if (g_budget_busy) ImGui::TextDisabled("Measuring...");
}

// mouse in the viewport
void ViewportInput(bool hovered) {
  ImGuiIO& io = ImGui::GetIO();
  if (hovered) {
    if (io.MouseWheel != 0) g_dist = std::clamp(g_dist * std::pow(0.88f, io.MouseWheel), 10.0f, 2000.0f);
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
      const ImVec2 d = io.MouseDelta;
      g_yaw -= ScreenX() * d.x * 0.006f;
      g_pitch = std::clamp(g_pitch + d.y * 0.006f, -0.2f, 1.55f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
      const ImVec2 d = io.MouseDelta;
      const V3 fwd = Norm(g_target - Eye()), right = Norm(Cross({0, -1, 0}, fwd)), up = Cross(fwd, right);
      g_target = g_target - right * (ScreenX() * d.x * g_dist * 0.0015f) + up * (d.y * g_dist * 0.0015f);
    }
  }
  Obj* sel = g_sel >= 0 && g_sel < int(g_objs.size()) ? &g_objs[g_sel] : nullptr;
  const bool can_edit = !g_readonly && sel && sel->movable && g_tool != Tool::kSelect;
  // the start of every selected object (a drag moves them together)
  static std::vector<std::pair<int, std::array<float, 7>>> drag_starts;
  if (g_grid) {  // a 1 m grid on the floor round the view's target, 5 m lines brighter
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = g_area_lo >= 0 ? g_target.y : kMatY + 10.0f;  // (the arena floor is a metre under the mat)
    const float cx = std::round(g_target.x / 50) * 50, cz = std::round(g_target.z / 50) * 50;
    for (int k = -30; k <= 30; ++k) {
      const ImU32 col = k % 5 ? IM_COL32(255, 255, 255, 18) : IM_COL32(255, 255, 255, 45);
      ImVec2 a, b;
      if (ToScreen({cx + k * 10.f, y, cz - 300}, a) && ToScreen({cx + k * 10.f, y, cz + 300}, b)) dl->AddLine(a, b, col);
      if (ToScreen({cx - 300, y, cz + k * 10.f}, a) && ToScreen({cx + 300, y, cz + k * 10.f}, b)) dl->AddLine(a, b, col);
    }
  }
  if (g_readonly) return;  // (the view scene: no selection, no gizmo, no edits)
  // gizmo: axes from the selection's pivot (screen space), 6 m long
  ImVec2 base_s, axis_s[3];
  bool gizmo = false;
  V3 at;
  if (can_edit) {
    at = sel->pivot + sel->pos;
    const V3 axes[3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}};
    gizmo = ToScreen(at, base_s);
    for (int a = 0; a < 3 && gizmo; ++a) gizmo &= ToScreen(at + axes[a] * (g_dist * 0.12f), axis_s[a]);
    if (gizmo && g_tool == Tool::kMove) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 cols[3] = {IM_COL32(235, 60, 60, 255), IM_COL32(70, 220, 90, 255), IM_COL32(70, 130, 255, 255)};
      for (int a = 0; a < 3; ++a) {
        dl->AddLine(base_s, axis_s[a], cols[a], 3);
        dl->AddCircleFilled(axis_s[a], 5, cols[a]);
      }
    } else if (gizmo && g_tool == Tool::kRotate) {  // a ring round the pivot: drag left / right turns
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImVec2 prev;
      bool have = false;
      for (int k = 0; k <= 48; ++k) {
        const float a = k * 2 * kPi / 48, r = g_dist * 0.1f;
        ImVec2 q;
        if (!ToScreen(at + V3{std::cos(a) * r, 0, std::sin(a) * r}, q)) { have = false; continue; }
        if (have) dl->AddLine(prev, q, IM_COL32(70, 220, 90, 230), 2.5f);
        prev = q, have = true;
      }
      dl->AddText(ImVec2(base_s.x + 8, base_s.y - 20), IM_COL32(220, 255, 220, 255), "drag left / right to turn");
    } else if (gizmo && g_tool == Tool::kScale) {  // a box on the pivot: drag left / right scales
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRect(ImVec2(base_s.x - 8, base_s.y - 8), ImVec2(base_s.x + 8, base_s.y + 8), IM_COL32(255, 200, 60, 255), 2, 0, 2.5f);
      dl->AddLine(base_s, ImVec2(base_s.x + 40, base_s.y), IM_COL32(255, 200, 60, 200), 2);
      dl->AddText(ImVec2(base_s.x + 8, base_s.y - 20), IM_COL32(255, 240, 200, 255), "drag left / right to scale");
    }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g_drag_axis = -1;
    const ImVec2 m = io.MousePos;
    if (gizmo && g_tool == Tool::kMove) {
      for (int a = 0; a < 3; ++a) {
        // distance from the mouse to the axis segment
        const ImVec2 s = base_s, e = axis_s[a];
        const float vx = e.x - s.x, vy = e.y - s.y, l2 = vx * vx + vy * vy;
        const float t = l2 > 0 ? std::clamp(((m.x - s.x) * vx + (m.y - s.y) * vy) / l2, 0.0f, 1.0f) : 0;
        const float dx = s.x + vx * t - m.x, dy = s.y + vy * t - m.y;
        if (dx * dx + dy * dy < 64) { g_drag_axis = a; break; }
      }
    }
    V3 ro, rd;
    Ray(m, ro, rd);
    if (g_drag_axis < 0) {
      float t;
      const int hit = Pick(ro, rd, &t);
      if (hit >= 0 && Selected(hit) && can_edit && g_tool == Tool::kMove && !io.KeyCtrl) {
        g_drag_axis = 3;  // drag on the floor plane
        g_drag_hit = ro + rd * t;
      } else if (io.KeyCtrl) {
        ToggleSelect(hit);
      } else if (hit >= 0 && Selected(hit) && can_edit && g_tool != Tool::kSelect) {
        // (a selected object clicked in Turn / Scale: the drag below)
      } else {
        SelectOne(hit);
      }
      sel = g_sel >= 0 && g_sel < int(g_objs.size()) ? &g_objs[g_sel] : nullptr;
    }
    if (g_drag_axis >= 0 && sel) {
      PushUndo();
      g_drag_start_pos = sel->pos;
      g_drag_start_yaw = sel->yaw;
      g_drag_start_pitch = sel->pitch, g_drag_start_roll = sel->roll;
      g_drag_start_scale = sel->scale;
      g_drag_mouse = m;
    }
    if (sel && sel->movable && g_drag_axis < 0 && Selected(int(sel - g_objs.data())) && g_tool != Tool::kMove &&
        g_tool != Tool::kSelect && !io.KeyCtrl) {
      PushUndo();
      g_drag_axis = 4;  // rotate / scale by horizontal drag
      g_drag_start_yaw = sel->yaw;
      g_drag_start_pitch = sel->pitch, g_drag_start_roll = sel->roll;
      g_drag_start_scale = sel->scale;
      g_drag_mouse = m;
    }
    if (g_drag_axis >= 0) {
      drag_starts.clear();
      for (int i : Selection()) {
        const Obj& o = g_objs[i];
        if (o.movable) drag_starts.push_back({i, {o.pos.x, o.pos.y, o.pos.z, o.yaw, o.scale, o.pitch, o.roll}});
      }
    }
  }
  if (g_drag_axis >= 0 && sel && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 m = io.MousePos;
    if (g_drag_axis <= 2) {
      // along an axis: the mouse's move projected on the axis's screen direction
      const float vx = axis_s[g_drag_axis].x - base_s.x, vy = axis_s[g_drag_axis].y - base_s.y;
      const float l = std::sqrt(vx * vx + vy * vy);
      if (l > 1) {
        const float px = ((m.x - g_drag_mouse.x) * vx + (m.y - g_drag_mouse.y) * vy) / l;
        float units = px / l * (g_dist * 0.12f);
        if (g_snap) units = std::round(units / g_snap_step) * g_snap_step;
        V3 p = g_drag_start_pos;
        if (g_drag_axis == 0) p.x += units;
        if (g_drag_axis == 1) p.y -= units;
        if (g_drag_axis == 2) p.z += units;
        sel->pos = p;
      }
    } else if (g_drag_axis == 3) {
      // on the horizontal plane through the grab point
      V3 ro, rd;
      Ray(m, ro, rd);
      if (std::fabs(rd.y) > 1e-4f) {
        const float t = (g_drag_hit.y - ro.y) / rd.y;
        const V3 now = ro + rd * t;
        V3 p = g_drag_start_pos + V3{now.x - g_drag_hit.x, 0, now.z - g_drag_hit.z};
        if (g_snap) p.x = std::round(p.x / g_snap_step) * g_snap_step, p.z = std::round(p.z / g_snap_step) * g_snap_step;
        sel->pos = p;
      }
    } else if (g_drag_axis == 4) {
      const float dx = m.x - g_drag_mouse.x;
      if (g_tool == Tool::kRotate) {
        // (the turn by default; Shift: tilt forward, Alt: tilt sideways)
        const float start = io.KeyShift ? g_drag_start_pitch : io.KeyAlt ? g_drag_start_roll : g_drag_start_yaw;
        float a = start + ScreenX() * dx * 0.01f;
        if (g_snap && g_turn_snap > 0) a = std::round(a / g_turn_snap) * g_turn_snap;
        sel->yaw = g_drag_start_yaw, sel->pitch = g_drag_start_pitch, sel->roll = g_drag_start_roll;
        (io.KeyShift ? sel->pitch : io.KeyAlt ? sel->roll : sel->yaw) = a;
      } else if (g_tool == Tool::kScale) {
        sel->scale = g_drag_start_scale * std::exp(dx * 0.005f);
      }
    }
    Constrain(*sel);
    ApplyObj(*sel);
    // the others follow: the same move, the same turn, the same scale factor
    const V3 dpos = sel->pos - g_drag_start_pos;
    const float dyaw = sel->yaw - g_drag_start_yaw, fscale = g_drag_start_scale > 0 ? sel->scale / g_drag_start_scale : 1;
    const float dpitch = sel->pitch - g_drag_start_pitch, droll = sel->roll - g_drag_start_roll;
    for (const auto& [i, st] : drag_starts) {
      if (i == g_sel || i < 0 || i >= int(g_objs.size())) continue;
      Obj& o = g_objs[i];
      o.pos = V3{st[0], st[1], st[2]} + dpos;
      o.yaw = st[3] + dyaw;
      o.pitch = st[5] + dpitch, o.roll = st[6] + droll;
      o.scale = st[4] * fscale;
      Constrain(o);
      ApplyObj(o);
    }
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_drag_axis = -1;
  // keys
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyCtrl && !io.KeyShift) Undo();
    if ((ImGui::IsKeyPressed(ImGuiKey_Y) && io.KeyCtrl) || (ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyCtrl && io.KeyShift)) Redo();
    if (ImGui::IsKeyPressed(ImGuiKey_D) && io.KeyCtrl) DuplicateSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_A) && io.KeyCtrl) {  // select every movable object shown
      g_multi.clear();
      for (size_t i = 0; i < g_objs.size(); ++i)
        if (g_objs[i].movable && !g_objs[i].hidden && !g_objs[i].outside) (g_sel < 0 ? g_sel = int(i) : (g_multi.push_back(int(i)), 0));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) SelectOne(-1);
    // arrow keys nudge the selection by the snap step (Shift: x10; PageUp / PageDown: up / down)
    {
      V3 d;
      const float step = g_snap_step * (io.KeyShift ? 10 : 1);
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d.x -= ScreenX() * step;  // (left / right on the screen from the hard camera)
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d.x += ScreenX() * step;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d.z -= step;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d.z += step;
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) d.y -= step;
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) d.y += step;
      if (d.x || d.y || d.z) {
        PushUndo();
        for (int i : Selection()) {
          Obj& o = g_objs[i];
          if (!o.movable) continue;
          o.pos = o.pos + d;
          Constrain(o);
          ApplyObj(o);
        }
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_W)) g_tool = Tool::kMove;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) g_tool = Tool::kRotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) g_tool = Tool::kScale;
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) g_tool = Tool::kSelect;
    if (ImGui::IsKeyPressed(ImGuiKey_F) && sel) g_target = sel->pivot + sel->pos;
  }
}

// The viewport's picture into a PNG file the user picks.
void SaveViewPicture() {
  if (!g_rt) return;
  wchar_t file[MAX_PATH] = L"arena.png";
  OPENFILENAMEW ofn = {sizeof ofn};
  ofn.lpstrFilter = L"PNG picture\0*.png\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"png";
  ofn.Flags = OFN_OVERWRITEPROMPT;
  if (!GetSaveFileNameW(&ofn)) return;
  D3D11_TEXTURE2D_DESC td;
  g_rt->GetDesc(&td);
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return;
  g_ctx->CopyResource(staging, g_rt);
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
    Image img;
    img.w = int(td.Width), img.h = int(td.Height);
    img.rgba.resize(size_t(img.w) * img.h * 4);
    for (int y = 0; y < img.h; ++y) {
      std::memcpy(&img.rgba[size_t(y) * img.w * 4], static_cast<const uint8_t*>(ms.pData) + size_t(y) * ms.RowPitch, size_t(img.w) * 4);
      for (int x = 0; x < img.w; ++x) img.rgba[(size_t(y) * img.w + x) * 4 + 3] = 255;
    }
    g_ctx->Unmap(staging, 0);
    Log(SavePng(U8(fs::path(file)), img) ? "Picture saved: " + U8(fs::path(file)) : "The picture could not be written.");
  }
  staging->Release();
}

void ViewPreset(int which) {
  switch (which) {
    case 0: g_target = {0, -12, 0}, g_yaw = 0.0f, g_pitch = 0.32f, g_dist = 190; break;   // hard camera
    case 1: g_target = {0, -12, 0}, g_yaw = 0.0f, g_pitch = 1.5f, g_dist = 420; break;    // top
    case 2: g_target = {0, -12, 0}, g_yaw = kPi, g_pitch = 0.25f, g_dist = 260; break;    // from the stage
    case 3: g_target = {0, -12, 0}, g_yaw = kPi / 2, g_pitch = 0.25f, g_dist = 190; break;  // from the side (test aid --editor-view 3)
  }
}

}  // namespace

bool Lighting::Default() const {
  return color[0] == 1 && color[1] == 1 && color[2] == 1 && strength == 1 && crowd;
}

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Hooks& hooks) {
  g_dev = dev;
  g_ctx = ctx;
  g_hooks = hooks;
  CreatePipeline();
}

void FocusArea();  // (below)

void SetArena(Arena* arena, const std::string& title) {
  if (g_budget_thread.joinable()) g_budget_thread.join();
  g_arena = arena;
  g_title = title;
  g_budget_edits = -1;
  {
    std::lock_guard lock(g_budget_mutex);
    g_budget_file_max = 0;
  }
  BuildObjects();
  Edited();
  if (g_area_lo >= 0) FocusArea();
  if (g_test_view >= 0) ViewPreset(g_test_view);
  if (!g_test_select.empty())
    for (size_t i = 0; i < g_objs.size(); ++i)
      if (g_test_select[0] == '#' ? g_arena->models[g_objs[i].model].id == std::atoi(g_test_select.c_str() + 1)  // (#<model id>)
                                  : g_objs[i].label.rfind(g_test_select, 0) == 0) {
        g_sel = int(i);
        g_target = g_objs[i].pivot;
        break;
      }
}

void SetArea(const std::vector<std::pair<int, int>>& ids, const float* spot) {
  g_area = ids;
  g_area_lo = ids.empty() ? -1 : ids[0].first;
  for (int k = 0; k < 5; ++k) g_spot[k] = spot ? spot[k] : 0.0f;
  if (g_arena) BuildObjects();
}

// The camera on the shown area (its models' middle, from above at an angle).
void FocusArea() {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const Obj& o : g_objs) {
    if (o.outside || o.effect) continue;
    for (const auto& vs : o.base)
      for (const auto& v : vs)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  }
  if (lo[0] > hi[0]) return;
  g_target = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  g_target.y = hi[1];  // (y down: the floor)
  g_dist = std::clamp(std::max(hi[0] - lo[0], hi[2] - lo[2]) * 1.3f, 60.0f, 600.0f);
  g_yaw = 0.6f, g_pitch = 1.0f;
}

bool Busy() { return g_budget_busy; }
bool HasArena() { return g_arena != nullptr; }
int EditCount() { return g_edits; }

// ---- the view scene
struct ViewScene {
  Arena* arena = nullptr;
  std::string title;
  std::vector<Obj> objs;
  std::map<std::pair<int, int>, GpuMesh> gpu;
  std::map<std::string, ID3D11ShaderResourceView*> tex;
  std::vector<bool> model_dirty;
  int sel = -1;
  std::vector<int> multi;
  RingSpec ring;
  Lighting light;
  std::vector<Snapshot> undo, redo;
  V3 target{0, -12, 0};
  int area_lo = -1;
  std::vector<std::pair<int, int>> area;
  float spot[5] = {};
  float yaw = 0.0f, pitch = 0.32f, dist = 190;
  int edits = 0, budget_edits = -1;
  std::chrono::steady_clock::time_point last_edit;
};
ViewScene g_view;

// The view scene in, the edited one out (and back: the same call again).
void SwapScene() {
  ViewScene& v = g_view;
  std::swap(g_arena, v.arena), std::swap(g_title, v.title), std::swap(g_objs, v.objs), std::swap(g_gpu, v.gpu);
  std::swap(g_tex, v.tex), std::swap(g_model_dirty, v.model_dirty), std::swap(g_sel, v.sel), std::swap(g_multi, v.multi);
  std::swap(g_ring, v.ring), std::swap(g_light, v.light), std::swap(g_undo, v.undo), std::swap(g_redo, v.redo);
  std::swap(g_target, v.target), std::swap(g_area_lo, v.area_lo), std::swap(g_area, v.area);
  for (int k = 0; k < 5; ++k) std::swap(g_spot[k], v.spot[k]);
  std::swap(g_yaw, v.yaw), std::swap(g_pitch, v.pitch), std::swap(g_dist, v.dist);
  const int e = g_edits;
  g_edits = v.edits, v.edits = e;
  std::swap(g_budget_edits, v.budget_edits), std::swap(g_last_edit, v.last_edit);
}

std::vector<RingPart> RingPlacements(const svrfmt::Arena& a) {
  std::vector<RingPart> out;
  const auto flags = ModelFlagsOf(a);
  bool per_rope = false;  // (the twelve per-rope models replace the four side ones, as the game draws them)
  for (const auto& am : a.models) per_rope |= RoleOf(am.id) == Role::kRopePerRope;
  auto put = [&](int mi, const M4& m) {
    RingPart p;
    p.model = mi;
    std::memcpy(p.m, m.m, sizeof p.m);
    out.push_back(p);
  };
  auto rope_y = [](int r) { return kMatY - kGameBase - float(r) * kGameGap; };  // (the game's own rope heights)
  for (int mi = 0; mi < int(a.models.size()); ++mi) {
    const auto& am = a.models[size_t(mi)];
    if (am.zone != Zone::kRing) continue;
    const uint32_t id = am.id;
    switch (RoleOf(id)) {
      case Role::kNormal: {
        const auto f = flags.find(id);
        if (f != flags.end() && f->second.find("m(") != std::string::npos) break;  // (placed by an animation track)
        put(mi, M4());
        bool mirrored = false;
        if (f != flags.end())
          for (char s1 : {' ', '\t', '\r'})
            for (char s2 : {' ', '\t', '\r'}) mirrored |= f->second.find(std::string(1, s1) + "r" + s2) != std::string::npos;
        if (mirrored) {  // (the game's mirrored copy, x -> -x)
          M4 m;
          m.m[0] = -1;
          put(mi, m);
        }
        break;
      }
      case Role::kRopeSide:
        if (!per_rope)
          for (int r = 0; r < 3; ++r) put(mi, Place(RopeOffset(id) + V3{0, rope_y(r), 0}, 0));
        break;
      case Role::kRopePerRope:
        put(mi, Place(RopeOffset(kSideRope[(id - 900) % 4]) + V3{0, rope_y(int(id - 900) / 4), 0}, 0));
        break;
      case Role::kPad:
        for (int r = 0; r < 3; ++r) put(mi, Place({0, rope_y(r), 0}, 0));
        break;
      case Role::kTurnbuckle:
        for (int corner = 0; corner < 4; ++corner)
          for (int r = 0; r < 3; ++r) put(mi, TurnbuckleAt(corner, rope_y(r)));
        break;
      case Role::kApron:  // (as drawn above: 974 on the z sides, 973 on the x sides, each out along its front)
        for (int side = 0; side < 2; ++side) {
          const float yaw = (id == 974 ? 0 : kPi / 2) + float(side) * kPi, c = std::cos(yaw), s = std::sin(yaw);
          put(mi, Place({-kApronOut * s, 0, -kApronOut * c}, yaw));
        }
        break;
      default:
        break;
    }
  }
  return out;
}

void ViewArena(Arena* arena, const std::string& title, const std::string& manifest) {
  if (g_budget_thread.joinable()) g_budget_thread.join();  // (it reads the edited arena)
  SwapScene();
  ForgetGpu();
  ForgetTextures();
  g_arena = arena, g_title = title;
  FromManifest(manifest);
  if (g_arena) BuildObjects();
  ViewPreset(0);
  SwapScene();
}

void DrawView(float w, float h) {
  if (!g_view.arena) {
    ImGui::TextDisabled("No arena to show.");
    return;
  }
  SwapScene();
  g_readonly = true;
  const int iw = std::max(16, int(w)), ih = std::max(16, int(h));
  ResizeTarget(iw, ih);
  g_vp_min = ImGui::GetCursorScreenPos();
  g_vp_size = ImVec2(float(iw), float(ih));
  RenderScene();
  ImGui::InvisibleButton("view_vp", g_vp_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::GetWindowDrawList()->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(g_rt_srv)), g_vp_min, ImVec2(g_vp_min.x + iw, g_vp_min.y + ih));
  if (!g_vcam_set) ViewportInput(hovered);
  if (g_view_hint)
    ImGui::GetWindowDrawList()->AddText(ImVec2(g_vp_min.x + 8, g_vp_min.y + 6), IM_COL32(200, 200, 210, 160),
                                        "Right drag: orbit   Middle drag: pan   Wheel: zoom   (a view: nothing here changes the arena)");
  g_readonly = false;
  SwapScene();
}

void SetViewCamera(const float view[16], const float proj[16]) {
  std::memcpy(g_vcam_view.m, view, sizeof g_vcam_view.m);
  std::memcpy(g_vcam_proj.m, proj, sizeof g_vcam_proj.m);
  g_vcam_set = true;
}
void ClearViewCamera() { g_vcam_set = false; }
void SetTextureOverride(const std::string& name, ID3D11ShaderResourceView* srv) {
  if (srv) g_tex_override[name] = srv;
  else g_tex_override.erase(name);
}
void SetViewExtra(ViewExtraFn fn) { g_view_extra = std::move(fn); }
void SetGameTransparency(bool on) { g_game_alpha = on && !std::getenv("SVR2011_MM_NO_GAME_ALPHA"), g_cut_arena = nullptr; }
void SetStageTime(float seconds, bool entrance) { g_stage_time = std::max(0.0f, seconds), g_stage_entrance = entrance; }

void SetSelfLit(const std::vector<std::string>& textures, const std::vector<std::string>& masks) {
  g_self_lit = std::set<std::string>(textures.begin(), textures.end());
  g_led_mask = std::set<std::string>(masks.begin(), masks.end());
}
void SetViewHint(bool on) { g_view_hint = on; }

void ClearView() {
  SwapScene();
  ForgetGpu();
  ForgetTextures();
  g_objs.clear(), g_model_dirty.clear(), g_undo.clear(), g_redo.clear();
  g_arena = nullptr;
  SwapScene();
}

void TestEdit() {
  if (!g_arena) return;
  if (g_sel >= 0 && g_objs[g_sel].movable) {
    g_objs[g_sel].pos.x += 20;
    ApplyObj(g_objs[g_sel]);
    Log("test edit: moved " + g_objs[g_sel].label + " 2 m");
  }
  if (g_area_lo < 0) g_target = {0, 0, 45};  // ringside, in front of the hard camera (else: the area's middle)
  if (g_area_lo >= 0) {
    if (g_spot[3] > 0) g_target = {g_spot[0] + g_spot[3] * 0.5f, g_spot[1], g_spot[2]};
    AddBox(15, 15, 15);
    char at[96];
    std::snprintf(at, sizeof at, "test edit: box at %.0f %.0f %.0f on %s", g_target.x, g_target.y, g_target.z,
                  g_sel >= 0 ? g_arena->models[g_objs[g_sel].model].model.name.c_str() : "?");
    Log(at);
  } else {
    AddBox(20, 20, 20);
  }
  g_ring.ropes[2].tint = 0xFF2020;
  g_light.color[2] = 0.8f;  // a little warmer
  Log("test edit: box added, top rope red, light warmer");
}

void TestLibrary(int lib) {
  if (!g_arena || lib < 0 || lib >= int(g_hooks.library.size())) return;
  g_lib.src = std::make_unique<Arena>();
  if (!g_lib.src->Load(g_hooks.library[lib].second)) return;
  g_lib.arena = lib;
  g_lib.models = LibraryModels(*g_lib.src);
  int n = 0;
  for (int k : g_lib.models)
    if (g_lib.src->models[k].zone == Zone::kEntrance) AddFromLibrary(k, false), ++n;
  Log("test library: " + std::to_string(n) + " entrance models from " + g_hooks.library[lib].first);
}

void TestStart(int view, const std::string& select) {
  g_test_view = view;
  g_test_select = select;
}
void TestPicture(const std::string& path) { g_test_picture = path; }
void TestObjects(const std::string& add, const std::string& export_dir, int remove_part) {
  g_test_add_object = add, g_test_export_dir = export_dir, g_test_remove_part = remove_part;
}
bool RunObjectTests() {
  if (g_test_add_object.empty() && g_test_export_dir.empty() && g_test_remove_part < 0) return false;
  // the --select object: exported as it is, then a part removed
  if (g_sel >= 0 && !g_test_export_dir.empty()) ExportObject(g_objs[g_sel], g_test_export_dir);
  if (g_sel >= 0 && g_test_remove_part >= 0) {
    Obj& o = g_objs[g_sel];
    if (g_test_remove_part == 0) {  // (0: every part)
      for (size_t k = 0; k < o.meshes.size(); ++k) RemovePart(o, k);
    } else {
      RemovePart(o, size_t(g_test_remove_part - 1));
    }
  }
  if (!g_test_add_object.empty()) {
    std::string file = g_test_add_object;
    int from = 0;
    float at[3] = {};
    bool placed = false;
    if (const size_t c = file.find(','); c != std::string::npos && c > 2) {  // (not the drive's colon)
      const std::string rest = file.substr(c + 1);
      file.resize(c);
      placed = std::sscanf(rest.c_str(), "%d,%f,%f,%f", &from, &at[0], &at[1], &at[2]) == 4;
    }
    if (placed) g_target = {at[0] * 10, 0, at[2] * 10};
    const int i = AddFileObject(file, from);
    if (i >= 0 && placed) {
      Obj& o = g_objs[i];
      o.pos = {at[0] * 10 - o.pivot.x, -at[1] * 10 - o.pivot.y, at[2] * 10 - o.pivot.z};
      ApplyObj(o);
    }
    Log(std::string("test objects: ") + file + (i >= 0 ? " added" : " NOT added"));
  }
  return true;
}

void Shutdown() {
  if (g_budget_thread.joinable()) g_budget_thread.join();
  if (g_lib.loader.joinable()) g_lib.loader.join();
}
RingSpec& Ring() { return g_ring; }
Lighting& Light() { return g_light; }

// A changed static model's node sphere (world space: what the game culls the
// model by) grown to hold its meshes, added ones too.
void GrowSpheres(Arena& a) {
  for (auto& am : a.models) {
    if (!am.changed && !am.added) continue;
    if (am.model.nodes.size() != 1) continue;
    float* ns = am.model.nodes[0].sphere;
    for (const auto& s : am.model.meshes) {
      if (s.verts.size() <= 1) continue;
      const V3 c{s.sphere[0], s.sphere[1], s.sphere[2]}, n{ns[0], ns[1], ns[2]};
      const float d = std::sqrt(Dot(c - n, c - n));
      if (d + s.sphere[3] <= ns[3]) continue;
      if (ns[3] <= 0 || d + ns[3] <= s.sphere[3]) {  // (none yet, or the mesh's holds it)
        ns[0] = c.x, ns[1] = c.y, ns[2] = c.z, ns[3] = s.sphere[3];
        continue;
      }
      const float r = (d + ns[3] + s.sphere[3]) / 2;
      const V3 m = n + (c - n) * ((r - ns[3]) / std::max(d, 1e-6f));
      ns[0] = m.x, ns[1] = m.y, ns[2] = m.z, ns[3] = r;
    }
  }
}

void ApplyBuild(Arena& a) {
  GrowSpheres(a);
  if (g_light.color[0] != 1 || g_light.color[1] != 1 || g_light.color[2] != 1 || g_light.strength != 1) {
    const float c[3] = {g_light.color[0] * g_light.strength, g_light.color[1] * g_light.strength,
                        g_light.color[2] * g_light.strength};
    for (auto& am : a.models) {
      if (g_area_lo >= 0 && !am.added && !InArea(am.id)) continue;  // (backstage: the room only)
      for (auto& s : am.model.meshes)
        for (auto& p : s.params)
          if (p.type == 0x0d && p.value.size() >= 16 && (p.name == "g_f4MatAmbCol" || p.name == "g_f4MatDifCol"))
            for (int k = 0; k < 3; ++k) PutBeF(&p.value[4 * k], BeF(&p.value[4 * k]) * c[k]);
      am.changed = true;
    }
  }
  if (g_area_lo >= 0) return;  // (backstage: no crowd, no ring)
  if (!g_light.crowd) {
    const int n = HideCrowd(a);
    Log("  crowd left out (" + std::to_string(n) + " crowd models)");
  }
  RingReport rep;
  ApplyRing(a, g_ring, rep);
  for (const auto& w : rep.warnings) Log("  ring: " + w);
}

std::string ManifestLines() {
  std::string s = g_ring.Default() ? "" : g_ring.ManifestLines();
  if (!g_light.Default()) {
    char b[128];
    std::snprintf(b, sizeof b, "light.color=%.3f %.3f %.3f\nlight.strength=%.3f\n", g_light.color[0], g_light.color[1],
                  g_light.color[2], g_light.strength);
    s += b;
    if (!g_light.crowd) s += "crowd=0\n";
  }
  if (std::fabs(g_light.game_scale - 1.0f) > 0.004f) {  // (the game's arena_lights.cpp)
    char b[48];
    std::snprintf(b, sizeof b, "light_scale=%.2f\n", g_light.game_scale);
    s += b;
  }
  if (g_light.match_light == 1) s += "match_light=entrance\n";
  if (g_light.match_light == 2) s += "match_light=entrance_spot\n";
  if (g_light.tron_live) s += "titantron=live\n";
  return s;
}

void FromManifest(const std::string& text) {
  g_ring = RingSpec();
  g_ring.FromManifest(text);
  g_light = Lighting();
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("light.color=", 0) == 0) {
      std::istringstream v(line.substr(12));
      v >> g_light.color[0] >> g_light.color[1] >> g_light.color[2];
      g_light.preset = int(std::size(kPresets)) - 1;
    } else if (line == "crowd=0") {
      g_light.crowd = false;
    } else if (line.rfind("light_scale=", 0) == 0) {
      g_light.game_scale = std::clamp(std::strtof(line.c_str() + 12, nullptr), 0.5f, 2.0f);
    } else if (line == "match_light=entrance") {
      g_light.match_light = 1;
    } else if (line == "match_light=entrance_spot") {
      g_light.match_light = 2;
    } else if (line == "titantron=live") {
      g_light.tron_live = true;
    } else if (line.rfind("light.strength=", 0) == 0) {
      g_light.strength = std::strtof(line.c_str() + 15, nullptr);
      g_light.preset = int(std::size(kPresets)) - 1;
    }
  }
}

void Draw() {
  if (!g_arena) {
    ImGui::TextDisabled("Pick an arena on the Arenas page first.");
    return;
  }
  const float scale = ImGui::GetFontSize() / 17.0f;  // (the Mod Maker font is 17 px at 96 dpi)
  // toolbar
  auto tool = [](const char* label, Tool t) {
    const bool on = g_tool == t;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
    if (ImGui::Button(label)) g_tool = t;
    if (on) ImGui::PopStyleColor();
    ImGui::SameLine();
  };
  tool("Select (Q)", Tool::kSelect);
  tool("Move (W)", Tool::kMove);
  tool("Turn (E)", Tool::kRotate);
  tool("Scale (R)", Tool::kScale);
  ImGui::Checkbox("Snap", &g_snap);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70 * scale);
  const char* steps[] = {"1 cm", "5 cm", "10 cm", "25 cm", "50 cm", "1 m", "5 m"};
  const float step_v[] = {0.1f, 0.5f, 1, 2.5f, 5, 10, 50};
  int si = 4;
  for (int k = 0; k < 7; ++k)
    if (std::fabs(step_v[k] - g_snap_step) < 0.01f) si = k;
  if (ImGui::Combo("##step", &si, steps, 7)) g_snap_step = step_v[si];
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("The snap step: moves and arrow-key nudges round to it.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(62 * scale);
  {
    const char* turns[] = {"free", "1\xC2\xB0", "5\xC2\xB0", "15\xC2\xB0", "45\xC2\xB0"};
    const float turn_v[] = {0, 1, 5, 15, 45};
    int ti = 3;
    for (int k = 0; k < 5; ++k)
      if (std::fabs(turn_v[k] * kPi / 180 - g_turn_snap) < 1e-4f) ti = k;
    if (ImGui::Combo("##turn", &ti, turns, 5)) g_turn_snap = turn_v[ti] * kPi / 180;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The turn snap (with Snap on). Turn tool: drag turns, Shift+drag tilts forward,\n"
                        "Alt+drag tilts sideways; the inspector takes typed angles.");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(g_undo.empty());
  if (ImGui::Button("Undo")) Undo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(g_redo.empty());
  if (ImGui::Button("Redo")) Redo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (ImGui::Button("Hard camera")) ViewPreset(0);
  ImGui::SameLine();
  if (ImGui::Button("Top")) ViewPreset(1);
  ImGui::SameLine();
  if (ImGui::Button("From the stage")) ViewPreset(2);
  ImGui::SameLine();
  ImGui::Checkbox("Ring", &g_show_ring);
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &g_grid);
  ImGui::SameLine();
  ImGui::Checkbox("Mirror", &g_mirror);
  ImGui::SameLine();
  if (ImGui::Button("Picture...")) SaveViewPicture();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("The view as a PNG file.");
  if (g_hooks.test_in_game) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - 150 * scale));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.42f, 0.25f, 1));
    if (ImGui::Button("Test in game", ImVec2(150 * scale, 0))) g_hooks.test_in_game();
    ImGui::PopStyleColor();
  }
  const float left = 210 * scale, right = 290 * scale;
  ImGui::BeginChild("outliner", ImVec2(left, 0), true);
  Outliner();
  ImGui::EndChild();
  ImGui::SameLine();
  // viewport
  ImGui::BeginChild("viewport", ImVec2(-right, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const int w = std::max(16, int(avail.x)), h = std::max(16, int(avail.y));
  ResizeTarget(w, h);
  g_vp_min = ImGui::GetCursorScreenPos();
  g_vp_size = ImVec2(float(w), float(h));
  RenderScene();
  ImGui::InvisibleButton("vp", g_vp_size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::GetWindowDrawList()->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(g_rt_srv)), g_vp_min,
                                       ImVec2(g_vp_min.x + w, g_vp_min.y + h));
  ViewportInput(hovered);
  ImGui::GetWindowDrawList()->AddText(ImVec2(g_vp_min.x + 8, g_vp_min.y + 6), IM_COL32(200, 200, 210, 200),
                                      "Right drag: orbit   Middle drag: pan   Wheel: zoom   F: focus   Ctrl+click: add to selection   "
                                      "Arrows: nudge   Ctrl+Z / Ctrl+Y: undo / redo");
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("inspector", ImVec2(0, 0), true);
  if (ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen)) Inspector();
  if (ImGui::CollapsingHeader("Add", ImGuiTreeNodeFlags_DefaultOpen)) AddPanel();
  if (ImGui::CollapsingHeader("Library (other arenas)")) LibraryPanel();
  if (g_area_lo < 0 && ImGui::CollapsingHeader("Ring Kit", ImGuiTreeNodeFlags_DefaultOpen)) RingKitPanel();
  if (ImGui::CollapsingHeader("Lighting")) LightingPanel();
  if (ImGui::CollapsingHeader("Size budget", ImGuiTreeNodeFlags_DefaultOpen)) BudgetPanel();
  ImGui::EndChild();
}

bool Budget(BudgetInfo& out) {
  std::lock_guard lock(g_budget_mutex);
  if (!g_budget_file_max) return false;
  out.file = g_budget_file, out.file_max = g_budget_file_max;
  out.unpacked = g_budget_unpacked, out.unpacked_max = g_budget_unpacked_max;
  out.biggest = g_budget_biggest, out.biggest_id = g_budget_biggest_id;
  return true;
}

// ---- for the Simple arena maker (editor.h)

std::vector<std::string> TextureNames() {
  std::vector<std::string> out;
  if (!g_arena) return out;
  for (const auto& b : g_arena->bundles)
    for (const auto& t : b.textures) out.push_back(t.name);
  return out;
}

std::vector<std::string> ModelTextures(uint32_t lo, uint32_t hi) {
  std::vector<std::string> out;
  if (!g_arena) return out;
  for (const auto& am : g_arena->models)
    if (am.id >= lo && am.id <= hi)
      for (const auto& s : am.model.meshes) {
        const int slot = DiffuseSlot(s);
        if (slot >= 0 && slot < int(am.model.textures.size()) &&
            std::find(out.begin(), out.end(), am.model.textures[size_t(slot)]) == out.end())
          out.push_back(am.model.textures[size_t(slot)]);
      }
  return out;
}

bool TextureImage(const std::string& name, Image& out, int* w, int* h) {
  const BundleTexture* t = g_arena ? g_arena->FindTexture(name) : nullptr;
  if (!t) return false;
  DdsInfo info;
  if (!DdsInfoOf(t->data, info)) return false;
  if (w) *w = info.w;
  if (h) *h = info.h;
  return DdsDecode(t->data, out);
}

bool ReplaceTexture(const std::string& name, const Image& img) {
  if (!g_arena || img.rgba.empty()) return false;
  for (auto& b : g_arena->bundles)
    for (auto& t : b.textures) {
      if (t.name != name) continue;
      DdsInfo info;
      if (!DdsInfoOf(t.data, info)) return false;
      const DxtFormat fmt = info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format;
      t.data = DdsEncode(Resize(img, info.w, info.h), fmt, info.mips > 1);
      b.changed = true;
      ForgetTextures();
      Edited();
      return true;
    }
  return false;
}

void DrawViewport(float w, float h) {
  if (!g_arena) {
    ImGui::TextDisabled("No arena yet.");
    return;
  }
  const int iw = std::max(16, int(w)), ih = std::max(16, int(h));
  UpdateBudget();  // (the size, for the problems list: measured after edits, as the editor's panel does)
  ResizeTarget(iw, ih);
  g_vp_min = ImGui::GetCursorScreenPos();
  g_vp_size = ImVec2(float(iw), float(ih));
  RenderScene();
  ImGui::InvisibleButton("simple_vp", g_vp_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::GetWindowDrawList()->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(g_rt_srv)), g_vp_min, ImVec2(g_vp_min.x + iw, g_vp_min.y + ih));
  if (hovered) {  // (a look: turn, move and zoom the camera - nothing here selects or edits)
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0) g_dist = std::clamp(g_dist * std::pow(0.88f, io.MouseWheel), 10.0f, 2000.0f);
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      const ImVec2 d = io.MouseDelta;
      g_yaw -= ScreenX() * d.x * 0.006f;
      g_pitch = std::clamp(g_pitch + d.y * 0.006f, -0.2f, 1.55f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
      const ImVec2 d = io.MouseDelta;
      const V3 fwd = Norm(g_target - Eye()), right = Norm(Cross({0, -1, 0}, fwd)), up = Cross(fwd, right);
      g_target = g_target - right * (ScreenX() * d.x * g_dist * 0.0015f) + up * (d.y * g_dist * 0.0015f);
    }
  }
  ImGui::GetWindowDrawList()->AddText(ImVec2(g_vp_min.x + 8, g_vp_min.y + 6), IM_COL32(200, 200, 210, 200),
                                      "Drag: turn   Middle drag: move   Wheel: zoom");
}

bool TextureBytes(const std::string& name, Bytes& out) {
  const BundleTexture* t = g_arena ? g_arena->FindTexture(name) : nullptr;
  if (!t) return false;
  out = t->data;
  return true;
}

bool SetTextureBytes(const std::string& name, const Bytes& dds) {
  if (!g_arena) return false;
  for (auto& b : g_arena->bundles)
    for (auto& t : b.textures)
      if (t.name == name) {
        t.data = dds;
        b.changed = true;
        ForgetTextures();
        Edited();
        return true;
      }
  return false;
}

void SetViewPreset(int which) { ViewPreset(which); }
void DrawRingKit() { RingKitPanel(); }
void DrawLighting() { LightingPanel(); }

}  // namespace editor
