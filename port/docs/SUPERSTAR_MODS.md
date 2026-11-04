# Superstar mods (arenas branch)

New playable characters from `<game>/Mods/Superstars/<folder>/`, up to 72.

- **Game:** `src/superstar_mods.cpp`.
- **Select screen EXTRA list:** `src/managers.cpp`.
- **Making them:** the Mod Maker's Superstars page.
- **Installing them:** the launcher's Mods tab (PC and Android).

## A mod folder (a .svrmod is a zip of one)

| file | |
|---|---|
| `manifest.txt` | `type=superstar`, `id=`, `name=` (31 chars), `short=`, `base=<id>`, `style=<name>`, `ratings=<7 numbers>`, `abilities=<ids>`, `moves=<file>`, `entrance=<number>`, `announcer=<NAME>`, `author=`, `version=`, `song=<file>`, `movie=<file>`, `call=<0-83>`, `height=<scale>`, `pose=<id>`, `voice=<file>`, `attire2..4=<file>`, `attire2..4_name=<text>`, `attire1..4_entrance=<number>`, `cae=<file>` |
| `ch.pac` | the model pac (EPK8, like `pac/ch/chNNN.pac`). It can be any character's: the `EMD` names get the slot's id |
| `theme.<ext>` | (optional) the entrance song: .mp3 .m4a .aac .wav .flac .wma .ogg |
| `movie.bik` | (optional) the entrance movie: a 320x320 Bink, made in the launcher's Movies tab |
| `render.dds`, `render_small.dds` | (optional) its select-screen render: 512 x 512 and the 256 x 256 bust, DXT5 (the Mod Maker makes them from any picture) |
| `render_full.dds` | (optional) a separate 512 x 512 full-body picture for SSFB (the select screen in some modes); without it SSFB is `render.dds` too |
| `voice.<ext>` | (optional) a recording of the name: the ring announcer "says" it |
| `attireN.pac` | (optional, N = 2-4) another model pac: its first attire becomes the mod's attire N |
| `threads.txt` | (optional) its Superstar Threads parts, from `tools/threads_parts.py` (see Superstar Threads) |
| `moves.txt` | (optional, `moves=`) its own moves over the base's: lines `0xOFF=<move id>`, OFF a byte offset in the profile's move block (0..0x1BF, even) |
| `disabled` | (optional) turns the mod off |

`base=` is the disc superstar the mod starts from, which supplies its stats,
moves, entrance motions and pyro, and select render. It must be one of the 91
playable ones, those with `SSFA` renders in `pac/DLC_HD.pac` (which includes
The Hurricane, 274).

## Ids: 72 slots

The pool, in `kPool`:

- 61 disc ids whose `CHAR/DAT` record is a blank placeholder: name "0",
  loaded, not selectable, with no model, render, entrance or match data in any
  pac (111, 114, 121, ...; the 22 from 228 on were added later, 50 -> 72);
- then the free DLC slots **59-69** (real DLC uses 51-58).

Each of these has a record, a profile and room in the save.

The ceiling: the game indexes 242 ids (0-321, table 0x82DB3610; 322 means
"no character"). 65 more indexed ids are "absent" (present flag
0x82DB3AE0 + id off): with that flag set they could hold mods too, about 137
in all - not done, untested. Past that there is no free record, profile or
save room without changing the game's index and the save layout.

Each mod keeps its id in `Mods/Superstars/slots.txt` (`<folder>\t<id>`):

- Adding or removing a mod never moves another one's data in the saves.
- Pool ids with no mod are kept unselectable, so a removed mod's record
  left in a save can't be picked.
- Remove a mod's line from slots.txt to free its id.

## How it works

### Files

- Each mod's `ch.pac` gets its EMD names renamed to its id.
- Its base's renders are written to `ssfNNN.pac`.
- Both go in `Mods/SuperstarOverlay/`. They are rebuilt only when the
  source is newer.
- They are mounted with the game's `sub_826A1780` from `smods:`, a device of
  its own made after the files are written. The game's `GAME:` device lists
  its folders once at start, so it would not see files made this run.

### The game's directory buffer

Each mount appends the pac's directory to the game's directory buffer: vfs+56
is the start, vfs+60 the end, and vfs is at 0x82ED5FEC. There is no room for
dozens more: 15 mods crashed. So before mounting, the buffer moves to a bigger
allocation. What was registered keeps pointing into the old buffer, which is
left alone. The render pacs' header +4, the directory size, is their own.

