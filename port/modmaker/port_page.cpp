// Port page (checklist D3): something from another WWE disc into SvR 2011, in
// three steps - a disc, what to port (superstar, arena, moves, entrance,
// belt, weapon), the thing itself - through the porting chats' converters
// (tools/<kind>_port.py, the contract in docs/MOD_MAKER_3_PLAN.md section 4:
// `games`, `list`, `convert`, JSON on stdout). The list says what 2011
// already has; the converted mod opens on its page to finish and save.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

#include "app.h"
#include "disc_list.h"
#include "full_preview.h"
#include "json.h"
#include "move_check.h"
#include "simple_port.h"
#include "svrfmt/disc.h"
#include "tool_run.h"

#include <shellapi.h>

namespace mm {
namespace port_page {

namespace {

struct Kind {
  const char* title;     // the tab
  const wchar_t* script; // tools/<kind>_port.py
  const char* about;
};
const Kind kKinds[] = {
    {"Superstars", L"superstar_port.py", "A whole superstar mod: model, pictures, theme, titantron, ratings, entrance and calls where 2011 has them."},
    {"Moves", L"moves_port.py", "Moves 2011 lacks, as a move pack (the Move pack tab saves it; a superstar mod can carry it)."},
    {"Entrances", L"entrance_port.py", "An entrance as a move pack with its own number, for a superstar mod's entrance= (and per attire)."},
    {"Arenas", L"arena_port.py", "An arena mod on one of 2011's arena slots, with its banner, loading and VS pictures."},
    {"Attires", L"attire_port.py", "Attires for a superstar - a mod's own, or new ones for 2011's roster."},
    {"Belts", L"belt_port.py", "Title belts' looks, as a texture pack."},
    {"Weapons", L"weapon_port.py", "Weapons' looks, as a texture pack."},
    {"Backstage", L"backstage_port.py", "A backstage room from another game, built into a copy of your own 2011 backstage files and "
                                        "installed into the game's Mods\\Backstage (its own row in the BACKSTAGE list). Backstage "
                                        "mods are switched off in the game for now: Backstage Brawl plays the game's own backstage, "
                                        "so a built room is kept but not loaded."},
    {"VS screens", L"vs_port.py", "An older game's VS screens (SvR 2008 - 2010: the match set-up page behind the two superstars) as a VS style, "
                                  "installed into the game's Mods\\VsStyles: pick it in GRAPHICS > VS SCREEN STYLE."},
};
constexpr int kKindCount = int(sizeof kKinds / sizeof kKinds[0]);

int g_kind = 0;
int g_disc = -1;               // index into Discs()
std::wstring g_folder;         // or an extracted folder
std::map<int, Json> g_games;   // kind -> its `games` answer
std::map<int, bool> g_games_asked;
Json g_list;                   // the `list` answer for g_list_for
std::string g_list_for;        // kind|game|src
std::string g_list_error;
int g_pick = -1;               // the chosen row
char g_find[64] = "";
bool g_hide_have = false;
Json g_result;                 // the last `convert` answer
std::string g_result_error;
char g_options[256] = "";      // extra converter options (advanced)
// --port-test: the steps run by themselves (list, convert, open)
std::string g_test_item;
int g_force_kind = -1;  // (the test's tab: ImGui reports the first tab selected until SetSelected takes)
bool g_same_moves = false;      // (PS2 superstars: --same-moves, no "similar" stand-ins)
bool g_vs_renders = true;       // (VS screens: --renders, that game's renders for 2011's superstars too)
bool g_test_fp = false;         // (--port-fullpreview: an entrance test's result opens in the Full preview)
std::string g_test_as_attire;   // (--port-as-attire <star>,<slot>,<from>[,<name>][;<save .svrmod>]: a superstar result goes on as an attire)
int g_test_stage = -1;          // 0 list, 1 convert, 2 open, 3 done
std::string g_test_game;        // (--port-test kind@game: a folder's game, as picked in its combo)

std::wstring g_image;  // a disc image picked by path (test aid), not from the list
std::wstring g_file;   // or one arena / entrance file (.pac): the converter's detect says its game and what a lone file lacks
Json g_detect;         // the detect answer for g_detect_for
std::wstring g_detect_for;
std::string g_detect_error;
int g_file_game = 0;   // 0: the detected game, n: its also[n - 1] (the file can't tell, e.g. SvR 2008 / 2009)
// one character file (superstar_port convert --src <pac>): a PS2 archive's character, the superstar number when the
// file's name lacks it, the name; g_lone_args go on the convert's command line
int g_char_pick = -1;
int g_lone_id = 0;
char g_lone_name[64] = "";
std::vector<std::wstring> g_lone_args;
// Embedded (a Simple mode's steps): the result is handed back (EmbeddedResult), not opened on its page.
bool g_embedded = false;
std::wstring g_embedded_result;
std::wstring g_embedded_picture;  // (an arena result: the disc's picture of it, from the scan)
std::string g_embedded_name;      // (an arena result: the game's own name of it, from the scan; "" none)

// ---- the work folder (<game>\Mods\.convert\port): the converters' results and their copies of disc files
constexpr uint64_t kTrimAbove = 3ull << 30, kTrimTo = 3ull << 29;  // 3 GB -> 1.5 GB
std::atomic<uint64_t> g_cache_bytes{0};
std::atomic<bool> g_cache_busy{false};
bool g_cache_known = false;

fs::path WorkDir() { return fs::path(g_game) / L"Mods" / L".convert" / L"port"; }

uint64_t FolderBytes(const fs::path& f) {
  uint64_t n = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(f, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) n += it->file_size(ec);
  return n;
}

// Measure on a thread; trim: the oldest work data out until under kTrimTo - only the converters' own dot folders in
// each result's folder (.src_<game>: the disc's files copied, .work_*: steps), never a converted mod (its .svrmod,
// its folder, report.json): the whole result folders went, oldest first, and a player converting eight 2K14
// superstars kept the last three (reports 20261101-110225, 20261101-122401). clear_all (Clear it, asked first): every
// result too; work_only with it: the work data alone.
void CacheJob(bool trim, bool clear_all, fs::path keep = {}, bool work_only = false) {
  if (g_cache_busy.exchange(true)) return;
  std::thread([dir = WorkDir(), trim, clear_all, keep, work_only] {
    std::error_code ec;
    struct Item {
      fs::path path;
      fs::file_time_type when;
      uint64_t bytes;
    };
    std::vector<Item> results, work;  // (result folders; the work data inside them)
    uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_directory(ec)) continue;
      const uint64_t b = FolderBytes(e.path());
      results.push_back({e.path(), e.last_write_time(ec), b});
      total += b;
      for (const auto& w : fs::directory_iterator(e.path(), ec))
        if (w.is_directory(ec) && w.path().filename().wstring().rfind(L".", 0) == 0)
          work.push_back({w.path(), w.last_write_time(ec), FolderBytes(w.path())});
    }
    std::string removed;
    auto drop = [&](const Item& it) {
      fs::remove_all(it.path, ec);
      if (!ec) total -= std::min(total, it.bytes), removed += (removed.empty() ? "" : ", ") + Utf8(it.path.wstring().substr(dir.wstring().size() + 1));
    };
    const auto oldest = [](const Item& a, const Item& b) { return a.when < b.when; };
    if (clear_all && !work_only) {
      for (const auto& it : results) drop(it);
    } else if (clear_all || (trim && total > kTrimAbove)) {
      std::sort(work.begin(), work.end(), oldest);
      for (const auto& it : work) {
        if (!clear_all && total <= kTrimTo) break;
        if (!clear_all && !keep.empty() && fs::equivalent(it.path.parent_path(), keep, ec)) continue;  // (the one just made)
        drop(it);
      }
    }
    const bool over = trim && total > kTrimAbove;  // (results alone over the limit: said, never removed)
    g_cache_bytes = total;
    g_cache_busy = false;
    OnUiThread([removed, over, total] {
      g_cache_known = true;
      if (!removed.empty()) Log("port: work data removed: " + removed);
      if (over) Log("port: the converted mods in Mods\\.convert\\port take " + Human(size_t(total)) +
                    " - install or save the ones you want, then Clear it (nothing is removed by itself)");
    });
  }).detach();
}

void CacheRow() {
  if (!g_cache_known && !g_cache_busy) CacheJob(false, false);
  const uint64_t b = g_cache_bytes;
  ImGui::TextDisabled("Port work folder: %s", g_cache_busy && !g_cache_known ? "measuring ..." : Human(size_t(b)).c_str());
  ImGui::SameLine();
  Hint("The converters' results and their copies of the disc's files (Mods\\.convert\\port in the game folder). "
       "Converted mods open from here; once saved or installed they aren't needed. Above 3 GB the oldest copies "
       "of disc files are removed after a conversion - never a converted mod.");
  ImGui::SameLine();
  ImGui::BeginDisabled(Busy() || g_cache_busy || !b);
  if (ImGui::SmallButton("Clear it")) ImGui::OpenPopup("##clearwork");
  ImGui::EndDisabled();
  if (ImGui::BeginPopup("##clearwork")) {  // (asked: a converted mod not yet installed or saved lives only here)
    ImGui::TextUnformatted("Converted mods you haven't installed or saved are only in this folder.");
    if (ImGui::Button("Remove the disc copies and work files only")) CacheJob(false, true, {}, true), ImGui::CloseCurrentPopup();
    if (ImGui::Button("Remove everything, converted mods too")) CacheJob(false, true), ImGui::CloseCurrentPopup();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

std::wstring Source() {
  if (!g_file.empty()) return g_file;
  if (!g_image.empty()) return g_image;
  if (!g_folder.empty()) return g_folder;
  const auto& d = Discs();
  return g_disc >= 0 && g_disc < int(d.size()) ? d[size_t(g_disc)].path : L"";
}
std::string SourceId() {
  if (!g_file.empty()) return {};  // (a lone file: its game comes from detect)
  if (!g_image.empty()) {
    static std::wstring for_image;
    static std::string id;
    if (for_image != g_image) {
      svrfmt::Disc disc;
      for_image = g_image;
      id = disc.Open(g_image) ? disc.Id() : "";
    }
    return id;
  }
  if (!g_folder.empty()) {  // (an extracted disc: its default.xex / SYSTEM.CNF says which game)
    static std::wstring for_folder;
    static std::string id;
    if (for_folder != g_folder) for_folder = g_folder, id = disc_list::Identify(g_folder);
    return id;
  }
  const auto& d = Discs();
  return g_disc >= 0 && g_disc < int(d.size()) ? d[size_t(g_disc)].id : "";
}

bool HaveScript(int k) {
  std::error_code ec;
  return !ToolsDir().empty() && fs::exists(fs::path(ToolsDir()) / kKinds[k].script, ec);
}

// The converter's game for the picked disc: {id, name, status} (null when it doesn't know the disc).
const Json& GameFor(int k) {
  static const Json kNone;
  const auto it = g_games.find(k);
  if (it == g_games.end() || it->second.type != Json::Type::kArray) return kNone;
  const std::string id = SourceId();
  if (id.empty()) return kNone;
  for (const auto& g : it->second.items) {  // disc_hint, or any of disc_hints (one game on several discs: x360 + PS2)
    if (g["disc_hint"].Str() == id) return g;
    for (const auto& h : g["disc_hints"].items)
      if (h.Str() == id) return g;
  }
  return kNone;
}

void AskGames(int k) {
  if (g_games_asked[k] || !HaveScript(k) || Busy() || PythonExe().empty()) return;
  g_games_asked[k] = true;
  RunToolJson(kKinds[k].script, {L"games"}, [k](int code, const std::string& out) {
    Json j;
    if (code == 0 && ParseJson(out, j)) g_games[k] = std::move(j);
    else Log(std::string("port: ") + Utf8(kKinds[k].script) + " games failed (exit " + std::to_string(code) + ")");
  });
}

std::vector<std::wstring> Options() {  // the advanced field, split on spaces ("quotes" keep spaces)
  std::vector<std::wstring> v;
  const std::string t = g_options;
  std::string cur;
  bool q = false;
  for (char c : t) {
    if (c == '"') q = !q;
    else if (c == ' ' && !q) {
      if (!cur.empty()) v.push_back(Wide(cur)), cur.clear();
    } else cur += c;
  }
  if (!cur.empty()) v.push_back(Wide(cur));
  return v;
}

// An item's handle for convert: "key" (most converters), "id", or entrance_port's "number".
std::string ItemKey(const Json& item) {
  for (const char* f : {"key", "id", "number"})
    if (const std::string v = item[f].Str(); !v.empty()) return v;
  return {};
}

// WWE '12 - 2K17 pacs are xcompressed: the converters read them through svrmod.exe (the Mod Maker's own pac tool),
// always the Mod Maker Tools folder's (SvrmodExe) passed as --svrmod - none from a search of their own. Missing: an
// empty --svrmod, which RunToolJson turns into the tools panel ("Missing: svrmod.exe").
std::vector<std::wstring> SvrmodArgs(int k, const std::string& game) {
  const std::wstring script = kKinds[k].script;
  if (game.rfind("wwe", 0) != 0 || (script != L"superstar_port.py" && script != L"entrance_port.py" && script != L"moves_port.py")) return {};
  return {L"--svrmod", SvrmodExe()};
}

void List(int k, const std::string& game) {
  const std::wstring src = Source();
  g_list_for = std::to_string(k) + "|" + game + "|" + Utf8(src);
  g_list = Json();
  g_list_error.clear();
  g_pick = -1;
  std::vector<std::wstring> largs = {L"list", L"--game", Wide(game), L"--src", src};
  if (std::wstring(kKinds[k].script) != L"backstage_port.py")  // (its list takes no --pac11: the rooms don't depend on it)
    largs.push_back(L"--pac11"), largs.push_back((fs::path(g_game) / L"pac").wstring());
  for (auto& x : SvrmodArgs(k, game)) largs.push_back(x);
  RunToolJson(kKinds[k].script, largs,
              [want = g_list_for](int code, const std::string& out) {
                if (want != g_list_for) return;
                Json j;
                if (code == 0 && ParseJson(out, j) && j.type == Json::Type::kArray) {
                  g_list = std::move(j);
                  Status("Listed " + std::to_string(g_list.items.size()) + " items.");
                } else {
                  Json e;
                  g_list_error = ParseJson(out, e) && !e["error"].Str().empty() ? e["error"].Str()
                                 : "the converter stopped (exit " + std::to_string(code) + "): see the log";
                }
              });
}

int g_template = 0;          // moves: --template <2011 move id> (0: the converter's own pick)
char g_template_find[48] = "";

// A free 2011 move record for a converted move (moves_port --id): a WAZE record with no motion in any m.pac bank and
// not used by an installed move pack - the one with the move's name first; 0 = let the converter pick.
int FreeMoveRecord(const std::string& name) {
  std::set<int> taken;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Moves", ec)) {
    Bytes t;
    if (!e.is_directory(ec) || !svrfmt::ReadFile(Utf8((e.path() / L"pack.txt").wstring()), t)) continue;
    const std::string s(t.begin(), t.end());
    for (size_t at = 0; at < s.size();) {
      size_t nl = s.find('\n', at);
      if (nl == std::string::npos) nl = s.size();
      char word[16] = "";
      int id = 0;
      if (std::sscanf(s.substr(at, nl - at).c_str(), "%15s %d", word, &id) == 2 &&
          (!std::strcmp(word, "waze") || !std::strcmp(word, "wazecopy") || !std::strcmp(word, "wazename")))
        taken.insert(id);
      at = nl + 1;
    }
  }
  int first = 0;
  for (const auto& [id, m] : MoveInfos()) {
    if (anims_page::AnyMotion(id) || taken.count(id)) continue;
    if (!name.empty() && Lower(m.name) == Lower(name)) return id;
    if (!first) first = id;
  }
  return first;
}

// The picture picker (a game whose renders can't be tied to superstars - WWE 2K17's hashed names): the converter's
// `pictures` verb lists every render as a thumbnail with its best guess for the item; the pick goes to convert as
// --picture <key>.
struct PicturePick {
  std::string for_;        // kind|game|src|item the list is for
  bool asked = false;
  Json list;               // {"pictures": [{key, thumb, w, h, name}], "guess": {key, score, confident, from} | null}
  std::string error;
  std::string pick;        // the key picked ("" none)
  std::map<std::string, Picture> thumbs;
  char find[48] = "";
};
PicturePick g_pp;

bool NeedsPicture(const Json& item) { return item["needs_picture"].Truthy(); }
// The converter's "status": "crashes" (converts, but 2011 crashes at load - its full pass) or "special" (not an item
// of the kind, e.g. a WWE '12+ backstage room): greyed, not pickable, its "reason" as the tooltip.
bool Blocked(const Json& item) {
  const std::string st = item["status"].Str();
  return st == "crashes" || st == "special";
}
void BlockedRow(const std::string& text, const Json& item) {
  ImGui::TextDisabled("%s", text.c_str());
  if (ImGui::IsItemHovered()) {
    const std::string why = item["reason"].Str();
    ImGui::SetTooltip("%s%s", item["status"].Str() == "crashes" ? "Crashes the game: " : "Not portable: ",
                      why.empty() ? "known from the converter's full pass." : why.c_str());
  }
}

void AskPictures(int k, const std::string& game, const Json& item) {
  const std::string want = std::to_string(k) + "|" + game + "|" + Utf8(Source()) + "|" + ItemKey(item);
  if (g_pp.for_ == want && (g_pp.asked || Busy())) return;
  if (g_pp.for_ != want) g_pp = PicturePick(), g_pp.for_ = want;
  if (Busy()) return;
  g_pp.asked = true;
  const fs::path cache = WorkDir() / Wide(game + "_pictures");
  std::error_code ec;
  fs::create_directories(cache, ec);
  std::vector<std::wstring> pargs = {L"pictures", L"--game", Wide(game), L"--src", Source(), L"--item", Wide(ItemKey(item)), L"--out", cache.wstring()};
  for (auto& x : SvrmodArgs(k, game)) pargs.push_back(x);
  RunToolJson(kKinds[k].script, pargs,
              [want](int code, const std::string& out) {
    if (g_pp.for_ != want) return;
    Json j;
    if (code != 0 || !ParseJson(out, j) || j.type != Json::Type::kObject) {
      g_pp.error = "the converter couldn't list the pictures (exit " + std::to_string(code) + "): see the log";
      return;
    }
    g_pp.list = std::move(j);
    const Json& g = g_pp.list["guess"];
    if (g.type == Json::Type::kObject && g["confident"].Truthy()) g_pp.pick = g["key"].Str();
  });
}

// The grid: every picture, the guess outlined, the pick shown big.
void PicturePicker() {
  if (!g_pp.error.empty()) {
    ImGui::TextColored(kBad, "%s", g_pp.error.c_str());
    return;
  }
  const Json& pics = g_pp.list["pictures"];
  if (pics.items.empty()) {
    ImGui::TextDisabled("Reading the disc's pictures ...");
    return;
  }
  const Json& g = g_pp.list["guess"];
  const std::string guess = g.type == Json::Type::kObject ? g["key"].Str() : "";
  ImGui::TextUnformatted("The select picture");
  ImGui::SameLine();
  if (!guess.empty()) {
    char score[16];
    std::snprintf(score, sizeof score, "%d%%", int(std::lround(g["score"].n * 100)));
    ImGui::TextDisabled("(best guess: %s match%s%s)", score, g["from"].Str().empty() ? "" : " with ", g["from"].Str().c_str());
  } else {
    ImGui::TextDisabled("(this game's pictures have no names: pick the one that's this superstar)");
  }
  bool named = false;  // (a find box only when the pictures have names: 2K17's are hashes)
  for (const auto& p : pics.items) named |= !p["name"].Str().empty();
  if (named) {
    ImGui::SetNextItemWidth(200 * g_scale);
    ImGui::InputTextWithHint("##picfind", "find by name", g_pp.find, sizeof g_pp.find);
    ImGui::SameLine();
  }
  if (ImGui::SmallButton("No picture")) g_pp.pick.clear();
  const std::string f = Lower(g_pp.find);
  const float tw = 72 * g_scale;
  ImGui::BeginChild("pics", ImVec2(-200 * g_scale, 2.2f * (tw + 8 * g_scale)), true);
  const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 6 * g_scale) / (tw + 6 * g_scale)));
  int n = 0;
  for (const auto& p : pics.items) {
    const std::string key = p["key"].Str(), name = p["name"].Str();
    if (!f.empty() && Lower(name).find(f) == std::string::npos && Lower(key).find(f) == std::string::npos) continue;
    if (n++ % per_row) ImGui::SameLine(0, 6 * g_scale);
    ImGui::PushID(key.c_str());
    ImGui::BeginGroup();  // (one item for SameLine, whatever the picture draws)
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::IsRectVisible(ImVec2(tw, tw))) {  // (thumbnails load on first sight)
      auto it = g_pp.thumbs.find(key);
      if (it == g_pp.thumbs.end()) {
        Image img;
        if (LoadPicture(Wide(p["thumb"].Str()), img)) it = g_pp.thumbs.emplace(key, Picture()).first, it->second.Set(std::move(img));
        else it = g_pp.thumbs.emplace(key, Picture()).first;
      }
      if (!it->second.Empty()) it->second.Draw(tw, tw);
      else ImGui::Dummy(ImVec2(tw, tw));
    } else {
      ImGui::Dummy(ImVec2(tw, tw));
    }
    ImGui::EndGroup();
    if (ImGui::IsItemClicked()) g_pp.pick = key;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.empty() ? key.c_str() : name.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (key == g_pp.pick) dl->AddRect(at, ImVec2(at.x + tw, at.y + tw), IM_COL32(220, 40, 60, 255), 4 * g_scale, 0, 3 * g_scale);
    else if (key == guess) dl->AddRect(at, ImVec2(at.x + tw, at.y + tw), IM_COL32(240, 200, 90, 255), 4 * g_scale, 0, 1.5f * g_scale);
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (const auto it = g_pp.thumbs.find(g_pp.pick); !g_pp.pick.empty() && it != g_pp.thumbs.end() && !it->second.Empty())
    it->second.Draw(180 * g_scale, 180 * g_scale);
  else
    ImGui::TextDisabled(g_pp.pick.empty() ? "No picture picked:\nthe mod keeps the\nbase superstar's." : "");
  ImGui::EndGroup();
}

