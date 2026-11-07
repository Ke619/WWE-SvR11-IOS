# Backstage mods (arenas branch)

A backstage mod is a rebuilt backstage brawl room.

**Switched off (29 Oct 2026, the user's call for 2.1.6: "off, stock kept").** `BackstageModsOff()` in `src/no_mods.h`
returns true: `LoadBackstage` reads no Mods\Backstage folder (it logs each as "backstage mod '<name>' skipped"),
no backstage gimmick pac is mounted (`BackstageGimmickPacs`), none is offered or checked online, and Backstage
Brawl always plays the game's own backstage. The files stay installed. The launcher's Mods tab and the Mod Maker's
Installed page show them off (they can't be switched on); Android's Mods page labels them OFF. Undo: return false
there and drop those UI guards. Road to WrestleMania's walks, scenes and story matches are untouched.

- **Game:** `LoadBackstage` in `src/arena_mods.cpp`.
- **Making them:** the Mod Maker's Arenas page → Backstage areas → Open area
  in Arena Editor.
- **Installing them:** the launcher's Mods tab (PC and Android).

## The game's backstage

All seven backstage brawl rooms, the corridor and the Road to WrestleMania
rooms are in one file, `pac/bg/bg78.pac`. There is no per-room file and no
room table. The match rule picks the room (1v1 rules 0x1B-0x21), and about 25
functions test the room with 7 hard-coded predicates.

A room is a set of model ids in bg78. Entry 50001 holds the flags.

| room | rule (1v1) | models | the game's box (sub_8224EF28) |
|---|---|---|---|
| Parking lot | 0x1B | 0-19 | 125 0 -410, half 38 x 50 |
| GM's office | 0x1C | 20-39 | -338 -8 217, half 30 x 27 |
| Locker room A | 0x1D | 40-59 | 290 -8 -190, half 34 x 34 |
| Locker room B | 0x1E | 60-79 | 326 -8 241, half 34 x 34 |
| Large locker room | 0x1F | 80-99 | 460 -8 -76, half 25 x 25 |
| Interview area | 0x20 | 160-179 and 140-159 | 0 -8 141, half 30 x 30 |
| Catering area | 0x21 | 140-159 | 20 -8 -190, half 45 x 30 |

About the box column:

- `SVR2011_TEST_BOX_LOG=1` logs these values.
- They are not the area the fighters are kept in. In a parking-lot match the
  fighters roam well beyond it.
- The match cameras do see its middle, so the test edit puts its box there.

The cars, crates, bins and other weapons are placed by the game. They are
not models in bg78 (the 1000+ object ids listed in the research have no
models there), so the editor can't show them and they stay.

## A mod folder (a .svrmod is a zip of one)

| file | |
|---|---|
| `manifest.txt` | `type=backstage`, `id=`, `name=`, `author=`, `version=`, `area=<0-6>` |
| `arena.pac` | the whole bg78, with one room rebuilt |
| `disabled` | (optional) turns the mod off |

Mods install into `<game>/Mods/Backstage/<id>/`. At start the game takes the
last enabled folder by name and redirects BG78 to it (`SetArenaDefault(78)`).
Only one backstage mod plays at a time; the log warns when more are
installed. Every backstage room and Road to WrestleMania read the mod's
bg78, which is why the build keeps the other rooms as they are.

## Areas of their own (row=)

A backstage mod can be an area of its own instead of replacing a room.
Extra manifest keys:

| key | |
|---|---|
| `row=<label>` | a row in ONE ON ONE -> BACKSTAGE, after its room's row; its `arena.pac` plays only in matches from that row (the room's rule, e.g. 0x1B) |
| `gimmick=<4 chars>` | `gimmick.pac`: a small pac with only GMGB/<name> (cars, props, hot spots); mounted as one more pac, played instead of the room's GMGB package in that row's matches |
| `box=<x>,<z>,<half x>,<half z>` | the fight box (camera target, clamp, AI) |
| `camera=<units>` | caps the match camera's distance |
| `camera_height=<units>` | the camera's eye at least that high (looks over objects round the area) |

Game side: match_types.cpp (row, box, camera hooks), arena_mods.cpp
(LoadBackstage, UseBackstageRow, the sub_824B0090 rename, a guard on gimmick
element type 25), move_packs.cpp (gimmick.pac in the overlay pac list).

**Parking Lot (2008)** (Bundled Mods\parking_lot_2008.svrmod): SvR 2008's
Parking Lot Brawl stage (bg56) and its vehicles (GMGA/0056), built by
`tools/svr08_stage.py` and `tools/svr08_gimmick.py` with `--rotate 180
--spread 2 --offset 142.1,-495.1` (gimmick: `--elements scenery
--hotspots-2011`). Pitfalls found on the way:
- File-level links aren't followed by the game's opens (folder links are),
  so a pac must be served through the overlay list.
- EPAC header +4 is the table size: the game reads only that much.
- 2008 type-0 records with their own locators froze the wrestlers; 2011
  packages carry common wrestler motions 12200-12231 the wrestlers' own
  code plays (missing: a wrestler stood still).
- 2008's own hot spots (types 20-28) aren't used; three cars get 2011's
  front hot spot. Break parts are dropped.
- 2008's ring is tight (140 x 112); spread x2 for a big open lot. 2011's
  1B camera sits on +z looking -z: the lot is turned 180 so 2008's front
  faces it.

## The Mod Maker

Open area in Arena Editor loads bg78 and shows only that room
(`editor::SetArea`).

- **Hidden:** models of other rooms, effects, and ceilings and light rigs.
  "Show ceilings" brings the ceilings and rigs back.
- **Ring Kit:** none.
- **Outliner:** lists the room's models under "Area".
- **Added objects** are attached to the room's floor model and take its
  material. Attached to another model of the room, they can be invisible in
  game: park_obj02's first mesh is a decal.
- **Build:**
  - The light tint goes to the room's models only.
  - There is no crowd or ring step.
  - FitFile may shrink only the room's own textures. Halving textures shared
    with other rooms froze the interview area at load ("world update stuck").
  - Changed models' node spheres (world space, what the game culls a model
    by) grow to hold their meshes.

## Tests

- **Mod Maker:** `--backstage <0-6>` opens a room.
  `--backstage 0 --test-edit-save runs\test_back.svrmod` adds a 1.5 m box
  next to the box centre and saves, writing its log to `<file>.log`.
- **Launcher:** `--mods-add <game> <file.svrmod>`.
- **Game:** `SVR2011_TEST_RULE=1B` makes ONE ON ONE NORMAL play that rule
  (1B-21 are the rooms), so `superstar_test.ps1 -Down 0` plays a backstage
  brawl.

Verified (2026-10-04):

- **Parking lot:** the log shows "BG78 -> Mods/Backstage/test_edit/arena.pac",
  and the added box stands in the fight with the floor's texture.
- **Interview area:** with that mod installed it plays (after the FitFile
  fix).
- **Arena mods:** still play after the JBOY reader fix.

Not done: two backstage mods for different rooms can't be combined. A merge
at start (each mod's room models into one bg78) would allow that.
