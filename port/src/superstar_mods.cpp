// Superstar mods (arenas branch): up to 137 new playable characters from
// <game>/Mods/Superstars/<id>/ (manifest.txt: name=, short=, base=<character
// id>, song=, movie=; ch.pac: a character model pac; a "disabled" file turns
// one off). docs/SUPERSTAR_MODS.md has the whole story.
//
// The game's character database (from re_roster): ids 50..69 are the 20 DLC
// slots (real DLC uses 51..58) and many disc ids hold blank placeholder
// records; each has a record, profile and save room but nobody in it. A
// superstar mod takes one of those ids (kPool, kept in slots.txt):
// - files: the model pac's EMD entries are named "%06d%02d" (id, attire*10 +
//   kind); a copy with the slot's id in the names, and a pack of select
//   screen renders (SSFA/SSFB/SSFC "%04d" = attire*1000 + id) copied from
//   the base character's in DLC_HD.pac, both in Mods/SuperstarOverlay and
//   mounted with the game's own pac mount (sub_826A1780(path, 0)) once its
//   file system is up (end of sub_825953B0 / sub_82595428);
// - records: the base character's 260-byte roster record (0x82E407C0 +
//   index * 260, index from the u16 table at 0x82DB3610) and 1056-byte
//   profile (0x82E7C920 + index * 1056) copied to the slot, with the mod's
//   names, selectable (+221) and DLC (+257) set and its own id as "same
//   person" (+228); again after the loaders (sub_82B89E50, sub_82594C78), a
//   save load (sub_8257A218) and before every roster list is built
//   (sub_82736C68), since saves carry the records;
// - owned: DLC-flagged ids must be owned (sub_828C1288 / sub_82589198):
//   mods are.
// The select screen shows them in the EXTRA list (managers.cpp, the M tile).
#include "caller_nv.h"
#include "big_roster.h"
#include "cae_mod_motions.h"
#include "no_mods.h"
#include "caw_limits.h"
#include "mem_trace.h"
#include "superstar_mods.h"
#include "title_new.h"
#include "mod_transfer.h"
#include "p2p.h"
#include "match_box.h"
#include "superstar_threads.h"
#include "story_mods.h"
#include "universe_mods.h"
#include "universe_welcome.h"
#include "move_packs.h"
#include "startup_pump.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"
#include "crowd_signs.h"
#include "jukebox.h"
#include "media_mods.h"
#include "vs_styles.h"
#include "caw_parts.h"
#include "music.h"
#include "hd_movies.h"
#include "user_movies.h"
#include "ascii_fold.h"  // (launcher/)
#include "../modmaker/svrfmt/bytes.h"
#include "util.h"

namespace fs = std::filesystem;

REXCVAR_DEFINE_BOOL(per_mod_saves, false, "Gameplay",
                    "Experimental: each superstar mod's save data (record, moves, entrance) in Saves/.mods/<mod>.dat instead "
                    "of SaveData.dat - an uninstalled mod leaves the save clean, a reinstalled one gets its data back");

namespace {

// The ids mods take (137): disc ids whose roster record is a blank placeholder
// ("0", loaded from CHAR/DAT, not selectable) with no model, select render,
// entrance or match data anywhere in the game's pacs (61 of them; the same
// DAT / PRO entries in the disc's and the DLCs' chEtc), then the free DLC
// slots (59-69; real DLC uses 51-58). Each has a record, profile and save
// slot (the game indexes 242 ids, 0-321). (Disc ids first: a DLC slot's
// placeholder profile changes with the DLC mounted - IsBlank / RecordBlanks.)
// The 22 after 227 were added later (50 -> 72 slots); a mod keeps its id (slots.txt).
// Then 65 ids the game indexes but keeps "absent" (present flag 0x82DB3AE0 +
// id off, read only by sub_828B7078: the CHAR/DAT and CHAR/PRO loaders skip
// them, the roster getters give none): the same blank "0" placeholder records
// and profiles as the others, no model, render or titantron (108 and 109 have
// a model pac left: not used). A mod on one is made present (the hook on
// sub_828B7078 below); unused ones stay absent. 72 -> 137, the new ids after
// the old so existing mods and saves keep theirs.
constexpr uint32_t kPool[] = {111, 114, 121, 127, 128, 129, 130, 136, 141, 148, 149, 151, 152,
                              154, 155, 157, 162, 163, 167, 168, 172, 173, 181, 185, 189, 200,
                              202, 203, 204, 206, 207, 209, 213, 214, 220, 221, 223, 225, 227,
                              228, 229, 230, 232, 233, 246, 247, 252, 258, 260, 266, 269, 270,
                              273, 280, 285, 286, 302, 303, 304, 307, 320,
                              59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
                              113, 116, 119, 120, 124, 133, 134, 138, 140, 142, 144, 146, 156,
                              159, 170, 171, 180, 183, 184, 193, 194, 196, 197, 198, 199, 210,
                              211, 212, 215, 216, 217, 222, 226, 235, 236, 237, 238, 239, 240,
                              241, 242, 243, 244, 245, 248, 249, 250, 253, 254, 259, 264, 298,
                              300, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319};
static_assert(std::size(kPool) == 137);
constexpr uint32_t kOwnId = 32, kOwnId2 = 218;  // u16 own id in the record
constexpr uint32_t kSignIds = 210;               // u16[4]: the crowd signs its fans hold (id*10 + 1..4)
constexpr uint32_t kSelectTile = 226;            // u16: its tile in the select grid's portrait table
constexpr int kMinUsableMoves = 40;  // (EnsureMoveset: fewer usable moves than this - the move-set doesn't work)
bool g_moves_checked = false;        // (CheckModProfiles: the mods' moves checked since the profiles were set up)
constexpr uint32_t kIdToIndex = 0x82DB3610;  // u16 per id
constexpr uint32_t kRecords = 0x82E407C0, kRecordSize = 260;
constexpr uint32_t kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kFullName = 34, kSecondName = 102, kShortName = 170, kNameLen = 32;
constexpr uint32_t kScale = 28;  // the superstar's height scale (4.12 fixed point)
constexpr uint32_t kSelectable = 221, kDlc = 257, kSamePerson = 228, kAbilities = 230;

struct Mod {
  std::string folder, name, short_name;
  fs::path dir;       // its folder (Mods/Superstars/<folder>, or OnlineMods/Superstars/<folder>@<crc>)
  bool temp = false;  // fetched from a session's host for this run (OnlineMods): never in saves, Universe, Threads
  uint32_t base = 0, slot = 0;
  // its key (Mods/Superstars/keys.txt, 400-999, never changes): its overlay files are named and their entries stamped
  // with it, whatever id the mod holds - the id's model / render lookups are answered with the key's entries (KeyAlias)
  uint32_t key = 0;
  std::string ch_guest, ssf_guest;  // the overlay files, as the game opens them
  std::vector<std::string> attire_guests;  // extra attires' pacs (manifest attire<N>=)
  uint32_t attires = 1;                    // attires it has (its pacs' EMD models)
  uint32_t signs = 0;                      // its own crowd signs (sign1..4.dds; crowd_signs.h)
  std::vector<fs::path> sign_files;
  std::string attire_names[4];             // (manifest attire<N>_name=; attire 1: the game's ORIGINAL ATTIRE)
  uint32_t attire_pairs = 0;               // guest: 4 x {u32 name string id, s32 unlock} (sub_828C4060)
  std::string song;                 // its theme: a USER PLAYLIST name ("" = the base's)
  int movie = 0;                    // its entrance movie: a user movie id (0 = the base's)
  int music = 0;                    // a theme of the game's own (manifest music=1-253, when there's no song=; 0 = the base's)
  // every theme / movie the port has set for it (<folder>/.applied): a slot
  // still holding one of them (or the base's) follows the manifest; anything
  // else was the player's choice in the game and stays
  std::vector<std::string> songs_set;
  std::vector<int> movies_set;
  std::vector<int> musics_set;
  std::vector<int> entrances_set;  // (entrance= numbers set: a slot still holding one follows the manifest)
  int call = -1;  // name call: -1 the base's, else a Created Superstar nickname (0-83)
  // made with a fighting style (Mod Maker "Create a new superstar"): base= is
  // only its template - its own attributes, a nickname name call (The
  // Superstar unless call=) and the generic select render (unless its own)
  bool styled = false;
  int ratings[7] = {-1, -1, -1, -1, -1, -1, -1};
  fs::path voice;  // its own recorded name for the ring announcer (manifest voice=)
  // its loading picture (manifest loading=<file>, else the folder's load.dds; 1024 x 512 DXT5): the NOW LOADING
  // screen of a match it is in (arena_mods.cpp SetSuperstarLoading)
  fs::path loading;
  float call_gain = 0.0f;  // call_gain=<dB>: its level against the game's own calls (0 = as loud as they are)
  // a superstar the game's sound banks still have (manifest announcer=<NAME>,
  // e.g. JEFFHARDY): the ring announcer says it with the game's own
  // RA_*_<NAME>_* clips, in the categories where the base says its own name
  std::string version;  // (manifest version=)
  std::string announcer;
  // its hometown for the ring announcer (manifest hometown=<CITY>[,<STATE>]:
  // the game's own clips, RA_*_HTC_<CITY>_0 / RA_*_HTS_<STATE>_0, e.g.
  // hometown=CAMERONNC or hometown=DEATHVALLEY,PARTSUNKNOWN): the CAW
  // introduction ("from <city>, <state> ... <name>") with its name calls
  std::string hometown_city, hometown_state;
  // its own moves over the base's (manifest moves=<file>: lines "0xOFF=id",
  // a byte offset in the profile's move block, 0..0x1BF, and a move id)
  std::vector<std::pair<uint16_t, uint16_t>> moves;
  // the ids moves_port.py renumber moved its moves from (manifest renumbered=<old>><new> ...): a save made with the
  // older version holds the old ids - such a slot gets the new id (an old id of 16000 and up can be a 2011 record
  // with bits but no motions, which counts as usable: the save's move would play as nothing)
  std::map<uint16_t, uint16_t> renumbered;
  int entrance = -1;            // entrance number (manifest entrance=; profile +0x1C0 +0x14..+0x18)
  int attire_entrance[4] = {-1, -1, -1, -1};  // per attire (manifest attire<N>_entrance=; EntranceForAttire)
  int victory = -1;  // its victory scene when it wins a singles match (manifest victory=; VictoryOfWinner)
  fs::path victory_song;  // its music when it wins (manifest victory_song= / victory_gain=: media_mods.h SetVictoryTrack)
  float victory_gain = 0.0f;
  // a created entrance (manifest cae=<file>: a whole 284-byte entrance block as the game holds it, big-endian -
  // its five scene parts are copied in: motion, pyro and the rest per scene, as CREATE AN ENTRANCE's ADVANCED
  // CREATION makes them); its parts' hash (.applied cae=) so a player's own later edit stays
  std::vector<uint8_t> cae;
  uint32_t cae_hash = 0;
  // what it puts into a profile (base, own moves, entrance numbers, created entrance, theme, movie, victory):
  // a save's profile for the slot made for another mod - or this one before an update - isn't kept (CheckSavedProfiles)
  uint32_t fingerprint = 0;
  std::vector<uint32_t> caes_set;
  std::vector<uint8_t> abilities;  // (manifest abilities=a,b,...; record +230.., up to 8)
  // its height: a factor of the base's (manifest height=1.05; 0 = the base's)
  double height = 0;
  // its select / VS screen pose (manifest pose=): a superstar id (< 512, that
  // superstar's pose) or a pose motion id itself; 0 = its base's pose
  uint32_t pose = 0;
};
std::vector<Mod> g_mods;
std::vector<std::string> g_extra_mounts;  // other overlay pacs (smods:\<file>; AddOverlayMount)
constexpr uint32_t kAttireNameIds = 0xB100;                   // our string ids for attire names
std::vector<std::pair<uint32_t, uint32_t>> g_attire_names;  // string id -> guest text
rex::memory::Memory* g_memory = nullptr;
fs::path g_game;
bool g_mounted = false;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
using svrfmt::Rd32, svrfmt::Wr32;

// A model pac's EMD entry names ("%06d%02d": character id, attire*10 + kind)
// get the slot's id, whichever character the pac was made for (the base's
// own or another one's). Same length: the directory is patched in place.
// (An EPK8 group header's count is in dwords: 4 per 16-byte entry.)
// `only` >= 0: an extra attire's pac - its attire `only` (usually 0) becomes
// attire `as`, the rest are put out of the way (id 999999). `drop`: attires
// (bit mask) of the main pac other pacs replace, put out of the way too.
bool RenameModel(svrfmt::Bytes& d, uint32_t to, int only = -1, int as = 0, uint32_t drop = 0) {
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPK8", 4)) return false;
  char b[8];
  std::snprintf(b, sizeof b, "%06u", to);
  int n = 0;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const bool emd = !std::memcmp(&d[p], "EMD ", 4);
    const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
    p += 12;
    for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16) {
      if (!emd || !std::all_of(&d[p], &d[p + 8], [](uint8_t c) { return c >= '0' && c <= '9'; })) continue;
      const int attire = d[p + 6] - '0';
      const bool keep = only >= 0 ? attire == only : !(drop >> attire & 1);
      std::memcpy(&d[p], keep ? b : "999999", 6);
      if (keep && only >= 0) d[p + 6] = uint8_t('0' + as);
      n += keep;
    }
  }
  return n > 0;
}

// The other kinds of model a character's pac has for its attire 1 (kind = the
// entry name's last digit; 2 is the wrestler): the entrance gear superstars -
// Undertaker, CM Punk, John Cena, Shawn Michaels, John Morrison - have a kind 4
// ("%06d04": the coat / hat / shirt they come out in) -> bit mask of kinds.
// A NEW attire for one of them (attire 3 for Cena or Undertaker, through a
// guest pac or the pac overlay alike, with or without a kind 4 entry beside it)
// never finished loading the match: the loading screen waits for ever. Their
// third attires in the DLC (CM Punk's 00013122) come with the DLC's own
// chEtc.pac - per-attire character data a mod doesn't give yet. Until then
// they take replaced attires only (LoadAttireMods, the launchers' rule).
uint32_t ExtraKinds(const fs::path& pac, uint32_t id) {
  uint32_t kinds = 0;
  FILE* f = std::fopen(pac.string().c_str(), "rb");
  if (!f) return 0;
  svrfmt::Bytes d(0x4000);
  const bool ok = std::fread(d.data(), 1, d.size(), f) == d.size();
  std::fclose(f);
  if (!ok || std::memcmp(d.data(), "EPK8", 4)) return 0;
  char want[8];
  std::snprintf(want, sizeof want, "%06u", id);
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const bool emd = !std::memcmp(&d[p], "EMD ", 4);
    const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
    p += 12;
    for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16)
      if (emd && !std::memcmp(&d[p], want, 6) && d[p + 6] == '0' && d[p + 7] >= '0' && d[p + 7] <= '9' &&
          d[p + 7] != '2')
        kinds |= 1u << (d[p + 7] - '0');
  }
  return kinds;
}

// The base's select screen renders, renamed to the slot: an EPAC with
// SSFA / SSFB / SSFC.
// A mod's own select picture (render.dds 512 x 512 and render_small.dds
// 256 x 256, DXT5; the Mod Maker makes them) replaces the base's for every
// attire: SSFA and SSFB are the 512 picture, SSFC the small one. The game's
// entries are BPE-packed DDS; so are ours.
svrfmt::Bytes OwnRender(const fs::path& file, int size) {
  svrfmt::Bytes d;
  svrfmt::DdsInfo info;
  if (!svrfmt::ReadFile(file.string(), d) || !svrfmt::DdsInfoOf(d, info) || info.w != size || info.h != size)
    return {};
  return svrfmt::BpeEncode(d);  // (compressed: the render loader's buffer is sized for that)
}

bool BuildRenders(const fs::path& out, uint32_t base, uint32_t slot, const fs::path& folder, bool generic = false) {
  svrfmt::Bytes d;
  svrfmt::Epac src;
  if (!svrfmt::ReadFile((g_game / "pac" / "DLC_HD.pac").string(), d) || !svrfmt::EpacRead(d, src)) return false;
  const svrfmt::Bytes big = OwnRender(folder / "render.dds", 512), small = OwnRender(folder / "render_small.dds", 256);
  // (render_full.dds, optional: SSFB's own full-body picture; else SSFB is render.dds too)
  const svrfmt::Bytes full = OwnRender(folder / "render_full.dds", 512);
  auto own_for = [&](const std::string& type) -> const svrfmt::Bytes& {
    return type == "SSFC" ? small : (type == "SSFB" && !full.empty()) ? full : big;
  };
  svrfmt::Epac e;
  e.header = src.header;
  e.trailer = src.trailer;
  int n = 0;
  for (const auto& g : src.groups) {
    if (g.type != "SSFA" && g.type != "SSFB" && g.type != "SSFC") continue;
    svrfmt::EpacGroup og;
    og.type = g.type;
    for (const auto& en : g.entries) {
      const int key = std::atoi(en.name.c_str());
      if (generic) {  // (the game's silhouette, 9000, for every attire)
        if (key != 9000) continue;
        const svrfmt::Bytes& own = own_for(g.type);
        for (int a = 0; a < 4; ++a) {
          char gn[8];
          std::snprintf(gn, sizeof gn, "%04d", a * 1000 + int(slot));
          og.entries.push_back({gn, own.empty() ? en.data : own});
          ++n;
        }
        continue;
      }
      if (key % 1000 != int(base)) continue;
      char nm[8];
      std::snprintf(nm, sizeof nm, "%04d", (key / 1000) * 1000 + int(slot));
      const svrfmt::Bytes& own = own_for(g.type);
      og.entries.push_back({nm, own.empty() ? en.data : own});
      ++n;
    }
    if (!og.entries.empty()) e.groups.push_back(std::move(og));
  }
  if (!n) return false;
  svrfmt::Bytes o = svrfmt::EpacWrite(e);
  // (header +4: the directory's size - what the mount copies into the file
  // system's directory; DLC_HD's is far bigger)
  uint32_t toc = 0;
  for (const auto& g : e.groups) toc += 12 + 12 * uint32_t(g.entries.size());
  o[4] = uint8_t(toc), o[5] = uint8_t(toc >> 8), o[6] = uint8_t(toc >> 16), o[7] = uint8_t(toc >> 24);
  return svrfmt::WriteFile(out.string(), o);
}

// A mod's overlay file is rebuilt when its sources change: <out>.src holds
// each source's name, size and time as they were built from (times alone
// won't do: an installed mod's files carry the zip's old dates).
std::string Stamp(const std::vector<fs::path>& srcs) {
  // (the overlay format's version: a change rebuilds every overlay once -
  // 2: models kept within the match's vertex budget, FitModelBudget)
  std::string st = "overlay 2\n";
  std::error_code ec;
  for (const auto& p : srcs) {
    if (!fs::exists(p, ec)) continue;
    st += p.filename().string() + " " + std::to_string(static_cast<unsigned long long>(fs::file_size(p, ec))) + " " +
          std::to_string(static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count())) + "\n";
  }
  return st;
}

bool Current(const fs::path& out, const std::vector<fs::path>& srcs) {
  std::error_code ec;
  if (!fs::exists(out, ec)) return false;
  std::string old;
  if (FILE* t = std::fopen((out.string() + ".src").c_str(), "rb")) {
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, t)) > 0) old.append(buf, n);
    std::fclose(t);
  }
  return old == Stamp(srcs);
}

void MarkCurrent(const fs::path& out, const std::vector<fs::path>& srcs) {
  if (FILE* t = std::fopen((out.string() + ".src").c_str(), "wb")) {
    const std::string st = Stamp(srcs);
    std::fwrite(st.data(), 1, st.size(), t);
    std::fclose(t);
  }
}

// Each mod keeps its id (Mods/Superstars/slots.txt: "<folder>\t<id>"): the
// saves hold the records by id, so adding or removing a mod moves no one.
std::vector<std::pair<std::string, uint32_t>> ReadSlots(const fs::path& file) {
  std::vector<std::pair<std::string, uint32_t>> out;
  if (FILE* t = std::fopen(file.string().c_str(), "rb")) {
    char line[512];
    while (std::fgets(line, sizeof line, t)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t tab = l.rfind('\t');
      if (tab == std::string::npos) continue;
      const uint32_t id = uint32_t(std::atoi(l.c_str() + tab + 1));
      if (std::find(std::begin(kPool), std::end(kPool), id) != std::end(kPool) ||
          (svr2011::BigRosterOn() && id >= svr2011::kFirstHighId && id <= svr2011::kLastHighId))  // (big_roster: past 322)
        out.push_back({l.substr(0, tab), id});
    }
    std::fclose(t);
  }
  return out;
}

// Each mod's key (Mods/Superstars/keys.txt: "<folder>\t<key>"): ids its overlay files are stamped with, outside
// the game's (0-321) and under 1000 (a render's name is attire * 1000 + id, SSFA/%04d) - a mod can then hold any id,
// or none, with its files mounted once.
constexpr uint32_t kFirstKey = 400, kLastKey = 999;
constexpr uint32_t kNobodyId = 512;  // (the game's "nobody": never a new mod's key)
std::vector<std::pair<std::string, uint32_t>> ReadKeys(const fs::path& file) {
  std::vector<std::pair<std::string, uint32_t>> out;
  if (FILE* t = std::fopen(file.string().c_str(), "rb")) {
    char line[512];
    while (std::fgets(line, sizeof line, t)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t tab = l.rfind('\t');
      if (tab == std::string::npos) continue;
      const uint32_t k = uint32_t(std::atoi(l.c_str() + tab + 1));
      if (k >= kFirstKey && k <= kLastKey) out.push_back({l.substr(0, tab), k});
    }
    std::fclose(t);
  }
  return out;
}

// A mod's theme and entrance movie (manifest song= / movie=, files in its
// folder) go where USER PLAYLIST and USER MOVIES find them, as Community
// Creations downloads do: the song as "<Music>/<name>.<ext>" (a playlist named
// after the mod), the movie (a 320x320 .bik, the launcher's Movies tab makes
// them) as "<Custom Movies>/<name>.bik" with a user movie id.
std::string FileName(std::string s) {
  for (char& c : s)
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  if (s.size() > 39) s.resize(39);  // (the entrance keeps 39 characters of the playlist name)
  return s;
}

// The movie's file name: plain letters ("Demon Finn Bálor" -> "Demon Finn Balor") - the game's paths are 8-bit and
// USER MOVIES skips a name with accents, so such a mod's entrance played titantron 999 (2.1.7 report, Discovery 36 /
// 103). The folder's, or "mod <hash of the folder>", where the name has no plain letters at all.
std::string MovieName(const std::string& name, const std::string& folder) {
  const auto plain = [](const std::string& utf8) {
    std::string s;
    for (wchar_t c : fs::u8path(utf8).wstring()) {
      const wchar_t f = ascii_fold_char(c);
      if (f >= 32 && f < 127) s += char(f);
    }
    return s.find_first_not_of(" ._-") == std::string::npos ? std::string() : FileName(s);
  };
  std::string n = plain(name);
  if (n.empty()) n = plain(folder);
  return n.empty() ? fmt::format("mod {:08X}", uint32_t(std::hash<std::string>{}(folder))) : n;
}

bool CopyIfChanged(const fs::path& from, const fs::path& to) {
  std::error_code ec, e1, e2;
  // (same size and not older: kept - a re-encoded theme can keep its size)
  if (fs::exists(to, ec) && fs::file_size(to, ec) == fs::file_size(from, ec) &&
      fs::last_write_time(to, e1) >= fs::last_write_time(from, e2))
    return true;
  fs::create_directories(to.parent_path(), ec);
  return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

// The Music folder's playlist names the port gave mods' themes (Music\.mod_themes: "<name>\t<mod folder>"): a
// playlist name is both what USER PLAYLIST shows and what an entrance keeps, so two mods - or a mod and a player's
// own song - with the same name played one file (2.1.3 report: themes swapped between converted superstars).
std::map<std::string, std::string> g_theme_owner;
bool g_theme_owner_read = false;

fs::path ThemeOwnersFile() {
  const fs::path music = svr2011::UserMusicFolder();
  return music.empty() ? fs::path() : music / ".mod_themes";
}

void ReadThemeOwners() {
  if (g_theme_owner_read) return;
  g_theme_owner_read = true;
  std::ifstream in(ThemeOwnersFile());
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (const size_t t = l.find('\t'); t != std::string::npos) g_theme_owner[l.substr(0, t)] = l.substr(t + 1);
  }
}

void WriteThemeOwners() {
  std::ofstream out(ThemeOwnersFile(), std::ios::trunc);
  for (const auto& [name, folder] : g_theme_owner) out << name << '\t' << folder << '\n';
}

bool SameBytes(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  char x[65536], y[65536];
  while (fa && fb) {
    fa.read(x, sizeof x), fb.read(y, sizeof y);
    if (fa.gcount() != fb.gcount() || std::memcmp(x, y, size_t(fa.gcount()))) return false;
  }
  return true;
}

// The playlist name a mod's theme gets: the mod's name, unless another mod owns it or the player has a song /
// playlist of that name - then "<name> [<mod folder>]" (39 characters, as the entrance keeps). A song file the port
// wrote before this list existed (the same bytes as the mod's theme) is the mod's own.
std::string ThemeName(const Mod& m, const fs::path& music, const fs::path& theme) {
  ReadThemeOwners();
  const std::string plain = FileName(m.name);
  auto taken = [&](const std::string& n) {
    if (const auto it = g_theme_owner.find(n); it != g_theme_owner.end()) return it->second != m.folder;
    std::error_code ec;
    if (fs::is_directory(music / n, ec)) return true;
    for (const auto& e : fs::directory_iterator(music, ec)) {
      if (!e.is_regular_file(ec) || e.path().stem().string() != n) continue;
      return !SameBytes(e.path(), theme);  // (a player's own song - or the port's, from before the list)
    }
    return false;
  };
  std::string name = plain;
  if (taken(name)) {
    const std::string tag = " [" + m.folder.substr(0, 30) + "]";
    name = FileName(plain.substr(0, plain.size() + tag.size() > 39 ? 39 - tag.size() : plain.size()) + tag);
  }
  if (g_theme_owner[name] != m.folder) g_theme_owner[name] = m.folder, WriteThemeOwners();
  return name;
}

void InstallMedia(Mod& m, const fs::path& folder, const std::string& song, const std::string& movie, float song_gain) {
  std::error_code ec;
  if (!song.empty() && fs::exists(folder / song, ec)) {
    const fs::path music = svr2011::UserMusicFolder();
    const std::string theme = music.empty() ? std::string() : ThemeName(m, music, folder / song);
    const fs::path to = music / (theme + fs::path(song).extension().string());
    if (!music.empty() && CopyIfChanged(folder / song, to)) {
      m.song = theme;
      svr2011::SetSongGain(to, song_gain);
    }
  }
  // (an .mp4: played HD where the platform decodes it, else its .bik of the
  // same name - the Mod Maker makes both; hd_movies.h)
  const bool mp4 = !movie.empty() && fs::path(movie).extension() == ".mp4";
  const std::string bik = mp4 ? fs::path(movie).replace_extension(".bik").string() : movie;
  if (!movie.empty() && fs::path(bik).extension() == ".bik") {
    const fs::path movies = svr2011::UserMoviesFolder();
    const std::string file = MovieName(m.name, m.folder) + ".bik";
    bool ok = !movies.empty() && fs::exists(folder / bik, ec) && CopyIfChanged(folder / bik, movies / file);
    if (mp4 && !movies.empty() && fs::exists(folder / movie, ec) && svr2011::RegisterHdMovie(file, folder / movie)) ok = true;
    if (ok) {
      m.movie = svr2011::ReserveUserMovie(file);
      svr2011::FinishUserMovie(file);
    }
  }
  if (!m.song.empty() || m.movie)
    REXLOG_INFO("[svr2011] superstar mods: {}: theme \"{}\", movie {}", m.name, m.song, m.movie);
}

// The profile's entrance (profile +0x1C0, 284 bytes; re_entrance): music id
// u16 at +0x10 (254 = USER PLAYLIST, by the name at +0xCC, UTF-16BE), movie
// id u16 at +0x12. The base's motions and pyro stay (+0xC7 = 0: default).
constexpr uint32_t kEntrance = 0x1C0, kMusic = 0x10, kMovie = 0x12, kSongName = 0xCC, kUserPlaylist = 254;
std::string SongOf(const uint8_t* e) {  // the USER PLAYLIST name (ASCII part)
  std::string n;
  for (uint32_t i = 0; i < 40; ++i) {
    const uint16_t c = uint16_t(e[kSongName + 2 * i] << 8 | e[kSongName + 2 * i + 1]);
    if (!c) break;
    n.push_back(c < 128 ? char(c) : '?');
  }
  return n;
}

// A fresh profile (the base's copied): the mod's own moves (big-endian u16
// move ids at byte offsets in the move block, below the entrance) and its
// entrance number (+0x14, and the alternates +0x16, +0x18).
// A move the game has no usable record for (none, or one 2011 kept for a cut
// move: no category, no motion) stays the base's: such ids froze or crashed
// the game in matches and in CREATE A MOVESET (players' mods made elsewhere).
void SetOwnMoves(uint8_t* profile, const Mod& m) {
  std::string skipped;
  for (const auto& [off, id] : m.moves) {
    if (off + 1u >= kEntrance) continue;
    if (svr2011::MoveUsable(id) == 0) {
      skipped += fmt::format(" 0x{:03X}={}", off, id);
      continue;
    }
    profile[off] = uint8_t(id >> 8), profile[off + 1] = uint8_t(id);
  }
  if (!skipped.empty())
    REXLOG_WARN("[svr2011] superstar mods: {}: moves the game doesn't have, the base's kept:{}", m.folder, skipped);
  if (m.entrance >= 0)
    for (uint32_t k : {0x14u, 0x16u, 0x18u})
      profile[kEntrance + k] = uint8_t(m.entrance >> 8), profile[kEntrance + k + 1] = uint8_t(m.entrance);
}

// The mod's fingerprint (Mod::fingerprint): FNV-1a over what SetOwnMoves / SetEntranceMedia / SetCaeParts put in.
uint32_t Fingerprint(const Mod& m) {
  uint32_t h = 2166136261u;
  const auto add = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
  };
  const auto add_int = [&](int64_t v) { add(&v, sizeof v); };
  add_int(m.base);
  for (const auto& [off, id] : m.moves) add_int(int64_t(off) << 16 | id);
  add_int(m.entrance);
  for (int e : m.attire_entrance) add_int(e);
  add(m.cae.data(), m.cae.size());
  add(m.song.data(), m.song.size());
  add_int(m.movie);
  add_int(m.music);
  add_int(m.victory);
  return h;
}

// A created entrance's part of the entrance block (docs/ENTRANCE_ATTIRES.md 1): the five scene parts (34 bytes each
// from +0x1C: motion +14, pyro +10 (5000 = none), ...) and the created-entrance flags +0xC6..+0xC9 (+0xC7 != 0: the
// per-scene parts play instead of the single number at +0x14).
constexpr uint32_t kCaeSize = 284, kCaeFrom = 0x1C, kCaeTo = 0xCA;
uint32_t CaeHash(const uint8_t* e) {
  uint32_t h = 2166136261u;
  for (uint32_t k = kCaeFrom; k < kCaeTo; ++k) h = (h ^ e[k]) * 16777619u;
  return h;
}

// The mod's created entrance (cae=) into the profile while the slot's parts are the base's or one the port set
// before: a player's own CREATE AN ENTRANCE edit stays.
void SetCaeParts(uint8_t* e, const Mod& m, const uint8_t* b) {
  if (m.cae.size() != kCaeSize) return;
  const uint32_t now = CaeHash(e);
  if (now == m.cae_hash) return;
  if (std::memcmp(e + kCaeFrom, b + kCaeFrom, kCaeTo - kCaeFrom) &&
      std::find(m.caes_set.begin(), m.caes_set.end(), now) == m.caes_set.end())
    return;
  std::memcpy(e + kCaeFrom, m.cae.data() + kCaeFrom, kCaeTo - kCaeFrom);
  REXLOG_INFO("[svr2011] superstar mods: id {} entrance: created entrance from the mod (scene motions {} {} {} {} {})", m.slot,
              e[kCaeFrom + 14] << 8 | e[kCaeFrom + 15], e[kCaeFrom + 48] << 8 | e[kCaeFrom + 49],
              e[kCaeFrom + 82] << 8 | e[kCaeFrom + 83], e[kCaeFrom + 116] << 8 | e[kCaeFrom + 117],
              e[kCaeFrom + 150] << 8 | e[kCaeFrom + 151]);
}

// The profile's entrance music and movie follow the mod's manifest while they
// are still what the port set before or the base's (`base_profile`): a new
// theme or movie added to an installed mod reaches a slot a save already has.
void SetEntranceMedia(uint8_t* profile, const Mod& m, const uint8_t* base_profile) {
  uint8_t* e = profile + kEntrance;
  uint8_t before[0x100];
  std::memcpy(before, e, sizeof before);
  const uint8_t* b = base_profile + kEntrance;
  const uint16_t music = uint16_t(e[kMusic] << 8 | e[kMusic + 1]);
  const bool ours = (music == kUserPlaylist &&
                     std::find(m.songs_set.begin(), m.songs_set.end(), SongOf(e)) != m.songs_set.end()) ||
                    std::find(m.musics_set.begin(), m.musics_set.end(), int(music)) != m.musics_set.end();
  const bool bases = !std::memcmp(e + kMusic, b + kMusic, 2) &&
                     (music != kUserPlaylist || !std::memcmp(e + kSongName, b + kSongName, 80));
  if (ours || bases) {
    if (!m.song.empty()) {
      e[kMusic] = 0, e[kMusic + 1] = uint8_t(kUserPlaylist);
      for (uint32_t i = 0; i < 40; ++i) {
        const uint16_t c = i < m.song.size() && i < 39 ? uint8_t(m.song[i]) : 0;
        e[kSongName + 2 * i] = uint8_t(c >> 8), e[kSongName + 2 * i + 1] = uint8_t(c);
      }
    } else {
      std::memcpy(e + kMusic, b + kMusic, 2);
      std::memcpy(e + kSongName, b + kSongName, 80);
      if (m.music) e[kMusic] = uint8_t(m.music >> 8), e[kMusic + 1] = uint8_t(m.music);  // (a theme of the game's own)
    }
  }
  const int movie = e[kMovie] << 8 | e[kMovie + 1];
  if (std::find(m.movies_set.begin(), m.movies_set.end(), movie) != m.movies_set.end() ||
      !std::memcmp(e + kMovie, b + kMovie, 2)) {
    if (m.movie) e[kMovie] = uint8_t(m.movie >> 8), e[kMovie + 1] = uint8_t(m.movie);
    else std::memcpy(e + kMovie, b + kMovie, 2);
  }
  // The entrance number (+0x14, alternates +0x16 / +0x18) the same way: the manifest's entrance= while
  // the slot holds one the port set before, the base's, or none (0 / 0xFFFF: a save made while the slot
  // was a placeholder - CREATE AN ENTRANCE opened the Sandman's slot on 0). A player's own pick stays.
  if (m.entrance >= 0) {
    const int now = e[0x14] << 8 | e[0x15];
    const int base_e = b[0x14] << 8 | b[0x15];
    if (now == 0 || now == 0xFFFF || now == base_e ||
        std::find(m.entrances_set.begin(), m.entrances_set.end(), now) != m.entrances_set.end())
      for (uint32_t k : {0x14u, 0x16u, 0x18u})
        e[k] = uint8_t(m.entrance >> 8), e[k + 1] = uint8_t(m.entrance);
  }
  SetCaeParts(e, m, b);
  if (std::memcmp(before, e, sizeof before))
    REXLOG_INFO("[svr2011] superstar mods: id {} entrance: music {}{}, movie {}", m.slot,
                int(e[kMusic] << 8 | e[kMusic + 1]), m.song.empty() ? "" : " (" + m.song + ")",
                int(e[kMovie] << 8 | e[kMovie + 1]));
}