### Records and profiles: no resetting

A slot is set up from the base only when it isn't the mod's yet:

- **The record:** set up when its name isn't the mod's (a new mod, or a save
  from before it). It gets the mod's names, its own id at +32, +218 and +228,
  selectable and the DLC flag.
- **The profile:** set up while it still equals the blank one `CHAR/PRO`
  loaded for the slot.

The game reloads `CHAR/DAT` and `CHAR/PRO` after a save loads (the DLC scan,
`sub_825A09B0`), which puts the placeholders back. The slot as it was just
before is remembered and put back, so the save's copy and the player's edits
stay. The save (`SaveData.dat`) holds both records and profiles.

Tested: the game wrote a save holding an edited mod (ratings 20). On the
next start it still had ratings 20 after the save and the reloads.

### Theme and movie

At start, `song=` is copied to `Music/<name>.<ext>`, making it a USER
PLAYLIST. `movie=` is copied to `Custom Movies/<name>.bik` and gets a user
movie id, kept in that folder's ids.txt. The movie copy happens before the
Custom Movies device is made.

When the profile is first set up, its entrance block (profile +0x1C0;
scratchpad re_entrance) gets:

- music id +0x10 = 254 (USER PLAYLIST), with the name at +0xCC (UTF-16BE);
- movie id +0x12 = the user movie.

The base's motions and pyro stay, because +0xC7 stays 0, the default entrance.

### Name call

The ring announcer and the commentary pick the name call by character id
(scratchpad re_namecall), so the mod ids had none:

- the announcer's name table says "dummy" for them;
- the commentary banks `Comm_<id>` don't exist.

A mod gets one of two calls:

- **By default, its base's.** The announcer's participant struct gets the
  base's id as call index (`sub_825EA208`: +4). The commentary bank name is
  built from the base's id (`sub_8261EAB0`).
- **With `call=N`, a Created Superstar nickname** (0 Alpha ... 77 The
  Superstar ... 83 Youngblood; the Mod Maker lists them). The announcer
  struct gets +0 = 0 and +20 = N, as a CAW's. The commentary uses bank
  `CAS_*` N+1.

`sub_825FDCD8`, the "has commentary" filter, lets mods through.

**A recorded name (`voice=`).** The game's sound engine can't take new lines,
so the port plays the recording itself:

1. The mod's own entries go into the announcer's name table (from
   sound.pac; T from `sub_825FDDC0(*(sub_825E5E68() + 32))`). Categories 2,
   4, 5, 8, 40, 41 and 42, at the mod's id, become `RA_JR_MOD_<id>_<c>`.
2. The announcer posts `Play_RA_TC_MOD_<id>_8` where a superstar's name
   goes. The sound engine has no such event, so it is silent and the
   announcer carries on.
3. The event hook (`sub_82BEC030`) plays the recording on its own voice
   (music.h `PlayClip`, a once-only player beside the USER PLAYLIST one).

The commentators keep the base's or the nickname call. Tested with
`SVR2011_TEST_NAMECALL=1`, which logs the calls and the posted events:

| mod | commentary | announcer |
|---|---|---|
| Modded Test (base Jericho) | `0104` | `Play_RA_TC_SSP_CHRISJERICHO_0` |
| Mod 13 (`call=77`) | `CAS_The_Superstar_0` | `Play_RA_TC_CAS_Superstar_0` |

### Attires

How many attires a character has, and each one's name and unlock, comes from
misc.pac's COS table (`BATS/INIT` entry 5, keyed by character id; scratchpad
re_attire). The models don't decide it. The table has room for 80 records and
the game uses about 66, and the mods aren't in it, so the select screen offered
them no CHANGE ATTIRE.

Two lookups now answer for mods:

- `sub_828C4140(mgr, id, mode)` gives the count: the attires the mod's
  model pacs hold, contiguous from attire 1.
- `sub_828C4060(mgr, id, attire)` gives the `{name string id, unlock}` pair,
  from a port-made table:
  - the name of attire 1 is the game's ORIGINAL ATTIRE; the others are
    `attireN_name=` or "ATTIRE N", served by menu_hooks' string lookup through
    `SuperstarModString`;
  - the unlock is -1, always available. `sub_828C2240` reads that.

`attireN=<pac>` puts another model pac's first attire in as attire N. It goes
in an overlay pac of its own, `chNNN_aN.pac`, and the main pac's attire N is put
aside. So mods get all their base's attires, plus up to 4 in all.

