// Building arenas from parts: an empty arena (only the ring, the floor and,
// if wanted, the ringside parts) and models copied in from any other arena
// (the prop library). The arena file the game loads still takes the place of
// one shipped arena (its slot: memory room, VS screen style); everything you
// see can come from elsewhere.
#pragma once

#include <string>
#include <vector>

#include "arena.h"

namespace svrfmt {

struct EmptyOptions {
  bool keep_ringside = true;  // barrier, steps, announce table, timekeeper area
  bool keep_floor = true;     // the floor models (ar_ground*, the ring's floor and shadows)
};
struct EmptyReport {
  int models_emptied = 0, textures_shrunk = 0;
  size_t bytes_freed = 0;  // unpacked
};
// Empties every other model (one vertex, no triangles: the game still finds
// it) and shrinks the textures nothing visible uses to 4 x 4.
void MakeEmpty(Arena& a, const EmptyOptions& opt, EmptyReport& rep);

// The crowd (the people in the nested pack 0x4E20) left out: no one in the
// seats. Returns the number of crowd models emptied.
int HideCrowd(Arena& a);

// The crowd's colour is the arena texture "aud_lightmap" (RAW's red, SmackDown's blue; report 5fbf5a). Its DXT1 / DXT5
// colour blocks rewritten in place (same size): each end colour made grey at its own brightness, times `rgb` and
// `bright` - (1,1,1) x 1 is neutral; `rgb` null keeps its own colour, only x `bright`. Returns false when the arena
// has none (or it isn't DXT1 / DXT5).
bool HasAudienceLightmap(const Arena& a);
bool TintAudienceLightmap(Arena& a, const float rgb[3], float bright);
// `from`'s aud_lightmap over `to`'s (same name and size only): a reopened mod's tint undone before it's applied again.
bool CopyAudienceLightmap(Arena& to, const Arena& from);

// The models of an arena worth offering in the library (not empty, not the ring kit).
std::vector<int> LibraryModels(const Arena& src);

// Copies model `src_model` of `src` into `dst`, as new static meshes on
// `host_model` (rigged parts stand in their bind pose). Its textures come
// along (renamed when `dst` has a different texture of the same name).
// Returns the first new mesh index on the host (-1 on failure).
int CopyModelInto(Arena& dst, int host_model, const Arena& src, int src_model, std::string* error = nullptr);

}  // namespace svrfmt