void Convert(int k, const std::string& game, const Json& item) {
  const std::string key = ItemKey(item);
  std::string safe;
  for (char c : std::wstring(kKinds[k].script) == L"backstage_port.py" ? key : game + "_" + key) safe += std::isalnum(uint8_t(c)) ? c : '_';
  // a backstage room is built into a copy of the player's own bg78 / gm pacs, straight into the game's Mods\Backstage
  const bool backstage = std::wstring(kKinds[k].script) == L"backstage_port.py";
  const fs::path out = backstage ? fs::path(g_game) / L"Mods" / L"Backstage" / Wide(safe)
                                 : WorkDir() / Wide(std::string(kKinds[k].title) + "_" + safe);
  std::error_code ec;
  fs::create_directories(out, ec);
  const bool entrance = std::wstring(kKinds[k].script) == L"entrance_port.py";
  std::vector<std::wstring> args = {L"convert", L"--game", Wide(game), L"--src", Source(), L"--out", out.wstring(),
                                    L"--pac11", backstage ? fs::path(g_game).wstring() : (fs::path(g_game) / L"pac").wstring()};
  if (entrance) args.push_back(L"--entrance"), args.push_back(Wide(key));  // (its own options: --entrance, --number)
  else args.push_back(L"--item"), args.push_back(Wide(key));
  const bool moves = std::wstring(kKinds[k].script) == L"moves_port.py";
  if (moves && g_template > 0) args.push_back(L"--template"), args.push_back(std::to_wstring(g_template));
  if (moves) {  // (a free 2011 record, so packs don't collide: the converter's own pick can't see installed packs)
    if (!anims_page::MovesIndexed()) {
      Status("Reading m.pac's moves first (a few seconds) - press Convert again.");
      return;
    }
    if (const int id = FreeMoveRecord(item["name"].Str()); id > 0) {
      args.push_back(L"--id"), args.push_back(std::to_wstring(id));
      Log("port: move record " + std::to_string(id) + " for " + item["name"].Str());
    }
  }
  for (auto& x : SvrmodArgs(k, game)) args.push_back(x);
  if (g_same_moves && game.rfind("ps2_", 0) == 0 && std::wstring(kKinds[k].script) == L"superstar_port.py") args.push_back(L"--same-moves");
  if (std::wstring(kKinds[k].script) == L"superstar_port.py")  // (the attire picker's choice: which four, in order)
    if (const std::string at = simple_port::AttireChoice(game, Source(), key); !at.empty()) args.push_back(L"--attires"), args.push_back(Wide(at));
  if (entrance && !g_settings.bink2_tool.empty())  // (its --titantron: Bink 2 movies through the player's tool)
    args.push_back(L"--bink2-tool"), args.push_back(g_settings.bink2_tool);
  if (NeedsPicture(item) && !g_pp.pick.empty()) args.push_back(L"--picture"), args.push_back(Wide(g_pp.pick));
  if (g_vs_renders && std::wstring(kKinds[k].script) == L"vs_port.py") args.push_back(L"--renders");
  for (auto& o : Options()) args.push_back(o);
  for (auto& o : g_lone_args) args.push_back(o);
  g_result = Json();
  g_result_error.clear();
  auto run = [k, out, entrance](std::vector<std::wstring> a) {
    RunToolJson(kKinds[k].script, a, [out, entrance](int code, const std::string& text) {
    Json j;
    if (!ParseJson(text, j) || j.type != Json::Type::kObject) {
      g_result_error = "the converter stopped (exit " + std::to_string(code) + "): see the log";
      return;
    }
    g_result = std::move(j);
    if (g_result["out"].Str().empty()) {  // (entrance_port's report has no out / opens: its pack is <out>\pack)
      Json o;
      o.type = Json::Type::kString;
      o.s = Utf8((entrance ? out / L"pack" : out).wstring());
      g_result.fields.push_back({"out", std::move(o)});
      if (entrance && g_result["opens"].Str().empty()) {
        Json m;
        m.type = Json::Type::kString;
        m.s = "moves";
        g_result.fields.push_back({"opens", std::move(m)});
      }
    }
    if (code != 0 || !g_result["ok"].Truthy())
      g_result_error = g_result["error"].Str().empty() ? "the conversion failed (exit " + std::to_string(code) + ")" : g_result["error"].Str();
    else
      Status("Converted: " + g_result["out"].Str());
    CacheJob(true, false, fs::path(Wide(g_result["out"].Str())).parent_path());  // (measure, trim above 3 GB)
    });
  };
  if (!entrance) {
    run(args);
    return;
  }
  // an entrance needs a free 2011 entrance number: the converter's free-numbers first
  const std::wstring user = [] {  // (a number given in the options wins)
    const std::string o = g_options;
    const size_t at = o.find("--number");
    return at == std::string::npos ? std::wstring() : Wide(o.substr(at));
  }();
  if (!user.empty()) {
    run(args);
    return;
  }
  RunToolJson(kKinds[k].script, {L"free-numbers", L"--game-pac", (fs::path(g_game) / L"pac").wstring(), L"--mods",
                                 (fs::path(g_game) / L"Mods").wstring()},
              [run, args](int code, const std::string& text) {
                Json j;
                if (code != 0 || !ParseJson(text, j) || j.items.empty()) {
                  g_result_error = "no free 2011 entrance number found (free-numbers: exit " + std::to_string(code) + ")";
                  return;
                }
                auto a = args;
                a.push_back(L"--number"), a.push_back(Wide(j.items.front().Str()));
                Log("port: entrance number " + j.items.front().Str());
                WhenIdle([run, a] { run(a); });  // (after this job: one job at a time)
              });
}