Tested: ADVANCED shows CHANGE ATTIRE "ATTIRE 3", and the model switches to the
attire pac's (Orton on a Jericho mod).

### Attire mods (an attire for a stock superstar)

`Mods/Attires/<folder>/` (the Mod Maker's Attires page, the launcher's Mods
tab), `manifest.txt`:

| key | |
|---|---|
| `type=attire` | |
| `superstar=<id>` | a superstar the game has (100-321), or an installed DLC superstar (51-58, see below). A superstar mod takes `attireN=` in its own manifest instead |
| `attire=<1-4>` or `new` | replace that attire, or take the first one the superstar's own pac doesn't have |
| `name=` | shown on CHANGE ATTIRE (31 characters) |
| `model=attire.pac` | a character model pac; its first attire is used |
| `entrance=<n>` | optional, the entrance that attire gets (as `attireN_entrance=`) |

- An entrance only (no `attire.pac`, no `model=`; the Mod Maker's Simple entrance
  porting writes these): `attire=<1-4>` + `entrance=<n>` gives one of the
  superstar's own attires that entrance and nothing else - no name, no attire
  slot of its own, so the two-mods rule below and the launcher's Play guard leave
  it alone. Of two for one attire the first folder (A-Z) is used.

- A new attire: the model's first attire is copied to
  `Mods/SuperstarOverlay/ch<id>_x<N>.pac` with the superstar's id and the
  attire's digit in its EMD names, and mounted with the other overlay pacs. The
  game finds models by entry name.
- A replaced attire (one the game's own pac has): of two entries with one name
  the game uses the first, its own, so a mounted pac isn't enough. The model
  goes into an overlay copy of the game's pac instead: a generated move pack
  (`Mods/SuperstarOverlay/attirepacks/<folder>/pack.txt`: `pacentry
  ch\chNNN.pac EMD <entry> entry.bin`), merged by the move-pack overlay
  (docs/MOVE_PACKS.md). Model entries are stored as plain PACHs, as the game's.
- The two lookups above answer for the superstar too: the count goes up to
  the highest attire a mod gives (4 at most), and a mod's attire gets its
  `{name, unlock -1}` pair (string ids from 0xB400); the others keep the game's.
- 4 attires at most. Two mods on one attire (the user's rule, 13 Oct): an attire
  is free when the superstar's own pac doesn't have it and no switched-on attire
  mod asks for it.
  - The launchers (PC Mods tab: `mods_attire_guard`, when an attire mod is
    installed and when Play starts the game; Android Mods page: `attireGuard`,
    on install, on switching one on and on Play) keep the attire for the mod
    installed first and move the newer one to the next free attire, rewriting
    its `attire=` (with a `# moved from attire N` line) and saying so. With none
    free they tell the player to switch one off.
  - The game (games started without a launcher): the attire numbers asked for
    are given in folder order (A-Z), the first keeping its attire; then the ones
    that lost theirs and the `new` ones take the free attires in folder order,
    or are left out with a log line. The same every start.
  - The Mod Maker's Attires page warns when the attire chosen is already used
    by an installed mod.
  - Superstar mods' `attireN=` can't clash with these: attire mods only take
    stock superstars.
- Entrance gear superstars (Undertaker 103, CM Punk 131, John Cena 139, Shawn
  Michaels 145, John Morrison 175: their pac has a kind 4 model, `%06d04`, the
  coat / hat / shirt they come out in) take replaced attires only. Any attire
  past their own never finishes loading the match (tested: Cena / Undertaker
  attire 3 through a guest pac, through the pac overlay, with a kind 4 row
  beside it, even with no mod at all through the test aid); Jericho and Mark
  Henry load new attires fine. Their DLC third attires (CM Punk's 00013122)
  come with the DLC's own chEtc.pac, so per-attire character data is the lead.
  Until that is found the game leaves a new attire for them out (log line),
  the launchers count no free attire for them and say why, and the Mod Maker
  shouldn't offer one.
  - Test: `"SvR2011 Launcher.exe" --mods-attires <game>` runs the launcher's
    guard and prints what it did.
- A `disabled` file in the folder turns it off.

- DLC superstars and DLC attires (2.1.10). The installed DLC packages (unpacked by dlc.cpp to
  `<user data>\0000000000000000\5451085D\00000002\<package>`, only ever read) hold model pacs
  `pac\ch\ch<id>_dlcNN.pac`: the DLC superstars' own (Vol. 2 ch51-56, Vol. 3 ch57 / ch58 - Lex Luger 51,
  British Bulldog 52, ...) and extra attires of disc superstars (Vol. 2 ch145 / ch178, Vol. 3 ch131 / ch279).
  - They count as the superstar's own attires (`OwnAttires`): a DLC superstar takes attire mods like a disc
    one, and a `new` attire for a disc superstar skips the DLC's attires.
  - A new attire: the guest pac, as for disc superstars (the game finds it by name).
  - A replaced DLC attire: a guest pac with the same name loses to the DLC's (tested), and the game reads a
    package's pacs at the places its `info\catalog_hd.arc` / `catalog_sd.arc` give (8-character model names,
    big-endian sector and size). So the generated pack says `pacentry dlc\ch\ch52_dlc02.pac EMD 00005202
    entry.bin`; move_packs makes a copy of that package file with the entry (`DlcCopies` /
    `BuildEntryPacFile`), the catalogs with its places (`PatchCatalog`, EPAC and EPK8 names), and serves both
    when the game mounts the package (`OnContentMount`) - the same path as the packs' DLC m / mpsp / misc copies.
  - The DLC superstars have no record in the game's attire (COS) table (count 0): the count answers the mods'
    highest attire, and the `{name, unlock}` lookup gives ORIGINAL ATTIRE / always for attires with no record,
    else ADVANCED offered no CHANGE ATTIRE.
  - Tools: the Mod Maker's Attires page lists them under "DLC" (`DlcStars`, names from the package's chEtc.pac)
    and counts their DLC pacs; the launchers' attire guards (PC `own_attires`, Android `ownAttires`) read the DLC
    pacs and take ids 51-58 when installed.
  - Tested 4 Nov (hidden, private data): Lex Luger new attire 2 picked on the select screen (Downloaded
    Superstars, ADVANCED, CHANGE ATTIRE) and played; British Bulldog attire 1 replaced; Shawn Michaels' DLC
    attire 2 replaced; Orton's disc attire 1 still replaced; after a match-end save, a new start (overlay up to
    date) plays them the same. Test aid `SVR2011_TEST_ATTIRE_LOG=1` logs the count / pair lookups.

