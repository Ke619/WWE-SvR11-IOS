// The Arena page's project and helpers, shared with its Simple mode (simple_arena.cpp: the guided view of the same
// project, like simple_star.cpp for the Superstar page).
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "svrfmt/arena.h"

namespace mm {
namespace arena_page {

// The mod being made
struct Project {
  int arena = -1;                 // tile in g_arenas the mod starts from (-1: none / backstage)
  int backstage = -1;             // the area being made (else an arena)
  std::unique_ptr<svrfmt::Arena> edited;  // after an import / in the editor
  std::string fbx;                // the Blender file it came from
  Picture banner;                 // 256 x 128
  char name[96] = "";
  char author[64] = "";
  char version[16] = "1.0";
  char city[64] = "";             // city=: the entrance title card's city (empty: the base arena's)
  char crowd_area[64] = "";       // crowd_area=: crowd fighting's area beyond the barricade, "x0,z0,x1,z1" in stage x / z
  std::map<std::string, Image> vs;  // the VS screen: replaced textures of the slot's theme (or of its own theme)
  // its own VS screen theme (vs/theme.pac, tools/vs_port.py theme: VSTL / VSTI / VSTF), any disc's or 2011's;
  // empty: the slot's. The replaced textures are baked into it when the mod is made.
  svrfmt::Bytes theme_pac;
  std::string theme_label;          // vs_theme=<where it is from>, e.g. "SvR 2010 Royal Rumble"
  std::string theme_font;           // the font's, when taken from another theme
  std::string id;                   // the id of the mod it was opened from (kept: a new version replaces it)
  // the arena.pac of the mod it was opened from and the 3D editor's edit count then: unedited since, a new version
  // keeps that file as it is (no refit, no textures halved again - only the mod's other parts changed)
  svrfmt::Bytes opened_pac;
  int opened_edits = -1;
  Picture load;                     // the loading screen (1024 x 512), optional
  Picture card;                     // the entrance title card's logo (card.dds), optional
  // backstage: an area of its own (arena_mods.cpp / match_types.cpp read these)
  bool own_area = false;
  char row[32] = "";                // row=<label>: its row in ONE ON ONE -> BACKSTAGE
  char gimmick[8] = "";             // gimmick=<4 chars> with gimmick.pac
  std::wstring gimmick_pac;
  bool has_box = false;
  float box[4] = {0, 0, 30, 30};    // box=x,z,half x,half z (game units, 10 cm)
  float camera = 0, camera_height = 0;  // 0 = the game's
  std::string source;               // source=<game> bgNN: the other game's arena it was converted from
  // crowd signs made with the arena (Simple arena): installed / saved as a sign pack of their own beside it (the
  // game's signs aren't tied to an arena: they join every match's)
  std::vector<std::pair<std::string, Picture>> signs;  // (name, 128 x 64)
  std::map<std::string, Bytes> originals;  // textures as they were before Simple's pictures (this session: "Original")
};
extern Project g_proj;
extern int g_sel;

// The Simple arena's steps (Problem::step).
enum SimpleStep { kSaStart, kSaName, kSaPictures, kSaSigns, kSaRing, kSaVs, kSaLook, kSaFinish, kSaCount };
extern bool g_simple;      // the Arena page shows its Simple mode
extern int g_simple_step;

void StartProject(int tile);
// An arena file into the project in the background (prepare: before it is handed over); to_editor shows the 3D editor.
void LoadInBackground(const std::string& pac, std::function<void(svrfmt::Arena&)> prepare, bool to_editor, bool no_crowd,
                      const std::string& fbx = "");
// A new arena from nothing on `slot` (the ring, the floor, the ringside parts).
void NewArena(int slot, bool to_editor = true);
std::string ModId();
void SaveMod();
// The mod (and its crowd signs, <id>_signs.svrmod beside it) written to `file` in the background; done(ok) after.
void SaveModTo(const std::wstring& file, std::function<void(bool)> done = nullptr);
void InstallMod(bool start);
void LoadVsTheme(int slot);
void VsTab();
void ArenaProblems(std::vector<Problem>& p);
// The sign pack made with the arena: its id and files ("" / empty: none).
std::string SignsId();
std::vector<ZipEntry> SignsFiles();

// simple_arena.cpp
void SimpleDraw();

}  // namespace arena_page
}  // namespace mm
