// Media mods (arenas branch): packs in <game>/Mods/Media/<pack>/ (a
// manifest.txt, type=media; a "disabled" file turns one off) that replace
// the game's own media. Later packs (by folder name) win. Manifest lines
// (<id> = a character id, the files beside the manifest):
//
//   video.<id>=<file.bik>    that superstar's entrance (titantron) video
//                            (or <file.mp4>: played HD where the platform
//                            decodes it, else <file.bik> beside it; hd_movies.h)
//   theme.<id>=<song>        his entrance theme (.mp3 / .m4a / .wav / ...)
//   theme_gain.<id>=<dB>     that theme's level, -20..+6 (the Mod Maker's
//                            loudness match; 0 / none: as the file is)
//   render.<id>=<file.dds>   his select / menu renders: 512 x 512 DXT5
//   bust.<id>=<file.dds>     ... the 256 x 256 bust
//   icon.<id>=<file.dds>     ... the 64 x 64 face icon
//   arena.<nn>=<bgNN.pac>    arena nn with its own screen pictures (the
//                            Mod Maker retextures the screens' flip-book)
//   victory_theme.<id>=<song> his music when he wins, in place of his theme
//                            (docs/VICTORY_SCENES.md 5)
//   victory_theme_gain.<id>=<dB>  that track's level, -20..+6
//   sound.<event>=<file>     a game sound, by its event name without "Play_"
//                            (Menu_Music, MUS_0161_0_0, SVR10_Chant_Sena_001,
//                            elbow_mid_0231_0_0 ...; SVR2011_TEST_NAMECALL=1
//                            logs every event the game posts)
//   menu_music=<file>        = sound.Menu_Music
//
// How each one gets in:
//  - videos: copied into Custom Movies (user movie ids, user_movies.h);
//    themes: copied into the Music folder (a USER PLAYLIST). When a match
//    sets up its wrestlers, each one's copy of his profile (the match record,
//    sub_828B5BC0(record, desc): desc+0 the id, profile at record+260) gets
//    the replacement in its entrance block (+0x1C0: music +0x10 = 254 with the
//    playlist's name at +0xCC, movie +0x12) - only the match's copy, the
//    roster and the saves keep the game's;
//  - renders: an overlay pac (Mods/SuperstarOverlay/media.pac, mounted with
//    the superstar mods' files) with them as RSFA / RSFB / RSFC "%04d" (every
//    attire key) and the face icons' bank as RENU/SSFD; the game's file name
//    resolver (superstar_mods.cpp's sub_826B87B0 hook) is pointed at those
//    names instead of SSFA / SSFB / SSFC / MENU/SSFD (scratchpad re_renders);
//  - arenas: arena_mods' default for that arena (SetArenaDefault);
//  - sounds: every game sound is an event posted by name (sub_82BEC030(name,
//    object, ...), scratchpad re_audio). A replaced "Play_<event>" isn't
//    posted: the file plays on a host voice instead (music.h HostSound*),
//    tied to the event's game object - its stop events ("Stop_*" on that
//    object, the global Stop_All_* / Stop_Menu_Music / chants ones), pause
//    and resume (Pause_start / Pause_Resume, Pause_ALL_Audio /
//    Resume_ALL_Audio) and the object's release (sub_82BB6898) stop or pause
//    it, and the game's "is the object still sounding" (sub_82BB69E8) says
//    yes while it plays (else music players think it ended at once). Volume:
//    the game's music / SFX / voice settings (*(0x82EC4C18)+36: floats).
#include "big_roster.h"
#include "media_mods.h"
#include "no_mods.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"
#include "arena_mods.h"
#include "music.h"
#include "hd_movies.h"
#include "superstar_mods.h"
#include "user_movies.h"

namespace fs = std::filesystem;

