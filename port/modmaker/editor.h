// The Arena Editor (Mod Maker, arenas branch): the arena in 3D with its
// objects listed by zone; move / turn / scale / duplicate / hide objects,
// add objects (OBJ / FBX files, boxes), retexture, the Ring Kit, lighting
// presets and the size budget. Edits go straight into the project's Arena;
// the Ring Kit and lighting are applied when the mod is built (ApplyBuild).
#pragma once

#include <d3d11.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "svrfmt/arena.h"
#include "svrfmt/ring_kit.h"

namespace editor {

struct Lighting {
  int preset = 0;                 // index in Presets() (0 = the arena as made)
  float color[3] = {1, 1, 1};     // multiplied into every material colour
  float strength = 1.0f;
  bool crowd = true;              // the people in the seats
  // the crowd's colour (the arena's aud_lightmap, svrfmt TintAudienceLightmap): 0 as made, 1 neutral, 2 crowd_color;
  // crowd_bright scales it either way
  int crowd_mode = 0;
  float crowd_color[3] = {1, 0.9f, 0.75f};
  float crowd_bright = 1.0f;
  bool CrowdDefault() const { return crowd_mode == 0 && crowd_bright == 1.0f; }
  bool Default() const;
  // In the game (manifest light_scale= / match_light=: the game's arena_lights.cpp): the arena's lights x
  // game_scale (with the player's ARENA BRIGHTNESS); match_light 0 the game's, 1 the entrance's in the match,
  // 2 the entrance's with its coloured key light.
  float game_scale = 1.0f;
  int match_light = 0;
  // manifest titantron=live: the video screens show the match camera in the match (the game's arena_mods.cpp
  // TitantronFile); off, the arena's own screens show then (the entrance video stays either way)
  bool tron_live = false;
};

struct Hooks {
  std::function<void(const std::string&)> log;
  std::function<void()> test_in_game;  // install the mod and start the game
  std::function<std::string(const char* title)> pick_picture;  // a picture file dialog ("" = none)
  // the prop library: arenas to take models from (name, pac path)
  std::vector<std::pair<std::string, std::string>> library;
};

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Hooks& hooks);
// The arena being edited (nullptr: none). Rebuilds the object list when it changes.
void SetArena(svrfmt::Arena* arena, const std::string& title);
// Only the models with ids lo..hi are shown and edited (a backstage area in
// bg78, which holds seven); -1, -1: all. Before SetArena.
// spot: the game's box for the area (sub_8224EF28: centre x y z, half x z);
// the test edit's box goes beside its centre, where a match camera sees it.
void SetArea(const std::vector<std::pair<int, int>>& ids, const float* spot = nullptr);
// The editor's whole UI, filling the current ImGui window region.
void Draw();
// A second, read-only scene (the Installed mods page): an arena shown without touching the one being edited.
// ViewArena takes the arena (kept alive by the caller until ClearView) and its manifest (ring / lighting keys);
// DrawView draws it in w x h with an orbit camera only.
void ViewArena(svrfmt::Arena* arena, const std::string& title, const std::string& manifest);
// An arena's ring kit as the game puts it out (its own rope heights): each part's model (index into arena.models)
// with its world matrix (row-major, row vectors, game space: y down) - for views that draw a ring without the
// editor's scene (char_preview's basic ring).
struct RingPart {
  int model = -1;
  float m[16] = {};
};
std::vector<RingPart> RingPlacements(const svrfmt::Arena& a);
void DrawView(float w, float h);
void ClearView();
// The view's camera given by the caller (the Full preview: an entrance's camera track): view and projection,
// game coordinates, row vectors (HLSL row_major + mul(v, M)) - DrawView uses it and the mouse doesn't move it,
// until ClearViewCamera. The projection's depth range should be 1 .. 5000 (the editor's).
void SetViewCamera(const float view[16], const float proj[16]);
void ClearViewCamera();
// An arena texture drawn with another picture (the Full preview: the titantron movie on the screen's tf_titan);
// nullptr puts the arena's own back. The caller keeps the view alive while it's set.
void SetTextureOverride(const std::string& name, ID3D11ShaderResourceView* srv);
// Textures drawn self-lit (their own colours, the lighting preset's tint only - not the vertex colours or the light):
// the Full preview's screens, lit in game by their own light set. masks: an LED wall's dot grid textures (drawn over
// the screen: their holes lit dots, the screen between them). Empty: none.
void SetSelfLit(const std::vector<std::string>& textures, const std::vector<std::string>& masks = {});
// Alpha cut-outs only on the models whose C351 line has a transparency flag (s a k c i j o), the rest opaque - as the
// game draws them (the Full preview: bg29's barrier panels have alpha but no flag). Off: every texture's alpha cuts.
void SetGameTransparency(bool on);
// The clock and look of the arena's own stage lights (its light sets, 0x7530) and of the beams / glows its motions
// (0x7531) put in place: seconds since the show began (animated sets and motions loop at 60 frames a second), and
// entrance: the entrance's effect models ("e" in 0xC351) instead of the match's ("g"). The editor's view: 0, the match.
void SetStageTime(float seconds, bool entrance);
// Drawn after the view's scene into the same target and depth (w x h), with the camera used: the Full preview's
// wrestler. Empty: none.
using ViewExtraFn = std::function<void(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h,
                                       const float view[16], const float proj[16])>;
