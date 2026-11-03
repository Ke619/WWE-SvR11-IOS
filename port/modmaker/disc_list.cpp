// My game discs (disc_list.h): the list, Find my discs and the page.
#include "disc_list.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "app.h"
#include "svrfmt/disc.h"

namespace mm {
namespace disc_list {

namespace {

// The ids are the converters' disc_hint(s) (tools/*_port.py `games`; docs/MOD_MAKER_3_PLAN.md).
// (disc ids: the 360 title id; PS2 serials PAL first, then NTSC-U - as tools/ps2_port.py SERIALS)
const std::vector<Game> kGames = {
    {"svr2007", "SmackDown vs. Raw 2007", true, {"545107E0"}},
    {"svr2008", "SmackDown vs. Raw 2008", true, {"5451080B"}},
    {"svr2009", "SmackDown vs. Raw 2009", true, {"54510826"}},
    {"svr2010", "SmackDown vs. Raw 2010", true, {"54510844"}},
    {"low", "Legends of WrestleMania", true, {"54510838"}},
    {"wwe12", "WWE '12", true, {"54510890"}},
    {"wwe13", "WWE '13", true, {"545108B4"}},
    {"wwe2k14", "WWE 2K14", true, {"545408B2"}},
    {"wwe2k15", "WWE 2K15", true, {"545408B6"}},
    {"wwe2k16", "WWE 2K16", true, {"315A07D2"}},
    {"wwe2k17", "WWE 2K17", true, {"545408BA"}},
    {"ps2_jbi", "SmackDown! Just Bring It", false, {"SLES_504.77", "SLUS_203.16", "SLUS_202.69", "SLUS_201.22"}},
    {"ps2_sym", "SmackDown! Shut Your Mouth", false, {"SLES_512.83", "SLUS_204.83", "SLUS_205.12"}},
    {"ps2_hctp", "SmackDown! Here Comes the Pain", false, {"SLES_520.36", "SLUS_207.87"}},
    {"ps2_svr04", "SmackDown! vs. Raw", false, {"SLES_527.81", "SLUS_210.60", "SLUS_209.36"}},
    {"ps2_svr06", "SmackDown! vs. Raw 2006", false, {"SLES_536.76", "SLUS_212.86", "SLUS_211.34"}},
    {"ps2_svr07", "SmackDown vs. Raw 2007 (PS2)", false, {"SLES_544.89", "SLUS_214.27"}},
    {"ps2_svr08", "SmackDown vs. Raw 2008 (PS2)", false, {"SLES_548.79", "SLUS_216.45"}},
    {"ps2_svr09", "SmackDown vs. Raw 2009 (PS2)", false, {"SLES_552.51", "SLUS_218.10"}},
    {"ps2_svr10", "SmackDown vs. Raw 2010 (PS2)", false, {"SLES_555.45", "SLUS_219.01"}},
    {"ps2_svr11", "SmackDown vs. Raw 2011 (PS2)", false, {"SLES_556.35", "SLUS_219.39"}},
};

Settings::MyDisc* Find(const std::string& key) {
  for (auto& d : g_settings.my_discs)
    if (d.key == key) return &d;
  return nullptr;
}

// ---- reading an id

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// The title id from a default.xex header (XEX2 execution info, key 0x40006): as svrfmt::Disc and discfs.py do.
std::string XexId(const fs::path& xex) {
  Bytes h;
  FILE* f = _wfopen(xex.c_str(), L"rb");
  if (!f) return {};
  h.resize(0x10000);
  h.resize(std::fread(h.data(), 1, h.size(), f));
  std::fclose(f);
  if (h.size() < 0x18 || std::memcmp(h.data(), "XEX2", 4)) return {};
  const uint32_t n = std::min<uint32_t>(Be32(&h[0x14]), 64);
  for (uint32_t k = 0; k < n && 0x18 + k * 8 + 8 <= h.size(); ++k)
    if (Be32(&h[0x18 + k * 8]) == 0x00040006) {
      const uint32_t v = Be32(&h[0x18 + k * 8 + 4]);
      if (v + 16 <= h.size()) {
        char t[16];
        std::snprintf(t, sizeof t, "%08X", Be32(&h[v + 12]));
        return t;
      }
    }
  return {};
}

// The serial from SYSTEM.CNF's BOOT2 line (BOOT2 = cdrom0:\SLES_548.79;1).
std::string CnfId(const fs::path& cnf) {
  Bytes b;
  if (!svrfmt::ReadFile(PathStr(cnf), b)) return {};
  const std::string s(b.begin(), b.end());
  const size_t at = s.find("BOOT2");
  if (at == std::string::npos) return {};
  const size_t eq = s.find('=', at), eol = s.find_first_of("\r\n", at);
  if (eq == std::string::npos || eq > eol) return {};
  std::string v = s.substr(eq + 1, eol == std::string::npos ? std::string::npos : eol - eq - 1);
  if (const size_t semi = v.find(';'); semi != std::string::npos) v.resize(semi);
  if (const size_t sl = v.find_last_of("\\/:"); sl != std::string::npos) v = v.substr(sl + 1);
  while (!v.empty() && v.back() == ' ') v.pop_back();
  while (!v.empty() && v.front() == ' ') v.erase(0, 1);
  return Upper(v);
}

std::string FolderId(const fs::path& dir, bool* xbox) {
  std::error_code ec;
  if (fs::is_regular_file(dir / L"default.xex", ec)) {
    if (xbox) *xbox = true;
    return XexId(dir / L"default.xex");
  }
  if (fs::is_regular_file(dir / L"SYSTEM.CNF", ec)) {
    if (xbox) *xbox = false;
    return CnfId(dir / L"SYSTEM.CNF");
  }
  return {};
}

// ---- Find my discs

struct Found {
  std::wstring path;
  std::string id;
};
std::atomic<bool> g_scanning{false}, g_stop{false};
std::mutex g_scan_mutex;
std::string g_scan_now;       // what the search is looking at (under the mutex)
std::atomic<int> g_scan_dirs{0}, g_scan_isos{0};
std::string g_last_result;    // the last search's summary (the page shows it)
std::wstring g_test_report;   // --discs-find ...,<report>
bool g_test_pending = false;
std::wstring g_test_folder;

void WriteReport() {
  std::string t = "result=" + g_last_result + "\n";
  for (const auto& g : kGames) t += std::string(g.key) + "=" + Utf8(PathFor(g.key)) + "|" + IdFor(g.key) + "\n";
  svrfmt::WriteFile(Utf8(g_test_report), Bytes(t.begin(), t.end()));
  WriteLogTo(g_test_report + L".log");
}

void ApplyFound(std::vector<Found> found, const std::wstring& folder, bool stopped) {
  const auto order = [](const Found& f) {  // (in the page's order)
    const Game* g = GameById(f.id);
    return g ? size_t(g - kGames.data()) : kGames.size();
  };
  std::stable_sort(found.begin(), found.end(), [&](const Found& a, const Found& b) { return order(a) < order(b); });
  int added = 0, kept = 0, other = 0;
  std::string names;
  for (const auto& f : found) {
    const Game* g = GameById(f.id);
    if (!g) {
      ++other;
      Log("discs: not one the converters know: " + Utf8(f.path) + " (" + (f.id.empty() ? std::string("no id") : f.id) + ")");
      continue;
    }
    if (!PathFor(g->key).empty()) {  // (a disc that is still there stays: the user's own pick)
      if (PathFor(g->key) != f.path) Log("discs: " + std::string(g->name) + " already set; also at " + Utf8(f.path));
      ++kept;
      continue;
    }
    Set(g->key, f.path, f.id);
    ++added;
    names += (names.empty() ? "" : ", ") + std::string(g->name);
    Log("discs: " + FullName(*g) + " = " + Utf8(f.path));
  }
  g_last_result = std::string(stopped ? "Stopped. " : "") + "Found " + std::to_string(added) + " new" +
                  (names.empty() ? "" : " (" + names + ")") + (kept ? ", " + std::to_string(kept) + " already set" : "") +
                  (other ? ", " + std::to_string(other) + " other disc" + (other == 1 ? "" : "s") + " not used" : "") +
                  " in " + Utf8(folder) + ".";
  Status(g_last_result);
  RescanDiscs();
  if (!g_test_report.empty()) {
    WriteReport();
    g_test_report.clear();
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  }
}

// ---- the page's state

double g_exists_at = -10;
std::map<std::string, bool> g_exists;  // key -> the set path is there (checked every few seconds)

bool Exists(const std::string& key) {
  if (ImGui::GetTime() - g_exists_at > 3) {
    g_exists.clear();
    std::error_code ec;
    for (const auto& d : g_settings.my_discs) g_exists[d.key] = !d.path.empty() && fs::exists(d.path, ec);
    g_exists_at = ImGui::GetTime();
  }
  const auto it = g_exists.find(key);
  return it != g_exists.end() && it->second;
}

std::string g_note;  // a message under the list (a picked file wasn't that game ...)

void PickFor(const Game& g, bool folder) {
  static const COMDLG_FILTERSPEC kIso[1] = {{L"Disc images (*.iso)", L"*.iso"}};
  const std::wstring title = Wide(std::string(folder ? "The extracted folder of " : "The disc image of ") + FullName(g));
  const std::wstring start = OtherGameStart(g.key);
  const std::wstring p = folder ? PickFolder(title.c_str(), start) : PickFile(false, title.c_str(), kIso, 1, nullptr, nullptr, start);
  if (p.empty()) return;
  if (const std::string why = NotGameFiles(p); !why.empty()) {  // (not set: the game keeps what it had)
    g_note = FullName(g) + ": " + why;
    Log("discs: not set for " + FullName(g) + ": " + Utf8(p) + " - " + why);
    return;
  }
  bool xbox = g.xbox;
  const std::string id = Identify(p, &xbox);
  const Game* is = GameById(id);
  OtherGamePicked(p, is ? is->key : g.key);
  if (is && is != &g) {
    Set(is->key, p, id);
    g_note = "That disc is " + FullName(*is) + ", not " + FullName(g) + ": it is set for " + is->name + ".";
  } else if (!is) {
    Set(g.key, p, id);
    g_note = id.empty() ? FullName(g) + ": the disc's id couldn't be read there - it is used as you picked it."
                        : FullName(g) + ": that disc's id (" + id + ") isn't this game's - it is used as you picked it.";
  } else {
    Set(g.key, p, id);
    g_note.clear();
    Status(FullName(g) + ": " + Utf8(p));
  }
}

void GameRow(const Game& g) {
  ImGui::PushID(g.key);
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  const std::wstring path = Find(g.key) ? Find(g.key)->path : L"";
  const bool there = !path.empty() && Exists(g.key);
  if (there) ImGui::TextUnformatted(g.name);
  else ImGui::TextDisabled("%s", g.name);
  ImGui::TableNextColumn();
  if (path.empty()) {
    ImGui::TextDisabled("not set");
  } else {
    std::error_code ec;
    const bool dir = fs::is_directory(path, ec);
    if (there) ImGui::TextUnformatted((Utf8(path) + (dir ? "  (folder)" : "")).c_str());
    else ImGui::TextColored(kWarn, "not found: %s", Utf8(path).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDisc id: %s", Utf8(path).c_str(), IdFor(g.key).c_str());
    if (there && dir)  // (some of the game's files: what that can do)
      if (const std::string note = FolderNote(g.key); !note.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kWarn, "%s", note.c_str());
        ImGui::PopTextWrapPos();
      }
  }
  ImGui::TableNextColumn();  // what the converters bring from it (their `games` lists, asked once)
  {
    const std::string kinds = port_page::KindsFor(IdFor(g.key));
    if (!kinds.empty()) {
      ImGui::TextDisabled("%s", Lower(kinds).c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("What the From another game tabs can bring from this game: %s.", Lower(kinds).c_str());
    }
  }
  ImGui::TableNextColumn();
  const char* pick = path.empty() ? "Add..." : "Change...";
  if (ImGui::SmallButton(pick)) ImGui::OpenPopup("pick");
  if (ImGui::BeginPopup("pick")) {
    if (ImGui::MenuItem("A disc image (.iso)...")) PickFor(g, false);
    if (ImGui::MenuItem("An extracted folder...")) PickFor(g, true);
    ImGui::EndPopup();
  }
  if (!path.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) Remove(g.key), g_note.clear();
  }
  ImGui::PopID();
}

void Table(bool xbox) {
  ImGui::TextDisabled(xbox ? "XBOX 360" : "PLAYSTATION 2");
  if (!ImGui::BeginTable(xbox ? "x360" : "ps2", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    return;
  ImGui::TableSetupColumn("game", ImGuiTableColumnFlags_WidthFixed, 250 * g_scale);
  ImGui::TableSetupColumn("disc", ImGuiTableColumnFlags_WidthStretch, 3);
  ImGui::TableSetupColumn("kinds", ImGuiTableColumnFlags_WidthStretch, 2);
  ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed, 150 * g_scale);
  for (const auto& g : kGames)
    if (g.xbox == xbox) GameRow(g);
  ImGui::EndTable();
}

}  // namespace

const std::vector<Game>& Games() { return kGames; }

const Game* GameByKey(const std::string& key) {
  for (const auto& g : kGames)
    if (key == g.key) return &g;
  return nullptr;
}

const Game* GameById(const std::string& disc_id) {
  if (disc_id.empty()) return nullptr;
  const std::string u = Upper(disc_id);
  for (const auto& g : kGames)
    for (const char* id : g.ids)
      if (u == id) return &g;
  return nullptr;
}

std::string FullName(const Game& g) {
  const std::string n = g.name;
  if (n.find("(PS2)") != std::string::npos) return n;
  return n + (g.xbox ? " (Xbox 360)" : " (PS2)");
}

std::wstring PathFor(const std::string& key) {
  const Settings::MyDisc* d = Find(key);
  std::error_code ec;
  return d && !d->path.empty() && fs::exists(d->path, ec) ? d->path : L"";
}

std::string IdFor(const std::string& key) {
  const Settings::MyDisc* d = Find(key);
  if (d && !d->id.empty()) return d->id;
  const Game* g = GameByKey(key);
  return g && !g->ids.empty() ? g->ids[0] : "";
}

void Set(const std::string& key, const std::wstring& path, const std::string& disc_id) {
  if (path.empty()) return Remove(key);
  Settings::MyDisc* d = Find(key);
  if (!d) {
    g_settings.my_discs.push_back({key, "", L""});
    d = &g_settings.my_discs.back();
  }
  d->path = path;
  d->id = disc_id.empty() ? Identify(path) : disc_id;
  // (kept in the page's order)
  std::stable_sort(g_settings.my_discs.begin(), g_settings.my_discs.end(), [](const Settings::MyDisc& a, const Settings::MyDisc& b) {
    const auto at = [](const std::string& k) {
      for (size_t i = 0; i < kGames.size(); ++i)
        if (k == kGames[i].key) return i;
      return kGames.size();
    };
    return at(a.key) < at(b.key);
  });
  g_exists_at = -10;
  SaveSettings();
  RescanDiscs();
}

void Remove(const std::string& key) {
  auto& v = g_settings.my_discs;
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Settings::MyDisc& d) { return d.key == key; }), v.end());
  g_exists_at = -10;
  SaveSettings();
  RescanDiscs();
}