- Every model (a superstar mod's ch.pac and attire pacs, an attire mod's model)
  must fit the match's vertex memory: 192 pages per wrestler
  (`tools/model_budget.py`). Over it, the blood overlays (0x7532, 0x7531,
  0x7530) are left out of the overlay copy; still over, the mod is left out,
  with a log line.

#### A superstar from another game as attires of a game superstar

The Mod Maker, From another game -> Superstar: after converting, **Or put it on a game superstar as an
attire...** opens the Attires page with the converted mod taken in ("From a superstar mod..." on the Attires
page takes an installed one, a .svrmod or a project the same way). Pick the game superstar, the attire number
and **Its attire** (which of the ported attires); save or install, then again for the next attire.

What follows the attire: the model (with its entrance gear, unless left out) and the entrance ("Keep its
entrance": `entrance=`). What can't: the moveset, theme / music, titantron, ratings, abilities and name call are
the superstar's profile (CHAR/PRO and the record), one per superstar for every attire - so they stay the game
superstar's. (The game's only attire-dependent entrance - Rey Mysterio's attire 1 - is code, and Swagger's
attire 1 music the same; none of it is data an attire mod can carry.) For his whole moveset and theme too, install
the converted superstar as a superstar mod of its own.

Test aids: `--port-test "superstar@wwe13,<disc folder>,101,--base 101 --no-port" --port-as-attire
"101,3,1,Stone Cold 13;<save .svrmod>"` (convert, then attire 3 of 101 from its attire 1); `--attire
101,4,<mod .svrmod>,<save>` with `--attire-from 2,Stone Cold 13 B`.

Tested: an attire mod replacing Mark Henry's attire 1 with Sabu's model plays
in a match; new attires (guest pacs) on Mark Henry (2) and Chris Jericho (3 and
4, the launcher's move) play in matches (13 Oct).

### Select screen framing

Where the panel's 3D model stands comes from a placement table keyed by
character id (DLC_HD/SD.pac `MMDL/BIND`, "MMPS": per layout `{id, dx, dy,
dz}`; scratchpad re_framing). Ids not in the table get no offsets, so mods
stood too high, with the head cut off at the top.

`sub_8273D1D0(mgr, slot, layout, out)` looks up the slot's id. A hook gives
it the base's id for a mod, so the mod stands where its base does.

### Select picture

The select panel draws the mod's own model live (a Jericho model on an Orton
base shows Jericho), so a custom model shows itself.

