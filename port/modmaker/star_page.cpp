// Superstar page: a new playable character under the character select's
// M tile (the game: superstar_mods.cpp, docs/SUPERSTAR_MODS.md). A fighting
// style picks the roster template its moves, entrance motions and starting
// attributes come from; the mod brings its own model pac, name, theme song,
// entrance movie, attires, select picture, crowd signs and name recording.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <functional>
#include <memory>
#include <set>
#include <thread>

#include "app.h"
#include "audio_out.h"
#include "mod_check.h"
#include "char_preview.h"
#include "json.h"
#include "pac_attires.h"
#include "star_page_internal.h"
#include "full_preview.h"
#include "rig_map.h"
#include "svrfmt/char_fbx.h"
#include "svrfmt/pac.h"
#include "tool_run.h"
#include "svrfmt/png.h"
#include "disc_list.h"
#include "simple_port.h"
#include "imgui_internal.h"

namespace mm {
namespace star_page {

using namespace svrfmt;

const Style kStyles[kStyleCount] = {
    {"Powerhouse", "Big, slow and strong: power slams and bear hugs.", 125, false},
    {"Brawler", "Strikes, clotheslines and a power finisher.", 160, false},
    {"All-rounder", "A bit of everything: the main-event style.", 139, false},
    {"Technical", "Holds, counters and a submission finisher.", 104, false},
    {"Submission specialist", "Wears opponents down with holds.", 267, false},
    {"High flyer", "Fast, light, off the top rope.", 123, false},
    {"Showman", "Flashy moves and taunts.", 218, false},
    {"Diva: powerhouse", "A strong Diva: slams and power moves.", 224, true},
    {"Diva: high flyer", "A quick, acrobatic Diva.", 164, true},
    {"Diva: technical", "A Diva who wrestles holds and counters.", 143, true},
};



// The Created Superstar nicknames the announcer and commentary can say
// (call=<index>; the game's CAS_* list, sub_8261EAB0's table at 0x82043F98).
const char* const kNickNames[84] = {
    "Alpha", "Andersen", "Angel", "Angus", "Archangel", "Azrael", "Bison", "Brittany", "Chen", "Dregs", "Dynamite",
    "El Jefe", "Franco", "Gonzalez", "Griffin", "Grime", "Hero", "Icon", "Jessica", "Jester", "Justice", "Kowalczyk",
    "Lassiter", "Lee", "Lilith", "Mantis", "Matsuda", "Matsumoto", "Maverick", "Mercer", "Amazing", "Black", "Macho",
    "Omega", "Quinn", "Rodriguez", "Santos", "Savior", "Silva", "Skinner", "Sokolov", "Sophia", "Sullivan", "Sunshine",
    "The Bad Guy", "The Barbarian", "The Bruiser", "Champ", "The Cowboy", "The Disaster", "The Dog", "The Dominator",
    "The Future", "The Gangster", "The Hardcore Icon", "(no commentary)", "The King", "The Maniac", "The Masked Man",
    "The Mastodon", "The Mechanic", "The Monster", "The Motor", "The Natural", "The Nightmare", "The Ninja",
    "The Olympian", "The Phenom", "The Prince", "The Princess", "The Professor", "The Rocker", "The Samoan",
    "The Samurai", "The Scorpion", "The Show", "The Soldier", "The Superstar", "The Tornado", "Thunder", "Vega",
    "Williams", "Yosef", "Youngblood"};

const char* const kAttributes[7] = {"Grapple", "Submission", "Speed", "Strikes", "Hardcore", "Charisma", "Durability"};

// Abilities (record +230..: up to 8 ids; the name's string id is 0x9EA8 + id,
// sub_828B73D8). The eleven the 2011 roster uses; the other ids in the string
// table are SvR 2009/2010 leftovers no record has.
struct Ability {
  int id;
  const char* name;
  const char* about;
};
const Ability kAbilities[] = {
    {1, "Dirty Pin", "Pins with the feet on the ropes or a handful of tights."},
    {7, "Move Thief", "Steals the opponent's finisher."},
    {9, "Hammer Throw", "Irish whips with more force: the opponent hits harder."},
    {11, "Resiliency", "Kicks out of a pin once when nearly beaten."},
    {12, "Durability", "Takes longer to wear down."},
    {14, "Kip-Up", "Springs back to the feet when down."},
    {19, "Outside Dives", "Dives to the outside of the ring."},
    {20, "Springboard Dives", "Springboard attacks off the ropes."},
    {22, "Leverage Pin", "A quick pin from a reversal."},
    {23, "Fired Up", "Finishers pass to a partner / a comeback when down."},
    {24, "Ring Escape", "Rolls out of the ring to recover."},
};

StarProject g_star;
void GoStep(int step);  // (below: a fix that needs a choice opens its step in Simple mode)
std::string Who();
std::wstring StyleChPac() {
  return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(kStyles[g_star.style].template_id) + L".pac")).wstring();
}
// test aids: --star <id> picks the model, --star-* files, --test-star-save <file> saves the mod and quits
int g_test_id = 0;
extern std::string g_test_cae;  // (--star-cae: TestCae)
struct MadeEntrance {  // (a created entrance the game listed: MadeEntrances)
  int id = 0;
  std::string name;
};
std::vector<MadeEntrance> MadeEntrances();
bool UseMadeEntrance(int id, const std::string& name);
void CreatedEntranceRow();
extern std::string g_test_import;  // (--star-import-moveset: TestImportMoveset)
void RunTestImport(const std::string& spec);
std::wstring g_test_model, g_test_picture, g_test_save, g_test_full, g_test_load;
std::string g_test_hometown;
int g_test_pose = -1;
std::string g_test_name;
std::wstring g_test_fbx;  // --star-fbx <fbx>[,<fbx_map>]
int g_test_simple = -1;   // --simple <step>
std::string g_test_gear;  // --star-gear <attire>:<op>,...
std::string g_test_gear_fbx;  // --star-gear-fbx <attire>:<fbx>
std::string g_test_gear_from;  // --star-gear-from <attire>:<donor id>: the transplant, the log to <work>\gear.log, quit
std::wstring g_test_take;  // --simple-take <svrmod>,<0 look | 1 everything>
bool g_test_take_all = false;
bool g_test_fbx_make = false;
std::wstring g_test_fbx_report;  // --fbx-report <out>  // --star-fbx-make: Make the model from the FBX, the log to <work>\make.log, quit
std::string g_test_fbx_map;
bool g_test_done = false;

// Cheap per-file checks for the Problems list (cached by path + size + time).
struct FileCheck {
  bool exists = false, ok = false;
  size_t size = 0;
  int w = 0, h = 0;  // (a movie's)
  std::string note;
};
const FileCheck& Check(const std::wstring& path, const char* kind) {
  static std::map<std::wstring, std::pair<fs::file_time_type, FileCheck>> cache;
  std::error_code ec;
  const auto t = fs::last_write_time(path, ec);
  auto it = cache.find(path);
  if (it != cache.end() && (ec || it->second.first == t)) return it->second.second;
  FileCheck c;
  c.exists = !ec;
  if (c.exists) {
    c.size = size_t(fs::file_size(path, ec));
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
      uint8_t head[32] = {};
      const size_t n = std::fread(head, 1, sizeof head, f);
      std::fclose(f);
      if (!std::strcmp(kind, "pac")) {
        c.ok = n >= 4 && !std::memcmp(head, "EPK8", 4) && c.size >= 0x4000;
        if (!c.ok) c.note = n >= 4 && !std::memcmp(head, "EPAC", 4) ? "an EPAC (an arena or menu pac), not a character model pac (EPK8)"
                                                                      : "not a character model pac (EPK8)";
      } else if (!std::strcmp(kind, "bik") && n >= 8 && !std::memcmp(head + 4, "ftyp", 4)) {
        c.ok = true;  // (an .mp4: the game plays it in HD)
      } else if (!std::strcmp(kind, "bik")) {
        c.ok = n >= 28 && !std::memcmp(head, "BIK", 3);
        if (c.ok) c.w = int(Le32(head + 20)), c.h = int(Le32(head + 24));
        else c.note = "not a Bink (.bik) movie";
      } else {
        c.ok = n > 0;
      }
    }
  }
  cache[path] = {t, c};
  return cache[path].second;
}

std::string StarId() { return IdFrom(g_star.name, "superstar"); }

void SetPicture(const Image& src) {
  Image render, bust, icon;
  MakeRenders(g_star.game_look ? GameLook(src) : src, render, bust, icon);
  g_star.picture.Set(std::move(render));
  if (!g_star.own_bust) g_star.picture_small = std::move(bust);
  Touch();
}

// The model pac's own portrait (every attire PACH's children 200 / 201: a 256 x 256 DXT5 DDS, BPE): the bust.
// Converters fill it from the other game's bust; a stock pac copy would show that superstar.
bool SetPortrait(Bytes& pac, const Image& bust) {
  svrfmt::Epac e;
  if (!bust.w || !svrfmt::EpacRead(pac, e)) return false;
  const Bytes dds = svrfmt::BpeEncode(DdsEncode(bust.w == 256 && bust.h == 256 ? bust : Resize(bust, 256, 256), DxtFormat::kDxt5, false));
  int n = 0;
  for (auto& g : e.groups)
    for (auto& en : g.entries) {
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::IsPach(en.data) || !svrfmt::PachRead(en.data, kids)) continue;
      bool hit = false;
      for (auto& k : kids)
        if (k.id == 200 || k.id == 201) k.data = dds, hit = true;
      if (hit) en.data = svrfmt::PachWrite(kids), ++n;
    }
  if (!n) return false;
  pac = svrfmt::EpacWrite(e);
  return true;
}

// Everything the build needs, copied off the UI thread.
constexpr int kReduceMax = 120000;  // (Reduce detail's top: the big-model memory, tools/model_budget.py LIMIT - a
                                    // 124,000-vertex model took 1,489 of its 1,600 pages)

struct Job {
  std::string needs;    // (kNeedsVersion for a model over the old 192 pages, written into the manifest)
  std::string project;  // (ModProjectText: the page's project keys, into the mod)
  std::map<int, int> moveset;
  std::string id, manifest;
  std::wstring model, song, victory_song, movie, voice, cae, attires[4];
  std::string threads;  // (the Threads parts tab's threads.txt)
  std::string attire_names[4];
  Image picture, picture_small, picture_full, picture_load;
  std::vector<Image> signs;
  std::wstring moves, pack;
  std::map<int, std::string> gear;  // (entrance gear choices)
  std::map<MatKey, MatEdit> mats;   // (the material panel's edits)
};

// ---- entrances (D5)

// An entrance number picked from the installed ones (stock + packs); -1 = the default.
bool g_open_entrance_picker = false;  // (a fix: "Pick a 2011 entrance" opens the superstar's entrance list)

bool EntrancePicker(const char* label, int& value, const char* none, float width) {
  if (g_open_entrance_picker && !std::strcmp(label, "Entrance")) {
    g_open_entrance_picker = false;
    ImGui::SetScrollHereY(0.3f);
    ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID(label)));
  }
  std::string cur = none;
  if (value > 0) {
    cur = std::to_string(value);
    for (const auto& e : Entrances())
      if (e.number == value) cur += "  " + (e.name.empty() ? std::string("(no name)") : e.name) + (e.from.empty() ? "" : "  [" + e.from + "]");
  }
  bool changed = false;
  ImGui::SetNextItemWidth(width * g_scale);
  if (ImGui::BeginCombo(label, cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    static char find[32] = "";
    if (ImGui::IsWindowAppearing()) find[0] = 0, ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##f", "find a number or a name", find, sizeof find);
    if (ImGui::Selectable(none, value <= 0)) value = -1, changed = true;
    if (!EntrancesReady()) ImGui::TextDisabled("Reading the entrances ...");
    const std::string f = Lower(find);
    for (const auto& e : Entrances()) {
      const std::string row = std::to_string(e.number) + "  " + (e.name.empty() ? "(no name)" : e.name) + (e.from.empty() ? "" : "  [" + e.from + "]");
      if (!f.empty() && Lower(row).find(f) == std::string::npos) continue;
      if (ImGui::Selectable((row + "##" + std::to_string(e.number)).c_str(), value == e.number)) value = e.number, changed = true;
    }
    ImGui::EndCombo();
  }
  return changed;
}

// The model pac carries an entrance outfit: an EMD entry of kind 4 for its first attire ("%06d%02d", last digits 04).
bool HasEntranceOutfit(const std::wstring& pac) {
  static std::map<std::wstring, bool> cache;
  const auto it = cache.find(pac);
  if (it != cache.end()) return it->second;
  PacIndex idx;
  bool has = false;
  if (ReadPacIndex(fs::path(pac), idx))
    for (const auto& e : idx.entries)
      if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(6, 2, "04") == 0) has = true;
  return cache[pac] = has;
}

// ---- the moveset by name (D4)

struct SlotNameRow {
  std::string menu, slot;
  bool fixed = false;  // the same move for the whole roster: not shown
};
std::map<int, SlotNameRow> g_slot_names;  // slot -> label (tools/pro_slots.json, when there)
bool g_slot_names_loaded = false;
char g_moveset_find[48] = "";
bool g_moveset_all = false;  // every slot, or the changed ones
bool g_moveset_open_once = false;  // (test aid: the section open)
bool g_watching = false;            // a slot's move plays in the preview
int g_watch_slot_test = -1;         // (test aid --star-watch: this slot's move, once the moves are indexed)
int g_pick_slot = -1;
char g_pick_find[48] = "";
bool g_pick_any = false;

// tools/announcer_calls.json (Extras): the ring announcer's clips - cities (HTC), states (HTS), superstar names (SSN)
struct Calls {
  bool loaded = false;
  std::vector<std::string> cities, states, names;
};
const Calls& AnnouncerCalls() {
  static Calls c;
  if (c.loaded) return c;
  c.loaded = true;
  Bytes b;
  Json j;
  if (!ReadFile(Utf8((fs::path(ToolsDir()) / L"announcer_calls.json").wstring()), b) || !ParseJson(std::string(b.begin(), b.end()), j)) return c;
  auto list = [&](const char* key, std::vector<std::string>& out) {
    for (const auto& e : j[key].items)
      if (const std::string clip = e["clip"].Str(); !clip.empty()) out.push_back(clip);
  };
  list("cities", c.cities), list("states", c.states), list("superstars", c.names);
  std::sort(c.names.begin(), c.names.end());
  return c;
}

// A clip picked from a list (with a find box), or typed; "" = none.
bool ClipPicker(const char* label, char* buf, size_t size, const std::vector<std::string>& clips, const char* none, float width = 320) {
  bool changed = false;
  ImGui::SetNextItemWidth(width * g_scale);
  if (ImGui::BeginCombo(label, buf[0] ? buf : none, ImGuiComboFlags_HeightLarge)) {
    static char find[32] = "";
    if (ImGui::IsWindowAppearing()) find[0] = 0, ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##find", "find, or type a clip name and press Enter", find, sizeof find);
    if (ImGui::IsItemDeactivatedAfterEdit() && ImGui::IsKeyPressed(ImGuiKey_Enter) && find[0]) {
      std::string t;
      for (char ch : std::string(find)) if (std::isalnum(uint8_t(ch))) t += char(std::toupper(uint8_t(ch)));
      std::snprintf(buf, size, "%s", t.c_str()), changed = true;
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::Selectable(none, !buf[0])) buf[0] = 0, changed = true;
    std::string f;
    for (char ch : std::string(find)) if (std::isalnum(uint8_t(ch))) f += char(std::toupper(uint8_t(ch)));
    for (const auto& c : clips) {
      if (!f.empty() && c.find(f) == std::string::npos) continue;
      if (ImGui::Selectable(c.c_str(), c == buf)) std::snprintf(buf, size, "%s", c.c_str()), changed = true;
    }
    ImGui::EndCombo();
  }
  return changed;
}

// ---- the select / VS screen pose (D7): the game's superstar -> pose table (DLC_HD.pac MMDL MMMN) and the
// generic poses it rotates through; 32592-32596 (the DLC superstars') aren't on the disc, so not offered
const std::pair<int, int> kStarPoses[] = {{50, 32591}, {51, 32595}, {52, 32596}, {54, 32592}, {55, 32593}, {56, 32594}, {57, 32529}, {58, 32555}, {100, 32520}, {101, 32551}, {102, 32521}, {103, 32522}, {104, 32523}, {107, 32524}, {110, 32525}, {112, 32553}, {117, 32545}, {123, 32526}, {125, 32549}, {131, 32527}, {135, 32528}, {137, 32554}, {139, 32529}, {145, 32530}, {160, 32531}, {161, 32532}, {174, 32580}, {175, 32533}, {176, 32534}, {179, 32543}, {182, 32581}, {195, 32590}, {201, 32535}, {208, 32555}, {218, 32536}, {219, 32537}, {224, 32583}, {251, 32548}, {267, 32546}, {272, 32538}, {275, 32539}, {276, 32540}, {277, 32541}, {279, 32542}, {283, 32547}, {290, 32582}, {306, 32552}};
const std::vector<PoseChoice>& Poses() {
  static std::vector<PoseChoice> v;
  if (!v.empty()) return v;
  std::map<int, std::string> by;  // motion -> the superstars who use it
  for (const auto& [star, motion] : kStarPoses) {
    if (motion >= 32592 && motion <= 32596) continue;
    const StarInfo* si = StarById(star);
    std::string& l = by[motion];
    l += (l.empty() ? "" : ", ") + (si ? si->name : "superstar " + std::to_string(star));
  }
  std::vector<PoseChoice> stars;
  for (const auto& [motion, names] : by) stars.push_back({motion, names});
  std::sort(stars.begin(), stars.end(), [](const PoseChoice& a, const PoseChoice& b) { return a.label < b.label; });
  for (auto& c : stars) v.push_back(c);
  for (int k = 0; k < 6; ++k) v.push_back({32500 + k, "Generic (men) " + std::to_string(k + 1)});
  for (int k = 0; k < 6; ++k) v.push_back({32510 + k, "Generic (women) " + std::to_string(k + 1)});
  v.push_back({32544, "Unused 1 (no superstar has it)"});
  v.push_back({32550, "Unused 2 (no superstar has it)"});
  return v;
}
std::string PoseLabel(int id) {
  for (const auto& c : Poses())
    if (c.id == id) return c.label;
  return id < 0 ? "" : "pose " + std::to_string(id);
}

// pac\DLC.pac MMDL MOTN child 0: the poses' YMKs bank (loaded once)
std::shared_ptr<const std::vector<uint8_t>> g_pose_bank;
bool g_pose_bank_tried = false;
void PreviewPose(int id) {
  auto play = [id] {
    std::string err;
    char_preview::SetDummy(L"");
    if (!g_pose_bank || !char_preview::SetMotion(g_pose_bank, id, 10, &err)) Log("pose: not shown: " + err), Status("That pose can't be shown: " + (g_pose_bank ? err : std::string("pac\\DLC.pac has no pose bank")) + ".");
    else g_watching = true, Log("pose: showing " + std::to_string(id));
  };
  if (g_pose_bank_tried) return play();
  g_pose_bank_tried = true;
  const std::wstring file = (fs::path(g_game) / L"pac" / L"DLC.pac").wstring();
  RunInBackground([file, play] {
    Progress("Reading the select screen poses ...");
    Bytes d;
    Epac e;
    std::shared_ptr<const std::vector<uint8_t>> bank;
    if (ReadFile(Utf8(file), d) && EpacRead(d, e))
      for (const auto& g : e.groups)
        if (g.type == "MMDL")
          for (const auto& en : g.entries) {
            std::vector<PachEntry> kids;
            if (en.name.rfind("MOTN", 0) == 0 && PachRead(Unpack(en.data), kids))
              for (const auto& k : kids)
                if (k.id == 0) bank = std::make_shared<const std::vector<uint8_t>>(Unpack(k.data));
          }
    OnUiThread([bank, play] {
      Log("pose bank: " + (bank ? std::to_string(bank->size()) + " bytes" : std::string("not found")));
      g_pose_bank = bank;
      play();
    });
  });
}

// tools/pro_slots.json: [{"off": 122, "menu": "...", "slot": "...", "fixed": false}, ...]
void LoadSlotNames() {
  if (g_slot_names_loaded) return;
  g_slot_names_loaded = true;
  Bytes b;
  if (!ReadFile(Utf8((fs::path(ToolsDir()) / L"pro_slots.json").wstring()), b)) return;
  const std::string t(b.begin(), b.end());
  auto str = [&](size_t obj, size_t end, const char* key) {
    const size_t k = t.find(std::string("\"") + key + "\"", obj);
    if (k == std::string::npos || k > end) return std::string();
    const size_t q1 = t.find('"', t.find(':', k) + 1), q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
    return q1 == std::string::npos || q2 == std::string::npos || q2 > end ? std::string() : t.substr(q1 + 1, q2 - q1 - 1);
  };
  for (size_t at = t.find('{'); at != std::string::npos; at = t.find('{', at + 1)) {
    const size_t end = t.find('}', at);
    if (end == std::string::npos) break;
    const size_t k = t.find("\"off\"", at);
    if (k == std::string::npos || k > end) continue;
    const int off = std::atoi(t.c_str() + t.find(':', k) + 1);
    SlotNameRow n;
    n.menu = str(at, end, "menu");
    n.slot = str(at, end, "slot");
    const size_t f = t.find("\"fixed\"", at);
    n.fixed = f != std::string::npos && f < end && t.compare(t.find(':', f) + 1, 5, " true") == 0;
    if (off >= 0 && off < 2 * kMoveSlots && !(off & 1)) g_slot_names[off / 2] = n;
  }
  Log("moveset: " + std::to_string(g_slot_names.size()) + " slot names from pro_slots.json");
}

std::string SlotLabel(int slot) {
  LoadSlotNames();
  char off[16];
  std::snprintf(off, sizeof off, "0x%03X", slot * 2);
  const auto it = g_slot_names.find(slot);
  if (it != g_slot_names.end() && !it->second.slot.empty())
    return (it->second.menu.empty() ? "" : it->second.menu + ": ") + it->second.slot;
  const int o = slot * 2;  // (known without the table: taunts, signatures, finishers)
  if (o >= 0x12 && o <= 0x18) return "Taunt " + std::to_string((o - 0x12) / 2 + 1);
  if (o == 0x1E || o == 0x20) return "Signature " + std::to_string((o - 0x1E) / 2 + 1);
  if (o == 0x22 || o == 0x24) return "Finisher " + std::to_string((o - 0x22) / 2 + 1);
  return std::string("Slot ") + off;
}

std::string SlotName(int slot) { return SlotLabel(slot); }
void ShowPose(int motion);

bool SlotFixed(int slot) {
  LoadSlotNames();
  const auto it = g_slot_names.find(slot);
  return it != g_slot_names.end() && it->second.fixed;
}

std::string MovesetText() {
  std::string s;
  for (const auto& [slot, id] : g_star.moveset) {
    char b[32];
    std::snprintf(b, sizeof b, "%s0x%03X=%d", s.empty() ? "" : ",", slot * 2, id);
    s += b;
  }
  return s;
}

// "0xOFF=id" items split by sep (a project's key, or a moves.txt's lines)
// "0xOFF=id" items; a ported mod's moves.txt says before a slot where its move came from ("# same: <move> (<game>)" /
// "# similar: <move> (<game>) -> <2011 move>"): kept as that slot's note.
void ParseMoveset(const std::string& t, char sep) {
  std::string note;
  for (size_t at = 0; at < t.size();) {
    size_t e = t.find(sep, at);
    if (e == std::string::npos) e = t.size();
    std::string item = t.substr(at, e - at);
    at = e + 1;
    if (!item.empty() && item.back() == '\r') item.pop_back();
    if (item.rfind("# same:", 0) == 0 || item.rfind("# similar:", 0) == 0) {
      note = item.substr(2);
      continue;
    }
    const size_t eq = item.find('=');
    if (eq == std::string::npos) continue;
    const int off = int(std::strtol(item.c_str(), nullptr, 0));
    const int id = std::atoi(item.c_str() + eq + 1);
    if (off >= 0 && off < 2 * kMoveSlots && !(off & 1) && id > 0) {
      g_star.moveset[off / 2] = id;
      if (!note.empty()) g_star.moveset_note[off / 2] = note;
    }
    note.clear();
  }
}

// A slot's note as a short tag ("similar: DDT 16") and the whole line for the tooltip.
void MoveNoteTag(int slot) {
  const auto it = g_star.moveset_note.find(slot);
  if (it == g_star.moveset_note.end()) return;
  const std::string& n = it->second;
  const bool similar = n.rfind("similar:", 0) == 0;
  std::string tag = n.substr(0, n.find(" ("));
  ImGui::SameLine();
  ImGui::TextColored(similar ? ImVec4(1, 0.8f, 0.35f, 1) : ImVec4(0.55f, 0.75f, 0.55f, 1), "%s", tag.c_str());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", similar ? (n + "\nA guess: the source game's move wasn't in 2011, so a similar one was picked - swap it if you like.").c_str() : n.c_str());
}

// Moves the installed move packs add (and the project's own pack): id, the stock move its record copies, name, pack
struct PackMove {
  int id = 0, from = 0;
  std::string name, pack;
};
const std::vector<PackMove>& PackMoves() {
  static std::vector<PackMove> v;
  static double when = -100;
  if (ImGui::GetTime() - when < 3) return v;
  when = ImGui::GetTime();
  v.clear();
  std::vector<std::pair<fs::path, std::string>> packs;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Moves", ec))
    if (e.is_directory(ec)) packs.push_back({e.path(), Utf8(e.path().filename().wstring())});
  if (!g_star.pack.empty()) packs.push_back({fs::path(g_star.pack), "this mod's pack"});
  for (const auto& [dir, label] : packs) {
    Bytes t;
    if (!ReadFile(PathStr(dir / L"pack.txt"), t)) continue;
    std::map<int, PackMove> by;
    const std::string text(t.begin(), t.end());
    for (size_t at = 0; at < text.size();) {
      size_t nl = text.find('\n', at);
      if (nl == std::string::npos) nl = text.size();
      std::string line = text.substr(at, nl - at);
      at = nl + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      int id = 0, from = 0;
      char word[16] = "";
      if (std::sscanf(line.c_str(), "%15s %d", word, &id) < 2 || id <= 0) continue;
      if (std::strcmp(word, "waze") && std::strcmp(word, "wazecopy") && std::strcmp(word, "wazename")) continue;  // (motions, records)
      PackMove& m = by[id];
      m.id = id, m.pack = label;
      if (!std::strcmp(word, "wazecopy") && std::sscanf(line.c_str(), "%*s %d %d", &id, &from) == 2) m.from = from;
      else if (!std::strcmp(word, "waze")) m.from = m.from ? m.from : id;
      else if (!std::strcmp(word, "wazename")) {
        const size_t sp = line.find(' ', line.find(' ') + 1);
        if (sp != std::string::npos) m.name = line.substr(sp + 1);
      }
    }
    for (auto& [id, m] : by)
      if (m.from) v.push_back(m);
  }
  return v;
}

void MovePicker() {
  static int opened_for = -1;  // the slot the popup was opened for: opened once, so a click outside closes it for good
  if (g_pick_slot < 0) {
    opened_for = -1;
    return;
  }
  if (opened_for != g_pick_slot) ImGui::OpenPopup("pick_move"), opened_for = g_pick_slot;
  // (every frame: a popup sizes itself to its contents, which left the list with no height)
  ImGui::SetNextWindowSize(ImVec2(460 * g_scale, 420 * g_scale), ImGuiCond_Always);
  if (ImGui::BeginPopup("pick_move")) {
    ImGui::TextUnformatted(SlotLabel(g_pick_slot).c_str());
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##f", "find a move", g_pick_find, sizeof g_pick_find);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // (the find box would only let go of it)
      g_pick_slot = -1, opened_for = -1;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    ImGui::SameLine();
    ImGui::Checkbox("all moves", &g_pick_any);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every move, not only the ones of this slot's kind (those may not play from it).");
    const std::string f = Lower(g_pick_find);
    ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), true);
    auto row = [&](int id) {
      const std::string& n = MoveName(id);
      if (!f.empty() && Lower(n).find(f) == std::string::npos && std::to_string(id).find(f) == std::string::npos) return;
      const std::string label = n + "  (" + std::to_string(id) + ")##" + std::to_string(id);
      const auto it = g_star.moveset.find(g_pick_slot);
      if (ImGui::Selectable(label.c_str(), it != g_star.moveset.end() && it->second == id)) {
        g_star.moveset[g_pick_slot] = id, g_star.moveset_note.erase(g_pick_slot), g_star.moveset_note[g_pick_slot] = "edited";
        Touch();
        g_pick_slot = -1;
        ImGui::CloseCurrentPopup();
      }
    };
    for (const PackMove& pm : PackMoves()) {  // installed packs' moves first (fitted by the move each copies)
      if (!g_pick_any && !MoveFitsSlot(g_pick_slot, pm.from)) continue;
      const std::string n = (pm.name.empty() ? MoveName(pm.from) : pm.name);
      if (!f.empty() && Lower(n).find(f) == std::string::npos && std::to_string(pm.id).find(f) == std::string::npos) continue;
      const auto it = g_star.moveset.find(g_pick_slot);
      if (ImGui::Selectable((n + "  (" + std::to_string(pm.id) + ")  [" + pm.pack + "]##p" + std::to_string(pm.id)).c_str(),
                            it != g_star.moveset.end() && it->second == pm.id)) {
        g_star.moveset[g_pick_slot] = pm.id, g_star.moveset_note[g_pick_slot] = "edited";
        Touch();
        g_pick_slot = -1;
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("From the move pack %s (a copy of move %d's record).", pm.pack.c_str(), pm.from);
    }
    if (g_pick_any) for (const auto& [id, m] : MoveInfos()) row(id);
    else for (int id : MovesForSlot(g_pick_slot)) row(id);
    ImGui::EndChild();
    static int typed = 0;
    ImGui::SetNextItemWidth(110 * g_scale);
    ImGui::InputInt("##id", &typed, 0);
    ImGui::SameLine();
    if (ImGui::Button("Use this move id") && typed > 0) g_star.moveset[g_pick_slot] = typed, g_star.moveset_note[g_pick_slot] = "edited", Touch(), g_pick_slot = -1, ImGui::CloseCurrentPopup();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A move by number - e.g. one a move pack adds (it isn't in the game's list).");
    ImGui::EndPopup();
  } else {
    g_pick_slot = -1, opened_for = -1;
  }
}

