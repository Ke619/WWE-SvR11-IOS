// Media page (the game: media_mods.cpp, docs/MEDIA_MODS.md): one pack that
// replaces the game's own media - superstars' entrance videos, renders and
// entrance themes, arena screens' pictures, the menu music and any game
// sound. Three tabs (Titantron videos, Menus & renders, Audio) fill one pack.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "mod_check.h"
#include "svrfmt/arena.h"
#include "svrfmt/pac.h"
extern "C" {
#include "../launcher/bink_decode.h"
}

namespace mm {
namespace media_page {

using namespace svrfmt;

namespace {

struct MediaStar {
  std::wstring video, theme, victory;  // files
  Picture render;             // 512; the bust (256) and icon (64) are made from it
  Image bust, icon;
};
struct MediaArena {
  std::vector<Picture> frames;  // the screens' flip-book (10 frames)
};
struct MediaPack {
  char name[64] = "", author[64] = "", version[16] = "1.0";
  std::map<int, MediaStar> stars;    // character id -> replacements
  std::map<int, MediaArena> arenas;  // arena number -> screens
  std::wstring menu_music;
  std::vector<std::pair<std::string, std::wstring>> sounds;  // event (no "Play_") -> file
};
MediaPack g_media;
int g_star = -1;   // index in Stars()
int g_arena = 0;   // tile in g_arenas
char g_event[64] = "";
int g_test_arena = -1;  // --media-arena <bg number> --media-video <bik> --media-picture <file>... --test-media-save <file>
std::wstring g_test_video, g_test_save;
std::vector<std::wstring> g_test_pictures;

std::string MediaId() { return IdFrom(g_media.name, "media"); }

bool StarPicker() {
  const auto& stars = Stars();
  ImGui::SetNextItemWidth(320 * g_scale);
  const char* cur = g_star >= 0 ? stars[g_star].name.c_str() : "(pick a superstar)";
  if (ImGui::BeginCombo("Superstar", cur)) {
    for (int i = 0; i < int(stars.size()); ++i) {
      const auto it = g_media.stars.find(stars[i].id);
      const bool has = it != g_media.stars.end() && (!it->second.video.empty() || !it->second.theme.empty() || !it->second.victory.empty() || !it->second.render.Empty());
      if (ImGui::Selectable((stars[i].name + (has ? "  *" : "") + "##m" + std::to_string(i)).c_str(), g_star == i))
        g_star = i;
    }
    ImGui::EndCombo();
  }
  return g_star >= 0;
}

// The arena's screen flip-book: its textures named <prefix>_animNN (00-09).
std::vector<std::string> ScreenFrames(Arena& a) {
  std::vector<std::string> names;
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures) {
      const size_t at = t.name.rfind("_anim");
      if (at != std::string::npos && t.name.size() == at + 7 && std::isdigit(uint8_t(t.name[at + 5])) &&
          std::isdigit(uint8_t(t.name[at + 6])))
        names.push_back(t.name);
    }
  std::sort(names.begin(), names.end());
  return names;
}

void SetArenaFrames(int arena, std::vector<Image> frames) {
  MediaArena& m = g_media.arenas[arena];
  m.frames.clear();
  for (auto& f : frames) {
    Picture p;
    p.Set(std::move(f));
    m.frames.push_back(std::move(p));
  }
  if (m.frames.empty()) g_media.arenas.erase(arena);
  Touch();
}

// A .bik -> 10 frames, evenly spread.
std::vector<Image> FramesFromVideo(const std::wstring& file) {
  std::vector<Image> out;
  wchar_t err[256] = L"";
  BinkReader* r = bink_open(file.c_str(), err, 256);
  if (!r) {
    Status("Not a Bink video (.bik): " + Utf8(file) + " - the launcher's Movies tab makes them.");
    return out;
  }
  const int w = bink_width(r), h = bink_height(r), n = std::max(1, bink_frames(r));
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  int pos = 0;
  for (int k = 0; k < 10; ++k) {
    const int want = k * n / 10;
    while (pos <= want) bink_next(r), ++pos;
    bink_bgra(r, bgra.data());
    Image img;
    img.w = w, img.h = h;
    img.rgba.resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4)
      img.rgba[i] = bgra[i + 2], img.rgba[i + 1] = bgra[i + 1], img.rgba[i + 2] = bgra[i], img.rgba[i + 3] = 255;
    out.push_back(std::move(img));
  }
  bink_close(r);
  return out;
}

// Everything the build needs, copied off the UI thread.
struct Job {
  std::string id, name, author, version;
  struct Star { int id; std::wstring video, theme; Image render, bust, icon; std::wstring victory; };
  std::vector<Star> stars;
  std::map<int, std::vector<Image>> arenas;
  std::wstring menu_music;
  std::vector<std::pair<std::string, std::wstring>> sounds;
  std::string project;  // (ModProjectText: the page's project keys, into the mod)
};

void PrepareJob(Job& j) {
  if (!g_media.name[0]) std::snprintf(g_media.name, sizeof g_media.name, "My Media");
  j.id = MediaId(), j.name = g_media.name, j.author = g_media.author, j.version = g_media.version;
  for (const auto& [id, s] : g_media.stars) j.stars.push_back({id, s.video, s.theme, s.render.image, s.bust, s.icon, s.victory});
  for (const auto& [num, a] : g_media.arenas)
    for (const auto& f : a.frames) j.arenas[num].push_back(f.image);
  j.menu_music = g_media.menu_music;
  j.sounds = g_media.sounds;
  j.project = ModProjectText(PageId::kMedia);
}

bool Empty() {
  for (const auto& [id, s] : g_media.stars)
    if (!s.video.empty() || !s.theme.empty() || !s.victory.empty() || !s.render.Empty()) return false;
  return g_media.arenas.empty() && g_media.menu_music.empty() && g_media.sounds.empty();
}

// The pack's files (in the background).
bool BuildFiles(const Job& j, std::vector<ZipEntry>& files) {
  std::string man = "type=media\nid=" + j.id + "\nname=" + j.name + "\nauthor=" + j.author + "\nversion=" + j.version + "\n" + MadeWith();
  auto add_file = [&](const std::string& key, const std::wstring& src, const std::string& stem) -> bool {
    Bytes d;
    if (!ReadFile(Utf8(src), d)) {
      Status("Could not read " + Utf8(src));
      return false;
    }
    const std::string n = stem + Utf8(fs::path(src).extension().wstring());
    files.push_back({n, std::move(d)});
    man += key + "=" + n + "\n";
    return true;
  };
  for (const auto& s : j.stars) {
    const std::string i = std::to_string(s.id);
    if (!s.video.empty()) {  // (as an mp4: a Bink is shrunk to one; the Bink beside it with "Keep the Bink too")
      std::string why;
      const std::string name = ModMovieFiles(s.video, "video_" + i, files, &why);
      if (name.empty()) {
        Status("The entrance video can't go in the pack: " + why + ".");
        return false;
      }
      man += "video." + i + "=" + name + "\n";
    }
    if (!s.theme.empty() && !add_file("theme." + i, s.theme, "theme_" + i)) return false;
    if (!s.victory.empty() && !add_file("victory_theme." + i, s.victory, "victory_" + i)) return false;
    if (s.render.w) {
      files.push_back({"render_" + i + ".dds", DdsEncode(s.render, DxtFormat::kDxt5, false)});
      files.push_back({"bust_" + i + ".dds", DdsEncode(s.bust, DxtFormat::kDxt5, false)});
      files.push_back({"icon_" + i + ".dds", DdsEncode(s.icon, DxtFormat::kDxt5, false)});
      man += "render." + i + "=render_" + i + ".dds\nbust." + i + "=bust_" + i + ".dds\nicon." + i + "=icon_" + i + ".dds\n";
    }
  }
  for (const auto& [num, frames] : j.arenas) {
    if (frames.empty()) continue;
    char bg[16];
    std::snprintf(bg, sizeof bg, "bg%02d.pac", num);
    Progress(std::string("Rebuilding ") + bg + " with the new screens ...");
    Arena a;
    std::string err;
    if (!a.Load(PathStr(fs::path(g_game) / L"pac" / L"bg" / fs::u8path(bg)), &err)) {
      Status(std::string(bg) + ": " + err);
      return false;
    }
    const auto names = ScreenFrames(a);
    if (names.empty()) {
      Log(std::string(bg) + " has no screen flip-book (<name>_anim00..09) to replace: skipped.");
      continue;
    }
    std::vector<std::string> keep;
    for (size_t k = 0; k < names.size(); ++k) {
      BundleTexture* t = a.FindTexture(names[k]);
      DdsInfo info;
      if (!t || !DdsInfoOf(t->data, info)) continue;
      const Image& src = frames[k * frames.size() / names.size()];
      const DxtFormat f = info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format;
      t->data = DdsEncode(Resize(src, info.w, info.h), f, info.mips > 1);
      keep.push_back(names[k]);
      for (auto& b : a.bundles)
        for (auto& bt : b.textures)
          if (&bt == t) b.changed = true;
    }
    a.FitFile(keep);
    const Bytes pac = a.Save(&err);
    if (!err.empty()) {
      Status(std::string(bg) + ": " + err);
      return false;
    }
    files.push_back({bg, pac});
    man += "arena." + std::to_string(num) + "=" + bg + "\n";
  }
  if (!j.menu_music.empty() && !add_file("menu_music", j.menu_music, "menu_music")) return false;
  for (size_t k = 0; k < j.sounds.size(); ++k)
    if (!add_file("sound." + j.sounds[k].first, j.sounds[k].second, "sound_" + std::to_string(k))) return false;
  files.insert(files.begin(), ZipEntry{"manifest.txt", Bytes(man.begin(), man.end())});
  AddModProject(files, j.project);
  return files.size() > 2 || (files.size() > 1 && j.project.empty());
}

void SaveMod() {
  Job j;
  PrepareJob(j);
  const std::wstring f = PickFile(true, L"Save the media pack", kModFilter, 1, L"svrmod", (Wide(j.id) + L".svrmod").c_str());
  if (f.empty()) return;
  const std::string out = Utf8(f);
  RunInBackground([j, out] {
    std::vector<ZipEntry> files;
    if (!BuildFiles(j, files)) return;
    if (WriteFile(out, ZipWrite(files))) Status("Saved " + out + " (add it in the launcher's Mods tab with +).");
    else Status("The media pack could not be written: " + out);
  });
}

void Install() {
  Job j;
  PrepareJob(j);
  RunInBackground([j] {
    std::vector<ZipEntry> files;
    if (!BuildFiles(j, files)) return;
    const fs::path dir = fs::path(g_game) / L"Mods" / L"Media" / fs::u8path(j.id);
    std::string why;
    if (!InstallModFiles(dir, files, &why)) {
      Status("Not installed: " + why);
      return;
    }
    Status("Installed: it takes effect the next time the game starts (" + PathStr(dir) + ")." + InstallReport("Media", dir));
  });
}

void Problems(std::vector<Problem>& p) {
  if (Empty()) p.push_back(Error("Nothing is in it yet: add a video, a song or a sound to replace."));
  if (!g_media.name[0]) p.push_back(Warning("It has no name yet: it'd be called \"My Media\".")
                                        .With("Call it \"My Media\"", [] { std::snprintf(g_media.name, sizeof g_media.name, "My Media"); Touch(); }));
  std::error_code ec;
  for (const auto& [id, s] : g_media.stars) {
    const StarInfo* st = StarById(id);
    const std::string who = st ? st->name : std::to_string(id);
    if (!s.video.empty() && !fs::exists(s.video, ec)) p.push_back(Error(who + "'s video file is missing: " + FileName(s.video)));
    if (!s.theme.empty() && !fs::exists(s.theme, ec)) p.push_back(Error(who + "'s theme file is missing: " + FileName(s.theme)));
    if (!s.victory.empty() && !fs::exists(s.victory, ec)) p.push_back(Error(who + "'s victory track is missing: " + FileName(s.victory)));
  }
  if (!g_media.menu_music.empty() && !fs::exists(g_media.menu_music, ec)) p.push_back(Error("The menu music file is missing."));
  for (const auto& [ev, f] : g_media.sounds)
    if (!fs::exists(f, ec)) p.push_back(Error("The sound file for " + ev + " is missing: " + FileName(f)));
  bool themes = false;
  for (const auto& [id, s] : g_media.stars) themes |= !s.theme.empty() || !s.victory.empty();
  if (themes) p.push_back(Warning("Your entrance songs will play much louder than the game's own music.", "Make them quieter in a sound editor, or use the Superstar page's \"Match the game\"."));
}

void VideosTab() {
  ImGui::TextWrapped("Superstars' entrance videos (320 x 320; mp4, or Bink - the packs carry them as mp4) and the "
                     "arena screens' pictures between entrances.");
  ImGui::Spacing();
  if (StarPicker()) {
    MediaStar& s = g_media.stars[Stars()[g_star].id];
    if (FileRow("Entrance video...", s.video, "the game's", kBinkFilter, 1)) Touch();
    ImGui::SameLine();
    BinkFromVideoButton("From a video...", s.video, Stars()[g_star].name);
    MovieSizeRow(s.video);
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Arena screens");
  Hint("The arena's screens show a flip-book of pictures while no entrance video plays. A video gives 10 frames spread "
       "over it; pictures are shown in turn.");
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Arena", g_arenas[g_arena].name)) {
    for (int i = 0; i < 20; ++i) {
      const bool has = g_media.arenas.count(g_arenas[i].number) > 0;
      if (ImGui::Selectable((std::string(g_arenas[i].name) + (has ? "  *" : "")).c_str(), g_arena == i)) g_arena = i;
    }
    ImGui::EndCombo();
  }
  const int num = g_arenas[g_arena].number;
  if (ImGui::Button("From a video (.bik)...", ImVec2(220 * g_scale, 0))) {
    static const COMDLG_FILTERSPEC kBik[1] = {{L"Bink movie (*.bik)", L"*.bik"}};  // (the frames are read with the Bink reader)
    const std::wstring f = PickFile(false, L"Screen video", kBik, 1);
    if (!f.empty()) {
      auto frames = FramesFromVideo(f);
      if (!frames.empty()) SetArenaFrames(num, std::move(frames));
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("From pictures...", ImVec2(220 * g_scale, 0))) {
    std::vector<Image> frames;
    for (const auto& f : PickFiles(L"Screen pictures (played in turn)", kPictureFilter, 1)) {
      Image img;
      if (LoadPicture(f, img)) frames.push_back(std::move(img));
    }
    if (!frames.empty()) SetArenaFrames(num, std::move(frames));
  }
  if (auto it = g_media.arenas.find(num); it != g_media.arenas.end() && !it->second.frames.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x##arena")) SetArenaFrames(num, {});
    else
      for (size_t k = 0; k < it->second.frames.size(); ++k) {
        if (k) ImGui::SameLine();
        ImGui::Image(it->second.frames[k].Id(), ImVec2(96 * g_scale, 48 * g_scale));
      }
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled("the arena's own screens");
  }
}

void RendersTab() {
  ImGui::TextWrapped("A superstar's pictures in the menus: the render, the bust and the face icon, all from one "
                     "picture (a PNG with a transparent background is best).");
  ImGui::Spacing();
  if (!StarPicker()) return;
  MediaStar& s = g_media.stars[Stars()[g_star].id];
  if (ImGui::Button("Picture...", ImVec2(220 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Superstar picture", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) {
      Image render;
      MakeRenders(img, render, s.bust, s.icon);
      s.render.Set(std::move(render));
      Touch();
    }
  }
  if (!s.render.Empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) s.render.Clear(), s.bust = s.icon = Image(), Touch();
    else {
      s.render.Draw(256 * g_scale, 256 * g_scale);
      ImGui::SameLine();
      ImGui::TextDisabled("render 512, bust 256 and icon 64 are made from it");
    }
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled("the game's");
    if (Picture* r = StarRender(Stars()[g_star].id); r && !r->Empty()) r->Draw(256 * g_scale, 256 * g_scale);
  }
}

void AudioTab() {
  ImGui::TextWrapped("Entrance themes, the menu music and any game sound (crowd chants, hits ...) replaced by your "
                     "own files (.mp3 .m4a .wav .flac .wma .ogg).");
  ImGui::Spacing();
  if (StarPicker()) {
    MediaStar& s = g_media.stars[Stars()[g_star].id];
    if (FileRow("Entrance theme...", s.theme, "the game's", kSoundFilter, 1)) Touch();
    if (FileRow("Victory track...", s.victory, "their theme", kSoundFilter, 1,
                "Plays when they win, in place of their theme. Only this PC hears it: online, each player hears their own packs' track.")) Touch();
  }
  ImGui::Separator();
  if (FileRow("Menu music...", g_media.menu_music, "the game's", kSoundFilter, 1,
              "Plays in the menus instead of the game's music (the jukebox's MY MUSIC does this per song too).")) Touch();
  ImGui::Separator();
  ImGui::TextUnformatted("Game sounds");
  Hint("By the game's event name without \"Play_\" (e.g. SVR10_Chant_Sena_001 for the Cena chant, elbow_mid_0231_0_0). "
       "SVR2011_TEST_SOUND_LOG=1 in a test run lists the events as they play.");
  ImGui::SetNextItemWidth(320 * g_scale);
  ImGui::InputTextWithHint("##event", "event name", g_event, sizeof g_event);
  ImGui::SameLine();
  ImGui::BeginDisabled(!g_event[0]);
  if (ImGui::Button("Sound file...", ImVec2(160 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"Sound file", kSoundFilter, 1);
    std::string ev = g_event;
    if (ev.rfind("Play_", 0) == 0) ev = ev.substr(5);
    if (!f.empty()) g_media.sounds.push_back({ev, f}), g_event[0] = 0, Touch();
  }
  ImGui::EndDisabled();
  for (size_t k = 0; k < g_media.sounds.size(); ++k) {
    ImGui::PushID(int(k));
    ImGui::Text("%s  ->  %s", g_media.sounds[k].first.c_str(), FileName(g_media.sounds[k].second).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      g_media.sounds.erase(g_media.sounds.begin() + long(k));
      Touch();
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
  }
}

int g_tab = 0;  // the part shown: 0 Titantron videos, 1 Menus & renders, 2 Audio (one pack holds all three)

void Draw() {
  if (g_test_arena >= 0 && (!g_test_video.empty() || !g_test_pictures.empty())) {
    std::vector<Image> frames;
    for (const auto& f : g_test_pictures) {
      Image img;
      if (LoadPicture(f, img)) frames.push_back(std::move(img));
    }
    SetArenaFrames(g_test_arena, frames.empty() ? FramesFromVideo(g_test_video) : std::move(frames));
    if (const int t = ArenaTileOf(g_test_arena); t >= 0) g_arena = t;
    g_test_arena = -1;
    if (!g_test_save.empty()) {
      Job j;
      PrepareJob(j);
      std::vector<ZipEntry> files;
      if (BuildFiles(j, files) && WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  Heading("Media", "One pack that replaces the game's own media: entrance videos and themes, renders, arena screens, "
                   "the menu music and game sounds. Every item is optional.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("media_body", ImVec2(0, -48 * g_scale), false);
  TextField("Pack name", g_media.name, sizeof g_media.name, "My Media");
  TextField("Author (optional)", g_media.author, sizeof g_media.author);
  TextField("Version (optional)", g_media.version, sizeof g_media.version, "1.0");
  ImGui::Spacing();
  // (the parts are the Media group's tabs in the sidebar's tab row: SetTab)
  if (g_tab == 1) RendersTab();
  else if (g_tab == 2) AudioTab();
  else VideosTab();
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, false)) {
    case 1: SaveMod(); break;
    case 2: Install(); break;
  }
}

std::string StateText() {
  std::string s = std::string("name=") + g_media.name + "\nauthor=" + g_media.author + "\nversion=" + g_media.version +
                  "\nmenu=" + Utf8(g_media.menu_music) + "\n";
  for (const auto& [id, st] : g_media.stars)
    s += std::to_string(id) + ":" + Utf8(st.video) + "|" + Utf8(st.theme) + "|" + Utf8(st.victory) + "|" + std::to_string(st.render.image.rgba.size()) + "\n";
  for (const auto& [num, a] : g_media.arenas) s += "arena" + std::to_string(num) + "=" + std::to_string(a.frames.size()) + "\n";
  for (const auto& [ev, f] : g_media.sounds) s += ev + "=" + Utf8(f) + "\n";
  return s;
}

void Reset() { g_media = MediaPack(); g_star = -1; }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_media.name);
  out.Key("author", g_media.author);
  out.Key("version", g_media.version);
  for (const auto& [id, s] : g_media.stars) {
    const std::string i = std::to_string(id);
    out.File("video." + i, ProjectMovie(s.video), "media/video_" + i);
    out.File("theme." + i, s.theme, "media/theme_" + i);
    out.File("victory_theme." + i, s.victory, "media/victory_" + i);
    out.Png("render." + i, s.render.image, "media/render_" + i);
  }
  for (const auto& [num, a] : g_media.arenas)
    for (size_t k = 0; k < a.frames.size(); ++k)
      out.Png("screen." + std::to_string(num) + "." + std::to_string(k), a.frames[k].image,
              "media/screen_" + std::to_string(num) + "_" + std::to_string(k));
  out.File("menu_music", g_media.menu_music, "media/menu_music");
  for (size_t k = 0; k < g_media.sounds.size(); ++k) {
    out.Key("sound_event" + std::to_string(k), g_media.sounds[k].first);
    out.File("sound" + std::to_string(k), g_media.sounds[k].second, "media/sound_" + std::to_string(k));
  }
}

bool Read(const ProjectIn& in, bool mod) {
  std::snprintf(g_media.name, sizeof g_media.name, "%s", in.Get("name").c_str());
  std::snprintf(g_media.author, sizeof g_media.author, "%s", in.Get("author").c_str());
  std::snprintf(g_media.version, sizeof g_media.version, "%s", in.Get("version").c_str());
  if (!g_media.version[0]) std::snprintf(g_media.version, sizeof g_media.version, "1.0");
  auto file = [&](const std::string& key) -> std::wstring {
    const std::string n = in.Get(key.c_str());
    if (n.empty()) return {};
    const std::string from = in.Get((key + ".from").c_str());
    if (!mod && !from.empty() && fs::exists(Wide(from))) return Wide(from);
    return in.Find(n) ? in.Extract(n) : std::wstring();
  };
  // every key of the text
  std::string line;
  for (size_t at = 0; at <= in.text.size(); ++at) {
    if (at < in.text.size() && in.text[at] != '\n') { line += in.text[at]; continue; }
    const size_t eq = line.find('=');
    if (eq != std::string::npos && !line.substr(0, eq).ends_with(".from")) {  // (.from: where a file came from, not a key)
      const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
      auto id_of = [&](const char* prefix) { return key.rfind(prefix, 0) == 0 ? std::atoi(key.c_str() + std::strlen(prefix)) : -1; };
      if (int id = id_of("video."); id > 0) g_media.stars[id].video = file(key);
      else if (int id = id_of("theme."); id > 0) g_media.stars[id].theme = file(key);
      else if (int id = id_of("victory_theme."); id > 0) g_media.stars[id].victory = file(key);
      else if (int id = id_of("render."); id > 0) {
        Image img;
        const ZipEntry* e = in.Find(val);
        if (e && DdsDecode(e->data, img)) {
          MediaStar& s = g_media.stars[id];
          if (mod) {  // (bust and icon come with the mod)
            s.render.Set(img);
            const ZipEntry* b = in.Find(in.Get(("bust." + std::to_string(id)).c_str()));
            const ZipEntry* ic = in.Find(in.Get(("icon." + std::to_string(id)).c_str()));
            if (!b || !DdsDecode(b->data, s.bust) || !ic || !DdsDecode(ic->data, s.icon)) {
              Image r;
              MakeRenders(img, r, s.bust, s.icon);
            }
          } else {
            Image r;
            MakeRenders(img, r, s.bust, s.icon);
            s.render.Set(std::move(r));
          }
        }
      } else if (int num = id_of("screen."); num >= 0 && !mod) {
        Image img;
        const ZipEntry* e = in.Find(val);
        if (e && DdsDecode(e->data, img)) {
          Picture p;
          p.Set(std::move(img));
          g_media.arenas[num].frames.push_back(std::move(p));
        }
      } else if (int num = id_of("arena."); num >= 0 && mod) {
        // a mod carries the whole bgNN.pac: its frames come back out
        const ZipEntry* e = in.Find(val);
        Arena a;
        if (e && a.LoadData(e->data)) {
          for (const auto& n : ScreenFrames(a)) {
            Image img;
            if (BundleTexture* t = a.FindTexture(n); t && DdsDecode(t->data, img)) {
              Picture p;
              p.Set(std::move(img));
              g_media.arenas[num].frames.push_back(std::move(p));
            }
          }
        }
      } else if (key == "menu_music") {
        g_media.menu_music = file(key);
      } else if (key.rfind("sound.", 0) == 0 && mod) {
        g_media.sounds.push_back({key.substr(6), file(key)});
      } else if (int k = id_of("sound_event"); k >= 0) {
        g_media.sounds.push_back({val, file("sound" + std::to_string(k))});
      }
    }
    line.clear();
  }
  return true;
}

}  // namespace

void TestVictory(int id, const std::wstring& song) { g_media.stars[id].victory = song; }

void TestStart(int arena, const std::wstring& video, const std::vector<std::wstring>& pictures, const std::wstring& save) {
  g_test_arena = arena, g_test_video = video, g_test_pictures = pictures, g_test_save = save;
}

PageHooks hooks = {
    "media", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

void SetTab(int tab) { g_tab = tab; }
int Tab() { return g_tab; }

}  // namespace media_page
}  // namespace mm
