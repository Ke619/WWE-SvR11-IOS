// Other games page: content from the other SmackDown games brought over, the
// way the bundled mods were made.
//   WWE '13 arena      svrfmt/wwe13 (built in): a WWE '13 bgNN.pac onto a 2011
//                      host arena -> the Arena page (then banner, name, save)
//   WWE '13 superstar  xdec (built in) + tools/w13_char.py -> a ch.pac for the
//                      Superstar page
//   SvR 2010 superstar tools/svr10_superstar.py -> a whole .svrmod, opened on
//                      the Superstar page
//   SvR 2010 moves     tools/svr10_moves.py build + movepack.py -> a move pack
//                      folder on the Moves page
//   SvR 2008 backstage tools/svr08_stage.py + svr08_gimmick.py -> the Parking
//                      Lot stage and its cars on the Backstage page
// The Python tools ship in "Mod Maker Tools" next to the Mod Maker and need
// Python 3 with numpy and Pillow (ffmpeg too for the 2010 themes).
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "disc_list.h"
#include "svrfmt/pac.h"
#include "svrfmt/wwe13.h"
#include "tool_run.h"

#include <shellapi.h>

namespace mm {
namespace other_page {

using namespace svrfmt;

namespace {

// ---- where the other games' files are (remembered in the settings? kept for the session)
std::wstring g_w13_pac, g_w10_pac, g_w08_pac;

// A game's pac folder: its extracted folder in My game discs first, else the old extract next to the port.
std::wstring Guess(const char* game) {
  std::error_code ec;
  const std::wstring mine = disc_list::PathFor(game);
  if (!mine.empty() && fs::is_directory(fs::path(mine) / L"pac", ec)) return (fs::path(mine) / L"pac").wstring();
  return L"";
}

// Under a folder row: where My game discs has that game (a disc image the tools here can't read: the Port page can).
void MyDiscNote(const char* game) {
  const std::wstring mine = disc_list::PathFor(game);
  std::error_code ec;
  if (mine.empty() || fs::is_directory(mine, ec)) return;
  ImGui::TextDisabled("Your %s disc image is in My game discs: the From another game tab reads it as it is (Superstars, Moves, ...).",
                      disc_list::GameByKey(game) ? disc_list::GameByKey(game)->name : game);
}

void FolderRow(const char* label, std::wstring& folder, const wchar_t* title) {
  ImGui::PushID(label);
  if (ImGui::Button(label, ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFolder(title);
    if (!f.empty()) folder = f;
  }
  ImGui::SameLine();
  if (folder.empty()) ImGui::TextDisabled("not set");
  else ImGui::TextUnformatted(Utf8(folder).c_str());
  ImGui::PopID();
}

bool ToolsReady(bool ffmpeg_too) {
  const std::string missing = PythonMissing();
  if (!missing.empty()) {
    ImGui::TextColored(kWarn, "Needs %s.", missing.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Install what's missing...")) OpenToolsPanel();
    ImGui::SameLine();
    if (ImGui::SmallButton("Recheck")) RecheckTools();
    return false;
  }
  if (ToolsDir().empty()) {
    ImGui::TextColored(kWarn, "The converter scripts were not found (a \"Mod Maker Tools\" folder next to the Mod Maker).");
    ImGui::SameLine();
    if (ImGui::SmallButton("Recheck")) RecheckTools();
    return false;
  }
  if (ffmpeg_too && FfmpegExe().empty()) {
    ImGui::TextColored(kWarn, "ffmpeg is needed too for the theme songs.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Install it...")) OpenToolsPanel();
    return false;
  }
  return true;
}

std::wstring Pac11() { return (fs::path(g_game) / L"pac").wstring(); }

// ---- WWE '13 arena (built in)

std::wstring g_w13_file;
int g_w13_host = 1;  // tile (RAW: the bundled ones used it)
Wwe13Options g_w13;
char g_w13_drop[128] = "";
char g_w13_name[64] = "";
std::wstring g_w13_picture;  // the banner + loading picture (the Port page's scan of the disc, or picked), "" none
bool g_show_arena_tab = false;  // (the Port page handed a file: that tab comes up)

// A WWE '12 - 2K17 arena pac onto a host tile, in the background; the result is the Arena page's project.
void Convert(const std::wstring& file, int host, const std::string& name, const Wwe13Options& opt, bool to_editor,
             const std::wstring& picture) {
  std::string source = Utf8(fs::path(file).stem().wstring());  // ("Arenas_wwe2k16_bg01" from the Port page, or "bg01")
  if (source.rfind("Arenas_", 0) == 0) source = source.substr(7);
  std::replace(source.begin(), source.end(), '_', ' ');
  RunInBackground([file, host, opt, name, source, to_editor, picture] {
    Progress("Reading the arena ...");
    Bytes w13, hostpac;
    if (!ReadFile(Utf8(file), w13)) { Status("Could not read " + Utf8(file)); return; }
    if (!ReadFile(ArenaPath(host), hostpac)) { Status("Could not read the host arena."); return; }
    Progress("Converting (a minute or so) ...");
    Bytes out;
    Wwe13Report rep;
    std::string err;
    if (!BuildArenaFromWwe13(w13, hostpac, opt, out, rep, &err)) { Status("The conversion failed: " + err); return; }
    for (const auto& n : rep.notes) Log("  " + n);
    if (!rep.halved.empty()) Log("  " + std::to_string(rep.halved.size()) + " pictures halved to fit");
    char b[200];
    std::snprintf(b, sizeof b, "Converted: %d models, %d meshes, %d entries (%d from the host, %d dropped, %d ring parts).",
                  rep.conv.models, rep.conv.meshes, rep.entries, rep.from_host, rep.dropped, rep.ring_parts);
    Status(b);
    OnUiThread([out, host, name, source, to_editor, picture] { arena_page::SetArenaFromPac(out, host, name, source, to_editor, picture); });
  });
}

void Wwe13ArenaTab() {
  ImGui::TextWrapped("A WWE '12, '13, 2K14, 2K15, 2K16 or 2K17 arena (its pac\\bg\\bgNN.pac, compressed or not; the From another game tab reads it off "
                     "the disc) rebuilt as a 2011 arena in the "
                     "place of a host arena, which gives the room, the ring and the aprons. The result opens on the "
                     "Arena page: give it a banner and a name there and save it. This is how RAW IS WAR, SmackDown "
                     "1999, Royal Rumble 1998 and King of the Ring 1998 were made.");
  ImGui::Spacing();
  {  // the arenas on My game discs (the Port page lists a disc's arenas and copies the pick here)
    std::vector<std::string> keys;
    for (const char* k : {"wwe12", "wwe13", "wwe2k14", "wwe2k15", "wwe2k16", "wwe2k17"})
      if (!disc_list::PathFor(k).empty()) keys.push_back(k);
    if (keys.empty()) {
      ImGui::TextDisabled("No WWE '12 - 2K17 disc in My game discs yet.");
      ImGui::SameLine();
      if (ImGui::SmallButton("My game discs...")) GoTo(PageId::kDiscs);
    } else {
      ImGui::TextUnformatted("From my discs:");
      for (const auto& k : keys) {
        ImGui::SameLine();
        if (ImGui::SmallButton(disc_list::GameByKey(k)->name)) port_page::Open(L"arena_port.py", disc_list::PathFor(k));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its arenas, read off the disc: pick one and it comes back here.");
      }
    }
  }
  if (FileRow("Arena file...", g_w13_file, "pick a bgNN.pac", kPacFilter, 1)) {
    g_w13_picture.clear();  // (another file: the picture was the old one's)
    if (g_w13_name[0] == 0) std::snprintf(g_w13_name, sizeof g_w13_name, "%s", IdFrom(FileName(g_w13_file), "wwe13").c_str());
  }
  FileRow("Banner picture...", g_w13_picture, "none: set the banner and loading picture on the Arena page", kPictureFilter, 1,
          "The arena select banner and the loading screen picture. From the disc's own picture of the arena when it "
          "came from the From another game tab; pick another or remove it here, or change them on the Arena page after.");
  if (!g_w13_picture.empty()) {
    static std::wstring shown;
    static Picture pic;
    if (shown != g_w13_picture) {
      shown = g_w13_picture;
      Image img;
      if (LoadPicture(g_w13_picture, img)) pic.Set(Resize(img, 256, 128));
      else pic.Clear();
    }
    if (!pic.Empty()) pic.Draw(256 * g_scale, 128 * g_scale);
  }
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Host arena (the room it plays in)", g_arenas[g_w13_host].name)) {
    for (int i = 0; i < 20; ++i)
      if (ImGui::Selectable(g_arenas[i].name, g_w13_host == i)) g_w13_host = i;
    ImGui::EndCombo();
  }
  Hint("The host's file size and memory are the room the new arena must fit. RAW's was used for the bundled ones.");
  TextField("Name", g_w13_name, sizeof g_w13_name, "the arena's name");
  if (ImGui::CollapsingHeader("Options")) {
    ImGui::Indent();
    ImGui::Checkbox("The host's ring models (ids, bones, rope physics)", &g_w13.host_ring);
    ImGui::Checkbox("... carrying WWE '13's ring meshes and pictures", &g_w13.ring_meshes);
    ImGui::Checkbox("The host's aprons (WWE '13's pictures on them)", &g_w13.host_aprons);
    ImGui::Checkbox("The host's crowd instead of WWE '13's", &g_w13.host_crowd);
    ImGui::Checkbox("WWE '13's barrier corner pieces", &g_w13.barrier_corners);
    ImGui::Checkbox("WWE '13's full rope (heavier)", &g_w13.rope_hi);
    ImGui::Checkbox("Leave unused pictures out", &g_w13.prune_textures);
    ImGui::Checkbox("Role lights (stands / barrier lit like RAW)", &g_w13.role_lights);
    ImGui::SetNextItemWidth(120 * g_scale);
    ImGui::InputInt("Stand light group", &g_w13.stand_light);
    ImGui::SetNextItemWidth(120 * g_scale);
    ImGui::SliderFloat("Canvas tone", &g_w13.mat_tone, 0.3f, 1.5f, "%.2f");
    ImGui::Checkbox("No spotlight shadows on the canvas", &g_w13.no_spot_shadows);
    TextField("Drop meshes (id:k, ...)", g_w13_drop, sizeof g_w13_drop, "e.g. 1234:0, 1234:1");
    Hint("A model over about 430 KB crashes the arena load (the biggest any 2011 arena has): drop its heaviest "
         "meshes. The report after a conversion names the big ones.");
    ImGui::Unindent();
  }
  ImGui::Spacing();
  const bool special = !g_w13_file.empty() && SpecialStage(Utf8(fs::path(g_w13_file).filename().wstring()));
  if (special)
    ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "bg60 and up in WWE '12 - 2K17 are backstage rooms and studio stages, not arenas: on an arena "
                                                   "slot they show a white mat over a black void, or crash (bg78).");
  ImGui::BeginDisabled(g_w13_file.empty() || Busy() || special);
  PushAccent();
  if (ImGui::Button("Convert and open on the Arena page", ImVec2(320 * g_scale, 0))) {
    Wwe13Options opt = g_w13;
    opt.drop_mesh.clear();
    for (const char* p = g_w13_drop; *p;) {
      unsigned id = 0;
      int k = 0;
      if (std::sscanf(p, "%u:%d", &id, &k) == 2) opt.drop_mesh.push_back({id, k});
      while (*p && *p != ',') ++p;
      if (*p == ',') ++p;
    }
    Convert(g_w13_file, g_w13_host, g_w13_name, opt, true, g_w13_picture);
  }
  PopAccent();
  ImGui::EndDisabled();
}

// ---- one character file from any game (superstar_port.py convert --src <pac>, on the Port page)

std::wstring g_star_file;

void StarFileTab() {
  ImGui::PushTextWrapPos(0);
  ImGui::TextWrapped("One character file from any of your other games, without the disc: a chNNN.pac of SvR 2007 to WWE "
                     "2K17 (compressed or not) or a PS2 game's CH.PAC (it holds many characters: you pick one). The disc "
                     "steps read which game it's from and turn it into a superstar mod with the model, its outfits, "
                     "entrance gear and hair; it opens on the Superstar page to finish.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  FileRow("Character file...", g_star_file, "pick a chNNN.pac or a PS2 CH.PAC", kPacFilter, 1);
  ImGui::BeginDisabled(g_star_file.empty());
  PushAccent();
  if (ImGui::Button("Read it with the disc steps", ImVec2(320 * g_scale, 0))) port_page::Open(L"superstar_port.py", g_star_file);
  PopAccent();
  ImGui::EndDisabled();
}

// ---- WWE '13 superstar (xdec built in, w13_char.py)

std::wstring g_w13ch_file, g_w13ch_template, g_w13ch_portrait, g_w13ch_out;

void Wwe13StarTab() {
  ImGui::TextWrapped("A WWE '13 character model (pac\\ch\\chNNN.pac) as a ch.pac SvR 2011 loads: the Biped skeleton "
                     "renamed to 2011's, the shaders mapped, a 2011 model as the template for what WWE '13 lacks. "
                     "The result goes on the Superstar page as the model.");
  ImGui::Spacing();
  FileRow("WWE '13 character file...", g_w13ch_file, "pick a chNNN.pac", kPacFilter, 1);
  FileRow("2011 template (optional)...", g_w13ch_template, "pac\\ch\\ch104.pac (John Cena's rig)", kPacFilter, 1,
          "A 2011 model of the same build: its skeleton frames and face animation are used.");
  FileRow("Portrait (optional)...", g_w13ch_portrait, "none", kPictureFilter, 1, "A 256 x 256 DDS from WWE '13's menu (SSFC).");
  if (!ToolsReady(false)) return;
  ImGui::BeginDisabled(g_w13ch_file.empty() || Busy());
  PushAccent();
  if (ImGui::Button("Convert and use on the Superstar page", ImVec2(320 * g_scale, 0))) {
    const std::wstring src = g_w13ch_file;
    const std::wstring tmpl = g_w13ch_template.empty() ? (fs::path(g_game) / L"pac" / L"ch" / L"ch104.pac").wstring() : g_w13ch_template;
    const std::wstring portrait = g_w13ch_portrait;
    const fs::path work = fs::path(g_game) / L"Mods" / L".convert";
    std::error_code ec;
    fs::create_directories(work, ec);
    const std::wstring dec = (work / (fs::path(src).stem().wstring() + L"_dec.pac")).wstring();
    const std::wstring out = (work / (fs::path(src).stem().wstring() + L"_2011.pac")).wstring();
    // the decode built in, then the script
    Bytes d, plain;
    std::string err;
    if (!ReadFile(Utf8(src), d)) { Status("Could not read " + Utf8(src)); }
    else {
      if (IsXcompress(d)) {
        if (!XcompressDecode(d, plain, &err)) { Status("The file did not decode: " + err); return; }
      } else {
        plain = d;
      }
      WriteFile(Utf8(dec), plain);
      std::vector<std::wstring> args = {dec, out, L"--template", tmpl};
      if (!portrait.empty()) args.push_back(L"--portrait"), args.push_back(portrait);
      RunTool(L"w13_char.py", args, [out](int code) {
        if (code != 0) { Status("w13_char.py failed (see the log)."); return; }
        star_page::SetModel(out);
        GoTo(PageId::kStar);
        Status("The WWE '13 model is on the Superstar page: " + Utf8(out));
      });
    }
  }
  PopAccent();
  ImGui::EndDisabled();
}

// ---- SvR 2010 superstar (svr10_superstar.py)

int g_w10_id = 100;
int g_w10_model = 0;  // 0 default, 1 from 2010, 2 from 2011
bool g_w10_no_moves = false;
float g_w10_height = 1.0f;  // --height (1: left out)
char g_w10_out[260] = "";

void Svr10StarTab() {
  ImGui::TextWrapped("An SvR 2010 superstar as a whole mod, the way the 20 bundled ones were made: the 2010 model "
                     "converted (or 2011's own when it has one), the 2010 renders, theme (through ffmpeg) and "
                     "titantron, ratings, abilities, the move-set slot by slot with the moves 2011 lacks ported "
                     "into a move pack, the entrance and announcer name when 2011 has them. The mod opens on the "
                     "Superstar page.");
  ImGui::Spacing();
  if (g_w10_pac.empty()) g_w10_pac = Guess("svr2010");
  FolderRow("SvR 2010 pac folder...", g_w10_pac, L"The SvR 2010 \"pac\" folder (its extract)");
  MyDiscNote("svr2010");
  ImGui::SetNextItemWidth(120 * g_scale);
  ImGui::InputInt("2010 superstar id", &g_w10_id);
  Hint("The 2010 roster id (100 ...). The tool's survey lists them: run it once below.");
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::Combo("Model", &g_w10_model, "the tool's choice\0from SvR 2010\0from SvR 2011\0");
  ImGui::Checkbox("Keep the base's moves (no move pack)", &g_w10_no_moves);
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::SliderFloat("Height (scale)", &g_w10_height, 0.8f, 1.25f, "%.2f");
  Hint("The mod's height=: its size against the base superstar's (1.00 leaves it out; about 0.90 - 1.15 plays "
       "best). It can be changed on the Superstar page afterwards, where the preview shows it.");
  if (!ToolsReady(true)) return;
  if (ImGui::Button("Survey the 2010 roster (ids and names into the log)", ImVec2(320 * g_scale, 0)))
    RunTool(L"svr10_superstar.py", {L"survey", L"--pac10", g_w10_pac, L"--pac11", Pac11()}, [](int) {});
  ImGui::BeginDisabled(g_w10_pac.empty() || Busy());
  PushAccent();
  if (ImGui::Button("Make the mod and open it on the Superstar page", ImVec2(320 * g_scale, 0))) {
    const std::wstring out = PickFile(true, L"Where the .svrmod goes", kModFilter, 1, L"svrmod",
                                      (L"svr10_" + std::to_wstring(g_w10_id) + L".svrmod").c_str());
    if (!out.empty()) {
      std::vector<std::wstring> args = {std::to_wstring(g_w10_id), out, L"--pac10", g_w10_pac, L"--pac11", Pac11()};
      if (g_w10_model) args.push_back(L"--model"), args.push_back(g_w10_model == 1 ? L"10" : L"11");
      if (g_w10_no_moves) args.push_back(L"--no-moves");
      if (std::lround(g_w10_height * 100) != 100) {
        wchar_t hb[16];
        std::swprintf(hb, 16, L"%.2f", g_w10_height);
        args.push_back(L"--height"), args.push_back(hb);
      }
      RunTool(L"svr10_superstar.py", args, [out](int code) {
        if (code != 0) { Status("svr10_superstar.py failed (see the log)."); return; }
        ProjectOpen(out);
      });
    }
  }
  PopAccent();
  ImGui::EndDisabled();
}

// ---- SvR 2010 moves (svr10_moves.py build + movepack.py)

char g_w10_moves[256] = "";

void Svr10MovesTab() {
  ImGui::TextWrapped("SvR 2010 moves the 2011 game lacks, ported into a move pack: their motions (attacker, victim, "
                     "cameras), table records and category bits. The pack opens on the Move pack tab (save it as a "
                     "mod, or hand it to a superstar mod).");
  ImGui::Spacing();
  if (g_w10_pac.empty()) g_w10_pac = Guess("svr2010");
  FolderRow("SvR 2010 pac folder...", g_w10_pac, L"The SvR 2010 \"pac\" folder (its extract)");
  MyDiscNote("svr2010");
  TextField("Move ids", g_w10_moves, sizeof g_w10_moves, "e.g. 3088 3274 7573 (2010 move ids, space or comma separated)", 420);
  Hint("The 2010 move ids (the Game assets page shows 2011's misc.pac MOVS/WAZE names; 2010's are in its own "
       "misc.pac). A superstar's whole missing set comes with the SvR 2010 superstar tab instead.");
  if (!ToolsReady(false)) return;
  ImGui::BeginDisabled(g_w10_pac.empty() || !g_w10_moves[0] || Busy());
  PushAccent();
  if (ImGui::Button("Port the moves and open the pack", ImVec2(320 * g_scale, 0))) {
    const std::wstring out = PickFolder(L"A folder for the move pack");
    if (!out.empty()) {
      std::string ids = g_w10_moves;
      for (char& c : ids) if (c == ' ') c = ',';
      const std::wstring work = (fs::path(out) / L"work").wstring();
      RunTool(L"svr10_moves.py", {L"build", g_w10_pac, Pac11(), Wide(ids), L"--out", work}, [out, work](int code) {
        if (code != 0) { Status("svr10_moves.py failed (see the log)."); return; }
        RunTool(L"movepack.py", {(fs::path(work) / L"movepack").wstring(), out}, [out](int code2) {
          if (code2 != 0) { Status("movepack.py failed (see the log)."); return; }
          moves_page::TestOpen(out);
          GoTo(PageId::kMoves);
        });
      });
    }
  }
  PopAccent();
  ImGui::EndDisabled();
}

// ---- SvR 2008 backstage (svr08_stage.py + svr08_gimmick.py)

int g_w08_rotate = 1;
float g_w08_spread = 2.0f, g_w08_off[2] = {142.1f, -495.1f};
bool g_w08_gimmick = true;

void Svr08Tab() {
  ImGui::TextWrapped("SvR 2008's Parking Lot brawl stage (bg56) as 2011's parking room, with its cars and props as "
                     "the area's objects - the bundled Parking Lot (2008) mod. The settings below are the ones it "
                     "used. The result opens on the Backstage tab as an area of its own.");
  ImGui::Spacing();
  if (g_w08_pac.empty()) g_w08_pac = Guess("svr2008");
  FolderRow("SvR 2008 pac folder...", g_w08_pac, L"The SvR 2008 \"pac\" folder (its extract)");
  MyDiscNote("svr2008");
  ImGui::SetNextItemWidth(160 * g_scale);
  ImGui::Combo("Turn", &g_w08_rotate, "0\0" "180 degrees (faces the 2011 camera)\0");
  ImGui::SetNextItemWidth(160 * g_scale);
  ImGui::SliderFloat("Spread", &g_w08_spread, 1.0f, 3.0f, "x%.1f");
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::InputFloat2("Offset x, z", g_w08_off, "%.1f");
  ImGui::Checkbox("The 2008 cars and props too (gimmick pac)", &g_w08_gimmick);
  if (!ToolsReady(false)) return;
  ImGui::BeginDisabled(g_w08_pac.empty() || Busy());
  PushAccent();
  if (ImGui::Button("Convert and open on the Backstage tab", ImVec2(320 * g_scale, 0))) {
    const fs::path work = fs::path(g_game) / L"Mods" / L".convert";
    std::error_code ec;
    fs::create_directories(work, ec);
    const std::wstring bg56 = (fs::path(g_w08_pac) / L"bg" / L"bg56.pac").wstring();
    const std::wstring gm08 = (fs::path(g_w08_pac) / L"gm.pac").wstring();
    const std::wstring bg78 = (fs::path(g_game) / L"pac" / L"bg" / L"bg78.pac").wstring();
    const std::wstring out78 = (work / L"svr08_bg78.pac").wstring(), outgm = (work / L"svr08_gimmick.pac").wstring();
    wchar_t off[64], spread[16];
    std::swprintf(off, 64, L"%.1f,%.1f", g_w08_off[0], g_w08_off[1]);
    std::swprintf(spread, 16, L"%.1f", g_w08_spread);
    const std::wstring rot = g_w08_rotate ? L"180" : L"0";
    const bool gimmick = g_w08_gimmick;
    RunTool(L"svr08_stage.py", {bg56, bg78, out78, L"--rotate", rot, L"--spread", spread, L"--offset", off},
            [=](int code) {
              if (code != 0) { Status("svr08_stage.py failed (see the log)."); return; }
              auto finish = [=](const std::wstring& gpac) {
                Bytes pac;
                if (!ReadFile(Utf8(out78), pac)) { Status("The converted bg78 could not be read."); return; }
                arena_page::SetBackstageFromPac(pac, 0, "Parking Lot (2008)", gpac, "GMGB");
                GoTo(PageId::kBackstage);
              };
              if (!gimmick) { finish(L""); return; }
              RunTool(L"svr08_gimmick.py", {gm08, bg56, (fs::path(g_game) / L"pac" / L"gm.pac").wstring(), L"-",
                                            L"--elements", L"scenery", L"--hotspots-2011", L"--rotate", rot, L"--spread",
                                            spread, L"--offset", off, L"--pac-out", outgm},
                      [=](int code2) {
                        if (code2 != 0) { Status("svr08_gimmick.py failed (see the log)."); finish(L""); return; }
                        finish(outgm);
                      });
            });
  }
  PopAccent();
  ImGui::EndDisabled();
}

}  // namespace

void ConvertArena(const std::wstring& file, int host, const std::string& name, bool to_editor, const std::wstring& picture) {
  Convert(file, host, name, g_w13, to_editor, picture);
}

void UseArenaFile(const std::wstring& file, const std::string& name, const std::wstring& picture) {
  g_w13_file = file;
  g_w13_picture = picture;
  if (!picture.empty()) Log("arena: banner + loading picture from " + Utf8(picture));
  std::snprintf(g_w13_name, sizeof g_w13_name, "%s", name.c_str());
  g_show_arena_tab = true;
}

// test aid: --convert-w13 <wwe13 bgNN.pac>,<host tile>: converts at start (then --shot shows the editor)
void TestConvertW13(const std::wstring& file, int host) {
  g_w13_file = file;
  g_w13_host = host;
  std::snprintf(g_w13_name, sizeof g_w13_name, "WWE 13 arena");
  Wwe13Options opt = g_w13;
  const std::string name = g_w13_name;
  RunInBackground([file, host, opt, name] {
    Bytes w13, hostpac;
    if (!ReadFile(Utf8(file), w13) || !ReadFile(ArenaPath(host), hostpac)) { Status("test: files not read"); return; }
    Bytes out;
    Wwe13Report rep;
    std::string err;
    if (!BuildArenaFromWwe13(w13, hostpac, opt, out, rep, &err)) { Status("test: conversion failed: " + err); return; }
    for (const auto& n : rep.notes) Log("  " + n);
    char b[200];
    std::snprintf(b, sizeof b, "test: converted %d models, %d meshes, %d entries (%d from host, %d dropped), %zu halved",
                  rep.conv.models, rep.conv.meshes, rep.entries, rep.from_host, rep.dropped, rep.halved.size());
    Status(b);
    OnUiThread([out, host, name] { arena_page::SetArenaFromPac(out, host, name); });
  });
}

// ---- Bink 2 videos (.bk2: WWE 2K15 and later) - opt-in, the player's own tool

std::string g_bink2_state;  // "" not asked, else what Find said
bool g_bink2_asked = false;

void Bink2Section() {
  if (!ImGui::CollapsingHeader("Enable Bink 2 conversion (.bk2 videos: WWE 2K15 and later)")) return;
  ImGui::Indent();
  ImGui::TextWrapped("Titantrons and movies of WWE 2K15 and later are Bink 2. The Mod Maker can turn them into the "
                     "game's movies when you have a tool that reads Bink 2 - RAD Video Tools (free from RAD) or an "
                     "ffmpeg built with a Bink 2 decoder. Neither comes with the Mod Maker. Then \"From a video...\" "
                     "takes .bk2 files.");
  ImGui::SetNextItemWidth(420 * g_scale);
  std::string shown = g_settings.bink2_tool.empty() ? std::string("(look in the usual places)") : Utf8(g_settings.bink2_tool);
  ImGui::InputText("##bk2", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
  ImGui::SameLine();
  if (ImGui::Button("Choose the tool...")) {
    static const COMDLG_FILTERSPEC kExe[1] = {{L"radvideo64.exe or ffmpeg.exe", L"*.exe"}};
    const std::wstring f = PickFile(false, L"RAD Video Tools (radvideo64.exe) or a Bink 2 ffmpeg.exe", kExe, 1);
    if (!f.empty()) g_settings.bink2_tool = f, SaveSettings(), g_bink2_asked = false;
  }
  if (!g_settings.bink2_tool.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) g_settings.bink2_tool.clear(), SaveSettings(), g_bink2_asked = false;
  }
  if (!g_bink2_asked && !Busy() && ToolsReady(false)) {
    g_bink2_asked = true;
    g_bink2_state = "looking ...";
    std::vector<std::wstring> args = {L"find"};
    if (!g_settings.bink2_tool.empty()) args.push_back(L"--tool"), args.push_back(g_settings.bink2_tool);
    RunToolJson(L"bink2_convert.py", args, [](int code, const std::string& text) {
      const auto field = [&](const char* key) {
        const size_t k = text.find(std::string("\"") + key + "\"");
        if (k == std::string::npos) return std::string();
        const size_t q1 = text.find('"', text.find(':', k) + 1), q2 = text.find('"', q1 + 1);
        return q1 == std::string::npos || q2 == std::string::npos ? std::string() : text.substr(q1 + 1, q2 - q1 - 1);
      };
      g_bink2_state = code == 0 ? "found: " + field("kind") + " - " + field("path") : "not found";
    });
  }
  ImGui::TextColored(g_bink2_state.rfind("found", 0) == 0 ? ImVec4(0.55f, 0.85f, 0.55f, 1) : kWarn, "%s", g_bink2_state.c_str());
  if (g_bink2_state == "not found") {
    ImGui::SameLine();
    if (ImGui::SmallButton("RAD's download page"))
      ShellOpen(L"https://www.radgametools.com/bnkdown.htm");
  }
  ImGui::Unindent();
}

void Draw() {
  Heading("Other games", "Arenas from WWE '12 to 2K17, superstars, moves and backstage areas from WWE '13, SvR 2010 and SvR 2008, "
                         "converted the way the bundled mods were, each landing on its page to finish and save.");
  DrawTabs(~0u);
}

void DrawTabs(unsigned mask) {
  if (g_game.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("Only the other games' extracted files are read; the Python tools (Mod Maker Tools) do the SvR "
                      "2010 / 2008 / WWE '13 character work. Python: %s. Tools: %s. ffmpeg: %s.",
                      PythonExe().empty() ? "not found" : Utf8(PythonExe()).c_str(),
                      ToolsDir().empty() ? "not found" : Utf8(ToolsDir()).c_str(),
                      FfmpegExe().empty() ? "not found" : "found");
  ImGui::PopTextWrapPos();
  if (mask & (kSvr10Star | kW13Star)) Bink2Section();  // (the themes)
  if (ImGui::BeginTabBar("other_tabs")) {
    if ((mask & kW13Arena) && ImGui::BeginTabItem("WWE '12 - 2K17 arena", nullptr, g_show_arena_tab ? ImGuiTabItemFlags_SetSelected : 0)) {
      g_show_arena_tab = false;
      Wwe13ArenaTab();
      ImGui::EndTabItem();
    }
    if ((mask & kStarFile) && ImGui::BeginTabItem("Superstar file (any game)")) { StarFileTab(); ImGui::EndTabItem(); }
    if ((mask & kW13Star) && ImGui::BeginTabItem("WWE '13 superstar")) { Wwe13StarTab(); ImGui::EndTabItem(); }
    if ((mask & kSvr10Star) && ImGui::BeginTabItem("SvR 2010 superstar")) { Svr10StarTab(); ImGui::EndTabItem(); }
    if ((mask & kSvr10Moves) && ImGui::BeginTabItem("SvR 2010 moves")) { Svr10MovesTab(); ImGui::EndTabItem(); }
    if ((mask & kSvr08) && ImGui::BeginTabItem("SvR 2008 backstage")) { Svr08Tab(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
}

}  // namespace other_page
}  // namespace mm
