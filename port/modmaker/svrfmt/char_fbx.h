// A superstar's model (ch.pac, EPK8) to FBX: the 2011 skeleton as named bones
// in their bind (rest) pose, every model of one attire (body, hair, lashes,
// attachments) as a skinned mesh with its weights, UVs and normals, and the
// textures as PNG files beside it. Blood overlays are left out.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "bytes.h"
#include "texture.h"

namespace svrfmt {

struct CharExportReport {
  int bones = 0, models = 0, meshes = 0, vertices = 0, triangles = 0, textures = 0;
  std::vector<std::string> warnings;
};

// attire: 1-4 (the EMD entry ......<attire-1>2). Writes <out_dir>/<name>.fbx and <out_dir>/textures/*.png.
// A source bone named like hair (hair, ponytail, braid, bangs, fringe, pigtail, dread, kami, ...): FBX import
// turns chains of them under the head into the game's moving hair (svr11_build "hair_chains").
bool IsHairBoneName(const std::string& name);
// VRChat's secondary bones (J_Sec_*: the avatar's own hair / cloth physics) - not needed in the game (their weights
// go to the bone above) unless asked for.
bool IsSecondaryBoneName(const std::string& name);
// A bone named like loose cloth (jacket, coat, skirt, cape, scarf, ribbon, sleeve ...): the import's suggestion for
// the game's spring cloth chains.
bool IsClothBoneName(const std::string& name);

// opaque_colour: a colour picture's alpha is a shine level unless the mesh is a cut-out, but other programs take it as
// opacity (Blender: a see-through superstar), so those meshes link <texture>_opaque.png, the picture with no alpha; the
// FBX import takes the alpha as shine only when asked, so nothing is lost coming back.
bool ExportCharacterFbx(const Bytes& pac, int attire, const std::string& out_dir, const std::string& name, CharExportReport& rep,
                        bool opaque_colour = false);

// An FBX's skeleton: every bone node (a skeleton node, or one a skin cluster uses), parent index (-1: none), in
// file order, and for each its world rest position; false with err when the file doesn't read.
struct FbxBone {
  std::string name;
  int parent = -1;
  float pos[3] = {};
  bool skinned = false;  // a skin cluster uses it (it has weights)
};
bool ReadFbxSkeleton(const std::string& path, std::vector<FbxBone>& out, std::string& err);

// The FBX's moving chains (the game's spring kami_b chains, docs/HAIR_PHYSICS.md): runs of unmapped bones (targets
// "" - ReadFbxSkeleton order) root to tip. Hair: named like hair, under the head's source bone; hair -1 (auto) leaves
// out VRChat's secondary J_Sec_* ones, 0 none, 1 all. Cloth: the bones `cloth` says (by name; a name not in it: by
// IsClothBoneName). Hair needs 4 joints, cloth 3 (a tip is added past the last); the game moves 16 at most, hair first.
struct FbxChains {
  std::vector<std::vector<int>> hair, cloth;
  int short_hair = 0, short_cloth = 0, secondary = 0, over = 0;  // (too short; J_Sec_ left out; past the 16)
};
FbxChains FindFbxChains(const std::vector<FbxBone>& bones, const std::vector<std::string>& targets, int hair,
                        const std::map<std::string, bool>& cloth);

// FBX import (F3): the FBX's meshes posed into the 2011 bind pose (the template's A-pose: each mapped source bone turned
// to the template bone's direction, the hands rolled to its knuckle line; the mesh follows by its own skin), in game
// axes (10 cm, -Y up, -Z front, +X the character's left, hips at 0), scaled so the legs are the template's length
// (the feet on the floor; rep.scale), with the bones' weights moved to the 2011 bones (targets: one per ReadFbxSkeleton
// bone, "" = folds into its nearest mapped parent, none: root); its textures in one sheet. Writes <out_dir>/build.json
// (the input of tools/svr11_build.py), color.png and normal.png.
struct FbxImportReport {
  int meshes = 0, vertices = 0, triangles = 0, textures = 0, atlas = 0, aimed = 0, bones_used = 0;
  int reduced_from = 0;  // the vertices before the budget's reduction (0: none needed)
  double scale = 1, height_cm = 0;  // (height: the model as it came, feet to the top)
  double aim_error_deg = 0;         // (test: the posed bones against the template's directions)
  std::string aim_worst;
  std::vector<std::string> warnings, skipped, mesh_lines;
  // Reduce detail per mesh: "<name>: <before> -> <after> vertices[, <n> merges across bones]" (small meshes: "kept")
  std::vector<std::string> reduced_lines;
  int cross_merges = 0;  // vertices merged into one weighted to other bones (a stretched finger, a torn seam)
};
// The user's choices per mesh (by FbxMeshInfo::name): on / off (default: the visible ones), its kind ("skin", "eye",
// "hair", "lash"; default by its name), a picture file in place of its material's.
// A mesh's material for svr11_build ("material" in build.json): every field optional (-1 / empty: the builder's own).
struct FbxMaterial {
  float specular = -1;          // g_fSpecularLev
  std::string normal;           // a PNG normal map
  bool has_tint = false;
  float tint[3] = {1, 1, 1};    // into g_f4MatDifCol
  int cutout = -1, double_sided = -1;
  int flip = -1;                // 1: the faces as the FBX has them (Flip faces; by default they're turned to the game's)
  std::string specular_alpha;   // "texture" | "flat" | "brightness"
  std::string shine_mask;       // a picture: its brightness is where it shines (into the colour picture's alpha)
};
struct FbxImportOptions {
  std::map<std::string, FbxMaterial> material;  // by mesh name (as `include`)
  std::map<std::string, bool> include;
  std::map<std::string, std::string> kind, picture;
  int max_vertices = 0;      // Reduce detail: all meshes together cut to about this (0 = as they are; never on its own)
  bool keep_face = true;     // ... the head's vertices kept (only the rest is reduced)
  int hair = -1;             // moving hair (FindFbxChains): -1 auto (no J_Sec_*), 0 none, 1 every hair bone
  std::map<std::string, bool> cloth;  // spring cloth bones by source name (left out: by name, IsClothBoneName)
  bool gear_json = false;    // write gear.json (entrance gear pieces for svr11_build --gear-json --onto) instead of build.json
};
bool FbxToBuildInput(const std::string& fbx_path, const std::vector<std::string>& targets, const Bytes& template_pac,
                     const std::string& template_path, const std::string& out_dir, FbxImportReport& rep,
                     const FbxImportOptions& opts = {});
// The import report (before Make): what's wrong with the model, in plain words with a fix each - polycount against
// the game's budget, skin (rigid meshes, unweighted vertices, over 4 bones), rig (missing main bones, no fingers /
// eyes, unknown names), mesh (no UVs, NaN points, degenerate triangles, hidden parts), pictures (missing, unreadable,
// big, many materials), size. targets / rig: the mapping (ReadFbxSkeleton order) and DetectRig's name.
struct FbxIssue {
  int level = 0;  // 2 must fix, 1 warning, 0 tip
  std::string text, fix;
};
std::vector<FbxIssue> FbxInspect(const std::string& fbx_path, const std::vector<std::string>& targets, const std::string& rig,
                                 const FbxImportOptions& opts);
// The FBX's meshes as the import sees them: one per mesh node and material ("node / material").
struct FbxMeshInfo {
  std::string name, kind;  // (kind: the guess)
  int vertices = 0, triangles = 0;
  bool hidden = false, has_texture = false;
};
bool FbxListMeshes(const std::string& fbx_path, std::vector<FbxMeshInfo>& out, std::string& err);
// Each mesh's colour picture (by FbxMeshInfo::name, fitted into size x size) as its material has it - the Materials
// list's thumbnails. Slow the first time (a texture is looked for in the folders around the FBX): off the UI thread.
bool FbxMeshPictures(const std::string& fbx_path, int size, std::map<std::string, Image>& out);

// Model checks for the Problems list (D12; found by robert32's SvR Port Doctor): a mesh with not-a-number values
// (the game crashes), eyes far from their eye bones (they pop out when the face blinks: the eyeball vertices' centre
// against l_eye / r_eye, in game units). Empty when the model reads and passes.
struct CharModelIssues {
  std::vector<std::string> nan_meshes;
  double eye_off[2] = {0, 0};  // l_eye, r_eye: the distance (-1: no eye bone or no vertices on it)
  bool read = false;
  // (every attire) moving-hair settings (child 0x3C) the game can't take - a chain named twice or more than
  // kHairLinesMax chains: crashed / hung the match load (Asuka 2K17: 6 chains x 4)
  std::vector<std::string> hair;
  // wrestler entries ("%06d%02d", kind 2) sharing an attire digit: all but the first are never offered, and a
  // match asking for one hangs (2K16 / 2K17 ports before 2.1.2 named every attire "...0402")
  std::vector<std::string> attire_clash;
};
constexpr int kHairLinesMax = 16;  // the most chain lines a stock 0x3C has
// With moving hair (chain lines in 0x3C) an attire's models (JBOY children, unpacked) fit in this: the largest stock
// attire. More ran out of room at the match load (Asuka 2K17: crash building her blood overlay, or a hang).
constexpr size_t kChainModelBytesMax = 1006376;
// The hair-settings problems of every attire of a superstar pac (CharModelIssues::hair).
std::vector<std::string> CheckHairSettings(const Bytes& pac);
CharModelIssues CheckCharacterModel(const Bytes& pac, int attire);

// Test: the exported file read back (ufbx) against the pac - bones, bind matrices, vertices, weights; a line per check.
std::vector<std::string> CheckCharacterFbx(const Bytes& pac, int attire, const std::string& fbx_path);

}  // namespace svrfmt