// ---- a whole moveset from somewhere else (player request, 20 Oct): a 2011 superstar's, one of the superstar mods
// installed in the game, or another game's superstar (the Port moves menu, simple_port.h ForStar)
int g_imp_src = -1;  // -1 closed, 0 a 2011 superstar, 1 one of my mods, 2 another game
char g_imp_find[48] = "";
int g_imp_pick = -1;  // (the list's chosen row)
bool g_imp_test_save = false;  // (test aid game:<game>:<item>: the project saved once their moves are in)
struct ImportMod {
  fs::path dir;
  std::string name;
  int base = 0;
};
std::vector<ImportMod> g_imp_mods;

// The moves a move pack brings (pack.txt's waze / wazecopy / wazename lines: the game plays them whatever misc.pac
// says), read once per pack folder and change.
const std::set<int>& PackMoveIds(const std::wstring& pack) {
  static std::map<std::wstring, std::pair<fs::file_time_type, std::set<int>>> cache;
  static const std::set<int> kNone;
  if (pack.empty()) return kNone;
  std::error_code ec;
  const fs::path f = fs::path(pack) / L"pack.txt";
  const auto t = fs::last_write_time(f, ec);
  auto& c = cache[pack];
  if (ec || c.first == t) return c.second;
  c.first = t, c.second.clear();
  Bytes b;
  ReadFile(PathStr(f), b);
  const std::string text(b.begin(), b.end());
  for (size_t at = 0; at < text.size();) {
    size_t e = text.find('\n', at);
    if (e == std::string::npos) e = text.size();
    const std::string l = text.substr(at, e - at);
    at = e + 1;
    for (const char* k : {"waze ", "wazecopy ", "wazename "})
      if (l.rfind(k, 0) == 0) c.second.insert(std::atoi(l.c_str() + std::strlen(k)));
  }
  return c.second;
}

// A move the game can play (as its MoveUsable): misc.pac has a record with its move kinds, or a move pack brings it.
// (Stock profiles hold a few the game can't: RVD's 0x0E0 10280 - in a match such a slot gets the base's move.)
// True when the game's moves didn't load (nothing to tell by).
bool MoveUsable(int id, const std::wstring& pack) {
  const auto& infos = MoveInfos();
  if (infos.empty() || PackMoveIds(pack).count(id)) return true;
  for (const PackMove& pm : PackMoves())  // (the installed move packs' and this mod's)
    if (pm.id == id) return true;
  const auto it = infos.find(id);
  return it != infos.end() && std::any_of(std::begin(it->second.bits), std::end(it->second.bits), [](uint8_t b) { return b != 0; });
}

// Slot -> move id ("0xOFF=id" lines, a moves.txt) on top of `full`; a ported mod's "# same: / # similar:" notes too.
void OverlayMoves(const std::string& t, std::vector<int>& full, std::map<int, std::string>& notes) {
  std::string note;
  for (size_t at = 0; at < t.size();) {
    size_t e = t.find('\n', at);
    if (e == std::string::npos) e = t.size();
    std::string item = t.substr(at, e - at);
    at = e + 1;
    if (!item.empty() && item.back() == '\r') item.pop_back();
    if (item.rfind("# same:", 0) == 0 || item.rfind("# similar:", 0) == 0) {
      note = item.substr(2);
      continue;
    }
    const size_t eq = item.find('=');
    if (eq == std::string::npos || item[0] == '#') continue;
    const int off = int(std::strtol(item.c_str(), nullptr, 0)), id = std::atoi(item.c_str() + eq + 1);
    if (off >= 0 && off < 2 * kMoveSlots && !(off & 1) && id > 0) {
      full[size_t(off / 2)] = id;
      if (!note.empty()) notes[off / 2] = note;
    }
    note.clear();
  }
}

// The whole moveset `full` (every slot) as the project's: the slots that differ from the style's become its picks
// (the earlier picks and a moves list file go: they'd sit under or over the new set), a pack joins the project's.
void TakeWholeMoveset(const std::vector<int>& full, const std::map<int, std::string>& notes, const std::wstring& pack,
                      const std::string& from) {
  const std::vector<int> base = ProfileMoves(kStyles[g_star.style].template_id);
  g_star.moveset.clear(), g_star.moveset_note.clear(), g_star.moves.clear();
  int n = 0;
  std::string skipped;
  for (int s = 0; s < int(full.size()) && s < kMoveSlots; ++s) {
    const int was = s < int(base.size()) ? base[size_t(s)] : 0;
    if (!full[size_t(s)] || full[size_t(s)] == was) continue;
    if (!MoveUsable(full[size_t(s)], pack)) {  // (the game couldn't play it: the style's stays)
      char b[32];
      std::snprintf(b, sizeof b, " 0x%03X=%d", s * 2, full[size_t(s)]);
      skipped += b;
      continue;
    }
    g_star.moveset[s] = full[size_t(s)], ++n;
    if (const auto it = notes.find(s); it != notes.end()) g_star.moveset_note[s] = it->second;
  }
  if (!pack.empty()) g_star.pack = simple_port::CombinePacks(g_star.pack, pack);
  Touch();
  if (!skipped.empty()) Log("import moveset: " + from + ": moves the game can't play, the style's kept:" + skipped);
  Log("import moveset: " + from + ": " + std::to_string(n) + " slots differ from the style's" + (pack.empty() ? "" : ", with its move pack"));
  Status(from + "'s whole moveset is your superstar's now (" + std::to_string(n) + " slots differ from your fighting style's).");
}

bool ImportFromStar(int id) {
  const std::vector<int> moves = ProfileMoves(id);
  const StarInfo* st = StarById(id);
  if (moves.empty() || !st) return Status("That superstar's moves couldn't be read (misc.pac)."), false;
  std::vector<int> full(size_t(kMoveSlots), 0);
  std::copy_n(moves.begin(), std::min(moves.size(), full.size()), full.begin());
  TakeWholeMoveset(full, {}, L"", st->name);
  return true;
}

void ReadImportMods() {
  g_imp_mods.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec)) {
    Bytes man;
    if (!e.is_directory(ec) || !ReadFile(PathStr(e.path() / L"manifest.txt"), man)) continue;
    const std::string t(man.begin(), man.end());
    ImportMod m;
    m.dir = e.path();
    m.name = Value(t, "name");
    if (m.name.empty()) m.name = Utf8(e.path().filename().wstring());
    m.base = std::atoi(Value(t, "base").c_str());
    g_imp_mods.push_back(std::move(m));
  }
  std::sort(g_imp_mods.begin(), g_imp_mods.end(), [](const ImportMod& a, const ImportMod& b) { return Lower(a.name) < Lower(b.name); });
}

// A superstar mod's moveset: its base's moves, its moves list on top, its move pack (moves\pack.txt) with it.
bool ImportFromMod(const fs::path& dir) {
  Bytes man;
  if (!ReadFile(PathStr(dir / L"manifest.txt"), man)) return Status("That mod has no manifest.txt."), false;
  const std::string t(man.begin(), man.end());
  std::string name = Value(t, "name"), file = Value(t, "moves");
  if (name.empty()) name = Utf8(dir.filename().wstring());
  if (file.empty()) file = "moves.txt";
  std::vector<int> full(size_t(kMoveSlots), 0);
  const std::vector<int> moves = ProfileMoves(std::atoi(Value(t, "base").c_str()));
  std::copy_n(moves.begin(), std::min(moves.size(), full.size()), full.begin());
  std::map<int, std::string> notes;
  Bytes mv;
  const bool listed = ReadFile(PathStr(dir / fs::u8path(file)), mv);
  if (listed) OverlayMoves(std::string(mv.begin(), mv.end()), full, notes);
  if (moves.empty() && !listed) return Status(name + " has no moves to bring: no moves list, and its base's couldn't be read."), false;
  std::error_code ec;
  const fs::path pack = dir / L"moves";
  TakeWholeMoveset(full, notes, fs::exists(pack / L"pack.txt", ec) ? pack.wstring() : std::wstring(), name);
  return true;
}

void ImportMovesetSection() {
  ImGui::Spacing();
  ImGui::TextUnformatted("Import a whole moveset from:");
  static const char* const kSrc[] = {"A 2011 superstar", "One of my superstar mods", "Another game"};
  for (int i = 0; i < 3; ++i) {
    if (i) ImGui::SameLine();
    if (ImGui::RadioButton(kSrc[i], g_imp_src == i)) {
      g_imp_src = i, g_imp_pick = -1, g_imp_find[0] = 0;
      if (i == 1) ReadImportMods();
    }
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Close##imp")) g_imp_src = -1;
  ImGui::TextDisabled("The whole set replaces this superstar's moves (the moves you picked and a moves list file).");
  if (g_imp_src == 2) {  // (the Port moves menu: a game, a superstar, their moves - "Use ... for my superstar")
    simple_port::ForStarResult r;
    if (simple_port::ForStar(simple_port::kMoves, r)) {
      g_star.moveset.clear(), g_star.moveset_note.clear(), g_star.moves.clear();
      std::string text;
      for (const auto& l : r.lines) text += l + "\n";
      ParseMoveset(text, '\n');
      g_star.pack = simple_port::CombinePacks(g_star.pack, r.pack);
      for (auto it = g_star.moveset.begin(); it != g_star.moveset.end();) {  // (ones the game can't play: the style's)
        if (MoveUsable(it->second, g_star.pack)) {
          ++it;
          continue;
        }
        Log("import moveset: the game can't play move " + std::to_string(it->second) + ": the style's kept");
        g_star.moveset_note.erase(it->first);
        it = g_star.moveset.erase(it);
      }
      Touch();
      Log("import moveset: " + r.what + ": " + std::to_string(r.lines.size()) + " slots");
      Status(r.what + " are your superstar's now (the slots that couldn't come over keep your fighting style's).");
      g_imp_src = -1;
      if (g_imp_test_save) g_imp_test_save = false, TestSaveMod(g_test_save), PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    return;
  }
  ImGui::SetNextItemWidth(240 * g_scale);
  ImGui::InputTextWithHint("##impf", "find a superstar", g_imp_find, sizeof g_imp_find);
  const std::string f = Lower(g_imp_find);
  std::string chosen;
  ImGui::BeginChild("imp_list", ImVec2(360 * g_scale, 200 * g_scale), true);
  if (g_imp_src == 0) {
    for (const auto& st : Stars()) {
      if (!f.empty() && Lower(st.name).find(f) == std::string::npos) continue;
      if (ImGui::Selectable((st.name + "##" + std::to_string(st.id)).c_str(), g_imp_pick == st.id)) g_imp_pick = st.id;
      if (g_imp_pick == st.id) chosen = st.name;
    }
  } else {
    if (g_imp_mods.empty()) ImGui::TextDisabled("No superstar mods in the game's Mods\\Superstars.");
    for (int i = 0; i < int(g_imp_mods.size()); ++i) {
      const ImportMod& m = g_imp_mods[size_t(i)];
      if (!f.empty() && Lower(m.name).find(f) == std::string::npos) continue;
      if (ImGui::Selectable((m.name + "##m" + std::to_string(i)).c_str(), g_imp_pick == i)) g_imp_pick = i;
      if (g_imp_pick == i) chosen = m.name;
    }
  }
  ImGui::EndChild();
  ImGui::BeginDisabled(chosen.empty());
  if (ImGui::Button(chosen.empty() ? "Use their moveset" : ("Use " + chosen + "'s moveset").c_str(), ImVec2(300 * g_scale, 0))) {
    const bool ok = g_imp_src == 0 ? ImportFromStar(g_imp_pick) : ImportFromMod(g_imp_mods[size_t(g_imp_pick)].dir);
    if (ok) g_imp_src = -1;
  }
  ImGui::EndDisabled();
}

void MovesetSection() {
  const int changed = int(g_star.moveset.size());
  const std::string head = "Moveset" + (changed ? " (" + std::to_string(changed) + " changed)" : std::string(": the style's")) + "###moveset";
  const bool scroll = g_moveset_open_once;
  if (g_moveset_open_once) ImGui::SetNextItemOpen(true), g_moveset_open_once = false;
  if (!ImGui::CollapsingHeader(head.c_str())) return;
  if (scroll) ImGui::SetScrollHereY(0.0f);
  ImGui::Indent();
  ImGui::TextWrapped("The moves the superstar does, slot by slot: the style's superstar's moves, and the ones you "
                     "pick instead. Each slot's list holds the game's moves of that kind (a strike slot takes "
                     "strikes, a finisher slot finishers). The game's CREATE A MOVE-SET can change them later too.");
  if (MoveInfos().empty()) {
    ImGui::TextDisabled("The game's moves didn't load (misc.pac).");
    ImGui::Unindent();
    return;
  }
  const std::vector<int> base = ProfileMoves(kStyles[g_star.style].template_id);
  ImGui::SetNextItemWidth(240 * g_scale);
  ImGui::InputTextWithHint("##mf", "find a slot or a move", g_moveset_find, sizeof g_moveset_find);
  ImGui::SameLine();
  ImGui::Checkbox("all slots", &g_moveset_all);
  ImGui::SameLine();
  if (changed && ImGui::SmallButton("Back to the style's moves")) g_star.moveset.clear(), Touch();
  ImGui::SameLine();
  if (ImGui::SmallButton("Tag team moves")) std::snprintf(g_moveset_find, sizeof g_moveset_find, "%s", "tag team");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The 8 double-team slots: 4 standing, 4 from the corner. They play only in tag matches, with a "
                      "partner. Pick 2011's own, or port one from another game (the Port moves menu).");
  ImGui::SameLine();
  if (ImGui::SmallButton("Import a whole moveset...")) g_imp_src = g_imp_src < 0 ? 0 : -1;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Every move of a 2011 superstar, of one of your superstar mods (with its own move pack), or of a "
                      "superstar from another game (the Port moves menu).");
  if (g_imp_src >= 0) ImportMovesetSection();
  if (!g_moveset_all && !changed) ImGui::TextDisabled("Tick \"all slots\" (or find one) to change a move.");
  const std::string f = Lower(g_moveset_find);
  const bool can_watch = anims_page::MovesIndexed();
  if (g_watching) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Back to the idle")) char_preview::ClearMotion(), char_preview::SetDummy(L""), g_watching = false;
  }
  if (ImGui::BeginTable("moveset", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                        ImVec2(0, 300 * g_scale))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 220 * g_scale);
    ImGui::TableSetupColumn("the style's move", ImGuiTableColumnFlags_WidthFixed, 200 * g_scale);
    ImGui::TableSetupColumn("this superstar's move", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24 * g_scale);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * g_scale);
    ImGui::TableHeadersRow();
    for (int s = 0; s < kMoveSlots; ++s) {
      const auto it = g_star.moveset.find(s);
      const bool mine = it != g_star.moveset.end();
      if (SlotFixed(s) && !mine) continue;
      const int was = s < int(base.size()) ? base[size_t(s)] : 0;
      if (!was && !mine) continue;  // (an empty slot in the style's set)
      const std::string label = SlotLabel(s);
      const std::string& was_name = MoveName(was);
      const std::string now_name = mine ? (MoveName(it->second).empty() ? "move " + std::to_string(it->second) : MoveName(it->second)) : "";
      if (!f.empty() && Lower(label).find(f) == std::string::npos && Lower(was_name).find(f) == std::string::npos &&
          Lower(now_name).find(f) == std::string::npos)
        continue;
      if (f.empty() && !g_moveset_all && !mine) continue;
      ImGui::PushID(s);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(label.c_str());
      if (s * 2 >= 0x18E && s * 2 <= 0x19C && ImGui::IsItemHovered())  // (the tag team slots: how the game starts them)
        ImGui::SetTooltip(s * 2 <= 0x194
                              ? "A standing double team, in tag matches: drag a groggy opponent to your corner, tag "
                                "in, then attack the one your partner is holding."
                              : "A corner double team, in tag matches: Irish whip the opponent into your corner, "
                                "where your partner waits.");
      MoveNoteTag(s);
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", was_name.empty() ? (was ? std::to_string(was).c_str() : "-") : was_name.c_str());
      ImGui::TableNextColumn();
      const bool fits = !mine || MoveFitsSlot(s, it->second);
      if (mine && !fits) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      if (ImGui::Button(mine ? (now_name + "##p").c_str() : "the style's (pick another)##p", ImVec2(-1, 0)))
        g_pick_slot = s, g_pick_find[0] = 0, g_pick_any = false;
      if (mine && !fits) ImGui::PopStyleColor();
      ImGui::TableNextColumn();
      if (mine && ImGui::SmallButton("x")) g_star.moveset.erase(s), g_star.moveset_note.erase(s), Touch();
      ImGui::TableNextColumn();
      const int play = mine ? it->second : was;
      const bool test_now = g_watch_slot_test == s && can_watch && !Busy();
      if (test_now) g_watch_slot_test = -1;
      ImGui::BeginDisabled(!can_watch || Busy() || !anims_page::HasMotion(play));
      if (ImGui::SmallButton(">##w") || test_now) {  // the move in the preview: the model does it, the style's superstar takes it
        char_preview::SetDummy((fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(kStyles[g_star.style].template_id) + L".pac")).wstring());
        anims_page::WatchMove(play);
        g_watching = true;
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(anims_page::HasMotion(play) ? "Watch it in the preview (the style's superstar takes it)."
                          : can_watch ? "This move's motion isn't in the game's banks (a move pack's: Animations > Play in game)."
                                      : "Reading the game's moves ...");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  MovePicker();
  ImGui::Unindent();
}

// theme_gain= / call_gain= as written (1 decimal), "" at 0.0 (the key left out)
std::string DbText(float db, float lo, float hi) {
  const int t = int(std::lround(std::clamp(db, lo, hi) * 10));
  if (!t) return "";
  char b[16];
  std::snprintf(b, sizeof b, "%s%d.%d", t < 0 ? "-" : "", std::abs(t) / 10, std::abs(t) % 10);
  return b;
}
std::string GainText() { return DbText(g_star.theme_gain, -20.0f, 6.0f); }
std::string CallGainText() { return DbText(g_star.call_gain, -20.0f, 12.0f); }

// the theme's measured loudness (a worker measures; the page shows it)
std::wstring g_lufs_for;
double g_lufs = 0;
std::string g_lufs_note;
bool g_lufs_busy = false;

void MeasureTheme() {
  if (g_star.song.empty() || g_lufs_busy) return;
  g_lufs_busy = true;
  g_lufs_note = "measuring ...";
  const std::wstring f = g_star.song;
  std::thread([f] {
    double l = 0;
    std::string err;
    const bool ok = MeasureLufs(f, &l, &err);
    OnUiThread([f, ok, l, err] {
      g_lufs_busy = false;
      if (f != g_star.song) return;  // (another song by now)
      g_lufs_for = f;
      g_lufs = l;
      char b[160];
      if (ok) std::snprintf(b, sizeof b, "%.1f LUFS (the game's themes sit near %.0f)", l, kThemeTargetLufs);
      g_lufs_note = ok ? b : "could not measure: " + err;
      if (!ok) g_lufs_for.clear();
    });
  }).detach();
}

// ---- the game's own themes (music= ids 1-253: the stock superstars' profile music, decoded with wwise.py to listen)

struct StockTheme {
  int music = 0;
  std::string names;  // the superstars with it
};

const std::vector<StockTheme>& StockThemes() {
  static std::vector<StockTheme> v;
  if (v.empty()) {
    std::map<int, std::string> by;
    for (const StarInfo& st : Stars())
      if (const int m = ProfileMusic(st.id); m >= 1 && m <= 253) by[m] += (by[m].empty() ? "" : ", ") + st.name;
    for (const auto& [m, n] : by) v.push_back({m, n});
    std::sort(v.begin(), v.end(), [](const StockTheme& a, const StockTheme& b) { return Lower(a.names) < Lower(b.names); });
  }
  return v;
}

std::string StockThemeName(int music) {
  for (const StockTheme& t : StockThemes())
    if (t.music == music) return t.names;
  return "theme " + std::to_string(music);
}

int g_theme_playing = 0;  // the stock theme playing (0: none)
bool g_theme_start = false;  // (Load decodes in the background: played once it's ready)
std::string g_theme_note;

// Plays (or stops) a stock theme: decoded once into Mods\.convert\themes (the Full preview's way).
void PlayStockTheme(int music) {
  if (g_theme_playing == music && (g_theme_start || audio_out::Playing())) {
    audio_out::Pause();
    g_theme_playing = 0, g_theme_start = false;
    return;
  }
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"themes";
  const fs::path mp3 = work / (L"music_" + std::to_wstring(music) + L".mp3");
  auto play = [music](const fs::path& f) {
    audio_out::SetMuted(g_behind);  // (test runs are silent)
    audio_out::Load(f.wstring());
    g_theme_playing = music, g_theme_start = true, g_theme_note.clear();
  };
  std::error_code ec;
  if (fs::exists(mp3, ec) && fs::file_size(mp3, ec) > 0) return play(mp3);
  fs::create_directories(work, ec);
  char ev[32];
  std::snprintf(ev, sizeof ev, "Play_MUS_%04d_0_0", music);
  g_theme_note = "getting the theme from the game's sound files...";
  if (!RunToolJson(L"wwise.py", {L"theme", (fs::path(g_game) / L"sound" / L"Music.pck").wstring(), Wide(ev), mp3.wstring()},
                   [mp3, play](int code, const std::string& text) {
                     std::error_code e2;
                     if (code == 0 && fs::exists(mp3, e2)) play(mp3);
                     else g_theme_note = "couldn't read that theme (see the log)", Log("stock theme: wwise.py: " + text);
                   }))
    g_theme_note = "listening needs the Mod Maker tools (the launcher's Setup page)";
}

