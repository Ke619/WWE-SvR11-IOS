// Installed mods page (LOOK): every mod in <game>\Mods\<type>\<folder>, viewed
// read-only - superstars with their model, attires, renders and their
// moveset / taunts / pose played here, attires, move packs (each move played
// on a superstar picked here), arenas and the picture mods. Nothing is
// written unless clicked: switching a mod off writes the launcher's
// "disabled" file, "Open in its page" opens the folder as a project (its
// files copied into the project cache, the folder only read).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "app.h"
#include "mod_check.h"
#include "char_preview.h"
#include "editor.h"
#include "svrfmt/arena.h"
#include "svrfmt/pac.h"
#include "svrfmt/mod_travel.h"

namespace mm {
namespace installed_page {

using namespace svrfmt;

namespace {

// The type folders the launcher installs into (the *Overlay folders are the game's own output).
const char* const kKinds[] = {"Superstars", "Attires", "Arenas", "Moves", "Media", "Signs", "Backgrounds", "Backstage", "MatchTypes"};

struct Mod {
  std::string kind;  // its folder's type ("Superstars")
  fs::path dir;
  std::string text;  // manifest.txt
  std::string name, version, author, made_with;
  bool off = false;  // the launcher's "disabled" file
  bool locked = false;  // a backstage mod (src/no_mods.h BackstageModsOff) or the removed free_roaming_backstage type: shown off
};
std::vector<Mod> g_mods;
std::wstring g_scanned_for;
int g_sel = -1;
std::string g_test_select;  // --installed <kind>/<folder>[,<move>[,<key>]]: selected at start (and the move played)
int g_test_move = 0, g_test_key = -1;

void Scan() {
  g_mods.clear();
  g_scanned_for = g_game;
  std::error_code ec;
  for (const char* kind : kKinds) {
    std::vector<Mod> found;
    for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / fs::u8path(kind), ec)) {
      if (!e.is_directory(ec) || e.path().filename().wstring().rfind(L".", 0) == 0) continue;
      Mod m;
      m.kind = kind, m.dir = e.path();
      Bytes t;
      if (ReadFile(PathStr(e.path() / L"manifest.txt"), t)) m.text.assign(t.begin(), t.end());
      m.name = Value(m.text, "name");
      if (m.name.empty()) m.name = Utf8(e.path().filename().wstring());
      m.version = Value(m.text, "version"), m.author = Value(m.text, "author"), m.made_with = Value(m.text, "made_with");
      m.locked = m.kind == "Backstage" || (m.kind == "MatchTypes" && e.path().filename() == L"free_roaming_backstage");
      m.off = m.locked || fs::exists(e.path() / L"disabled", ec);
      found.push_back(std::move(m));
    }
    std::sort(found.begin(), found.end(), [](const Mod& a, const Mod& b) { return Lower(a.name) < Lower(b.name); });
    for (auto& m : found) g_mods.push_back(std::move(m));
  }
  g_sel = -1;
}

// ---- the selected mod's view (built once per selection)
struct PackMove {
  int id = 0, from = 0, frames = 0;
  std::string name;
};
struct View {
  int index = -1;
  std::vector<Problem> problems;
  std::vector<Picture> pics;
  std::vector<std::string> pic_names;
  std::vector<std::string> files;  // (the folder's other files, for the record)
  std::string online;  // (whether the game's host can send it to players who join: svrfmt/mod_travel.h)
  bool online_ok = true;
  // superstar / attire
  std::wstring model;
  std::vector<std::pair<int, std::wstring>> attires;  // (attire number, its pac: the model or an attireN.pac)
  int shown_attire = 1;
  std::map<int, int> moveset;  // slot -> move
  int base = -1, pose = -1;
  // move pack (a superstar's own moves\ too)
  fs::path pack;
  std::vector<PackMove> moves;
  std::map<int, std::vector<std::pair<std::array<int, 3>, std::string>>> motions;  // id -> ((x, y, frames), file)
  int play_star = -1, play_dummy = -1;
};
View g_view;
std::unique_ptr<svrfmt::Arena> g_arena;  // (an arena mod's, shown by the editor's view scene)
std::string g_arena_name, g_arena_text;  // (what it was shown with: RestoreView)

std::map<int, int> ReadMoveset(const fs::path& f) {
  std::map<int, int> out;
  Bytes b;
  if (!ReadFile(PathStr(f), b)) return out;
  const std::string s(b.begin(), b.end());
  for (size_t at = 0; at < s.size();) {
    size_t nl = s.find('\n', at);
    if (nl == std::string::npos) nl = s.size();
    const std::string line = s.substr(at, nl - at);
    at = nl + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const int off = int(std::strtol(line.c_str(), nullptr, 0)), id = std::atoi(line.c_str() + eq + 1);
    if (off >= 0 && off < 2 * kMoveSlots && !(off & 1) && id > 0) out[off / 2] = id;
  }
  return out;
}

void ReadPack(const fs::path& dir) {
  g_view.pack = dir;
  Bytes b;
  if (!ReadFile(PathStr(dir / L"pack.txt"), b)) return;
  const std::string s(b.begin(), b.end());
  std::map<int, PackMove> by;
  for (size_t at = 0; at < s.size();) {
    size_t nl = s.find('\n', at);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(at, nl - at);
    at = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    char word[16] = "", pac[64] = "", path[96] = "", file[260] = "";
    int id = 0, x = 0, y = 0, frames = 0, from = 0;
    if (std::sscanf(line.c_str(), "%15s", word) != 1) continue;
    if (!std::strcmp(word, "motion") &&
        std::sscanf(line.c_str(), "%*s %63s %95s %d %d %d %d %259s", pac, path, &id, &x, &y, &frames, file) == 7) {
      g_view.motions[id].push_back({{x, y, frames}, std::string(pac) + "|" + path + "|" + file});
      if (y == 0) by[id].id = id, by[id].frames = std::max(by[id].frames, frames);
    } else if (!std::strcmp(word, "wazecopy") && std::sscanf(line.c_str(), "%*s %d %d", &id, &from) == 2) {
      by[id].id = id, by[id].from = from;
    } else if (!std::strcmp(word, "wazename") && std::sscanf(line.c_str(), "%*s %d", &id) == 1) {
      const size_t sp = line.find(' ', line.find(' ') + 1);
      by[id].id = id;
      if (sp != std::string::npos) by[id].name = line.substr(sp + 1);
    } else if (!std::strcmp(word, "waze") && std::sscanf(line.c_str(), "%*s %d", &id) == 1) {
      by[id].id = id;
      if (!by[id].from) by[id].from = id;
    }
  }
  for (auto& [id, m] : by)
    if (g_view.motions.count(id)) g_view.moves.push_back(m);
}

// A pack move's motions as a bank (the target bank's header + event-size table, then the pack's entries and data):
// what the 3D view plays. null if the target bank or a file can't be read.
std::shared_ptr<const Bytes> PackBank(int id) {
  const auto it = g_view.motions.find(id);
  if (it == g_view.motions.end() || it->second.empty()) return nullptr;
  // the target bank, for its header: "<pac>|<group>/<entry>/<child>|<file>"
  const std::string first = it->second.front().second;
  const size_t a = first.find('|'), b = first.find('|', a + 1);
  const std::string pac = first.substr(0, a), where = first.substr(a + 1, b - a - 1);
  const size_t s1 = where.find('/'), s2 = where.find('/', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos) return nullptr;
  std::string group = where.substr(0, s1), entry = where.substr(s1 + 1, s2 - s1 - 1);
  const int child = std::atoi(where.c_str() + s2 + 1);
  while (group.size() < 4) group += ' ';
  Bytes raw;
  if (!ReadPacEntry(fs::path(g_game) / L"pac" / fs::u8path(pac), entry.c_str(), raw, group.c_str())) return nullptr;
  std::vector<PachEntry> kids;
  Bytes head;
  if (PachRead(Unpack(raw), kids))
    for (const auto& k : kids)
      if (int(k.id) == child) head = Unpack(k.data);
  if (head.size() < 0x114) return nullptr;
  // the entries and their data
  std::vector<std::pair<std::array<int, 3>, Bytes>> parts;
  for (const auto& [xyf, ref] : it->second) {
    Bytes d;
    if (!ReadFile(PathStr(g_view.pack / fs::u8path(ref.substr(ref.rfind('|') + 1))), d)) return nullptr;
    parts.push_back({xyf, std::move(d)});
  }
  auto bank = std::make_shared<Bytes>(head.begin(), head.begin() + 0x110);
  const uint32_t n = uint32_t(parts.size());
  bank->resize(0x114 + 16 * size_t(n));
  PutLe32(&(*bank)[0x110], n);
  uint32_t off = 0;
  Bytes data;
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* e = &(*bank)[0x114 + 16 * i];
    e[0] = uint8_t(parts[i].first[1]), e[1] = uint8_t(parts[i].first[0]);
    e[2] = uint8_t(id), e[3] = uint8_t(id >> 8);
    PutLe32(e + 4, off);
    PutLe32(e + 8, uint32_t(parts[i].first[2]));
    PutLe32(e + 12, 0);
    data.insert(data.end(), parts[i].second.begin(), parts[i].second.end());
    off += uint32_t(parts[i].second.size());
  }
  bank->insert(bank->end(), data.begin(), data.end());
  return bank;
}