namespace {

struct Pack {
  fs::path folder;
  std::string name;
  std::vector<std::pair<std::string, std::string>> lines;  // key -> value
};

rex::memory::Memory* g_memory = nullptr;
std::map<uint32_t, int> g_videos;          // character id -> user movie id
std::map<uint32_t, std::string> g_themes;  // character id -> USER PLAYLIST name
std::map<std::string, uint32_t> g_alias;   // virtual name (no leading '/') -> guest name ("/RSFA/0161")
std::map<std::string, fs::path> g_sounds;  // event (without "Play_") -> file
struct Voice {
  int handle;
  uint32_t object;
  int category;  // 0 music, 1 sfx, 2 voice / crowd
  bool paused;
};
std::mutex g_voice_mutex;
std::vector<Voice> g_voices;

// Victory tracks (docs/VICTORY_SCENES.md 5). The game starts the win's music as it does an entrance's, by the theme
// and not by the person: who it is for comes from the match's people (their entrance blocks, as the match copied them).
struct VictoryTrack {
  fs::path file;
  float gain_db = 0.0f;
};
struct MatchPerson {
  uint32_t music = 0;       // the entrance block's +0x10 (254: USER PLAYLIST)
  std::u16string playlist;  // ... its name, +0xCC
};
std::mutex g_win_mutex;
std::map<uint32_t, VictoryTrack> g_victory_tracks;  // character id ->
std::map<uint32_t, MatchPerson> g_match_people;     // character id -> as last set up
std::atomic<bool> g_win_music{false};
std::atomic<bool> g_host_voices{false};  // (a victory track has played on a host voice: its stops and "sounding" count)
int g_win_voice = -1;                    // the win's host voice
fs::path g_win_voice_file;
uint8_t* g_base = nullptr;

fs::path Game() { return rex::filesystem::GetExecutableFolder(); }

std::vector<Pack> ReadPacks() {
  if (svr2011::NoMods()) return {};  // (Start Without Mods: no_mods.h)
  std::vector<Pack> packs;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(Game() / "Mods" / "Media", ec)) {
    if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
    Pack p;
    p.folder = e.path();
    p.name = e.path().filename().string();
    FILE* t = std::fopen((e.path() / "manifest.txt").string().c_str(), "rb");
    if (!t) continue;
    char line[512];
    while (std::fgets(line, sizeof line, t)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t eq = l.find('=');
      if (eq == std::string::npos) continue;
      if (l.compare(0, eq, "name") == 0) p.name = l.substr(eq + 1);
      p.lines.push_back({l.substr(0, eq), l.substr(eq + 1)});
    }
    std::fclose(t);
    packs.push_back(std::move(p));
  }
  std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.folder < b.folder; });
  return packs;
}

// "video.161" -> 161 (key prefix "video.").
bool KeyId(const std::string& key, const char* prefix, uint32_t& id) {
  const size_t n = std::strlen(prefix);
  if (key.compare(0, n, prefix) || key.size() <= n || !std::isdigit(uint8_t(key[n]))) return false;
  id = uint32_t(std::atoi(key.c_str() + n));
  return true;
}

// A pack's file as copied into Custom Movies / Music: "<pack> <id>" (39 chars
// at most: an entrance keeps that much of a playlist's name).
std::string MediaName(const Pack& p, const char* what, uint32_t id) {
  std::string n = p.folder.filename().string() + " " + what + " " + std::to_string(id);
  for (char& c : n)
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
  if (n.size() > 39) n.resize(39);
  return n;
}

