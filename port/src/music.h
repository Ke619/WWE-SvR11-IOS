// WWE SmackDown vs. Raw 2011 - user music for entrances.
//
// On the Xbox 360, Create An Entrance -> Music -> USER PLAYLIST let a player
// use a song from the console's music library as a Superstar's entrance
// music: the game lists the library's playlists through the Xbox music player
// (XMP) and has it play the chosen one during the entrance. Here the library
// is the "Music" folder beside the game: every subfolder is a playlist named
// after it (put one song in each, as the game asks), and a song placed
// directly in the folder is a playlist of its own, named after the file.
// .mp3, and anything else Media Foundation decodes (.wma, .m4a, .aac, .wav,
// .flac). The folder is read each time the game asks, so songs can be added
// while it runs.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace svr2011 {

// Serves `folder` to the game's music player (xmp_app.h, HostMusic). Call once.
void InstallUserMusic(const std::filesystem::path& folder);

// The Music folder the playlists come from (SVR2011_MUSIC or <game>/Music).
std::filesystem::path UserMusicFolder();

// The song a playlist plays: the first song of folder <name>, or the loose
// file whose stem is <name> (as USER PLAYLIST lists them); empty if none.
std::filesystem::path UserMusicSong(const std::string& name);

// Every song in the Music folder and its subfolders (paths relative to the
// folder, sorted): MY WWE -> JUKEBOX -> MY MUSIC (jukebox.h).
std::vector<std::filesystem::path> UserMusicSongs();

// A song file's gain in dB (a mod's theme_gain=, -20..+6): applied while the
// entrance player plays that file (by its file name; 0 removes it).
void SetSongGain(const std::filesystem::path& file, float db);

// Plays a short sound file once on its own voice (any format the songs can
// be; honours audio_mute) - a superstar mod's recorded name call.
// A short clip, once, on its own player; `db`: its level (0 = the file's own).
void PlayClip(const std::filesystem::path& file, float db = 0.0f);

// Host sounds (media mods' replacements of the game's sounds): Start gives a
// handle (-1: none free), each on its own voice; Active while playing or
// paused (a looping one until stopped). Volume 0..1 (audio_mute honoured).
int HostSoundStart(const std::filesystem::path& file, bool loop, float volume, float gain_db = 0.0f);
void HostSoundStop(int handle);
void HostSoundPause(int handle, bool paused);
void HostSoundVolume(int handle, float volume);
bool HostSoundActive(int handle);

}  // namespace svr2011
