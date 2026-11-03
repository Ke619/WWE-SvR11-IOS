// The Arena page's Simple mode: a step-by-step front end over the same project (g_proj) for a first arena, in the
// style of Simple superstar (simple_steps.h). Each step is one card - a title, one "What is this?" line, only that
// step's controls - beside a 3D view of the arena. The full page and the 3D editor stay one click away and nothing is
// lost going back and forth.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "app.h"
#include "arena_page_internal.h"
#include "disc_list.h"
#include "editor.h"
#include "mod_check.h"
#include "simple_steps.h"

namespace mm {
namespace arena_page {

namespace {

using simple::Card;
using simple::CardsPerRow;
using simple::CardWidth;

const simple::Step kSteps[kSaCount] = {
    {"Start from", "Every arena starts from another one: an arena in the game, one from your other WWE games, or an empty hall with only the ring."},
    {"Name and picture", "The arena's name and its picture on the arena select screen."},
    {"Aprons and screens", "The pictures round the ring and on the big screens: the ring aprons, the barrier banners, the titantron and the stage's screens and logos."},
    {"Crowd signs", "Signs the fans hold up. Any picture is fitted onto a white board."},
    {"Ring and lights", "The ropes, the turnbuckles and corner pads, and how the arena is lit."},
    {"VS and loading", "The pictures behind the two superstars before the match, and the picture while the match loads."},
    {"Look at it", "Your arena in 3D: turn it and zoom in to check everything."},
    {"Finish", "Check everything, then save your arena or put it in the game."},
};

// The games with arenas the converters take (arena_port.py `games`: SvR 2007 - 2010 and Legends of WrestleMania ready,
// WWE '12 - 2K17 native).
const char* const kArenaGames[] = {"svr2007", "svr2008", "svr2009", "svr2010", "low", "wwe12", "wwe13", "wwe2k14", "wwe2k15", "wwe2k16", "wwe2k17"};
bool NativeGame(const std::string& key) { return key.rfind("wwe", 0) == 0; }

int g_start_mode = -1;      // 0 an arena in the game, 1 from my other games, 2 an empty hall
std::string g_disc_key;     // (mode 1) the game picked
bool choosing = true;       // (a game picked: its cards fold away, the arenas get the room)
std::wstring g_lone_file;   // (mode 1) or one arena file (.pac) on its own, no disc
int g_native_host = 1;      // a WWE '12 - 2K17 arena's host tile (RAW, as the bundled ones)
std::string g_name_typed;   // the name the player typed (kept when the start changes)

// ---- test aid --simple-arena-start "<start>;key=value;...": start = tile:<n> | empty:<n> | disc:<game>:<item>[:<host>];
// then name=, banner=<picture>, tex=<texture>:<picture> (apron:<picture>: the first apron), sign=<picture>,
// load=<picture>, step=<n>, save=<file.svrmod> (saved, the log beside it, quit)
std::vector<std::string> g_test;
int g_test_stage = -1;  // 0 start, 1 wait for the arena, 2 the rest, 3 done, 4 installing
int g_test_step = -1;
std::string g_test_log;  // (install=: the log file written when the install is done)

// ---- pictures

// A picture box (w x h) with "Pick a picture..." under it; a picture file dropped on it does the same. True with the
// picture in `got`.
bool PictureBox(const char* id, Picture* shown, float w, float h, const wchar_t* title, Image& got, const char* empty_text) {
  ImGui::PushID(id);
  bool picked = false;
  ImGui::BeginGroup();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (shown && !shown->Empty()) {
    shown->Draw(w, h);
  } else {
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_TextDisabled), 4 * g_scale, 0, 1.5f * g_scale);
    const ImVec2 ts = ImGui::CalcTextSize(empty_text);
    dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (h - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), empty_text);
  }
  if (ImGui::Button("Pick a picture...", ImVec2(w, 0))) {
    const std::wstring f = PickFile(false, title, kPictureFilter, 1);
    if (!f.empty() && LoadPicture(f, got)) picked = true;
    else if (!f.empty()) {
      Status("That file isn't a picture the Mod Maker can read (PNG, JPG, BMP, TGA, or a DXT1 / DXT5 DDS).");
      Log("Simple arena: not a picture it reads - " + Utf8(f));
    }
  }
  ImGui::EndGroup();
  std::vector<std::wstring> dropped;
  if (DroppedOnLastItem(dropped) && !dropped.empty()) {
    if (LoadPicture(dropped[0], got)) picked = true;
    else Status("That file isn't a picture the Mod Maker can read.");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) ImGui::SetTooltip("Pick a picture, or drop one here.");
  ImGui::PopID();
  return picked;
}

// The arena's textures as pictures (decoded on first sight, again after a change).
std::map<std::string, Picture> g_thumbs;
const svrfmt::Arena* g_thumbs_for = nullptr;
std::map<std::string, std::pair<int, int>> g_sizes;