bool CopyIfChanged(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (fs::exists(to, ec) && fs::file_size(to, ec) == fs::file_size(from, ec)) return true;
  fs::create_directories(to.parent_path(), ec);
  return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

void Wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

uint32_t GuestString(const std::string& s) {
  const uint32_t at = g_memory->SystemHeapAlloc(uint32_t(s.size() + 1));
  std::memcpy(g_memory->TranslateVirtual<char*>(at), s.c_str(), s.size() + 1);
  return at;
}

// The renders' overlay pac: RSFA / RSFB / RSFC entries and RENU/SSFD.
void BuildRenders(const std::vector<Pack>& packs) {
  std::map<uint32_t, fs::path> big, bust, icon;
  for (const auto& p : packs)
    for (const auto& [k, v] : p.lines) {
      uint32_t id;
      if (KeyId(k, "render.", id)) big[id] = p.folder / v;
      if (KeyId(k, "bust.", id)) bust[id] = p.folder / v;
      if (KeyId(k, "icon.", id)) icon[id] = p.folder / v;
    }
  if (big.empty() && bust.empty() && icon.empty()) return;
  svrfmt::Bytes d;
  svrfmt::Epac src;
  if (!svrfmt::ReadFile((Game() / "pac" / "DLC_HD.pac").string(), d) || !svrfmt::EpacRead(d, src)) return;
  svrfmt::Epac e;
  e.header = src.header;
  e.trailer = src.trailer;
  auto add = [&](const char* type, const char* from, const std::map<uint32_t, fs::path>& files) {
    svrfmt::EpacGroup g;
    g.type = type;
    for (const auto& [id, file] : files) {
      svrfmt::Bytes dds;
      svrfmt::DdsInfo info;
      if (!svrfmt::ReadFile(file.string(), dds) || !svrfmt::DdsInfoOf(dds, info)) {
        REXLOG_WARN("[svr2011] media mods: {} is not a DDS picture", file.string());
        continue;
      }
      const svrfmt::Bytes packed = svrfmt::BpeEncode(dds);  // (compressed, as the game's: read in place)
      for (uint32_t attire = 0; attire < 4; ++attire) {  // (every attire's key: attire*1000 + id)
        char key[8];
        std::snprintf(key, sizeof key, "%04u", attire * 1000 + id);
        g.entries.push_back({key, packed});
        g_alias[std::string(from) + "/" + key] = GuestString(std::string("/") + type + "/" + key);
      }
    }
    if (!g.entries.empty()) e.groups.push_back(std::move(g));
  };
  add("RSFA", "SSFA", big);
  add("RSFB", "SSFB", big);
  add("RSFC", "SSFC", bust);
  if (!icon.empty()) {  // the face icons' bank: MENU/SSFD = PACH {0: BPE(PACH {id: DDS 64 x 64})}
    for (const auto& g : src.groups)
      for (const auto& en : g.entries)
        if (g.type == "MENU" && en.name == "SSFD") {
          std::vector<svrfmt::PachEntry> outer, inner;
          if (!svrfmt::PachRead(en.data, outer) || outer.empty() ||
              !svrfmt::PachRead(svrfmt::Unpack(outer[0].data), inner))
            break;
          std::map<uint32_t, svrfmt::Bytes> all;
          for (auto& x : inner) all[x.id] = std::move(x.data);
          for (const auto& [id, file] : icon) {
            svrfmt::Bytes dds;
            if (svrfmt::ReadFile(file.string(), dds)) all[id] = std::move(dds);
          }
          inner.clear();
          for (auto& [id, data] : all) inner.push_back({id, std::move(data)});
          outer[0].data = svrfmt::BpeEncode(svrfmt::PachWrite(inner));
          svrfmt::EpacGroup og;
          og.type = "RENU";
          og.entries.push_back({"SSFD", svrfmt::PachWrite(outer)});
          e.groups.push_back(std::move(og));
          g_alias["MENU/SSFD"] = GuestString("/RENU/SSFD");
        }
  }
  if (e.groups.empty()) return;
  svrfmt::Bytes o = svrfmt::EpacWrite(e);
  uint32_t toc = 0;  // (header +4: the directory's size)
  for (const auto& g : e.groups) toc += 12 + 12 * uint32_t(g.entries.size());
  o[4] = uint8_t(toc), o[5] = uint8_t(toc >> 8), o[6] = uint8_t(toc >> 16), o[7] = uint8_t(toc >> 24);
  const fs::path out = Game() / "Mods" / "SuperstarOverlay" / "media.pac";
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  if (!svrfmt::WriteFile(out.string(), o)) return;
  svr2011::AddOverlayMount("media.pac");
  REXLOG_INFO("[svr2011] media mods: renders for {} superstars, icons for {}", std::max(big.size(), bust.size()),
              icon.size());
}

}  // namespace

