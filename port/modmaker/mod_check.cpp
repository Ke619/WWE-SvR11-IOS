// The mod doctor (mod_check.h). Every rule here is the game's or the launcher's (file:line in the comments refer to
// port/src and port/launcher); messages say what goes wrong in the game and what to do.
#include "mod_check.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <functional>
#include <sstream>

#include "svrfmt/arena.h"
#include "tool_run.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"
#include "svrfmt/wwe13.h"
#include "svrfmt/zip_write.h"

namespace mm {

namespace fs = std::filesystem;
using namespace svrfmt;

namespace {

struct Manifest {
  bool found = false, bom = false;
  std::vector<std::pair<std::string, std::string>> keys;  // in order, raw values (untrimmed)
  std::vector<std::string> long_lines;
  std::string Get(const std::string& k) const {
    for (const auto& [a, b] : keys)
      if (a == k) return b;
    return "";
  }
  bool Has(const std::string& k) const {
    for (const auto& [a, b] : keys)
      if (a == k) return true;
    return false;
  }
};

Manifest ReadManifest(const fs::path& dir) {
  Manifest m;
  Bytes b;
  if (!ReadFile(PathStr(dir / "manifest.txt"), b)) return m;
  m.found = true;
  size_t at = 0;
  if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) m.bom = true, at = 3;
  std::string text(b.begin() + long(at), b.end());
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > 510) m.long_lines.push_back(line.substr(0, 40));
    if (line.empty() || line[0] == '#') continue;
    const size_t e = line.find('=');
    if (e == std::string::npos) continue;
    m.keys.push_back({line.substr(0, e), line.substr(e + 1)});
  }
  return m;
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  return s;
}
bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}
std::string Ext(const std::string& f) { return Lower(PathStr(fs::u8path(f).extension())); }
bool Ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c >= 0x80) return false;
  return true;
}
bool IsInt(const std::string& s, long lo, long hi, long* out = nullptr) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (*end) return false;
  if (out) *out = v;
  return v >= lo && v <= hi;
}

// A DDS picture's size and format ("" fine, else what's off) against w x h and the formats allowed.
std::string DdsIssue(const fs::path& f, int w, int h, std::initializer_list<DxtFormat> formats, const char* format_name) {
  Bytes b;
  DdsInfo info;
  if (!ReadFile(PathStr(f), b) || !DdsInfoOf(b, info)) return "isn't a DDS picture";
  std::string issue;
  if (w && (info.w != w || info.h != h)) issue = "is " + std::to_string(info.w) + " x " + std::to_string(info.h) + " (needs " + std::to_string(w) + " x " + std::to_string(h) + ")";
  bool fmt_ok = formats.size() == 0;
  for (DxtFormat x : formats) fmt_ok |= info.format == x;
  if (!fmt_ok) issue += std::string(issue.empty() ? "is" : ", and") + " not " + format_name;
  return issue;
}

// Bink 1 movie: "BIK", width at +0x14, height at +0x18.
bool BinkSize(const fs::path& f, int& w, int& h) {
  FILE* fp = _wfopen(f.c_str(), L"rb");
  if (!fp) return false;
  uint8_t head[0x1C] = {};
  const bool ok = std::fread(head, 1, sizeof head, fp) == sizeof head;
  std::fclose(fp);
  if (!ok || std::memcmp(head, "BIK", 3)) return false;
  w = int(head[0x14] | head[0x15] << 8), h = int(head[0x18] | head[0x19] << 8);
  return true;
}

bool PacIsEpk8WithModels(const fs::path& f, bool want_attire_entry = false) {
  Bytes b;
  Epac e;
  if (!ReadFile(PathStr(f), b) || b.size() < 0x4000 || std::memcmp(b.data(), "EPK8", 4) || !EpacRead(b, e)) return false;
  for (const auto& g : e.groups) {
    if (g.type.rfind("EMD", 0) != 0) continue;
    for (const auto& en : g.entries)
      if (en.name.size() >= 8 && std::all_of(en.name.begin(), en.name.begin() + 8, [](char c) { return std::isdigit(uint8_t(c)); }) &&
          (!want_attire_entry || en.name[7] == '2'))
        return true;
  }
  return false;
}

const char* const kPcThemes[] = {".mp3", ".wma", ".m4a", ".aac", ".wav", ".flac"};
bool PcTheme(const std::string& ext) { return std::find(std::begin(kPcThemes), std::end(kPcThemes), ext) != std::end(kPcThemes); }

// ---- fixes on an installed mod's folder (the doctor's buttons)

// The manifest rewritten: key set to value ("" removes the line), the rest as it was (no BOM, no trailing spaces).
void SetKey(const fs::path& dir, const std::string& key, const std::string& value) {
  Bytes b;
  ReadFile(PathStr(dir / "manifest.txt"), b);
  size_t at = b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;
  std::istringstream in(std::string(b.begin() + long(at), b.end()));
  std::string out, line;
  bool done = key.empty();
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t e = line.find('=');
    if (e != std::string::npos && line[0] != '#') {
      std::string v = line.substr(e + 1);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
      if (!key.empty() && line.compare(0, e, key) == 0 && e == key.size()) {
        if (!done && !value.empty()) out += key + "=" + value + "\n";
        done = true;
        continue;
      }
      out += line.substr(0, e) + "=" + v + "\n";
    } else {
      out += line + "\n";
    }
  }
  if (!done && !value.empty()) out += key + "=" + value + "\n";
  WriteFile(PathStr(dir / "manifest.txt"), Bytes(out.begin(), out.end()));
}
std::function<void()> Repair(const fs::path& dir) { return [dir] { SetKey(dir, "", ""); }; }
std::function<void()> Set(const fs::path& dir, const std::string& key, const std::string& value) {
  return [dir, key, value] { SetKey(dir, key, value); };
}
std::function<void()> TurnOff(const fs::path& dir) {
  return [dir] { WriteFile(PathStr(dir / "disabled"), Bytes()); };
}
std::function<void()> OpenIt(const fs::path& dir) {
  return [dir] { ProjectOpenFolder(dir.wstring()); };
}
const char* kOpen = "Open it in the Mod Maker";  // (its page: fix it there and Install again)

// The mod's .svrmod unpacked over its installed folder again (picked by the player; it must be the same mod).
std::function<void()> Reinstall(const fs::path& dir) {
  return [dir] {
    static const COMDLG_FILTERSPEC kMod[1] = {{L"Mods (*.svrmod)", L"*.svrmod"}};
    const std::wstring f = PickFile(false, (L"The mod file of " + dir.filename().wstring()).c_str(), kMod, 1);
    if (f.empty()) return;
    Bytes zip;
    std::vector<ZipEntry> files;
    if (!ReadFile(Utf8(f), zip) || !ZipRead(zip, files)) {
      Status("That mod file is damaged: nothing was changed.");
      return;
    }
    std::string id;
    for (const auto& e : files)
      if (e.name == "manifest.txt") {
        std::istringstream in(std::string(e.data.begin(), e.data.end()));
        for (std::string l; std::getline(in, l);)
          if (l.rfind("id=", 0) == 0) id = Trim(l.substr(3));
      }
    while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
    if (id != PathStr(dir.filename())) {
      Status("That's a different mod (" + (id.empty() ? std::string("no name") : id) + "): nothing was changed.");
      return;
    }
    int written = 0;
    std::error_code ec;
    for (const auto& e : files) {
      if (e.name.empty() || e.name.back() == '/' || e.name.find("..") != std::string::npos || e.name.find(':') != std::string::npos) continue;
      const fs::path out = dir / fs::u8path(e.name);
      fs::create_directories(out.parent_path(), ec);
      written += WriteFile(PathStr(out), e.data);
    }
    Status("Re-installed " + std::to_string(written) + " files of " + id + ".");
  };
}

// pack.txt without the lines whose animation file isn't in the folder (the game then plays the base's for those).
// Moves on ids of 16000 and up (older ports): moves_port.py renumber moves them to free records below the banks'
// last ids, in the mod itself (pack.txt, the follow-ups, the moves list, the version), so the ids stay put. The game
// leaves such a move's body motion out and keeps the base's move instead (move_packs.cpp; report a745b2).
std::function<void()> RenumberMoves(const fs::path& mod) {
  return [mod] {
    const fs::path game = fs::path(g_game);
    Status("Moving " + PathStr(mod.filename()) + "'s moves to ids the game plays ...");
    const bool started = RunToolJson(L"moves_port.py", {L"renumber", L"--mod", mod.wstring(), L"--pac11", (game / L"pac").wstring(),
                                                         L"--mods", (game / L"Mods").wstring()},
                                     [mod](int code, const std::string& out) {
                                       size_t n = 0;
                                       for (size_t at = out.find("\"new\":"); at != std::string::npos; at = out.find("\"new\":", at + 1)) ++n;
                                       const bool kept = out.find("\"kept\": [{") != std::string::npos ||
                                                         out.find("\"kept\":[{") != std::string::npos;
                                       if (code != 0) Status("The moves couldn't be moved (see the log): nothing was changed.");
                                       else Status(std::to_string(n) + " move" + (n == 1 ? " is" : "s are") + " on new ids" +
                                                   (kept ? " - some had no free id left and stay off (see the log)" : "") +
                                                   ". The game picks them up at its next start.");
                                       FixChanged();
                                     });
    if (!started) Status("The move tool couldn't start (see the Mod Maker tools).");
  };
}

