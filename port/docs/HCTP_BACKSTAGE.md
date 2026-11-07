# HCTP backstage areas (branch hctp-backstage)

SmackDown! Here Comes the Pain (PS2, 2003) backstage areas played in SvR2011
as backstage mods with their own BACKSTAGE row. The first one is the
**Arena Back Entrance** (HCTP stage 0x30, "BackOpenSpace" in HCTP's code):
two levels, the small chain-link cage, crates, valves, a button that rolls a
cart out of the garage.

## Where things are

- For players: `tools/backstage_port.py` (Mod Maker Tools; the converter contract) builds any room from
  their own disc into an installable `Mods\Backstage\<name>` folder:
  `list --game hctp --src <ISO>` (hctp30 .. hctp36), `convert --game hctp --src <ISO> --item hctp31
  --out <Mods\Backstage\Parking Lot (HCTP)> --pac11 <2011 game folder>` (about 14 s a room). It reads the
  disc through `discfs` (`tools/hctp/hctp_unpack.py`) and runs `tools/hctp/build_room.py`.
- The disc: `D:\Roms\Playstation 2\WWE Games\WWE SmackDown! Here Comes the Pain.iso`,
  extracted to `D:\Xbox Games Ports\HCTP Extract` (`PAC\*.PAC`, unpacked
  `_unpacked\bg\stg_XXXX`, `_unpacked\gm`, `_unpacked\m`).
- Viewer pages (three.js, one per room): `HCTP Extract\viewer\index.html`.
- Converter scripts in `port/tools/hctp` (research notes in the session scratchpad):
  - `hpac.py` (archives), `yobj.py` (models, textures), `to_glb.py` /
    `room_view.py` (viewer);
  - `hctp2jboy.py` (models -> JBOY), `hctp_fake56.py` (room -> stand-in
    stage for `svr08_stage.py`), `hctp_props.py` / `build_props.py` /
    `hctp_gimmick.py` (props, climb spots, motions -> GMGB package),
    `build_room30.sh` (all of it);
  - `mot/hctp2ymks.py` (HCTP motions -> 2011 YMKs);
  - `re/re_gimmicks.md` (HCTP's gimmick code), `re/re_svr2011.md` (2011's
    collision attributes, gimmick record types, weapons),
    `mot/re_motion.md` (motion formats).

## Formats (HCTP vs 2011)

Same family. DPAC/PAC archives, BPE, YOBJ (= JBOY little-endian, POF0), HMD
collision (identical 96-byte triangle records with an attribute word), model
flags (50001), camera zones (50005). Units and axes are the same (1 = 10 cm,
y down). HCTP models store geometry as PS2 VIF packets; prop vertices are in
model space with per-batch nodes (batch node number = node index + 1).

Motions: the same bank container and frame stream; the body record has the
same 21 bones in the same order and the same rotation convention, so HCTP
motions convert without retargeting (only re-encoded).

## How the room is built

1. Room models -> parking-room slot of a bg78 (rotated 180, offset 180,-440),
   HCTP collision (room + props at rest) as the parking HMD pair. The stage
   entry STG/0078 must stay last in bg78's STG group (a row match indexes the
   group by position). Empty parking ids get stub models.
2. Gimmick package GMGB/78H3 (gimmick.pac): 2011's corridor parts, HCTP props
   as T23 objects (with their node rigs), climb spots (T22 walk / T21 run on
   the 0xDF faces), and a bank = 78PK's + converted HCTP motions (keys
   12501-12544).
3. Manifest: `row=`, `gimmick=78H3`, `box=`, `camera_height=`, and new keys:
   - `start=<x>,<z>,<facing deg>[,<y>];...` start places (match_types.cpp);
   - `weapons=<model>,<x>,<y>,<z>,<yaw>;...` weapons at the bell (45010 swap);
   - `hctp=30` turns on `src/hctp_room.cpp` for that row.

## What 2011 can't do from data (src/hctp_room.cpp)