Picture* Thumb(const std::string& name) {
  if (g_thumbs_for != g_proj.edited.get()) g_thumbs.clear(), g_sizes.clear(), g_thumbs_for = g_proj.edited.get();
  auto it = g_thumbs.find(name);
  if (it == g_thumbs.end()) {
    Image img;
    int w = 0, h = 0;
    it = g_thumbs.emplace(name, Picture()).first;
    if (editor::TextureImage(name, img, &w, &h)) {
      for (size_t k = 3; k < img.rgba.size(); k += 4) img.rgba[k] = 255;  // (shown without its see-through parts)
      it->second.Set(std::move(img));
      g_sizes[name] = {w, h};
    }
  }
  return &it->second;
}

bool ReplaceArenaTexture(const std::string& name, const Image& img) {
  if (!g_proj.originals.count(name)) {
    Bytes was;
    if (editor::TextureBytes(name, was)) g_proj.originals[name] = std::move(was);
  }
  if (!editor::ReplaceTexture(name, img)) return false;
  g_thumbs.erase(name);
  Touch();
  return true;
}

void RestoreTexture(const std::string& name) {
  const auto it = g_proj.originals.find(name);
  if (it == g_proj.originals.end()) return;
  editor::SetTextureBytes(name, it->second);
  g_proj.originals.erase(it);
  g_thumbs.erase(name);
  Touch();
}

// The arena's pictures in groups, by what they are (the names the game's arenas use).
struct Group {
  const char* title;
  const char* about;
  std::vector<std::string> names;
};
std::vector<Group> g_groups;
const svrfmt::Arena* g_groups_for = nullptr;

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

const std::vector<Group>& Groups() {
  if (g_groups_for == g_proj.edited.get()) return g_groups;
  g_groups_for = g_proj.edited.get();
  g_groups = {
      {"Ring aprons", "The skirt round the ring. Wide pictures work best (the apron is long and low).", {}},
      {"Barrier banners", "The banners on the barrier round the ring.", {}},
      {"Titantron and big screens", "The big screen over the stage and the screens beside it.", {}},
      {"Stage logos and signs", "Logos, flags and signs round the stage and the arena.", {}},
  };
  if (!g_proj.edited) return g_groups;
  const auto skip = [](const std::string& l) {
    for (const char* p : {"sdw", "shd", "shad", "glow", "flare", "lig", "mask", "ref", "cube", "blood", "beam", "light", "moya"})
      if (Has(l, p)) return true;
    return l.size() > 2 && l.compare(l.size() - 2, 2, "_n") == 0;  // (normal maps)
  };
  std::set<std::string> taken;
  auto add = [&](int g, const std::string& n) {
    if (taken.insert(n).second) g_groups[size_t(g)].names.push_back(n);
  };
  for (const auto& n : editor::ModelTextures(973, 974))  // (the apron models)
    if (!skip(Lower(n))) add(0, n);
  for (const auto& n : editor::TextureNames()) {
    const std::string l = Lower(n);
    if (skip(l)) continue;
    if (l.rfind("rin_ep", 0) == 0) add(0, n);
    else if (Has(l, "fen") && l.rfind("rin_", 0) != 0) add(1, n);
    else if (Has(l, "titan") || Has(l, "tiwall") || Has(l, "_tv") || Has(l, "screen") || Has(l, "monitor")) add(2, n);
    else if (Has(l, "logo") || Has(l, "bill") || Has(l, "flag") || Has(l, "anim") || Has(l, "_ent") || Has(l, "dotcom") ||
             Has(l, "banner") || Has(l, "sign"))
      add(3, n);
  }
  return g_groups;
}

// ---- the steps

void StartGrid(bool empty) {
  ImGui::TextDisabled(empty ? "The empty hall plays in the place of one of the game's arenas: pick which (its room, its VS screen style)."
                            : "Pick the arena to start from. It is copied: the game's own stays as it is.");
  const float tw = 150 * g_scale, th = tw * 0.5f;
  const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 8 * g_scale) / (tw + 8 * g_scale)));
  for (int i = 0; i < 20; ++i) {
    if (i % per_row) ImGui::SameLine(0, 8 * g_scale);
    ImGui::PushID(i);
    ImGui::BeginGroup();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!g_arenas[i].tile.Empty()) g_arenas[i].tile.Draw(tw, th);
    else ImGui::Button(g_arenas[i].name, ImVec2(tw, th));
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(p.x + tw, ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight()), true);
    ImGui::TextUnformatted(g_arenas[i].name);
    ImGui::PopClipRect();
    ImGui::EndGroup();
    const bool on = g_proj.arena == i && g_proj.source.empty() && (empty == (g_start_mode == 2));
    if (on) ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + tw + 2, p.y + th + 2), IM_COL32(230, 25, 50, 255), 4, 0, 3 * g_scale);
    if (ImGui::IsItemClicked() && !Busy()) {
      if (empty) {
        NewArena(i, false);
      } else {
        g_proj.arena = -1;  // (StartProject keeps a project already on this tile)
        StartProject(i);
        LoadInBackground(ArenaPath(i), nullptr, false, false);
      }
      g_proj.source.clear();
      if (!g_name_typed.empty()) std::snprintf(g_proj.name, sizeof g_proj.name, "%s", g_name_typed.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_arenas[i].name);
    ImGui::PopID();
  }
}