std::function<void()> DropBrokenMoves(const fs::path& pack) {
  return [pack] {
    Bytes b;
    if (!ReadFile(PathStr(pack / "pack.txt"), b)) return;
    std::istringstream in(std::string(b.begin(), b.end()));
    std::string out, line;
    int dropped = 0;
    while (std::getline(in, line)) {
      std::string l = line;
      if (!l.empty() && l.back() == '\r') l.pop_back();
      std::istringstream ls(l);
      std::vector<std::string> t;
      for (std::string w; ls >> w;) t.push_back(w);
      const bool motion = !t.empty() && t[0] == "motion" && t.size() >= 8, entry = !t.empty() && t[0] == "pacentry" && t.size() >= 5;
      if ((motion && !Exists(pack / fs::u8path(t[7]))) || (entry && !Exists(pack / fs::u8path(t[4])))) {
        ++dropped;
        continue;
      }
      out += l + "\n";
    }
    WriteFile(PathStr(pack / "pack.txt"), Bytes(out.begin(), out.end()));
    Status("Took out " + std::to_string(dropped) + " broken move" + (dropped == 1 ? "" : "s") + ".");
  };
}

// What a manifest key is, for people ("the theme song", "outfit 2" ...).
std::string Thing(const std::string& key) {
  if (key == "song") return "the theme song";
  if (key == "movie") return "the big screen video";
  if (key == "voice") return "the name recording";
  if (key == "menu_music") return "the menu music";
  if (key.rfind("attire", 0) == 0 && key.size() == 7) return "outfit " + key.substr(6);
  const size_t dot = key.find('.');
  const std::string what = key.substr(0, dot), id = dot == std::string::npos ? "" : key.substr(dot + 1);
  if (what == "video") return "the big screen video (" + id + ")";
  if (what == "theme") return "the theme song (" + id + ")";
  if (what == "sound") return "the sound \"" + id + "\"";
  if (what == "render" || what == "bust" || what == "icon") return "a select picture (" + id + ")";
  if (what == "arena") return "the arena (" + id + ")";
  return key;
}

// A theme / song key's file: there, plays on PC.
void SongCheck(const fs::path& dir, const std::string& key, const std::string& raw, std::vector<Problem>& p) {
  const std::string f = Trim(raw);
  if (f.empty()) return;
  if (!Exists(dir / fs::u8path(f))) {
    p.push_back(Error(Thing(key) + " (" + f + ") is missing from the mod: it would be silent.").With("Leave it out", Set(dir, key, "")).With(kOpen, OpenIt(dir)));
    return;
  }
  const std::string e = Ext(f);
  if (e == ".ogg")
    p.push_back(Warning(Thing(key) + " is an OGG file: it plays on Android, but on PC it's silent.", "Use an MP3 instead.").With(kOpen, OpenIt(dir)));
  else if (!PcTheme(e))
    p.push_back(Warning(Thing(key) + " is a " + e + " file, which the game can't play: it would be silent.", "Use an MP3 instead.").With(kOpen, OpenIt(dir)));
}

// A Bink movie's likely size as an .mp4 (H.264 CRF 20 + AAC: about 0.04 bits a pixel, 128 kb/s sound); 0 unknown.
uint64_t Mp4Guess(const fs::path& f) {
  FILE* fp = _wfopen(f.c_str(), L"rb");
  if (!fp) return 0;
  uint8_t h[0x24] = {};
  const bool ok = std::fread(h, 1, sizeof h, fp) == sizeof h;
  std::fclose(fp);
  if (!ok || std::memcmp(h, "BIK", 3)) return 0;
  auto u32 = [&](int o) { return uint32_t(h[o] | h[o + 1] << 8 | h[o + 2] << 16 | uint32_t(h[o + 3]) << 24); };
  const double frames = u32(8), w = u32(0x14), hh = u32(0x18), num = u32(0x1C), den = u32(0x20);
  if (!frames || !w || !hh || !num || !den) return 0;
  const double fps = num / den, seconds = frames / fps;
  return uint64_t(seconds * (w * hh * fps * 0.04 + 128000) / 8);
}

// The "Shrink it" fix: the mod's Bink movie as an .mp4 beside it, the key pointed at it, the Bink kept only with
// the Mod Maker's "Keep the Bink too" (the game's fallback where MP4 can't play). In the background.
std::function<void()> ShrinkIt(const fs::path& dir, const std::string& key, const std::string& file) {
  return [dir, key, file] {
    RunInBackground([dir, key, file] {
      const fs::path bik = dir / fs::u8path(file), mp4 = fs::path(bik).replace_extension(L".mp4");
      std::error_code ec;
      const uint64_t before = fs::file_size(bik, ec);
      Progress("Making " + file + " an mp4 ...");
      const std::string why = ShrinkMovie(bik.wstring(), mp4.wstring());
      OnUiThread([dir, key, file, bik, mp4, before, why] {
        std::error_code ec2;
        if (!why.empty()) {
          Status("The movie couldn't be shrunk: " + why + ".");
          return;
        }
        Set(dir, key, PathStr(mp4.filename()))();
        if (!g_settings.keep_bik) fs::remove(bik, ec2);
        Status(file + " is an mp4 now: " + SizeText(before) + " -> " + SizeText(fs::file_size(mp4, ec2)) + ".");
      });
    });
  };
}

// A movie key's file: .bik (320 x 320 for a titantron), or .mp4 (a .bik of the same name beside it: the fallback).
void MovieCheck(const fs::path& dir, const std::string& key, const std::string& raw, bool titantron, std::vector<Problem>& p) {
  const std::string f = Trim(raw);
  if (f.empty()) return;
  const fs::path path = dir / fs::u8path(f);
  const std::string e = Ext(f);
  if (!Exists(path)) {
    p.push_back(Error(Thing(key) + " (" + f + ") is missing from the mod: the big screen would show the usual video.")
                    .With("Leave it out", Set(dir, key, "")).With(kOpen, OpenIt(dir)));
    return;
  }
  if (e == ".mp4") {
    fs::path bik = path;
    bik.replace_extension(".bik");
    if (!Exists(bik))
      p.push_back(Warning(Thing(key) + " is an mp4: it plays where Windows plays mp4. On Steam Deck (Proton) or Windows N the "
                       "usual video plays instead, unless a Bink of the same name is beside it.").Note());
    return;
  }
  if (e != ".bik") {
    p.push_back(Warning(Thing(key) + " is a kind of video the game can't play.", "The Mod Maker turns any video into one it can.").With(kOpen, OpenIt(dir)));
    return;
  }
  int w = 0, h = 0;
  if (!BinkSize(path, w, h))
    p.push_back(Error(Thing(key) + " is damaged or not a video the game can play.").With("Leave it out", Set(dir, key, "")).With(kOpen, OpenIt(dir)));
  else if (titantron && (w != 320 || h != 320))
    p.push_back(Warning(Thing(key) + " isn't square: the big screen is square, so it plays stretched."));
  // a Bink is big: as an mp4 the mod gets much smaller
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec), guess = Mp4Guess(path);
  if (!ec && guess && guess * 10 < size * 8 && size > (2u << 20))
    p.push_back(Warning(Thing(key) + " is a Bink movie (" + SizeText(size) + "): as an mp4 it would be about " + SizeText(guess) +
                     ", a much smaller mod.").With("Shrink it", ShrinkIt(dir, key, f)));
}

// Keys whose value is a file name: a trailing space makes the game miss the file (superstar / arena / media
// manifests aren't trimmed by the game).
void TrailingSpaces(const fs::path& dir, const Manifest& m, std::vector<Problem>& p) {
  for (const auto& [k, v] : m.keys)
    if (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
      p.push_back(Warning("A line of the mod's settings (" + Thing(k) + ") ends with a space: the game may not find that file.")
                      .With("Repair it", Repair(dir), "the spaces are gone"));
      return;  // (one repair does them all)
    }
}