// ---- "native" games (arenas of WWE '12 / '13 / 2K14): the Mod Maker's own converter (Other games page) takes
// the arena pac; here the disc's (or folder's) pac\bg\bgNN.pac are listed and the picked one is copied out to it.
struct NativePac {
  std::string name;  // bgNN.pac
  int entry = -1;    // in the disc (-1: g_folder file)
  uint64_t size = 0;
};
std::string g_native_for;
std::vector<NativePac> g_native;
int g_native_pick = -1;
std::string g_native_test;  // (--port-test: the item to use)
std::map<std::string, Picture> g_arena_thumbs;  // a scanned arena's picture (its PNG in the arena list cache)
std::set<std::string> g_scan_asked;            // discs whose arena list was built (or asked) this session
char g_arena_find[48] = "";
bool g_scanning_list = false;

// "Build the list": the disc read once for its arenas' names and pictures (arena_port.py scan, cached per disc), then
// listed again from the cache.
void ScanArenas(int k, const std::string& game) {
  if (Busy() || g_scanning_list) return;
  g_scan_asked.insert(game + "|" + Utf8(Source()));
  g_scanning_list = true;
  std::vector<std::wstring> args = {L"scan", L"--game", Wide(game), L"--src", Source()};
  RunToolJson(kKinds[k].script, args, [k, game](int code, const std::string&) {
    g_scanning_list = false;
    if (code != 0) Status("The arena list couldn't be built (see the log): the arenas show by file name.");
    g_list_for.clear();  // (listed again: names and pictures from the cache now)
    WhenIdle([k, game] { List(k, game); });
  });
}

// A native pac's entry in the converter's list ("bg37"), or null: its status / reason is the one source of truth.
const Json& NativeItem(const NativePac& p) {
  static const Json none;
  const std::string key = Lower(p.name.substr(0, p.name.rfind('.')));
  for (const Json& it : g_list.items)
    if (Lower(ItemKey(it)) == key) return it;
  return none;
}
bool NativeBlocked(const NativePac& p) {
  const Json& it = NativeItem(p);
  return it.IsNull() ? SpecialStage(p.name) : Blocked(it);
}

