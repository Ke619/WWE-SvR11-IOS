// Create A Superstar glasses from an OBJ and a PNG (the game: caw_parts.cpp, docs/CAW_PARTS.md). The mod (manifest.txt
// type=cawpart) is made by tools/caw_part.py; it installs into <game>\Mods\CawParts\<id>.
#include <cstdio>
#include <filesystem>

#include "app.h"
#include "json.h"
#include "tool_run.h"

namespace mm {
namespace caw_part_page {

using namespace svrfmt;

namespace {

struct Part {
  char name[64] = "";
  std::wstring obj, png;
};
Part g_p;

// --caw-part-test <file>: lines obj= png= name=; --test-caw-part-save <.svrmod, or "install">: made once, then it closes.
std::wstring g_test_spec, g_test_save;

const COMDLG_FILTERSPEC kObjFilter[1] = {{L"OBJ model (*.obj)", L"*.obj"}};

std::string PartId() { return IdFrom(g_p.name[0] ? g_p.name : "glasses", "glasses"); }

fs::path WorkDir() {
  std::error_code ec;
  const fs::path d = fs::temp_directory_path(ec) / L"SvR2011 Mod Maker CAW parts" / L"build";
  fs::create_directories(d, ec);
  return d;
}

void Problems(std::vector<Problem>& p) {
  std::error_code ec;
  if (g_game.empty()) p.push_back(Error("Making a part reads a stock pair of glasses: set the game folder first."));
  if (!g_p.name[0]) p.push_back(Error("Give the glasses a name."));
  if (g_p.obj.empty()) p.push_back(Error("Pick the OBJ model."));
  else if (!fs::exists(g_p.obj, ec)) p.push_back(Error("The OBJ model isn't there any more: " + Utf8(g_p.obj)));
  if (g_p.png.empty()) p.push_back(Error("Pick the picture."));
  else if (!fs::exists(g_p.png, ec)) p.push_back(Error("The picture isn't there any more: " + Utf8(g_p.png)));
}

// caw_part make into a work folder, its files handed on (UI thread).
void Build(std::function<void(std::vector<ZipEntry>)> done) {
  if (!ConvertersReady()) return;
  std::error_code ec;
  const fs::path out = WorkDir() / fs::u8path(PartId());
  fs::remove_all(out, ec);
  const std::string made = MadeWith();
  const size_t eq = made.find('=');
  const std::vector<std::wstring> args = {L"make", L"--game-pac", (fs::path(g_game) / L"pac").wstring(), L"--obj", g_p.obj, L"--png", g_p.png,
                                          L"--out", out.wstring(), L"--id", Wide(PartId()), L"--name", Wide(g_p.name),
                                          L"--made-with", Wide(made.substr(eq + 1, made.find('\n') - eq - 1))};
  const std::string project = ModProjectText(PageId::kCawPart);
  RunToolJson(L"caw_part.py", args, [done, project, out](int code, const std::string& text) {
    Json j;
    ParseJson(text, j);
    if (code != 0 || !j["ok"].b) {
      Status("The glasses could not be made: " + (j["error"].Str().empty() ? std::string("see the log") : j["error"].Str()));
      return;
    }
    std::vector<ZipEntry> files;
    std::error_code ec2;
    for (fs::directory_iterator it(out, ec2), end; it != end && !ec2; it.increment(ec2)) {
      if (!it->is_regular_file(ec2)) continue;
      Bytes d;
      if (!ReadFile(PathStr(it->path()), d)) {
        Status("The glasses could not be read back from " + PathStr(it->path()));
        return;
      }
      files.push_back({Utf8(it->path().filename().wstring()), std::move(d)});
    }
    AddModProject(files, project);
    done(std::move(files));
  });
}

void InstallFiles(const std::vector<ZipEntry>& files, const std::string& id) {
  const fs::path dir = fs::path(g_game) / L"Mods" / L"CawParts" / fs::u8path(id);
  std::string why;
  if (!InstallModFiles(dir, files, &why)) {
    Status("Not installed: " + why);
    return;
  }
  Status("Installed: the glasses are at the end of Create A Superstar's list from the next game start (" + PathStr(dir) + ").");
}

void SavePack() {
  const std::wstring f = PickFile(true, L"Save the glasses", kModFilter, 1, L"svrmod", (Wide(PartId()) + L".svrmod").c_str());
  if (f.empty()) return;
  Build([f](std::vector<ZipEntry> files) {
    if (WriteFile(Utf8(f), ZipWrite(files))) Status("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
    else Status("The glasses could not be written.");
  });
}

void InstallPack() {
  const std::string id = PartId();
  Build([id](std::vector<ZipEntry> files) { InstallFiles(files, id); });
}

void TestRun() {
  if (g_test_spec.empty() || Busy()) return;
  Bytes d;
  const std::wstring spec = g_test_spec;
  g_test_spec.clear();
  if (!ReadFile(Utf8(spec), d)) {
    Log("test: no CAW part spec");
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
    if (k == "obj") g_p.obj = Wide(v);
    else if (k == "png") g_p.png = Wide(v);
    else if (k == "name") std::snprintf(g_p.name, sizeof g_p.name, "%s", v.c_str());
  }
  Touch();
  if (g_test_save.empty()) return;  // (only filled in: for a shot of the page)
  const std::wstring save = g_test_save;
  g_test_save.clear();
  const std::string id = PartId();
  Build([save, id](std::vector<ZipEntry> files) {
    if (save == L"install") InstallFiles(files, id);
    else if (WriteFile(Utf8(save), ZipWrite(files))) Log("test: saved " + Utf8(save));
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  });
}

void Draw() {
  TestRun();
  Heading("CAW glasses", "Your own glasses in Create A Superstar, from an OBJ model and a picture. Glasses only for now.");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("caw_part_body", ImVec2(0, -48 * g_scale), false);
  if (PythonExe().empty() || ToolsDir().empty()) {
    ImGui::TextWrapped("This page needs the Mod Maker tools (Python).");
    if (ImGui::Button("Open Mod Maker tools")) OpenToolsPanel();
    ImGui::Spacing();
  }
  ImGui::SetNextItemWidth(320 * g_scale);
  if (ImGui::InputText("Name", g_p.name, sizeof g_p.name)) Touch();
  ImGui::Spacing();
  if (ImGui::Button("Model (.obj)...", ImVec2(180 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"The glasses (OBJ)", kObjFilter, 1);
    if (!f.empty()) g_p.obj = f, Touch();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(g_p.obj.empty() ? "none" : Utf8(g_p.obj).c_str());
  if (ImGui::Button("Picture...", ImVec2(180 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"The glasses' picture", kPictureFilter, 1);
    if (!f.empty()) g_p.png = f, Touch();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(g_p.png.empty() ? "none" : Utf8(g_p.png).c_str());
  ImGui::Spacing();
  ImGui::TextWrapped("The model's first object is the frame and a second one the lenses. It's made to fit the stock glasses' width, facing the way "
                     "Blender's front view does.");
  ImGui::TextWrapped("The glasses are fixed to the head, so they don't follow the face's shape sliders: they can sit wrong on a heavily reshaped face.");
  ImGui::TextWrapped("The player's two colours multiply the picture. On a game without this mod, a Created Superstar wearing them shows the first stock glasses.");
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, false)) {
    case 1: SavePack(); break;
    case 2: InstallPack(); break;
  }
}

std::string StateText() { return std::string("name=") + g_p.name + "\nobj=" + Utf8(g_p.obj) + "\npng=" + Utf8(g_p.png) + "\n"; }

void Reset() { g_p = Part(); }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_p.name);
  out.File("obj", g_p.obj, "caw/part.obj");
  out.File("png", g_p.png, "caw/part.png");
}

bool ReadProject(const ProjectIn& in) {
  std::snprintf(g_p.name, sizeof g_p.name, "%s", in.Get("name").c_str());
  if (!in.Get("obj").empty()) g_p.obj = in.Extract(in.Get("obj"));
  if (!in.Get("png").empty()) g_p.png = in.Extract(in.Get("png"));
  return true;
}

// (A finished part can't go back into an OBJ and a picture: its project inside the mod can.)
bool ReadMod(const ProjectIn& in) {
  std::snprintf(g_p.name, sizeof g_p.name, "%s", in.Get("name").c_str());
  return true;
}

}  // namespace

void TestStart(const std::wstring& spec, const std::wstring& save) {
  g_test_spec = spec;
  g_test_save = save;
}

PageHooks hooks = {"cawpart", Draw, Problems, Reset, StateText, WriteProject, ReadProject, ReadMod};

}  // namespace caw_part_page
}  // namespace mm