void ManifestBasics(const fs::path& dir, const Manifest& m, const std::string& want_type, std::vector<Problem>& p) {
  if (!m.found) {
    p.push_back(Error("The mod has no settings file (manifest.txt): the launcher won't install it.", "Make it again in the Mod Maker."));
    return;
  }
  if (m.bom)
    p.push_back(Warning("The mod's settings file was saved in a way the game misreads: its first line (" + Thing(m.keys.empty() ? "?" : m.keys.front().first) +
                        ") is lost.").With("Repair it", Repair(dir), "the settings file is repaired"));
  for (const auto& l : m.long_lines) p.push_back(Warning("A line of the mod's settings is too long for the game (" + l + "...).").With(kOpen, OpenIt(dir)));
  const std::string type = Lower(Trim(m.Get("type")));
  if (!want_type.empty() && type != want_type)
    p.push_back(Error("This is a " + (type.empty() ? std::string("mod of no kind") : type + " mod") + " in the " + want_type + " folder: the game won't load it here.")
                    .With("Make it a " + want_type + " mod", Set(dir, "type", want_type)));
  const std::string id = Trim(m.Get("id"));
  if (id.empty())
    p.push_back(Error("The mod has no short name (id): the launcher won't install it.").With("Use its folder's name", Set(dir, "id", PathStr(dir.filename()))));
  else if (id == "." || id == ".." || id.find_first_of("/\\:") != std::string::npos)
    p.push_back(Error("The mod's short name (" + id + ") has characters a folder can't have.").With("Use its folder's name", Set(dir, "id", PathStr(dir.filename()))));
}

void UnknownKeys(const fs::path& dir, const Manifest& m, std::initializer_list<const char*> known, std::initializer_list<const char*> prefixes,
                 std::vector<Problem>& p) {
  static const char* const kCommon[] = {"type", "id", "name", "author", "version", "made_with", "description", "source", "source_entrance",
                                        "source_movie", "entrance_note"};
  std::string unknown;
  for (const auto& [k, v] : m.keys) {
    bool ok = std::find_if(std::begin(kCommon), std::end(kCommon), [&](const char* c) { return k == c; }) != std::end(kCommon);
    for (const char* c : known) ok |= k == c;
    for (const char* c : prefixes) ok |= k.rfind(c, 0) == 0;
    if (!ok) unknown += (unknown.empty() ? "" : ", ") + k;
  }
  if (!unknown.empty())
    p.push_back(Problem{false, "The mod has settings the game doesn't know (" + unknown + " - typing mistakes?): they're ignored.", ""});
}

std::set<std::string> LeftOut(const fs::path& superstars) {
  std::set<std::string> s;
  Bytes b;
  if (!ReadFile(PathStr(superstars / "left_out.txt"), b)) return s;
  std::istringstream in(std::string(b.begin(), b.end()));
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (!l.empty()) s.insert(l);
  }
  return s;
}

// ---- per type

void CheckPackTxt(const fs::path& dir, const fs::path& mod, const std::string& who, std::vector<Problem>& p,
                  const std::string& star = "", const std::string& base_name = "") {
  Bytes b;
  if (!ReadFile(PathStr(dir / "pack.txt"), b)) {
    p.push_back(Error(who + " are broken (their list is missing): none of them would play.").With(kOpen, OpenIt(mod)));
    return;
  }
  const fs::path pacdir = fs::path(g_game) / L"pac";
  std::istringstream in(std::string(b.begin(), b.end()));
  std::string line;
  int n = 0, missing = 0, bad = 0;
  std::string first_missing, fatal;
  std::set<int> high_motion, own;  // (motion ids of 16000 and up / the moves the pack gives itself)
  auto is_hex = [](const std::string& s, size_t len) { return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(uint8_t(c)); }); };
  while (std::getline(in, line)) {
    ++n;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::vector<std::string> t;
    for (std::string w; ls >> w;) t.push_back(w);
    if (t.empty() || t[0][0] == '#') continue;
    const std::string& kind = t[0];
    if (kind == "motion") {
      if (t.size() < 8) { ++bad; continue; }
      if (!Exists(dir / fs::u8path(t[7]))) { if (!missing++) first_missing = t[7]; }
      if (!g_game.empty() && !Exists(pacdir / fs::u8path(t[1]))) fatal = t[1];
      if (const int id = std::atoi(t[3].c_str()); id >= 16000) high_motion.insert(id);
    } else if (kind == "pacentry") {
      if (t.size() < 5 || t[3].size() > 8 || Ext(t[1]) != ".pac" || t[1].find("..") != std::string::npos) { ++bad; continue; }
      if (!Exists(dir / fs::u8path(t[4]))) { if (!missing++) first_missing = t[4]; }
      if (!g_game.empty() && !Exists(pacdir / fs::u8path(t[1]))) fatal = t[1];
    } else if (kind == "waze") {
      if (t.size() < 3 || !is_hex(t[2], 32)) ++bad;
      else own.insert(std::atoi(t[1].c_str()));
    } else if (kind == "wazecopy") {
      if (t.size() < 3 || std::atoi(t[1].c_str()) == 0 || t[1] == t[2]) ++bad;
      else own.insert(std::atoi(t[1].c_str()));
    } else if (kind == "wazename") {
      if (t.size() < 3) ++bad;
      else own.insert(std::atoi(t[1].c_str()));
    } else if (kind == "exh") {
      if (t.size() < 3 || !is_hex(t[2], 72)) ++bad;
    } else if (kind == "evt") {
      if (t.size() < 4 || !is_hex(t[2], 32)) ++bad;
    } else if (kind == "mbd") {
      if (t.size() < 3 || !is_hex(t[2], 16)) ++bad;
    }
  }
  if (!fatal.empty())
    p.push_back(Error(who + " point at a game file this game doesn't have (" + fatal + "): while they're on, NO extra moves play - "
                      "not even other mods'.").With("Turn them off", TurnOff(mod), "turned off").With(kOpen, OpenIt(mod)));
  if (missing) {
    const std::string text = !star.empty()
        ? "Some of " + star + "'s moves are missing their animation files - those moves won't play, so the game uses " +
              (base_name.empty() ? std::string("the fighting style's") : base_name + "'s") + " moves instead."
        : who + ": " + std::to_string(missing) + " animation file" + (missing == 1 ? " is" : "s are") + " missing, so " +
              (missing == 1 ? "that move doesn't" : "those moves don't") + " play.";
    p.push_back(Error(text).With("Re-install the mod", Reinstall(mod), "the mod's files are put back")
                    .With("Remove the broken moves", DropBrokenMoves(dir), "the broken moves are taken out"));
  }
  if (bad) p.push_back(Warning(who + ": " + std::to_string(bad) + " line" + (bad == 1 ? " is" : "s are") + " damaged and skipped.").With(kOpen, OpenIt(mod)));
  {  // (moves past 16000: installed mods only - the fix changes the mod in the game's Mods folder)
    std::string ids;
    int past = 0;
    for (int id : high_motion)
      if (own.count(id)) ids += (past++ ? ", " : "") + std::to_string(id);
    std::error_code ec2;
    const fs::path mods = fs::absolute(fs::path(g_game) / L"Mods", ec2), at = fs::absolute(mod, ec2);
    if (past && !g_game.empty() && PathStr(at).rfind(PathStr(mods), 0) == 0)
      p.push_back(Warning(who + ": " + std::to_string(past) + " move" + (past == 1 ? " is" : "s are") + " on an id past 16000 (" + ids +
                              ") - the game can't play " + (past == 1 ? "it" : "them") + ", so the base's move is used instead.",
                          "The fix moves them to free ids the game plays, in the mod itself (a copy of the old files is kept "
                          "in the Mod Maker tools' work\\renumber_backup folder).")
                      .With("Move them to ids the game plays", RenumberMoves(mod), "the moves are being moved"));
  }
  uintmax_t size = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file(ec)) size += e.file_size(ec);
  if (size > 16u * 1024 * 1024)
    p.push_back(Warning(who + " are big (" + std::to_string(size >> 20) + " MB): the game takes longer to start, and a match has room for "
                        "about 16 MB of moves for everyone."));
}