The renders (`SSFA/SSFB/SSFC`) are loaded by the render manager (0x82739xxx -
0x8273Cxxx, scratchpad re_renders):

- SSFB, the 512 render, is used on the select screen in some modes;
- SSFC, the 256 bust, is used on the match-setup page, in WWE Universe, in the
  online lobby and on others.

A mod's own `render.dds` / `render_small.dds` replace the base's there for every
attire, BPE-compressed like the game's. They must be compressed: entries are
decoded in place, so a "stored" one is too big.

### Theme and movie after the slot exists

`<folder>/.applied` lists every theme and movie the port has set for the mod.
A slot whose entrance music or movie is still one of those, or the base's,
follows the manifest; anything else was the player's choice in Create An
Entrance and stays. So a song added to an installed mod reaches a slot a
save already has (tested), and removing it brings back the base's.

### Overlay rebuilds

`<out>.src` records each source's size and time. Time alone won't do: an
installed .svrmod's files carry the zip's old dates.

### Owned and listed

DLC-flagged ids must be owned, and mods are. The DLC tile's filter
(`sub_8244A128`) leaves mods out. They are listed under the M (EXTRA) tile,
after the managers.

The managers are Stephanie McMahon, Theodore Long, Paul Bearer, Tiffany and The
Hurricane. Hornswoggle (195) is left out: his moves and animations are broken.

## The MODDED badge

The select panel shows DLC characters with the "DOWNLOADABLE CONTENT" badge,
the texture `DLCtex01` (menuHD.pac `MENx/CHSI` 7001; 256 x 64 DXT5, one per
language). It is layout node `cursor+312` in `sub_82466830`.

- **Tracking:** a hook on that panel's DLC check (`sub_828B6CD0` called from
  0x82466E40) records which panels hover a mod.
- **Finding the texture:** a background thread finds it in guest memory, by
  the swapped DXT5 blocks of any language's original.
- **Swapping:** it writes the "MODDED" pixels while any panel hovers a mod,
  and the original otherwise.

The art comes from `tools/make_modded_badge.py`, which writes
`src/modded_badge.inc`.

## Keys for superstars the game still knows (ports from SvR 2010)

These keys apply when the port first copies the base's record and profile to
the slot. A save made with the mod keeps the player's own edits.

- **`moves=<file>`:** each `0xOFF=id` line writes a big-endian u16 move id at
  that byte of the profile (CHAR/PRO, 1056 bytes; the moves are +0x000..+0x1BF,
  the entrance block +0x1C0). The ids must have motion in 2011's m.pac.
- **`entrance=<number>`:** the entrance number, written at profile +0x1C0
  +0x14, +0x16 and +0x18. Example: 535 is Jeff Hardy's, which 2011 ships in
  `evt/Nyujyo7.pac`, along with its pyro sound bank `Pyro_Jeff_Hardy` in
  Entrance.pck, though no profile uses it. The music and movie still follow
  `song=` / `movie=`. Any entrance number works: a stock one, or one a move
  pack adds (converted from another game: `tools/entrance_port.py`,
  docs/OTHER_GAMES_ENTRANCES.md).
- **`cae=<file>`:** a created entrance (CREATE AN ENTRANCE's ADVANCED
  CREATION: motion, pyro and the rest per scene) as a 284-byte entrance block,
  big-endian as the game holds it. The mod's slot plays it until the player
  makes their own. Details: docs/ENTRANCE_ATTIRES.md 1.
