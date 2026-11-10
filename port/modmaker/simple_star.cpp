// The Superstar page's Simple mode: a step-by-step front end over the same
// project (g_star) for a first superstar. Each step is one card - a title, one
// "What is this?" line, only that step's controls - beside the 3D view; the mod
// is valid from the first step (the fighting style's superstar fills in what
// isn't chosen yet). The full page stays one click away and nothing is lost
// going back and forth.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#include "app.h"
#include "char_preview.h"
#include <cmath>
#include <memory>

#include "simple_port.h"
#include "simple_steps.h"
#include "star_page_internal.h"
#include "svrfmt/png.h"
#include "tool_run.h"

namespace mm {
namespace star_page {

void ParseMoveset(const std::string& t, char sep);  // (star_page.cpp: "0xOFF=id" items into g_star.moveset)

namespace {

using simple::Card;
using simple::CardWidth;
using simple::CardsPerRow;
using simple::HeadingButton;
using simple::Plain;

const simple::Step kSteps[kStepCount] = {
    {"Name and style", "Give your superstar a name and pick how they fight. The style gives them a full set of moves to start with."},
    {"Look", "This is what your superstar looks like: their body, face and clothes."},
    {"Attires", "Attires are outfits. A superstar can have up to 4, and you pick one before a match."},
    {"Music", "The song that plays when your superstar walks out to the ring."},
    {"Entrance", "How your superstar walks out: the moves on the way to the ring, the lights and the big screen video."},
    {"Moves", "Your superstar's best moves: the signature move and the finishers."},
    {"Taunts", "Taunts are the poses and gestures your superstar does to show off during a match."},
    {"Pictures", "The picture on the character select screen, and the pose your superstar strikes there."},
    {"Extras", "Fine-tuning: how strong your superstar is, how tall, and what the announcer calls them."},
    {"Finish", "Check everything, then save your superstar or put them in the game."},
};

// Which game superstar a model file is (pac\ch\chNNN.pac of the game folder), 0 if it isn't one.
int GameStarOf(const std::wstring& model) {
  const fs::path p(model);
  const std::wstring stem = p.stem().wstring();
  if (stem.size() < 3 || stem.rfind(L"ch", 0) != 0 || Lower(Utf8(p.parent_path().filename().wstring())) != "ch") return 0;
  for (size_t k = 2; k < stem.size(); ++k)
    if (!iswdigit(stem[k])) return 0;
  return _wtoi(stem.c_str() + 2);
}

std::wstring GameChPac(int id) { return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac")).wstring(); }

// ---- each step's sources: the game's own / your files (as before), or from another game - the porting menus
// (simple_port.h ForStar: a disc, a folder of the game's files or one file) or the converted superstar's part

int g_src[16] = {};  // per step: 0 the game's own / my files, 1 from another game

// Test aid --simple-chain "<kind>:<game>:<item>;...;save=<file>": the Simple superstar made from other games, stage
// by stage - import (the Look: the whole superstar's look), entrance / moves / taunts (that step's From another game),
// attire / pictures / music / extras (that part of the import) - then saved as a .svrmod and the Mod Maker closed.
struct ChainStage {
  std::string kind, game, item;
};
std::vector<ChainStage> g_chain;
size_t g_chain_at = 0;
bool g_chain_started = false, g_chain_done = false;
std::wstring g_chain_save;

// The cards at the top of a step; true: "From another game" is picked (the step shows that instead).
bool FromAnotherGame(int step, const char* own, const char* own_line, const char* other_line) {
  const float w = CardWidth(), h = 74 * g_scale;
  if (Card("src_own", own, own_line, g_src[step] == 0, w, h)) g_src[step] = 0;
  if (1 % CardsPerRow(w)) ImGui::SameLine(0, 8 * g_scale);
  if (Card("src_other", "From another game", other_line, g_src[step] == 1, w, h)) g_src[step] = 1;
  ImGui::Spacing();
  return g_src[step] == 1;
}

// A porting menu's result into the project (entrance / moves / taunts).
void TakeForStar(const simple_port::ForStarResult& r, int step) {
  g_chain_done = true;
  g_star.pack = simple_port::CombinePacks(g_star.pack, r.pack);
  if (r.entrance > 0) g_star.entrance = r.entrance;
  if (!r.movie.empty()) g_star.movie = r.movie;
  if (!r.lines.empty()) {
    std::string t;
    for (const auto& l : r.lines) t += l + "\n";
    ParseMoveset(t, '\n');
  }
  Touch();
  g_src[step] = 0;  // (back to the step's own view: the result shows there)
  Status(r.what + (r.entrance > 0 ? " is your superstar's now." : " are your superstar's now."));
}

// A converted superstar from another game (the Look step's converter), one part of it taken.
void PartFromAnotherGame(StarPart part, int step, int slot = 1) {
  std::wstring got;
  bool all = false;
  if (simple_port::SuperstarImport(got, all, false) && TakePartFromMod(got, part, slot)) g_src[step] = 0, g_chain_done = true;
}

void StepTitle(int step) { simple::StepTitle(kSteps[step]); }

// A step's choice still the style's: said plainly.
void FromStyle(const char* what) {
  ImGui::TextDisabled("%s: from %s (your fighting style).", what, kStyles[g_star.style].name);
}

// ---- the steps

void StepName() {
  TextField("Name", g_star.name, sizeof g_star.name, "your superstar's name", 260);
  TextField("Short name", g_star.short_name, sizeof g_star.short_name, "optional: for small spaces", 260);
  ImGui::Spacing();
  ImGui::TextUnformatted("How do they fight?");
  const float w = CardWidth(), h = 80 * g_scale;
  const int per_row = CardsPerRow(w);
  for (int i = 0; i < kStyleCount; ++i) {
    if (i % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card(kStyles[i].name, kStyles[i].name, kStyles[i].about, g_star.style == i, w, h) && g_star.style != i) {
      const std::wstring was = StyleChPac();
      g_star.style = i;
      g_star.ratings_set = false;  // (the new style's attributes)
      if (g_star.model.empty() || g_star.model == was) g_star.model = StyleChPac();  // (still the style's look: follows it)
      Touch();
    }
  }
}

char g_find[64] = "";
int g_look_mode = -1;  // the Look step's way in (-1: by the project)
int g_look_tab = -1;   // (test aid: the tab opened, 0 Model / 1 Materials)
void StepLook_();
// The Look step's Materials tab: an FBX's materials (used when it's made), else the model's parts.
void LookMaterials() {
  if (g_look_mode == 2 && !g_star.fbx.empty()) {
    FbxMaterials(true);
    FbxMakeButton(g_star.model.find(L".convert") != std::wstring::npos ? "Make the model again" : "Make the model", 240);
    ImGui::SameLine();
    ImGui::TextDisabled("(the 3D view shows the model once it's made)");
    MaterialsSection(true, false, false);
    return;
  }
  MaterialsSection(true, false, true);
}
void StepLook() {
  if (!ImGui::BeginTabBar("looktabs")) return;
  const int open = g_look_tab;
  g_look_tab = -1;
  if (ImGui::BeginTabItem("Model", nullptr, open == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
    ImGui::Spacing();
    StepLook_();
    MaterialsSection(true, false, false);  // (its edits still in the 3D view)
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Materials", nullptr, open == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
    ImGui::Spacing();
    LookMaterials();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}
void StepLook_() {
  int& mode = g_look_mode;  // 0 a game superstar, 1 an older disc, 2 an FBX
  if (mode < 0) mode = g_star.fbx.empty() ? 0 : 2;  // (a project with an FBX opens on it)
  const int now = GameStarOf(g_star.model);
  if (mode == 2 && !g_star.fbx.empty()) {  // (an FBX on its way in: the ways in as one small row - room for its steps)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("The look from:");
    static const char* kWays[] = {"A superstar in the game", "An older game", "A 3D model (FBX)"};
    const int cur = mode;
    for (int k = 0; k < 3; ++k) {
      ImGui::SameLine();
      if (k == cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.59f, 0.98f, 1));
      if (ImGui::SmallButton(kWays[k])) mode = k;
      if (k == cur) ImGui::PopStyleColor();
    }
    ImGui::Spacing();
  } else {
    const float w = CardWidth(), h = 80 * g_scale;
    const int per_row = CardsPerRow(w);
    if (Card("game", "A superstar in the game", "Use the look of anyone on the roster.", mode == 0, w, h)) mode = 0;
    if (1 % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card("disc", "From an older game", "Someone from your other WWE games, or one character file.", mode == 1, w, h)) mode = 1;
    if (2 % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card("fbx", "Import a 3D model", "Your own model from Blender, Unity, VRChat or Mixamo (FBX).", mode == 2, w, h)) mode = 2;
    ImGui::Spacing();
    if (const StarInfo* s = now ? StarById(now) : nullptr) ImGui::Text("Now: looks like %s.", s->name.c_str());
    else if (!g_star.model.empty()) ImGui::Text("Now: %s.", Lower(FileName(g_star.model)) == "ch.pac" ? "the look you brought in" : FileName(g_star.model).c_str());
    ImGui::Spacing();
  }
  if (mode == 0) {
    ImGui::SetNextItemWidth(260 * g_scale);
    ImGui::InputTextWithHint("##find", "Find a superstar...", g_find, sizeof g_find);
    ImGui::BeginChild("roster", ImVec2(0, 0), false);
    const float tw = 96 * g_scale, th = 120 * g_scale;
    const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 6 * g_scale) / (tw + 6 * g_scale)));
    int n = 0;
    const std::string f = Lower(g_find);
    for (const auto& st : Stars()) {
      if (!f.empty() && Lower(st.name).find(f) == std::string::npos) continue;
      if (n++ % per_row) ImGui::SameLine(0, 6 * g_scale);
      ImGui::PushID(st.id);
      ImGui::BeginGroup();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const bool on = now == st.id;
      if (ImGui::IsRectVisible(ImVec2(tw, th))) {  // (pictures decode on first sight: only the visible ones)
        if (Picture* pic = StarRender(st.id); pic && !pic->Empty()) pic->Draw(tw, tw);
        else ImGui::Dummy(ImVec2(tw, tw));
      } else {
        ImGui::Dummy(ImVec2(tw, tw));
      }
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
      ImGui::TextUnformatted(st.name.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndGroup();
      const ImVec2 q = ImGui::GetItemRectMax();
      if (on) ImGui::GetWindowDrawList()->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Text), 4 * g_scale, 0, 2 * g_scale);
      if (ImGui::IsItemClicked()) g_star.model = GameChPac(st.id), Touch();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look like %s", st.name.c_str());
      ImGui::PopID();
    }
    ImGui::EndChild();
  } else if (mode == 2) {
    FbxRows(true);
  } else {  // (a game from My game discs, its superstars with pictures: simple_port.cpp)
    std::wstring got;
    bool all = false;
    if (simple_port::SuperstarImport(got, all)) TakeFromMod(got, all), g_chain_done = true;
  }
  ImGui::Spacing();
  if (ImGui::SmallButton("Use a model file instead...")) {
    const std::wstring f = PickFile(false, L"A character model (ch.pac)", kPacFilter, 1);
    if (!f.empty()) g_star.model = f, Touch();
  }
}

// A popup to pick a game superstar by name (true when picked).
bool RosterPopup(const char* id, int& picked) {
  bool hit = false;
  if (ImGui::BeginPopup(id)) {
    static char find[64] = "";
    if (ImGui::IsWindowAppearing()) find[0] = 0, ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(240 * g_scale);
    ImGui::InputTextWithHint("##f", "Find a superstar...", find, sizeof find);
    ImGui::BeginChild("names", ImVec2(240 * g_scale, 300 * g_scale));
    const std::string f = Lower(find);
    for (const auto& st : Stars()) {
      if (!f.empty() && Lower(st.name).find(f) == std::string::npos) continue;
      if (ImGui::Selectable((st.name + "##" + std::to_string(st.id)).c_str())) picked = st.id, hit = true, ImGui::CloseCurrentPopup();
    }
    ImGui::EndChild();
    ImGui::EndPopup();
  }
  return hit;
}

std::string LookName(const std::wstring& pac) {
  const int id = GameStarOf(pac);
  if (const StarInfo* s = id ? StarById(id) : nullptr) return s->name + "'s";
  return FileName(pac);
}

void StepAttires_() ;

void StepAttires() {
  StepAttires_();
  ImGui::Spacing();
  if (!g_star.model.empty()) {  // (Superstar Threads: the parts players can recolour - the same editor as the full one's tab)
    ImGui::TextUnformatted("Colour parts");
    ImGui::SameLine();
    ImGui::TextDisabled("(the parts Superstar Threads lets players recolour: trunks, boots, pads...)");
    for (int a : AttireCount()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(("Attire " + std::to_string(a) + "...").c_str())) ThreadsOpen(a), GoTo(PageId::kThreads);
    }
    ImGui::Spacing();
  }
  MaterialsSection(true);
}
int g_attire_slot = 1;  // (from another game: the attire it goes in, 1-3 = attire 2-4)
void StepAttires_() {
  if (FromAnotherGame(2, "The game's or my files", "Another 2011 superstar's look, or a model file.",
                      "A superstar's look from your other games, as one of your attires.")) {
    ImGui::TextUnformatted("As attire");
    for (int a = 1; a < 4; ++a) {
      ImGui::SameLine();
      if (ImGui::RadioButton(std::to_string(a + 1).c_str(), g_attire_slot == a)) g_attire_slot = a;
    }
    PartFromAnotherGame(StarPart::kAttire, 2, g_attire_slot);
    return;
  }
  const std::vector<int> own = OwnAttires();
  auto own_has = [&](int n) {  // (the game counts them in order from 1)
    for (int k = 1; k <= n; ++k)
      if (std::find(own.begin(), own.end(), k) == own.end()) return false;
    return true;
  };
  ImGui::TextUnformatted("Attire 1 is your superstar's look (the Look step).");
  ImGui::Spacing();
  for (int a = 1; a < 4; ++a) {
    ImGui::PushID(a);
    const bool from_model = g_star.attires[a].empty() && own_has(a + 1);
    const std::string now = !g_star.attires[a].empty() ? LookName(g_star.attires[a]) + " outfit"
                            : from_model                ? std::string("the look's own attire ") + std::to_string(a + 1)
                                                        : std::string("none");
    ImGui::Text("Attire %d:", a + 1);
    ImGui::SameLine(90 * g_scale);
    ImGui::TextUnformatted(now.c_str());
    ImGui::Indent(90 * g_scale);
    if (ImGui::SmallButton("Another superstar's...")) ImGui::OpenPopup("roster");
    int picked = 0;
    if (RosterPopup("roster", picked)) g_star.attires[a] = GameChPac(picked), Touch();
    ImGui::SameLine();
    if (ImGui::SmallButton("A model file...")) {
      const std::wstring f = PickFile(false, L"A character model (ch.pac): its first attire", kPacFilter, 1);
      if (!f.empty()) g_star.attires[a] = f, Touch();
    }
    bool removed = false;
    if (!g_star.attires[a].empty() || from_model) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) RemoveAttire(a + 1), removed = true;  // (the later ones move up: shown next frame)
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Takes attire %d out of the mod%s; the attires after it move up one.", a + 1,
                          from_model ? " (cut from the model when you save)" : "");
    }
    if (!removed && (!g_star.attires[a].empty() || from_model)) {
      ImGui::SameLine();
      if (ImGui::SmallButton("See it")) g_view_attire = a + 1;
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::InputTextWithHint("##name", ("its name (ATTIRE " + std::to_string(a + 1) + ")").c_str(), g_star.attire_names[a],
                                   sizeof g_star.attire_names[a]))
        Touch();
    }
    ImGui::Unindent(90 * g_scale);
    ImGui::Spacing();
    ImGui::PopID();
    if (removed) break;  // (the list changed: drawn again next frame)
    if (g_star.attires[a].empty() && !from_model) break;  // (in order: the next one after this)
  }
  ImGui::Separator();
  GearRows(true);
}