void CheckSuperstar(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(dir, m, "superstar", p);
  TrailingSpaces(dir, m, p);
  UnknownKeys(dir, m, {"short", "base", "song", "music", "theme_gain", "theme_gain_auto", "movie", "voice", "call_gain", "renumbered", "loading", "style", "ratings", "call", "hometown", "announcer", "height", "pose",
                       "entrance", "victory", "abilities", "moves", "pack", "game_look", "cae"},
              {"attire"}, p);
  const std::string name = Trim(m.Get("name"));
  const std::string who = name.empty() ? std::string("The superstar") : name;
  if (const std::string mu = Trim(m.Get("music")); !mu.empty()) {  // (a theme of the game's own, by its music id)
    const int v = std::atoi(mu.c_str());
    if (v < 1 || v > 253)
      p.push_back(Warning(who + "'s game theme isn't one the game has: the fighting style's theme plays.").With("Use the style's theme", Set(dir, "music", "")));
    else if (!Trim(m.Get("song")).empty())
      p.push_back(Warning(who + " has both their own song and a game theme: their own song plays.")
                      .With("Keep the own song", Set(dir, "music", "")).With("Keep the game theme", Set(dir, "song", "")));
  }
  if (const std::string c = Trim(m.Get("cae")); !c.empty()) {  // (a created entrance: the game's 284-byte block)
    std::error_code ec;
    const auto size = fs::file_size(dir / fs::u8path(c), ec);
    if (ec || size != 284)
      p.push_back(Warning(who + "'s created entrance file (" + c + ") is " + (ec ? std::string("missing") : "damaged") +
                          ": " + who + " walks out with the entrance number instead.").With("Leave it out", Set(dir, "cae", "")));
  }
  const fs::path ch = dir / "ch.pac";
  if (!Exists(ch))
    p.push_back(Error(who + " has no body (the model file is missing): the game leaves " + who + " out.").With(kOpen, OpenIt(dir)).With("Turn it off", TurnOff(dir)));
  else if (!PacIsEpk8WithModels(ch))
    p.push_back(Error(who + "'s model file isn't a wrestler model: the game leaves " + who + " out.").With(kOpen, OpenIt(dir)).With("Turn it off", TurnOff(dir)));
  else {
    BudgetProblems(ch.wstring(), who, false, p);
    ModelCheckProblems(ch.wstring(), who, p);
    // Superstar Threads paints a mod's parts from its threads.txt (tools/threads_parts.py makes it from the model's
    // colour masks); installed mods only (a .svrmod's check runs on a cache copy)
    std::error_code ec;
    const fs::path mods = fs::absolute(fs::path(g_game) / L"Mods", ec), at = fs::absolute(dir, ec);
    const bool installed = !g_game.empty() && PathStr(at).rfind(PathStr(mods), 0) == 0;
    if (installed && !Exists(dir / "threads.txt")) {
      if (ThreadsNoMasks(dir))
        p.push_back(Warning(who + "'s model has no colour masks: Superstar Threads can't recolour " + who + "'s attires.",
                            "The fix paints colour masks for the model (its parts found from the body areas) and names them; "
                            "the model's old files are kept in the mod's .threads_backup folder.")
                        .With("Make colour masks and Threads parts", [dir, who] {
                          Status("Making colour masks and Superstar Threads parts for " + who + " ...");
                          MakeThreadsParts(dir, [who, dir](int code, const std::string& msg) {
                            if (code == 0)
                              Status(who + "'s colour masks and Superstar Threads parts are made" + (msg.empty() ? "" : ": " + msg) +
                                     " (the old model files are in " + PathStr(dir / ".threads_backup") + ").");
                            else Status("The colour masks couldn't be made (see the log).");
                            FixChanged();
                          }, true);
                        }, "colour masks and Threads parts are being made"));
      else
        p.push_back(Warning(who + " has no Superstar Threads parts: Superstar Threads can't recolour " + who + "'s attires.")
                        .With("Make Superstar Threads parts", [dir, who] {
                          Status("Making " + who + "'s Superstar Threads parts ...");
                          MakeThreadsParts(dir, [who](int code, const std::string& msg) {
                            if (code == 0) Status(who + "'s Superstar Threads parts are made" + (msg.empty() ? "." : ": " + msg + "."));
                            else if (code == 3) Status("This model has no colour masks: Superstar Threads can't recolour " + who + "'s attires.");
                            else Status("The Superstar Threads parts couldn't be made (see the log).");
                            FixChanged();
                          });
                        }, "Superstar Threads parts are being made"));
    }
  }
  if (!IsInt(Trim(m.Get("base")), 100, 321))
    p.push_back(Error(who + " has no fighting style: the game leaves " + who + " out.").With(kOpen, OpenIt(dir)));
  if (!Ascii(name))
    p.push_back(Warning(who + "'s name has letters the game can't show (accents or symbols): it shows wrong, and the video and song may "
                        "not be found.").With(kOpen, OpenIt(dir)));
  if (name.size() > 31)
    p.push_back(Warning(who + "'s name is too long: the game cuts it after 31 letters.").With("Cut it to 31 letters", Set(dir, "name", name.substr(0, 31))));
  SongCheck(dir, "song", m.Get("song"), p);
  MovieCheck(dir, "movie", m.Get("movie"), true, p);
  if (const std::string v = Trim(m.Get("voice")); !v.empty() && !Exists(dir / fs::u8path(v)))
    p.push_back(Warning("The name recording is missing from the mod: the announcer won't say " + who + "'s name.").With("Leave it out", Set(dir, "voice", "")));
  if (m.Has("ratings")) {
    int n = 0;
    std::istringstream r(m.Get("ratings"));
    for (std::string x; std::getline(r, x, ',');) ++n;
    if (n != 7)
      p.push_back(Warning(who + "'s ratings aren't complete: the game uses the fighting style's.").With("Use the style's ratings", Set(dir, "ratings", "")));
  }
  if (m.Has("call") && !IsInt(Trim(m.Get("call")), 0, 83))
    p.push_back(Warning("The announcer's call for " + who + " isn't one the game has: none is said.").With("Leave it out", Set(dir, "call", "")));
  if (const std::string h = Trim(m.Get("height")); !h.empty()) {
    const double v = std::atof(h.c_str());
    if (v < 0.8 || v > 1.25)
      p.push_back(Warning(who + "'s height is outside what the game allows (80% - 125% of normal): the game limits it.")
                      .With(v < 0.8 ? "Set it to 80%" : "Set it to 125%", Set(dir, "height", v < 0.8 ? "0.80" : "1.25")));
  }
  auto entrance_check = [&](const std::string& key, const std::string& whose) {
    long n = 0;
    if (!IsInt(Trim(m.Get(key)), 1, 65535, &n) || n < 100 || !EntrancesReady()) return;
    for (const auto& e : Entrances())
      if (e.number == n) return;
    if (Exists(dir / "moves" / "pack.txt")) return;  // (its own pack may add it)
    p.push_back(Warning(whose + " has no entrance installed: in the game " + who + " walks out with the fighting style's.")
                    .With("Leave it (the style's entrance)", Set(dir, key, "")).With(kOpen, OpenIt(dir)));
  };
  entrance_check("entrance", who);
  for (int a = 1; a <= 4; ++a) {
    const std::string k = "attire" + std::to_string(a);
    entrance_check(k + "_entrance", who + " in outfit " + std::to_string(a));
    const std::string f = Trim(m.Get(k));
    if (f.empty()) continue;
    if (!Exists(dir / fs::u8path(f)))
      p.push_back(Warning("Outfit " + std::to_string(a) + "'s file is missing from the mod: the outfit doesn't show.").With("Leave the outfit out", Set(dir, k, "")));
    else if (!PacIsEpk8WithModels(dir / fs::u8path(f)))
      p.push_back(Error("Outfit " + std::to_string(a) + "'s file isn't a wrestler model.").With("Leave the outfit out", Set(dir, k, "")));
    else
      BudgetProblems((dir / fs::u8path(f)).wstring(), "Outfit " + std::to_string(a), true, p);
  }
  for (const auto& [f, size] : {std::pair<const char*, int>{"render.dds", 512}, {"render_small.dds", 256}, {"render_full.dds", 512}})
    if (Exists(dir / f))
      if (const std::string is = DdsIssue(dir / f, size, size, {}, ""); !is.empty())
        p.push_back(Warning(who + "'s select picture is the wrong size or kind: the game shows the fighting style's picture instead.").With(kOpen, OpenIt(dir)));
  for (int k = 1; k <= 4; ++k) {
    const fs::path f = dir / ("sign" + std::to_string(k) + ".dds");
    if (!Exists(f)) continue;
    if (const std::string is = DdsIssue(f, 128, 64, {DxtFormat::kDxt1}, "DXT1"); !is.empty())
      p.push_back(Problem{false, "Crowd sign " + std::to_string(k) + " is the wrong size or kind: it may look wrong in the crowd.", ""});
  }
  if (const std::string mv = Trim(m.Get("moves")); !mv.empty()) {
    Bytes b;
    if (!ReadFile(PathStr(dir / fs::u8path(mv)), b)) {
      p.push_back(Error(who + "'s move list is missing from the mod: " + who + " fights with the fighting style's moves.").With("Use the style's moves", Set(dir, "moves", "")));
    } else {
      std::istringstream in(std::string(b.begin(), b.end()));
      int bad = 0, unknown = 0;
      const bool pack = Exists(dir / "moves" / "pack.txt");
      for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        const size_t e = l.find('=');
        const long off = e == std::string::npos ? -1 : std::strtol(l.substr(0, e).c_str(), nullptr, 16);
        const long id = e == std::string::npos ? 0 : std::atol(l.c_str() + e + 1);
        if (off < 0 || off > 0x1BF || off % 2 || id < 1 || id > 65535) { ++bad; continue; }
        if (!MoveInfos().empty() && !MoveInfos().count(int(id)) && !pack) ++unknown;
      }
      if (bad) p.push_back(Warning(std::to_string(bad) + " line" + (bad == 1 ? " of " + who + "'s move list is" : "s of " + who + "'s move list are") + " damaged and skipped.").With(kOpen, OpenIt(dir)));
      if (unknown)
        p.push_back(Warning(std::to_string(unknown) + " of " + who + "'s moves aren't in the game: " + who + " does the fighting style's move in "
                            + (unknown == 1 ? "that place." : "those places.")).With(kOpen, OpenIt(dir)));
    }
  }
  if (Exists(dir / "moves" / "pack.txt")) {
    long base = 0;
    const StarInfo* b = IsInt(Trim(m.Get("base")), 100, 321, &base) ? StarById(int(base)) : nullptr;
    CheckPackTxt(dir / "moves", dir, who + "'s extra moves", p, who, b ? b->name : "");
  }
  if (LeftOut(dir.parent_path()).count(PathStr(dir.filename())))
    p.push_back(Error("The game left " + who + " out the last time it started: too many superstar mods are on, or " + who + "'s model is "
                      "too big for the game's memory.").With("Turn " + who + " off", TurnOff(dir)).With(kOpen, OpenIt(dir)));
}

