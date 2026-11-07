// Arena and Backstage pages: a new arena from one of the game's 20 (edited in
// Blender or the 3D editor, or built from nothing), its banner and VS screen;
// a backstage brawl room rebuilt (bg78). Builds the .svrmod (manifest.txt,
// arena.pac, banner.dds, vs/*.dds) and installs it. The game: arena_mods.cpp.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "app.h"
#include "arena_page_internal.h"
#include "json.h"
#include "mod_check.h"
#include "tool_run.h"
#include <shellapi.h>

#include "editor.h"
#include "svrfmt/arena.h"
#include "svrfmt/arena_build.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/pac.h"
#include "svrfmt/ring_kit.h"

namespace mm {
namespace arena_page {

using namespace svrfmt;


// Backstage brawl areas: all seven are rooms of one file, bg78.pac (models
// by id range); a backstage brawl's match type decides the room. A backstage
// mod is bg78 with one room rebuilt: the game plays it in that room's matches.
struct Area {
  const char* name;
  const char* about;
  std::vector<std::pair<int, int>> ids;  // its models' ids (1000+: its objects: cars, crates...)
  // the game's box for the area (sub_8224EF28): centre x y z, half x z
  float spot[5];
};
const Area kAreas[7] = {
    {"Parking lot", "Cars, a truck and the loading bay.", {{0, 19}, {1040, 1069}}, {125, 0, -410, 38, 50}},
    {"GM's office", "The desk, the couch and the TV.", {{20, 39}, {1000, 1024}}, {-338, -8, 217, 30, 27}},
    {"Locker room A", "Lockers, benches and the showers.", {{40, 59}, {1090, 1109}}, {290, -8, -190, 34, 34}},
    {"Locker room B", "The second locker room.", {{60, 79}, {1070, 1089}}, {326, -8, 241, 34, 34}},
    {"Large locker room", "The big room with the massage tables.", {{80, 99}, {1025, 1039}}, {460, -8, -76, 25, 25}},
    {"Interview area", "The backdrop, the cameras and the corridor.", {{160, 179}, {140, 159}, {1110, 1139}}, {0, -8, 141, 30, 30}},
    {"Catering area", "Tables, food and the drinks.", {{140, 159}, {1130, 1139}}, {20, -8, -190, 45, 30}},
};

Project g_proj;
bool g_simple = false;  // (kept through a reset: the mode, not the project)
int g_simple_step = 0;
int g_sel = 0;    // the tile under the cursor on the Start tab
int g_area = 0;   // the backstage area picked
int g_tab = 0;

// an arena loaded in the background, handed to the UI thread (the editor holds g_proj.edited)
std::mutex g_pending_mutex;
std::unique_ptr<Arena> g_pending;
std::string g_pending_fbx;
bool g_pending_to_editor = false;
bool g_pending_no_crowd = false;  // (a new empty arena)
// test aid (--test-edit-save <file>): once the editor has its arena, editor::TestEdit,
// name "Test Edit", save the mod there and quit
std::wstring g_test_save;
int g_test_lib = -1;
std::atomic<int> g_test_state{0};

void LeaveBackstage() {
  if (g_proj.backstage < 0) return;
  g_proj.backstage = -1;
  editor::SetArea({});
  g_proj.arena = -1;
}

void StartProject(int i) {
  LeaveBackstage();
  if (g_proj.arena == i) return;
  g_proj.arena = i;
  g_proj.edited.reset();
  g_proj.fbx.clear();
  editor::SetArena(nullptr, "");
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s (custom)", g_arenas[i].name);
  Touch();
}

void LoadInBackground(const std::string& pac, std::function<void(Arena&)> prepare, bool to_editor, bool no_crowd,
                      const std::string& fbx) {
  RunInBackground([=] {
    Progress("Reading the arena file ...");
    auto a = std::make_unique<Arena>();
    std::string err;
    if (!a->Load(pac, &err)) {
      Status("The arena could not be read: " + err);
      return;
    }
    if (prepare) prepare(*a);
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx = fbx;
    g_pending_to_editor = to_editor;
    g_pending_no_crowd = no_crowd;
  });
}

void ExportArena(int i) {
  const std::wstring dir = PickFolder(L"Export to Blender: choose a folder for the arena files");
  if (dir.empty()) return;
  const std::string out = PathStr(fs::path(dir) / (Wide(std::string(g_arenas[i].banner + 6)) + L" export"));
  const std::string pac = ArenaPath(i), title = g_arenas[i].name;
  RunInBackground([pac, out, title] {
    Progress("Exporting " + title + " ...");
    Arena a;
    std::string err;
    if (!a.Load(pac, &err)) { Status(err); return; }
    ExportReport rep;
    if (!svrfmt::ExportArena(a, out, title, rep)) {
      for (const auto& w : rep.warnings) Log("  " + w);
      Status("The export failed (see the log).");
      return;
    }
    char b[256];
    std::snprintf(b, sizeof b, "Exported %d objects and %d textures to %s", rep.models, rep.textures, out.c_str());
    Status(b);
    Log("  Open arena.fbx in Blender (File > Import > FBX). 1 Blender unit = 1 metre.");
    ShellOpen(Wide(out));
  });
}

void ImportArena(int i) {
  const COMDLG_FILTERSPEC spec[] = {{L"FBX (*.fbx)", L"*.fbx"}};
  const std::wstring fbx = PickFile(false, L"Import from Blender: the edited arena FBX", spec, 1);
  if (fbx.empty()) return;
  StartProject(i);
  const std::string pac = ArenaPath(i), fbx8 = Utf8(fbx);
  RunInBackground([pac, fbx8] {
    Progress("Importing " + fbx8 + " ...");
    auto a = std::make_unique<Arena>();
    std::string err;
    if (!a->Load(pac, &err)) { Status(err); return; }
    ImportOptions opt;
    ImportReport rep;
    const bool ok = ImportFbx(*a, fbx8, opt, rep);
    for (const auto& w : rep.warnings) Log("  " + w);
    for (const auto& e : rep.errors) Log("  error: " + e);
    if (!ok) { Status("The import failed (see the log)."); return; }
    char b[256];
    std::snprintf(b, sizeof b, "Imported: %d models changed, %d objects added, textures %d replaced %d added",
                  rep.models_changed, rep.objects_added, rep.textures_replaced, rep.textures_added);
    std::string serr;
    a->Save(&serr);  // (checks it would load)
    if (!serr.empty()) { Status("error: " + serr); return; }
    Status(b);
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx = fbx8;
    g_pending_to_editor = false;
  });
}

// A new arena from nothing: the slot's file with everything but the ring,
// the floor and the ringside parts emptied. The slot (the arena it loads in
// place of) only decides the room it has and the VS screen style.
void NewArena(int slot, bool to_editor) {
  StartProject(slot);
  std::snprintf(g_proj.name, sizeof g_proj.name, "New Arena");
  LoadInBackground(ArenaPath(slot), [](Arena& a) {
    EmptyOptions opt;
    EmptyReport rep;
    MakeEmpty(a, opt, rep);
    char b[200];
    std::snprintf(b, sizeof b, "New empty arena: %d objects and %d textures cleared (%.1f MB of room). Add objects, "
                  "props from other arenas (Library) and your own files.", rep.models_emptied, rep.textures_shrunk,
                  rep.bytes_freed / 1048576.0);
    Status(b);
  }, to_editor, true);
}

std::string ModId() {
  return !g_proj.id.empty() ? g_proj.id : IdFrom(g_proj.name, g_proj.backstage >= 0 ? "backstage" : "arena");
}

Bytes BakeTheme(const Bytes& pac, const std::map<std::string, Image>& mine);

// A mod build: copied and set up on the UI thread (the editor keeps
// editing g_proj.edited), compressed in the background.
struct BuildJob {
  bool backstage = false;
  int area = -1;
  std::vector<std::string> keep;  // textures FitFile leaves as they are
  std::shared_ptr<Arena> arena;
  std::string manifest, id;
  Image banner, load, card;
  std::map<std::string, Image> vs;
  Bytes theme_pac;  // its own VS theme (the vs pictures go into it)
  Bytes kept_pac;   // the opened mod's arena.pac, unedited since: written as it is
  std::wstring gimmick_pac;
  std::string project;  // (ModProjectText: the page's project keys, into the mod)
};

bool PrepareBuild(BuildJob& job) {
  job.project = ModProjectText(g_proj.backstage >= 0 ? PageId::kBackstage : PageId::kArena);
  if (g_proj.backstage >= 0) {  // a backstage area: bg78 with the room rebuilt
    if (!g_proj.edited) { Status("Open the area in the 3D editor first."); return false; }
    job.arena = std::make_shared<Arena>(*g_proj.edited);
    editor::ApplyBuild(*job.arena);
    job.id = ModId();
    job.backstage = true;
    job.area = g_proj.backstage;
    // only the room's own textures may be made smaller to fit
    for (const auto& am : job.arena->models) {
      bool mine = am.added;
      for (const auto& [lo, hi] : kAreas[g_proj.backstage].ids) mine |= am.id >= uint32_t(lo) && am.id <= uint32_t(hi);
      if (!mine) job.keep.insert(job.keep.end(), am.model.textures.begin(), am.model.textures.end());
    }
    job.manifest = "type=backstage\nid=" + job.id + "\nname=" + g_proj.name + "\nauthor=" + g_proj.author +
                   "\nversion=" + g_proj.version + "\n" + MadeWith() + "area=" + std::to_string(g_proj.backstage) + "\n";
    if (g_proj.own_area) {
      char b[160];
      job.manifest += std::string("row=") + g_proj.row + "\n";
      if (g_proj.gimmick[0] && !g_proj.gimmick_pac.empty()) {
        job.manifest += std::string("gimmick=") + g_proj.gimmick + "\n";
        job.gimmick_pac = g_proj.gimmick_pac;
      }
      if (g_proj.has_box) {
        std::snprintf(b, sizeof b, "box=%.1f,%.1f,%.1f,%.1f\n", g_proj.box[0], g_proj.box[1], g_proj.box[2], g_proj.box[3]);
        job.manifest += b;
      }
      if (g_proj.camera > 0) std::snprintf(b, sizeof b, "camera=%.1f\n", g_proj.camera), job.manifest += b;
      if (g_proj.camera_height > 0) std::snprintf(b, sizeof b, "camera_height=%.1f\n", g_proj.camera_height), job.manifest += b;
    }
    return true;
  }
  if (g_proj.arena < 0) { Status("Pick an arena first."); return false; }
  job.arena = std::make_shared<Arena>();
  if (g_proj.edited) {
    *job.arena = *g_proj.edited;
  } else if (!job.arena->Load(ArenaPath(g_proj.arena))) {
    Status("The arena file could not be read.");
    return false;
  }
  editor::ApplyBuild(*job.arena);
  job.banner = g_proj.banner.image;
  job.load = g_proj.load.image;
  job.card = g_proj.card.image;
  job.vs = g_proj.vs;
  job.theme_pac = g_proj.theme_pac;
  if (!g_proj.opened_pac.empty() && g_proj.opened_edits >= 0 && editor::EditCount() == g_proj.opened_edits)
    job.kept_pac = g_proj.opened_pac;
  if (job.banner.rgba.empty()) {
    job.banner.w = 256;
    job.banner.h = 128;
    job.banner.rgba.assign(256 * 128 * 4, 40);
    for (size_t i = 3; i < job.banner.rgba.size(); i += 4) job.banner.rgba[i] = 255;
  }
  job.id = ModId();
  job.manifest = "type=arena\nid=" + job.id + "\nname=" + g_proj.name + "\nauthor=" + g_proj.author +
                 "\nversion=" + g_proj.version + "\n" + MadeWith() + "base=" + g_arenas[g_proj.arena].banner + "\n" +
                 (g_proj.source.empty() ? std::string() : "source=" + g_proj.source + "\n") +
                 (g_proj.theme_pac.empty() ? std::string() : "vs_theme=" + g_proj.theme_label + "\n") +
                 (g_proj.theme_pac.empty() || g_proj.theme_font.empty() ? std::string() : "vs_font=" + g_proj.theme_font + "\n") +
                 (g_proj.city[0] ? std::string("city=") + g_proj.city + "\n" : std::string()) +
                 (g_proj.crowd_area[0] ? std::string("crowd_area=") + g_proj.crowd_area + "\n" : std::string()) +
                 editor::ManifestLines();
  return true;
}

// manifest.txt, arena.pac, banner.dds (false on error, logged)
bool FinishBuild(BuildJob& job, std::vector<ZipEntry>& files) {
  if (!job.kept_pac.empty()) {  // (the arena unedited since the mod was opened: its file as it was)
    Log("  The arena wasn't edited: its arena.pac stays as it was.");
    files.push_back({"manifest.txt", Bytes(job.manifest.begin(), job.manifest.end())});
    AddModProject(files, job.project);
    files.push_back({"arena.pac", std::move(job.kept_pac)});
    files.push_back({"banner.dds", DdsEncode(job.banner, DxtFormat::kDxt5, false)});
    if (job.load.w) files.push_back({"load.dds", DdsEncode(job.load, DxtFormat::kDxt5, false)});
    if (job.card.w) files.push_back({"card.dds", DdsEncode(job.card, DxtFormat::kDxt5, false)});
    if (!job.theme_pac.empty()) {
      files.push_back({"vs/theme.pac", BakeTheme(job.theme_pac, job.vs)});
      return true;
    }
    for (const auto& [name, img] : job.vs) files.push_back({"vs/" + name + ".dds", DdsEncode(img, DxtFormat::kDxt5, false)});
    return true;
  }
  if (const int grown = GrowVisibility(*job.arena))  // (moved / turned objects: svrfmt/arena.h)
    Log("  " + std::to_string(grown) + " visibility boxes grown to hold moved objects.");
  Progress("Fitting the arena into the game's room ...");
  job.arena->file_max = kArenaFileMax;  // (bigger than shipped: needs= below - only when the stage is the pac's last entry)
  const auto halved = job.arena->FitFile(job.keep);
  if (!halved.empty())
    Log("  " + std::to_string(halved.size()) + " textures halved to fit the game's room for this arena.");
  Progress("Compressing the arena ...");
  std::string err;
  Bytes pac = job.arena->Save(&err);
  if (!err.empty()) { Status("error: " + err); return false; }
  if (ArenaNeedsNew(*job.arena, pac.size())) {  // (bigger than shipped / more 3D detail: svrfmt/arena.h)
    job.manifest += std::string("needs=") + kNeedsVersion + "\n";
    Log(std::string("  The arena is bigger than its slot's or holds more 3D detail than the game's own: it needs version ") +
        kNeedsVersion + " or newer (older ones crash loading it).");
  }
  files.push_back({"manifest.txt", Bytes(job.manifest.begin(), job.manifest.end())});
  AddModProject(files, job.project);
  files.push_back({"arena.pac", std::move(pac)});
  if (job.backstage) {
    if (!job.gimmick_pac.empty()) {
      Bytes g;
      if (!ReadFile(Utf8(job.gimmick_pac), g)) { Status("The gimmick pac could not be read: " + Utf8(job.gimmick_pac)); return false; }
      files.push_back({"gimmick.pac", std::move(g)});
    }
    return true;
  }
  files.push_back({"banner.dds", DdsEncode(job.banner, DxtFormat::kDxt5, false)});
  if (job.load.w) files.push_back({"load.dds", DdsEncode(job.load, DxtFormat::kDxt5, false)});
  if (job.card.w) files.push_back({"card.dds", DdsEncode(job.card, DxtFormat::kDxt5, false)});
  if (!job.theme_pac.empty()) {  // its own VS theme, the replaced pictures in it
    Progress("The VS screen ...");
    files.push_back({"vs/theme.pac", BakeTheme(job.theme_pac, job.vs)});
    return true;
  }
  for (const auto& [name, img] : job.vs)  // (same size as the original: the VS tab resizes)
    files.push_back({"vs/" + name + ".dds", DdsEncode(img, DxtFormat::kDxt5, false)});
  return true;
}

void SaveModTo(const std::wstring& f, std::function<void(bool)> done);

std::string SignsId() { return g_proj.signs.empty() ? std::string() : ModId() + "_signs"; }

std::vector<ZipEntry> SignsFiles() {
  if (g_proj.signs.empty()) return {};
  std::vector<std::pair<std::string, Image>> signs;
  for (const auto& [n, pic] : g_proj.signs) signs.push_back({n, pic.image});
  return signs_page::PackFiles(SignsId(), std::string(g_proj.name) + " (signs)", g_proj.author, signs);
}

void SaveMod() {
  BuildJob job;
  if (!PrepareBuild(job)) return;
  const std::wstring f = PickFile(true, L"Save the mod", kModFilter, 1, L"svrmod", (Wide(job.id) + L".svrmod").c_str());
  if (f.empty()) return;
  SaveModTo(f);
}

void SaveModTo(const std::wstring& f, std::function<void(bool)> done) {
  BuildJob job;
  if (!PrepareBuild(job)) {
    if (done) done(false);
    return;
  }
  const std::string out = Utf8(f);
  // the crowd signs: a sign pack of their own beside it (<name>_signs.svrmod)
  const std::vector<ZipEntry> signs = g_proj.backstage < 0 ? SignsFiles() : std::vector<ZipEntry>();
  const std::string signs_out = signs.empty() ? "" : Utf8((fs::path(f).parent_path() / fs::u8path(SignsId() + ".svrmod")).wstring());
  RunInBackground([job, out, signs, signs_out, done]() mutable {
    std::vector<ZipEntry> files;
    const auto finish = [&](bool ok) {
      if (done) OnUiThread([done, ok] { done(ok); });
    };
    if (!FinishBuild(job, files)) return finish(false);
    if (!WriteFile(out, ZipWrite(files))) { Status("The mod could not be written: " + out); return finish(false); }
    if (!signs.empty() && !WriteFile(signs_out, ZipWrite(signs))) { Status("The crowd signs could not be written: " + signs_out); return finish(false); }
    Status("Saved " + out + (signs.empty() ? "" : " and its crowd signs " + signs_out) + " (add them in the launcher's Mods tab with +).");
    finish(true);
  });
}

bool InstallFiles(const BuildJob& job, const std::vector<ZipEntry>& files) {
  const fs::path dir = fs::path(g_game) / L"Mods" / (job.backstage ? L"Backstage" : L"Arenas") / fs::u8path(job.id);
  std::error_code ec;
  std::string why;  // (the old install replaced as a whole: e.g. VS pictures no longer replaced go)
  if (!InstallModFiles(dir, files, &why)) {
    Status("Not installed: " + why);
    return false;
  }
  if (job.backstage)
    Status(std::string("Installed: backstage brawls in the ") + kAreas[job.area].name + " play it (" + PathStr(dir) + ")." + InstallReport("Backstage", dir));
  else
    Status("Installed: it is on the arena select pages after the game's arenas (" + PathStr(dir) + ")." + InstallReport("Arenas", dir));
  return true;
}

// Builds and installs into <game>/Mods/...; then, with `start`, starts the game.
void InstallMod(bool start) {
  if (start && GameRunning()) {
    Status("The game is running: close it first (mods load when it starts).");
    return;
  }
  BuildJob job;
  if (!PrepareBuild(job)) return;
  const std::vector<ZipEntry> signs = g_proj.backstage < 0 ? SignsFiles() : std::vector<ZipEntry>();
  const std::string signs_id = SignsId();
  RunInBackground([job, start, signs, signs_id]() mutable {
    std::vector<ZipEntry> files;
    if (!FinishBuild(job, files) || !InstallFiles(job, files)) return;
    if (!signs.empty() && !signs_page::InstallPack(signs_id, signs)) { Status("The crowd signs could not be installed (see the log)."); return; }
    if (start) StartGame();
  });
}

// A mod's / project's arena.pac back into the editor: budget from the host
// arena's shipped file, the Ring Kit's output and the lighting undone (both
// apply again from the manifest's ring.* / light.* when the mod is saved).
bool LoadEditedArena(const Bytes& data, int host, int area, const std::string& manifest, std::string* err) {
  auto a = std::make_unique<Arena>();
  if (!a->LoadData(data, err)) return false;
  Arena shipped;
  const std::string host_pac = area >= 0 ? PathStr(fs::path(g_game) / L"pac" / L"bg" / L"bg78.pac") : ArenaPath(host);
  if (shipped.Load(host_pac)) {
    a->original_file = shipped.original_file;
    a->original_unpacked = shipped.original_unpacked;
    if (area < 0) {
      auto ring_part = [](uint32_t id) { return id == 952 || (id >= 956 && id <= 967); };
      for (auto& m : a->models)
        if (ring_part(m.id))
          for (const auto& s : shipped.models)
            if (s.id == m.id) m.model = s.model, m.changed = true;
      for (auto& e : a->entries)  // the crowd (crowd=0 empties it at save)
        if (e.id == 0x4E20)
          for (const auto& se : shipped.entries)
            if (se.id == e.id) e.data = se.data;
      auto per_rope = [](uint32_t id) { return id >= 900 && id <= 911; };
      a->models.erase(std::remove_if(a->models.begin(), a->models.end(), [&](const ArenaModel& m) { return per_rope(m.id); }),
                      a->models.end());
      a->entries.erase(std::remove_if(a->entries.begin(), a->entries.end(), [&](const PachEntry& e) { return per_rope(e.id); }),
                       a->entries.end());
    }
  }
  editor::FromManifest(manifest);
  if (area < 0) {  // the lighting is in the material colours: out again (it applies at save)
    const editor::Lighting& l = editor::Light();
    if (!l.Default()) {
      float inv[3];
      for (int k = 0; k < 3; ++k) inv[k] = l.color[k] * l.strength > 1e-4f ? 1.0f / (l.color[k] * l.strength) : 1.0f;
      for (auto& am : a->models) {
        if (am.id == 952 || (am.id >= 956 && am.id <= 967)) continue;
        for (auto& s : am.model.meshes)
          for (auto& p : s.params)
            if (p.type == 0x0d && p.value.size() >= 16 && (p.name == "g_f4MatAmbCol" || p.name == "g_f4MatDifCol"))
              for (int k = 0; k < 3; ++k) PutBeF(&p.value[4 * k], BeF(&p.value[4 * k]) * inv[k]);
        am.changed = true;
      }
    }
  }
  g_proj.arena = area >= 0 ? -1 : host;
  g_proj.backstage = area;
  if (area >= 0) editor::SetArea(kAreas[area].ids, kAreas[area].spot), g_area = area;
  else editor::SetArea({}), g_sel = host;
  g_proj.edited = std::move(a);
  g_proj.fbx.clear();
  editor::SetArena(g_proj.edited.get(), g_proj.name);
  return true;
}

// ---- the mod type's name/author/version and the VS screen (shared tabs)

// A title card logo: at most 512 x 256, its shape kept (the game fits it into each of the card's logo pictures).
Image CardLogo(const Image& img) {
  const float k = std::min({1.0f, 512.0f / float(std::max(1, img.w)), 256.0f / float(std::max(1, img.h))});
  return k < 1.0f ? Resize(img, std::max(4, int(img.w * k)), std::max(4, int(img.h * k))) : img;
}

void DetailsFields() {
  TextField("Name", g_proj.name, sizeof g_proj.name, g_proj.backstage >= 0 || g_page == PageId::kBackstage ? "the mod's name" : "the arena's name on the select page");
  TextField("Author (optional)", g_proj.author, sizeof g_proj.author);
  TextField("Version (optional)", g_proj.version, sizeof g_proj.version, "1.0");
  if (g_proj.backstage < 0 && g_page != PageId::kBackstage)
    TextField("City (optional)", g_proj.city, sizeof g_proj.city, "the entrance title card's city, e.g. PITTSBURGH, PA");
  if (g_proj.backstage < 0 && g_page != PageId::kBackstage)
    TextField("Crowd area (optional)", g_proj.crowd_area, sizeof g_proj.crowd_area,
              "crowd fighting beyond the barricade (Extreme Rules): x0,z0,x1,z1 in the stage's x / z, e.g. 75,-135,190,-40 - "
              "a player at the barricade there holds Y to climb in");
}

struct VsTexture {
  std::string name;
  int w = 0, h = 0;
  Picture original, mine;
};
int g_vs_slot = -1;
int g_vs_rev = 0, g_vs_loaded_rev = -1;  // (the own theme changed: its pictures read again)
std::vector<VsTexture> g_vs;

// ---- an own VS theme (vs/theme.pac)

// Its pictures: entry VSTI, PACH child 0x1B59 (a texture bundle).
bool ThemePictures(const Bytes& pac, std::vector<BundleTexture>& out) {
  Epac e;
  if (!EpacRead(pac, e)) return false;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != "VSTI") continue;
      std::vector<PachEntry> kids;
      if (!PachRead(Unpack(en.data), kids)) return false;
      for (const auto& k : kids)
        if (k.id == 0x1B59) return BundleRead(Unpack(k.data), out);
    }
  return false;
}

