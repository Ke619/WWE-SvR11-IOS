// Fewer people in the seats (CROWD DENSITY; weak phones: Mali mode). arena_crowd = auto (off in Mali mode) /
// on / medium / off. Off and medium: the arena files the game opens are copies with all or about half of the
// crowd's people emptied (one vertex, no triangles - the game still finds them), made once in the background
// and kept in Mods/ArenaCrowdless (medium's named half_...).
#pragma once

#include <filesystem>
#include <functional>

namespace svr2011::crowd {

// The crowd for this run, read once: 0 none, 1 about half, 2 everyone (arena_crowd, or Mali mode for auto).
int Level();
// Whether arena files are served as copies (Level() below 2).
bool UsesCopies();

// The file to serve for arena file `source` (BGnn.PAC): its copy for this level
// once made, else `source` itself - and the copy is then made in the
// background; `ready` runs (on that thread) when it is there.
std::filesystem::path Serve(const std::filesystem::path& source, const std::filesystem::path& cache_dir,
                            std::function<void()> ready);

}  // namespace svr2011::crowd