// <folder>/.applied: "song=<name>" / "movie=<id>" / "music=<id>" / "entrance=<n>" lines, all ever set.
void ReadApplied(Mod& m, const fs::path& folder) {
  if (FILE* t = std::fopen((folder / ".applied").string().c_str(), "rb")) {
    char line[256];
    while (std::fgets(line, sizeof line, t)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      if (l.rfind("song=", 0) == 0) m.songs_set.push_back(l.substr(5));
      if (l.rfind("movie=", 0) == 0) m.movies_set.push_back(std::atoi(l.c_str() + 6));
      if (l.rfind("music=", 0) == 0) m.musics_set.push_back(std::atoi(l.c_str() + 6));
      if (l.rfind("entrance=", 0) == 0) m.entrances_set.push_back(std::atoi(l.c_str() + 9));
      if (l.rfind("cae=", 0) == 0) m.caes_set.push_back(uint32_t(std::strtoul(l.c_str() + 4, nullptr, 16)));
    }
    std::fclose(t);
  }
  bool add_song = !m.song.empty() && std::find(m.songs_set.begin(), m.songs_set.end(), m.song) == m.songs_set.end();
  bool add_movie = m.movie && std::find(m.movies_set.begin(), m.movies_set.end(), m.movie) == m.movies_set.end();
  bool add_entrance = m.entrance >= 0 &&
                      std::find(m.entrances_set.begin(), m.entrances_set.end(), m.entrance) == m.entrances_set.end();
  bool add_music = m.music && std::find(m.musics_set.begin(), m.musics_set.end(), m.music) == m.musics_set.end();
  bool add_cae = m.cae_hash && std::find(m.caes_set.begin(), m.caes_set.end(), m.cae_hash) == m.caes_set.end();
  if (!add_song && !add_movie && !add_entrance && !add_music && !add_cae) return;
  if (FILE* t = std::fopen((folder / ".applied").string().c_str(), "ab")) {
    if (add_song) std::fprintf(t, "song=%s\n", m.song.c_str()), m.songs_set.push_back(m.song);
    if (add_movie) std::fprintf(t, "movie=%d\n", m.movie), m.movies_set.push_back(m.movie);
    if (add_entrance) std::fprintf(t, "entrance=%d\n", m.entrance), m.entrances_set.push_back(m.entrance);
    if (add_music) std::fprintf(t, "music=%d\n", m.music), m.musics_set.push_back(m.music);
    if (add_cae) std::fprintf(t, "cae=%08x\n", m.cae_hash), m.caes_set.push_back(m.cae_hash);
    std::fclose(t);
  }
}

// Which attires (bit mask) a model pac has for character `id` (its EMD names).
uint32_t AttireMask(const fs::path& pac, uint32_t id) {
  uint32_t mask = 0;
  FILE* f = std::fopen(pac.string().c_str(), "rb");
  if (!f) return 0;
  svrfmt::Bytes d(0x4000);
  const bool ok = std::fread(d.data(), 1, d.size(), f) == d.size();
  std::fclose(f);
  if (!ok || std::memcmp(d.data(), "EPK8", 4)) return 0;
  char want[8];
  std::snprintf(want, sizeof want, "%06u", id);
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const bool emd = !std::memcmp(&d[p], "EMD ", 4);
    const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
    p += 12;
    for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16)
      if (emd && !std::memcmp(&d[p], want, 6) && d[p + 6] >= '0' && d[p + 6] <= '3' && d[p + 7] == '2')
        mask |= 1u << (d[p + 6] - '0');
  }
  return mask;
}

// ---- A wrestler's vertex memory in a match (tools/model_budget.py)
// Each wrestler's model gets a 768 KB physical window (the game's XMemAlloc
// allocator 128): 192 pages of 4 KB. The loader (sub_826E73F8) makes a vertex
// buffer (vertices x 32 bytes with one weight block, x 44 with 2-4) and an
// index buffer (x 2) per mesh, each rounded up to pages. Over 192 the next one
// gets nothing and the match load crashes (an unchecked null). The largest
// stock models fill it exactly. A mod's model over it: its blood overlays
// (children 0x7532, 0x7531, 0x7530; stock ch164 has none) are left out of the
// overlay copy until it fits, else the mod is left out - with a log line. With the big-model memory (model_mem.cpp:
// a full window takes fallback memory) the limit is ModelPageLimit() pages (caw_limits.h), not 192.

uint32_t JboyPages(const svrfmt::Bytes& d) {
  if (d.size() < 0x48 || std::memcmp(d.data(), "JBOY", 4)) return 0;
  auto r32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? Rd32(&d[o]) : 0; };
  constexpr size_t B = 8;
  const uint32_t nmesh = r32(0x18), mptr = r32(0x1C);
  uint32_t pages = 0;
  for (uint32_t i = 0; i < nmesh && i < 4096; ++i) {
    const size_t o = B + mptr + 0xB4 * size_t(i);
    if (o + 0xB4 > d.size()) break;
    const uint32_t verts = r32(o), strips = r32(o + 4), blocks = r32(o + 0x5C), iptr = r32(o + 0x98);
    uint64_t idx = 0;
    for (uint32_t k = 0; k < strips && k < 4096; ++k) idx += r32(B + iptr + 12 * size_t(k) + 4);
    const uint64_t vb = uint64_t(verts) * (blocks <= 1 ? 32 : 44), ib = idx * 2;
    pages += uint32_t((vb + 4095) / 4096 + (ib + 4095) / 4096);
  }
  return pages;
}

// Pages of one EMD entry (an attire's PACH); `kids` gets its children.
uint32_t EntryPages(const svrfmt::Bytes& entry, std::vector<svrfmt::PachEntry>* kids = nullptr) {
  std::vector<svrfmt::PachEntry> k;
  if (!svrfmt::PachRead(svrfmt::Unpack(entry), k)) return 0;
  uint32_t pages = 0;
  for (const auto& c : k) pages += JboyPages(svrfmt::Unpack(c.data));
  if (kids) *kids = std::move(k);
  return pages;
}

// Keeps a model pac's attires (kind 2 entries) within the budget: blood
// overlays left out where needed. -> false: an attire still doesn't fit.
// `notes` gets what was done.
bool FitModelBudget(svrfmt::Bytes& pac, std::string& notes) {
  svrfmt::Epac e;
  if (!svrfmt::EpacRead(pac, e)) return true;  // (not readable here: left to the game)
  bool changed = false, fits = true;
  for (auto& g : e.groups) {
    if (g.type != "EMD ") continue;
    for (auto& en : g.entries) {
      if (en.name.size() < 8 || en.name[7] != '2') continue;
      std::vector<svrfmt::PachEntry> kids;
      uint32_t pages = EntryPages(en.data, &kids);
      const uint32_t before = pages;
      for (uint32_t drop : {0x7532u, 0x7531u, 0x7530u}) {
        if (pages <= svr2011::ModelPageLimit()) break;
        const auto it = std::find_if(kids.begin(), kids.end(), [&](const svrfmt::PachEntry& c) { return c.id == drop; });
        if (it == kids.end()) continue;
        pages -= JboyPages(svrfmt::Unpack(it->data));
        kids.erase(it);
        en.data = svrfmt::PachWrite(kids);
        changed = true;
      }
      if (pages != before)
        notes += " attire " + std::to_string((en.name[6] - '0') + 1) + ": " + std::to_string(before) + " -> " +
                 std::to_string(pages) + " pages (blood overlays left out);";
      if (pages > svr2011::ModelPageLimit()) {
        fits = false;
        notes += " attire " + std::to_string((en.name[6] - '0') + 1) + ": " + std::to_string(pages) + " pages;";
      }
    }
  }
  if (changed) pac = svrfmt::EpacWrite(e);
  return fits;
}

void LoadMods() {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Superstars";
  if (!fs::is_directory(dir, ec)) return;
  if (FILE* t = std::fopen((dir / "limit.txt").string().c_str(), "wb")) {  // (the launcher's count: Mods tab)
    std::fprintf(t, "%u\n", unsigned(std::size(kPool)));
    std::fclose(t);
  }
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "ch.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  // (then the mods fetched from a session's host, this run only: RESTART AND REJOIN - mod_transfer.h)
  std::vector<std::pair<fs::path, bool>> sources;
  for (const auto& f : folders) sources.push_back({f, false});
  const fs::path overlay = g_game / "Mods" / "SuperstarOverlay";
  const fs::path online_overlay = overlay / "online";  // (smods:\online\..., cleared at every start)
  fs::remove_all(online_overlay, ec);
  if (svr2011::OnlineModsLoad()) {
    std::vector<fs::path> fetched;
    for (const auto& e : fs::directory_iterator(svr2011::OnlineModsRoot() / "Superstars", ec))
      if (e.is_directory() && fs::exists(e.path() / "ch.pac", ec)) fetched.push_back(e.path());
    std::sort(fetched.begin(), fetched.end());
    for (const auto& f : fetched) sources.push_back({f, true});
    if (!fetched.empty()) fs::create_directories(online_overlay, ec);
  }
  fs::create_directories(overlay, ec);
  auto slots = ReadSlots(dir / "slots.txt");
  bool slots_changed = false;
  auto keys = ReadKeys(dir / "keys.txt");
  bool keys_changed = false;
  std::set<uint32_t> keys_taken;  // (this run's, the fetched mods' too)
  for (const auto& k : keys) keys_taken.insert(k.second);
  auto key_of = [&](const std::string& folder, bool temp) -> uint32_t {
    if (!temp)
      for (const auto& k : keys)
        if (k.first == folder) return k.second;
    for (uint32_t k = kFirstKey; k <= kLastKey; ++k) {
      if (k == kNobodyId || keys_taken.count(k)) continue;  // (512: see sub_828B8190's hook)
      keys_taken.insert(k);
      if (!temp) keys.push_back({folder, k}), keys_changed = true;  // (a fetched mod's: this run only)
      return k;
    }
    return 0;
  };
  std::vector<std::string> left_out;  // (over the limit: Mods/Superstars/left_out.txt, for the launcher)
  auto slot_of = [&](const std::string& folder) -> uint32_t {
    for (const auto& s : slots)
      if (s.first == folder) return s.second;
    for (uint32_t id : kPool) {
      bool taken = false;
      for (const auto& s : slots) taken |= s.second == id;
      if (taken) continue;
      slots.push_back({folder, id});
      slots_changed = true;
      return id;
    }
    // (big_roster: the pool taken - ids past the game's 322, entries past its 242)
    if (svr2011::BigRosterOn())
      for (uint32_t id = svr2011::kFirstHighId; id <= svr2011::kLastHighId; ++id) {
        bool taken = false;
        for (const auto& s : slots) taken |= s.second == id;
        if (taken) continue;
        slots.push_back({folder, id});
        slots_changed = true;
        return id;
      }
    // All ids taken: one held by a mod whose folder is gone (removed by hand,
    // or by an older launcher that left its line) or switched off ("disabled":
    // the player's way to choose which mods play when there are more than
    // ids) is given to this one. Only then - a mod moved out for a while and
    // back keeps its id (and the edits saves hold for it) while there is room.
    // Mods are taken in folder order, those holding an id first in effect (an
    // id never moves from a mod that's on), so who waits is the same each start.
    for (auto& s : slots) {
      std::error_code e;
      const fs::path held = dir / fs::path(s.first);  // (folder names as written: ANSI code page)
      const bool gone = !fs::exists(held / "ch.pac", e), off = fs::exists(held / "disabled", e);
      if (!gone && !off) continue;
      REXLOG_INFO("[svr2011] superstar mods: id {} of {} (its folder is {}) goes to {}", s.second, s.first,
                  gone ? "gone" : "switched off", folder);
      s.first = folder;
      slots_changed = true;
      return s.second;
    }
    return 0;
  };
  // (a fetched mod's id: a free one of the pool - none the player's slots hold, so a player's mod never meets
  // its edits; this run only, never written)
  std::set<uint32_t> temp_taken;
  auto temp_slot = [&]() -> uint32_t {
    for (uint32_t id : kPool) {
      bool taken = temp_taken.count(id) != 0;
      for (const auto& s : slots) taken |= s.second == id;
      if (taken) continue;
      temp_taken.insert(id);
      return id;
    }
    return 0;
  };
  for (const auto& [f, temp] : sources) {
    svr2011::KeepWindowAlive();  // (startup_pump.h)
    const fs::path& ov = temp ? online_overlay : overlay;
    const std::string guest = temp ? "smods:\\online\\" : "smods:\\";
    Mod m;
    m.folder = f.filename().string();
    m.dir = f, m.temp = temp;
    if (temp) {
      m.folder = m.folder.substr(0, m.folder.rfind('@'));
      if (std::any_of(g_mods.begin(), g_mods.end(), [&](const Mod& o) { return o.folder == m.folder; })) {
        REXLOG_INFO("[svr2011] superstar mods: {} fetched from the host, but this game has its own - its own plays",
                    m.folder);
        continue;
      }
    }
    m.name = m.short_name = m.folder;
    std::string song, movie, moves;
    float song_gain = 0.0f;  // theme_gain=<dB>: the theme's level (-20..+6, the Mod Maker's loudness match)
    std::string attires[4];  // attire<N>= (1-4): another pac's first attire as attire N
    if (FILE* t = std::fopen((f / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) m.name = l.substr(5);
        if (l.rfind("short=", 0) == 0) m.short_name = l.substr(6);
        if (l.rfind("base=", 0) == 0) m.base = uint32_t(std::atoi(l.c_str() + 5));
        if (l.rfind("song=", 0) == 0) song = l.substr(5);
        if (l.rfind("theme_gain=", 0) == 0) song_gain = std::clamp(float(std::atof(l.c_str() + 11)), -20.0f, 6.0f);
        if (l.rfind("call_gain=", 0) == 0) m.call_gain = std::clamp(float(std::atof(l.c_str() + 10)), -20.0f, 12.0f);
        if (l.rfind("renumbered=", 0) == 0) {
          std::istringstream in(l.substr(11));
          for (std::string t; in >> t;)
            if (const size_t g = t.find('>'); g != std::string::npos) {
              const int from = std::atoi(t.c_str()), to = std::atoi(t.c_str() + g + 1);
              if (from > 0 && from < 0x8000 && to > 0 && to < 0x8000) m.renumbered[uint16_t(from)] = uint16_t(to);
            }
        }
        if (l.rfind("movie=", 0) == 0) movie = l.substr(6);
        if (l.rfind("voice=", 0) == 0 && l.size() > 6) m.voice = f / l.substr(6);
        if (l.rfind("loading=", 0) == 0 && l.size() > 8) m.loading = f / l.substr(8);
        if (l.rfind("style=", 0) == 0) m.styled = true;
        if (l.rfind("ratings=", 0) == 0) {
          const char* q = l.c_str() + 8;
          for (int k = 0; k < 7 && *q; ++k) {
            m.ratings[k] = std::clamp(std::atoi(q), 1, 99);
            while (*q && *q != ',') ++q;
            if (*q == ',') ++q;
          }
        }
        if (l.rfind("attire", 0) == 0 && l.size() > 8 && l[6] >= '1' && l[6] <= '4' && l[7] == '=')
          attires[l[6] - '1'] = l.substr(8);
        if (l.rfind("attire", 0) == 0 && l.size() > 12 && l[6] >= '1' && l[6] <= '4' && !l.compare(7, 6, "_name="))
          m.attire_names[l[6] - '1'] = l.substr(13);
        if (l.rfind("call=", 0) == 0 && l.size() > 5 && std::isdigit(uint8_t(l[5])))
          m.call = std::clamp(std::atoi(l.c_str() + 5), 0, 83);
        if (l.rfind("hometown=", 0) == 0) {
          std::string* to = &m.hometown_city;
          for (char c : l.substr(9)) {
            if (c == ',') to = &m.hometown_state;
            else if (std::isalnum(uint8_t(c)) && to->size() < 40) to->push_back(char(std::toupper(uint8_t(c))));
          }
        }
        if (l.rfind("announcer=", 0) == 0) {
          for (char c : l.substr(10))
            if (std::isalnum(uint8_t(c)) && m.announcer.size() < 24) m.announcer.push_back(char(std::toupper(uint8_t(c))));
        }
        if (l.rfind("height=", 0) == 0) {
          const double h = std::atof(l.c_str() + 7);
          if (h > 0) m.height = std::clamp(h, 0.80, 1.25);
        }
        if (l.rfind("pose=", 0) == 0) m.pose = uint32_t(std::clamp(std::atoi(l.c_str() + 5), 0, 65535));
        if (l.rfind("entrance=", 0) == 0) m.entrance = std::clamp(std::atoi(l.c_str() + 9), 0, 65535);
        if (l.rfind("victory=", 0) == 0) m.victory = std::clamp(std::atoi(l.c_str() + 8), 0, 65535);
        if (l.rfind("victory_song=", 0) == 0 && l.size() > 13 && fs::exists(f / fs::u8path(l.substr(13)), ec))
          m.victory_song = f / fs::u8path(l.substr(13));
        if (l.rfind("victory_gain=", 0) == 0) m.victory_gain = std::clamp(float(std::atof(l.c_str() + 13)), -20.0f, 6.0f);
        if (l.rfind("cae=", 0) == 0) {  // (a created entrance's block: CaeParts)
          std::ifstream in(f / fs::u8path(l.substr(4)), std::ios::binary);
          std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          if (b.size() == kCaeSize) m.cae = std::move(b), m.cae_hash = CaeHash(m.cae.data());
          else REXLOG_WARN("[svr2011] superstar mods: {}: cae={} is not a {}-byte entrance block ({} bytes): left out", m.folder,
                           l.substr(4), kCaeSize, b.size());
        }
        if (l.rfind("music=", 0) == 0) {  // (1-253: the game's own themes; 254 is the user playlist, 255 none)
          const int v = std::atoi(l.c_str() + 6);
          if (v >= 1 && v <= 253) m.music = v;
        }
        if (l.rfind("attire", 0) == 0 && l.size() > 17 && l[6] >= '1' && l[6] <= '4' && !l.compare(7, 10, "_entrance="))
          m.attire_entrance[l[6] - '1'] = std::clamp(std::atoi(l.c_str() + 17), 0, 65535);
        if (l.rfind("abilities=", 0) == 0) {
          for (const char* q = l.c_str() + 10; *q && m.abilities.size() < 8;) {
            if (const int a = std::atoi(q); a > 0 && a < 256) m.abilities.push_back(uint8_t(a));
            while (*q && *q != ',') ++q;
            if (*q == ',') ++q;
          }
        }
        if (l.rfind("moves=", 0) == 0 && l.size() > 6) moves = l.substr(6);
        if (l.rfind("version=", 0) == 0) m.version = l.substr(8);  // (per-mod saves: written in its file)
      }
      std::fclose(t);
    }
    if (m.base < 100 || m.base > 321) {
      REXLOG_WARN("[svr2011] superstar mods: {} has no base= character (100-321)", m.folder);
      continue;
    }
    if (!m.voice.empty() && !fs::exists(m.voice, ec)) m.voice.clear();
    if (!moves.empty())
      if (FILE* t = std::fopen((f / moves).string().c_str(), "rb")) {
        char line[256];
        while (std::fgets(line, sizeof line, t)) {
          char* eq = std::strchr(line, '=');
          if (line[0] == '#' || !eq) continue;
          const long off = std::strtol(line, nullptr, 0), id = std::strtol(eq + 1, nullptr, 0);
          if (off >= 0 && off < 0x1C0 && !(off & 1) && id > 0 && id < 65536)
            m.moves.push_back({uint16_t(off), uint16_t(id)});
        }
        std::fclose(t);
      }
    if (m.styled && m.call < 0) m.call = 77;  // (The Superstar: never the template's name)
    {
      std::vector<fs::path> signs;
      for (int k = 1; k <= 4; ++k)
        if (fs::exists(f / ("sign" + std::to_string(k) + ".dds"), ec))
          signs.push_back(f / ("sign" + std::to_string(k) + ".dds"));
      m.signs = uint32_t(signs.size());
      // (added below, once the mod has its id)
      m.sign_files = std::move(signs);
    }
    if (!temp) InstallMedia(m, f, song, movie, song_gain);  // (a fetched mod's theme / movie: not copied in)
    ReadApplied(m, f);
    m.slot = temp ? temp_slot() : slot_of(m.folder);
    // Test aid SVR2011_TEST_SLOT_SWAP=<folder>,<folder>: those two mods take each other's ids this run (slots.txt
    // unchanged) - another PC's numbering for online tests (svrtest lan --join-env)
    if (const char* sw = std::getenv("SVR2011_TEST_SLOT_SWAP"); sw && !temp && m.slot) {
      const std::string s = sw;
      const size_t comma = s.find(',');
      if (comma != std::string::npos) {
        const std::string a = s.substr(0, comma), other = m.folder == a ? s.substr(comma + 1) : a;
        if (m.folder == a || m.folder == s.substr(comma + 1)) {
          m.slot = slot_of(other);
          REXLOG_INFO("[svr2011] superstar mods: test - {} takes {}'s id {} (SVR2011_TEST_SLOT_SWAP)", m.folder, other, m.slot);
        }
      }
    }
    if (!m.slot) {
      REXLOG_WARN("[svr2011] superstar mods: only {} fit, {} left out (switch another mod off to make room)",
                  std::size(kPool), m.folder);
      if (!temp) left_out.push_back(m.folder);
      continue;
    }
    if (m.slot >= svr2011::kFirstHighId &&
        svr2011::BigRosterGiveIndex(g_memory->virtual_membase(), m.slot, m.base) >= svr2011::kMaxIndices) {
      REXLOG_WARN("[svr2011] superstar mods: big roster - no table entry left for {} (id {}) - left out", m.folder,
                  m.slot);
      if (!temp) left_out.push_back(m.folder);
      continue;
    }
    m.key = key_of(m.folder, temp);
    if (!m.key) {
      REXLOG_WARN("[svr2011] superstar mods: no key left for {} ({} at most) - left out", m.folder, kLastKey - kFirstKey + 1);
      if (!temp) left_out.push_back(m.folder);
      continue;
    }
    char nm[32];
    uint32_t drop = 0;
    for (int a = 0; a < 4; ++a)
      if (!attires[a].empty() && fs::exists(f / attires[a], ec)) drop |= 1u << a;
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.key);
    if (!Current(ov / nm, {f / "ch.pac", f / "manifest.txt"})) {
      svrfmt::Bytes ch;
      if (!svrfmt::ReadFile((f / "ch.pac").string(), ch)) {
        REXLOG_WARN("[svr2011] superstar mods: {}: ch.pac is not a character model pac (EPK8 with EMD models)", m.folder);
        continue;
      }
      std::string notes;
      if (!FitModelBudget(ch, notes)) {
        REXLOG_WARN("[svr2011] superstar mods: {}: model over the match's 768 KB per-wrestler budget ({} pages "
                    "at most):{} - left out, it would crash the match load (tools/model_budget.py)",
                    m.folder, svr2011::ModelPageLimit(), notes);
        left_out.push_back(m.folder);
        continue;
      }
      if (!notes.empty()) REXLOG_INFO("[svr2011] superstar mods: {}: vertex memory:{}", m.folder, notes);
      if (!RenameModel(ch, m.key, -1, 0, drop) || !svrfmt::WriteFile((ov / nm).string(), ch)) {
        REXLOG_WARN("[svr2011] superstar mods: {}: ch.pac is not a character model pac (EPK8 with EMD models)", m.folder);
        continue;
      }
      MarkCurrent(ov / nm, {f / "ch.pac", f / "manifest.txt"});
    }
    m.ch_guest = guest + nm;
    for (int a = 0; a < 4; ++a) {
      if (!(drop >> a & 1)) continue;
      char an[32];
      std::snprintf(an, sizeof an, "ch%03u_a%d.pac", m.key, a + 1);
      const fs::path src = f / attires[a];
      if (!Current(ov / an, {src, f / "manifest.txt"})) {
        svrfmt::Bytes d;
        std::string notes;
        if (svrfmt::ReadFile(src.string(), d) && !FitModelBudget(d, notes)) {
          REXLOG_WARN("[svr2011] superstar mods: {}: {} is over the match's 768 KB per-wrestler budget:{} - attire left out",
                      m.folder, attires[a], notes);
          continue;
        }
        if (d.empty() || !RenameModel(d, m.key, 0, a) || !svrfmt::WriteFile((ov / an).string(), d)) {
          REXLOG_WARN("[svr2011] superstar mods: {}: {} is not a character model pac", m.folder, attires[a]);
          continue;
        }
        MarkCurrent(ov / an, {src, f / "manifest.txt"});
      }
      m.attire_guests.push_back(guest + an);
    }
    {  // the attires its models give (contiguous from 1)
      uint32_t mask = 0;
      std::vector<fs::path> pacs = {ov / nm};  // (nm: chNNN.pac still)
      for (int a = 0; a < 4; ++a)
        if (drop >> a & 1) {
          char an[32];
          std::snprintf(an, sizeof an, "ch%03u_a%d.pac", m.key, a + 1);
          pacs.push_back(ov / an);
        }
      for (const auto& pp : pacs) mask |= AttireMask(pp, m.key);
      m.attires = 1;
      while (m.attires < 4 && (mask >> m.attires & 1)) ++m.attires;
    }
    std::snprintf(nm, sizeof nm, "ssf%03u.pac", m.key);
    // (the renders depend on base=: rebuilt with the manifest)
    const std::vector<fs::path> render_srcs = {f / "manifest.txt", f / "render.dds", f / "render_small.dds",
                                               f / "render_full.dds"};
    if (Current(ov / nm, render_srcs) ||
        (BuildRenders(ov / nm, m.base, m.key, f, m.styled) && (MarkCurrent(ov / nm, render_srcs), true)))
      m.ssf_guest = guest + nm;
    else
      REXLOG_WARN("[svr2011] superstar mods: {}: base {} has no select render (not a playable superstar)", m.folder,
                  m.base);
    if (m.signs) svr2011::AddCharacterSigns(svr2011::ModSignBase(m.key), m.sign_files);
    REXLOG_INFO("[svr2011] superstar mods: {} as id {} (from {}, key {}){}", m.name, m.slot, m.base, m.key,
                temp ? " - fetched from the session's host, this run only" : "");
    m.fingerprint = Fingerprint(m);
    if (!m.victory_song.empty()) svr2011::SetVictoryTrack(m.slot, m.victory_song, m.victory_gain);
    g_mods.push_back(std::move(m));
  }
  {  // the mods left out at this start, one folder a line (none: no file)
    const fs::path lo = dir / "left_out.txt";
    if (left_out.empty()) {
      fs::remove(lo, ec);
    } else if (FILE* t = std::fopen(lo.string().c_str(), "wb")) {
      for (const auto& f : left_out) std::fprintf(t, "%s\n", f.c_str());
      std::fclose(t);
    }
  }
  {  // (the overlay files of before the keys - named and stamped with a pool id: nothing mounts them now)
    for (const auto& e : fs::directory_iterator(overlay, ec)) {
      const std::string n = e.path().filename().string();
      unsigned id = 0;
      char rest[32] = {};
      if ((std::sscanf(n.c_str(), "ch%3u%31s", &id, rest) == 2 || std::sscanf(n.c_str(), "ssf%3u%31s", &id, rest) == 2) &&
          id < kFirstKey && std::find(std::begin(kPool), std::end(kPool), id) != std::end(kPool) &&
          (!std::strncmp(rest, ".pac", 4) || !std::strncmp(rest, "_a", 2)))
        fs::remove(e.path(), ec);
    }
  }
  if (keys_changed)
    if (FILE* t = std::fopen((dir / "keys.txt").string().c_str(), "wb")) {
      for (const auto& k : keys) std::fprintf(t, "%s\t%u\n", k.first.c_str(), k.second);
      std::fclose(t);
    }
  if (slots_changed) {
    if (FILE* t = std::fopen((dir / "slots.txt").string().c_str(), "wb")) {
      for (const auto& s : slots) std::fprintf(t, "%s\t%u\n", s.first.c_str(), s.second);
      std::fclose(t);
    }
  }
}

const Mod* ModOf(uint32_t id);

// ---- Attire mods (D6): another attire for a stock superstar.
// <game>/Mods/Attires/<folder>/manifest.txt: type=attire, superstar=<id>,
// attire=<1-4>|new, name=, model=<a ch pac: its first attire is used>,
// entrance= (optional). The model pac's first attire is copied to the overlay
// with the superstar's id and the attire's digit in its EMD names
// (ch<id>_x<N>.pac), mounted with the other overlay pacs; the game finds
// models by entry name. "new" takes the first attire the superstar's own pac
// doesn't have. Four attires at most; of two mods on one attire the first
// folder (A-Z) keeps it and the other takes a free attire (LoadAttireMods; the
// launchers move it for good in its manifest). The count and the {name, unlock} pair answer for it
// (sub_828C4140 / sub_828C4060 below).
struct AttireMod {
  std::string folder, name;
  uint32_t id = 0, attire = 0;  // attire: 0-3 (the digit)
  int entrance = -1;
  int victory = -1;  // (victory=: VictoryOfWinner)
};
std::vector<AttireMod> g_attire_mods;
std::unordered_map<uint32_t, uint32_t> g_attire_pairs;  // id -> guest 4 x {name string id, unlock}
std::unordered_map<uint32_t, uint32_t> g_attire_own;    // id -> attires its own pac has (bit mask)
std::set<uint32_t> g_attire_gear;                         // ids with entrance gear models (ExtraKinds): no new attires
constexpr uint32_t kAttireModNameIds = 0xB400;           // string ids for attire mods' names
std::unordered_map<uint32_t, uint32_t> g_attire_disc;   // id -> attires the game's pac\ch\chNNN.pac has

// The installed DLC packages (PrepareAttirePacks: <user data>\0000000000000000\<title>\00000002, unpacked by
// dlc.cpp, only read) and their character model pacs: <pkg>\pac\ch\ch<id>_dlcNN.pac - the DLC superstars' own
// (ids 51-58: Vol. 2 ch51-56, Vol. 3 ch57 / ch58) and extra attires for disc superstars (Vol. 2 ch145 / ch178,
// Vol. 3 ch131 / ch279). The game finds every attire by its entry name in whatever pac is mounted, so these
// count as the superstar's own attires too.
fs::path g_attire_dlc;
const std::vector<std::pair<uint32_t, fs::path>>& DlcCharacterPacs() {
  static std::vector<std::pair<uint32_t, fs::path>> pacs;
  static bool listed = false;
  if (listed || g_attire_dlc.empty()) return pacs;
  listed = true;
  std::error_code ec;
  for (const auto& pkg : fs::directory_iterator(g_attire_dlc, ec))
    for (const auto& e : fs::directory_iterator(pkg.path() / "pac" / "ch", ec)) {
      const std::string n = e.path().filename().string();
      unsigned id = 0, nn = 0;
      char tail[8] = {};
      if (std::sscanf(n.c_str(), "ch%u_dlc%u.%3s", &id, &nn, tail) == 3 && std::string(tail) == "pac" && id &&
          id < 1000)
        pacs.push_back({id, e.path()});
    }
  std::sort(pacs.begin(), pacs.end());
  return pacs;
}
// A DLC superstar: no pac of the game's own (ids below 100), a model pac in an installed DLC package.
bool DlcSuperstar(uint32_t id) {
  if (id >= 100) return false;
  for (const auto& p : DlcCharacterPacs())
    if (p.first == id) return true;
  return false;
}
// Which attires superstar `id` has (bit mask): the game's pac and the DLC's; gear: ExtraKinds of any of them.
uint32_t OwnAttires(uint32_t id, bool* gear = nullptr) {
  char stock[32];
  std::snprintf(stock, sizeof stock, "ch%03u.pac", id);
  const fs::path disc = g_game / "pac" / "ch" / stock;
  uint32_t mask = id >= 100 ? AttireMask(disc, id) : 0;
  bool g = id >= 100 && ExtraKinds(disc, id);
  g_attire_disc[id] = mask;
  for (const auto& p : DlcCharacterPacs())
    if (p.first == id) {
      mask |= AttireMask(p.second, id);
      g = g || ExtraKinds(p.second, id);
    }
  if (gear) *gear = g;
  return mask;
}

// Attire mods with no model (Simple entrance porting: "a superstar in the game"): only an entrance for one of the
// superstar's own attires - not an attire of their own (no name, no slot of the attire rule); the first folder
// (A-Z) for an attire keeps it. (id, attire digit) -> entrance.
std::map<std::pair<uint32_t, uint32_t>, int> g_attire_entrance_only;
std::map<std::pair<uint32_t, uint32_t>, int> g_attire_victory_only;  // (the same for victory=)

const AttireMod* AttireModOf(uint32_t id, uint32_t attire) {
  for (const auto& a : g_attire_mods)
    if (a.id == id && a.attire == attire) return &a;
  return nullptr;
}