// The theme with the replaced pictures (by name, fitted to each picture's size and format) in it.
Bytes BakeTheme(const Bytes& pac, const std::map<std::string, Image>& mine) {
  Epac e;
  if (mine.empty() || !EpacRead(pac, e)) return pac;
  for (auto& g : e.groups)
    for (auto& en : g.entries) {
      if (en.name != "VSTI") continue;
      const bool packed = IsBpe(en.data);
      std::vector<PachEntry> kids;
      if (!PachRead(Unpack(en.data), kids)) return pac;
      for (auto& k : kids) {
        if (k.id != 0x1B59) continue;
        const bool kpacked = IsBpe(k.data);
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(k.data), texs)) return pac;
        for (auto& t : texs) {
          const auto it = mine.find(t.name);
          DdsInfo info;
          if (it == mine.end() || !DdsInfoOf(t.data, info)) continue;
          t.data = DdsEncode(Resize(it->second, info.w, info.h),
                             info.format == DxtFormat::kDxt1 ? DxtFormat::kDxt1 : DxtFormat::kDxt5, info.mips > 1);
        }
        const Bytes raw = BundleWrite(texs);
        k.data = kpacked ? BpeEncode(raw) : raw;
      }
      const Bytes p = PachWrite(kids);
      en.data = packed ? BpeEncode(p) : p;
    }
  return EpacWrite(e);
}

