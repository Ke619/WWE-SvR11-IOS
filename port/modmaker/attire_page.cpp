// Attires page (checklist D6): a new or replaced attire for a superstar the
// game has (or a superstar mod), as a mod of its own - type=attire, installed
// into <game>/Mods/Attires/<id> (format agreed with Superstar Porting, who
// builds the game side): superstar=<id>, attire=<1-4>|new, name=, model=
// attire.pac (a ch pac; its first attire is used), entrance= (optional).
// A superstar has 4 attires at most; the renders stay the superstar's.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "model_materials.h"
#include "mod_check.h"
#include "char_preview.h"
#include "pac_attires.h"
#include "svrfmt/pac.h"

namespace mm {
namespace attire_page {

using namespace svrfmt;

namespace {

struct AttireProject {
  int star = -1;          // 2011 id
  int slot = 0;           // 1-4: replace that attire; 0: a new one
  char name[32] = "", author[64] = "", version[16] = "1.0";
  std::wstring model;     // a ch pac
  int from = 1;           // which of its attires (attire.pac carries only that one, as attire 1)
  int entrance = -1;      // entrance= (-1: the superstar's)
  std::string gear;       // entrance gear: "" as the model has it, "off" left out, "from<k>" the model's attire k's
  std::map<MatKey, MatEdit> mats;  // the material panel's edits (keyed by the model pac's attire shown, gear)
};
// The editor of the attires inside a pac (a copy: saved elsewhere, the game's pac folder never written)
struct {
  std::wstring file, src;
  Epac pac, src_pac;
  bool loaded = false, src_loaded = false, dirty = false;
  int src_attire = 1, target = 0;  // target 0: a new attire
} g_ed;
std::wstring g_ed_test;  // --pac-attires <pac>,<op>[:...],...: edits, saves, quits
void Editor();
void RunEditorTest();
void Editor();
void RunEditorTest();
AttireProject g_at;
std::wstring g_test_save;
int g_test_from = 0;          // (--attire-from: which of the source's attires)
std::wstring g_use_star;      // (UseStarMod: taken in on the next frame)
std::string g_test_name;      // (and the attire's name)
struct {
  int star = -1;
  std::wstring model;
  int slot = 0;
  bool pending = false;
} g_test;

fs::path StockPac(int id) { return fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac"); }
// A superstar's model pacs: the game's pac\ch\chNNN.pac (disc superstars) and the installed DLC's ch<id>_dlcNN.pac
// (a DLC superstar's own, DLC attires of disc superstars) - the game counts both as their own attires.
std::vector<fs::path> ModelPacs(int id) {
  std::vector<fs::path> out;
  if (id >= 100) out.push_back(StockPac(id));
  for (const auto& p : DlcModelPacs(id)) out.push_back(p);
  return out;
}

// The superstar's attires now: its model pac's EMD entries "%06d%02d" (attire * 10 + kind; kind 2 = a model).
std::vector<int> CurrentAttires(int id) {
  static std::map<int, std::vector<int>> cache;
  if (cache.count(id)) return cache[id];
  std::vector<int> out;
  char want[8];
  std::snprintf(want, sizeof want, "%06d", id);
  for (const fs::path& pac : ModelPacs(id)) {  // (the game's pac and the DLC's: a DLC superstar has only those)
    PacIndex idx;
    if (ReadPacIndex(pac, idx))
      for (const auto& e : idx.entries)
        if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, want) == 0 && e.name[7] == '2') {
          const int a = (e.name[6] - '0') + 1;
          if (a >= 1 && a <= 4 && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
        }
  }
  std::sort(out.begin(), out.end());
  return cache[id] = out;
}

std::string ModId() {
  const StarInfo* s = StarOrDlcById(g_at.star);
  return IdFrom(std::string(g_at.name[0] ? g_at.name : "attire") + "_" + (s ? s->name : std::to_string(g_at.star)), "attire");
}

bool Build(std::vector<ZipEntry>& files) {
  Bytes pac;
  if (g_at.star < 0 || g_at.model.empty() || !ReadFile(Utf8(g_at.model), pac) || pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
    Status("Pick the superstar and a character model pac first.");
    return false;
  }
  for (int a = 1; a <= 9; ++a)
    for (const bool gear : {false, true}) {
      std::map<std::pair<uint32_t, int>, MatEdit> ed;
      for (const auto& [k, e] : g_at.mats)
        if (std::get<0>(k) == a && std::get<1>(k) == gear) ed[{std::get<2>(k), std::get<3>(k)}] = e;
      std::string err;
      if (!ed.empty() && !PatchMaterials(pac, a, gear, ed, &err)) Log("materials, attire " + std::to_string(a) + ": " + err);
    }
  if (g_at.from > 1 || !g_at.gear.empty()) {  // (another attire of the pac: only it, as attire 1; the gear choice)
    Epac e;
    std::string err;
    bool ok = EpacRead(pac, e);
    if (ok && g_at.gear.rfind("from", 0) == 0) ok = PacCopyGear(e, g_at.from, Epac(e), std::atoi(g_at.gear.c_str() + 4), &err);
    if (ok && g_at.from > 1) ok = PacOnlyAttire(e, g_at.from, &err);
    if (ok && g_at.gear == "off") ok = PacRemoveGear(e, 1, &err);
    if (!ok) {
      Status("Attire " + std::to_string(g_at.from) + " of the model could not be taken: " + err + ".");
      return false;
    }
    pac = EpacWrite(e);
  }
  std::string man = "type=attire\nid=" + ModId() + "\nsuperstar=" + std::to_string(g_at.star) +
                    "\nattire=" + (g_at.slot ? std::to_string(g_at.slot) : std::string("new")) + "\nname=" + g_at.name +
                    "\nmodel=attire.pac\nauthor=" + g_at.author + "\nversion=" + g_at.version + "\n" + MadeWith();
  if (g_at.entrance > 0) man += "entrance=" + std::to_string(g_at.entrance) + "\n";
  files.push_back({"manifest.txt", Bytes(man.begin(), man.end())});
  AddModProject(files, ModProjectText(PageId::kAttire));
  files.push_back({"attire.pac", std::move(pac)});
  return true;
}

void Save() {
  std::vector<ZipEntry> files;
  if (!Build(files)) return;
  const std::wstring f = PickFile(true, L"Save the attire mod", kModFilter, 1, L"svrmod", (Wide(ModId()) + L".svrmod").c_str());
  if (f.empty()) return;
  Status(WriteFile(Utf8(f), ZipWrite(files)) ? "Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +)." : "The mod could not be written.");
}

void Install() {
  std::vector<ZipEntry> files;
  if (!Build(files)) return;
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Attires" / fs::u8path(ModId());
  std::string why;
  if (!InstallModFiles(dir, files, &why)) {
    Status("Not installed: " + why);
    return;
  }
  Status("Installed into " + PathStr(dir) + ": the attire is on CHANGE ATTIRE from the next game start." + InstallReport("Attires", dir));
}

// The attires of a superstar that installed, switched-on attire mods ask for (attire=1-4; this mod's own folder and
// "disabled" ones left out): slot -> the first mod's name (A-Z by folder, as the game and the launcher take them).
// The rule (game, launcher, Android): an attire is taken by the superstar's own pac or by such a mod; two mods on one
// attire clash - the launcher moves the newer to the next free attire (neither), or asks to switch one off.
std::map<int, std::string> ModAttires(int star) {
  std::map<int, std::string> out;
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Attires", ec))
    if (e.is_directory(ec)) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& d : dirs) {
    if (PathStr(d.filename()) == ModId() || fs::exists(d / L"disabled", ec)) continue;
    Bytes t;
    if (!ReadFile(PathStr(d / L"manifest.txt"), t)) continue;
    const std::string m(t.begin(), t.end());
    if (Value(m, "type") != "attire" || Value(m, "superstar") != std::to_string(star)) continue;
    const int a = std::atoi(Value(m, "attire").c_str());
    if (a < 1 || a > 4) continue;
    const std::string name = Value(m, "name");
    out.emplace(a, name.empty() ? PathStr(d.filename()) : name);
  }
  return out;
}
// A superstar who wears entrance gear (their pac's attire 1 has a model besides the wrestler: EMD "<id>0" + kind,
// kind != 2 - the coats / hats / shirts of Undertaker, Punk, Cena, HBK, Morrison): any attire beyond their own
// never finishes loading the match, so only their own can be replaced (the game and the launchers follow this too).
bool EntranceGear(int star) {
  static std::map<int, bool> cache;
  if (const auto it = cache.find(star); it != cache.end()) return it->second;
  PacIndex idx;
  bool gear = false;
  char id[8];
  std::snprintf(id, sizeof id, "%06d", star);
  for (const fs::path& pac : ModelPacs(star))
    if (ReadPacIndex(pac, idx))
      for (const auto& e : idx.entries)
        if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, id) == 0 && e.name[6] == '0' && e.name[7] != '2') gear = true;
  return cache[star] = gear;
}
std::string GearText(const std::string& star, size_t own) {
  return star + " wears entrance gear: new attires for them don't load in the game yet - replace attire " +
         (own > 1 ? "1-" + std::to_string(own) : std::string("1")) + " instead.";
}