void NativeArenas(int k, const std::string& game, const std::string& game_name) {
  const std::string want = game + "|" + Utf8(Source());
  std::error_code dec;
  const std::wstring src = Source();
  const bool src_folder = fs::is_directory(src, dec);
  if (g_native_for != want) {  // the bg pacs, once per source (the disc's table is in memory: quick)
    g_native_for = want, g_native.clear(), g_native_pick = -1;
    if (!src_folder) {
      svrfmt::Disc disc;
      if (disc.Open(src)) {
        const int bg = disc.Find("pac/bg");
        if (bg >= 0)
          for (int c : disc.At(bg).children)
            if (const auto& e = disc.At(c); !e.dir && Lower(e.name).rfind("bg", 0) == 0 && Lower(e.name).ends_with(".pac"))
              g_native.push_back({e.name, c, e.size});
      }
    } else {
      std::error_code ec;
      for (const auto& e : fs::directory_iterator(fs::path(src) / L"pac" / L"bg", ec)) {
        const std::string n = Utf8(e.path().filename().wstring());
        if (e.is_regular_file(ec) && Lower(n).rfind("bg", 0) == 0 && Lower(n).ends_with(".pac")) g_native.push_back({n, -1, e.file_size(ec)});
      }
    }
    std::sort(g_native.begin(), g_native.end(), [](const NativePac& a, const NativePac& b) { return a.name < b.name; });
  }
  if (g_list_for != std::to_string(k) + "|" + want) List(k, game);  // (the converter's list: only each pac's status)
  if (g_embedded) ImGui::TextWrapped("The Mod Maker converts %s arenas itself: pick the arena.", game_name.c_str());
  else ImGui::TextWrapped("The Mod Maker converts %s arenas itself: pick the arena's pac, then finish below (extracted files) "
                          "(host arena, options, Convert).", game_name.c_str());
  if (g_native.empty()) {
    ImGui::TextColored(kWarn, "No pac\bg\bgNN.pac found here.");
    return;
  }
  // the arenas as a grid of pictures with the game's own names (the list built once per disc: names + pictures)
  bool named = false, pictured = false;
  for (const Json& it : g_list.items) named |= !it["bg"].Str().empty() && it["name"].Str() != it["bg"].Str(), pictured |= !it["thumb"].Str().empty();
  const std::string disc_key = game + "|" + Utf8(Source());
  if (!g_list.items.empty() && !pictured && !g_scan_asked.count(disc_key) && !Busy()) ScanArenas(k, game);  // (the first time: by itself)
  if (g_scanning_list) {
    ImGui::TextColored(kWarn, "Building the list of %s's arenas (their names and pictures) - once per disc, about a minute ...", game_name.c_str());
  } else if (!pictured) {
    ImGui::BeginDisabled(Busy());
    if (ImGui::Button("Build the list")) ScanArenas(k, game);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(named ? "(adds the arenas' pictures)" : "(reads the arenas' names and pictures off the disc once)");
  }
  ImGui::SetNextItemWidth(260 * g_scale);
  ImGui::InputTextWithHint("##findarena", "Find an arena...", g_arena_find, sizeof g_arena_find);
  ImGui::SameLine();
  ImGui::TextDisabled("%d arenas", int(g_native.size()));
  const std::string find = Lower(g_arena_find);
  auto title_of = [](const NativePac& p) {
    const Json& it = NativeItem(p);
    const std::string n = it.IsNull() ? std::string() : it["name"].Str();
    return n.empty() ? p.name.substr(0, p.name.rfind('.')) : n;
  };
  const float tw = 176 * g_scale, th = tw * 0.5625f, gap = 8 * g_scale;
  ImGui::BeginChild("arenagrid", ImVec2(0, std::max(220 * g_scale, ImGui::GetContentRegionAvail().y - 60 * g_scale)), true);
  const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (tw + gap)));
  for (int pass = 0; pass < 2; ++pass) {  // the arenas, then the ones that crash / aren't arenas (can't be picked)
    int n = 0;
    for (int i = 0; i < int(g_native.size()); ++i) {
      const NativePac& np = g_native[size_t(i)];
      if (NativeBlocked(np) != (pass == 1)) continue;
      const std::string title = title_of(np);
      if (!find.empty() && Lower(title).find(find) == std::string::npos && Lower(np.name).find(find) == std::string::npos) continue;
      if (pass == 1 && n == 0) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Not portable: they crash the game, or they're backstage rooms / studio stages");
      }
      if (n++ % per_row) ImGui::SameLine(0, gap);
      ImGui::PushID(i);
      ImGui::BeginGroup();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const Json& it = NativeItem(np);
      const std::string thumb = it.IsNull() ? std::string() : it["thumb"].Str();
      Picture* pic = nullptr;
      if (!thumb.empty()) {
        auto t = g_arena_thumbs.find(thumb);
        if (t == g_arena_thumbs.end()) {
          Image img;
          t = g_arena_thumbs.emplace(thumb, Picture()).first;
          if (LoadPicture(Wide(thumb), img)) t->second.Set(std::move(img));
        }
        if (!t->second.Empty()) pic = &t->second;
      }
      if (pass == 1) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f);
      if (pic) ImGui::Image(pic->Id(), ImVec2(tw, th));
      else ImGui::Button(np.name.c_str(), ImVec2(tw, th));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
      ImGui::TextUnformatted(title.c_str());
      ImGui::PopTextWrapPos();
      if (pass == 1) ImGui::PopStyleVar();
      ImGui::EndGroup();
      const bool hovered = ImGui::IsItemHovered();
      if (g_native_pick == i)
        ImGui::GetWindowDrawList()->AddRect(ImVec2(at.x - 2, at.y - 2), ImVec2(at.x + tw + 2, at.y + th + 2), IM_COL32(230, 25, 50, 255), 4, 0, 3 * g_scale);
      if (pass == 0 && ImGui::IsItemClicked()) g_native_pick = i;
      if (hovered) {
        char mb[32];
        std::snprintf(mb, sizeof mb, "%.1f MB", np.size / 1048576.0);
        if (pass == 1 && !it.IsNull()) {
          const std::string why = it["reason"].Str();
          ImGui::SetTooltip("%s (%s, %s)\n%s%s", title.c_str(), np.name.c_str(), mb, it["status"].Str() == "crashes" ? "Crashes the game: " : "Not portable: ",
                            why.empty() ? "known from the converter's full pass." : why.c_str());
        } else if (pass == 1) {
          ImGui::SetTooltip("%s (%s)\nA backstage room or a studio stage, not an arena: on an arena slot it shows a white mat over a black void, or crashes.",
                            title.c_str(), np.name.c_str());
        } else {
          ImGui::SetTooltip("%s\n%s, %s", title.c_str(), np.name.c_str(), mb);
        }
      }
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  if (!g_native_test.empty() && (!g_list.items.empty() || !g_list_error.empty())) {  // (test: pick by name, once the statuses are in)
    for (int i = 0; i < int(g_native.size()); ++i)
      if ((Lower(g_native[size_t(i)].name) == Lower(g_native_test) || Lower(g_native[size_t(i)].name) == Lower(g_native_test) + ".pac") &&
          !NativeBlocked(g_native[size_t(i)]))  // (bg07 or bg07.pac)
        g_native_pick = i;
    if (g_native_pick < 0) Log("port test: no arena pac " + g_native_test), g_native_test.clear();
  }
  if (g_native_pick >= 0 && NativeBlocked(g_native[size_t(g_native_pick)])) g_native_pick = -1;  // (its status came in after the pick)
  ImGui::BeginDisabled(g_native_pick < 0 || Busy());
  PushAccent();
  const bool use = ImGui::Button(g_embedded ? "4. Use this arena" : "4. Use this arena", ImVec2(320 * g_scale, 0)) ||
                   (!g_native_test.empty() && g_native_pick >= 0 && !Busy());
  PopAccent();
  ImGui::EndDisabled();
  if (!use) return;
  g_native_test.clear();
  const NativePac p = g_native[size_t(g_native_pick)];
  const std::wstring image = src_folder ? L"" : src, folder = src_folder ? src : L"";
  const fs::path out = WorkDir() / Wide("Arenas_" + game + "_" + p.name);
  const bool embedded = g_embedded;
  // the scan's picture (the full-size backdrop when the scan kept it, else its thumb) as the banner + loading picture,
  // and the game's own name of the arena with the game's ("RAW (WWE 2K17)": most are SvR2011's own shows too) as the
  // mod's name
  const Json& item = NativeItem(p);
  const std::wstring thumb = Wide(!item["picture"].Str().empty() ? item["picture"].Str() : item["thumb"].Str());
  const std::string stem = fs::path(p.name).stem().string(), scanned = item["name"].Str();
  // (the arena select shows about 30 letters: a long one gets "(2K17)", then nothing)
  const disc_list::Game* from = disc_list::GameByKey(game);
  std::string title = !scanned.empty() && Lower(scanned) != Lower(stem) && Lower(scanned).rfind("arena bg", 0) != 0 ? scanned : "";
  if (!title.empty() && from) {
    const std::string full = from->name, brief = full.rfind("WWE ", 0) == 0 ? full.substr(4) : full;
    if (title.size() + full.size() + 3 <= 30) title += " (" + full + ")";
    else if (title.size() + brief.size() + 3 <= 30) title += " (" + brief + ")";
  }
  RunInBackground([p, image, folder, out, game, embedded, thumb, title, stem] {
    Progress("Copying " + p.name + " off the disc ...");
    Bytes b;
    bool ok = false;
    if (p.entry >= 0) {
      svrfmt::Disc disc;
      ok = disc.Open(image) && disc.ReadAll(p.entry, b);
    } else {
      ok = svrfmt::ReadFile(Utf8((fs::path(folder) / L"pac" / L"bg" / Wide(p.name)).wstring()), b);
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    if (!ok || !svrfmt::WriteFile(Utf8(out.wstring()), b)) { Status("Could not copy " + p.name + "."); return; }
    OnUiThread([out, p, game, embedded, thumb, title, stem] {
      if (embedded) {  // (a Simple mode converts it itself)
        Log("port: " + p.name + " -> " + Utf8(out.wstring()) + (title.empty() ? "" : " (" + title + ")"));
        g_embedded_result = out.wstring(), g_embedded_picture = thumb, g_embedded_name = title;
        CacheJob(true, false, out.parent_path());
        return;
      }
      Log("port: " + p.name + " -> " + Utf8(out.wstring()) + " (Arena > From another game)");
      other_page::UseArenaFile(out.wstring(), !title.empty() ? title : IdFrom(game + "_" + stem, "arena"), thumb);
      GoTo(PageId::kOther);
      CacheJob(true, false, out.parent_path());
    });
  });
}

// The converted thing onto its page.