void StartFromDisc() {
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("Your games from My game discs. Pick one, list its arenas and convert one. Greyed arenas crash the game "
                      "or aren't arenas (backstage rooms, studios).");
  ImGui::PopTextWrapPos();

  const bool lone = !g_lone_file.empty();
  if (lone && !choosing) {
    ImGui::Text("The arena file %s", FileName(g_lone_file).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Change")) choosing = true;
  } else if (!g_disc_key.empty() && !disc_list::PathFor(g_disc_key).empty() && !choosing) {
    const disc_list::Game* g = disc_list::GameByKey(g_disc_key);
    ImGui::Text("Arenas from %s", g ? g->name : g_disc_key.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Change game")) choosing = true;
  }
  const float w = std::min(CardWidth(), 200 * g_scale), h = 58 * g_scale;
  const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 8 * g_scale) / (w + 8 * g_scale)));
  int n = 0, missing = 0;
  for (const char* key : kArenaGames) {
    if (!choosing && (lone || (!g_disc_key.empty() && !disc_list::PathFor(g_disc_key).empty()))) break;
    const disc_list::Game* g = disc_list::GameByKey(key);
    if (!g) continue;
    if (disc_list::PathFor(key).empty()) {
      ++missing;
      continue;
    }
    if (n++ % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card(key, g->name, NativeGame(key) ? "converted by the Mod Maker" : "converted by the arena tool", !lone && g_disc_key == key, w, h) &&
        (lone || g_disc_key != key))
      g_disc_key = key, g_lone_file.clear(), port_page::ClearEmbeddedResult(), choosing = false;
    else if (g_disc_key == key && ImGui::IsItemClicked()) choosing = false;
  }
  if (choosing || (g_disc_key.empty() && !lone)) {
    if (ImGui::Button("One arena file (.pac)...")) {
      static const COMDLG_FILTERSPEC kPac[1] = {{L"An arena file (bgNN.pac)", L"*.pac"}};
      const std::wstring f = PickFile(false, L"One arena file from another game (SvR 2007 - WWE 2K17: pac\\bg\\bgNN.pac)", kPac, 1,
                                      nullptr, nullptr, OtherGameStart());
      if (!f.empty()) OtherGamePicked(f), g_lone_file = f, port_page::ClearEmbeddedResult(), choosing = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(no disc: just the arena's bgNN.pac, e.g. one a friend sent)");
  }
  if (!n && choosing) ImGui::TextColored(kWarn, "None of your games with arenas is in My game discs yet.");
  if (missing && choosing) {
    ImGui::TextDisabled("%d more game%s with arenas can be added (SvR 2007 - 2010, Legends of WrestleMania, WWE '12 - 2K17).", missing, missing == 1 ? "" : "s");
    ImGui::SameLine();
    if (ImGui::SmallButton("My game discs...")) GoTo(PageId::kDiscs);
  }
  if (g_lone_file.empty() && (g_disc_key.empty() || disc_list::PathFor(g_disc_key).empty())) return;
  if (choosing && !g_lone_file.empty()) return;
  ImGui::Separator();
  port_page::SetSource(!g_lone_file.empty() ? g_lone_file : disc_list::PathFor(g_disc_key));
  const std::wstring got = port_page::EmbeddedResult();
  const std::string ext = Lower(Utf8(fs::path(got).extension().wstring()));
  if (!got.empty() && ext == ".svrmod") {  // SvR 2007 - 2010: the converter's arena mod, opened as the project
    port_page::ClearEmbeddedResult();
    const std::string keep = g_name_typed;
    if (ProjectOpen(got)) {
      GoTo(PageId::kArena);
      g_simple = true, g_simple_step = kSaStart, g_start_mode = 1;
      if (!keep.empty()) std::snprintf(g_proj.name, sizeof g_proj.name, "%s", keep.c_str());
      Status("The converted arena is your arena now: next, its name and picture.");
    }
    return;
  }
  if (!got.empty() && ext == ".pac") {  // WWE '12 - 2K17: the arena pac, converted here onto a host arena
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s is ready to convert.", FileName(got).c_str());
    ImGui::SetNextItemWidth(320 * g_scale);
    if (ImGui::BeginCombo("Plays in the place of", g_arenas[g_native_host].name)) {
      for (int i = 0; i < 20; ++i)
        if (ImGui::Selectable(g_arenas[i].name, g_native_host == i)) g_native_host = i;
      ImGui::EndCombo();
    }
    Hint("The game arena your arena loads in place of: its room (the file size the arena must fit) and its VS screen "
         "style. RAW's was used for the bundled ones.");
    ImGui::BeginDisabled(Busy());
    PushAccent();
    const bool go = ImGui::Button("Convert it", ImVec2(220 * g_scale, 0)) || (g_test_stage == 1 && !Busy());
    PopAccent();
    ImGui::EndDisabled();
    if (go) {
      const disc_list::Game* g = g_lone_file.empty() ? disc_list::GameByKey(g_disc_key) : nullptr;
      const std::string stem = Utf8(fs::path(got).stem().wstring());
      const std::string scanned = port_page::EmbeddedName();  // (the game's own name of the arena, from the scan)
      const std::string name = !g_name_typed.empty() ? g_name_typed
                               : !g_lone_file.empty() ? stem
                               : !scanned.empty()     ? scanned
                                                      : std::string(g ? g->name : "Arena") + " " + stem.substr(stem.rfind('_') + 1);
      const std::wstring picture = port_page::EmbeddedPicture();  // (the disc's picture: banner + loading screen)
      port_page::ClearEmbeddedResult();
      other_page::ConvertArena(got, g_native_host, name, false, picture);
    }
    return;
  }
  port_page::EmbeddedFlow(L"arena_port.py", false);
}

void StepStart() {
  int& mode = g_start_mode;
  if (mode < 0) mode = !g_proj.source.empty() ? 1 : 0;
  const float w = CardWidth(), h = 96 * g_scale;
  const int per_row = CardsPerRow(w);
  if (Card("game", "An arena in the game", "Start from one of the game's 20 arenas and make it your own.", mode == 0, w, h)) mode = 0;
  if (1 % per_row) ImGui::SameLine(0, 8 * g_scale);
  if (Card("disc", "From my other games", "An arena from SvR 2007 - 2010 or WWE '12 - 2K17 (My game discs).", mode == 1, w, h)) mode = 1;
  if (2 % per_row) ImGui::SameLine(0, 8 * g_scale);
  if (Card("empty", "An empty hall", "Only the ring, the floor and the ringside: build the rest in the 3D editor.", mode == 2, w, h)) mode = 2;
  ImGui::Spacing();
  if (g_proj.arena >= 0) {
    const std::string now = !g_proj.source.empty() ? "converted from " + g_proj.source + ", in the place of " + g_arenas[g_proj.arena].name
                                                   : std::string("from ") + g_arenas[g_proj.arena].name;
    ImGui::Text("Now: %s%s.", now.c_str(), g_proj.edited ? "" : " (loading ...)");
    ImGui::TextDisabled("Picking another start replaces the arena; its name stays.");
  } else {
    ImGui::TextDisabled("Nothing picked yet.");
  }
  ImGui::Spacing();
  ImGui::BeginChild("start_body", ImVec2(0, 0), false);
  if (mode == 1) StartFromDisc();
  else StartGrid(mode == 2);
  ImGui::EndChild();
}

void StepName() {
  if (TextField("Name", g_proj.name, sizeof g_proj.name, "the arena's name on the select screen", 300)) g_name_typed = g_proj.name, Touch();
  TextField("Author (optional)", g_proj.author, sizeof g_proj.author, nullptr, 300);
  ImGui::Spacing();
  ImGui::TextUnformatted("The select screen picture");
  ImGui::TextDisabled("Shown at 256 x 128 on the arena select pages. Without one, a dark picture is used.");
  Image img;
  if (PictureBox("banner", &g_proj.banner, 256 * g_scale, 128 * g_scale, L"The arena select picture (shown at 256 x 128)", img, "no picture yet")) {
    g_proj.banner.Set(svrfmt::Resize(img, 256, 128));
    Touch();
  }
  if (!g_proj.banner.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) g_proj.banner.Clear(), Touch();
  }
}