namespace svr2011 {

void CopyMediaMovies(const fs::path& movies) {
  for (const auto& p : ReadPacks())
    for (const auto& [k, v] : p.lines) {
      uint32_t id;
      // (an .mp4's: its .bik fallback of the same name; hd_movies.h)
      const fs::path bik = fs::path(v).extension() == ".mp4" ? fs::path(v).replace_extension(".bik") : fs::path(v);
      if (KeyId(k, "video.", id) && bik.extension() == ".bik" && fs::exists(p.folder / bik))
        CopyIfChanged(p.folder / bik, movies / (MediaName(p, "video", id) + ".bik"));
      // (an .mp4: its stub and probe now, before the mount - hd_movies.h)
      if (KeyId(k, "video.", id) && fs::path(v).extension() == ".mp4" && fs::exists(p.folder / v))
        PrepareHdMovie(movies, MediaName(p, "video", id) + ".bik", p.folder / v);
    }
}

void InstallMediaMods(rex::memory::Memory* memory) {
  g_memory = memory;
  const auto packs = ReadPacks();
  if (packs.empty()) return;
  std::error_code ec;
  for (const auto& p : packs)
    for (const auto& [k, v] : p.lines) {
      uint32_t id;
      if (KeyId(k, "video.", id)) {
        const std::string file = MediaName(p, "video", id) + ".bik";
        if (fs::path(v).extension() == ".mp4" && fs::exists(p.folder / v, ec)) RegisterHdMovie(file, p.folder / v);
        if (fs::exists(UserMoviesFolder() / file, ec)) {
          g_videos[id] = ReserveUserMovie(file);
          FinishUserMovie(file);
        }
      } else if (KeyId(k, "theme.", id)) {
        const std::string name = MediaName(p, "theme", id);
        const fs::path to = UserMusicFolder() / (name + fs::path(v).extension().string());
        if (fs::exists(p.folder / v, ec) && CopyIfChanged(p.folder / v, to)) {
          g_themes[id] = name;
          float gain = 0.0f;
          for (const auto& [gk, gv] : p.lines)
            if (gk == "theme_gain." + std::to_string(id)) gain = float(std::atof(gv.c_str()));
          SetSongGain(to, gain);
        }
      } else if (KeyId(k, "victory_theme.", id)) {
        if (fs::exists(p.folder / v, ec)) {
          float gain = 0.0f;
          for (const auto& [gk, gv] : p.lines)
            if (gk == "victory_theme_gain." + std::to_string(id)) gain = float(std::atof(gv.c_str()));
          SetVictoryTrack(id, p.folder / v, gain);
        }
      } else if (k.rfind("sound.", 0) == 0 || k == "menu_music") {
        if (fs::exists(p.folder / v, ec)) g_sounds[k == "menu_music" ? "Menu_Music" : k.substr(6)] = p.folder / v;
      } else if (KeyId(k, "arena.", id) && id < 100) {
        const fs::path rel = fs::relative(p.folder / v, Game(), ec);
        if (fs::exists(p.folder / v, ec)) SetArenaDefault(int(id), rel.generic_string());
      }
    }
  BuildRenders(packs);
  REXLOG_INFO("[svr2011] media mods: {} packs: {} videos, {} themes, {} sounds", packs.size(), g_videos.size(),
              g_themes.size(), g_sounds.size());
}

uint32_t MediaAlias(const char* name) {
  if (g_alias.empty() || !name) return 0;
  if (*name == '/') ++name;
  static const bool log = std::getenv("SVR2011_TEST_VFS_LOG") != nullptr;
  if (log && (!std::strncmp(name, "SSF", 3) || !std::strncmp(name, "MENU/SSF", 8)))
    REXLOG_INFO("[svr2011] media mods: lookup {:.24} -> {}", name, g_alias.count(name) ? "ours" : "game's");
  const auto it = g_alias.find(name);
  return it == g_alias.end() ? 0 : it->second;
}

}  // namespace svr2011