// The lowest attire neither in the superstar's pac nor asked for by a mod (0: none of the 4; none for entrance gear).
int NextFreeAttire(int star) {
  if (EntranceGear(star)) return 0;
  const auto now = CurrentAttires(star);
  const auto mods = ModAttires(star);
  for (int a = 1; a <= 4; ++a)
    if (std::find(now.begin(), now.end(), a) == now.end() && !mods.count(a)) return a;
  return 0;
}

std::vector<int> GearOf(const std::wstring& pac) {
  std::vector<int> v;
  for (const auto& g : GearBudget(pac))
    if (g.entry.size() >= 8 && std::isdigit(uint8_t(g.entry[6]))) v.push_back(g.entry[6] - '0' + 1);
  return v;
}

// A superstar mod as this attire (one step): an installed one (Mods\Superstars\<folder>), a .svrmod or a superstar
// project (.svrproj) - its model pac (a mod's or a project's copy taken out into the project cache), its name and
// entrance. The rest of the page (which of its attires, the gear, the clash checks) then works on that pac.
struct StarSource {
  std::wstring model;
  std::string name;
  int entrance = -1;
};
bool ReadStarSource(const std::wstring& src, StarSource& out, std::string* why) {
  std::error_code ec;
  out = StarSource();
  if (fs::is_directory(src, ec)) {  // (installed: the folder's manifest and its files as they are)
    Bytes t;
    if (!ReadFile(PathStr(fs::path(src) / L"manifest.txt"), t)) return *why = "the folder has no manifest.txt", false;
    const std::string m(t.begin(), t.end());
    if (Value(m, "type") != "superstar") return *why = "it isn't a superstar mod (type=" + Value(m, "type") + ")", false;
    const std::string inner = Value(m, "model").empty() ? "ch.pac" : Value(m, "model");
    out.model = (fs::path(src) / fs::u8path(inner)).wstring();
    out.name = Value(m, "name");
    out.entrance = Value(m, "entrance").empty() ? -1 : std::atoi(Value(m, "entrance").c_str());
  } else {
    Bytes zip;
    ProjectIn in;
    in.dir = fs::path(src).parent_path() / fs::path(src).stem();
    if (!ReadFile(Utf8(src), zip) || !ZipRead(zip, in.files)) return *why = "it can't be read as a .svrmod or a project", false;
    const bool mod = Lower(FileName(src)).ends_with(".svrmod");
    const ZipEntry* txt = in.Find(mod ? "manifest.txt" : "project.txt");
    if (!txt) return *why = mod ? "the mod has no manifest.txt" : "the project has no project.txt", false;
    in.text.assign(txt->data.begin(), txt->data.end());
    if (mod && in.Get("type") != "superstar") return *why = "it isn't a superstar mod (type=" + in.Get("type") + ")", false;
    if (mod) {
      const std::string inner = in.Get("model").empty() ? "ch.pac" : in.Get("model");
      if (in.Find(inner)) out.model = in.Extract(inner);
    } else {  // (a project: the model's original path while it is there, else the project's copy)
      const std::string from = in.Get("model.from");
      out.model = !from.empty() && fs::exists(Wide(from), ec) ? Wide(from) : in.Get("model").empty() ? L"" : in.Extract(in.Get("model"));
    }
    out.name = in.Get("name");
    out.entrance = in.GetInt("entrance", -1);
  }
  if (out.model.empty() || !fs::exists(out.model, ec)) return *why = "it has no model pac (ch.pac)", false;
  return true;
}