std::wstring ChPac(int id) { return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac")).wstring(); }

void Select(int i) {
  if (g_arena) editor::ClearView(), g_arena.reset();
  g_sel = i;
  g_view = View();
  g_view.index = i;
  if (i < 0 || i >= int(g_mods.size())) return;
  const Mod& m = g_mods[size_t(i)];
  std::error_code ec;
  // online: the game sends a joiner the host's gameplay mods it lacks (the same check as the game's host)
  if (m.kind == "Superstars" || m.kind == "Moves" || m.kind == "Arenas" || m.kind == "Backstage") {
    std::string why;
    g_view.online_ok = ModCanTravel(m.dir, &why);
    g_view.online = g_view.online_ok ? "Online: can be sent to players who join you" : "Online: can't be sent - " + why;
  } else {
    g_view.online = "Online: not sent (only superstars, move packs, arenas and backstage mods travel)";
  }
  // pictures (here and one folder down: an arena's vs\), other files
  auto add_pictures = [&](const fs::path& dir, const std::string& prefix) {
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string ext = Lower(Utf8(e.path().extension().wstring()));
      const std::string name = prefix + Utf8(e.path().filename().wstring());
      if ((ext == ".dds" || ext == ".png" || ext == ".jpg") && g_view.pics.size() < 16) {
        Image img;
        bool ok = false;
        if (ext == ".dds") {
          Bytes d;
          ok = ReadFile(PathStr(e.path()), d) && DdsDecode(d, img);
        } else {
          ok = LoadPicture(e.path().wstring(), img);
        }
        if (ok) {
          Picture p;
          p.Set(std::move(img));
          g_view.pics.push_back(std::move(p));
          g_view.pic_names.push_back(name);
        }
      } else if (prefix.empty()) {
        g_view.files.push_back(name);
      }
    }
  };
  add_pictures(m.dir, "");
  for (const auto& e : fs::directory_iterator(m.dir, ec))
    if (e.is_directory(ec) && Lower(Utf8(e.path().filename().wstring())) != "moves") add_pictures(e.path(), Utf8(e.path().filename().wstring()) + "\\");
  // files the manifest names but the folder lacks
  for (size_t at = 0; at < m.text.size();) {
    size_t nl = m.text.find('\n', at);
    if (nl == std::string::npos) nl = m.text.size();
    std::string line = m.text.substr(at, nl - at);
    at = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), v = line.substr(eq + 1);
    const std::string ext = Lower(v.size() > 4 ? v.substr(v.rfind('.') == std::string::npos ? v.size() : v.rfind('.')) : "");
    static const std::set<std::string> kFiles = {".pac", ".dds", ".bik", ".mp3", ".wav", ".ogg", ".txt", ".png", ".mp4", ".bk2"};
    if (kFiles.count(ext) && key != "made_with" && !fs::exists(m.dir / fs::u8path(v), ec))
      g_view.problems.push_back(Error("The manifest names " + v + " (" + key + "=) but the folder has no such file."));
  }
  {  // the mod doctor (mod_check.cpp): every rule the game and the launcher apply to this type
    auto more = CheckModFolder(m.kind, m.dir);
    g_view.problems.insert(g_view.problems.end(), more.begin(), more.end());
  }
  if (m.kind == "Superstars" || m.kind == "Attires") {
    g_view.model = (m.dir / (m.kind == "Superstars" ? L"ch.pac" : L"attire.pac")).wstring();
    if (!fs::exists(g_view.model, ec)) {
      g_view.model.clear();
    } else {
      for (int a : ModelAttires(g_view.model))
        if (a <= 4) g_view.attires.push_back({a, g_view.model});
      if (m.kind == "Attires" && g_view.attires.size() > 1) g_view.attires.resize(1);
    }
    for (int a = 2; a <= 4 && m.kind == "Superstars"; ++a)  // attireN.pac files replace / add attire N
      if (const std::string f = Value(m.text, ("attire" + std::to_string(a)).c_str()); !f.empty() && fs::exists(m.dir / fs::u8path(f), ec)) {
        auto it = std::find_if(g_view.attires.begin(), g_view.attires.end(), [&](const auto& p) { return p.first == a; });
        if (it != g_view.attires.end()) it->second = (m.dir / fs::u8path(f)).wstring();
        else g_view.attires.push_back({a, (m.dir / fs::u8path(f)).wstring()});
      }
    std::sort(g_view.attires.begin(), g_view.attires.end());
    std::string mv = Value(m.text, "moves");
    if (mv.empty()) mv = "moves.txt";
    g_view.moveset = ReadMoveset(m.dir / fs::u8path(mv));
    g_view.base = std::atoi(Value(m.text, "base").c_str());
    if (!g_view.base) g_view.base = -1;
    if (const std::string p = Value(m.text, "pose"); !p.empty()) g_view.pose = std::atoi(p.c_str());
    if (fs::exists(m.dir / L"moves" / L"pack.txt", ec)) ReadPack(m.dir / L"moves");
  }
  if (m.kind == "Moves") ReadPack(m.dir);
  if (m.kind == "Arenas") {  // the arena itself, read for the read-only 3D view
    std::string file = Value(m.text, "file");
    if (file.empty()) file = "arena.pac";
    auto a = std::make_unique<svrfmt::Arena>();
    std::string err;
    if (a->Load(PathStr(m.dir / fs::u8path(file)), &err)) {
      g_arena = std::move(a);
      g_arena_name = m.name, g_arena_text = m.text;
      editor::ViewArena(g_arena.get(), m.name, m.text);
    } else {
      g_view.problems.push_back(Error("The arena didn't read: " + err + "."));
    }
  }
  const auto& stars = Stars();
  for (int k = 0; k < int(stars.size()); ++k) {  // (a move pack's moves played on: the base, else the first)
    if (stars[size_t(k)].id == (g_view.base > 0 ? g_view.base : 139)) g_view.play_star = k;
    if (stars[size_t(k)].id == 160) g_view.play_dummy = k;
  }
  if (g_view.play_star < 0 && !stars.empty()) g_view.play_star = 0;
}

