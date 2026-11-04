# Move packs (arenas branch)

Moves the game doesn't have, added by mods. The first pack is the 31 SvR 2010
moves Jeff Hardy needs, including Swanton Bomb 1, Extreme Twist of Fate and
Extreme Neckbreaker Drop. The game's own files are never written.

- **Game:** `src/move_packs.cpp`.
- **Making a pack:**
  - `tools/svr10_moves.py` (ports SvR 2010 moves).
  - `tools/movepack.py` (writes the folder a mod carries).
- **Research:** scratchpad `svr10/re_moveport.md`.

## What a move is

- **Motions:** YMKs banks keyed (id, x = phase or variant, y = track).
  - Tracks: 0 is the attacker, 1 the victim; other tracks are props and
    cameras. The victim's reaction lives under the same move id.
  - Where the same key is stored:
    - `m.pac`: `MOT/BM..` (the whole set), `MOT/RR..` (the per-wrestler
      match bank is gathered from these), `MVMT/<category>` (menus and the
      moveset preview), `RU/RURR` (rumble).
    - `mpsp.pac` `MOTP/GAME`: the banks every match loads for everyone.
  - A bank: "YMKs", a 256-byte event-size table at 0x10, u32 count at
    0x110, then entries {u8 y, u8 x, u16 id, u32 offset, u32 frames, u32
    runtime}, sorted by key. The motion data follows in directory order; an
    entry's size is the next offset minus its own.
- **`misc.pac` `MOVS/WAZE`:** 160-byte move records (id at +0x90). The first
  16 bytes are category bits that decide which moveset slots a move may go
  in. 2011 kept the records of the moves it cut but cleared those bits.
- **`misc.pac` `WAZA/DATA`:** children 0 (EXH: 36-byte per-key move data),
  1 (per-key sound events) and 2 (MBD).
  - Each child is a PACH of groups, sorted by (u16 id, u8, u8).
  - `BATS/INIT` and `BATH/INIT` hold the same three as children 10-12.

2010 data needs these conversions in `svr10_moves.py`:

- Event op 87 has bit 7 set in 2010 and never in 2011: cleared.
- Moves on an opponent sitting in the corner (2010 WAZE +0x73 = 12): 2011
  moved its sitting-in-the-corner pose 253 units (about 25 cm) out of the
  corner and re-rooted its own such moves (3422: victim root Z -326 -> -577).
  A 2010 one ported as is put the victim's hips inside the post, so he
  floated above the turnbuckle (Umaga's Running Knee 5832 on Sabu). The
  victim's (y 1 / 51) root Z gets -253 and the attacker's (y 0 / 50) +253,
  per x (a running move's x=20 keys; not its run-up x=0), when the victim
  starts where 2010's pose is (first root Z above -450). BM/RR copies only:
  the packed MOTP copies' root isn't decoded (whether matches play MOTP or
  BM for these isn't confirmed).
- The victim's end: a victim track's (y 1) last event op 49 names the motion
  that follows (sub_821D48D0 -> sub_82395E10); 0 names none and the game
  goes straight to the get-up (motion 20). 2011's victims that end on the
  mat name a down motion (443-448, 473-478: lying, selling). Several 2010
  ones name 0 - Jeff's Hurricanrana 8 (7573): the opponent got up at once,
  ~250 frames before the stock Hurricanrana 5 (7450 -> 446). A ported victim
  that ends low (last root Y above -400) with op 49 -> 0 gets the target of
  the most similar stock move (same EXH group and x, fewest differing EXH
  bytes, its victim chained to a motion), in the BM and MOTP copies.

- Top-rope moves: 2011 seats the opponent on the top turnbuckle first (stock
  23100 lifts him; its last event, op 79 y -1000, raises his base 1000 units),
  and slots 0x140-0x146 play moves that start from that seat (WAZE +0x73 13
  facing out, 14 facing in; 23163's victim starts at root Y -210). A 2010 one
  (situation 10/11: Super Hurricanrana 5735, Superplex 5736, Sunset Powerbomb
  5771) lifts him from the mat itself, so played there he went a body-height
  above the turnbuckle (Sabu). Not converted: `svr10_superstar.py` puts only
  situation 13/14 moves in those slots (a same-name 2011 move, else the base's).
  Slot 0x10A (corner grapple, standing opponent) still takes situation 10.

`svr10_moves.py fix <2010 pac> <2011 pac> <mod.svrmod>...` applies both to a
built mod (version +0.1, a pack.txt line so the overlay rebuilds; a second
run changes nothing).

The MOTP copies are otherwise carried over unchanged (packed).

## A pack

`pack.txt`, one item per line:

```
motion <m.pac|mpsp.pac> <TYPE/NAME/child/...> <id> <x> <y> <frames> <file>
waze <id> <16 bytes hex>            the move's category bits
exh <group> <36 bytes hex>
evt <group> <16 bytes hex> <events hex>
mbd <group> <8 bytes hex>
pacentry <pac under pac\> <GROUP> <name> <file>   a whole entry of an EPK8 pac
```

The motion files go in `motions/`.

`pacentry` adds (or replaces) a whole entry of an EPK8 pac, unpacked in the
pack: e.g. a converted SvR 2008 entrance, its script and tracks
(docs/SVR08_ENTRANCES.md):

```
pacentry evt\Nyujyo5.pac EVPE 504 entrance/504_evp.bin
pacentry evt\Nyujyo5.pac EVTE 504 entrance/504_evt.bin
```

A pac named `dlc\<path under the package's pac\>` is an installed DLC package's own file, e.g.
`pacentry dlc\ch\ch52_dlc02.pac EMD 00005202 entry.bin` (attire mods replacing a DLC attire,
docs/SUPERSTAR_MODS.md): its copy with the entry goes to `Mods/PacOverlay/dlc/<package>/`, the package's
catalogs get the copy's places, and both are served when the game mounts the package - as the DLC
m / mpsp / misc copies below. The package's files are never written.

The pac is copied to `Mods/PacOverlay/evt/Nyujyo5.pac` with the entries in
(numeric name order, the `65535` end mark last, BPE-packed, the packer's
footer kept) and the overlay pac list names the copy
(`mods\pacoverlay\evt\nyujyo5.pac`). The game's own file is never written.
Of two packs' entries with the same group and name the first read is used.
The entry files are in the stamp, so a changed one rebuilds the overlay.

Where a pack goes:
- in a superstar mod: `Mods/Superstars/<mod>/moves/`
- on its own: `Mods/Moves/<pack>/`

A `disabled` file in the mod or pack folder turns it off. Keys the game
already has are left alone.

Every move a pack defines (a `waze`, `wazename` or `wazecopy` line) is named
`MOD: <name>` in its record, so the game lists it so everywhere (CREATE A
MOVE-SET, the moves lists): `MOD: Mandible Claw`. The game's own moves keep
their names.

Installing a mod with custom moves (a pack, or a superstar mod's `moves/pack.txt`)
asks first - "experimental and most likely broken", Yes / No, No the default,
"Don't warn me again": the launcher (+ and Discovery) and the Mod Maker
(`launcher/move_warning.c`; the tick is `Mods\.no_move_warning`), the Android
launcher's Add a mod (`ModsPage.askMoves`; the tick in its preferences). With no
one to ask (`--mods-add`, a hidden Mod Maker test) it is only logged
(`Mods\.install.log`, the Mod Maker's log); the port's bundled mods aren't asked
about. Test aid `SVR2011_MOVE_WARNING_SHOT=<file.bmp>`: the box drawn off-screen,
saved and answered No.

A pack of its own may carry a `manifest.txt` (`type=moves`, `name=`): its
entrances are listed in CREATE AN ENTRANCE as `MOD: <name>` (as a superstar
mod's are), else as the entrance script's name and the game from `pack.txt`'s
first line. The Mod Maker's Simple entrance porting names them after the
superstar.

A move a pack adds only plays for a wrestler whose moveset has it (a superstar
mod's `moves.txt`, or Create-A-Moveset): each wrestler's match bank is gathered
from the `MOT/RR..` banks by his profile. Forced in a test
(`SVR2011_TEST_FORCE_MOVE`) on a wrestler without it, it isn't found.

`tools/moves_port.py` (WWE '12 / '13) writes such packs from other games' moves:
the source's bodies on a 2011 template move's record, banks and events
(docs/OTHER_GAMES.md).

## How the game uses them

Packs are read in path order: `Mods\Moves\<pack>`, then
`Mods\Superstars\<mod>\moves`, A-Z. Of two packs' copies of one motion key
the first read is used (`movepacks 7`; before, either). Copies that differ are
logged once per pair of packs, e.g. `move packs: "Moves" and "jeff_hardy"
carry different motions for moves 2504 3410 7573: "Moves"'s are used` - an
old exported move pack hides a mod's fixed motions that way.

1. **At start-up** (`InstallMovePacks`, before the game mounts its pacs):
   - Every enabled pack is merged into copies of `m.pac`, `misc.pac` and
     `mpsp.pac` in `<game>/Mods/PacOverlay` (and of each pac `pacentry` lines
     name, under `PacOverlay/evt` etc.; `movepacks 8`).
   - Unchanged entries are streamed from the originals. Changed ones are
     rebuilt and compressed for real (BPE).
   - New YMBs motions (submissions, taunts) go AFTER the bank's stock entries
     (before its end mark), never between them (`movepacks 9`): the game keeps
     YMBs motions by their place in the bank, so one inserted in key order
     shifted the stock motions after it. Sabu's ported camel clutch (5962)
     ahead of Camel Clutch 2 (21450, every superstar's) made that move read
     another motion's data: stretched bodies, then a crash. YMKs banks are
     looked up by key and keep the sorted insert.
   - `stamp.txt` records the packs and the game files. The merge runs again
     only when one of them changes; it takes about 3 s.
   - With no packs the overlay files are deleted.
2. **The pac list:** the overlay holds copies of `pac/plist360.h` and
   `plist360_4x3.h` that name the merged pacs (`mods\pacoverlay\m.pac`).
   The game reads its pac list from there (`sub_826B9280`, `arena_mods.cpp`).
3. **Mounting:**
   - The game normally finds pac entries through the pre-built directory
     `plist360.arc`, a copy of every listed pac's table. It only knows the
     original files.
   - Grown entries make loads fail: a crash at start-up, or a hang while a
     match loads.
   - With an overlay the game instead mounts the pacs by reading each one's
     own table: `sub_825953B0` in place of `sub_82595428`
     (`superstar_mods.cpp`).
   - `tools/plist_fix.py` can refresh an arc instead, for tests on patched
     files.
4. **Installed DLC:**
   - DLC Vol. 2 brings its own `pac\m_dlc02.pac` (MOT/RR00-02, RB00, BMD0,
     RRBS, BA00) and `pac\mpsp_dlc02.pac` (all of MOTP/GAME). With it
     installed the game reads those, not the overlay's, so the packs go into
     copies of them too: `Mods\PacOverlay\dlc\<package>\<name>`.
   - The package (`<user data>\0000000000000000\5451085D\00000002\<package>`)
     is only ever read. Test folders link their user data to the player's,
     and an older build that merged into the package changed the player's
     DLC. If a package still has a `<name>.orig` from that build, it is put
     back at start-up.
   - The package's `info\catalog_hd.arc` / `catalog_sd.arc` are pre-built
     directories of its pacs (each entry's name, big-endian sector and size).
     The game reads the pacs through them, so a merged copy read at the old
     places crashed every match. Copies of the catalogs with the copies'
     places go beside them. An entry is found by name plus old place; a copy
     that can't be found so isn't served.
   - When the game mounts the package (`dlc0:`, `dlc1:` ... in the order it
     finds them; SDK `SetContentMountHook`) its root gets links
     `<root>:\pac\<name>` and `<root>:\info\<catalog>` to the copies. The SDK
     picks the longest matching link, and an open checks a link for the file
     itself before its folder's.
   - One merge at a time: `Mods\PacOverlay\merge.lock` (a folder), and every
     pac is written as `.part` and renamed.
   - The DLC's `misc.pac` is not copied (one with the packs' records crashed
     the pac loading); its MOVS/WAZE gets the packs' records as it loads.

## Tests

- **`SVR2011_TEST_MOVE_LOG=<id>,...`:** logs the motion lookups
  (`sub_823941F8`) of those move ids, found or not, and failed allocations.
  - Lookups on a loading thread at match start only check the global
    banks.
  - Lookups on the match thread are the moves actually played.
  - Lookups of tracks 50-100 that aren't found are the game probing
    optional tracks.
- **`SVR2011_TEST_PLIST=<folder>`:** use a pac list from that folder (an
  overlay made by hand) instead of merging.
- **`superstar_test.ps1 -Com -ComDown 5 -Down 4`:** the computer plays the
  Jeff Hardy mod (list entry 5) against The Hurricane.

Verified (2026-10-04), against the computer as Jeff Hardy, with the game's
files untouched:
- The overlay merged by the game is byte-identical to the
  `svr10_moves.py` build.
- Ported moves play in matches: Snap Jab 2 (3088), ECW Feint Wheel Kick
  (3274), and Hurricanrana 8 (7573, both tracks).

Not verified:
- How the ported motions look move by move.
- The moveset lists in Create-A-Moveset.
- The Royal Rumble move (8414).