std::vector<int> StarAttiresOf(int star) {  // the attires in the stock superstar's pac
  std::vector<int> v;
  PacIndex idx;
  char id[8];
  std::snprintf(id, sizeof id, "%06d", star);
  if (ReadPacIndex(fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(star) + L".pac"), idx))
    for (const auto& e : idx.entries)
      if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, id) == 0 && e.name[7] == '2') v.push_back(e.name[6] - '0' + 1);
  return v;
}
bool GearStar(int star) {
  PacIndex idx;
  char id[8];
  std::snprintf(id, sizeof id, "%06d", star);
  if (ReadPacIndex(fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(star) + L".pac"), idx))
    for (const auto& e : idx.entries)
      if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, id) == 0 && e.name[6] == '0' && e.name[7] != '2') return true;
  return false;
}

void CheckAttire(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(dir, m, "attire", p);
  UnknownKeys(dir, m, {"superstar", "attire", "model", "entrance", "victory", "gear"}, {}, p);
  long star = 0;
  if (!IsInt(Trim(m.Get("superstar")), 100, 321, &star))
    p.push_back(Error("This outfit isn't set for any superstar: the game leaves it out.").With(kOpen, OpenIt(dir)));
  const std::string at = Trim(m.Get("attire"));
  long slot = 0;
  if (at != "new" && !IsInt(at, 1, 4, &slot))
    p.push_back(Error("This outfit isn't set to one of the superstar's 4 outfit places: the game leaves it out.").With("Add it as a new outfit", Set(dir, "attire", "new")));
  const std::string model = Trim(m.Get("model"));
  const StarInfo* si = star >= 100 ? StarById(int(star)) : nullptr;
  const std::string who = si ? si->name : std::string("the superstar");
  long entrance = 0;
  if (!Exists(dir / "attire.pac") && model.empty() && IsInt(Trim(m.Get("entrance")), 1, 65535, &entrance)) {
    // (no model: only an entrance for one of the superstar's own attires - Simple entrance porting)
    if (at == "new")
      p.push_back(Error("This entrance isn't tied to one of " + who + "'s outfits, so the game can't use it.").With("Use outfit 1", Set(dir, "attire", "1")));
    if (slot && star >= 100 && !g_game.empty()) {
      const auto own = StarAttiresOf(int(star));
      if (std::find(own.begin(), own.end(), int(slot)) == own.end())
        p.push_back(Warning("This entrance is for outfit " + at + ", which " + who + " doesn't have: it's only used when a mod gives them that outfit.")
                        .With("Use outfit 1", Set(dir, "attire", "1")));
    }
    std::error_code ec;
    std::vector<fs::path> dirs;  // (another entrance for the same attire: the first A-Z keeps it)
    for (const auto& e : fs::directory_iterator(dir.parent_path(), ec))
      if (e.is_directory(ec) && e.path() != dir && !Exists(e.path() / "disabled")) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
      const Manifest o = ReadManifest(d);
      if (Trim(o.Get("superstar")) == std::to_string(star) && Trim(o.Get("attire")) == at && !Trim(o.Get("entrance")).empty() &&
          !Exists(d / "attire.pac") && Trim(o.Get("model")).empty() && d < dir) {
        p.push_back(Warning("Another mod (\"" + PathStr(d.filename()) + "\") also gives " + who + " an entrance for outfit " + at +
                            ", and it wins: this one isn't used.").With("Turn this one off", TurnOff(dir)).With("Turn the other off", TurnOff(d)));
        break;
      }
    }
    return;
  }
  if (!Exists(dir / "attire.pac"))
    p.push_back(Error("The outfit's model file is missing: the launcher won't install it.").With(kOpen, OpenIt(dir)));
  else if (!PacIsEpk8WithModels(dir / "attire.pac", true))
    p.push_back(Error("The outfit's model file isn't a wrestler model.").With(kOpen, OpenIt(dir)));
  else
    BudgetProblems((dir / "attire.pac").wstring(), "This outfit", true, p);
  if (star >= 100 && !g_game.empty() && GearStar(int(star))) {
    const auto own = StarAttiresOf(int(star));
    if (at == "new" || (slot && std::find(own.begin(), own.end(), int(slot)) == own.end()))
      p.push_back(Error("This superstar wears a coat or gear for their entrance, and new outfits for them don't load in the game: it's "
                        "left out.").With("Replace their outfit 1 instead", Set(dir, "attire", "1")));
  }
  if (slot && !g_game.empty()) {  // another switched-on attire mod on the same slot (the first A-Z keeps it)
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(dir.parent_path(), ec))
      if (e.is_directory(ec) && e.path() != dir && !Exists(e.path() / "disabled")) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
      const Manifest o = ReadManifest(d);
      if (Trim(o.Get("superstar")) == std::to_string(star) && Trim(o.Get("attire")) == at &&
          (Exists(d / "attire.pac") || !Trim(o.Get("model")).empty())) {  // (an entrance-only mod holds no attire)
        p.push_back(Warning("Another outfit mod (\"" + PathStr(d.filename()) + "\") wants the same outfit place: when you play, the launcher "
                            "moves one of them to a free place.").With("Add this one as a new outfit", Set(dir, "attire", "new")));
        break;
      }
    }
  }
}

const char* const kArenaBases[] = {"arena_SD", "arena_RAW", "arena_WM26", "arena_slam", "arena_SS", "arena_HinC", "arena_Judg",
                                   "arena_BASH", "arena_NoC", "arena_BP", "arena_RR", "arena_BR", "arena_Series", "arena_TLC",
                                   "arena_ttt", "arena_EC", "arena_back", "arena_EXT", "arena_ECW", "arena_drui"};

