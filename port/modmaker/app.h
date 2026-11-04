// SvR2011 Mod Maker: what every page shares (app.cpp owns it).
//
// The app is one ImGui window: a title bar (project: new / open / save), a
// rail of pages on the left (Make: Arena, Backstage, Superstar, Moves, Crowd
// signs, Media; Look: Game assets, Animations, Icons & renders; Help), the
// page, and a status line with the log and the Problems list folding out.
//
// A page is a namespace with Draw(), Problems(), Reset() and the project
// hooks (StateText / WriteProject / ReadProject); app.cpp lists them in
// kPages. Pages never block: anything slow goes through RunInBackground and
// Progress; anything the user could get wrong is a Problem (an error keeps
// Save and Install off; a warning is shown with what to do).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shobjidl.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "svrfmt/texture.h"
#include "svrfmt/zip_write.h"

namespace mm {

namespace fs = std::filesystem;
using svrfmt::Bytes;
using svrfmt::Image;
using svrfmt::ZipEntry;

// ---------------------------------------------------------------- window, GPU

extern ID3D11Device* g_dev;
extern ID3D11DeviceContext* g_ctx;
extern HWND g_wnd;
extern std::wstring g_game;  // the installed game folder ("" = none)
extern float g_scale;        // UI scale (1 at 96 dpi with the 17 px font)
extern bool g_behind;        // --behind: a test run; the window is never shown, games it starts stay hidden too
// Opens a file, folder or link with the shell (Explorer, the browser) - never in a --behind test (logged instead),
// never an empty path.
void ShellOpen(const std::wstring& what);

// An RGBA picture as a shader resource (nullptr for an empty picture).
ID3D11ShaderResourceView* MakeTexture(const Image& img);
inline ImTextureID Tex(ID3D11ShaderResourceView* v) { return ImTextureID(reinterpret_cast<uintptr_t>(v)); }
// A texture let go of while the UI is being built is released after the frame is drawn and presented: this frame's
// draw list may still hold it (a picture box's thumbnail replaced by the picture just picked - report
// 20261102-050209: Intel's D3D11 driver crashed binding the freed view when the frame was drawn).
void ReleaseAfterFrame(IUnknown* v);
inline void Release(ID3D11ShaderResourceView*& v) { if (v) ReleaseAfterFrame(v), v = nullptr; }

// A picture kept with its GPU copy (the copy is made on first use, on the UI thread).
struct Picture {
  Image image;
  ID3D11ShaderResourceView* tex = nullptr;
  ~Picture() { Release(tex); }
  Picture() = default;
  Picture(const Picture& o) : image(o.image) {}
  Picture& operator=(const Picture& o) { image = o.image; Release(tex); return *this; }
  Picture(Picture&& o) noexcept : image(std::move(o.image)), tex(o.tex) { o.tex = nullptr; }
  Picture& operator=(Picture&& o) noexcept { image = std::move(o.image); Release(tex); tex = o.tex; o.tex = nullptr; return *this; }
  void Set(Image img) { image = std::move(img); Release(tex); }
  void Clear() { image = Image(); Release(tex); }
  bool Empty() const { return image.rgba.empty(); }
  ImTextureID Id() { if (!tex && !Empty()) tex = MakeTexture(image); return Tex(tex); }
  // Draws it, fitted into w x h (keeps the shape).
  void Draw(float w, float h);
};

// ---------------------------------------------------------------- strings, files

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
std::string PathStr(const fs::path& p);  // UTF-8
std::string Lower(std::string s);
std::string Upper(std::string s);
// A folder / file name from a title: letters and digits, '_' between words.
std::string IdFrom(const std::string& name, const char* fallback);
std::string FileName(const std::wstring& path);  // the last part, UTF-8
std::string Human(size_t bytes);                // "1.2 MB"
// "made_with=Mod Maker <version>\n": every manifest the Mod Maker writes carries it
// (the launcher flags mods without it as not made here).
std::string MadeWith();
// key=value text (manifests, projects): the value of a key ("" if absent).
std::string Value(const std::string& text, const char* key);
std::vector<std::string> Values(const std::string& text, const char* key);  // every line with that key

// Dialogs (on the UI thread).
// `start`: the folder the dialog opens in (a folder picker: its parent, with that folder picked; "" = Windows' choice).
std::wstring PickFile(bool save, const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec,
                      const wchar_t* def_ext = nullptr, const wchar_t* def_name = nullptr, const std::wstring& start = {});
std::vector<std::wstring> PickFiles(const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec);
std::wstring PickFolder(const wchar_t* title, const std::wstring& start = {});
// Another game's files: where they were last picked (a disc folder, a pac file), per game (a My game discs key; "" any
// game) - the From another game pickers open there (2.1.8 report 20261103-040523-d343a2: "doesn't default to the folder
// extracted"). OtherGameStart: that game's, else the last one picked, else the game's My game discs folder.
std::wstring OtherGameStart(const std::string& game = "");
void OtherGamePicked(const std::wstring& path, const std::string& game = "");
// Test aid --test-pick <path>: a hidden run's next file / folder dialog answers <path> (once).
void TestPick(const std::wstring& path);
extern const COMDLG_FILTERSPEC kPictureFilter[1], kSoundFilter[1], kBinkFilter[1], kPacFilter[1], kModFilter[1],
    kProjectFilter[1], kOpenFilter[1];
// A picture file -> Image (false logged).
bool LoadPicture(const std::wstring& file, Image& out);

// ---------------------------------------------------------------- log, status, work

void Log(const std::string& s);
void WriteLogTo(const std::wstring& file);  // the whole log into a file (test aids)
// The status line's message (also logged).
void Status(const std::string& s);
// One background job at a time; Busy() while it runs. The job may call
// Progress() to say what it is doing (shown in the status line).
void RunInBackground(std::function<void()> fn);
bool Busy();
// fn once no background job runs (a job's own done callback comes while it still counts as busy).
void WhenIdle(std::function<void()> fn);
void Progress(const std::string& what);
std::string ProgressNow();  // what the running job last said (a converter's "<n>%  <text>"; "" none)
// Runs fn on the UI thread before the next frame (from a background job).
void OnUiThread(std::function<void()> fn);

// ---------------------------------------------------------------- problems

// A one-click fix: a button that does the work (not an instruction).
struct FixAction {
  std::string label;            // "Use a 2011 entrance instead"
  std::function<void()> run;    // done on the UI thread; it may start a background job
  std::string done;             // what Simple's Fix it says it did ("" = the label)
};
// What's wrong, in plain words: what the player will see in the game and why - no ids, file keys or tool names.
struct Problem {
  bool error = true;   // false: a warning (shown, does not block)
  std::string text;    // what is wrong
  std::string fix;     // a short line on what the buttons do / what else to do (optional)
  int step = -1;      // (the Simple modes: the step that fixes it)
  std::vector<FixAction> fixes;  // the fix buttons; the first is the recommended one (Simple's Fix it applies it)
  bool note = false;             // nothing to fix: said so the player knows (no Fix it)
  Problem With(std::string label, std::function<void()> run, std::string done = "") && {
    fixes.push_back({std::move(label), std::move(run), std::move(done)});
    return std::move(*this);
  }
  Problem At(int s) && {
    step = s;
    return std::move(*this);
  }
  Problem Note() && {
    note = true;
    return std::move(*this);
  }
};
inline Problem Error(std::string text, std::string fix = "") { return {true, std::move(text), std::move(fix)}; }
inline Problem Warning(std::string text, std::string fix = "") { return {false, std::move(text), std::move(fix)}; }
int ErrorCount(const std::vector<Problem>& p);
// A problem's fix buttons on the current line (SameLine first); true when one was pressed (its run() done, said in
// the status bar).
bool FixButtons(const Problem& p, bool accent_first = true);
// Runs a fix and says so: "Done: <what>".
void RunFix(const FixAction& f);
int FixesDone();
// A fix that finished later (a tool it started): the pages check again, as after RunFix.
void FixChanged();
// Superstar Threads parts for a superstar mod folder (tools/threads_parts.py: <folder>\threads.txt from its model's
// colour masks). done(code, message) on the UI thread: 0 made, 3 the model has no colour masks, else failed.
// make_masks: a model without them gets them painted first (--make-masks; the model's old files kept in
// <dir>\.threads_backup).
void MakeThreadsParts(const std::filesystem::path& dir, std::function<void(int code, const std::string& message)> done,
                      bool make_masks = false);
// The folders threads_parts.py found no colour masks in (this session): the doctor says so instead of offering it.
bool ThreadsNoMasks(const std::filesystem::path& dir);
// Test aid --run-fix: tries the pending one on this list (logged); true when it ran or reported.
bool TestFixOn(const std::vector<Problem>& problems);

// ---- installing a mod's files (every page's Install)
// The files into <game>\Mods\<kind>\<id> (dir): written into a staging folder beside it (sub folders made), then
// swapped in for the old one. False with why: the real Windows error in plain words - access denied (read-only,
// Windows' Controlled folder access, an antivirus, OneDrive), a file in use (by which program; the Mod Maker's own
// preview is closed and it tries once more), a path too long, a full drive. May run on a worker thread.
bool InstallModFiles(const fs::path& dir, const std::vector<ZipEntry>& files, std::string* why);
// The programs that have a file under `dir` open (Windows' Restart Manager), "" none / unknown.
std::string WhoHolds(const fs::path& dir);  // (how many fixes ran: pages that cache problems check again when it changes)
// A Save / Install button row: disabled (with the errors as a tooltip) while
// the page has errors or a job runs. Returns which was pressed (0 none,
// 1 save as mod, 2 install, 3 install and start the game), after confirming
// warnings once.
int ModButtons(const std::vector<Problem>& problems, bool with_test, float width = 0);
// Draws the problems inline (a page's top).
void DrawProblems(const std::vector<Problem>& problems);

// ---------------------------------------------------------------- game data

struct ArenaInfo {
  const char* banner;  // texture name in MatchHD.pac (and manifest base=)
  int number;          // pac/bg/bgNN.pac
  const char* name;
  Picture tile;        // its select banner
};
extern ArenaInfo g_arenas[20];  // the arena select screen's order
std::string ArenaPath(int tile);  // UTF-8 path of the tile's pac
int ArenaTileOf(int number);      // tile index of bgNN (-1)

struct StarInfo {
  int id = 0;
  std::string name;
};
// The playable superstars (CHAR/DAT names, SSFA renders), sorted by name.
// Loaded on first use (quick).
const std::vector<StarInfo>& Stars();
const StarInfo* StarById(int id);
// The installed DLC's superstars (ids 51-58: a model pac pac\ch\ch<id>_dlcNN.pac in a package the game unpacked,
// <game>\UserData\0000000000000000\5451085D\00000002\<package>; names from its chEtc.pac, "(DLC)" added), sorted by
// name - attire mods can take them (the Attires page). DlcModelPacs: a superstar's model pacs in those packages (a
// DLC superstar's own; DLC attires of disc superstars, e.g. ch145_dlc02.pac). StarOrDlcById: either list.
const std::vector<StarInfo>& DlcStars();
std::vector<std::filesystem::path> DlcModelPacs(int id);
const StarInfo* StarOrDlcById(int id);
// A superstar's 7 ratings (record +0..+6), 74 if unknown.
int StarRating(int id, int k);
// A superstar's body scale (record +28, 4.12 fixed point: the game stretches the
// skeleton by it), 1 if unknown.
float StarScale(int id);
// The base's select render (DLC_HD SSFA), decoded on first use (cached).
Picture* StarRender(int id);
// A pac's table without reading the whole file: EPAC (12-byte entries,
// 4-char names) and EPK8 (16-byte entries, 8-char names).
struct PacEntryInfo {
  std::string group, name;
  uint64_t offset = 0;
  uint32_t size = 0;
};
struct PacIndex {
  bool epk8 = false;
  std::vector<PacEntryInfo> entries;
};
bool ReadPacIndex(const fs::path& file, PacIndex& out);
// The same from the file's first 0x4000 bytes (in memory / from a disc).
bool ParsePacIndex(const Bytes& head, PacIndex& out);
// One entry's data (by group + name, or the first entry with that name when
// group is nullptr); false if absent.
bool ReadPacEntry(const fs::path& file, const char* name, Bytes& out, const char* group = nullptr);
bool ReadPacEntry(const fs::path& file, const PacEntryInfo& e, Bytes& out);

// The game's strings (string.pac, English): id -> text ("" if none), loaded on
// first use. The game's own router: ids >= 50000 table 1, >= 45000 table 2,
// >= 40000 table 0, else the screen tables 3..13 then 0.
const std::string& GameString(int id);

// The game's move names (misc.pac MOVS/WAZE: 160-byte records, the name at
// +0x10, the id at +0x90), loaded on first use; "" if unknown.
const std::string& MoveName(int id);
const std::map<int, std::string>& MoveNames();
// The entrances the game can play (checklist D5): the stock ones (pac\evt\
// Nyujyo*.pac, EVP scripts, name at +0x22) and those installed move packs add
// (pack.txt "pacentry <pac> EVPx <number> <file>" in Mods\Moves\* and in
// superstar mods' moves\). Read in the background: empty until EntrancesReady().
struct EntranceInfo {
  int number = 0;
  std::string name;
  std::string from;  // "" stock, else the pack folder's name
};
const std::vector<EntranceInfo>& Entrances();
bool EntrancesReady();
void ReloadEntrances();
// A pack folder's entrances (its pack.txt pacentry EVP lines).
std::vector<EntranceInfo> PackEntrances(const fs::path& pack);
// The victory scenes a superstar can have (manifest victory=, docs/VICTORY_SCENES.md): the game's own singles ones
// (pac\evt\Winning*.pac: 6000 + id for the superstars who have one, 7000-7107 the generic singles and divas ones)
// and installed packs' EVP scripts numbered 6000-7999.
const std::vector<EntranceInfo>& Victories();
constexpr bool IsVictoryNumber(int n) { return n >= 6000 && n < 8000; }
// A song's integrated loudness in LUFS (BS.1770; loudness.cpp). Slow: call it from a worker.
bool MeasureLufs(const std::wstring& file, double* lufs, std::string* error);
constexpr double kThemeTargetLufs = -24.0;  // where a mod's theme sits next to the game's own
// The move-set (checklist D4). A profile's move block is 224 u16 move ids
// (CHAR/PRO +0x000..+0x1BF); a slot takes the moves whose WAZE category bits
// include the slot's rule (the bits >= 90% of the roster's moves in that slot
// share), and for the seated-top corner slots the same starting situation.
constexpr int kMoveSlots = 224;
struct MoveInfo {
  int id = 0;
  std::string name;
  uint8_t bits[16] = {};
  int situation = 0;
};
const std::map<int, MoveInfo>& MoveInfos();
// A superstar's 224 slots (empty if it has no profile).
std::vector<int> ProfileMoves(int star_id);
// The material panel (material_panel.cpp): the meshes of `pac`'s attire `attire_in_pac` (gear: its entrance gear
// model) with their shine / normal map / colour / see-through / both sides; edits in `edits` keyed by the 3D view's
// attire (view_attire), previewed live. Simple's plain words with `simple`. -> true when an edit changed.
bool MaterialPanel(const std::wstring& pac, int attire_in_pac, bool gear, int view_attire,
                   std::map<std::tuple<int, bool, uint32_t, int>, struct MatEdit>& edits, bool simple);
// test aid (--mat-select): "all" or "<model id>:<mesh>/..." picked in the panel, then ":spec=..,tint=r/g/b,cut=,both=,flip="
// applied to all of them as the panel's "all picked" row does; MaterialTestApply: the same without the panel drawn
// (a test save right at start), true when it ran
void MaterialTestSelect(const std::string& spec);
bool MaterialTestApply(const std::wstring& pac, int attire_in_pac, bool gear, int view_attire,
                       std::map<std::tuple<int, bool, uint32_t, int>, struct MatEdit>& edits);
// A superstar's entrance theme: the game's music id (profile +0x1C0 +0x10; 1-253 the game's own themes), 0 if none.
int ProfileMusic(int star_id);
// Whether a move fits a slot (unknown moves: false); the moves that do, by name.
bool MoveFitsSlot(int slot, int move_id);
const std::vector<int>& MovesForSlot(int slot);
bool GameRunning();
void StartGame(const std::wstring& extra_env = L"");

// ---------------------------------------------------------------- project

// What a page saves into a project (project.txt lines and files) and reads
// back. Files inside the project zip are named by the page ("star/ch.pac").
struct ProjectOut {
  std::string text;                 // key=value lines
  bool keys_only = false;           // (a mod's .mm_project.txt: the keys, no files - ModProjectText)
  std::vector<ZipEntry> files;
  // slow work (an arena's compression) done in the background before the zip is written
  std::vector<std::function<void(std::vector<ZipEntry>&)>> deferred;
  void Key(const std::string& k, const std::string& v) { if (!v.empty()) text += k + "=" + v + "\n"; }
  void Key(const std::string& k, int v) { text += k + "=" + std::to_string(v) + "\n"; }
  // Copies the file into the project (false logged); returns its name there.
  std::string File(const std::string& key, const std::wstring& path, const std::string& as);
  void Png(const std::string& key, const Image& img, const std::string& as);
};
struct ProjectIn {
  std::string text;
  std::vector<ZipEntry> files;
  fs::path dir;  // the project file's folder (for relative paths)
  std::string Get(const char* key) const { return Value(text, key); }
  int GetInt(const char* key, int def) const { const auto v = Get(key); return v.empty() ? def : std::atoi(v.c_str()); }
  const ZipEntry* Find(const std::string& name) const;
  // A file of the project written next to it in the project's cache folder
  // (so pages can keep using paths); "" if absent.
  std::wstring Extract(const std::string& name) const;
  bool Png(const char* key, Image& out) const;
};

// The project's type is its page. Pages the rail shows.
enum class PageId { kArena, kEditor, kBackstage, kStar, kMoves, kSigns, kBackgrounds, kMedia, kMatch, kOther, kCaw, kAssets, kAnims, kIcons, kHelp, kPort, kAttire, kInstalled, kDiscs, kPortEntrance, kPortMoves, kPortTaunts,
                    kStarPort, kArenaPort, kMovesPort, kBelts, kThreads, kTitle, kCount };  // (the last four: "From another game" / belt tabs)
// What only a project holds (materials, the FBX steps, Reduce detail, gear picks, an arena's camera...) for a mod:
// the page's project keys without its files, every absolute path cut to its file name (nothing of the author's
// folders goes out with a shared mod). UI thread (it reads the page). "" for a page with no project keys.
std::string ModProjectText(PageId page);
// ... into the mod's files as ".mm_project.txt" (a dot file: the game, the bundled rules and uploads leave it out);
// opening the mod (read_mod) takes back the keys its manifest lacks.
void AddModProject(std::vector<ZipEntry>& files, const std::string& text);

extern PageId g_page;
void GoTo(PageId p);
const char* PageName(PageId p);
// A page by its test-aid name (--page-name / --page-at: arena, editor, ... old names too); false: none.
bool PageByName(const std::wstring& name, PageId& page);
// The mod-type pages' project hooks.
struct PageHooks {
  const char* type;  // manifest type= ("" = not a mod page)
  std::function<void()> draw;
  std::function<void(std::vector<Problem>&)> problems;
  std::function<void()> reset;
  std::function<std::string()> state;             // text that changes with every edit (dirty check)
  std::function<void(ProjectOut&)> write;
  std::function<bool(const ProjectIn&)> read;
  std::function<bool(const ProjectIn&)> read_mod;  // a .svrmod of this type back into the page
};
// Marks the project changed (pages whose state isn't all in state()).
void Touch();

// The current project: file and type page.
extern std::wstring g_project_file;  // "" = unsaved
extern PageId g_project_page;
bool ProjectDirty();
void ProjectNew(PageId type);
bool ProjectSave(bool ask_name);
bool ProjectOpen(const std::wstring& file);
// An installed mod's folder (Mods\<type>\<folder>) opened as a project, as its .svrmod would be (the folder is only read).
bool ProjectOpenFolder(const std::wstring& dir);  // .svrproj or .svrmod
void ProjectReset();

// ---------------------------------------------------------------- settings

struct Settings {
  std::wstring game;
  std::vector<std::wstring> recent;  // projects, newest first
  int win_x = -1, win_y = -1, win_w = 0, win_h = 0;
  bool maximized = false;
  bool log_open = false;
  int font = 17;
  std::vector<std::wstring> disc_folders;  // where the disc images are ("" list = the defaults)
  std::wstring bink2_tool;                 // radvideo64.exe / a Bink 2 ffmpeg the player chose ("" = look for one)
  // My game discs (disc_list.h): a game's disc image or extracted folder, and the disc id read from it
  struct MyDisc {
    std::string key, id;
    std::wstring path;
  };
  std::vector<MyDisc> my_discs;
  bool discs_seeded = false;  // the old disc folders were searched once to fill My game discs
  bool keep_bik = false;      // mods keep a movie's Bink beside its .mp4 (the game's fallback where MP4 can't play)
  bool moves_warning_off = false;  // "Don't show this again" on the move-converting warning
  std::vector<std::pair<std::string, std::wstring>> other_game;  // OtherGamePicked: game key ("" any) -> the path
};
extern Settings g_settings;
void SaveSettings();
void AddRecent(const std::wstring& file);

// ---------------------------------------------------------------- disc images (checklist D1)

// The user's other WWE discs: My game discs (disc_list.h), each game's disc image or extracted folder that is there.
// The first time, the old disc folders (default D:\Roms\Xbox 360\WWE and D:\Roms\Playstation 2\WWE Games) are
// searched to fill it.
struct DiscRef {
  std::wstring path;
  std::string id;    // XEX title id / PS2 serial
  std::string game;  // the game's name (by id), else the file name
  bool xbox = false;
};
// Empty until DiscsReady() (only that first search takes a while).
const std::vector<DiscRef>& Discs();
bool DiscsReady();
void RescanDiscs();
std::vector<std::wstring> DiscFolders();
// "SmackDown vs. Raw 2008 (Xbox 360)" for a title id / serial ("" if unknown).
std::string DiscGameName(const std::string& id);

// ---------------------------------------------------------------- widgets

// A labelled file row: button opens the picker, shows the file name, 'x'
// clears. Returns true when changed.
bool FileRow(const char* label, std::wstring& file, const char* none, const COMDLG_FILTERSPEC* spec, UINT nspec,
             const char* tip = nullptr, float width = 220);
// A labelled text field (label on the left).
bool TextField(const char* label, char* buf, size_t size, const char* hint = nullptr, float width = 320);
void Hint(const char* text);  // (?) with a tooltip
// Files dropped on the window from Explorer: true (with the files) when they were dropped on the last ImGui item this
// frame - a drop target for that item (a drop nobody takes is forgotten after a frame). Test aid: the --ui-script
// event "drop <x> <y> <file>".
bool DroppedOnLastItem(std::vector<std::wstring>& files);
void Heading(const char* title, const char* about);
void PushAccent();  // the red main-action button colours
void PopAccent();
const ImVec4 kAccent{0.78f, 0.06f, 0.18f, 1};
const ImVec4 kGood{0.12f, 0.42f, 0.25f, 1};
const ImVec4 kWarn{1, 0.75f, 0.3f, 1};
const ImVec4 kBad{1, 0.35f, 0.35f, 1};

// ---------------------------------------------------------------- pages

namespace arena_page { extern PageHooks hooks; void Tick(); void OpenEditor(int tile); void OpenModFile(const std::wstring& f);
                       void SetVsTest(const std::string& spec);  // (--vs-test: the VS screen tab's steps, arena_page.cpp)
                       void TestInGame(); void SetTestSave(const std::wstring& f, int lib); void StartNew(int tile);
                       void StartBackstage(int area);
                       // a converted arena.pac as the arena project (host tile, name), into the 3D editor
                       // picture: a PNG made the banner (256 x 128) and loading picture (1024 x 512), "" none
                       void SetArenaFromPac(const Bytes& pac, int host, const std::string& name, const std::string& source = "",
                                            bool to_editor = true, const std::wstring& picture = L"");
                       // Simple arena (simple_arena.cpp): the Arena page's step-by-step mode at <step> (test aid)
                       void TestSimple(int step); void TestSimpleStart(const std::string& spec);
                       // a converted bg78 as a backstage project: area, name, and its own-area gimmick pac (or "")
                       void SetBackstageFromPac(const Bytes& pac, int area, const std::string& name,
                                                const std::wstring& gimmick_pac, const std::string& gimmick); }
namespace backstage_page { extern PageHooks hooks; }
namespace star_page { void TestMaterials(const std::string& spec, bool open);
extern PageHooks hooks; std::string SlotName(int slot); void ShowPose(int motion); void TestStart(int id, const std::wstring& model, const std::string& name,
                      const std::wstring& picture, const std::wstring& save);
                      void SetModel(const std::wstring& ch_pac);  // (a converted model)
                      void TestFiles(const std::wstring& song, const std::wstring& movie, const std::wstring& voice, int call);
                      void TestHeight(float scale); void TestCallGain(float db); void TestMoveset(const std::string& spec); void ThreadsDraw(); void ThreadsTest(const std::string& spec); void TestCae(const std::string& id); void TestImportMoveset(const std::string& spec); void TestEntrance(int n); void TestVictory(int n); void TestVictoryPort(const std::string& spec); void TestInstall(); void TestWatchSlot(int off); void TestFull(const std::wstring& png); void TestLoad(const std::wstring& png); void TestHometown(const std::string& h); void TestPose(int id);
                      void TestFbx(const std::wstring& fbx, const std::string& map); void TestSimple(int step); void TestSimpleFrom(int step); void TestSimpleChain(const std::string& spec);
                      // entrance gear from another superstar (tools/svr11_build.py --gear-from / --onto): the game superstars
                      // with gear, a popup to pick one, and the transplant onto <target pac>'s attire (done(new pac) on success)
                      const std::vector<int>& GearDonors(); bool GearDonorPopup(const char* id, int& picked);
                      void GearTransplant(const std::wstring& target, int attire, int donor, std::function<void(const std::wstring&)> done); void TestGear(const std::string& spec); void TestGearFrom(const std::string& spec); void TestGearFbx(const std::string& spec); void TestSimpleTake(const std::wstring& svrmod, bool everything); void TestSimpleLook(int mode); void TestLookTab(int tab); void TestFbxStep(int step); void TestSimpleCapture(const std::wstring& png); void TestSimpleRing(int n); void TestFbxMake(); void TestFbxReport(const std::wstring& out);  // FBX import mapping (map: fbx_map= text)
                      // the ring announcer's superstar name clips (tools/announcer_calls.json) and the clip picker
                      const std::vector<std::string>& AnnouncerNames();
                      bool PickClip(const char* label, char* buf, size_t size, const std::vector<std::string>& clips, const char* none, float width);
                      bool PickEntrance(const char* label, int& value, const char* none, float width = 320);
                      bool EntranceOutfit(const std::wstring& pac); }
namespace attire_page { extern PageHooks hooks; void TestPacAttires(const std::wstring& spec); void TestStart(int star, const std::wstring& model, int slot, const std::wstring& save); void TestKeepEntrance(); void TestFrom(int from, const std::string& name);
                       // a superstar mod (a converted one: the From another game result) taken in as the model of a new attire
                       void UseStarMod(const std::wstring& src); }
namespace signs_page { extern PageHooks hooks; void TestStart(const std::vector<std::wstring>& files, const std::wstring& save);
                       Image SignPicture(const Image& src);
                       // a sign pack's files (manifest.txt + NN_<name>.dds) from 128 x 64 signs; installed into
                       // <game>\Mods\Signs\<id> (false: written nothing / logged)
                       std::vector<ZipEntry> PackFiles(const std::string& id, const std::string& name, const std::string& author,
                                                       const std::vector<std::pair<std::string, Image>>& signs);
                       bool InstallPack(const std::string& id, const std::vector<ZipEntry>& files); }
namespace title_page { extern PageHooks hooks; void TestStart(const std::wstring& spec, const std::wstring& save); }
namespace backgrounds_page { extern PageHooks hooks; void TestStart(const std::vector<std::wstring>& files, const std::wstring& save); }
namespace media_page { extern PageHooks hooks; void SetTab(int tab); int Tab();  // 0 videos, 1 menus & renders, 2 audio
                       void TestStart(int arena, const std::wstring& video,
                       const std::vector<std::wstring>& pictures, const std::wstring& save); }
namespace moves_page { extern PageHooks hooks; void OpenPack(const std::wstring& folder); void TestPut(const std::wstring& spec); void TestOpen(const std::wstring& folder); void TestSave(const std::wstring& file); }
namespace caw_page { void Draw(); void TestStart(int index, int attire, const std::wstring& picture, bool save); void TestAnnouncer(const std::string& clip); }
namespace other_page { void Draw();
                       // only the tabs in mask, for the "From another game" tabs
                       enum : unsigned { kW13Arena = 1, kStarFile = 2, kW13Star = 4, kSvr10Star = 8, kSvr10Moves = 16, kSvr08 = 32 };
                       void DrawTabs(unsigned mask);
                       void TestConvertW13(const std::wstring& file, int host);
                       // a WWE '12 - 2K17 bgNN.pac onto host tile `host` with the tab's options (svrfmt/wwe13), in the
                       // background, then the result as the Arena page's project (to_editor: the 3D editor shown)
                       // picture: the banner + loading picture (a PNG, e.g. the Port page's scan of the disc), "" none
                       void ConvertArena(const std::wstring& file, int host, const std::string& name, bool to_editor,
                                         const std::wstring& picture = L"");
                       // an arena pac (WWE '12 / '13 / 2K14 bgNN.pac) into the built-in converter's tab, shown next;
                       // picture: its banner + loading picture (the disc's arena picture from the scan), "" none
                       void UseArenaFile(const std::wstring& file, const std::string& name, const std::wstring& picture = L""); }
namespace match_page { extern PageHooks hooks; void TestTab(int tab, int rule); }
namespace port_page { void Draw();
                      // only the kinds in mask (bit k: Superstars, Moves, Entrances, Arenas, Attires, Belts, Weapons,
                      // Backstage), for the pages' "From another game" tabs; Kind(): the shown kind's bit number
                      enum : unsigned { kSuperstars = 1, kMovesKind = 2, kEntrances = 4, kArenas = 8, kAttires = 16, kBelts = 32,
                                        kWeapons = 64, kBackstageKind = 128, kVsScreens = 256 };
                      void DrawKinds(unsigned mask); int Kind();
                      void TestAsAttire(const std::string& spec);  // (--port-as-attire)
                      void TestStart(const std::string& spec); void TestClear(); void TestTrim(); void TestFullPreview();
                      // the superstar steps alone (Simple mode): a converted .svrmod comes back in EmbeddedResult()
                      void SuperstarFlow(); std::wstring EmbeddedResult(); void ClearEmbeddedResult();
                      // with an arena EmbeddedResult(): the disc's picture of it (a PNG from the scan), "" none
                      std::wstring EmbeddedPicture();
                      // with an arena EmbeddedResult(): the game's own name of it (from the scan, e.g. "Raw"), "" none
                      std::string EmbeddedName();
                      // one character file instead of a disc, embedded: a "Pick a character file" button, then the
                      // lone file's steps (game, character, convert); the .svrmod comes back in EmbeddedResult()
                      void SuperstarFileFlow();
                      // Any kind's steps embedded in another page (Simple modes): `script` is the converter
                      // (L"arena_port.py" ...); with_disc_row: the disc picker first. The result comes back in
                      // EmbeddedResult(): a converted .svrmod, a result folder (moves, entrances), or for WWE '12 - 2K17
                      // arenas the bgNN.pac copied off the disc (to convert with other_page::ConvertArena).
                      void EmbeddedFlow(const wchar_t* script, bool with_disc_row = true);
                      // The disc (image or folder) the steps read; "" none.
                      void SetSource(const std::wstring& path); std::wstring CurrentSource();
                      // The Port page on a kind (its tab) and a disc.
                      void Open(const wchar_t* script, const std::wstring& source);
                      // The converter kinds (tab titles) whose `games` know this disc id as ready / native, asked
                      // in the background one converter at a time ("" until known).
                      std::string KindsFor(const std::string& disc_id); }
namespace assets_page { void Draw(); void TestOpen(const std::string& path); void TestSource(const std::wstring& source); }
namespace pac_editor {  // a copy of any pac, edited and saved elsewhere (Game assets)
bool Open(svrfmt::Bytes pac, const std::string& label, const std::wstring& from, const std::string& note);
bool IsOpen();
void Close();
void Draw();
void TestEdit(const std::wstring& spec);
}
namespace anims_page {
// A pack's motions for one move as a bank the 3D view plays (pack.txt `motion` lines: every phase and track of it,
// under the target bank's header) - nullptr when they don't read.
std::shared_ptr<const std::vector<uint8_t>> PackBank(const std::filesystem::path& pack, int id);
// Move `id` from `bank` played in the 3D view as the game plays it (char_preview::SetMove: its phases, both people,
// the follow-ups read from m.pac); alt >= 0: that other ending too.
bool PlayMove(std::shared_ptr<const std::vector<uint8_t>> bank, int id, int alt, std::string* err = nullptr);
void TestStrip(const std::wstring& spec);  // (--watch-strip <png>,<frames>)
bool AnyMotion(int id); void TestKey(int key); void TestCompare(const std::wstring& spec); bool MovesIndexed(); bool HasMotion(int id); void WatchMove(int id); void TestWatch(int star_id, int dummy_id, int move); void Draw(); void TestStart(int star_id, int bank, int motion, int dummy_id); void TestPlay(int star_id, int dummy_id, int move); }
namespace icons_page { void Draw(); void TestTab(int tab); }
namespace installed_page { void Draw(); void TestSelect(const std::string& spec); void RestoreView(); }
namespace help_page { void Draw(); }

// A superstar model (ch.pac) to FBX, picked folder, in the background (svrfmt/char_fbx): <folder>\<name>.fbx + textures.
void ExportModelFbx(const std::wstring& pac, int attire, const std::string& name);
// A character model pac's match memory (D12): per wrestler attire entry (EMD ...2) the 4 KB pages its meshes'
// vertex and index buffers take - the game gives each wrestler 192; over that the match load crashes.
// A wrestler's vertex memory (tools/model_budget.py): 1600 pages since 2.1.6 (the game's big-model memory), 192 before
// - a model over that needs kNeedsVersion (its manifest says so; older games leave it out). Six 1,489-page superstars
// in a 6-man play at 60 fps at 1080p, about 40 at 4x supersampling with 2x AA (the warning's text says so).
constexpr int kModelPages = 1600, kModelPagesWarn = 1530, kModelPagesOld = 192;
constexpr int kModelPagesHeavy = 1024;  // (over it: the frame-rate note - the old limit, six of which keep 60 fps anywhere)
constexpr const char* kNeedsVersion = "2.1.6";
struct ModelPages {
  std::string entry;  // the EMD entry ("00013902": attire 1)
  int pages = 0;
  int blood = 0;  // of them, the blood overlays' (JBOY children 0x7530-0x7532: the game drops them first when over)
};
const std::vector<ModelPages>& ModelBudget(const std::wstring& pac);  // (cached by file time / size)
// The entrance gear entries' pages (EMD kind 4: "<id><attire-1>4"): it loads instead of the attire's model for the
// entrance (stock Undertaker's gear + attire together are ~370 pages and play), so each counts against 192 alone.
const std::vector<ModelPages>& GearBudget(const std::wstring& pac);
// The attires a model pac holds (its kind-2 EMD entries %06d%02d: attire*10 + 2), 1-based, in order. The game
// uses a superstar mod's in-pac attires contiguous from 1, at most 4.
std::vector<int> ModelAttires(const std::wstring& pac);
// The problem line for a model over (error) or near (warning) the budget; who: "The model", "Attire 2" ...
// D12 model checks (CheckCharacterModel; robert32's SvR Port Doctor findings): NaN values, eyes off their bones.
void ModelCheckProblems(const std::wstring& pac, const std::string& who, std::vector<Problem>& out);
// use (optional): a fix that swaps in a lighter copy of the model (without its blood) - it gets the copy's path.
void BudgetProblems(const std::wstring& pac, const std::string& who, bool first_only, std::vector<Problem>& out,
                    std::function<void(const std::wstring&)> use = nullptr);

// The superstar picture -> the select renders (512 render, 256 bust, 64 icon).
void MakeRenders(const Image& src, Image& render, Image& bust, Image& icon);
// A picture framed the game's way on a size x size canvas: a cut-out (transparent background) is cropped to the
// figure and fills the height, centred (the stock renders: top to bottom, ~85% wide); a picture without
// transparency is fitted whole, top aligned.
Image FitFigure(const Image& src, int size);
// A NOW LOADING picture (load.dds: exactly 1024 x 512): a wide picture fills it; a square or tall one sits whole on
// the right half (centred when wider) over a blurred, darkened copy of itself covering the screen.
Image FitLoading(const Image& src);
// The figure's tone half way to the stock renders' median (brightness 0.39, contrast 0.21, saturation 0.41).
Image GameLook(const Image& src);

// A video or picture -> a 320 x 320 Bink entrance movie (the launcher's movie
// maker), in the background; done(out or "" on failure) on the UI thread.
// fit: 0 whole picture with bars, 1 fill, 2 stretch; seconds: 0 = all.
// The movie made from `video` (movie_make: the titantron layout) as `out` (a .bik), then that as an .mp4 beside it (same
// name): done() gets the .mp4 ("" failed; the .bik when no .mp4 could be made). Both stay in the work folder.
void MakeBink(const std::wstring& video, const std::wstring& out, int fit, int seconds, std::function<void(std::wstring)> done);
// A Bink movie's .mp4 in the work folder (<game>\Mods\.convert\movies, one per file: path + size + time), made now when
// missing (blocks: a worker thread); "" + why when it can't be. CachedMp4: the same without making it ("" not yet).
std::wstring ShrunkMovie(const std::wstring& bik, std::string* why = nullptr);
std::wstring CachedMp4(const std::wstring& bik);
// The movie files a mod carries for `src` (.bik / .bk2 / .mp4) as <stem>.mp4 (a Bink shrunk to one) and, with
// g_settings.keep_bik, the Bink as <stem>.bik (the game's fallback); a Bink that can't be shrunk goes as it is.
// Returns the manifest's movie value ("" + why: nothing usable).
std::string ModMovieFiles(const std::wstring& src, const std::string& stem, std::vector<ZipEntry>& files, std::string* why);
// "12.3 MB"
std::string SizeText(uint64_t bytes);
// What a project keeps for a movie slot: its .mp4 (a Bink's, once made) else the file as it is.
std::wstring ProjectMovie(const std::wstring& movie);
// The movie slot's line: its size, and as an .mp4 (made in the background when it is a Bink), plus the keep-Bink option.
void MovieSizeRow(const std::wstring& movie);
// A "From a video..." button that makes the Bink into <game>\Mods\.convert and sets `movie`.
bool BinkFromVideoButton(const char* label, std::wstring& movie, const std::string& stem);
// The footer under every superstar 3D view: Reset view, Blood (off by default) and its Damage level (the game's
// light / medium / heavy blood), the mouse hint. One state for all views.
void ViewFooter();
// WWE '12 - 2K17 "bgNN" pacs from 60 up aren't arenas (docs/ARENA_PORT_RESULTS.md, special stages): backstage rooms
// (bg67, bg78 - which crashes on an arena slot) and studio / photo stages (a white mat over a black void). The
// number of a "...bgNN..." file / source name (-1: none), and whether it is one of those.
int ArenaPacNumber(const std::string& name);
inline bool SpecialStage(const std::string& name) { return ArenaPacNumber(name) >= 60; }

}  // namespace mm