void LoadAttireMods() {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Attires";
  const fs::path overlay = g_game / "Mods" / "SuperstarOverlay";
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec) && !fs::exists(e.path() / "disabled", ec) && fs::exists(e.path() / "manifest.txt", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  fs::create_directories(overlay, ec);
  struct Pending {
    AttireMod a;
    fs::path folder;
    std::string model, which;
    int asked = 0;  // attire=<1-4> it asked for (0: new)
  };
  std::vector<Pending> pending;
  for (const auto& f : folders) {
    Pending p;
    p.folder = f;
    p.model = "attire.pac";
    p.a.folder = f.filename().string();
    std::string type;
    if (FILE* t = std::fopen((f / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == '#') continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "type") type = v;
        else if (k == "superstar") p.a.id = uint32_t(std::atoi(v.c_str()));
        else if (k == "attire") p.which = v;
        else if (k == "name") p.a.name = v.substr(0, 31);
        else if (k == "model") p.model = v;
        else if (k == "entrance") p.a.entrance = std::clamp(std::atoi(v.c_str()), 0, 65535);
        else if (k == "victory") p.a.victory = std::clamp(std::atoi(v.c_str()), 0, 65535);
      }
      std::fclose(t);
    }
    if (type != "attire") continue;
    if (((p.a.id < 100 || p.a.id > 321) && !DlcSuperstar(p.a.id)) || ModOf(p.a.id)) {
      REXLOG_WARN("[svr2011] attire mods: {}: superstar={} is not a stock or installed DLC superstar (a superstar mod "
                  "takes attireN= in its own manifest)", p.a.folder, p.a.id);
      continue;
    }
    if (p.which != "new") {
      p.asked = std::atoi(p.which.c_str());
      if (p.asked < 1 || p.asked > 4) {
        REXLOG_WARN("[svr2011] attire mods: {}: attire={} (1-4 or new)", p.a.folder, p.which);
        continue;
      }
    }
    if (!fs::exists(f / p.model, ec)) {  // (no model: an entrance / victory for one of the superstar's own attires, or nothing)
      if (p.asked && p.a.victory > 0 &&
          g_attire_victory_only.emplace(std::make_pair(p.a.id, uint32_t(p.asked - 1)), p.a.victory).second)
        REXLOG_INFO("[svr2011] attire mods: {} -> superstar {} attire {}: victory {} (no model of its own)", p.a.folder,
                    p.a.id, p.asked, p.a.victory);
      if (p.asked && p.a.entrance > 0) {
        if (g_attire_entrance_only.emplace(std::make_pair(p.a.id, uint32_t(p.asked - 1)), p.a.entrance).second)
          REXLOG_INFO("[svr2011] attire mods: {} -> superstar {} attire {}: entrance {} (no model of its own)", p.a.folder,
                      p.a.id, p.asked, p.a.entrance);
        else
          REXLOG_WARN("[svr2011] attire mods: {}: superstar {} attire {} has an entrance from another mod already - left out",
                      p.a.folder, p.a.id, p.asked);
      } else if (!p.asked || p.a.victory <= 0) {
        REXLOG_WARN("[svr2011] attire mods: {}: no {} (and no entrance= for attire 1-4) - left out", p.a.folder, p.model);
      }
      continue;
    }
    if (!g_attire_own.count(p.a.id)) {
      bool gear = false;
      g_attire_own[p.a.id] = OwnAttires(p.a.id, &gear);
      if (gear) g_attire_gear.insert(p.a.id);
    }
    pending.push_back(std::move(p));
  }
  // Two mods on one attire (the launcher moves the newer one to a free attire; this is the same rule for
  // games started without it, and it's the same every start): the attire numbers asked for are given in
  // folder order (A-Z), the first keeping its attire; then the ones that lost theirs and the attire=new
  // ones take the superstar's free attires - none in its own pac and no mod's - in folder order, or are
  // left out with a log line.
  auto place = [&](Pending& p) {
    AttireMod& a = p.a;
    const uint32_t own = g_attire_own[a.id];
    // (replaces the game's or the DLC's: its model is in the pac overlay or a copy of the DLC's pac, PrepareAttirePacks)
    if (own >> a.attire & 1) {
      REXLOG_INFO("[svr2011] attire mods: {} -> superstar {} attire {} (replaces the {}) ({})", a.folder, a.id,
                  a.attire + 1, g_attire_disc[a.id] >> a.attire & 1 ? "game's" : "DLC's", a.name);
      g_attire_mods.push_back(std::move(a));
      return;
    }
    char an[40];
    std::snprintf(an, sizeof an, "ch%03u_x%u.pac", a.id, a.attire + 1);
    const fs::path src = p.folder / p.model;
    if (!Current(overlay / an, {src, p.folder / "manifest.txt"})) {
      svrfmt::Bytes d;
      std::string notes;
      if (svrfmt::ReadFile(src.string(), d) && !FitModelBudget(d, notes)) {
        REXLOG_WARN("[svr2011] attire mods: {}: {} is over the match's 768 KB per-wrestler budget:{} - left out", a.folder,
                    p.model, notes);
        return;
      }
      if (d.empty() || !RenameModel(d, a.id, 0, int(a.attire)) || !svrfmt::WriteFile((overlay / an).string(), d)) {
        REXLOG_WARN("[svr2011] attire mods: {}: {} is not a character model pac (EPK8 with EMD models)", a.folder,
                    p.model);
        return;
      }
      MarkCurrent(overlay / an, {src, p.folder / "manifest.txt"});
    }
    g_extra_mounts.push_back(std::string("smods:\\") + an);
    REXLOG_INFO("[svr2011] attire mods: {} -> superstar {} attire {} (new) ({})", a.folder, a.id, a.attire + 1, a.name);
    g_attire_mods.push_back(std::move(a));
  };
  std::vector<Pending*> later;
  for (auto& p : pending) {
    if (!p.asked) {
      later.push_back(&p);
      continue;
    }
    p.a.attire = uint32_t(p.asked - 1);
    if (g_attire_gear.count(p.a.id) && !(g_attire_own[p.a.id] >> p.a.attire & 1)) {
      REXLOG_WARN("[svr2011] attire mods: {}: superstar {} wears entrance gear - a new attire {} for them doesn't load "
                  "yet (only their own attires can be replaced) - left out", p.a.folder, p.a.id, p.asked);
      continue;
    }
    if (const AttireMod* o = AttireModOf(p.a.id, p.a.attire)) {
      REXLOG_WARN("[svr2011] attire mods: {}: superstar {} attire {} is taken by {} (first folder A-Z keeps it) - it "
                  "takes a free attire", p.a.folder, p.a.id, p.asked, o->folder);
      later.push_back(&p);
      continue;
    }
    place(p);
  }
  std::sort(later.begin(), later.end(), [](const Pending* x, const Pending* y) { return x->folder < y->folder; });
  for (Pending* p : later) {
    uint32_t taken = g_attire_gear.count(p->a.id) ? 0xF : g_attire_own[p->a.id];  // (gear: no new attires)
    for (const auto& o : g_attire_mods)
      if (o.id == p->a.id) taken |= 1u << o.attire;
    p->a.attire = 4;
    for (uint32_t k = 0; k < 4; ++k)
      if (!(taken >> k & 1)) {
        p->a.attire = k;
        break;
      }
    if (p->a.attire >= 4) {
      if (g_attire_gear.count(p->a.id))
        REXLOG_WARN("[svr2011] attire mods: {}: superstar {} wears entrance gear - new attires for them don't load yet "
                    "- left out (switch one off)", p->a.folder, p->a.id);
      else
        REXLOG_WARN("[svr2011] attire mods: {}: superstar {} has no free attire (4 at most) - left out (switch one off)",
                    p->a.folder, p->a.id);
      continue;
    }
    if (p->asked)
      REXLOG_WARN("[svr2011] attire mods: {}: superstar {} attire {} -> attire {} (that one was taken)", p->a.folder,
                  p->a.id, p->asked, p->a.attire + 1);
    place(*p);
  }
}

const Mod* ModOf(uint32_t id) {
  for (const auto& m : g_mods)
    if (m.slot == id) return &m;
  return nullptr;
}

// sub_826A1780(path, 0) for each overlay file; the path in a frame below the caller's.
// Each mount appends the pac's directory (header +4 bytes from 0x800) to the
// file system's directory buffer (vfs+56 start, vfs+60 end; vfs at
// 0x82ED5FEC), which has no room to spare for dozens more: it moves to a
// bigger buffer first. What was registered keeps pointing into the old one,
// which stays as it is.
constexpr uint32_t kVfs = 0x82ED5FEC, kDirStart = 56, kDirEnd = 60;
void GrowDirectory(uint8_t* base) {
  const uint32_t vfs = Rd32(base + kVfs);
  if (!vfs) return;
  const uint32_t start = Rd32(base + vfs + kDirStart), end = Rd32(base + vfs + kDirEnd);
  if (!start || end < start || end - start > (64u << 20)) return;
  uint32_t need = 0x10000;
  for (const auto& m : g_mods) need += uint32_t(2 + m.attire_guests.size()) * 0x4000;  // (a directory each, at most)
  need += uint32_t(g_extra_mounts.size()) * 0x4000;
  const uint32_t at = g_memory->SystemHeapAlloc(end - start + need, 0x40);
  if (!at) return;
  std::memcpy(base + at, base + start, end - start);
  Wr32(base + vfs + kDirStart, at);
  Wr32(base + vfs + kDirEnd, at + (end - start));
  REXLOG_INFO("[svr2011] superstar mods: file system directory {} KB, moved with {} KB more", (end - start) >> 10,
              need >> 10);
}

void Mount(PPCContext& ctx, uint8_t* base) {
  if (g_mounted || (g_mods.empty() && g_extra_mounts.empty())) return;
  g_mounted = true;
  GrowDirectory(base);
  const auto saved = ctx;
  const uint32_t str = saved.r1.u32 - 0x300;
  for (const auto& f : g_extra_mounts) {
    std::memcpy(base + str, f.c_str(), f.size() + 1);
    ctx = saved;
    ctx.r1.u64 = saved.r1.u32 - 0x400;
    ctx.r3.u64 = str;
    ctx.r4.u64 = 0;
    sub_826A1780(ctx, base);
    REXLOG_INFO("[svr2011] superstar mods: mounted {} ({})", f, ctx.r3.u32);
  }
  for (const auto& m : g_mods) {
    std::vector<const std::string*> files = {&m.ch_guest, &m.ssf_guest};
    for (const auto& a : m.attire_guests) files.push_back(&a);
    for (const std::string* p : files) {
      if (p->empty()) continue;
      std::memcpy(base + str, p->c_str(), p->size() + 1);
      ctx = saved;
      ctx.r1.u64 = saved.r1.u32 - 0x400;
      ctx.r3.u64 = str;
      ctx.r4.u64 = 0;
      sub_826A1780(ctx, base);
      REXLOG_INFO("[svr2011] superstar mods: mounted {} ({})", *p, ctx.r3.u32);
    }
  }
  ctx = saved;
}

void PutName(uint8_t* rec, uint32_t off, const std::string& s) {
  std::memset(rec + off, 0, kNameLen);
  std::memcpy(rec + off, s.data(), std::min<size_t>(s.size(), kNameLen - 1));
}

// The mods' records and profiles. A slot is set up from the base only when it
// isn't this mod's yet - a save's copy (with the player's edits) stays:
// - the record when its name isn't the mod's (a new mod, or a save from
//   before it): the base's, with the mod's names, own id, selectable and DLC;
// - the profile (moves, entrance, ...) while it is still the blank one
//   CHAR/PRO loaded for the slot (g_blank).
// Pool ids no mod has (a mod removed since the save) are not selectable.
// The game reloads CHAR/DAT and CHAR/PRO after a save has loaded (the DLC
// scan, sub_825A09B0), which puts the slots' placeholders back: what a slot
// held just before (the save's copy, the player's edits) is kept (Remember)
// and goes back instead of the base's.
std::vector<std::pair<uint32_t, svrfmt::Bytes>> g_blank;  // slot -> CHAR/PRO's profile
// (g_blank and g_kept grow in the loaders' hooks while CheckModProfiles reads
// them on the game thread)
std::mutex g_slots_mutex;
struct Kept {
  uint32_t slot = 0;
  svrfmt::Bytes record, profile;
};
std::vector<Kept> g_kept;
void RecordStubs(uint8_t* base);     // (per-mod saves, below)
void PutBackEntries(uint8_t* base);

Kept& KeptFor(uint32_t slot) {
  for (auto& k : g_kept)
    if (k.slot == slot) return k;
  g_kept.push_back({slot, {}, {}});
  return g_kept.back();
}

// A slot's placeholder profile can come in several versions: DLC Vol03
// ships its own chEtc.pac whose CHAR/PRO placeholders for ids 59-69 differ
// from the disc's, and the game reloads CHAR/PRO from it at each DLC scan.
// Every version the loader writes into a mod's slot is kept (RecordBlanks).
bool IsBlank(uint32_t slot, const uint8_t* profile) {
  bool any = false;
  for (const auto& b : g_blank)
    if (b.first == slot) {
      any = true;
      if (!std::memcmp(profile, b.second.data(), kProfileSize)) return true;
    }
  return !any;  // (CHAR/PRO not loaded yet: nothing of the mod's there)
}

// Before CHAR/DAT or CHAR/PRO (re)loads: the mods' slots as they are.
void Remember(uint8_t* base) {
  std::lock_guard lock(g_slots_mutex);
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512) continue;
    const uint8_t* sr = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
    if (std::strncmp(reinterpret_cast<const char*>(sr + kFullName), m.name.c_str(), kNameLen - 1)) continue;
    Kept& k = KeptFor(m.slot);
    k.record.assign(sr, sr + kRecordSize);
    const uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    if (!IsBlank(m.slot, sp)) k.profile.assign(sp, sp + kProfileSize);
  }
}

// The record's crowd signs: the mod's own (crowd_signs.cpp adds them at
// ModSignBase + 1..4, repeated to fill 4), else its base's.
void SetSigns(uint8_t* sr, const uint8_t* br, const Mod& m) {
  for (uint32_t k = 0; k < 4; ++k) {
    if (m.signs) {
      const uint32_t v = svr2011::ModSignBase(m.key) + 1 + k % m.signs;
      sr[kSignIds + k * 2] = uint8_t(v >> 8), sr[kSignIds + k * 2 + 1] = uint8_t(v);
    } else {
      sr[kSignIds + k * 2] = br[kSignIds + k * 2], sr[kSignIds + k * 2 + 1] = br[kSignIds + k * 2 + 1];
    }
  }
}

// A mod's record never has its base's select-grid tile (+226): the grid's
// tile -> record lookup takes the last record with the tile's number, the
// mod's copy (DLC, in the EXTRA list instead), and the base's tile picked
// nobody - Stone Cold, Matt Hardy, Mr. McMahon and William Regal couldn't be
// chosen (2.0.3 / 2.0.4). 0 = no tile, as the placeholders have.
// Its height (manifest height=): record +28 is the superstar's scale, 4.12
// fixed point (4096 = 1.0) - the character's skeleton is built with its bone
// lengths times it, and grapples / IK follow. Always from the base's value
// (never compounding over a kept or saved record); clamped to what the
// game's moves still fit.
// Universe's brand (record +16 + mode; Universe +17: 0 RAW, 1 SmackDown, 4 the legends, 6 the unused ids). A mod
// built on an unused id (base 115 ...) carried that 6, and the rankings of every brand's title skip anyone not on the
// title's brand (sub_825AF0F0 -> sub_828B8038(id, 1)): only the Unified Tag ranking, which takes teams, showed mods
// (2.2.0 report e7e305). A 6 becomes RAW or SmackDown, by the slot (both brands get mods); a brand the player set stays.
// But FREE AGENT in Universe is that same 6 (2.2.1 report fe6cee: AJ Styles set to free agent went back to a brand
// at the next match / Universe start - every record write made it RAW or SmackDown again). So only a record whose
// other brand byte (+16) is 6 too - the base's "no brand", carried - is given one, once: +16 takes the same brand,
// which marks the record done. A 6 at +17 with a brand at +16 is the player's free agent and stays.
constexpr uint32_t kBrand = 16, kUniverseBrand = 17;
void UniverseBrand(uint8_t* sr, const Mod& m) {
  if (sr[kBrand] != 6) return;  // (done, or a branded base: +17 is the player's - a 6 there is FREE AGENT)
  if (sr[kUniverseBrand] == 6) {
    sr[kUniverseBrand] = uint8_t(m.slot & 1);
    static std::set<uint32_t> told;
    if (told.insert(m.slot).second)
      REXLOG_INFO("[svr2011] superstar mods: id {} in Universe on {} (its base's record had no brand)", m.slot,
                  m.slot & 1 ? "SmackDown" : "RAW");
  }
  sr[kBrand] = sr[kUniverseBrand];  // (a 2.2.1 save's mod already moved off 6: marked too, as it is)
}

void SetHeight(uint8_t* sr, const uint8_t* br, const Mod& m) {
  if (m.height <= 0) return;
  const long v = std::clamp(std::lround(Rd16(br + kScale) * m.height), 3300L, 4900L);
  if (Rd16(sr + kScale) != uint32_t(v))
    REXLOG_INFO("[svr2011] superstar mods: id {} height x{:.2f} ({} -> {})", m.slot, m.height, Rd16(br + kScale), v);
  sr[kScale] = uint8_t(v >> 8), sr[kScale + 1] = uint8_t(v);
}

void OwnSelectTile(uint8_t* sr, const uint8_t* br) {
  if (Rd16(sr + kSelectTile) && Rd16(sr + kSelectTile) == Rd16(br + kSelectTile)) sr[kSelectTile] = sr[kSelectTile + 1] = 0;
}

// A slot that changed hands at a save load (ApplyRecords: another mod's record there) gets this mod's profile -
// but the save's own profile for the slot (the old mod's moves, entrance and user movie) is written after that:
// sub_82594C78, whose hook does the handover, loads the records; the profiles come a moment later, after the hooks.
// With the record this mod's by then, every later pass kept the old mod's profile as a player's edit (a mod swapped
// into a slot played the old one's entrance and movie; 25 Oct). So the profile given is watched for a short while
// (RedoHandovers, once a frame and before a match's people are made): the first change in that time is the save's
// write, and the handover is made again; a change after it (a player's CREATE AN ENTRANCE edit) stays.
struct Handover {
  svrfmt::Bytes given;  // the profile the slot was given
  int frames = 0;       // frames left to watch it
};
std::map<uint32_t, Handover> g_handed;
constexpr int kHandoverFrames = 600;  // (10 s; the save's write comes within milliseconds)
void RedoHandovers(uint8_t* base, bool frame);

// A fetched mod's id (temp, this run only): its record + profile as the save had them before the mod was put
// there - written back into the save it builds (OnlineTempForSave), so the save never keeps the host's mod and
// a player's mod given that id later never meets its data. Taken at the first apply after a save load.
std::map<uint32_t, std::vector<uint8_t>> g_temp_saved, g_temp_live;  // id -> record + profile

// The fetched mods' ids as the save FILE has them (<saves>\SaveData.dat, the last one loaded or written - never one
// with a fetched mod in it: 0x18-byte header; records at file 0x1C + index * 260, profiles at 0x18 + 0x3C160 +
// index * 1056 - checked against the live tables). Not the live tables nor the in-memory image (*0x82E407B4): the
// game rebuilds that from the tables, and the start's own apply put the mod in them before (s4save: the base's
// profile in the save, B vs A). Read before each save is built and at a save load; no file (a new player): the
// first ApplyRecords' snapshot stays. Proof: a save built with a fetched mod loaded = byte-identical to one without.
fs::path g_save_file;

void TempFromSave(uint8_t* base) {
  constexpr size_t kHeader = 0x18, kSaveProfiles = 0x3C160;
  if (g_save_file.empty() || std::none_of(g_mods.begin(), g_mods.end(), [](const Mod& m) { return m.temp; })) return;
  svrfmt::Bytes file;
  if (!svrfmt::ReadFile(g_save_file.string(), file) || file.size() < kHeader + kSaveProfiles + 512 * kProfileSize) return;
  for (const auto& m : g_mods) {
    if (!m.temp) continue;
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512) continue;
    const uint8_t* r = file.data() + kHeader + 4 + si * kRecordSize;  // (records: 4 bytes after the header)
    const uint8_t* p = file.data() + kHeader + kSaveProfiles + si * kProfileSize;
    std::vector<uint8_t> b(r, r + kRecordSize);
    b.insert(b.end(), p, p + kProfileSize);
    g_temp_saved[m.slot] = std::move(b);
  }
}

// ---- The save cleaner's reference (the launcher's Saves tab, "Clean save of unused mods"; save_cleaner.c): each
// mod id's placeholder record and profile as CHAR/DAT and CHAR/PRO load them - before this run loads a save, ids
// no mod holds now - in <saves>/.universe/pool_placeholders.bin. A save whose record or profile at a mod id is none
// of them holds a mod's data (one removed or turned off: its name, moves, entrance, Threads, edits); the launcher
// writes the placeholder back. (DLC ids 59-69 have a version per DLC chEtc: each one seen is kept.)
// With them each id's 40-byte entry (block B +0x3E648 + index * 40: kEntries40, the per-mod saves' 'E40 ').
// The file: "SVRPOOL2", u32 record size, u32 profile size, u32 entry size, u32 the records', the profiles' and the
// entries' offsets in SaveData.dat, u32 count, then per version: u16 id, u16 index (little-endian), the record (its
// selectable byte 0), the profile, the entry.
bool g_save_loaded = false;  // (a save was loaded this run: the slots hold its data now, not the placeholders)
std::map<uint32_t, std::vector<svrfmt::Bytes>> g_pool_blank;  // id -> versions (record + profile)

void KeepPoolPlaceholders(uint8_t* base) {
  constexpr uint32_t kEntry = 0x82E7C918 + 0x3E648, kEntrySize = 40;  // (kEntries40, below)
  constexpr uint32_t kFileProfiles = 0x18 + 0x3C160;                  // (CheckSavedProfiles: kProfiles in the file)
  if (g_save_loaded || g_save_file.empty()) return;
  bool added = false;
  for (uint32_t id : kPool) {
    if (ModOf(id)) continue;
    const uint32_t si = svr2011::RosterIndex(base, id);
    if (si >= 512) continue;
    const uint8_t* r = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
    if (base[0x82DB3AE0 + id] == 1 && !r[kFullName]) continue;  // (CHAR/DAT not loaded yet; an absent id: as it is)
    svrfmt::Bytes b(r, r + kRecordSize);
    b[kSelectable] = 0;
    b.insert(b.end(), base + svr2011::RosterEntry(kProfiles, kProfileSize, si), base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kProfileSize);
    b.insert(b.end(), base + svr2011::RosterEntry(kEntry, kEntrySize, si), base + svr2011::RosterEntry(kEntry, kEntrySize, si) + kEntrySize);
    b.insert(b.begin(), {uint8_t(id), uint8_t(id >> 8), uint8_t(si), uint8_t(si >> 8)});
    auto& v = g_pool_blank[id];
    if (std::find(v.begin(), v.end(), b) != v.end()) continue;
    v.push_back(std::move(b));
    added = true;
  }
  if (!added) return;
  auto u32 = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
  std::string out = "SVRPOOL2";
  u32(out, kRecordSize), u32(out, kProfileSize), u32(out, kEntrySize);
  u32(out, 0x1C), u32(out, kFileProfiles), u32(out, kEntry - kProfiles + kFileProfiles);
  uint32_t n = 0;
  for (const auto& [id, v] : g_pool_blank) n += uint32_t(v.size());
  u32(out, n);
  for (const auto& [id, v] : g_pool_blank)
    for (const auto& b : v) out.append(reinterpret_cast<const char*>(b.data()), b.size());
  std::error_code ec;
  const fs::path f = g_save_file.parent_path() / ".universe" / "pool_placeholders.bin";
  fs::create_directories(f.parent_path(), ec);
  const fs::path tmp = f.string() + ".tmp";
  {
    std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
    o.write(out.data(), std::streamsize(out.size()));
    if (!o) return;
  }
  fs::rename(tmp, f, ec);
  if (!ec) REXLOG_INFO("[svr2011] superstar mods: {} mod ids' placeholders ({} versions) for the save cleaner: {}",
                       g_pool_blank.size(), n, f.string());
}

// Which mod each slot's profile in the save was made for (13, 2.1.6 hold): <saves>/.universe/mod_profiles.txt,
// "<id><TAB><folder><TAB><fingerprint>" a line, written when the game writes its save. A slot whose mod is another
// one now (its id given to a new mod - slots.txt) or the same one changed (an update: other moves, entrance, movie)
// has a stale profile in the save - kept, it played the old moves and entrance under the new mod (a same name kept
// it as "the player's edits"). At a save load such a slot is handed over (ApplyRecords: the mod's own profile;
// RedoHandovers: again after the save's late write). A save from before the file: a slot whose saved profile has
// fewer than half of the mod's own moves (or, with none, not its entrance) is taken as another mod's.
std::set<uint32_t> g_stale;  // slots to hand over at this load's ApplyRecords
fs::file_time_type g_marks_seen;
fs::path MarksFile() { return g_save_file.empty() ? fs::path() : g_save_file.parent_path() / ".universe" / "mod_profiles.txt"; }
fs::file_time_type SaveWriteTime() {
  std::error_code ec;
  const auto t = fs::last_write_time(g_save_file, ec);
  return ec ? fs::file_time_type{} : t;
}

void WriteMarks() {
  const fs::path f = MarksFile();
  if (f.empty()) return;
  std::error_code ec;
  fs::create_directories(f.parent_path(), ec);
  if (FILE* t = std::fopen(f.string().c_str(), "wb")) {
    for (const auto& m : g_mods)
      if (!m.temp) std::fprintf(t, "%u\t%s\t%08X\n", m.slot, m.folder.c_str(), m.fingerprint);
    std::fclose(t);
  }
  REXLOG_INFO("[svr2011] superstar mods: the save written - which mod each slot's profile is: {}", f.string());
}

void CheckSavedProfiles(uint8_t* base) {
  constexpr size_t kHeader = 0x18, kSaveProfiles = 0x3C160;
  g_stale.clear();
  g_marks_seen = SaveWriteTime();  // (the save just loaded: not a write)
  svrfmt::Bytes file;
  if (g_save_file.empty() || !svrfmt::ReadFile(g_save_file.string(), file) ||
      file.size() < kHeader + kSaveProfiles + 512 * kProfileSize)
    return;
  struct Mark {
    std::string folder;
    uint32_t fp = 0;
  };
  std::map<uint32_t, Mark> marks;
  if (FILE* t = std::fopen(MarksFile().string().c_str(), "rb")) {
    char line[600];
    while (std::fgets(line, sizeof line, t)) {
      char folder[512] = {};
      unsigned id = 0, fp = 0;
      if (std::sscanf(line, "%u\t%511[^\t]\t%x", &id, folder, &fp) == 3) marks[id] = {folder, fp};
    }
    std::fclose(t);
  }
  for (const auto& m : g_mods) {
    if (m.temp) continue;
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512) continue;
    const uint8_t* p = file.data() + kHeader + kSaveProfiles + si * kProfileSize;
    if (std::getenv("SVR2011_TEST_SLOT_LOG"))
      REXLOG_INFO("[svr2011] superstar mods: test - id {} is save index {} (id 100's: {}), fingerprint {:08X}", m.slot,
                  si, svr2011::RosterIndex(base, 100), m.fingerprint);
    if (IsBlank(m.slot, p)) continue;  // (the placeholder saved: the mod's own profile goes in anyway)
    std::string why;
    if (const auto k = marks.find(m.slot); k != marks.end()) {
      if (k->second.folder != m.folder) why = "made for " + k->second.folder;
      else if (k->second.fp != m.fingerprint) why = "made for an older " + m.folder;
    } else {
      // (a save from before the file, or a slot new to it: does the profile look like this mod's?)
      size_t own = 0, same = 0;
      for (const auto& [off, id] : m.moves)
        if (off + 1u < kEntrance && svr2011::MoveUsable(id)) ++own, same += (p[off] << 8 | p[off + 1]) == id;
      const int ent = p[kEntrance + 0x14] << 8 | p[kEntrance + 0x15];
      if (own ? same * 2 < own : m.entrance >= 0 && ent != m.entrance)
        why = own ? fmt::format("{} of its {} own moves", same, own) : fmt::format("entrance {} not {}", ent, m.entrance);
    }
    if (why.empty()) continue;
    g_stale.insert(m.slot);
    REXLOG_INFO("[svr2011] superstar mods: id {} ({}) - the save's profile isn't this mod's ({}): its own moves and "
                "entrance instead", m.slot, m.folder, why);
  }
}

// Once a frame: the game wrote its save - the marks follow (the slots hold these mods' profiles now).
void MarksTick(uint8_t* base) {
  static uint32_t frames = 0;
  if (g_save_file.empty() || ++frames % 60) return;
  // Test aid: SVR2011_TEST_SLOT_LOG=1 - each mod slot's entrance number and first own move, when they change
  // (the profile a slot ends up with after a load and the save's late write)
  if (static const bool on = std::getenv("SVR2011_TEST_SLOT_LOG") != nullptr; on) {
    static std::map<uint32_t, uint32_t> last;
    for (const auto& m : g_mods) {
      const uint32_t si = svr2011::RosterIndex(base, m.slot);
      if (si >= 512) continue;
      const uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
      const uint32_t ent = sp[kEntrance + 0x14] << 8 | sp[kEntrance + 0x15];
      const uint32_t mv = m.moves.empty() ? 0 : uint32_t(sp[m.moves[0].first] << 8 | sp[m.moves[0].first + 1]);
      if (last.count(m.slot) && last[m.slot] == (ent << 16 | mv)) continue;
      last[m.slot] = ent << 16 | mv;
      REXLOG_INFO("[svr2011] superstar mods: test - id {} ({}) entrance {} first own move {} (the mod's: {} / {})",
                  m.slot, m.folder, ent, mv, m.entrance, m.moves.empty() ? 0 : m.moves[0].second);
    }
  }
  const auto t = SaveWriteTime();
  if (t == fs::file_time_type{} || t == g_marks_seen) return;
  const bool first = g_marks_seen == fs::file_time_type{};
  g_marks_seen = t;
  if (first) return;  // (the save there at the start: not one written now)
  WriteMarks();
}

// A mod's record selectable: a fetched one (temp) only while the game is online (its lobby and matches) - the
// offline game (WWE Universe's own generation at a save load, the other modes) never sees it in the roster, so
// nothing it makes for the save differs from a start without it.
uint8_t Selectable(const Mod& m) { return m.temp && !svr2011::P2POnlineMode() ? 0 : 1; }

// A slot's placeholder record name (CHAR/DAT's): "0" on a pool id, "DLC <n>" on a free DLC id (59-69).
bool PlaceholderName(uint32_t slot, const std::string& name) {
  if (name.empty() || name == "0") return true;
  if (slot < 59 || slot > 69 || name.size() < 5 || name.compare(0, 4, "DLC ") != 0) return false;
  return std::all_of(name.begin() + 4, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

void ApplyRecords(uint8_t* base) {
  // Test aid: SVR2011_TEST_ID_TABLE=1 - once, the ids whose present flag
  // (0x82DB3AE0 + id, sub_828B7078) is off, with their record index and name.
  if (static bool told = false; !told && std::getenv("SVR2011_TEST_ID_TABLE")) {
    told = true;
    std::string absent;
    int n = 0;
    for (uint32_t id = 0; id < 322; ++id) {
      if (base[0x82DB3AE0 + id] == 1) continue;
      const uint32_t si = svr2011::RosterIndex(base, id);
      absent += fmt::format(" {}:{}:{}", id, si, si < 512 ? base[svr2011::RosterEntry(kRecords, kRecordSize, si) + kFullName] : 0);
      ++n;
    }
    REXLOG_INFO("[svr2011] superstar mods: test - {} ids absent (id:index:name byte){}", n, absent);
  }
  for (uint32_t id : kPool) {
    if (ModOf(id)) continue;
    const uint32_t si = svr2011::RosterIndex(base, id);
    if (si < 512) base[svr2011::RosterEntry(kRecords, kRecordSize, si) + kSelectable] = 0;
  }
  for (const auto& m : g_mods) {
    const uint32_t bi = svr2011::RosterIndex(base, m.base), si = svr2011::RosterIndex(base, m.slot);
    if (bi >= 512 || si >= 512 || bi == si) continue;
    uint8_t* br = base + svr2011::RosterEntry(kRecords, kRecordSize, bi);
    uint8_t* sr = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
    if (!br[kFullName]) continue;  // (not loaded yet)
    if (m.temp && !g_temp_saved.count(m.slot)) {  // (the id as it was, an unused pool slot: not selectable)
      std::vector<uint8_t> b(sr, sr + kRecordSize);
      b[kSelectable] = 0;
      const uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
      b.insert(b.end(), sp, sp + kProfileSize);
      g_temp_saved[m.slot] = std::move(b);
    }
    uint8_t* bp = base + svr2011::RosterEntry(kProfiles, kProfileSize, bi);
    uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    bool have_blank = false;
    for (const auto& b : g_blank) have_blank |= b.first == m.slot;
    if (have_blank && IsBlank(m.slot, sp)) {
      const Kept* k = nullptr;
      for (const auto& x : g_kept)
        if (x.slot == m.slot && !x.profile.empty()) k = &x;
      if (k) {
        std::memcpy(sp, k->profile.data(), kProfileSize);
        svr2011::ThreadsSlots(sp, m.folder);  // (Threads slots: kept by folder)
      } else {
        std::memcpy(sp, bp, kProfileSize);
        svr2011::ThreadsSlots(sp, m.folder);  // (not the base's Threads slots)
        SetOwnMoves(sp, m);
        REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {}{}", m.base, m.slot,
                    m.moves.empty() && m.entrance < 0 ? "" : " (its own moves / entrance)");
      }
    }
    if (have_blank) SetEntranceMedia(sp, m, bp);
    SetSigns(sr, br, m);
    // (the save's profile for the slot made for another mod or an older version of this one: CheckSavedProfiles)
    const bool stale = g_stale.erase(m.slot) > 0;
    if (stale) {
      std::memcpy(sp, bp, kProfileSize);
      svr2011::ThreadsSlots(sp, m.folder);
      SetOwnMoves(sp, m);
      SetEntranceMedia(sp, m, bp);
      g_handed[m.slot] = {svrfmt::Bytes(sp, sp + kProfileSize), kHandoverFrames};  // (RedoHandovers)
      REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {} (its saved profile was another mod's)", m.base, m.slot);
    }
    if (!std::strncmp(reinterpret_cast<char*>(sr + kFullName), m.name.c_str(), kNameLen - 1)) {
      sr[kSelectable] = Selectable(m), sr[kDlc] = 1;
      OwnSelectTile(sr, br);  // (a save from before 2.0.5 has the base's)
      SetHeight(sr, br, m);
      static std::vector<std::pair<uint32_t, uint32_t>> told;  // (slot, its ratings when last logged)
      const uint32_t r = uint32_t(sr[0]) << 16 | sr[1] << 8 | sr[2];
      auto t = std::find_if(told.begin(), told.end(), [&](const auto& x) { return x.first == m.slot; });
      if (t == told.end() || t->second != r) {
        if (t == told.end()) told.push_back({m.slot, r});
        else t->second = r;
        REXLOG_INFO("[svr2011] superstar mods: id {} kept as loaded (ratings {} {} {} ...)", m.slot, sr[0], sr[1],
                    sr[2]);
      }
      UniverseBrand(sr, m);
      continue;  // (already: as set up, or from a save)
    }
    const std::string was(reinterpret_cast<char*>(sr + kFullName), 0,
                          strnlen(reinterpret_cast<char*>(sr + kFullName), kNameLen));
    // The slot changes hands (another mod's record there: its id given to this
    // one, slots.txt) - its profile, the moves and entrance a save kept for
    // the old mod, goes too: this mod's, as for a new slot. (A placeholder's
    // record, "0", comes with its blank profile: nothing to take back.)
    // (A placeholder record - "0", or "DLC <n>" on the DLC ids 59-69, which the game puts back at each CHAR/DAT
    // reload after a save load - is no other mod's: the profile there is the save's, the player's edits. 2.1.8
    // report a82070: mods on 59-65 lost their edited move-sets at every start.)
    if (!stale && !PlaceholderName(m.slot, was) && !IsBlank(m.slot, sp)) {
      std::memcpy(sp, bp, kProfileSize);
      svr2011::ThreadsSlots(sp, m.folder);
      SetOwnMoves(sp, m);
      SetEntranceMedia(sp, m, bp);
      g_handed[m.slot] = {svrfmt::Bytes(sp, sp + kProfileSize), kHandoverFrames};  // (RedoHandovers)
      REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {} (the slot held \"{}\": its moves and entrance not "
                  "kept for {})", m.base, m.slot, was, m.name);
    }
    {
      // As it was before the game reloaded its records. A save loaded into
      // the slot brings its record AND profile: a removed mod's, when a new
      // mod has its id now (2.1.5 report: the new mod played with the deleted
      // one's moves and entrance - this record went back, the profile stayed).
      // The profile went back above; the record goes back here.
      const Kept* k = nullptr;
      for (const auto& x : g_kept)
        if (x.slot == m.slot && !x.record.empty()) k = &x;
      if (k) {
        std::memcpy(sr, k->record.data(), kRecordSize);
        for (uint32_t off : {kOwnId, kOwnId2, kSamePerson}) sr[off] = uint8_t(m.slot >> 8), sr[off + 1] = uint8_t(m.slot);
        OwnSelectTile(sr, br);
        SetHeight(sr, br, m);
        UniverseBrand(sr, m);
        continue;
      }
    }
    std::memcpy(sr, br, kRecordSize);
    OwnSelectTile(sr, br);
    PutName(sr, kFullName, m.name);
    PutName(sr, kSecondName, m.name);
    PutName(sr, kShortName, m.short_name);
    sr[kSelectable] = Selectable(m);
    sr[kDlc] = 1;
    for (uint32_t off : {kOwnId, kOwnId2, kSamePerson}) sr[off] = uint8_t(m.slot >> 8), sr[off + 1] = uint8_t(m.slot);
    SetSigns(sr, br, m);
    for (int k = 0; k < 7; ++k)  // (its attributes: +0..+6 and the copy at +8)
      if (m.ratings[k] > 0) sr[k] = sr[8 + k] = uint8_t(m.ratings[k]);
    if (!m.abilities.empty())
      for (uint32_t k = 0; k < 8; ++k) sr[kAbilities + k] = k < m.abilities.size() ? m.abilities[k] : 0;
    SetHeight(sr, br, m);
    UniverseBrand(sr, m);

    // test aid: SVR2011_TEST_STAR_EDIT=<id> - that mod's ratings set to 20 (as
    // an edit made in the game would), to see them kept through a save
    if (const char* e = std::getenv("SVR2011_TEST_STAR_EDIT"); e && uint32_t(std::atoi(e)) == m.slot)
      for (uint32_t i = 0; i < 15; ++i) sr[i] = i == 7 ? sr[i] : 20;
    REXLOG_INFO("[svr2011] superstar mods: record {} -> id {} ({}; was \"{}\")", m.base, m.slot, m.name, was);
  }
  RedoHandovers(base, false);
  PutBackEntries(base);
}

// `frame`: called once a frame (the watch counts down); else from a load or before a match.
void RedoHandovers(uint8_t* base, bool frame) {
  for (const auto& m : g_mods) {
    const auto h = g_handed.find(m.slot);
    if (h == g_handed.end()) continue;
    const uint32_t bi = svr2011::RosterIndex(base, m.base), si = svr2011::RosterIndex(base, m.slot);
    if (bi >= 512 || si >= 512 || bi == si) continue;
    uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    const uint8_t* bp = base + svr2011::RosterEntry(kProfiles, kProfileSize, bi);
    if (std::memcmp(sp, h->second.given.data(), kProfileSize)) {
      // (the save's profile for the slot, written after the handover: this mod's again - once)
      const int movie = sp[kEntrance + kMovie] << 8 | sp[kEntrance + kMovie + 1];
      const int entrance = sp[kEntrance + 0x14] << 8 | sp[kEntrance + 0x15];
      std::memcpy(sp, bp, kProfileSize);
      svr2011::ThreadsSlots(sp, m.folder);
      SetOwnMoves(sp, m);
      SetEntranceMedia(sp, m, bp);
      REXLOG_INFO("[svr2011] superstar mods: id {} - the save wrote the old mod's profile (entrance {}, movie {}) "
                  "after the handover: {}'s again (entrance {}, movie {})", m.slot, entrance, movie, m.name,
                  sp[kEntrance + 0x14] << 8 | sp[kEntrance + 0x15], sp[kEntrance + kMovie] << 8 | sp[kEntrance + kMovie + 1]);
      g_handed.erase(h);
      continue;
    }
    if (frame && --h->second.frames <= 0) g_handed.erase(h);
  }
}

// CHAR/PRO loaded: each slot's blank profile (before ApplyRecords).
// After a CHAR/PRO load: what the loader wrote into a mod's slot (changed
// from `before`) is a placeholder version of that slot - kept, unless known.
void RecordBlanks(uint8_t* base, const std::vector<svrfmt::Bytes>& before) {
  std::lock_guard lock(g_slots_mutex);
  for (size_t i = 0; i < g_mods.size() && i < before.size(); ++i) {
    const auto& m = g_mods[i];
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512 || before[i].empty()) continue;
    const uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    if (!std::memcmp(sp, before[i].data(), kProfileSize)) continue;  // (not rewritten)
    int n = 0;
    bool known = false;
    for (const auto& b : g_blank)
      if (b.first == m.slot) ++n, known |= !std::memcmp(sp, b.second.data(), kProfileSize);
    if (known) continue;
    g_blank.push_back({m.slot, svrfmt::Bytes(sp, sp + kProfileSize)});
    REXLOG_INFO("[svr2011] superstar mods: id {} placeholder profile #{} recorded", m.slot, n + 1);
  }
}

void KeepBlankProfiles(uint8_t* base) {
  std::lock_guard lock(g_slots_mutex);
  for (const auto& m : g_mods) {
    bool have = false;
    for (const auto& b : g_blank) have |= b.first == m.slot;
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (have || si >= 512) continue;
    const uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    g_blank.push_back({m.slot, svrfmt::Bytes(sp, sp + kProfileSize)});
  }
}

// ---- The "MODDED" badge
// The select screen's panel shows a DLC-flagged character with the
// "DOWNLOADABLE CONTENT" badge (layout node cursor+312, sub_82466830), the
// texture DLCtex01 (menuHD.pac MENx/CHSI, 7001; 256 x 64 DXT5, one per
// language). Mods are DLC-flagged, so the same badge shows; while a panel
// hovers a mod, that texture's pixels (in guest memory, 16-bit words swapped)
// say "MODDED" (tools/make_modded_badge.py) and go back for real DLC. Both
// panels share the texture: while one shows a real DLC character (British
// Bulldog...), it stays the DLC badge and a mod's panel shows no badge (a mod
// on the other side made the DLC character "MODDED").
const uint8_t kModdedRgba[] = {
#include "modded_badge.inc"
};
constexpr uint32_t kBadgeW = 256, kBadgeH = 64;
constexpr uint32_t kBadgeCaller = 0x82466E40;  // sub_82466830: sub_828B6CD0(id) at the badge
constexpr uint32_t kCaePickCaller = 0x824633CC;  // sub_82463090 state 14: sub_828B6CD0(id) - downloaded: easy editor

struct Badge {
  svrfmt::Bytes data;  // swapped as in memory
  size_t sig = 0;      // a distinctive 64 bytes in it
};
std::vector<Badge> g_badges;  // [0]: MODDED, then the originals
std::mutex g_badge_mutex;
// select screen cursor -> what its panel shows: 0 no badge, 1 a mod, 2 real DLC
std::vector<std::pair<uint32_t, int>> g_panel_mod;
int64_t g_badge_seen = 0;                            // last panel update (a select screen is up)
bool g_badge_thread = false;

svrfmt::Bytes Swap16(const uint8_t* p, size_t n) {
  svrfmt::Bytes o(p, p + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(o[i], o[i + 1]);
  return o;
}

size_t Signature(const svrfmt::Bytes& d) {
  for (size_t at = 0; at + 64 <= d.size(); at += 64) {
    int distinct = 0;
    for (int i = 0; i < 4; ++i) {
      bool seen = false;
      for (int j = 0; j < i; ++j) seen |= !std::memcmp(&d[at + i * 16], &d[at + j * 16], 16);
      distinct += !seen;
    }
    if (distinct >= 4) return at;
  }
  return 0;
}

// The originals from menuHD.pac (MENU, MENF, MENG, MENI, MENE: CHSI) and MODDED.
void LoadBadges() {
  FILE* f = std::fopen((g_game / "pac" / "menu" / "menuHD.pac").string().c_str(), "rb");
  if (!f) return;
  svrfmt::Bytes toc(0x4000);
  if (std::fread(toc.data(), 1, toc.size(), f) != toc.size() || std::memcmp(toc.data(), "EPAC", 4)) {
    std::fclose(f);
    return;
  }
  auto le32 = [&](size_t p) { return uint32_t(toc[p]) | toc[p + 1] << 8 | toc[p + 2] << 16 | uint32_t(toc[p + 3]) << 24; };
  std::vector<svrfmt::Bytes> found;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!le32(p)) break;
    const uint32_t cnt = le32(p + 4) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12) {
      if (std::memcmp(&toc[p], "CHSI", 4)) continue;
      svrfmt::Bytes e(size_t(le32(p + 8)) * 0x100);
      std::fseek(f, long(0x4000 + size_t(le32(p + 4)) * 0x800), SEEK_SET);
      if (std::fread(e.data(), 1, e.size(), f) != e.size()) continue;
      std::vector<svrfmt::PachEntry> pe;
      if (!svrfmt::PachRead(e, pe)) continue;
      for (const auto& x : pe) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(x.data), texs)) continue;
        for (const auto& t : texs)
          if (t.name == "DLCtex01" && t.data.size() == 128 + kBadgeW * kBadgeH) found.push_back(t.data);
      }
    }
  }
  std::fclose(f);
  if (found.empty()) return;
  svrfmt::Image img;
  img.w = kBadgeW, img.h = kBadgeH;
  img.rgba.assign(kModdedRgba, kModdedRgba + sizeof kModdedRgba);
  const svrfmt::Bytes dds = svrfmt::DdsEncode(img, svrfmt::DxtFormat::kDxt5, false);
  g_badges.push_back({Swap16(dds.data() + 128, kBadgeW * kBadgeH), 0});
  for (const auto& o : found) {
    Badge b{Swap16(o.data() + 128, kBadgeW * kBadgeH), 0};
    b.sig = Signature(b.data);
    g_badges.push_back(std::move(b));
  }
  REXLOG_INFO("[svr2011] superstar mods: badge ({} languages)", found.size());
}