// One entry of a theme pac replaced by another's (the font: VSTF).
Bytes ThemeWithEntry(const Bytes& pac, const Bytes& from, const char* name) {
  Epac e, f;
  if (!EpacRead(pac, e) || !EpacRead(from, f)) return pac;
  const EpacEntry* src = nullptr;
  for (const auto& g : f.groups)
    for (const auto& en : g.entries)
      if (en.name == name) src = &en;
  if (!src) return pac;
  for (auto& g : e.groups)
    for (auto& en : g.entries)
      if (en.name == name) en.data = src->data;
  return EpacWrite(e);
}

// The theme picker: VS themes of the player's 2011 and of their discs (vs_port.py themes), previews, a pick.
struct ThemePick {
  bool open = false;
  bool font = false;      // picking a font (the theme's VSTF) instead of a whole theme
  int source = -1;        // -1 the 2011 game, else an index into Sources()
  std::string asked;      // the source the list is for
  Json list;
  std::string error;
  std::map<std::string, Picture> previews;
} g_tp;

struct ThemeSource {
  std::string game, label;
  std::wstring path;
};
std::vector<ThemeSource> ThemeSources() {
  static const std::pair<const char*, const char*> kIds[] = {
      {"54510844", "svr2010"}, {"54510826", "svr2009"}, {"5451080B", "svr2008"}};
  std::vector<ThemeSource> out;
  for (const auto& d : Discs())
    for (const auto& [id, game] : kIds)
      if (d.id == id) out.push_back({game, d.game, d.path});
  return out;
}