// A match's wrestler set up (sub_828B5BC0(record, desc): desc+0 the
// character id; record+260 his profile's copy): his replaced video / theme.
REX_EXTERN(__imp__sub_828B5BC0);
REX_HOOK_RAW(sub_828B5BC0) {
  const uint32_t record = ctx.r3.u32, desc = ctx.r4.u32;
  __imp__sub_828B5BC0(ctx, base);
  g_base = base;
  svr2011::WinMusicOff();
  struct NotePerson {  // (on the way out: what the code below left in the copy)
    uint8_t* base;
    uint32_t record, desc;
    ~NotePerson() { svr2011::detail::NoteMatchPerson(base, record, desc); }
  } note{base, record, desc};
  if (std::getenv("SVR2011_TEST_PRO_LOG") && record && desc) {  // (test aid: what the match copied)
    const uint8_t* e = base + record + 260 + 0x1C0;
    const uint32_t id = uint32_t(base[desc] << 24 | base[desc + 1] << 16 | base[desc + 2] << 8 | base[desc + 3]);
    // (the same id's profile in the CHAR/PRO table, and the record in CHAR/DAT)
    const uint32_t si = id < 512 ? svr2011::RosterIndex(base, id) : 999;
    const uint8_t* tp = si < 512 ? base + svr2011::RosterEntry(0x82E7C920, 1056, si) : nullptr;
    const uint8_t* tr = si < 512 ? base + svr2011::RosterEntry(0x82E407C0, 260, si) : nullptr;
    REXLOG_INFO("[svr2011] match profile: id {} music {} movie {} entrance {} | table {} {} {} | copy==table {} "
                "record==table {} | record {:08X} desc {:08X}",
                id, uint32_t(e[0x10] << 8 | e[0x11]), uint32_t(e[0x12] << 8 | e[0x13]), uint32_t(e[0x14] << 8 | e[0x15]),
                tp ? uint32_t(tp[0x1D0] << 8 | tp[0x1D1]) : 0, tp ? uint32_t(tp[0x1D2] << 8 | tp[0x1D3]) : 0,
                tp ? uint32_t(tp[0x1D4] << 8 | tp[0x1D5]) : 0, tp ? !std::memcmp(tp, base + record + 260, 1056) : false,
                tr ? !std::memcmp(tr, base + record, 260) : false, record, desc);
  }
  if ((g_videos.empty() && g_themes.empty()) || !record || !desc) return;
  const uint32_t id = uint32_t(base[desc] << 24 | base[desc + 1] << 16 | base[desc + 2] << 8 | base[desc + 3]);
  if (id < 50 || (!g_videos.count(id) && !g_themes.count(id))) return;  // (a Created Superstar's own: his CAE)
  uint8_t* e = base + record + 260 + 0x1C0;  // the entrance block
  if (const auto v = g_videos.find(id); v != g_videos.end() && v->second) Wr16(e + 0x12, uint16_t(v->second));
  if (const auto t = g_themes.find(id); t != g_themes.end()) {
    Wr16(e + 0x10, 254);  // USER PLAYLIST, by name
    for (uint32_t i = 0; i < 40; ++i) Wr16(e + 0xCC + 2 * i, i < t->second.size() && i < 39 ? uint8_t(t->second[i]) : 0);
  }
  REXLOG_DEBUG("[svr2011] media mods: id {}'s entrance: video {}, theme \"{}\"", id,
              g_videos.count(id) ? g_videos[id] : 0, g_themes.count(id) ? g_themes[id] : std::string());
}