void StockThemeRow() {
  static bool open = false;
  static char find[64] = "";
  if (g_theme_start && audio_out::State() != 0) {  // (decoded, or failed)
    g_theme_start = false;
    if (audio_out::State() == 1) audio_out::Play(0), Log("stock theme: " + std::to_string(g_theme_playing) + " playing (" + std::to_string(int(audio_out::Length())) + " s)");
    else g_theme_playing = 0, g_theme_note = "couldn't play it: " + audio_out::Error();
  }
  const bool playing = g_theme_playing && (g_theme_start || audio_out::Playing());
  if (g_star.music > 0) {
    ImGui::TextUnformatted("The game's theme:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.6f, 1), "%s", StockThemeName(g_star.music).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(playing && g_theme_playing == g_star.music ? "Stop##st" : "Play##st")) PlayStockTheme(g_star.music);
    ImGui::SameLine();
    if (ImGui::SmallButton("x##st")) g_star.music = 0, Touch();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the style's theme.");
  }
  if (ImGui::Button("A game theme...")) open = true, find[0] = 0, ImGui::OpenPopup("stock_theme");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("One of the game's own entrance themes, by superstar: it plays at the game's level and the mod carries no file.");
  if (!g_theme_note.empty()) ImGui::SameLine(), ImGui::TextDisabled("%s", g_theme_note.c_str());
  ImGui::SetNextWindowSize(ImVec2(440 * g_scale, 420 * g_scale), ImGuiCond_Always);  // (a popup sizes itself to its contents)
  if (ImGui::BeginPopup("stock_theme")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##find", "find a superstar", find, sizeof find);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    const std::string f = Lower(find);
    ImGui::BeginChild("themes", ImVec2(0, 0), true);
    for (const StockTheme& t : StockThemes()) {
      if (!f.empty() && Lower(t.names).find(f) == std::string::npos) continue;
      ImGui::PushID(t.music);
      if (ImGui::SmallButton(playing && g_theme_playing == t.music ? "Stop" : "Play")) PlayStockTheme(t.music);
      ImGui::SameLine();
      if (ImGui::Selectable(t.names.c_str(), g_star.music == t.music)) {
        g_star.music = t.music, g_star.song.clear(), Touch();
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    if (StockThemes().empty()) ImGui::TextDisabled("The game's superstars couldn't be read (the game folder's pac\\ch\\chEtc.pac).");
    ImGui::EndChild();
    ImGui::EndPopup();
  } else if (open) {
    open = false;
    if (g_theme_playing) audio_out::Pause(), g_theme_playing = 0, g_theme_start = false;  // (closing the list stops it)
  }
}

void ThemeGainRow() {
  if (g_star.song.empty()) return;
  ImGui::Indent();
  ImGui::SetNextItemWidth(236 * g_scale);
  ImGui::SliderFloat("##gain", &g_star.theme_gain, -20.0f, 6.0f, "%+.1f dB");
  ImGui::SameLine(0, 4 * g_scale);
  ImGui::SetNextItemWidth(80 * g_scale);
  ImGui::InputFloat("Theme volume", &g_star.theme_gain, 0, 0, "%.1f");
  g_star.theme_gain = std::clamp(std::round(g_star.theme_gain * 10) / 10, -20.0f, 6.0f);
  Hint("How loud the theme plays, in dB over the file: the game plays a mod's theme at full level, about 7 dB over "
       "its own. Measure finds the song's loudness; Match the game sets the volume so it sits with the game's "
       "themes (about -24 LUFS).");
  ImGui::SameLine();
  ImGui::BeginDisabled(g_lufs_busy);
  if (ImGui::SmallButton("Measure")) MeasureTheme();
  ImGui::EndDisabled();
  const bool measured = g_lufs_for == g_star.song;
  if (measured) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Match the game")) {
      g_star.theme_gain = float(std::clamp(kThemeTargetLufs - g_lufs, -20.0, 6.0));
      g_star.theme_gain = std::round(g_star.theme_gain * 10) / 10;
      Touch();
    }
  }
  if (!g_lufs_note.empty() && (measured || g_lufs_busy || g_lufs_note.rfind("could", 0) == 0)) ImGui::TextDisabled("%s", g_lufs_note.c_str());
  ImGui::Unindent();
}

// call_gain=: the mod's own name recording (voice=) plays at the game's call level (-7 dB) plus this
int g_test_scroll_call = 0;  // (--star-call-gain: the row scrolled into view for a screenshot, a few frames)
void CallGainRow() {
  if (g_test_scroll_call > 0 && --g_test_scroll_call < 20) ImGui::SetScrollHereY(0.5f);
  ImGui::Indent();
  ImGui::BeginDisabled(g_star.voice.empty());
  ImGui::SetNextItemWidth(236 * g_scale);
  if (ImGui::SliderFloat("##callgain", &g_star.call_gain, -20.0f, 12.0f, "%+.1f dB")) Touch();
  ImGui::SameLine(0, 4 * g_scale);
  ImGui::SetNextItemWidth(80 * g_scale);
  if (ImGui::InputFloat("Ring announcer volume", &g_star.call_gain, 0, 0, "%.1f")) Touch();
  g_star.call_gain = std::clamp(std::round(g_star.call_gain * 10) / 10, -20.0f, 12.0f);
  ImGui::EndDisabled();
  Hint("0 = as loud as the game's own announcer calls. It sets how loud the name recording (below) plays when the "
       "ring announcer calls the superstar.");
  if (g_star.voice.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(for a name recording, below)");
  }
  ImGui::Unindent();
}

// height= as written (2 decimals), "" at 1.00 (the key left out)
std::string HeightText() {
  const int c = int(std::lround(std::clamp(g_star.height, 0.8f, 1.25f) * 100));
  if (c == 100) return "";
  char b[8];
  std::snprintf(b, sizeof b, "%d.%02d", c / 100, c % 100);
  return b;
}

bool PrepareJob(Job& j) {
  const Style& st = kStyles[g_star.style];
  if (!g_star.ratings_set) {
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StarRating(st.template_id, k);
    g_star.ratings_set = true;
  }
  j.id = StarId();
  j.project = ModProjectText(PageId::kStar);
  {  // (a model over the old 192 pages: newer games only)
    int worst = 0;
    if (!g_star.model.empty())
      for (const auto& p : ModelBudget(g_star.model)) worst = std::max(worst, p.pages);
    j.needs = worst > kModelPagesOld ? kNeedsVersion : "";
  }
  j.manifest = "type=superstar\nid=" + j.id + "\nname=" + g_star.name + "\nshort=" +
               (g_star.short_name[0] ? g_star.short_name : g_star.name) + "\nbase=" + std::to_string(st.template_id) +
               "\nstyle=" + st.name + "\nauthor=" + g_star.author + "\nversion=" + g_star.version + "\n" + MadeWith();
  if (!j.needs.empty()) j.manifest += "needs=" + j.needs + "\n";
  j.manifest += "ratings=";
  for (int k = 0; k < 7; ++k) j.manifest += std::to_string(g_star.ratings[k]) + (k < 6 ? "," : "\n");
  if (g_star.call >= 0) j.manifest += "call=" + std::to_string(g_star.call) + "\n";
  if (const std::string h = HeightText(); !h.empty()) j.manifest += "height=" + h + "\n";
  if (const std::string g = GainText(); !g.empty() && !g_star.song.empty()) j.manifest += "theme_gain=" + g + "\n";
  if (const std::string g = CallGainText(); !g.empty() && !g_star.voice.empty()) j.manifest += "call_gain=" + g + "\n";
  if (g_star.entrance >= 0) j.manifest += "entrance=" + std::to_string(g_star.entrance) + "\n";
  if (g_star.victory > 0) j.manifest += "victory=" + std::to_string(g_star.victory) + "\n";
  if (const std::string g = DbText(g_star.victory_gain, -20.0f, 6.0f); !g.empty() && !g_star.victory_song.empty()) j.manifest += "victory_gain=" + g + "\n";
  if (g_star.song.empty() && g_star.music > 0) j.manifest += "music=" + std::to_string(g_star.music) + "\n";
  for (int a = 1; a < 4; ++a)
    if (g_star.attire_entrance[a] > 0)
      j.manifest += "attire" + std::to_string(a + 1) + "_entrance=" + std::to_string(g_star.attire_entrance[a]) + "\n";
  if (g_star.announcer[0]) j.manifest += std::string("announcer=") + g_star.announcer + "\n";
  if (g_star.pose >= 0) j.manifest += "pose=" + std::to_string(g_star.pose) + "\n";
  if (g_star.hometown_city[0])
    j.manifest += std::string("hometown=") + g_star.hometown_city + (g_star.hometown_state[0] ? std::string(",") + g_star.hometown_state : "") + "\n";
  if (g_star.own_abilities) {
    j.manifest += "abilities=";
    for (size_t k = 0; k < g_star.abilities.size(); ++k) j.manifest += (k ? "," : "") + std::to_string(g_star.abilities[k]);
    j.manifest += "\n";
  }
  if (!g_star.moves.empty() || !g_star.moveset.empty()) j.manifest += "moves=moves.txt\n";
  j.moves = g_star.moves, j.pack = g_star.pack, j.moveset = g_star.moveset;
  j.victory_song = g_star.victory_song;
  j.model = g_star.model, j.song = g_star.song, j.movie = g_star.movie, j.voice = g_star.voice, j.cae = g_star.cae;
  j.threads = g_star.threads;
  j.gear = g_star.gear;
  j.mats = g_star.mats;
  for (int a = 1; a < 4; ++a) j.attires[a] = g_star.attires[a], j.attire_names[a] = g_star.attire_names[a];
  j.picture = g_star.picture.image, j.picture_small = g_star.picture_small, j.picture_full = g_star.picture_full;
  j.picture_load = g_star.picture_load;
  for (const auto& s : g_star.signs) j.signs.push_back(s.image);
  return true;
}

// The mod's files (in the background).
bool BuildFiles(Job& j, std::vector<ZipEntry>& files) {
  Progress("Reading the model ...");
  Bytes ch;
  if (!ReadFile(Utf8(j.model), ch) || ch.size() < 0x4000 || std::memcmp(ch.data(), "EPK8", 4)) {
    Status("The model is not a character model pac (EPK8): " + Utf8(j.model));
    return false;
  }
  std::string man = j.manifest;
  if (j.picture_small.w) {
    Progress("Putting the bust in the model's portrait ...");
    SetPortrait(ch, j.picture_small);
  }
  {  // entrance gear choices for the model's own attires
    std::map<int, std::string> ops;
    for (const auto& [a, op] : j.gear)
      if (a >= 1 && a <= 4 && (a == 1 || j.attires[a - 1].empty())) ops[a] = op;
    Epac e;
    std::string err;
    if (!ops.empty() && EpacRead(ch, e)) {
      if (PacApplyGear(e, ops, &err)) ch = EpacWrite(e);
      else Log("entrance gear: " + err);
    }
  }
  // the material panel's edits: the model's own attires (and their gear) here, the other pacs' below
  auto patch = [&](Bytes& pac, int view_attire, int attire_in_pac) {
    for (const bool gear : {false, true}) {
      std::map<std::pair<uint32_t, int>, MatEdit> ed;
      for (const auto& [k, e] : j.mats)
        if (std::get<0>(k) == view_attire && std::get<1>(k) == gear) ed[{std::get<2>(k), std::get<3>(k)}] = e;
      std::string err;
      if (!ed.empty() && !PatchMaterials(pac, attire_in_pac, gear, ed, &err)) Log("materials, attire " + std::to_string(view_attire) + ": " + err);
    }
  };
  for (int a = 1; a <= 4; ++a)
    if (a == 1 || j.attires[a - 1].empty()) patch(ch, a, a);
  files.push_back({"ch.pac", std::move(ch)});
  auto add = [&](const std::wstring& src, const std::string& stem, const char* key, const char* what) {
    if (src.empty()) return true;
    Bytes d;
    if (!ReadFile(Utf8(src), d)) {
      Status(std::string("Could not read the ") + what + ": " + Utf8(src));
      return false;
    }
    const std::string n = stem + Utf8(fs::path(src).extension().wstring());
    files.push_back({n, std::move(d)});
    man += std::string(key) + "=" + n + "\n";
    return true;
  };
  if (!add(j.song, "theme", "song", "theme song")) return false;
  if (!add(j.victory_song, "victory", "victory_song", "victory track")) return false;
  for (int a = 1; a < 4; ++a) {
    if (j.attires[a].empty()) continue;
    Bytes pac;
    if (!ReadFile(Utf8(j.attires[a]), pac) || pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
      Status("Attire " + std::to_string(a + 1) + " is not a character model pac (EPK8).");
      return false;
    }
    const std::string n = "attire" + std::to_string(a + 1) + ".pac";
    if (j.picture_small.w) SetPortrait(pac, j.picture_small);
    if (const auto g = j.gear.find(a + 1); g != j.gear.end() && g->second == "off") {  // (its first attire's gear left out)
      Epac e;
      std::string err;
      if (EpacRead(pac, e) && PacRemoveGear(e, 1, &err)) pac = EpacWrite(e);
    }
    patch(pac, a + 1, 1);
    files.push_back({n, std::move(pac)});
    man += "attire" + std::to_string(a + 1) + "=" + n + "\n";
    if (!j.attire_names[a].empty()) man += "attire" + std::to_string(a + 1) + "_name=" + j.attire_names[a] + "\n";
  }
  if (!add(j.voice, "voice", "voice", "name recording")) return false;
  if (!j.movie.empty()) {  // (an .mp4: a Bink is shrunk to one; the Bink beside it with "Keep the Bink too")
    std::string why;
    const std::string name = ModMovieFiles(j.movie, "movie", files, &why);
    if (name.empty()) {
      Status("The entrance movie can't go in the mod: " + why + ".");
      return false;
    }
    man += "movie=" + name + "\n";
  }
  if (!j.threads.empty()) files.push_back({"threads.txt", Bytes(j.threads.begin(), j.threads.end())});  // (Threads parts tab)
  if (!j.cae.empty()) {  // (a created entrance: the game's block as it wrote it - superstar_mods.cpp cae=)
    Bytes c;
    if (!ReadFile(Utf8(j.cae), c) || c.size() != 284) {
      Status("The created entrance's file is missing or damaged: pick it again (Created entrance).");
      return false;
    }
    files.push_back({"cae.bin", std::move(c)});
    man += "cae=cae.bin\n";
  }
  if (!j.moves.empty() || !j.moveset.empty()) {  // the file's lines, then the page's picks (later lines win)
    Bytes t;
    if (!j.moves.empty() && !ReadFile(Utf8(j.moves), t)) { Status("Could not read the moves list: " + Utf8(j.moves)); return false; }
    if (!t.empty() && t.back() != '\n') t.push_back('\n');
    for (const auto& [slot, id] : j.moveset) {
      char line[32];
      std::snprintf(line, sizeof line, "0x%03X=%d\n", slot * 2, id);
      t.insert(t.end(), line, line + std::strlen(line));
    }
    files.push_back({"moves.txt", std::move(t)});
  }
  if (!j.pack.empty()) {  // the move pack: pack.txt and motions/* as moves/
    Progress("Copying the move pack ...");
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(j.pack, ec)) {
      if (!e.is_regular_file(ec)) continue;
      Bytes f;
      if (!ReadFile(PathStr(e.path()), f)) { Status("Could not read " + PathStr(e.path())); return false; }
      files.push_back({"moves/" + fs::relative(e.path(), j.pack, ec).generic_string(), std::move(f)});
    }
  }
  Progress("Encoding the pictures ...");
  for (size_t k = 0; k < j.signs.size() && k < 4; ++k)
    files.push_back({"sign" + std::to_string(k + 1) + ".dds", DdsEncode(j.signs[k], DxtFormat::kDxt1, true)});
  if (j.picture.w) {
    files.push_back({"render.dds", DdsEncode(j.picture, DxtFormat::kDxt5, false)});
    files.push_back({"render_small.dds", DdsEncode(j.picture_small, DxtFormat::kDxt5, false)});
  }
  if (j.picture_full.w) files.push_back({"render_full.dds", DdsEncode(j.picture_full, DxtFormat::kDxt5, false)});
  if (j.picture_load.w == 1024 && j.picture_load.h == 512)  // (the game takes exactly 1024 x 512 DXT5)
    files.push_back({"load.dds", DdsEncode(j.picture_load, DxtFormat::kDxt5, false)});
  files.insert(files.begin(), ZipEntry{"manifest.txt", Bytes(man.begin(), man.end())});
  AddModProject(files, j.project);
  return true;
}

// The project's own model (an FBX's, a converted one - not one of the game's pac\ch): it gets Superstar Threads parts
// (threads.txt, tools/threads_parts.py) when the mod is saved or installed.
bool OwnModel(const std::wstring& model) {
  if (model.empty()) return false;
  std::error_code ec;
  const std::wstring stock = fs::absolute(fs::path(g_game) / L"pac" / L"ch", ec).wstring(), m = fs::absolute(model, ec).wstring();
  return Lower(Utf8(m)).rfind(Lower(Utf8(stock)), 0) != 0;
}

// The mod's model files (manifest + .pac) into a work folder, threads_parts.py there, its threads.txt added to the
// files; then `then` (in the background), with or without it.
void WithThreadsParts(const std::string& id, std::vector<ZipEntry> files, std::function<void(std::vector<ZipEntry>&)> then) {
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"threads" / fs::u8path(id);
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  for (const auto& f : files)
    if (f.name == "manifest.txt" || (f.name.size() > 4 && f.name.compare(f.name.size() - 4, 4, ".pac") == 0 && f.name.find('/') == std::string::npos))
      WriteFile(PathStr(work / fs::u8path(f.name)), f.data);
  OnUiThread([work, files, then] {
    WhenIdle([work, files, then] {
      MakeThreadsParts(work, [work, files, then](int code, const std::string&) mutable {
        Bytes t;
        if (code == 0 && ReadFile(PathStr(work / L"threads.txt"), t)) files.push_back({"threads.txt", t});
        RunInBackground([files, then]() mutable { then(files); });
      });
    });
  });
}

void SaveMod() {
  Job j;
  if (!PrepareJob(j)) return;
  const std::wstring f = PickFile(true, L"Save the superstar mod", kModFilter, 1, L"svrmod", (Wide(j.id) + L".svrmod").c_str());
  if (f.empty()) return;
  const std::string out = Utf8(f);
  RunInBackground([j, out]() mutable {
    std::vector<ZipEntry> files;
    if (!BuildFiles(j, files)) return;
    auto write = [out](std::vector<ZipEntry>& files) {
      if (WriteFile(out, ZipWrite(files))) Status("Saved " + out + " (add it in the launcher's Mods tab with +).");
      else Status("The mod could not be written: " + out);
    };
    if (OwnModel(j.model) && j.threads.empty()) WithThreadsParts(j.id, std::move(files), write);  // (made in the tab: kept)
    else write(files);
  });
}

void Install(bool start) {
  if (start && GameRunning()) {
    Status("The game is running: close it first (mods load when it starts).");
    return;
  }
  Job j;
  if (!PrepareJob(j)) return;
  RunInBackground([j, start]() mutable {
    std::vector<ZipEntry> files;
    if (!BuildFiles(j, files)) return;
    const fs::path dir = fs::path(g_game) / L"Mods" / L"Superstars" / fs::u8path(j.id);
    std::string why;
    if (!InstallModFiles(dir, files, &why)) {
      Status("Not installed: " + why);
      return;
    }
    auto done = [dir, start] {
      Status("Installed: under the M tile of the character select from the next game start (" + PathStr(dir) + ")." + InstallReport("Superstars", dir));
      if (start) StartGame();
    };
    if (!OwnModel(j.model) || !j.threads.empty()) return done();
    OnUiThread([dir, done] {  // (its own model: its Superstar Threads parts first, then the doctor's report)
      WhenIdle([dir, done] { MakeThreadsParts(dir, [done](int, const std::string&) { RunInBackground(done); }); });
    });
  });
}

// ---- a model from an FBX: the source skeleton, its automatic mapping (rig_map) and the hand edits (g_star.fbx_map)

const COMDLG_FILTERSPEC kFbxFilter[1] = {{L"FBX model (*.fbx)", L"*.fbx"}};
const char* const kFbxCore[] = {"root", "koshi", "mune", "kubi", "atama", "l_sakotsu", "l_ninoude", "l_kote", "l_te",
                                "r_sakotsu", "r_ninoude", "r_kote", "r_te", "l_momo", "l_sune", "l_ashi", "r_momo", "r_sune", "r_ashi"};

struct FbxRig {
  std::wstring path;
  fs::file_time_type time{};
  std::vector<FbxBone> bones;
  std::vector<std::string> auto_map;  // MapRig's
  std::vector<int> depth;
  std::string rig, error;
  std::vector<std::string> notes;  // what the matching decided on its own (a doubled eye bone)
  std::vector<FbxMeshInfo> meshes;
};
FbxRig g_fbx_rig;
bool g_fbx_open_once = false;  // (test aid: the mapping table open)

// The FBX's skeleton (read again when the file or its time changes).
const FbxRig& FbxSkeleton() {
  std::error_code ec;
  const auto t = fs::last_write_time(g_star.fbx, ec);
  if (g_fbx_rig.path == g_star.fbx && g_fbx_rig.time == t) return g_fbx_rig;
  g_fbx_rig = FbxRig();
  g_fbx_rig.path = g_star.fbx, g_fbx_rig.time = t;
  if (g_star.fbx.empty()) return g_fbx_rig;
  if (!ReadFbxSkeleton(Utf8(g_star.fbx), g_fbx_rig.bones, g_fbx_rig.error)) return g_fbx_rig;
  std::vector<RigBone> rb;
  for (const auto& b : g_fbx_rig.bones) rb.push_back({b.name, b.parent});
  g_fbx_rig.auto_map = MapRig(rb);
  g_fbx_rig.rig = DetectRig(rb);
  // two bones for one eye (Eye_L and LeftEye): the one holding weights stays, the other moves with its parent
  for (const char* eye : {"l_eye", "r_eye"}) {
    std::vector<size_t> on;
    for (size_t i = 0; i < g_fbx_rig.auto_map.size(); ++i)
      if (g_fbx_rig.auto_map[i] == eye) on.push_back(i);
    if (on.size() < 2) continue;
    size_t keep = on[0];
    for (size_t i : on)
      if (g_fbx_rig.bones[i].skinned && !g_fbx_rig.bones[keep].skinned) keep = i;
    std::string dropped;
    for (size_t i : on)
      if (i != keep) g_fbx_rig.auto_map[i].clear(), dropped += (dropped.empty() ? "" : ", ") + g_fbx_rig.bones[i].name;
    g_fbx_rig.notes.push_back(std::string(eye[0] == 'l' ? "Two left eye bones: " : "Two right eye bones: ") + g_fbx_rig.bones[keep].name +
                              (g_fbx_rig.bones[keep].skinned ? " (it holds the eye's weights)" : "") + " is the eye; " + dropped +
                              " moves with its parent.");
  }
  std::string merr;
  FbxListMeshes(Utf8(g_star.fbx), g_fbx_rig.meshes, merr);
  for (size_t i = 0; i < g_fbx_rig.bones.size(); ++i) {
    int d = 0;
    for (int p = g_fbx_rig.bones[i].parent; p >= 0 && d < int(g_fbx_rig.bones.size()); p = g_fbx_rig.bones[size_t(p)].parent) ++d;
    g_fbx_rig.depth.push_back(d);
  }
  return g_fbx_rig;
}

// A source bone's 2011 bone: the hand edit, else the automatic one ("" = folds into its parent).
std::string FbxTarget(const FbxRig& r, size_t i) {
  const auto e = g_star.fbx_map.find(r.bones[i].name);
  return e != g_star.fbx_map.end() ? e->second : r.auto_map[i];
}
// Where a bone's weights end up: its own 2011 bone or its nearest mapped parent's ("" = nowhere: above the hips).
std::string FbxWeightsTo(const FbxRig& r, size_t i) {
  for (int p = int(i), n = 0; p >= 0 && n <= int(r.bones.size()); p = r.bones[size_t(p)].parent, ++n)
    if (std::string t = FbxTarget(r, size_t(p)); !t.empty()) return t;
  return {};
}

// fbx_map=<source>><2011>|... ("<source>>" = folds); the same "key>value|..." for the per-mesh keys (bone and mesh
// names have neither '|' nor '>' in the known rigs; a picture path's '>' can't be in a Windows path)
std::string PairsText(const std::map<std::string, std::string>& m) {
  std::string t;
  for (const auto& [k, v] : m) t += (t.empty() ? "" : "|") + k + ">" + v;
  return t;
}
void ParsePairs(const std::string& t, std::map<std::string, std::string>& m) {
  m.clear();
  size_t a = 0;
  while (a < t.size()) {
    size_t b = t.find('|', a);
    if (b == std::string::npos) b = t.size();
    const std::string item = t.substr(a, b - a);
    if (const size_t g = item.find('>'); g != std::string::npos && g > 0) m[item.substr(0, g)] = item.substr(g + 1);
    a = b + 1;
  }
}
std::string FbxMapText() { return PairsText(g_star.fbx_map); }
void ParseFbxMap(const std::string& t) { ParsePairs(t, g_star.fbx_map); }

// The import report: FbxInspect's list for the FBX and the choices (read in the background when they change), and
// what Make found (g_fbx_after: the reduction, the size, the builder's notes, the built model's checks).
std::vector<FbxIssue> g_fbx_report, g_fbx_after;
std::string g_fbx_report_key;  // what g_fbx_report is for
bool g_fbx_report_busy = false;
FbxImportOptions FbxOptions() {
  FbxImportOptions opts;
  for (const auto& [k, v] : g_star.fbx_on) opts.include[k] = v == "1";
  opts.kind = g_star.fbx_kind, opts.picture = g_star.fbx_pic;
  opts.max_vertices = g_star.fbx_reduce, opts.keep_face = g_star.fbx_keep_face;
  opts.hair = g_star.fbx_hair;
  for (const auto& [k, v] : g_star.fbx_cloth) opts.cloth[k] = v == "1";
  for (const auto& [name, e] : g_star.fbx_mat) {  // (the builder's per-mesh material)
    svrfmt::FbxMaterial f;
    if (e.has_spec) f.specular = e.spec;
    if (e.normal == 1) f.normal = Utf8(e.normal_file);
    if (e.has_tint) f.has_tint = true, std::copy(e.tint, e.tint + 3, f.tint);
    f.cutout = e.cutout, f.double_sided = e.two_sided, f.flip = e.flip;
    if (e.spec_alpha) f.specular_alpha = e.spec_alpha == 1 ? "texture" : e.spec_alpha == 2 ? "flat" : "brightness";
    if (!e.shine_mask.empty()) f.shine_mask = Utf8(e.shine_mask);
    opts.material[name] = f;
  }
  return opts;
}
void IssueList(const std::vector<FbxIssue>& list) {
  for (size_t k = 0; k < list.size(); ++k) {
    const FbxIssue& is = list[k];
    ImGui::PushID(int(k));
    static const ImVec4 kCol[3] = {ImVec4(0.6f, 0.75f, 1, 1), ImVec4(1, 0.8f, 0.35f, 1), ImVec4(1, 0.45f, 0.4f, 1)};
    static const char* kLabel[3] = {"Tip:", "Warning:", "Must fix:"};
    const int lv = std::clamp(is.level, 0, 2);
    ImGui::TextColored(kCol[lv], "%s", kLabel[lv]);
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(is.text.c_str());
    if (!is.fix.empty()) ImGui::TextDisabled("Fix it: %s", is.fix.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopID();
  }
}
void FbxReportPanel(const FbxRig& r, bool show = true) {
  std::error_code ec;
  const auto t = fs::last_write_time(g_star.fbx, ec);
  const std::string key = Utf8(g_star.fbx) + "|" + std::to_string(t.time_since_epoch().count()) + "|" + FbxMapText() + "|" +
                          PairsText(g_star.fbx_on) + "|" + PairsText(g_star.fbx_pic) + "|" + std::to_string(g_star.fbx_reduce) +
                          (g_star.fbx_keep_face ? "f" : "") + "|" + std::to_string(g_star.fbx_hair) + "|" + PairsText(g_star.fbx_cloth);
  if (key != g_fbx_report_key && !g_fbx_report_busy && !Busy()) {
    g_fbx_report_busy = true;
    std::vector<std::string> targets;
    for (size_t i = 0; i < r.bones.size(); ++i) targets.push_back(FbxTarget(r, i));
    const std::string fbx = Utf8(g_star.fbx), rig = r.rig;
    const FbxImportOptions opts = FbxOptions();
    RunInBackground([key, fbx, targets, rig, opts] {
      Progress("Checking the model ...");
      auto list = FbxInspect(fbx, targets, rig, opts);
      OnUiThread([key, list] { g_fbx_report = list, g_fbx_report_key = key, g_fbx_report_busy = false; });
    });
  }
  if (!show) return;
  int counts[3] = {};
  for (const auto& is : g_fbx_report) ++counts[std::clamp(is.level, 0, 2)];
  const std::string head = g_fbx_report_key.empty() ? std::string("Model check: reading ...")
                           : g_fbx_report.empty()   ? std::string("Model check: nothing wrong found")
                                                    : "Model check: " + std::to_string(counts[2]) + " must fix, " + std::to_string(counts[1]) +
                                                          " warnings, " + std::to_string(counts[0]) + " tips";
  if (g_fbx_report.empty()) {
    ImGui::TextDisabled("%s", head.c_str());
    return;
  }
  if (counts[2]) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::TreeNode("fbxreport", "%s", head.c_str())) {
    IssueList(g_fbx_report);
    ImGui::TreePop();
  }
}

// The FBX -> build.json (FbxToBuildInput) -> ch.pac (tools/svr11_build.py), in <game>\Mods\.convert\<name>_fbx.
constexpr const wchar_t* kFbxBuilder = L"svr11_build.py";
void MakeFromFbx() {
  const FbxRig& r = FbxSkeleton();
  if (!r.error.empty() || r.bones.empty()) return;
  std::vector<std::string> targets;
  for (size_t i = 0; i < r.bones.size(); ++i) targets.push_back(FbxTarget(r, i));
  const int tid = kStyles[g_star.style].template_id;
  const std::wstring tpl = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(tid) + L".pac")).wstring();
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / (Wide(IdFrom(g_star.name, "superstar")) + L"_fbx");
  const FbxImportOptions opts = FbxOptions();
  const std::string fbx = Utf8(g_star.fbx);
  g_fbx_after.clear();
  auto test_end = [work] {
    if (!g_test_fbx_make) return;
    WriteLogTo((work / L"make.log").wstring());
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  };
  RunInBackground([fbx, targets, tpl, work, opts, test_end] {
    Progress("Posing the FBX into the game's stance ...");
    Bytes pac;
    FbxImportReport rep;
    const bool ok = ReadFile(Utf8(tpl), pac) && FbxToBuildInput(fbx, targets, pac, Utf8(tpl), PathStr(work), rep, opts);
    for (const auto& w : rep.warnings) Log("  FBX: " + w);
    for (const auto& w : rep.skipped) Log("  FBX: left out " + w);
    if (!ok) {
      Status("The FBX could not be posed: " + (rep.warnings.empty() ? std::string("is the template ") + Utf8(tpl) + " there?" : rep.warnings.front()));
      Log("FBX: not posed");
      OnUiThread(test_end);
      return;
    }
    Log("FBX: " + std::to_string(rep.meshes) + " meshes, " + std::to_string(rep.vertices) + " vertices, " + std::to_string(rep.textures) +
        " pictures; scaled x" + std::to_string(rep.scale).substr(0, 5) + " to the game's legs (it was " +
        std::to_string(int(rep.height_cm + 0.5)) + " cm tall)");
    const double scale = rep.scale;
    std::vector<FbxIssue> after;
    if (rep.reduced_from) {
      after.push_back({0, "Cut from " + std::to_string(rep.reduced_from) + " to " + std::to_string(rep.vertices) + " vertices to fit the game.",
                       "Look it over in the 3D view; reduce it yourself in Blender for full control."});
      for (const auto& l : rep.reduced_lines) Log("  " + l);  // (each part's count: a part cut too far shows here)
    }
    for (const auto& w : rep.warnings) after.push_back({1, w, ""});
    {
      const double back = 1.0 / std::max(0.01, rep.scale);
      char line[200];
      std::snprintf(line, sizeof line, "It is %d cm tall. The game plays it at the fighting style's size; Height brings it back to x%.2f",
                    int(rep.height_cm + 0.5), std::clamp(back, 0.8, 1.25));
      std::string text = line;
      if (back < 0.8 || back > 1.25) text += std::string(" - the nearest the game allows, so it plays ") + (back < 0.8 ? "taller" : "shorter") + " than it is.";
      else text += ".";
      after.push_back({back < 0.8 || back > 1.25 ? 1 : 0, text, back < 0.8 || back > 1.25 ? "Fine for most models; the moves are made for sizes near the roster's." : ""});
    }
    OnUiThread([after] { g_fbx_after = after; });
    WhenIdle([work, tpl, scale, test_end] {
      const std::wstring in = (work / L"build.json").wstring(), out = (work / L"ch.pac").wstring(), report = (work / L"report.json").wstring();
      std::error_code ec;
      fs::remove(out, ec);
      const bool started = RunToolJson(kFbxBuilder, {in, out, report}, [out, scale, test_end](int code, const std::string& text) {
        // its answer: {"ok", "error", "warnings": [...], "attires": [{"pages", "dropped": [...]}]}
        Json j;
        const bool parsed = ParseJson(text, j);
        if (parsed)
          for (const auto& w : j["warnings"].items) Log("  model: " + w.Str());
        if (code != 0 || !fs::exists(out)) {
          const std::string why = parsed ? j["error"].Str() : std::string();
          Status("The model could not be built from the FBX" + (why.empty() ? std::string(" (see the log).") : ": " + why));
          g_fbx_after.push_back({2, "The model couldn't be built" + (why.empty() ? std::string(" (the log says why).") : ": " + why), "Leave out a part or two in the Outfit step, or reduce it in Blender, then Make again."});
          Log("svr11_build (" + std::to_string(code) + "): " + text);
          test_end();
          return;
        }
        if (parsed) {
          for (const auto& a : j["attires"].items) {
            Log("FBX model: " + a["pages"].Str() + " of " + std::to_string(kModelPages) + " memory pages" +
                (a["dropped"].items.empty() ? std::string() : ", " + std::to_string(a["dropped"].items.size()) + " blood layers left out to fit"));
            g_fbx_after.push_back({a["dropped"].items.empty() ? 0 : 1, "Uses " + a["pages"].Str() + " of the " + std::to_string(kModelPages) + " memory pages a wrestler gets" +
                                   (a["dropped"].items.empty() ? std::string(".") : "; its blood layers were left out to fit (no blood on it in matches)."), ""});
          }
          for (const auto& w : j["warnings"].items) g_fbx_after.push_back({0, w.Str(), ""});
        }
        {
          std::vector<Problem> checks;
          ModelCheckProblems(out, "The built model", checks);
          for (const auto& c : checks) g_fbx_after.push_back({c.error ? 2 : 1, c.text, c.fix});
        }
        g_star.model = out;
        // the game holds the hips at the fighting style's height: the model was scaled to it; height= brings it back
        const double back = 1.0 / std::max(0.01, scale);
        g_star.height = float(std::clamp(back, 0.8, 1.25));
        Touch();
        Status("Built the model from the FBX" + std::string(back < 0.8 || back > 1.25 ? " (its own size is beyond the height range: set to the nearest)" : "") + ".");
        Log("FBX: built " + Utf8(out) + ", height " + std::to_string(g_star.height).substr(0, 4));
        test_end();
      });
      if (!started) Status("The model builder (" + Utf8(kFbxBuilder) + ") couldn't start: see the log."), Log("FBX: builder not started"), test_end();
    });
  });
}

bool FbxMaterialCell(const std::string& mesh, bool simple);

// ---- the FBX import, step by step (the Simple Look step's "Import a 3D model", the full page's FBX row): 1 the
// file, 2 its skeleton, 3 its materials, 4 the outfit (which parts), 5 the check and Make - one step shown at a time
int g_fbx_step = 0;
const char* const kFbxSteps[] = {"Model", "Skeleton", "Materials", "Outfit", "Check and make"};
constexpr int kFbxStepCount = 5;

// A mesh's material ("node / material": the part after the slash; with none, the node's name).
std::string FbxMaterialOf(const std::string& mesh) {
  const size_t s = mesh.find(" / ");
  return s == std::string::npos ? mesh : mesh.substr(s + 3);
}
bool FbxMeshOn(const FbxMeshInfo& m) {
  const auto on = g_star.fbx_on.find(m.name);
  return on != g_star.fbx_on.end() ? on->second == "1" : !m.hidden;
}

