# The .svrmod format

This is the full spec of the WWE SmackDown vs. Raw 2011 PC port's mod files.
It is written for anyone who wants to read, write or play `.svrmod` files
outside this project, for example in another recompiled Yuke's game.

Every fact here was checked against the code. Each section names the source
files it comes from; paths are relative to `port/`. "The game" means the
port's own code (`src/`), which hooks the recompiled 2011 executable. "The
launcher" means the PC launcher (`launcher/`), and "Android" means the phone
app (`android/java/.../svr2011/`). The Mod Maker is `modmaker/`, and the
Python converters are `tools/`.

State: version 2.1.5, plus what main holds for 2.1.6 (the big-model memory,
`needs=`). Where 2.1.6 changes a number, both numbers are given.

Contents:

1. [The container](#1-the-container)
2. [manifest.txt](#2-manifesttxt)
3. [The mod types](#3-the-mod-types)
4. [How the Mod Maker and the converters build mods](#4-how-the-mod-maker-and-the-converters-build-mods)
5. [How the game installs and loads mods](#5-how-the-game-installs-and-loads-mods)
6. [The game's containers](#6-the-games-containers)
7. [Supporting .svrmod in another game (e.g. WWE '12)](#7-supporting-svrmod-in-another-game-eg-wwe-12)
8. [Tools](#8-tools)

---

## 1. The container

A `.svrmod` is a plain zip of one mod folder. The `manifest.txt` sits at the
zip's root, next to the mod's files.

### Writers

- **The Mod Maker** (`modmaker/svrfmt/zip_write.cpp`) writes stored entries
  only, with no compression. It sets general-purpose flag 0x0800, which means
  the names are UTF-8. Paths use `/`.
- **The converters** (`tools/wwe13_pack.py` and others) use Python's
  `zipfile` with `ZIP_STORED`.

Stored entries are the convention because most of the payload is already
compressed: BPE pacs, DXT pictures and Bink movies.

### Readers

The PC launcher (`launcher/unzip.c`, its own inflate) takes:

- stored (method 0) and deflate (method 8) entries; anything else, including
  zip64, fails the mod;
- files of 22 bytes up to 4 GB (`0xFFFFFFFF`), with the end-of-central-directory
  record found within the last 64 KB plus 22 bytes;
- unencrypted entries only (flag bit 0 set fails the entry).

Entry names:

- must be under 260 characters (`MAX_PATH`);
- are UTF-8 when flag 0x0800 is set, else the OEM code page;
- fail the mod when they start with `\`, contain `:`, contain a `..`
  component, or are `..`.

Subfolders are kept. For example, a superstar mod carries
`moves/pack.txt`, `moves/motions/...` and `vs/` pictures.

Android (`ModsPage.install`, `java.util.zip.ZipInputStream`) is similar:

- `\` is read as `/`;
- entries starting with `/`, containing `:` or a `..` component are skipped;
- **any file whose name starts with `.` is skipped** (see dot files).

### Size

No format limit applies beyond the launcher's 4 GB. What limits a mod is
what the game can hold: the model budgets in
[4.2](#42-superstar-models-budgets-and-chpac), and an arena that must fit
its shipped size ([4.4](#44-arenas)).

### The folder and its id

The launcher installs the zip into `<game>\Mods\<Type>\<id>\`. `<Type>` is
picked by `type=` ([3](#3-the-mod-types)), and `<id>` is the manifest's
`id=`.

The id is cleaned before use:

- The PC (`mods_tab.c safe_id`) keeps letters, digits, `_`, `-` and `.`, and
  turns anything else into `_`.
- Android keeps `[A-Za-z0-9_-]` only.

Ids should stay inside `[A-Za-z0-9_]` so that both platforms make the same
folder. The game keys everything persistent by this folder name:

- the superstar slot (`slots.txt`);
- the Threads slots (`Saves\.threads\<folder>.bin`);
- the Universe side table (`Saves\.universe\mods.bin`);
- the arena order (`order.txt`);
- the online keys.

A mod with the same id replaces the installed one. The new copy is unpacked
into `Mods\.incoming`, the old one is moved aside, the new one renamed in,
and the old one deleted. A held file leaves the old copy in `Mods\.old` for
the next start. A `disabled` marker in the old folder is carried over to the
new one.

### Files in a mod folder that are not the mod's content

| name | written by | meaning |
|---|---|---|
| `disabled` | launcher, game (backgrounds) | the mod is off; every loader skips the folder |
| `<file>.off` | game (BACKGROUNDS page) | one picture of a background pack is off |
| `.mm_project.txt` | Mod Maker | the Mod Maker's project settings for this mod ("keys only"): materials, FBX steps and similar. Opening the mod brings them back. Absolute paths are cut to file names and `*.from` keys are dropped (`modmaker/app.cpp ModProjectText / AddModProject`). The game ignores it |
| `.applied` | game | superstar mods: every `song=`, `movie=`, `music=`, `entrance=` and `cae=` value the game has ever set in the slot's profile, one `key=value` per line (`superstar_mods.cpp ReadApplied`). A slot that still holds one of them follows the manifest; anything else was the player's edit and is kept |
| `.bundled` / `.bundled_state` | launchers | at `Mods\` level, not in a mod: the bundled-mod record ([5.12](#512-bundled-mods)) |

**Dot files are local state.** The online mod CRC leaves them out
(`src/online_files.cpp Cosmetic`). Android does not install them. A mod must
never depend on one.

### Versioning

- **`version=`** is the mod's own version. The launcher compares versions as
  dot-separated decimal numbers (`mods_tab.c version_newer`; Android
  `Updater.newer`). Bundled installs never replace a newer copy the player
  already has. The online key carries `version=`, so a different version
  counts as a different mod.
- **There is no format-version key.** Formats only grow by new optional keys
  and files. A reader ignores keys it does not know.
- **`needs=<x.y.z>`** (2.1.6) is the oldest game version the mod works in.
  Today it is written only for a superstar model over the old 192-page window
  ([4.2](#42-superstar-models-budgets-and-chpac)). It is one constant in the
  tools: `tools/model_budget.py NEEDS_VERSION` and
  `modmaker/app.h kNeedsVersion`. Nothing reads it yet; a launcher display
  of it is planned.
- **`made_with=`** says who made the file: `Mod Maker <version>`, or
  `Port tools (...)` from the converters. The launcher shows it, and shows
  "non-Mod Maker" when it is missing.

---

## 2. manifest.txt

- **Format:** UTF-8 text, one `key=value` per line. LF or CRLF line ends.
- **Matching:** keys match from the first character of the line. There is no
  whitespace trimming in the game, so `name = X` is not `name=`. The PC
  launcher trims lines for the keys it shows.
- **Values:** a value runs to the end of the line.
- **Line length:** the game reads lines of up to 511 bytes (backstage
  manifests: 4095).
- **Duplicates:** a key given twice: the last one wins (each line overwrites).
- **Unknown keys** are ignored, so a manifest can carry keys for another game
  or tool.
- **Comments:** lines without `=` are ignored. Superstar `moves=` files also
  skip lines starting with `#`.

Keys every type has (the launcher reads these: `mods_tab.c read_manifest`):

| key | required | |
|---|---|---|
| `type=` | yes | the mod type ([3](#3-the-mod-types)). PC: case-insensitive. Android: a manifest without one is taken as `arena` |
| `id=` | yes | the folder name ([1](#the-folder-and-its-id)) |
| `name=` | no (default: the id) | shown in the launcher and, where it matters, in the game |
| `author=`, `version=` | no | shown in the launcher; `version=` drives updates and online keys |
| `made_with=` | no | the writer ([1](#versioning)) |
| `needs=` | no | the oldest game version that plays it (2.1.6) |

---

## 3. The mod types

The types, the folders they go to, and what the launcher requires before
it installs one (`launcher/mods_tab.c install_file`; Android
`ModsPage.install`):

| `type=` | folder | the launcher requires | the game reads it in |
|---|---|---|---|
| `superstar` | `Mods\Superstars\<id>` | `ch.pac`, `manifest.txt` | `src/superstar_mods.cpp` |
| `attire` | `Mods\Attires\<id>` | `attire.pac` | `src/superstar_mods.cpp` (attire mods) |
| `arena` | `Mods\Arenas\<id>` | `arena.pac`, `banner.dds` | `src/arena_mods.cpp`, `ring_rules.cpp`, `arena_lights.cpp` |
| `backstage` | `Mods\Backstage\<id>` | `arena.pac`, `manifest.txt` | `src/arena_mods.cpp`, `hctp_room.cpp`, `move_packs.cpp` |
| `moves` | `Mods\Moves\<id>` | `pack.txt` (PC only: Android has no route for it and refuses it) | `src/move_packs.cpp` |
| `signs` | `Mods\Signs\<id>` | nothing | `src/crowd_signs.cpp` |
| `Background` | `Mods\Backgrounds\<id>` | at least one `*.dds` | `src/backgrounds_page.cpp` |
| `media` | `Mods\Media\<id>` | nothing | `src/media_mods.cpp`, `hd_movies.cpp` |
| `matchtype` | `Mods\MatchTypes\<id>` | nothing | `src/match_types.cpp` (switches only, see 3.9) |
| `title` | `Mods\Titles\<id>` | `title=` key | `src/title_mods.cpp` (see `port/docs/TITLE_MODS.md`) |

Any other type is refused with "this version installs ...".

These also exist, but have no `.svrmod` route yet:

- weapon mods (`Mods\Weapons\<folder>`, 3.10);
- texture packs (`Texture Packs\`, 3.11).

### 3.1 type=superstar

A new playable character on one of 137 free roster ids
([5.2](#52-superstar-slots)). Read by `superstar_mods.cpp LoadMods`.

Files:

| file | |
|---|---|
| `ch.pac` | **required.** The model: an EPK8 like `pac\ch\chNNN.pac` ([6.2](#62-epk8-character-and-entrance-pacs), [4.2](#42-superstar-models-budgets-and-chpac)). Its EMD entries are renamed to the slot's id at load |
| `render.dds` | the select picture, 512 x 512 DXT5 (SSFA; also SSFB when there is no `render_full.dds`) |
| `render_full.dds` | optional: the full-body render (SSFB), 512 x 512 DXT5 |
| `render_small.dds` | the 256 x 256 bust (SSFC), DXT5 |
| `sign1.dds`..`sign4.dds` | optional: the fans' crowd signs, 128 x 64 DXT1 with mips |
| the `song=` file, the `movie=` file, the `voice=` file, the `loading=` file (`load.dds`) | as named |
| `attireN.pac` (any name, via `attire<N>=`) | optional: more attires, each another model pac's first attire |
| `moves/pack.txt` + `moves/...` | optional: a move pack carried inside the mod ([3.5](#35-typemoves)) |
| the `moves=` file | optional: the mod's own move-set over the base's |
| the `cae=` file | optional: a created entrance, a 284-byte entrance block exactly as the game holds it (big-endian) |
| `threads.txt` | optional: the mod's Superstar Threads parts ([5.9](#59-superstar-threads)) |

Manifest keys (in the order the converters write them,
`tools/superstar_port.py write_mod`):

| key | | |
|---|---|---|
| `base=<100-321>` | **required** | the stock superstar underneath: stats, moves, entrance motions, pyro, select render and the game's per-character data. It must have a select render (SSFA); otherwise the mod gets no picture and the log says "not a playable superstar". A mod without a valid `base=` is skipped |
| `name=` | | the full name, up to 31 bytes (record +34) |
| `short=` | | the short name (record +170) |
| `style=<text>` | optional | the Mod Maker's style name. Its presence makes the name call default to 77 ("The Superstar") when `call=` is missing |
| `ratings=a,b,c,d,e,f,g` | optional | grapple, submission, speed, strikes, hardcore, charisma, durability, each 1-99 |
| `abilities=a,b,...` | optional | up to 8 ability ids (1-255), e.g. 1 Dirty Pin, 7 Move Thief, 9 Hammer Throw, 11 Resiliency, 12 Durability, 14 Kip-Up, 19 Outside Dives, 20 Springboard Dives, 22 Leverage Pin, 23 Fired Up, 24 Ring Escape (record +230) |
| `moves=<file>` | optional | lines `0xOFF=<move id>`: the move at byte offset OFF (0..0x1BF, even) of the profile's move block. Only these slots change; the rest stay the base's |
| `entrance=<n>` | optional | the entrance motions and pyro: an installed entrance number (a stock superstar's, or one a move pack's `pacentry` lines add). Goes to profile +0x1C0 +0x14 (alternates +0x16 / +0x18) |
| `attire<N>_entrance=<n>` | optional | attire N's own entrance (N 1-4) |
| `cae=<file>` | optional | a created entrance block (284 bytes; any other size is left out) |
| `announcer=<NAME>` | optional | letters and digits, upper-cased, up to 24: a name the ring announcer's sound banks have clips of (e.g. `JEFFHARDY`). Wins over `voice=` |
| `voice=<file>` | optional | a recording of the name for the announcer |
| `loading=<file>` | optional | the match loading picture (1024 x 512 DXT5 .dds; default: the folder's `load.dds`): the NOW LOADING screen of a match the superstar is in - person 0's, else the next person's in slot order; it wins over a custom arena's own |
| `call=<0-83>` | optional | the Created Superstar nickname the announcer and commentary use |
| `hometown=<CITY>[,<STATE>]` | optional | letters and digits, upper-cased, up to 40 each. The ring announcer's clips `RA_*_HTC_<CITY>_0` / `RA_*_HTS_<STATE>_0`; the lists are in `tools/announcer_calls.json`. E.g. `hometown=DEATHVALLEY,PARTSUNKNOWN` |
| `height=<scale>` | optional | the size against the base's, 0.80-1.25 (clamped; missing or 0 = the base's). The game scales the skeleton (record +28, 4.12 fixed point: 4096 = 1.0, kept within 3300-4900). From 4139 on, the game counts the wrestler as "big". About 0.90-1.15 plays best |
| `pose=<id>` | optional | the select / VS screen pose: a pose motion id (32500 and up: 32500-32505 generic men, 32510-32515 women, 32520-32591 the superstars' own), or a superstar id below 512 for that superstar's pose |
| `song=<file>` | optional | the entrance theme (.mp3 .m4a .aac .wav .flac .wma .ogg). It plays at full level, so make it about -24 LUFS or use `theme_gain=` |
| `theme_gain=<dB>` | optional | -20.0..+6.0 dB (clamped). Left out, the PC launcher measures the song at install (or with Installed Mods -> Level music) and writes it, to -24 LUFS like the Mod Maker's meter (`launcher/music_level.c`) |
| `theme_gain_auto=<size>:<time>` | written by the launcher | marks a `theme_gain=` the launcher measured (the song's size and time then): measured again when the song changes. Without it, `theme_gain=` is the author's and is never changed |
| `music=<1-253>` | optional | instead of `song=`: one of the game's own themes by music id. 254 (the user playlist) and 255 (none) are not accepted. `song=` wins when both are given |
| `movie=<file>` | optional | the entrance (titantron) movie: a 320 x 320 Bink `.bik`, or an `.mp4` ([5.8](#58-media-theme-titantron)) |
| `attire<N>=<file>` | optional | N 1-4: another model pac whose first attire becomes attire N. When it is over the model budget, that attire alone is left out |
| `attire<N>_name=` | optional | the attire's name on CHANGE ATTIRE (attire 1: the game's ORIGINAL ATTIRE) |
| `needs=` | optional | see [1](#versioning) |

The game's record and profile offsets used above are listed in
[6.9](#69-chardat--charpro-roster-records-and-profiles).

### 3.2 type=attire

Another attire for a **stock** superstar (`superstar_mods.cpp`, attire mods;
`modmaker/attire_page.cpp`).

| key / file | |
|---|---|
| `attire.pac` | **required**: a model pac; its first attire is used |
| `superstar=<id>` | the stock superstar it is for |
| `attire=<1-4>` or `new` | which attire it replaces; `new` takes the first attire the superstar's own pac does not have |
| `model=attire.pac` | the model file (written by the Mod Maker) |
| `entrance=<n>` | optional: this attire's entrance |

Rules:

- Four attires at most per superstar.
- Of two mods on one attire, the first folder (A-Z) keeps it, and the other
  takes a free attire. The launchers move it for good in its manifest.
- The copy goes to `Mods\SuperstarOverlay\ch<id>_x<N>.pac`.
- An attire replacing one the game's pac has also becomes a generated move
  pack (one `pacentry` line) in `Mods\SuperstarOverlay\attirepacks`.

### 3.3 type=arena

A custom arena on the arena select's extra pages (`arena_mods.cpp
LoadCustomArenas`). Older mods may have `info.txt` in place of
`manifest.txt`; the game reads either.

| file | |
|---|---|
| `arena.pac` | **required**: the arena, an EPAC laid out like `pac\bg\bgNN.pac` ([6.1](#61-epac)). It must fit its base arena's shipped file and unpacked sizes ([4.4](#44-arenas)) |
| `banner.dds` | **required**: the select tile, 256 x 128 DXT5, no mips |
| `load.dds` | optional: the loading screen, 1024 x 512 DXT5 (anything else is left out) |
| `vs/<name>.dds` | optional: VS screen pictures replacing the base theme's texture `<name>`, DXT5, same size as the original |

| key | |
|---|---|
| `base=arena_XX` | **required**: the stock arena it plays as. It takes that arena's memory room, VS screen style and per-arena data. One of `arena_SD` (0), `arena_RAW` (1), `arena_ECW` (2), `arena_ttt` (3), `arena_slam` (4), `arena_BP` (5), `arena_HinC` (6), `arena_Series` (7), `arena_TLC` (8), `arena_RR` (9), `arena_EC` (10), `arena_WM26` (11), `arena_back` (12), `arena_Judg` (13), `arena_NoC` (14), `arena_BASH` (15), `arena_EXT` (16), `arena_SS` (17), `arena_BR` (18), `arena_drui` (19); the number is the game's arena number, bgNN (`arena_mods.cpp kTiles`) |
| `ring.ropes=b m t` | which ropes exist (1/0 each; bottom, middle, top). With none at all, rope gameplay is off (no rope breaks, runners brake) |
| `ring.rope_base=`, `ring.rope_gap=` | the bottom rope's height above the mat and the gap, in game units (1 = 10 cm). Kept in 1.0-8.0 and 1.5-6.0 with the top rope at most 12.5, else the game's own (`ring_rules.cpp`) |
| `light_scale=<k>` | the arena's light strength, 0.25-4.0, times the player's ARENA BRIGHTNESS (`arena_lights.cpp`) |
| `match_light=entrance` / `entrance_spot` | the match lights (light sets 10-14) take the entrance set 200's channels (`_spot` also its key light) |
| `titantron=live` | keep the match camera on the arena's screens (models 800-899); default: entrances only ([5.8](#58-media-theme-titantron)) |
| `source=` | Mod Maker: where the arena came from (not read by the game) |
| `ring.tints=`, `ring.turnbuckles=`, `ring.pads=`, `light.color=`, `light.strength=`, `crowd=0` | Mod Maker notes. Already baked into `arena.pac`, read back when the mod reopens (`modmaker/svrfmt/ring_kit.cpp`, `modmaker/editor.cpp`) |

### 3.4 type=backstage

A rebuilt backstage room (`pac\bg\bg78.pac`), or an area with its own menu
row (`arena_mods.cpp LoadBackstage`; `modmaker/arena_page.cpp`).

| key / file | |
|---|---|
| `arena.pac` | **required**: the whole `bg78.pac` with one room rebuilt |
| `area=<0-6>` | the room: 0 parking lot, 1 GM's office, 2 locker room A, 3 locker room B, 4 large locker room, 5 interview area, 6 catering |
| `row=<label>` | optional: an area of its own, as a row in ONE ON ONE -> BACKSTAGE after its room's row; the mod plays only there. At most 8 such rows. Without `row=`, the mod replaces the room for every backstage match; of several, one plays (the last enabled folder by name) |
| `box=x,z,hx,hz` | optional (with `row=`): the fight box's centre and half sizes, in game units |
| `camera=<units>` | optional: caps the match camera's distance |
| `camera_height=<units>` | optional: the camera's eye at least this high |
| `gimmick=<4 chars>` + `gimmick.pac` | optional (with `row=`): a pac holding only `GMGB/<name>`, the area's cars, props and hot spots, played instead of the room's |
| `start=x,z,facing[,y];...` | optional: the wrestlers' start places |
| `weapons=model,x,y,z,yaw;...` / `weapons=none` | optional: placed weapons (a model number below 256, or a weapon mod folder with `pool=backstage`), or none |
| `load_weapons=a,b,...` | optional: weapons to load for the area (numbers or weapon mod folders) |
| `hctp=<room>` | optional: an HCTP (SmackDown! Here Comes the Pain) room ported by the converters, e.g. 30 the Back Entrance. Turns on that room's rules (doors, climb walls) from the mod's `hctp.txt` (`hctp_room.cpp`) |

### 3.5 type=moves

A move pack (`move_packs.cpp`; written by `tools/movepack.py`,
`tools/moves_port.py`, `tools/ps2_moves.py`, the Mod Maker's Moves page).
The folder holds `pack.txt` and the files it names. A superstar mod can
carry the same pack in its `moves/` subfolder.

`pack.txt`, one item per line:

```
motion <m.pac|mpsp.pac> <TYPE/NAME/child/...> <id> <x> <y> <frames> <file> [<20 bytes hex>]
waze <id> <16 bytes hex>        the move's category bits (which move-set slots take it)
wazename <id> <name>            the move record's name
wazecopy <id> <from id>         a new move on a free record: a copy of another move's record with its own id (applied before waze / wazename)
exh <group> <36 bytes hex>      WAZA/DATA child 0, per-key move data
evt <group> <16 bytes hex> <events hex>   child 1, per-key sound events
mbd <group> <8 bytes hex>       child 2
pacentry <pac under pac\> <GROUP> <name> <file>
propfallback <new prop id> <game prop id>
```

- **`motion`:** a motion for the YMKs / YMBs bank at that path in `m.pac` or
  `mpsp.pac`. The key is (id, x = phase or variant, y = track: 0 attacker,
  1 victim, others props and cameras). YMBs banks (taunts, submissions,
  finishers: the ADPCM ones) also take the motion's 20-byte header (type,
  segments, interval, 0, 4 floats).
- **`pacentry`:** a whole entry, unpacked, of any EPK8 or EPAC pac, added or
  replacing the game's. Examples: an entrance `evt\Nyujyo5.pac EVPE/EVTE
  <number>`, or its pyro `prtcls.pac PCE6/LPE6 <0NNN>` (4-character names in
  EPAC). This is how converted entrances get in.
- **`propfallback`:** an entrance prop the pack adds to `evt\evtobj.pac SOBJ`
  (e.g. a ported bike). When that copy cannot be made, the scripts use the
  game's prop instead.
- **Keys the game already has are left as they are:** a pack adds; it does
  not replace stock motions.

### 3.6 type=signs

Any `*.dds` in the folder: 128 x 64 DXT1 with mips, as the game's own
(`audience.pac AUDE/BORD`). They join the general signs every match draws
its free sign slots from (ids 6001, 6011, ...). The Create-A-Superstar sign
picker keeps the game's 437 (`crowd_signs.cpp`).

### 3.7 type=Background

Main menu pictures, shown after the game's own 12. Each is 1024 x 512 DXT5,
one level, already fitted to where the menu draws its picture
(`launcher/bg_map.h`; `backgrounds_page.cpp`).

| key / file | |
|---|---|
| `picture.dds` | one picture (its name: `name=`) |
| `NN_<name>.dds` | a pack's pictures, in order |
| `logo=0\|1` | the game's logo over them (0: hidden) |
| `title.<file>=` | a pack picture's name in the game (default: the file's name) |
| `logo.<file>=` | one pack picture's logo (written by the game's BACKGROUNDS page) |

The game writes `disabled` (a single picture, or the whole pack off) and
`<file>.off` (one pack picture off).

### 3.8 type=media

Replaces the game's own media. Later packs (by folder name) win.
`<id>` is a character id (`media_mods.cpp`).

| key | |
|---|---|
| `video.<id>=<file>` | that superstar's entrance (titantron) video: a 320 x 320 `.bik`, or an `.mp4` played HD ([5.8](#58-media-theme-titantron)) |
| `theme.<id>=<file>` | the entrance theme |
| `theme_gain.<id>=<dB>` | that theme's level, -20..+6 (left out: the PC launcher measures and writes it, as `theme_gain=`, with `theme_gain_auto.<id>=`) |
| `render.<id>=`, `bust.<id>=`, `icon.<id>=` | the 512 x 512 render, 256 x 256 bust, 64 x 64 face icon (DDS, DXT5) |
| `arena.<nn>=<bgNN.pac>` | arena nn's own file with its screens' flip-book retextured (the Mod Maker's Media page) |
| `sound.<event>=<file>` | any game sound by its event name without `Play_` (e.g. `Menu_Music`, `MUS_0161_0_0`) |
| `menu_music=<file>` | = `sound.Menu_Music` |

### 3.9 type=matchtype

A manifest only (`docs/MATCH_TYPE_MODS.md`).

- **Kind 1** (`builtin=<key>`) switches one of the port's own match types; a
  `disabled` file leaves it out of the menus. **The game reads only these
  switches.**
- **Kind 2** keys (`base=`, `menu=`, `people=`, `slots=`, `opt.<rule>=`,
  `weapons=`, ...) are written by the Mod Maker's Match types page but not
  read by the game yet.

Match type switches are not part of the online mod set.

### 3.10 Weapon mods (folder only)

`Mods\Weapons\<folder>\` (`src/weapon_mods.h`):

- `weapon.pac`: a PACH like a `bgEtc.pac STG/WPON` entry (model JBOY at 0x0,
  texture set at 0xA, ...);
- `manifest.txt`: `name=`, `like=<stock weapon id>` (its settings are
  copied: grip type, sounds, weight, hits to break, damage, hit spheres),
  and `pool=extreme` / `backstage` (both when not given).

Each mod gets a weapon id from 160-255, kept in `Mods\Weapons\ids.txt`. The
game's weapon tables are rebuilt in memory at start; the game's file is
never written.

### 3.11 Texture packs (not .svrmod)

These are folders in `Texture Packs\`. Any file whose name ends with
`_<hash16>` (or is `<hash16>`), `.png` or `.dds`, replaces the texture with
that XXH3 content hash of its untiled top level, at any resolution
(`src/native/texture_packs.h`). They are a renderer feature, not a mod type.

---

## 4. How the Mod Maker and the converters build mods

### 4.1 The pipeline

The Mod Maker (C++ / ImGui) has one page per type. A page builds the files
in memory: its `PrepareJob` / `Build` makes the manifest, and `FinishBuild`
adds `manifest.txt`, `.mm_project.txt` and the payload. `ZipWrite` stores
them. Install is the same zip unpacked into `Mods\`.

Converting another game's data is done by the Python tools ([8](#8-tools)),
which the Mod Maker runs:

- **WWE '12 / '13 / 2K14-2K17 characters:** `superstar_port.py` drives
  `w13_char.py` (model), `ratings_port.py`, `abilities_port.py` and
  `moves_port.py`.
- **SvR 2007-2010 characters:** `svr07_char.py`, `svr08_char.py`,
  `svr10_char.py`, `svr10_superstar.py`.
- **PS2 discs:** `ps2_port.py`, `ps2_char.py`, `ps2_moves.py`.
- **FBX:** `svr11_build.py`, with `modmaker/svrfmt/char_fbx.*`.
- **Arenas from other games:** `arena_port.py`.
- **Entrances:** `hctp_entrance.py`, `jbi_entrance.py`, written as move-pack
  `pacentry` lines.
- **Superstar Threads parts:** `threads_parts.py`.
- **Single character pacs:** `lone_pac.py` finds which game and format a
  single character pac is from.

Every converter writes the plain mod folder, then packs it; the manifest
gets `made_with=Port tools (...)`.

The converters build a 2011 model on a template, the stock `ch104.pac`
(`superstar_port.py template104`). If its file lacks the needed children,
the template falls back to ch103 / 100 / 106 / 115 / 126. They copy its
offset, spring, capsule and face children (0x28, 0x32, 0x50, 0x64) and
retarget the source skeleton to 2011's.

For WWE '13-era models, robert32's SvR Port Doctor findings were re-checked
and used:

- at most 20 bones per mesh palette;
- the rigid accessory skeleton;
- the crash with two-section meshes;
- face pivots;
- alpha seams.

### 4.2 Superstar models: budgets and ch.pac

**ch.pac** is an EPK8 with one `EMD` group. Entry names are `"%06d%02d"`:
the character id, then attire * 10 + kind (2 = the model, 4 = the entrance
outfit). For example, `00010402` is id 104, attire 1, model. Each entry is a
PACH ([6.3](#63-pach)) whose children are:

| child | content |
|---|---|
| 0x0 | body JBOY |
| 0x2710 (10000) | hair JBOY, with its own node table (hair chains `kami_bNN_MM` under `atama`) |
| 0x4E20 | lashes JBOY |
| 0xA, 0xC | texture bundles ([6.5](#65-texture-bundle)): colour, normal, masks |
| 0xD-0xF | damage / blood mask sheets |
| 0x28 (40) | body offsets (text) |
| 0x32 (50) | spring muscle settings per body part (Shift-JIS text) |
| 0x3C (60) | spring hair settings per `kami_b` chain (text; at most 16 chains) |
| 0x3D (61) | hair collision capsules (`<bone> x0 y0 z0 r0 x1 y1 z1 r1 5` lines) |
| 0x50 (80) | capsules (binary) |
| 0x64 | face animation (a PACH) |
| 0xC8, 0xC9 | portraits (DDS) |
| 0x7530-0x7532 | blood overlay JBOYs |

**The vertex budget.** A match loads each wrestler's vertex and index
buffers through allocator 128. Until 2.1.5 these sat in a 768 KB window per
wrestler: 192 pages of 4 KB. The page count (`tools/model_budget.py`, the
same rule in `superstar_mods.cpp FitModelBudget`) works per mesh:

- the vertex buffer is vertices x 32 bytes with one weight block, or x 44
  with 2-4 blocks;
- the index buffer is indices x 2 bytes;
- each buffer is rounded up to whole 4 KB pages.

Only the kind-2 entries count. If a model is over the limit, the game:

1. drops the blood overlays, in the order 0x7532, 0x7531, 0x7530;
2. if it is still over, leaves the mod (or that attire) out and lists it in
   `Mods\Superstars\left_out.txt`.

Since 2.1.6:

- A full window takes fallback memory (`src/model_mem.cpp`), and the limit
  is **1024 pages** (`caw_limits.h ModelPageLimit`). That is about
  70,000 vertices; a 38,000-vertex model plays in 1v1, 6-man and Royal Rumble
  matches.
- The tools: `model_budget.py` has `LIMIT 1024`, `OLD_LIMIT 192` and
  `WARN 980`. The Mod Maker's Reduce detail goes up to 32,000 vertices.
- A mod over 192 pages gets `needs=2.1.6`; older games leave it out (no
  crash).

**Moving hair.** A model with hair chains also needs room for the blood
overlay copy built at load (`sub_826B6BA8`). The WWE '13 converter keeps
such a model under `w13_char.CHAIN_MODEL_BYTES_MAX`: 1,006,376 bytes of
unpacked JBOY children up to 2.1.5, 4,000,000 since 2.1.6. When it is
over, it drops blood, then lashes.

**Textures and blood.**

- Colour, normal and mask sheets are DXT in texture bundles.
- The skin's shine is the normal map's alpha with the skin shader (a culled
  `yBumpMapSS` mesh with `texTilingMap` and `g_fSweatSPA` set); every other
  shader uses the colour's alpha.
- Hair cards and lashes are alpha cut-outs: mesh flags 0x10 | 0x100.
- Blood is the 0xD-0xF mask sheets plus the 0x7530-0x7532 overlay meshes.

**The skeleton.** The game solves limbs (IK) on the template skeleton
(ch104) times `height=`, not on the model's own bones. So a model whose limb
lengths differ much from ch104's will have its grapples and hand contacts
off.

### 4.3 Move packs, YMKs and WAZE

A move is its motions plus a few tables:

- **motions:** YMKs / YMBs banks ([6.7](#67-ymks--ymbs-motion-banks)) in
  `m.pac` (MOT/BM.., MOT/RR.., MVMT/.. menu banks) and `mpsp.pac`
  (MOTP/GAME: the banks a match loads for every superstar);
- **`misc.pac` MOVS/WAZE:** the move record ([6.8](#68-movswaze-and-wazadata));
- **`misc.pac` WAZA/DATA:** EXH, sound events and MBD per key.

Converters:

- `movepack.py` cuts a pack from a game's banks.
- `moves_port.py` ports moves from other games: WWE '12-2K15 YANM curves are
  decoded by `yanm_decode.py` and retargeted by `w13_retarget.py`. Events,
  EXH and WAZE come from a similar stock 2011 move.
- `ps2_moves.py` ports PS2 movesets.

### 4.4 Arenas

An arena mod is a whole `bgNN.pac`.

- **The arena's memory.** The game keeps each arena in fixed memory: an
  arena that outgrows its shipped size fails to load (a crash, or endless
  NOW LOADING). `modmaker/svrfmt/arena.h Arena::FitFile` halves the largest
  textures (those not marked to keep) until the file and its unpacked total
  are no bigger than the base arena's shipped `bgNN.pac`. Lifting this rule
  is open work.
- **Mesh count.** A stage of about 400 meshes or more crashed the model
  loader in 2.1.5 (the same allocator-128 window as the superstars). The
  2.1.6 fallback memory is expected to lift it. Until that is tested in a
  game, the converters and the Arena Editor keep their mesh caps.
- **Converted arenas.** `arena_port.py` converts arenas from other games
  (SvR 2007-2010, WWE '12-2K17). It writes `base=` (the closest 2011 arena),
  a banner from the game's VS art, and `load.dds` / `vs/`. The Mod Maker's
  3D Arena Editor edits models, ropes, turnbuckles and lights, then bakes
  them into the pac.
- **Backstage rooms.** These rebuild one room of `bg78.pac`; only that room's
  own textures may be reduced.

### 4.5 Entrances (evt)

An entrance is a numbered script plus motions in an `evt\*.pac` EPK8
(EVPE / EVTE `<number>`), its pyro in `prtcls.pac` (PCE6 / LPE6), and its
props in `evt\evtobj.pac` SOBJ. Ported entrances come as move-pack
`pacentry` lines on free numbers. A superstar mod points at one with
`entrance=` / `attire<N>_entrance=`. CREATE AN ENTRANCE lists them too
([5.10](#510-create-an-entrance-mod-rows)).

---

## 5. How the game installs and loads mods

Mods are read once at start; a change needs a restart. "Start Without Mods"
(`no_mods.h`) skips every loader. Folders with a `disabled` file are skipped.

### 5.1 At start

Each loader reads its folder once, before the menus:

- **Superstars:** folders are sorted by name, and each one is given its slot
  in that order. Their overlay files are made in `Mods\SuperstarOverlay`.
  Once the file system is up, the game's own pac mount (`sub_826A1780`)
  mounts them as `smods:\<file>`.
- **Move packs:** merged into `Mods\PacOverlay` (5.4).
- **Weapons:** the weapon tables are rebuilt in memory from `bgEtc.pac`,
  with the mods' packs added.
- **Arenas:** in install order. **Backstage, media, signs, backgrounds:** in
  folder name order.

### 5.2 Superstar slots

The game indexes 242 character ids (0-321). A mod takes one of **137**
(`superstar_mods.cpp kPool`):

- 61 disc ids whose roster record is a blank `"0"` placeholder;
- the free DLC ids 59-69;
- 65 ids the game keeps "absent", made present for a mod.

Each slot has a 260-byte record, a 1056-byte profile and save room.

- **`Mods\Superstars\slots.txt`:** lines of `<folder>\t<id>`. A mod keeps
  its id across starts and while it is switched off, so the saves (which
  hold records by id) keep its edits.
- **When every id is taken**, a new mod gets an id held by a folder that is
  gone or switched off; then that slot "changes hands". The new mod gets a
  fresh copy of its base's profile, not the old mod's moves and entrance.
  Mods holding ids come first, and nothing moves from a mod that is on.
- **Other files the game writes:** `limit.txt` (137, for the launcher) and
  `left_out.txt` (mods over the limit or the budget).
- **The record and profile.** They are copied from `base=` into the slot,
  with the mod's names, selectable (+221), DLC (+257) and its own id as
  "same person" (+228). This is repeated after the loaders, after a save
  load, and before every roster list. DLC-flagged ids must be owned; mods
  are.
- **Edits kept.** The player may edit a mod in the game: ratings, move-set,
  entrance. A slot that still holds a value from `.applied` follows the
  manifest; any other value is the player's and stays.
- **WWE Universe** keeps 150 places in the save. Mods get a side table
  (`Saves\.universe\mods.bin`, keyed by folder), so they take no created or
  DLC places (`universe_mods.cpp`).

The overlay files (in `Mods\SuperstarOverlay`, mounted as `smods:\`):

| file | content |
|---|---|
| `chNNN.pac` | the model with its EMD names renamed to the slot |
| `chNNN_aN.pac` | extra attires |
| `ssfNNN.pac` | the select renders (SSFA / SSFB / SSFC `"%04d"` = attire * 1000 + id), from the mod's DDS files or the base's |
| `ch<id>_x<N>.pac` | attire mods |
| `media.pac` | media renders |
| `threads.pac` | Superstar Threads |

Each overlay file is rebuilt only when its sources change (a `.src` stamp of
sizes and times).

### 5.3 VFS and file name redirects

The game finds files by name. Three mechanisms point names at mod files:

- **Mounted overlay pacs** (`smods:\`) add entries the game looks up by
  entry name: models, renders.
- **The file name resolver** (the `sub_826B87B0` hook) answers render names
  with the media pack's RSFA / RSFB / RSFC entries in place of
  SSFA / SSFB / SSFC.
- **A directory link** for arenas: `\PAC\BG` is linked to
  `Mods\ArenaOverlay`. That folder holds hard links (or copies) of every
  original arena, with a redirected arena's entry pointing at the mod's
  file. Arenas are only open while they load, so entries are swapped between
  matches. The originals are never written.

### 5.4 The PacOverlay merge (move packs)

At start every enabled pack is read: `Mods\Moves\*`, `Mods\Superstars\*\moves`
and the generated attire packs. Then:

- **The merged copies.** Copies of `m.pac`, `misc.pac`, `mpsp.pac`, and any
  pac a `pacentry` names, are written to `Mods\PacOverlay`, together with
  copies of `plist360.h` / `plist360_4x3.h` naming them. Unchanged entries
  are streamed from the originals; changed ones are re-compressed (real
  BPE). The game reads its pac list from there and mounts each copy by its
  own table.
- **DLC packages.** Their `m` / `mpsp` / `misc` pacs get overlay copies
  too, plus copies of the package's `info\catalog_hd.arc` /
  `catalog_sd.arc` with the copies' places. The package itself is never
  written. A copy whose entries cannot all be found in a catalog is dropped,
  and the package's own pac plays.
- **The stamp.** A stamp (format `"movepacks 13"`, `move_packs.cpp
  kFormat`, plus every pack file's size and time) decides whether to merge
  again. A new format number re-merges every install once.
- **The merge lock.** `Mods\PacOverlay\merge.lock` is a folder, so one game
  merges at a time. A stale lock is taken over.
- **First wins.** Of two packs' copies of one motion, the first read wins
  (folders in name order).

### 5.5 Arena tiles, pages and the base arena

- **Page 1** is the game's 20 arenas. **Pages 2 and on** hold custom arenas
  in install order (`Mods\Arenas\order.txt`, one folder per line, written
  by the game), 20 per page, with no gaps.
- **Navigation:** LB / RB, or 1 / 3 and Q / E.
- **Install order.** New folders go last, by name; removed ones leave the
  list. Switched-off ones keep their place.
- **The base arena.** Whatever tile a mod is on, its match plays as its
  `base=` arena.
- **The titantron.** The game shows videos on an arena's models 800-899.
  On custom arenas those are flagged to show during entrances only (a copy,
  `Mods\ArenaOverlay\tron_<id>.pac`), so a ported arena's own screen art
  shows in the match. `titantron=live` turns that off.

### 5.6 Backstage rooms

- Room replacements (no `row=`) apply to every backstage match. Of several,
  one plays: the last enabled folder by name.
- Rows (`row=`, at most 8) appear in ONE ON ONE -> BACKSTAGE after their
  room's row.
- A gimmick pac goes through the move-pack merge.
- `hctp=` rows load the HCTP room rules from `hctp.txt`.

### 5.7 Crowd signs

Sign packs join the general signs (ids 6001+). A superstar mod's
`sign1..4.dds` go in as `id * 10 + 1..4`, replacing a DLC slot's dummies.
The merged bank is handed to the match loader in place of the file's.

### 5.8 Media, theme, titantron

- **Videos.** These are copied into the game's Custom Movies (user movie
  ids). A match's copy of the wrestler's profile gets the replacement in its
  entrance block (+0x1C0: music +0x10 = 254 with the playlist name at +0xCC,
  movie +0x12). Only the match's copy changes; the roster and the saves keep
  the game's values.
- **Themes.** These go to the Music folder as a user playlist.
- **Sounds.** Every game sound is an event posted by name. A replaced
  `Play_<event>` plays the file on a host voice, tied to the event's object
  (stop, pause and release follow it).
- **HD movies (`.mp4`).** Played with the platform's decoder (Windows: Media
  Foundation) at up to 1024 x 1024 on the titantron (`hd_movies.h`). A stub
  `.bik` of the movie's size and length goes into `Custom Movies\.hd`; the
  game's Bink player keeps the timing, and the frames are the decoder's. The
  launcher also makes a `.bik` fallback beside an `.mp4` (`mods_tab.c
  make_movie_fallbacks`), for platforms that cannot decode it.

### 5.9 Superstar Threads

Threads lists superstars from `edit\edit.pac EDIT/SSTL`. Each 484-byte record
has:

- the id;
- 48 part-name string ids (4 attires x 12; -1 = none);
- per part, an area, view and kind byte;
- default RGB values.

Part p paints channel (p-1) % 4 of the attire's Mask1/2/3 texture.

A mod's record is one of these:

- the base's record, when its model is the base's attire-1 model byte for
  byte;
- else one built from its `threads.txt`, lines of
  `<attire>.<part>=<name id>,<rrggbb>` (`tools/threads_parts.py`);
- else none: the mod is not listed.

The mod's slots live in `Saves\.threads\<folder>.bin`
(`superstar_threads.cpp`).

### 5.10 CREATE AN ENTRANCE mod rows

Entrances that packs add (`pacentry`) appear as a "- MODS -" group in both
CAE editors' motion lists. Rows are named `MOD: <label>`, the label being
the mod's `name=` (`cae_mod_motions.cpp`, `move_packs.cpp`). A saved
entrance keeps only numbers. If the mod is gone, the editor shows a
"MISSING MOD" row, and the match plays the superstar's own entrance (or
the default part). ADVANCED CREATION opens for mod superstars too.

### 5.11 Online

What both players need (`online_files.h/.cpp`, `online_mods.h`):

- **The mod set** is one line per enabled `Superstars`, `Moves`, `Arenas` or
  `Backstage` mod: `kind \t id \t version \t CRC \t name`.
  - The CRC folds, for every file in path order, the relative path and the
    file's CRC-32.
  - It leaves out cosmetic files (`.png .jpg .jpeg .bmp .dds .tga .ogg .wav
    .mp3 .xma .bik .wmv .mp4 .db .ini`) and dot files.
  - So pictures, sounds and movies may differ; data may not.
- **Move packs and backstage must match** to share a session.
- **Superstar and arena mods are per lobby, by key.**
  - A superstar mod can be picked when every player has it.
  - The host's custom arena plays when everyone has it. Otherwise START
    waits, and the chat says who lacks what.
  - Mod superstar ids are each PC's own (`slots.txt`), so they are
    translated where picks and records arrive.
- **Weapon mods** play in the host's Extreme Rules matches when every player
  has them.
- **Signs, media, backgrounds, texture packs and match type switches** are
  not compared.
- **Community Creations** refuses uploads that name a pack's move or
  entrance (a superstar's move-set or entrance, a move-set) at the confirm
  step. Allowed uploads carry an `X-Svr-ModFree` header
  (`online.cpp`, `online_net.cpp`).
- **Discovery** (the launcher's Mods -> Discovery, `discover_tab.c`)
  uploads the `.svrmod` as is, with up to four pictures (made 1024 x 768
  JPEG by the launcher).

### 5.12 Bundled mods

The release ships mods in `Bundled Mods\`. The launchers install them and
keep `Mods\.bundled_state` (`launcher/mods_tab.c`, Android
`BundledRules.java`). The file holds, per bundle:

- its version and folder;
- its state: `on`, `removed`, `kept <version>` or `retired`;
- per installed file, its size, time and FNV-1a 64 hash.

The rules:

| situation | what happens |
|---|---|
| never installed | installed switched off (except match types) |
| folder removed by the player | stays removed |
| newer version, unedited copy | updated, keeping on / off |
| newer version, edited copy | the player is asked: update (their copy goes to `Mods\.backup`) or keep theirs |
| unedited copy missing files | the player is asked; never fixed silently |
| the player's copy is newer | left alone |

Every decision is logged to `Mods\.install.log`. Bundle file counts skip
dot files and `disabled`.

### 5.13 Android

The game code is the same; differences are in install and platform:

- **Installing.** Dot files are not installed, ids keep `[A-Za-z0-9_-]`, a
  missing `type=` means arena, and `type=moves` has no route.
- **Graphics.** The renderer is Vulkan.
- **HD movies.** `.mp4` plays only where the phone decodes it; otherwise the
  `.bik` fallback plays.
- **Test status.** Big models (2.1.6) have not been tested on phones yet.

---

## 6. The game's containers

All 2011 Xbox 360 data is big-endian except where marked.

| container | used for | read and write |
|---|---|---|
| EPAC | arenas, menus, misc, sounds | `tools/svrfmt.py`, `modmaker/svrfmt/pac.h` |
| EPK8 | `ch\`, `evt\` | `tools/ch_tool.py`, `modmaker/svrfmt/pac.h` |
| PACH | the index inside entries | `tools/svrfmt.py`, `svrfmt::PachWrite` |
| BPE | entry compression | `svrfmt.bpe_decode / bpe_pack`, `svrfmt::Unpack` |
| JBOY | models | `tools/jboy.py`, `modmaker/svrfmt/jboy.h` |
| YMKs / YMBs | motions | `move_packs.cpp`, `tools/movepack.py` |

The pac editor (`modmaker/svrfmt/archive.h`) opens the archives of SvR 2007
through WWE 2K17: EPAC, EPK8, DPAC / DPK8 (PS2), and xcompress / ZLIB
wrappers.

### 6.1 EPAC

Little-endian.

| offset | content |
|---|---|
| 0x00 | `"EPAC"` |
| +4 | table bytes |
| +8 | data bytes |
| 0x800 | the group table |
| 0x4000 + sector * 0x800 | the data |

The group table holds 12-byte group headers `{char type[4], u32 entries*3,
u32 0}`. Each is followed by 12-byte entries `{char name[4], u32 sector,
u32 size / 0x100}`.

### 6.2 EPK8 (character and entrance pacs)

Like EPAC, but the entries are 16 bytes, `{char name[8], u32 sector, u32
size / 0x100}`, and the group's count is a u16. Character pacs hold one `EMD`
group named as in [4.2](#42-superstar-models-budgets-and-chpac).

### 6.3 PACH

`"PACH", u32 count`, then count x `{u32 id, u32 offset, u32 size}`.

- Offsets run from the end of the table.
- Entries are 4-byte aligned.
- The game looks ids up by binary search, so **keep ids sorted**.
- Children may themselves be PACH, BPE or JBOY.

### 6.4 BPE

Yuke's byte-pair compression (after Philip Gage):

- header: `"BPE "`, u32 0x100, u32 packed size, u32 unpacked size;
- then blocks of at most 4000 bytes, each a pair table, a u16 LE length and
  the data.

Entries are decoded in place, so the packed data must be smaller than the
unpacked.

### 6.5 Texture bundle

`{u32 count, 0x100, 0, 0x10}`, then 32-byte records `{char name[16], "dds\0",
u32 size, u32 offset}` and plain DDS data.

- Pictures the game draws straight (banners, loading screens, renders) are
  DXT5 with no mips unless stated.
- Signs are DXT1 with mips.

### 6.6 JBOY (YOBJ models)

`"JBOY", u32 len, data[len], "POF0", u32 len, pointer-fixup bytes`. Every
pointer is an offset from byte 8 and is listed in POF0 (`tools/jboy.py`).

The data block:

| offset | content |
|---|---|
| 0x18 | mesh count |
| 0x1C | -> meshes |
| 0x20 | node count |
| 0x24 | texture count |
| 0x28 | -> nodes |
| 0x2C | -> texture names (16 bytes each) |
| 0x30 | -> object name |

Each mesh record is 0xB4 bytes:

| offset | content |
|---|---|
| +00 | vertex count |
| +08 | palette count |
| +0C | palette[20]: node index + 1 |
| +5C | weight block count |
| +68 | -> -> vertices: pos f32x3, normal f32x3, colour ARGB (28 bytes) |
| +6C | -> weights: u8 bone[4] (255 none) + f32, per block |
| +70 | -> UVs (f32 x2) |
| +74 | material index |
| +78 | shader name[16] |
| +88 | render flags: 0x2 back-face cull (skin 0x6), 0x4 double-sided, 0x10 / 0x100 alpha |
| +8C | vertex format |
| +90 | parameter count |
| +94 | -> parameters |
| +98 | -> strips `{u32 prim (6 = strip), u32 count, -> u16[count]}` |
| +A4 | bounding sphere |

- **Parameters** are `{name[16], u16 type, u16 size, value}`, e.g.
  `g_fSpecularLev`, `g_f4MatDifCol` (the tint) and `g_fSweatSPA`.
- **Nodes** are 80 bytes: name[16], t f32x3 at +16, r f32x3 at +32 (Euler
  radians, R = Rz Ry Rx), parent at +48 (-1 = root), and the bounding
  sphere at +64.
- **Units and axes:** 1 unit = 10 cm, and -Y is up.

### 6.7 YMKs / YMBs (motion banks)

| offset | content |
|---|---|
| 0x00 | `"YMKs"` |
| 0x10 | a 256-byte event-size table |
| 0x110 | u32 count |
| 0x114 | count x 16-byte entries `{u8 y, u8 x, u16 id, u32 offset, u32 frames, u32 runtime}` |

- Entries are sorted by key, (id << 16 | x << 8 | y).
- The motion data follows in directory order; an entry's size is the next
  offset minus its own.
- YMBs banks (menu motions, taunts, finishers) hold ADPCM channels.
- The same layout carries arena light banks (light channels, sets 10-14 for
  the match, 200 for entrances; `arena_lights.cpp`).

### 6.8 MOVS/WAZE and WAZA/DATA

`misc.pac`:

- **MOVS/WAZE:** 160-byte move records, with the 16 category bytes at +0,
  the name at +0x10 and the id at +0x90.
- **WAZA/DATA** children, each a PACH of groups sorted by (u16 id, u8, u8):
  - 0: EXH, 36-byte per-key move data;
  - 1: sound events, 16-byte records plus an event pool;
  - 2: MBD, 8-byte records.
- **BATS/INIT and BATH/INIT** hold the same three tables as children 10, 11
  and 12.

### 6.9 CHAR/DAT / CHAR/PRO (roster records and profiles)

`chEtc.pac`.

**CHAR/DAT** records are 260 bytes, after a 4-byte header:

| offset | content |
|---|---|
| +0..6 | ratings |
| +28 | height scale (4.12) |
| +32 | id |
| +34 / +102 / +170 | names (32 bytes each) |
| +210 | sign ids, u16[4] |
| +221 | selectable |
| +226 | select tile |
| +228 | same person |
| +230 | abilities, u8[8] |
| +257 | DLC flag |

**CHAR/PRO** profiles are 1056 bytes:

| offset | content |
|---|---|
| +0..0x1BF | the move slots |
| +0x1C0 | the entrance block: music +0x10, movie +0x12, entrance number +0x14 / +0x16 / +0x18, playlist name +0xCC |
| +0x2DC / +0x348 / +0x3B4 | three 108-byte Threads slots |

### 6.10 String tables

`string.pac` SDB. Entry names are `NNLL`:

- LL is the language: 00 EN, 02 IT, 03 FR, 04 DE, 05 ES, 06 JA;
- NN 64 means all 14 tables.

Layout: `{u32 0, u32 count}`, then 16-byte records `{str_off, str_len, id,
0}` sorted by id. The text is UTF-8.

---

## 7. Supporting .svrmod in another game (e.g. WWE '12)

WWE '12 on the 360 is close to 2011 (`docs/OTHER_GAMES.md`):

- **Archives:** EPAC, with EPK8 for `ch\` and `evt\`; some leaves are ZLIB.
- **Arenas:** plain EPAC files in WWE '13's model format.
- **Characters:** the same EPK8 / EMD / PACH container, but '13-style models
  (a Biped rig in cm, 0xB8 mesh records, `yCh_*` shaders).
- **Textures:** 2011's texture sets.
- **Motions:** ZMOT / YANM curves, not YMKs.
- **Moves:** 160-byte WAZE records with '13-style move ids, and no EXH.
- **Profiles:** 1072 bytes, with the entrance block at +0x1D4.

### Portable as is

- The container ([1](#1-the-container)): the zip, the folder per id, dot
  files as local state, `disabled`, version comparison and `needs=`.
- The manifest's syntax and the common keys ([2](#2-manifesttxt)).
- Media keys whose files are plain: songs (`song=`, `theme.<id>=`,
  `theme_gain`), `.mp4` movies, `sound.<event>=` (if the game's sounds are
  posted events by name, as in 2011's Wwise build), and the picture slots
  as DDS (with sizes checked against the host).
- The ideas behind the slot pool and `slots.txt`, the arena pages, the
  online mod set CRC (with its cosmetic list), and the bundled-mod rules.

### 2011-specific (a WWE '12 loader must map or refuse these)

- **Model data.**
  - `ch.pac` content is 2011's JBOY (0xB4 meshes, 2011's skeleton, 1 unit =
    10 cm) and shaders. A WWE '12 loader needs a converter the other way
    (the reverse of `w13_char.py`), or must refuse superstar mods whose
    `made_with` is not its own.
  - The EMD naming (`"%06d%02d"`) and the PACH child ids
    ([4.2](#42-superstar-models-budgets-and-chpac)) need checking against
    WWE '12's.
- **Ids.**
  - `base=`, `pose=`, `music=`, `call=`, `entrance=` and `abilities=` are
    2011 ids: the roster 100-321, 2011's music, pose and ability numbering.
    `tools/ratings_port.py` and `abilities_port.py` hold the WWE '12 to 2011
    ability map; reverse it.
  - `moves=` offsets are into 2011's 1056-byte profile; WWE '12's is
    1072 bytes with another slot order.
  - Move ids in `pack.txt` are 2011's.
- **Motions.** `pack.txt` motions are 2011 YMKs keyed to 2011's skeleton.
  WWE '12 needs YANM; the existing path converts YANM to YMKs (`yanm_decode`,
  `w13_retarget`), not the other way.
- **Arenas.**
  - `arena.pac` is a 2011 `bgNN.pac`, and `base=arena_XX` names a 2011 tile.
    A WWE '12 arena loader would need its own tile names, its arena numbers,
    and its own size rule (2011's fixed arena memory).
  - The `ring.*` keys are in game units relative to 2011's ring; they apply
    if WWE '12's ring has the same dimensions.
  - Backstage `area=` 0-6 are 2011's `bg78` rooms.
- **Record offsets** (+221, +257 and so on), the slot pool ids, the
  allocator-128 budget, and everything named `sub_8...` are 2011
  executable facts.

### What a WWE '12 loader needs

1. The `type=` routing and folder layout ([3](#3-the-mod-types)), skipping
   types it cannot play.
2. Its own `made_with=` / `needs=` check, refusing a 2011 `ch.pac` or
   `arena.pac` unless it converts them.
3. A slot pool of its own blank roster ids, with a `slots.txt` keyed by
   folder.
4. Its own id maps for `base=`, `music=`, `pose=` and the abilities.
5. For shared online play: the same mod-set line and CRC rule, so that two
   games compare like for like.

The safest first step is to share the container, the manifest and the media
types, and to treat superstar, arena and move mods as per-game, marked by
`made_with` (or a future `game=` key) until converters exist both ways.

---

## 8. Tools

`tools/` (Python 3; run with `-I` on files from others):

| tool | what it does |
|---|---|
| `svrfmt.py` | EPAC, PACH, BPE read / write |
| `ch_tool.py` | EPK8 / EMD character pacs: unpack, list, repack |
| `jboy.py` | JBOY read / write / validate |
| `model_budget.py` | vertex pages per attire, `needs()` |
| `superstar_port.py` | a superstar mod from another game's data |
| `w13_char.py` | WWE '12-2K17 models -> 2011 |
| `svr07_char.py`, `svr08_char.py`, `svr10_char.py`, `svr10_superstar.py` | SvR 2007-2010 models and profiles |
| `ps2_port.py`, `ps2_char.py`, `ps2_moves.py` | PS2 discs |
| `svr11_build.py` | FBX -> 2011 model |
| `arena_port.py` | other games' arenas |
| `movepack.py`, `moves_port.py`, `yanm_decode.py`, `w13_retarget.py` | move packs, YANM decoding, retargeting |
| `hctp_entrance.py`, `jbi_entrance.py` | entrances as `pacentry` lines |
| `threads_parts.py` | `threads.txt` for a mod |
| `ratings_port.py`, `abilities_port.py` | per-game ratings and ability maps |
| `lone_pac.py` | which game a single character pac is from |
| `wwe13_pack.py` | packs a mod folder into a `.svrmod` (stored) |
| `make_matchtype_mods.py` | the match type switch bundles |
| `hair_sim.py` | the game's spring hair solver, offline |

`modmaker/svrfmt/` (C++): `pac.h` (EPAC / EPK8 / PACH / BPE), `archive.h`
(the multi-game pac editor), `jboy.h`, `texture.h` (DDS / DXT), `arena.h`
(arena load / save / `FitFile`), `ring_kit.h`, `char_fbx.h` (FBX import /
export), `zip_write.h`.