// An arena's 0xC35A, the "MipmapData" table (XML, texture -> mip level): its NumOfData (every stock arena says 200), -1
// none. WWE 2K16 / 2K17 arenas converted before 16 Oct kept their own (241): the game crashed just before the match
// (2K17 bg09, a render target resolve into a null texture; the 2011 arena's table alone made it play - its own trimmed
// to 234 entries still crashed). WWE '13 ports keep theirs (234) and play: the check is for 2K16 / 2K17 sources only.
int MipmapDataCount(const Epac& e) {
  std::vector<PachEntry> ents;
  if (e.groups.empty() || e.groups[0].entries.empty() || !PachRead(e.groups[0].entries[0].data, ents)) return -1;
  for (const auto& x : ents)
    if (x.id == 0xC35A) {
      const Bytes u = Unpack(x.data);
      const std::string t(u.begin(), u.end());
      const size_t at = t.find("NumOfData=\"");
      return at == std::string::npos ? -1 : std::atoi(t.c_str() + at + 11);
    }
  return -1;
}
// the fix: the 2011 RAW arena's table put in (as the converter does now)
std::function<void()> RepairMipmapData(const fs::path& f) {
  return [f] {
    Bytes b, rb;
    Epac e, r;
    std::vector<PachEntry> ents, rents;
    if (!ReadFile(PathStr(f), b) || !EpacRead(b, e) || e.groups.empty() || e.groups[0].entries.empty() ||
        !PachRead(e.groups[0].entries[0].data, ents))
      return;
    if (g_game.empty() || !ReadFile(PathStr(fs::path(g_game) / "pac" / "bg" / "bg01.pac"), rb) || !EpacRead(rb, r) ||
        r.groups.empty() || r.groups[0].entries.empty() || !PachRead(r.groups[0].entries[0].data, rents))
      return;
    for (const auto& x : rents)
      if (x.id == 0xC35A)
        for (auto& y : ents)
          if (y.id == 0xC35A) y.data = x.data;
    e.groups[0].entries[0].data = PachWrite(ents);
    WriteFile(PathStr(f), EpacWrite(e));
  };
}

// An arena's light bank (0x7530, unpacked; empty: none)
Bytes LightBankOf(const Epac& e) {
  std::vector<PachEntry> ents;
  if (e.groups.empty() || e.groups[0].entries.empty() || !PachRead(e.groups[0].entries[0].data, ents)) return {};
  for (const auto& x : ents)
    if (x.id == 0x7530) return Unpack(x.data);
  return {};
}
// The fix for dark match lights (svrfmt DarkLightSets): those sets from the game's RAW arena (bg01, only read) into the
// mod's own arena file, as the converter does now
std::function<void()> RepairLights(const fs::path& f) {
  return [f] {
    Bytes b, rb;
    Epac e, r;
    std::vector<PachEntry> ents;
    if (!ReadFile(PathStr(f), b) || !EpacRead(b, e) || e.groups.empty() || e.groups[0].entries.empty() ||
        !PachRead(e.groups[0].entries[0].data, ents))
      return;
    if (g_game.empty() || !ReadFile(PathStr(fs::path(g_game) / "pac" / "bg" / "bg01.pac"), rb) || !EpacRead(rb, r))
      return;
    const Bytes host = LightBankOf(r);
    if (host.empty()) return;
    for (auto& y : ents)
      if (y.id == 0x7530) {
        const Bytes bank = Unpack(y.data);
        y.data = BpeEncode(ReplaceLightSets(bank, host, DarkLightSets(bank)));
      }
    e.groups[0].entries[0].data = PachWrite(ents);
    WriteFile(PathStr(f), EpacWrite(e));
  };
}

void CheckArenaPac(const fs::path& dir, const fs::path& f, std::vector<Problem>& p) {
  Bytes b;
  Epac e;
  if (!Exists(f)) p.push_back(Error("The arena's file is missing: the game skips it.").With(kOpen, OpenIt(dir)).With("Turn it off", TurnOff(dir)));
  else if (!ReadFile(PathStr(f), b) || b.size() < 0x800 || std::memcmp(b.data(), "EPAC", 4) || !EpacRead(b, e))
    p.push_back(Error("The arena's file is damaged.").With(kOpen, OpenIt(dir)).With("Turn it off", TurnOff(dir)));
  else if (const std::string src = Trim(ReadManifest(dir).Get("source"));
           (src.rfind("wwe2k16", 0) == 0 || src.rfind("wwe2k17", 0) == 0) && MipmapDataCount(e) != 200) {
    p.push_back(Error("The arena was made from a WWE 2K16 / 2K17 arena by an older Mod Maker: it crashes the game just before the "
                      "match.").With("Repair it", RepairMipmapData(f), "repaired (the game's own texture detail table put in)")
                    .With("Turn it off", TurnOff(dir)));
  }
  Arena a;
  if (!b.empty() && !std::memcmp(b.data(), "EPAC", 4) && a.LoadData(b)) {
    // (svrfmt/arena.h: over the game's own arenas' 3D detail needs 2.1.6; over kMaxArenaVertsNew it crashes)
    if (const ArenaGeometry g = GeometryOf(a); g.Over())
      p.push_back(Error("The arena has more 3D detail than the game can load (" + std::to_string(g.verts) + " points, " +
                        std::to_string(g.meshes) + " meshes; it takes up to " + std::to_string(kMaxArenaVertsNew) +
                        " points and " + std::to_string(kMaxArenaMeshesNew) + " meshes): the game crashes in the match.")
                      .With("Turn it off", TurnOff(dir)).With(kOpen, OpenIt(dir)));
    else if (g.OverStock())
      p.push_back(Warning("The arena has more 3D detail than the game's own arenas (" + std::to_string(g.verts) +
                          " points, " + std::to_string(g.meshes) + " meshes; theirs: up to " +
                          std::to_string(kMaxArenaVerts) + " points, " + std::to_string(kMaxArenaMeshes) + " meshes).",
                          std::string("Version ") + kNeedsVersion + " or newer plays it; older ones crash loading it.")
                      .With(kOpen, OpenIt(dir)));
    int odd = 0;  // (pictures the game can't show: 2011's arenas use DXT1 / DXT3 / DXT5 and ARGB)
    for (const auto& set : a.bundles)
      for (const auto& t : set.textures) {
        DdsInfo info;  // (one that isn't a picture file at all is left alone: stock bg23 has one)
        if (DdsInfoOf(t.data, info) && info.format != DxtFormat::kDxt1 && info.format != DxtFormat::kDxt3 &&
            info.format != DxtFormat::kDxt5 && info.format != DxtFormat::kArgb)
          ++odd;
      }
    if (odd)
      p.push_back(Warning(std::to_string(odd) + " of the arena's pictures are of a kind the game's arenas don't use: they may show "
                          "black or wrong.").With(kOpen, OpenIt(dir)));
  }
}

void CheckArena(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  if (m.found) ManifestBasics(dir, m, "arena", p), TrailingSpaces(dir, m, p);
  CheckArenaPac(dir, dir / "arena.pac", p);
  {  // (the match lighting: svrfmt/wwe13.h DarkLightSets)
    Bytes b;
    Epac e;
    if (ReadFile(PathStr(dir / "arena.pac"), b) && b.size() >= 0x800 && !std::memcmp(b.data(), "EPAC", 4) && EpacRead(b, e)) {
      const std::set<int> dark = DarkLightSets(LightBankOf(e));
      if (dark.count(10))
        p.push_back(Warning("The arena's match lighting is much darker than the game's arenas: wrestlers and the ring look dark "
                            "in the match (bright at the entrance).")
                        .With("Repair it", RepairLights(dir / "arena.pac"), "repaired (the game's own match lighting put in)"));
      else if (dark.count(100))
        p.push_back(Warning("Parts of the arena have no light from any side: they look flat and dark.")
                        .With("Repair it", RepairLights(dir / "arena.pac"), "repaired (the game's own light for them put in)"));
    }
  }
  if (!Exists(dir / "banner.dds"))
    p.push_back(Error("The arena has no select screen picture: the launcher won't install it.").With(kOpen, OpenIt(dir)));
  else if (const std::string is = DdsIssue(dir / "banner.dds", 256, 128, {DxtFormat::kDxt5}, "DXT5"); !is.empty())
    p.push_back(Warning("The arena's select screen picture is the wrong size or kind: its tile shows dark.").With(kOpen, OpenIt(dir)));
  if (Exists(dir / "load.dds"))
    if (const std::string is = DdsIssue(dir / "load.dds", 1024, 512, {DxtFormat::kDxt5}, "DXT5"); !is.empty())
      p.push_back(Warning("The arena's loading picture is the wrong size or kind: the usual one shows.").With(kOpen, OpenIt(dir)));
  std::error_code ec;
  int bad_vs = 0;
  for (const auto& e : fs::directory_iterator(dir / "vs", ec))
    if (e.is_regular_file(ec) && Ext(PathStr(e.path())) == ".dds")
      if (const std::string is = DdsIssue(e.path(), 0, 0, {DxtFormat::kDxt5}, "DXT5"); !is.empty()) ++bad_vs;
  if (bad_vs) p.push_back(Warning(std::to_string(bad_vs) + " of the VS screen's pictures are the wrong kind: the usual ones show there.").With(kOpen, OpenIt(dir)));
  if (const std::string src = Trim(m.Get("source")); SpecialStage(src)) {  // (converted from a WWE '12+ special stage)
    if (ArenaPacNumber(src) == 78)
      p.push_back(Error("It was made from a backstage room, not an arena: on an arena slot it crashes the game.").With("Turn it off", TurnOff(dir)));
    else
      p.push_back(Warning("It was made from a studio or backstage stage, not an arena: in a match it shows a white mat over a black void.")
                      .With("Turn it off", TurnOff(dir)));
  }
  if (const std::string base = Trim(m.Get("base")); !base.empty() &&
      std::find_if(std::begin(kArenaBases), std::end(kArenaBases), [&](const char* x) { return base == x; }) == std::end(kArenaBases))
    p.push_back(Problem{false, "The arena isn't tied to one of the game's 20 arenas: it goes on an extra select page.", ""});
  if (const std::string g = Trim(m.Get("ring.rope_base")); !g.empty() && (std::atof(g.c_str()) < 1.0 || std::atof(g.c_str()) > 8.0))
    p.push_back(Warning("The ring's lowest rope is set to a height the game doesn't allow: the usual height is used.").With("Use the usual height", Set(dir, "ring.rope_base", "")));
  if (const std::string g = Trim(m.Get("ring.rope_gap")); !g.empty() && (std::atof(g.c_str()) < 1.5 || std::atof(g.c_str()) > 6.0))
    p.push_back(Warning("The gap between the ropes is set to a size the game doesn't allow: the usual gap is used.").With("Use the usual gap", Set(dir, "ring.rope_gap", "")));
}

