// The game's frame rate: frames a second up to the chosen one (setting
// frame_rate: 30 or 60), with the game's world always at its normal speed.
//
// The game's world advances in steps of 60 Hz (its timing block,
// sub_826E1AE8 SetFrameRate, stays at the game's own 60). Each frame the
// world update runs once per 60 Hz tick of real time - twice a frame at 30
// fps, or when the PC can't make 60 - and the picture is drawn once. The game
// draws at most 60 frames a second: its world and its menus are made in
// those steps.
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallFrameRate(rex::memory::Memory* memory);

// After the game sets its frame rate (sub_826E1AE8, sub_826E1D28): its 60
// again (menu_hooks.cpp).
void ApplyFrameRate(uint8_t* base);

// The chosen frame rate (30 or 60), and the frames the game draws now: 30
// only in a match (menus at 60).
int TargetFrameRate();
int FrameRateNow();

// A match on (from its entrances) or off (a main menu choice): menu_hooks.cpp.
void SetFrameRateInMatch(bool on);
// The last of those (any thread).
bool InMatch();

// A match may be starting (sub_823EEB98 - which some menus call too, e.g.
// ONLINE): `on_start` runs on the game's thread once the match's frame count
// has been counting for half a second (a real match); a main menu choice
// (SetFrameRateInMatch(false)) drops it.
void ArmMatchStart(void (*on_start)());

// Entrances and cutscenes at 30 frames a second (the original's; setting
// unlock_30fps off): on while one plays (menu_hooks.cpp).
void SetSceneThirtyFps(bool on);

// Setting full_speed (on): extra world updates when frames are slower than 60
// (and at 30 fps); off: one a frame, as the console - the game slows down
// instead (a way round any trouble with the extra updates).

// Online lockstep (p2p.cpp, during a match session): exactly one world update
// per frame at a 60 Hz frame clock, as on the console - both peers step the
// world alike; a device that can't make 60 runs slower instead of catching up.
void SetLockstep(bool on);
bool Lockstep();

// The world updates the frame running now got (1; 2 at 30 fps or a slow frame; 0 while the free camera holds it).
int WorldPassesThisFrame();

// A guest address in readable memory (a mapped, accessible page) - is a word a pointer?
bool GuestReadable(uint32_t address);
// Test aid (Windows): the hardware write watch of SVR2011_TEST_HW_WATCH on this guest word, from now on (once a run).
void HwWatchOn(uint8_t* base, uint32_t guest);

// Chooses the frame rate, at once (GRAPHICS -> FRAME RATE).
void SetTargetFrameRate(int fps);

// Writes the game's timing block for `fps` frames a second.
void WriteTiming(uint8_t* base, int fps);

// Test aid SVR2011_TEST_LATENCY=1: the native renderer published a frame
// (its backends' PublishFrame) - frames in flight and publish-to-screen
// time are logged once a second.
void LatencyOnPublish();
// ... and a game swap reached the native renderer (OnPresent).
void LatencyOnSwap();
// The render command queue's fullest in a match since the last call, in % of its buffer (record 0 at queue +76: +4
// units used, +8 units; a full buffer crashes the game - 27 Oct, six heavy superstars). Logged per match at its end
// (model_mem.cpp).
uint32_t RenderQueuePeakTake();

}  // namespace svr2011