// Thumbnails: each mesh's picture as its material has it (FbxMeshPictures, read off the UI thread once per file),
// a picture picked for it instead (by path).
struct FbxThumbs {
  std::wstring fbx;
  fs::file_time_type time{};
  bool busy = false;
  std::map<std::string, Picture> pics;
};
FbxThumbs g_fbx_thumbs;
std::map<std::string, Picture> g_file_thumbs;
Picture* FileThumb(const std::string& path) {
  auto it = g_file_thumbs.find(path);
  if (it == g_file_thumbs.end()) {
    Image img;
    Picture p;
    if (LoadPicture(Wide(path), img) && img.w > 0 && img.h > 0) {
      const double f = std::min(1.0, 96.0 / std::max(img.w, img.h));
      p.Set(Resize(img, std::max(1, int(img.w * f)), std::max(1, int(img.h * f))));
    }
    it = g_file_thumbs.emplace(path, std::move(p)).first;
  }
  return &it->second;
}
Picture* FbxThumb(const std::string& mesh) {
  if (const auto p = g_star.fbx_pic.find(mesh); p != g_star.fbx_pic.end()) return FileThumb(p->second);
  std::error_code ec;
  const auto t = fs::last_write_time(g_star.fbx, ec);
  if ((g_fbx_thumbs.fbx != g_star.fbx || g_fbx_thumbs.time != t) && !g_fbx_thumbs.busy) {
    g_fbx_thumbs = FbxThumbs();
    g_fbx_thumbs.fbx = g_star.fbx, g_fbx_thumbs.time = t, g_fbx_thumbs.busy = true;
    const std::wstring fbx = g_star.fbx;
    std::thread([fbx, t] {
      auto got = std::make_shared<std::map<std::string, Image>>();
      FbxMeshPictures(Utf8(fbx), 96, *got);
      OnUiThread([fbx, t, got] {
        if (g_fbx_thumbs.fbx != fbx || g_fbx_thumbs.time != t) return;
        for (auto& [k, img] : *got) g_fbx_thumbs.pics[k].Set(std::move(img));
        g_fbx_thumbs.busy = false;
      });
    }).detach();
  }
  const auto it = g_fbx_thumbs.pics.find(mesh);
  return it == g_fbx_thumbs.pics.end() ? nullptr : &it->second;
}
// A thumbnail in a side x side box (a grey box saying so when there's none).
void Thumb(Picture* p, float side, const char* none) {
  if (p && !p->Empty()) {
    p->Draw(side, side);
    return;
  }
  const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + side, a.y + side);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, IM_COL32(70, 70, 76, 255), 4 * g_scale);
  const ImVec2 ts = ImGui::CalcTextSize(none);
  dl->AddText(ImVec2(a.x + (side - ts.x) / 2, a.y + (side - ts.y) / 2), IM_COL32(170, 170, 180, 255), none);
  ImGui::Dummy(ImVec2(side, side));
}
// Has the picture see-through places (an alpha below 255 somewhere)?
bool HasClearPlaces(Picture* p) {
  if (!p || p->Empty()) return false;
  for (size_t q = 3; q < p->image.rgba.size(); q += 4)
    if (p->image.rgba[q] < 200) return true;
  return false;
}

// A picture choice in a row: its label, the file's name (or `none`), Pick... and x. True when it changed.
bool PictureChoice(const char* label, std::string& file, const char* none, const char* tip, const wchar_t* title) {
  bool ch = false;
  ImGui::PushID(label);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  ImGui::SameLine(150 * g_scale);
  if (file.empty()) ImGui::TextDisabled("%s", none);
  else ImGui::TextUnformatted(FileName(Wide(file)).c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Pick...")) {
    const std::wstring f = PickFile(false, title, kPictureFilter, 1);
    if (!f.empty()) file = Utf8(f), ch = true;
  }
  if (!file.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) file.clear(), ch = true;
  }
  ImGui::PopID();
  return ch;
}

// The materials of the FBX (step 3, and the Simple Look step's Materials tab): one row per material - its picture,
// the pictures for its colour, bumps and shine, both sides / flip faces / see-through - for every part that uses it
// (each part on its own below). Used when the model is made. True when something changed.
bool FbxMaterials(bool simple, bool intro) {
  const FbxRig& r = FbxSkeleton();
  if (g_star.fbx.empty() || !r.error.empty() || r.meshes.empty()) {
    ImGui::TextDisabled("Pick an FBX first (step 1).");
    return false;
  }
  std::vector<std::pair<std::string, std::vector<const FbxMeshInfo*>>> groups;  // (the FBX's order)
  for (const auto& m : r.meshes) {
    const std::string mat = FbxMaterialOf(m.name);
    auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return x.first == mat; });
    if (g == groups.end()) groups.push_back({mat, {}}), g = groups.end() - 1;
    g->second.push_back(&m);
  }
  if (intro) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Each material of your model with its picture. What you choose here is used when the model is made, "
                        "for every part that uses the material.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  }
  bool changed = false;
  const float side = 76 * g_scale;
  for (size_t gi = 0; gi < groups.size(); ++gi) {
    const auto& [mat, parts] = groups[gi];
    ImGui::PushID(int(gi));
    auto edit_of = [&](const std::string& mesh) {
      const auto it = g_star.fbx_mat.find(mesh);
      return it == g_star.fbx_mat.end() ? MatEdit() : it->second;
    };
    auto pic_of = [&](const std::string& mesh) {
      const auto it = g_star.fbx_pic.find(mesh);
      return it == g_star.fbx_pic.end() ? std::string() : it->second;
    };
    const std::string first = parts.front()->name;
    MatEdit e = edit_of(first);
    bool mixed = false;
    for (const auto* p : parts) {
      const MatEdit o = edit_of(p->name);
      mixed |= o.cutout != e.cutout || o.two_sided != e.two_sided || o.flip != e.flip || o.normal_file != e.normal_file ||
               o.shine_mask != e.shine_mask || pic_of(p->name) != pic_of(first);
    }
    // (an edit for the whole material: on each of its parts)
    auto apply = [&](const std::function<void(MatEdit&)>& fn) {
      for (const auto* p : parts) {
        MatEdit x = edit_of(p->name);
        fn(x);
        if (x.Empty()) g_star.fbx_mat.erase(p->name);
        else g_star.fbx_mat[p->name] = x;
      }
      changed = true;
    };
    Picture* pic = FbxThumb(first);
    ImGui::BeginGroup();
    Thumb(pic, side, parts.front()->has_texture || g_star.fbx_pic.count(first) ? "..." : "none");
    ImGui::EndGroup();
    ImGui::SameLine(0, 14 * g_scale);
    ImGui::BeginGroup();
    ImGui::TextUnformatted(mat.c_str());
    ImGui::SameLine();
    int on = 0;
    for (const auto* p : parts) on += FbxMeshOn(*p);
    ImGui::TextDisabled("%d part%s%s%s", int(parts.size()), parts.size() == 1 ? "" : "s", on < int(parts.size()) ? (", " + std::to_string(parts.size() - on) + " left out").c_str() : "",
                        mixed ? ", set apart" : "");
    if (ImGui::IsItemHovered()) {
      std::string names;
      for (const auto* p : parts) names += p->name + (FbxMeshOn(*p) ? "" : "  (left out in step 4)") + "\n";
      ImGui::SetTooltip("%s", names.c_str());
    }
    std::string colour = pic_of(first);
    if (PictureChoice("Colour picture", colour, parts.front()->has_texture ? "its own (from the FBX)" : "none - plain grey",
                      "The picture painted on it. Pick one when the FBX has none, or to use another.", L"The material's colour picture")) {
      for (const auto* p : parts) {
        if (colour.empty()) g_star.fbx_pic.erase(p->name);
        else g_star.fbx_pic[p->name] = colour;
      }
      changed = true;
    }
    std::string bumps = e.normal == 1 ? Utf8(e.normal_file) : "";
    if (PictureChoice(simple ? "Bumps picture" : "Normal map", bumps, "its own (or none)",
                      "A normal map (the blue-purple picture) for small bumps: folds, seams, muscles.", L"The material's normal map (PNG)"))
      apply([&](MatEdit& x) { x.normal = bumps.empty() ? 0 : 1, x.normal_file = Wide(bumps); });
    std::string shine = Utf8(e.shine_mask);
    if (PictureChoice("Shine picture", shine, "none - the same shine all over",
                      "A grey picture of where it shines: white shines most, black not at all (wet skin, leather, metal parts).\n"
                      "It goes where the game reads it - on skin parts that's the bumps picture's alpha.", L"Where it shines (a grey picture)"))
      apply([&](MatEdit& x) { x.shine_mask = Wide(shine); });
    // the faces
    bool both = e.two_sided == 1, flip = e.flip == 1, cut = e.cutout == 1;
    if (ImGui::Checkbox("Draw both sides", &both)) apply([&](MatEdit& x) { x.two_sided = both ? 1 : -1; });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The game draws its back too (its flag 0x2 off): capes, open jackets, skirts, hair cards, single-layer\n"
                        "cloth - anything you can see the inside of. Off: only its front, as the game's bodies.\n"
                        "On a skin part the game then uses its plain shader: no sweat in matches.");
    ImGui::SameLine(0, 16 * g_scale);
    if (ImGui::Checkbox("Flip faces", &flip)) apply([&](MatEdit& x) { x.flip = flip ? 1 : -1; });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Turns its faces the other way round. The import already turns every part to the game's way, so use\n"
                        "this only for a part that still shows inside out (its faces point inward in the FBX).");
    ImGui::SameLine(0, 16 * g_scale);
    if (ImGui::Checkbox(simple ? "See-through where clear" : "See-through cut-out", &cut)) apply([&](MatEdit& x) { x.cutout = cut ? 1 : -1; });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Where the colour picture is transparent the game doesn't draw it: lace, mesh, torn cloth, hair cards.\n"
                        "The cut is at an alpha of about 0.2-0.3 (50-80 of 255): fainter than that is a hole, the rest is solid -\n"
                        "the game has no half see-through on superstars. Needs a picture with transparency (PNG).\n"
                        "On a skin part its sweat is turned off (the game's skin shader can't cut).");
    {  // (the flat look: no shine, no bumps - the game's superstar shaders have no unlit / self-lit setting)
      bool flat = e.has_spec && e.spec <= 0.0f && e.spec_alpha == 2;
      if (ImGui::Checkbox("Flat look", &flat))
        apply([&](MatEdit& x) {
          if (flat) x.has_spec = true, x.spec = 0, x.spec_alpha = 2, x.normal = 0, x.normal_file.clear(), x.shine_mask.clear();
          else x.has_spec = false, x.spec_alpha = 0;
        });
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Flat, cartoon-like shading: no shine (specular 0, even) and no bumps (a flat normal map) - the closest the\n"
                          "game's superstar shaders get to unlit: they have no self-lit setting, so the arena's lights still shade it.");
    }
    if (cut && pic && !pic->Empty() && !HasClearPlaces(pic))
      ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Its picture has no transparent places: nothing will be cut.");
    if (ImGui::TreeNode("more", "More: shine level, colour")) {
      bool hs = e.has_spec;
      if (ImGui::Checkbox(simple ? "My own shine level" : "Shine (specular level)", &hs)) apply([&](MatEdit& x) { x.has_spec = hs; });
      if (e.has_spec) {
        ImGui::SameLine();
        float v = e.spec;
        ImGui::SetNextItemWidth(160 * g_scale);
        if (ImGui::SliderFloat("##spec", &v, 0, 4, "%.2f", ImGuiSliderFlags_Logarithmic)) apply([&](MatEdit& x) { x.has_spec = true, x.spec = v; });
      }
      static const char* kAlpha[] = {"as the builder does", "the picture's alpha", "even (flat)", "the picture's brightness"};
      int sa = e.spec_alpha;
      ImGui::SetNextItemWidth(200 * g_scale);
      if (ImGui::Combo(simple ? "Where it shines" : "Shine pattern", &sa, kAlpha, 4)) apply([&](MatEdit& x) { x.spec_alpha = sa; });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Without a shine picture: which places shine more.");
      bool ht = e.has_tint;
      float tint[3] = {e.tint[0], e.tint[1], e.tint[2]};
      if (ImGui::Checkbox(simple ? "Colour it" : "Colour (multiplier)", &ht)) apply([&](MatEdit& x) { x.has_tint = ht; });
      if (e.has_tint) {
        ImGui::SameLine();
        if (ImGui::ColorEdit3("##tint", tint, ImGuiColorEditFlags_NoInputs)) apply([&](MatEdit& x) { x.has_tint = true, std::copy(tint, tint + 3, x.tint); });
      }
      ImGui::TreePop();
    }
    if (parts.size() > 1 && ImGui::TreeNode("each", "Set each part on its own")) {
      for (const auto* p : parts) {
        ImGui::PushID(p->name.c_str());
        changed |= FbxMaterialCell(p->name, simple);
        ImGui::SameLine();
        ImGui::TextUnformatted(p->name.c_str());
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (changed) Touch();
  return changed;
}

// The Make button (step 5; the Materials tab's "Make it again").
void FbxMakeButton(const char* label, float width) {
  const FbxRig& r = FbxSkeleton();
  ImGui::BeginDisabled(Busy() || !r.error.empty() || r.bones.empty());
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.12f, 0.2f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.2f, 0.28f, 1));
  if (ImGui::Button(label, ImVec2(width * g_scale, 34 * g_scale))) MakeFromFbx();
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Poses it into the game's stance with the skeleton matching, moves its weights to the 2011 bones, packs its\n"
                      "pictures into one sheet and builds the game's model file on the fighting style's superstar.");
}

void FbxRows(bool simple) {
  if (!simple && g_star.fbx.empty()) {  // (the full page: one row until an FBX is picked)
    FileRow("Model from an FBX (optional)...", g_star.fbx, "none", kFbxFilter, 1,
            "A rigged model (Unity / VRChat, Mixamo, Blender Rigify, 3ds Max Biped, UE Mannequin and rigs named like them):\n"
            "its bones are matched to the 2011 skeleton by their names, then it's made into the game's model in five steps.");
    return;
  }
  const FbxRig& r = FbxSkeleton();
  const bool picked = !g_star.fbx.empty(), readable = picked && r.error.empty();
  int mapped = 0;
  std::set<std::string> have;
  if (readable)
    for (size_t i = 0; i < r.bones.size(); ++i)
      if (std::string t = FbxTarget(r, i); !t.empty()) ++mapped, have.insert(t);
  std::string missing_list;
  for (const char* c : kFbxCore)
    if (!have.count(c)) missing_list += (missing_list.empty() ? "" : ", ") + std::string(c);
  int on = 0, tris = 0;
  for (const auto& m : r.meshes)
    if (FbxMeshOn(m)) ++on, tris += m.triangles;
  if (readable) FbxReportPanel(r, false);  // (the model check keeps up in the background)
  int counts[3] = {};
  for (const auto& is : g_fbx_report) ++counts[std::clamp(is.level, 0, 2)];
  bool made = false;
  for (const auto& is : g_fbx_after) made |= is.level < 2;
  const bool done[kFbxStepCount] = {readable, readable && missing_list.empty(), readable, readable && on > 0, made};
  if (!readable) g_fbx_step = 0;
  g_fbx_step = std::clamp(g_fbx_step, 0, kFbxStepCount - 1);
  if (!simple) ImGui::TextUnformatted("Model from an FBX"), ImGui::Indent();  // (the full page: under the Files rows)

  // the steps, a row of buttons (wrapped to the card's width)
  {
    const float avail = ImGui::GetContentRegionAvail().x, gap = 6 * g_scale;
    float x = 0;
    for (int i = 0; i < kFbxStepCount; ++i) {
      char label[64];
      std::snprintf(label, sizeof label, "%d  %s##fbxstep%d", i + 1, kFbxSteps[i], i);
      const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2 + 4 * g_scale;
      if (i && x + gap + w <= avail) ImGui::SameLine(0, gap), x += gap;
      else if (i) x = 0;
      x += w;
      const bool cur = i == g_fbx_step;
      int pushed = 0;
      if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.12f, 0.2f, 1)), ++pushed;
      else if (done[i]) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1)), ++pushed;
      ImGui::BeginDisabled(i > 0 && !readable);
      if (ImGui::Button(label, ImVec2(w, 0))) g_fbx_step = i;
      ImGui::EndDisabled();
      ImGui::PopStyleColor(pushed);
    }
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  auto heading = [&](const char* line) {
    ImGui::Text("%d. %s", g_fbx_step + 1, kFbxSteps[g_fbx_step]);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", line);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
  };

  switch (g_fbx_step) {
    case 0: {
      heading("A rigged model (one with a skeleton) from Blender, Unity / VRChat, Mixamo, 3ds Max or UE: its bones are "
              "matched to the game's by their names.");
      FileRow("Pick an FBX file...", g_star.fbx, "none yet", kFbxFilter, 1, "An .fbx with a skeleton and its skin weights.");
      if (!picked) break;
      ImGui::Spacing();
      if (!r.error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "It could not be read: %s", r.error.c_str());
        break;
      }
      int pics = 0;
      for (const auto& m : r.meshes) pics += m.has_texture || g_star.fbx_pic.count(m.name);
      ImGui::Text("Skeleton:");
      ImGui::SameLine(110 * g_scale);
      ImGui::TextDisabled("%s, %d bones", r.rig.c_str(), int(r.bones.size()));
      ImGui::Text("Parts:");
      ImGui::SameLine(110 * g_scale);
      ImGui::TextDisabled("%d (%d in the outfit, %d triangles)", int(r.meshes.size()), on, tris);
      ImGui::Text("Pictures:");
      ImGui::SameLine(110 * g_scale);
      if (pics < int(r.meshes.size())) ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "%d of %d parts have one (pick the rest in step 3)", pics, int(r.meshes.size()));
      else ImGui::TextDisabled("every part has one");
      break;
    }
    case 1: {
      heading("Each of your model's bones is matched to one of the game's, so the game's moves move it. Most rigs match "
              "by themselves: change a bone only when it's wrong.");
      if (missing_list.empty()) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "All the main bones are matched (%d of %d bones).", mapped, int(r.bones.size()));
      else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "Not matched yet: %s - those body parts won't move in the game.", missing_list.c_str());
        ImGui::PopTextWrapPos();
      }
      for (const auto& n : r.notes) ImGui::TextDisabled("%s", n.c_str());
      ImGui::BeginDisabled(g_star.fbx_map.empty());
      if (ImGui::SmallButton("Back to the automatic matching")) g_star.fbx_map.clear(), Touch();
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextDisabled("(%d changed by hand; a bone left unmatched moves with its parent)", int(g_star.fbx_map.size()));
      // moving hair and cloth: the game's spring chains
      std::vector<std::string> cur_targets;
      for (size_t i = 0; i < r.bones.size(); ++i) cur_targets.push_back(FbxTarget(r, i));
      std::map<std::string, bool> cloth_ticks;
      for (const auto& [k, v] : g_star.fbx_cloth) cloth_ticks[k] = v == "1";
      const FbxChains chains = FindFbxChains(r.bones, cur_targets, g_star.fbx_hair, cloth_ticks);
      std::vector<int> chain_of(r.bones.size(), -1);  // (the bone's chain: 0 hair, 1 cloth)
      for (const auto& c : chains.hair)
        for (int b : c) chain_of[size_t(b)] = 0;
      for (const auto& c : chains.cloth)
        for (int b : c) chain_of[size_t(b)] = 1;
      {
        static const char* kHair[] = {"its hair bones, not VRChat's secondary (J_Sec_*) ones", "none (the hair moves with the head)", "every hair bone"};
        int h = g_star.fbx_hair < 0 ? 0 : g_star.fbx_hair == 0 ? 1 : 2;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Moving hair:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(320 * g_scale);
        if (ImGui::Combo("##hair", &h, kHair, 3)) g_star.fbx_hair = h == 0 ? -1 : h == 1 ? 0 : 1, Touch();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The game moves up to 16 chains of hair or cloth with springs (4 bones or more each). Bones left out move\n"
                            "with the head: VRChat's secondary hair bones aren't needed for that.");
        ImGui::SameLine();
        ImGui::TextDisabled("%d strand(s)%s", int(chains.hair.size()), chains.secondary ? (", " + std::to_string(chains.secondary) + " J_Sec_* left out").c_str() : "");
        ImGui::TextDisabled("Moving cloth: %d chain(s) from the bones ticked in the Cloth column (jacket, coat, skirt, cape... ticked by their name).",
                            int(chains.cloth.size()));
        if (chains.over) ImGui::TextColored(ImVec4(1, 0.8f, 0.35f, 1), "%d chain(s) past the 16 the game moves: they ride on their bone.", chains.over);
      }
      ImGui::Spacing();
      const auto& targets = TargetBones();
      const float h = std::max(260.0f * g_scale, ImGui::GetContentRegionAvail().y - 60 * g_scale);
      if (ImGui::BeginTable("fbxmap", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Your model's bone");
        ImGui::TableSetupColumn("The game's bone");
        ImGui::TableSetupColumn("Moves with");
        ImGui::TableSetupColumn("Cloth", ImGuiTableColumnFlags_WidthFixed, 56 * g_scale);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.bones.size(); ++i) {
          const auto& b = r.bones[i];
          const std::string cur = FbxTarget(r, i);
          const bool edited = g_star.fbx_map.count(b.name) > 0;
          ImGui::PushID(int(i));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Indent(float(std::min(r.depth[i], 12)) * 8 * g_scale + 0.001f);
          if (b.skinned) ImGui::TextUnformatted(b.name.c_str());
          else ImGui::TextDisabled("%s", b.name.c_str());
          if (!b.skinned && ImGui::IsItemHovered()) ImGui::SetTooltip("No vertices use this bone.");
          ImGui::Unindent(float(std::min(r.depth[i], 12)) * 8 * g_scale + 0.001f);
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-1);
          if (edited) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.35f, 1));
          const std::string shown = cur.empty() ? std::string("(moves with its parent)") : cur;
          if (ImGui::BeginCombo("##t", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (edited) ImGui::PopStyleColor();
            auto pick = [&](const std::string& t) {
              if (t == r.auto_map[i]) g_star.fbx_map.erase(b.name);
              else g_star.fbx_map[b.name] = t;
              Touch();
            };
            if (ImGui::Selectable("(moves with its parent)", cur.empty())) pick("");
            for (const auto& t : targets)
              if (ImGui::Selectable(t.c_str(), t == cur)) pick(t);
            ImGui::EndCombo();
          } else if (edited) {
            ImGui::PopStyleColor();
          }
          if (edited && ImGui::IsItemHovered())
            ImGui::SetTooltip("Changed (the automatic choice: %s)", r.auto_map[i].empty() ? "moves with its parent" : r.auto_map[i].c_str());
          ImGui::TableNextColumn();
          const std::string to = FbxWeightsTo(r, i);
          if (chain_of[i] == 0) ImGui::TextColored(ImVec4(0.55f, 0.75f, 1, 1), "moving hair");
          else if (chain_of[i] == 1) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "moving cloth");
          else if (cur.empty()) ImGui::TextDisabled("%s", to.empty() ? "nothing (above the hips)" : to.c_str());
          ImGui::TableNextColumn();
          if (cur.empty()) {  // (only an unmatched bone can be cloth)
            const auto ov = g_star.fbx_cloth.find(b.name);
            bool tick = ov != g_star.fbx_cloth.end() ? ov->second == "1" : IsClothBoneName(b.name);
            if (ImGui::Checkbox("##cloth", &tick)) {
              if (tick == IsClothBoneName(b.name)) g_star.fbx_cloth.erase(b.name);
              else g_star.fbx_cloth[b.name] = tick ? "1" : "0";
              Touch();
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Loose cloth (a jacket's tail, a skirt, a cape, a ribbon): the game swings it with a spring chain - 3 bones\n"
                                "in a row or more. Unticked: it moves stiffly with its parent bone.");
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      break;
    }
    case 2:
      heading("Each material of your model with its picture, and how the game draws it - for every part that uses it. "
              "Used when the model is made (step 5).");
      FbxMaterials(simple, false);
      break;
    case 3: {
      heading("The parts your superstar wears. Hidden parts (an avatar's toggled-off clothes) start left out. This outfit "
              "becomes attire 1; more attires come in the Attires step.");
      static const char* kKinds[] = {"skin", "eye", "hair", "lash"};
      static const char* kKindNames[] = {"body / clothes", "eye", "hair", "eyelashes"};
      const float h = std::min(std::max(200.0f * g_scale, ImGui::GetContentRegionAvail().y - 60 * g_scale), (40.0f + 32.0f * float(r.meshes.size())) * g_scale);
      if (ImGui::BeginTable("fbxmeshes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Part");
        ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 84 * g_scale);
        ImGui::TableSetupColumn("It is", ImGuiTableColumnFlags_WidthFixed, 140 * g_scale);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.meshes.size(); ++i) {
          const FbxMeshInfo& m = r.meshes[i];
          ImGui::PushID(int(i));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          bool o = FbxMeshOn(m);
          if (ImGui::Checkbox(m.name.c_str(), &o)) {
            if (o == !m.hidden) g_star.fbx_on.erase(m.name);
            else g_star.fbx_on[m.name] = o ? "1" : "0";
            Touch();
          }
          if (m.hidden && ImGui::IsItemHovered()) ImGui::SetTooltip("Hidden in the FBX.");
          ImGui::TableNextColumn();
          ImGui::Text("%d", m.triangles);
          ImGui::TableNextColumn();
          const auto k_it = g_star.fbx_kind.find(m.name);
          const std::string kind = k_it != g_star.fbx_kind.end() ? k_it->second : m.kind;
          int ki = 0;
          for (int k = 0; k < 4; ++k)
            if (kind == kKinds[k]) ki = k;
          ImGui::SetNextItemWidth(-1);
          if (ImGui::BeginCombo("##kind", kKindNames[ki])) {
            for (int k = 0; k < 4; ++k)
              if (ImGui::Selectable(kKindNames[k], ki == k)) {
                if (m.kind == kKinds[k]) g_star.fbx_kind.erase(m.name);
                else g_star.fbx_kind[m.name] = kKinds[k];
                Touch();
              }
            ImGui::EndCombo();
          }
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the game draws it: skin and clothes, an eye (shiny, it looks around), hair, eyelashes.");
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
      ImGui::TextDisabled("%d of %d parts, %d triangles (the game takes up to about 32,000 vertices - step 5 checks it)", on, int(r.meshes.size()), tris);
      break;
    }
    default: {
      heading("The model checked, then made into the game's model file. Make it again after any change.");
      for (const auto& n : r.notes) ImGui::TextColored(ImVec4(0.6f, 0.75f, 1, 1), "Note:"), ImGui::SameLine(), ImGui::TextWrapped("%s", n.c_str());
      if (g_fbx_report_key.empty()) ImGui::TextDisabled("Checking the model ...");
      else if (g_fbx_report.empty()) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1), "Nothing wrong found.");
      else {
        ImGui::Text("Found: %d must fix, %d warnings, %d tips", counts[2], counts[1], counts[0]);
        ImGui::Indent();
        IssueList(g_fbx_report);
        ImGui::Unindent();
      }
      ImGui::Spacing();
      {  // Reduce detail: only when asked (it used to cut every big model, a face too)
        bool reduce = g_star.fbx_reduce > 0;
        if (ImGui::Checkbox("Reduce detail", &reduce)) g_star.fbx_reduce = reduce ? 10000 : 0, Touch();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Fewer vertices so it fits the game (up to about 32,000 for a wrestler). Off: the model as it is - Make says\n"
                            "when it's too big. For full control reduce it in Blender instead (Decimate modifier).");
        if (reduce) {
          ImGui::SameLine();
          ImGui::SetNextItemWidth(200 * g_scale);
          int v = g_star.fbx_reduce;
          if (ImGui::SliderInt("vertices##reduce", &v, 4000, kReduceMax, "to about %d")) g_star.fbx_reduce = std::clamp(v, 4000, kReduceMax), Touch();
          ImGui::SameLine();
          if (ImGui::Checkbox("Keep the face", &g_star.fbx_keep_face)) Touch();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("The head and its hair left as they are: only the rest of the model is reduced.");
        }
      }
      ImGui::Spacing();
      FbxMakeButton(made ? "Make the model again" : "Make the model", 260);
      if (!g_fbx_after.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("After Make");
        ImGui::Indent();
        IssueList(g_fbx_after);
        ImGui::Unindent();
        if (made) ImGui::TextDisabled("The 3D view shows it. Save or install the superstar in the Finish step.");
      }
      break;
    }
  }
  // Back / Next
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::BeginDisabled(g_fbx_step == 0);
  if (ImGui::Button("< Back##fbx", ImVec2(110 * g_scale, 0))) --g_fbx_step;
  ImGui::EndDisabled();
  if (g_fbx_step + 1 < kFbxStepCount) {
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10 * g_scale, ImGui::GetContentRegionMax().x - 150 * g_scale));
    ImGui::BeginDisabled(!readable);
    char next[64];
    std::snprintf(next, sizeof next, "Next: %s >##fbx", kFbxSteps[g_fbx_step + 1]);
    if (ImGui::Button(next, ImVec2(150 * g_scale, 0))) ++g_fbx_step;
    ImGui::EndDisabled();
  }
  if (!simple) ImGui::Unindent();
}

void FbxProblems(std::vector<Problem>& p) {
  if (g_star.fbx.empty()) return;
  const FbxRig& r = FbxSkeleton();
  if (!r.error.empty()) {
    p.push_back(Error("The FBX file couldn't be opened (" + r.error + ").")
                    .With("Pick another FBX...", [] { GoStep(kStepLook); }));
    return;
  }
  std::set<std::string> have;
  for (size_t i = 0; i < r.bones.size(); ++i)
    if (std::string t = FbxTarget(r, i); !t.empty()) have.insert(t);
  std::string miss;
  for (const char* c : kFbxCore)
    if (!have.count(c)) miss += (miss.empty() ? "" : ", ") + std::string(c);
  if (!miss.empty())
    p.push_back(Warning("Some of the FBX's bones aren't matched to the game's skeleton (" + miss + "): those body parts won't move in the game.")
                    .With("Match them...", [] { g_fbx_step = 1, GoStep(kStepLook); }, "the bone list is open"));
}