void OpenResult() {
  const std::string out = g_result["out"].Str(), opens = g_result["opens"].Str();
  const fs::path p = Wide(out);
  std::error_code ec;
  if (g_embedded && !out.empty()) {  // (a .svrmod, or a result folder: the Simple mode takes it from here)
    g_embedded_result = p.wstring();
    return;
  }
  if (Lower(p.extension().string()) == ".svrmod") {
    ProjectOpen(p.wstring());
    return;
  }
  if (opens == "moves" && fs::exists(p / L"pack.txt", ec)) {
    moves_page::TestOpen(p.wstring());
    GoTo(PageId::kMoves);
    return;
  }
  if (opens == "backstage" && fs::exists(p / L"manifest.txt", ec)) {  // (installed already: opened as a project, the folder only read)
    if (ProjectOpenFolder(p.wstring())) Status("Installed into Mods\\Backstage\\" + Utf8(p.filename().wstring()) + ", opened on the Backstage tab.");
    return;
  }
  if (opens == "vs_style") {  // a VS style folder: into the game's Mods\VsStyles\<game id> (vs_port.py)
    const std::string as = g_result["install_as"].Str().empty() ? Utf8(p.filename().wstring()) : g_result["install_as"].Str();
    const fs::path dst = fs::path(g_game) / L"Mods" / L"VsStyles" / Wide(as);
    fs::create_directories(dst, ec);
    for (const wchar_t* f : {L"manifest.txt", L"vs.pac"})
      if (!ec) fs::copy_file(p / f, dst / f, fs::copy_options::overwrite_existing, ec);
    if (!ec && fs::exists(p / L"renders", ec)) {  // (its renders: a media pack of their own)
      const fs::path media = fs::path(g_game) / L"Mods" / L"Media" / Wide(as + "_renders");
      fs::copy(p / L"renders", media, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    Status(ec ? "Could not install the style: " + ec.message()
              : "Installed into Mods\\VsStyles\\" + as + ": pick it in the game's GRAPHICS > VS SCREEN STYLE (from the next start).");
    return;
  }
  if (opens == "belt" || opens == "weapon") {  // a texture pack folder: into the game's Texture Packs
    const fs::path dst = fs::path(g_game) / L"Texture Packs" / p.filename();
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(p, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    Status(ec ? "Could not copy the pack: " + ec.message()
              : "Installed into Texture Packs\\" + Utf8(p.filename().wstring()) + ": tick it in the launcher's Texture packs tab.");
    return;
  }
  if (out.empty() || !fs::exists(p, ec)) {
    Status("The converter didn't say where its result is: see the log.");
    return;
  }
  if (g_test_stage >= 0) {  // (tests never open windows)
    Log("port test: would open " + out);
    return;
  }
  ShellOpen(p.wstring());
}

std::wstring PickOneFile() {
  static const COMDLG_FILTERSPEC kPac[1] = {{L"An arena, entrance or character file (*.pac)", L"*.pac"}};
  const std::wstring f = PickFile(false, L"One file from another game: an arena (pac\\bg\\bgNN.pac), an entrance (Nyujyo0.pac, Entrance01.pac) "
                         L"or a character (pac\\ch\\chNNN.pac, a PS2 game's CH.PAC)", kPac, 1, nullptr, nullptr, OtherGameStart());
  if (!f.empty()) OtherGamePicked(f);
  return f;
}
bool CharacterName(const std::string& stem);
// the one-file source, read as a character file (not named like an arena or entrance pac)
bool CharacterFile() {
  const std::string n = Lower(Utf8(fs::path(g_file).stem().wstring()));
  return !g_file.empty() && (CharacterName(n) || (n.rfind("bg", 0) != 0 && n.rfind("nyujyo", 0) != 0 && n.rfind("entrance", 0) != 0 &&
                                                  n.rfind("evt", 0) != 0));
}
// The kinds one file can be (each converter's detect reads it), and the tab a file's name points to.
bool TakesOneFile(int k) {
  const std::wstring s = kKinds[k].script;
  return s == L"arena_port.py" || s == L"entrance_port.py" || s == L"superstar_port.py";
}
// a character file's name: chNNN (360), CH / CH2 (a PS2 game's archive of characters)
bool CharacterName(const std::string& stem) {
  if (stem.rfind("ch", 0) != 0) return false;
  for (size_t i = 2; i < stem.size(); ++i)
    if (!std::isdigit(uint8_t(stem[i]))) return false;
  return true;
}
void TabForFile(const std::wstring& f) {
  const std::string n = Lower(Utf8(fs::path(f).stem().wstring()));
  bool digits = n.size() == 5;
  for (char c : n) digits &= std::isdigit(uint8_t(c)) != 0;
  const bool entrance = digits || n.rfind("nyujyo", 0) == 0 || n.rfind("entrance", 0) == 0 || n.rfind("evt", 0) == 0 || n.rfind("en3", 0) == 0;
  const wchar_t* script = CharacterName(n) ? L"superstar_port.py" : entrance ? L"entrance_port.py" : L"arena_port.py";
  for (int k = 0; k < kKindCount; ++k)
    if (std::wstring(kKinds[k].script) == script) g_kind = g_force_kind = k;
}

// One file (no disc): the kind's detect says the game (or a few it may be) and what a lone file can't bring. Arenas
// of WWE '12 - 2K17: the Mod Maker converts the file itself (as an arena off the disc); the rest: the usual list +
// convert, with the game it returns.
std::string FileSource(int k) {
  const std::wstring for_this = g_file + L"|" + kKinds[k].script;
  const bool arena = std::wstring(kKinds[k].script) == L"arena_port.py";
  const bool star = std::wstring(kKinds[k].script) == L"superstar_port.py";
  if (g_detect_for != for_this && !Busy()) {
    g_detect_for = for_this, g_detect = Json(), g_detect_error.clear(), g_file_game = 0;
    g_char_pick = -1, g_lone_id = 0, g_lone_name[0] = 0, g_result = Json(), g_result_error.clear();
    std::vector<std::wstring> dargs = {L"detect", L"--src", g_file};
    if (star)  // (its detect: xcompressed pacs through svrmod.exe, a PS2 archive's character list cached by the game's pac)
      dargs.insert(dargs.end(), {L"--pac11", (fs::path(g_game) / L"pac").wstring(), L"--svrmod", SvrmodExe()});
    RunToolJson(kKinds[k].script, dargs, [for_this, star](int code, const std::string& out) {
      if (for_this != g_detect_for) return;
      Json j;
      const bool parsed = ParseJson(out, j) && j.type == Json::Type::kObject;
      if (code == 0 && parsed && (star ? j["ok"].Truthy() && !j["games"].items.empty() : !j["game"].Str().empty())) g_detect = std::move(j);
      else g_detect_error = parsed && !j["error"].Str().empty() ? j["error"].Str() : "the converter stopped (exit " + std::to_string(code) + "): see the log";
    });
  }
  if (!g_detect_error.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kBad, "%s", g_detect_error.c_str());
    ImGui::PopTextWrapPos();
    return {};
  }
  if (g_detect.type != Json::Type::kObject || g_detect_for != for_this) {
    ImGui::TextDisabled("Reading the file ...");
    return {};
  }
  const char* what = arena ? "An arena" : star ? "A superstar" : "Entrances";
  std::vector<std::string> ids = {g_detect["game"].Str()};
  for (const Json& a : g_detect["also"].items) ids.push_back(a.Str());
  if (star) {  // (superstar_port: games[] = {id, name}, every game the file fits; game set only when it's one)
    ids.clear();
    for (const Json& a : g_detect["games"].items) ids.push_back(a["id"].Str());
  }
  auto name_of = [k](const std::string& id) {
    for (const Json& g : g_detect["games"].items)
      if (g["id"].Str() == id && !g["name"].Str().empty()) return g["name"].Str();
    for (const Json& g : g_games[k].items)
      if (g["id"].Str() == id) return g["name"].Str();
    return id;
  };
  g_file_game = std::clamp(g_file_game, 0, int(ids.size()) - 1);
  const std::string game = ids[size_t(g_file_game)];
  if (g_detect["sure"].Truthy() || ids.size() == 1) {
    const std::string from = star ? name_of(game)
                             : !g_detect["game_name"].Str().empty() ? g_detect["game_name"].Str() : g_detect["name"].Str();
    if (!arena && g_detect["entrances"].Int() > 0)
      ImGui::Text("2. %d entrance%s from %s", int(g_detect["entrances"].Int()), g_detect["entrances"].Int() == 1 ? "" : "s", from.c_str());
    else
      ImGui::Text("2. %s from %s", what, from.c_str());
  } else {
    std::string all;
    for (size_t i = 0; i < ids.size(); ++i) all += (i ? (i + 1 == ids.size() ? " or " : ", ") : "") + name_of(ids[i]);
    ImGui::TextWrapped("2. %s from %s (the file looks the same in these games).", what, all.c_str());
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::BeginCombo("Which game it's from", name_of(game).c_str())) {
      for (int i = 0; i < int(ids.size()); ++i)
        if (ImGui::Selectable(name_of(ids[size_t(i)]).c_str(), g_file_game == i)) g_file_game = i;
      ImGui::EndCombo();
    }
    Hint(star ? "These games keep their characters the same way: pick the one the file is from (its disc's name)."
              : "Only the names from the port's list depend on it: the file itself converts the same.");
  }
  ImGui::PushTextWrapPos(0);
  const Json& lacks = arena ? g_detect["lacks"] : g_detect["limits"];  // (arena_port: lacks, entrance_port: limits)
  if (!lacks.items.empty()) {
    ImGui::TextDisabled("On its own, the file can't bring:");
    for (const Json& l : lacks.items) {  // (the converter's words, without its command-line options)
      std::string t = l.Str();
      for (const char* opt : {" (with --pac11)", " (--pac11)", ": --titantron needs the disc"})
        for (size_t at; (at = t.find(opt)) != std::string::npos;) t.erase(at, std::strlen(opt));
      ImGui::BulletText("%s", t.c_str());
    }
  }
  for (const Json& n : g_detect["notes"].items) ImGui::TextColored(kWarn, "%s", n.Str().c_str());
  if (!g_detect["note"].Str().empty()) ImGui::TextDisabled("%s", g_detect["note"].Str().c_str());
  ImGui::PopTextWrapPos();
  if (g_detect["convert"].Str() != "mod_maker") return game;
  // WWE '12 - 2K17: the file is the arena pac the Mod Maker converts
  const Json& item = g_detect["items"].items.empty() ? Json() : g_detect["items"].items.front();
  const std::string title = item["name"].Str().empty() ? Utf8(fs::path(g_file).stem().wstring()) : item["name"].Str();
  if (Blocked(item)) {
    BlockedRow(title + (item["status"].Str() == "crashes" ? "  (crashes the game)" : "  (not portable)"), item);
    g_native_test.clear();
    return {};
  }
  ImGui::Text("3. %s", title.c_str());
  ImGui::BeginDisabled(Busy());
  PushAccent();
  const bool use = ImGui::Button((std::string(g_embedded ? "4. Use this arena" : "4. Use this arena") + "##lone").c_str(),
                                 ImVec2(320 * g_scale, 0)) ||
                   (!g_native_test.empty() && !Busy());
  PopAccent();
  ImGui::EndDisabled();
  if (use) {
    g_native_test.clear();
    Log("port: one file " + Utf8(g_file) + " (" + game + ")");
    if (g_embedded) {
      g_embedded_result = g_file;
    } else {
      other_page::UseArenaFile(g_file, IdFrom(game + "_" + title, "arena"));
      GoTo(PageId::kOther);
    }
  }
  return {};
}

// A converter's progress ("<n>%  <text>" in the status line) as a bar while its job runs.
void ProgressBar() {
  const std::string p = ProgressNow();
  const int pct = std::atoi(p.c_str());
  const size_t sp = p.find("%  ");
  ImGui::ProgressBar(std::clamp(pct, 0, 100) / 100.0f, ImVec2(420 * g_scale, 0),
                     sp == std::string::npos ? "working ..." : (p.substr(0, sp + 1) + "  " + p.substr(sp + 3)).c_str());
}

// "pictures / renders (the base's)" -> "pictures / renders"; "name and profile (--name, else the file name)" -> "name
// and profile" (the list says up front that the base superstar's stand in)
std::string PlainMissing(std::string t) {
  for (size_t a; (a = t.find(" (")) != std::string::npos;) {
    const size_t b = t.find(')', a);
    if (b == std::string::npos) break;
    t.erase(a, b - a + 1);
  }
  return t;
}

// One character file (superstar_port convert --src <pac>): the character of a PS2 archive, the superstar number a
// 360 file's name doesn't give, the name, then convert (2 - 7 minutes for PS2: the progress shows) and the mod opens
// on the Superstar page; what one file can't bring is listed in plain words.
void LoneStar(int k, const std::string& game) {
  const Json& chars = g_detect["characters"];
  const bool ps2 = chars.type == Json::Type::kArray && !chars.items.empty();
  std::string key;
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("On its own, the file brings the model, its outfits, entrance gear and hair. The pictures, theme, "
                      "titantron, entrance, moves, ratings and name stay the base superstar's until you set them.");
  if (!g_detect["note"].Str().empty()) ImGui::TextDisabled("%s", g_detect["note"].Str().c_str());
  ImGui::PopTextWrapPos();
  if (ps2) {  // an archive of characters: which one
    if (g_test_stage == 0 && !g_test_item.empty())
      for (int i = 0; i < int(chars.items.size()); ++i)
        if (chars.items[size_t(i)]["id"].Str() == g_test_item) g_char_pick = i;
    if (chars.items.size() == 1) g_char_pick = 0;
    ImGui::Text("3. Which character (%d in this file)", int(chars.items.size()));
    auto label = [](const Json& c) {
      std::string outfits;
      for (const Json& a : c["attires"].items) outfits += (outfits.empty() ? "" : ", ") + std::to_string(a.Int() + 1);
      const std::string n = c["name"].Str();
      return (n.empty() ? "Character " + c["id"].Str() : n + "  (" + c["id"].Str() + ")") +
             (outfits.empty() ? "" : "   outfits " + outfits);
    };
    if (ImGui::BeginChild("chars", ImVec2(420 * g_scale, std::min(8.0f, float(chars.items.size())) * ImGui::GetTextLineHeightWithSpacing() + 8 * g_scale), true)) {
      for (int i = 0; i < int(chars.items.size()); ++i)
        if (ImGui::Selectable((label(chars.items[size_t(i)]) + "##c" + std::to_string(i)).c_str(), g_char_pick == i)) g_char_pick = i;
    }
    ImGui::EndChild();
    Hint("The file holds several characters by number: the names live on the disc. Each one's outfits are listed; "
         "convert one and look at it on the Superstar page.");
    if (g_char_pick >= 0 && g_char_pick < int(chars.items.size())) key = chars.items[size_t(g_char_pick)]["id"].Str();
  } else if (g_detect["id"].Int() > 0) {
    key = std::to_string(g_detect["id"].Int());
    ImGui::Text("3. Superstar %s of %s", key.c_str(), Utf8(fs::path(g_file).filename().wstring()).c_str());
  } else {  // a renamed file: the number the game gave the superstar
    ImGui::SetNextItemWidth(120 * g_scale);
    ImGui::InputInt("3. The superstar's number", &g_lone_id);
    Hint("The file's name doesn't say it: the number in the game's own name for it (chNNN.pac).");
    if (g_lone_id > 0) key = std::to_string(g_lone_id);
  }
  if (ps2 && !key.empty()) simple_port::AttirePicker(game, Source(), key);
  ImGui::SetNextItemWidth(260 * g_scale);
  ImGui::InputTextWithHint("Name", "the file's name if left empty", g_lone_name, sizeof g_lone_name);
  if (game.rfind("ps2_", 0) == 0) {
    ImGui::Checkbox("Same moves only (no similar ones)", &g_same_moves);
    Hint("The moves stay the base superstar's (one file has no moveset); this only matters if you add his moves later.");
  }
  ImGui::SetNextItemWidth(360 * g_scale);
  ImGui::InputTextWithHint("Options (advanced)", "e.g. --base 104 --height 1.05", g_options, sizeof g_options);
  Hint("Extra options the converter takes, e.g. --base <2011 superstar id> to start from that superstar.");
  const auto go = [k, game, &key] {
    Json item;
    item.type = Json::Type::kObject;
    Json v;
    v.type = Json::Type::kString;
    v.s = key;
    item.fields.push_back({"key", std::move(v)});
    g_lone_args.clear();
    if (g_lone_name[0]) g_lone_args = {L"--name", Wide(g_lone_name)};
    Log("port: one character file " + Utf8(g_file) + " (" + game + ", " + key + ")");
    Convert(k, game, item);
    g_lone_args.clear();
  };
  if (g_test_stage == 0 && !Busy()) {  // (--port-test Superstars,<file>,<character or number>)
    if (key.empty()) Log("port test: no character " + g_test_item), g_test_stage = 3;
    else go(), g_test_stage = 2;
  }
  if (g_test_stage == 2 && !Busy() && (g_result.type == Json::Type::kObject || !g_result_error.empty())) {
    Log("port test: convert " + (g_result_error.empty() ? "ok: " + g_result["out"].Str() : "FAILED: " + g_result_error));
    for (const Json& m : g_result["missing"].items) Log("port test: missing " + PlainMissing(m.Str()));
    g_test_stage = 3;
    if (g_result_error.empty()) OpenResult();
  }
  ImGui::BeginDisabled(Busy() || key.empty());
  PushAccent();
  if (ImGui::Button(ps2 ? "4. Convert this character" : "4. Convert this superstar")) go();
  PopAccent();
  ImGui::EndDisabled();
  if (Busy() && g_result.type != Json::Type::kObject && g_result_error.empty()) {
    ImGui::SameLine();
    ProgressBar();
    if (game.rfind("ps2_", 0) == 0) ImGui::TextDisabled("A PS2 character takes a few minutes.");
  }
  if (!g_result_error.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kBad, "%s", g_result_error.c_str());
    ImGui::PopTextWrapPos();
  }
  if (g_result.type != Json::Type::kObject || !g_result["ok"].Truthy()) return;
  ImGui::SameLine();
  PushAccent();
  if (ImGui::Button(g_embedded ? "5. Use it" : "5. Open it on the Superstar page")) OpenResult();
  PopAccent();
  ImGui::PushTextWrapPos(0);
  if (!g_result["missing"].items.empty()) {
    ImGui::TextColored(kWarn, "Not in one file - the base superstar's are used until you set your own:");
    for (const Json& m : g_result["missing"].items) ImGui::BulletText("%s", PlainMissing(m.Str()).c_str());
  }
  for (const char* list : {"dropped", "notes"})
    for (const auto& w : g_result[list].items)
      ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.7f, 1), "%s%s", std::string(list) == "dropped" ? "left out: " : "", w.Str().c_str());
  ImGui::PopTextWrapPos();
}