void StepMusic() {
  if (FromAnotherGame(3, "A game theme or my song", "One of 2011's themes, or a song file of yours.",
                      "Their theme and titantron video from that game.")) {
    PartFromAnotherGame(StarPart::kMusic, 3);
    return;
  }
  FileRow("Pick a song...", g_star.song, g_star.music > 0 ? "the game's theme below" : "the style's", kSoundFilter, 1,
          "An MP3, WAV, OGG or other sound file.");
  if (!g_star.song.empty() && g_star.music) g_star.music = 0;  // (a song file picked: it wins)
  ImGui::TextDisabled("or");
  StockThemeRow();
  if (g_star.song.empty() && !g_star.music) FromStyle("Music");
  ImGui::Spacing();
  ThemeGainRow();
  if (!g_star.song.empty()) ImGui::TextDisabled("Tip: \"Match the game\" makes it as loud as the game's own songs.");
}

// The entrance's walk to the ring on the model: tools/entrance_ring.py turns entrance n's own track bank (its
// pac\evt Nyujyo pac, or a mod's move pack) into a YMKs bank keyed as motion n, x = segment (4 = the ring part).
int g_ring_shown = -1;  // the entrance the preview shows
void WatchEntrance(int n) {
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"entrance_ring";
  std::error_code ec;
  fs::create_directories(work, ec);
  const std::vector<std::wstring> args = {std::to_wstring(n), L"--game-pac", (fs::path(g_game) / L"pac").wstring(), L"--mods",
                                          (fs::path(g_game) / L"Mods").wstring(), L"--out", work.wstring()};
  const bool started = RunToolJson(L"entrance_ring.py", args, [n, work](int code, const std::string& text) {
    svrfmt::Bytes b;
    if (code != 0 || !svrfmt::ReadFile(PathStr(work / (L"entrance_" + std::to_wstring(n) + L".ymks")), b)) {
      Status(code == 2 ? "That entrance has no walk to show." : "The entrance couldn't be read (see the log).");
      Log("entrance_ring: " + text);
      return;
    }
    auto bank = std::make_shared<const std::vector<uint8_t>>(std::move(b));
    std::string err;
    char_preview::SetDummy(L"");
    if (!char_preview::SetMotion(bank, n, 4, &err)) return Status("The entrance can't be shown: " + err + ".");
    char_preview::Play().loop = true;
    g_ring_shown = n;
    Status("The entrance's walk to the ring.");
  });
  if (!started) Status("The entrance preview needs the Mod Maker tools (Python): see the log.");
}