// ---- entrance gear (EMD kind 4: the whole wrestler in the outfit worn for the entrance) per attire

// The pac an attire comes from and its attire in it (the model's own, or an attire file's first).
std::pair<std::wstring, int> AttireSource(int a) {
  if (a > 1 && !g_star.attires[a - 1].empty()) return {g_star.attires[a - 1], 1};
  return {g_star.model, a};
}
std::vector<int> GearOf(const std::wstring& pac) {
  std::vector<int> v;
  for (const auto& p : GearBudget(pac))
    if (p.entry.size() >= 8 && std::isdigit(uint8_t(p.entry[6]))) v.push_back(p.entry[6] - '0' + 1);
  return v;
}
int GearPages(const std::wstring& pac, int attire) {
  for (const auto& p : GearBudget(pac))
    if (p.entry.size() >= 8 && p.entry[6] - '0' + 1 == attire) return p.pages;
  return 0;
}
int AttirePages(const std::wstring& pac, int attire) {
  for (const auto& p : ModelBudget(pac))
    if (p.entry.size() >= 8 && p.entry[6] - '0' + 1 == attire) return p.pages;
  return 0;
}
std::vector<int> AttireCount() {  // the attires the mod has, 1..4
  const std::vector<int> own = g_star.model.empty() ? std::vector<int>() : ModelAttires(g_star.model);
  std::vector<int> v;
  for (int a = 1; a <= 4; ++a)
    if (a == 1 || !g_star.attires[a - 1].empty() || std::find(own.begin(), own.end(), a) != own.end()) v.push_back(a);
  return v;
}
// What attire a wears for its entrance: 0 none, else the attire (of the model) whose gear it is. In game an attire
// with no gear of its own wears attire 1's (HCTP Entrance Porting, played: no ..14 -> ..04).
int GearShown(int a) {
  const auto op = g_star.gear.find(a);
  if (op != g_star.gear.end() && op->second == "off") return 0;
  if (op != g_star.gear.end() && op->second.rfind("from", 0) == 0) return std::atoi(op->second.c_str() + 4);
  const auto [pac, in] = AttireSource(a);
  const auto g = pac.empty() ? std::vector<int>() : GearOf(pac);
  if (std::find(g.begin(), g.end(), in) != g.end()) return a;
  if (a > 1 && pac == g_star.model && GearShown(1) == 1) return 1;  // (attire 1's, the game's fallback)
  return 0;
}

// The Full preview (full_preview.cpp) of entrance n for this project: its superstar in the attire the preview
// shows, in that attire's entrance gear (GearShown), with the project's theme, titantron and move pack.
void OpenFullPreview(int n, int start_part) {
  full_preview::Request r;
  r.entrance = n;
  r.star = kStyles[g_star.style].template_id;
  const int gear = n >= 6000 ? 0 : GearShown(g_view_attire);  // (a victory scene: in the attire, as after a match)
  const auto [pac, in] = AttireSource(gear ? gear : g_view_attire);
  r.model = !pac.empty() ? pac : StyleChPac();
  r.attire = !pac.empty() ? in : 1;
  r.gear = gear != 0;
  r.song = n >= 6000 && !g_star.victory_song.empty() ? g_star.victory_song : g_star.song, r.movie = g_star.movie, r.pack = g_star.pack;
  r.title = g_star.name[0] ? g_star.name : "your superstar";
  r.start_part = start_part;
  r.height = g_star.height;  // (the mod's height=: on the superstar's own size)
  full_preview::Open(r);
}

const std::vector<int>& GearDonors() {
  static std::vector<int> v;
  static bool done = false;
  if (done) return v;
  done = true;
  for (const auto& st : Stars()) {
    PacIndex idx;
    char id[8];
    std::snprintf(id, sizeof id, "%06d", st.id);
    if (ReadPacIndex(fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(st.id) + L".pac"), idx))
      for (const auto& e : idx.entries)
        if (e.group == "EMD " && e.name.size() == 8 && e.name.compare(0, 6, id) == 0 && e.name[7] == '4') {
          v.push_back(st.id);
          break;
        }
  }
  return v;
}

// Entrance gear another superstar mod installed in the game has (its ch.pac's kind-4 entries): one row per attire.
struct ModGear {
  std::string label;
  std::wstring pac;
  int attire = 1;
};
const std::vector<ModGear>& ModGearDonors() {
  static std::vector<ModGear> v;
  static double when = -100;
  if (ImGui::GetTime() - when < 5) return v;
  when = ImGui::GetTime();
  v.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec)) {
    const fs::path pac = e.path() / L"ch.pac";
    if (!e.is_directory(ec) || !fs::exists(pac, ec)) continue;
    Bytes man;
    std::string name = Utf8(e.path().filename().wstring());
    if (ReadFile(PathStr(e.path() / L"manifest.txt"), man)) {
      const std::string n = Value(std::string(man.begin(), man.end()), "name");
      if (!n.empty()) name = n;
    }
    for (int a : GearOf(pac.wstring())) v.push_back({name + " (mod), attire " + std::to_string(a), pac.wstring(), a});
  }
  return v;
}

int g_gear_disc_attire = 0;  // "From my game discs": the attire the converted superstar's gear goes to (0: closed)
ModGear g_gear_mod_pick;     // (GearDonorPopup: a mod's gear picked)

// picked: a 2011 superstar's id; or -1 with g_gear_mod_pick a mod's; or -2 "From my game discs".
bool GearDonorPopup(const char* id, int& picked) {
  bool hit = false;
  if (ImGui::BeginPopup(id)) {
    ImGui::TextDisabled("Whose entrance gear?");
    for (int d : GearDonors()) {
      const StarInfo* s = StarById(d);
      if (ImGui::Selectable(((s ? s->name : std::to_string(d)) + "##gd" + std::to_string(d)).c_str())) picked = d, hit = true, ImGui::CloseCurrentPopup();
    }
    const auto& mods = ModGearDonors();
    if (!mods.empty()) ImGui::Separator(), ImGui::TextDisabled("Your superstar mods");
    for (size_t k = 0; k < mods.size(); ++k)
      if (ImGui::Selectable((mods[k].label + "##gm" + std::to_string(k)).c_str())) g_gear_mod_pick = mods[k], picked = -1, hit = true, ImGui::CloseCurrentPopup();
    ImGui::Separator();
    if (ImGui::Selectable("From my game discs... (a superstar of another game)")) picked = -2, hit = true, ImGui::CloseCurrentPopup();
    ImGui::Separator();
    ImGui::PushTextWrapPos(300 * g_scale);
    ImGui::TextDisabled("Coats, jackets, hats and robes carry over. A shirt painted on the skin (Cena's) doesn't.");
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
  }
  return hit;
}

// The entrance gear of attire `donor_attire` of `donor_pac` (any wrestler pac with a kind-4 entry: the game's, a
// mod's, a superstar converted from another game) fitted onto this attire's body (svr11_build --gear-from).
void GearTransplantFrom(const std::wstring& target, int attire, const std::wstring& donor_pac, int donor_attire, const std::string& name,
                        std::function<void(const std::wstring&)> done) {
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"gear";
  std::error_code ec;
  fs::create_directories(work, ec);
  uint32_t h = 2166136261u;
  for (wchar_t c : donor_pac) h = (h ^ uint32_t(c)) * 16777619u;
  const std::wstring stem = fs::path(target).stem().wstring() + L"_a" + std::to_wstring(attire) + L"_gear" + std::to_wstring(h % 100000) +
                            L"_" + std::to_wstring(donor_attire);
  const std::wstring out = (work / (stem + L".pac")).wstring(), report = (work / (stem + L".json")).wstring();
  fs::remove(out, ec);
  Status("Fitting " + name + "'s entrance gear ...");
  Log("gear: " + name + " (" + Utf8(donor_pac) + ", attire " + std::to_string(donor_attire) + ") onto attire " + std::to_string(attire));
  const bool started = RunToolJson(L"svr11_build.py", {L"--gear-from", donor_pac + L"," + std::to_wstring(donor_attire), L"--onto", target + L"," + std::to_wstring(attire), out, report},
                                   [out, name, done](int code, const std::string& text) {
    Json j;
    const bool parsed = ParseJson(text, j);
    if (code != 0 || !fs::exists(out) || (parsed && !j["ok"].Truthy())) {
      const std::string why = parsed ? j["error"].Str() : std::string();
      Status(name + "'s entrance gear couldn't be fitted" + (why.empty() ? std::string(" (see the log).") : ": " + why));
      Log("gear: " + text);
      return;
    }
    for (const char* list : {"notes", "warnings"})
      for (const auto& w : j[list].items) Log("  gear: " + w.Str());
    Status(name + "'s entrance gear fitted: " + j["gear_meshes"].Str() + " pieces, " + j["extra_bones"].Str() + " extra bones, " + j["pages"].Str() +
           " of " + std::to_string(kModelPages) + " memory pages.");
    done(out);
  });
  if (!started) Status("Fitting gear needs the Mod Maker tools (Python): see the log.");
}
void GearTransplant(const std::wstring& target, int attire, int donor, std::function<void(const std::wstring&)> done) {
  const StarInfo* who = StarById(donor);
  GearTransplantFrom(target, attire, (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(donor) + L".pac")).wstring(), 1,
                     who ? who->name : std::to_string(donor), std::move(done));
}

// Entrance gear from an FBX: the pieces (a coat or hat skinned to a humanoid rig) posed onto this model's body
// (FbxToBuildInput with it as the template, automatic bone matching), then svr11_build --gear-json --onto.
void GearFromFbx(const std::wstring& target, int attire, const std::wstring& fbx, std::function<void(const std::wstring&)> done) {
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"gear" / (fs::path(fbx).stem().wstring() + L"_gear");
  const std::wstring out = (work.parent_path() / (fs::path(target).stem().wstring() + L"_a" + std::to_wstring(attire) + L"_fbxgear.pac")).wstring();
  Status("Fitting the gear from " + Utf8(fs::path(fbx).filename().wstring()) + " ...");
  RunInBackground([target, attire, fbx, work, out, done] {
    Progress("Posing the gear onto the model ...");
    std::vector<FbxBone> fb;
    std::string err;
    Bytes pac;
    FbxImportReport rep;
    FbxImportOptions opts;
    opts.gear_json = true;
    bool ok = ReadFbxSkeleton(Utf8(fbx), fb, err) && ReadFile(Utf8(target), pac);
    if (ok) {
      std::vector<RigBone> rb;
      for (const auto& b : fb) rb.push_back({b.name, b.parent});
      ok = FbxToBuildInput(Utf8(fbx), MapRig(rb), pac, Utf8(target), PathStr(work), rep, opts);
    }
    for (const auto& w : rep.warnings) Log("  gear FBX: " + w);
    if (!ok) {
      Status("The gear FBX couldn't be read: " + (rep.warnings.empty() ? err : rep.warnings.front()));
      return;
    }
    WhenIdle([target, attire, work, out, done] {
      const std::wstring report = out.substr(0, out.size() - 4) + L".json";
      const bool started = RunToolJson(L"svr11_build.py", {L"--gear-json", (work / L"gear.json").wstring(), L"--onto", target + L"," + std::to_wstring(attire), out, report},
                                       [out, done](int code, const std::string& text) {
        Json j;
        const bool parsed = ParseJson(text, j);
        if (code != 0 || !fs::exists(out) || (parsed && !j["ok"].Truthy())) {
          const std::string why = parsed ? j["error"].Str() : std::string();
          Status("The gear couldn't be fitted" + (why.empty() ? std::string(" (see the log).") : ": " + why));
          Log("gear: " + text);
          return;
        }
        for (const char* list : {"notes", "warnings"})
          for (const auto& w : j[list].items) Log("  gear: " + w.Str());
        Status("The gear is fitted: " + j["gear_meshes"].Str() + " pieces, " + j["pages"].Str() + " of " + std::to_string(kModelPages) + " memory pages.");
        done(out);
      });
      if (!started) Status("Fitting gear needs the Mod Maker tools (Python): see the log.");
    });
  });
}

void ApplyTestMaterials();
extern bool g_test_open_mats;
// The FBX table's material cell: a button opening the mesh's builder material (svr11_build "material").
bool FbxMaterialCell(const std::string& mesh, bool simple) {
  bool ch = false;
  MatEdit e = g_star.fbx_mat.count(mesh) ? g_star.fbx_mat[mesh] : MatEdit();
  if (ImGui::SmallButton(e.Empty() ? "as built##mat" : "changed##mat")) ImGui::OpenPopup("fbxmat");
  if (!e.Empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("Applied when the model is made (Make the model from the FBX).");
  ImGui::SetNextWindowSize(ImVec2(420 * g_scale, 0), ImGuiCond_Always);
  if (ImGui::BeginPopup("fbxmat")) {
    ImGui::TextUnformatted(mesh.c_str());
    ImGui::TextDisabled("Used when the model is made from the FBX.");
    bool hs = e.has_spec;
    if (ImGui::Checkbox(simple ? "My own shine" : "Shine (specular level)", &hs)) e.has_spec = hs, ch = true;
    if (e.has_spec) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160 * g_scale);
      if (ImGui::SliderFloat("##spec", &e.spec, 0, 4, "%.2f", ImGuiSliderFlags_Logarithmic)) ch = true;
    }
    static const char* kAlpha[] = {"as the builder does", "the picture's alpha", "even (flat)", "the picture's brightness"};
    ImGui::SetNextItemWidth(200 * g_scale);
    if (ImGui::Combo(simple ? "Where it shines" : "Shine pattern", &e.spec_alpha, kAlpha, 4)) ch = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Which places shine more: the picture's alpha, the same everywhere, or its bright places. The builder writes it where the\n"
                        "game reads it: skin parts into the normal map's alpha, other parts into the colour picture's (see-through parts: the alpha is the see-through).");
    ImGui::TextUnformatted(simple ? "Bumps:" : "Normal map:");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", e.normal == 1 ? Utf8(fs::path(e.normal_file).filename().wstring()).c_str() : "the FBX's");
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick...")) {
      static const COMDLG_FILTERSPEC kPng[1] = {{L"PNG pictures", L"*.png"}};
      const std::wstring f = PickFile(false, L"A normal map (PNG)", kPng, 1);
      if (!f.empty()) e.normal = 1, e.normal_file = f, ch = true;
    }
    if (e.normal) ImGui::SameLine(), (ImGui::SmallButton("x##n") ? (e.normal = 0, e.normal_file.clear(), ch = true) : false);
    bool ht = e.has_tint;
    if (ImGui::Checkbox(simple ? "Colour it" : "Colour (multiplier)", &ht)) e.has_tint = ht, ch = true;
    if (e.has_tint) ImGui::SameLine(), ch |= ImGui::ColorEdit3("##tint", e.tint, ImGuiColorEditFlags_NoInputs);
    static const char* kTri[] = {"as the builder does", "no", "yes"};
    int cut = e.cutout + 1, both = e.two_sided + 1;
    ImGui::SetNextItemWidth(160 * g_scale);
    if (ImGui::Combo(simple ? "See-through where clear" : "See-through cut-out", &cut, kTri, 3)) e.cutout = cut - 1, ch = true;
    ImGui::SetNextItemWidth(160 * g_scale);
    if (ImGui::Combo(simple ? "Both sides" : "Double-sided", &both, kTri, 3)) e.two_sided = both - 1, ch = true;
    bool flip = e.flip == 1;
    if (ImGui::Checkbox("Flip faces", &flip)) e.flip = flip ? 1 : -1, ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns its faces the other way round: for a part that still shows inside out in the game.");
    ImGui::TextUnformatted("Shine picture:");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", e.shine_mask.empty() ? "none" : Utf8(fs::path(e.shine_mask).filename().wstring()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick...##shine")) {
      const std::wstring f = PickFile(false, L"Where it shines (a grey picture)", kPictureFilter, 1);
      if (!f.empty()) e.shine_mask = f, ch = true;
    }
    if (!e.shine_mask.empty()) ImGui::SameLine(), (ImGui::SmallButton("x##shine") ? (e.shine_mask.clear(), ch = true) : false);
    if (!e.Empty() && ImGui::Button("As built")) e = MatEdit(), ch = true;
    ImGui::EndPopup();
  }
  if (ch) {
    if (e.Empty()) g_star.fbx_mat.erase(mesh);
    else g_star.fbx_mat[mesh] = e;
  }
  return ch;
}

void MaterialsSection(bool simple, bool header, bool show_list) {
  if (g_star.model.empty()) {
    if (!header) ImGui::TextDisabled("Pick a look first (the Model tab).");
    return;
  }
  ApplyTestMaterials();
  const bool gear = char_preview::Gear() && char_preview::HasGear();
  const auto [pac, in] = AttireSource(g_view_attire);
  int n = 0;
  for (const auto& [k, e] : g_star.mats) n += !e.Empty();
  const std::string head = std::string(simple ? "Shine, bumps and colour" : "Materials") + (n ? " (" + std::to_string(n) + " changed)" : "") +
                           "###materials";
  if (g_test_open_mats) ImGui::SetNextItemOpen(true, ImGuiCond_Always), g_test_open_mats = false;
  if (header ? !ImGui::CollapsingHeader(head.c_str()) : !show_list) {
    std::map<std::pair<uint32_t, int>, MatEdit> preview;  // (still shown while folded)
    for (const auto& [k, e] : g_star.mats)
      if (std::get<0>(k) == g_view_attire && std::get<1>(k) == gear) preview[{std::get<2>(k), std::get<3>(k)}] = e;
    char_preview::SetMaterialPreview(preview);
    return;
  }
  if (MaterialPanel(pac, in, gear, g_view_attire, g_star.mats, simple)) Touch();
}

void GearFromDisc();
void GearRows(bool simple) {
  if (g_star.model.empty()) return;
  const std::vector<int> own_gear = GearOf(g_star.model);
  bool any = !own_gear.empty();
  for (int a = 2; a <= 4; ++a)
    if (!g_star.attires[a - 1].empty() && !GearOf(g_star.attires[a - 1]).empty()) any = true;
  if (!any && g_star.gear.empty())
    ImGui::TextDisabled("Entrance gear: none yet (they walk out in their attire) - a coat, hat or robe can come from another superstar:");
  ImGui::TextUnformatted("Entrance gear");
  ImGui::SameLine();
  ImGui::TextDisabled(simple ? "(a coat, hat or shirt worn only for the entrance)" : "(EMD kind 4: the outfit worn for the entrance, per attire)");
  for (int a : AttireCount()) {
    ImGui::PushID(1000 + a);
    const auto [pac, in] = AttireSource(a);
    const auto pg = GearOf(pac);
    const int shown = GearShown(a);
    const bool chosen = g_star.gear.count(a) > 0;
    const bool has = shown != 0 || (chosen && g_star.gear[a] == "off");
    ImGui::Text("Attire %d:", a);
    ImGui::SameLine(90 * g_scale);
    if (!shown) ImGui::TextDisabled("%s", chosen ? "gear left out" : "no gear");
    else if (shown == a) ImGui::TextUnformatted("its own gear");
    else if (!chosen) ImGui::Text("attire %d's (the game's default)", shown);
    else ImGui::Text("attire %d's gear", shown);
    ImGui::SameLine();
    ImGui::BeginDisabled(!shown);
    if (ImGui::SmallButton("Show it")) {
      g_view_attire = a;
      char_preview::SetGear(true);
    }
    ImGui::EndDisabled();
    if (has) {
      ImGui::SameLine();
      bool off = g_star.gear.count(a) && g_star.gear[a] == "off";
      if (ImGui::Checkbox("Leave it out", &off)) {
        if (off) g_star.gear[a] = "off";
        else g_star.gear.erase(a);
        Touch();
      }
    }
    {
      ImGui::SameLine();
      ImGui::BeginDisabled(Busy());
      if (ImGui::SmallButton("From another superstar...")) ImGui::OpenPopup("gear_donor");
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(Busy());
      if (ImGui::SmallButton("From a 3D model...")) {
        const std::wstring f = PickFile(false, L"The gear as an FBX (a coat or hat skinned to a humanoid rig)", kFbxFilter, 1);
        if (!f.empty()) {
          const int at = a;
          const bool own = pac == g_star.model;
          GearFromFbx(pac, in, f, [at, own](const std::wstring& made) {
            if (own) g_star.model = made;
            else g_star.attires[at - 1] = made;
            g_star.gear.erase(at);
            g_view_attire = at;
            char_preview::SetGear(true);
            Touch();
          });
        }
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A coat, jacket, hat or robe made in Blender (or any 3D program), skinned to a humanoid rig\n"
                          "(the Mod Maker matches its bones by name, as for a whole model): fitted onto this attire's body.");
      int donor = 0;
      if (GearDonorPopup("gear_donor", donor)) {
        const int at = a;
        const bool own = pac == g_star.model;
        auto put = [at, own](const std::wstring& made) {
          if (own) g_star.model = made;
          else g_star.attires[at - 1] = made;
          g_star.gear.erase(at);
          g_view_attire = at;
          char_preview::SetGear(true);
          Touch();
        };
        if (donor > 0) GearTransplant(pac, in, donor, put);
        else if (donor == -1) GearTransplantFrom(pac, in, g_gear_mod_pick.pac, g_gear_mod_pick.attire, g_gear_mod_pick.label, put);
        else if (donor == -2) g_gear_disc_attire = a;
      }
    }
    if (pac == g_star.model) {  // (the same model: another of its attires' gear)
      for (int k : own_gear) {
        if (k == a || shown == k) continue;
        ImGui::SameLine();
        if (ImGui::SmallButton(("Use attire " + std::to_string(k) + "'s gear").c_str())) g_star.gear[a] = "from" + std::to_string(k), Touch();
      }
      if (g_star.gear.count(a) && g_star.gear[a].rfind("from", 0) == 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Undo")) g_star.gear.erase(a), Touch();
      }
    }
    ImGui::PopID();
  }
  GearFromDisc();
}

// "From my game discs": a superstar of another game converted (its look: the Simple import list), then its entrance
// gear (the converters keep kind 4: WWE '12-2K15, SvR 2008-2010, PS2) fitted onto the attire picked.
void GearFromDisc() {
  if (!g_gear_disc_attire) return;
  ImGui::Separator();
  ImGui::Text("Entrance gear for attire %d from another game's superstar", g_gear_disc_attire);
  ImGui::SameLine();
  if (ImGui::SmallButton("Close##geardisc")) {
    g_gear_disc_attire = 0;
    return;
  }
  std::wstring mod;
  bool all = false;
  if (!simple_port::SuperstarImport(mod, all, false)) return;
  const int at = g_gear_disc_attire;
  g_gear_disc_attire = 0;
  Bytes z;
  std::vector<ZipEntry> files;
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"gear" / (fs::path(mod).stem().wstring() + L"_donor.pac");
  if (!ReadFile(PathStr(mod), z) || !ZipRead(z, files)) return Status("The converted superstar couldn't be read (see the log).");
  for (const auto& f : files)
    if (f.name == "ch.pac") WriteFile(PathStr(work), f.data);
  const std::vector<int> g = GearOf(work.wstring());
  const std::string name = Utf8(fs::path(mod).stem().wstring());
  if (g.empty()) return Status(name + " has no entrance gear in that game (no coat or hat model): pick another superstar.");
  const auto [pac, in] = AttireSource(at);
  const bool own = pac == g_star.model;
  GearTransplantFrom(pac, in, work.wstring(), g[0], name, [at, own](const std::wstring& made) {
    if (own) g_star.model = made;
    else g_star.attires[at - 1] = made;
    g_star.gear.erase(at);
    g_view_attire = at;
    char_preview::SetGear(true);
    Touch();
  });
}

// The superstar's name for messages ("Your superstar" until it has one).
std::string Who() { return g_star.name[0] ? std::string(g_star.name) : std::string("Your superstar"); }

void GoStep(int step) {  // (a fix that needs a choice: its step in Simple mode)
  if (g_star.simple) g_star.simple_step = step;
}

// The game's name of a disc key ("WWE '13").
std::string GameName(const std::string& key) {
  const disc_list::Game* g = disc_list::GameByKey(key);
  return g ? g->name : key;
}

// A converted pack (<work>\pack) into this mod's moves: it becomes the project's pack, or its files and pack.txt
// lines join the one it has.
void JoinPack(const fs::path& pack) {
  std::error_code ec;
  if (g_star.pack.empty()) {
    g_star.pack = pack.wstring();
  } else {  // (the mod's own moves: the entrance's files and pack.txt lines join them)
    for (const auto& e : fs::recursive_directory_iterator(pack, ec)) {
      if (!e.is_regular_file(ec) || e.path().filename() == L"pack.txt") continue;
      const fs::path to = fs::path(g_star.pack) / fs::relative(e.path(), pack, ec);
      fs::create_directories(to.parent_path(), ec);
      fs::copy_file(e.path(), to, fs::copy_options::overwrite_existing, ec);
    }
    Bytes a, b;
    ReadFile(PathStr(fs::path(g_star.pack) / L"pack.txt"), a);
    ReadFile(PathStr(pack / L"pack.txt"), b);
    std::string have(a.begin(), a.end());
    if (!have.empty() && have.back() != '\n') have += "\n";
    for (size_t at = 0; at < b.size();) {
      size_t e = std::string(b.begin(), b.end()).find('\n', at);
      if (e == std::string::npos) e = b.size();
      const std::string line(b.begin() + long(at), b.begin() + long(e));
      if (!line.empty() && have.find(line) == std::string::npos) have += line + "\n";
      at = e + 1;
    }
    WriteFile(PathStr(fs::path(g_star.pack) / L"pack.txt"), Bytes(have.begin(), have.end()));
  }
}

// A ported superstar's own entrance, converted again from the disc in My game discs and carried in this mod (as
// superstar_port.py does: entrance_port free-numbers, then convert; the pack joins the mod's moves).
void PortEntrance() {
  const std::wstring disc = disc_list::PathFor(g_star.source_game);
  if (disc.empty()) {
    GoTo(PageId::kDiscs);
    Status("Add your " + GameName(g_star.source_game) + " disc here first, then press the fix again.");
    return;
  }
  const std::wstring pac11 = (fs::path(g_game) / L"pac").wstring(), mods = (fs::path(g_game) / L"Mods").wstring();
  const std::wstring work = (fs::path(g_game) / L"Mods" / L".convert" / L"entrance_fix" / Wide(g_star.source_game + "_" + std::to_string(g_star.source_entrance))).wstring();
  RunToolJson(L"entrance_port.py", {L"free-numbers", L"--game-pac", pac11, L"--mods", mods}, [disc, pac11, work](int code, const std::string& out) {
    Json j;
    std::vector<int> free;
    if (code == 0 && ParseJson(out, j)) {
      const Json& list = j.type == Json::Type::kArray ? j : j["free"];
      for (const auto& x : list.items) free.push_back(x.Int());
    }
    if (free.empty()) {
      Status("No free entrance number left in the game: remove an installed entrance pack first.");
      return;
    }
    const int number = std::find(free.begin(), free.end(), g_star.entrance) != free.end() ? g_star.entrance : free[0];
    std::vector<std::wstring> args = {L"convert", L"--game", Wide(g_star.source_game), L"--src", disc, L"--entrance",
                                      std::to_wstring(g_star.source_entrance), L"--number", std::to_wstring(number), L"--out", work,
                                      L"--name", Wide(Who()), L"--game-pac", pac11};
    if (g_star.source_game.rfind("wwe", 0) == 0) args.push_back(L"--svrmod"), args.push_back(SvrmodExe());
    WhenIdle([args, work, number] {
      RunToolJson(L"entrance_port.py", args, [work, number](int code, const std::string& out) {
        const fs::path pack = fs::path(work) / L"pack";
        std::error_code ec;
        if (code != 0 || !fs::exists(pack / L"pack.txt", ec)) {
          Json e;
          Status("The entrance didn't convert" + (ParseJson(out, e) && !e["error"].Str().empty() ? ": " + e["error"].Str() : std::string(" (see the log).")));
          return;
        }
        JoinPack(pack);
        g_star.entrance = number;
        g_star.entrance_note.clear();
        Touch();
        Status(Who() + "'s own entrance is in the mod now (from " + GameName(g_star.source_game) + ").");
      });
    });
  });
}

// ---- victory (victory=, victory_song=; docs/VICTORY_SCENES.md)
std::wstring g_victory_lufs_for;
double g_victory_lufs = 0;
std::string g_victory_lufs_note;
bool g_victory_lufs_busy = false;

void MeasureVictoryTrack() {
  if (g_star.victory_song.empty() || g_victory_lufs_busy) return;
  g_victory_lufs_busy = true;
  g_victory_lufs_note = "measuring ...";
  const std::wstring f = g_star.victory_song;
  std::thread([f] {
    double l = 0;
    std::string err;
    const bool ok = MeasureLufs(f, &l, &err);
    OnUiThread([f, ok, l, err] {
      g_victory_lufs_busy = false;
      if (f != g_star.victory_song) return;  // (another song by now)
      g_victory_lufs_for = ok ? f : std::wstring();
      g_victory_lufs = l;
      char b[160];
      if (ok) std::snprintf(b, sizeof b, "%.1f LUFS (the game's themes sit near %.0f)", l, kThemeTargetLufs);
      g_victory_lufs_note = ok ? b : "could not measure: " + err;
    });
  }).detach();
}