fs::path ThemeWork() { return fs::path(g_game) / L"Mods" / L".convert" / L"vs"; }

void AskThemes() {
  const auto sources = ThemeSources();
  const bool own = g_tp.source < 0 || g_tp.source >= int(sources.size());
  const std::string game = own ? "svr2011" : sources[size_t(g_tp.source)].game;
  const std::wstring src = own ? std::wstring(g_game) : sources[size_t(g_tp.source)].path;
  const std::string want = game + "|" + Utf8(src);
  if (g_tp.asked == want || Busy()) return;
  g_tp.asked = want;
  g_tp.list = Json();
  g_tp.error.clear();
  g_tp.previews.clear();
  std::error_code ec;
  fs::create_directories(ThemeWork() / L"previews", ec);
  RunToolJson(L"vs_port.py", {L"themes", L"--game", Wide(game), L"--src", src, L"--pac11", fs::path(g_game).wstring(),
                              L"--out", (ThemeWork() / L"previews").wstring()},
              [want](int code, const std::string& out) {
                if (g_tp.asked != want) return;
                Json j;
                if (code == 0 && ParseJson(out, j) && j.type == Json::Type::kArray) g_tp.list = std::move(j);
                else g_tp.error = "the converter couldn't list the themes (exit " + std::to_string(code) + "): see the log";
              });
}

void PickTheme(const std::string& game, const std::wstring& src, const std::string& key, const std::string& label) {
  const fs::path out = ThemeWork() / Wide("picked_" + game + "_" + key + ".pac");
  const bool font = g_tp.font;
  RunToolJson(L"vs_port.py", {L"theme", L"--game", Wide(game), L"--src", src, L"--pac11", fs::path(g_game).wstring(),
                              L"--theme", Wide(key), L"--out", out.wstring()},
              [out, label, font](int code, const std::string&) {
                Bytes d;
                if (code != 0 || !ReadFile(PathStr(out), d)) {
                  Status("The theme couldn't be made: see the log.");
                  return;
                }
                if (font && !g_proj.theme_pac.empty()) {
                  g_proj.theme_pac = ThemeWithEntry(g_proj.theme_pac, d, "VSTF");
                  g_proj.theme_font = label;
                  Status("VS screen: the font of " + label + ".");
                } else {
                  g_proj.theme_pac = std::move(d);
                  g_proj.theme_label = label;
                  g_proj.theme_font.clear();
                  g_proj.vs.clear();  // (pictures replaced in the theme it had)
                  Status("VS screen: " + label + ".");
                }
                ++g_vs_rev;
                Touch();
                Log("VS screen: " + std::string(font ? "font of " : "") + label);
              });
  g_tp.open = false;
}

// Test aid (--vs-test "<step>;<step>..."): the VS screen tab's steps on the opened mod, as its buttons do -
// theme=<game>,<src>,<n>[,<label>] (Pick a theme), font=<game>,<src>,<n>[,<label>] (Font from another theme), replace=<picture>,<png>
// (Replace...), version=<v>, city=<text>, crowd_area=<x0,z0,x1,z1>, card=<picture>, save=<file.svrmod> (the mod made, then the Mod Maker closes; the log beside it).
std::vector<std::string> g_vs_test;
size_t g_vs_test_at = 0;
int g_vs_test_wait = 0;  // frames (an opened mod settles first)

void VsTestStep() {
  if (g_vs_test_at >= g_vs_test.size() || Busy() || ++g_vs_test_wait < 30) return;
  const std::string step = g_vs_test[g_vs_test_at++];
  g_vs_test_wait = 0;
  const size_t eq = step.find('=');
  const std::string what = step.substr(0, eq), arg = eq == std::string::npos ? "" : step.substr(eq + 1);
  std::vector<std::string> p;
  for (size_t at = 0; at <= arg.size();) {
    const size_t c = arg.find(',', at);
    p.push_back(arg.substr(at, c == std::string::npos ? std::string::npos : c - at));
    if (c == std::string::npos) break;
    at = c + 1;
  }
  Log("vs test: " + step);
  if ((what == "theme" || what == "font") && (p.size() == 3 || p.size() == 4)) {
    g_tp.font = what == "font";
    PickTheme(p[0], Wide(p[1]), p[2], p.size() == 4 ? p[3] : p[0] + " " + p[2]);
  } else if (what == "replace" && p.size() == 2) {
    LoadVsTheme(g_proj.arena);
    Image img;
    for (const auto& t : g_vs)
      if (t.name == p[0] && LoadPicture(Wide(p[1]), img)) g_proj.vs[t.name] = Resize(img, t.w, t.h), Touch();
    if (!g_proj.vs.count(p[0])) Log("vs test: no picture " + p[0]);
  } else if (what == "card") {
    Image img;
    if (LoadPicture(Wide(arg), img)) g_proj.card.Set(CardLogo(img)), Touch();
    else Log("vs test: no picture " + arg);
  } else if (what == "city") {
    std::snprintf(g_proj.city, sizeof g_proj.city, "%s", arg.c_str());
    Touch();
  } else if (what == "crowd_area") {
    std::snprintf(g_proj.crowd_area, sizeof g_proj.crowd_area, "%s", arg.c_str());
    Touch();
  } else if (what == "version") {
    std::snprintf(g_proj.version, sizeof g_proj.version, "%s", arg.c_str());
    Touch();
  } else if (what == "save") {
    SaveModTo(Wide(arg), [arg](bool ok) {
      Log(std::string("vs test: ") + (ok ? "saved " : "save FAILED ") + arg);
      WriteLogTo(Wide(arg) + L".log");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    });
  }
}

void ThemePicker() {
  if (!g_tp.open) return;
  ImGui::SeparatorText(g_tp.font ? "Pick the theme whose font to use" : "Pick a VS screen theme");
  const auto sources = ThemeSources();
  std::string cur = g_tp.source < 0 ? "SvR 2011 (your game)" : sources[size_t(g_tp.source)].label;
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("From", cur.c_str())) {
    if (ImGui::Selectable("SvR 2011 (your game)", g_tp.source < 0)) g_tp.source = -1;
    for (int i = 0; i < int(sources.size()); ++i)
      if (ImGui::Selectable((sources[size_t(i)].label + "##" + std::to_string(i)).c_str(), g_tp.source == i)) g_tp.source = i;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Close")) g_tp.open = false;
  if (sources.empty()) ImGui::TextDisabled("Your SvR 2008 - 2010 discs show here once they're in My game discs.");
  AskThemes();
  if (!g_tp.error.empty()) ImGui::TextColored(kWarn, "%s", g_tp.error.c_str());
  if (g_tp.list.type != Json::Type::kArray) {
    ImGui::TextDisabled(Busy() ? "Reading the themes ..." : "");
    return;
  }
  const bool own = g_tp.source < 0 || g_tp.source >= int(sources.size());
  const std::string game = own ? "svr2011" : sources[size_t(g_tp.source)].game;
  const std::wstring src = own ? std::wstring(g_game) : sources[size_t(g_tp.source)].path;
  const std::string from = own ? "SvR 2011" : sources[size_t(g_tp.source)].label;
  const float w = 240 * g_scale, h = w / 2;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (w + 10 * g_scale)));
  int i = 0;
  for (const auto& it : g_tp.list.items) {
    const std::string key = it["key"].Str(), name = it["name"].Str();
    if (i++ % cols) ImGui::SameLine();
    ImGui::PushID(key.c_str());
    ImGui::BeginGroup();
    auto& pic = g_tp.previews[key];
    if (pic.Empty() && !it["preview"].Str().empty()) {
      Image img;
      if (LoadPicture(Wide(it["preview"].Str()), img)) pic.Set(std::move(img));
    }
    pic.Draw(w, h);
    const bool research = it["status"].Str() == "research";
    ImGui::BeginDisabled(research || Busy());
    if (ImGui::Button((name + "##pick").c_str(), ImVec2(w, 0))) PickTheme(game, src, key, from + " " + name);
    ImGui::EndDisabled();
    if (research && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", it["reason"].Str().c_str());
    ImGui::EndGroup();
    ImGui::PopID();
  }
}

// The VS screen (the match screen behind the two Superstars) is a theme per
// arena: menu/MatchHD.pac group M<nn>I (nn = the arena's bg number), DXT5
// textures. A custom arena shows its slot's theme; the mod can replace any
// of its textures (vs/<name>.dds, same size).
void LoadVsTheme(int slot) {
  if (slot == g_vs_slot && g_vs_rev == g_vs_loaded_rev) return;
  g_vs.clear();
  g_vs_slot = slot;
  g_vs_loaded_rev = g_vs_rev;
  if (!g_proj.theme_pac.empty()) {  // (its own theme's pictures)
    std::vector<BundleTexture> texs;
    if (ThemePictures(g_proj.theme_pac, texs))
      for (const auto& t : texs) {
        VsTexture v;
        v.name = t.name;
        Image img;
        if (!DdsDecode(t.data, img) || img.w < 64 || img.h < 32) continue;
        v.w = img.w, v.h = img.h;
        v.original.Set(std::move(img));
        g_vs.push_back(std::move(v));
      }
    return;
  }
  if (slot < 0 || g_game.empty()) return;
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"menu" / L"MatchHD.pac"), d) || !EpacRead(d, e)) return;
  char group[8];
  std::snprintf(group, sizeof group, "M%02dI", g_arenas[slot].number);
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != group) continue;
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(pe.data), texs)) continue;
        for (const auto& t : texs) {
          VsTexture v;
          v.name = t.name;
          Image img;
          if (!DdsDecode(t.data, img) || img.w < 64 || img.h < 32) continue;  // (tiny: dots, glows)
          v.w = img.w, v.h = img.h;
          v.original.Set(std::move(img));
          g_vs.push_back(std::move(v));
        }
      }
    }
}

