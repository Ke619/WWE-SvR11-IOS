# Other games: what can come to SvR 2011

One section per kind of asset. Each porting chat keeps its own section up to
date; the converters follow docs/MOD_MAKER_3_PLAN.md section 4
(`tools/<kind>_port.py games / list / convert`).

## Arenas

Converter: `port/tools/arena_port.py` (Arena Porting). It reads the disc
through `tools/discfs.py` (an ISO or an extracted folder). It writes a
normal arena mod: `<out>/<id>/` and `<out>/<id>.svrmod` with `manifest.txt`,
`arena.pac`, `banner.dds`, and where the game has the art, `load.dds` and
`vs/<name>.dds`. The manifest's `base=` decides the tile (on the custom arena
pages) and the VS theme. The game gives the arena file the stage name of the
slot it is played in (`arena_mods.cpp` RedirectArena), so any base slot works.

| Game | Arena files | State | How |
|---|---|---|---|
| **SvR 2007 (360)** | `pac/bg/bg<k>.pac`, many arenas each | **ready** | as 2008, plus the host's arena motions |
| **SvR 2008 (360)** | `pac/bg/bgNN.pac`, EPAC | **ready** | texture sets moved to 2011's ids, crowd from a 2011 arena (`--pac11`) |
| **SvR 2009 (360)** | `pac/bg/bgNN.pac`, EPAC | **ready** | as 2008, plus light numbers remapped |
| **Legends of WrestleMania (360)** | `pac/bg/bg201-216, 219.pac`, EPAC | **ready** | SvR 2009's format (the 2008 path); see below |
| **SvR 2010 (360)** | `pac/bg/bgNN.pac`, EPAC | **ready** | the same format as 2011: the file is used as it is, except its motion bank (2011 RAW's) and match light sets too dark for 2011 (WrestleMania 25: set 10 ground 0.20 - 2011 RAW's, via `svrmod lights`) |
| **WWE '12 (360)** | `pac/bg/bgNN.pac`, plain EPAC | **ready (in the Mod Maker)** | the WWE '13 converter (`svrmod wwe13`) takes it as is |
| **WWE '13 (360)** | `pac/bg/bgNN.pac`, `0x0FF512ED` LZX container around an EPK8 | **ready (in the Mod Maker)** | `svrfmt/wwe13.cpp` (`svrmod wwe13`); see below |
| **WWE 2K14 (360)** | `pac/bg/bgNN.pac`, EPAC with ZLIB entries | **ready (in the Mod Maker)** | the WWE '13 converter: ZLIB unpacked, the host's arena motions |
| **WWE 2K15 - 2K17 (360)** | `pac/bg/bgNN.pac`: 2K15 as 2K14, 2K16 in the LZX container, 2K17 an HSPC of SHDCs | **ready (in the Mod Maker)** | the WWE '13 converter. Their screens (`layout_<show>_movie` / `layout_arena_movie` / `layout_mini_tron`) become 2011's `titantron_movie` / `arena_movie`, and move to the ids 2011 plays videos on (entrance 803 / 806 / 808, loop 801 / 807; the rest merged into the biggest of their kind; announce-table monitors kept): otherwise they played the live camera, in entrances too. 2K17's select pictures: MatchHD.pac `Ar<NN>`. WWE 2K14's screens are main&banner (titantron), <show>_header&wall and mini_tron (loops); WWE '12's minitron_movie and any other ...movie name are loops. A picture an arena names but doesn't carry (2K17 WCW Nitro's light cards pas_light / pas_light02) gets a clear one instead of drawing white. Long names get "(2K17)" or no game (the select shows ~30 letters). Tested in a match (27 Oct): 2K17 SummerSlam, RAW, WrestleMania 32 (85,915 points: 2.1.6+), WCW Nitro, NXT TakeOver: Brooklyn; 2K16 SmackDown, Royal Rumble '15, WrestleMania 15; 2K15 NXT, Hell in a Cell; 2K14 Raw, WrestleMania 29, WrestleMania 3. |

### SvR 2010

- **Format:** the same EPAC / PACH / JBOY / texture sets as 2011. The entries
  are 2011's, except that there is no announce table set (0x192, so the table
  shows the game's default pictures) and a smaller crowd pack.
- **Tested:** 2010's RAW (bg01) plays unchanged in 2011 (crowd, ring, props),
  and so does the converted No Mercy (bg06) on the Hell in a Cell slot.
- **Arenas:**

  | bg | Show | Notes |
  |---|---|---|
  | 00 | SmackDown | |
  | 01 | RAW | |
  | 02 | ECW | |
  | 04 | SummerSlam | |
  | 05 | Unforgiven | |
  | 06 | No Mercy | |
  | 07 | Survivor Series | |
  | 08 | Armageddon | |
  | 09 | Royal Rumble | |
  | 10 | No Way Out | |
  | 11 | WrestleMania 25 | |
  | 12 | Backlash | |
  | 13 | Judgment Day | |
  | 14 | Night of Champions | |
  | 15 | The Great American Bash | |
  | 18 | Cyber Sunday | |
  | 24 | Extreme Rules | no banner or VS art on the disc |
  | 16, 20, 22, 25 | | story copies (RAW, No Way Out, Royal Rumble, Road to WrestleMania), left out |
  | 23 | | practice ring, left out |
  | 30 | | not in the arena list |

- **Pictures:**
  - The banner comes from 2010's arena select (MatchHD `MENU/MASI`, the
    `arena_*` textures). 2010's PPV banners are square, so they are shown
    whole over the arena's darkened VS picture.
  - The loading picture is the biggest picture of 2010's VS theme for that
    arena (`MENU/M<nn>I`).
  - That picture is also used for the VS screen when the base slot's 2011
    theme has a single 1024 x 512 background: SmackDown `bg_sd`, RAW
    `RAW01`, Tribute to the Troops `troops_tex05`, SummerSlam `BG`,
    WrestleMania `wm_bg01`. Otherwise the base theme stays.