std::vector<std::string> SetKeys() {
  std::vector<std::string> out;
  for (const auto& g : kGames)
    if (Find(g.key)) out.push_back(g.key);
  return out;
}

std::string Identify(const std::wstring& path, bool* xbox) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) return FolderId(path, xbox);
  svrfmt::Disc d;
  if (!d.Open(path)) return {};
  if (xbox) *xbox = d.Xbox();
  return Upper(d.Id());
}

// The files the moveset / taunt verbs read per game (Superstar Porting, 15 Oct: their disc caches; checked against the
// discs: SvR 2007 / 2008 have no chEtc.pac); the motion pacs
// are listed by their first (the verbs read whichever exist; a move whose pac is missing just can't come).
struct Needs {
  const char* key;
  std::vector<const char*> moves;
};
const std::vector<Needs>& NeedTable() {
  static const std::vector<Needs> t = {
      {"svr2007", {"pac/m.pac", "pac/misc.pac"}},  // (2007 / 2008: the superstars' records are in misc.pac - no
      {"svr2008", {"pac/m.pac", "pac/misc.pac"}},  // chEtc.pac; menuHD.pac and the model pacs optional: superstar_port 1a9ceab)
      {"svr2009", {"pac/m.pac", "pac/misc.pac", "pac/ch/chEtc.pac"}},
      {"svr2010", {"pac/m.pac", "pac/misc.pac", "pac/ch/chEtc.pac", "pac/DLC_HD.pac"}},
      {"wwe12", {"pac/m.pac", "pac/misc.pac", "pac/ch/chEtc.pac"}},
      {"wwe13", {"pac/misc.pac", "pac/ch/chEtc.pac", "pac/mot00.pac"}},
      {"wwe2k14", {"pac/createmode.pac", "pac/string.pac", "pac/ch/chEtc.pac", "pac/mot00.pac"}},
      {"wwe2k15", {"pac/createmode.pac", "pac/string.pac", "pac/ch/chEtc.pac", "pac/mot00.pac"}},
      {"wwe2k16", {"pac/createmode.pac", "pac/string.pac", "pac/ch/chEtc.pac", "pac/mot00.pac"}},
      {"wwe2k17", {"pac/createmode.pac", "pac/string_360.pac", "pac/ch/chEtc.pac", "pac/ingame/motion/all_mot0.pac"}},
  };
  return t;
}