void CheckBackstage(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(dir, m, "backstage", p);
  CheckArenaPac(dir, dir / "arena.pac", p);
  if (m.Has("row") && !IsInt(Trim(m.Get("area")), 0, 6))
    p.push_back(Error("The area isn't set to one of the game's backstage rooms: the game leaves it out.").With(kOpen, OpenIt(dir)));
  if (const std::string g = Trim(m.Get("gimmick")); !g.empty()) {
    if (g.size() != 4) p.push_back(Warning("The props' name isn't 4 letters: the area has no props.").With("Leave the props out", Set(dir, "gimmick", "")));
    if (!Exists(dir / "gimmick.pac")) p.push_back(Warning("The props' file is missing: the area has no props.").With("Leave the props out", Set(dir, "gimmick", "")));
  }
}

void CheckSigns(const fs::path& dir, std::vector<Problem>& p) {
  std::error_code ec;
  int n = 0, upper = 0, bad = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string ext = PathStr(e.path().extension());
    if (Lower(ext) != ".dds") continue;
    if (ext != ".dds") { ++upper; continue; }
    ++n;
    if (const std::string is = DdsIssue(e.path(), 128, 64, {DxtFormat::kDxt1}, "DXT1"); !is.empty()) ++bad;
  }
  if (bad) p.push_back(Warning(std::to_string(bad) + " sign" + (bad == 1 ? " is" : "s are") + " the wrong size or kind: they may look wrong or be skipped.").With(kOpen, OpenIt(dir)));
  if (upper)
    p.push_back(Warning(std::to_string(upper) + " sign file" + (upper == 1 ? "'s name ends" : "s' names end") + " in capitals (.DDS): the game skips them.")
                    .With("Rename them", [dir] {
                      std::error_code e2;
                      for (const auto& e : fs::directory_iterator(dir, e2))
                        if (e.is_regular_file(e2) && PathStr(e.path().extension()) != ".dds" && Lower(PathStr(e.path().extension())) == ".dds") {
                          fs::path to = e.path();
                          to.replace_extension(".dds");
                          fs::rename(e.path(), to, e2);
                        }
                    }, "renamed"));
  if (!n && !upper) p.push_back(Error("There are no signs in it.").With("Turn it off", TurnOff(dir)));
}

void CheckMedia(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  ManifestBasics(dir, m, "media", p);
  TrailingSpaces(dir, m, p);
  int low = 0;
  for (const auto& [k, v] : m.keys) {
    const size_t dot = k.find('.');
    const std::string what = dot == std::string::npos ? k : k.substr(0, dot), id = dot == std::string::npos ? "" : k.substr(dot + 1);
    if (!id.empty() && std::isdigit(uint8_t(id[0])) && std::atoi(id.c_str()) < 50 && (what == "video" || what == "theme")) ++low;
    if (what == "video") MovieCheck(dir, k, v, true, p);
    else if (what == "theme" || k == "menu_music" || what == "sound") SongCheck(dir, k, v, p);
    else if (what == "render" || what == "bust" || what == "icon") {
      const int size = what == "render" ? 512 : what == "bust" ? 256 : 64;
      const std::string f = Trim(v);
      if (!Exists(dir / fs::u8path(f))) p.push_back(Error(Thing(k) + " is missing from the mod.").With("Leave it out", Set(dir, k, "")));
      else if (const std::string is = DdsIssue(dir / fs::u8path(f), size, size, {}, ""); !is.empty())
        p.push_back(Warning(Thing(k) + " is the wrong size or kind: the usual picture shows.").With(kOpen, OpenIt(dir)));
    } else if (what == "arena") {
      const std::string f = Trim(v);
      if (!Exists(dir / fs::u8path(f))) p.push_back(Error(Thing(k) + "'s file is missing from the mod: the usual arena plays.").With("Leave it out", Set(dir, k, "")));
    }
  }
  if (low)
    p.push_back(Warning(std::to_string(low) + " video" + (low == 1 ? " or song is" : "s or songs are") + " set for Created Superstars, which the "
                        "game doesn't change: they're unused."));
}

void CheckBackgrounds(const fs::path& dir, std::vector<Problem>& p) {
  std::error_code ec;
  int n = 0, bad = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec) || Lower(PathStr(e.path().extension())) != ".dds") continue;
    ++n;
    if (const std::string is = DdsIssue(e.path(), 1024, 512, {DxtFormat::kDxt5}, "DXT5"); !is.empty()) ++bad;
  }
  if (bad) p.push_back(Warning(std::to_string(bad) + " of the pictures " + (bad == 1 ? "is" : "are") + " the wrong size or kind: the menu skips them.").With(kOpen, OpenIt(dir)));
  if (!n) p.push_back(Error("There are no pictures in it: the launcher won't install it.").With("Turn it off", TurnOff(dir)));
}

void CheckMatchType(const fs::path& dir, std::vector<Problem>& p) {
  static const char* const kIds[] = {"falls_count_anywhere", "championship_scramble", "royal_rumble_15_25", "lumberjack", "free_roaming_backstage",
                                     "backstage_more_people", "weapons_everywhere", "slobber_knocker", "three_stages_of_hell", "elimination",
                                     "mystery_opponent", "hiac_tables_ladders", "hiac_roof_start", "gauntlet_eliminator", "cage_weapons",
                                     "inferno_weapons", "online_first_blood"};
  const std::string f = PathStr(dir.filename());
  if (std::find_if(std::begin(kIds), std::end(kIds), [&](const char* x) { return f == x; }) == std::end(kIds))
    p.push_back(Warning("The game doesn't know this match type (brand-new match types don't work in the game yet): it does nothing.")
                    .With("Turn it off", TurnOff(dir)));
}

// A title mod (docs/TITLE_MODS.md): a known title key, something to replace, pictures named by a texture hash.
constexpr const char* kTitleKeys[] = {"IC", "US", "WWE", "WHC", "WWE_TAG", "WORLD_TAG", "ECW", "DIVAS", "WOMENS",
                                      "HARDCORE", "CRUISER", "MILLION", "WCW"};