// Other games' singles victory scenes (entrance_port.py list --kind victory) on the discs in My game discs: SvR
// 2008-2010 and Legends of WrestleMania keep them as 2011 does (pac\evt\Winning*.pac). Read once, a disc at a time.
struct OtherVictory {
  std::string game, game_name;  // (entrance_port's id, the disc's name)
  std::wstring disc;
  int number = 0;
  std::string name;
};
std::vector<OtherVictory> g_other_victories;
int g_other_victories_state = 0;  // 0 not read, 1 reading, 2 read
bool g_victory_converting = false;
constexpr std::pair<const char*, const char*> kVictoryGames[] = {
    {"svr2008", "svr08"}, {"svr2009", "svr09"}, {"svr2010", "svr10"}, {"low", "low"}};

void ReadOtherVictories(size_t k = 0) {
  g_other_victories_state = 1;
  for (; k < std::size(kVictoryGames) && disc_list::PathFor(kVictoryGames[k].first).empty(); ++k) {}
  if (k >= std::size(kVictoryGames)) {
    g_other_victories_state = 2;
    return;
  }
  const std::wstring disc = disc_list::PathFor(kVictoryGames[k].first);
  const std::string game = kVictoryGames[k].second, name = GameName(kVictoryGames[k].first);
  WhenIdle([k, disc, game, name] {
    const bool started = RunToolJson(L"entrance_port.py", {L"list", L"--kind", L"victory", L"--game", Wide(game), L"--src", disc,
                                                          L"--game-pac", (fs::path(g_game) / L"pac").wstring()},
                                     [k, disc, game, name](int code, const std::string& out) {
      Json j;
      if (code == 0 && ParseJson(out, j))
        for (const auto& r : j.items)
          g_other_victories.push_back({game, name, disc, int(r["number"].Int()), r["name"].Str()});
      ReadOtherVictories(k + 1);
    });
    if (!started) ReadOtherVictories(k + 1);
  });
}

// One of them into this mod: converted to a free 6300+ number, its pack joins the mod's moves, victory= set.
void PortVictory(const OtherVictory& v) {
  const std::wstring pac11 = (fs::path(g_game) / L"pac").wstring(), mods = (fs::path(g_game) / L"Mods").wstring();
  const std::wstring work = (fs::path(g_game) / L"Mods" / L".convert" / L"victory" / Wide(v.game + "_" + std::to_string(v.number))).wstring();
  std::set<int> own;  // (numbers this mod's pack has already)
  if (!g_star.pack.empty())
    for (const auto& e : PackEntrances(g_star.pack)) own.insert(e.number);
  g_victory_converting = true;
  Status("Converting " + v.name + " from " + v.game_name + " ...");
  RunToolJson(L"entrance_port.py", {L"free-numbers", L"--kind", L"victory", L"--game-pac", pac11, L"--mods", mods},
              [v, pac11, work, own](int code, const std::string& out) {
    Json j;
    int number = 0;
    if (code == 0 && ParseJson(out, j))
      for (const auto& x : j.items)
        if (!own.count(int(x.Int()))) { number = int(x.Int()); break; }
    if (!number) {
      g_victory_converting = false;
      Status("No free victory number left (6300-6999): remove an installed victory pack first.");
      return;
    }
    const std::vector<std::wstring> args = {L"convert", L"--kind", L"victory", L"--game", Wide(v.game), L"--src", v.disc,
                                            L"--entrance", std::to_wstring(v.number), L"--number", std::to_wstring(number),
                                            L"--out", work, L"--name", Wide(v.name.substr(0, 29)), L"--game-pac", pac11};
    WhenIdle([args, work, number, v] {
      RunToolJson(L"entrance_port.py", args, [work, number, v](int code, const std::string& out) {
        g_victory_converting = false;
        const fs::path pack = fs::path(work) / L"pack";
        std::error_code ec;
        if (code != 0 || !fs::exists(pack / L"pack.txt", ec)) {
          Json e;
          Status("The victory didn't convert" + (ParseJson(out, e) && !e["error"].Str().empty() ? ": " + e["error"].Str() : std::string(" (see the log).")));
          return;
        }
        JoinPack(pack);
        g_star.victory = number;
        Touch();
        Status(v.name + " (" + v.game_name + ") is " + Who() + "'s victory now (scene " + std::to_string(number) + ").");
      });
    });
  });
}

// The Victory row: the game's scenes, installed packs' and this mod's own (Victories(), the mod's pack), and other
// games' (converted when picked); "Preview" plays the chosen one in the Full preview.
bool g_test_victory_open = false;
std::string g_test_victory_port;  // (--star-victory-port <game>,<number>: that scene converted once the list is read)
void VictoryRow() {
  std::vector<EntranceInfo> mine;
  if (!g_star.pack.empty())
    for (auto& e : PackEntrances(g_star.pack))
      if (IsVictoryNumber(e.number)) e.from = "this mod", mine.push_back(std::move(e));
  auto label = [](const EntranceInfo& v) { return std::to_string(v.number) + "  " + v.name + (v.from.empty() ? "" : "  [" + v.from + "]"); };
  std::string cur = "The game's (a generic one)";
  for (const std::vector<EntranceInfo>* list : {&Victories(), static_cast<const std::vector<EntranceInfo>*>(&mine)})
    for (const auto& v : *list)
      if (v.number == g_star.victory) cur = label(v);
  if (g_star.victory > 0 && cur[0] == 'T') cur = std::to_string(g_star.victory) + "  (not installed)";
  if (!g_test_victory_port.empty() && g_other_victories_state == 0) ReadOtherVictories();
  if (!g_test_victory_port.empty() && g_other_victories_state == 2 && !g_victory_converting) {
    for (const auto& v : g_other_victories)
      if (g_test_victory_port == v.game + "," + std::to_string(v.number)) PortVictory(v);
    g_test_victory_port.clear();
  }
  ImGui::SetNextItemWidth(320 * g_scale);
  ImGui::BeginDisabled(g_victory_converting);
  if (ImGui::BeginCombo("Victory", g_victory_converting ? "Converting ..." : cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (g_other_victories_state == 0) ReadOtherVictories();
    if (ImGui::Selectable("The game's (a generic one)", g_star.victory <= 0)) g_star.victory = -1, Touch();
    for (const std::vector<EntranceInfo>* list : {static_cast<const std::vector<EntranceInfo>*>(&mine), &Victories()})
      for (const auto& v : *list)
        if (ImGui::Selectable((label(v) + "##v" + v.from).c_str(), g_star.victory == v.number)) g_star.victory = v.number, Touch();
    ImGui::SeparatorText("From another game (converted into this mod)");
    if (g_other_victories_state != 2) ImGui::TextDisabled("Reading your game discs ...");
    else if (g_other_victories.empty())
      ImGui::TextDisabled("None: add SvR 2008, 2009, 2010 or Legends of WrestleMania in My game discs.");
    for (const auto& v : g_other_victories)
      if (ImGui::Selectable((v.name + "  [" + v.game_name + " " + std::to_string(v.number) + "]##o" + v.game).c_str(), false))
        PortVictory(v);
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(g_star.victory <= 0 || g_victory_converting);
  if (ImGui::Button("Preview##victory")) OpenFullPreview(g_star.victory);
  ImGui::EndDisabled();
  Hint("What the superstar does after winning a singles match: another superstar's celebration (The Rock, Cena, "
       "Kofi ...), one of the game's generic ones, one a move pack adds (EVP scripts numbered 6000-7999), or one "
       "from SvR 2008-2010 / Legends of WrestleMania (My game discs), converted into this mod when picked. Tag, "
       "title and match-type celebrations stay the game's.");
  if (FileRow("Victory track (optional)...", g_star.victory_song, "their theme", kSoundFilter, 1,
              "Plays when they win, in place of their theme, in any match where the game would play it. Only "
              "this PC hears it: online, each player hears their own mods' track."))
    g_victory_lufs_for.clear(), g_victory_lufs_note.clear(), Touch();
  if (g_star.victory_song.empty()) return;
  ImGui::Indent();
  ImGui::SetNextItemWidth(236 * g_scale);
  ImGui::SliderFloat("Track volume", &g_star.victory_gain, -20.0f, 6.0f, "%+.1f dB");
  g_star.victory_gain = std::clamp(std::round(g_star.victory_gain * 10) / 10, -20.0f, 6.0f);
  ImGui::SameLine();
  ImGui::BeginDisabled(g_victory_lufs_busy);
  if (ImGui::SmallButton("Measure##victory")) MeasureVictoryTrack();
  ImGui::EndDisabled();
  if (g_victory_lufs_for == g_star.victory_song) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Match the game##victory")) {
      g_star.victory_gain = std::round(float(std::clamp(kThemeTargetLufs - g_victory_lufs, -20.0, 6.0)) * 10) / 10;
      Touch();
    }
  }
  if (!g_victory_lufs_note.empty()) ImGui::TextDisabled("%s", g_victory_lufs_note.c_str());
  ImGui::Unindent();
}

void GearProblems(std::vector<Problem>& p) {
  if (g_star.model.empty()) return;
  for (int a : AttireCount()) {
    const int shown = GearShown(a);
    if (!shown) continue;
    const auto [pac, in] = AttireSource(a);
    // (the gear model loads instead of the attire's for the entrance: stock Undertaker's two together are ~370 pages)
    const int gear = GearPages(shown == a ? pac : g_star.model, shown == a ? in : shown);
    if (gear > kModelPages)
      p.push_back(Warning("Attire " + std::to_string(a) + "'s entrance outfit (the coat or hat worn on the way to the ring) is too big "
                          "for the game's memory: the entrance may not load.")
                      .With("Leave the entrance outfit out", [a] { g_star.gear[a] = "off"; Touch(); }, "the entrance outfit is left out"));
  }
}

void Problems(std::vector<Problem>& p) {
  size_t mark = p.size();
  auto tag = [&](int step) {  // (Simple mode's "Fix it": the step for what the last section added)
    for (size_t k = mark; k < p.size(); ++k)
      if (p[k].step < 0) p[k].step = step;
    mark = p.size();
  };
  const std::string who = Who();
  FbxProblems(p);
  tag(kStepLook);
  GearProblems(p);
  tag(kStepAttires);
  if (!g_star.name[0]) p.push_back(Error("Your superstar has no name yet.").With("Type a name", [] { GoStep(kStepName); }, "the name step is open"));
  tag(kStepName);
  const std::wstring style_pac = StyleChPac();
  auto use_style_model = [style_pac] { g_star.model = style_pac; Touch(); };
  if (g_star.model.empty()) {
    p.push_back(Error(who + " has no body yet: pick a model.")
                    .With("Use the fighting style's body", use_style_model, "the fighting style's body is used")
                    .With("Pick a model...", [] { GoStep(kStepLook); }, "the Look step is open"));
  } else {
    const FileCheck& c = Check(g_star.model, "pac");
    if (!c.exists)
      p.push_back(Error(who + "'s model file was moved or deleted (" + FileName(g_star.model) + "), so there is no body.")
                      .With("Pick it again...", [] { GoStep(kStepLook); }, "the Look step is open")
                      .With("Use the fighting style's body", use_style_model, "the fighting style's body is used"));
    else if (!c.ok)
      p.push_back(Error("The file picked as " + who + "'s model isn't a wrestler model (" + c.note + ").")
                      .With("Pick another...", [] { GoStep(kStepLook); }, "the Look step is open")
                      .With("Use the fighting style's body", use_style_model, "the fighting style's body is used"));
  }
  const std::vector<int> own_attires = g_star.model.empty() ? std::vector<int>() : ModelAttires(g_star.model);
  auto in_model_attire = [&](int n) {
    for (int k = 1; k <= n; ++k)
      if (std::find(own_attires.begin(), own_attires.end(), k) == own_attires.end()) return false;
    return true;
  };
  tag(kStepLook);
  if (own_attires.size() > 4)
    p.push_back(Warning("The model has " + std::to_string(own_attires.size()) + " outfits: the game only offers the first 4.").Note());
  for (int a = 1; a < 4; ++a) {
    if (g_star.attires[a].empty()) continue;
    auto drop = [a] { g_star.attires[a].clear(); Touch(); };
    const FileCheck& c = Check(g_star.attires[a], "pac");
    if (!c.exists)
      p.push_back(Error("Outfit " + std::to_string(a + 1) + "'s file was moved or deleted (" + FileName(g_star.attires[a]) + ").")
                      .With("Remove outfit " + std::to_string(a + 1), drop).With("Pick it again...", [] { GoStep(kStepAttires); }));
    else if (!c.ok)
      p.push_back(Error("The file picked as outfit " + std::to_string(a + 1) + " isn't a wrestler model (" + c.note + ").")
                      .With("Remove outfit " + std::to_string(a + 1), drop).With("Pick another...", [] { GoStep(kStepAttires); }));
    if (a > 1 && g_star.attires[a - 1].empty() && !in_model_attire(a))
      p.push_back(Warning("Outfit " + std::to_string(a + 1) + " is set but outfit " + std::to_string(a) + " is empty: the game shows outfits "
                          "in order, so this one never appears.")
                      .With("Move it to outfit " + std::to_string(a), [a] {
                        std::swap(g_star.attires[a - 1], g_star.attires[a]);
                        std::swap(g_star.attire_names[a - 1], g_star.attire_names[a]);
                        std::swap(g_star.attire_entrance[a - 1], g_star.attire_entrance[a]);
                        Touch();
                      }, "moved up one outfit"));
  }
  tag(kStepAttires);
  if (!g_star.movie.empty()) {
    auto drop = [] { g_star.movie.clear(); Touch(); };
    const FileCheck& c = Check(g_star.movie, "bik");
    if (!c.exists)
      p.push_back(Error("The big screen video was moved or deleted (" + FileName(g_star.movie) + ").")
                      .With("Play no video of my own", drop, "the video is left out (the style's plays)").With("Pick it again...", [] { GoStep(kStepEntrance); }));
    else if (!c.ok)
      p.push_back(Error("The big screen video isn't a video the game can play (" + c.note + ").")
                      .With("Pick another video...", [] { GoStep(kStepEntrance); }).With("Play no video of my own", drop, "the video is left out"));
    else if (c.w && (c.w != 320 || c.h != 320))
      p.push_back(Warning("The big screen video is " + std::to_string(c.w) + " x " + std::to_string(c.h) +
                          ": the big screen is square, so it plays stretched.").Note());
  }
  tag(kStepEntrance);
  if (!g_star.song.empty() && !Check(g_star.song, "any").exists)
    p.push_back(Error("The theme song was moved or deleted (" + FileName(g_star.song) + ").")
                    .With("Play no song of my own", [] { g_star.song.clear(); Touch(); }, "the song is left out (the style's plays)")
                    .With("Pick it again...", [] { GoStep(kStepMusic); }));
  if (!g_star.victory_song.empty() && !Check(g_star.victory_song, "any").exists)
    p.push_back(Error("The victory track was moved or deleted (" + FileName(g_star.victory_song) + ").")
                    .With("Play their theme for the win", [] { g_star.victory_song.clear(); Touch(); }, "the track is left out"));
  if (!g_star.victory_song.empty() && g_star.victory_gain == 0.0f)
    p.push_back(Warning("The victory track will play much louder than the game's own music.", "Victory section: Measure, then Match the game."));
  if (!g_star.voice.empty() && !Check(g_star.voice, "any").exists)
    p.push_back(Error("The name recording was moved or deleted (" + FileName(g_star.voice) + ").")
                    .With("Leave the recording out", [] { g_star.voice.clear(); Touch(); }));
  if (!g_star.song.empty() && g_star.theme_gain == 0.0f)
    p.push_back(Warning("The theme song will play much louder than the game's own music.")
                    .With("Match the game's volume", [] {
                      if (g_lufs_for == g_star.song) {
                        g_star.theme_gain = std::round(float(std::clamp(kThemeTargetLufs - g_lufs, -20.0, 6.0)) * 10) / 10;
                        Touch();
                      } else {
                        MeasureTheme();  // (then Match: the measurement comes back in a moment)
                        GoStep(kStepMusic);
                      }
                    }, "the song's volume is set to the game's"));
  tag(kStepMusic);
  if (g_star.picture.Empty())
    p.push_back(Warning("No picture for the select screen: players see a dark outline instead of " + who + ".")
                    .With("Use the 3D view as the picture", [] {
                      std::vector<uint8_t> rgba;
                      Image img;
                      img.w = img.h = 512;
                      if (char_preview::Capture(512, rgba)) img.rgba = std::move(rgba), SetPicture(img), Touch();
                    }, "the 3D view is the select picture")
                    .With("Pick a picture...", [] { GoStep(kStepPictures); }));
  tag(kStepPictures);
  for (const auto& [slot, id] : g_star.moveset) {
    const int s = slot;
    auto style_move = [s] { g_star.moveset.erase(s); Touch(); };
    if (!MoveInfos().count(id)) {
      if (g_star.pack.empty())
        p.push_back(Warning(SlotLabel(slot) + ": that move isn't in the game, so " + who + " does the fighting style's move there.")
                        .With("Use the style's move", style_move).With("Pick another...", [] { GoStep(kStepMoves); }));
    } else if (!MoveFitsSlot(slot, id)) {
      p.push_back(Warning(SlotLabel(slot) + ": " + MoveName(id) + " is a different kind of move - the game may skip it there.")
                      .With("Use the style's move", style_move).With("Pick another...", [] { GoStep(kStepMoves); }));
    }
  }
  tag(kStepMoves);
  if (EntrancesReady()) {  // entrances that aren't installed: no entrance plays
    std::set<int> have;
    for (const auto& e : Entrances()) have.insert(e.number);
    if (!g_star.pack.empty())
      for (const auto& e : PackEntrances(g_star.pack)) have.insert(e.number);
    const bool can_port = !g_star.source_game.empty() && g_star.source_entrance >= 0;
    if (g_star.entrance > 0 && !have.count(g_star.entrance)) {
      Problem pr = Warning(who + " has no entrance yet - in the game " + who + " would walk out with none." +
                           (g_star.entrance_note.empty() ? "" : " (Their own didn't come across" +
                            [] { std::string n = g_star.entrance_note;  // (superstar_port's "not converted: <why>" / "not ported (...)")
                                 for (const char* pre : {"not converted: ", "not converted", "not ported "})
                                   if (n.rfind(pre, 0) == 0) n = n.substr(std::strlen(pre));
                                 return n.empty() ? std::string(".)") : ": " + n + ")"; }()));
      if (can_port)
        pr = std::move(pr).With("Port " + who + "'s entrance from " + GameName(g_star.source_game), PortEntrance,
                                "converting " + who + "'s entrance (it takes a minute)");
      pr = std::move(pr).With("Use a 2011 entrance instead", [] { GoStep(kStepEntrance); g_open_entrance_picker = true; }, "the entrance list is open");
      pr = std::move(pr).With("Leave it (the style's entrance)", [] { g_star.entrance = -1; Touch(); }, "the fighting style's entrance is used");
      p.push_back(std::move(pr));
    }
    for (int a = 1; a < 4; ++a)
      if ((!g_star.attires[a].empty() || g_star.attire_entrance[a] > 0) && g_star.attire_entrance[a] > 0 && !have.count(g_star.attire_entrance[a]))
        p.push_back(Warning("In outfit " + std::to_string(a + 1) + ", " + who + " would walk out with no entrance.")
                        .With("Use " + who + "'s usual entrance", [a] { g_star.attire_entrance[a] = -1; Touch(); }));
  }
  tag(kStepEntrance);
  for (int a = 0; a < 4; ++a) {  // the match memory (D12): every attire of the model, the first of attire pacs
    const std::wstring& f = a ? g_star.attires[a] : g_star.model;
    if (!f.empty())
      BudgetProblems(f, a ? "Outfit " + std::to_string(a + 1) : who, a != 0, p, [a](const std::wstring& lighter) {
        (a ? g_star.attires[a] : g_star.model) = lighter;
        Touch();
      });
    if (!f.empty()) ModelCheckProblems(f, a ? "Outfit " + std::to_string(a + 1) : who, p);
  }
  for (int a : AttireCount()) {  // gear inherited from the superstar a model came from: said once, plainly
    if (GearShown(a) && !g_star.gear.count(a)) {
      p.push_back(Warning("Outfit " + std::to_string(a) + " comes with an entrance coat or hat from the wrestler the model was made from.")
                      .With("Keep it", [a] { g_star.gear[a] = ""; Touch(); }, "the entrance outfit is kept")
                      .With("Leave it out", [a] { g_star.gear[a] = "off"; Touch(); }, "the entrance outfit is left out")
                      .At(kStepAttires));
    }
  }
  tag(kStepLook);
  if (!g_star.moves.empty() && !Check(g_star.moves, "any").exists)
    p.push_back(Error("The moves list file was moved or deleted (" + FileName(g_star.moves) + ").")
                    .With("Use the fighting style's moves", [] { g_star.moves.clear(); Touch(); }));
  if (!g_star.pack.empty()) {
    std::error_code ec;
    if (!fs::exists(fs::path(g_star.pack) / L"pack.txt", ec))
      p.push_back(Error("The folder of extra moves (" + FileName(g_star.pack) + ") is broken: it has no list of its moves, so none of them would play.")
                      .With("Leave the extra moves out", [] { g_star.pack.clear(); Touch(); }));
  }
  {  // (picked, imported or from a test: moves the game can't play - in a match those slots get the base's)
    std::vector<int> bad;
    for (const auto& [slot, id] : g_star.moveset)
      if (!MoveUsable(id, g_star.pack)) bad.push_back(slot);
    if (!bad.empty()) {
      std::string list;
      for (size_t k = 0; k < bad.size() && k < 4; ++k)
        list += (k ? ", " : "") + SlotLabel(bad[k]) + " (move " + std::to_string(g_star.moveset[bad[k]]) + ")";
      p.push_back(Warning(std::to_string(bad.size()) + " move(s) in the moveset the game can't play: " + list +
                          (bad.size() > 4 ? ", ..." : "") + ". In a match those slots would do the base superstar's moves.")
                      .With("Use the style's moves there", [bad] {
                        for (int s : bad) g_star.moveset.erase(s), g_star.moveset_note.erase(s);
                        Touch();
                      }, "the style's moves are back in those slots"));
    }
  }
  tag(kStepMoves);
  auto clean = [](char* s) {  // letters and digits only, capitals (the announcer's clip names)
    char* o = s;
    for (const char* c = s; *c; ++c)
      if (std::isalnum(uint8_t(*c))) *o++ = char(std::toupper(uint8_t(*c)));
    *o = 0;
  };
  for (char c : std::string(g_star.announcer))
    if (!std::isalnum(uint8_t(c))) {
      p.push_back(Error("The announcer's name can only have letters and digits (like JEFFHARDY): the announcer wouldn't say it.")
                      .With("Take out the other characters", [clean] { clean(g_star.announcer); Touch(); }));
      break;
    }
  bool bad_town = false;
  for (const char* h : {g_star.hometown_city, g_star.hometown_state})
    for (const char* c = h; *c; ++c) bad_town |= !std::isalnum(uint8_t(*c));
  if (bad_town)
    p.push_back(Error("The hometown can only have letters and digits (like CAMERONNC): the announcer wouldn't say it.")
                    .With("Take out the other characters", [clean] { clean(g_star.hometown_city), clean(g_star.hometown_state); Touch(); }));
  if (g_star.hometown_state[0] && !g_star.hometown_city[0])
    p.push_back(Warning("A hometown state without a city isn't said by the announcer.")
                    .With("Pick a city...", [] { GoStep(kStepExtras); }).With("Clear the state", [] { g_star.hometown_state[0] = 0; Touch(); }));
  if (g_star.own_abilities && g_star.abilities.empty())
    p.push_back(Warning(who + " is set to have their own abilities, but none are picked: " + who + " would have none.")
                    .With("Use the fighting style's abilities", [] { g_star.own_abilities = false; Touch(); })
                    .With("Pick abilities...", [] { GoStep(kStepExtras); }));
  tag(kStepExtras);
  if (std::strlen(g_star.name) > 20 && !g_star.short_name[0])
    p.push_back(Warning("The name is long: the match screens show a short name, and there isn't one yet.")
                    .With("Make one from the name", [] {
                      std::string n = g_star.name;
                      const size_t sp = n.find_last_of(' ');  // (the last word, as the game's own: "ORTON")
                      std::string s = sp != std::string::npos && n.size() - sp - 1 >= 3 ? n.substr(sp + 1) : n.substr(0, 20);
                      std::snprintf(g_star.short_name, sizeof g_star.short_name, "%s", s.c_str());
                      Touch();
                    }));
  if (!g_game.empty()) {
    int mods = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec))
      if (e.is_directory(ec) && fs::exists(e.path() / L"manifest.txt", ec)) ++mods;
    if (mods >= 137)
      p.push_back(Error("The game already has 137 superstar mods - the most it can hold. Installing this one would leave one out.")
                      .With("See my installed mods", [] { GoTo(PageId::kInstalled); }));
    else if (mods >= 132)
      p.push_back(Warning(std::to_string(137 - mods) + " superstar mod places left in the game (it holds 137).").Note());
  }
  tag(kStepName);
}

int g_view_attire = 1;  // the attire the preview shows (1-4)

void Preview() {
  // the attire shown: an attire<N> file's first attire, else the model's own attire N
  const std::vector<int> own = g_star.model.empty() ? std::vector<int>() : ModelAttires(g_star.model);
  std::vector<int> have;
  for (int a = 1; a <= 4; ++a)
    if (a == 1 || !g_star.attires[a - 1].empty() || std::find(own.begin(), own.end(), a) != own.end()) have.push_back(a);
  if (std::find(have.begin(), have.end(), g_view_attire) == have.end()) g_view_attire = 1;
  const bool from_file = g_view_attire > 1 && !g_star.attires[g_view_attire - 1].empty();
  {  // entrance gear: the choice made for this attire (left out / another attire's)
    const int shown = GearShown(g_view_attire);
    if (char_preview::Gear() && shown && shown != g_view_attire && !from_file) {
      char_preview::SetAttire(shown);
    } else {
      char_preview::SetAttire(from_file ? 1 : g_view_attire);
      if (char_preview::Gear() && !shown) char_preview::SetGear(false);
    }
  }
  char_preview::SetModel(from_file ? g_star.attires[g_view_attire - 1] : g_star.model, g_game);
  if (have.size() > 1) {
    ImGui::TextDisabled("Attire");
    for (int a : have) {
      ImGui::SameLine();
      const bool on = a == g_view_attire;
      if (on) PushAccent();
      if (ImGui::SmallButton((std::to_string(a) + "##va").c_str())) g_view_attire = a;
      if (on) PopAccent();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", a > 1 && !g_star.attires[a - 1].empty() ? ("Attire " + std::to_string(a) + ": " + FileName(g_star.attires[a - 1])).c_str()
                                                                         : ("Attire " + std::to_string(a) + " of the model").c_str());
    }
  }
  char_preview::SetHeight(g_star.height * StarScale(kStyles[g_star.style].template_id));  // as the game: base x height=
  ImGui::BeginGroup();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float w = std::max(220 * g_scale, avail.x);
  const float h = std::clamp(avail.y - (g_star.picture.Empty() ? 66 : 176) * g_scale, 300 * g_scale, w * 1.5f);
  char_preview::Draw(w, h);
  ViewFooter();
  if (!char_preview::Status().empty()) ImGui::TextDisabled("%s", char_preview::Status().c_str());
  if (!g_star.picture.Empty()) {
    g_star.picture.Draw(110 * g_scale, 110 * g_scale);
    ImGui::SameLine();
    ImGui::TextDisabled("The select screen's picture.");
  }
  ImGui::EndGroup();
}

