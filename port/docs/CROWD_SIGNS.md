# Crowd signs (arenas branch)

New signs for the crowd to hold up. Code: `src/crowd_signs.cpp`.

- Mod Maker: Crowd signs page (sign packs), Superstars page (Crowd signs...).
- Launcher Mods tab (PC and Android): installs both.

## The game's signs

The full research is in scratchpad re_signs.

`pac/audience.pac` AUDE/BORD is a PACH whose entry 0 is a PACH of 437 signs:

- each one a texture bundle with one `no01.dds`, 128 x 64 DXT1 with 8 mip
  levels;
- found by sign id, with a binary search, so the ids stay sorted.

| ids | |
|---|---|
| id*10 + 1..4 | a character's signs. The roster record's +210 u16[4] says which a character's fans hold. DLC slots 50-69 have "Dummy" placeholders. |
| 4001..4721 | the Create-A-Superstar sign picker's |
| 5001..5151 | the 16 general signs (`sub_821E9A48`, hard-coded 500..515 * 10 + 1) |

A match shows at most 16 signs:

- first the wrestlers' (from +210: 4 each, fewer with 4-6 wrestlers);
- then general ones.

The table is 16 x {id, character} at 0x82E36958, and the wrestlers' count is
at 0x82E369FC. The match loads the whole bank (`sub_821EA4E0(data, size)`
copies it, allocated by size).

The Create-A-Superstar picker loads the bank on its own (`sub_828DF0F0`). It is
tied to the game's 437 entries and hides some by position, so it keeps the
original bank.

## What the port adds

**A character's own signs.** A superstar mod's `sign1..4.dds` go in as
id*10 + 1..4, replacing a DLC slot's placeholders. Its record's +210 points at
them, repeated to fill 4. Without its own signs, the mod's fans hold its
base's signs.

**Sign packs.** `<game>/Mods/Signs/<pack>/` holds `manifest.txt`
(`type=signs`, `id`, `name`, ...) and `*.dds` files, in name order. A
`disabled` file turns a pack off.

- The signs go in as 6001, 6011, ... up to 9991, so about 400 in all.
- They join the general signs: each match's free slots are drawn at random
  from the game's 16 and the packs'.

**The bank.** The merged bank (audience.pac's plus ours, sorted) is built at
start in guest memory. The match loader gets it instead of the file's.
Nothing in Game Files changes.

**Memory.** Each match load copies the whole bank twice into the match-load
heap (heap 12; lr 821EA528), about 3.9 KB a sign: 637 signs made a 2,492 KB
bank, 4.9 MB of copies. The game's heap 12 is 4.5 MB; the port makes it
16 MB since 2.1.9 (`ring_rules.cpp`), and copies that don't fit still come
from the fallback memory. So the limit on signs is the 400 pack ids above, not
memory: at the most (the game's 437, 400 pack signs, 4 for each of 137
superstar mods - about 5.4 MB) the copies and the match's own 3 MB fit. With
4.5 MB (2.1.8), a heavy install's load ended "out of game memory" (report
ca9157).

## Pictures

The Mod Maker fits any picture, without stretching, onto a white 128 x 64
board, and writes DXT1 with mips: the game's own format (5608 bytes, the same
header).

## Tests

`superstar_test.ps1` logs each match's 16 signs ("crowd signs: this match's
..."). Verified 2026-10-03:

- Modded Test's own signs, "MODDED TEST #1" and "GO MODDED!", held by its fans;
- a pack sign, "SVR 2011 LIVES", in the crowd;
- the pack's ids drawn into the general slots;
- Mod Maker `--page 4 --sign <pic>... --test-sign-save` → launcher
  `--mods-add` → game.

## Limits

- At most 16 different signs in one match: fixed in the game.
- Pack signs don't appear in the Create-A-Superstar sign picker.
