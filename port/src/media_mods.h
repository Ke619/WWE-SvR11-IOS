// Media mods (arenas branch): packs that replace the game's superstars'
// entrance videos, entrance themes and renders, arena screen pictures, and
// sounds / menu music. See media_mods.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Before the Custom Movies folder is mounted (its device lists the files
// once): the packs' entrance videos are copied into it.
void CopyMediaMovies(const std::filesystem::path& movies);

// After the user music / movies and the arena mods, before the superstar
// mods (it writes its overlay pac for them to mount).
void InstallMediaMods(rex::memory::Memory* memory);

// The virtual file the game asked for, if a pack replaces it: the guest
// address of the replacement's name (superstar_mods.cpp's resolver hook).
uint32_t MediaAlias(const char* name);

// An audio event the game posts (name, game object): true if a pack replaces
// it (played on a host voice; *playing_id what to return) - else the game's.
// Stop / pause events update the host voices and return false.
bool MediaEvent(uint8_t* base, const char* name, uint32_t object, uint32_t* playing_id);

// Victory tracks (docs/VICTORY_SCENES.md 5): a superstar's own music for the win, in place of his theme.
// A superstar mod's victory_song= (its id, once it has one; a media pack's victory_theme.<id>= comes in itself).
void SetVictoryTrack(uint32_t id, const std::filesystem::path& file, float gain_db);
// The match's result is in (replays.cpp: the match's end task): the music the game starts from now on is the win's.
void WinMusicOn();
// A new match sets up, or the menus' music: that's over.
void WinMusicOff();
bool WinMusicNow();
// The win's song is this USER PLAYLIST: true if its owner has a track (*file, *gain_db).
bool VictoryTrackOfPlaylist(const std::u16string& playlist, std::filesystem::path* file, float* gain_db);
namespace detail {
void NoteMatchPerson(const uint8_t* base, uint32_t record, uint32_t desc);
}

}  // namespace svr2011