void Draw() {
  if (g_test_pose >= 0 && g_test_done && char_preview::ModelHeight() > 0 && !Busy()) {
    Log("test: pose " + std::to_string(g_test_pose) + " (" + PoseLabel(g_test_pose) + ")");
    PreviewPose(g_test_pose), g_test_pose = -1;
  }
  if (!g_test_done) {  // the test aids, once the page shows
    g_test_done = true;
    if (g_test_id) {
      for (int i = 0; i < int(std::size(kStyles)); ++i)
        if (kStyles[i].template_id == g_test_id) g_star.style = i;
      if (const StarInfo* s = StarById(g_test_id)) std::snprintf(g_star.name, sizeof g_star.name, "Test %s", s->name.c_str());
      g_star.model = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(g_test_id) + L".pac")).wstring();
    }
    if (!g_test_model.empty()) g_star.model = g_test_model;
    if (!g_test_fbx.empty()) g_star.fbx = g_test_fbx, ParseFbxMap(g_test_fbx_map), g_fbx_open_once = true;
    if (g_test_fbx_make && !g_star.fbx.empty()) WhenIdle([] { MakeFromFbx(); });
    if (!g_test_fbx_report.empty() && !g_star.fbx.empty()) {
      const FbxRig& r = FbxSkeleton();
      std::vector<std::string> targets;
      for (size_t i = 0; i < r.bones.size(); ++i) targets.push_back(FbxTarget(r, i));
      std::string text = "rig: " + r.rig + "\n";
      static const char* kLabel[3] = {"TIP", "WARNING", "MUST FIX"};
      for (const auto& is : FbxInspect(Utf8(g_star.fbx), targets, r.rig, FbxOptions()))
        text += std::string(kLabel[std::clamp(is.level, 0, 2)]) + ": " + is.text + (is.fix.empty() ? "" : "\n    fix: " + is.fix) + "\n";
      WriteFile(Utf8(g_test_fbx_report), Bytes(text.begin(), text.end()));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    if (!g_test_take.empty()) TakeFromMod(g_test_take, g_test_take_all);
    for (size_t a = 0; a < g_test_gear.size();) {
      size_t b = g_test_gear.find(',', a);
      if (b == std::string::npos) b = g_test_gear.size();
      const std::string item = g_test_gear.substr(a, b - a);
      if (const size_t c = item.find(':'); c != std::string::npos) g_star.gear[std::atoi(item.c_str())] = item.substr(c + 1);
      a = b + 1;
    }
    if (const size_t c = g_test_gear_from.find(':'); c != std::string::npos && !g_star.model.empty()) {
      const int at = std::atoi(g_test_gear_from.c_str()), donor = std::atoi(g_test_gear_from.c_str() + c + 1);
      auto finish = [] {
        WriteLogTo((fs::path(g_game) / L"Mods" / L".convert" / L"gear" / L"gear.log").wstring());
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      };
      const std::string spec = g_test_gear_from.substr(c + 1);  // (<id>, or pac=<path>,<attire>: any pac's gear)
      WhenIdle([at, donor, finish, spec] {
        auto put = [finish](const std::wstring& made) { g_star.model = made, Log("test: gear fitted " + Utf8(made)), finish(); };
        if (spec.rfind("pac=", 0) == 0) {
          const size_t k = spec.rfind(',');
          GearTransplantFrom(g_star.model, at, Wide(spec.substr(4, k - 4)), std::atoi(spec.c_str() + k + 1), "test", put);
        } else {
          GearTransplant(g_star.model, at, donor, put);
        }
      });
      // (a failure closes too: after the tool, Busy() is false and the model unchanged)
    }
    if (const size_t c = g_test_gear_fbx.find(':'); c != std::string::npos && !g_star.model.empty()) {
      const int at = std::atoi(g_test_gear_fbx.c_str());
      const std::wstring fbx = Wide(g_test_gear_fbx.substr(c + 1));
      WhenIdle([at, fbx] {
        GearFromFbx(g_star.model, at, fbx, [](const std::wstring& made) {
          g_star.model = made, Log("test: gear fitted " + Utf8(made));
          WriteLogTo((fs::path(g_game) / L"Mods" / L".convert" / L"gear" / L"gear.log").wstring());
          PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        });
      });
    }
    if (g_test_simple >= 0) g_star.simple = true, g_star.simple_step = std::min(g_test_simple, int(kStepCount) - 1);
    if (!g_test_name.empty()) std::snprintf(g_star.name, sizeof g_star.name, "%s", g_test_name.c_str());
    if (!g_test_picture.empty()) {
      Image img;
      if (LoadPicture(g_test_picture, img)) SetPicture(img);
    }
    if (Image img; !g_test_full.empty() && LoadPicture(g_test_full, img)) g_star.picture_full = FitFigure(img, 512);
    if (Image img; !g_test_load.empty() && LoadPicture(g_test_load, img)) g_star.picture_load = FitLoading(img);
    if (g_test_pose >= 0) g_star.pose = g_test_pose;  // (previewed once the model is in: below)
    if (const size_t c = g_test_hometown.find(','); !g_test_hometown.empty()) {
      std::snprintf(g_star.hometown_city, sizeof g_star.hometown_city, "%s", g_test_hometown.substr(0, c).c_str());
      std::snprintf(g_star.hometown_state, sizeof g_star.hometown_state, "%s", c == std::string::npos ? "" : g_test_hometown.substr(c + 1).c_str());
    }
    if (!g_test_import.empty()) RunTestImport(g_test_import);  // (after the style: the import is set against it)
    if (!g_test_cae.empty())
      for (const auto& m : MadeEntrances())
        if (std::to_string(m.id) == g_test_cae) UseMadeEntrance(m.id, m.name);
    ApplyTestMaterials();  // (--star-mat before the test save)
    if (!g_test_save.empty() && g_test_import.rfind("game:", 0) != 0) {  // (game: saves once the port is done)
      Job j;
      PrepareJob(j);
      std::vector<ZipEntry> files;
      auto write = [](std::vector<ZipEntry>& files) {
        if (WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      };
      if (!BuildFiles(j, files)) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      else if (OwnModel(j.model) && j.threads.empty()) WithThreadsParts(j.id, std::move(files), write);  // (as Save as mod)
      else write(files);
    }
  }
  if (g_star.simple) return SimpleDraw();
  {
    const float top = ImGui::GetCursorPosY();
    Heading("Superstar", "A new playable character under the M tile of the character select (up to 137 mods).");
    const ImVec2 after = ImGui::GetCursorPos();
    const float bw = 280 * g_scale;
    ImGui::SetCursorPos(ImVec2(std::max(0.0f, ImGui::GetWindowWidth() - bw - 24 * g_scale), top));
    PushAccent();
    if (ImGui::Button("Make my first superstar (Simple)", ImVec2(bw, 0))) g_star.simple = true;
    PopAccent();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A step-by-step helper for this same superstar: name, look, music, entrance, moves, pictures.\nSwitch back here any time - nothing is lost.");
    ImGui::SetCursorPos(after);
  }
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("form", ImVec2(-360 * g_scale, -48 * g_scale), false);
  TextField("Name", g_star.name, sizeof g_star.name, "the superstar's name (up to 31 letters)");
  TextField("Short name (optional)", g_star.short_name, sizeof g_star.short_name, "on the match screens");
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Fighting style", kStyles[g_star.style].name)) {
    for (int i = 0; i < int(std::size(kStyles)); ++i)
      if (ImGui::Selectable(kStyles[i].name, g_star.style == i)) {
        g_star.style = i;
        g_star.ratings_set = false;
      }
    ImGui::EndCombo();
  }
  Hint("The style decides the move-set, the entrance motions and the starting attributes (from one of the game's "
       "superstars underneath). The moves can be changed in the game's CREATE A MOVE-SET.");
  ImGui::TextDisabled("%s", kStyles[g_star.style].about);
  if (!g_star.ratings_set) {
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StarRating(kStyles[g_star.style].template_id, k);
    g_star.ratings_set = true;
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Attributes");
  for (int k = 0; k < 7; ++k) {
    ImGui::SetNextItemWidth(320 * g_scale);
    if (ImGui::SliderInt(kAttributes[k], &g_star.ratings[k], 1, 99)) g_star.ratings_set = true;
  }
  {  // the overall (D13), as the game works it out from the seven
    ImGui::SetNextItemWidth(320 * g_scale);
    ImGui::BeginDisabled();
    int shown = OverallRating(g_star.ratings);
    ImGui::SliderInt("Overall", &shown, 1, 100);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(the game's: from the seven together)");
  }
  {  // height=: the model's size from its feet
    ImGui::SetNextItemWidth(236 * g_scale);
    ImGui::SliderFloat("##height", &g_star.height, 0.8f, 1.25f, "%.2f");
    ImGui::SameLine(0, 4 * g_scale);
    ImGui::SetNextItemWidth(80 * g_scale);
    ImGui::InputFloat("Height (scale)", &g_star.height, 0, 0, "%.2f");
    g_star.height = std::clamp(std::round(g_star.height * 100) / 100, 0.8f, 1.25f);
    const float base = StarScale(kStyles[g_star.style].template_id);
    Hint("The superstar's size against the base superstar's (the style's): 1.00 is the base's own size (the key "
         "is left out), 1.05 is 5% taller. The game stretches the skeleton, so grapples and moves follow. For "
         "converted or older models that come out too tall or too short. The game's moves are made for sizes "
         "near the roster's: about 0.90 - 1.15 plays best. The preview shows the size the game will use.");
    ImGui::SameLine();
    ImGui::TextDisabled("%+d%%", int(std::lround((g_star.height - 1) * 100)));
    if (const float tall = char_preview::ModelHeight(); tall > 0) {
      ImGui::SameLine();
      ImGui::TextDisabled("about %d cm", int(std::lround(tall * 10 * base * g_star.height)));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Measured on the model in its stance, at the size the game uses (the base's own size is x%.2f).", base);
    }
    if (g_star.height != 1.0f) {
      ImGui::SameLine();
      if (ImGui::SmallButton("1.00##hreset")) g_star.height = 1.0f;
    }
  }
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Name call (optional)", g_star.call < 0 ? "The Superstar" : kNickNames[g_star.call])) {
    if (ImGui::Selectable("The Superstar##d", g_star.call < 0)) g_star.call = -1;
    for (int i = 0; i < 84; ++i)
      if (ImGui::Selectable((std::string(kNickNames[i]) + "##n" + std::to_string(i)).c_str(), g_star.call == i))
        g_star.call = i;
    ImGui::EndCombo();
  }
  Hint("What the ring announcer and the commentators call the superstar: one of the nicknames a Created Superstar "
       "can have (or record the name below).");
  TextField("Author (optional)", g_star.author, sizeof g_star.author);
  TextField("Version (optional)", g_star.version, sizeof g_star.version, "1.0");
  MovesetSection();
  static bool more_once = false;  // (--star-victory: the section open, scrolled to)
  if (g_test_victory_open) ImGui::SetNextItemOpen(true), more_once = true, g_test_victory_open = false;
  if (ImGui::CollapsingHeader("More: entrance, announcer, abilities, moves")) {
    if (more_once) ImGui::SetScrollHereY(0.1f), more_once = false;
    ImGui::Indent();
    if (EntrancePicker("Entrance", g_star.entrance, "The style's")) Touch();
    ImGui::SameLine();
    if (ImGui::Button("Full preview")) OpenFullPreview(g_star.entrance >= 0 ? g_star.entrance : kStyles[g_star.style].template_id);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The whole entrance in the practice arena: its camera, the theme, the titantron.");
    Hint("The entrance motions and pyro (the music and movie are the files above): any the game has - every "
         "superstar's, Jeff Hardy's from SvR 2010 it ships unused (535) - or one an installed move pack adds "
         "(the From another game tab makes them from other games). Attires 2-4 can have their own.");
    CreatedEntranceRow();
    VictoryRow();
    {
      ImGui::SetNextItemWidth(320 * g_scale);
      const std::string cur = g_star.pose < 0 ? "The style's" : PoseLabel(g_star.pose);
      if (ImGui::BeginCombo("Select screen pose", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("The style's", g_star.pose < 0)) g_star.pose = -1, Touch();
        for (const auto& c : Poses())
          if (ImGui::Selectable((c.label + "##" + std::to_string(c.id)).c_str(), g_star.pose == c.id)) {
            g_star.pose = c.id, Touch();
            if (!g_star.model.empty()) PreviewPose(c.id);
          }
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(g_star.pose < 0 || g_star.model.empty() || Busy());
      if (ImGui::SmallButton(">##pose")) PreviewPose(g_star.pose);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Watch the pose in the preview.");
      Hint("How the superstar stands on the character select's 3D panel (either side of VS): a superstar's own pose "
           "(named by who has it) or one of the game's generic ones. The style's: the base superstar's.");
    }
    const Calls& calls = AnnouncerCalls();
    if (ClipPicker("Announcer name", g_star.announcer, sizeof g_star.announcer, calls.names, "none (optional)")) Touch();
    Hint("A name the ring announcer's sound banks have clips of (as the banks spell it: JEFFHARDY) - the list is the "
         "names the game has; type another and press Enter. The announcer then says it. It wins over a name "
         "recording; the name call above stays for the commentators.");
    if (ClipPicker("Hometown", g_star.hometown_city, sizeof g_star.hometown_city, calls.cities, "none (optional)")) Touch();
    Hint("Where the ring announcer says the superstar is from (\"from Cameron, NC ...\"): the cities the game has "
         "clips of, as in Create-a-Superstar's HOMETOWN. A city with its state in the clip (CAMERONNC) needs no state.");
    ImGui::SameLine();
    if (ClipPicker("##state", g_star.hometown_state, sizeof g_star.hometown_state, calls.states, "no state", 140)) Touch();
    if (calls.cities.empty()) ImGui::TextDisabled("(announcer_calls.json not found next to the tools: type the clip names)");
    if (ImGui::Checkbox("Own abilities", &g_star.own_abilities)) Touch();
    Hint("The superstar's abilities (up to 8). Unticked: the style's.");
    if (g_star.own_abilities) {
      ImGui::Indent();
      int col = 0;
      for (const auto& a : kAbilities) {
        bool on = std::find(g_star.abilities.begin(), g_star.abilities.end(), a.id) != g_star.abilities.end();
        if (col++ % 3) ImGui::SameLine(0, 20 * g_scale);
        ImGui::BeginDisabled(!on && g_star.abilities.size() >= 8);
        if (ImGui::Checkbox(a.name, &on)) {
          if (on) g_star.abilities.push_back(a.id);
          else g_star.abilities.erase(std::remove(g_star.abilities.begin(), g_star.abilities.end(), a.id), g_star.abilities.end());
          Touch();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.about);
        ImGui::EndDisabled();
      }
      ImGui::Unindent();
    }
    const COMDLG_FILTERSPEC txt[] = {{L"Moves list (*.txt)", L"*.txt"}};
    FileRow("Moves list file (optional)...", g_star.moves, "none: the slots below", txt, 1,
            "A moves.txt someone made: lines 0xOFF=<move id> (OFF a byte offset in the profile's move block, "
            "0..0x1BF, even). Your picks in the Moveset section below go after its lines and win.");
    ImGui::PushID("pack");
    if (ImGui::Button("Move pack folder (optional)...", ImVec2(220 * g_scale, 0))) {
      const std::wstring f = PickFolder(L"The move pack folder (pack.txt + motions)");
      if (!f.empty()) g_star.pack = f, Touch();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Moves the game doesn't have, carried inside the mod (moves/): a folder with pack.txt and "
                        "motions/, as the Move pack tab saves one.");
    ImGui::SameLine();
    if (g_star.pack.empty()) ImGui::TextDisabled("none");
    else ImGui::TextUnformatted(FileName(g_star.pack).c_str());
    if (!g_star.pack.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) g_star.pack.clear(), Touch();
    }
    ImGui::PopID();
    ImGui::Unindent();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Files");
  FileRow("Model (ch.pac)...", g_star.model, "required: the superstar's model", kPacFilter, 1,
          "A character model pac: one of the game's (pac\\ch\\chNNN.pac) or one converted from another game.");
  if (!g_star.model.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Export to FBX...")) ExportModelFbx(g_star.model, g_view_attire, g_star.name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The model (the attire the preview shows) with its skeleton, skin weights and textures, for Blender and other tools.");
  }
  FbxRows(false);
  GearRows(false);
  MaterialsSection(false);
  FileRow("Theme song (optional)...", g_star.song, g_star.music > 0 ? "the game's theme below" : "the style's", kSoundFilter, 1);
  if (!g_star.song.empty() && g_star.music) g_star.music = 0;  // (a song file picked: it wins)
  StockThemeRow();
  ThemeGainRow();
  CallGainRow();
  FileRow("Entrance movie (optional)...", g_star.movie, "the style's", kBinkFilter, 1,
          "A 320 x 320 movie (.mp4, or a Bink .bik): From a video... makes one here, the launcher's Movies tab too. Mods carry it as an mp4.");
  ImGui::SameLine();
  BinkFromVideoButton("From a video...", g_star.movie, g_star.name);
  MovieSizeRow(g_star.movie);
  const std::vector<int> own = g_star.model.empty() ? std::vector<int>() : ModelAttires(g_star.model);
  auto in_model = [&](int n) {  // (the game counts them contiguous from 1)
    for (int k = 1; k <= n; ++k)
      if (std::find(own.begin(), own.end(), k) == own.end()) return false;
    return true;
  };
  for (int a = 1; a < 4; ++a) {
    const bool inpac = in_model(a + 1);
    const std::string label = "Attire " + std::to_string(a + 1) + (inpac ? " (the model's own)..." : " (optional)...");
    const std::string none = inpac ? "the model's attire " + std::to_string(a + 1) : std::string("none");
    FileRow(label.c_str(), g_star.attires[a], none.c_str(), kPacFilter, 1,
            inpac ? "The model pac has this attire itself. A pac here replaces it: that pac's first attire."
                  : "Another model pac: its first attire becomes this attire.");
    if (!g_star.attires[a].empty() || inpac) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160 * g_scale);
      ImGui::InputTextWithHint(("##an" + std::to_string(a)).c_str(), ("ATTIRE " + std::to_string(a + 1)).c_str(),
                               g_star.attire_names[a], sizeof g_star.attire_names[a]);
      ImGui::SameLine();
      ImGui::PushID(a);
      if (EntrancePicker("##ae", g_star.attire_entrance[a], "its entrance: the superstar's", 200)) Touch();
      ImGui::PopID();
    }
  }
  ImGui::TextDisabled("%s", own.size() > 1 ? ("The model has " + std::to_string(std::min<size_t>(own.size(), 4)) +
                                                " attires of its own; a pac above replaces that attire.").c_str()
                                             : "Attire 1 is the model above; each extra attire is another pac's first attire.");
  {  // its fans' crowd signs
    ImGui::BeginDisabled(g_star.signs.size() >= 4);
    if (ImGui::Button("Crowd signs (optional)...", ImVec2(220 * g_scale, 0)))
      for (const auto& f : PickFiles(L"Signs for the superstar's fans (up to 4)", kPictureFilter, 1)) {
        Image img;
        if (g_star.signs.size() < 4 && LoadPicture(f, img)) {
          Picture p;
          p.Set(signs_page::SignPicture(img));
          g_star.signs.push_back(std::move(p));
          Touch();
        }
      }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_star.signs.empty()) ImGui::TextDisabled("none");
    for (auto& s : g_star.signs) {
      ImGui::SameLine();
      ImGui::Image(s.Id(), ImVec2(64 * g_scale, 32 * g_scale));
    }
    if (!g_star.signs.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x##signs")) g_star.signs.clear(), Touch();
    }
  }
  FileRow("Name recording (optional)...", g_star.voice, "none (the name call above)", kSoundFilter, 1,
          "A short recording of the name, said the ring announcer's way: it plays when he announces the superstar.");
  ImGui::PushID("pic");
  if (ImGui::Button("Select picture (optional)...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Select screen picture (a PNG with a transparent background is best)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) g_star.picture_src = img, SetPicture(img), Log("Select picture set from " + Utf8(f));
  }
  ImGui::SameLine();
  if (g_star.picture.Empty()) ImGui::TextDisabled("a silhouette");
  else ImGui::TextUnformatted("your picture");
  if (!g_star.picture.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) g_star.picture.Clear(), g_star.picture_small = Image(), g_star.picture_src = Image(), g_star.own_bust = false, Touch();
  }
  Hint("Framed the way the game's renders are: a picture with a transparent background is cropped to the person, "
       "who fills the picture top to bottom, centred. It is the select screen's render and the full-body one (the "
       "game's SSFA and SSFB); the bust below is the small one (SSFC) and the model's own portrait.");
  ImGui::Indent(16 * g_scale);
  ImGui::BeginDisabled(g_star.picture_src.w == 0 && !g_star.game_look);
  if (ImGui::Checkbox("Game look", &g_star.game_look) && g_star.picture_src.w) SetPicture(g_star.picture_src);
  ImGui::EndDisabled();
  Hint("Moves the picture's brightness, contrast and colour half way toward the game's renders. Pick the picture "
       "again to change this after reopening a project.");
  if (ImGui::Button("Bust picture (optional)...", ImVec2(204 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Bust picture: head and shoulders (a PNG with a transparent background is best)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      g_star.picture_small = FitFigure(g_star.game_look ? GameLook(img) : img, 256), g_star.own_bust = true, Touch();
      Log("Bust picture set from " + Utf8(f));
    }
  }
  ImGui::SameLine();
  if (g_star.own_bust) {
    ImGui::TextUnformatted("your bust");
    ImGui::SameLine();
    if (ImGui::SmallButton("x##bust")) {
      g_star.own_bust = false, g_star.picture_small = Image();
      if (g_star.picture_src.w) SetPicture(g_star.picture_src);
      else if (!g_star.picture.Empty()) {  // (from the framed render)
        Image render, bust, icon;
        MakeRenders(g_star.picture.image, render, bust, icon);
        g_star.picture_small = std::move(bust);
      }
      Touch();
    }
  } else ImGui::TextDisabled(g_star.picture.Empty() ? "none" : "made from the picture");
  if (ImGui::Button("Full body picture (optional)...", ImVec2(204 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Full body picture (a PNG with a transparent background is best)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      g_star.picture_full = FitFigure(g_star.game_look ? GameLook(img) : img, 512), Touch();
      Log("Full body picture set from " + Utf8(f));
    }
  }
  ImGui::SameLine();
  if (g_star.picture_full.w) {
    ImGui::TextUnformatted("your full body picture");
    ImGui::SameLine();
    if (ImGui::SmallButton("x##full")) g_star.picture_full = Image(), Touch();
  } else ImGui::TextDisabled(g_star.picture.Empty() ? "none" : "the select picture");
  Hint("The full-body render (512 x 512, the game's SSFB: the screens that show the whole superstar). Without one "
       "the select picture is used, as the game's own superstars mostly do. Framed like the game's.");
  static int scroll_to_load = 40;  // (--star-load: the row scrolled into view for a screenshot, a few frames)
  if (!g_test_load.empty() && scroll_to_load > 0 && --scroll_to_load < 20) ImGui::SetScrollHereY(0.5f);
  if (ImGui::Button(g_star.picture_load.w ? "Replace the loading picture..." : "Loading picture (optional)...", ImVec2(204 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Loading screen picture (fitted to 1024 x 512)", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      g_star.picture_load = FitLoading(img), Touch();
      Log("Loading picture set from " + Utf8(f));
    }
  }
  ImGui::SameLine();
  if (g_star.picture_load.w) {
    static Picture load_view;
    if (load_view.image.rgba != g_star.picture_load.rgba) load_view.Set(g_star.picture_load);
    load_view.Draw(96 * g_scale, 48 * g_scale);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The NOW LOADING picture (load.dds)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove##load")) g_star.picture_load = Image(), Touch();
  } else {
    ImGui::TextDisabled("none: the arena's or the game's");
  }
  Hint("The picture on the NOW LOADING screen before a match (1024 x 512). In a match the first player's superstar's "
       "picture shows, else the next one's that has one; it wins over a custom arena's own. Any picture is fitted: a "
       "wide one fills it, a square or tall one sits on the right over a blurred copy of itself."); 
  ImGui::TextDisabled("Bust:");
  Hint("The small render (256 x 256, the game's SSFC) and the model's own portrait. Made from the top of the select "
       "picture unless you give one; a head-and-shoulders picture, cropped and framed like the game's.");
  if (!g_star.picture.Empty()) {
    g_star.picture.Draw(72 * g_scale, 72 * g_scale);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select screen and full body (render.dds)");
    if (g_star.picture_small.w) {
      static Picture bust_view;
      if (bust_view.image.rgba != g_star.picture_small.rgba) bust_view.Set(g_star.picture_small);
      ImGui::SameLine();
      bust_view.Draw(72 * g_scale, 72 * g_scale);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bust and the model's portrait (render_small.dds)");
    }
  }
  if (g_star.picture_full.w) {
    static Picture full_view;
    if (full_view.image.rgba != g_star.picture_full.rgba) full_view.Set(g_star.picture_full);
    if (!g_star.picture.Empty()) ImGui::SameLine();
    full_view.Draw(72 * g_scale, 72 * g_scale);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Full body (render_full.dds)");
  }
  ImGui::Unindent(16 * g_scale);
  ImGui::PopID();
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("preview", ImVec2(0, -48 * g_scale), true);
  Preview();
  ImGui::EndChild();
  switch (ModButtons(problems, true)) {
    case 1: SaveMod(); break;
    case 2: Install(false); break;
    case 3: Install(true); break;
  }
}

std::string StateText() {
  std::string s = std::string("name=") + g_star.name + "\nshort=" + g_star.short_name + "\nstyle=" +
                  std::to_string(g_star.style) + "\ncall=" + std::to_string(g_star.call) + "\nauthor=" + g_star.author +
                  "\nversion=" + g_star.version + "\nheight=" + HeightText() + "\ngain=" + GainText() + "\nmodel=" + Utf8(g_star.model) + "\nsong=" + Utf8(g_star.song) + "\nvictory_song=" + Utf8(g_star.victory_song) + "\nvictory_gain=" + DbText(g_star.victory_gain, -20.0f, 6.0f) + "\nmusic=" + std::to_string(g_star.music) + "\nmats=" + MaterialsText(g_star.mats) +
                  "\nmovie=" + Utf8(g_star.movie) + "\nvoice=" + Utf8(g_star.voice) + "\ncall_gain=" + CallGainText() + "\nloading=" + std::to_string(g_star.picture_load.rgba.size()) + "\nratings=";
  // (the style's own attributes, filled in when the page is first drawn, aren't a change: a new project stays clean)
  bool style_ratings = true;
  for (int k = 0; k < 7; ++k) style_ratings &= g_star.ratings[k] == StarRating(kStyles[g_star.style].template_id, k);
  if (!g_star.ratings_set || style_ratings) s += "style";
  else
    for (int k = 0; k < 7; ++k) s += std::to_string(g_star.ratings[k]) + ",";
  for (int a = 1; a < 4; ++a)
    s += "\nattire" + std::to_string(a + 1) + "=" + Utf8(g_star.attires[a]) + "|" + g_star.attire_names[a] + "|" +
         std::to_string(g_star.attire_entrance[a]);
  s += "\nmoveset=" + MovesetText() + "\ncae=" + Utf8(g_star.cae) + "\nthreads=" + g_star.threads;
  s += "\nentrance=" + std::to_string(g_star.entrance) + "\nvictory=" + std::to_string(g_star.victory) + "\npose=" + std::to_string(g_star.pose) + "\nannouncer=" + g_star.announcer + "\nhometown=" + g_star.hometown_city + "," + g_star.hometown_state + "\nmoves=" + Utf8(g_star.moves) +
       "\npack=" + Utf8(g_star.pack) + "\nabil=" + std::to_string(g_star.own_abilities);
  for (int a : g_star.abilities) s += "," + std::to_string(a);
  for (const auto& [a, op] : g_star.gear) s += "\ngear" + std::to_string(a) + "=" + op;
  s += "\nfbx=" + Utf8(g_star.fbx) + "\nfbx_map=" + FbxMapText() + "\nfbx_on=" + PairsText(g_star.fbx_on) + "\nfbx_kind=" +
       PairsText(g_star.fbx_kind) + "\nfbx_pic=" + PairsText(g_star.fbx_pic) + "\nfbx_mat=" + std::to_string(g_star.fbx_mat.size()) + "\nsimple=" + std::to_string(g_star.simple);
  return s;
}

void Reset() { g_star = StarProject(); }

// "Made in the game": the created entrances the game found in its profiles at its last start (superstar_mods.cpp
// ExportCreatedEntrances: <game>\Mods\.cae\<id>.bin + list.txt) - built in the game's own CREATE AN ENTRANCE (ADVANCED
// CREATION: a motion, pyro and the rest per scene, with its preview), then given to this mod (cae=).
std::vector<MadeEntrance> MadeEntrances() {
  static std::vector<MadeEntrance> v;
  static double when = -100;
  if (ImGui::GetTime() - when < 3) return v;
  when = ImGui::GetTime();
  v.clear();
  Bytes t;
  if (!ReadFile(PathStr(fs::path(g_game) / L"Mods" / L".cae" / L"list.txt"), t)) return v;
  const std::string s(t.begin(), t.end());
  for (size_t at = 0; at < s.size();) {
    size_t e = s.find('\n', at);
    if (e == std::string::npos) e = s.size();
    const std::string l = s.substr(at, e - at);
    at = e + 1;
    if (const size_t tab = l.find('\t'); tab != std::string::npos) v.push_back({std::atoi(l.c_str()), l.substr(tab + 1)});
  }
  return v;
}

// A created entrance into the project: a copy (the game rewrites its folder at every start).
bool UseMadeEntrance(int id, const std::string& name) {
  const fs::path from = fs::path(g_game) / L"Mods" / L".cae" / (std::to_wstring(id) + L".bin");
  Bytes b;
  if (!ReadFile(PathStr(from), b) || b.size() != 284) return Status("That created entrance's file is gone: start the game once to list them again."), false;
  const fs::path to = fs::path(g_game) / L"Mods" / L".convert" / L"cae" / (std::to_wstring(id) + L"_" + std::to_wstring(std::time(nullptr)) + L".bin");
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  if (!WriteFile(PathStr(to), b)) return Status("The created entrance couldn't be copied (" + PathStr(to) + ")."), false;
  g_star.cae = to.wstring(), g_star.cae_name = name;
  Touch();
  Log("created entrance: " + name + " (" + std::to_string(id) + ") -> " + PathStr(to));
  Status(Who() + " walks out with " + name + "'s created entrance now (made in CREATE AN ENTRANCE).");
  return true;
}

void CreatedEntranceRow() {
  const auto made = MadeEntrances();
  ImGui::SetNextItemWidth(320 * g_scale);
  const std::string cur = g_star.cae.empty() ? std::string("None (the entrance above)") : g_star.cae_name + " (made in the game)";
  if (ImGui::BeginCombo("Created entrance", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable("None (the entrance above)", g_star.cae.empty())) g_star.cae.clear(), g_star.cae_name.clear(), Touch();
    for (const auto& m : made)
      if (ImGui::Selectable((m.name + " (made in the game)##" + std::to_string(m.id)).c_str(), false)) UseMadeEntrance(m.id, m.name);
    if (made.empty()) ImGui::TextDisabled("None yet: make one in the game (below), then start the game once.");
    ImGui::EndCombo();
  }
  Hint("An entrance built in the game's own CREATE AN ENTRANCE (ADVANCED CREATION: a motion, pyro and the rest for each "
       "scene, with its preview): give it to any superstar or Created Superstar there, start the game once, and pick "
       "it here. The mod then walks out with it (the music and movie stay the files above) until the player makes "
       "their own for it in the game.");
}

// Test aid --star-cae <id>: that created entrance (Mods\.cae) into the project.
std::string g_test_cae;
void TestCae(const std::string& id) { g_test_cae = id; }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_star.name);
  out.Key("short", g_star.short_name);
  out.Key("style", kStyles[g_star.style].name);
  out.Key("author", g_star.author);
  out.Key("version", g_star.version);
  std::string r;
  for (int k = 0; k < 7; ++k) r += std::to_string(g_star.ratings[k]) + (k < 6 ? "," : "");
  out.Key("ratings", r);
  if (g_star.call >= 0) out.Key("call", g_star.call);
  if (const std::string h = HeightText(); !h.empty()) out.Key("height", h);
  out.File("model", g_star.model, "star/model");
  out.Key("fbx", Utf8(g_star.fbx));  // (the FBX stays where it is: its textures sit beside it)
  out.Key("fbx_map", FbxMapText());
  if (g_star.simple) out.Key("simple", 1), out.Key("simple_step", g_star.simple_step);
  {
    std::map<std::string, std::string> g;
    for (const auto& [a, op] : g_star.gear) g[std::to_string(a)] = op;
    out.Key("gear", PairsText(g));
  }
  out.Key("fbx_on", PairsText(g_star.fbx_on));
  out.Key("fbx_kind", PairsText(g_star.fbx_kind));
  out.Key("fbx_pic", PairsText(g_star.fbx_pic));
  if (g_star.fbx_reduce > 0) out.Key("fbx_reduce", std::to_string(g_star.fbx_reduce) + (g_star.fbx_keep_face ? "" : ",all"));
  if (g_star.fbx_hair != -1) out.Key("fbx_hair", std::to_string(g_star.fbx_hair));
  out.Key("fbx_cloth", PairsText(g_star.fbx_cloth));
  {
    std::string t;
    for (const auto& [name, e] : g_star.fbx_mat) {
      std::string line = MaterialsText({{MatKey{1, false, 0u, 0}, e}});
      if (!line.empty() && line.back() == '\n') line.pop_back();
      t += (t.empty() ? "" : "\t") + name + "\x1f" + line;
    }
    out.Key("fbx_mat", t);
  }
  {
    std::string t = MaterialsText(g_star.mats);  // (one line: entries apart by tabs)
    std::replace(t.begin(), t.end(), '\n', '\t');
    out.Key("materials", t);
  }
  out.File("song", g_star.song, "star/theme");
  if (g_star.music > 0) out.Key("music", std::to_string(g_star.music));
  if (const std::string g = GainText(); !g.empty()) out.Key("theme_gain", g);
  out.File("movie", ProjectMovie(g_star.movie), "star/movie");
  if (g_settings.keep_bik && !g_star.movie.empty()) {  // (the Bink kept beside the mp4)
    const fs::path m = g_star.movie, b = Lower(PathStr(m.extension())) == ".bik" ? m : fs::path(m).replace_extension(L".bik");
    std::error_code ec;
    if (fs::exists(b, ec)) out.File("movie_bik", b.wstring(), "star/movie_bik");
  }
  out.File("cae", g_star.cae, "star/cae");
  if (!g_star.threads.empty() && !out.keys_only) {
    out.Key("threads", "star/threads.txt");
    out.files.push_back({"star/threads.txt", Bytes(g_star.threads.begin(), g_star.threads.end())});
  }
  if (!g_star.cae_name.empty()) out.Key("cae_name", g_star.cae_name);
  out.File("voice", g_star.voice, "star/voice");
  if (const std::string g = CallGainText(); !g.empty()) out.Key("call_gain", g);
  for (int a = 1; a < 4; ++a) {
    out.File("attire" + std::to_string(a + 1), g_star.attires[a], "star/attire" + std::to_string(a + 1));
    out.Key("attire" + std::to_string(a + 1) + "_name", g_star.attire_names[a]);
    if (g_star.attire_entrance[a] > 0) out.Key("attire" + std::to_string(a + 1) + "_entrance", g_star.attire_entrance[a]);
  }
  if (g_star.entrance >= 0) out.Key("entrance", g_star.entrance);
  if (g_star.victory > 0) out.Key("victory", g_star.victory);
  out.File("victory_song", g_star.victory_song, "star/victory");
  if (const std::string g = DbText(g_star.victory_gain, -20.0f, 6.0f); !g.empty()) out.Key("victory_gain", g);
  if (!g_star.source_game.empty()) out.Key("source", g_star.source_game + (g_star.source_id >= 0 ? " " + std::to_string(g_star.source_id) : ""));
  if (g_star.source_entrance >= 0) out.Key("source_entrance", g_star.source_entrance);
  if (!g_star.entrance_note.empty()) out.Key("entrance_note", g_star.entrance_note);
  out.Key("announcer", g_star.announcer);
  if (g_star.pose >= 0) out.Key("pose", std::to_string(g_star.pose));
  if (g_star.hometown_city[0])
    out.Key("hometown", std::string(g_star.hometown_city) + (g_star.hometown_state[0] ? std::string(",") + g_star.hometown_state : ""));
  if (g_star.own_abilities) {
    std::string a = "-";  // (a lone '-': own abilities, none chosen)
    for (size_t k = 0; k < g_star.abilities.size(); ++k) a = (k ? a + "," : std::string()) + std::to_string(g_star.abilities[k]);
    out.Key("abilities", a);
  }
  out.File("moves", g_star.moves, "star/moves");
  if (!g_star.moveset.empty()) out.Key("moveset", MovesetText());
  if (!g_star.pack.empty()) {  // the pack's files, as the mod carries them
    out.Key("pack", "moves");
    out.Key("pack.from", Utf8(g_star.pack));
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(out.keys_only ? fs::path() : fs::path(g_star.pack), ec)) {
      if (!e.is_regular_file(ec)) continue;
      Bytes f;
      if (ReadFile(PathStr(e.path()), f)) out.files.push_back({"moves/" + fs::relative(e.path(), g_star.pack, ec).generic_string(), std::move(f)});
    }
  }
  out.Png("picture", g_star.picture.image, "star/picture");
  if (g_star.own_bust) out.Png("bust", g_star.picture_small, "star/bust");
  if (g_star.picture_full.w) out.Png("full", g_star.picture_full, "star/full");
  if (g_star.picture_load.w) out.Png("loading", g_star.picture_load, "star/loading");
  if (g_star.game_look) out.Key("game_look", "1");
  for (size_t k = 0; k < g_star.signs.size(); ++k) out.Png("sign" + std::to_string(k + 1), g_star.signs[k].image, "star/sign" + std::to_string(k + 1));
}

