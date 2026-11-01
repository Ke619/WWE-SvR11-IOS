// WWE SmackDown vs. Raw 2011 - HD entrance movies: a mod's movie= / video.<id>=
// may be an .mp4, played with the platform's own decoder (Windows: Media
// Foundation) at up to 1024 x 1024 on the titantron (see hd_movies.cpp).
#pragma once

#include <filesystem>
#include <string>

namespace svr2011 {

// User movie `file` ("<name>.bik" in Custom Movies, which holds the mod's
// .bik fallback, or nothing yet) is played from `mp4` where the platform can
// decode it: a stub .bik of the movie's size and length goes into Custom
// Movies\.hd (the game's Bink player keeps its timing; the frames are the
// decoder's). When there's no fallback, the stub is the user movie too.
// true: the mp4 plays here.
bool RegisterHdMovie(const std::string& file, const std::filesystem::path& mp4);

// The same, at start before Custom Movies is mounted (`movies`): the probe
// and the stub then cost nothing during a match, and the stub is a file the
// mount found. RegisterHdMovie later just finds it.
bool PrepareHdMovie(const std::filesystem::path& movies, const std::string& file, const std::filesystem::path& mp4);

// The file the game opens for user movie `file` (".hd\<file>" for an HD
// one, else `file`).
std::string HdMovieFile(const std::string& file);

// The .mp4 user movie `file` plays (empty: none registered).
std::filesystem::path HdMovieMp4(const std::string& file);

}  // namespace svr2011
