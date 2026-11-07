// An arena file (pac/bg/bgNN.pac) as the Mod Maker sees it: its models,
// textures and the entries it does not edit (kept byte for byte), plus the
// Blender round trip (export: arena.fbx + textures/*.png + arena.json).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "jboy.h"
#include "pac.h"
#include "texture.h"

namespace svrfmt {

enum class Zone { kRing, kRingside, kEntrance, kFree };
const char* ZoneName(Zone z);
Zone ZoneOf(const std::string& model_name);

struct ArenaModel {
  uint32_t id = 0;  // PACH id
  Model model;
  Zone zone = Zone::kFree;
  bool changed = false;
  bool added = false;  // not in the shipped file: Save inserts it (ids sorted)
};

struct ArenaTextureSet {
  uint32_t id = 0;  // PACH id of the bundle
  std::vector<BundleTexture> textures;
  bool changed = false;
  size_t original_raw = 0;  // unpacked size as shipped (the game's budget)
};

struct Arena {
  Epac epac;
  size_t group = 0, entry = 0;          // the STG entry holding the PACH
  std::vector<PachEntry> entries;       // all entries, as stored
  std::vector<ArenaModel> models;       // top-level JBOY entries
  std::vector<ArenaTextureSet> bundles; // top-level texture bundles

  size_t original_file = 0;  // .pac size as shipped
  double fit_margin = 1.0;   // FitFile's target as a share of the budget
  // FitFile's file budget when the arena may be bigger than shipped (kArenaFileMax, 2.1.6); 0: the shipped size. Taken
  // only when the stage is the pac's last entry (StageLast): the game reads every entry at the place the shipped
  // file had it, so a bigger stage may only push the file's end out, no entry after it.
  size_t file_max = 0;
  bool StageLast() const;

  bool Load(const std::string& path, std::string* error = nullptr);
  // From memory (a mod's arena.pac). The budget (original_*) is then the
  // mod's; set it from the host arena's shipped file.
  bool LoadData(const Bytes& data, std::string* error = nullptr);
  // The game keeps each arena in fixed memory: an arena that outgrows its
  // shipped size fails to load (crash or endless NOW LOADING). Halves the
  // largest textures not in `keep` until every texture set is back within
  // its shipped size. Returns the textures that were reduced.
  std::vector<std::string> FitBudget(const std::vector<std::string>& keep);
  // Added models need room too: halves textures (not in `keep`) until the
  // whole file and its unpacked total are no bigger than shipped.
  std::vector<std::string> FitFile(const std::vector<std::string>& keep);
  size_t original_unpacked = 0;  // all entries, unpacked, as shipped
  // Rebuild the .pac: changed models/bundles re-compressed, the rest as read.
  // *error is set if a changed entry would not load (packed >= unpacked).
  Bytes Save(std::string* error = nullptr) const;
  // Texture by name across bundles (nullptr if none).
  BundleTexture* FindTexture(const std::string& name);
};

// An arena's geometry against what 2011's arenas hold: every model's vertices and strip indices, summed. The stock
// maxima (15 Oct, all 27 stock bgNN.pac): 65,450 vertices (bg16), 110,522 indices (bg16). WWE 2K16 ports over them
// (bg06 / bg12 / bg15: 78,935-91,476 vertices) crashed at load (lr 8215A008, the arena's buffers); every port that
// played was under (up to 65,431). That was the stage's heap (6 MB, heap 10): since 2.1.6 the game gives it fallback
// memory (src/ring_rules.cpp, caw_limits.h kStageHeap) and an arena may hold more - so kMaxArenaVerts is now where an
// arena needs 2.1.6 (its manifest says needs=), kMaxArenaVertsNew the most it may have. Tested 27 Oct (bg16 with its
// models repeated): 327,250 vertices / 552,610 indices / 17.6 MB with six 1,489-page superstars in a 6-man (60 fps
// at 1080p); 458,150 / 773,654 / 21.5 MB in a 1v1 and a 6-man (60 fps); 654,500 crashed (512 MB physical memory).
constexpr size_t kMaxArenaVerts = 65450, kMaxArenaIndices = 110522;
constexpr size_t kMaxArenaVertsNew = 350000, kMaxArenaIndicesNew = 590000;
// Meshes (drawn pieces): each takes render commands every frame, and too many fill the match's command queue - the
// game crashes, in a 6-man about 40 s in (27 Oct: bg16 x5, 1,770 meshes, with stock wrestlers). bg16 x3 (1,062) held
// 5 minutes with six 1,489-page superstars; kMaxArenaMeshesNew keeps headroom under it. The game's own arenas have up
// to kMaxArenaMeshes (bg01); more needs 2.1.6 (the stage heap's per-mesh arrays: fallback memory). tools/arena_port.py
// STOCK_MESHES / MAX_MESHES are the same.
constexpr size_t kMaxArenaMeshes = 446, kMaxArenaMeshesNew = 900;
// An arena's file may outgrow its slot's shipped one since 2.1.6 (the game reads its stage entry at its own size:
// src/arena_mods.cpp ServedStageUnits) - tested to 21.5 MB (above); FitFile keeps it under this (file_max).
constexpr size_t kArenaFileMax = size_t(21) << 20;
struct ArenaGeometry {
  size_t verts = 0, indices = 0, meshes = 0;
  bool Over() const { return verts > kMaxArenaVertsNew || indices > kMaxArenaIndicesNew || meshes > kMaxArenaMeshesNew; }
  bool OverStock() const {  // (needs 2.1.6)
    return verts > kMaxArenaVerts || indices > kMaxArenaIndices || meshes > kMaxArenaMeshes;
  }
};
ArenaGeometry GeometryOf(const Arena& a);
// The arena saved as `saved_size` bytes needs 2.1.6: bigger than its slot's shipped file, or more 3D detail than the
// game's own arenas (older games crash loading it).
bool ArenaNeedsNew(const Arena& a, size_t saved_size);

// The visibility tree (entry 0xC364: a text of named boxes, one per arena model, in metres on 3ds Max axes -
// X = x / 10, Y = +-z / 10, Z = -y / 10 - under a root box): every changed model's box, and its parents' up to
// the root, grown to hold its geometry as it is now (an object moved or turned in the editor would otherwise be
// outside its box: the game may leave it out of view). Boxes only grow: models that didn't move stay as they are.
// -> the boxes grown.
int GrowVisibility(Arena& a);

// A model's nodes' bounding spheres (the game culls a model whose node sphere is out of view - an object moved off
// its shipped place vanished: bg01's arena_chair02 raised over the ring, 20 Oct): each node without its own
// transform (t = r = 0, as arena models' are) refitted around the spheres of the meshes it carries (its palette
// index; all meshes for a model's only node). -> the nodes refitted.
int FitNodeSpheres(Model& m);

struct ExportReport {
  int models = 0, meshes = 0, triangles = 0, textures = 0, textures_failed = 0;
  std::vector<std::string> warnings;
};
// out_dir/arena.fbx, out_dir/textures/<name>.png, out_dir/arena.json
bool ExportArena(const Arena& a, const std::string& out_dir, const std::string& title, ExportReport& rep);

}  // namespace svrfmt