void DiscRow() {
  const auto& discs = Discs();
  std::string label = !g_file.empty()   ? Utf8(fs::path(g_file).filename().wstring()) + " (one file)"
                      : !g_image.empty()  ? Utf8(fs::path(g_image).filename().wstring())
                      : !g_folder.empty() ? Utf8(fs::path(g_folder).filename().wstring()) + " (folder)"
                      : g_disc >= 0 && g_disc < int(discs.size()) ? discs[size_t(g_disc)].game
                                                                   : "pick a disc";
  ImGui::SetNextItemWidth(420 * g_scale);
  if (ImGui::BeginCombo("1. The disc", label.c_str())) {
    if (!DiscsReady()) ImGui::TextDisabled("Looking for your discs ...");
    else if (discs.empty()) ImGui::TextDisabled("No discs yet: add them on My game discs (below).");
    for (int i = 0; i < int(discs.size()); ++i)
      if (ImGui::Selectable((discs[size_t(i)].game + "##" + std::to_string(i)).c_str(), Source() == discs[size_t(i)].path))
        g_disc = i, g_folder.clear(), g_image.clear(), g_file.clear(), g_list = Json(), g_list_for.clear(), g_result = Json();
    ImGui::Separator();
    if (ImGui::Selectable("An extracted folder ...")) {
      const std::wstring f = PickFolder(L"A folder with another game's files (an extracted disc)", OtherGameStart());
      if (const std::string why = disc_list::NotGameFiles(f); !why.empty()) Status(why), Log("port: " + Utf8(f) + " - " + why);
      else if (!f.empty()) OtherGamePicked(f), g_folder = f, g_image.clear(), g_file.clear(), g_list = Json(), g_list_for.clear(), g_result = Json();
    }
    if (ImGui::Selectable("One file: an arena, an entrance or a character (.pac) ...")) {
      const std::wstring f = PickOneFile();
      if (!f.empty()) SetSource(f), TabForFile(f);
    }
    if (ImGui::Selectable("My game discs ...")) GoTo(PageId::kDiscs);
    ImGui::EndCombo();
  }
  Hint("Your WWE games from My game discs (set once there: Find my discs searches a folder for them): Xbox 360 SvR "
       "2007 to 2K17 and the PS2 games, disc images read as they are, or extracted folders. Or one file on its own: an "
       "arena (bgNN.pac, SvR 2007 to WWE 2K17), an entrance file (Nyujyo / Entrance pacs, the PS2 games' EVT pacs) or a "
       "character (chNNN.pac from any of the Xbox 360 games, a PS2 game's CH.PAC).");
}

}  // namespace

void TestClear() { CacheJob(false, true); }
void TestTrim() { CacheJob(true, false); }  // (--port-trim: the after-conversion trim, nothing kept)
void TestFullPreview() { g_test_fp = true; }
void TestAsAttire(const std::string& spec) { g_test_as_attire = spec; }
// The ported entrance (g_result: its move pack and number), played whole by `who` before it is installed.
int g_fp_who = 139;
void PortedEntrancePreview(const Json& item) {
  full_preview::Request r;
  r.entrance = std::atoi(g_result["number"].Str().c_str());
  r.star = g_fp_who;
  r.model = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(g_fp_who) + L".pac")).wstring();
  r.pack = Wide(g_result["out"].Str());
  r.title = (item["name"].Str().empty() ? ItemKey(item) : item["name"].Str()) + " (ported)";
  Log("port: full preview of entrance " + std::to_string(r.entrance) + " from " + g_result["out"].Str());
  full_preview::Open(r);
}