// ---- Sounds
namespace {

constexpr uint32_t kSoundMgr = 0x82EC4C18;  // -> +36: the volume floats (music, -, SFX, voice ...)

int Category(const std::string& e) {
  if (e.find("MUS_") == 0 || e.find("Music") != std::string::npos) return 0;
  if (e.find("Chant") != std::string::npos || e.find("Crowd") != std::string::npos || e.find("RA_") == 0) return 2;
  return 1;
}

float Volume(uint8_t* base, int category) {
  const uint32_t mgr = uint32_t(base[kSoundMgr] << 24 | base[kSoundMgr + 1] << 16 | base[kSoundMgr + 2] << 8 |
                                base[kSoundMgr + 3]);
  if (!mgr) return 1.0f;
  const uint32_t at = mgr + 36;
  const uint32_t vols = uint32_t(base[at] << 24 | base[at + 1] << 16 | base[at + 2] << 8 | base[at + 3]);
  if (!vols) return 1.0f;
  const int index = category == 0 ? 0 : category == 1 ? 2 : 3;
  const uint8_t* f = base + vols + index * 4;
  uint32_t bits = uint32_t(f[0] << 24 | f[1] << 16 | f[2] << 8 | f[3]);
  float v;
  std::memcpy(&v, &bits, 4);
  return v >= 0.0f && v <= 1.0f ? v : 1.0f;
}

// Stops (or pauses / resumes) host voices: on `object` (0xFFFFFFFF: any), of
// `category` (-1: any).
void ForVoices(uint32_t object, int category, int what) {  // 0 stop, 1 pause, 2 resume
  std::lock_guard lock(g_voice_mutex);
  for (auto it = g_voices.begin(); it != g_voices.end();) {
    const bool hit = (object == 0xFFFFFFFFu || it->object == object) && (category < 0 || it->category == category);
    if (!hit) {
      ++it;
      continue;
    }
    if (what == 0) {
      if (it->category == 0) REXLOG_INFO("[svr2011] media mods: music voice {} stopped", it->handle);
      svr2011::HostSoundStop(it->handle);
      it = g_voices.erase(it);
      continue;
    }
    it->paused = what == 1;
    svr2011::HostSoundPause(it->handle, it->paused);
    ++it;
  }
}

bool ObjectSounding(uint32_t object) {
  std::lock_guard lock(g_voice_mutex);
  for (auto it = g_voices.begin(); it != g_voices.end();) {
    if (!svr2011::HostSoundActive(it->handle)) {
      it = g_voices.erase(it);
      continue;
    }
    if (it->object == object) return true;
    ++it;
  }
  return false;
}

}  // namespace

namespace {
constexpr uint32_t kSlotTable = 0x82E3DDFC, kSlotsAt = 132, kSlotSize = 52, kSlotKey = 16;
constexpr uint32_t kFighters = 0x82E3CC50, kLost = 447, kBeatenBy = 448;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// The win's music is for the person in the match whose theme it is. Two people can share one (a mod with a stock
// theme, a mirror match): a person who beat someone goes first, and if it's him, his own track or none.
template <typename Theme>
bool WinnerTrack(const uint8_t* base, const Theme& is_his, VictoryTrack* out) {
  const uint32_t table = Be32(base + kSlotTable);
  if (!table) return false;
  bool won[16] = {};
  for (uint32_t i = 0; i < 8; ++i)
    if (const uint32_t c = Be32(base + kFighters + i * 4); c && base[c + kLost] && base[c + kBeatenBy] < 16) won[base[c + kBeatenBy]] = true;
  std::lock_guard lock(g_win_mutex);
  for (const bool winners : {true, false}) {
    for (uint32_t person = 0; person < 16; ++person) {
      if (won[person] != winners) continue;
      const uint32_t id = Be32(base + table + kSlotsAt + person * kSlotSize + kSlotKey) / 100;
      const auto p = g_match_people.find(id);
      if (!id || p == g_match_people.end() || !is_his(p->second)) continue;
      const auto t = g_victory_tracks.find(id);
      if (t == g_victory_tracks.end()) {
        REXLOG_INFO("[svr2011] victory track: the music is superstar {}'s, who has none", id);
        return false;
      }
      *out = t->second;
      REXLOG_INFO("[svr2011] victory track: superstar {} ({})", id, winners ? "the winner" : "by his theme");
      return true;
    }
  }
  REXLOG_INFO("[svr2011] victory track: the music is nobody's in the match");
  return false;
}
}  // namespace

