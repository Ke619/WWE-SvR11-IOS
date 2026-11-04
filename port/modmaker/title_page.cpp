// Championship titles page (the game: title_mods.cpp, docs/TITLE_MODS.md): one of the game's titles with a new name,
// new pictures and a new look for its belt. The mod (manifest.txt type=title, textures\) is made by
// tools/title_port.py; it installs into <game>\Mods\Titles\<id>.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "app.h"
#include "json.h"
#include "mod_check.h"
#include "tool_run.h"

namespace mm {
namespace title_page {

using namespace svrfmt;

namespace {

struct TitlePack {
  std::string key;  // IC, US, ... (title_port titles)
  char name[120] = "", champion[120] = "", author[64] = "", version[16] = "1.0";
  std::wstring art, picture;
  std::map<std::string, std::wstring> belt;  // belt texture -> picture
  std::vector<ZipEntry> extra;               // an opened mod's other files (a belt model's pack.txt ...): kept
};
TitlePack g_p;

Json g_titles;               // title_port titles
bool g_titles_asked = false;
std::string g_titles_error;
bool g_stock_asked = false;  // the belts' stock textures as PNGs (for the thumbnails)
std::map<std::wstring, Picture> g_thumbs;

// --title-test <file>: lines title= name= champion= art= picture= set=<texture>=<picture>; --test-title-save <file>
// (.svrmod, or "install": into the game folder): made once, then the Mod Maker closes.
std::wstring g_test_spec, g_test_save;

fs::path WorkDir(const char* sub) {
  std::error_code ec;
  const fs::path d = fs::temp_directory_path(ec) / L"SvR2011 Mod Maker titles" / sub;
  fs::create_directories(d, ec);
  return d;
}
fs::path StockDir() { return WorkDir("stock"); }

const Json* TitleRow(const std::string& key) {
  for (const auto& t : g_titles.items)
    if (t["key"].Str() == key) return &t;
  return nullptr;
}

void AskTitles() {
  if (g_titles_asked || Busy() || PythonExe().empty() || ToolsDir().empty()) return;
  g_titles_asked = true;
  RunToolJson(L"title_port.py", {L"titles"}, [](int code, const std::string& out) {
    Json j;
    if (code == 0 && ParseJson(out, j) && j.type == Json::Type::kArray) g_titles = std::move(j);
    else g_titles_error = "The titles' list could not be read (see the log).";
  });
}

void AskStock() {  // (once g_titles is there; only thumbnails: without them the page works the same)
  if (g_stock_asked || Busy() || g_titles.items.empty() || g_game.empty()) return;
  g_stock_asked = true;
  RunToolJson(L"belt_port.py", {L"list", L"--game", L"svr2011", L"--src", g_game, L"--pictures", StockDir().wstring()},
              [](int code, const std::string&) {
                if (code != 0) Log("titles: the belts' stock pictures could not be read (exit " + std::to_string(code) + ")");
                g_thumbs.clear();
              });
}

Picture& Thumb(const std::wstring& file) {
  auto it = g_thumbs.find(file);
  if (it != g_thumbs.end()) return it->second;
  Picture& p = g_thumbs[file];
  std::error_code ec;
  Image img;
  if (!file.empty() && fs::exists(file, ec) && LoadPicture(file, img)) p.Set(std::move(img));
  return p;
}

void DrawThumb(const std::wstring& file, float w, float h) {
  ImGui::BeginGroup();  // (Picture::Draw ends on a new line: a following SameLine starts at the picture's top)
  Picture& p = Thumb(file);
  if (p.Empty()) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::Dummy(ImVec2(w, h));
  } else {
    p.Draw(w, h);
  }
  ImGui::EndGroup();
}

// The name without its "ship", as the game does (title_mods.cpp): what the holder is called without champion=.
std::string DefaultChampion() {
  const std::string n = g_p.name;
  if (n.empty()) return {};
  const std::string up = Upper(n);
  return up.size() > 4 && up.compare(up.size() - 4, 4, "SHIP") == 0 ? n.substr(0, n.size() - 4) : n + " Champion";
}

std::string PackId() { return IdFrom(std::string(g_p.name[0] ? g_p.name : "title") + "_" + g_p.key, "title"); }

void PickInto(std::wstring& into, const wchar_t* title) {
  const std::wstring f = PickFile(false, title, kPictureFilter, 1);
  if (f.empty()) return;
  into = f;
  Touch();
}

// A picture row: the thumbnail, Choose / Clear.
void PictureRow(const char* id, std::wstring& file, const wchar_t* dialog, float w, float h) {
  ImGui::PushID(id);
  DrawThumb(file, w, h);
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (ImGui::Button("Choose...", ImVec2(110 * g_scale, 0))) PickInto(file, dialog);
  if (!file.empty()) {
    if (ImGui::SmallButton("Clear")) file.clear(), Touch();
    ImGui::TextDisabled("%s", Utf8(fs::path(file).filename().wstring()).c_str());
  }
  ImGui::EndGroup();
  ImGui::PopID();
}

void Problems(std::vector<Problem>& p) {
  if (g_p.key.empty()) {
    p.push_back(Error("Pick the title to replace."));
    return;
  }
  if (!g_p.name[0] && g_p.art.empty() && g_p.picture.empty() && g_p.belt.empty())
    p.push_back(Error("Nothing to replace yet: give the title a new name, a picture or a belt texture."));
  const Json* t = TitleRow(g_p.key);
  if (!t) return;
  if (!g_p.picture.empty() && !(*t)["pictures"].items.empty()) {
    const Json& s = (*t)["pictures"].items[0];
    const Picture& pic = Thumb(g_p.picture);
    const int w = int(s["width"].n), h = int(s["height"].n);
    if (!pic.Empty() && w && h && pic.image.w * h != pic.image.h * w)
      p.push_back(Warning("The belt picture is " + std::to_string(pic.image.w) + " x " + std::to_string(pic.image.h) + "; the game's is " +
                              std::to_string(w) + " x " + std::to_string(h) + ", so it will look stretched.",
                          "Use a picture of the same shape."));
  }
  for (const auto& b : (*t)["belt"].items)
    if (b["shared"].b && g_p.belt.count(b["name"].Str()))
      p.push_back(Warning("The belt texture " + b["name"].Str() + " is used by other titles' belts too: they change with it."));
}

// title_port make into a work folder, its files handed on (UI thread).
void Build(std::function<void(std::vector<ZipEntry>)> done) {
  if (!ConvertersReady()) return;
  std::error_code ec;
  const fs::path out = WorkDir("build");
  fs::remove_all(out, ec);
  fs::create_directories(out, ec);
  const std::string made = MadeWith();
  const size_t eq = made.find('=');
  std::vector<std::wstring> args = {L"make", L"--title", Wide(g_p.key), L"--out", out.wstring(), L"--id", Wide(PackId()),
                                    L"--made-with", Wide(made.substr(eq + 1, made.find('\n') - eq - 1))};
  const auto add = [&](const wchar_t* flag, const std::wstring& v) {
    if (!v.empty()) args.push_back(flag), args.push_back(v);
  };
  add(L"--name", Wide(g_p.name));
  add(L"--champion", Wide(g_p.champion));
  add(L"--author", Wide(g_p.author));
  add(L"--version", Wide(g_p.version));
  add(L"--art", g_p.art);
  add(L"--picture", g_p.picture);
  for (const auto& [tex, pic] : g_p.belt) add(L"--set", Wide(tex) + L"=" + pic);
  const std::string project = ModProjectText(PageId::kTitle);
  const std::vector<ZipEntry> extra = g_p.extra;
  RunToolJson(L"title_port.py", args, [done, project, extra](int code, const std::string& text) {
    Json j;
    ParseJson(text, j);
    if (code != 0 || !j["ok"].b) {
      Status("The title mod could not be made: " + (j["error"].Str().empty() ? std::string("see the log") : j["error"].Str()));
      return;
    }
    std::vector<ZipEntry> files;
    const fs::path mod = Wide(j["out"].Str());
    std::error_code ec2;
    for (fs::recursive_directory_iterator it(mod, ec2), end; it != end && !ec2; it.increment(ec2)) {
      if (!it->is_regular_file(ec2)) continue;
      Bytes d;
      if (!ReadFile(PathStr(it->path()), d)) {
        Status("The title mod could not be read back from " + PathStr(it->path()));
        return;
      }
      files.push_back({Utf8(fs::relative(it->path(), mod, ec2).generic_wstring()), std::move(d)});
    }
    for (const auto& e : extra) files.push_back(e);
    AddModProject(files, project);
    for (const auto& w : j["warnings"].items) Log("titles: " + w.Str());
    done(std::move(files));
  });
}

void SavePack() {
  const std::wstring f = PickFile(true, L"Save the title mod", kModFilter, 1, L"svrmod", (Wide(PackId()) + L".svrmod").c_str());
  if (f.empty()) return;
  Build([f](std::vector<ZipEntry> files) {
    if (WriteFile(Utf8(f), ZipWrite(files))) Status("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
    else Status("The title mod could not be written.");
  });
}

void InstallFiles(const std::vector<ZipEntry>& files, const std::string& id) {
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Titles" / fs::u8path(id);
  std::string why;
  if (!InstallModFiles(dir, files, &why)) {
    Status("Not installed: " + why);
    return;
  }
  Status("Installed: the game shows the title's new name, belt and pictures from the next game start (" + PathStr(dir) + ")." + InstallReport("Titles", dir));
}

void InstallPack() {
  const std::string id = PackId();
  Build([id](std::vector<ZipEntry> files) { InstallFiles(files, id); });
}

void TestRun() {
  if (g_test_spec.empty() || g_titles.items.empty() || Busy()) return;
  Bytes d;
  const std::wstring spec = g_test_spec;
  g_test_spec.clear();
  if (!ReadFile(Utf8(spec), d)) {
    Log("test: no title spec");
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    return;
  }
  const std::string s(d.begin(), d.end());
  for (size_t at = 0; at < s.size();) {
    size_t e = s.find('\n', at);
    if (e == std::string::npos) e = s.size();
    std::string line = s.substr(at, e - at);
    at = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "title") g_p.key = v;
    else if (k == "name") std::snprintf(g_p.name, sizeof g_p.name, "%s", v.c_str());
    else if (k == "champion") std::snprintf(g_p.champion, sizeof g_p.champion, "%s", v.c_str());
    else if (k == "art") g_p.art = Wide(v);
    else if (k == "picture") g_p.picture = Wide(v);
    else if (k == "set" && v.find('=') != std::string::npos) g_p.belt[v.substr(0, v.find('='))] = Wide(v.substr(v.find('=') + 1));
  }
  Touch();
  if (g_test_save.empty()) return;  // (only filled in: for a shot of the page)
  const std::wstring save = g_test_save;
  g_test_save.clear();
  const std::string id = PackId();
  Build([save, id](std::vector<ZipEntry> files) {
    if (save == L"install") InstallFiles(files, id);
    else if (WriteFile(Utf8(save), ZipWrite(files))) Log("test: saved " + Utf8(save));
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  });
}

void Draw() {
  AskTitles();
  AskStock();
  TestRun();
  Heading("Championship titles", "One of the game's titles with a new name, new pictures and a new look for its belt. Holders and "
                                 "title matches stay as they are. Your save isn't changed: without the mod the game's title comes back.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("title_body", ImVec2(0, -48 * g_scale), false);
  if (g_titles.items.empty()) {
    if (!g_titles_error.empty()) ImGui::TextWrapped("%s", g_titles_error.c_str());
    else if (PythonExe().empty() || ToolsDir().empty()) {
      ImGui::TextWrapped("This page needs the Mod Maker tools (Python).");
      if (ImGui::Button("Open Mod Maker tools")) OpenToolsPanel();
    } else ImGui::TextDisabled("Reading the titles...");
    ImGui::EndChild();
    return;
  }
  const Json* row = TitleRow(g_p.key);
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::BeginCombo("Title to replace", row ? (*row)["name"].Str().c_str() : "Pick one")) {
    for (const auto& t : g_titles.items) {
      const std::string k = t["key"].Str();
      if (ImGui::Selectable(t["name"].Str().c_str(), k == g_p.key) && k != g_p.key) {
        g_p.key = k;
        g_p.belt.clear();  // (each belt has its own textures)
        Touch();
      }
    }
    ImGui::EndCombo();
  }
  row = TitleRow(g_p.key);
  if (!row) {
    ImGui::Spacing();
    DrawProblems(problems);
    ImGui::EndChild();
    ModButtons(problems, false);
    return;
  }
  ImGui::Spacing();
  TextField("New name", g_p.name, sizeof g_p.name, (*row)["name"].Str().c_str());
  Hint("The title's name wherever the game writes it. Empty: the game's name stays.");
  const std::string def = DefaultChampion();
  TextField("The holder is called", g_p.champion, sizeof g_p.champion, def.empty() ? "(the game's)" : def.c_str());
  Hint("\"... CHAMPION\" on the Universe rankings. Empty: the new name without its \"ship\".");
  TextField("Author (optional)", g_p.author, sizeof g_p.author);
  TextField("Version (optional)", g_p.version, sizeof g_p.version, "1.0");
  ImGui::SeparatorText("Pictures");
  if (!(*row)["art"].items.empty()) {
    ImGui::TextUnformatted("Championship art (beside the holder: Championship Scramble, Universe rankings)");
    PictureRow("art", g_p.art, L"The championship art", 200 * g_scale, 112 * g_scale);
  } else {
    ImGui::TextDisabled("The game has no championship art for this title.");
  }
  ImGui::Spacing();
  if (!(*row)["pictures"].items.empty()) {
    const Json& s = (*row)["pictures"].items[0];
    ImGui::Text("Belt picture (%s x %s)", s["width"].Str().c_str(), s["height"].Str().c_str());
    const float w = 320 * g_scale;
    PictureRow("picture", g_p.picture, L"The belt picture", w, w * float(s["height"].n) / std::max(1.0f, float(s["width"].n)));
  }
  ImGui::SeparatorText("The belt's look");
  const auto& belt = (*row)["belt"].items;
  if (belt.empty()) {
    ImGui::TextDisabled("This title's belt has no textures to replace: its name and pictures can change.");
  } else {
    ImGui::TextWrapped("The belt in entrances, celebrations, cut scenes and when picked up in a match. Left: the game's texture. "
                       "Right: yours (any size; the same shape looks best).");
    const float t = 96 * g_scale;
    for (const auto& b : belt) {
      const std::string n = b["name"].Str();
      ImGui::PushID(n.c_str());
      DrawThumb((StockDir() / fs::u8path(g_p.key) / fs::u8path(n + ".png")).wstring(), t, t);
      ImGui::SameLine();
      auto it = g_p.belt.find(n);
      std::wstring file = it == g_p.belt.end() ? std::wstring() : it->second;
      DrawThumb(file, t, t);
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::Text("%s  (%s x %s)", n.c_str(), b["width"].Str().c_str(), b["height"].Str().c_str());
      if (b["shared"].b) ImGui::TextDisabled("Other titles' belts use this one too.");
      if (ImGui::Button("Choose...", ImVec2(110 * g_scale, 0))) {
        PickInto(file, L"A belt texture");
        if (!file.empty()) g_p.belt[n] = file;
      }
      if (it != g_p.belt.end()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
          g_p.belt.erase(n);
          Touch();
        }
      }
      ImGui::EndGroup();
      ImGui::PopID();
    }
  }
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, false)) {
    case 1: SavePack(); break;
    case 2: InstallPack(); break;
  }
}