// A kind's steps: the game, the list, convert, the result.
void KindBody(int k) {
  ImGui::TextDisabled("%s", kKinds[k].about);
  if (std::wstring(kKinds[k].script) == L"backstage_port.py")
    ImGui::TextColored(kWarn, "The room is read from your own Here Comes the Pain disc (PS2): pick your disc image above. Nothing of it "
                              "comes with the Mod Maker or the port.");
  if (!HaveScript(k)) {
    ImGui::TextDisabled("This converter (%s) isn't in this build yet.", Utf8(kKinds[k].script).c_str());
    return;
  }
  AskGames(k);
  if (!g_games.count(k)) {
    ImGui::TextDisabled(g_games_asked[k] ? "Asking the converter which games it knows ..." : "");
    return;
  }
  std::string game;
  const Json& g = g_file.empty() ? GameFor(k) : Json();
  if (!g_file.empty()) {
    if (!TakesOneFile(k)) {
      ImGui::TextDisabled("One file on its own works for superstars, arenas and entrances: see those tabs.");
      return;
    }
    game = FileSource(k);
    if (game.empty()) return;
    if (std::wstring(kKinds[k].script) == L"superstar_port.py") {  // (one character file: no list, the file is the item)
      LoneStar(k, game);
      return;
    }
  } else if (g.IsNull() && (g_folder.empty() || !SourceId().empty())) {
    ImGui::TextColored(kWarn, "This converter doesn't know this disc (%s).", SourceId().c_str());
    return;
  }
  if (g_file.empty() && !g.IsNull()) {
    game = g["id"].Str();
    const std::string status = g["status"].Str();
    ImGui::Text("2. %s: %s", g["name"].Str().c_str(), status == "ready" ? "ready" : status.c_str());
    if (status == "native") {
      NativeArenas(k, game, g["name"].Str());
      return;
    }
    if (status != "ready") {
      ImGui::TextColored(kWarn, "Not portable yet: the format of this game is still being worked out.");
      return;
    }
  } else if (g_file.empty()) {  // a folder: pick the game
    static int pick = 0;
    const auto& all = g_games[k].items;
    if (all.empty()) return;
    for (int i = 0; !g_test_game.empty() && i < int(all.size()); ++i)
      if (all[size_t(i)]["id"].Str() == g_test_game) pick = i;
    pick = std::clamp(pick, 0, int(all.size()) - 1);
    ImGui::SetNextItemWidth(320 * g_scale);
    if (ImGui::BeginCombo("2. The folder's game", all[size_t(pick)]["name"].Str().c_str())) {
      for (int i = 0; i < int(all.size()); ++i)
        if (ImGui::Selectable((all[size_t(i)]["name"].Str() + "##" + std::to_string(i)).c_str(), pick == i)) pick = i;
      ImGui::EndCombo();
    }
    game = all[size_t(pick)]["id"].Str();
  }
  const std::string want = std::to_string(k) + "|" + game + "|" + Utf8(Source());
  if (g_test_stage == 0 && !Busy()) List(k, game), g_test_stage = 1;
  const bool index_wait = std::wstring(kKinds[k].script) == L"moves_port.py" && !anims_page::MovesIndexed();  // (Convert needs it)
  if (g_test_stage == 1 && !Busy() && !index_wait && g_list_for == want && (!g_list.items.empty() || !g_list_error.empty())) {
    if (!g_list_error.empty()) Log("port test: list failed: " + g_list_error), g_test_stage = 3;
    for (int i = 0; g_test_stage == 1 && i < int(g_list.items.size()); ++i)
      if (!g_test_item.empty() && g_test_item.back() == '?' && ItemKey(g_list.items[size_t(i)]) + "?" == g_test_item) {
        Log("port test: " + std::to_string(g_list.items.size()) + " listed; selected " + g_test_item);  // ("<key>?": picked, not converted)
        g_pick = i, g_test_stage = 3;
      } else if (ItemKey(g_list.items[size_t(i)]) == g_test_item && Blocked(g_list.items[size_t(i)])) {
        Log("port test: " + g_test_item + " is " + g_list.items[size_t(i)]["status"].Str() + ": not converted");
        g_test_stage = 3;
      } else if (ItemKey(g_list.items[size_t(i)]) == g_test_item) {
        Log("port test: " + std::to_string(g_list.items.size()) + " listed; converting " + g_test_item);
        g_pick = i;
        Convert(k, game, g_list.items[size_t(i)]);
        g_test_stage = 2;
      }
    if (g_test_stage == 1) Log("port test: no item " + g_test_item), g_test_stage = 3;
  }
  if (g_test_stage == 2 && !Busy() && (g_result.type == Json::Type::kObject || !g_result_error.empty())) {
    Log("port test: convert " + (g_result_error.empty() ? "ok: " + g_result["out"].Str() : "FAILED: " + g_result_error));
    g_test_stage = 3;
    if (g_result_error.empty()) {
      if (g_test_fp && std::wstring(kKinds[k].script) == L"entrance_port.py" && g_pick >= 0 && g_pick < int(g_list.items.size()))
        PortedEntrancePreview(g_list.items[size_t(g_pick)]);
      else if (!g_test_as_attire.empty() && g_result["opens"].Str() == "superstar") {
        const std::string& t = g_test_as_attire;
        const size_t semi = t.find(';');
        const std::string a = t.substr(0, semi), save = semi == std::string::npos ? "" : t.substr(semi + 1);
        const size_t c1 = a.find(','), c2 = a.find(',', c1 + 1), c3 = a.find(',', c2 + 1);
        attire_page::TestStart(std::atoi(a.c_str()), Wide(g_result["out"].Str()), std::atoi(a.c_str() + c1 + 1), Wide(save));
        attire_page::TestFrom(std::atoi(a.c_str() + c2 + 1), c3 == std::string::npos ? "" : a.substr(c3 + 1));
        Log("port test: the superstar goes on " + a + " as an attire");
        GoTo(PageId::kAttire);
      } else
        OpenResult();
    }
  }
  ImGui::BeginDisabled(Busy());
  if (ImGui::Button(g_list_for == want ? "List again" : "3. List what's on the disc")) List(k, game);
  ImGui::EndDisabled();
  if (g_list_for != want) return;
  if (!g_list_error.empty()) ImGui::TextColored(kBad, "%s", g_list_error.c_str());
  if (g_list.items.empty()) {
    if (Busy()) ImGui::TextDisabled("Reading the disc ...");
    return;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::InputTextWithHint("##find", "find", g_find, sizeof g_find);
  ImGui::SameLine();
  ImGui::Checkbox("hide what 2011 already has", &g_hide_have);
  const std::string f = Lower(g_find);
  const float list_h = std::max(160 * g_scale, ImGui::GetContentRegionAvail().y - 190 * g_scale);
  if (ImGui::BeginTable("items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, list_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("2011", ImGuiTableColumnFlags_WidthFixed, 150 * g_scale);
    ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < int(g_list.items.size()); ++i) {
      const Json& it = g_list.items[size_t(i)];
      const bool have = it["in_2011"].Truthy();  // (a name / id, or true; false / missing = new)
      if (have && g_hide_have) continue;
      const std::string name = it["name"].Str().empty() ? ItemKey(it) : it["name"].Str();
      if (!f.empty() && Lower(name).find(f) == std::string::npos && Lower(ItemKey(it)).find(f) == std::string::npos) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (Blocked(it)) BlockedRow(name + (it["status"].Str() == "crashes" ? "  (crashes)" : "  (not portable)"), it);
      else if (ImGui::Selectable((name + "##" + std::to_string(i)).c_str(), g_pick == i, ImGuiSelectableFlags_SpanAllColumns)) g_pick = i;
      ImGui::TableNextColumn();
      if (have) ImGui::TextColored(kWarn, "has it (%s)", it["in_2011"].Str().c_str());
      else ImGui::TextDisabled("new");
      if (it["in_svr07"].Truthy()) {  // (svr06 entrances: SvR 2007 has the same one, a better source)
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "SvR 2007 has it");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("SvR 2007's version of this entrance is the better source: port it from that disc.");
      }
      ImGui::TableNextColumn();
      std::string details;  // the converter's other fields, short
      for (const auto& [key, v] : it.fields) {
        if (key == "name" || key == "in_2011" || key == "in_svr07" || key == "key" || key == "status" || key == "reason" || v.type == Json::Type::kArray || v.type == Json::Type::kObject || v.IsNull()) continue;
        if (!details.empty()) details += "  ";
        details += key + " " + v.Str();
      }
      ImGui::TextDisabled("%s", details.c_str());
    }
    ImGui::EndTable();
  }
  if (g_pick < 0 || g_pick >= int(g_list.items.size()) || Blocked(g_list.items[size_t(g_pick)])) {
    ImGui::TextDisabled("4. Pick one to convert.");
    return;
  }
  const Json& item = g_list.items[size_t(g_pick)];
  if (game.rfind("ps2_", 0) == 0 && std::wstring(kKinds[k].script) == L"superstar_port.py") {
    ImGui::Checkbox("Same moves only (no similar ones)", &g_same_moves);
    Hint("Ticked: only the moves 2011 has too come over; a move it lacks keeps the fighting style's. Unticked: a "
         "similar 2011 move stands in for one it lacks (marked \"similar\" in the moveset, to swap if you like).");
  }
  if (std::wstring(kKinds[k].script) == L"vs_port.py") {
    ImGui::Checkbox("That game's renders for 2011's superstars too", &g_vs_renders);
    Hint("Ticked: the superstars both games have (the same name) show this game's renders - on the VS screen and "
         "everywhere 2011 shows renders - as a media pack of their own (Mods\Media, off there to go back).");
  }
  if (NeedsPicture(item)) {
    AskPictures(k, game, item);
    PicturePicker();
  }
  if (std::wstring(kKinds[k].script) == L"superstar_port.py") simple_port::AttirePicker(game, Source(), ItemKey(item));
  ImGui::SetNextItemWidth(360 * g_scale);
  const bool room = std::wstring(kKinds[k].script) == L"backstage_port.py";
  ImGui::InputTextWithHint("Options (advanced)", room ? "e.g. --name \"Parking Lot\" (the row's name)" : "e.g. --height 1.05 --no-moves", g_options, sizeof g_options);
  Hint("Extra options the converter takes (each converter's --help lists them), e.g. --base <id> --height <scale> "
       "--no-moves for superstars. Usually none are needed.");
  if (std::wstring(kKinds[k].script) == L"moves_port.py") {  // the 2011 move it is built on
    ImGui::SetNextItemWidth(360 * g_scale);
    const std::string cur = g_template > 0 ? MoveName(g_template) + "  (" + std::to_string(g_template) + ")" : "automatic (the converter picks one of the same kind)";
    if (ImGui::BeginCombo("Template (a 2011 move)", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##tf", "find a 2011 move by name or number", g_template_find, sizeof g_template_find);
      if (ImGui::Selectable("automatic", g_template <= 0)) g_template = 0;
      const std::string f = Lower(g_template_find);
      int shown = 0;
      for (const auto& [id, m] : MoveInfos()) {
        if (!anims_page::AnyMotion(id)) continue;  // (only moves with motions can be templates)
        if (!f.empty() && Lower(m.name).find(f) == std::string::npos && std::to_string(id).find(f) == std::string::npos) continue;
        if (++shown > 300) { ImGui::TextDisabled("... type more to narrow it down"); break; }
        if (ImGui::Selectable((m.name + "  (" + std::to_string(id) + ")##t" + std::to_string(id)).c_str(), g_template == id)) g_template = id;
      }
      ImGui::EndCombo();
    }
    Hint("The 2011 move the ported move is built on: its table record (category, damage, where it can be done), its "
         "timing events and banks. Pick one of the same kind - a powerbomb for a powerbomb - when the automatic "
         "choice looks wrong (shown after converting).");
  }
  ImGui::BeginDisabled(Busy());
  PushAccent();
  if (ImGui::Button(("4. Convert " + (item["name"].Str().empty() ? ItemKey(item) : item["name"].Str())).c_str())) Convert(k, game, item);
  PopAccent();
  ImGui::EndDisabled();
  if (!g_result_error.empty()) ImGui::TextColored(kBad, "%s", g_result_error.c_str());
  if (g_result.type == Json::Type::kObject && g_result["ok"].Truthy()) {
    ImGui::SameLine();
    if (ImGui::Button(g_result["opens"].Str() == "belt" || g_result["opens"].Str() == "weapon" ? "Install the pack" :
                      g_result["opens"].Str() == "vs_style" ? "Install the style" : g_embedded ? "5. Use it" : "5. Open it to finish")) OpenResult();
    if (g_result["opens"].Str() == "superstar" && !g_embedded) {  // (or onto a superstar of the game: one of its attires)
      ImGui::SameLine();
      if (ImGui::Button("Or put it on a game superstar as an attire...")) {
        attire_page::UseStarMod(Wide(g_result["out"].Str()));
        GoTo(PageId::kAttire);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The Attires page with this superstar's model taken in: pick the game superstar, the attire\n"
                          "number and which of the ported attires (once per attire). Its entrance can come along; its\n"
                          "moveset, theme and ratings belong to a superstar, not to an attire, so they stay the game's.");
    }
    if (const int t = std::atoi(g_result["template"].Str().c_str()); t > 0)  // (moves: what it was built on)
      ImGui::TextDisabled("Built on the 2011 move %s (%d)%s; record %s. Not right? Pick a template above and convert again.",
                          MoveName(t).c_str(), t, g_template > 0 ? "" : ", picked automatically", g_result["id"].Str().c_str());
    if (g_result["opens"].Str() == "moves" && std::wstring(kKinds[k].script) != L"entrance_port.py")
      ImGui::TextColored(kWarn, "To use it in matches, put it in a superstar's moveset (the Move pack tab, or a superstar's Moveset section).");
    if (std::wstring(kKinds[k].script) == L"entrance_port.py" && std::atoi(g_result["number"].Str().c_str()) > 0) {
      // the ported entrance, played whole before it is installed: its own pack is looked in first
      const StarInfo* w = StarById(g_fp_who);
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::BeginCombo("##fpwho", w ? w->name.c_str() : "a superstar", ImGuiComboFlags_HeightLarge)) {
        for (const auto& st : Stars())
          if (ImGui::Selectable((st.name + "##fp" + std::to_string(st.id)).c_str(), st.id == g_fp_who)) g_fp_who = st.id;
        ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::Button("Full preview")) PortedEntrancePreview(item);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("The whole ported entrance in the practice arena, worn by the superstar picked here.");
    }
    for (const char* list : {"warnings", "dropped", "notes"}) {
      const Json& v = g_result[list];
      for (const auto& w : v.items)
        ImGui::TextColored(std::string(list) == "warnings" ? kWarn : ImVec4(0.65f, 0.65f, 0.7f, 1), "%s%s",
                           std::string(list) == "dropped" ? "left out: " : "", w.Str().c_str());
    }
    // a ported move: the inspector and "Will it work in-game?" before it goes in a moveset (move_check.h)
    if (g_result["opens"].Str() == "moves" && std::wstring(kKinds[k].script) == L"moves_port.py") {
      const int id = std::atoi(g_result["id"].Str().c_str());
      const fs::path pack = Wide(g_result["out"].Str());
      std::error_code ec;
      if (id > 0 && fs::exists(pack / L"pack.txt", ec)) move_check::DrawPanel(pack, id);
    }
  }
}