void VsTab() {
  const int slot = g_proj.arena >= 0 ? g_proj.arena : g_sel;
  LoadVsTheme(slot);
  if (g_proj.theme_pac.empty())
    ImGui::TextWrapped("The screen behind the two Superstars before the match: the %s theme, which your arena uses "
                       "because it plays in that arena's place - or pick any theme of your games for it. Replace any "
                       "of its pictures (they are fitted to the same size).", g_arenas[slot].name);
  else
    ImGui::TextWrapped("The screen behind the two Superstars before the match: %s%s%s, your arena's own. Replace any "
                       "of its pictures (they are fitted to the same size).", g_proj.theme_label.c_str(),
                       g_proj.theme_font.empty() ? "" : ", with the font of ", g_proj.theme_font.c_str());
  if (ImGui::Button("Pick a theme...")) g_tp.open = true, g_tp.font = false;
  ImGui::SameLine();
  ImGui::BeginDisabled(g_proj.theme_pac.empty());
  if (ImGui::Button("Font from another theme...")) g_tp.open = true, g_tp.font = true;
  ImGui::SameLine();
  if (ImGui::Button("Back to the slot's theme")) {
    g_proj.theme_pac.clear(), g_proj.theme_label.clear(), g_proj.theme_font.clear(), g_proj.vs.clear();
    ++g_vs_rev;
    Touch();
  }
  ImGui::EndDisabled();
  ThemePicker();
  ImGui::Spacing();
  if (g_vs.empty()) {
    ImGui::TextDisabled("No VS screen theme found for this arena.");
    return;
  }
  const float col = 300 * g_scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (col + 10 * g_scale)));
  int i = 0;
  for (auto& t : g_vs) {
    if (i++ % cols) ImGui::SameLine();
    ImGui::PushID(t.name.c_str());
    ImGui::BeginGroup();
    const auto mine = g_proj.vs.find(t.name);
    if (mine != g_proj.vs.end() && t.mine.Empty()) t.mine.Set(mine->second);
    if (mine == g_proj.vs.end() && !t.mine.Empty()) t.mine.Clear();
    const float h = std::min(col * t.h / t.w, 200 * g_scale);
    (t.mine.Empty() ? t.original : t.mine).Draw(col, h);
    ImGui::Text("%s  %dx%d%s", t.name.c_str(), t.w, t.h, t.mine.Empty() ? "" : "  (yours)");
    if (ImGui::Button("Replace...")) {
      const std::wstring f = PickFile(false, L"A picture for this part of the VS screen", kPictureFilter, 1);
      Image img;
      if (!f.empty() && LoadPicture(f, img)) {
        g_proj.vs[t.name] = Resize(img, t.w, t.h);
        t.mine.Clear();
        Touch();
        Log("VS screen: " + t.name + " from " + Utf8(f));
      }
    }
    if (!t.mine.Empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Original")) g_proj.vs.erase(t.name), Touch();
    }
    ImGui::Dummy(ImVec2(col, 4));
    ImGui::EndGroup();
    ImGui::PopID();
  }
}

// ---- the Start tab: the grid of the game's arenas

void ArenaGrid() {
  ImGui::BeginChild("grid", ImVec2(-320 * g_scale, 0), false);
  ImGui::TextDisabled("The game's arenas, read from the game folder and never changed. Pick the one to start from.");
  const float avail = ImGui::GetContentRegionAvail().x;
  const int cols = std::clamp(int(avail / (190 * g_scale)), 2, 5);
  const ImGuiStyle& st = ImGui::GetStyle();
  const float tw = (avail - st.ItemSpacing.x * (cols - 1)) / cols - 1;
  const float iw = tw - st.FramePadding.x * 2, ih = iw * 0.5f;
  for (int i = 0; i < 20; ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(i);
    const bool sel = g_sel == i && g_proj.backstage < 0;
    ImGui::BeginGroup();
    if (!g_arenas[i].tile.Empty()) {
      if (ImGui::ImageButton("t", g_arenas[i].tile.Id(), ImVec2(iw, ih))) g_sel = i;
    } else if (ImGui::Button(g_arenas[i].name, ImVec2(tw, ih + st.FramePadding.y * 2))) {
      g_sel = i;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) OpenEditor(i);
    {  // the name, clipped to the tile
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::PushClipRect(p, ImVec2(p.x + tw, p.y + ImGui::GetTextLineHeight()), true);
      ImGui::TextUnformatted(g_arenas[i].name);
      ImGui::PopClipRect();
    }
    ImGui::EndGroup();
    if (sel) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), IM_COL32(230, 25, 50, 255),
                                          4, 0, 3 * g_scale);
    }
    if (g_proj.arena == i) {
      const ImVec2 a = ImGui::GetItemRectMin();
      ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + 6, a.y + 4), IM_COL32(255, 255, 255, 255), "YOURS");
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void StartSide() {
  ImGui::BeginChild("side", ImVec2(0, 0), true);
  const ArenaInfo& a = g_arenas[g_sel];
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
  ImGui::TextUnformatted(a.name);
  ImGui::PopFont();
  ImGui::TextDisabled("pac/bg/bg%02d.pac", a.number);
  const float bw = ImGui::GetContentRegionAvail().x;
  g_arenas[g_sel].tile.Draw(bw, bw * 0.5f);
  ImGui::Spacing();
  ImGui::BeginDisabled(Busy());
  PushAccent();
  if (ImGui::Button("Open in the 3D editor", ImVec2(-1, 0))) OpenEditor(g_sel);
  PopAccent();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move, add and retexture objects, the Ring Kit, lighting (double-click a tile does the same).");
  if (ImGui::Button("Start an empty arena", ImVec2(-1, 0))) NewArena(g_sel);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Only the ring, the floor and the ringside parts: build the rest.\n"
                      "It plays in this arena's place (its room and VS screen style).");
  ImGui::Spacing();
  ImGui::TextDisabled("Blender");
  if (ImGui::Button("Export to Blender...", ImVec2(-1, 0))) ExportArena(g_sel);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("arena.fbx + textures as PNG + arena.json into a folder you choose.");
  if (ImGui::Button("Import from Blender...", ImVec2(-1, 0))) ImportArena(g_sel);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("The edited FBX back onto this arena (then edit on in 3D, or save).");
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::TextDisabled("Your arena");
  if (g_proj.arena >= 0)
    ImGui::TextWrapped("From %s%s%s.", g_arenas[g_proj.arena].name, g_proj.fbx.empty() ? "" : ", edited in Blender",
                       g_proj.edited ? (g_proj.fbx.empty() ? ", open in the 3D editor" : "") : " (as the game has it)");
  else
    ImGui::TextWrapped("None yet: open an arena in the 3D editor, start an empty one, or import from Blender.");
  ImGui::EndChild();
}

void DetailsTab() {
  DetailsFields();
  Hint("The name is what the arena select page shows under the banner. Mods are kept apart by the id made from "
       "the name (letters and digits): two mods with the same name replace each other.");
  ImGui::Spacing();
  ImGui::TextDisabled("Banner (the select page's picture, 256 x 128)");
  if (ImGui::Button("Banner picture...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Banner picture (shown at 256 x 128)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      g_proj.banner.Set(Resize(img, 256, 128));
      Touch();
      Log("Banner set from " + Utf8(f));
    }
  }
  if (!g_proj.banner.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x##banner")) g_proj.banner.Clear(), Touch();
  }
  if (g_proj.banner.Empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("none: a dark banner with the name is used");
  } else {
    g_proj.banner.Draw(256 * g_scale, 128 * g_scale);
  }
  ImGui::Spacing();
  ImGui::TextDisabled("Loading screen (shown while the match loads, 1024 x 512)");
  if (ImGui::Button("Loading screen picture...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Loading screen picture (shown at 1024 x 512)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      g_proj.load.Set(Resize(img, 1024, 512));
      Touch();
      Log("Loading screen set from " + Utf8(f));
    }
  }
  if (!g_proj.load.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x##load")) g_proj.load.Clear(), Touch();
  }
  if (g_proj.load.Empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("none: the arena it plays in place of keeps its loading screen");
  } else {
    g_proj.load.Draw(512 * g_scale, 256 * g_scale);
  }
  ImGui::Spacing();
  ImGui::TextDisabled("Entrance title card logo (the show's logo on the card as the entrances start; transparent PNG)");
  if (ImGui::Button("Card logo picture...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Title card logo (a transparent picture)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      g_proj.card.Set(CardLogo(img));
      Touch();
      Log("Title card logo set from " + Utf8(f));
    }
  }
  if (!g_proj.card.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x##card")) g_proj.card.Clear(), Touch();
  }
  if (g_proj.card.Empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("none: the card shows the logo of the arena it plays in place of");
  } else {
    g_proj.card.Draw(float(g_proj.card.image.w) * g_scale, float(g_proj.card.image.h) * g_scale);
  }
}