std::vector<std::string> MissingFiles(const std::string& key, const std::string& what) {
  std::vector<std::string> out;
  const std::wstring path = PathFor(key);
  std::error_code ec;
  if (path.empty() || !fs::is_directory(path, ec) || what != "moves") return out;
  const Game* g = GameByKey(key);
  if (g && !g->xbox) {  // (PS2: the ELF, the profiles and the pacs - the whole disc)
    if (!fs::exists(fs::path(path) / L"SYSTEM.CNF", ec)) out.push_back("the whole disc (SYSTEM.CNF and everything with it)");
    return out;
  }
  for (const auto& n : NeedTable())
    if (key == n.key)
      for (const char* f : n.moves)
        if (!fs::exists(fs::path(path) / fs::u8path(f), ec)) {
          std::string w = f;
          std::replace(w.begin(), w.end(), '/', '\\');
          out.push_back(w);
        }
  return out;
}

std::string FolderNote(const std::string& key) {
  const std::wstring path = PathFor(key);
  std::error_code ec;
  if (path.empty() || !fs::is_directory(path, ec) || !FolderId(path, nullptr).empty()) return {};
  std::string note = "Some of the game's files (not a whole disc): porting uses what's here and says which file it needs when "
                     "one isn't.";
  const auto miss = MissingFiles(key, "moves");
  if (!miss.empty()) {
    note += " Moves and taunts need ";
    for (size_t i = 0; i < miss.size(); ++i) note += (i ? (i + 1 == miss.size() ? " and " : ", ") : "") + miss[i];
    note += " from that game.";
  }
  return note;
}