void StepPictures() {
  if (!g_proj.edited) {
    ImGui::TextDisabled("Pick where your arena starts from first (step 1).");
    return;
  }
  ImGui::TextDisabled("Pick a picture for any of them, or drop one on it. Each is fitted to that picture's size.");
  const float tw = 150 * g_scale;
  int shown = 0;
  for (const auto& g : Groups()) {
    if (g.names.empty()) continue;
    ++shown;
    ImGui::Spacing();
    ImGui::TextUnformatted(g.title);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g.about);
    const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 10 * g_scale) / (tw + 10 * g_scale)));
    int n = 0;
    for (const auto& name : g.names) {
      if (n++ % per_row) ImGui::SameLine(0, 10 * g_scale);
      ImGui::BeginGroup();
      Picture* pic = Thumb(name);
      const float h = tw * 0.5f;  // (one height a row: the picture fitted in, its shape kept)
      Image img;
      if (PictureBox(name.c_str(), pic, tw, h, Wide("A picture for " + name).c_str(), img, name.c_str())) {
        if (ReplaceArenaTexture(name, img)) Log("Simple arena: " + name + " replaced");
        else {
          Status(name + " couldn't take that picture (its texture is of a kind the Mod Maker doesn't rewrite).");
          Log("Simple arena: " + name + " NOT replaced (" + std::to_string(img.w) + "x" + std::to_string(img.h) + ")");
        }
      }
      const auto sz = g_sizes.find(name);
      ImGui::TextDisabled("%s", name.c_str());
      if (sz != g_sizes.end()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d", sz->second.first, sz->second.second);
      }
      if (g_proj.originals.count(name)) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "yours");
        ImGui::SameLine();
        if (ImGui::SmallButton(("Original##" + name).c_str())) RestoreTexture(name);
      }
      ImGui::EndGroup();
    }
  }
  if (!shown) ImGui::TextDisabled("This arena has no apron, banner or screen pictures the Mod Maker knows by name: the 3D editor "
                                  "retextures any object (click it, then \"Use a picture...\").");
  ImGui::Spacing();
  ImGui::TextDisabled("Any other picture: open the 3D editor, click the object, then \"Use a picture...\".");
}