// Backstage: the area-of-its-own settings (row=, gimmick=, box=, camera=, camera_height=).
void OwnAreaFields() {
  ImGui::Checkbox("An area of its own", &g_proj.own_area);
  Hint("Instead of replacing the room, the mod gets a row of its own in ONE ON ONE -> BACKSTAGE (after the room's "
       "row) and plays only in matches started from it. The room stays as the game has it.");
  if (!g_proj.own_area) return;
  ImGui::Indent();
  TextField("Row name", g_proj.row, sizeof g_proj.row, "e.g. MY GARAGE (the menu row)");
  ImGui::Checkbox("Fight box", &g_proj.has_box);
  Hint("Where the fight is (the camera's target, where the wrestlers are kept, where the computer goes): centre x and "
       "z and the half sizes, in game units (1 = 10 cm). Without one the room's box is used.");
  if (g_proj.has_box) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::InputFloat4("x, z, half x, half z", g_proj.box, "%.1f")) Touch();
  }
  ImGui::SetNextItemWidth(160 * g_scale);
  if (ImGui::InputFloat("Camera distance cap (0 = the game's)", &g_proj.camera, 0, 0, "%.1f")) Touch();
  Hint("Caps how far the match camera backs away (game units). A small area wants a cap, or the camera stands among "
       "the objects.");
  ImGui::SetNextItemWidth(160 * g_scale);
  if (ImGui::InputFloat("Camera height, at least (0 = the game's)", &g_proj.camera_height, 0, 0, "%.1f")) Touch();
  Hint("The camera's eye is kept at least this high (game units): it looks over the objects round the area.");
  ImGui::TextDisabled("Objects (optional)");
  TextField("Gimmick name (4 letters)", g_proj.gimmick, sizeof g_proj.gimmick, "e.g. GMGB");
  if (FileRow("Gimmick pac...", g_proj.gimmick_pac, "none: the room's own cars, crates and weapons", kPacFilter, 1,
              "A small pac with only GMGB/<name>: the cars, props and hot spots of the area, played instead of the "
              "room's in that row's matches (tools/svr08_gimmick.py makes one from SvR 2008 data)."))
    Touch();
  ImGui::Unindent();
}

// ---------------------------------------------------------------- public

void SetVsTest(const std::string& spec) {
  g_vs_test.clear();
  for (size_t at = 0; at <= spec.size();) {
    const size_t c = spec.find(';', at);
    g_vs_test.push_back(spec.substr(at, c == std::string::npos ? std::string::npos : c - at));
    if (c == std::string::npos) break;
    at = c + 1;
  }
}

void Tick() {
  VsTestStep();
  std::lock_guard lock(g_pending_mutex);
  if (!g_pending) {
    if (g_test_state == 2 && !Busy()) {
      WriteLogTo(g_test_save + L".log");  // (the test scripts read it)
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      g_test_state = 3;
    }
    return;
  }
  g_proj.edited = std::move(g_pending);
  g_proj.fbx = g_pending_fbx;
  editor::SetArena(g_proj.edited.get(), g_proj.name);
  if (g_pending_to_editor) GoTo(PageId::kEditor);
  g_pending_to_editor = false;
  if (g_pending_no_crowd) editor::Light().crowd = false;
  g_pending_no_crowd = false;
  Touch();
  if (!g_test_save.empty() && g_test_state == 0) {
    g_test_state = 1;
    if (g_test_lib >= 0) editor::TestLibrary(g_test_lib);
    if (!editor::RunObjectTests()) editor::TestEdit();
    // and the VS screen: the theme's biggest picture as a red / yellow checker
    if (g_proj.backstage < 0) LoadVsTheme(g_proj.arena);
    const VsTexture* big = nullptr;
    for (const auto& t : g_vs)
      if (!big || t.w * t.h > big->w * big->h) big = &t;
    if (big) {
      Image img;
      img.w = big->w, img.h = big->h;
      img.rgba.resize(size_t(img.w) * img.h * 4);
      for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
          uint8_t* q = &img.rgba[(size_t(y) * img.w + x) * 4];
          const bool on = ((x / 64) + (y / 64)) & 1;
          q[0] = on ? 230 : 250, q[1] = on ? 20 : 210, q[2] = on ? 30 : 0, q[3] = 255;
        }
      g_proj.vs[big->name] = img;
      Log("test: VS screen " + big->name + " replaced");
    }
    std::snprintf(g_proj.name, sizeof g_proj.name, "Test Edit");
    BuildJob job;
    if (PrepareBuild(job)) {
      const std::string out = Utf8(g_test_save);
      RunInBackground([job, out]() mutable {
        std::vector<ZipEntry> files;
        if (FinishBuild(job, files) && WriteFile(out, ZipWrite(files))) Log("test: saved " + out);
        else Log("test: build failed");
        g_test_state = 2;
      });
    }
  }
}

void OpenEditor(int i) {
  LeaveBackstage();
  if (g_proj.arena == i && g_proj.edited) {
    GoTo(PageId::kEditor);
    return;
  }
  StartProject(i);
  LoadInBackground(ArenaPath(i), nullptr, true, false);
}

void StartNew(int tile) {
  g_sel = tile;
  NewArena(tile);
}

void StartBackstage(int area) {
  if (g_proj.backstage == area && g_proj.edited) {
    GoTo(PageId::kEditor);
    return;
  }
  g_area = area;
  g_proj.backstage = area;
  g_proj.arena = -1;
  g_proj.edited.reset();
  g_proj.fbx.clear();
  editor::SetArena(nullptr, "");
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s (custom)", kAreas[area].name);
  editor::SetArea(kAreas[area].ids, kAreas[area].spot);
  Touch();
  LoadInBackground(PathStr(fs::path(g_game) / L"pac" / L"bg" / L"bg78.pac"), nullptr, true, false);
}

void SetArenaFromPac(const Bytes& pac, int host, const std::string& name, const std::string& source, bool to_editor,
                     const std::wstring& picture) {
  LeaveBackstage();
  g_proj = Project();
  if (Image img; !picture.empty() && LoadPicture(picture, img)) {  // (the disc's arena picture: banner + loading screen)
    g_proj.load.Set(Resize(img, 1024, 512));
    Image half = img;  // (halved step by step down to the banner: one bilinear step from 1024 wide would alias)
    while (half.w >= 512 && half.h >= 256) half = Resize(half, half.w / 2, half.h / 2);
    g_proj.banner.Set(Resize(half, 256, 128));
    Log("Banner and loading picture from " + Utf8(picture));
  }
  editor::Ring() = RingSpec();
  editor::Light() = editor::Lighting();
  editor::SetArena(nullptr, "");
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s", name.c_str());
  g_proj.source = source;
  g_sel = host;
  g_proj.arena = host;
  auto a = std::make_unique<Arena>();
  std::string err;
  if (!a->LoadData(pac, &err)) { Status("The converted arena could not be loaded: " + err); return; }
  Arena shipped;  // the budget is the host arena's shipped size
  if (shipped.Load(ArenaPath(host))) a->original_file = shipped.original_file, a->original_unpacked = shipped.original_unpacked;
  {
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx.clear();
    g_pending_to_editor = to_editor;
  }
  Status(to_editor ? "The converted arena is open in the 3D editor; the Arena page has its name, banner and the Save."
                   : "The converted arena is your arena now.");
}

void SetBackstageFromPac(const Bytes& pac, int area, const std::string& name, const std::wstring& gimmick_pac,
                         const std::string& gimmick) {
  g_proj = Project();
  editor::Ring() = RingSpec();
  editor::Light() = editor::Lighting();
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s", name.c_str());
  g_area = area;
  std::string err;
  if (!LoadEditedArena(pac, -1, area, "", &err)) { Status("The converted backstage file could not be loaded: " + err); return; }
  if (!gimmick_pac.empty()) {
    g_proj.own_area = true;
    std::snprintf(g_proj.row, sizeof g_proj.row, "%s", Upper(name).substr(0, 31).c_str());
    std::snprintf(g_proj.gimmick, sizeof g_proj.gimmick, "%s", gimmick.substr(0, 4).c_str());
    g_proj.gimmick_pac = gimmick_pac;
  }
  Touch();
  GoTo(PageId::kBackstage);
  Status("The converted area is on the Backstage tab (and in the 3D editor).");
}

void SetTestSave(const std::wstring& f, int lib) {
  g_test_save = f;
  g_test_lib = lib;
}

void TestInGame() { InstallMod(true); }

// A saved mod back into the editor (arena or backstage).
void OpenModFile(const std::wstring& f) { ProjectOpen(f); }

// ---- the project / mod hooks (arena and backstage)