void SetViewExtra(ViewExtraFn fn);
// The view's labels / mouse hint shown (the Full preview turns them off).
void SetViewHint(bool on);
bool Busy();
bool HasArena();   // an arena is set
int EditCount();   // grows with every edit (the project's dirty check)
// The size budget (measured in the background after edits): the arena's file
// and unpacked sizes against the shipped one's, and the biggest model you
// changed or added (unpacked), for the problems list. False until measured.
struct BudgetInfo {
  size_t file = 0, file_max = 0, unpacked = 0, unpacked_max = 0;
  size_t biggest = 0;          // the biggest changed / added model, unpacked
  uint32_t biggest_id = 0;
};
bool Budget(BudgetInfo& out);
void Shutdown();
// test aid: a camera preset (0 hard camera, 1 top, 2 from the stage; -1 none)
// and an object to select (by name) once an arena is set
void TestStart(int view, const std::string& select);
// test aid: the selected object gets this picture, as "Use a picture..." gives one
void TestPicture(const std::string& path);
// --test-add-object / --test-export-object / --test-remove-part (on the --select object); RunObjectTests runs them in
// place of the test edit (false: none given)
void TestObjects(const std::string& add, const std::string& export_dir, int remove_part);
bool RunObjectTests();
// test aid: moves the selection 2 m, adds a 2 m box at ringside, red top rope, warmer light
void TestEdit();
// test aid: every entrance-zone model of library arena `lib` added in its place
void TestLibrary(int lib);

svrfmt::RingSpec& Ring();
Lighting& Light();
// The Ring Kit and lighting, onto a copy of the edited arena that is about
// to be saved (then FitFile).
void ApplyBuild(svrfmt::Arena& a);
// manifest lines for the ring settings (ring.*) and the lighting (light.*)
std::string ManifestLines();
void FromManifest(const std::string& text);

// ---- the Simple arena maker (simple_arena.cpp): the edited arena's pictures and a view of it
std::vector<std::string> TextureNames();  // every texture of the edited arena
// The textures the models with ids lo..hi draw with (the aprons: 973 - 974).
std::vector<std::string> ModelTextures(uint32_t lo, uint32_t hi);
// A texture decoded (its full size; w / h: its size in the arena).
bool TextureImage(const std::string& name, svrfmt::Image& out, int* w = nullptr, int* h = nullptr);
// A texture replaced by a picture, fitted to its size and format (an edit: the mod carries it).
bool ReplaceTexture(const std::string& name, const svrfmt::Image& img);
// The edited arena's 3D view alone, w x h (turn, move, zoom; no tools).
// A texture's DDS as stored / put back (the Simple arena's "Original").
bool TextureBytes(const std::string& name, svrfmt::Bytes& out);
bool SetTextureBytes(const std::string& name, const svrfmt::Bytes& dds);
void DrawViewport(float w, float h);
void SetViewPreset(int which);  // 0 hard camera, 1 top, 2 from the stage, 3 from the side
void DrawRingKit();             // the Ring Kit panel (ropes, turnbuckles, pads, heights)
void DrawLighting();            // the lighting panel (presets, colour, crowd)

}  // namespace editor
