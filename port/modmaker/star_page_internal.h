// The Superstar page's project and helpers, shared with its Simple mode
// (simple_star.cpp: the guided view of the same project).
#pragma once
#include "model_materials.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "app.h"

namespace mm {
namespace star_page {

// Fighting styles: a new superstar's moves, entrance motions and starting
// attributes come from one (the game needs a full move-set). Each is a
// template from the roster underneath.
struct Style {
  const char* name;
  const char* about;
  int template_id;  // a playable superstar (its move-set, entrance motions)
  bool female;
};
constexpr int kStyleCount = 10;
extern const Style kStyles[kStyleCount];

struct StarProject {
  int style = 0;
  int ratings[7] = {74, 74, 74, 74, 74, 74, 74};
  bool ratings_set = false;  // (the style's, until moved)
  char name[32] = "", short_name[32] = "", author[64] = "", version[16] = "1.0";
  std::wstring model, song, movie, voice;
  std::string threads;   // threads.txt made in the Threads parts tab ("" = none: an own model gets threads_parts.py's)
  std::wstring cae;      // a created entrance (cae=: the game's 284-byte entrance block, "Made in the game")
  std::string cae_name;  // whose it was in the game (shown)
  std::wstring attires[4];        // [1..3]: attires 2-4 from other model pacs (their first attire)
  char attire_names[4][32] = {};  // [1..3]: their names on CHANGE ATTIRE ("" = ATTIRE N)
  int attire_entrance[4] = {-1, -1, -1, -1};  // [1..3]: attire<N>_entrance= (-1: the superstar's entrance)
  // where a ported superstar came from (superstar_port.py: source=<game> <their id>, source_entrance=, entrance_note=)
  std::string source_game, entrance_note;
  int source_id = -1, source_entrance = -1;
  Picture picture;                // its own select picture (512): else a silhouette
  Image picture_small;            // the 256 bust made with it (or own_bust)
  bool own_bust = false;          // picture_small is the user's own bust picture
  Image picture_full;             // own full-body picture (render_full.dds -> SSFB; none: render.dds)
  Image picture_load;             // the NOW LOADING picture (load.dds, 1024 x 512 DXT5; none: the arena's / the game's)
  bool game_look = false;         // the picture's tone moved toward the game's renders
  Image picture_src;              // the picture as picked (this session: the game look can be undone)
  int call = -1;                  // -1: "The Superstar"
  float height = 1.0f;            // height=<scale> from the feet (0.80-1.25; 1: left out)
  float theme_gain = 0.0f;        // theme_gain=<dB> (-20..+6; 0: left out)
  float call_gain = 0.0f;         // call_gain=<dB>: the name recording against the game's own calls (-20..+12; 0: left out)
  std::vector<Picture> signs;     // its fans' signs (up to 4; 128 x 64)
  // the SvR 2010-style extras (the game: superstar_mods.cpp)
  int pose = -1;                  // pose=<motion id>: the select / VS panel's pose (-1: the style's)
  int music = 0;                  // music= : a theme of the game's own (its music id 1-253) instead of a song file; 0 = none
  int entrance = -1;              // entrance= number (-1: the style's); a superstar's id, or 535 (Jeff Hardy's, unused)
  int victory = -1;               // victory= scene (-1: the game's; Victories())
  std::wstring victory_song;      // victory_song= his music when he wins (none: his theme)
  float victory_gain = 0.0f;      // victory_gain=<dB> (-20..+6; 0: left out)
  char announcer[32] = "";        // announcer=<NAME>: the ring announcer's own clips of that name
  char hometown_city[32] = "";    // hometown=<CITY>[,<STATE>]: the announcer's own hometown clips
  char hometown_state[32] = "";
  std::vector<int> abilities;     // abilities=a,b,... (empty: the style's)
  bool own_abilities = false;
  std::wstring moves;             // moves=moves.txt: lines 0xOFF=<move id> (a file picked as it is)
  std::map<int, int> moveset;     // the page's picks: slot (0..223, offset slot*2) -> move id, after the file's lines
  std::map<int, std::string> moveset_note;  // slot -> where a ported move came from ("same: ..." / "similar: ..." / "edited")
  std::wstring pack;              // a move pack folder (pack.txt + motions/) copied in as moves/
  // a model from an FBX (import): the file and the hand edits to its automatic bone mapping
  std::wstring fbx;
  std::map<std::string, std::string> fbx_map;  // source bone -> 2011 bone ("" = folds into its parent)
  std::map<std::string, std::string> fbx_on, fbx_kind, fbx_pic;  // per mesh ("node / material"): "1"/"0", kind, picture file
  // Simple mode (simple_star.cpp): the guided view of this same project, and its step
  bool simple = false;
  int simple_step = 0;
  // entrance gear per attire (1-4): "off" leaves it out, "from<k>" gives it the model's attire k's gear
  std::map<int, std::string> gear;
  std::map<MatKey, MatEdit> mats;  // the material panel's edits: (attire 1-4, gear, model id, mesh) -> edit
  std::map<std::string, MatEdit> fbx_mat;  // FBX import: a mesh's material for the builder (by mesh name)
  int fbx_reduce = 0;          // FBX import's Reduce detail: about this many vertices (0: off - never on its own)
  int fbx_hair = -1;           // ... moving hair: -1 auto (not VRChat's J_Sec_* bones), 0 none, 1 every hair bone
  std::map<std::string, std::string> fbx_cloth;  // ... spring cloth bones ticked ("1") / unticked ("0") by hand
  bool fbx_keep_face = true;   // ... the head left as it is
};
extern StarProject g_star;
// The overall rating as the game works it out (sub_828B7950, used by the select screen's OVERALL list and ~28 more
// places): the seven attributes summed, x 10/63 (a float), + 0.5 cut toward zero, + 1, at most 100.
inline int OverallRating(const int r[7]) {
  int sum = 0;
  for (int k = 0; k < 7; ++k) sum += r[k];
  const int v = int(float(sum) * 0.15873015f + 0.5f) + 1;
  return v > 100 ? 100 : v;
}
extern int g_view_attire;  // the attire the preview shows (1-4)
// The material panel for the model the 3D view shows (its attire, its gear): under a header of its own, or (header
// false) just the list - show_list false: none of it, its edits still shown in the 3D view.
void MaterialsSection(bool simple, bool header = true, bool show_list = true);
// The Full preview window (full_preview.cpp) of entrance n for this project (star_page.cpp).
void OpenFullPreview(int n, int start_part = 0);  // (start_part: full_preview::Request)

// Simple mode's steps; Problems tag each problem with the step that fixes it (Problem::step).
enum SimpleStep { kStepName, kStepLook, kStepAttires, kStepMusic, kStepEntrance, kStepMoves, kStepTaunts, kStepPictures,
                  kStepExtras, kStepFinish, kStepCount };

void Problems(std::vector<Problem>& p);
void Preview();  // the 3D view, the attire buttons, the select picture
bool EntrancePicker(const char* label, int& value, const char* none, float width = 320);
void ThemeGainRow();
// "A game theme...": the game's own themes by superstar, each with a play button; sets g_star.music.
void StockThemeRow();
std::string StockThemeName(int music);  // the superstars whose theme it is
void SaveMod();
void Install(bool start);
void SetPicture(const Image& src);
std::wstring StyleChPac();  // the fighting style's superstar's model (pac\ch\ch<id>.pac)
void SimpleDraw();          // simple_star.cpp
void FbxRows(bool simple);  // the FBX import, step by step
bool FbxMaterials(bool simple, bool intro = true);  // the FBX's materials (its step 3; Simple's Materials tab)
void FbxMakeButton(const char* label, float width);
void GearRows(bool simple);  // entrance gear per attire: has it, show it, leave it out, give it another attire's
std::vector<int> AttireCount();  // the attires the mod has, 1..4 (star_page.cpp)
void ThreadsOpen(int attire);    // the Threads parts tab on that attire (threads_page.cpp)
// the move-set: the picker (g_pick_slot >= 0 opens it), slot names, the preview of a pose
extern int g_pick_slot;
extern char g_pick_find[48];
extern bool g_pick_any;
extern bool g_watching;
void MovePicker();
std::string SlotLabel(int slot);
void PreviewPose(int id);
void MoveNoteTag(int slot);  // a ported slot's "same / similar / edited" tag (moves.txt's comments)
struct PoseChoice {
  int id;
  std::string label;
};
const std::vector<PoseChoice>& Poses();
std::string PoseLabel(int id);
extern const char* const kNickNames[84];
extern const char* const kAttributes[7];
bool TakeFromMod(const std::wstring& svrmod, bool everything);
// One part of a converted superstar (.svrmod) into this project: an attire (`slot` 1-3: attire 2-4), the pictures,
// the music (theme + titantron) or the extras (strength, height, abilities, announcer).
enum class StarPart { kAttire, kPictures, kMusic, kExtras };
bool TakePartFromMod(const std::wstring& svrmod, StarPart part, int slot = 1);
bool TestSaveMod(const std::wstring& file);  // (test aids: the project as a .svrmod, now)
  // a converted superstar's look (or all of it) into this project  // Model from an FBX: pick, report, bone matching, meshes, Make

}  // namespace star_page
}  // namespace mm