using svr2011::NowMs;

bool Committed(uint32_t at) {
  auto* heap = g_memory->LookupHeap(at);
  rex::memory::HeapAllocationInfo info{};
  return heap && heap->QueryRegionInfo(at & ~0xFFFu, &info) && info.region_size &&
         (info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead);
}

// Which badge the texture at guest holds (-1: none of them).
int BadgeAt(uint8_t* b, uint32_t guest) {
  if (!guest || !Committed(guest) || !Committed(guest + kBadgeW * kBadgeH - 1)) return -1;
  for (size_t i = 0; i < g_badges.size(); ++i)
    if (!std::memcmp(b + guest, g_badges[i].data.data(), g_badges[i].data.size())) return int(i);
  return -1;
}

void BadgeLoop() {
  rex::thread::set_current_thread_name("superstar badges");
  uint32_t guest = 0;
  int original = 1;  // the language's original there
  int64_t last_scan = 0;
  int scans = 0;  // this visit of the select screen
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool want = false, active;
    {
      std::lock_guard lock(g_badge_mutex);
      active = NowMs() - g_badge_seen < 5000;
      if (!active) g_panel_mod.clear(), scans = 0;
      bool dlc = false;
      for (const auto& p : g_panel_mod) want |= p.second == 1, dlc |= p.second == 2;
      want = want && !dlc;
    }
    uint8_t* b = g_memory->virtual_membase();
    int now = BadgeAt(b, guest);
    if (now < 0) guest = 0;
    if (!guest) {
      if (!active || scans >= 10 || NowMs() - last_scan < 1000) continue;
      last_scan = NowMs();
      ++scans;
      const auto scan_t0 = std::chrono::steady_clock::now();
      // (the menu textures: physical memory, seen at 0xE0000000+ in the
      // VS screen search; then the other views)
      for (uint64_t lo : {0xE0000000ull, 0xA0000000ull}) {
        const uint64_t hi = lo == 0xE0000000ull ? 0xFFFF0000ull : 0xE0000000ull;
        for (uint64_t at = lo; at < hi && !guest;) {
          auto* heap = g_memory->LookupHeap(uint32_t(at));
          rex::memory::HeapAllocationInfo info{};
          if (!heap || !heap->QueryRegionInfo(uint32_t(at) & ~0xFFFu, &info) || !info.region_size) {
            at = (at & ~0xFFFull) + 0x10000;
            continue;
          }
          const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, hi);
          // (a texture's base is 4 KB aligned - the GPU's fetch constant holds it in 4 KB units: only page starts
          // are tried, each badge's signature bytes at its place there - 256 times fewer compares than every 16
          // bytes, which held a core for seconds through a match's load)
          if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead))
            for (uint64_t t = (at + 0xFFF) & ~0xFFFull; t < end && !guest; t += 0x1000)
              for (size_t i = 0; i < g_badges.size(); ++i) {
                const auto& g = g_badges[i];
                if (t + g.sig + 64 > end || std::memcmp(b + t + g.sig, g.data.data() + g.sig, 64)) continue;
                if (BadgeAt(b, uint32_t(t)) == int(i)) {
                  guest = uint32_t(t);
                  break;
                }
              }
          at = std::max<uint64_t>(end, at + 0x1000);
        }
        if (guest) break;
      }
      if (!guest) continue;
      now = BadgeAt(b, guest);
      REXLOG_INFO("[svr2011] superstar mods: badge texture at {:08X} (scan {} of this select screen, {} ms)", guest, scans,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scan_t0).count());
    }
    if (now > 0) original = now;
    const int target = want ? 0 : original;
    if (now != target) std::memcpy(b + guest, g_badges[target].data.data(), g_badges[target].data.size());
  }
}

}  // namespace

// CREATE AN ENTRANCE, ADVANCED CREATION of a superstar without a created entrance: the editor's block
// (sub_82879AF8) gets the created flag and five scene parts from his default template, looked up by id
// (sub_82883A10(list, id); none: a generic one whose parts name no scene - the entrance manager then had no
// entrant, the scene ended and the editor backed out to the menu). A mod: its own entrance's template, else
// its base's.
constexpr uint32_t kCaeTemplateCaller = 0x82879CF4;
REX_EXTERN(__imp__sub_82883A10);
REX_HOOK_RAW(sub_82883A10) {
  const Mod* m = uint32_t(ctx.lr) == kCaeTemplateCaller ? ModOf(ctx.r4.u32) : nullptr;
  if (!m) {
    __imp__sub_82883A10(ctx, base);
    return;
  }
  const auto in = ctx;
  for (const int id : {m->entrance, int(m->base)}) {
    if (id <= 0) continue;
    ctx = in;
    ctx.r4.u64 = uint32_t(id);
    __imp__sub_82883A10(ctx, base);
    if (ctx.r3.u32) {
      REXLOG_INFO("[svr2011] create an entrance: {} starts from the scene template of {}", m->name, id);
      return;
    }
  }
  ctx = in;
  __imp__sub_82883A10(ctx, base);
}

// ... and its music, movie and entrance numbers from his defaults (sub_828C3318(out, id): the profile's default
// copy, entrance block +0x04..+0x0C, out = {entrance, champion, movie, music, multi-title}) - a mod's are the
// base's there: its own (the profile's current ones) instead.
constexpr uint32_t kCaeDefaultsCallers[] = {0x82879C28, 0x82879C84};
REX_EXTERN(__imp__sub_828C3318);
REX_HOOK_RAW(sub_828C3318) {
  const uint32_t out = ctx.r3.u32, id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_828C3318(ctx, base);
  if (lr != kCaeDefaultsCallers[0] && lr != kCaeDefaultsCallers[1]) return;
  const uint32_t si = id < 512 ? svr2011::RosterIndex(base, id) : 512;
  if (!out || si >= 512 || !ModOf(id)) return;
  const uint8_t* e = base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kEntrance;
  std::memcpy(base + out + 0, e + 0x14, 2);  // entrance
  std::memcpy(base + out + 2, e + 0x16, 2);  // champion
  std::memcpy(base + out + 4, e + kMovie, 2);
  std::memcpy(base + out + 6, e + kMusic, 2);
  std::memcpy(base + out + 8, e + 0x18, 2);  // multi-title
}

// CREATE AN ENTRANCE backing out to its menu says nothing on screen and left nothing in the log (report a81bee): the
// superstar is picked, the game asks for his entrance scene at once (EntranceFallback, cae_mod_motions.cpp) - or it
// never does and the next thing is the menu's load. That's logged with what the mod asks for.
// (test aid: SVR2011_TEST_CAE_BACKOUT=1 - a scene asked for doesn't count, so leaving the editor logs the line)
namespace {
std::atomic<uint32_t> g_cae_picked{0};
std::atomic<int64_t> g_cae_picked_at{0};
}  // namespace
namespace svr2011 {
void CaePicked(uint32_t id) {
  g_cae_picked_at = std::chrono::steady_clock::now().time_since_epoch().count();
  g_cae_picked = id;
}
void CaeSceneAsked() {
  static const bool test = std::getenv("SVR2011_TEST_CAE_BACKOUT") != nullptr;
  if (!test) g_cae_picked = 0;
}
void CaeMenuLoads() {
  const uint32_t id = g_cae_picked.exchange(0);
  if (!id) return;
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch() -
                                                    std::chrono::steady_clock::duration(g_cae_picked_at.load())).count();
  const Mod* m = ModOf(id);
  if (!m) {
    REXLOG_WARN("[svr2011] create an entrance: backed out of id {} after {:.1f} s - its entrance scene never started (not a mod)", id, secs);
    return;
  }
  const char* why = m->entrance <= 0                         ? "no entrance= of its own, its base's scene never started"
                    : !EntranceScriptExists(m->entrance) ? "its entrance has no script, and its scene never started"
                                                             : "its entrance has a script, but its scene never started";
  REXLOG_WARN("[svr2011] create an entrance: backed out of {} ({}, id {}, base {}) after {:.1f} s - {} (entrance {}, movie {}, music {})",
              m->name, m->folder, id, int(m->base), secs, why, m->entrance, m->movie, m->music);
}
}  // namespace svr2011

// The select screen's panel: "is it DLC" for the badge (r26: the id; r31:
// the cursor).
REX_EXTERN(__imp__sub_828B6CD0);
REX_HOOK_RAW(sub_828B6CD0) {
  // CREATE AN ENTRANCE: the superstar picked (select screen state 14, sub_82463090) goes to the EASY / ADVANCED
  // popup unless he is "downloaded" - then straight to the easy editor (state 23). Mods sit on downloaded slots,
  // and the user asked for ADVANCED on the real DLC superstars too: none counts as downloaded here.
  if (uint32_t(ctx.lr) == kCaePickCaller) {
    svr2011::CaePicked(ctx.r3.u32);
    __imp__sub_828B6CD0(ctx, base);
    if (ctx.r3.u32) {
      static bool told = false;
      if (!told) {
        told = true;
        REXLOG_INFO("[svr2011] create an entrance: ADVANCED CREATION offered for downloaded superstars too");
      }
      ctx.r3.u64 = 0;
    }
    return;
  }
  if (uint32_t(ctx.lr) != kBadgeCaller || g_badges.empty()) {
    __imp__sub_828B6CD0(ctx, base);
    return;
  }
  const uint32_t cursor = SVR_CALLER_NV(ctx, r31);
  const bool mod = ModOf(ctx.r3.u32) != nullptr;
  __imp__sub_828B6CD0(ctx, base);
  const int shows = !ctx.r3.u32 ? 0 : mod ? 1 : 2;
  std::lock_guard lock(g_badge_mutex);
  g_badge_seen = NowMs();
  bool found = false, other_dlc = false;
  for (auto& p : g_panel_mod) {
    if (p.first == cursor) p.second = shows, found = true;
    else other_dlc |= p.second == 2;
  }
  if (!found) g_panel_mod.push_back({cursor, shows});
  if (shows == 1 && other_dlc) ctx.r3.u64 = 0;  // (the texture is the DLC badge now: none on the mod)
  if (!g_badge_thread) {
    g_badge_thread = true;
    std::thread(BadgeLoop).detach();
  }
}

// The DLC tile's list filter (sub_8244A128(screen, id): DLC-flagged,
// selectable, owned): mods are in the EXTRA list instead.
REX_EXTERN(__imp__sub_8244A128);
REX_HOOK_RAW(sub_8244A128) {
  if (ModOf(ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8244A128(ctx, base);
}

// Test aid: SVR2011_TEST_VFS_LOG=1 logs each virtual file lookup that finds
// nothing (sub_826B87B0(vfs, "/TYPE/NAME", out)), once per name.
// A mod superstar's model and select renders, asked for by the id it holds (EMD/%06d%02d: id, attire*10 + kind;
// SSFA/B/C/%04d: attire*1000 + id; with or without a leading /): its overlay entries are stamped with its key
// (Mod::key), so the name asked for becomes the key's - the guest copy of the new name, or 0.
uint32_t g_key_names = 0;  // guest: 32 x 64 bytes, used in turn (lookups come from two threads)
std::atomic<uint32_t> g_key_next{0};
uint32_t KeyAlias(uint8_t* base, const char* n) {
  const char* p = n[0] == '/' ? n + 1 : n;
  char out[64];
  unsigned v = 0, kind = 0;
  const Mod* m = nullptr;
  // Test aid: SVR2011_TEST_EMD_ASK=<name>=<name> (8 digits each, e.g. 00016102=00016300): that model is asked for
  // under the other name - an empty pool id's model in a match. SVR2011_TEST_EMD_STANDIN_OFF=1: no stand-in (the crash).
  static const std::string ask = [] { const char* e = std::getenv("SVR2011_TEST_EMD_ASK"); return std::string(e ? e : ""); }();
  static const bool standin_off = std::getenv("SVR2011_TEST_EMD_STANDIN_OFF") != nullptr;
  char asked[16];
  if (ask.size() == 17 && !std::strncmp(p, "EMD/", 4) && !ask.compare(0, 8, p + 4, strnlen(p + 4, 12))) {
    std::snprintf(asked, sizeof asked, "EMD/%s", ask.c_str() + 9);
    p = asked;
  }
  if (!std::strncmp(p, "EMD/", 4) && strnlen(p, 16) == 12 && std::sscanf(p + 4, "%6u%2u", &v, &kind) == 2) {
    m = ModOf(v);
    // A pool id with no mod on it has no model in any pac (a save can still name one, from a mod since removed: report
    // 11ffd2 '/EMD/00016300', every start). The loader then opens a null pac name (sub_826A1D38's request +72) and
    // crashes in sub_826BB8A0; a failed open only retries, so the name is answered here: the model a match would use
    // for the disc's own first superstar. (Other kinds - 4 gear, 5, 6 - are optional: the game takes the miss.)
    // big_roster's ids count too, whether it is on or not; those from 400 can also be a mod's key, whose overlay
    // entries are real: left to the lookup.
    const bool empty_id = !m && (std::find(std::begin(kPool), std::end(kPool), v) != std::end(kPool) ||
                                 (v >= svr2011::kFirstHighId && v <= svr2011::kLastHighId &&
                                  std::none_of(g_mods.begin(), g_mods.end(), [v](const Mod& x) { return x.key == v; })));
    if (empty_id && !standin_off && (kind % 10 == 0 || kind % 10 == 2)) {
      static std::atomic<int> said{0};
      if (said++ < 8)
        REXLOG_WARN("[svr2011] superstar mods: '/EMD/{:06}{:02}' - no mod on id {}: the model of id 139 stands in", v, kind, v);
      std::snprintf(out, sizeof out, "%.*sEMD/00013902", int(n[0] == '/'), n);
    } else if (p == asked) {
      std::snprintf(out, sizeof out, "%.*s%s", int(n[0] == '/'), n, asked);
    } else {
      if (!m) return 0;
      // An attire the mod no longer has (removed in the Mod Maker, suggestion 04d611 - a save or a setup can still
      // pick it): the model is missing and the loader crashed in sub_826BB8A0, so attire 1's model stands in.
      unsigned k = kind;
      if (k % 10 == 2 && k / 10 >= m->attires) {
        k = 2;
        static std::atomic<int> said{0};
        if (said++ < 8)
          REXLOG_WARN("[svr2011] superstar mods: {} has no attire {} ({} attires): attire 1's model stands in", m->name,
                      kind / 10 + 1, m->attires);
      }
      if (m->key == v && k == kind) return 0;
      std::snprintf(out, sizeof out, "%.*sEMD/%06u%02u", int(p - n), n, m->key, k);
    }
  } else if (g_mods.empty()) {
    return 0;
  } else if (p[0] == 'S' && p[1] == 'S' && p[2] == 'F' && p[3] >= 'A' && p[3] <= 'C' && p[4] == '/' &&
             strnlen(p, 16) == 9 && std::sscanf(p + 5, "%4u", &v) == 1) {
    if (!(m = ModOf(v % 1000)) || m->key == v % 1000) return 0;
    std::snprintf(out, sizeof out, "%.*s%.5s%04u", int(p - n), n, p, v / 1000 * 1000 + m->key);
  } else {
    return 0;
  }
  if (!g_key_names && !(g_key_names = g_memory->SystemHeapAlloc(32 * 64, 0x10))) return 0;
  const uint32_t at = g_key_names + (g_key_next++ % 32) * 64;
  std::memcpy(base + at, out, std::strlen(out) + 1);
  return at;
}

REX_EXTERN(__imp__sub_826B87B0);
REX_HOOK_RAW(sub_826B87B0) {
  static const bool log = [] {
    const char* e = std::getenv("SVR2011_TEST_VFS_LOG");
    return e && *e == '1';
  }();
  // Superstar Threads' list with the mods' records (superstar_threads.h): asked for by another name
  if (const char* n = reinterpret_cast<const char*>(base + ctx.r4.u32); !std::strcmp(n, "/EDIT/SSTL") || !std::strcmp(n, "EDIT/SSTL"))
    svr2011::LogThreadsSlots(base);
  if (const uint32_t alias = svr2011::ThreadsAlias(reinterpret_cast<const char*>(base + ctx.r4.u32))) ctx.r4.u64 = alias;
  // the chosen VS screen style's theme (vs_styles.h): asked for by another name, as below
  if (const uint32_t alias = svr2011::VsStyleAlias(reinterpret_cast<const char*>(base + ctx.r4.u32))) ctx.r4.u64 = alias;
  // CAW parts from mods (caw_parts.h): the part lists and thumbnails with their records, by the overlay's names
  if (const uint32_t alias = svr2011::CawPartsAlias(reinterpret_cast<const char*>(base + ctx.r4.u32))) ctx.r4.u64 = alias;
  // a media mod's replacement (renders: media_mods.h) - the first match in
  // the archive list wins, so the game's own would: asked for by another name
  if (const uint32_t alias = svr2011::MediaAlias(reinterpret_cast<const char*>(base + ctx.r4.u32))) {
    ctx.r4.u64 = alias;
    if (log) {
      __imp__sub_826B87B0(ctx, base);
      REXLOG_INFO("[svr2011] media mods: {:.16} resolved {}", reinterpret_cast<const char*>(base + alias), ctx.r3.u32);
      return;
    }
  }
  // a superstar mod's model / renders: its key's entries (KeyAlias)
  else if (const uint32_t key = KeyAlias(base, reinterpret_cast<const char*>(base + ctx.r4.u32))) ctx.r4.u64 = key;
  // Test aid: SVR2011_TEST_VFS_TRACE=<text> - every lookup whose name holds that text: the result and the
  // out record's first 16 bytes (which archive answered), once per name and result
  static const std::string trace = [] { const char* e = std::getenv("SVR2011_TEST_VFS_TRACE"); return std::string(e ? e : ""); }();
  if (!trace.empty()) {
    const std::string nm(reinterpret_cast<const char*>(base + ctx.r4.u32), 0,
                         strnlen(reinterpret_cast<const char*>(base + ctx.r4.u32), 64));
    if (nm.find(trace) != std::string::npos) {
      const uint32_t out = ctx.r5.u32, caller = uint32_t(ctx.lr);
      std::string chain;  // (the callers above: the guest stack's back chain, the saved LR at frame - 8)
      for (uint32_t fr = ctx.r1.u32, k = 0; k < 6 && fr > 0x70000000u && fr < 0x80000000u; ++k) {
        fr = Rd32(base + fr);
        if (fr <= 0x70000000u || fr >= 0x80000000u) break;
        chain += fmt::format(" {:08X}", Rd32(base + fr - 8));
      }
      __imp__sub_826B87B0(ctx, base);
      std::string o;
      for (int i = 0; out && i < 16; ++i) o += fmt::format("{:02X}", base[out + i]);
      static std::mutex tm;
      static std::vector<std::string> told;
      const std::string k = nm + "|" + o;
      std::lock_guard lock(tm);
      if (std::find(told.begin(), told.end(), k) == told.end()) {
        told.push_back(k);
        REXLOG_INFO("[svr2011] vfs trace: {} -> {:08X} out {} (from {:08X}{})", nm, ctx.r3.u32, o, caller, chain);
      }
      return;
    }
  }
  if (!log) {
    const auto asked = ctx;
    __imp__sub_826B87B0(ctx, base);
    // (a part a saved CAW names that no pac has - its mod gone, or another machine's: that category's first)
    if (!ctx.r3.u32 && uint32_t(asked.lr) != 0x826A17FC)
      if (const uint32_t alias = svr2011::CawPartsMissing(reinterpret_cast<const char*>(base + asked.r4.u32))) {
        const auto lr = ctx.lr;
        ctx = asked;
        ctx.lr = lr;
        ctx.r4.u64 = alias;
        __imp__sub_826B87B0(ctx, base);
      }
    return;
  }
  const std::string name(reinterpret_cast<const char*>(base + ctx.r4.u32), 0,
                         strnlen(reinterpret_cast<const char*>(base + ctx.r4.u32), 64));
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_826B87B0(ctx, base);
  if (ctx.r3.u32) return;
  static std::mutex m;
  static std::vector<std::string> seen;
  std::lock_guard lock(m);
  if (std::find(seen.begin(), seen.end(), name) != seen.end()) return;
  seen.push_back(name);
  REXLOG_INFO("[svr2011] vfs miss: {} (from {:08X})", name, caller);
}

// ---- The name call
// The ring announcer and the commentary pick a Superstar's name call by its
// character id (scratchpad re_namecall): mod ids have none (the announcer's
// table says "dummy", the commentary banks don't exist). A mod gets its
// base's call, or (call=) a Created Superstar nickname, as a CAW's.
// Announcer: sub_825EA208(out, slot) fills {+0 id, +4 call index, ..., +20
// u16 nickname (read when +0 < 50)}.
// Test aid: SVR2011_TEST_NAMECALL=1 logs the announcer's ids, the commentary
// banks and the audio events posted ("Play_...").
bool NameCallLog() {
  static const bool on = [] {
    const char* e = std::getenv("SVR2011_TEST_NAMECALL");
    return e && *e == '1';
  }();
  return on;
}
// The announcer's name table (from sound.pac): 45 categories of 64-byte
// event names ("RA_JR_SSP_<NAME>_0"); T = *(*(0x82EC4C18) + 32) + 4, count
// of category c at T+420+c*8, its names at *(T+424+c*8). Categories 2, 4, 5,
// 8, 40, 41, 42 are by character id ("dummy" for the mods' ids).
constexpr uint32_t kSoundMgr = 0x82EC4C18;
constexpr uint32_t kIdCategories[] = {2, 4, 5, 8, 9, 40, 41, 42};
// The table: T from sub_825FDDC0(*(sub_825E5E68() + 32), &T), as
// sub_825EA4E8 reads it.
uint32_t CallTable(PPCContext& ctx, uint8_t* base) {
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  sub_825E5E68(ctx, base);
  uint32_t T = 0;
  if (const uint32_t obj = ctx.r3.u32 ? Rd32(base + ctx.r3.u32 + 32) : 0) {
    const uint32_t out = saved.r1.u32 - 0x100;
    Wr32(base + out, 0);
    ctx.r3.u64 = obj;
    ctx.r4.u64 = out;
    sub_825FDDC0(ctx, base);
    T = Rd32(base + out);
  }
  ctx = saved;
  return T;
}

void WriteCallNames(PPCContext& ctx, uint8_t* base, uint32_t id) {
  const uint32_t T = CallTable(ctx, base);

  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: table {:08X}", T);
  if (!T) return;
  for (uint32_t c : kIdCategories) {
    const uint32_t count = Rd32(base + T + 420 + c * 8), names = Rd32(base + T + 424 + c * 8);
    if (!names || id >= count) continue;
    char n[64];
    std::snprintf(n, sizeof n, "RA_JR_MOD_%u_%u", id, c);
    if (!std::strcmp(reinterpret_cast<const char*>(base + names + id * 64), n)) continue;  // (already)
    if (NameCallLog())
      REXLOG_INFO("[svr2011] name call: table {} [{}] was \"{:.60}\"", c, id,
                  reinterpret_cast<const char*>(base + names + id * 64));
    std::memset(base + names + id * 64, 0, 64);
    std::memcpy(base + names + id * 64, n, std::strlen(n));
  }
}
// announcer=<NAME>: every category where the base's entry says the base's own
// name ("RA_JR_SSN_MATTHARDY_1": the name from category 8) gets the same
// entry with NAME instead ("RA_JR_SSN_JEFFHARDY_1"), at the mod's id. The
// game picks the RA_TC_ announcer's from the RA_JR_ name itself.
void WriteAnnouncerNames(PPCContext& ctx, uint8_t* base, const Mod& m) {
  const uint32_t T = CallTable(ctx, base);
  if (!T) return;
  auto entry = [&](uint32_t c, uint32_t id) -> char* {
    const uint32_t count = Rd32(base + T + 420 + c * 8), names = Rd32(base + T + 424 + c * 8);
    return names && id < count ? reinterpret_cast<char*>(base + names + id * 64) : nullptr;
  };
  const char* b8 = entry(8, m.base);  // "RA_JR_SSN_<BASE NAME>_<n>"
  if (!b8 || std::strncmp(b8, "RA_JR_SSN_", 10)) return;
  std::string token(b8 + 10, strnlen(b8 + 10, 50));
  if (const size_t u = token.rfind('_'); u != std::string::npos) token.resize(u);
  if (token.empty()) return;
  for (uint32_t c = 1; c <= 45; ++c) {  // (the categories are 1..45: sub_825EA4E8)
    const char* b = entry(c, m.base);
    char* d = entry(c, m.slot);
    if (!b || !d) continue;
    std::string n(b, strnlen(b, 63));
    const size_t at = n.find("_" + token + "_");
    if (at == std::string::npos) continue;
    n.replace(at + 1, token.size(), m.announcer);
    if (n.size() > 63 || !std::strcmp(d, n.c_str())) continue;
    if (NameCallLog()) REXLOG_INFO("[svr2011] name call: table {} [{}] \"{:.60}\" -> {}", c, m.slot, d, n);
    std::memset(d, 0, 64);
    std::memcpy(d, n.data(), n.size());
  }
}
// hometown=: the announcer's introduction scripts (sound.pac, sub_825EA590)
// are a Superstar's (+0 >= 50: his whole recorded introduction, category 16)
// or a Created Superstar's (... city (category 10, index +12), state (11,
// +16; left out when the city's clip names it), the nickname's name calls
// (12 / 13 / 43 / 44 / 45, and 3 "accompanied by", index +20)). A mod with a
// hometown takes the second with the city and state set, and its nickname is
// a virtual index (kNameRow + the row of its name calls by character id)
// that sub_825EA4E8 below points at those rows (8 / 9 / 40 / 41 / 42 / 2).
constexpr uint32_t kNameRow = 0x4000;  // (below 0x8000: the index is sign-extended)
constexpr uint32_t kNoState = 59;      // (a state with no clip: THE CARIBBEAN)

// The entry of category c whose name is "RA_JR_<prefix>_<token>_..." (-1: none).
int FindCall(uint8_t* base, uint32_t T, uint32_t c, const char* prefix, const std::string& token) {
  const uint32_t count = Rd32(base + T + 420 + c * 8), names = Rd32(base + T + 424 + c * 8);
  if (!names) return -1;
  const std::string want = std::string("RA_JR_") + prefix + "_" + token + "_";
  for (uint32_t i = 0; i < count; ++i)
    if (!std::strncmp(reinterpret_cast<const char*>(base + names + i * 64), want.c_str(), want.size())) return int(i);
  return -1;
}

bool Hometown(PPCContext& ctx, uint8_t* base, uint32_t out, const Mod& m) {
  const uint32_t T = CallTable(ctx, base);
  if (!T) return false;
  const int city = FindCall(base, T, 10, "HTC", m.hometown_city);
  const int state = m.hometown_state.empty() ? -1 : FindCall(base, T, 11, "HTS", m.hometown_state);
  if (city < 0 || (state < 0 && !m.hometown_state.empty())) {
    static bool told = false;
    if (!told) REXLOG_WARN("[svr2011] superstar mods: {}: no hometown clip {},{}", m.name, m.hometown_city, m.hometown_state);
    told = true;
    return false;
  }
  uint32_t nick;
  if (!m.announcer.empty()) {
    WriteAnnouncerNames(ctx, base, m);
    nick = kNameRow + m.slot;
  } else if (!m.voice.empty()) {
    WriteCallNames(ctx, base, m.slot);
    nick = kNameRow + m.slot;
  } else if (m.call >= 0) {
    nick = uint32_t(m.call);  // (a Created Superstar nickname's own calls)
  } else {
    nick = kNameRow + m.base;
  }
  Wr32(base + out, 0);  // (the Created Superstar script)
  Wr32(base + out + 12, uint32_t(city));
  Wr32(base + out + 16, state < 0 ? kNoState : uint32_t(state));
  base[out + 20] = uint8_t(nick >> 8), base[out + 21] = uint8_t(nick);
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: {} from city {} state {}, names row {:X}", m.name, city, state, nick);
  return true;
}

// sub_825EA4E8(_, &name, category, index): the announcer's event name. A
// virtual nickname (Hometown) in a nickname category: the name call of that
// row in the same kind of category by character id.
REX_EXTERN(__imp__sub_825EA4E8);
REX_HOOK_RAW(sub_825EA4E8) {
  const uint32_t name = ctx.r4.u32, c = ctx.r5.u32, i = ctx.r6.u32 & 0xFFFF;
  __imp__sub_825EA4E8(ctx, base);
  if (!name || i < kNameRow || i >= kNameRow + 0x1000) return;
  uint32_t to = 0;
  switch (c) {
    case 12: to = 8; break;
    case 13: to = 9; break;
    case 43: to = 40; break;
    case 44: to = 41; break;
    case 45: to = 42; break;
    case 3: to = 2; break;
    default: return;
  }
  const uint32_t T = CallTable(ctx, base);
  if (!T) return;
  const uint32_t row = i - kNameRow, count = Rd32(base + T + 420 + to * 8), names = Rd32(base + T + 424 + to * 8);
  if (names && row < count) Wr32(base + name, names + row * 64);
}

// A Created Superstar with a stock Superstar's name calls: <game>/Mods/
// caw_announcer.txt, lines "<his full name>=<NAME>" (a name call's clip,
// port/tools/announcer_calls.json's superstars, or one the banks still have:
// JEFFHARDY). The announcer then takes the Superstar introduction - NAME's
// own recorded one (his hometown, his name). A NAME with no row of its own
// gets the CAW's row (by its id, 0..49: "dummy", unused) written from Matt
// Hardy's with the name swapped (WriteAnnouncerNames).
constexpr uint32_t kTemplateCall = 112;  // (Matt Hardy: every category his name is in)
std::map<std::string, std::string> CawCalls() {
  if (svr2011::NoMods()) return {};  // (Start Without Mods: no_mods.h)
  static std::map<std::string, std::string> calls;
  static fs::file_time_type stamp{};
  const fs::path file = g_game / "Mods" / "caw_announcer.txt";
  std::error_code ec;
  const auto t = fs::last_write_time(file, ec);
  if (ec) {
    calls.clear();
    return calls;
  }
  if (t == stamp) return calls;
  stamp = t;
  calls.clear();
  if (FILE* f = std::fopen(file.string().c_str(), "rb")) {
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t eq = l.rfind('=');
      if (eq == std::string::npos || eq == 0) continue;
      std::string name;
      for (char c : l.substr(eq + 1))
        if (std::isalnum(uint8_t(c)) && name.size() < 24) name.push_back(char(std::toupper(uint8_t(c))));
      if (!name.empty()) calls[l.substr(0, eq)] = name;
    }
    std::fclose(f);
  }
  REXLOG_INFO("[svr2011] superstar mods: {} Created Superstars with stock name calls", calls.size());
  return calls;
}

// The Created Superstar in slot `slot`: his full name (roster record +34,
// 8-bit, as the roster's names are) and id (+32).
bool CawName(PPCContext& ctx, uint8_t* base, uint32_t slot, std::string* name, uint32_t* id) {
  const uint32_t roster = Rd32(base + 0x82EDEA88);
  if (!roster) return false;
  const auto saved = ctx;
  ctx.r3.u64 = roster;
  ctx.r4.u64 = slot;
  sub_828B5A18(ctx, base);
  const uint32_t rec = ctx.r3.u32;
  ctx = saved;
  if (!rec) return false;
  *id = uint32_t(base[rec + 32]) << 8 | base[rec + 33];
  name->clear();
  for (uint32_t k = 0; k < 40 && base[rec + 34 + k]; ++k) name->push_back(char(base[rec + 34 + k]));
  return !name->empty();
}

void CawCall(PPCContext& ctx, uint8_t* base, uint32_t out, uint32_t slot) {
  const auto calls = CawCalls();
  if (calls.empty() && !NameCallLog()) return;
  std::string name;
  uint32_t id = 0;
  if (!CawName(ctx, base, slot, &name, &id) || id >= 50) return;
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: Created Superstar \"{}\" (id {})", name, id);
  const auto c = calls.find(name);
  if (c == calls.end()) return;
  const uint32_t T = CallTable(ctx, base);
  if (!T) return;
  if (const int s = FindCall(base, T, 8, "SSN", c->second); s >= 50) {
    Wr32(base + out, uint32_t(s));
    Wr32(base + out + 4, uint32_t(s));
  } else {
    Mod t;
    t.base = kTemplateCall, t.slot = id, t.announcer = c->second;
    WriteAnnouncerNames(ctx, base, t);
    Wr32(base + out, kTemplateCall);  // (the Superstar script; his rows at the CAW's id)
    Wr32(base + out + 4, id);
  }
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: Created Superstar {} (id {}) as {}", name, id, c->second);
}