// The installed superstar mods: folder -> name (A-Z by folder).
std::vector<std::pair<std::wstring, std::string>> InstalledStars() {
  std::vector<std::pair<std::wstring, std::string>> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec)) {
    Bytes t;
    if (!e.is_directory(ec) || !ReadFile(PathStr(e.path() / L"manifest.txt"), t)) continue;
    const std::string m(t.begin(), t.end());
    if (Value(m, "type") != "superstar") continue;
    out.emplace_back(e.path().wstring(), Value(m, "name").empty() ? PathStr(e.path().filename()) : Value(m, "name"));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool g_keep_entrance = false;  // (From a superstar mod: its entrance= goes with the attire)

// The source taken in: the model, the attire's name when none is typed, the entrance if asked.
bool TakeStarSource(const std::wstring& src) {
  StarSource s;
  std::string why;
  if (!ReadStarSource(src, s, &why)) return Status("That superstar mod can't be used: " + why + "."), false;
  g_at.model = s.model, g_at.from = 1, g_at.gear.clear();
  if (!g_at.name[0] && !s.name.empty()) std::snprintf(g_at.name, sizeof g_at.name, "%s", s.name.c_str());
  if (g_keep_entrance && s.entrance > 0) g_at.entrance = s.entrance;
  Log("attire: from the superstar mod " + Utf8(src) + " - model " + Utf8(s.model) + ", entrance " +
      (g_keep_entrance && s.entrance > 0 ? std::to_string(s.entrance) : std::string("the superstar's")));
  Status((s.name.empty() ? std::string("The superstar mod") : s.name) + "'s look is the model now" +
         (g_keep_entrance && s.entrance > 0 ? ", with its entrance." : ".") + " Pick which of its attires below.");
  Touch();
  return true;
}

void Problems(std::vector<Problem>& p) {
  if (g_at.star < 0) p.push_back(Error("No superstar yet.", "Pick whose attire this is."));
  if (g_at.model.empty()) p.push_back(Error("No model yet.", "A character model pac (ch.pac): its first attire becomes this attire."));
  else {
    Bytes head;
    std::error_code ec;
    const bool exists = fs::exists(g_at.model, ec);
    if (FILE* f = exists ? _wfopen(g_at.model.c_str(), L"rb") : nullptr) {
      head.resize(4);
      if (std::fread(head.data(), 1, 4, f) != 4) head.clear();
      std::fclose(f);
    }
    if (!exists) p.push_back(Error("The model file is missing: " + FileName(g_at.model)));
    else if (head.size() < 4 || std::memcmp(head.data(), "EPK8", 4))
      p.push_back(Error("That isn't a character model pac (EPK8)" + std::string(head.size() == 4 && !std::memcmp(head.data(), "EPAC", 4) ? ": an EPAC is an arena or menu pac." : ".")));
    else {
      BudgetProblems(g_at.model, "This attire", true, p);

    }
  }
  if (g_at.star >= 0) {
    const auto now = CurrentAttires(g_at.star);
    if (!g_at.slot && now.size() >= 4)
      p.push_back(Error("The superstar already has 4 attires - the game's limit.", "Replace one of them instead."));
    const auto mods = ModAttires(g_at.star);
    const int next = NextFreeAttire(g_at.star);
    const StarInfo* who = StarOrDlcById(g_at.star);
    const std::string star = who ? who->name : "superstar " + std::to_string(g_at.star);
    const bool gear = EntranceGear(g_at.star);
    const bool own_slot = g_at.slot && std::find(now.begin(), now.end(), g_at.slot) != now.end();
    if (gear && !own_slot) {
      p.push_back(Error(GearText(star, now.size()), "Pick Replace 1" + std::string(now.size() > 1 ? " - " + std::to_string(now.size()) : "") + "."));
    } else if (g_at.slot && mods.count(g_at.slot)) {
      p.push_back(Warning("Attire " + std::to_string(g_at.slot) + " of " + star + " is already used by \"" + mods.at(g_at.slot) + "\" (installed). " +
                              (next ? "If you install this, the launcher moves it to attire " + std::to_string(next) + "."
                                    : gear ? star + " wears entrance gear, so it can't move to another attire: switch one of the two off."
                                           : star + " has no free attire (4 at most): the launcher will ask to switch one off."),
                          next ? "Pick attire " + std::to_string(next) + " (or A new attire) now." : "Switch one attire mod off in the launcher's Mods tab."));
    } else if (g_at.slot && std::find(now.begin(), now.end(), g_at.slot) == now.end()) {
      p.push_back(Warning("The superstar has no attire " + std::to_string(g_at.slot) + " now: this adds it as attire " + std::to_string(g_at.slot) + "."));
    }
    if (!gear && !g_at.slot && !next && now.size() < 4)
      p.push_back(Error(star + " has no free attire (4 at most): installed attire mods use the rest.", "Replace one of the attires, or switch an attire mod off in the launcher's Mods tab."));
  }
  if (!g_at.name[0]) p.push_back(Warning("No name: CHANGE ATTIRE shows ATTIRE N."));
  if (!g_at.model.empty() && g_at.gear != "off") {  // the gear's memory, on top of the attire's own (until measured)
    const int k = g_at.gear.rfind("from", 0) == 0 ? std::atoi(g_at.gear.c_str() + 4) : g_at.from;
    int gear = 0;  // (it loads instead of the attire's model for the entrance: stock gear + attire reach ~370 pages)
    for (const auto& g : GearBudget(g_at.model))
      if (g.entry.size() >= 8 && g.entry[6] - '0' + 1 == k) gear = g.pages;
    if (gear > kModelPages)
      p.push_back(Warning("The entrance gear needs " + std::to_string(gear) + " of the " + std::to_string(kModelPages) +
                              " memory pages a wrestler gets: the entrance may not load.", "Leave the gear out, or use a lighter one."));

  }
}

void Draw() {
  if (!g_ed_test.empty()) RunEditorTest();
  if (!g_use_star.empty()) {
    TakeStarSource(g_use_star);
    g_use_star.clear();
  }
  if (g_test.pending) {
    g_test.pending = false;
    g_at.star = g_test.star, g_at.slot = g_test.slot;
    if (Lower(FileName(g_test.model)).ends_with(".pac")) {
      g_at.model = g_test.model;
      std::snprintf(g_at.name, sizeof g_at.name, "RING GEAR");
    } else {
      TakeStarSource(g_test.model);  // (a superstar mod: an installed folder, a .svrmod or a project)
    }
    if (g_test_from > 0) g_at.from = g_test_from;
    if (!g_test_name.empty()) std::snprintf(g_at.name, sizeof g_at.name, "%s", g_test_name.c_str());
  }
  if (!g_test_save.empty() && g_at.star >= 0 && !g_at.model.empty()) {
    std::vector<ZipEntry> files;
    if (Build(files) && WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
    g_test_save.clear();
  }
  Heading("Attires", "A new attire for a superstar the game has (or a superstar mod), or one replacing an attire: "
                     "its model, name and entrance. It shows on CHANGE ATTIRE; the superstar's pictures stay.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("attire_body", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, -48 * g_scale), false);
  const auto& stars = Stars();
  const StarInfo* who = StarOrDlcById(g_at.star);
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Superstar", who ? who->name.c_str() : g_at.star >= 0 ? std::to_string(g_at.star).c_str() : "(pick one)")) {
    for (const auto& s : stars)
      if (ImGui::Selectable((s.name + "##" + std::to_string(s.id)).c_str(), g_at.star == s.id)) g_at.star = s.id, g_at.slot = EntranceGear(s.id) ? 1 : 0, Touch();
    // (the installed DLC's superstars: their attires are in the DLC packages - the game replaces / adds them the same way)
    if (!DlcStars().empty()) ImGui::SeparatorText("DLC");
    for (const auto& s : DlcStars())
      if (ImGui::Selectable((s.name + "##" + std::to_string(s.id)).c_str(), g_at.star == s.id)) g_at.star = s.id, g_at.slot = EntranceGear(s.id) ? 1 : 0, Touch();
    ImGui::EndCombo();
  }
  if (g_at.star >= 0) {
    const auto now = CurrentAttires(g_at.star);
    std::string list;
    for (int a : now) list += (list.empty() ? "" : ", ") + std::to_string(a);
    ImGui::TextDisabled("Attires in the model pac: %s (of 4; some may be locked in the game)", list.empty() ? "none found" : list.c_str());
    const bool gear = EntranceGear(g_at.star);
    ImGui::BeginDisabled(gear);
    if (ImGui::RadioButton("A new attire", g_at.slot == 0)) g_at.slot = 0, Touch();
    ImGui::EndDisabled();
    const auto mods = ModAttires(g_at.star);
    for (int a = 1; a <= 4; ++a) {
      ImGui::SameLine();
      const bool exists = std::find(now.begin(), now.end(), a) != now.end();
      const bool modded = mods.count(a) > 0;
      if (gear && !exists) continue;  // (entrance gear: their own attires only)
      if (ImGui::RadioButton(((exists ? "Replace " : "Attire ") + std::to_string(a) + (modded ? " (a mod's)" : exists ? "" : " (free?)")).c_str(), g_at.slot == a)) g_at.slot = a, Touch();
      if (ImGui::IsItemHovered()) {
        if (modded) ImGui::SetTooltip("Used by the installed attire mod \"%s\".", mods.at(a).c_str());
        else if (!exists) ImGui::SetTooltip("No model for it in the superstar's pac: free if the game allows (its attire table decides how many a superstar has).");
        else ImGui::SetTooltip("The game's own attire %d: this replaces it.", a);
      }
    }
    const int next = NextFreeAttire(g_at.star);
    if (gear) {
      const StarInfo* w = StarOrDlcById(g_at.star);
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "%s", GearText(w ? w->name : std::to_string(g_at.star), now.size()).c_str());
      ImGui::PopTextWrapPos();
    }
    if (g_at.slot && mods.count(g_at.slot)) {
      ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Attire %d is already used by \"%s\".", g_at.slot, mods.at(g_at.slot).c_str());
      if (next) {
        ImGui::SameLine();
        if (ImGui::SmallButton(("Use attire " + std::to_string(next)).c_str())) g_at.slot = next, Touch();
      }
    } else if (g_at.slot && std::find(now.begin(), now.end(), g_at.slot) != now.end()) {
      ImGui::TextDisabled("Replaces the game's attire %d.", g_at.slot);
    } else if (!g_at.slot && !gear) {
      ImGui::TextDisabled("%s", next ? ("It becomes attire " + std::to_string(next) + " (the next free one).").c_str() : "No attire is free: replace one instead.");
    }
  }
  if (FileRow("Model (ch.pac)...", g_at.model, "none", kPacFilter, 1, "A character model pac - one of the game's, a superstar mod's or a "
              "converted one (From another game tab): the attire picked below becomes this attire."))
    g_at.from = 1;
  ImGui::BeginDisabled(Busy());
  if (ImGui::Button("From a superstar mod...")) ImGui::OpenPopup("attire_star_mod");
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox("Keep its entrance", &g_keep_entrance);
  Hint("One step: a superstar mod (installed, a .svrmod or a superstar project) becomes this attire of the superstar "
       "picked above - its model, its name, and its entrance if kept. Pick which of its attires below.");
  if (ImGui::BeginPopup("attire_star_mod")) {
    const auto stars = InstalledStars();
    if (stars.empty()) ImGui::TextDisabled("No superstar mods installed.");
    for (const auto& [dir, name] : stars)
      if (ImGui::Selectable((name + "##" + Utf8(dir)).c_str())) TakeStarSource(dir);
    ImGui::Separator();
    if (ImGui::Selectable("A .svrmod or project file...")) {
      const std::wstring f = PickFile(false, L"A superstar mod or project", kOpenFilter, 1, L"svrmod", L"");
      if (!f.empty()) TakeStarSource(f);
    }
    ImGui::EndPopup();
  }
  if (!g_at.model.empty()) {  // which of the pac's attires
    const std::vector<int> own = ModelAttires(g_at.model);
    if (own.size() > 1) {
      ImGui::TextDisabled("Its attire");
      for (int a : own) {
        ImGui::SameLine();
        if (ImGui::RadioButton((std::to_string(a) + "##from").c_str(), g_at.from == a)) g_at.from = a, Touch();
      }
      Hint("The model pac holds several attires: the one picked becomes this attire (the mod carries only it).");
    }
  }
  if (!g_at.model.empty()) {  // entrance gear (EMD kind 4): what this attire wears for the entrance
    const auto gear = GearOf(g_at.model);
    const bool has = std::find(gear.begin(), gear.end(), g_at.from) != gear.end();
    ImGui::TextDisabled("Entrance gear");
    ImGui::SameLine();
    const int from_k = g_at.gear.rfind("from", 0) == 0 ? std::atoi(g_at.gear.c_str() + 4) : 0;
    if (g_at.gear == "off") ImGui::TextUnformatted("left out");
    else if (from_k) ImGui::Text("attire %d's", from_k);
    else ImGui::TextUnformatted(has ? "its own (worn for the entrance)" : "none");
    if (has || from_k) {
      ImGui::SameLine();
      bool off = g_at.gear == "off";
      if (ImGui::Checkbox("Leave it out##gear", &off)) g_at.gear = off ? "off" : "", Touch();
    }
    for (int k : gear) {
      if (k == g_at.from || k == from_k) continue;
      ImGui::SameLine();
      if (ImGui::SmallButton(("Use attire " + std::to_string(k) + "'s gear").c_str())) g_at.gear = "from" + std::to_string(k), Touch();
    }
    if (from_k) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Undo##gear")) g_at.gear.clear(), Touch();
    }
    if ((has || from_k) && g_at.gear != "off") {
      ImGui::SameLine();
      if (ImGui::SmallButton("Show it##gear")) char_preview::SetAttire(from_k ? from_k : g_at.from), char_preview::SetGear(true);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(Busy());
    if (ImGui::SmallButton("From another superstar...##gear")) ImGui::OpenPopup("gear_donor");
    ImGui::EndDisabled();
    int donor = 0;
    if (star_page::GearDonorPopup("gear_donor", donor))
      star_page::GearTransplant(g_at.model, g_at.from, donor, [](const std::wstring& made) {
        g_at.model = made, g_at.gear.clear();
        char_preview::SetGear(true);
        Touch();
      });
    Hint("A coat, hat or shirt worn only for the entrance (the model's EMD kind 4 for that attire: the whole wrestler in it). "
         "Leave it out to walk out in the attire itself; From another superstar fits their coat or hat onto this model.");
  }
  TextField("Name on CHANGE ATTIRE", g_at.name, sizeof g_at.name, "e.g. 2003 GEAR");
  if (star_page::PickEntrance("Entrance (optional)", g_at.entrance, "the superstar's")) Touch();
  TextField("Author (optional)", g_at.author, sizeof g_at.author);
  TextField("Version (optional)", g_at.version, sizeof g_at.version, "1.0");
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::Spacing();
  {
    const int from_k = g_at.gear.rfind("from", 0) == 0 ? std::atoi(g_at.gear.c_str() + 4) : 0;
    const bool gear = char_preview::Gear() && char_preview::HasGear();
    const int shown = gear && from_k ? from_k : g_at.from;
    int n = 0;
    for (const auto& [k, e] : g_at.mats) n += !e.Empty();
    const std::string head = "Materials" + (n ? " (" + std::to_string(n) + " changed)" : std::string()) + "###materials";
    if (ImGui::CollapsingHeader(head.c_str())) {
      if (MaterialPanel(g_at.model, shown, gear, shown, g_at.mats, false)) Touch();
    } else {
      std::map<std::pair<uint32_t, int>, MatEdit> preview;
      for (const auto& [k, e] : g_at.mats)
        if (std::get<0>(k) == shown && std::get<1>(k) == gear) preview[{std::get<2>(k), std::get<3>(k)}] = e;
      char_preview::SetMaterialPreview(preview);
    }
  }
  if (ImGui::CollapsingHeader("Attires inside a model pac")) Editor();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginGroup();
  {  // (the gear another attire lends: shown on that attire's entry)
    const int from_k = g_at.gear.rfind("from", 0) == 0 ? std::atoi(g_at.gear.c_str() + 4) : 0;
    char_preview::SetAttire(char_preview::Gear() && from_k ? from_k : g_at.from);
    if (g_at.gear == "off" && char_preview::Gear()) char_preview::SetGear(false);
  }
  char_preview::SetModel(g_at.model, g_game);
  char_preview::SetHeight(1.0f);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  char_preview::Draw(avail.x, std::max(200.0f, avail.y - 96 * g_scale));
  ViewFooter();
  if (!char_preview::Status().empty()) ImGui::TextDisabled("%s", char_preview::Status().c_str());
  ImGui::EndGroup();
  switch (ModButtons(problems, false)) {
    case 1: Save(); break;
    case 2: Install(); break;
  }
}

