// WWE SmackDown vs. Raw 2011 - saves as plain files (saves.h).

#include "saves.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace svr2011 {

namespace {

namespace fs = std::filesystem;

constexpr const char* kTitleDir = "5451085D";
constexpr const char* kSavedGameDir = "00000001";

bool IsProfileDir(const fs::path& p) {
  const std::string name = p.filename().string();
  if (name.size() != 16 || name == "0000000000000000") return false;
  for (char c : name)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// One package folder -> <saves>\<name> (its single file) or, with more files,
// the folder itself; header and thumbnail -> <saves>\.info.
void MovePackage(const fs::path& package, const fs::path& header, const fs::path& saves) {
  const std::string name = package.filename().string();
  const fs::path info = saves / ".info";
  std::error_code ec;
  if (fs::exists(saves / name)) {
    REXLOG_WARN("[svr2011] saves: {} is already in {}; the old copy stays in {}", name,
                saves.string(), package.string());
    return;
  }
  std::vector<fs::path> files;
  bool dirs = false;
  for (const auto& e : fs::directory_iterator(package, ec)) {
    if (e.is_directory()) {
      dirs = true;
    } else if (e.path().filename() == "__thumbnail.png") {
      fs::rename(e.path(), info / (name + ".png"), ec);
    } else {
      files.push_back(e.path());
    }
  }
  if (files.size() == 1 && !dirs) {
    fs::rename(files[0], saves / name, ec);
    if (ec) {
      REXLOG_WARN("[svr2011] saves: could not move {} ({})", files[0].string(), ec.message());
      return;
    }
    const std::string inner = files[0].filename().string();
    if (inner != "SaveData.Dat") {
      if (FILE* f = std::fopen((info / (name + ".file")).string().c_str(), "wb")) {
        std::fwrite(inner.data(), 1, inner.size(), f);
        std::fclose(f);
      }
    }
    fs::remove(package, ec);
  } else {
    fs::rename(package, saves / name, ec);
    if (ec) {
      REXLOG_WARN("[svr2011] saves: could not move {} ({})", package.string(), ec.message());
      return;
    }
  }
  if (fs::exists(header)) fs::rename(header, info / (name + ".header"), ec);
  REXLOG_INFO("[svr2011] saves: moved {} to {}", name, saves.string());
}

// The display name the game gives a save file ("00CreateSuperStar.cas" ->
// "01.CREATED SUPERSTAR"). Replays and highlight reels are named by the
// player; a copy that lost theirs gets a numbered one.
std::string DisplayName(const std::string& file) {
  if (file == "SaveData.dat") return "WWE SmackDown vs. RAW 2011";
  static const struct {
    const char* suffix;
    const char* name;
  } kKinds[] = {{"CreateSuperStar.cas", "CREATED SUPERSTAR"},
                {"PaintTool.pt", "PAINT TOOL"},
                {"RecordingDat.rec", "REPLAY"},
                {"SceneDat.scn", "HIGHLIGHT REEL"}};
  for (const auto& k : kKinds) {
    const std::string suffix = k.suffix;
    if (file.size() == suffix.size() + 2 && file.compare(2, std::string::npos, suffix) == 0 &&
        std::isdigit(static_cast<unsigned char>(file[0])) &&
        std::isdigit(static_cast<unsigned char>(file[1]))) {
      char name[64];
      std::snprintf(name, sizeof(name), "%02d.%s", std::stoi(file.substr(0, 2)) + 1, k.name);
      return name;
    }
  }
  return file;
}

// Saves copied without their .info headers (copy tools that skip hidden or
// dot folders, e.g. to a Steam Deck): the game then reports the Created
// Superstar data as damaged, as it finds them by their display names. The
// headers are rebuilt (XCONTENT_AGGREGATE_DATA, as the runtime writes them).
void RestoreHeaders(const fs::path& saves) {
  std::error_code ec;
  const fs::path info = saves / ".info";
  fs::create_directories(info, ec);
#if defined(__SWITCH__)
  std::vector<fs::path> empties;
#endif
  for (const auto& e : fs::directory_iterator(saves, ec)) {
    const std::string file = e.path().filename().string();
    if (file.empty() || file[0] == '.') continue;
    const fs::path header = info / (file + ".header");
#if defined(__SWITCH__)
    // An empty save is one an early Switch build lost; the game would call it damaged.
    // (Here, before the game lists its saves.)
    if (e.is_regular_file(ec) && e.file_size(ec) == 0) {
      fs::path gone = e.path();
      empties.push_back(gone);
      fs::remove(header, ec);
      continue;
    }
#endif
    if (fs::exists(header)) continue;
    uint8_t h[328] = {};
    h[3] = 1;  // device_id
    h[7] = 1;  // content_type: saved game
    const std::string name = DisplayName(file);
    for (size_t i = 0; i < name.size() && i < 127; ++i) h[8 + i * 2 + 1] = uint8_t(name[i]);
    for (size_t i = 0; i < file.size() && i < 41; ++i) h[264 + i] = uint8_t(file[i]);
    const uint8_t title[4] = {0x54, 0x51, 0x08, 0x5D};  // xuid (312) 0, title_id (320)
    std::memcpy(h + 320, title, 4);
    if (FILE* f = std::fopen(header.string().c_str(), "wb")) {
      std::fwrite(h, 1, sizeof(h), f);
      std::fclose(f);
      REXLOG_INFO("[svr2011] saves: rebuilt the missing header of {} (\"{}\")", file, name);
    }
  }
#if defined(__SWITCH__)
  for (const fs::path& p : empties) {
    fs::remove(p, ec);
    REXLOG_INFO("[svr2011] saves: removed the empty save {}", p.filename().string());
  }
#endif
}

}  // namespace

void UseFlatSaves(rex::system::KernelState* kernel_state, const fs::path& user_data,
                  const fs::path& saves) {
  std::error_code ec;
  fs::create_directories(saves / ".info", ec);
  for (const auto& profile : fs::directory_iterator(user_data, ec)) {
    if (!profile.is_directory() || !IsProfileDir(profile.path())) continue;
    const fs::path packages = profile.path() / kTitleDir / kSavedGameDir;
    const fs::path headers = profile.path() / kTitleDir / "Headers" / kSavedGameDir;
    std::vector<fs::path> list;
    for (const auto& p : fs::directory_iterator(packages, ec))
      if (p.is_directory()) list.push_back(p.path());
    for (const auto& p : list) MovePackage(p, headers / (p.filename().string() + ".header"), saves);
  }
  RestoreHeaders(saves);
  if (kernel_state && kernel_state->content_manager()) {
    kernel_state->content_manager()->SetFlatSavesRoot(saves);
    REXLOG_INFO("[svr2011] saves: {}", saves.string());
  }
}

}  // namespace svr2011