void AddSigns(const std::vector<std::wstring>& files) {
  for (const auto& f : files) {
    Image img;
    if (!LoadPicture(f, img)) continue;
    Picture pic;
    pic.Set(signs_page::SignPicture(img));
    g_proj.signs.push_back({Utf8(fs::path(f).stem().wstring()), std::move(pic)});
    Touch();
  }
}

void StepSigns() {
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("The crowd holds them up in every match, with the game's own signs (the game's signs aren't tied to one arena). "
                      "They are saved and installed with the arena, as a sign pack of their own.");
  ImGui::PopTextWrapPos();
  if (ImGui::Button("Add pictures...", ImVec2(200 * g_scale, 0))) AddSigns(PickFiles(L"Sign pictures", kPictureFilter, 1));
  std::vector<std::wstring> dropped;
  if (DroppedOnLastItem(dropped)) AddSigns(dropped);
  ImGui::SameLine();
  ImGui::TextDisabled("%zu sign%s", g_proj.signs.size(), g_proj.signs.size() == 1 ? "" : "s");
  ImGui::Spacing();
  ImGui::BeginChild("signs", ImVec2(0, 0), false);
  const float tw = 128 * 1.4f * g_scale, th = 64 * 1.4f * g_scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 14 * g_scale)));
  for (size_t i = 0; i < g_proj.signs.size(); ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(int(i));
    ImGui::BeginGroup();
    ImGui::Image(g_proj.signs[i].second.Id(), ImVec2(tw, th));
    ImGui::TextDisabled("%s", g_proj.signs[i].first.substr(0, 16).c_str());
    ImGui::SameLine(tw - 56 * g_scale);
    const bool gone = ImGui::SmallButton("remove");
    ImGui::EndGroup();
    ImGui::PopID();
    if (gone) {
      g_proj.signs.erase(g_proj.signs.begin() + long(i));
      Touch();
      break;
    }
  }
  if (g_proj.signs.empty()) {  // the whole empty area takes dropped pictures
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(80 * g_scale, ImGui::GetContentRegionAvail().y)));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_TextDisabled), 6 * g_scale, 0, 1.5f * g_scale);
    const char* t = "No signs yet: add pictures, or drop them here.";
    const ImVec2 ts = ImGui::CalcTextSize(t);
    ImGui::GetWindowDrawList()->AddText(ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
    if (DroppedOnLastItem(dropped)) AddSigns(dropped);
  }
  ImGui::EndChild();
}

void StepRing() {
  if (!g_proj.edited) {
    ImGui::TextDisabled("Pick where your arena starts from first (step 1).");
    return;
  }
  const float w = std::min(480 * g_scale, ImGui::GetContentRegionAvail().x);
  ImGui::TextUnformatted("The ring");
  ImGui::BeginChild("ringkit", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY);
  editor::DrawRingKit();
  ImGui::EndChild();
  ImGui::Separator();
  ImGui::TextUnformatted("The lights");
  ImGui::BeginChild("lights", ImVec2(w, 0), ImGuiChildFlags_AutoResizeY);
  editor::DrawLighting();
  ImGui::EndChild();
  ImGui::TextDisabled("The 3D view on the right shows the changes.");
}

