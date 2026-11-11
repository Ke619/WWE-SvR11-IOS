// The install check: before anything reads the game's files, are they on the SD card where the NSP looks for them
// (sdmc:/SvR2011)? A missing or partial copy otherwise crashes somewhere deep in a load. On a failure: a plain text
// screen (what is missing, where it looked, what to do) until + is pressed, the same list in
// SvR2011/logs/nx_install_check.txt (and a line in nx_crash.txt), then the game closes.
//
// It looks at the card only (sdmc:/ paths): the file wrappers in nx_main.cpp would also find the NSP's own files.

#include <dirent.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr const char* kCard = "sdmc:/SvR2011";

struct Need {
  const char* path;   // under SvR2011
  bool folder;        // a folder that must hold at least one file
  long long min_size; // a file's smallest plausible size
};

// What the game needs to start. Others (DLC, pad_icons, nxeart, the shader pack) the game copes without.
const Need kNeeds[] = {
    {"default.xex", false, 1 << 20},
    {"plist360.arc", false, 64 << 10},
    {"pac", true, 0},
    {"pac/_startup.pac", false, 64 << 10},
    {"pac/menu", true, 0},
    {"sound", true, 0},
    {"sound/Game.pck", false, 1 << 20},
    {"movies", true, 0},
};

bool HasAFile(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return false;
  bool found = false;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    found = true;
    break;
  }
  closedir(d);
  return found;
}

std::vector<std::string> Check() {
  std::vector<std::string> missing;
  struct stat st = {};
  if (stat(kCard, &st) != 0 || !S_ISDIR(st.st_mode)) {
    missing.push_back("the SvR2011 folder itself");
    return missing;
  }
  for (const Need& n : kNeeds) {
    const std::string p = std::string(kCard) + "/" + n.path;
    if (stat(p.c_str(), &st) != 0) {
      missing.push_back(std::string(n.path) + (n.folder ? "/ (missing)" : " (missing)"));
    } else if (n.folder) {
      if (!S_ISDIR(st.st_mode) || !HasAFile(p)) missing.push_back(std::string(n.path) + "/ (empty)");
    } else if (!S_ISREG(st.st_mode) || st.st_size < n.min_size) {
      missing.push_back(std::string(n.path) + " (incomplete: " + std::to_string((long long)st.st_size) + " bytes)");
    }
  }
  // Saves: the game must be able to write there.
  const std::string saves = std::string(kCard) + "/Saves", probe = saves + "/.install_check";
  mkdir(saves.c_str(), 0777);
  if (FILE* f = fopen(probe.c_str(), "wb")) {
    const bool wrote = fputs("ok", f) >= 0;
    fclose(f);
    unlink(probe.c_str());
    if (!wrote) missing.push_back("Saves/ (can't be written)");
  } else {
    missing.push_back("Saves/ (can't be written: is the card locked or full?)");
  }
  return missing;
}

void Log(const std::vector<std::string>& missing) {
  mkdir(kCard, 0777);
  mkdir("sdmc:/SvR2011/logs", 0777);
  if (FILE* f = fopen("sdmc:/SvR2011/logs/nx_install_check.txt", "w")) {
    if (missing.empty()) {
      fprintf(f, "install check: OK (%s)\n", kCard);
    } else {
      fprintf(f, "install check: FAILED, looked in %s\n", kCard);
      for (const std::string& m : missing) fprintf(f, "  missing: %s\n", m.c_str());
    }
    fclose(f);
  }
  if (missing.empty()) return;
  if (FILE* f = fopen("sdmc:/SvR2011/logs/nx_crash.txt", "a")) {
    fprintf(f, "install check failed (%zu problems, first: %s) - see logs/nx_install_check.txt\n", missing.size(),
            missing[0].c_str());
    fclose(f);
  }
}

void Show(const std::vector<std::string>& missing) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  printf("\n  SvR 2011 isn't installed correctly\n");
  printf("  ==================================\n\n");
  printf("  The game looked for its files in:\n    %s\n\n", "SD card: /SvR2011");
  printf("  Missing or incomplete:\n");
  const size_t shown = missing.size() < 8 ? missing.size() : 8;
  for (size_t i = 0; i < shown; i++) printf("    - %s\n", missing[i].c_str());
  if (missing.size() > shown) printf("    ... and %zu more (SvR2011/logs/nx_install_check.txt)\n", missing.size() - shown);
  printf("\n  What to do:\n");
  printf("    On the PC launcher, open Setup -> Switch and click\n");
  printf("    Make Switch files, with the SD card connected.\n");
  printf("    Copy everything to the top of the card, so the card has\n");
  printf("    a SvR2011 folder. See docs/SWITCH_INSTALL.md.\n\n");
  printf("  Press + (or HOME) to close.\n");
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
}

}  // namespace

// True when the game may start. (C linkage: called from userAppInit, inside nx_main.cpp's extern "C".)
extern "C" bool NxInstallCheck() {
  const std::vector<std::string> missing = Check();
  Log(missing);
  if (missing.empty()) return true;
  Show(missing);
  return false;
}