- **`attire<N>_entrance=<number>`** (N = 1-4, the mod's attire): that attire
  walks out with its own entrance, with or without a title - like Rey
  Mysterio's "Evil" attire, which the stock game does in code. A created
  entrance (CREATE AN ENTRANCE) and a team's entrance still win. Details:
  docs/ENTRANCE_ATTIRES.md.
- **`abilities=a,b,...`:** up to 8 ability ids at record +230 (0 ends the
  list). 2011 uses 1, 7, 9, 11, 12, 14, 19, 20, 22, 23, 24.
- **`announcer=<NAME>`:** the ring announcer's own clips of a name the sound
  banks still have.
  - At the mod's id in the announcer name table, every category where the
    base's entry says the base's name ("RA_JR_SSN_MATTHARDY_1"; the name is
    taken from category 8) gets the same entry with NAME instead.
  - The game picks RA_TC_ from RA_JR_ itself.
  - 2011's RA.pck has real Jeff Hardy recordings by both announcers
    (`*_SSN_JEFFHARDY_0..2` name calls of 2-3 s, `*_SSP_JEFFHARDY_0..5,9`
    introductions of 8-12 s), which nothing used.
  - It takes priority over `voice=`. `call=` then only picks the commentary.
- **`pose=<id>`:** the pose its model plays on the character select / VS
  panel. A motion id (32500 and up), or a superstar id (below 512) to copy that
  superstar's pose; missing = the base's pose. `sub_8273D4A0(mgr, slot)` picks
  it: the slot's id in the {id, motion} pairs of DLC_HD/SD.pac MMDL "MMMN"
  (mgr+1392, 90 pairs; -1 = none), else the next of a rotating list (male
  32500-32505, female 32510-32515), else 32500. Mods aren't in the pairs, so
  before this they took a rotating pose. Poses in the table: 32520-32555,
  32580-32583, 32590-32596 (`SVR2011_TEST_POSE_LOG=1` logs the table;
  `SVR2011_TEST_POSE_AS=<id>=<mod id>` puts a mod's pose on that superstar's
  panel, for tests). The select screen is the only place the game uses it.
- **`height=<scale>`:** its size against the base's, 0.80-1.25 (1.05 = 5%
  taller; missing = the base's own). Record +28 is the superstar's scale, 4.12
  fixed point (4096 = 1.0, most are near it): the skeleton is built with its
  bone lengths times it (sub_826E75A8), and grapples / IK follow. The port
  writes base +28 x scale, kept within 3300-4900 (so about x0.81-x1.20 for a
  4096 base), from the base's value each time (never compounding through a
  save). At 4139 and up the game counts the wrestler as "big" (another
  variant of some moves). The moves are made for about 1.0: 0.90-1.15 plays
  best. Unlike the other keys here it is set on every load, saves included.
- **SvR 2010 model:** `tools/svr10_char.py <2010 pac> <id> <2011 pac> <out>`
  turns a 2010 chNNN.pac into one 2011 loads. It swaps in 2011's face
  animation child (0x64) and uses the 2010 select bust as the 256x256
  portrait (0xc8/0xc9).

## Hornswoggle (small rig)
Hornswoggle's model (ch195) is the only one with its own skeleton: legs 0.42x
and arms 0.51x the standard rig's. Standard motions made him float with
straight limbs. `src/small_rig.cpp` retargets the pose before the IK solve
for id 195 and for any mod with `base=195`: root height scaled above the
feet, hands and feet pulled in around shoulders and hips. Cvar
`small_rig_retarget` (on). Grapples still use the standard contact points
(his hands land short). The bundled `hornswoggle.svrmod` uses his 2011
model, renders and announcer name, a small-wrestler moveset (Frog Splash as
the Tadpole Splash, headbutts, bites, sentons, no lifts) and the General
Entrance (522; he has no theme of his own).

## The Mod Maker: Create a new superstar

The Superstars page makes a new superstar from a form; it doesn't start from
another superstar. Fields marked (optional) can stay empty.

- **Fighting style:** picks the `base=` template (its moves, entrance motions
  and pyro) and is written as `style=`. Powerhouse 125, Brawler 160,
  All-rounder 139, Technical 104, Submission 267, High flyer 123, Showman 218,
  Diva power 224, Diva high flyer 164, Diva technical 143.
- **Attributes:** 7 sliders, filled from the style's template (chEtc
  CHAR/DAT) and written as `ratings=`. The game writes them into the record
  (slots +0..+6) when it sets the mod up. Speed, Charisma and Durability are
  certain; Grapple, Submission, Strikes and Hardcore are inferred from who
  tops each slot.
- A styled mod without `call=` is called "The Superstar" (77). Its select
  picture is the generic silhouette (9000) until it has one.
- **Model (required):** the ch.pac.

### The 3D preview

`modmaker/char_preview.cpp` shows the model at the right, skinned and playing
the game's idle stance, so the modder sees that the model works. Drag to turn
it.