void ShowModel() {
  if (g_view.model.empty() && g_view.attires.empty()) return;
  std::wstring pac = g_view.model;
  int attire = 1;
  for (const auto& [a, f] : g_view.attires)
    if (a == g_view.shown_attire) pac = f, attire = f == g_view.model ? a : 1;
  char_preview::SetAttire(attire);
  char_preview::SetModel(pac, g_game);
}

void PlayMove(int id) {
  if (g_view.motions.count(id)) {  // (the mod's own pack)
    const auto bank = PackBank(id);
    std::string err;
    if (!bank || !anims_page::PlayMove(bank, id, -1, &err))
      Status("That move's motions can't be shown: " + (bank ? err : std::string("its bank or files didn't read")) + ".");
    else
      Status("Playing move " + std::to_string(id) + " from the pack.");
    return;
  }
  anims_page::WatchMove(id);
}

void SuperstarView(const Mod& m) {
  if (!g_view.attires.empty() && g_view.attires.size() > 1) {
    ImGui::TextDisabled("Attire");
    for (const auto& [a, f] : g_view.attires) {
      ImGui::SameLine();
      const bool on = a == g_view.shown_attire;
      if (on) PushAccent();
      if (ImGui::SmallButton((std::to_string(a) + "##at").c_str())) g_view.shown_attire = a;
      if (on) PopAccent();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f == g_view.model ? "in the model pac" : FileName(f).c_str());
    }
  }
  if (!g_view.model.empty()) {
    if (ImGui::SmallButton("Export to FBX...")) {
      std::wstring pac = g_view.model;
      int attire = g_view.shown_attire;
      for (const auto& [a, f] : g_view.attires)
        if (a == g_view.shown_attire && f != g_view.model) pac = f, attire = 1;
      ExportModelFbx(pac, attire, m.name);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The model (the attire shown) with its skeleton, skin weights and textures, for Blender and other tools.");
  }
  if (m.kind != "Superstars") return;
  // the motions: idle, pose, then every moveset slot it sets
  const bool ready = anims_page::MovesIndexed();
  if (ImGui::SmallButton("Idle")) char_preview::ClearMotion(), char_preview::SetDummy(L"");
  if (g_view.pose >= 0) {
    ImGui::SameLine();
    if (ImGui::SmallButton(("Select screen pose " + std::to_string(g_view.pose)).c_str())) char_preview::SetDummy(L""), star_page::ShowPose(g_view.pose);
  }
  ImGui::SameLine();
  ImGui::TextDisabled(ready ? "%zu moveset slots set" : "%zu moveset slots set (reading m.pac ...)", g_view.moveset.size());
  if (g_view.moveset.empty()) {
    ImGui::TextDisabled("Its moveset is its base superstar's (no moves.txt).");
    return;
  }
  if (ImGui::BeginTable("moveset", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * g_scale))) {
    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 230 * g_scale);
    ImGui::TableSetupColumn("move", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * g_scale);
    for (const auto& [slot, id] : g_view.moveset) {
      ImGui::PushID(slot);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(star_page::SlotName(slot).c_str());
      ImGui::TableNextColumn();
      std::string n = MoveName(id);
      for (const auto& pm : g_view.moves)
        if (pm.id == id && !pm.name.empty()) n = pm.name + " (its pack)";
      ImGui::Text("%s  (%d)", n.empty() ? "?" : n.c_str(), id);
      ImGui::TableNextColumn();
      const bool can = g_view.motions.count(id) || (ready && anims_page::HasMotion(id));
      ImGui::BeginDisabled(!can || Busy());
      if (ImGui::SmallButton(">")) {
        if (g_view.base > 0) char_preview::SetDummy(ChPac(g_view.base));
        PlayMove(id);
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

void PackView() {
  const auto& stars = Stars();
  ImGui::SetNextItemWidth(220 * g_scale);
  if (ImGui::BeginCombo("Played by", g_view.play_star >= 0 ? stars[size_t(g_view.play_star)].name.c_str() : "pick")) {
    for (int k = 0; k < int(stars.size()); ++k)
      if (ImGui::Selectable((stars[size_t(k)].name + "##ps" + std::to_string(k)).c_str(), g_view.play_star == k)) g_view.play_star = k;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220 * g_scale);
  if (ImGui::BeginCombo("on", g_view.play_dummy >= 0 ? stars[size_t(g_view.play_dummy)].name.c_str() : "pick")) {
    for (int k = 0; k < int(stars.size()); ++k)
      if (ImGui::Selectable((stars[size_t(k)].name + "##pd" + std::to_string(k)).c_str(), g_view.play_dummy == k)) g_view.play_dummy = k;
    ImGui::EndCombo();
  }
  if (g_view.play_star >= 0) char_preview::SetAttire(1), char_preview::SetModel(ChPac(stars[size_t(g_view.play_star)].id), g_game);
  if (ImGui::BeginTable("packmoves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 240 * g_scale))) {
    ImGui::TableSetupColumn("move", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * g_scale);
    for (const auto& pm : g_view.moves) {
      ImGui::PushID(pm.id);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d", pm.id);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(!pm.name.empty() ? pm.name.c_str() : pm.from ? ("as " + MoveName(pm.from)).c_str() : "(motions only)");
      ImGui::TableNextColumn();
      ImGui::Text("%d", pm.frames);
      ImGui::TableNextColumn();
      if (ImGui::SmallButton(">")) {
        if (g_view.play_dummy >= 0) char_preview::SetDummy(ChPac(stars[size_t(g_view.play_dummy)].id));
        PlayMove(pm.id);
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
}

}  // namespace

void TestSelect(const std::string& spec) { g_test_select = spec; }

// The arena shown again in the editor's read-only view (after the Full preview used that view).
void RestoreView() {
  if (g_arena) editor::ViewArena(g_arena.get(), g_arena_name, g_arena_text);
}

std::vector<Problem> g_set_problems;   // CheckInstalledSet: the mods together
std::wstring g_set_for;                 // (the game folder it was made for)
std::vector<Problem> g_file_problems;  // the last "Check a .svrmod..."
std::string g_file_checked;

void Draw() {
  if (g_scanned_for != g_game) Scan();
  if (g_set_for != g_game) g_set_problems = CheckInstalledSet(), g_set_for = g_game;
  static int fixes_seen = 0;
  if (fixes_seen != FixesDone()) {  // (a fix button changed a mod: everything checked again, the same mod kept)
    fixes_seen = FixesDone();
    const fs::path was = g_sel >= 0 && g_sel < int(g_mods.size()) ? g_mods[size_t(g_sel)].dir : fs::path();
    Scan();
    g_set_problems = CheckInstalledSet();
    for (int i = 0; i < int(g_mods.size()); ++i)
      if (g_mods[size_t(i)].dir == was) Select(i);
  }
  if (!g_test_select.empty()) {  // (test aid: "<kind>/<folder>[,<move>[,<key>]]")
    const size_t c = g_test_select.find(',');
    if (c != std::string::npos) std::sscanf(g_test_select.c_str() + c + 1, "%d,%d", &g_test_move, &g_test_key);
    const std::string want = Lower(g_test_select.substr(0, c));
    for (int i = 0; i < int(g_mods.size()); ++i)
      if (Lower(g_mods[size_t(i)].kind + "/" + Utf8(g_mods[size_t(i)].dir.filename().wstring())) == want) Select(i);
    g_test_select.clear();
  }
  if (g_test_move && g_sel >= 0 && char_preview::ModelHeight() > 0 && !Busy() &&
      (g_view.motions.count(g_test_move) || anims_page::MovesIndexed())) {
    const auto& stars = Stars();
    if (g_mods[size_t(g_sel)].kind == "Moves" && g_view.play_dummy >= 0) char_preview::SetDummy(ChPac(stars[size_t(g_view.play_dummy)].id));
    else if (g_view.base > 0) char_preview::SetDummy(ChPac(g_view.base));
    PlayMove(g_test_move);
    if (g_test_key >= 0) char_preview::Play().key = float(g_test_key), char_preview::Play().playing = false;
    Log("test: played " + std::to_string(g_test_move));
    g_test_move = 0;
  }
  Heading("Installed mods", "Every mod in the game folder's Mods, to look at: superstars with their model, attires and moves "
                            "played here, move packs on any superstar, arenas and the picture mods. Read-only unless you click.");
  ImGui::BeginChild("mods_list", ImVec2(300 * g_scale, 0), true);
  if (ImGui::SmallButton("Rescan")) Scan(), g_set_problems = CheckInstalledSet();
  ImGui::SameLine();
  if (ImGui::SmallButton("Check a .svrmod...")) {
    const std::wstring f = PickFile(false, L"A mod to check (.svrmod)", kModFilter, 1);
    if (!f.empty()) g_file_problems = CheckSvrmod(f), g_file_checked = FileName(f);
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Checks a mod file the way the game and the launcher will read it, without installing it.");
  ImGui::SameLine();
  if (ImGui::SmallButton(("The open project: " + std::string(PageName(g_project_page))).c_str())) GoTo(g_project_page);
  if (!g_file_checked.empty()) {
    if (ImGui::TreeNodeEx("filecheck", ImGuiTreeNodeFlags_DefaultOpen, "%s: %s", g_file_checked.c_str(), g_file_problems.empty() ? "nothing wrong found" : "to check")) {
      if (!g_file_problems.empty()) DrawProblems(g_file_problems);
      if (ImGui::SmallButton("Close##fc")) g_file_checked.clear();
      ImGui::TreePop();
    }
  }
  if (!g_set_problems.empty() && ImGui::TreeNodeEx("setcheck", ImGuiTreeNodeFlags_DefaultOpen, "Your mods together (%d)", int(g_set_problems.size()))) {
    DrawProblems(g_set_problems);
    ImGui::TreePop();
  }
  if (g_mods.empty()) ImGui::TextWrapped("No mods in %s.", PathStr(fs::path(g_game) / L"Mods").c_str());
  std::string last;
  bool open = true;
  for (int i = 0; i < int(g_mods.size()); ++i) {
    const Mod& m = g_mods[size_t(i)];
    if (m.kind != last) {
      last = m.kind;
      int n = 0;
      for (const auto& x : g_mods) n += x.kind == m.kind;
      open = ImGui::CollapsingHeader((m.kind + " (" + std::to_string(n) + ")###k" + m.kind).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    }
    if (!open) continue;
    const std::string label = m.name + (m.version.empty() ? "" : "  " + m.version) + (m.off ? "  (off)" : "") + "##m" + std::to_string(i);
    if (m.off) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.6f, 1));
    if (ImGui::Selectable(label.c_str(), g_sel == i)) Select(i);
    if (m.off) ImGui::PopStyleColor();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("mod_view", ImVec2(0, 0), false);
  if (g_sel < 0 || g_sel >= int(g_mods.size())) {
    ImGui::TextDisabled("Pick a mod on the left.");
    ImGui::EndChild();
    return;
  }
  Mod& m = g_mods[size_t(g_sel)];
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.25f);
  ImGui::TextUnformatted(m.name.c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s  %s%s%s%s", m.kind.c_str(), Utf8(m.dir.filename().wstring()).c_str(), m.version.empty() ? "" : ("  v" + m.version).c_str(),
                      m.author.empty() ? "" : ("  by " + m.author).c_str(), m.made_with.empty() ? "" : ("  (" + m.made_with + ")").c_str());
  if (!g_view.online.empty()) {
    if (g_view.online_ok) ImGui::TextDisabled("%s", g_view.online.c_str());
    else ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1), "%s", g_view.online.c_str());
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("When you host an online session, players who join without this mod can download it straight "
                        "from your game. Only the game's and mods' own file types travel (no .exe, .dll, scripts ...).");
  }
  // the actions: only on a click
  if (m.locked)
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1), "%s",
                       m.kind == "Backstage" ? "Backstage mods are switched off: Backstage Brawl plays the game's own "
                                               "backstage. This one stays installed but the game doesn't load it."
                                             : "FREE-ROAMING BACKSTAGE is removed from the game for now: this stays "
                                               "installed but the game doesn't use it.");
  bool on = !m.off;
  ImGui::BeginDisabled(m.locked);
  const bool flipped = ImGui::Checkbox("Switched on", &on);
  ImGui::EndDisabled();
  if (flipped) {
    std::error_code ec;
    const fs::path f = m.dir / L"disabled";
    if (on) fs::remove(f, ec);
    else WriteFile(PathStr(f), Bytes());
    m.off = fs::exists(f, ec);
    Status(m.name + (m.off ? " is switched off" : " is switched on") + ": the game takes it from its next start.");
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("As the launcher's Mods tab: a \"disabled\" file in the mod's folder.");
  ImGui::SameLine();
  if (ImGui::Button("Open in its page")) ProjectOpenFolder(m.dir.wstring());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens it as a project on its page (to change it, then save or install again). The installed folder is only read.");
  ImGui::SameLine();
  if (ImGui::Button("Show folder")) ShellOpen(m.dir.wstring());
  if (!g_view.problems.empty()) DrawProblems(g_view.problems);
  static int test_frames = 0;
  if (++test_frames > 60) TestFixOn(g_view.problems);  // (test aid --run-fix, on the selected mod)
  ImGui::Separator();
  // the type's view: the 3D view (superstars, attires, move packs) on the right of its lists
  const bool three_d = m.kind == "Superstars" || m.kind == "Attires" || m.kind == "Moves";
  if (m.kind == "Arenas" && g_arena) {  // the arena in 3D (a view: the open arena project is untouched)
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    editor::DrawView(avail.x, std::max(260.0f * g_scale, avail.y * 0.62f));
  }
  if (three_d) {
    ImGui::BeginChild("lists", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0), false);
    if (m.kind == "Moves") PackView();
    else SuperstarView(m);
    if (!g_view.pics.empty()) ImGui::Separator();
  } else {
    ImGui::BeginChild("lists", ImVec2(0, 0), false);
  }
  for (size_t k = 0; k < g_view.pics.size(); ++k) {  // its pictures
    if (k % 4) ImGui::SameLine();
    ImGui::BeginGroup();
    const float s = (m.kind == "Signs" ? 160.0f : 120.0f) * g_scale;
    const Image& img = g_view.pics[k].image;
    const float hgt = img.w ? s * float(img.h) / float(img.w) : s;
    g_view.pics[k].Draw(s, std::min(hgt, s * 1.5f));
    ImGui::TextDisabled("%s", g_view.pic_names[k].c_str());
    ImGui::EndGroup();
  }

  if (ImGui::CollapsingHeader("manifest.txt")) ImGui::TextUnformatted(m.text.c_str());
  ImGui::EndChild();
  if (three_d) {
    ImGui::SameLine();
    ImGui::BeginChild("view3d", ImVec2(0, 0), true);
    if (m.kind != "Moves") ShowModel();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    char_preview::Draw(avail.x, std::max(200.0f, avail.y - (m.kind != "Moves" ? 56 : 30) * g_scale));
    if (m.kind != "Moves") ViewFooter();
    ImGui::TextDisabled("%s", char_preview::Status().c_str());
    ImGui::EndChild();
  }
  ImGui::EndChild();
}

}  // namespace installed_page
}  // namespace mm