// A stock or DLC Superstar the announcer's table has no name for: DLC John Cena (purple, id 57) and DLC Undertaker
// (biker, id 58) are "dummy" in every by-id category, the DLC's own sound_dlc_02.pac copy too (report
// 20261101-023939-02ef48: no ring-announcer name, generic commentary), and their commentary ids 10007 / 10008 make
// no bank. They borrow the calls of the same person - the record's +228 (same person) when that one has a name,
// else the roster record with the same name that has one (JOHN CENA 139, UNDERTAKER 103). 0: none (a name of its
// own, a mod, or no donor). Category 8 is the name ("RA_JR_SSN_<NAME>_1").
bool HasCallName(uint8_t* base, uint32_t T, uint32_t id) {
  const uint32_t count = Rd32(base + T + 420 + 8 * 8), names = Rd32(base + T + 424 + 8 * 8);
  return names && id < count && std::strcmp(reinterpret_cast<const char*>(base + names + id * 64), "dummy") != 0;
}
const uint8_t* RosterRecord(uint8_t* base, uint32_t id) {
  if (!id || id >= 1000) return nullptr;
  const uint32_t si = svr2011::RosterIndex(base, id);
  if (si >= 512) return nullptr;
  const uint8_t* r = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
  return Rd16(r + kOwnId) == id ? r : nullptr;
}
uint32_t CallDonor(PPCContext& ctx, uint8_t* base, uint32_t id) {
  static std::mutex mu;
  static std::map<uint32_t, uint32_t> found;  // (per id, once the table is up)
  if (id < 50 || id >= 1000 || ModOf(id)) return 0;
  {
    std::lock_guard lock(mu);
    if (const auto f = found.find(id); f != found.end()) return f->second;
  }
  const uint32_t T = CallTable(ctx, base);
  const uint8_t* rec = RosterRecord(base, id);
  if (!T || !rec) return 0;
  uint32_t donor = 0;
  if (!HasCallName(base, T, id)) {
    const uint32_t same = Rd16(rec + kSamePerson);
    if (same != id && RosterRecord(base, same) && !ModOf(same) && HasCallName(base, T, same)) donor = same;
    const std::string name(reinterpret_cast<const char*>(rec + kFullName), strnlen(reinterpret_cast<const char*>(rec + kFullName), 32));
    for (uint32_t other = 50; !donor && !name.empty() && other < 1000; ++other) {
      const uint8_t* o = other != id && !ModOf(other) ? RosterRecord(base, other) : nullptr;
      if (o && !std::strncmp(reinterpret_cast<const char*>(o + kFullName), name.c_str(), 32) && HasCallName(base, T, other))
        donor = other;
    }
    REXLOG_INFO("[svr2011] name call: {} (id {}, same person {}) has no name call - {}", name, id, same,
                donor ? fmt::format("uses id {}'s", donor) : std::string("none to borrow"));
  }
  std::lock_guard lock(mu);
  found[id] = donor;
  return donor;
}

REX_EXTERN(__imp__sub_825EA208);
REX_HOOK_RAW(sub_825EA208) {
  const uint32_t out = ctx.r3.u32, slot = ctx.r4.u32;
  __imp__sub_825EA208(ctx, base);
  if (!out) return;
  if (Rd32(base + out) < 50) {
    CawCall(ctx, base, out, slot);
    return;
  }
  if (NameCallLog())
    REXLOG_INFO("[svr2011] name call: announcer id {} call {} nickname {}", Rd32(base + out), Rd32(base + out + 4),
                Rd16(base + out + 20));
  const Mod* m = ModOf(Rd32(base + out));
  if (!m) {
    if (const uint32_t donor = CallDonor(ctx, base, Rd32(base + out)))
      Wr32(base + out + 4, donor);  // (his own id stays: the call index is the donor's, as a mod's base)
    return;
  }
  if (!m->hometown_city.empty() && Hometown(ctx, base, out, *m)) return;
  if (!m->announcer.empty()) {  // (the game's own clips of that name)
    WriteAnnouncerNames(ctx, base, *m);
    return;
  }
  if (!m->voice.empty()) {  // (its own recording: the mod's own, silent, event names; PlayVoice)
    WriteCallNames(ctx, base, m->slot);
    return;
  }
  if (m->call < 0) {
    Wr32(base + out + 4, m->base);
  } else {
    Wr32(base + out, 0);  // (a Created Superstar's: the nickname is used)
    base[out + 20] = uint8_t(m->call >> 8), base[out + 21] = uint8_t(m->call);
  }
}
// Commentary: the bank "Comm_<name>" for n = the id (ids 50-69: n + 9950;
// CAWs: nickname + 1, the CAS_* names) - sub_8261EAB0(buf, n) makes the
// name, sub_825FDCD8(n) leaves out ids without commentary (0).
const Mod* ModOfCommentary(uint32_t n) { return ModOf(n >= 10000 && n < 10070 ? n - 9950 : n); }
REX_EXTERN(__imp__sub_8261EAB0);
REX_HOOK_RAW(sub_8261EAB0) {
  const uint32_t n = ctx.r4.u32, buf = ctx.r3.u32;
  if (const Mod* m = ModOfCommentary(ctx.r4.u32)) {
    ctx.r4.u64 = m->call < 0 ? m->base : uint32_t(m->call + 1);
  } else if (const uint32_t id = n >= 10000 && n < 10070 ? n - 9950 : n; id >= 50) {
    if (const uint32_t donor = CallDonor(ctx, base, id))  // (DLC Cena / Undertaker: the same person's commentary)
      ctx.r4.u64 = donor < 70 ? donor + 9950 : donor;
  }
  const uint32_t used = ctx.r4.u32;
  __imp__sub_8261EAB0(ctx, base);
  if (NameCallLog() && buf)
    REXLOG_INFO("[svr2011] name call: commentary {} -> {} \"{:.40}\"", n, used, reinterpret_cast<const char*>(base + buf));
}
// ---- An entrance per attire (manifest attire<N>_entrance=<number>)
//
// The match takes a wrestler's entrance number from the entrance manager's
// copy of his profile block (S = mgr + 36 + idx * 280, idx = his match slot):
// sub_82756538(mgr, idx, part) the normal one (part 0: S+16 unless he has a
// created entrance, S+195 != 0), sub_82756718 / sub_82756768(mgr, idx) the
// champion and multi-title ones; sub_8275E480 then picks among them by the
// titles he holds. The stock game varies it by attire only in code (Rey
// Mysterio's "Evil" attire -> 512 in sub_8275E480). Here a mod's attire with
// its own entrance gets it from all three, so it is used with or without a
// title; a team entrance (sub_82756260) and a created entrance are left alone.
// The attire is the match slot's costume key (slot record
// *(0x82E3DDFC) + 132 + idx * 52, +16: id * 100 + attire * 10 + kind; mod
// attire N = attire digit N - 1). Research: docs/ENTRANCE_ATTIRES.md.
namespace {
constexpr uint32_t kSlotTable = 0x82E3DDFC, kSlotsAt = 132, kSlotSize = 52, kSlotKey = 16;
int EntranceForAttire(PPCContext& ctx, uint8_t* base, uint32_t mgr, uint32_t idx, bool part0) {
  if (idx >= 16) return -1;
  const uint32_t table = Rd32(base + kSlotTable);
  if (!table) return -1;
  const uint32_t key = Rd32(base + table + kSlotsAt + idx * kSlotSize + kSlotKey);
  const Mod* m = ModOf(key / 100);
  const uint32_t attire = key / 10 % 10;
  int own = -1;  // (an attire mod's entrance=)
  if (!m) {
    if (const AttireMod* am = AttireModOf(key / 100, attire); am && am->entrance > 0) own = am->entrance;
    else if (const auto it = g_attire_entrance_only.find({key / 100, attire}); it != g_attire_entrance_only.end()) own = it->second;
  }
  if (own <= 0 && (!m || attire >= 4 || m->attire_entrance[attire] <= 0)) return -1;
  if (part0 && base[mgr + 36 + idx * 280 + 195]) return -1;  // (a created entrance)
  const auto saved = ctx;
  ctx.r3.u64 = mgr;
  ctx.r4.u64 = idx;
  sub_82756260(ctx, base);  // (a team's entrance)
  const bool team = ctx.r3.u32 != 0;
  ctx = saved;
  if (team) return -1;
  static int logged = 0;
  const int n = own > 0 ? own : m->attire_entrance[attire];
  if (logged++ < 16)
    REXLOG_INFO("[svr2011] superstar mods: {} attire {}: entrance {}", m ? m->name : std::to_string(key / 100), attire + 1, n);
  return n;
}

// A mod whose profile keeps entrance 0 (a save's entrance block all zero - kept as the mod's own profile, the save's
// late write after SetEntranceMedia): CREATE AN ENTRANCE made its entrance stage, part 0 played 0 and the stage ended
// at once - the editor backed out to its menu (Android reports a81bee / 9d32ce: Kane '06, Shawn Michaels, Owen Hart;
// reproduced on PC with a zeroed block). Entrance 0 for a mod's slot (not a created entrance's): its own entrance= when
// that has a script, else its base's default.
uint32_t ModZeroEntrance(uint8_t* base, uint32_t mgr, uint32_t idx, uint32_t n) {
  if (n || idx >= 16 || base[mgr + 36 + idx * 280 + 195]) return n;  // (a created entrance: its parts are its own)
  const uint32_t table = Rd32(base + kSlotTable);
  if (!table) return n;
  const uint32_t key = Rd32(base + table + kSlotsAt + idx * kSlotSize + kSlotKey);
  const Mod* m = ModOf(key / 100);
  if (!m) return n;
  uint32_t use = m->entrance > 0 && svr2011::EntranceScriptExists(m->entrance) ? uint32_t(m->entrance) : 0;
  if (!use) {
    const uint32_t bi = svr2011::RosterIndex(base, m->base);
    if (bi < 512) {
      const uint8_t* e = base + svr2011::RosterEntry(kProfiles, kProfileSize, bi) + kEntrance;
      use = Rd16(e + 0x08) ? Rd16(e + 0x08) : Rd16(e + 0x14);  // (the base's default copy, else its current one)
    }
  }
  if (!use) return n;
  // (and the zeros healed where the editor and the next save read them: the entrance manager's copy - +16 entrance,
  // +18 champion, +20 multi-title - and the profile's +0x14 / +0x16 / +0x18; else the editor's rows showed the
  // first entrance of each list)
  auto heal = [&](uint32_t at) {
    if (!base[at] && !base[at + 1]) base[at] = uint8_t(use >> 8), base[at + 1] = uint8_t(use);
  };
  for (const uint32_t k : {16u, 18u, 20u}) heal(mgr + 36 + idx * 280 + k);
  if (const uint32_t si = svr2011::RosterIndex(base, m->slot); si < 512)
    for (const uint32_t k : {0x14u, 0x16u, 0x18u}) heal(svr2011::RosterEntry(kProfiles, kProfileSize, si) + kEntrance + k);
  static std::set<uint32_t> told;
  if (told.insert(m->slot).second)
    REXLOG_WARN("[svr2011] superstar mods: {} (id {}) - its saved entrance is 0 (an empty entrance block): entrance {} "
                "instead", m->name, m->slot, use);
  return use;
}
}  // namespace
// Test aid SVR2011_TEST_CAE_LOG: the entrance block CREATE AN ENTRANCE's page loads
// (sub_82753570(mgr, out): out = a copy of mgr+36, 280 bytes) - music +12, movie +14,
// entrance +16, champion +18, multi-title +20, created entrance flag +195.
REX_EXTERN(__imp__sub_82753570);
REX_HOOK_RAW(sub_82753570) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82753570(ctx, base);
  static const bool on = std::getenv("SVR2011_TEST_CAE_LOG") != nullptr;
  if (on && out)
    REXLOG_INFO("[svr2011] cae: block music {} movie {} entrance {} champion {} multi {} created {}", Rd16(base + out + 12),
                Rd16(base + out + 14), Rd16(base + out + 16), Rd16(base + out + 18), Rd16(base + out + 20),
                base[out + 195]);
}

REX_EXTERN(__imp__sub_82756538);
REX_HOOK_RAW(sub_82756538) {
  const uint32_t mgr = ctx.r3.u32, idx = ctx.r4.u32, part = ctx.r5.u32;
  __imp__sub_82756538(ctx, base);
  if (part == 0 && ctx.r3.u32 != 0) {
    const auto r = ctx.r3;
    if (const int n = EntranceForAttire(ctx, base, mgr, idx, true); n > 0) ctx.r3.u64 = uint32_t(n);
    else ctx.r3 = r;
  }
  // (a number whose mod is gone: cae_mod_motions.cpp; a mod's empty saved entrance: ModZeroEntrance)
  ctx.r3.u64 = svr2011::EntranceFallback(base, mgr, idx, int(part), ctx.r3.u32);
  if (part == 0) ctx.r3.u64 = ModZeroEntrance(base, mgr, idx, ctx.r3.u32);
  // (test aid SVR2011_TEST_CAE_LOG: the number each slot's scene part plays - a created entrance's are 10000+)
  static const bool on = std::getenv("SVR2011_TEST_CAE_LOG") != nullptr;
  static std::set<uint64_t> told;
  if (on && idx < 16 && told.insert(uint64_t(mgr) << 32 | uint64_t(idx) << 28 | uint64_t(part & 0xFF) << 20 | (ctx.r3.u32 & 0xFFFFF)).second)
    REXLOG_INFO("[svr2011] cae: slot {} part {} plays {} (created {})", idx, part, ctx.r3.u32,
                base[mgr + 36 + idx * 280 + 195]);
}
// (test aid SVR2011_TEST_CAE_LOG: a team's entrance as the match asks it - sub_823373A8(mgr, idx) reads the team
// record's tag or trio entrance (TEAM MANAGER's ENTRANCE SETTING); its slots ask no singles number then)
REX_EXTERN(__imp__sub_823373A8);
REX_HOOK_RAW(sub_823373A8) {
  const uint32_t idx = ctx.r4.u32;
  __imp__sub_823373A8(ctx, base);
  static const bool on = std::getenv("SVR2011_TEST_CAE_LOG") != nullptr;
  static int told = 0;
  if (on && told++ < 24) REXLOG_INFO("[svr2011] cae: slot {} team entrance {}", idx, ctx.r3.u32 & 0xFFFF);
}
REX_EXTERN(__imp__sub_82756718);
REX_HOOK_RAW(sub_82756718) {
  const uint32_t mgr = ctx.r3.u32, idx = ctx.r4.u32;
  __imp__sub_82756718(ctx, base);
  const auto r = ctx.r3;
  if (const int n = EntranceForAttire(ctx, base, mgr, idx, false); n > 0) ctx.r3.u64 = uint32_t(n);
  else ctx.r3 = r;
  ctx.r3.u64 = svr2011::EntranceFallback(base, mgr, idx, -1, ctx.r3.u32);
  ctx.r3.u64 = ModZeroEntrance(base, mgr, idx, ctx.r3.u32);
}
REX_EXTERN(__imp__sub_82756768);
REX_HOOK_RAW(sub_82756768) {
  const uint32_t mgr = ctx.r3.u32, idx = ctx.r4.u32;
  __imp__sub_82756768(ctx, base);
  const auto r = ctx.r3;
  if (const int n = EntranceForAttire(ctx, base, mgr, idx, false); n > 0) ctx.r3.u64 = uint32_t(n);
  else ctx.r3 = r;
  ctx.r3.u64 = svr2011::EntranceFallback(base, mgr, idx, -1, ctx.r3.u32);
  ctx.r3.u64 = ModZeroEntrance(base, mgr, idx, ctx.r3.u32);
}

// Victory scenes (docs/VICTORY_SCENES.md): after the match the winner's scene is a script EVP? n from
// pac\evt\Winning*.pac, queued in the entrance queue (*(0x82E3DDFC) + 5872) at +416 and started by
// sub_8275DEE0: 6000 + id for the 26 superstars who have their own (6139 John Cena), else a generic one (7000-7107
// the singles and divas ones; tag, trio, manager, title and match-type scenes are 7150+ and 8000+). A mod's
// victory= (a superstar mod, or an attire mod for that attire, with or without a model) replaces a singles scene
// with any scene that exists - another superstar's, a generic one or a pack's own (pacentry EVPx <n>).
namespace {
constexpr uint32_t kQueue = 5872, kVictoryAt = 416, kFighters = 0x82E3CC50, kPerson = 1156, kLost = 447,
                   kBeatenBy = 448;
int VictoryOfWinner(const uint8_t* base, uint32_t table, uint32_t* key_out) {
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t c = Rd32(base + kFighters + i * 4);
    if (!c || !base[c + kLost]) continue;
    const uint32_t person = base[c + kBeatenBy];
    if (person >= 16) return -1;
    const uint32_t key = Rd32(base + table + kSlotsAt + person * kSlotSize + kSlotKey), attire = key / 10 % 10;
    *key_out = key;
    if (const Mod* m = ModOf(key / 100)) return m->victory;
    if (const AttireMod* am = AttireModOf(key / 100, attire); am && am->victory > 0) return am->victory;
    const auto it = g_attire_victory_only.find({key / 100, attire});
    return it != g_attire_victory_only.end() ? it->second : -1;
  }
  return -1;
}
}  // namespace
REX_EXTERN(__imp__sub_8275DEE0);
REX_HOOK_RAW(sub_8275DEE0) {
  if (const uint32_t table = Rd32(base + kSlotTable)) {
    uint8_t* at = base + table + kQueue + kVictoryAt;
    const uint32_t now = Rd32(at);
    uint32_t key = 0;
    const int want = now >= 6000 && now <= 7107 ? VictoryOfWinner(base, table, &key) : -1;
    if (now >= 6000 && now <= 7107)
      REXLOG_INFO("[svr2011] victory: scene {} for superstar {} attire {}", now, key / 100, key / 10 % 10 + 1);
    if (want > 0 && uint32_t(want) != now) {
      if (svr2011::EntranceScriptExists(want)) {
        Wr32(at, uint32_t(want));
        REXLOG_INFO("[svr2011] victory: superstar {} attire {}: scene {} instead of {}", key / 100, key / 10 % 10 + 1, want, now);
      } else {
        REXLOG_WARN("[svr2011] victory: superstar {}: victory={} has no script - the game's {} plays", key / 100, want, now);
      }
    }
  }
  __imp__sub_8275DEE0(ctx, base);
}

// Audio events by name (sub_82BEC030(name, ...)): a mod's own announcer
// event ("Play_RA_xx_MOD_<id>_<category>", WriteCallNames) plays its
// recorded name instead (the game's sound engine has no such event: silent).
REX_EXTERN(__imp__sub_82BEC030);
REX_HOOK_RAW(sub_82BEC030) {
  if (ctx.r3.u32 >= 0x10000) {
    const char* e = reinterpret_cast<const char*>(base + ctx.r3.u32);
    // Entrance music with a live vocal: R-Truth's entrance posts
    // "Play_MUS_0272_2_0_RTruthVO" (Music.pck: two streamed sounds - the
    // theme, as "Play_MUS_0272_2_0" plays it, and his vocal), then at once
    // "Pause_start" on the same object and "Pause_Resume" a moment later. The
    // theme's stream is still opening when the pause comes and stays paused:
    // only the vocal played (B20; level 0.04 = crowd only, 0.07-0.08 with the
    // theme). The pause right after such an event is not passed on (the
    // pause/resume pair is a fraction of a second; a resume of a sound that
    // isn't paused does nothing): theme and vocal both play.
    static uint32_t vo_object = 0;
    static auto vo_at = std::chrono::steady_clock::time_point{};
    if (!std::strncmp(e, "Play_MUS_", 9)) {
      const size_t n = std::strlen(e);
      vo_object = n > 3 && !std::strcmp(e + n - 2, "VO") ? ctx.r4.u32 : 0;
      vo_at = std::chrono::steady_clock::now();
    } else if (vo_object && ctx.r4.u32 == vo_object && !std::strcmp(e, "Pause_start")) {
      vo_object = 0;
      if (std::chrono::steady_clock::now() - vo_at < std::chrono::milliseconds(500)) {
        REXLOG_INFO("[svr2011] entrance music: the pause right after the music with a vocal is not passed on");
        ctx.r3.u64 = 0x7E000002u;  // (a playing id: not 0)
        return;
      }
    }
    uint32_t id = 0;
    if (svr2011::JukeboxEvent(base, e, ctx.r4.u32)) {  // (jukebox.h: every menu song off)
      ctx.r3.u64 = 0x7E000001u;  // (a playing id: not 0)
      return;
    }
    if (svr2011::MediaEvent(base, e, ctx.r4.u32, &id)) {  // (a media mod's sound instead: media_mods.h)
      ctx.r3.u64 = id;
      return;
    }
    if (!std::strncmp(e, "Play_", 5)) {
      if (NameCallLog()) REXLOG_INFO("[svr2011] name call: event {:.60}", e);
      if (!std::strncmp(e, "Play_RA_", 8))
        if (const char* mod = std::strstr(e, "_MOD_"))
          if (const Mod* m = ModOf(uint32_t(std::atoi(mod + 5))); m && !m->voice.empty()) {
            // The game's own calls play 7 dB down: RA.pck's one actor-mixer (1AB691A1), every RA sound's parent,
            // has a volume of -7.0. A mod's call (its own player, at the file's level) the same, then call_gain=.
            constexpr float kGameCalls = -7.0f;
            REXLOG_INFO("[svr2011] superstar mods: {}: name call {} at {:.1f} dB (the game's calls' {:.0f}{})", m->name,
                        m->voice.filename().string(), kGameCalls + m->call_gain, kGameCalls,
                        m->call_gain ? fmt::format(", call_gain {:+.1f}", m->call_gain) : std::string());
            svr2011::PlayClip(m->voice, kGameCalls + m->call_gain);
          }
    }
  }
  const bool log_ra = NameCallLog() && ctx.r3.u32 >= 0x10000 &&
                      !std::strncmp(reinterpret_cast<const char*>(base + ctx.r3.u32), "Play_RA_", 8);
  std::string ev = log_ra ? std::string(reinterpret_cast<const char*>(base + ctx.r3.u32), 0, 60) : "";
  __imp__sub_82BEC030(ctx, base);
  if (log_ra) REXLOG_INFO("[svr2011] name call: {} -> playing id {}", ev, ctx.r3.u32);
}
REX_EXTERN(__imp__sub_825FDCD8);
REX_HOOK_RAW(sub_825FDCD8) {
  if (const Mod* m = ModOfCommentary(ctx.r3.u32)) {
    if (m->call >= 0) {  // (a nickname: the CAS_* banks all have commentary)
      ctx.r3.u64 = 1;
      return;
    }
    ctx.r3.u64 = m->base;  // (as the base would be)
  }
  __imp__sub_825FDCD8(ctx, base);
}

// ---- Attires
// How many attires a character has, and each one's name and unlock, come from
// misc.pac's COS table (BATS/INIT entry 5, by character id; 80 records, most
// taken): the mods aren't in it, so the select screen offered no CHANGE
// ATTIRE. Its lookups answer for them instead: sub_828C4140(mgr, id, mode)
// the count, sub_828C4060(mgr, id, attire) the {name string id, unlock}
// pair (unlock -1: always; sub_828C2240's "available" reads it).
REX_EXTERN(__imp__sub_828C4140);
REX_HOOK_RAW(sub_828C4140) {
  if (const Mod* m = ModOf(ctx.r4.u32); m && m->attires > 1 && m->attire_pairs) {
    ctx.r3.u64 = m->attires;
    return;
  }
  const uint32_t id = ctx.r4.u32, mode = ctx.r5.u32;
  __imp__sub_828C4140(ctx, base);
  const uint32_t game = ctx.r3.u32;
  if (g_attire_pairs.count(id)) {  // (attire mods: up to the last attire one of them gives)
    uint32_t n = ctx.r3.u32;
    for (const auto& a : g_attire_mods)
      if (a.id == id) n = std::max(n, a.attire + 1);
    ctx.r3.u64 = std::min(n, 4u);
  }
  static const bool log = std::getenv("SVR2011_TEST_ATTIRE_LOG") != nullptr;  // (test aid: the count asked for)
  if (log)
    REXLOG_INFO("[svr2011] attire count: id {} mode {} -> {} (the game's {}, lr {:08X})", id, mode, ctx.r3.u32, game,
                uint32_t(ctx.lr));
}
REX_EXTERN(__imp__sub_828C4060);
REX_HOOK_RAW(sub_828C4060) {
  if (const Mod* m = ModOf(ctx.r4.u32); m && m->attires > 1 && m->attire_pairs) {
    ctx.r3.u64 = ctx.r5.u32 < m->attires ? m->attire_pairs + ctx.r5.u32 * 8 : 0;
    return;
  }
  const uint32_t id = ctx.r4.u32, attire = ctx.r5.u32;
  const auto it = g_attire_pairs.find(id);
  if (it != g_attire_pairs.end() && attire < 4 && AttireModOf(id, attire)) {
    ctx.r3.u64 = it->second + attire * 8;
    return;
  }
  __imp__sub_828C4060(ctx, base);
  // (a superstar the table has no record for - the DLC superstars 51-58: the count's 0 - with attire mods: the
  // others are ORIGINAL ATTIRE, always available, as the port's table has them - else the select screen counted
  // only the mods' attires as available and offered no CHANGE ATTIRE)
  if (!ctx.r3.u32 && it != g_attire_pairs.end() && attire < 4) ctx.r3.u64 = it->second + attire * 8;
  static const bool log = std::getenv("SVR2011_TEST_ATTIRE_LOG") != nullptr;
  if (log) REXLOG_INFO("[svr2011] attire pair: id {} attire {} -> {:08X}", id, attire, ctx.r3.u32);
}

// ---- The select screen's model framing
// Where the panel's 3D model stands comes from a placement table by character
// id (DLC_HD/SD.pac MMDL/BIND "MMPS": per layout {id, dx, dy, dz}; scratchpad
// re_framing). Ids not in it get no offsets: a mod stood too high, its head
// cut off at the top. sub_8273D1D0(mgr, slot, layout, out) looks the slot's
// id up (slot = mgr + slot * 116: the id at +64 when +20 == 1, else +44): for
// a mod, its base's. An id the table doesn't have (the managers - Stephanie,
// Teddy Long, Paul Bearer, Tiffany, The Hurricane, Hornswoggle - floated in
// the EXTRA list; a mod whose base is one of them too) takes the entry of a
// listed character of the same gender and the nearest height (record +28).
uint32_t FramingStandIn(uint8_t* base, uint32_t mgr, uint32_t layout, uint32_t id) {
  const auto rec = [&](uint32_t i) -> const uint8_t* {
    const uint32_t si = i < 512 ? svr2011::RosterIndex(base, i) : 0xFFFF;
    return si < 512 ? base + svr2011::RosterEntry(kRecords, kRecordSize, si) : nullptr;
  };
  const uint8_t* r = rec(id);
  if (!r) return 0;
  const uint32_t count = Rd32(base + mgr + 788 + 44 * layout);
  const uint32_t entries = Rd32(base + mgr + 792 + 44 * layout);
  if (!entries || count > 1024) return 0;
  uint32_t best = 0, best_d = ~0u;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t c = Rd32(base + entries + i * 16);
    const uint8_t* cr = c >= 100 ? rec(c) : nullptr;  // (disc superstars; 50-69 are DLC/placeholders)
    if (!cr || cr[208] != r[208]) continue;
    const uint32_t d = uint32_t(std::abs(int(Rd16(cr + 28)) - int(Rd16(r + 28))));
    if (d < best_d) best = c, best_d = d;
  }
  return best;
}
REX_EXTERN(__imp__sub_8273D1D0);
REX_HOOK_RAW(sub_8273D1D0) {
  const uint32_t mgr = ctx.r3.u32, slot = mgr + ctx.r4.u32 * 116, layout = ctx.r5.u32;
  const uint32_t field = slot + (Rd32(base + slot + 20) == 1 ? 64 : 44);
  const uint32_t id = Rd32(base + field);
  const Mod* m = ModOf(id);
  const uint32_t look = m ? m->base : id;
  const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  Wr32(base + field, look);
  __imp__sub_8273D1D0(ctx, base);
  if (ctx.r3.u32 == 0 && look >= 50 && layout < 14) {
    if (const uint32_t s = FramingStandIn(base, mgr, layout, look)) {
      static std::set<uint32_t> told;
      if (told.insert(look).second) REXLOG_INFO("[svr2011] select framing: id {} uses {}'s", look, s);
      ctx.r3.u64 = mgr, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
      Wr32(base + field, s);
      __imp__sub_8273D1D0(ctx, base);
    }
  }
  Wr32(base + field, id);
}

// ---- The select / VS screen pose
// sub_8273D4A0(mgr, slot) gives the motion the panel's model plays: the
// slot's id (as for the framing above) looked up in {id, motion} pairs
// (mgr+1392, count mgr+1380; motion -1 = none); not found, the next motion of
// a rotating list (mgr+1388 / +1376 when sub_828B6950() == 1, else mgr+1384 /
// +1372); no lists, 32500. A mod isn't in the pairs, so it took a rotating
// pose: now its base's, or the manifest's pose= (a superstar id: that
// superstar's pose; else a motion id itself).
uint32_t PoseOf(uint8_t* base, uint32_t mgr, uint32_t id) {
  const uint32_t pairs = Rd32(base + mgr + 1392), count = Rd32(base + mgr + 1380);
  if (!pairs || count > 4096) return 0;
  for (uint32_t i = 0; i < count; ++i)
    if (Rd32(base + pairs + i * 8) == id && Rd32(base + pairs + i * 8 + 4) != 0xFFFFFFFFu)
      return Rd32(base + pairs + i * 8 + 4);
  return 0;
}
// ---- A Created Superstar's Menu Screen Pose (report 123925)
// The editor's page keeps one byte per attire in the .cas and previews it with motion 1010, x = pose * 2 (+ 1 for a
// diva: the bank has 18 poses, the odd x only for the first 9), from pac\edit\edit.pac's EDIT/MOT. The game reads the
// byte nowhere else: a CAW on the select screen isn't in the pairs above and takes a rotating pose. Now, when a CAW has
// a pose set, the select screen also loads EDIT/MOT (its own loader, as the MMDL/MOTN one) and that CAW's panel plays
// it. Byte 0 is the first pose and also "never set": those keep the rotating poses.
constexpr uint32_t kCawBlocks = 0xB6364, kCawBlock = 0x24818, kCawPoseByte = 0x24814;
constexpr uint32_t kCawPoseMotion = 1010;
uint32_t g_pose_bank = 0;                               // (guest: the loader of EDIT/MOT, while it's loaded)
int g_panel_pose_x[6] = {-1, -1, -1, -1, -1, -1};       // (per panel: the x of its pose)

int CawPose(uint32_t id, uint32_t attire) {
  if (id >= 50 || attire >= 4 || g_save_file.empty()) return 0;
  // (test aid SVR2011_TEST_CAW_POSE=<id>=<pose>[,...]: that CAW's pose, whatever its file holds)
  static const std::string test = svr2011::Env("SVR2011_TEST_CAW_POSE");
  for (size_t at = 0; at < test.size();) {
    size_t end = test.find(',', at);
    if (end == std::string::npos) end = test.size();
    const std::string one = test.substr(at, end - at);
    if (const size_t eq = one.find('='); eq != std::string::npos && uint32_t(std::atoi(one.c_str())) == id)
      return std::atoi(one.c_str() + eq + 1);
    at = end + 1;
  }
  char name[32];
  std::snprintf(name, sizeof name, "%02uCreateSuperStar.cas", id);
  std::ifstream in(g_save_file.parent_path() / name, std::ios::binary);
  if (!in) return 0;
  in.seekg(kCawBlocks + attire * kCawBlock + kCawPoseByte);
  char pose = 0;
  if (!in.get(pose)) return 0;
  return uint8_t(pose) < 18 ? uint8_t(pose) : 0;
}

bool AnyCawPose() {
  for (uint32_t id = 0; id < 50; ++id)
    for (uint32_t attire = 0; attire < 4; ++attire)
      if (CawPose(id, attire)) return true;
  return false;
}

void FreePoseBank(PPCContext& ctx, uint8_t* base) {
  if (!g_pose_bank) return;
  const uint32_t loader = g_pose_bank;
  g_pose_bank = 0;
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  ctx.r3.u64 = loader;
  sub_8274D0D8(ctx, base);
  ctx.r3.u64 = loader, ctx.r4.u64 = 1;
  sub_8269B2D0(ctx, base);
  ctx = saved;
  REXLOG_INFO("[svr2011] select pose: EDIT/MOT freed");
}