void CheckTitle(const fs::path& dir, std::vector<Problem>& p) {
  const Manifest m = ReadManifest(dir);
  if (Trim(m.Get("new")) == "1") {  // (a new title: title_new.cpp)
    p.push_back(Warning("New titles aren't supported yet: the game leaves this mod out."));
    return;
  }
  std::string key = Trim(m.Get("title"));
  for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (std::none_of(std::begin(kTitleKeys), std::end(kTitleKeys), [&](const char* k) { return key == k; }))
    p.push_back(Error("Its title= isn't one of the game's titles (IC, US, WWE, WHC, WWE_TAG, WORLD_TAG, ECW, DIVAS, WOMENS, "
                      "HARDCORE, CRUISER, MILLION, WCW): the game leaves it out.").With("Turn it off", TurnOff(dir)));
  std::error_code ec;
  int pictures = 0, unnamed = 0;
  for (const auto& e : fs::recursive_directory_iterator(dir / "textures", ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string ext = Lower(PathStr(e.path().extension())), stem = PathStr(e.path().stem());
    if (ext != ".dds" && ext != ".png") continue;
    const size_t u = stem.rfind('_');
    const std::string h = u == std::string::npos ? stem : stem.substr(u + 1);
    const bool hashed = h.size() == 16 && std::all_of(h.begin(), h.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
    (hashed ? pictures : unnamed) += 1;
  }
  if (unnamed) p.push_back(Warning(std::to_string(unnamed) + " of its pictures have no texture hash at the end of the name (<name>_<16 hex>): the game skips them.").With(kOpen, OpenIt(dir / "textures")));
  if (Trim(m.Get("name")).empty() && !pictures && !Exists(dir / "pack.txt"))
    p.push_back(Error("It changes nothing: no name=, no pictures in textures\\, no pack.txt.").With("Turn it off", TurnOff(dir)));
}

}  // namespace

std::string KindOfType(const std::string& type) {
  const std::string t = Lower(Trim(type));
  if (t == "superstar") return "Superstars";
  if (t == "attire") return "Attires";
  if (t == "arena") return "Arenas";
  if (t == "moves") return "Moves";
  if (t == "backstage") return "Backstage";
  if (t == "signs") return "Signs";
  if (t == "media") return "Media";
  if (t == "background") return "Backgrounds";
  if (t == "matchtype") return "MatchTypes";
  if (t == "title") return "Titles";
  return "";
}

std::vector<Problem> CheckModFolder(const std::string& kind, const fs::path& dir) {
  std::vector<Problem> p;
  if (kind == "Superstars") CheckSuperstar(dir, p);
  else if (kind == "Attires") CheckAttire(dir, p);
  else if (kind == "Arenas") CheckArena(dir, p);
  else if (kind == "Moves") {
    CheckPackTxt(dir, dir, "These moves", p);
    p.push_back(Problem{false, "Moves on their own don't install on Android yet: they work on PC, or inside a superstar mod.", ""});
  } else if (kind == "Backstage") CheckBackstage(dir, p);
  else if (kind == "Signs") CheckSigns(dir, p);
  else if (kind == "Media") CheckMedia(dir, p);
  else if (kind == "Backgrounds") CheckBackgrounds(dir, p);
  else if (kind == "MatchTypes") CheckMatchType(dir, p);
  else if (kind == "Titles") CheckTitle(dir, p);
  if (Exists(dir / "disabled"))
    p.push_back(Problem{false, "It's switched off: the game doesn't load it.", ""}.With("Switch it on", [dir] {
      std::error_code ec;
      fs::remove(dir / "disabled", ec);
    }));
  return p;
}

std::vector<Problem> CheckInstalledSet() {
  std::vector<Problem> p;
  if (g_game.empty()) return p;
  const fs::path mods = fs::path(g_game) / L"Mods";
  std::error_code ec;
  auto on_dirs = [&](const char* kind) {
    std::vector<fs::path> v;
    for (const auto& e : fs::directory_iterator(mods / kind, ec))
      if (e.is_directory(ec) && !Exists(e.path() / "disabled")) v.push_back(e.path());
    return v;
  };
  const auto stars = on_dirs("Superstars");
  int with_pac = 0, movies = 0;
  for (const auto& d : stars) {
    with_pac += Exists(d / "ch.pac");
    if (!Trim(ReadManifest(d).Get("movie")).empty()) ++movies;
  }
  if (with_pac > 137) p.push_back(Error(std::to_string(with_pac) + " superstar mods are on, and the game holds 137: the rest are left out.").With("See them", [] { GoTo(PageId::kInstalled); }));
  const auto left = LeftOut(mods / "Superstars");
  if (!left.empty()) {
    std::string l;
    for (const auto& f : left) l += (l.empty() ? "" : ", ") + f;
    p.push_back(Error("The last time the game started it left out: " + l + " (too many superstar mods on, or a model too big for the game's memory).", "Pick each one to see why."));
  }
  for (const auto& d : on_dirs("Media")) {
    const Manifest m = ReadManifest(d);
    for (const auto& [k, v] : m.keys) movies += k.rfind("video.", 0) == 0;
  }
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Custom Movies", ec)) movies += e.is_regular_file(ec);
  if (movies > 200) p.push_back(Warning(std::to_string(movies) + " videos from mods and Custom Movies: the game has room for 200, so the rest don't play."));
  int signs = 0;
  for (const auto& d : on_dirs("Signs"))
    for (const auto& e : fs::directory_iterator(d, ec)) signs += e.is_regular_file(ec) && PathStr(e.path().extension()) == ".dds";
  // (400: the sign packs' ids, 6001-9991 - crowd_signs.cpp; not memory: the match load's heap (16 MB since 2.1.9)
  // takes the whole bank's two copies even at 400 pack signs plus 4 for each of 137 superstars)
  if (signs > 400) p.push_back(Warning(std::to_string(signs) + " crowd signs are on in sign packs: the game has room for 400 of them, so the rest never show."));
  int rows = 0, plain = 0;
  for (const auto& d : on_dirs("Backstage")) (ReadManifest(d).Has("row") ? rows : plain) += 1;
  if (rows > 8) p.push_back(Warning(std::to_string(rows) + " new backstage areas are on: the menu holds 8, so the rest don't show."));
  if (plain > 1) p.push_back(Warning(std::to_string(plain) + " backstage mods change the same area: only one of them plays."));
  {  // (two title mods on one title: the first by folder name plays - title_mods.cpp)
    std::map<std::string, std::vector<std::string>> by_title;
    auto titles = on_dirs("Titles");
    std::sort(titles.begin(), titles.end());
    for (const auto& d : titles) {
      std::string key = Trim(ReadManifest(d).Get("title"));
      for (char& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
      by_title[key].push_back(PathStr(d.filename()));
    }
    for (const auto& [key, folders] : by_title)
      if (folders.size() > 1) {
        std::string rest;
        for (size_t i = 1; i < folders.size(); ++i) rest += (i > 1 ? ", " : "") + folders[i];
        p.push_back(Warning(std::to_string(folders.size()) + " title mods replace the " + key + " title: " + folders[0] +
                            " plays, the game leaves out " + rest + ".").With("See them", [] { GoTo(PageId::kInstalled); }));
      }
  }
  return p;
}

std::string InstallReport(const std::string& kind, const fs::path& dir) {
  // (on the UI thread: the model budget caches aren't shared with the install's worker)
  OnUiThread([kind, dir] {
    const auto p = CheckModFolder(kind, dir);
    int errors = 0, warnings = 0;
    for (const auto& x : p) {
      if (x.error) ++errors;
      else ++warnings;
      Log(std::string(x.error ? "  must fix: " : "  check: ") + x.text + (x.fix.empty() ? "" : " - " + x.fix));
    }
    if (errors || warnings)
      Status("Installed - the check found " + (errors ? std::to_string(errors) + " to fix" + (warnings ? ", " : "") : std::string()) +
             (warnings ? std::to_string(warnings) + " to look at" : std::string()) + " (in the log, and on the Installed mods page).");
  });
  return "";
}

std::vector<Problem> CheckSvrmod(const std::wstring& file) {
  std::vector<Problem> p;
  Bytes zip;
  std::vector<ZipEntry> files;
  if (!ReadFile(Utf8(file), zip) || !ZipRead(zip, files)) {
    p.push_back(Error("This mod file is damaged: the launcher can't open it.", "Make it again in the Mod Maker."));
    return p;
  }
  // unpacked into the cache (the checks read files)
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / L"svr2011_modcheck" / fs::path(file).stem();
  fs::remove_all(dir, ec);
  for (const auto& f : files) {
    if (f.name.find("..") != std::string::npos || f.name.find(':') != std::string::npos) {
      p.push_back(Error("This mod file has a file inside it the launcher refuses (" + f.name + ").", "Make it again in the Mod Maker."));
      continue;
    }
    const fs::path out = dir / fs::u8path(f.name);
    fs::create_directories(out.parent_path(), ec);
    WriteFile(PathStr(out), f.data);
  }
  const Manifest m = ReadManifest(dir);
  const std::string kind = KindOfType(m.Get("type"));
  if (!m.found) p.push_back(Error("This mod file has no settings file inside: the launcher won't install it.", "Make it again in the Mod Maker."));
  else if (kind.empty()) p.push_back(Error("This mod file is of a kind the launcher doesn't know.", "Make it again in the Mod Maker."));
  else {
    auto more = CheckModFolder(kind, dir);
    for (auto& x : more) x.fixes.clear();  // (a copy in the temp folder: install it, then fix it on the Installed page)
    p.insert(p.end(), more.begin(), more.end());
  }
  fs::remove_all(dir, ec);
  return p;
}

}  // namespace mm