void StepVs() {
  ImGui::TextUnformatted("The loading picture");
  ImGui::TextDisabled("Shown while the match loads (1024 x 512). Without one, the arena it plays in place of keeps its own.");
  Image img;
  if (PictureBox("load", &g_proj.load, 320 * g_scale, 160 * g_scale, L"The loading picture (shown at 1024 x 512)", img, "no picture yet")) {
    g_proj.load.Set(svrfmt::Resize(img, 1024, 512));
    Touch();
  }
  if (!g_proj.load.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove##load")) g_proj.load.Clear(), Touch();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("The VS screen");
  if (g_proj.arena < 0) {
    ImGui::TextDisabled("Pick where your arena starts from first (step 1).");
    return;
  }
  VsTab();
}

void StepLook() {
  if (!g_proj.edited) {
    ImGui::TextDisabled(g_proj.arena >= 0 ? "Loading the arena ..." : "Pick where your arena starts from first (step 1).");
    return;
  }
  if (ImGui::Button("From the hard camera")) editor::SetViewPreset(0);
  ImGui::SameLine();
  if (ImGui::Button("From above")) editor::SetViewPreset(1);
  ImGui::SameLine();
  if (ImGui::Button("From the stage")) editor::SetViewPreset(2);
  ImGui::SameLine();
  if (ImGui::Button("From the side")) editor::SetViewPreset(3);
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - 240 * g_scale));
  if (ImGui::Button("Open the full 3D editor", ImVec2(240 * g_scale, 0))) GoTo(PageId::kEditor);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move, add, hide and retexture objects, props from other arenas, your own models.\n"
                                                "Your Simple steps stay as they are: come back with the Arena page.");
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  editor::DrawViewport(avail.x, std::max(120.0f, avail.y - 34 * g_scale));
  editor::BudgetInfo b;
  if (editor::Budget(b))
    ImGui::TextDisabled("Size: %s of the %s the game has room for%s.", Human(std::max(b.file, b.unpacked)).c_str(),
                        Human(std::max(b.file_max, b.unpacked_max)).c_str(),
                        b.file > b.file_max || b.unpacked > b.unpacked_max ? " - saving makes the biggest pictures smaller to fit" : "");
  else
    ImGui::TextDisabled("Measuring the size ...");
}

std::vector<Problem> g_doctor;  // the mod doctor's findings on the installed arena (Finish)
bool g_doctor_ran = false;

void StepFinish() {
  int& step = g_simple_step;
  ImGui::TextUnformatted("Your arena");
  ImGui::Indent();
  const auto row = [&](int at, const std::string& text) { simple::SummaryRow(kSteps, at, text, step); };
  row(kSaStart, g_proj.arena < 0 ? std::string("(nothing yet)")
                : !g_proj.source.empty() ? "converted from " + g_proj.source
                : std::string(g_start_mode == 2 ? "an empty hall in the place of " : "from ") + g_arenas[g_proj.arena].name);
  row(kSaName, std::string(g_proj.name[0] ? g_proj.name : "(no name yet)") + (g_proj.banner.Empty() ? ", no picture" : ", your picture"));
  row(kSaPictures, g_proj.originals.empty() ? std::string("as they were") : std::to_string(g_proj.originals.size()) + " of your own");
  row(kSaSigns, g_proj.signs.empty() ? std::string("none") : std::to_string(g_proj.signs.size()) + " sign" + (g_proj.signs.size() == 1 ? "" : "s"));
  row(kSaRing, editor::Ring().Default() && editor::Light().Default() ? std::string("as they were")
               : std::to_string(editor::Ring().VisibleRopes()) + " ropes" + (editor::Light().Default() ? "" : ", your lights"));
  row(kSaVs, std::to_string(g_proj.vs.size()) + " VS picture" + (g_proj.vs.size() == 1 ? "" : "s") + (g_proj.load.Empty() ? "" : ", a loading picture"));
  ImGui::Unindent();
  ImGui::Spacing();
  std::vector<Problem> problems;
  ArenaProblems(problems);
  simple::ProblemList(problems, step, kSaFinish);
  ImGui::Spacing();
  ImGui::Spacing();
  switch (ModButtons(problems, true, (ImGui::GetContentRegionAvail().x - 16 * g_scale) / 3 / g_scale)) {
    case 1: SaveMod(); break;
    case 2: InstallMod(false), g_doctor_ran = false; break;
    case 3: InstallMod(true), g_doctor_ran = false; break;
  }
  if (!g_proj.signs.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("The crowd signs go with it: %s_signs.svrmod beside a saved mod, Mods\\Signs when installed.", ModId().c_str());
    ImGui::PopTextWrapPos();
  }
  // the mod doctor on what's installed
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Arenas" / fs::u8path(ModId());
  std::error_code ec;
  if (!fs::exists(dir / L"manifest.txt", ec)) return;
  ImGui::Spacing();
  ImGui::BeginDisabled(Busy());
  if (ImGui::Button("Check the installed arena (mod doctor)")) {
    g_doctor = CheckModFolder("Arenas", dir);
    if (!SignsId().empty() && fs::exists(fs::path(g_game) / L"Mods" / L"Signs" / fs::u8path(SignsId()), ec))
      for (auto& p : CheckModFolder("Signs", fs::path(g_game) / L"Mods" / L"Signs" / fs::u8path(SignsId()))) g_doctor.push_back(p);
    g_doctor_ran = true;
  }
  ImGui::EndDisabled();
  if (!g_doctor_ran) return;
  if (g_doctor.empty()) {
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "The installed arena loads the way the game reads it.");
    return;
  }
  DrawProblems(g_doctor);
}