std::string StateText() {
  std::string s = "title=" + g_p.key + "\nname=" + g_p.name + "\nchampion=" + g_p.champion + "\nauthor=" + g_p.author + "\nversion=" + g_p.version +
                  "\nart=" + Utf8(g_p.art) + "\npicture=" + Utf8(g_p.picture) + "\n";
  for (const auto& [k, v] : g_p.belt) s += "belt." + k + "=" + Utf8(v) + "\n";
  return s;
}

void Reset() { g_p = TitlePack(); }

void WriteProject(ProjectOut& out) {
  out.Key("title", g_p.key);
  out.Key("name", g_p.name);
  out.Key("champion", g_p.champion);
  out.Key("author", g_p.author);
  out.Key("version", g_p.version);
  out.File("art", g_p.art, "title/art");
  out.File("picture", g_p.picture, "title/picture");
  int i = 0;
  for (const auto& [k, v] : g_p.belt) {
    out.Key("belt_name" + std::to_string(i), k);
    out.File("belt" + std::to_string(i), v, "title/belt" + std::to_string(i));
    ++i;
  }
}

void ReadKeys(const ProjectIn& in) {
  g_p.key = in.Get("title");
  std::snprintf(g_p.name, sizeof g_p.name, "%s", in.Get("name").c_str());
  std::snprintf(g_p.champion, sizeof g_p.champion, "%s", in.Get("champion").c_str());
  std::snprintf(g_p.author, sizeof g_p.author, "%s", in.Get("author").c_str());
  std::snprintf(g_p.version, sizeof g_p.version, "%s", in.Get("version").c_str());
  if (!g_p.version[0]) std::snprintf(g_p.version, sizeof g_p.version, "1.0");
}