std::string NotGameFiles(const std::wstring& folder) {
  std::error_code ec;
  const fs::path dir(folder);
  if (folder.empty() || !fs::is_directory(dir, ec) || !FolderId(folder, nullptr).empty()) return {};
  for (const auto& n : NeedTable())  // (a partial copy of a game: FolderNote says what it can do)
    for (const char* f : n.moves)
      if (fs::exists(dir / fs::u8path(f), ec)) return {};
  for (const char* f : {"pac/ch/chEtc.pac", "pac/misc.pac", "pac/m.pac", "pac/createmode.pac"})
    if (fs::exists(dir / fs::u8path(f), ec)) return {};
  // The console's own packages (Content\0000000000000000\<title id>\00000002\<40 hex>): STFS files, "LIVE" / "PIRS" / "CON "
  int packages = 0;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && packages < 1;
       it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2) || it->file_size(e2) < 0x1000) continue;
    char magic[4] = {};
    if (FILE* f = _wfopen(it->path().c_str(), L"rb")) {
      std::fread(magic, 1, 4, f);
      std::fclose(f);
    }
    packages += !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4) || !std::memcmp(magic, "CON ", 4);
  }
  const std::string name = Utf8(dir.filename().wstring());
  if (packages)
    return "\"" + name + "\" holds Xbox 360 packages (DLC or a game update as the console keeps them), not a game's files. "
           "DLC superstars can't be read from the DLC alone - their names, moves and entrances are in the game's own files: "
           "set the game's disc (an .iso, or the whole extracted disc with default.xex). A DLC superstar's model can still come "
           "in on its own: unpack the package with an Xbox 360 package tool, then From another game > One file > its "
           "pac\\ch\\chNNN.pac.";
  return "\"" + name + "\" has none of a game's own files (no default.xex, pac\\misc.pac, pac\\ch\\chEtc.pac ...): is it DLC? "
         "DLC superstars can't be read from the DLC alone - their names, moves and entrances are in the game's own files: "
         "set the game's disc (an .iso, or the whole extracted disc with default.xex). A DLC superstar's model can still come "
         "in on its own: From another game > One file > its pac\\ch\\chNNN.pac.";
}