void TestStart(const std::string& spec) {
  std::vector<std::string> p;
  for (size_t at = 0; at <= spec.size();) {
    size_t c = spec.find(',', at);
    if (c == std::string::npos || p.size() == 3) c = spec.size();  // (the options may hold commas)
    p.push_back(spec.substr(at, c - at));
    at = c + 1;
  }
  if (p.size() < 3) return;
  if (const size_t at = p[0].find('@'); at != std::string::npos) g_test_game = p[0].substr(at + 1), p[0].resize(at);
  for (int k = 0; k < kKindCount; ++k)
    if (Lower(kKinds[k].title) == Lower(p[0]) || Utf8(kKinds[k].script) == p[0] + "_port.py") g_kind = g_force_kind = k;
  const std::wstring src = Wide(p[1]);
  std::error_code ec;
  SetSource(src);
  g_test_item = p[2];
  g_native_test = g_kind >= 0 && std::wstring(kKinds[g_kind].script) == L"arena_port.py" ? p[2] : "";
  if (p.size() > 3) std::snprintf(g_options, sizeof g_options, "%s", p[3].c_str());
  g_test_stage = 0;
}

void Draw() { DrawKinds(~0u); }

// The kinds in `mask` (bit k: kKinds[k]) on one of the "From another game" tabs (Superstar, Arena, Moves &
// entrances, Media): the disc row, those kinds' tabs, the shown kind's steps.
void DrawKinds(unsigned mask) {
  if (PythonExe().empty() || !PythonMissing().empty()) {
    ImGui::TextColored(kWarn, "The converters need Python 3 with numpy and Pillow: missing %s.", PythonMissing().c_str());
    if (ImGui::Button("Install what's missing...")) OpenToolsPanel();
    ImGui::SameLine();
    if (ImGui::SmallButton("Check again")) RecheckTools();
    return;
  }
  if (!(mask >> g_kind & 1)) {  // (the shown kind isn't one of these: the first that is)
    for (int k = 0; k < kKindCount; ++k)
      if (mask >> k & 1) {
        g_kind = k, g_result = Json(), g_result_error.clear();
        break;
      }
  }
  DiscRow();
  CacheRow();
  if (Source().empty()) return;
  ImGui::Spacing();
  int shown = 0;
  for (int k = 0; k < kKindCount; ++k) shown += mask >> k & 1;
  if (shown > 1 && ImGui::BeginTabBar("kinds")) {
    for (int k = 0; k < kKindCount; ++k)
      if ((mask >> k & 1) && ImGui::BeginTabItem(kKinds[k].title, nullptr, k == g_force_kind ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (g_force_kind < 0 || g_force_kind == k) {
          g_force_kind = -1;
          if (g_kind != k) g_kind = k, g_result = Json(), g_result_error.clear();
        }
        ImGui::EndTabItem();
      }
    ImGui::EndTabBar();
  }
  KindBody(g_kind);
}

int Kind() { return g_kind; }

void SuperstarFlow() { EmbeddedFlow(L"superstar_port.py"); }

void SuperstarFileFlow() {
  const bool lone = !g_file.empty() && CharacterFile();
  if (ImGui::Button(lone ? "Another character file..." : "Pick a character file...")) {
    const std::wstring f = PickOneFile();
    if (!f.empty()) SetSource(f);
  }
  if (!lone) {
    Hint("A chNNN.pac from any of your Xbox 360 WWE games (SvR 2007 to WWE 2K17) or a PS2 game's CH.PAC.");
    return;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", Utf8(fs::path(g_file).filename().wstring()).c_str());
  EmbeddedFlow(L"superstar_port.py", false);
}

void EmbeddedFlow(const wchar_t* script, bool with_disc_row) {
  if (PythonExe().empty() || !PythonMissing().empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kWarn, "Bringing things from older games needs Python 3 with numpy and Pillow: missing %s.", PythonMissing().c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Install what's missing...")) OpenToolsPanel();
    ImGui::SameLine();
    if (ImGui::SmallButton("Check again")) RecheckTools();
    return;
  }
  if (with_disc_row) DiscRow();
  if (Source().empty()) return;
  int k = 0;
  for (int i = 0; i < kKindCount; ++i)
    if (std::wstring(kKinds[i].script) == script) k = i;
  if (g_kind != k) g_kind = k, g_result = Json(), g_result_error.clear();
  g_embedded = true;
  KindBody(k);
  g_embedded = false;
}

void SetSource(const std::wstring& path) {
  if (path == CurrentSource()) return;
  g_image.clear(), g_folder.clear(), g_file.clear(), g_disc = -1;
  const auto& d = Discs();
  for (int i = 0; i < int(d.size()); ++i)
    if (d[size_t(i)].path == path) g_disc = i;
  std::error_code ec;
  if (g_disc < 0 && !path.empty())
    (fs::is_directory(path, ec) ? g_folder : Lower(Utf8(fs::path(path).extension().wstring())) == ".pac" ? g_file : g_image) = path;
  g_list = Json(), g_list_for.clear(), g_result = Json(), g_result_error.clear();
}
std::wstring CurrentSource() { return Source(); }

void Open(const wchar_t* script, const std::wstring& source) {
  for (int k = 0; k < kKindCount; ++k)
    if (std::wstring(kKinds[k].script) == script) g_kind = g_force_kind = k;
  SetSource(source);
  GoTo(PageId::kPort);  // (the app shows the kind's own "From another game" tab)
}

std::string KindsFor(const std::string& disc_id) {
  std::string out;
  for (int k = 0; k < kKindCount; ++k) {
    if (!HaveScript(k)) continue;
    if (!g_games.count(k)) {  // (asked one at a time, when nothing else runs)
      AskGames(k);
      continue;
    }
    for (const auto& g : g_games[k].items) {
      bool is = g["disc_hint"].Str() == disc_id;
      for (const auto& h : g["disc_hints"].items) is |= h.Str() == disc_id;
      const std::string st = g["status"].Str();
      if (is && (st == "ready" || st == "native")) {
        out += (out.empty() ? "" : ", ") + std::string(kKinds[k].title);
        break;
      }
    }
  }
  return out;
}

std::wstring EmbeddedResult() { return g_embedded_result; }
std::wstring EmbeddedPicture() { return g_embedded_picture; }
std::string EmbeddedName() { return g_embedded_name; }
void ClearEmbeddedResult() { g_embedded_result.clear(), g_embedded_picture.clear(), g_embedded_name.clear(); }

}  // namespace port_page
}  // namespace mm
