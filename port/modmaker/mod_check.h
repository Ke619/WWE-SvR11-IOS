// The mod doctor: an installed mod folder (or a .svrmod) checked the way the
// game and the launcher read it - the keys and files each type needs, their
// formats and sizes, the limits - so a mod that won't load says why, in plain
// words with a fix (the rules: port/src superstar_mods / arena_mods /
// move_packs / crowd_signs / media_mods / backgrounds, launcher/mods_tab.c).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "app.h"

namespace mm {

// kind: the Mods sub-folder ("Superstars", "Attires", "Arenas", "Moves", "Backstage", "Signs", "Media",
// "Backgrounds", "MatchTypes").
std::vector<Problem> CheckModFolder(const std::string& kind, const std::filesystem::path& dir);
// The installed mods together: the game's limits (137 superstars, 400 sign-pack signs, 200 movies, 8 backstage rows).
std::vector<Problem> CheckInstalledSet();
// A .svrmod file: unpacked to the cache and checked as its manifest's type says.
std::vector<Problem> CheckSvrmod(const std::wstring& file);
// The Mods sub-folder for a manifest's type= ("" unknown).
std::string KindOfType(const std::string& type);
// After an install: the folder checked, each problem in the log; "" when clean, else a short note for the status line.
std::string InstallReport(const std::string& kind, const std::filesystem::path& dir);

}  // namespace mm