bool Scanning() { return g_scanning; }

void FindIn(const std::wstring& folder) { FindInFolders({folder}); }

void FindInFolders(const std::vector<std::wstring>& folders) {
  if (folders.empty() || g_scanning.exchange(true)) return;
  g_stop = false;
  g_scan_dirs = 0, g_scan_isos = 0;
  std::wstring folder;  // (the summary's "in ...")
  for (const auto& f : folders) folder += (folder.empty() ? L"" : L", ") + f;
  Log("discs: looking for disc images and game folders in " + Utf8(folder));
  std::thread([folders, folder] {
    std::vector<Found> found;
    std::vector<std::pair<fs::path, int>> todo;
    for (auto it = folders.rbegin(); it != folders.rend(); ++it) todo.push_back({fs::path(*it), 0});
    while (!todo.empty() && !g_stop) {
      const auto [dir, depth] = todo.back();
      todo.pop_back();
      ++g_scan_dirs;
      {
        std::lock_guard lock(g_scan_mutex);
        g_scan_now = Utf8(dir.wstring());
      }
      if (const std::string id = FolderId(dir, nullptr); !id.empty()) {  // an extracted disc: not searched further
        found.push_back({dir.wstring(), id});
        continue;
      }
      std::error_code ec;
      std::vector<fs::path> isos, dirs;
      for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const fs::path& p = it->path();
        const std::wstring name = p.filename().wstring();
        if (it->is_directory(e2)) {
          if (name.empty() || name[0] == L'$' || name[0] == L'.' || name == L"System Volume Information" || name == L"Windows") continue;
          if (it->is_symlink(e2)) continue;
          const DWORD attr = GetFileAttributesW(p.c_str());  // (junctions too: they can loop back up the tree)
          if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
          if (depth < 8) dirs.push_back(p);
        } else if (it->is_regular_file(e2) && Lower(Utf8(p.extension().wstring())) == ".iso") {
          isos.push_back(p);
        }
      }
      std::sort(isos.begin(), isos.end());
      for (const auto& p : isos) {
        if (g_stop) break;
        ++g_scan_isos;
        {
          std::lock_guard lock(g_scan_mutex);
          g_scan_now = Utf8(p.wstring());
        }
        svrfmt::Disc d;
        if (d.Open(p.wstring())) found.push_back({p.wstring(), Upper(d.Id())});
      }
      std::sort(dirs.rbegin(), dirs.rend());  // (a stack: the first in name order comes out first)
      for (const auto& p : dirs) todo.push_back({p, depth + 1});
    }
    const bool stopped = g_stop;
    OnUiThread([found, folder, stopped] {
      g_scanning = false;
      ApplyFound(found, folder, stopped);
    });
  }).detach();
}