// ---- the test aid

void TestRun() {
  if (g_test_stage < 0 || g_test.empty()) return;
  if (g_test_stage == 4) {  // (install=1: once the build and the copy are done, the result into the log, then close)
    if (Busy()) return;
    g_test_stage = 3;
    Log("simple arena test: install done (the status line above says where)");
    WriteLogTo(fs::path(Wide(g_test_log)).wstring());
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    return;
  }
  if (g_test_stage == 0) {
    const std::string s = g_test[0];
    std::vector<std::string> p;
    for (size_t at = 0; at <= s.size();) {
      size_t c = s.find(':', at);
      if (c == std::string::npos) c = s.size();
      p.push_back(s.substr(at, c - at));
      at = c + 1;
    }
    if (p[0] == "tile" && p.size() > 1) {
      g_start_mode = 0;
      g_proj.arena = -1;
      StartProject(std::clamp(std::atoi(p[1].c_str()), 0, 19));
      LoadInBackground(ArenaPath(g_proj.arena), nullptr, false, false);
    } else if (p[0] == "empty" && p.size() > 1) {
      g_start_mode = 2;
      NewArena(std::clamp(std::atoi(p[1].c_str()), 0, 19), false);
    } else if (p[0] == "file" && p.size() > 3) {  // file:<item>:<host>:<path of one arena pac>
      g_start_mode = 1;
      g_native_host = std::clamp(std::atoi(p[2].c_str()), 0, 19);
      const size_t at = s.find(':', s.find(':', s.find(':') + 1) + 1) + 1;
      g_lone_file = Wide(s.substr(at)), g_disc_key.clear(), choosing = false;
      port_page::TestStart("Arenas," + s.substr(at) + "," + p[1]);
    } else if (p[0] == "disc" && p.size() > 2) {
      g_start_mode = 1;
      g_disc_key = p[1], choosing = false;
      if (p.size() > 3) g_native_host = std::clamp(std::atoi(p[3].c_str()), 0, 19);
      const std::wstring src = disc_list::PathFor(g_disc_key);
      if (src.empty()) Log("simple arena test: " + g_disc_key + " isn't in My game discs");
      port_page::TestStart("Arenas," + Utf8(src) + "," + p[2]);
    }
    g_test_stage = 1;
    Log("simple arena test: start " + s);
    return;
  }
  if (g_test_stage == 1) {
    static const auto since = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - since > std::chrono::minutes(20)) {  // (never an arena: the log says why)
      Log("simple arena test: no arena after 20 minutes - stopped");
      if (!g_test_log.empty()) WriteLogTo(fs::path(Wide(g_test_log)).wstring());
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      g_test_stage = 3;
      return;
    }
    if (!g_proj.edited || Busy() || editor::Busy()) return;
    g_test_stage = 2;
  }
  if (g_test_stage != 2) return;
  g_test_stage = 3;
  std::wstring save;
  bool install = false;
  for (size_t k = 1; k < g_test.size(); ++k) {
    const std::string& t = g_test[k];
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = t.substr(0, eq), v = t.substr(eq + 1);
    Image img;
    if (key == "name") std::snprintf(g_proj.name, sizeof g_proj.name, "%s", v.c_str()), g_name_typed = v;
    else if (key == "banner" && LoadPicture(Wide(v), img)) g_proj.banner.Set(svrfmt::Resize(img, 256, 128));
    else if (key == "load" && LoadPicture(Wide(v), img)) g_proj.load.Set(svrfmt::Resize(img, 1024, 512));
    else if (key == "sign") AddSigns({Wide(v)});
    else if (key == "step") g_simple_step = std::clamp(std::atoi(v.c_str()), 0, int(kSaCount) - 1);
    else if (key == "save") save = Wide(v);
    else if (key == "install") install = v == "1", g_test_log = v == "1" ? g_test_log : v;  // (install=1 | install=<log file>)
    else if (key == "tex") {  // <texture>:<picture> (apron: the first apron picture)
      std::string tex = v.substr(0, v.find(':')), file = v.substr(v.find(':') + 1);  // (the first colon: names have none)
      if (tex == "apron") tex = Groups()[0].names.empty() ? "" : Groups()[0].names[0];
      if (!tex.empty() && LoadPicture(Wide(file), img)) Log(std::string("simple arena test: ") + tex + (ReplaceArenaTexture(tex, img) ? " replaced" : " NOT replaced"));
      else if (!tex.empty()) Log("simple arena test: " + tex + " - not a picture it reads: " + file);
    }
  }
  if (g_test_step >= 0) g_simple_step = g_test_step;
  {  // what the steps see, into the log
    for (const auto& g : Groups()) {
      std::string names;
      for (const auto& n : g.names) names += " " + n;
      Log(std::string("simple arena test: ") + g.title + ":" + names);
    }
    std::vector<Problem> problems;
    ArenaProblems(problems);
    for (const auto& p : problems) Log(std::string("simple arena test: problem (step ") + std::to_string(p.step) + ") " + p.text);
  }
  if (install || !g_test_log.empty()) {  // (Finish's "Put it in the game": into the game's Mods\Arenas, the game not started)
    if (g_test_log.empty()) g_test_log = Utf8((save.empty() ? fs::path(L"simple_arena_install") : fs::path(save)).wstring()) + ".log";
    Log("simple arena test: install");
    InstallMod(false);
    g_test_stage = 4;
    return;
  }
  if (!save.empty()) {
    SaveModTo(save, [save](bool ok) {
      Log(std::string("simple arena test: save ") + (ok ? "ok " : "FAILED ") + Utf8(save));
      WriteLogTo(save + L".log");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    });
  }
}

}  // namespace

