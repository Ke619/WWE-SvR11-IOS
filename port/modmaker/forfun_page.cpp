// For Fun page: the whole arena or a whole superstar out to FBX and PNG, to look at or use in other programs, from the
// user's own game files into the user's own folder.
//  - arena: svrfmt::ExportArenaFull (every object where the game shows it, ring and lights included);
//  - superstar: svrfmt::ExportCharacterFbx (skeleton, weights, one attire, colour and normal maps), which the Superstar
//    page's FBX import takes back.
// What's picked starts as what's open: the Arena page's arena, the Superstar page's model; a list picks any other.
#include <algorithm>
#include <cstdio>

#include "app.h"
#include "arena_page_internal.h"
#include "star_page_internal.h"
#include "svrfmt/arena_full.h"
#include "svrfmt/char_fbx.h"

namespace mm {
namespace forfun_page {

using namespace svrfmt;

namespace {

int g_arena = -2;  // -1: the one open on the Arena page; else an arena tile; -2: not chosen yet (the open one if any)
int g_star_id = -2;  // -1: the Superstar page's model; else a superstar's id; -2: not chosen yet
int g_attire = 1;
std::wstring g_attires_of;
std::vector<int> g_attires;

// test aids: --forfun-arena <tile or -1>,<dir>; --forfun-star <id or -1>,<attire>,<dir> (exported once, then the window closes)
int g_test_arena = -2, g_test_star = -2, g_test_attire = 1;
std::wstring g_test_dir;

bool OpenArena() { return arena_page::g_proj.arena >= 0 && arena_page::g_proj.backstage < 0; }

std::string OpenArenaName() {
  if (arena_page::g_proj.name[0]) return arena_page::g_proj.name;
  return OpenArena() ? g_arenas[arena_page::g_proj.arena].name : "";
}

std::wstring OpenStarModel() {
  const std::wstring& m = star_page::g_star.model;
  return Lower(FileName(m)).ends_with(".pac") ? m : std::wstring();
}

std::wstring StarPac(int id) {
  if (id < 0) return OpenStarModel();
  std::error_code ec;
  const fs::path disc = fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac");
  if (fs::exists(disc, ec)) return disc.wstring();
  const auto dlc = DlcModelPacs(id);
  return dlc.empty() ? std::wstring() : dlc.front().wstring();
}

std::string StarName(int id) {
  if (id < 0) return star_page::g_star.name[0] ? star_page::g_star.name : "the Superstar page's model";
  const StarInfo* s = StarOrDlcById(id);
  return s ? s->name : "superstar " + std::to_string(id);
}

void ExportArena(int choice, const std::wstring& dir) {
  std::string title;
  std::shared_ptr<svrfmt::Arena> copy;
  std::string pac;
  if (choice < 0) {
    title = OpenArenaName();
    if (arena_page::g_proj.edited) copy = std::make_shared<svrfmt::Arena>(*arena_page::g_proj.edited);  // (with the edits)
    else pac = ArenaPath(arena_page::g_proj.arena);
  } else {
    title = g_arenas[choice].name;
    pac = ArenaPath(choice);
  }
  const std::string out = PathStr(fs::path(dir) / fs::u8path(IdFrom(title, "arena") + "_fbx"));
  RunInBackground([copy, pac, out, title] {
    Progress("Exporting " + title + " ...");
    svrfmt::Arena loaded;
    std::string err;
    if (!copy && !loaded.Load(pac, &err)) {
      Status(err);
      return;
    }
    svrfmt::ArenaFullReport rep;
    const bool ok = svrfmt::ExportArenaFull(copy ? *copy : loaded, out, title, rep);
    for (const auto& w : rep.warnings) Log("  " + w);
    if (!ok) {
      Status("The arena could not be written (see the log).");
      return;
    }
    char b[320];
    std::snprintf(b, sizeof b, "Exported %s: %d objects (%d ring parts, %d placed by their motion, %d mirrored), %d triangles, %d textures, %d normal map links, %d lights.",
                  title.c_str(), rep.objects, rep.ring_parts, rep.placed, rep.mirrored, rep.triangles, rep.textures, rep.normal_maps, rep.lights);
    Status(b);
    Log(std::string("for fun: ") + b + " " + out);
    if (!g_behind) ShellOpen(Wide(out));
  });
}

void ExportStar(int choice, int attire, const std::wstring& dir) {
  const std::wstring pac = StarPac(choice);
  if (pac.empty()) {
    Status("No model file for that superstar.");
    return;
  }
  const std::string name = StarName(choice), id = IdFrom(name, "superstar") + (attire > 1 ? "_attire" + std::to_string(attire) : "");
  const std::string out = PathStr(fs::path(dir) / fs::u8path(id + "_fbx"));
  RunInBackground([pac, attire, out, id, name] {
    Progress("Exporting " + name + " ...");
    Bytes b;
    svrfmt::CharExportReport rep;
    const bool ok = ReadFile(Utf8(pac), b) && svrfmt::ExportCharacterFbx(b, attire, out, id, rep, true);
    for (const auto& w : rep.warnings) Log("  " + w);
    if (!ok) {
      Status("The superstar could not be written: " + (rep.warnings.empty() ? std::string("see the log") : rep.warnings.front()));
      return;
    }
    const std::string text = "Exported " + name + ": " + std::to_string(rep.bones) + " bones, " + std::to_string(rep.meshes) + " meshes, " +
                             std::to_string(rep.vertices) + " vertices, " + std::to_string(rep.textures) + " textures.";
    Status(text);
    Log("for fun: " + text + " " + out);
    if (!g_behind) ShellOpen(Wide(out));
  });
}

void ArenaCard() {
  ImGui::SeparatorText("Full arena");
  ImGui::TextWrapped("The whole arena as one FBX: every object where the game shows it, the ring put together, the lights, "
                     "and every texture as a PNG with normal maps linked. For looking at or using in other programs; to edit "
                     "an arena and bring it back, use the Arena page's Export to Blender.");
  if (g_arena == -2) g_arena = OpenArena() ? -1 : 0;
  if (g_arena == -1 && !OpenArena()) g_arena = 0;
  const std::string now = g_arena < 0 ? "Open on the Arena page: " + OpenArenaName() : std::string(g_arenas[g_arena].name);
  ImGui::SetNextItemWidth(420 * g_scale);
  if (ImGui::BeginCombo("Arena", now.c_str())) {
    if (OpenArena() && ImGui::Selectable(("Open on the Arena page: " + OpenArenaName()).c_str(), g_arena == -1)) g_arena = -1;
    for (int i = 0; i < 20; ++i)
      if (g_arenas[i].name && ImGui::Selectable(g_arenas[i].name, g_arena == i)) g_arena = i;
    ImGui::EndCombo();
  }
  ImGui::BeginDisabled(Busy() || g_game.empty());
  if (ImGui::Button("Export the arena...", ImVec2(240 * g_scale, 0))) {
    const std::wstring dir = PickFolder(L"A folder for the arena's FBX and textures");
    if (!dir.empty()) ExportArena(g_arena, dir);
  }
  ImGui::EndDisabled();
}

void StarCard() {
  ImGui::SeparatorText("Full superstar");
  ImGui::TextWrapped("A superstar as an FBX: skeleton, skin weights, one attire's meshes, and colour and normal maps as PNGs. "
                     "It comes back in: on the Superstar page, pick the FBX as the model and Make it.");
  if (g_star_id == -2) g_star_id = OpenStarModel().empty() ? -3 : -1;
  if (g_star_id == -1 && OpenStarModel().empty()) g_star_id = -3;
  const auto& stars = Stars();
  if (g_star_id == -3 && !stars.empty()) g_star_id = stars.front().id;
  const std::string now = g_star_id < 0 ? "On the Superstar page: " + StarName(-1) : StarName(g_star_id);
  ImGui::SetNextItemWidth(420 * g_scale);
  if (ImGui::BeginCombo("Superstar", now.c_str())) {
    if (!OpenStarModel().empty() && ImGui::Selectable(("On the Superstar page: " + StarName(-1)).c_str(), g_star_id == -1)) g_star_id = -1;
    for (const auto& s : stars)
      if (ImGui::Selectable((s.name + "##" + std::to_string(s.id)).c_str(), g_star_id == s.id)) g_star_id = s.id;
    for (const auto& s : DlcStars())
      if (ImGui::Selectable((s.name + "##" + std::to_string(s.id)).c_str(), g_star_id == s.id)) g_star_id = s.id;
    ImGui::EndCombo();
  }
  const std::wstring pac = StarPac(g_star_id);
  if (pac != g_attires_of) {
    g_attires_of = pac;
    g_attires = pac.empty() ? std::vector<int>() : ModelAttires(pac);
    g_attire = g_attires.empty() ? 1 : g_attires.front();
  }
  ImGui::SetNextItemWidth(160 * g_scale);
  if (ImGui::BeginCombo("Attire", ("Attire " + std::to_string(g_attire)).c_str())) {
    for (int a : g_attires)
      if (ImGui::Selectable(("Attire " + std::to_string(a)).c_str(), a == g_attire)) g_attire = a;
    ImGui::EndCombo();
  }
  ImGui::BeginDisabled(Busy() || pac.empty());
  if (ImGui::Button("Export the superstar...", ImVec2(240 * g_scale, 0))) {
    const std::wstring dir = PickFolder(L"A folder for the superstar's FBX and textures");
    if (!dir.empty()) ExportStar(g_star_id, g_attire, dir);
  }
  ImGui::EndDisabled();
}

}  // namespace

void Draw() {
  if ((g_test_arena != -2 || g_test_star != -2) && !Busy()) {
    if (g_test_arena != -2) {
      const int a = g_test_arena;
      g_test_arena = -2;
      ExportArena(a, g_test_dir);
    } else {
      const int s = g_test_star;
      g_test_star = -2;
      ExportStar(s, g_test_attire, g_test_dir);
    }
    WhenIdle([] { PostMessageW(g_wnd, WM_CLOSE, 0, 0); });
  }
  Heading("For Fun", "The whole arena or a whole superstar out to FBX and PNG, from your game into a folder of yours.");
  ImGui::BeginChild("forfun_body", ImVec2(0, 0), false);
  ArenaCard();
  ImGui::Spacing();
  StarCard();
  ImGui::EndChild();
}

void TestStart(const std::wstring& arena, const std::wstring& star) {
  const auto split = [](const std::wstring& v, wchar_t c) {
    std::vector<std::wstring> out;
    size_t at = 0;
    for (int k = 0; k < 2; ++k) {
      const size_t e = v.find(c, at);
      if (e == std::wstring::npos) break;
      out.push_back(v.substr(at, e - at));
      at = e + 1;
    }
    out.push_back(v.substr(at));
    return out;
  };
  if (!arena.empty()) {
    const size_t c = arena.find(L',');
    g_test_arena = _wtoi(arena.substr(0, c).c_str());
    g_test_dir = arena.substr(c + 1);
  }
  if (!star.empty()) {
    const auto p = split(star, L',');
    if (p.size() == 3) g_test_star = _wtoi(p[0].c_str()), g_test_attire = _wtoi(p[1].c_str()), g_test_dir = p[2];
  }
}

}  // namespace forfun_page
}  // namespace mm