// A project or a mod back into the page. Files inside come out into the
// project's cache folder so the page keeps paths.
bool Read(const ProjectIn& in, bool mod) {
  std::snprintf(g_star.name, sizeof g_star.name, "%s", in.Get("name").c_str());
  std::snprintf(g_star.short_name, sizeof g_star.short_name, "%s", in.Get("short").c_str());
  if (!std::strcmp(g_star.short_name, g_star.name)) g_star.short_name[0] = 0;
  std::snprintf(g_star.author, sizeof g_star.author, "%s", in.Get("author").c_str());
  std::snprintf(g_star.version, sizeof g_star.version, "%s", in.Get("version").c_str());
  if (!g_star.version[0]) std::snprintf(g_star.version, sizeof g_star.version, "1.0");
  const std::string style = in.Get("style");
  const int base = in.GetInt("base", -1);
  for (int i = 0; i < int(std::size(kStyles)); ++i)
    if (style == kStyles[i].name || (style.empty() && base == kStyles[i].template_id)) g_star.style = i;
  const std::string ratings = in.Get("ratings");
  if (!ratings.empty()) {
    int k = 0;
    for (size_t at = 0; at < ratings.size() && k < 7; ++k) {
      g_star.ratings[k] = std::clamp(std::atoi(ratings.c_str() + at), 1, 99);
      const size_t c = ratings.find(',', at);
      at = c == std::string::npos ? ratings.size() : c + 1;
    }
    g_star.ratings_set = true;
  }
  g_star.call = in.GetInt("call", -1);
  if (const std::string h = in.Get("height"); !h.empty()) g_star.height = std::clamp(float(std::atof(h.c_str())), 0.8f, 1.25f);
  g_star.entrance = in.GetInt("entrance", -1);
  g_star.victory = in.GetInt("victory", -1);
  {
    const std::string src = in.Get("source");  // "<game> <their id>" (a ported superstar)
    const size_t sp = src.find(' ');
    g_star.source_game = src.substr(0, sp);
    g_star.source_id = sp == std::string::npos ? -1 : std::atoi(src.c_str() + sp + 1);
    g_star.source_entrance = in.GetInt("source_entrance", -1);
    g_star.entrance_note = in.Get("entrance_note");
  }
  g_star.pose = in.GetInt("pose", -1);
  if (g_star.pose >= 0 && g_star.pose < 512)  // (an id below 512: that superstar's pose)
    for (const auto& [star, motion] : kStarPoses)
      if (star == g_star.pose) { g_star.pose = motion; break; }
  for (int a = 1; a < 4; ++a) g_star.attire_entrance[a] = in.GetInt(("attire" + std::to_string(a + 1) + "_entrance").c_str(), -1);
  std::snprintf(g_star.announcer, sizeof g_star.announcer, "%s", in.Get("announcer").c_str());
  {
    const std::string h = in.Get("hometown");
    const size_t c = h.find(',');
    std::snprintf(g_star.hometown_city, sizeof g_star.hometown_city, "%s", h.substr(0, c).c_str());
    std::snprintf(g_star.hometown_state, sizeof g_star.hometown_state, "%s", c == std::string::npos ? "" : h.substr(c + 1).c_str());
  }
  if (const std::string a = in.Get("abilities"); !a.empty()) {
    g_star.own_abilities = true;
    for (size_t at = 0; at < a.size();) {
      if (const int id = std::atoi(a.c_str() + at); id > 0 && id < 256) g_star.abilities.push_back(id);
      const size_t c = a.find(',', at);
      at = c == std::string::npos ? a.size() : c + 1;
    }
  }
  auto file = [&](const char* key, const char* mod_name) -> std::wstring {
    const std::string n = in.Get(key);
    if (mod) {  // (a mod: the manifest names the file inside)
      const std::string inner = n.empty() ? (mod_name ? mod_name : "") : n;
      if (inner.empty() || !in.Find(inner)) return {};
      return in.Extract(inner);
    }
    if (n.empty()) return {};
    // the original path still there? else the project's copy
    const std::string from = in.Get((std::string(key) + ".from").c_str());
    if (!from.empty() && fs::exists(Wide(from))) return Wide(from);
    return in.Extract(n);
  };
  g_star.model = file("model", "ch.pac");
  g_star.fbx = Wide(in.Get("fbx"));
  ParseFbxMap(in.Get("fbx_map"));
  g_star.simple = in.GetInt("simple", 0) != 0;
  g_star.simple_step = std::clamp(in.GetInt("simple_step", 0), 0, int(kStepCount) - 1);
  {
    std::map<std::string, std::string> g;
    ParsePairs(in.Get("gear"), g);
    for (const auto& [a, op] : g) g_star.gear[std::atoi(a.c_str())] = op;
  }
  ParsePairs(in.Get("fbx_on"), g_star.fbx_on);
  ParsePairs(in.Get("fbx_kind"), g_star.fbx_kind);
  ParsePairs(in.Get("fbx_pic"), g_star.fbx_pic);
  {  // (Reduce detail: "<vertices>[,all]"; none: off)
    const std::string t = in.Get("fbx_reduce");
    g_star.fbx_reduce = t.empty() ? 0 : std::clamp(std::atoi(t.c_str()), 0, kReduceMax);
    g_star.fbx_keep_face = t.find(",all") == std::string::npos;
  }
  g_star.fbx_hair = in.Get("fbx_hair").empty() ? -1 : std::clamp(std::atoi(in.Get("fbx_hair").c_str()), -1, 1);
  ParsePairs(in.Get("fbx_cloth"), g_star.fbx_cloth);
  {
    const std::string t = in.Get("fbx_mat");
    for (size_t at = 0; at < t.size();) {
      size_t e = t.find('\t', at);
      if (e == std::string::npos) e = t.size();
      const std::string item = t.substr(at, e - at);
      at = e + 1;
      const size_t sep = item.find('\x1f');
      if (sep == std::string::npos) continue;
      const auto m = MaterialsFromText(item.substr(sep + 1));
      if (!m.empty()) g_star.fbx_mat[item.substr(0, sep)] = m.begin()->second;
    }
  }
  if (mod && g_star.model.empty() && in.Find("ch.pac")) g_star.model = in.Extract("ch.pac");
  g_star.moves = file("moves", "moves.txt");
  if (mod && g_star.moves.empty() && !in.Get("moves").empty() && in.Find(in.Get("moves"))) g_star.moves = in.Extract(in.Get("moves"));
  ParseMoveset(in.Get("moveset"), ',');
  if (mod && !g_star.moves.empty()) {  // a mod's moves.txt comes into the slot list (editable by name)
    Bytes t;
    if (ReadFile(Utf8(g_star.moves), t)) ParseMoveset(std::string(t.begin(), t.end()), '\n'), g_star.moves.clear();
  }
  {  // the move pack: every moves/* file out, the folder is the pack
    bool any = false;
    std::wstring folder;
    for (const auto& e : in.files)
      if (e.name.rfind("moves/", 0) == 0) {
        const std::wstring f = in.Extract(e.name);
        if (!any && !f.empty()) folder = fs::path(f).parent_path().wstring(), any = true;
        if (e.name.find('/', 6) != std::string::npos && !f.empty())  // (a sub folder: the pack root is one up)
          folder = fs::path(f).parent_path().parent_path().wstring();
      }
    const std::string from = in.Get("pack.from");
    if (!mod && !from.empty() && fs::exists(fs::path(Wide(from)) / L"pack.txt")) g_star.pack = Wide(from);
    else if (any) g_star.pack = folder;
  }
  g_star.song = file("song", nullptr);
  g_star.victory_song = file("victory_song", nullptr);
  if (const std::string g = in.Get("victory_gain"); !g.empty()) g_star.victory_gain = std::clamp(float(std::atof(g.c_str())), -20.0f, 6.0f);
  {
    std::string t = in.Get("materials");
    std::replace(t.begin(), t.end(), '\t', '\n');
    g_star.mats = MaterialsFromText(t);
  }
  if (const int m = std::atoi(in.Get("music").c_str()); m >= 1 && m <= 253) g_star.music = m;
  if (const std::string g = in.Get("theme_gain"); !g.empty()) g_star.theme_gain = std::clamp(float(std::atof(g.c_str())), -20.0f, 6.0f);
  g_star.movie = file("movie", nullptr);
  if (Lower(PathStr(fs::path(g_star.movie).extension())) == ".mp4") {  // (its Bink: put back beside it, same name)
    std::wstring b;
    if (mod) {
      const std::string inner = PathStr(fs::path(in.Get("movie")).replace_extension(".bik"));
      if (in.Find(inner)) b = in.Extract(inner);
    } else {
      b = file("movie_bik", nullptr);
    }
    const fs::path to = fs::path(g_star.movie).replace_extension(L".bik");
    std::error_code ec;
    if (!b.empty() && fs::path(b) != to) fs::copy_file(b, to, fs::copy_options::overwrite_existing, ec);
  }
  g_star.cae = file("cae", nullptr);
  {  // (threads.txt: the project's own, or a mod's)
    const std::string tf = mod ? std::string("threads.txt") : in.Get("threads");
    for (const auto& f : in.files)
      if (!tf.empty() && f.name == tf) g_star.threads.assign(f.data.begin(), f.data.end());
  }
  g_star.cae_name = in.Get("cae_name");
  if (!g_star.cae.empty() && g_star.cae_name.empty()) g_star.cae_name = "this mod's";
  g_star.voice = file("voice", nullptr);
  if (const std::string g = in.Get("call_gain"); !g.empty()) g_star.call_gain = std::clamp(float(std::atof(g.c_str())), -20.0f, 12.0f);
  for (int a = 1; a < 4; ++a) {
    g_star.attires[a] = file(("attire" + std::to_string(a + 1)).c_str(), nullptr);
    std::snprintf(g_star.attire_names[a], sizeof g_star.attire_names[a], "%s",
                  in.Get(("attire" + std::to_string(a + 1) + "_name").c_str()).c_str());
  }
  Image img;
  if (mod) {
    if (const ZipEntry* r = in.Find("render.dds"); r && DdsDecode(r->data, img)) {
      g_star.picture.Set(img);
      if (const ZipEntry* s = in.Find("render_small.dds"); !s || !DdsDecode(s->data, g_star.picture_small)) {
        Image render, bust, icon;
        MakeRenders(img, render, bust, icon);
        g_star.picture_small = bust;
      }
    }
    if (const ZipEntry* f = in.Find("render_full.dds"); f) DdsDecode(f->data, g_star.picture_full);
    {  // the NOW LOADING picture: load.dds, or the file the manifest's loading= names (converters write both)
      const std::string named = in.Get("loading");
      const ZipEntry* l = in.Find(named.empty() ? std::string("load.dds") : named);
      if (Image li; l && DdsDecode(l->data, li)) g_star.picture_load = FitLoading(li);
    }
    for (int k = 1; k <= 4; ++k)
      if (const ZipEntry* s = in.Find("sign" + std::to_string(k) + ".dds"); s && DdsDecode(s->data, img)) {
        Picture p;
        p.Set(img);
        g_star.signs.push_back(std::move(p));
      }
  } else {
    g_star.game_look = false;  // (the saved picture has it already)
    if (in.Png("bust", img)) g_star.picture_small = img, g_star.own_bust = true;
    if (in.Png("full", img)) g_star.picture_full = img;
    if (in.Png("loading", img)) g_star.picture_load = FitLoading(img);
    if (in.Png("picture", img)) SetPicture(img);
    g_star.game_look = in.GetInt("game_look", 0) != 0;
    for (int k = 1; k <= 4; ++k)
      if (in.Png(("sign" + std::to_string(k)).c_str(), img)) {
        Picture p;
        p.Set(img);
        g_star.signs.push_back(std::move(p));
      }
  }
  if (mod && g_star.model.empty()) Log("The mod has no ch.pac: pick a model.");
  return true;
}


// Test aids: --star-mat "<attire>,<gear 0/1>,<model id>,<mesh>:spec=1.5,tint=1/0.3/0.3,cut=1,both=0,normal=flat|asis|<png>"
// (; between meshes) - the material panel's edits as if made by hand; --open-materials unfolds its section.
std::string g_test_mats;
bool g_test_open_mats = false;
void TestMaterials(const std::string& spec, bool open) {
  if (!spec.empty()) g_test_mats = spec;
  if (open) g_test_open_mats = true;
}
void ApplyTestMaterials() {
  if (const auto [pac, in] = AttireSource(g_view_attire); MaterialTestApply(pac, in, false, g_view_attire, g_star.mats)) Touch();
  if (g_test_mats.empty()) return;
  std::string all = g_test_mats;
  g_test_mats.clear();
  for (size_t at = 0; at <= all.size();) {
    size_t e = all.find(';', at);
    if (e == std::string::npos) e = all.size();
    const std::string one = all.substr(at, e - at);
    at = e + 1;
    int a = 1, g = 0, mesh = 0;
    unsigned id = 0;
    const size_t colon = one.find(':');
    if (colon == std::string::npos || std::sscanf(one.c_str(), "%d,%d,%u,%d", &a, &g, &id, &mesh) != 4) continue;
    MatEdit& m = g_star.mats[{a, g != 0, uint32_t(id), mesh}];
    std::string rest = one.substr(colon + 1);
    for (size_t p = 0; p <= rest.size();) {
      size_t q = rest.find(',', p);
      if (q == std::string::npos) q = rest.size();
      const std::string kv = rest.substr(p, q - p);
      p = q + 1;
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (k == "spec") m.has_spec = true, m.spec = float(std::atof(v.c_str()));
      if (k == "tint" && std::sscanf(v.c_str(), "%f/%f/%f", &m.tint[0], &m.tint[1], &m.tint[2]) == 3) m.has_tint = true;
      if (k == "cut") m.cutout = std::atoi(v.c_str());
      if (k == "both") m.two_sided = std::atoi(v.c_str());
      if (k == "flip") m.flip = std::atoi(v.c_str());
      if (k == "normal") m.normal = v == "flat" ? 2 : v == "asis" ? 0 : 1, m.normal_file = m.normal == 1 ? Wide(v) : std::wstring();
    }
    Log("test: material " + one);
  }
  Touch();
}

void TestStart(int id, const std::wstring& model, const std::string& name, const std::wstring& picture,
               const std::wstring& save) {
  g_test_id = id, g_test_model = model, g_test_name = name, g_test_picture = picture, g_test_save = save;
}

void SetModel(const std::wstring& ch_pac) {
  g_star.model = ch_pac;
  Touch();
}

void TestEntrance(int n) { g_star.entrance = n; }
void TestVictory(int n) { g_star.victory = n, g_test_victory_open = true; }
void TestVictorySong(const std::wstring& file, float db) { g_star.victory_song = file, g_star.victory_gain = db, g_test_victory_open = true, Touch(); }
void TestVictoryPort(const std::string& spec) { g_test_victory_port = spec, g_test_victory_open = true; }
void TestInstall() { Install(false); }
bool PickEntrance(const char* label, int& value, const char* none, float width) { return EntrancePicker(label, value, none, width); }
bool EntranceOutfit(const std::wstring& pac) { return HasEntranceOutfit(pac); }
void TestFull(const std::wstring& png) { g_test_full = png; }
void TestLoad(const std::wstring& png) { g_test_load = png; }
void ShowPose(int motion) { PreviewPose(motion); }
const std::vector<std::string>& AnnouncerNames() { return AnnouncerCalls().names; }
bool PickClip(const char* label, char* buf, size_t size, const std::vector<std::string>& clips, const char* none, float width) {
  return ClipPicker(label, buf, size, clips, none, width);
}
void TestHometown(const std::string& h) { g_test_hometown = h; }
void TestPose(int id) { g_test_pose = id; }
void TestWatchSlot(int off) { g_watch_slot_test = off / 2, g_moveset_open_once = true, g_moveset_all = true; }

void TestMoveset(const std::string& spec) {
  ParseMoveset(spec, ',');
  g_moveset_open_once = true;
}

// --star-import-moveset star:<id> | mod:<folder in Mods\Superstars>: that whole moveset into the project; open:<0-2>: the
// import shown with that source; game:<game>:<item>: the Another game source, Use once ready (then --test-star-save).
std::string g_test_import;
void TestImportMoveset(const std::string& spec) { g_test_import = spec, g_moveset_open_once = true; }
void RunTestImport(const std::string& spec) {
  if (spec.rfind("star:", 0) == 0) ImportFromStar(std::atoi(spec.c_str() + 5));
  else if (spec.rfind("open:", 0) == 0) g_imp_src = std::clamp(std::atoi(spec.c_str() + 5), 0, 2), g_imp_src == 1 ? ReadImportMods() : void();
  else if (spec.rfind("game:", 0) == 0 && spec.find(':', 5) != std::string::npos) {  // (then saved when done)
    const size_t c = spec.find(':', 5);
    g_imp_src = 2, g_imp_test_save = !g_test_save.empty();
    simple_port::ForStarTest(simple_port::kMoves, spec.substr(5, c - 5), spec.substr(c + 1));
  } else if (spec.rfind("mod:", 0) == 0) ImportFromMod(fs::path(g_game) / L"Mods" / L"Superstars" / fs::u8path(spec.substr(4)));
  else Log("import moveset test: star:<id> or mod:<folder>, not " + spec);
}

void TestFbx(const std::wstring& fbx, const std::string& map) { g_test_fbx = fbx, g_test_fbx_map = map; }

// A converted superstar (.svrmod) into this project: just its look (model, attires, height), or everything it
// brings (music, movie, pictures, moves, entrance) - the name, style and Simple step stay the user's when set.
bool TakeFromMod(const std::wstring& svrmod, bool everything) {
  Bytes zip;
  ProjectIn in;
  in.dir = fs::path(svrmod).parent_path() / fs::path(svrmod).stem();
  if (!ReadFile(Utf8(svrmod), zip) || !ZipRead(zip, in.files)) return Status("That converted superstar can't be read: " + Utf8(svrmod)), false;
  const ZipEntry* txt = in.Find("manifest.txt");
  if (!txt) return Status("The converted superstar has no manifest."), false;
  in.text.assign(txt->data.begin(), txt->data.end());
  const StarProject keep = g_star;
  if (!Read(in, true)) {
    g_star = keep;
    return Status("The converted superstar couldn't be read."), false;
  }
  if (!everything) {
    const StarProject got = g_star;
    g_star = keep;
    g_star.model = got.model, g_star.height = got.height;
    for (int a = 1; a < 4; ++a) g_star.attires[a] = got.attires[a], std::memcpy(g_star.attire_names[a], got.attire_names[a], sizeof got.attire_names[a]);
  } else {
    if (keep.name[0]) std::memcpy(g_star.name, keep.name, sizeof g_star.name), std::memcpy(g_star.short_name, keep.short_name, sizeof g_star.short_name);
    g_star.simple = keep.simple, g_star.simple_step = keep.simple_step;
  }
  Touch();
  Status(everything ? "The superstar came in with its look, music, pictures and moves." : "Your superstar has the new look.");
  return true;
}

bool TakePartFromMod(const std::wstring& svrmod, StarPart part, int slot) {
  Bytes zip;
  ProjectIn in;
  in.dir = fs::path(svrmod).parent_path() / fs::path(svrmod).stem();
  if (!ReadFile(Utf8(svrmod), zip) || !ZipRead(zip, in.files)) return Status("That converted superstar can't be read: " + Utf8(svrmod)), false;
  const ZipEntry* txt = in.Find("manifest.txt");
  if (!txt) return Status("The converted superstar has no manifest."), false;
  in.text.assign(txt->data.begin(), txt->data.end());
  const StarProject keep = g_star;
  if (!Read(in, true)) {
    g_star = keep;
    return Status("The converted superstar couldn't be read."), false;
  }
  const StarProject got = g_star;
  g_star = keep;
  const std::string who = got.name[0] ? std::string(got.name) : std::string("them");
  switch (part) {
    case StarPart::kAttire:  // (their look as one of yours: attire 2-4)
      slot = std::clamp(slot, 1, 3);
      g_star.attires[slot] = got.model;
      std::snprintf(g_star.attire_names[slot], sizeof g_star.attire_names[slot], "%s", who.c_str());
      Status("Attire " + std::to_string(slot + 1) + " is " + who + "'s look now.");
      break;
    case StarPart::kPictures:  // (their select picture, bust and full picture - the converter makes them from their renders)
      g_star.picture = got.picture, g_star.picture_small = got.picture_small, g_star.own_bust = got.own_bust;
      g_star.picture_full = got.picture_full, g_star.picture_src = got.picture_src, g_star.game_look = got.game_look;
      if (got.picture_load.w) g_star.picture_load = got.picture_load;
      Status(got.picture.Empty() ? who + " came without pictures: the converter found none." : "The pictures are " + who + "'s now.");
      break;
    case StarPart::kMusic:  // (their theme and titantron video)
      g_star.song = got.song, g_star.music = got.music, g_star.theme_gain = got.theme_gain;
      if (!got.movie.empty()) g_star.movie = got.movie;
      Status(got.song.empty() && !got.music ? who + " came without a theme." : "The music is " + who + "'s now.");
      break;
    case StarPart::kExtras:  // (their strength, height and what the announcer says)
      std::memcpy(g_star.ratings, got.ratings, sizeof g_star.ratings);
      g_star.ratings_set = true;
      g_star.height = got.height, g_star.call = got.call;
      g_star.abilities = got.abilities, g_star.own_abilities = got.own_abilities;
      std::memcpy(g_star.announcer, got.announcer, sizeof g_star.announcer);
      std::memcpy(g_star.hometown_city, got.hometown_city, sizeof g_star.hometown_city);
      std::memcpy(g_star.hometown_state, got.hometown_state, sizeof g_star.hometown_state);
      Status("Strength, height and the announcer's lines are " + who + "'s now.");
      break;
  }
  Touch();
  return true;
}

bool TestSaveMod(const std::wstring& file) {
  Job j;
  PrepareJob(j);
  std::vector<ZipEntry> files;
  const bool ok = BuildFiles(j, files) && WriteFile(Utf8(file), ZipWrite(files));
  Log(ok ? "test: saved " + Utf8(file) : "test: the mod couldn't be saved");
  return ok;
}

void TestSimple(int step) { g_test_simple = step; }
void TestGear(const std::string& spec) { g_test_gear = spec; }
void TestGearFrom(const std::string& spec) { g_test_gear_from = spec; }
void TestGearFbx(const std::string& spec) { g_test_gear_fbx = spec; }
void TestSimpleTake(const std::wstring& svrmod, bool everything) { g_test_take = svrmod, g_test_take_all = everything; }
void TestFbxMake() { g_test_fbx_make = true; }
void TestFbxStep(int step) { g_fbx_step = step; }
void TestFbxReport(const std::wstring& out) {  // with --star-fbx: the import report to <out> (automatic mapping), quit
  g_test_fbx_report = out;
}

void TestHeight(float scale) { g_star.height = std::clamp(scale, 0.8f, 1.25f); }
void TestCallGain(float db) { g_star.call_gain = std::clamp(db, -20.0f, 12.0f), g_test_scroll_call = 40; }

void TestFiles(const std::wstring& song, const std::wstring& movie, const std::wstring& voice, int call) {
  if (!song.empty()) g_star.song = song;
  if (!movie.empty()) g_star.movie = movie;
  if (!voice.empty()) g_star.voice = voice;
  if (call >= 0) g_star.call = call;
}

PageHooks hooks = {
    "superstar", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace star_page
}  // namespace mm