bool ReadProject(const ProjectIn& in) {
  ReadKeys(in);
  if (!in.Get("art").empty()) g_p.art = in.Extract(in.Get("art"));
  if (!in.Get("picture").empty()) g_p.picture = in.Extract(in.Get("picture"));
  for (int i = 0; i < 999; ++i) {
    const std::string n = std::to_string(i), k = in.Get(("belt_name" + n).c_str()), f = in.Get(("belt" + n).c_str());
    if (k.empty()) break;
    if (!f.empty()) g_p.belt[k] = in.Extract(f);
  }
  return true;
}

// A mod back into the page: textures\<owner>_<name>_<hash16>.<ext>.
bool ReadMod(const ProjectIn& in) {
  ReadKeys(in);
  for (const auto& e : in.files) {
    if (e.name == "manifest.txt" || e.name == ".mm_project.txt") continue;
    if (e.name.rfind("textures/", 0) != 0) {
      g_p.extra.push_back(e);
      continue;
    }
    const std::string stem = Utf8(fs::u8path(e.name).stem().wstring());
    const size_t a = stem.find('_'), b = stem.rfind('_');
    if (a == std::string::npos || b <= a) continue;
    const std::string owner = stem.substr(0, a), name = stem.substr(a + 1, b - a - 1);
    if (owner == "menuSD") continue;  // (the menuHD copy gives the picture)
    if (name.rfind("BG_", 0) == 0) g_p.art = in.Extract(e.name);
    else if (name.rfind("belt_", 0) == 0) g_p.picture = in.Extract(e.name);
    else g_p.belt[name] = in.Extract(e.name);
  }
  return true;
}

}  // namespace

void TestStart(const std::wstring& spec, const std::wstring& save) {
  g_test_spec = spec;
  g_test_save = save;
}

PageHooks hooks = {"title", Draw, Problems, Reset, StateText, WriteProject, ReadProject, ReadMod};

}  // namespace title_page
}  // namespace mm