// (as sub_8273F5C0 asks for MMDL/MOTN, in the select screen's heap)
void LoadPoseBank(PPCContext& ctx, uint8_t* base, uint32_t mgr) {
  const auto saved = ctx;
  const uint32_t sp = saved.r1.u32 - 0x300, name = sp + 96, args = sp + 128;
  ctx.r1.u64 = sp;
  sub_8269A478(ctx, base);
  const uint32_t was = ctx.r3.u32;
  ctx.r3.u64 = Rd32(base + mgr + 712);
  sub_8269AC60(ctx, base);
  ctx.r3.u64 = 80;
  sub_82150000(ctx, base);
  if (const uint32_t loader = ctx.r3.u32) {
    ctx.r3.u64 = loader;
    sub_8274D098(ctx, base);
    std::memset(base + name, 0, 20);
    std::memcpy(base + name, "/EDIT/MOT", 9);
    ctx.r3.u64 = args;
    sub_8273E9A0(ctx, base);
    ctx.r3.u64 = args, ctx.r4.u64 = 32, ctx.r5.u64 = name, ctx.r6.u64 = 20;
    sub_826A0468(ctx, base);
    sub_825743A8(ctx, base);
    Wr32(base + args + 32, ctx.r3.u32);
    Wr32(base + args + 36, 1);
    Wr32(base + args + 40, 0);
    Wr32(base + args + 44, 0);
    Wr32(base + args + 48, 1);
    Wr32(base + args + 52, 0);
    Wr32(base + args + 56, 0);
    Wr32(base + args + 60, 99);
    ctx.r3.u64 = loader, ctx.r4.u64 = args;
    sub_8274D7A0(ctx, base);
    g_pose_bank = loader;
    REXLOG_INFO("[svr2011] select pose: a Created Superstar has a Menu Screen Pose - EDIT/MOT loaded");
  }
  ctx.r3.u64 = was;
  sub_8269AC60(ctx, base);
  ctx = saved;
}

REX_EXTERN(__imp__sub_8273F5C0);
REX_HOOK_RAW(sub_8273F5C0) {
  const uint32_t mgr = ctx.r3.u32, before = Rd32(base + mgr + 728);
  __imp__sub_8273F5C0(ctx, base);
  if (Rd32(base + mgr + 728) == before) return;  // (the screen's load didn't start on this call)
  const auto r3 = ctx.r3.u64;
  FreePoseBank(ctx, base);
  if (AnyCawPose()) LoadPoseBank(ctx, base, mgr);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_8273D758);
REX_HOOK_RAW(sub_8273D758) {
  FreePoseBank(ctx, base);
  std::fill(std::begin(g_panel_pose_x), std::end(g_panel_pose_x), -1);
  __imp__sub_8273D758(ctx, base);
}
// A panel's motion starts here (r5 motion, r6 x); a pose put off to later is in the panel's +120 for sub_8273DF80.
REX_EXTERN(__imp__sub_8273D978);
REX_HOOK_RAW(sub_8273D978) {
  if (ctx.r5.u32 == kCawPoseMotion && ctx.r4.u32 < 6 && g_panel_pose_x[ctx.r4.u32] >= 0)
    ctx.r6.u64 = uint32_t(g_panel_pose_x[ctx.r4.u32]);
  __imp__sub_8273D978(ctx, base);
}
// (r3 the screen, r4 the panel) A panel's first motion starts here, with x 0 or the +120 sub_8273D978 put off: while it
// runs, motion 1010 is that panel's pose - in the check that it's there and in the start.
int g_starting_pose_x = -1;
REX_EXTERN(__imp__sub_8238C808);
REX_HOOK_RAW(sub_8238C808) {
  if (g_starting_pose_x >= 0 && ctx.r4.u32 == kCawPoseMotion) ctx.r5.u64 = uint32_t(g_starting_pose_x);
  __imp__sub_8238C808(ctx, base);
}
REX_EXTERN(__imp__sub_82396E80);
REX_HOOK_RAW(sub_82396E80) {
  if (g_starting_pose_x >= 0 && ctx.r4.u32 == kCawPoseMotion) ctx.r5.u64 = uint32_t(g_starting_pose_x);
  __imp__sub_82396E80(ctx, base);
}
REX_EXTERN(__imp__sub_8273DF80);
REX_HOOK_RAW(sub_8273DF80) {
  g_starting_pose_x = ctx.r4.u32 < 6 ? g_panel_pose_x[ctx.r4.u32] : -1;
  __imp__sub_8273DF80(ctx, base);
  g_starting_pose_x = -1;
}

REX_EXTERN(__imp__sub_8273D4A0);
REX_HOOK_RAW(sub_8273D4A0) {
  const uint32_t mgr = ctx.r3.u32, slot = mgr + ctx.r4.u32 * 116;
  const uint32_t id = Rd32(base + slot + (Rd32(base + slot + 20) == 1 ? 64 : 44));
  static bool listed = false;
  if (!listed && std::getenv("SVR2011_TEST_POSE_LOG")) {
    listed = true;
    const uint32_t pairs = Rd32(base + mgr + 1392), count = Rd32(base + mgr + 1380);
    std::string t;
    for (uint32_t i = 0; pairs && i < count && i < 4096; ++i)
      t += fmt::format(" {}:{}", Rd32(base + pairs + i * 8), int32_t(Rd32(base + pairs + i * 8 + 4)));
    REXLOG_INFO("[svr2011] select pose: {} pairs:{}", count, t);
    for (int k = 0; k < 2; ++k) {
      const uint32_t list = Rd32(base + mgr + (k ? 1388 : 1384)), n = Rd32(base + mgr + (k ? 1376 : 1372));
      std::string u;
      for (uint32_t i = 0; list && i < n && i < 256; ++i) u += fmt::format(" {}", Rd32(base + list + i * 4));
      REXLOG_INFO("[svr2011] select pose: rotating list {} ({}):{}", k, n, u);
    }
  }
  // (test aid SVR2011_TEST_POSE_AS=<id>=<mod id>: that superstar's panel
  // takes the mod's pose - the select cursor starts on a disc superstar)
  uint32_t as = id;
  if (const char* v = std::getenv("SVR2011_TEST_POSE_AS"); v && uint32_t(std::atoi(v)) == id)
    if (const char* q = std::strchr(v, '=')) as = uint32_t(std::atoi(q + 1));
  // (a Created Superstar with a pose set, once its bank has a slot; else as any CAW)
  if (ctx.r4.u32 < 6) g_panel_pose_x[ctx.r4.u32] = -1;
  if (id < 50 && ctx.r4.u32 < 6 && g_pose_bank && Rd32(base + g_pose_bank + 44) != 0xFFFFFFFFu) {
    const uint32_t which = Rd32(base + slot + 20) == 1 ? 64 : 44;
    if (const int pose = CawPose(id, Rd32(base + slot + which + 4))) {
      const uint32_t panel = ctx.r4.u32;
      const auto saved = ctx;
      ctx.r1.u64 = saved.r1.u32 - 0x100;
      ctx.r3.u64 = id;
      sub_828B6950(ctx, base);
      const bool diva = ctx.r3.u32 == 1;
      ctx = saved;
      g_panel_pose_x[panel] = pose * 2 + (diva && pose < 9 ? 1 : 0);
      static std::set<uint32_t> told_caw;
      if (told_caw.insert(id << 8 | uint32_t(pose)).second)
        REXLOG_INFO("[svr2011] select pose: Created Superstar {} attire {} plays its Menu Screen Pose {} (x {})", id,
                    Rd32(base + slot + which + 4), pose, g_panel_pose_x[panel]);
      ctx.r3.u64 = kCawPoseMotion;
      return;
    }
  }
  const Mod* m = ModOf(as);
  uint32_t motion = 0;
  if (m) motion = m->pose >= 512 ? m->pose : PoseOf(base, mgr, m->pose ? m->pose : m->base);
  if (!motion) {
    __imp__sub_8273D4A0(ctx, base);
    if (std::getenv("SVR2011_TEST_POSE_LOG")) REXLOG_INFO("[svr2011] select pose: id {} -> {}", id, ctx.r3.u32);
    return;
  }
  static std::set<uint32_t> told;
  if (told.insert(id << 16 ^ motion).second)
    REXLOG_INFO("[svr2011] select pose: mod id {} plays {}", as, motion);
  ctx.r3.u64 = motion;
}

// The file system's pacs registered (from the ARC / from each pac's header).
REX_EXTERN(__imp__sub_825953B0);
REX_HOOK_RAW(sub_825953B0) {
  __imp__sub_825953B0(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  Mount(ctx, base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82595428);
REX_HOOK_RAW(sub_82595428) {
  // (a pac overlay - move packs: mount the pacs by reading each one's own
  // table - sub_825953B0, which mounts ours too - not the pre-built directory)
  if (!svr2011::PacListFolder().empty()) {
    REXLOG_INFO("[svr2011] pac list: mounting from the pacs' own tables");
    sub_825953B0(ctx, base);
    return;
  }
  __imp__sub_82595428(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  Mount(ctx, base);
  ctx.r3.u64 = r3;
}

// The roster records loaded (CHAR/DAT), the profiles (CHAR/PRO), a save loaded.
REX_EXTERN(__imp__sub_82B89E50);
REX_HOOK_RAW(sub_82B89E50) {
  Remember(base);
  __imp__sub_82B89E50(ctx, base);
  const auto r3 = ctx.r3.u64;
  RecordStubs(base);  // (per-mod saves: each mod slot as the game loads it, before ApplyRecords)
  // Test aid: SVR2011_TEST_SLOT_HELD=<mod id> - once the slot has a kept record
  // (a later load: a save), it gets what a save from before a mod swap brings:
  // another mod's record name and a stock superstar's profile (id 100's).
  // Logs whether that profile is still there after ApplyRecords.
  static const int held = std::getenv("SVR2011_TEST_SLOT_HELD") ? std::atoi(std::getenv("SVR2011_TEST_SLOT_HELD")) : -1;
  static bool held_done = false;
  uint8_t* held_sp = nullptr;
  svrfmt::Bytes held_profile;
  if (held >= 0 && !held_done) {
    const uint32_t si = svr2011::RosterIndex(base, uint32_t(held)), oi = svr2011::RosterIndex(base, 100);
    bool kept = false;
    for (const auto& k : g_kept) kept |= k.slot == uint32_t(held) && !k.record.empty();
    if (kept && si < 512 && oi < 512) {
      held_done = true;
      PutName(base + svr2011::RosterEntry(kRecords, kRecordSize, si), kFullName, "OLD MOD");
      held_sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
      std::memcpy(held_sp, base + svr2011::RosterEntry(kProfiles, kProfileSize, oi), kProfileSize);
      held_profile.assign(held_sp, held_sp + kProfileSize);
      REXLOG_INFO("[svr2011] superstar mods: test - id {} given \"OLD MOD\"'s record and id 100's profile", held);
    }
  }
  ApplyRecords(base);
  if (held_sp)
    REXLOG_INFO("[svr2011] superstar mods: test - id {} after the load: profile {}, name \"{}\"", held,
                std::memcmp(held_sp, held_profile.data(), kProfileSize) ? "its own" : "STILL THE OLD MOD'S",
                reinterpret_cast<const char*>(base + svr2011::RosterEntry(kRecords, kRecordSize, svr2011::RosterIndex(base, uint32_t(held))) + kFullName));
  ctx.r3.u64 = r3;
}
bool g_pro_load_ran = false;  // (sub_82594C78 ran inside sub_82B89DC0)
REX_EXTERN(__imp__sub_82594C78);
REX_HOOK_RAW(sub_82594C78) {
  g_pro_load_ran = true;
  Remember(base);
  std::vector<svrfmt::Bytes> before;  // (the mods' slots before the load: RecordBlanks)
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    before.push_back(si < 512 ? svrfmt::Bytes(base + svr2011::RosterEntry(kProfiles, kProfileSize, si),
                                              base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kProfileSize)
                              : svrfmt::Bytes());
  }
  __imp__sub_82594C78(ctx, base);
  const auto r3 = ctx.r3.u64;
  KeepBlankProfiles(base);
  RecordBlanks(base, before);
  KeepPoolPlaceholders(base);  // (the save cleaner's reference: before ApplyRecords sets the mods)
  ApplyRecords(base);
  // test aid: SVR2011_TEST_PRO_LOG=1 - each mod slot's entrance after the load
  // (music, movie, entrance number; a placeholder reads 255 255 4000)
  if (std::getenv("SVR2011_TEST_PRO_LOG"))
    for (const auto& m : g_mods) {
      const uint32_t si = svr2011::RosterIndex(base, m.slot);
      if (si >= 512) continue;
      const uint8_t* e = base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kEntrance;
      REXLOG_INFO("[svr2011] superstar mods: CHAR/PRO loaded: id {} music {} movie {} entrance {}", m.slot,
                  Rd16(e + kMusic), Rd16(e + kMovie), Rd16(e + 0x14));
    }
  ctx.r3.u64 = r3;
}
// The DLC manager's restore after a DLC scan (sub_8259CAA8, its backup made
// by sub_8259C9A8): writes the DLC ids' records and profiles back - for a
// mod on 59-69 the placeholder - after the port set them up. Same as after a
// CHAR/PRO load: the new placeholder version is recorded, the mod's put back.
REX_EXTERN(__imp__sub_8259CAA8);
REX_HOOK_RAW(sub_8259CAA8) {
  Remember(base);
  std::vector<svrfmt::Bytes> before;
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    before.push_back(si < 512 ? svrfmt::Bytes(base + svr2011::RosterEntry(kProfiles, kProfileSize, si),
                                              base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kProfileSize)
                              : svrfmt::Bytes());
  }
  __imp__sub_8259CAA8(ctx, base);
  const auto r3 = ctx.r3.u64;
  if (std::getenv("SVR2011_TEST_PRO_LOG")) REXLOG_INFO("[svr2011] superstar mods: DLC restore (sub_8259CAA8)");
  RecordBlanks(base, before);
  KeepPoolPlaceholders(base);
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
// The CHAR/PRO loader's callback (sub_82B89DC0). For a DLC's chEtc (DLC
// Vol03 at each DLC scan) it writes the DLC ids' profiles - placeholders for
// a mod on 59-69 - without reaching sub_82594C78: then, as after a CHAR/PRO
// load, the new placeholder version is recorded and the mods put back. (When
// sub_82594C78 ran, its hook did that.)
REX_EXTERN(__imp__sub_82B89DC0);
REX_HOOK_RAW(sub_82B89DC0) {
  std::vector<svrfmt::Bytes> before;
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    before.push_back(si < 512 ? svrfmt::Bytes(base + svr2011::RosterEntry(kProfiles, kProfileSize, si),
                                              base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kProfileSize)
                              : svrfmt::Bytes());
  }
  g_pro_load_ran = false;
  __imp__sub_82B89DC0(ctx, base);
  const auto r3 = ctx.r3.u64;
  if (!g_pro_load_ran) {
    if (std::getenv("SVR2011_TEST_PRO_LOG"))
      REXLOG_INFO("[svr2011] superstar mods: CHAR/PRO written without a load (a DLC's chEtc)");
    RecordBlanks(base, before);
    KeepPoolPlaceholders(base);
    ApplyRecords(base);
  }
  RedoHandovers(base, false);  // (a handed-over slot's saved profile, if the loader wrote it by now)
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_8259C9A8);
REX_HOOK_RAW(sub_8259C9A8) {
  if (std::getenv("SVR2011_TEST_PRO_LOG")) REXLOG_INFO("[svr2011] superstar mods: DLC backup (sub_8259C9A8)");
  __imp__sub_8259C9A8(ctx, base);
}
REX_EXTERN(__imp__sub_8257A218);
REX_HOOK_RAW(sub_8257A218) {
  g_save_loaded = true;  // (KeepPoolPlaceholders: the slots hold the save's data from here)
  __imp__sub_8257A218(ctx, base);
  const auto r3 = ctx.r3.u64;
  TempFromSave(base);  // (the fetched mods' ids as this save has them: from its image, before they're written)
  CheckSavedProfiles(base);  // (slots whose saved profile is another mod's: handed over by ApplyRecords)
  ApplyRecords(base);
  svr2011::PerModSavesLoaded(base);  // (key refs: the save's teams / titles / cards naming a mod)
  svr2011::BigRosterTestCard(base);
  g_moves_checked = false;  // (the save's profiles - its kept edits - get CheckModProfiles' move check too)
  svr2011::UniverseModsLoaded(base);  // (the mods' Universe side table for this save)
  svr2011::NewTitlesLoaded(base);  // (new titles' holders and cards: title_new.h)
  svr2011::NewTitlesTestCheck(ctx, base);
  ctx.r3.u64 = r3;
}

// WWE Universe's title holders (14 titles x 2 u16 ids at save + 0x7CD70) take mod ids as they are: a mod has its
// Universe entry (universe_mods.cpp), so a title it holds is booked like any other (47438dc kept mods off titles
// while they had none).

// Once a frame (the game's pad reads, frame_rate.cpp): a mod's profile that
// is a placeholder again gets the mod's back. A save's copy of a slot can be
// the placeholder (saved before the mod was installed): loading it put Jeff
// Hardy's back to it after the hooks above ran, and CREATE A MOVESET - no
// roster list (below) on the way - read the placeholder's finisher 15271, a
// move that doesn't exist (crash, 2.0.3). A save's edited copy is no
// placeholder and stays.
namespace svr2011 {
// A mod slot's move-set as the CPU and the player use it: the u16 move ids below the entrance block. Usable: ones
// the game has a record with motions for (MoveUsable). A profile copied from a base that is a blank placeholder,
// or a save's broken copy, has few or none: the CPU (and the player) got stuck in matches (player report, 20 Oct).
struct MovesetCount {
  int set = 0, usable = 0;
};
MovesetCount CountMoves(const uint8_t* profile) {
  MovesetCount c;
  for (uint32_t off = 0; off + 1 < kEntrance; off += 2) {
    const uint16_t id = Rd16(profile + off);
    if (!id || id == 0xFFFF) continue;
    ++c.set;
    c.usable += svr2011::MoveUsable(id) == 1;
  }
  return c;
}
bool MovesetWorks(const MovesetCount& c) { return c.usable >= kMinUsableMoves; }

// Fail-safe: a mod slot whose move-set doesn't work gets a stock superstar's whole move-set (one of the mod's
// gender - record +208 - picked by the mod's folder name: the same one every run), then the mod's own usable
// moves on top - never a stuck match.
void EnsureMoveset(uint8_t* base, uint8_t* sp, const Mod& m) {
  const MovesetCount now = CountMoves(sp);
  if (MovesetWorks(now)) return;
  const uint32_t mi = svr2011::RosterIndex(base, m.slot);
  const int gender = mi < 512 ? base[svr2011::RosterEntry(kRecords, kRecordSize, mi) + 208] : -1;
  std::vector<uint32_t> donors;
  for (uint32_t id = 100; id <= 321; ++id) {
    if (ModOf(id) || std::find(std::begin(kPool), std::end(kPool), id) != std::end(kPool)) continue;
    const uint32_t i = svr2011::RosterIndex(base, id);
    if (i >= 512 || !base[svr2011::RosterEntry(kRecords, kRecordSize, i) + kFullName]) continue;
    if (gender >= 0 && base[svr2011::RosterEntry(kRecords, kRecordSize, i) + 208] != gender) continue;
    if (MovesetWorks(CountMoves(base + svr2011::RosterEntry(kProfiles, kProfileSize, i)))) donors.push_back(id);
  }
  if (donors.empty()) {
    REXLOG_WARN("[svr2011] superstar mods: {}: its move-set doesn't work ({} of {} moves usable) and no stock one "
                "was found to give it", m.folder, now.usable, now.set);
    return;
  }
  uint32_t h = 2166136261u;
  for (char c : m.folder) h = (h ^ uint8_t(c)) * 16777619u;
  const uint32_t donor = donors[h % donors.size()];
  const uint8_t* dp = base + svr2011::RosterEntry(kProfiles, kProfileSize, svr2011::RosterIndex(base, donor));
  std::memcpy(sp, dp, kEntrance);  // (the move block only: below the entrance)
  for (const auto& [off, id] : m.moves)
    if (off + 1u < kEntrance && svr2011::MoveUsable(id) == 1) sp[off] = uint8_t(id >> 8), sp[off + 1] = uint8_t(id);
  const MovesetCount after = CountMoves(sp);
  REXLOG_WARN("[svr2011] superstar mods: {}: its move-set didn't work ({} of {} moves usable) - superstar {}'s (gender {}) "
              "given instead, with the mod's own on top ({} usable now)", m.folder, now.usable, now.set, donor, gender, after.usable);
}

// The created entrances (CREATE AN ENTRANCE's ADVANCED CREATION) the profiles hold, for the Mod Maker's "Made in the
// game" (a mod's cae=): <game>\Mods\.cae\<id>.bin (the 284-byte block as the game holds it) and list.txt
// ("<id>\t<name>"). Only that folder is written - never a save or a DLC file; a file only when it changed.
void ExportCreatedEntrances(const uint8_t* base) {
  static uint32_t frames = 0;
  static std::string last;  // (what was written: the profiles' created entrances, compared in memory)
  if (++frames % 300) return;  // (every 5 s: a save loaded, an entrance made in the game)
  {
    const uint32_t ri = svr2011::RosterIndex(base, 100);  // (the profiles loaded)
    if (ri >= 512 || !CountMoves(base + svr2011::RosterEntry(kProfiles, kProfileSize, ri)).set) return;
  }
  std::string now;
  for (uint32_t id = 1; id < 512; ++id) {
    const uint32_t i = svr2011::RosterIndex(base, id);
    if (i < 512 && base[svr2011::RosterEntry(kProfiles, kProfileSize, i) + kEntrance + 0xC7])
      now.append(reinterpret_cast<const char*>(base + svr2011::RosterEntry(kProfiles, kProfileSize, i) + kEntrance), kCaeSize);
  }
  if (now == last) return;
  last = now;
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / ".cae";
  std::string list;
  std::set<std::string> keep = {"list.txt"}, seen;  // (seen: name + block - a copy slot of the same superstar once)
  for (uint32_t id = 1; id < 512; ++id) {
    const uint32_t i = svr2011::RosterIndex(base, id);
    if (i >= 512 || !base[svr2011::RosterEntry(kRecords, kRecordSize, i) + kFullName]) continue;
    const uint8_t* e = base + svr2011::RosterEntry(kProfiles, kProfileSize, i) + kEntrance;
    if (!e[0xC7]) continue;
    bool real = true;  // (each scene's motion: 0, or 10000 + 1000 * scene + an entrance)
    int parts = 0;
    for (uint32_t p = 0; p < 5; ++p) {
      const int n = e[kCaeFrom + 34 * p + 14] << 8 | e[kCaeFrom + 34 * p + 15];
      if (n && (n < int(10000 + 1000 * p) || n >= int(11000 + 1000 * p))) real = false;
      parts += n != 0;
    }
    if (!real || !parts) continue;
    char name[kNameLen + 1] = {};
    std::memcpy(name, base + svr2011::RosterEntry(kRecords, kRecordSize, i) + kFullName, kNameLen);
    if (!seen.insert(std::string(name) + std::string(reinterpret_cast<const char*>(e), kCaeSize)).second) continue;
    const std::string file = std::to_string(id) + ".bin";
    keep.insert(file);
    list += std::to_string(id) + "\t" + name + "\n";
    std::vector<uint8_t> have;
    if (std::ifstream in(dir / file, std::ios::binary); in) have.assign(std::istreambuf_iterator<char>(in), {});
    if (have.size() == kCaeSize && !std::memcmp(have.data(), e, kCaeSize)) continue;
    fs::create_directories(dir, ec);
    std::ofstream(dir / file, std::ios::binary).write(reinterpret_cast<const char*>(e), kCaeSize);
  }
  std::string old;
  if (std::ifstream in(dir / "list.txt", std::ios::binary); in) old.assign(std::istreambuf_iterator<char>(in), {});
  if (old != list && (!list.empty() || fs::exists(dir, ec))) {
    fs::create_directories(dir, ec);
    std::ofstream(dir / "list.txt", std::ios::binary) << list;
    REXLOG_INFO("[svr2011] superstar mods: created entrances for the Mod Maker: {} in {}",
                std::count(list.begin(), list.end(), '\n'), dir.string());
  }
  for (const auto& f : fs::directory_iterator(dir, ec))  // (ones gone from the profiles)
    if (!keep.count(f.path().filename().string())) fs::remove(f.path(), ec);
}

// Test aid SVR2011_TEST_SAVE_DUMP=<dir>: every 10 s, the save image in memory (0x812C0 bytes from 0x82E407C0: block A
// then block B, as the writer writes them) to <dir>\save_<n>.bin when it changed, and the mods' ids to <dir>\ids.txt -
// diffing the dumps finds the save's fields that hold a mod's id (superstar_threads.cpp's save-time dumps share the
// folder).
void SaveDumpTick(uint8_t* base) {
  static const char* dir = std::getenv("SVR2011_TEST_SAVE_DUMP");
  if (!dir) return;
  static auto next = std::chrono::steady_clock::now();
  static uint64_t last = 0;
  static int n = 0;
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(10);
  constexpr uint32_t kImage = 0x82E407C0, kImageSize = 0x812C0;
  const uint8_t* p = base + kImage;
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < kImageSize; ++i) h = (h ^ p[i]) * 1099511628211ull;
  if (h == last) return;
  last = h;
  const fs::path d(dir);
  std::error_code ec;
  fs::create_directories(d, ec);
  std::ofstream(d / ("save_" + std::to_string(n) + ".bin"), std::ios::binary)
      .write(reinterpret_cast<const char*>(p), kImageSize);
  std::string ids;
  {
    std::lock_guard lock(g_slots_mutex);
    for (const auto& m : g_mods) ids += m.folder + "\t" + std::to_string(m.slot) + "\t" + std::to_string(m.key) + "\n";
  }
  std::ofstream(d / "ids.txt", std::ios::binary) << ids;
  REXLOG_INFO("[svr2011] save dump: save_{}.bin", n);
  ++n;
}

void CheckModProfiles(uint8_t* base) {
  SaveDumpTick(base);
  ExportCreatedEntrances(base);
  {
    std::lock_guard lock(g_slots_mutex);
    RedoHandovers(base, true);  // (a slot handed over at a save load: the save's own write after it)
  }
  UniverseModsTick(base);  // (the game wrote its save: the mods' Universe side table beside it)
  MarksTick(base);  // (and which mod each slot's saved profile is: mod_profiles.txt)
  svr2011::StoryModsTick();  // (Story Designer's stories that name a mod: story_mods.cpp)
  UniverseTeamsTick(base);  // (Universe's teams and stables made / changed / dissolved: logged)
  svr2011::NewTitlesTick(base);  // (the title list's pages: title_new.cpp)
  UniverseWelcomeTick(base);  // (the Universe welcome accepted: kept as seen in the save file at once)
  std::lock_guard lock(g_slots_mutex);
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512) continue;
    bool have_blank = false;
    for (const auto& b : g_blank) have_blank |= b.first == m.slot;
    if (have_blank && IsBlank(m.slot, base + svr2011::RosterEntry(kProfiles, kProfileSize, si))) {
      ApplyRecords(base);
      g_moves_checked = false;  // (the profiles were set up again: checked again)
      return;
    }
  }
  // Once the move records are loaded: a mod's own move the game has no usable
  // record for (SetOwnMoves may have run before they were) goes back to the
  // base's - only where the slot still holds the mod's id; an edit only when it
  // holds a move the game has no record for (then the mod's own).
  bool& checked = g_moves_checked;
  if (checked || MoveUsable(0) < 0) return;
  {  // (the profiles loaded: a stock superstar holds its moves - else later)
    const uint32_t ri = svr2011::RosterIndex(base, 100);  // (THE ROCK)
    if (ri >= 512 || !CountMoves(base + svr2011::RosterEntry(kProfiles, kProfileSize, ri)).set) return;
  }
  checked = true;
  if (const char* f = std::getenv("SVR2011_TEST_DUMP_ENTRANCES")) {  // (test aid: every profile's entrance block, hex)
    if (FILE* o = std::fopen(f, "wb")) {
      for (uint32_t id = 0; id < 512; ++id) {
        const uint32_t i = svr2011::RosterIndex(base, id);
        if (i >= 512) continue;
        const uint8_t* e = base + svr2011::RosterEntry(kProfiles, kProfileSize, i) + kEntrance;
        std::fprintf(o, "%u ", id);
        for (uint32_t k = 0; k < 284; ++k) std::fprintf(o, "%02x", e[k]);
        std::fputc('\n', o);
      }
      std::fclose(o);
      REXLOG_INFO("[svr2011] superstar mods: test: entrance blocks written to {}", f);
    }
  }
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot), bi = svr2011::RosterIndex(base, m.base);
    if (si >= 512 || bi >= 512) continue;
    uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    const uint8_t* bp = base + svr2011::RosterEntry(kProfiles, kProfileSize, bi);
    // test aid: SVR2011_TEST_SAVED_MOVE=<folder>,<0xOFF>,<id> - that mod's slot holds the id, as a save made with
    // an older version of the mod would (moves_port.py renumber: a save holding the old id)
    if (const char* t = std::getenv("SVR2011_TEST_SAVED_MOVE")) {  // (several: ';' between)
      std::istringstream all(t);
      for (std::string v; std::getline(all, v, ';');) {
        const size_t c1 = v.find(','), c2 = c1 == std::string::npos ? c1 : v.find(',', c1 + 1);
        if (c2 == std::string::npos || v.substr(0, c1) != m.folder) continue;
        const uint32_t off = uint32_t(std::strtoul(v.c_str() + c1 + 1, nullptr, 0));
        const uint16_t id = uint16_t(std::strtoul(v.c_str() + c2 + 1, nullptr, 0));
        if (off + 1u < kEntrance) {
          sp[off] = uint8_t(id >> 8), sp[off + 1] = uint8_t(id);
          REXLOG_INFO("[svr2011] superstar mods: test: {} slot 0x{:03X} holds {}", m.folder, off, id);
        }
      }
    }
    std::string fixed, repaired, moved;
    if (!m.renumbered.empty())  // (a save of the mod's older version: its old ids, in any slot - an edited move-set too)
      for (uint32_t off = 0; off + 1 < kEntrance; off += 2) {
        const uint16_t raw = Rd16(sp + off);
        const auto r = m.renumbered.find(uint16_t(raw & 0x7FFF));
        if (r == m.renumbered.end() || MoveUsable(r->second) != 1) continue;
        const uint16_t v = uint16_t((raw & 0x8000) | r->second);
        sp[off] = uint8_t(v >> 8), sp[off + 1] = uint8_t(v);
        moved += fmt::format(" 0x{:03X}={}->{}", off, r->first, r->second);
      }
    if (!moved.empty())
      REXLOG_WARN("[svr2011] superstar mods: {}: saved moves on the ids the mod had before (renumbered), its new ids put "
                  "in:{}", m.folder, moved);
    for (const auto& [off, id] : m.moves) {
      if (off + 1u >= kEntrance) continue;
      const uint16_t cur = uint16_t(Rd16(sp + off) & 0x7FFF);
      if (cur == id) {
        if (MoveUsable(id) != 0) continue;
        sp[off] = bp[off], sp[off + 1] = bp[off + 1];
        fixed += fmt::format(" 0x{:03X}={}", off, id);
      } else if ((!cur || MoveUsable(cur) == 0) && MoveUsable(id) == 1) {
        // (a saved edit - or an older version of the mod - holding a move the game has no record for, or none:
        // the mod's own move. Sabu 1.5's finisher 6861 had no record - 1.7 moved it to 6027 - and a save whose
        // move-set was edited kept 6861: the finisher started and stopped after the flip.)
        sp[off] = uint8_t(id >> 8), sp[off + 1] = uint8_t(id);
        repaired += fmt::format(" 0x{:03X}={}->{}", off, cur, id);
      }
    }
    if (!fixed.empty())
      REXLOG_WARN("[svr2011] superstar mods: {}: moves the game doesn't have, the base's put back:{}", m.folder, fixed);
    if (!repaired.empty())
      REXLOG_WARN("[svr2011] superstar mods: {}: saved moves the game has no record for, the mod's own put back:{}",
                  m.folder, repaired);
    {  // (any slot - an edited move-set too - holding a move with no motion anywhere: the base's)
      std::string none;
      for (uint32_t off = 0; off + 1 < kEntrance; off += 2) {
        const uint16_t cur = uint16_t(Rd16(sp + off) & 0x7FFF);
        if (!cur || !svr2011::NoMotionMove(cur)) continue;
        sp[off] = bp[off], sp[off + 1] = bp[off + 1];
        none += fmt::format(" 0x{:03X}={}", off, cur);
      }
      if (!none.empty())
        REXLOG_WARN("[svr2011] superstar mods: {}: saved moves with no motion in the game or a pack (a removed or "
                    "renumbered pack's), the base's put in:{}", m.folder, none);
    }
    if (const char* t = std::getenv("SVR2011_TEST_EMPTY_MOVESET"); t && m.folder == t) {  // (test aid: a broken move-set)
      std::memset(sp, 0, kEntrance);
      REXLOG_INFO("[svr2011] superstar mods: test: {}'s move-set emptied", m.folder);
    }
    EnsureMoveset(base, sp, m);
  }
}
}  // namespace svr2011

// The select lists' order: sub_82736E00 (the team editor's member list) and
// sub_82734560 (exhibition's) rank each id by its record's +226 (the grid
// tile number: rank = how many keys are smaller), and equal keys take the
// same row. Mods have no tile (0, OwnSelectTile): all of them took one row
// (the last mod won) and the rows they left kept id 0 - a nameless created
// superstar ("DEFAULT"): team members couldn't be picked as the player
// meant, exhibition showed generics (2.0.4). While those sorts run, each
// mod's record has a key of its own after every real one (0x7000 + n).
void ModSortKeys(uint8_t* base, bool on) {
  uint32_t n = 0;
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512) continue;
    uint8_t* key = base + svr2011::RosterEntry(kRecords, kRecordSize, si) + kSelectTile;
    const uint32_t mine = 0x7000 + n++;
    if (on && !Rd16(key)) key[0] = uint8_t(mine >> 8), key[1] = uint8_t(mine);
    else if (!on && Rd16(key) == mine) key[0] = key[1] = 0;
  }
}
REX_EXTERN(__imp__sub_82736E00);
REX_HOOK_RAW(sub_82736E00) {
  ModSortKeys(base, true);
  __imp__sub_82736E00(ctx, base);
  const auto r3 = ctx.r3.u64;
  ModSortKeys(base, false);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82734560);
REX_HOOK_RAW(sub_82734560) {
  ModSortKeys(base, true);
  __imp__sub_82734560(ctx, base);
  const auto r3 = ctx.r3.u64;
  ModSortKeys(base, false);
  ctx.r3.u64 = r3;
}

// A roster list is about to be built.
REX_EXTERN(__imp__sub_82736C68);
REX_HOOK_RAW(sub_82736C68) {
  ApplyRecords(base);
  __imp__sub_82736C68(ctx, base);
}

// Owned (DLC-flagged ids must be): mods are - for WWE Universe's rankings
// (sub_825AF0F0's loop) and its owned snapshot (sub_825CA488) only those with
// a Universe entry (universe_mods.cpp: all of them, up to 137).
constexpr uint32_t kUniverseOwnedCallers[] = {0x825AF190, 0x825CA518};
REX_EXTERN(__imp__sub_828C1288);
REX_HOOK_RAW(sub_828C1288) {
  if (ModOf(ctx.r3.u32)) {
    const uint32_t lr = uint32_t(ctx.lr);
    bool universe = false;
    for (uint32_t c : kUniverseOwnedCallers) universe |= lr == c;
    ctx.r3.u64 = !universe || svr2011::UniverseModIn(ctx.r3.u32) ? 1 : 0;
    return;
  }
  __imp__sub_828C1288(ctx, base);
}