- **Base slot:** the 2011 arena of the same show (SmackDown 0, RAW 1, ECW 2,
  SummerSlam 4, Survivor Series 7, Royal Rumble 9, Judgment Day 13, Night of
  Champions 14, Bash 15, Extreme Rules 16). Otherwise its own number.
  `--base` changes it.

### SvR 2007

- **Format:** `pac/bg/bg0.pac`-`bg7.pac` each hold several stages: groups
  `STG<k>` with entries `00NN`. The arenas are 0000-0019, in `bg0`-`bg1`
  (0020+ are copies); `bg5`-`bg7` are backstage.
  - The texture sets are 0xC8 / 0xC9, as in 2008.
  - The crowd is loose models and seat files (0x4E20-0x52FF).
  - The group count's high 16 bits hold a per-file constant
    (`svrfmt.epac_read` masks it).
- **Conversion:** the 2008 path. 2007's crowd pieces and 0x9C40-0x9CFF are
  left out. Its arena motions (0x7530) crashed the 2011 load, found by
  swapping entries a group at a time, so they come from the 2011 host.
- **Tested:** RAW (0001) plays in 2011.
- **Arenas:** 00 SmackDown, 01 RAW, 02 ECW (?), 04 SummerSlam,
  05 Unforgiven, 06 No Mercy, 07 Survivor Series, 08 Armageddon,
  09 Royal Rumble, 10 No Way Out, 11 WrestleMania 22, 12 Backlash,
  13 Judgment Day, 14 Vengeance, 15 Bash, 16 New Year's Revolution,
  17 One Night Stand, 18 Cyber Sunday (?), 19 Saturday Night's Main Event
  (from the texture names).
- **Pictures:** no VS picture was found for RAW yet, so the banner is plain.

### SvR 2008

- **Format:** the same EPAC / JBOY models, but the arena's texture sets are
  entries 0xC8 (main) and 0xC9 (cube maps). In 2011 those ids are models, so
  the unchanged file crashed the arena load (`sub_826D73D0`).
- **What 2008 has no copy of:** a crowd (0x4E20-0x4E23, 0x4E2A), announce
  table pictures (0x192), a card (0xEE48), 0xEA6C, a visibility tree
  (0xC364) and 0x7922 / 0x80E8.
- **Conversion:**
  1. 0xC8 / 0xC9 move to 0x190 / 0x191.
  2. The crowd, 0x192, 0xEE48 and 0xEA6C come from the 2011 arena of the
     base slot (`--pac11 <SvR 2011 game folder>`).
  3. The biggest textures lose their top mip until the file is no bigger
     than that 2011 arena's.

  2008's own 0x7530 (another format than 2011's YMKs) is kept. With it, the
  arena loads and plays.
- **Tested:** RAW (bg01) plays in 2011: red ropes, 2008's white
  turnbuckles, the 2011 crowd, 4 textures halved, no heap change.
- **Arenas:**

  | bg | Show | | bg | Show |
  |---|---|---|---|---|
  | 00 | SmackDown | | 10 | No Way Out |
  | 01 | RAW | | 11 | WrestleMania 23 |
  | 02 | ECW | | 12 | Backlash |
  | 03 | Sunday Night Heat | | 13 | Judgment Day |
  | 04 | SummerSlam | | 14 | Vengeance |
  | 05 | Unforgiven | | 15 | The Great American Bash |
  | 06 | No Mercy | | 16 | New Year's Revolution |
  | 07 | Survivor Series | | 17 | One Night Stand |
  | 08 | Armageddon | | 18 | December to Dismember |
  | 09 | Royal Rumble | | 19 | Cyber Sunday |

  bg20-27 are story copies, and bg56-73 are backstage areas (`tools/svr08_stage.py`).
- **Pictures:** 2008 has no arena select banners. The banner, loading and VS
  pictures are cut from the arena's VS theme (MatchHD `M<nn>I`).
- **Crowd seats:** the 2011 base arena's crowd sits in that hall's seat
  layout, so it fits 2008's stands only roughly. RAW looks right; other halls
  are not checked yet.

### SvR 2009

- **Format:** as 2008 (texture sets at 0xC8 / 0xC9), plus a crowd (0x4E20 /
  0x4E21) and 0x7922 in older formats. Those are replaced by the 2011 base
  arena's crowd, and 0x7922 is left out.
- **Lights:** 2009's model flags (0xC351) use light `l(502)`, which 2011
  doesn't have; it crashed the arena load. Every light number the 2011 base
  arena has no copy of goes to its nearest one of the same hundred
  (502 -> 501, 106 -> 105). `arena_port.py` does this for 2008 too.
- **Tested:** RAW (bg01) plays in 2011.
- **Arenas:** 00 SmackDown, 01 RAW, 02 ECW, 03 Tribute to the Troops,
  04 SummerSlam, 05 Unforgiven, 06 No Mercy, 07 Survivor Series,
  08 Armageddon, 09 Royal Rumble, 10 No Way Out, 11 WrestleMania XXIV,
  12 Backlash, 13 Judgment Day, 14 Night of Champions, 15 Bash,
  16 One Night Stand, 17 Cyber Sunday, 18 Vengeance (from the texture
  names; check in game).

### Legends of WrestleMania

- **Format:** SvR 2009's arenas (`arena_port.py` game `low`, the 2008 path:
  texture sets moved, crowd / table pictures / card from 2011 RAW, 2011's
  light set 10, light numbers remapped).