void TestSimple(int step) { g_simple = true, g_test_step = step, g_simple_step = std::clamp(step, 0, int(kSaCount) - 1); }
void TestSimpleStart(const std::string& spec) {
  g_simple = true;
  g_test.clear();
  for (size_t at = 0; at <= spec.size();) {
    size_t c = spec.find(';', at);
    if (c == std::string::npos) c = spec.size();
    if (c > at) g_test.push_back(spec.substr(at, c - at));
    at = c + 1;
  }
  for (const auto& t : g_test)  // (install=<log file>: known from the start - a run that never gets an arena writes it too)
    if (t.rfind("install=", 0) == 0 && t != "install=1") g_test_log = t.substr(8);
  g_test_stage = g_test.empty() ? -1 : 0;
}

void SimpleDraw() {
  TestRun();
  int& step = g_simple_step;
  step = std::clamp(step, 0, int(kSaCount) - 1);
  const float top = ImGui::GetCursorPosY();
  Heading("Your arena", "Step by step: each step has one thing to choose. Everything you skip stays as the arena you start from has it.");
  if (simple::HeadingButton("Full editor", top, false)) g_simple = false;
  std::vector<Problem> problems;
  ArenaProblems(problems);
  bool has_error[kSaCount] = {};
  simple::ErrorSteps(problems, kSaCount, has_error);
  simple::StepList(kSteps, kSaCount, step, has_error);
  ImGui::SameLine();
  const bool side_view = step != kSaLook && step != kSaStart;  // (one 3D view a frame: the Look step has the big one)
  ImGui::BeginChild("card", ImVec2(side_view ? -330 * g_scale : 0, -52 * g_scale), true);
  simple::StepTitle(kSteps[step]);
  switch (step) {
    case kSaStart: StepStart(); break;
    case kSaName: StepName(); break;
    case kSaPictures: StepPictures(); break;
    case kSaSigns: StepSigns(); break;
    case kSaRing: StepRing(); break;
    case kSaVs: StepVs(); break;
    case kSaLook: StepLook(); break;
    default: StepFinish(); break;
  }
  ImGui::EndChild();
  if (side_view) {
    ImGui::SameLine();
    ImGui::BeginChild("view", ImVec2(0, -52 * g_scale), true);
    if (g_proj.edited) {
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      editor::DrawViewport(avail.x, std::max(100.0f, avail.y - 72 * g_scale));
      if (ImGui::SmallButton("Hard camera")) editor::SetViewPreset(0);
      ImGui::SameLine();
      if (ImGui::SmallButton("Above")) editor::SetViewPreset(1);
      ImGui::SameLine();
      if (ImGui::SmallButton("Side")) editor::SetViewPreset(3);
      if (ImGui::SmallButton("Open the 3D editor")) GoTo(PageId::kEditor);
    } else {
      ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled(g_proj.arena >= 0 ? "Loading the arena ..." : "Your arena shows here once you pick where it starts from (step 1).");
      ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
  }
  simple::BottomBar(step, kSaCount, simple::StepError(problems, step));
}

}  // namespace arena_page
}  // namespace mm