void StepEntranceOwn();
void StepEntrance() {
  if (FromAnotherGame(4, "The game's entrances", "One of 2011's, or one you added.",
                      "A superstar's entrance from your other games: a disc, a folder or one file.")) {
    simple_port::ForStarResult r;
    if (simple_port::ForStar(simple_port::kEntrance, r)) TakeForStar(r, 4);
    return;
  }
  StepEntranceOwn();
}
void StepEntranceOwn() {
  if (EntrancePicker("Entrance", g_star.entrance, "The style's", 360)) Touch();
  if (g_star.entrance < 0) FromStyle("Entrance");
  {
    const int n = g_star.entrance >= 0 ? g_star.entrance : kStyles[g_star.style].template_id;  // (the style's: its superstar's)
    ImGui::BeginDisabled(Busy());
    if (ImGui::Button("Watch the entrance", ImVec2(240 * g_scale, 0))) {
      if (char_preview::HasGear()) char_preview::SetGear(true);  // (worn for the entrance, as in the game)
      OpenFullPreview(n);  // (the Full preview: all five parts, placed as in the game, from the stage to the ring)
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("A window with the whole entrance in the practice arena - stage, ramp and ring - with the entrance's camera, your "
                        "theme and big screen video (\"Skip to the ring\" there jumps to the walk to the ring; the pyro only shows in the game).");
  }
  ImGui::Spacing();
  GearRows(true);  // (each attire's entrance gear; "Entrance gear" under the 3D view shows or hides it)
  ImGui::Separator();
  ImGui::TextUnformatted("Big screen video (optional)");
  FileRow("Pick a video file...", g_star.movie, "the style's", kBinkFilter, 1, "A video made for the game's big screen (.mp4 or .bik).");
  ImGui::SameLine();
  BinkFromVideoButton("Make one from any video...", g_star.movie, g_star.name);
  MovieSizeRow(g_star.movie);
}

// A move-set slot as a row: its move (yours or the style's), Change..., Watch it, the style's again.
void SlotRow(int slot, const char* label, bool two_people) {
  ImGui::PushID(slot);
  const std::vector<int> base = ProfileMoves(kStyles[g_star.style].template_id);
  const auto mine = g_star.moveset.find(slot);
  const int was = slot < int(base.size()) ? base[size_t(slot)] : 0;
  const int now = mine != g_star.moveset.end() ? mine->second : was;
  ImGui::TextUnformatted(label);
  ImGui::SameLine(150 * g_scale);
  ImGui::TextUnformatted(now ? MoveName(now).c_str() : "-");
  if (mine == g_star.moveset.end()) ImGui::SameLine(), ImGui::TextDisabled("(the style's)");
  else MoveNoteTag(slot);
  ImGui::Indent(150 * g_scale);
  if (ImGui::SmallButton("Change...")) g_pick_slot = slot, g_pick_find[0] = 0, g_pick_any = false;
  ImGui::SameLine();
  ImGui::BeginDisabled(!now || !anims_page::HasMotion(now) || Busy());
  if (ImGui::SmallButton("Watch it")) {
    char_preview::SetDummy(two_people ? StyleChPac() : std::wstring());
    anims_page::WatchMove(now);
    g_watching = true;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && now && !anims_page::HasMotion(now))
    ImGui::SetTooltip(anims_page::MovesIndexed() ? "This move can only be watched in the game." : "Reading the game's moves ...");
  if (mine != g_star.moveset.end()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Use the style's")) g_star.moveset.erase(slot), g_star.moveset_note.erase(slot), Touch();
  }
  ImGui::Unindent(150 * g_scale);
  ImGui::Spacing();
  ImGui::PopID();
}

void StepMoves() {
  if (FromAnotherGame(5, "The game's moves", "Change a move to any of 2011's.",
                      "A superstar's moves from your other games: a disc or a folder of its files.")) {
    simple_port::ForStarResult r;
    if (simple_port::ForStar(simple_port::kMoves, r)) TakeForStar(r, 5);
    return;
  }
  SlotRow(0x1E / 2, "Signature move", true);
  SlotRow(0x20 / 2, "Signature move 2", true);
  SlotRow(0x22 / 2, "Finisher", true);
  SlotRow(0x24 / 2, "Finisher 2", true);
  MovePicker();
  ImGui::Spacing();
  ImGui::TextDisabled("All the other moves come from %s. The full editor's Moveset changes any of them.", kStyles[g_star.style].name);
}

void StepTaunts() {
  if (FromAnotherGame(6, "The game's taunts", "Change a taunt to any of 2011's.",
                      "A superstar's taunts from your other games: a disc or a folder of its files.")) {
    simple_port::ForStarResult r;
    if (simple_port::ForStar(simple_port::kTaunts, r)) TakeForStar(r, 6);
    return;
  }
  for (int k = 0; k < 4; ++k) SlotRow((0x12 + 2 * k) / 2, ("Taunt " + std::to_string(k + 1)).c_str(), false);
  MovePicker();
}

void StepPictures() {
  if (FromAnotherGame(7, "The 3D view or my picture", "Take it from the 3D view, or a picture of yours.",
                      "Their pictures, made by the converter from that game's renders.")) {
    PartFromAnotherGame(StarPart::kPictures, 7);
    return;
  }
  ImGui::TextUnformatted("Pose on the select screen");
  ImGui::SetNextItemWidth(300 * g_scale);
  const std::string cur = g_star.pose < 0 ? "The style's" : PoseLabel(g_star.pose);
  if (ImGui::BeginCombo("##pose", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable("The style's", g_star.pose < 0)) g_star.pose = -1, Touch();
    for (const auto& c : Poses())
      if (ImGui::Selectable((c.label + "##" + std::to_string(c.id)).c_str(), g_star.pose == c.id)) g_star.pose = c.id, Touch(), PreviewPose(c.id);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(g_star.pose < 0 || Busy());
  if (ImGui::SmallButton("Show it")) PreviewPose(g_star.pose);
  ImGui::EndDisabled();
  ImGui::Spacing();
  ImGui::TextUnformatted("The select screen picture");
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("Turn and zoom the 3D view, play the pose, then take it - or use a picture of your own (a PNG with a see-through background works best).");
  ImGui::PopTextWrapPos();
  PushAccent();
  if (ImGui::Button("Take it from the 3D view", ImVec2(220 * g_scale, 0))) {
    std::vector<uint8_t> rgba;
    if (char_preview::Capture(768, rgba)) {
      Image img;
      img.w = img.h = 768;
      img.rgba = std::move(rgba);
      g_star.picture_src = img;
      SetPicture(img);
      Status("The select picture is the 3D view's.");
    } else {
      Status("The 3D view has nothing to take yet.");
    }
  }
  PopAccent();
  ImGui::SameLine();
  if (ImGui::Button("Use my own picture...", ImVec2(200 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"The select screen picture", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) g_star.picture_src = img, SetPicture(img);
  }
  if (!g_star.picture.Empty()) {
    ImGui::Spacing();
    g_star.picture.Draw(180 * g_scale, 180 * g_scale);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("How the select screen shows it.");
    if (ImGui::Checkbox("Game look", &g_star.game_look) && !g_star.picture_src.rgba.empty()) SetPicture(g_star.picture_src);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the colours toward the game's own pictures.");
    if (ImGui::SmallButton("Remove")) g_star.picture.Clear(), g_star.picture_small = Image(), Touch();
    ImGui::EndGroup();
  }
}

void StepExtras() {
  if (FromAnotherGame(8, "Set them here", "Strength, height and what the announcer says.",
                      "Their strength, height, abilities and announcer lines from that game.")) {
    PartFromAnotherGame(StarPart::kExtras, 8);
    return;
  }
  ImGui::Text("Strength - overall %d", OverallRating(g_star.ratings));
  for (int k = 0; k < 7; ++k) {
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::SliderInt(kAttributes[k], &g_star.ratings[k], 1, 99)) g_star.ratings_set = true, Touch();
  }
  ImGui::Spacing();
  ImGui::SetNextItemWidth(260 * g_scale);
  if (ImGui::SliderFloat("Height", &g_star.height, 0.8f, 1.25f, "x%.2f")) Touch();
  if (const float tall = char_preview::ModelHeight(); tall > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("about %d cm", int(std::lround(tall * 10 * StarScale(kStyles[g_star.style].template_id) * g_star.height)));
  }
  ImGui::SetNextItemWidth(260 * g_scale);
  if (ImGui::BeginCombo("Announcer calls them", g_star.call < 0 ? "The Superstar" : kNickNames[g_star.call])) {
    if (ImGui::Selectable("The Superstar##d", g_star.call < 0)) g_star.call = -1, Touch();
    for (int i = 0; i < 84; ++i)
      if (ImGui::Selectable((std::string(kNickNames[i]) + "##n" + std::to_string(i)).c_str(), g_star.call == i)) g_star.call = i, Touch();
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("More (abilities, hometown, the announcer's own name clips) in the full editor.");
}

void StepFinish() {
  ImGui::TextUnformatted("Your superstar");
  ImGui::Indent();
  auto row = [](int step, const std::string& text) { simple::SummaryRow(kSteps, step, text, g_star.simple_step); };
  row(kStepName, std::string(g_star.name[0] ? g_star.name : "(no name yet)") + ", " + kStyles[g_star.style].name);
  const int now = GameStarOf(g_star.model);
  const StarInfo* s = now ? StarById(now) : nullptr;
  row(kStepLook, s ? "looks like " + s->name : g_star.model.empty() ? std::string("(none yet)") : FileName(g_star.model));
  {
    int extra = 0;
    for (int a = 1; a < 4; ++a) extra += !g_star.attires[a].empty();
    const int own = int(OwnAttires().size());
    row(kStepAttires, std::to_string(std::max(own, 1 + extra)) + " attire" + (std::max(own, 1 + extra) == 1 ? "" : "s"));
  }
  row(kStepMusic, !g_star.song.empty() ? FileName(g_star.song) : g_star.music > 0 ? "the game's theme: " + StockThemeName(g_star.music) : std::string("the style's"));
  row(kStepEntrance, g_star.entrance < 0 ? std::string("the style's") : "entrance " + std::to_string(g_star.entrance));
  {
    int moves = 0, taunts = 0;
    for (const auto& [slot, id] : g_star.moveset) (slot * 2 >= 0x12 && slot * 2 <= 0x18 ? taunts : moves) += 1;
    row(kStepMoves, moves ? std::to_string(moves) + " of your own, the rest the style's" : std::string("the style's"));
    row(kStepTaunts, taunts ? std::to_string(taunts) + " of your own" : std::string("the style's"));
  }
  row(kStepPictures, g_star.picture.Empty() ? std::string("none yet (a silhouette)") : std::string("your picture") + (g_star.pose >= 0 ? ", your pose" : ""));
  ImGui::Unindent();
  ImGui::Spacing();
  std::vector<Problem> problems;
  Problems(problems);
  simple::ProblemList(problems, g_star.simple_step, kStepFinish);
  ImGui::Spacing();
  ImGui::Spacing();
  switch (ModButtons(problems, true, (ImGui::GetContentRegionAvail().x - 16 * g_scale) / 3 / g_scale)) {
    case 1: SaveMod(); break;
    case 2: Install(false); break;
    case 3: Install(true); break;
  }
}

}  // namespace

void TestSimpleLook(int mode) { g_look_mode = mode; }
void TestLookTab(int tab) { g_look_tab = tab; }
std::wstring g_test_capture;  // --simple-capture <png>: the 3D view taken as the select picture, saved, quit
void TestSimpleCapture(const std::wstring& png) { g_test_capture = png; }
bool g_test_simple_chain = false;
int g_test_ring = -1;  // --simple-ring <entrance>: its walk to the ring in the preview (with --simple-capture: taken mid-walk)
void TestSimpleRing(int n) { g_test_ring = n; }
void TestSimpleChain(const std::string& spec) {
  g_chain.clear(), g_chain_at = 0, g_chain_started = false;
  for (size_t a = 0, b; a <= spec.size(); a = b + 1) {
    b = spec.find(';', a);
    if (b == std::string::npos) b = spec.size();
    const std::string part = spec.substr(a, b - a);
    if (part.rfind("save=", 0) == 0) {
      g_chain_save = Wide(part.substr(5));
      continue;
    }
    const size_t c1 = part.find(':'), c2 = c1 == std::string::npos ? c1 : part.find(':', c1 + 1);
    if (c2 == std::string::npos) continue;
    g_chain.push_back({part.substr(0, c1), part.substr(c1 + 1, c2 - c1 - 1), part.substr(c2 + 1)});
  }
  g_test_simple_chain = true;
}
void TestSimpleFrom(int step) {
  if (step >= 0 && step < 16) g_src[step] = 1;
}

void ChainTick() {
  if (g_chain_at >= g_chain.size()) return;
  int& step = g_star.simple_step;
  const ChainStage& c = g_chain[g_chain_at];
  static const std::pair<const char*, int> kSteps[] = {{"import", 1}, {"attire", 2}, {"music", 3}, {"entrance", 4},
                                                       {"moves", 5},  {"taunts", 6}, {"pictures", 7}, {"extras", 8}};
  int at = -1;
  for (const auto& [name, n] : kSteps)
    if (c.kind == name) at = n;
  if (at < 0) {
    Log("simple superstar test: no stage " + c.kind), g_chain_at = g_chain.size();
    return;
  }
  if (!g_chain_started) {
    g_chain_started = true, g_chain_done = false;
    step = at;
    Log("simple superstar test: stage " + c.kind + " " + c.game + " " + c.item);
    if (c.kind == "import") {
      g_look_mode = 1;
      simple_port::TestStart("import," + c.game + "," + c.item + ",0,add");
    } else if (c.kind == "entrance" || c.kind == "moves" || c.kind == "taunts") {
      g_src[at] = 1;
      simple_port::ForStarTest(c.kind == "entrance" ? simple_port::kEntrance : c.kind == "moves" ? simple_port::kMoves : simple_port::kTaunts,
                               c.game, c.item);
    } else {
      g_src[at] = 1;
      simple_port::TestStart("import," + c.game + "," + c.item + ",0,add");
    }
    return;
  }
  static double since = 0;  // (a stage that never ends: logged, the Mod Maker closed)
  if (!since) since = ImGui::GetTime();
  if (!g_chain_done && ImGui::GetTime() - since > 900) {
    Log("simple superstar test: stage " + c.kind + " didn't finish in 15 minutes");
    g_chain_at = g_chain.size();
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    return;
  }
  if (!g_chain_done || Busy()) return;
  since = 0;
  Log("simple superstar test: stage " + c.kind + " done");
  g_chain_started = false;
  if (++g_chain_at < g_chain.size()) return;
  if (!g_chain_save.empty()) TestSaveMod(g_chain_save);
  PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

void SimpleDraw() {
  ChainTick();
  int& step = g_star.simple_step;
  step = std::clamp(step, 0, int(kStepCount) - 1);
  if (g_star.model.empty()) g_star.model = StyleChPac();  // (valid from the first step: the style's look)
  if (!g_star.ratings_set) {
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StarRating(kStyles[g_star.style].template_id, k);
    g_star.ratings_set = true;
  }
  {  // (brought in with everything: their moves come after the look - simple_port.cpp)
    std::wstring pack;
    std::vector<std::string> lines;
    if (simple_port::SuperstarImportMoves(pack, lines)) {
      if (!pack.empty()) g_star.pack = pack;
      std::string t;
      for (const auto& l : lines) t += l + "\n";
      ParseMoveset(t, '\n');
      Touch();
      Status(std::to_string(lines.size()) + " of their moves are in.");
    }
  }
  const float top = ImGui::GetCursorPosY();
  Heading("Your superstar", "Step by step: each step has one thing to choose. Everything you skip comes from your fighting style.");
  if (HeadingButton("Full editor", top, false)) g_star.simple = false;
  std::vector<Problem> problems;
  Problems(problems);
  bool step_has_error[kStepCount] = {};
  simple::ErrorSteps(problems, kStepCount, step_has_error);
  simple::StepList(kSteps, kStepCount, step, step_has_error);  // (the steps, left)
  ImGui::SameLine();

  // the step's card (middle)
  ImGui::BeginChild("card", ImVec2(-330 * g_scale, -52 * g_scale), true);
  StepTitle(step);
  switch (step) {
    case kStepName: StepName(); break;
    case kStepLook: StepLook(); break;
    case kStepAttires: StepAttires(); break;
    case kStepMusic: StepMusic(); break;
    case kStepEntrance: StepEntrance(); break;
    case kStepMoves: StepMoves(); break;
    case kStepTaunts: StepTaunts(); break;
    case kStepPictures: StepPictures(); break;
    case kStepExtras: StepExtras(); break;
    default: StepFinish(); break;
  }
  ImGui::EndChild();
  ImGui::SameLine();

  static int capture_frames = 0;
  static bool ring_asked = false;
  if (g_test_ring >= 0 && !ring_asked && char_preview::ModelHeight() > 0 && !Busy()) ring_asked = true, WatchEntrance(g_test_ring);
  if (g_test_ring >= 0 && g_ring_shown >= 0 && char_preview::Play().key < 300) char_preview::Play().key = 300;  // (mid-walk)
  if (!g_test_capture.empty() && char_preview::ModelHeight() > 0 && (g_test_ring < 0 || g_ring_shown >= 0) && ++capture_frames > 30) {
    std::vector<uint8_t> rgba;
    Image img;
    img.w = img.h = 512;
    if (char_preview::Capture(512, rgba)) {
      img.rgba = std::move(rgba);
      SetPicture(img);
      svrfmt::SavePng(Utf8(g_test_capture), img);
      if (!g_star.picture.Empty()) svrfmt::SavePng(Utf8(g_test_capture) + ".render.png", g_star.picture.image);
    }
    g_test_capture.clear();
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  }
  // the 3D view (right): drag to turn, wheel to zoom
  ImGui::BeginChild("view", ImVec2(0, -52 * g_scale), true);
  Preview();  // (its footer: Reset view, Blood, Damage)
  ImGui::EndChild();

  simple::BottomBar(step, kStepCount, simple::StepError(problems, step));  // (Back / Next: Next waits for the step's error)
}

}  // namespace star_page
}  // namespace mm