- **Model:** EPK8 group `EMD `, entry `%06d%02d` (attire*10 + kind); kind 2 is
  the model, and the preview takes attire 1's. The entry is a PACH:
  - Child 0 is the body JBOY, whose nodes are the whole skeleton.
  - Hair (0x2710), eyelash (0x4e20) and attachments are further JBOYs. All
    are in the body's model space, and their nodes map to the body's by name.
  - The blood overlays (`blood_*`) are skipped.
  - Texture bundles hold plain PC DDS. Meshes use their `texDiffuse` name
    (case-insensitive), mostly `color`.
- **JBOY nodes and weights** (corrected in `svrfmt/jboy.*`; arenas read the
  same way):
  - Node: r at +32, parent at +48.
  - Local matrix: T(t)·Rz·Ry·Rx.
  - Weights are interleaved per vertex: (vertex k, influence j) is at
    `8*(nb*k + j)`.
  - Palettes hold node index + 1.
  - Vertices are in bind pose, so a vertex is skinned with
    pose(j) * inverse(bind(j)).
- **Idle:** `pac/m.pac` (EPAC, only its index and the one entry are read) →
  `MVMT`/`STAT` → PACH child 3, a YMBs bank; motion id 20000, x 10, y 0, one
  second.
  - 21 rotation channels (root, koshi, mune, kubi, atama, then each arm's
    sakotsu, ninoude, kote and te, then each leg's momo, sune, ashi and
    tsumasaki) replace the node's rotation.
  - A root move.
  - Per limb an IK target and a pole angle. Upper and lower limb bones carry
    no data; the preview solves them as two bones, with the knee or elbow
    turned by the pole.
  - Rotations are keyed 30 a second, moves twice that.
  - Channels are Yamaha-style ADPCM. The full format: scratchpad research
    `re_charmodel.md`, summarised in the file's header.
- The game's own IK solver (sub_823A4DD8) wasn't decoded; the preview's IK is
  an approximation that looks right on Jericho, The Rock and a Diva.

## Superstar Threads

`src/superstar_threads.cpp`, `tools/threads_parts.py`. Threads lists the
superstars of `edit\edit.pac` EDIT/SSTL: one PACH child, a u16 count (68), then
484-byte records:

| bytes | |
|---|---|
| 0 | u32 id (little-endian) |
| 4 | 48 u32 part-name string ids, 12 per attire (attire a, part p at (a-1)*12+p-1); -1 = none |
| 196, 244, 292 | per part: area (the camera: 1 head, 2 arms, 3 chest, 5 waist / hands, 6 legs, 8 feet), view, kind |
| 340 | per part: its default RGB |

Part p of an attire paints where channel (p-1) % 4 (R G B A) of that attire's
`Mask1` / `Mask2` / `Mask3` texture is set; the masks sit in the model's EMD
entry next to `color` (children 0x0d-0x0f) and no mesh uses them. The names are
the game's (string.pac group 0600 and its translations): ELBOW PADS, TRUNKS 1,
KNEE PADS DESIGN 2 ... Each name always comes with the same area / view / kind.

A superstar's three slots are the last 324 bytes of his 1056-byte profile
(+0x2DC, +0x348, +0x3B4: 108 bytes each; +0 = -1 when empty, else the attire it
started from; RGB per part from +4, the name at +0x28).

At start the port writes `Mods\SuperstarOverlay\threads.pac`: the game's SSTL
plus a record for each listed mod, as EDIT/SSTM. The game's lookup of
`/EDIT/SSTL` is pointed at it; edit.pac is never written.

- **Same model as its base** (attire-1 models byte for byte: an attire swap, a
  renamed copy): the base's record under the mod's id.
- **`threads.txt`** in the mod folder: its own parts. Lines
  `<attire>.<part>=<name id>,<rrggbb>` (`format=1` first; `#` starts a
  comment). The name id must be one a stock record uses; its area / view / kind
  are taken from the stock parts of that name. `tools/threads_parts.py <mod
  folder>` writes it from the model's masks: each used channel is named after
  the body area under it (the bone weights of the vertices that sample it, and
  their height) - TRUNKS 1 / 2 for two large waist channels, TRUNKS DESIGN n for
  small ones, KNEE PADS, KICKPADS, BOOTS, SOLES ... - and gets the texture's
  colour there as its default. `--json` is for the Mod Maker, `--names` lists the
  usable names. superstar_port.py and svr10_superstar.py run it on every mod they
  make. On the stock models it names the Rock's 8 parts as the game does, and
  the others close to it.