| HCTP | status |
|---|---|
| Climb crates / walkway / ladder / cage (wall attributes) | 2011 backstage fighters walk in motion 20500, which never reaches the wall switch (sub_82306B10); hctp_room asks the T22 search itself when a fighter presses into a 0xDF wall |
| Walk off a ledge | works (gravity + floor triangles) |
| Cage walls: grab (X) + mash -> both panels of a side break, vanish, stop blocking | works (player and AI); the panels' collision carries the panel tag (0xA0 + prop), so climbing accepts those tags too |
| Hits / grabs through the standing fence (HCTP cells) | hooks on the hit broker's vt[8] sub_82238F28 and vt[10] sub_8222F678: inside-cage vs outside-cage pairs on the ground are dropped while that side's panels stand; works (test: fighters at x 155 / 166 either side of the east panels, 48 CPU hits across, all dropped, no damage) |
| Roof: 2nd knock-down breaks it, fall into the cage | works (DropIn nudges whoever stands over the hole; gravity does the rest) |
| Button -> shutter opens, cart rolls out, crates fall | works: X in the button zone (12510), shutter 12525 then hidden, cart moved along z (MoveRec + pose copy off, obj+1140), hits whoever is in its way (340 = 12516), crates 12529, fade, 240 HCTP frames later the closing shutter (HCTP 1265, 12530) and everything resets |
| Valves -> steam jets that hurt | works: X in the valve zone toggles (12511 / 12512, 121-frame gap, off after 250); 5 zyouki puffs per jet moved along it; burn 12509 in the jet volume (HMD 2058 / 2068); valve 210 also drops the barrel stack |
| Equipment crate: whip a wrestler in, he's shut in and climbs out | works with the test switch (12513 attacker/victim/crate on the crate's spot, held on the last pose, 12514 at HCTP 420); trigger = whipped run (motion 59) into the crate's floor zone, untested with a real whip |
| Stacks (cardboard, barrels, pipes) collapse and drop a weapon | cardboard + barrels work (2011 has no cardboard box / barrel / lead pipe: trash can 63 / tire iron 120) |
| Railings fall | coded, untested |
| Jump down off the cage top (HCTP GapIn, 12572 -> 12505) | works: X on the cage top facing its front or east edge puts him on the edge and he jumps to the floor |
| Step into the broken roof hole (12092 -> 12506) | works: X while walking toward the hole from the frame (within 6 of it) steps in; he lands in the cage ~1.5 s later |
| Weapons the props drop | cardboard box 17 (stack), trash can 63 (barrels), kendo stick 88 for HCTP's lead pipe (2011 has none); their models load through load_weapons= (hook on sub_82301148, the per-area wanted-model list) |
| Equipment crate rest pose | 12559 = the crate's own track of 12513, 1 frame (was a tilted green slab) |

Test switches (SVR2011_TEST_HCTP_ROOF): `1` roof breaks at the first knock-down, `now` roof breaks at frame 300, `crate` fighter 1 shut in at frame 300, `steam` both valves opened at frame 300, `cart` the button cycle at frame 300, `props` railings, pipes and stacks knocked over at frame 300, `puff` a steam puff over fighter 0, `nograb` no cage-wall grabs or climbs (fence tests).

Camera: in an HCTP row (hctp=), fighters at different heights (one on the cage top) are framed head to feet (match_types.cpp, the sub_82227690 sphere: centre between the lowest feet and the highest head). One on top and one far off below can still leave the lower one at the frame's edge (the camera= distance cap).

