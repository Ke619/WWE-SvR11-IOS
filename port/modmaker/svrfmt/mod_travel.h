// WWE SmackDown vs. Raw 2011 - which mod files may travel online, host to joiner (the game's mod_transfer.cpp)
// and what the Mod Maker says about a mod (installed_page.cpp): one check, so the two never disagree.
// No game or platform code: the game, its Android build and the Mod Maker all compile it.
#pragma once

#include <filesystem>
#include <string>

namespace svrfmt {

// A path inside a mod folder ("moves/motions/x.ymk"; '/' between parts) that may travel: allowed kinds only,
// no "..", no absolute or drive paths, no ':' (streams), no device names (CON, NUL ...); .bin only under
// a moves/ or entrance/ folder, .toml / .ini only as the manifest. (why: the reason, for the log / the player.)
bool ModFileAllowed(const std::string& rel, std::string* why = nullptr);

// A file outside the mod's key (pictures, sounds, movies and the like; this PC's own .applied / disabled): the
// only kind left behind when it can't travel - the joiner's copy still matches the host's. The game's mod set
// (online_files.cpp) keys mods by the other files.
bool CosmeticFile(const std::filesystem::path& file);

// A manifest's text as it counts for the mod's key: without what each PC writes for itself - the launcher's music level
// at install (theme_gain=, and theme_gain_auto= holding the song's size and THIS PC's file time) and its movie shrink
// (movie=, video.<id>=: pictures, sounds and movies are out of the key anyway), a victory track - and without line-end differences or
// blank lines. The same mod on two PCs has the same key.
std::string ManifestKeyText(const std::string& manifest);

// A whole mod folder: true if it can be sent to players who join; else unsendable = the first file that
// can't ("tool.exe (a file type that isn't sent: ...)"). The game's host decides with this too.
bool ModCanTravel(const std::filesystem::path& dir, std::string* unsendable = nullptr);

}  // namespace svrfmt