- **Models without masks** (ports from SvR 2006-2009 and the PS2 games):
  `threads_parts.py <mod> --make-masks` paints them first (`--dry-run --preview
  <png>` shows them over the colour texture). The skin's colour is read off the
  head meshes (warm, mid-bright texels: not hair, beard or eyes); a texel of a
  clothing mesh whose colour is far from it (RGB distance over 60) is cloth -
  per texel, as a legs mesh holds trousers and knees. Cloth regions in the
  texture (specks and seams opened away) get the body area under them and their
  colour; regions of one area and colour make one channel, and the biggest
  eight (arms, top, waist, legs, feet in that order) go into Mask1 / Mask2
  (children 0x0d / 0x0e, A4R4G4B4, value 11 as the stock masks; BPE-compressed -
  the game unpacks in place - by svrmod.exe or, without it, the Python coder,
  about 20 s an attire). Left out: thin or skin-surrounded dark patterns
  (tattoos), small skin-coloured regions (chest hair). Each mask's DDS reserved
  words (+32) hold "SVRT" and its channels' body groups, so the names follow the
  painter's group even where a model's hands sample its trousers' texels.
  Height is measured along the skeleton (foot- to head-weighted vertices), as PS2
  models stand in a T-pose. Empty masks (the PS2 converter writes blank ones) and
  another model's (a template's: most of what they mark lies outside this
  model's colour triangles) are replaced; `--remake-masks` replaces any. The
  pac's first version is kept in `<mod>\.threads_backup`. superstar_port.py does
  it on every mod it makes whose model needs it. Tested in game: Sabu (SvR 2008:
  GLOVES, TRUNKS 1/2, TIGHTS, BOOTS, BOOTS DESIGN, SOLES; attire 2 adds a
  SINGLET) and Goldberg (PS2 HCTP: WRIST TAPE, TRUNKS, KNEE PADS, BOOTS - his
  tattoos left alone); Carlito (SvR 2008) gets TRUNKS and TIGHTS.
- **Neither:** not listed; the log says why ("... has no threads.txt (Mod
  Maker: make Threads parts)").
- **Slots:** kept by the mod's folder in `Saves\.threads\<folder>.bin` (324
  bytes), not by the id it holds this run. They are put into its profile as it
  is set up (empty when there is no file - never the base's), and written when
  the game saves (the file is removed when all three are empty).

## Known limits

- A mod has at most 4 attires.
- Superstar Threads lists a mod only with its base's model or a threads.txt, and paints only models
  with masks (see above).
- WWE Universe lists are not checked. The select screen itself is: one on one, tag team (2 on 2), and the
  COM side.

## Tests

`tools/superstar_test.ps1` runs an exhibition one on one: P1 opens the M
tile, steps `-Down` entries (the 5 managers come first) and picks a mod.
Options:

| option | effect |
|---|---|
| `-Com` | COM's pick goes through the M tile too |
| `-NoSkip` | lets the entrances play |
| `-Walk N` | only steps down the list, a screenshot per step |
| `-UserData <folder under runs>` | uses that save folder as it is |

Test aids (environment variables):

- `SVR2011_TEST_VFS_LOG=1`: every virtual file lookup that finds nothing.
- `SVR2011_TEST_STAR_EDIT=<id>`: that mod's ratings set to 20 when set up,
  to test that edits survive a save.
- `SVR2011_TEST_NAMECALL=1`: the announcer and commentary name calls and
  the posted `Play_` audio events.

`superstar_test.ps1 -MatchDown N` picks another match category (1 = TAG TEAM).

Mod Maker test aids: `--page 3 --star <id> [--star-song f] [--star-movie f]
[--star-picture f] [--star-call N] [--star-voice f] [--star-model ch.pac]
[--star-name text] [--test-star-save out.svrmod]`.
`--star <id>` loads `pac/ch/ch<id>.pac` as the model, so the 3D preview shows
it (`tools/modmaker_shot.ps1 -Extra "--page 3 --star 104"`).

Launcher test aid: `--mods-add <game> <file.svrmod>`.

Verified (2026-10-03):

- **List:** 16 mods (ids 59-69, 111, 114, 121, 127, 128) and the managers
  show in the list, with renders and the MODDED badge.
- **Matches:** a mod on a disc id (114) plays.
- **Full pipeline:** Mod Maker → .svrmod → launcher → game, an Orton-based mod
  with its own theme (played via USER PLAYLIST) and movie (on the titantron).
- **Saves:** edits survive.