Known: walking off the frame into the open hole drops a fighter in by gravity without the step-in act (HCTP's GapIn needs X).

Pitfalls:
- Moving a prop (MoveRec): write elem +32/+48 and call sub_824B9C40(elem); then mark the objects' motion component ('S3MW' via sub_8269DFB0(obj+1088), its +24 -> +12 = 0), or sub_824B46F8 copies the last motion's matrices back every frame. Not obj+1140: that skips the node commit too and the object isn't drawn. PlayRec (sub_824B91C0) sets S3MW running again.
- An object hidden (sub_824B8FE0 + 4198/4150) and shown again isn't drawn while no key plays on it: the steam puffs park under the floor (y 1000) instead.
- Models that never play a key must be converted without their node rig (build_props STILL, the puff 1237): with keep_nodes 2011 draws HCTP's raw node transforms (the cart was a grey plank at head height, the puff invisible).
- The closing shutter (1265) is hidden for the room's first 30 frames (SetUp): its elements may not exist on the first one.
- Placing a fighter for a gimmick act: yaw (+276) before the act, then play it, then write +288..+296, body (+304) +0/+8 (as sub_824B67D0, the T22 snap) - the act's root motion is absolute from where he is when it starts, turned by his yaw: local +z = (sin yaw, cos yaw).
- Weapon models: the game loads a hard-coded set per area (parking 65, 63, 17; sub_82301148); weapons= only places weapons. Extra models: load_weapons= in the manifest. (2011 model 120 is a refrigerator, not a tire iron.)
- 2011 props draw with an alpha cut-off (no blending): soft textures (the zyouki steam) get an ordered-dither alpha.

## Tests

- `SVR2011_TEST_HCTP_LOG=1`: wall switch, T22 search, motion key lookups.
- Start places put the fighters where a test needs them (e.g. the opponent
  upstairs, where the CPU takes a while to come down).

## Room data (hctp.txt, written by build_props.py)

`prop <name> <hctp id> <kind> <attr> <x> <y> <z> <ry> recs=... tris=...`,
`weapon <hctp id> <model> <x> <y> <z>`, `key <key> <frames>`. Each prop's
wall triangles carry the low attribute byte 0xA0 + its index, so a fighter's
touched attribute (+455) names the prop he hit; tris are HMD 1004 indices
(after the converter's padding dummies).

## Rooms 0x31 - 0x36

| room | row (build_room.py ROOMS) | offset dx, dz | interactions (src/hctp_room.cpp) |
|---|---|---|---|
| 0x31 | Parking Lot | 140, -437.7 | broken cars (door / bonnet fly off, crushed by a body on the roof), fence / oil drums / equipment box, generator shock (12079), Irish whip into the trailer (12078 sync), background traffic car, drivable forklift (X on / off, A drive, R1 brake, Y reverse, stick steer, B tips the crate; 321 reloads it) |
| 0x32 | Training Room | 103.5, -388 | lockers (shut in 120 frames), toilet stall, showers, shelves (items fall, become weapons), TV, breakable wall (cracks, then opens and knocks the TV over), sandbag (two pendulums, knocks down), treadmills (X on, A speeds up) |
| 0x33 | Times Square | 86.5, -408 | six taxis (brake, run over, explode when someone goes down on the roof), lamp-post poles (X climbs, X jumps off), shop windows, ladder (0xE0) / wall (0xE6) climbs, helicopter (its act-10 path; X grabs the ladder) |
| 0x34 | Outside the Arena | 205, -334, crop -60,345,-534,-322 | the extinguisher is 2011's own weapon (65); HCTP's other room-34 objects are dead data |
| 0x35 / 0x36 | Fountain Square / (dry) | 235, -617.5 / -607 | lion statues (run straight at one: climb 12005-7; X or 601 frames dives off), fountain throw (Irish whip near a basin, 12027 sync; room 35); HCTP's flood and tower fall are dead code |

What the converter does per room (beyond room 30's steps):
- Models from the room's own 50001 flag list; models with the same flags merged (the parking slot has 15
  free ids), event-only ('e') and light-motion additive layers left out, 'r' / 'y' (draw again mirrored /
  turned) baked in, more than 64 meshes split. A stage over ~13k vertices fails 2011's GPU buffer allocation
  at load (room 34: cropped).
- Big collision indexes: cell counts padded off 0xFF, a zero tail so the entry decompresses in place.
- Props of 22 more HCTP classes (hctp_props.KINDS), their tag params (p=), record models (m=) and HMD
  shapes (h<id>=) in hctp.txt; pole tops and the helicopter's path as `pole` / `heli` lines.
- Motion bank per room: bank_h3 + HCTP backstage acts 12000-12099 as 12600 + n, 12500-12599 as 12800 + n,
  the room's object acts (50102) n as 12700 + n; jumps to 2011's downs 443 / 448 / 476 kept.
- Weapons: HCTP's spots (50000 'w', GmWeaponPosRot) as 2011 weapons in `weapons=`, or `weapons=none`
  (else 2011's parking weapons lie in the room); what props drop in `load_weapons=`.

2011's input object (svr2011::FighterInput): +48 buttons held, +52 pressed this frame (X 0x80, A 0x40,
B 0x20, Y 0x10, R1 0x08), +56 the stick's 8-way direction (4 up the screen = -z, 6 right = -x, 0 down; -1 none).

Test switch `SVR2011_TEST_HCTP_ROOF=props` in rooms 31-36 drives the interactions on fighter 1 (cars,
generator, slam; racks, wall, locker, toilet, shower; lion; window, pole) and puts fighter 0 (the scripted
pad) by the forklift (31) / under the helicopter (33).

## The HCTP backstage as walkable worlds (2026-10-20, branch hctp-full)

`tools/backstage_port.py convert --item hctpworld` (Back Entrance, Parking Lot, Training Room, Times Square,
Outside the Arena) and `--item hctpfountain` (Fountain Square, Parking Lot, Back Entrance, Training Room) build
one backstage row each whose rooms the fighters walk between through HCTP's own gates (`tools/hctp/build_world.py
<unpacked> <pac11> <work> <mod> [rooms]`; the first room is where the match starts). Both are offered as a BETA
(the Mod Maker's Backstage page lists them; `SVR2011_HCTP_WORLDS=0` hides them) and need 2.1.6.
- Limits found: 2011 loads ~400 stage meshes at most (stock bg78 445 with its characters' share; 5 rooms =
  ~400 load, 6 = 625 fail in the stage loader sub_826E73F8 - the null comes from allocator 128's physical
  window 0x15140000-0x16230000, ~17 MB, shared with the stage textures and the gimmick package; texture bytes
  alone weren't it: six rooms at PS2 resolution still failed). Room 35 alone is 225 meshes (61 textures on
  its ground) - hence two worlds. World builds upscale PS2 textures 2x (see-through 1x; single rooms 4x / 2x),
  cap the stage bundle at 3.5 MB (world_stage.TEX_BUDGET) and leave out HCTP 'w' models (HCTP_DROP_W).
- The match camera's framed target stays inside the stock backstage (it stopped at x 325 when the rooms
  stood 1000 apart along x): all rooms lie one over the other in the parking room's place.
- Stage (`world_stage.py`): room k in model ids 20k..20k+19 (stock bg78 keeps one room per id block too);
  its texture names get the prefix `k`; stock models 0-199 not taken are stubs and their textures go; all
  rooms' collision in the parking pair 1003 / 1004 (rooms in order).
- Gimmick package: every room's props as objects 1000-1139 (stock CFRS uses 1000-1139 for its rooms),
  records in room order, then the climb spots room by room; the bank = bank_wr + each room's object acts from
  its own first key (31: 13000, 32: 13100, 33: 13200, 34: 13300, 35: 13400; room 30's are in bank_h3).
- hctp.txt: `room <n> <model lo> <hi> <object lo> <hi> <object act key> <x0> <x1> <z0> <z1> <tri lo> <hi>
  <spot lo> <hi>` before each room's lines (triangles after stage_wrap's dummies: k + k // 255), `door <id>
  <to> <x> <y> <z> <exit act> <way out> <x,z;x,z;... 8 spots> <way in>` (older builds: `<entry x> <entry z>
  <entry yaw>`) and `spawn <model> <x> <y> <z>` (weapons put down on the room's first visit; the first room's
  are the manifest's weapons=).
- HCTP's gates (SLES_520.36, `tools/hctp/re/gate/`: CGate4 / CMoveStage disassembly and gate_tables.py): each
  stage 50000.txt `m` point (id at weapon +0x30) is a gate. g_stg_audi_list (0x419FB0, 2 bytes per id) gives
  the stage it leads to (0 = the arena) and the point there you come in at; bord_group_list+0x10 (0x419F90,
  s16 per id) the exit act. CGate4::Action (backstage rules only, not in the first 300 frames, 209 only once
  hide item 11 is unlocked): a wrestler on his feet within 10 of the point on x, y and z who presses the action
  button (pad 0x80; the CPU too) while walking. CMoveStage plays the exit act on him at the point, the others
  stand (act 0), then loads the stage; Rear puts wrestler i at the entry point + its rotation of
  (offx[i], 0, offz[i] - 33), offx -9 9 -9 9 -12 12 0 14, offz 8 8 -8 -8 0 0 0 -10. CGate4::Draw draws a lit
  marker, no text.

  | gate (room) | leads to | exit act |   | gate (room) | leads to | exit act |
  |---|---|---|---|---|---|---|
  | 201 (30) | arena 200 | 15919 | | 206 (31) | 207 (34) / 208 (35) | 15919 |
  | 202 (30) | 203 (32) | 15900 | | 207 (34) | 206 (31) | 15919 |
  | 203 (32) | 202 (30) | 15907 | | 208 (35) | 206 (31) | 15919 |
  | 204 (30) | 205 (31) | 15902 | | 209 (34) | 210 (33) | 15909 |
  | 205 (31) | 204 (30) | 15902 | | 210 (33) | 209 (34) | 15909 |

  Fountain Square (35) is never next to Outside the Arena (34): 35 takes 34's place when the match started
  in stage 0x16 / 0x17 (31's gate 206 then leads to 35's 208) - hence two worlds, and Times Square (33, only
  through 34's 209) isn't in the fountain one. The exit acts (HCTP MOT_GAME/00004.bin 15900-15919) are in the
  world bank as 12900 + n. 201 (back to the arena) has no room here.
- Game (hctp_room.cpp): after each CFRS update (match_types.cpp hook) only the current room's models
  (sub_8252FF40) and its records' objects (sub_824B4198) are shown; the other rooms' collision triangles are
  lifted away (as DropCollision) and their climb spots inactive (sub_824B8FE0); only its props run. A player at a
  gate (within 10 on each axis in HCTP, 16 across here: 2011's walls keep him 12 from room 30's roof gate;
  an older door line: within 22, 4 up or down; on his feet: motion 0 /
  stance 20 / walking / running) sees the prompt "<Y> GO TO <room>" (DoorPromptOverlay, match_hud.h's panel;
  the button as his pad names it: Y, TRIANGLE or his keybind_y key) and goes through with Y; a CPU (port
  char+1152 >= 4) walks or runs into one for 30 frames (its AI chasing - CPU-vs-CPU keeps changing rooms);
  ROOF=walkin gives players the walk-in too. He plays the gate's exit act there facing out (held placement at
  the point; through once his motion is no longer the act or its frames have played - the act holds its last
  frame - at once if the bank lacks it; the others get act 0 and are kept where they stood meanwhile - HCTP
  stops the pads; the acts' screen fade, event op 18, is dropped by hctp2ymks.py: HCTP's next stage load fades
  back in, 2011 stayed black - DoorPromptOverlay draws the fade instead: black over the act's last 20 frames,
  back in over 20 in the new room),
  then every fighter goes through (in HCTP the opponent came along): fighter i at the other gate's spot i
  (older lines: side by side at its entry), the fight box moved to that room (SetFightBox), the camera's eye
  put at the framed target +
  its old offset for 30 views (SnapMatchCamera - moving the eye once didn't hold: the camera rebuilt it).
  A room's runtime state (later shows, taxis, poles, heli, forklift, what ShowRec hid) is kept while it is
  away. Test switches `SVR2011_TEST_HCTP_ROOF=tour`: a door every 6 seconds; `=door`: fighter 0 put on the first door's trigger 5 s in (walk with the scripted stick); `=fork` (room 31): fighter 0 gets on the forklift 5 s in; `SVR2011_TEST_HCTP_NOVIS=1 /
  models / objects`: the stock visibility only.
- Online (inputs-only netplay, svrtest lan): the doors work and stay in step (same at 300 / 600 / 1200 / 1500,
  the wrestlers at the other door's entry). Two things it took: (1) online the game's update puts a wrestler
  back on his physics body, which then eases toward him over several frames - a door's placement is held:
  written again at the start of the next 10 world frames, before the game's update (HctpBeforeFrame,
  frame_rate.cpp), with the body's other position copies +704 / +768; (2) a port (char+1152) is local - the
  other side's player has one of 4 or more - so online every wrestler counts as a player (Y; no CPU walk-in)
  and both games decide alike. The prompt shows only for this PC's own players.
- Weapons: lying ones are parked 5000 below the floor while their room is away (the match's weapon objects,
  72 pointers at 0x82DE0220, position +288..+296) and put back on return; one within 14 of a wrestler stays
  (held: it goes with him; one only lying by him shows in the next room).

HCTP's own model flags (50001.txt legend): s normal translucent, a additive translucent, k translucent
before shadows, z no z-write, m(n) motion, l(n) light motion, r / y also drawn mirrored / turned, n near
clip, e events only, w only in the move camera during play. 2011 reads s / a / z / n itself (stock bg78:
s on shadow / line decals, a + l(101..) on light cones). HCTP 'a' layers became plain translucent ('s') at a
quarter of their vertex alpha: 2011 draws 'a' in its light motion's colour (Times Square's traffic-light
glows, 208 x 208 ground quads with green / yellow vertex colours, covered the street solid green - also with
l(101)).

Facing conventions (2011): a fighter's yaw y faces (sin y, cos y); a gimmick object's local +z at yaw a
points (-sin a, cos a) - objects turn the other way. The traffic car (atan2(-dx, dz)) and the forklift
(model yaw pi - heading) were driving mirrored ("wrong way"). Wrestlers put at a prop take FighterYaw(prop yaw) = -yaw
(+ the HCTP turn): with the prop's own yaw they faced the mirrored way on props turned 90 (room 35's lions
at +-3pi/4, room 30's crate at -pi/2; props at 0 / pi are the same either way).

PS2 culling: the room shells wind like stock 2011 (every triangle agrees with its normals; normals unit),
but the PS2 drew everything from both sides. Converted meshes now clear render flag 0x2 (back-face cull) for
props and for cut-out / blended textures (fences, grilles, signs); solid walls stay culled, so a camera
outside a room still sees in. Room 33's taxis had mixed winding - double-sided now.

## State when paused (2026-10-14, branch hctp-resume)

Done and checked in game: all seven rooms build and load (60 fps) with their props, own weapons and
interactions - room 30 (cage, roof, cart, valves, crate, fence veto, roof step-in), 31 (cars, fence / drums /
box, generator, trailer slam, traffic car, forklift), 32 (lockers, toilet, showers, shelves, TV, wall,
sandbag, treadmill), 33 (taxis, poles, shop windows, ladder / wall climbs, helicopter ride), 35 / 36 (lion
climb). backstage_port.py converts any room from the player's ISO (checked: room 32 converted from the ISO
with the real Game Files as --pac11 plays in game; ModMakerUIRework's Port page "Backstage" kind on main).

Next steps (none started):
- Generator lights-out (HCTP CChangeLightColor: room lights to 0.15 for 160 frames + a glare): needs 2011's
  stage-light control found.
- Room 30's button lamps (CGmSwitchLight 1246): drawn as grey slabs as plain objects - needs an additive /
  emissive draw.
- Fountain throw (room 35): checked (21 Oct, ROOF=props puts both by the basin at 1100: 12627 x / y 0 and 1
  found, the throw plays). Treadmill: checked (ROOF=props puts fighter 0 on it at 800;
  A mashed: level 1, 12674; he runs facing the console). Sandbag knock-down: coded, not driven by a test yet.
- Motion keys: a call's (x, y) match the bank entry's second / first field (logged with SVR2011_TEST_HCTP_LOG);
  lookups the game makes itself for acts HCTP has in one part only (12679 y 1, 12606 / 12608 y 50 ..) miss
  harmlessly.
- Lion statue fade (HCTP draws it at 0.3 when a wrestler is behind it), broken-car crushed collision (2098),
  locker-door / TV / window weapons (2011 has no matching weapons), taxi and forklift solidity (static
  collision only).
- The 'X gimmicks' accept motion 0 (standing still) - a lying wrestler on the right spot can trigger them too;
  a better standing test would fix it.
- HCTP's own unknowns: re/re_rooms31_36.md "Unknown" sections.
