// Arena mods (arenas branch): custom arenas served in place of a host arena.
// Plan and findings: docs/ARENA_MOD_MAKER_PLAN.md.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::filesystem {
class VirtualFileSystem;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

void InstallArenaMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* fs);

// The arena select page label (an ImGui overlay).
void InstallArenaModsOverlay(rex::ui::ImGuiDrawer* drawer);

// The arena mods with their own VS theme (vs/theme.pac): {id, file} - vs_styles.cpp merges them into its overlay
// pac and gives each its group ("VA00" ...), used on the VS screen while that arena is chosen.
std::vector<std::pair<std::string, std::string>> ArenaThemePacs();
void SetArenaThemeGroup(const std::string& id, const std::string& group);

// Serve <game folder>/<relative_file> whenever the game opens arena BGnn
// (empty: back to the original). Takes effect at the next open (arenas are
// opened when they load, not held open).
void RedirectArena(int arena, const std::string& relative_file);
// The file arena BGnn plays when no custom arena is on its tile (a media
// mod's arena with its own screen pictures); RedirectArena(arena, "") goes
// back to it.
void SetArenaDefault(int arena, const std::string& relative_file);
// A match is loading (set-up to its people placed): a custom arena's loading
// pictures are swapped only then.
void SetMatchLoading(bool loading);
// Online (online_mods.cpp): the custom arena this game's next match plays
// (the arena select's pick: its folder in Mods/Arenas and the tile it plays
// on; "" when a stock arena), and another game's pick played here too (on
// that tile; "" back to none) - false when it isn't installed here.
std::string ArenaModInUse(int* tile);
// The manifest text of the custom arena the next match loads ("" when a stock arena): crowd_fight.cpp.
std::string ArenaModManifest();
bool UseArenaMod(const std::string& folder, int tile);

// String lookups (menu_hooks.cpp, r3 for the lookup from `caller`, 0 for the
// game's): the online arena screen's title on a custom arena's tile.
uint32_t ArenaSelectString(uint32_t caller);

// A superstar mod's loading picture (1024 x 512 DXT5 file; "" none) for the match being set up: the NOW LOADING
// screen shows it in place of the game's (and of a custom arena's own) while that match loads (test_match.cpp:
// person 0's, else the next person's in slot order).
void SetSuperstarLoading(const std::string& file);
// The arena number a match in that arena plays as (RedirectArena: the stock
// practice ring on another tile plays as 29); the arena itself otherwise.
int ArenaNumberFor(int arena);
// The match screen's arena read (sub_8247B6C8(screen), test_match.cpp's hook - the match loads by it): a custom
// arena's tile set to the arena it plays as.
void ScreenArenaRead(uint8_t* base, uint32_t screen);
// A match loads (menu_hooks.cpp sub_823EEB98): the live settings' and the match screen's arena become the arena a
// custom arena's tile plays as, before the load reads them.
void ArenaPlaysAsAtLoad(uint8_t* base);
// A player's pad buttons as the game read them (XInput bits, with LT / RT
// pressed as 0x400 / 0x800): LB / RB and LT / RT turn the arena select page.
void ArenaSelectPad(uint32_t user, uint16_t buttons);

// Backstage mods with an own menu row (manifest row=<label>): their bg78 plays
// only in matches from that row. area: the room (0 parking lot ... 6 catering).
struct BackstageRow {
  std::string label, file;
  std::string gimmick;  // its GMGB entry (gimmick.pac) played for the room's package ("" = the room's)
  // box=<x>,<z>,<half x>,<half z>: the fight box (where the cameras look and
  // the fighters are kept), when the area isn't where the room's was
  bool has_box = false;
  float box[4] = {};
  float camera = 0;  // camera=<units>: the match camera's farthest distance (0 = the game's)
  float camera_height = 0;  // camera_height=<units>: the match camera at least that high (0 = the game's)
  int area = -1;
  // start=<x>,<z>,<facing deg>[,<y>];...: where the fighters start (person 1, 2, ...), in place of the room's
  struct Start {
    float x, z, facing, y;
  };
  std::vector<Start> starts;
  // weapons=<model>,<x>,<y>,<z>,<yaw deg>;...: the weapons lying in the area at the bell, in place of
  // the room's (2011 weapon model numbers, world coordinates)
  struct Weapon {
    int model;
    float x, y, z, yaw;
  };
  std::vector<Weapon> weapons;
  bool no_weapons = false;  // weapons=none: no weapons lie in the area (not the room's own)
  std::string dir;  // the mod's folder (full path)
  // load_weapons=<model>,...: weapon models loaded for the match besides the area's own (the
  // game loads a hard-coded set per area; weapons= only places them) - for weapons made later
  std::vector<int> load_weapons;
  int hctp = 0;     // hctp=<room>: a SmackDown! Here Comes the Pain room (hctp_room.cpp: climbing etc.)
};
const std::vector<BackstageRow>& BackstageRows();
// The bg78 of row i for the next match (-1: the usual one).
void UseBackstageRow(int i);
extern int g_active_row;  // (the row in use, -1 none)

// The size (in 0x100 bytes) of stage entry `entry` ("/STG/0019") in the file a redirected arena serves, when it
// differs from the shipped file's - the size the game's file loader must read it at (model_mem.cpp); 0 otherwise.
uint32_t ServedStageUnits(const char* entry);

}  // namespace svr2011
