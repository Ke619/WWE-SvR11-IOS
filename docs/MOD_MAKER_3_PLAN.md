# Mod Maker 3 - plan (checklist section D)

Owner: the Mod Maker chat (ModMakerUIRework). Started 13 Oct 2026.
Source list: `CHECKLIST.md` section D (git-ignored, kept by Main).
Builds on Mod Maker 2 (`docs/MOD_MAKER_PLAN.md`, the 2.0.5 rework).

The aim: the Mod Maker is where everything from the user's other WWE discs
comes into SvR 2011. You pick a disc, see what is on it and what 2011
already has, and convert one thing at a time into a mod you can finish and
test. The converters live in `port/tools` and belong to the porting chats.
The Mod Maker reads the discs, runs the converters, shows their results and
edits the mods.

Game order (the user's): Xbox 360 SvR 2007, 2008, 2009, 2010, then WWE 12,
WWE '13, 2K14. Later: 2K15-2K17, Legends of WrestleMania, the PS2 games
(Just Bring It ... SvR 2011, Crush Hour).

## 1. What the discs look like (surveyed 13 Oct)

| Disc | Container | Partition | Files | Notes |
|---|---|---|---|---|
| X360 SvR 2007 | XDVDFS | 0xFD90000 (XGD2) | 290 | `pac` (185), `.bik`, `.xwb` music |
| X360 SvR 2008 / 2009 / 2010 | XDVDFS | 0xFD90000 | 369 / 372 / 374 | `pac` (254-261), `.bik`, `.yaf` sound |
| X360 Legends of WrestleMania | XDVDFS | 0xFD90000 | 409 | `pac` (236), `.bik`, `.yaf` |
| X360 WWE 12 | XDVDFS | 0xFD90000 | 564 | `pac` (268), `.bik`, `.pck` sound |
| X360 WWE '13 | XDVDFS | 0x2080000 (XGD3) | 716 | `pac` (356), `.bik2` |
| X360 2K14 / 2K15 | XDVDFS | 0x2080000 | 730 / 692 | `pac` (~370), `.bik2`/`.bk2`, `.xzp` |
| X360 2K16 / 2K17 | XDVDFS | 0x2080000 | 4488 / 5242 | `pac` (4083 / 4858: split per asset) |
| PS2 Just Bring It | ISO 9660 | - | 171 | `*.PAC` in the root |
| PS2 Shut Your Mouth | ISO 9660 | - | 299 | `PAC\` |
| PS2 Here Comes the Pain | ISO 9660 | - | 100 | `ED*`, `BGM`, dummy padding files |
| PS2 SvR, SvR 2006 | ISO 9660 | - | 205 | `BELT`, `BGM\*.AFS`, `ED*` |
| PS2 SvR 2007-2011 | ISO 9660 | - | 149-196 | `PAC\`, `PLISTPS2.ARC`, `.AFS`/`.YAF` sound |
| PS2 Crush Hour | ISO 9660 | - | 1065 | a different engine (`BUNDLES`, `SEASONS`) |

Every 360 disc keeps the same Yuke's layout (`pac\`, `movies\`, `sound\`,
`plist360.arc`). The pac files' first 4 bytes, counted per disc:

| X360 disc | EPAC | EPK8 | `0FF512ED` (xcompress, LZX) | other |
|---|---|---|---|---|
| SvR 2007 / 2008 / 2009 | 184 / 260 / 253 | - | - | 1 PACH |
| SvR 2010 | 152 | 105 | - | 1 PACH |
| Legends of WrestleMania | 235 | - | - | 1 PACH |
| WWE 12 | 161 | 106 | - | 1 PACH |
| WWE '13 / 2K14 / 2K15 | 17 / 23 / 19 | 10 / 9 / 13 | 328 / 334 / 337 | 1 PACH |
| 2K16 / 2K17 | - | - | 4082 / 4857 | 1 PACH / SHDC |

So up to WWE 12, the 2011 readers (EPAC, EPK8, PACH, BPE) open the pacs as
they are. From WWE '13 on, most pacs are xcompress-wrapped: the Mod Maker
already unwraps them (`xdec`, built in for WWE '13 arenas), and gets an EPAC
/ EPK8 inside. What the entries hold (model, motion and texture versions) is
D2's per-game table (Superstar Porting); the Mod Maker's viewers follow it.

## 2. Pages (what changes)

New:
- **Discs** (LOOK): the user's disc images (a folder list, default
  `D:\Roms\Xbox 360\WWE` and `D:\Roms\Playstation 2\WWE Games`, editable),
  each recognised by title id / file name with its game id from the
  converters' `games` list, and its status (ready / research).
- **Port** (MAKE): the wizard (D3). Disc, then a kind (superstar, arena,
  moves, entrance, attire, belt, weapon), then the item from the
  converter's `list`. Each item shows what 2011 already has. The converter
  runs and its result opens on the right page (Superstar, Arena, Moves ...)
  as a normal project.

Changed:
- **Game assets** (LOOK) becomes the EPAC tool (D16). Its sources are the
  game folder, any disc image or any folder. It browses, extracts, replaces,
  adds and removes entries, repacks EPAC / EPK8 / PACH with BPE (stored or
  packed), and saves as a copy; it never writes into `Game Files\pac`.
  Viewers stay lazy and cached (D17).
- **Superstar**: overall rating under the sliders (D13); hometown (D8);
  theme volume with a loudness meter (D7); the 4 renders with a picker and a
  template (D7); moveset by name from the game's lists, signatures and
  finishers included (D4); attires as a list with their own entrance and
  entrance outfit (D5 / D6); warnings for limits and crash risks (D12).
- **Animations / model view** (D9, D4): blood layer, finger motion, PS2 and
  360 motions decoded right, preview on the chosen model; a sturdier moveset
  viewer (every slot of a moveset played in order); collision boxes on/off,
  shown on two dummies (green = attacker, red = victim).
- **3D editor**: an entrance preview inside the arena (D5): the superstar
  walks the ramp in the arena being edited.
- **Belts / weapons** (D11): a page for the Arena Porting chat's belt and
  weapon mods once the format exists.
- **New attires for the roster** (D6): a mod that adds attires to a stock
  superstar (format from the game side, below).

## 3. Phases

Work goes in this order. Each phase ends with a commit and a hidden test
(`--behind --shot`).

| Phase | Items | Needs from others |
|---|---|---|
| P1 | D1 disc reading: `port/tools/discfs.py` (folder / XDVDFS / ISO 9660, for every converter) and the same in C++ for the Mod Maker; the Discs page; Game assets browsing a disc | - |
| P2 | D16 EPAC tool + D17 speed (startup time, cached viewers, a measured before / after) | - |
| P3 | D13 overall, D12 warnings, D4 moveset by name, D8 hometown (UI), D7 theme volume (UI) | game keys (section 5) |
| P4 | D3 port wizard on the converter contract; the "2011 already has" checklist | the converters' `games` / `list` / `convert` |
| P5 | D9 model / animation viewer, D4 sturdier moveset viewer and collision boxes | collision data (section 5) |
| P6 | D5 entrances: per-attire picker, entrance outfit, in-arena preview; D6 attires | HCTP Entrance + game keys |
| P7 | D7 renders (4 + template), select / VS animation; D11 belts / weapons | Arena Porting, game side |

## 4. The converter contract (all porting chats)

One script per kind, the same commands for all of them, called through
`tool_run.cpp` (Python 3 + numpy + Pillow; ffmpeg where needed):

```
py tools/<kind>_port.py games
    -> [{id, name, platform: "x360"|"ps2", disc_hint, status: "ready"|"research"}]
py tools/<kind>_port.py list --game <id> --src <ISO or extract folder> [--pac11 <2011 pac>]
    -> [{key, name, ... kind's fields ..., in_2011: <2011 id/number or null>}]
py tools/<kind>_port.py convert --game <id> --src <...> --item <key> --out <dir> [kind options]
    -> <dir>/report.json and the same JSON on stdout:
       {ok, out: <the .svrmod / pack folder / file made>, opens: "superstar"|"arena"|"moves"|...,
        warnings: [], dropped: [], notes: []}
```

- `<kind>`: `superstar` / `moves` / `attire` (Superstar Porting), `entrance`
  (HCTP Entrance Porting, agreed 13 Oct: its `--entrance`/`--number` options
  stay), `arena` / `belt` / `weapon` (Arena Porting), `media` (Extras).
- `--src` takes an ISO or a folder: converters read through
  `tools/discfs.py` and never need an extracted disc.
- stdout carries only JSON. Logs go to stderr, and progress lines read
  exactly `progress <0-100> <text>`.
- Exit codes: 0 ok, 2 bad input, 3 conversion failed; anything else is a
  crash (the Mod Maker shows the stderr tail).
- `in_2011` drives the wizard's "2011 already has this" column.
- Items may carry `status`: `ok`, `crashes` (converts but crashes 2011 at load -
  known from the full pass) or `special` (not an item of the kind, e.g. WWE '12+
  arena files bg60 and up: backstage rooms, studio / photo stages), with a
  `reason` for the last two. The wizard greys `crashes` and `special` items out.
  (`arena_port.py` sets it: `KNOWN_CRASHES`, `mark_items`.)
- Arenas of WWE '12 - 2K17 (`arena_port.py`): every `pac/bg/bgN.pac` of the disc (bg00 - bg59, the
  special stages 60+, WWE '12's bg200 / bg201), each item with `name` = the game's own arena name
  (from its string table, e.g. "RAW IS WAR '97", "NXT TAKEOVER: BROOKLYN"; the file key when the
  game has none), `bg` = the file key (`bg28`), and `thumb` = a PNG (320 x 160) of the arena's match
  intro / loading backdrop once the disc was scanned. `py tools/arena_port.py scan --game <id>
  --src <ISO or folder> [--cache <dir>]` reads the disc once (names and pictures, `progress` lines)
  and caches the list (default `%LOCALAPPDATA%/SvR2011/arena_lists/<game>_<key>/arenas.json` +
  `thumbs/bgNN.png`); `list` then answers from that cache (without a scan: names only, read once
  and cached too). WWE 2K17's menu pacs keep hashed entry names, but their texture bundles keep their
  own: MatchHD.pac's arena select pictures `Ar<NN>` (256 x 128) are its thumbnails (27 Oct).
- One arena file instead of a disc (`arena_port.py`, 15 Oct): `--src` may be a lone arena pac
  (a `bgNN.pac` as it is on the disc, any name; SvR 2007 - WWE 2K17, xcompressed or not).
  `py tools/arena_port.py detect --src <file.pac>` -> `{ok, file, game, game_name, sure, also: [games
  it may be instead], format, items: [the list items], convert: "arena_port"|"mod_maker", lacks:
  [plain words], notes, mod_maker: <the svrmod line, natives only>}`; exit 2 with `error` for a PS2
  pac (PS2 games keep every arena in one `BG.PAC`; no PS2 arenas yet) or a pac that isn't an arena.
  `list` / `convert` take the file with `--game` optional (the file's own game; a `--game` of another
  family is refused) and `--item` optional (the file's arena: its EPAC entry name, else the file name's
  bgNN; SvR 2007 files hold ten). `sure` false = the file can't tell (2008 / 2009, 2K14 / 2K15 / 2K16:
  the same convert). WWE '12 - 2K17: `convert` exits 3 as for a disc - the Mod Maker runs `svrmod
  wwe13 <the file> <host> <out>` itself. A SvR 2011 arena file is taken as it is (game `svr2011`).
  What a lone file can't give: the arena's name and pictures (the disc's menu files) - the name comes
  from the port's own list by number (else "Arena bgNN"), the banner is 2011's banner of the base arena
  (`--pac11`), no own loading / VS picture; WWE: bgNN, no thumbnail. Crowd, table pictures and the
  light sets 2007 - 2009 lack come from 2011 RAW as with the disc; no arena keeps textures in another pac.
- Output is a normal mod or pack: the Mod Maker opens it on its page, where
  the user finishes it and saves.

## 5. What the Mod Maker needs from the game side

| Need | For | Who | State (13 Oct) |
|---|---|---|---|
| `entrance=` per attire (`attire<N>_entrance=`), entrance outfit (entrance-only gear: jacket, mask, hat) | D5 | HCTP Entrance Porting | keys proposed; `entrance_port.py` on branch entrance-fixes (d4a11aa, 240693a) |
| `theme_gain=<dB>` (-20.0..+6.0, 1 decimal, missing = 0; media packs `theme_gain.<id>=`) | D7 | Extras | agreed; game side on branch media-d7d8 |
| `.mp4` entrance movies (`movie=` may name an .mp4, a same-named .bik fallback next to it for the Deck) | D7 | Extras | agreed; a C++ entry point in launcher/movie_maker.c (320 / 640 / 1024) |
| `hometown=<city clip>[,<state clip>]`, `announcer=<clip>`; clip list `tools/announcer_calls.json` | D8 | Extras | agreed; game side on media-d7d8, untested |
| CAWs using a stock name call: `<game>\Mods\caw_announcer.txt`, lines `<CAW full name>=<clip>` | D8 | Extras (game), Mod Maker (CAW pictures page) | agreed |
| a mod that adds attires to a stock superstar | D6 | Superstar Porting + game side | coming in docs/OTHER_GAMES.md |
| which files hold the 4 renders and how each is used | D7 | Superstar Porting | coming in docs/OTHER_GAMES.md |
| the select / VS screen pose motion per superstar (key + hook) | D7 | Superstar Porting (proposed to Main) | open |
| the characters' collision volumes | D4 | Superstar Porting | coming in docs/OTHER_GAMES.md |
| the overall rating formula | D13 | Mod Maker (offline, from the roster data) | done: sub_828B7950 = round(sum of the 7 x 10/63) + 1, at most 100 |
| belts / weapons: phase 1 texture packs (`belt_port.py` / `weapon_port.py`), phase 2 `type=belt` / `type=weapon` with a pac overlay | D11 | Arena Porting | phase 1 agreed; docs/OTHER_GAMES.md on branch arena-port |
| per-game format table (D2) | all | Superstar Porting | coming in docs/OTHER_GAMES.md |

## 6. The user's answers (13 Oct)

1. D7 select / VS screen animation: the pose motion. The user picks which
   in-game motion the superstar plays on the select and VS screens (needs a
   game-side hook).
2. D9 "right for every platform": both. PS2 and 360 motions must play right
   in the preview and after porting, and the Mod Maker should run on Linux,
   the Steam Deck and Android too. The second part is a later phase
   (section 8), assessed first.
3. D7 render template: makes the user's picture match. A frame, lighting and
   size template that turns their own picture into a render in the game's
   style.
4. D5 entrance outfits: entrance-only gear (jacket, mask, hat), worn for the
   entrance and taken off for the match. HCTP Entrance owns the game side.

## 7. Tests

- Mod Maker: always `--behind --shot <png>`, never a visible window, no
  input to the user's mouse / keyboard.
- Disc reading: every disc above lists, a file reads, and its bytes match
  `extract_iso.py` for 360 and a plain ISO 9660 walk for PS2.
- The EPAC tool: a repack with no change is byte-identical; a replaced entry
  reads back; the game loads a repacked copy (in a test game folder, with a
  test slot from Main).
- Converters through the wizard: the hidden Mod Maker runs `list` and
  `convert`, and the mod it makes passes the page's problems check.
- Game tests: only with a slot from Main; muted, `--monitor=2`, mnk_mode and
  p2p_port_mapping off.

## 8. The Mod Maker on Linux, the Steam Deck and Android (later phase)

What ties it to Windows today, and what each needs:

| Part | Today | For Linux / Deck | For Android |
|---|---|---|---|
| Window, input | Win32 | SDL3 (the game already uses SDL) | SDL3 (touch: bigger hit targets, no hover tooltips) |
| Drawing | Direct3D 11 (ImGui backend + the 3D editor / character preview shaders) | Vulkan or OpenGL 3.3 backend; the editor's and the preview's shaders as GLSL / SPIR-V | Vulkan / GLES 3 |
| UI | Dear ImGui | unchanged | unchanged, with a touch-sized style |
| File pickers | COM IFileDialog | SDL3 dialogs (portal on the Deck) | the Storage Access Framework (content URIs, not paths) |
| Video / audio decode (From a video, theme meter, .mp4) | Media Foundation | ffmpeg (libav*) or a bundled decoder | MediaCodec (as the game's Android build) |
| Converters | `py.exe` + Python tools (tool_run.cpp) | `python3` (present on the Deck's desktop mode) | no Python: converters run on a PC, or the needed ones get C++ ports |
| Paths, processes | `_wfopen`, CreateProcess, ShellExecute | POSIX equivalents (small wrapper) | app storage + SAF |

Running under Proton on the Deck may already work for most pages (D3D11 via
DXVK), but Media Foundation video decode does not, and Python tools would
need a Windows Python inside the prefix. The native route is: (1) move file
access, process start and pickers behind a small platform layer; (2) SDL3 +
a second ImGui / 3D backend; (3) replace Media Foundation with ffmpeg on
Linux. Android comes after that, with touch layout and SAF, and without the
Python converters.