void svr2011::detail::NoteMatchPerson(const uint8_t* base, uint32_t record, uint32_t desc) {
  if (!record || !desc) return;
  const uint8_t* e = base + record + 260 + 0x1C0;
  MatchPerson p;
  p.music = uint32_t(e[0x10]) << 8 | e[0x11];
  for (uint32_t i = 0; i < 40; ++i) {
    const char16_t c = char16_t(uint32_t(e[0xCC + 2 * i]) << 8 | e[0xCC + 2 * i + 1]);
    if (!c) break;
    p.playlist.push_back(c);
  }
  if (std::getenv("SVR2011_TEST_VICTORY_LOG"))
    REXLOG_INFO("[svr2011] victory track: person {} music {} playlist chars {}", Be32(base + desc), p.music, p.playlist.size());
  std::lock_guard lock(g_win_mutex);
  g_match_people[Be32(base + desc)] = std::move(p);
}

void svr2011::SetVictoryTrack(uint32_t id, const fs::path& file, float gain_db) {
  if (std::getenv("SVR2011_TEST_NO_VICTORY")) return;  // (test aid: this game plays without them - two test games share one Mods folder)
  std::lock_guard lock(g_win_mutex);
  g_victory_tracks[id] = {file, std::clamp(gain_db, -20.0f, 6.0f)};
  REXLOG_INFO("[svr2011] victory track: superstar {}: {}", id, file.filename().string());
}

void svr2011::WinMusicOn() {
  if (g_win_music.exchange(true)) return;
  REXLOG_INFO("[svr2011] victory track: the match's result is in: the win's music from now on");
  if (g_base && std::getenv("SVR2011_TEST_VICTORY_LOG"))  // (the live match settings: the rule, its title)
    REXLOG_INFO("[svr2011] victory track: rule {:02X}, title byte {}", g_base[0x82E3DE00], g_base[0x82E3DE00 + 37]);
}

void svr2011::WinMusicOff() {
  if (g_win_music.exchange(false)) REXLOG_INFO("[svr2011] victory track: the win's music is over");
  std::lock_guard lock(g_win_mutex);
  g_win_voice = -1;
  g_win_voice_file.clear();
}

bool svr2011::WinMusicNow() { return g_win_music; }

bool svr2011::VictoryTrackOfPlaylist(const std::u16string& playlist, fs::path* file, float* gain_db) {
  if (!g_win_music || !g_base) return false;
  VictoryTrack track;
  if (!WinnerTrack(g_base, [&playlist](const MatchPerson& p) { return p.music == 254 && p.playlist == playlist; }, &track)) return false;
  *file = track.file;
  *gain_db = track.gain_db;
  return true;
}