void TestFind(const std::wstring& spec) {
  const size_t c = spec.find(L',');
  g_test_folder = spec.substr(0, c);
  g_test_report = c == std::wstring::npos ? L"" : spec.substr(c + 1);
  g_test_pending = true;
}

void Draw() {
  if (g_test_pending) {
    g_test_pending = false;
    FindIn(g_test_folder);
  }
  const float top = ImGui::GetCursorPosY();
  Heading("My game discs", "Your other WWE games, set once: every From another game tab and Moves & entrances read them. Disc "
                           "images (.iso) are read as they are; nothing is copied or changed.");
  {  // Find my discs, on the heading's line
    const ImVec2 after = ImGui::GetCursorPos();
    const char* label = "Find my discs...";
    const float bw = ImGui::CalcTextSize(label).x + 40 * g_scale;
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, ImGui::GetWindowWidth() - bw - 24 * g_scale), top));
    ImGui::BeginDisabled(g_scanning);
    PushAccent();
    if (ImGui::Button(label, ImVec2(bw, 0))) {
      const std::wstring f = PickFolder(L"A folder with your disc images or game folders (searched with everything under it)",
                                        OtherGameStart());
      if (!f.empty()) OtherGamePicked(f), FindIn(f);
    }
    PopAccent();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Pick a folder (e.g. D:\\Roms): it and everything under it is searched for .iso files and extracted "
                        "game folders, and each disc is recognised by its id. Games you already set stay as they are.");
    ImGui::SetCursorPos(after);
  }
  if (g_scanning) {
    std::string now;
    {
      std::lock_guard lock(g_scan_mutex);
      now = g_scan_now;
    }
    const char spin[] = "|/-\\";
    ImGui::TextColored(kWarn, "%c  Looking ... %d folders, %d disc images so far", spin[int(ImGui::GetTime() * 8) & 3],
                       int(g_scan_dirs), int(g_scan_isos));
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop")) g_stop = true;
    ImGui::TextDisabled("%s", now.c_str());
  } else if (!g_last_result.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "%s", g_last_result.c_str());
    ImGui::PopTextWrapPos();
  }
  int set = 0;
  for (const auto& g : kGames) set += Exists(g.key);
  ImGui::Text("%d of %d games set.", set, int(kGames.size()));
  ImGui::SameLine();
  ImGui::TextDisabled("Grey: not set (or not found). Each one can be a disc image or an extracted folder.");
  if (!g_note.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kWarn, "%s", g_note.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Spacing();
  ImGui::BeginChild("discs", ImVec2(0, 0), false);
  Table(true);
  ImGui::Spacing();
  Table(false);
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("Only your own discs are read: nothing of any game comes with the Mod Maker. WWE '12 and later "
                      "keep most files compressed; they are unpacked when they are converted.");
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
}

}  // namespace disc_list
}  // namespace mm