bool ReadModOrProject(const ProjectIn& in, bool mod) {
  const std::string type = in.Get("type");
  const bool backstage = type == "backstage";
  const ZipEntry* pac = in.Find("arena.pac");
  int host = -1, area = -1;
  if (backstage) {
    area = in.GetInt("area", -1);
    if (area < 0 || area > 6) { Status("The mod does not say which backstage area it rebuilds (area=)."); return false; }
  } else {
    for (int i = 0; i < 20; ++i)
      if (in.Get("base") == g_arenas[i].banner) host = i;
    if (host < 0 && !mod) host = in.GetInt("tile", -1);
    if (host < 0) { Status("The mod does not say which arena it is built on (base=)."); return false; }
  }
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s", in.Get("name").c_str());
  g_proj.source = in.Get("source");
  g_proj.id = mod ? in.Get("id") : in.Get("mod_id");  // (a mod's own id; a project: the mod it came from)
  std::snprintf(g_proj.author, sizeof g_proj.author, "%s", in.Get("author").c_str());
  std::snprintf(g_proj.version, sizeof g_proj.version, "%s", in.Get("version").c_str());
  std::snprintf(g_proj.city, sizeof g_proj.city, "%s", in.Get("city").c_str());
  std::snprintf(g_proj.crowd_area, sizeof g_proj.crowd_area, "%s", in.Get("crowd_area").c_str());
  if (!g_proj.version[0]) std::snprintf(g_proj.version, sizeof g_proj.version, "1.0");
  g_proj.vs.clear();
  for (const auto& e : in.files)
    if (e.name.rfind("vs/", 0) == 0 && e.name.size() > 7) {
      Image img;
      if (DdsDecode(e.data, img)) g_proj.vs[e.name.substr(3, e.name.size() - 7)] = img;
    }
  g_proj.theme_pac.clear();
  if (const ZipEntry* tp = in.Find("vs/theme.pac")) g_proj.theme_pac = tp->data;
  g_proj.theme_label = in.Get("vs_theme");
  g_proj.theme_font = in.Get("vs_font");
  ++g_vs_rev;
  if (const ZipEntry* b = in.Find("banner.dds")) {
    Image img;
    if (DdsDecode(b->data, img)) g_proj.banner.Set(std::move(img));
  }
  if (const ZipEntry* l = in.Find("load.dds")) {
    Image img;
    if (DdsDecode(l->data, img)) g_proj.load.Set(std::move(img));
  }
  if (const ZipEntry* l = in.Find("card.dds")) {
    Image img;
    if (DdsDecode(l->data, img)) g_proj.card.Set(std::move(img));
  }
  g_proj.fbx = in.Get("fbx");
  for (int i = 0; !mod && i < 999; ++i) {  // (Simple arena's crowd signs: projects only)
    Image img;
    if (!in.Png(("sign" + std::to_string(i)).c_str(), img)) break;
    Picture pic;
    pic.Set(std::move(img));
    g_proj.signs.push_back({in.Get(("sign_name" + std::to_string(i)).c_str()), std::move(pic)});
  }
  if (!mod && !backstage && in.GetInt("simple", 0)) g_simple = true, g_simple_step = std::clamp(in.GetInt("simple_step", 0), 0, int(kSaCount) - 1);
  if (backstage) {
    std::snprintf(g_proj.row, sizeof g_proj.row, "%s", in.Get("row").c_str());
    std::snprintf(g_proj.gimmick, sizeof g_proj.gimmick, "%s", in.Get("gimmick").substr(0, 4).c_str());
    g_proj.own_area = g_proj.row[0] != 0;
    g_proj.has_box = std::sscanf(in.Get("box").c_str(), "%f,%f,%f,%f", &g_proj.box[0], &g_proj.box[1], &g_proj.box[2], &g_proj.box[3]) == 4;
    g_proj.camera = float(std::atof(in.Get("camera").c_str()));
    g_proj.camera_height = float(std::atof(in.Get("camera_height").c_str()));
    if (in.Find("gimmick.pac")) g_proj.gimmick_pac = in.Extract("gimmick.pac");
    else if (!mod) {
      const std::string from = in.Get("gimmick_pac.from");
      if (!from.empty() && fs::exists(Wide(from))) g_proj.gimmick_pac = Wide(from);
    }
  }
  if (!mod)  // rope pictures (project only: the mod has them baked in)
    for (int k = 0; k < 3; ++k) editor::Ring().ropes[k].texture = in.Get(("ring.tex" + std::to_string(k)).c_str());
  if (pac) {
    std::string err;
    if (!LoadEditedArena(pac->data, host, area, in.text, &err)) { Status("The arena in it could not be read: " + err); return false; }
    if (mod) g_proj.opened_pac = pac->data, g_proj.opened_edits = editor::EditCount();
    GoTo(PageId::kEditor);
  } else {  // (a project saved before any edit)
    g_proj.arena = backstage ? -1 : host;
    g_proj.backstage = area;
    g_sel = host >= 0 ? host : g_sel;
    if (area >= 0) g_area = area;
    editor::FromManifest(in.text);
  }
  return true;
}

void WriteProject(ProjectOut& out) {
  out.Key("name", g_proj.name);
  out.Key("author", g_proj.author);
  out.Key("version", g_proj.version);
  out.Key("city", g_proj.city);
  out.Key("crowd_area", g_proj.crowd_area);
  if (g_proj.backstage >= 0) out.Key("area", g_proj.backstage);
  else if (g_proj.arena >= 0) out.Key("base", g_arenas[g_proj.arena].banner), out.Key("tile", g_proj.arena);
  out.Key("fbx", g_proj.fbx);
  out.Key("source", g_proj.source);
  out.text += editor::ManifestLines();
  for (int k = 0; k < 3; ++k) out.Key("ring.tex" + std::to_string(k), editor::Ring().ropes[k].texture);
  if (g_proj.backstage >= 0 && g_proj.own_area) {
    char b[160];
    out.Key("row", g_proj.row);
    out.Key("gimmick", g_proj.gimmick);
    out.File("gimmick_pac", g_proj.gimmick_pac, "gimmick");
    if (g_proj.has_box) {
      std::snprintf(b, sizeof b, "%.1f,%.1f,%.1f,%.1f", g_proj.box[0], g_proj.box[1], g_proj.box[2], g_proj.box[3]);
      out.Key("box", b);
    }
    if (g_proj.camera > 0) std::snprintf(b, sizeof b, "%.1f", g_proj.camera), out.Key("camera", b);
    if (g_proj.camera_height > 0) std::snprintf(b, sizeof b, "%.1f", g_proj.camera_height), out.Key("camera_height", b);
  }
  if (!out.keys_only) {  // (the pictures: files, not keys)
    if (!g_proj.banner.Empty()) out.files.push_back({"banner.dds", DdsEncode(g_proj.banner.image, DxtFormat::kDxt5, false)});
    if (!g_proj.load.Empty()) out.files.push_back({"load.dds", DdsEncode(g_proj.load.image, DxtFormat::kDxt5, false)});
    if (!g_proj.card.Empty()) out.files.push_back({"card.dds", DdsEncode(g_proj.card.image, DxtFormat::kDxt5, false)});
    if (!g_proj.theme_pac.empty())  // (its own theme, the replaced pictures baked in)
      out.files.push_back({"vs/theme.pac", BakeTheme(g_proj.theme_pac, g_proj.vs)});
    else
      for (const auto& [name, img] : g_proj.vs) out.files.push_back({"vs/" + name + ".dds", DdsEncode(img, DxtFormat::kDxt5, false)});
  }
  if (!g_proj.id.empty()) out.Key("mod_id", g_proj.id);
  if (!g_proj.theme_pac.empty()) {
    out.Key("vs_theme", g_proj.theme_label);
    if (!g_proj.theme_font.empty()) out.Key("vs_font", g_proj.theme_font);
  }
  for (size_t i = 0; i < g_proj.signs.size(); ++i) {  // (Simple arena's crowd signs)
    out.Key("sign_name" + std::to_string(i), g_proj.signs[i].first);
    out.Png("sign" + std::to_string(i), g_proj.signs[i].second.image, "signs/" + std::to_string(i));
  }
  if (g_simple && g_proj.backstage < 0) out.Key("simple", 1), out.Key("simple_step", g_simple_step);
  if (g_proj.edited && !out.keys_only) {
    // the arena as edited (the kit / lighting apply at mod build, not here): a copy
    // taken now, compressed in the background
    auto copy = std::make_shared<Arena>(*g_proj.edited);
    GrowVisibility(*copy);  // (as at build)
    out.deferred.push_back([copy](std::vector<ZipEntry>& files) {
      Progress("Compressing the arena ...");
      std::string err;
      files.push_back({"arena.pac", copy->Save(&err)});
      if (!err.empty()) Log("project: " + err);
    });
  }
}

std::string StateText() {
  std::string s = std::string("name=") + g_proj.name + "\nauthor=" + g_proj.author + "\nversion=" + g_proj.version +
                  "\narena=" + std::to_string(g_proj.arena) + "\nbackstage=" + std::to_string(g_proj.backstage) +
                  "\nvs=" + std::to_string(g_proj.vs.size()) + "\nbanner=" + std::to_string(g_proj.banner.image.rgba.size()) +
                  "\nload=" + std::to_string(g_proj.load.image.rgba.size()) + "\nown=" + std::to_string(g_proj.own_area) +
                  "\nrow=" + g_proj.row + "\ngimmick=" + g_proj.gimmick + Utf8(g_proj.gimmick_pac) +
                  "\nbox=" + std::to_string(g_proj.has_box) + "\nsigns=" + std::to_string(g_proj.signs.size()) + "\n" + editor::ManifestLines();
  for (int k = 0; k < 3; ++k) s += editor::Ring().ropes[k].texture + ";";
  return s;
}