// ---- the editor of a pac's attires
bool LoadPac(const std::wstring& f, Epac& out) {
  Bytes d;
  return ReadFile(Utf8(f), d) && d.size() >= 0x4000 && !std::memcmp(d.data(), "EPK8", 4) && EpacRead(d, out);
}

bool InGamePac(const std::wstring& f) {  // (the game's own pac folder is never written)
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(fs::path(f), ec), b = fs::weakly_canonical(fs::path(g_game) / L"pac", ec);
  const std::string as = Lower(Utf8(a.wstring())), bs = Lower(Utf8(b.wstring()));
  return !bs.empty() && as.rfind(bs, 0) == 0;
}

std::string KindsText(const Epac& e, int a) {
  std::string s;
  for (int k : PacAttireKinds(e, a)) s += (s.empty() ? "" : ", ") + std::string(k == 2 ? "model" : k == 4 ? "entrance outfit" : "kind " + std::to_string(k));
  return s;
}

void Editor() {
  ImGui::TextWrapped("Several attires can live in one model pac (the game uses a superstar mod's first 4). Open a pac, "
                     "take attires in from other pacs or remove them, then save it as a copy - for a superstar mod's model, "
                     "or as the model of an attire mod above.");
  if (FileRow("Pac to edit...", g_ed.file, "none", kPacFilter, 1, "A character model pac. It is read; the edits go into a copy you save."))
    g_ed.loaded = !g_ed.file.empty() && LoadPac(g_ed.file, g_ed.pac), g_ed.dirty = false, g_ed.target = 0;
  if (g_ed.file.empty()) return;
  if (!g_ed.loaded) {
    ImGui::TextColored(kBad, "That isn't a character model pac (EPK8).");
    return;
  }
  const std::vector<int> have = PacAttires(g_ed.pac);
  if (ImGui::BeginTable("pac_attires", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
    for (int a : have) {
      ImGui::PushID(a);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Attire %d%s", a, a > 4 ? " (unused: over 4)" : "");
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", KindsText(g_ed.pac, a).c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Remove")) {
        std::string err;
        if (PacRemoveAttire(g_ed.pac, a, &err)) g_ed.dirty = true, Status("Attire " + std::to_string(a) + " removed (the later ones moved down).");
        else Status("Not removed: " + err + ".");
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Take an attire in");
  if (FileRow("From pac...", g_ed.src, "none", kPacFilter, 1, "Another character model pac: one of its attires is copied in."))
    g_ed.src_loaded = !g_ed.src.empty() && LoadPac(g_ed.src, g_ed.src_pac), g_ed.src_attire = 1;
  if (g_ed.src_loaded) {
    const std::vector<int> theirs = PacAttires(g_ed.src_pac);
    ImGui::TextDisabled("its attire");
    for (int a : theirs) {
      ImGui::SameLine();
      if (ImGui::RadioButton((std::to_string(a) + "##src").c_str(), g_ed.src_attire == a)) g_ed.src_attire = a;
    }
    ImGui::TextDisabled("as");
    ImGui::SameLine();
    if (ImGui::RadioButton("a new attire", g_ed.target == 0)) g_ed.target = 0;
    for (int a : have) {
      ImGui::SameLine();
      if (ImGui::RadioButton(("attire " + std::to_string(a) + " (replace)##t").c_str(), g_ed.target == a)) g_ed.target = a;
    }
    const int to = g_ed.target ? g_ed.target : (have.empty() ? 1 : have.back() + 1);
    ImGui::BeginDisabled(to > 4 && !g_ed.target);
    if (ImGui::Button(("Copy it in as attire " + std::to_string(to)).c_str())) {
      std::string err;
      if (PacCopyAttire(g_ed.pac, to, g_ed.src_pac, g_ed.src_attire, &err)) g_ed.dirty = true, Status("Attire " + std::to_string(to) + " taken from " + FileName(g_ed.src) + ".");
      else Status("Not copied: " + err + ".");
    }
    ImGui::EndDisabled();
    if (to > 4 && !g_ed.target) ImGui::TextDisabled("(4 attires already: the game uses no more)");
  }
  ImGui::Spacing();
  ImGui::BeginDisabled(!g_ed.dirty);
  PushAccent();
  if (ImGui::Button("Save the pac as...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(true, L"Save the edited model pac (a copy)", kPacFilter, 1, L"pac", fs::path(g_ed.file).filename().c_str());
    if (!f.empty() && InGamePac(f)) Status("Not into the game's pac folder: save the copy elsewhere (a superstar mod carries it).");
    else if (!f.empty()) {
      const bool ok = WriteFile(Utf8(f), EpacWrite(g_ed.pac));
      Status(ok ? "Saved " + Utf8(f) + "." : "Could not write " + Utf8(f) + ".");
      if (ok) g_ed.file = f, g_ed.dirty = false;
    }
  }
  PopAccent();
  ImGui::EndDisabled();
  if (g_ed.dirty) {
    ImGui::SameLine();
    ImGui::TextColored(kWarn, "edited, not saved");
  }
}

// --pac-attires <pac>,<op>,...: copy:<to>:<src pac>:<from> | remove:<a> | save:<out>; quits after
void RunEditorTest() {
  std::vector<std::wstring> parts;
  for (size_t at = 0; at <= g_ed_test.size();) {
    size_t c = g_ed_test.find(L',', at);
    if (c == std::wstring::npos) c = g_ed_test.size();
    parts.push_back(g_ed_test.substr(at, c - at));
    at = c + 1;
  }
  g_ed_test.clear();
  Epac pac;
  if (parts.empty() || !LoadPac(parts[0], pac)) { Log("test: pac-attires: can't read the pac"); PostMessageW(g_wnd, WM_CLOSE, 0, 0); return; }
  for (size_t i = 1; i < parts.size(); ++i) {
    const std::wstring& op = parts[i];
    std::string err;
    bool ok = true;
    if (op.rfind(L"remove:", 0) == 0) ok = PacRemoveAttire(pac, _wtoi(op.c_str() + 7), &err);
    else if (op.rfind(L"save:", 0) == 0) ok = !InGamePac(op.substr(5)) && WriteFile(Utf8(op.substr(5)), EpacWrite(pac));
    else if (op.rfind(L"copy:", 0) == 0) {  // copy:<to>:<src>:<from>  (the src path holds a ':' after the drive)
      const std::wstring rest = op.substr(5);
      const size_t a = rest.find(L':'), b = rest.rfind(L':');
      Epac src;
      ok = a != b && LoadPac(rest.substr(a + 1, b - a - 1), src) && PacCopyAttire(pac, _wtoi(rest.c_str()), src, _wtoi(rest.c_str() + b + 1), &err);
    }
    std::string list;
    for (int x : PacAttires(pac)) list += " " + std::to_string(x);
    Log("test: pac-attires " + Utf8(op) + (ok ? " ok" : " FAILED " + err) + "; attires:" + list);
  }
  PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

std::string StateText() {
  return "star=" + std::to_string(g_at.star) + "\nslot=" + std::to_string(g_at.slot) + "\nname=" + g_at.name +
         "\nmodel=" + Utf8(g_at.model) + "\nentrance=" + std::to_string(g_at.entrance) + "\nauthor=" + g_at.author +
         "\nversion=" + g_at.version + "\ngear=" + g_at.gear + "\nmats=" + MaterialsText(g_at.mats);
}

void Reset() { g_at = AttireProject(); }

void WriteProject(ProjectOut& out) {
  out.Key("superstar", g_at.star);
  out.Key("attire", g_at.slot ? std::to_string(g_at.slot) : std::string("new"));
  out.Key("name", g_at.name);
  out.Key("author", g_at.author);
  out.Key("version", g_at.version);
  if (g_at.entrance > 0) out.Key("entrance", g_at.entrance);
  out.Key("gear", g_at.gear);
  if (g_at.from > 1) out.Key("from_attire", g_at.from);
  out.File("model", g_at.model, "attire/model");
  {
    std::string t = MaterialsText(g_at.mats);  // (one line: entries apart by tabs)
    std::replace(t.begin(), t.end(), '\n', '\t');
    out.Key("materials", t);
  }
}

bool Read(const ProjectIn& in, bool mod) {
  g_at.star = in.GetInt("superstar", -1);
  const std::string a = in.Get("attire");
  g_at.slot = a == "new" || a.empty() ? 0 : std::clamp(std::atoi(a.c_str()), 1, 4);
  std::snprintf(g_at.name, sizeof g_at.name, "%s", in.Get("name").c_str());
  std::snprintf(g_at.author, sizeof g_at.author, "%s", in.Get("author").c_str());
  std::snprintf(g_at.version, sizeof g_at.version, "%s", in.Get("version").c_str());
  g_at.gear = in.Get("gear");
  {
    std::string t = in.Get("materials");
    std::replace(t.begin(), t.end(), '\t', '\n');
    g_at.mats = MaterialsFromText(t);
  }
  if (!g_at.version[0]) std::snprintf(g_at.version, sizeof g_at.version, "1.0");
  g_at.entrance = in.GetInt("entrance", -1);
  g_at.from = mod ? 1 : std::clamp(in.GetInt("from_attire", 1), 1, 9);  // (a mod's attire.pac holds only its attire)
  if (mod) {
    const std::string m = in.Get("model").empty() ? "attire.pac" : in.Get("model");
    if (in.Find(m)) g_at.model = in.Extract(m);
  } else {
    const std::string from = in.Get("model.from");
    g_at.model = !from.empty() && fs::exists(Wide(from)) ? Wide(from) : in.Extract(in.Get("model"));
  }
  return true;
}

}  // namespace

void TestPacAttires(const std::wstring& spec) { g_ed_test = spec; }

void TestStart(int star, const std::wstring& model, int slot, const std::wstring& save) {
  g_test = {star, model, slot, true};  // (applied on the page's first frame: the projects are reset after the command line)
  g_test_save = save;
}
void TestKeepEntrance() { g_keep_entrance = true; }
void TestFrom(int from, const std::string& name) { g_test_from = from, g_test_name = name; }
void UseStarMod(const std::wstring& src) { g_use_star = src; }

PageHooks hooks = {
    "attire", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace attire_page
}  // namespace mm