// ---- WWE Universe: mods in a side table (universe_mods.cpp)
// Universe keeps 150 superstars (P = *(0x82E407B4) + 0x364B4: 24-byte
// entries at P+16 - 0-49 created, 50-69 DLC, 70-149 a disc list of 80 ids at
// 0x82DACA40 - a rivalry byte per pair at P+3616, match cards from P+14942).
// sub_825B7660(id) gives an id's place: created and DLC ids their own number,
// disc-list ids 70 + their index. Mods taking their id as place wrote past
// the 150 entries (2.0.4/2.0.5 crashes; 0f87e22 kept them out). Now a mod's
// place is a virtual one landing on its side entry, its pairs on side bytes
// (sub_825AD850), and Universe's reset (sub_825B6C60) clears the side table.
// "Is this id present" (sub_828B7078(id): the flag 0x82DB3AE0 + id, read by
// the CHAR/DAT and CHAR/PRO loaders and the roster getters): a mod's id is,
// so the 65 ids the game keeps absent load their placeholder record and
// profile and show like the other mod ids when a mod takes one.
REX_EXTERN(__imp__sub_828B7078);
REX_HOOK_RAW(sub_828B7078) {
  if (ctx.r3.u32 < svr2011::kMaxIds && ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  if (ctx.r3.u32 >= 322) {  // (past the game's 322-byte table: big roster's ids hold mods only)
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828B7078(ctx, base);
}

// A superstar's kind (sub_828B78C0(id): 1 a stock one 100-321, 2 an added one - the DLC flag +257 -, 3 a Created
// Superstar 0-49, 0 none): its record is read for ids under 322 only, so a mod on big roster's ids 323-511 was
// "none". Online that left its lobby slot waiting for good - the stored superstar block makes the slot ready (state 8)
// for kinds 1-3 only (sub_828A6008), so a lobby with such a pick never started - and its pictures were never asked for.
// A mod is an added one wherever its id is.
REX_EXTERN(__imp__sub_828B78C0);
REX_HOOK_RAW(sub_828B78C0) {
  if (ctx.r3.u32 >= 322 && ctx.r3.u32 < svr2011::kMaxIds && ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 2;
    return;
  }
  __imp__sub_828B78C0(ctx, base);
}

// A pick (sub_828B8190(id, attire, kind)): id * 100 + attire * 10 + kind when that character's file is there, else
// id * 100 + attire * 10. Nobody (512) has no file, so nobody's pick is 51200 - unless a mod's key is 512: mods are
// keyed from 400, the 113th gets 512 and its ch512.pac is there. Then nobody picks as 51202, which the game doesn't
// take for nobody: Story Designer's NEW (sub_82625988 fills its six cast places with nobody's pick) crashed in
// sub_82750ED8 reading nobody's record. Nobody's pick is 51200 whatever files there are (keys already given stay).
REX_EXTERN(__imp__sub_828B8190);
REX_HOOK_RAW(sub_828B8190) {
  if (ctx.r3.u32 == kNobodyId) {
    ctx.r3.u64 = kNobodyId * 100;
    return;
  }
  __imp__sub_828B8190(ctx, base);
}

// A match slot from a lobby's person (sub_828BA250(L, slot, id, team, attire, kind, r9) - the online hand-off's
// sub_828BB290): ids from 322 empty the slot (sub_8257C1E0), so an online match with big roster's ids 323-511 had
// nobody in it. For such a mod the slot is filled as for any other id: +16 the pick (sub_828B8190(id, attire, r9)),
// +20 the kind (2: the record's +21), the team's bytes.
REX_EXTERN(__imp__sub_828BA250);
REX_HOOK_RAW(sub_828BA250) {
  const uint32_t L = ctx.r3.u32, slot = ctx.r4.u32, id = ctx.r5.u32, team = ctx.r6.u32, kind = ctx.r8.u32;
  if (id < 322 || id >= svr2011::kMaxIds || !ModOf(id)) {
    __imp__sub_828BA250(ctx, base);
    return;
  }
  const uint32_t si = svr2011::RosterIndex(base, id);
  if (si >= 512) {
    __imp__sub_828BA250(ctx, base);
    return;
  }
  PPCContext c = ctx;
  c.r3.u64 = id, c.r4.u64 = ctx.r7.u32, c.r5.u64 = ctx.r9.u32;
  sub_828B8190(c, base);
  Wr32(base + slot + 16, c.r3.u32);
  base[slot + 20] = kind == 2 ? base[svr2011::RosterEntry(kRecords, kRecordSize, si) + 21] : uint8_t(kind);
  if (int32_t(team) != -1) {
    base[slot + 4] = uint8_t(team), base[slot + 12] = 0;
    base[slot + 0] = base[L + team + 130], base[slot + 1] = base[L + team + 136];
    base[slot + 2] = base[L + 113], base[slot + 3] = base[L + team + 114];
  } else {
    base[slot + 0] = 0, base[slot + 12] = 1, base[slot + 1] = 0, base[slot + 2] = 2, base[slot + 3] = 0;
  }
  static std::atomic<int> told{0};
  if (told.fetch_add(1) < 12)
    REXLOG_INFO("[svr2011] superstar mods: match slot for id {} (past 321): pick {} (the game's empties those)", id,
                c.r3.u32);
}

// The online object (*0x82E3DD98, built by sub_824C44F8) keeps the roster's records at +1976 - 242 of 260 bytes,
// by record index, a 322-entry list right after them - and reads a superstar's profile at *(+1968) + id * 1056 +
// 48. Its readers (sub_824C3BA0 records by id through the index, sub_824C3B48 profiles by id) would run past
// both for big roster's indices from 242 / ids from 322: the record comes from the live table's extension instead
// (RosterEntry), the profile from the game's own fallback (sub_828B6938, the live profile). No online test
// reached them (lobby picks, CPU slots, matches); a guard, logged once.
REX_EXTERN(__imp__sub_824C3BA0);
REX_HOOK_RAW(sub_824C3BA0) {
  const uint32_t id = ctx.r3.u32;
  __imp__sub_824C3BA0(ctx, base);
  if (!ctx.r3.u32 || id < 322 || id >= svr2011::kMaxIds) return;
  const uint32_t si = svr2011::RosterIndex(base, id);
  if (si < svr2011::kStockIndices || si >= 512) return;
  ctx.r3.u64 = svr2011::RosterEntry(kRecords, kRecordSize, si);
  static std::atomic<bool> told{false};
  if (!told.exchange(true))
    REXLOG_INFO("[svr2011] superstar mods: online object record for id {} (index {}) - the live table's", id, si);
}
REX_EXTERN(__imp__sub_824C3B48);
REX_HOOK_RAW(sub_824C3B48) {
  if (ctx.r3.u32 >= 322 && ctx.r3.u32 < svr2011::kMaxIds) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) REXLOG_INFO("[svr2011] superstar mods: online object profile for id {} - the live one", ctx.r3.u32);
    sub_828B6938(ctx, base);
    return;
  }
  __imp__sub_824C3B48(ctx, base);
}

REX_EXTERN(__imp__sub_825B75A8);
REX_HOOK_RAW(sub_825B75A8) {  // (id, need owned) -> in Universe
  if (ModOf(ctx.r3.u32) && !svr2011::UniverseModIn(ctx.r3.u32)) {  // (a mod past the side table's room)
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825B75A8(ctx, base);
}
REX_EXTERN(__imp__sub_825B7660);
namespace {
int64_t UniversePlace(PPCContext& ctx, uint8_t* base, uint32_t id) {  // (any id: the game's place, a mod's side one)
  if (ModOf(id)) return svr2011::UniverseModPlace(base, id);
  const auto saved = ctx;
  ctx.r3.u64 = id;
  __imp__sub_825B7660(ctx, base);
  const int64_t place = int64_t(int32_t(ctx.r3.u32));
  ctx = saved;
  return place;
}
}  // namespace
REX_HOOK_RAW(sub_825B7660) {  // (id) -> Universe place, -1: none
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = uint64_t(svr2011::UniverseModPlace(base, ctx.r3.u32));
    return;
  }
  __imp__sub_825B7660(ctx, base);
}
REX_EXTERN(__imp__sub_825B7B40);
// (id, id) -> 1: the two may team up - both in Universe, not in one team already, and not both in teams. Its
// callers: after a match the partners' join step (sub_825C3498: one joins the other's team, sub_828C0F50 adds with
// no check of its own) and a new team (sub_825ACFF8).
REX_HOOK_RAW(sub_825B7B40) {
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  if (ModOf(a) || ModOf(b)) {  // (its own code looks ids up in the disc list: a mod's answered here)
    bool ok = UniversePlace(ctx, base, a) != -1 && UniversePlace(ctx, base, b) != -1;
    if (ok) {  // (its team rules, as its own code has them: this answer once skipped them, and after each match
      // a stock partner of a mod joined their team again - report a6aa1e: Matt Hardy 3 times in THE HARDYS)
      const auto saved = ctx;
      const auto team = [&](uint32_t x, uint32_t y) {  // sub_828C0650(x, y, nobody): the team that has both, -1
        ctx = saved;
        ctx.r3.u64 = x, ctx.r4.u64 = y, ctx.r5.u64 = 0x200;
        sub_828C0650(ctx, base);
        return int32_t(ctx.r3.u32);
      };
      ok = team(a, b) == -1 && (team(a, 0x200) == -1 || team(b, 0x200) == -1);
      ctx = saved;
    }
    ctx.r3.u64 = ok ? 1 : 0;
    return;
  }
  __imp__sub_825B7B40(ctx, base);
}
namespace {
// The rivalry table sub_825AD850's caller adds the index to, read from the caller's registers (at the callee's entry
// they are still the caller's). Its 7 callers (the whole exe searched): a table object's +4 or +12, or P+3616
// itself - and some objects are copies (sub_825C2BA0 -> sub_825AD950 on a stack copy). 0: not known.
uint32_t PairTable(const PPCContext& ctx, uint8_t* base) {
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t save = Rd32(base + 0x82E407B4);
  const uint32_t own = save ? save + 0x364B4 + 3616 : 0;  // (Universe's own: P+3616)
  auto at = [&](uint32_t obj, uint32_t off) { return obj ? Rd32(base + obj + off) : 0u; };
  if (lr >= 0x825AF908) return at(SVR_CALLER_NV(ctx, r31), 4);   // sub_825AF908: obj r31, *(obj+4)
  if (lr >= 0x825AF800) return own;                  // sub_825AF800: sub_8257A490 + 3616
  if (lr >= 0x825AECE8) return at(SVR_CALLER_NV(ctx, r27), 12);  // sub_825AECE8: obj r27, *(obj+12)
  if (lr >= 0x825ADA48) return own;                  // sub_825ADA48: only tests -1
  if (lr >= 0x825AD9C8) return at(SVR_CALLER_NV(ctx, r31), 4);   // sub_825AD9C8: obj r31
  if (lr >= 0x825AD950) return at(SVR_CALLER_NV(ctx, r31), 4);   // sub_825AD950: obj r31
  if (lr >= 0x825AD8E0) return at(SVR_CALLER_NV(ctx, r30), 4);   // sub_825AD8E0: obj r30
  return 0;
}
}  // namespace
REX_EXTERN(__imp__sub_825AD850);
REX_HOOK_RAW(sub_825AD850) {  // (id, id) -> the pair's rivalry index from the caller's table, -1: none
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  if (ModOf(a) || ModOf(b)) {
    const uint32_t lr = uint32_t(ctx.lr);
    const uint32_t table = lr >= 0x825AD8E0 && lr < 0x825AFA00 ? PairTable(ctx, base) : 0;
    if (!table) {  // (a caller not known: no pair)
      static bool said = false;
      if (!said) {
        said = true;
        REXLOG_WARN("[svr2011] universe mods: rivalry pair asked from lr {:08X} - none", lr);
      }
      ctx.r3.u64 = uint64_t(int64_t(-1));
      return;
    }
    const int64_t pa = ModOf(a) ? 0 : UniversePlace(ctx, base, a), pb = ModOf(b) ? 0 : UniversePlace(ctx, base, b);
    // (the weekly relationship pass's working copy lives in sub_825C3D70's stack frame, just above this call's
    // stack pointer: universe_mods.h)
    const uint32_t sp = ctx.r1.u32;
    const bool working_copy = table > sp && table - sp < 0x10000;
    ctx.r3.u64 = uint64_t(svr2011::UniverseModPair(base, table, a, pa, b, pb, working_copy));
    return;
  }
  __imp__sub_825AD850(ctx, base);
}
REX_EXTERN(__imp__sub_825B6C60);
REX_HOOK_RAW(sub_825B6C60) {  // Universe's reset (its tables cleared): the mods' side table too
  __imp__sub_825B6C60(ctx, base);
  svr2011::UniverseModsReset();
}
REX_EXTERN(__imp__sub_82589198);
REX_HOOK_RAW(sub_82589198) {
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82589198(ctx, base);
}

// ---- Per-mod save files (per_mod_saves, experimental, off by default)
// A mod superstar's save data - its slot's 260-byte record, 1056-byte profile and 40-byte entry (block B +0x3E648 +
// index * 40; the other per-id looking tables of block A aren't per id: +0x2FE04 is 26,280 bytes, +0x364AC 8) -
// lives in <saves>\.mods\<folder>.dat instead of SaveData.dat. At each save the game builds, every mod slot gets its
// stub - the slot as the game loads it before any mod (the placeholder record, profile and entry) - and the mod's
// own bytes go to its file; after the write they're put back. The save then holds no mod data: a mod uninstalled
// leaves it clean, and a 2.1.8 game reads it as ever. At start each mod's file (its fingerprint matching - an
// updated mod starts from its own defaults, as a stale profile does) seeds what the slot keeps (g_kept): a save's
// stub there takes it back (ApplyRecords). The link to the save is mod_profiles.txt (slot, folder, fingerprint).
// Turning it on: the save folder is backed up first (SaveBackups\per_mod_saves_<time>), the first save moves the
// data out. Turning it off: the files still seed the slots once (.mods\.on is there), the next save folds them back.
//
// File (big-endian): "SVRMODSV", u32 1, u16 n + folder, u16 n + version, u32 fingerprint, u16 the id it held,
// u32 the blocks' CRC-32, then blocks of u32 tag + u32 size + bytes: 'REC ' 260, 'PRO ' 1056, 'E40 ' 40. Unknown
// tags are skipped.
constexpr uint32_t kEntries40 = 0x82E7C918 + 0x3E648, kEntry40Size = 40;
constexpr char kModSaveMagic[8] = {'S', 'V', 'R', 'M', 'O', 'D', 'S', 'V'};

namespace {
struct ModSave {
  svrfmt::Bytes record, profile, entry;
};
std::map<uint32_t, svrfmt::Bytes> g_stub_record, g_stub_entry;  // slot -> as the game loaded it, before any mod
std::map<uint32_t, svrfmt::Bytes> g_saved_entry;                // slot -> the .dat's 40-byte entry, to put back
std::map<uint32_t, ModSave> g_live_while_saving;                // slot -> the mod's bytes while the save is written
std::map<std::string, svrfmt::Bytes> g_mod_save_written;         // folder -> the file's bytes last written
bool g_mod_saves_on = false, g_mod_saves_fold = false;

fs::path ModSavesDir() { return g_save_file.empty() ? fs::path() : g_save_file.parent_path() / ".mods"; }  // (a dot folder: the saves layer takes any other entry in Saves for a save package - a "Mods" folder crashed the game)

uint32_t Crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

void Put32(svrfmt::Bytes& o, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}
void PutStr(svrfmt::Bytes& o, const std::string& s) {
  o.push_back(uint8_t(s.size() >> 8)), o.push_back(uint8_t(s.size()));
  o.insert(o.end(), s.begin(), s.end());
}

svrfmt::Bytes ModSaveFile(const Mod& m, const ModSave& s) {
  svrfmt::Bytes blocks;
  for (const auto& [tag, data] : {std::pair<uint32_t, const svrfmt::Bytes*>{0x52454320, &s.record},
                                  {0x50524F20, &s.profile}, {0x45343020, &s.entry}})
    if (!data->empty()) Put32(blocks, tag), Put32(blocks, uint32_t(data->size())), blocks.insert(blocks.end(), data->begin(), data->end());
  svrfmt::Bytes o(kModSaveMagic, kModSaveMagic + 8);
  Put32(o, 1);
  PutStr(o, m.folder);
  PutStr(o, m.version);
  Put32(o, Fingerprint(m));
  o.push_back(uint8_t(m.slot >> 8)), o.push_back(uint8_t(m.slot));
  Put32(o, Crc32(blocks.data(), blocks.size()));
  o.insert(o.end(), blocks.begin(), blocks.end());
  return o;
}

// A mod's file read back: false when it's missing, damaged, another mod's or made for another version (fingerprint).
bool ReadModSave(const Mod& m, ModSave& s) {
  svrfmt::Bytes d;
  if (!svrfmt::ReadFile((ModSavesDir() / (m.folder + ".dat")).string(), d) || d.size() < 22 ||
      std::memcmp(d.data(), kModSaveMagic, 8) || Rd32(d.data() + 8) != 1)
    return false;
  size_t at = 12;
  auto str = [&](std::string& out) {
    if (at + 2 > d.size()) return false;
    const size_t n = Rd16(d.data() + at);
    if (at + 2 + n > d.size()) return false;
    out.assign(reinterpret_cast<const char*>(d.data() + at + 2), n), at += 2 + n;
    return true;
  };
  std::string folder, version;
  if (!str(folder) || !str(version) || at + 10 > d.size() || folder != m.folder) return false;
  const uint32_t fingerprint = Rd32(d.data() + at), crc = Rd32(d.data() + at + 6);
  at += 10;
  if (Crc32(d.data() + at, d.size() - at) != crc) {
    REXLOG_WARN("[svr2011] per-mod saves: {}.dat is damaged - its defaults instead", m.folder);
    return false;
  }
  if (fingerprint != Fingerprint(m)) {
    REXLOG_INFO("[svr2011] per-mod saves: {}.dat was made for another version of the mod - its defaults instead",
                m.folder);
    return false;
  }
  while (at + 8 <= d.size()) {
    const uint32_t tag = Rd32(d.data() + at), n = Rd32(d.data() + at + 4);
    if (at + 8 + n > d.size()) break;
    const svrfmt::Bytes b(d.data() + at + 8, d.data() + at + 8 + n);
    if (tag == 0x52454320 && n == kRecordSize) s.record = b;
    else if (tag == 0x50524F20 && n == kProfileSize) s.profile = b;
    else if (tag == 0x45343020 && n == kEntry40Size) s.entry = b;
    at += 8 + n;
  }
  g_mod_save_written[m.folder] = std::move(d);
  return !s.record.empty() && !s.profile.empty();
}

void WriteModSave(const Mod& m, const ModSave& s) {
  const svrfmt::Bytes f = ModSaveFile(m, s);
  if (const auto it = g_mod_save_written.find(m.folder); it != g_mod_save_written.end() && it->second == f) return;
  std::error_code ec;
  fs::create_directories(ModSavesDir(), ec);
  const fs::path to = ModSavesDir() / (m.folder + ".dat"), part = fs::path(to).concat(".tmp");
  if (FILE* o = std::fopen(part.string().c_str(), "wb")) {
    const bool ok = std::fwrite(f.data(), 1, f.size(), o) == f.size();
    std::fclose(o);
    if (ok) fs::rename(part, to, ec);
    if (ok && !ec) {
      g_mod_save_written[m.folder] = f;
      REXLOG_INFO("[svr2011] per-mod saves: {} (id {}) written to .mods\\{}.dat", m.name, m.slot, m.folder);
      return;
    }
  }
  REXLOG_WARN("[svr2011] per-mod saves: {}.dat could not be written", m.folder);
}

// The slots' stubs: the record and entry as the game's first CHAR/DAT load leaves them (before ApplyRecords), the
// profile CHAR/PRO's placeholder (g_blank).
void RecordStubs(uint8_t* base) {
  for (const auto& m : g_mods) {
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    if (si >= 512 || g_stub_record.count(m.slot)) continue;
    const uint8_t* sr = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
    if (!std::strncmp(reinterpret_cast<const char*>(sr + kFullName), m.name.c_str(), kNameLen - 1)) continue;
    g_stub_record[m.slot].assign(sr, sr + kRecordSize);
    const uint8_t* se = base + svr2011::RosterEntry(kEntries40, kEntry40Size, si);
    g_stub_entry[m.slot].assign(se, se + kEntry40Size);
  }
}

// At start: the option, the backup when it was just turned on, and the files seeding the slots.
void StartModSaves() {
  const fs::path dir = ModSavesDir();
  if (dir.empty()) return;
  std::error_code ec;
  const fs::path on_mark = dir / ".on";
  const bool was_on = fs::exists(on_mark, ec);
  g_mod_saves_on = REXCVAR_GET(per_mod_saves);
  // Test aid: SVR2011_TEST_PER_MOD_SAVES=0 / 1 overrides the setting.
  if (const char* v = std::getenv("SVR2011_TEST_PER_MOD_SAVES"); v && *v) g_mod_saves_on = *v == '1';
  // (big_roster: the indices from 242 up are in no table SaveData.dat holds - the mods' files are their only save)
  if (svr2011::BigRosterOn() && !g_mod_saves_on) {
    g_mod_saves_on = true;
    REXLOG_INFO("[svr2011] per-mod saves: on - big_roster needs them (per_mod_saves is off in the settings)");
  }
  if (g_mod_saves_on && !was_on) {
    // (turned on: the saves backed up first - the first save then moves the mods' data out)
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path backup = g_game / "SaveBackups" / fmt::format("per_mod_saves_{}", stamp);
    fs::create_directories(backup, ec);
    for (const auto& e : fs::directory_iterator(g_save_file.parent_path(), ec))
      if (e.is_regular_file(ec)) fs::copy_file(e.path(), backup / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    fs::create_directories(dir, ec);
    if (FILE* f = std::fopen(on_mark.string().c_str(), "wb")) std::fclose(f);
    REXLOG_INFO("[svr2011] per-mod saves: turned on - saves backed up to {}; the next save moves the mods' data out",
                backup.string());
  }
  g_mod_saves_fold = !g_mod_saves_on && was_on;
  if (!g_mod_saves_on && !g_mod_saves_fold) return;
  int seeded = 0;
  for (const auto& m : g_mods) {
    if (m.temp) continue;
    ModSave s;
    if (!ReadModSave(m, s)) continue;
    Kept& k = KeptFor(m.slot);
    k.record = s.record, k.profile = s.profile;
    // (the file's record names the id the mod had when it was saved: its id of this run - slots move with big_roster)
    if (k.record.size() >= kRecordSize)
      for (uint32_t off : {kOwnId, kOwnId2, kSamePerson}) k.record[off] = uint8_t(m.slot >> 8), k.record[off + 1] = uint8_t(m.slot);
    if (!s.entry.empty()) g_saved_entry[m.slot] = s.entry;
    ++seeded;
  }
  REXLOG_INFO("[svr2011] per-mod saves: {}{} mod file(s) read", g_mod_saves_on ? "on - " : "turned off - folding back; ",
              seeded);
}

// The 40-byte entries from the files: once the slot holds the mod's record again (its stub's entry still there).
void PutBackEntries(uint8_t* base) {
  for (auto it = g_saved_entry.begin(); it != g_saved_entry.end();) {
    const Mod* m = ModOf(it->first);
    const uint32_t si = m ? svr2011::RosterIndex(base, m->slot) : 512;
    const uint8_t* sr = si < 512 ? base + svr2011::RosterEntry(kRecords, kRecordSize, si) : nullptr;
    if (!sr || std::strncmp(reinterpret_cast<const char*>(sr + kFullName), m->name.c_str(), kNameLen - 1)) {
      ++it;
      continue;
    }
    std::memcpy(base + svr2011::RosterEntry(kEntries40, kEntry40Size, si), it->second.data(), kEntry40Size);
    it = g_saved_entry.erase(it);
  }
}

// Key refs (per_mod_saves): the save's other fields that name a superstar by id (step 0's map, docs/
// UNLIMITED_MOD_SLOTS.md) hold no mod's id in the file. While the save is built each one that holds a mod gets a
// stand-in (the mod's base superstar, else 0x200 = nobody) and Saves\.mods\refs.bin keeps (field, mod folder,
// stand-in); after a load each field that still holds its stand-in gets the mod's id back - a mod that's gone keeps
// the stand-in. Offsets are from the save image (*0x82E407B4); u16 ids, big-endian.
constexpr char kRefsMagic[8] = {'S', 'V', 'R', 'M', 'R', 'E', 'F', 'S'};
constexpr uint32_t kNobody = 0x200;

// Every u16 id field (save image offsets).
std::vector<uint32_t> IdFields() {
  std::vector<uint32_t> f;
  for (uint32_t t = 0; t < 70; ++t)  // teams / stables: 70 x 270 B, members at +2 (count at +0)
    for (uint32_t k = 0; k < 5; ++k) f.push_back(0xF5C8 + t * 270 + 2 + k * 2);
  for (uint32_t k = 0; k < 28; ++k) f.push_back(0x7CD70 + k * 2);  // title holders: 14 titles x 2
  constexpr uint32_t P = 0x364B4;
  for (uint32_t c = P + 14942; c + 88 <= P + 20808; c += 88)  // Universe cards: 16 B header, 6 people x 12 B
    for (uint32_t k = 0; k < 6; ++k) f.push_back(c + 16 + k * 12);
  for (uint32_t k = 0; k < 240; ++k) f.push_back(P + 20808 + k * 12), f.push_back(P + 20808 + k * 12 + 2);  // the list
  return f;
}

struct Ref {
  uint32_t off;
  std::string folder;
  uint16_t stub;
};
std::vector<std::pair<uint32_t, uint16_t>> g_refs_live;  // (field, the mod's id) while the save is built
svrfmt::Bytes g_refs_written;

fs::path RefsFile() { return ModSavesDir() / "refs.bin"; }

svrfmt::Bytes RefsBytes(const std::vector<Ref>& refs) {
  std::vector<std::string> folders;
  svrfmt::Bytes body;
  Put32(body, uint32_t(refs.size()));
  for (const Ref& r : refs) {
    auto it = std::find(folders.begin(), folders.end(), r.folder);
    if (it == folders.end()) it = folders.insert(folders.end(), r.folder);
    Put32(body, r.off);
    body.push_back(uint8_t((it - folders.begin()) >> 8)), body.push_back(uint8_t(it - folders.begin()));
    body.push_back(uint8_t(r.stub >> 8)), body.push_back(uint8_t(r.stub));
  }
  svrfmt::Bytes names;
  names.push_back(uint8_t(folders.size() >> 8)), names.push_back(uint8_t(folders.size()));
  for (const auto& s : folders) PutStr(names, s);
  names.insert(names.end(), body.begin(), body.end());
  svrfmt::Bytes o(kRefsMagic, kRefsMagic + 8);
  Put32(o, 1);
  Put32(o, Crc32(names.data(), names.size()));
  o.insert(o.end(), names.begin(), names.end());
  return o;
}

bool ReadRefs(const fs::path& file, std::vector<Ref>& refs) {
  svrfmt::Bytes d;
  if (!svrfmt::ReadFile(file.string(), d) || d.size() < 18 || std::memcmp(d.data(), kRefsMagic, 8) ||
      Rd32(d.data() + 8) != 1 || Crc32(d.data() + 16, d.size() - 16) != Rd32(d.data() + 12))
    return false;
  size_t at = 16;
  std::vector<std::string> folders(Rd16(d.data() + at));
  at += 2;
  for (auto& s : folders) {
    if (at + 2 > d.size()) return false;
    const size_t n = Rd16(d.data() + at);
    if (at + 2 + n > d.size()) return false;
    s.assign(reinterpret_cast<const char*>(d.data() + at + 2), n), at += 2 + n;
  }
  if (at + 4 > d.size()) return false;
  const uint32_t n = Rd32(d.data() + at);
  at += 4;
  if (d.size() - at < size_t(n) * 8) return false;
  for (uint32_t k = 0; k < n; ++k, at += 8) {
    const uint32_t fi = Rd16(d.data() + at + 4);
    if (fi >= folders.size()) return false;
    refs.push_back({Rd32(d.data() + at), folders[fi], uint16_t(Rd16(d.data() + at + 6))});
  }
  return true;
}

// A stand-in for a mod's id in the file: its base superstar when that is a stock one, else nobody.
uint16_t RefStub(const Mod& m) { return m.base && m.base < 322 && !ModOf(m.base) ? uint16_t(m.base) : uint16_t(kNobody); }

void WriteRefs(const std::vector<Ref>& refs) {
  const svrfmt::Bytes f = RefsBytes(refs);
  if (f == g_refs_written) return;
  std::error_code ec;
  fs::create_directories(ModSavesDir(), ec);
  const fs::path to = RefsFile(), part = fs::path(to).concat(".tmp"), bak = fs::path(to).concat(".bak");
  if (FILE* o = std::fopen(part.string().c_str(), "wb")) {
    const bool ok = std::fwrite(f.data(), 1, f.size(), o) == f.size();
    std::fclose(o);
    if (ok && fs::exists(to, ec)) fs::copy_file(to, bak, fs::copy_options::overwrite_existing, ec);  // (the last one)
    if (ok) fs::rename(part, to, ec);
    if (ok && !ec) {
      g_refs_written = f;
      REXLOG_INFO("[svr2011] per-mod saves: refs.bin written - {} field(s) name a mod", refs.size());
      return;
    }
  }
  REXLOG_WARN("[svr2011] per-mod saves: refs.bin could not be written");
}

// Around the save writer: the fields' mod ids out (saving: stand-ins in, refs.bin written), then back.
void RefsForSave(uint8_t* base, bool saving) {
  const uint32_t save = Rd32(base + 0x82E407B4);
  if (!save) return;
  if (!saving) {
    for (const auto& [off, id] : g_refs_live) svrfmt::Wr16(base + save + off, uint16_t(id));
    g_refs_live.clear();
    return;
  }
  std::vector<Ref> refs;
  for (const uint32_t off : IdFields()) {
    const uint32_t id = Rd16(base + save + off);
    const Mod* m = id < kNobody ? ModOf(id) : nullptr;
    if (!m || m->temp) continue;  // (a fetched mod: OnlineTempForSave)
    const uint16_t stub = RefStub(*m);
    refs.push_back({off, m->folder, stub});
    g_refs_live.emplace_back(off, uint16_t(id));
    svrfmt::Wr16(base + save + off, stub);
  }
  WriteRefs(refs);
}

// After a load: each field still holding its stand-in gets its mod's id back (the mod installed).
void RefsLoaded(uint8_t* base) {
  const uint32_t save = Rd32(base + 0x82E407B4);
  std::vector<Ref> refs;
  if (!save || ModSavesDir().empty() || !ReadRefs(RefsFile(), refs)) return;
  int back = 0, gone = 0, changed = 0;
  for (const Ref& r : refs) {
    const Mod* m = nullptr;
    for (const auto& x : g_mods)
      if (!x.temp && x.folder == r.folder) m = &x;
    if (Rd16(base + save + r.off) != r.stub) {
      ++changed;  // (the save was written without this refs.bin - another copy of SaveData.dat)
      continue;
    }
    if (!m) {
      ++gone;
      continue;
    }
    svrfmt::Wr16(base + save + r.off, uint16_t(m->slot));
    ++back;
  }
  REXLOG_INFO("[svr2011] per-mod saves: refs.bin - {} field(s) given their mod back, {} mod(s) not installed (stand-in "
              "kept), {} field(s) no longer the stand-in",
              back, gone, changed);
}
}  // namespace

namespace svr2011 {

// Around the save writer (superstar_threads.cpp's sub_827235D8 hook): the mods' bytes out to their files and the
// stubs in (saving), then back (after).
void PerModSavesForSave(uint8_t* base, bool saving) {
  if (!g_mod_saves_on) {
    if (!saving && g_mod_saves_fold) {  // (turned off: this save holds the mods' data again)
      std::error_code ec;
      fs::remove(ModSavesDir() / ".on", ec);
      g_mod_saves_fold = false;
      REXLOG_INFO("[svr2011] per-mod saves: off - the mods' data is back in the save (their files kept)");
    }
    return;
  }
  for (const auto& m : g_mods) {
    if (m.temp) continue;  // (a fetched mod: OnlineTempForSave)
    const uint32_t si = svr2011::RosterIndex(base, m.slot);
    const auto sr_stub = g_stub_record.find(m.slot);
    if (si >= 512 || sr_stub == g_stub_record.end()) continue;
    const svrfmt::Bytes* sp_stub = nullptr;
    for (const auto& b : g_blank)
      if (b.first == m.slot) {
        sp_stub = &b.second;
        break;
      }
    if (!sp_stub) continue;
    uint8_t* sr = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
    uint8_t* sp = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    uint8_t* se = base + svr2011::RosterEntry(kEntries40, kEntry40Size, si);
    if (saving) {
      if (std::strncmp(reinterpret_cast<const char*>(sr + kFullName), m.name.c_str(), kNameLen - 1)) continue;
      ModSave s{svrfmt::Bytes(sr, sr + kRecordSize), svrfmt::Bytes(sp, sp + kProfileSize),
                svrfmt::Bytes(se, se + kEntry40Size)};
      WriteModSave(m, s);
      std::memcpy(sr, sr_stub->second.data(), kRecordSize);
      std::memcpy(sp, sp_stub->data(), kProfileSize);
      if (const auto e = g_stub_entry.find(m.slot); e != g_stub_entry.end()) std::memcpy(se, e->second.data(), kEntry40Size);
      g_live_while_saving[m.slot] = std::move(s);
    } else if (const auto it = g_live_while_saving.find(m.slot); it != g_live_while_saving.end()) {
      std::memcpy(sr, it->second.record.data(), kRecordSize);
      std::memcpy(sp, it->second.profile.data(), kProfileSize);
      std::memcpy(se, it->second.entry.data(), kEntry40Size);
    }
  }
  if (saving && !g_live_while_saving.empty())
    REXLOG_INFO("[svr2011] per-mod saves: the save is built with {} mod slot(s) as stubs", g_live_while_saving.size());
  if (!saving) g_live_while_saving.clear();
  RefsForSave(base, saving);  // (teams, titles, Universe cards and list: key refs)
}

// After a save is loaded (the sub_8257A218 hook): its fields' mods back from refs.bin - also once while folding back.
void PerModSavesLoaded(uint8_t* base) {
  if (g_mod_saves_on || g_mod_saves_fold) RefsLoaded(base);
}

}  // namespace svr2011

namespace svr2011 {

void CopySuperstarMovies(const std::filesystem::path& movies) {
  if (svr2011::NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  const fs::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "Superstars";
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
    const std::string folder = e.path().filename().string();
    std::string name = folder, movie;
    if (FILE* t = std::fopen((e.path() / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) name = l.substr(5);
        if (l.rfind("movie=", 0) == 0) movie = l.substr(6);
      }
      std::fclose(t);
    }
    const std::string mp4 = !movie.empty() && fs::path(movie).extension() == ".mp4" ? movie : std::string();
    if (!mp4.empty()) movie = fs::path(movie).replace_extension(".bik").string();
    if (!movie.empty() && fs::path(movie).extension() == ".bik" && fs::exists(e.path() / movie, ec))
      CopyIfChanged(e.path() / movie, movies / (MovieName(name, folder) + ".bik"));  // (an .mp4's: its .bik fallback)
    // (an .mp4: its stub and probe now, before the mount - hd_movies.h)
    if (!mp4.empty() && fs::exists(e.path() / mp4, ec))
      svr2011::PrepareHdMovie(movies, MovieName(name, folder) + ".bik", e.path() / mp4);
  }
}

void InstallSuperstarMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* vfs) {
  g_memory = memory;
  // test aid (SVR2011_TEST_PRO_LOG): log when a mod slot's entrance number
  // changes in the CHAR/PRO table (who overwrites a mod's profile, and when)
  if (std::getenv("SVR2011_TEST_PRO_LOG"))
    std::thread([memory] {
      rex::thread::set_current_thread_name("pro log");
      std::vector<uint32_t> last(64, 0xFFFFFFFF);
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint8_t* base = memory->virtual_membase();
        for (size_t i = 0; i < g_mods.size() && i < last.size(); ++i) {
          const uint32_t si = svr2011::RosterIndex(base, g_mods[i].slot);
          if (si >= 512) continue;
          const uint32_t e = Rd16(base + svr2011::RosterEntry(kProfiles, kProfileSize, si) + kEntrance + 0x14);
          if (e != last[i]) {
            REXLOG_INFO("[svr2011] superstar mods: watch: id {} entrance {} -> {}", g_mods[i].slot, last[i], e);
            last[i] = e;
          }
        }
      }
    }).detach();
  g_game = rex::filesystem::GetExecutableFolder();
  LoadMods();
  {
    // (test aid SVR2011_TEST_HIGH_INDEX, big_roster on: mods with record indices from 242 up)
    std::vector<uint32_t> ids;
    for (const auto& m : g_mods) ids.push_back(m.slot);
    svr2011::BigRosterTestMove(memory->virtual_membase(), ids.data(), int(ids.size()));
  }
  StartModSaves();
  LoadAttireMods();
  // the attires' COS pairs and names (string ids kAttireNameIds + mod * 4 + attire)
  uint8_t* b = memory->virtual_membase();
  for (size_t i = 0; i < g_mods.size(); ++i) {
    Mod& m = g_mods[i];
    if (m.attires < 2) continue;
    m.attire_pairs = memory->SystemHeapAlloc(32);
    for (uint32_t a = 0; a < 4; ++a) {
      uint32_t name = 9;  // ORIGINAL ATTIRE
      if (a) {
        const std::string text = m.attire_names[a].empty() ? "ATTIRE " + std::to_string(a + 1) : m.attire_names[a];
        const uint32_t at = memory->SystemHeapAlloc(uint32_t(text.size() + 1));
        std::memcpy(b + at, text.c_str(), text.size() + 1);
        name = kAttireNameIds + uint32_t(i) * 4 + a;
        g_attire_names.push_back({name, at});
      }
      Wr32(b + m.attire_pairs + a * 8, name);
      Wr32(b + m.attire_pairs + a * 8 + 4, 0xFFFFFFFFu);
    }
    REXLOG_INFO("[svr2011] superstar mods: {}: {} attires", m.name, m.attires);
  }
  // attire mods' pairs: their names (string ids kAttireModNameIds + k), always available
  for (size_t k = 0; k < g_attire_mods.size(); ++k) {
    const AttireMod& a = g_attire_mods[k];
    uint32_t& pairs = g_attire_pairs[a.id];
    if (!pairs) {
      pairs = memory->SystemHeapAlloc(32);
      for (uint32_t i = 0; i < 4; ++i) Wr32(b + pairs + i * 8, 9), Wr32(b + pairs + i * 8 + 4, 0xFFFFFFFFu);
    }
    const std::string text = a.name.empty() ? "ATTIRE " + std::to_string(a.attire + 1) : a.name;
    const uint32_t at = memory->SystemHeapAlloc(uint32_t(text.size() + 1));
    std::memcpy(b + at, text.c_str(), text.size() + 1);
    const uint32_t name = kAttireModNameIds + uint32_t(k);
    g_attire_names.push_back({name, at});
    Wr32(b + pairs + a.attire * 8, name);
  }
  // Superstar Threads: the mods whose model is their base's, listed (superstar_threads.h)
  {
    std::vector<svr2011::ThreadsMod> tm;
    for (const auto& m : g_mods)
      if (!m.temp) tm.push_back({m.slot, m.base, m.folder, m.dir / "ch.pac"});
    if (svr2011::BuildThreadsOverlay(memory, g_game, g_game / "Mods" / "SuperstarOverlay", tm))
      g_extra_mounts.push_back("smods:\\threads.pac");
  }
  // The overlay files as smods:\<file>: a device of their own, made after they
  // are written (a host device lists its folder once, so GAME: would not see
  // the files made this run).
  if ((!g_mods.empty() || !g_extra_mounts.empty()) && vfs) {
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\SUPERSTARMODS",
                                                                    g_game / "Mods" / "SuperstarOverlay", true);
    if (!device->Initialize() || !vfs->RegisterDevice(std::move(device)) ||
        !vfs->RegisterSymbolicLink("smods:", "\\SUPERSTARMODS")) {
      REXLOG_WARN("[svr2011] superstar mods: could not mount Mods/SuperstarOverlay");
      g_mods.clear();
    }
  }
  if (!g_mods.empty()) LoadBadges();
  REXLOG_INFO("[svr2011] superstar mods: {}", g_mods.size());
}

