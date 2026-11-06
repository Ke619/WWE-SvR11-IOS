// The whole arena to one FBX to look at or use elsewhere (the Mod Maker's For Fun page). Unlike ExportArena (arena.h),
// which writes a file made to come back into the same arena and keeps every model as stored, this one puts things where
// the game shows them: models a motion places (entry 0x7531, m(<n>) in the flag table: beams, glows, light rigs, modelled
// at the centre) at their first frame, models the game also draws mirrored (flag r) twice, and the ring's parts, which the
// game places by code, each where it goes (ropes, pads, turnbuckles, apron: rigged in the game, here at rest). Models the
// game doesn't draw (the ropes' shadow copies) are left out. Materials link their
// normal maps (texNormal), and the default light set (100) comes as FBX lights; every set is in lights.json.
// It doesn't come back in: Import from Blender takes ExportArena's file.
#pragma once

#include <string>
#include <vector>

#include "arena.h"

namespace svrfmt {

struct ArenaFullReport {
  int objects = 0, placed = 0, mirrored = 0, ring_parts = 0, not_drawn = 0, meshes = 0, triangles = 0;
  int textures = 0, textures_failed = 0, normal_maps = 0, lights = 0, light_sets = 0;
  std::vector<std::string> warnings;
};

// Writes <out_dir>/arena.fbx, <out_dir>/textures/*.png, <out_dir>/lights.json.
bool ExportArenaFull(const Arena& a, const std::string& out_dir, const std::string& title, ArenaFullReport& rep);

}  // namespace svrfmt
