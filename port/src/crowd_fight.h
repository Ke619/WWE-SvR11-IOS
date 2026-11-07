// Crowd fighting (SvR 2008's): in an Extreme Rules match on an arena ported from SvR 2008, the fight can go
// through the open back barricade into the crowd area and up its steps. See crowd_fight.cpp.
#pragma once

#include <cstdint>

struct PPCContext;
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

// This match plays with the crowd area open (decided when its stage loads).
bool CrowdMatch();
// The match about to load opens the crowd area (its rule Extreme Rules, its arena a crowd one - CrowdCollisionId's
// rule), read from the live settings: usable before the stage loads (the weapons are made first).
bool CrowdAreaFor(uint8_t* base);

// The stage loader's collision look-up (sub_826A0D98 called from sub_82187258): the id to ask for -
// SvR 2008's crowd collision (999) in place of the match's (996) in a crowd match.
uint32_t CrowdCollisionId(uint8_t* base, uint32_t lr, uint32_t id);

// A fan hands a weapon over the barricade: a fighter who taunts (D-pad; moveset slots 9-12) on the floor by the
// barricade in a crowd match gets a weapon at their feet, picked up as any other. The game's own state only (the
// taunt's move look-up, positions, the match clock) - the same on every game of an online match (lockstep).
// From the move look-ups (cam_mod_moves.cpp, in the world update): the fighter asked for a move in this slot.
void CrowdMoveAsked(uint32_t fighter, uint32_t slot);
// Each world update (match_types.cpp MatchTypesUpdate): the asked hand-offs made.
void CrowdHandoffUpdate(PPCContext& ctx, uint8_t* base);

// Into the crowd: a player at the barricade in front of the crowd area (a crowd match on an arena with crowd_area=)
// sees "Y: CLIMB INTO THE CROWD"; Y climbs over and puts him on the crowd area's floor. Input-driven (the fighter's
// own synced pad), the same on every game of an online match. Each world update (from CrowdHandoffUpdate).
void CrowdVaultUpdate(PPCContext& ctx, uint8_t* base, uint32_t frames, bool new_match);
// The prompt over the match (svr2011_app.cpp).
void InstallCrowdOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace svr2011