std::vector<uint32_t> SuperstarModIds(bool with_online) {
  std::vector<uint32_t> ids;
  for (const auto& m : g_mods)
    if (with_online || !m.temp) ids.push_back(m.slot);
  return ids;
}

void OnlineTempForSave(uint8_t* base, bool saving) {
  // (the bytes to put back: the save image about to be rebuilt - the last one loaded or written, which never has a
  // fetched mod in it; the boot's save load doesn't come through sub_8257A218's hook)
  if (saving) TempFromSave(base);
  // Test aid: SVR2011_TEST_NO_TEMP_BLANK=1 - the fetched mods go into the save as they are (the check's control)
  static const bool off = std::getenv("SVR2011_TEST_NO_TEMP_BLANK") != nullptr;
  if (off) {
    if (saving) REXLOG_WARN("[svr2011] superstar mods: test - the save is built WITH the fetched mods");
    return;
  }
  for (const auto& [id, saved] : g_temp_saved) {
    const uint32_t si = svr2011::RosterIndex(base, id);
    if (si >= 512) continue;
    uint8_t* r = base + svr2011::RosterEntry(kRecords, kRecordSize, si);
    uint8_t* p = base + svr2011::RosterEntry(kProfiles, kProfileSize, si);
    if (saving) {
      std::vector<uint8_t> live(r, r + kRecordSize);
      live.insert(live.end(), p, p + kProfileSize);
      g_temp_live[id] = std::move(live);
      std::memcpy(r, saved.data(), kRecordSize), std::memcpy(p, saved.data() + kRecordSize, kProfileSize);
    } else if (const auto it = g_temp_live.find(id); it != g_temp_live.end()) {
      std::memcpy(r, it->second.data(), kRecordSize), std::memcpy(p, it->second.data() + kRecordSize, kProfileSize);
    }
  }
  if (saving && !g_temp_saved.empty())
    REXLOG_INFO("[svr2011] superstar mods: the save is built without the {} fetched mod(s)", g_temp_saved.size());
  if (!saving) g_temp_live.clear();
}

void SetSuperstarModsSaves(const fs::path& saves) { g_save_file = saves / "SaveData.dat"; }

bool IsOnlineTempSuperstar(uint32_t id) {
  const Mod* m = ModOf(id);
  return m && m->temp;
}

std::vector<SuperstarModEntrance> SuperstarModEntrances() {
  std::vector<SuperstarModEntrance> out;
  for (const auto& m : g_mods) {
    if (m.temp) continue;
    SuperstarModEntrance e{m.slot, m.name.empty() ? m.folder : m.name, {}, m.base};
    for (int n : {m.entrance, m.attire_entrance[0], m.attire_entrance[1], m.attire_entrance[2], m.attire_entrance[3]})
      if (n > 0 && std::find(e.numbers.begin(), e.numbers.end(), n) == e.numbers.end()) e.numbers.push_back(n);
    if (!e.numbers.empty()) out.push_back(std::move(e));
  }
  return out;
}

bool IsSuperstarMod(uint32_t id) { return ModOf(id) != nullptr; }
std::string SuperstarModFolder(uint32_t id) {
  const Mod* m = ModOf(id);
  return m ? m->folder : std::string();
}
void SuperstarModHandovers(uint8_t* base) { RedoHandovers(base, false); }
std::string SuperstarModLoading(uint32_t id) {
  const Mod* m = ModOf(id);
  if (!m) return {};
  std::error_code ec;
  const fs::path p = m->loading.empty() ? m->dir / "load.dds" : m->loading;
  return fs::is_regular_file(p, ec) ? p.string() : std::string();
}
uint32_t SuperstarModBase(uint32_t id) {
  const Mod* m = ModOf(id);
  return m ? m->base : 0;
}
uint32_t SuperstarModIdOf(const std::string& folder) {
  for (const auto& m : g_mods)
    if (!m.temp && m.folder == folder) return m.slot;
  return 0;
}
fs::path SuperstarModsSavesDir() { return g_save_file.empty() ? fs::path() : g_save_file.parent_path(); }

void AddOverlayMount(const std::string& file) { g_extra_mounts.push_back("smods:\\" + file); }

// Attire mods replacing an attire the game's own pac has: the game finds the
// first entry with a name, its own, so a mounted guest pac isn't used. The
// mod's model goes into an overlay copy of the game's pac instead, as a move
// pack's pacentry (move_packs.cpp): one generated pack per such mod.
void PrepareAttirePacks(const fs::path& game, const fs::path& dlc_root) {
  g_attire_dlc = dlc_root;
  std::error_code ec;
  const fs::path dir = game / "Mods" / "Attires", out = game / "Mods" / "SuperstarOverlay" / "attirepacks";
  std::set<std::string> keep;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec) && !fs::exists(e.path() / "disabled", ec) && fs::exists(e.path() / "manifest.txt", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  std::set<std::pair<uint32_t, uint32_t>> taken;
  for (const auto& f : folders) {
    std::string type, which, model = "attire.pac";
    uint32_t id = 0;
    if (FILE* t = std::fopen((f / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "type") type = v;
        else if (k == "superstar") id = uint32_t(std::atoi(v.c_str()));
        else if (k == "attire") which = v;
        else if (k == "model") model = v;
      }
      std::fclose(t);
    }
    const int n = std::atoi(which.c_str());
    if (type != "attire" || ((id < 100 || id > 321) && !DlcSuperstar(id)) || n < 1 || n > 4) continue;
    const uint32_t a = uint32_t(n - 1);
    char stock[32];
    std::snprintf(stock, sizeof stock, "ch%03u.pac", id);
    // the pac with that attire: the game's (pac list name ch\chNNN.pac), else a DLC package's (dlc\ch\ch<id>_dlcNN.pac:
    // move_packs' DLC copies - the package's catalogs say where the game reads it, a guest pac's same name loses)
    std::string target;
    if (id >= 100 && (AttireMask(game / "pac" / "ch" / stock, id) >> a & 1))
      target = std::string("ch\\") + stock;
    else
      for (const auto& p : DlcCharacterPacs())
        if (p.first == id && (AttireMask(p.second, id) >> a & 1)) {
          target = "dlc\\ch\\" + p.second.filename().string();
          for (auto& c : target) c = char(std::tolower(static_cast<unsigned char>(c)));
          break;
        }
    if (target.empty()) continue;  // (a new attire: mounted as a guest pac, LoadAttireMods)
    if (!fs::exists(f / model, ec)) continue;  // (no model: an entrance only, LoadAttireMods)
    if (!taken.insert({id, a}).second) continue;
    const fs::path pd = out / f.filename();
    keep.insert(f.filename().string());
    char name[16];
    std::snprintf(name, sizeof name, "%06u%u2", id, a);
    const std::string line = "pacentry " + target + " EMD " + name + " entry.bin\n";
    {  // (up to date: the same mod files and the same target - a DLC installed or gone since changes it)
      std::ifstream was(pd / "pack.txt", std::ios::binary);
      const std::string had((std::istreambuf_iterator<char>(was)), std::istreambuf_iterator<char>());
      if (had == line && Current(pd / "pack.txt", {f / model, f / "manifest.txt"})) continue;
    }
    svrfmt::Bytes d;
    svrfmt::Epac e;
    std::string notes;
    if (!svrfmt::ReadFile((f / model).string(), d) || !FitModelBudget(d, notes) || !svrfmt::EpacRead(d, e)) {
      REXLOG_WARN("[svr2011] attire mods: {}: {} is not a usable character model pac{}", f.filename().string(), model,
                  notes.empty() ? "" : " (over the match's vertex budget:" + notes + ")");
      continue;
    }
    const svrfmt::EpacEntry* first = nullptr;
    for (const auto& g : e.groups)
      if (g.type == "EMD ")
        for (const auto& en : g.entries)
          if (en.name.size() >= 8 && en.name[7] == '2' && (!first || en.name[6] < first->name[6])) first = &en;
    if (!first) continue;
    fs::create_directories(pd, ec);
    if (!svrfmt::WriteFile((pd / "entry.bin").string(), svrfmt::Unpack(first->data)) ||
        !svrfmt::WriteFile((pd / "pack.txt").string(), svrfmt::Bytes(line.begin(), line.end())))
      continue;
    MarkCurrent(pd / "pack.txt", {f / model, f / "manifest.txt"});
    REXLOG_INFO("[svr2011] attire mods: {}: {} attire {} through the pac overlay ({})", f.filename().string(), target, n,
                name);
  }
  for (const auto& e : fs::directory_iterator(out, ec))  // (packs of mods gone or switched off)
    if (e.is_directory(ec) && !keep.count(e.path().filename().string())) fs::remove_all(e.path(), ec);
}

uint32_t SuperstarModString(uint32_t id) {
  for (const auto& n : g_attire_names)
    if (n.first == id) return n.second;
  return 0;
}

}  // namespace svr2011

// Test aid: SVR2011_TEST_MOVE_LOG=<id>,<id>,... logs (once per key and
// result) each motion lookup of those move ids - sub_823941F8(banks, id, x,
// y, out index): key id<<16 | x<<8 | y, 0 when no loaded bank has it. Shows
// whether a ported move's motions are found in a match.
namespace {
const std::vector<uint32_t>& MoveLogIds() {
  static const std::vector<uint32_t> ids = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SVR2011_TEST_MOVE_LOG"))
      for (const char* q = e; *q;) {
        if (const long n = std::strtol(q, nullptr, 10); n > 0) v.push_back(uint32_t(n));
        while (*q && *q != ',') ++q;
        if (*q == ',') ++q;
      }
    return v;
  }();
  return ids;
}
}  // namespace
// Test aid: SVR2011_TEST_MOTION_SWAP=<id>=<new id>[,...] - a lookup of a motion
// with that id (any x, body tracks y 0/1) gets the new id's (x 0) instead: the
// select screen's model, or a stance, plays a chosen motion (an in-game
// motion viewer with a fixed camera).
const std::vector<std::pair<uint32_t, uint32_t>>& MotionSwaps() {
  static const std::vector<std::pair<uint32_t, uint32_t>> v = [] {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (const char* e = std::getenv("SVR2011_TEST_MOTION_SWAP"))
      for (const char* q = e; *q;) {
        char* end = nullptr;
        const unsigned long a = std::strtoul(q, &end, 10);
        if (end == q || *end != '=') break;
        const unsigned long b = std::strtoul(end + 1, &end, 10);
        out.push_back({uint32_t(a), uint32_t(b)});
        for (q = end; *q == ',' || *q == ' ';) ++q;
      }
    return out;
  }();
  return v;
}
// A bank whose keys are no longer all in order: a move pack's new YMBs motions
// go after the stock entries (move_packs.cpp: the game keeps YMBs motions by
// their place), and the game's binary search (sub_823941F8) then missed stock
// keys whose search path ran into that tail - with Sabu or The Sandman
// installed, Surfboard Stretch (21201) wasn't found: ground submissions froze
// in matches and in the Move-Set preview (camera left on the apron). A miss in
// such a bank is looked up again: binary search over the sorted part, then the
// tail one by one. Returns the bank's +272 (as the game's) and *index; 0 if
// none has the key.
// (Not covered: sub_82394740, a second bank search at motion start - no tail
// fallback; it only reads a per-slot byte, 0 for m.pac, so harmless so far.)
namespace {
uint8_t* GuestAt(uint8_t* base, uint32_t a) { return base + a + REX_PHYS_HOST_OFFSET(a); }  // (as the game's loads)
struct BankOrder { uint32_t count = 0, last = 0, sorted = 0; };  // sorted: entries in order from 0
std::mutex g_order_mutex;
std::unordered_map<uint32_t, BankOrder> g_order;  // bank -> its order (checked again when count or last key change)
uint32_t SortedPrefix(uint8_t* base, uint32_t bank, uint32_t count) {
  const uint8_t* e = GuestAt(base, bank + 276);
  const uint32_t last = Rd32(e + (count - 1) * 16);
  std::lock_guard lock(g_order_mutex);
  BankOrder& o = g_order[bank];
  if (o.count == count && o.last == last && o.sorted) return o.sorted;
  uint32_t n = 1;
  while (n < count && Rd32(e + n * 16) > Rd32(e + (n - 1) * 16)) ++n;
  o = {count, last, n};
  return n;
}
// A bank in a list that can't be one (2.1.6 report 20261030-212436-89cfee: every match crashed at its start - a
// slot's "bank" said 0xB7C4EFFF entries and the game's search read its last one 2 GB away, guest 0x21BAD310; a heavy
// install, 53 packs, updated in place from 2.1.3): its count over 65535. -> the bank's +0 adjusted (as the game does)
// or 0 when the slot is empty or fine; *bad true when it isn't a bank.
uint32_t BankAt(uint8_t* base, uint32_t banks, uint32_t k, bool* bad) {
  constexpr uint32_t kYmks = 0x734B4D59, kYmbs = 0x73424D59;  // (as the game reads them)
  *bad = false;
  uint32_t b = Rd32(GuestAt(base, banks + 4 * k));
  if (!b) return 0;
  const uint32_t magic = Rd32(GuestAt(base, b));
  if (magic != kYmks && magic != kYmbs) b -= 16;
  const uint32_t count = Rd32(GuestAt(base, b + 272));
  if (!count) return 0;
  // (only the count: packs leave banks out of order - their YMBs motions after the stock ones - and a key's id can
  // carry the 0x8000 flag, so key checks threw out good banks and their motions went missing)
  if (count > 65535) *bad = true;
  return b;
}

// Test aids: SVR2011_TEST_BAD_BANK=1 - each search goes over a copy of its list with a fake bank whose last entry
// is the report's fault address in its first empty slot - the game's own list untouched;
// SVR2011_TEST_BANK_GUARD_OFF=1 - the game's searches only (the crash).
bool BankGuardOff() {
  static const bool off = std::getenv("SVR2011_TEST_BANK_GUARD_OFF") != nullptr;
  return off;
}
uint32_t TestListWithBadBank(uint8_t* base, uint32_t banks) {  // -> the copy to search instead, or banks
  static const bool on = std::getenv("SVR2011_TEST_BAD_BANK") != nullptr;
  if (!on || !banks || !g_memory) return banks;
  static uint32_t fake = 0;
  thread_local uint32_t copy = 0;  // (each thread its own: searches run on several at once)
  {
    static std::mutex mu;
    std::lock_guard lock(mu);
    if (!fake && (fake = g_memory->SystemHeapAlloc(512)) != 0) {
      uint8_t* f = GuestAt(base, fake);
      std::memset(f, 0, 512);
      // (its count puts its last entry on the report's fault, guest 0x21BAD310 - unmapped, as there)
      const uint32_t n = (0x21BAD314u - (fake + 276)) / 16 + 1;
      const uint8_t magic[4] = {0x73, 0x4B, 0x4D, 0x59}, count[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
      std::memcpy(f, magic, 4), std::memcpy(f + 272, count, 4);  // (magic read as 0x734B4D59)
      REXLOG_INFO("[svr2011] motion banks: test - a fake bank at {:08X} (count {:08X})", fake, n);
    }
  }
  if (!copy) copy = g_memory->SystemHeapAlloc(512);
  if (!fake || !copy) return banks;
  uint8_t* c = GuestAt(base, copy);
  std::memcpy(c, GuestAt(base, banks), 272 + 68);  // (the 68 slots, then their bytes)
  for (uint32_t k = 0; k < 68; ++k)
    if (!Rd32(c + 4 * k)) {
      c[4 * k] = uint8_t(fake >> 24), c[4 * k + 1] = uint8_t(fake >> 16), c[4 * k + 2] = uint8_t(fake >> 8), c[4 * k + 3] = uint8_t(fake);
      return copy;
    }
  return banks;
}

// Any such bank in the list - the game's own search isn't safe with it. -> its slot (and what it held), or -1.
int BadBankSlot(uint8_t* base, uint32_t banks, uint32_t* bank = nullptr, uint32_t* count = nullptr) {
  for (uint32_t k = 0; k < 68; ++k) {
    bool bad;
    const uint32_t b = BankAt(base, banks, k, &bad);
    if (bad) {
      if (bank) *bank = b;
      if (count) *count = Rd32(GuestAt(base, b + 272));
      return int(k);
    }
  }
  return -1;
}

uint32_t FindInUnsortedBanks(uint8_t* base, uint32_t banks, uint32_t key, uint32_t out, bool all = false,
                             uint32_t* slot = nullptr) {
  for (uint32_t k = 0; k < 68; ++k) {
    bool bad;
    const uint32_t b = BankAt(base, banks, k, &bad);
    if (!b || bad) continue;
    const uint32_t count = Rd32(GuestAt(base, b + 272));
    if (!all && count > 20000) continue;
    const uint32_t sorted = SortedPrefix(base, b, count);
    if (sorted >= count && !all) continue;  // (in order: the game's search was right)
    const uint8_t* e = GuestAt(base, b + 276);
    int lo = 0, hi = int(sorted) - 1, at = -1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const uint32_t v = Rd32(e + mid * 16);
      if (v == key) { at = mid; break; }
      if (v < key) lo = mid + 1; else hi = mid - 1;
    }
    for (uint32_t i = sorted; at < 0 && i < count; ++i)
      if (Rd32(e + i * 16) == key) at = int(i);
    if (at < 0) continue;
    if (out) {
      uint8_t* q = GuestAt(base, out);
      q[0] = uint8_t(uint32_t(at) >> 24), q[1] = uint8_t(uint32_t(at) >> 16), q[2] = uint8_t(uint32_t(at) >> 8), q[3] = uint8_t(at);
    }
    if (slot) *slot = k;
    return b + 272;
  }
  return 0;
}
}  // namespace

// Test aid: SVR2011_TEST_BANK_COPY=1 - each bank the match load copies and trims into heap 42 (sub_82163808: the bank,
// dest, trimmed size back): where each lands and its size, for what overwrites one (report d10f64).
REX_EXTERN(__imp__sub_82163808);
REX_HOOK_RAW(sub_82163808) {
  static const bool log = std::getenv("SVR2011_TEST_BANK_COPY") != nullptr;
  const uint32_t src = ctx.r4.u32, dest = ctx.r5.u32, entry = ctx.r6.u32;
  const uint32_t table = Rd32(GuestAt(base, ctx.r3.u32 + 48));
  __imp__sub_82163808(ctx, base);
  // (what this match keeps past the 36 MB buffer's end would land on heap 42's other blocks: said once)
  if (table) {
    const uint32_t set = table + 352 * (Rd32(GuestAt(base, table + 704)) ^ 1), buffer = Rd32(GuestAt(base, set + 344));
    const uint32_t size = Rd32(GuestAt(base, set + 336));
    static bool told = false;
    if (!told && dest + ctx.r3.u32 > buffer + size) {
      told = true;
      REXLOG_ERROR("[svr2011] motion banks: this match keeps more than the {} MB motion buffer holds (bank at {:08X}, {} bytes)",
                   size >> 20, dest, ctx.r3.u32);
    }
  }
  if (log)
    REXLOG_INFO("[svr2011] bank copy: {:08X} ({} bytes) -> {:08X}, {} bytes back, count {}", src, entry, dest, ctx.r3.u32,
                Rd32(GuestAt(base, dest + 272)));
}

// The second bank search, at a motion's start: sub_82394740(banks, id, x, y) -> the per-slot byte at banks + 272 + k
// of the bank k that has the key (0 if none) - it reads each bank as sub_823941F8 does, so a slot that isn't a bank
// (BankAt) crashed it the same way: then the search here over the good banks.
REX_EXTERN(__imp__sub_82394740);
REX_HOOK_RAW(sub_82394740) {
  const uint32_t banks = TestListWithBadBank(base, ctx.r3.u32);
  ctx.r3.u64 = banks;
  if (!banks || BankGuardOff() || BadBankSlot(base, banks) < 0) {
    __imp__sub_82394740(ctx, base);
    return;
  }
  const uint32_t key = (ctx.r4.u32 & 0xFFFF) << 16 | (ctx.r5.u32 & 0xFF) << 8 | (ctx.r6.u32 & 0xFF);
  uint32_t slot = 0;
  ctx.r3.u64 = FindInUnsortedBanks(base, banks, key, 0, true, &slot) ? *GuestAt(base, banks + 272 + slot) : 0u;
}

REX_EXTERN(__imp__sub_823941F8);
REX_HOOK_RAW(sub_823941F8) {
  const auto& ids = MoveLogIds();
  uint32_t id = ctx.r4.u32 & 0xFFFF, x = ctx.r5.u32 & 0xFF;
  const uint32_t y = ctx.r6.u32 & 0xFF;
  for (const auto& [from, to] : MotionSwaps())
    if (id == from && y <= 1) {
      id = to, x = 0;
      ctx.r4.u64 = to;
      ctx.r5.u64 = 0;
    }
  const uint32_t banks = TestListWithBadBank(base, ctx.r3.u32), out = ctx.r7.u32;
  ctx.r3.u64 = banks;
  // (a slot that can't be a bank - BankAt: the search over the good banks here instead of the game's, which read
  // past it and crashed every match; the bank logged once)
  uint32_t bad_bank = 0, bad_count = 0;
  if (const int bad = banks && !BankGuardOff() ? BadBankSlot(base, banks, &bad_bank, &bad_count) : -1; bad >= 0) {
    static std::mutex bmu;
    static std::vector<uint32_t> told;
    {
      std::lock_guard lock(bmu);
      if (told.size() < 16 && std::find(told.begin(), told.end(), bad_bank) == told.end()) {
        told.push_back(bad_bank);
        REXLOG_ERROR("[svr2011] motion banks: slot {} of list {:08X} isn't a motion bank (at {:08X}, count {}) - left out "
                     "of the search (a damaged pac's bank?)", bad, banks, bad_bank, bad_count);
      }
    }
    ctx.r3.u64 = FindInUnsortedBanks(base, banks, id << 16 | x << 8 | y, out, true);
  } else {
    __imp__sub_823941F8(ctx, base);
  }
  if (!ctx.r3.u32 && banks) {
    static const bool off = std::getenv("SVR2011_TEST_YMBS_SEARCH_OFF") != nullptr;  // (test aid: the game's search only)
    if (!off) ctx.r3.u64 = FindInUnsortedBanks(base, banks, id << 16 | x << 8 | y, out);
  }
  // A weapon's motions go by its grip (x = the settings table's type) and a match loads only some grips (plain matches
  // MOT/WEAT, Extreme Rules MOT/WEAP): none for a sack in a plain match (a107ed), a fire extinguisher or dumbbell in
  // Extreme Rules; and no bank has the pull from under the ring (15481) for a drink cup, dumbbell, baseball bat or tire
  // (d88915). With no motion the wrestler stood there until someone hit him. Those play the steel chair's (grip 3, in
  // every match): the game's own weapons' probes for motions they don't have stay misses.
  static const bool grip_off = std::getenv("SVR2011_TEST_GRIP_FALLBACK_OFF") != nullptr;
  if (!ctx.r3.u32 && banks && out && !grip_off && y <= 1 && x >= 2 && x <= 63 && x != 3 && id >= 14000 && id < 16000) {
    auto find = [&](uint32_t want_id, uint32_t want_x, uint32_t want_y, uint32_t want_out) {
      ctx.r3.u64 = banks, ctx.r4.u64 = want_id, ctx.r5.u64 = want_x, ctx.r6.u64 = want_y, ctx.r7.u64 = want_out;
      __imp__sub_823941F8(ctx, base);
      return ctx.r3.u32;
    };
    if (BadBankSlot(base, banks) < 0 && (id == 15481 || !find(15080, x, 0, out))) {
      if (find(id, 3, y, out)) {
        static std::atomic<uint64_t> told{0};
        if (!(told.fetch_or(1ull << x) >> x & 1))
          REXLOG_INFO("[svr2011] weapons: no motion {} for grip {} in this match - the steel chair's instead", id, x);
      }
    } else {
      ctx.r3.u64 = 0;
    }
    ctx.r4.u64 = id, ctx.r5.u64 = x, ctx.r6.u64 = y, ctx.r7.u64 = out;
  }
  if (banks && y <= 1) svr2011::MatchBoxMotionLooked(id, ctx.r3.u32 != 0);  // (a body track found or not: match_box.h)
  // Test aid: SVR2011_TEST_BANK_DUMP=<id>[,<id>] - at a lookup of that id (x 0, y 0), once per id and bank
  // list: each bank in the list (its slot, magic, count, first / last key, whether the key is in its directory)
  static const std::vector<uint32_t> dump_ids = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SVR2011_TEST_BANK_DUMP"))
      for (const char* q = e; *q;) {
        if (const long n = std::strtol(q, nullptr, 10); n > 0) v.push_back(uint32_t(n));
        while (*q && *q != ',') ++q;
        if (*q == ',') ++q;
      }
    return v;
  }();
  if (!dump_ids.empty() && x == 0 && y == 0 && banks &&
      std::find(dump_ids.begin(), dump_ids.end(), id) != dump_ids.end()) {
    static std::mutex dmu;
    static std::vector<uint64_t> dumped;
    std::lock_guard lock(dmu);
    if (std::find(dumped.begin(), dumped.end(), uint64_t(id) << 32 | banks) == dumped.end()) {
      dumped.push_back(uint64_t(id) << 32 | banks);
      const uint32_t key = id << 16;
      std::string s;
      for (uint32_t k = 0; k < 68; ++k) {
        uint32_t b = Rd32(GuestAt(base, banks + 4 * k));
        if (!b) continue;
        const uint32_t magic = Rd32(GuestAt(base, b));
        if (magic != 0x734B4D59 && magic != 0x73424D59) b -= 16;
        const uint32_t count = Rd32(GuestAt(base, b + 272));
        if (!count || count > 20000) {
          s += fmt::format(" [{} {:08X} count {}]", k, b, count);
          continue;
        }
        const uint8_t* e = GuestAt(base, b + 276);
        bool has = false;
        for (uint32_t i = 0; i < count && !has; ++i) has = Rd32(e + i * 16) == key;
        s += fmt::format(" [{} {:08X} {:08X} n {} {:08X}..{:08X}{}", k, b, Rd32(GuestAt(base, b)), count, Rd32(e),
                         Rd32(e + (count - 1) * 16), has ? " HAS" : "");
        if (Rd32(e) <= key && key <= Rd32(e + (count - 1) * 16)) {  // (the key's neighbours: where it would be)
          uint32_t at = 0;
          while (at < count && Rd32(e + at * 16) < key) ++at;
          s += fmt::format(" at {}:", at);
          for (uint32_t i = at >= 3 ? at - 3 : 0; i < count && i < at + 3; ++i)
            s += fmt::format(" {:08X}", Rd32(e + i * 16));
        }
        s += "]";
      }
      REXLOG_INFO("[svr2011] bank dump: {} x 0 y 0 {} - list {:08X}:{}", id, ctx.r3.u32 ? "found" : "NOT FOUND", banks, s);
      if (const uint32_t b0 = Rd32(GuestAt(base, banks)); b0 && std::getenv("SVR2011_TEST_BANK_KEYS")) {  // (bank 0's keys, all)
        uint32_t b = b0;
        if (Rd32(GuestAt(base, b)) != 0x734B4D59 && Rd32(GuestAt(base, b)) != 0x73424D59) b -= 16;
        const uint32_t count = std::min<uint32_t>(Rd32(GuestAt(base, b + 272)), 20000);
        std::string keys;
        for (uint32_t i = 0; i < count; ++i) keys += fmt::format(" {:08X}", Rd32(GuestAt(base, b + 276) + i * 16));
        REXLOG_INFO("[svr2011] bank keys: {:08X} n {}:{}", b, count, keys);
      }
    }
  }
  static const bool all = [] { const char* e = std::getenv("SVR2011_TEST_MOVE_LOG"); return e && !std::strcmp(e, "all"); }();
  if (all) {  // (SVR2011_TEST_MOVE_LOG=all: every lookup once, found or not)
    static std::mutex amu;
    static std::vector<uint32_t> atold;
    const uint32_t k = id << 16 | x << 8 | y;
    std::lock_guard lock(amu);
    if (std::find(atold.begin(), atold.end(), k) == atold.end()) {
      atold.push_back(k);
      REXLOG_INFO("[svr2011] move log: lookup {} x {} y {} {}", id, x, y, ctx.r3.u32 ? "found" : "NOT FOUND");
    }
    return;
  }
  // (SVR2011_TEST_MOVE_LOG=missing: every body-track motion - y 0 / 1 - no
  // loaded bank has, for any id: a move that snaps instead of playing)
  static const bool missing = [] { const char* e = std::getenv("SVR2011_TEST_MOVE_LOG"); return e && !std::strcmp(e, "missing"); }();
  if (missing ? (ctx.r3.u32 || y > 1) : (ids.empty() || std::find(ids.begin(), ids.end(), id) == ids.end())) return;
  static std::mutex mu;
  static std::vector<uint64_t> told;
  const uint64_t k = uint64_t(id) << 17 | x << 9 | y << 1 | (ctx.r3.u32 ? 1 : 0);
  std::lock_guard lock(mu);
  if (std::find(told.begin(), told.end(), k) != told.end()) return;
  told.push_back(k);
  REXLOG_INFO("[svr2011] move log: motion {} x {} y {} {}", id, x, y, ctx.r3.u32 ? "found" : "NOT FOUND");
}
namespace svr2011 {
bool LoaderCacheForcedFull(uint32_t size);   // (model_mem.cpp)
uint32_t LoaderCacheFallback(uint32_t size);  // (model_mem.cpp)
}  // namespace svr2011
// (with SVR2011_TEST_MOVE_LOG: allocations that fail, and what sub_8258EB90
// - a data file read into memory - asked for)
REX_EXTERN(__imp__sub_8269BBA0);
REX_HOOK_RAW(sub_8269BBA0) {
  const uint32_t size = ctx.r3.u32, lr = uint32_t(ctx.lr);
  // The file loader's cache (sub_826BA928's retry loop, model_mem.cpp): a file that never fits takes the fallback
  // memory instead of retrying forever (the 21 Oct hang after the entrances).
  if (lr == 0x826BA9A8) {
    if (svr2011::LoaderCacheForcedFull(size))
      ctx.r3.u64 = 0;
    else
      __imp__sub_8269BBA0(ctx, base);
    if (!ctx.r3.u32) ctx.r3.u64 = svr2011::LoaderCacheFallback(size);
    return;
  }
  __imp__sub_8269BBA0(ctx, base);
  // Big models: a wrestler's model heap full copying its blood overlays (sub_826B6BA8; 2K17 Asuka crashed there) -
  // the block from the fallback memory, given back by the delete hook (CawHeapFree, frame_rate.cpp)
  if (!ctx.r3.u32 && size && lr == 0x826B6C4C && svr2011::BigModelMemory())
    if (const uint32_t p = svr2011::FallbackBlock(size)) {
      ctx.r3.u64 = p;
      static int told = 0;
      if (told++ < 3) REXLOG_INFO("[svr2011] model memory: a model heap full - {} bytes from the fallback memory", size);
    }
  // (the stage's heap full - an arena with more 3D detail than the game's own: caw_limits.h kStageHeap; new takes
  // its heap's blocks without sub_8269A8D8, whose hook does the same)
  if (!ctx.r3.u32 && size && svr2011::BigModelMemory()) {
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    if (heap == svr2011::kStageHeap)
      if (const uint32_t p = svr2011::FallbackBlock(size, true)) {
        ctx.r3.u64 = p;
        static int told = 0;
        if (told++ < 5 || told % 200 == 0)
          REXLOG_INFO("[svr2011] model memory: the stage's heap ({}) full - new {} bytes from the fallback memory ({} "
                      "so)", heap, size, told);
      }
  }
  if (!MoveLogIds().empty() && !ctx.r3.u32) REXLOG_WARN("[svr2011] move log: alloc {} bytes failed (lr {:08X})", size, lr);
  if (svr2011::MemTraceOn() && ctx.r3.u32) {  // (test aid: mem_trace.h - which heap gave it)
    const auto saved = ctx;
    sub_8269A478(ctx, base);
    const uint32_t heap = ctx.r3.u32;
    ctx = saved;
    svr2011::MemTraceNew(ctx.r3.u32, size, heap, lr);
  }
}
REX_EXTERN(__imp__sub_8258EB90);
REX_HOOK_RAW(sub_8258EB90) {
  if (!MoveLogIds().empty()) REXLOG_INFO("[svr2011] move log: data read {:08X} size {}", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8258EB90(ctx, base);
}

// Test aid (SVR2011_TEST_MOVE_LOG set): the per-superstar match bank's move id
// list (sub_8224D8C8(obj): u32 ids at obj+116, sorted, up to 267 read, 32767
// closing it) - how full it is.
REX_EXTERN(__imp__sub_8224CFD0);
REX_HOOK_RAW(sub_8224CFD0) {
  static const bool on = std::getenv("SVR2011_TEST_MOVE_LOG") != nullptr;
  if (on) {
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_8224CFD0(ctx, base);
    uint32_t n = 0, last = 0, end = 0;
    for (; n < 400; ++n) {
      const uint32_t v = Rd32(base + obj + 116 + 4 * n);
      if (v == 32767) { end = 1; break; }
      if (v == 0xDEADBEEF) break;
      last = v;
    }
    REXLOG_INFO("[svr2011] move log: match bank id list {} entries{} (last {}), read limit 267", n,
                end ? " + 32767" : " (no 32767)", last);
    return;
  }
  __imp__sub_8224CFD0(ctx, base);
}