void Reset() {
  LeaveBackstage();
  g_proj = Project();
  editor::SetArena(nullptr, "");
  editor::Ring() = RingSpec();
  editor::Light() = editor::Lighting();
}

void ArenaProblems(std::vector<Problem>& p) {
  if (g_proj.backstage >= 0) {  // (the Arena page while a backstage area is open)
    p.push_back(Error("A backstage area is open here, not an arena.").With("Go to the Backstage tab", [] { GoTo(PageId::kBackstage); }));
    return;
  }
  const auto go = [](int step) { return [step] { if (g_simple) g_simple_step = step; }; };
  if (g_proj.arena < 0) p.push_back(Error("Pick an arena to start from.").With("Pick one...", go(kSaStart)).At(kSaStart));
  if (g_proj.arena >= 0 && g_simple && !g_proj.edited) p.push_back(Error("The arena is still loading - one moment.").At(kSaStart));
  if (!g_proj.name[0]) p.push_back(Error("Your arena has no name yet.").With("Type a name", go(kSaName)).At(kSaName));
  else if (std::strlen(g_proj.name) > 31)
    p.push_back(Warning("The name is long: the arena select screen only shows about 30 letters of it.")
                    .With("Shorten it to 30 letters", [] { g_proj.name[30] = 0; Touch(); }).At(kSaName));
  if (g_proj.arena >= 0 && g_proj.banner.Empty())
    p.push_back(Warning("No picture for the arena select screen: players see a dark tile.").With("Pick a picture...", go(kSaName)).At(kSaName));
  if (g_proj.arena >= 0 && !g_proj.edited && g_proj.fbx.empty())
    p.push_back(Warning("Nothing is changed yet: this would be a copy of the game's own arena.").At(kSaStart).Note());
  if (editor::Ring().VisibleRopes() < 3 && editor::Ring().VisibleRopes() > 0)
    p.push_back(Warning("The ring has fewer than three ropes: wrestlers can only use the ropes that are left.")
                    .With("Put all three ropes back", [] { editor::Ring() = RingSpec(); Touch(); }, "the ring's ropes are back")
                    .With("Keep it this way", [] {}, "kept").At(kSaRing));
  if (editor::Ring().VisibleRopes() == 0)
    p.push_back(Warning("The ring has no ropes: no rope moves and no rope breaks - a hardcore-style ring.")
                    .With("Put the ropes back", [] { editor::Ring() = RingSpec(); Touch(); }, "the ring's ropes are back").At(kSaRing));
  if (g_proj.signs.size() > 64)
    p.push_back(Warning("A lot of crowd signs: a match shows at most 16 at a time, picked from the game's and every sign pack's.").At(kSaSigns).Note());
  // limits and crash risks (D12): a model the game can't hold, an arena over its room
  editor::BudgetInfo b;
  if (g_proj.arena >= 0 && editor::Budget(b)) {
    if (b.biggest > 430 * 1024)
      p.push_back(Warning("One of the objects you added or changed is very big (" + Human(b.biggest) + "): arenas with objects this big "
                          "have crashed while loading.", "Hide parts of it or use a simpler model.")
                      .With("Open it in the 3D editor", [] { GoTo(PageId::kEditor); }).At(kSaLook));
    else if (b.biggest > 360 * 1024)
      p.push_back(Warning("One of the objects you added or changed is big (" + Human(b.biggest) + "): close to the size that has crashed "
                          "arenas while loading.").With("Open it in the 3D editor", [] { GoTo(PageId::kEditor); }).At(kSaLook));
    if (b.file > b.file_max || b.unpacked > b.unpacked_max)
      p.push_back(Warning("The arena is bigger than the game has room for: when it's saved, its biggest pictures are made smaller "
                          "until it fits (they look a little softer).").At(kSaLook).Note());
  }
}

void BackstageProblems(std::vector<Problem>& p) {
  if (g_proj.backstage < 0) p.push_back(Error("Pick a backstage area to start from."));
  if (!g_proj.name[0]) p.push_back(Error("Your area has no name yet."));
  if (g_proj.backstage >= 0 && !g_proj.edited) p.push_back(Error("The area is still loading - one moment."));
  if (g_proj.own_area) {
    if (!g_proj.row[0]) p.push_back(Error("A new area needs the name players pick it by in the BACKSTAGE menu."));
    if (g_proj.gimmick[0] && std::strlen(g_proj.gimmick) != 4)
      p.push_back(Error("The props' name must be exactly 4 letters, as in their file.").With("Clear it (no props)", [] { g_proj.gimmick[0] = 0; Touch(); }));
    if (g_proj.gimmick[0] && g_proj.gimmick_pac.empty())
      p.push_back(Error("A props name is set but no props file is picked.").With("Clear it (no props)", [] { g_proj.gimmick[0] = 0; Touch(); }));
    if (!g_proj.gimmick_pac.empty() && !g_proj.gimmick[0])
      p.push_back(Error("A props file is picked but its 4-letter name isn't set: the game won't load the props.")
                      .With("Leave the props out", [] { g_proj.gimmick_pac.clear(); Touch(); }));
    if (!g_proj.has_box)
      p.push_back(Warning("No fighting area is marked: the room's own is used - wrong if your area is somewhere else in it."));
    if (g_proj.has_box && (g_proj.box[2] < 5 || g_proj.box[3] < 5))
      p.push_back(Warning("The fighting area is very small (under 50 cm each way): wrestlers barely fit.")
                      .With("Make it 3 x 3 m", [] { g_proj.box[2] = g_proj.box[3] = 15; Touch(); }));
  }
}

void DrawArena() {
  if (g_simple) return SimpleDraw();
  const float top = ImGui::GetCursorPosY();
  Heading("Arena", "A new arena on the arena select pages, made from one of the game's own: edited in 3D here or in "
                   "Blender, or built from an empty hall.");
  {
    const ImVec2 after = ImGui::GetCursorPos();
    const float bw = 300 * g_scale;
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, ImGui::GetWindowWidth() - bw - 24 * g_scale), top));
    PushAccent();
    if (ImGui::Button("Make an arena step by step (Simple)", ImVec2(bw, 0))) g_simple = true;
    PopAccent();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("A step-by-step helper for this same arena: where it starts from, its name and pictures, crowd signs, "
                        "the ring and the VS screen.\nSwitch back here any time - nothing is lost.");
    ImGui::SetCursorPos(after);
  }
  std::vector<Problem> problems;
  ArenaProblems(problems);
  if (ImGui::BeginTabBar("arena_tabs")) {
    if (ImGui::BeginTabItem("Start")) {
      ImGui::BeginChild("start", ImVec2(0, -48 * g_scale), false);
      ArenaGrid();
      ImGui::SameLine();
      StartSide();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Details")) {
      ImGui::BeginChild("details", ImVec2(0, -48 * g_scale), false);
      DetailsTab();
      ImGui::Spacing();
      DrawProblems(problems);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("VS screen")) {
      ImGui::BeginChild("vs", ImVec2(0, -48 * g_scale), false);
      VsTab();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  switch (ModButtons(problems, true)) {
    case 1: SaveMod(); break;
    case 2: InstallMod(false); break;
    case 3: InstallMod(true); break;
  }
}

void DrawBackstage() {
  Heading("Backstage", "A backstage brawl room rebuilt. The game's seven rooms are in one file: the room you make is "
                       "played in that room's backstage brawls. One backstage mod plays at a time.");
  std::vector<Problem> problems;
  BackstageProblems(problems);
  ImGui::BeginChild("bs", ImVec2(0, -48 * g_scale), false);
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Area", kAreas[g_area].name)) {
    for (int k = 0; k < 7; ++k)
      if (ImGui::Selectable(kAreas[k].name, g_area == k)) g_area = k;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", kAreas[g_area].about);
  ImGui::BeginDisabled(Busy());
  PushAccent();
  if (ImGui::Button("Open the area in the 3D editor", ImVec2(320 * g_scale, 0))) StartBackstage(g_area);
  PopAccent();
  ImGui::EndDisabled();
  ImGui::TextWrapped("Keep its floor and walls where they are (the game's walls and cameras for the room stay). The "
                     "cars, crates and other things the superstars can use are placed by the game: they are not shown "
                     "here and stay.");
  ImGui::Separator();
  DetailsFields();
  if (g_proj.backstage >= 0)
    ImGui::TextWrapped("Open: %s%s.", kAreas[g_proj.backstage].name, g_proj.edited ? "" : " (loading)");
  ImGui::Separator();
  OwnAreaFields();
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, true)) {
    case 1: SaveMod(); break;
    case 2: InstallMod(false); break;
    case 3: InstallMod(true); break;
  }
}

PageHooks hooks = {
    "arena",
    DrawArena,
    ArenaProblems,
    Reset,
    StateText,
    WriteProject,
    [](const ProjectIn& in) { return ReadModOrProject(in, false); },
    [](const ProjectIn& in) { return ReadModOrProject(in, true); },
};

}  // namespace arena_page

namespace backstage_page {
PageHooks hooks = {
    "backstage",
    arena_page::DrawBackstage,
    arena_page::BackstageProblems,
    arena_page::Reset,
    arena_page::StateText,
    arena_page::WriteProject,
    [](const ProjectIn& in) { return arena_page::ReadModOrProject(in, false); },
    [](const ProjectIn& in) { return arena_page::ReadModOrProject(in, true); },
};
}  // namespace backstage_page

}  // namespace mm