- **Arenas:** bg201-215 WrestleMania I - XV (its `string.pac` SDB 0100, entry
  674 + n - 200), bg216 Royal Rumble 2008 (MatchHD `MD8I` holds logoRR08),
  bg219 its Rumble-match copy (the same textures). The WrestleManias play as
  2011's WrestleMania slot, the Royal Rumbles as Royal Rumble. bg223 / 224 /
  225 / 250 are its Create Moves, prop check and approval stages: not listed
  (they still convert as one arena file).
- **Pictures:** MatchHD `M<arena in hex>I` (`MC9I` = bg201; 2008 / 2009 name
  them in decimal): the loading and VS pictures.
- **Tested:** WrestleMania III (bg203) plays in 2011 (24 Oct 2026).

### WWE '12

- **Format:** the arena files are plain EPAC (no LZX container), in WWE
  '13's model format (cm, 0xB8 mesh descriptors) with 2011's entries.
- **Conversion:** the WWE '13 converter (`svrmod wwe13` / the Mod Maker)
  takes them as they are. It now also adds the 2011 host's `aud_lightmap`
  when the arena has none; without it the crowd is black.
- **Tested:** RAW (bg01) plays in 2011, crowd lit.

### WWE 2K14

- **Format:** EPAC whose entries (and PACH children) are `ZLIB`-packed:
  `"ZLIB"`, u32 0x1230, u32 packed size, u32 unpacked size, then a zlib
  stream; equal sizes mean stored. A few small texts (0xEB29) are bare zlib
  streams. Unpacked, it is WWE '12 / '13's layout and model format.
- **Conversion:** the WWE '13 converter (`svrfmt::Unpack` reads ZLIB). Two
  2K14 entries crash 2011's arena load and come from the 2011 host:
  - the arena motions, 0x7530 YMKs (found by swapping entries one group at
    a time);
  - the bare-zlib texts.

  `tools/wwe2k14_unzlib.py` writes an unpacked copy for inspection.
- **Tested:** RAW (bg01) converted straight from the disc file plays in 2011
  (355 models; 19 entries from the host).
- **Size:** 2K14 arenas are big. RAW needed many textures halved to fit.

### WWE '13

Done in the Mod Maker (`svrfmt/wwe13.cpp`). Notes:

- The memory notes in the arenas work, and the Mod Maker manual, have the
  details.
- A single model over about 430 KB unpacked crashes the arena load. Use the
  `drop-mesh=` option.
- The TitanTron videos are Bink 2, which can't be converted.
- Arenas without a 0x192 table set get a made one.

## Superstars (models, attires, renders, movesets, moves, themes)

Superstar Porting chat. Converters: `tools/superstar_port.py`, `moves_port.py`,
`attire_port.py` (MOD_MAKER_3_PLAN.md section 4). They produce a normal
superstar mod (docs/SUPERSTAR_MODS.md) and a move pack (docs/MOVE_PACKS.md).
Entrances have their own file: [OTHER_GAMES_ENTRANCES.md](OTHER_GAMES_ENTRANCES.md)
(HCTP Entrance chat).

**Vehicle entrances (2.1.5).** The bike is 2011's own prop 2540 (`evtobj.pac`,
entrances 518/519): parent 9999, one world track, one 0x75 key a frame (root =
cm x 10, node 0 = body heading, 1/4 steering about y, 2/3 wheels about x), lights
on at the first move, off after the last. `wwe13_entrance.py` builds it from a
2K14 bike (prop 2540, channels `#0c5879e5` root, `#fdd95b24/25` wheels,
`#fdd95b27` steering). `hctp_entrance.py` maps HCTP prop 2035 to 2540 (HCTP nodes
1/4 wheels, 2 steering; world heading = 32768 - y). HCTP segments where the bike
is the rider's child are baked onto a world track from the rider (offset
(10, -790, -32) root units, heading +150). Heights: in a vehicle entrance the
rider's and the bike's slot origins (and HCTP's ramp pitch, slot param 4) are
baked into their roots, their 2011 slots stay at 0, and a baked bike near the
floor is put on it (root y 1217). HCTP's flat-stage versions (32xx / 35xx: the
stage segments' slot 12 down) get no ground fix. A 2K14 bike off the floor (409:
a raised stage) moves onto it frame by frame with its rider; the cameras stay.
Tested: 2K14 210 / 409, HCTP 3003 / 3411 (3203 / 3511 checked offline).