// Post an event by name: sub_82BEC030(name, object, flags, callback, cookie)
// -> a playing id (0: failed).
// (called from superstar_mods.cpp's hook: true = handled, *id the result)
bool svr2011::MediaEvent(uint8_t* base, const char* e, uint32_t object, uint32_t* id) {
  g_base = base;
  if (!std::strcmp(e, "Play_Menu_Music")) WinMusicOff();
  if (g_win_music && !std::strncmp(e, "Play_MUS_", 9)) {
    const uint32_t music = uint32_t(std::atoi(e + 9));
    VictoryTrack track;
    if (WinnerTrack(base, [music](const MatchPerson& p) { return p.music == music; }, &track)) {
      std::lock_guard lock(g_win_mutex);
      if (g_win_voice >= 0 && g_win_voice_file == track.file && svr2011::HostSoundActive(g_win_voice)) {
        *id = 0x7F000000u + uint32_t(g_win_voice + 1);  // (started already: goes on)
        return true;
      }
      ForVoices(object, 0, 0);
      const int h = svr2011::HostSoundStart(track.file, true, Volume(base, 0), track.gain_db);
      if (h >= 0) {
        std::lock_guard vlock(g_voice_mutex);
        g_voices.push_back({h, object, 0, false});
        g_host_voices = true;
      }
      g_win_voice = h, g_win_voice_file = track.file;
      REXLOG_INFO("[svr2011] victory track: {} -> {} ({:+.1f} dB)", e, track.file.filename().string(), track.gain_db);
      *id = 0x7F000000u + uint32_t(h + 1);
      return true;
    }
  }
  if (g_sounds.empty() && !g_host_voices) return false;
  {
    if (!std::strncmp(e, "Play_", 5)) {
      if (const auto it = g_sounds.find(e + 5); it != g_sounds.end()) {
        const int cat = Category(it->first);
        ForVoices(object, cat, 0);  // (a sound again on the same object: the new one)
        const bool loop = cat == 0;
        const int h = svr2011::HostSoundStart(it->second, loop, Volume(base, cat));
        if (h >= 0) {
          std::lock_guard lock(g_voice_mutex);
          g_voices.push_back({h, object, cat, false});
        }
        REXLOG_INFO("[svr2011] media mods: {} -> {}", e, it->second.filename().string());
        *id = 0x7F000000u + uint32_t(h + 1);  // (a playing id: not 0)
        return true;
      }
    } else if (!std::strncmp(e, "Stop_", 5)) {
      const std::string n = e + 5;
      if (object != 0xFFFFFFFFu) ForVoices(object, -1, 0);
      else if (n.find("Menu_Music") != std::string::npos) ForVoices(0xFFFFFFFFu, 0, 0);
      else if (n.find("Entrance_Music") != std::string::npos) ForVoices(0xFFFFFFFFu, 0, 0);
      else if (n.find("chant") != std::string::npos || n.find("Crowd") != std::string::npos) ForVoices(0xFFFFFFFFu, 2, 0);
      else if (n.find("SFX") != std::string::npos) ForVoices(0xFFFFFFFFu, 1, 0);
      else if (n.find("All_Audio") != std::string::npos || n.find("ALL_Audio") != std::string::npos) ForVoices(0xFFFFFFFFu, -1, 0);
    } else if (!std::strcmp(e, "Pause_start") || !std::strcmp(e, "Pause_ALL_Audio")) {
      ForVoices(object, -1, 1);
    } else if (!std::strcmp(e, "Pause_Resume") || !std::strcmp(e, "Resume_ALL_Audio")) {
      ForVoices(object, -1, 2);
    }
  }
  return false;
}

// A game object released (sub_82BB6898(sys, object)): its sounds stop.
REX_EXTERN(__imp__sub_82BB6898);
REX_HOOK_RAW(sub_82BB6898) {
  if (!g_sounds.empty() || g_host_voices) ForVoices(ctx.r4.u32, -1, 0);
  __imp__sub_82BB6898(ctx, base);
}

// "Is the object still sounding" (sub_82BB69E8(sys, object)): yes while one
// of ours plays on it.
REX_EXTERN(__imp__sub_82BB69E8);
REX_HOOK_RAW(sub_82BB69E8) {
  if ((!g_sounds.empty() || g_host_voices) && ObjectSounding(ctx.r4.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82BB69E8(ctx, base);
}
