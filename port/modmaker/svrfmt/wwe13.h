// WWE '13 (Xbox 360) arenas -> SvR2011 arenas. WWE '13 keeps the SvR2011
// arena layout (EPAC-style archive -> PACH -> BPE entries, JBOY models, DDS
// texture bundles, HMD collision, the same text parameter files), with these
// differences:
//   - pac files are XMemCompress'd: "0x0FF512ED" header, a table of chunk
//     sizes, 128 KB compressed chunks of LZX frames (u16 big-endian sizes);
//   - the archive magic is "EPK8" and group counts carry a flag in the high
//     16 bits (0x0010xxxx);
//   - JBOY mesh descriptors are 0xB8 bytes (an extra stream pointer at +0x74);
//   - models, collision and crowd seats are in centimetres (SvR2011: 10 cm);
//   - materials use the WWE '13 shaders (yBG_Default, yProp_*, lightmaps).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "arena.h"
#include "jboy.h"
#include "pac.h"
#include "texture.h"

namespace svrfmt {

// ---- containers
inline bool IsXcompress(const Bytes& b) { return b.size() >= 16 && Be32(b.data()) == 0x0FF512EDu; }
bool XcompressDecode(const Bytes& in, Bytes& out, std::string* error = nullptr);
// EPAC or EPK8 (any file the two games use for arenas), unpacking 0x0FF512ED first.
bool ArchiveRead(const Bytes& in, Epac& out, std::string* error = nullptr);

// ---- models
// A WWE '13 JBOY (0xB8-byte meshes). Reads into the SvR2011 model (unconverted).
bool IsJboy13(const Bytes& b);
bool Jboy13Read(const Bytes& in, Model& out, std::string* error = nullptr);

struct Convert13Stats {
  int models = 0, meshes = 0, uvscroll = 0, lightmaps_dropped = 0, lightmaps_baked = 0;
};
// Scales positions (verts, nodes, spheres) by `scale` and turns the WWE '13
// materials into SvR2011 yDefault / yUVScroll ones (colours, diffuse,
// specular and scroll layers kept; light maps, noise and bump maps dropped).
// Unused texture names are pruned (slots renumbered).
void ConvertModel13(Model& m, float scale, Convert13Stats& st);

// An entrance prop's evtobj entry (PACH {0: model, 10: textures, ...}) from WWE '12 - 2K16: its model converted, the rest kept.
bool ConvertProp13(const Bytes& in, Bytes& out, float scale, std::string* error = nullptr);

// HMD collision in cm -> 10 cm.
Bytes ConvertHmd13(const Bytes& raw, float scale);
// c351 model flags: shadow flags of floor pieces dropped (the light numbers stay:
// the arena keeps its own light bank 0x7530).
std::string ConvertFlags13(const std::string& text);
// Light groups by part (stands 108, barrier/floor mats 102), for parts WWE '13 lit with light maps.
std::string RelightByRole13(const std::string& text, const std::map<uint32_t, std::string>& names, int stand_light);
// c364 visibility tree (node boxes: WWE '13 cm, SvR2011 metres -> scale 0.01).
std::string ConvertVisTree13(const std::string& text, float scale);
// Crowd seat files of the nested crowd pack (cm -> 10 cm).
std::string ConvertCrowdText13(const std::string& text, float scale);

// ---- lights (entry 0x7530, the arena's light sets)
// A set's levels: ground / sky colour (y 0 / 1; 1.0 = full, -1 none) and
// whether one of its directional lights (y 10-13) has colour.
struct LightLevel {
  float ground = -1, sky = -1;
  bool directional = false, found = false;
};
LightLevel LightLevelOf(const Bytes& bank, int id);
// Sets 10-14 light the wrestlers and the ring in a match (the entrance's
// light zones switch to 200-202): SvR2011's ground colour there is 0.78,
// WWE '12 / '13 / 2K14's 0.14 (their lighting model is another) - a match
// far darker than the entrance. Set 100 lights every model without l(n);
// WWE '12 / '13 / 2K14's has neither directional light nor much ambient
// (0.39 + 0.12; RAW 0.79 + 0.79, stock bg67 1.06 + 0.53 without directional
// light): those parts are flat and dark.
// -> the sets of `bank` to take from SvR2011's RAW.
constexpr float kDarkMatchGround = 0.4f, kDarkDefaultAmbient = 1.0f;
std::set<int> DarkLightSets(const Bytes& bank);
// `bank` with the sets `ids` taken from `host` (the others kept) -> the ids replaced.
Bytes ReplaceLightSets(const Bytes& bank, const Bytes& host, const std::set<int>& ids,
                       std::vector<int>* replaced = nullptr);

// The announce tables' lid (SvR2011 puts set 0x192's ji_top on it): WWE '13's
// ji_top is the black cloth its separate desk (ji_desk) covers, so on
// SvR2011's table the whole lid shows black. A dark ji_top is tinted with the
// desk's colour (mid grey without a brighter ji_desk), its grain and logo
// kept -> pictures changed (0: none dark).
int LiftTableLids(Arena& a);

// ---- the whole arena
struct Wwe13Options {
  bool host_ring = true;     // SvR2011's ring models (ids, bones, sizes: the ring code and rope physics)
  bool ring_meshes = true;   // ... carrying WWE '13's ring meshes, textures and materials
  bool no_shadows = false;   // test: format bits 0 and 1 off on every non-ring model
  std::vector<std::pair<uint32_t, uint32_t>> hide;  // test: model id ranges drawn empty
  bool no_spot_shadows = true;  // "x" on every arena model: no spotlight shadows on the canvas
  struct Shift { uint32_t lo = 0, hi = 0; float d[3] = {0, 0, 0}; bool turn = false; };
  std::vector<Shift> shift;  // test: move (and turn 90 deg) model id ranges
  std::vector<std::pair<uint32_t, uint32_t>> tiny;  // test: bounding spheres shrunk
  std::vector<std::pair<uint32_t, int>> keep_mesh;  // test: only mesh k of a model drawn
  // meshes left out (model id, mesh): a model over ~430 KB unpacked (the
  // largest any SvR2011 arena has) crashes the arena load; RR / KOTR '98's
  // stage props carry 11533 vertices of floor cables
  std::vector<std::pair<uint32_t, int>> drop_mesh;
  int draw_word = -1;        // test: force mesh descriptor +0x88
  bool no_shmap = false;     // test: rename the vis tree's SHMAP_ groups
  float mat_tone = 0.75f;    // canvas ambient/diffuse scale (SvR2011's ring lights are brighter)
  bool rope_hi = false;      // WWE '13's full rope (10922 vertices) instead of its low one (449)
  bool host_crowd = false;   // the host's crowd pack instead of WWE '13's (converted)
  bool barrier_corners = true;  // WWE '13's corner pieces stretched into the barrier gaps (its code places them)
  bool host_aprons = true;   // aprons: SvR2011's mesh and sway bones, WWE '13's pictures and colours
  // Unused pictures left out. Off: SvR2011 code objects (announce tables,
  // steps, bell from bgEtc) take an arena's pictures of the same name, so a
  // picture no arena model uses can still be on screen.
  bool prune_textures = false;
  int stand_light = 108;     // light group for the stands (role lights)
  bool role_lights = true;   // stands, barrier: the host RAW arena's light groups (WWE '13 bakes them)
  double fit = 1.0;          // textures halved until the file / unpacked size is this share of the host's
  std::vector<BundleTexture> extra_textures;  // added to the first set (replacing by name)
  Bytes card;                // entry 0xEE48 (256 x 128 DXT5 arena card), if given
};
struct Wwe13Report {
  Convert13Stats conv;
  int entries = 0, from_host = 0, dropped = 0, placeholders = 0, ring_parts = 0;
  std::vector<std::string> notes;
  std::vector<std::string> halved;
};
// wwe13: the WWE '13 bgNN.pac (compressed or not); host: the SvR2011 arena
// it plays in place of (its room: file size and memory). out: the arena.pac.
bool BuildArenaFromWwe13(const Bytes& wwe13, const Bytes& host, const Wwe13Options& opt, Bytes& out,
                         Wwe13Report& rep, std::string* error = nullptr);

}  // namespace svrfmt