**The source game's own bike.** The pack adds it as a new `evtobj.pac` SOBJ
entry (`pacentry evt\evtobj.pac SOBJ <id> props/<id>.bin`, `entrance_props.py`):
the game copies evtobj.pac into Mods\PacOverlay with it and never writes its own.
`propfallback <id> <game prop>`: if that copy can't be made, the packs' entrance
scripts use 2011's prop and the other packs still play (test aid
SVR2011_TEST_NO_PROP_COPY). 2K14 2540 = 2011's mesh with 2K14's textures (the
black bike) -> 2541. HCTP 2035 -> 2542 (`hctp_bike.py`: its YOBJ, model-space
vertices on their nodes, its own 13 nodes). Prop entries are plain PACHs (a
BPE-packed one crashed `sub_826B6BA8`); palettes count nodes from 1; a prop
track's node k drives the node named `root` / `bone0k`. HCTP segments with the
bike on the rider: it heads along his path, wheels level, and he sits where he
sits in its world segment (3003: (17, -804, 680) root units in the bike's frame).
Fallback limits: 2540 under an HCTP track keeps the position, not the wheels.

States: **decoded** = we read it and a tool uses it or could; **partly** =
layout known, some fields not; **unknown** = not looked at or not found.

### A game's DLC superstars (`--dlc`)

`superstar_port.py ... --dlc <folder>` (the Mod Maker: My game discs, a game's **Add DLC folder**) adds the source
game's DLC superstars to `list` and `convert`, beside the disc, which is still needed. The folder holds packages as the
console keeps them (STFS, up to 3 levels down) or unpacked; another game's packages in it are skipped by title id.
`tools/dlc_src.py` reads them, `Src` falls back to them for files the disc doesn't have, and `merge_dlc` takes each
package's own superstars: those with a model pac the disc doesn't have (`pac/ch/ch<id>[_dlc<nn>].pac`). A list row has
`dlc: true` and its name ends " (DLC)".

| Game | Tables (`chEtc`, a whole table in the disc's format) | Renders | DLC superstars seen |
|---|---|---|---|
| SvR 2009 | `pac/ch/chEtc.pac` CHAR | `pac/DLC_HD.pac` | 10 (ids 54-68) |
| WWE '12 | `pac/ch/chEtc.pac` CHAR (its CHCM group also has a DAT: not the table) | `pac/DLC_HD_dlc<nn>.pac` | 11 (61-82) |
| WWE '13 | `chEtc_dlc<nn>.pac` CHAR | `DLC_HD_dlc<nn>.pac` | 14 (52, 65-85) |
| WWE 2K14 | `chEtc_dlc<nn>.pac` CHAR; names in `string_dlc<nn>.pac` | `DLC_HD_dlc<nn>.pac` | 20 (377-396) |
| WWE 2K15 | `chEtc_dlc<nn>.pac` CHAR; names as 2K14 | `dlc_hd_dlc<nn>.pac` | 19 |
| WWE 2K16 | `chEtc_dlc<nn>.pac` CH20 / CH40, id at +4; names as 2K14 | `dlc_hd_dlc<nn>.pac` | 32; model pacs are EPK8 (2K15's form), not the disc's EPAC |

The theme comes from the package's own bank (`sound/DLC_<nn>.pck` and kin), else the disc's.

Entrances and titantrons come from the packages too: `entrance_port.py` reads through `dlc_src.view` (the disc with the
packages of `SVR2011_PORT_DLC` beside it; `--dlc` sets it for the run). `reads` gives every copy of a file, since two
packages can hold the same name.

| Game | Entrance file in a package | Titantron in a package |
|---|---|---|
| SvR 2009 | `pac/evt/NyujyoD.pac` (`entrance_port` lists it; `superstar_port` takes 2011's by name, as for its disc superstars) | `NNN.bik`: Bink 1, as it is |
| WWE '12 | `pac/evt/NyujyoC.pac`, same name in Pack 3 and Pack 4, each with its own entrances | `.bik`: as it is |
| WWE '13 | `Entrance_dlc<nn>.pac` | `NNN.bik2`: early Bink 2 (KB2d), RAD Video Tools can't open it, so no titantron |
| WWE 2K14 | `Entrance_dlc<nn>.pac` | `NNN.bik2`: as '13 |
| WWE 2K15 | `entrance_dlc<nn>[b].pac` | `ent_NNNN_V_0.bk2`: Bink 2, needs the player's decoder |
| WWE 2K16 | `<5 digits>_dlc<nn>.pac`, EPK8 where the disc has EPAC | `ent_NNNN_V_0.bk2`: as 2K15 |

Props the DLC entrance uses that 2011's `evtobj.pac` lacks don't show, and the conversion warns about them, as for a
disc entrance; the packages' own `evtobj_dlc<nn>.pac` aren't read. Not read from a package yet: moves whose motions only
the DLC has (their slots keep the base's move), and the Moves & entrances tab doesn't get the DLC folder. SvR 2007 / 2008 /
Legends use the 2009 reader but no package has been tried. WWE All Stars isn't a source game; its packages are skipped.
A disc model with the same id wins: DLC attires of disc superstars aren't taken.

### What SvR 2011 has (the target)

| Asset | Where | Notes |
|---|---|---|
| Model + attires | `pac/ch/chNNN.pac`, EPK8 group `EMD `, entry `%06d%02d` = attire*10 + kind (2 = wrestler) | PACH: JBOY body (+ hair 0x2710, eyelash 0x4e20, blood 0x7530-2), texture sets 0xa (normal/noise/Mk) and 0xc (color), face animation 0x64, portrait 0xc8/0xc9 (256 x 256), 0x28/0x32/0x50 |
| Skeleton | in the JBOY | Yuke's bone names (momo, sune, kote, te, yubi..), 10 cm units, pelvis origin, 102 nodes. No collision bones (2007-2009 have 48 `k*` bones; see Collision) |
| Profile, ratings, names | `pac/ch/chEtc.pac` CHAR/DAT (264 B per id), CHAR/PRO (1056 B: moveset 0x000-0x1BE, entrance block +0x1c4) | Moveset slot labels: `tools/pro_slots.json` |
| Moves | `m.pac` YMKs/YMBs banks, `mpsp.pac` MOTP/GAME, `misc.pac` MOVS/WAZE (160 B, id +0x90) and WAZA/DATA (EXH 36 B) | docs/MOVE_PACKS.md |
| Renders | `pac/DLC_HD.pac` groups SSFA, SSFB, SSFC, entry = char id | the 4 pictures are below |
| Theme | `sound/Music.pck` (Wwise), event `Play_MUS_%04d_0_0` | mods carry `theme.mp3` |
| Titantron | `movies/titantron/%03d.bik` | Bink 1, 320 x 320 |

**The 4 superstar pictures:**

| Picture | Size | Used for |
|---|---|---|
| SSFA | 512 x 512 DXT5 | the select render; a character with an SSFA is one of the 91 playable ones |
| SSFB | 512 x 512 DXT5 | the full-body render on the select screen in some modes (mod `render.dds`) |
| SSFC | 256 x 256 DXT5 | the bust: match setup, WWE Universe, online lobby and other screens (mod `render_small.dds`) |
| portrait 0xc8/0xc9 | 256 x 256, inside the model pac | the model's own portrait; converters use the SSFC bust |

### Per game

| | SvR 2007 | SvR 2008 | SvR 2009 | SvR 2010 | WWE '12 | WWE '13 | WWE 2K14 |
|---|---|---|---|---|---|---|---|
| **Archives** | EPAC (count word: low 16 bits only) | EPAC | EPAC | EPAC, ch/evt EPK8 | EPAC, ch/evt EPK8, some ZLIB leaves | xcompress `0FF512ED` around EPK8 | xcompress like '13; ch pacs say EPAC but have 16-byte entries (bit 0 of the u16 at group+6 picks 8-char names, not the magic) |
| **Model** | like 2008: `ch/chNN.pac` EMD `%02d%d%d`, JBOY mesh 0xB4 - **decoded** | 2011 skeleton, older shaders - **decoded** (`svr08_char.py`) | like 2008: EMD hex id `6602` = 0x66 attire 0 kind 2 - **decoded** (`svr08_char.py` ran unchanged, not tried in game) | same as 2011 - **decoded** (`svr10_char.py`) | like '13 (Biped rig in cm, mesh 0xB8, `yCh_*` shaders) in 2011's container - **decoded** (`w13_char.py` reader) | Biped in cm, floor origin - **decoded** (`w13_char.py`) | like '13 - **decoded** (`w13_char.py` reader unchanged); several attires per pac, each with a second (kind 4, guess: low-detail) model |
| **Textures** | one diffuse + normal per body part (46 for Austin), no AO - **decoded**, needs baking into sheets | one set: color, normal, AO - **decoded** | like 2008 - **decoded** | same as 2011 - **decoded** | 2011's sets, color DXT1, no blood / cube map - **decoded** | like '12 - **decoded** | like '13; shared textures in `chCommon.pac` |
| **Skeleton** | 2008's minus `kubi_x` (139) | 2011's + 48 `k*` (140) | 2008's + `r/l_kata_m` (142) | 2011's | '13 Biped (107; '13 has 109) | Biped | '13 Biped (109) |
| **Motions** | 2008 banks (YMKs without the 16-byte header), 5 fewer event ops - **decoded** | YMKs without header, ops 192/193 - **decoded** | 2010 groups, 2008 banks, event table differs from 2008/2010/2011 (ops 4, 151, 208) - **partly** | YMKs, op 87 bit 7 - **decoded** | '13 ZMOT / YANM curves - **decoded** (`yanm_decode.py`), retargeted to 2011's body bones (`w13_retarget.py`) | ZMOT / YANM - **decoded**, retargeted | ZMOT / YANM like '13, groups renamed (C000, M001..), mot05/06 empty - **decoded**, retargeted |
| **Move records** | WAZE 144 B (id +0x88); EXH 24 B - **partly** | WAZE 144 B; EXH 32 B - **decoded** | WAZE 144 B, no EXH - **decoded** | WAZE 160 B; EXH 32 B - **decoded** | WAZE 160 B with '13's move ids; no EXH - **decoded** | '13 ids - **decoded** | WAZE 160 B in `createmode.pac` (2848, 390 new) - **decoded** |
| **Moveset (PRO)** | 412 B, other slot order - **partly** | 450 B - **decoded** (Sabu, Sandman) | 450 + 10 B at +0xEA, entrance block +0xE4 - **partly** | 932 B - **decoded** (`svr10_superstar.py` MAP) | 1072 B, '13 move ids, entrance block +0x1d4 - **partly** | 1180 B - **decoded** (Mankind) | 1276 B (about 48 slots more than '13) - **partly** |
| **Attires** | one EMD per attire; kind 4 = entrance gear | like 2007 | like 2008, kind 4 has its own costume bones | same as 2011 | same naming as 2011 | 3 per superstar (Mankind) | many (Cena 10); no face animation child |
| **Renders** | `menu/menuHD.pac` MENU SF2A 256 bust, SF2B 512 body, SF2D 256 waist-up - **decoded** | `menu/menuHD.pac` MENU SSFA/SSFC (nested PACH by id) - **decoded** | `DLC_HD.pac` MENU SSFA/B (256 x 512) / SSFC (nested PACH) - **decoded** | `DLC_HD.pac` SSFA/B/C - **decoded** | `menu/menuHD.pac` SSFA/B/C, 2011 sizes - **decoded** | `menuHD.pac` SSFA/B/C - **decoded** | `menu/menuHD.pac` SSFA/B/C, byte-for-byte 2011's format - **decoded** |
| **Theme** | XACT: `music/DVD_Music.xwb` + cue names in `sound.pac` XACT/MUSI (`%04d_%d`) - **decoded** (`tools/xact.py`) | `sound/music.yaf` id*100, XMA1 - **decoded** | `music1/2.yaf` id*100, XMA1 - **decoded** | `music1.yaf` id*100, XMA2 - **decoded** | `Music.pck` `Play_MUS_%04d_0_0` (bank v56) - **decoded** | `Music.pck` `Play_MUS_%04d_a_b` - **decoded** | events `Play_MUS_%04d_0_0` (PRO music) in `Game_Sound_E_S.pck`, their streams in `Game_Sound_M.pck` - **decoded** (`wwise.theme` stream_pck) |
| **Titantron** | `%02d.bik`, Bink 1 | Bink 1 | Bink 1 | Bink 1 | Bink 1 (some 640 / 1280) | **Bink 2: can't be converted** | **Bink 2** |
| **Names** | CHAR/DAT (176 B) +0x1C | CHAR/DAT (232 B) +0x20 | CHAR/DAT (252 B) +38, like 2010 | CHAR/DAT | CHAR/DAT +0x2E | string.pac SDB (ZLIB) | string.pac SDB 60000 + id |
| **Ratings / abilities** | 2 x 8, small scale; no abilities - **partly** | 8 values; no ability list: 2 fighting styles (DAT +227 / +228 raw) - **mapped** by style | 8 values like 2010; abilities DAT +232 raw (2011's ids) - **mapped** | 8 values + abilities (+234 raw, 2011's ids) - **mapped** | 12 values at DAT +4..+15 (copy at +16) - **mapped** to 2011's 7 (+5, +8, +9, +12, +7, +14, +13 [+5]; fitted on 117 shared superstars); abilities DAT +260 raw (2011's ids; 14 = Comeback) - **mapped** | ratings not in DAT / PRO (not found); abilities DAT +274 raw - **mapped** | 14 values at DAT +36..+49 - only loosely like 2011's (re-rated), not mapped; abilities DAT +56 raw - **mapped** |
| **Ids** | the 2008 id space (78 shared) | 0-92 | 2008 id + 100 = 2010/2011 id | 2011 ids | 2011 ids (100-292 the same people), new 322-343 | own | own; CAS-built superstars in `chHeads.pac` (159 Piper, 372-375) - big job |

Ratings: `tools/ratings_port.py` (superstar_port calls it): WWE '12 +4..+15, '13 +4..+17, 2K14 / 2K15 +36..+49,
2K16 +40..+53, 2K17 +44..+57, SvR 2007 +0..+7 (raw DAT; '13 on: 14 values, 2K15 on re-rated) fitted onto 2011's
seven on the superstars each shares with 2011 (mean error ~4.6-6.1 vs ~8-9 for the roster mean).

Abilities: `tools/abilities_port.py` (superstar_port calls it): SvR 2009 .. 2K17 share 2011's ability
numbering (14 is Comeback from '12 on: mapped to Kip-Up as a guess; 22 Possum Pin -> Leverage Pin); 2K15 +56,
2K16 +60, 2K17 +64 (raw DAT). SvR 2008's fighting styles and SvR 2007's ratings give a guessed set; the report
says which were guessed.

PS2 movesets: `tools/ps2_moves.py` (ps2_port calls it; `--report <game> <iso>` lists every superstar). Move
names: SYM `PAC/MOVES.PAC` CATE (lists of 40-byte {id, name +8}), HCTP / SvR 2004 MOVES.PAC WAZA (64-byte
{name[32], data[32] with the id at +8}; 0x20 / 0xA0-byte header), SvR 2006 - 2009 `PAC/MISC.PAC` MOVS/WAZE
(SvR 2007's 144-byte records), 2010 its 160-byte ones. Profiles: SYM / HCTP / SvR 2004 in the CHAR record
(+0x08 / +0x10 / +0x30, one layout), SvR 2006 CHAR/PRO (432 B), PS2 2007-2010 CHAR/PRO = the X360 records
(2010: its first 564 B), the X360 maps and 2011's ids. A slot's move is 2011's of the same name (or the same id
and name), else one of the same name with another number that 2011 uses in that kind of slot ("similar"),
else the base's - no PS2 motions are ported. Mapped slots that get a 2011 move: JBI 34 % (EDIT.PAC MOVS entry 999 names; its records are SYM's layout + 4 and SYM's ids), SYM 32 %, HCTP 37 %, SvR 2004
45 %, 2006 45 %, PS2 2007 60 %, 2008 60 %, 2009 69 %, 2010 82 % (offline, every superstar of each disc).

### Legends of WrestleMania (360)

`superstar_port.py` game `low` (disc id 54510838; `GameLoW`, on the SvR 2009 reader). SvR 2009's engine, all pacs
EPAC: `pac/ch/ch100-254.pac` + `chEtc.pac`, `pac/evt/Nyujyo0-3.pac`, themes `sound/music1.yaf` / `music2.yaf`
(record music id x 100), titantrons `movies/titantron/<movie id>.bik` (Bink 1).
- **Records:** chEtc CHAR/DAT 262 B = 2009's with 12 rating bytes (twice) where 2009 keeps 8 (name +46 raw, full
  name +114, short +182; read as 2009's record); CHAR/PRO 504 B, entrance block +0xE4 as 2009 (music, movie,
  entrance 4000 + id - 100, champion entrance). 89 superstars with models (ids 100-254).
- **Ids:** its own (100 The Rock); 2011's by name - 33 of them (its Ted DiBiase, 136, is the father: not 2011's 261).
- **Model:** `svr08_char.py` unchanged (The Rock: 2 attires, 152 of 1024 pages; Ultimate Warrior 202 pages: needs
  2.1.6+). Renders: `DLC_HD.pac` groups SSFA / SSFB / SSFC at the top level (2009 nests them in MENU), entries
  named by the decimal id. Loading pictures: `menu/LoadHD.pac` LOAD `S<id>` (not used yet).
- **Entrances:** `entrance_port.py` game `low` (2009's scripts plus LoW's generic extras); the ring-cart
  "Gondola" entrances (4170-4186, 4230-4233, 4350-4359) ride LoW's own cart (pac/evt/_EvtObj.pac SOBJ 2552 /
  2565, added by the pack under the same ids; propfallback 2011's jeep 2526) on its own world track - tested 4170
  (24 Oct 2026). Their scripts start at segment 1: no Create-An-Entrance parts.
- **Ratings:** its DAT +4..+15 rate on its own scheme (five real fields: +6 +7 +8 +12 +13, the rest 0x01):
  ratings_port 'low', fitted on 35 shared superstars - rough (leave-one-out 7.4-10.2 vs 9-12.5 for the roster
  mean). **Abilities:** none in its records (2009's list at +232 is zeros here): guessed from the ratings.
- **Not ported yet:** the **moveset** (the base's; the `moveset` / `moveset-pack` verbs say so): its CHAR/PRO
  is its own layout with its own move ids (not 2009's - SvR 2009 movesets are done, moveset_maps.json svr2009).
- **Tested** (24 Oct 2026): The Rock, Andre the Giant, Ultimate Warrior converted; Ultimate Warrior in a match with
  his model, theme, titantron and name plate.

### Converter plan

| From | Model | Renders | Theme | Moveset | Moves |
|---|---|---|---|---|---|
| SvR 2007 | done: `svr07_char.py` (per-part textures baked into 2011's sheets; superstar_port svr2007) | easy: SF2B -> SSFA/SSFB, SF2A -> SSFC + portrait | done: `tools/xact.py theme` (the longest MUS_<id>_N cue, ffmpeg) | new 412 B slot map | 2008 path; EXH 24 -> 36 B; most motions also exist in 2008 |
| SvR 2008 | done (`svr08_char.py`) | done | done (yaf) | done by hand (Sabu, Sandman): make it a slot map | done by hand: becomes the shared old-game path |
| SvR 2009 | `svr08_char.py` + hex entry names (small) | pad 256 x 512 to 512 | yaf as 2008 (music id at PRO +0xE4) | the 2008 map shifted 10 B | 2008 path + an event-op rewriter (biggest job) |
| SvR 2010 | done | done | done | done | done (`svr10_moves.py`) |
| WWE '12 | `w13_char.py` on the plain EPK8 (small) | trivial | Wwise as '13 (bank v56) | new 1072 B map + the '13 move-id matching | poses through `yanm_decode` + `w13_retarget` (`Retarget(ik=False)`); events / EXH / WAZE from a similar stock 2011 move (to build: `moves_port.py`) |
| WWE '13 | done (`w13_char.py`) | done | done | done by hand (Mankind) | a few by hand; general path as WWE '12 (to build: `moves_port.py`) |
| WWE 2K14 | `w13_char.py` (small; pick the wrestler model of each attire) | trivial (copy) | Wwise as '13 once the superstar -> event table is found | new 1276 B map + '13 matching | as WWE '12 (to build) |

Moves that 2011 already has are mapped by name and id to the stock move; only
the rest need porting. WWE '12 / '13 / 2K14 moves: the HCTP Entrance chat's
`tools/yanm_decode.py` and `w13_retarget.py` (shared, not copied) give the
attacker's and the victim's body keys at 30 Hz, both in the source's world
frame; no finger bones (the 2011 template move's are kept). A move's events,
EXH, WAZE and the victim's follow-up motion come from the most similar stock
2011 move (same category and EXH group, closest length), timed to the new
length, the way the WWE '13 Mankind moves were made. The `list` command reports per superstar which moves
exist in 2011, which can be ported and which can't.

### Building character models (FBX import, all converters)

`tools/svr11_build.py` builds a ch.pac from a neutral model (the Mod Maker's FBX import); the rules it and
`w13_char.py` / `svr08_char.py` follow are in `tools/svr11_skeleton.json` ("mesh_rules"), the rig maps in
`tools/rig_maps.json`. Checked against stock pacs (13 Oct):

- Vertex memory: 192 pages per attire (`model_budget.py`). Stock bodies reach 11,252 vertices (ch301); the
  page count, not a vertex count, is the limit. Palettes <= 20 bones; positions / normals / UVs must be finite.
- Hair / lash models (0x2710 / 0x4e20) have their own node tables, usually reduced (hair 29-141 nodes,
  lashes mostly 34), with 1-4 influences (hair mostly 4). Ours carry the full body table with up to 4
  influences and play (Mankind, the ch104 round trip, the Cena FBX sample).
- Entries: stock pacs hold 1-4 (attires); the EPK8 index size and the group count field (entries x 4) must
  match, or mounting crashes.
- Face pivots: the eyeball vertices sit 0.05-0.19 in front of `l_eye` / `r_eye` on stock models. When a
  model's differ from the template's by more than 0.15, `svr11_build.py` moves the bones under `atama` by the
  difference (the mesh stays); otherwise blinking pushes the lids forward and the eyes seem to pop out.
- Colour alpha: on skin it's the specular level, not opacity (stock ch104 has ~617k texels below 4 under
  its skin and draws them solid); hair / lash cards cut out with it.
- Texture slots a template block leaves unbound stay unbound, and mips are resized with RGB and alpha
  apart (Pillow premultiplies: low colour alpha turned the smaller mips into red specks on the skin).

The crash, face-pivot and alpha findings were first documented by robert32's SvR Port Doctor; we
re-measured each on stock data (only the face pivots and the NaN check changed our code).

### Collision

- 2011 models have **no collision bones**. 2007, 2008 and 2009 bodies carry 48
  `k*` bones (toe, heel, calf, shin, thigh, pelvis, palm, chest, back, head,
  jaw, neck, crotch, hip..); the converters fold them into their 2011 parent.
- 2008 has a collision table `misc.pac` COLT/C360; 2007 and 2009 don't.
- The model pac's child 0x28 is a small text table of 7 x/y/z triples (2011,
  '12 and '13; '12/'13 in cm). Guess: body offsets for hit checks. Child 0x50
  differs between games; converters keep the 2011 template's.
- For a collision view, the 2011 game data to read is still to be found
  (guess: CHAR/DAT or the 0x28 table).

### Visibility trees and borrowed entries (all SvR 2007-2009 conversions)

- SvR2011 draws only the arena models its visibility tree (0xC364) lists.
  2007-2009 arena files have no tree, so their floor, stands and barricades
  were culled: the user saw a black floor and no barricades. The converter
  now builds a flat tree, one node per arena model, boxed from its vertices
  in 2011's axes (x, z, -y) x 0.1.
- The borrowed entries (crowd, table pictures, card, light set) always come
  from 2011's RAW. 2008 SummerSlam with SummerSlam's own crashed the load on
  its slot; with RAW's it plays there. The base slot still sets the tile, the
  VS theme and the size budget.

## Belts and weapons (D11 research, Arena Porting)

### Where 2011 keeps them

- **Title belts:** models in `pac/evt/evtobj.pac`, group SOBJ, one entry per
  object id. SOBJ/LIST names them (64-byte records: u16 id, name).
  - There are 12 titles: IC, U.S., WWE, Hardcore, Women's, WWE Tag Team,
    Cruiserweight, World Heavyweight, World Tag Team, ECW, Divas and Million
    Dollar.
  - Each comes in several forms: Stretched 2002-2010 / 2305 / 2308 / 2512,
    Folded 2012-2020 / 2306 / 2309 / 2513, Rolled 2022-2030 / 2307 / 2310 /
    2514, and the variants 3xxx / 4xxx / 5xxx.
  - The belt you pick up in a match is a weapon: `bgEtc.pac`
    STG/WPON/0x000E, 14 packs, each a folded belt JBOY (named by its SOBJ id)
    with two textures and a small XML. The textures are `k2002`-`k2009_01/02`,
    `cenab_01/02` (WWE spinner), `2050`, `2306`, `2309` and `mdb_01/02`.
  - The `belts` branch scales them (title_belt_scale, `title_belts.cpp`).
- **Weapons:** `pac/bg/bgEtc.pac`, group STG/WPON (and WPBS, the backstage set).
  - **0x9C40** is the weapon settings text, one line per model number: `{mdl,
    type, se_hit, se_drop, hp, weight 0-3, damage, up to 5 hit spheres (x, y,
    z, r)}`. The model is the WPON pack id x 10 (40 chair -> 0x4, 140 belt ->
    0xE). The type is the behaviour class, e.g. 3 chair, 6 ladder, 11 table,
    14 belt, 21 bat, 30 crutch. There are 58 models: barbed bat, gong,
    chair, tables, announce tables, shovel, ladder, belts, steps, signs,
    crutch, cane, umbrella, trash can, fire extinguisher, speaker, monitor,
    bottles, tube light, tire, TV, Triple H's sledgehammer, bat, cue, tire
    iron, camera, pillow.
  - **0xAFC8-0xAFCF / 0xC350-0xC354** are per-stage placement texts: model,
    position, rotation, start motion, place (ring / outside / crowd).
  - **0x9C41 / 0xA028 (WADD) / 0xAFD2** are binary tables, not decoded yet.
  - **Packs:** each WPON pack is a PACH holding a model JBOY (0x0), a texture
    set (0xA) and optional extras.
  - The Fixes chat's `SVR2011_TEST_ITEM_LOG` (`belts` branch) logs the stock
    under the ring and the CPU's weapon choices.

### Swap or add?

- **Swap: yes.** A belt or weapon is a model + texture set in a pac entry.
  - **Reskin (same model):** works today with **texture packs** (native
    renderer: `Texture Packs\<pack>\..._<hash16>.dds/png`, matched on the
    XXH3 of the top level). A belt has two textures per title.
  - **Model swap:** needs the game to serve changed copies of `evtobj.pac` /
    `bgEtc.pac`. The move-pack overlay (Mods/PacOverlay, `move_packs.cpp`)
    already does that for m.pac / misc.pac / mpsp.pac.
- **Add a title: no.** The 12 title slots are fixed in the game's code and
  saves; 2011 has no Create-a-Championship. Unused slots (Hardcore,
  Cruiserweight, Million Dollar) could be given a new look, but showing them
  as new titles in the menus is game research.
- **Add a weapon: maybe.** The settings are a text table parsed by the game,
  so a new model number with an existing type may work, but the stock under
  the ring and the spawn lists are not mapped yet. Research first.

### From other games

- **SvR 2010:** the same weapon table (the same 58 models and types) and
  `evtobj.pac` belts in the same JBOY format, so its belt designs (the older
  titles) can be model swaps once the game side above exists.
- **WWE '13:** belts and weapons are JBOY with WWE '13's 0xB8 mesh descriptor
  inside its LZX container, which `svrfmt/wwe13.cpp` already reads; units
  are cm (x 0.1).
- **SvR 2008 and the rest:** research.

### Proposed mod formats (to agree with ModMakerUIRework)

- **Phase 1, reskins (no game work):** the Mod Maker writes a texture pack,
  `Texture Packs\<name>\<owner>_<texture>_<hash16>.dds`, from the stock
  belt or weapon textures (`belt_port.py` / `weapon_port.py list` give the
  names, sizes and hash16 of each title's or weapon's textures).
- **Phase 2, model swaps (game side needed):**
  - `type=belt`, `title=<IC|US|WWE|HARDCORE|WOMENS|WWE_TAG|CRUISER|WHC|WORLD_TAG|ECW|DIVAS|MILLION>`,
    with `stretched.jboy`, `folded.jboy`, `rolled.jboy` and `textures.bin`
    (a texture set).
  - `type=weapon`, `model=<number>`, with `model.jboy`, `textures.bin` and
    optional stats keys (`hp=`, `weight=`, `damage=`, `spheres=`).
  - Both go through a pac overlay for `evtobj.pac` / `bgEtc.pac`.
