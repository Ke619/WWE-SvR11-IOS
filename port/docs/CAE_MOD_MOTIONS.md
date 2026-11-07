# CREATE AN ENTRANCE: mod entrance motions (the MODS group)

Entrances that mods add (move packs' `pacentry EVP? <n>` lines: the entrance converters'
output, `tools/entrance_port.py`, and superstar mods' own entrances) can be picked in CREATE AN
ENTRANCE like the game's, and given to any superstar, CAW or team. Code: `src/cae_mod_motions.cpp`
(+ `move_packs.cpp` ModEntrances / EntranceScriptExists, the hooks in `user_movies.cpp` and
`superstar_mods.cpp`).

## What the player sees

- **EASY CREATION** (one page: MOTION, CHAMPION ENTRANCE, MULTI-TITLE ENTRANCE): the MOTION lists
  end with one row per mod entrance, `MOD: <label>` (tag/trio numbers 700-799 in the team lists).
- **ADVANCED CREATION** (scenes INTRO / STAGE / RAMP / RING-IN / RING): each scene's MOTION list ends
  with the mod entrances whose Create-An-Entrance part for that scene is installed
  (10000 + 1000k + n, k = scene 0-4 - the converters always install all five).
- The label: the superstar mod's name (manifest `name=`) for its own entrance - with the script's
  name when it has several - else the script's own name and the game it was converted from
  (`MOD: MANKIND (WWE '13)`).
- Every installed superstar mod has its own row too, `MOD: <name>`, for its `entrance=` /
  `attire<N>_entrance=` numbers - also when the number is a game entrance (most older mods use one: Jeff
  Hardy's 535 is listed as Matt Hardy's) or another mod's. In a scene list only when that scene's part
  exists. Editing a mod's entrance highlights its own row, not the first row with its number (the select
  screen's pick, `CaeNotePick` / `CaePreferredRow`).
- SELECT TEMPLATE (ADVANCED CREATION, a list per scene filled by sub_8287E0F0, categories = scenes 0-4)
  gets the same rows. A template pick reads a 16-byte template record by the row's value
  (sub_82883A68; +4 the motion): a mod's number has none - the null record crashed the game - so it gets
  a copy of the scene's first record with the mod's motion (29 Oct: MOD: MANKIND picked -> MOTION
  MOD: MANKIND). Test aid `SVR2011_TEST_CAE_LISTS=1`: every list filled (category, rows, first values).
- A stored number the list doesn't have keeps a row of its own instead of turning into row 0
  (ALICIA FOX) and being saved so: its mod's row, `ENTRANCE n` (a script the list leaves out,
  e.g. a DLC slot's own), or `MISSING MOD (n)` when no script n exists any more.

## What a save stores, and a removed mod

A created entrance stores only numbers (profile S+16, or part +14 at S+24+34p per scene) - the
same for mod rows, so saves, online peers and the Mod Maker need nothing new. When a stored number
has no script (its mod removed or disabled), the match's entrance getters (sub_82756538 per part,
sub_82756718 / sub_82756768 champion / multi-title) use, in order: the superstar's own entrance
(or its part for that scene: 10000 + 1000k + own), else the game's default parts 10507 / 10540 +
1000k; a log line names it (`cae mods: entrance 498 ... has no script (mod removed?): 169 instead`).
Numbers below 100 (DLC entrances, in DLC pacs) are never replaced.

## How it works (research: the game's lists)

- All CAE lists are data: `pac\DLC.pac` group ENEX entry LSD (categories of {value, string id,
  unlock}); sub_8287DFD0(list, r4, table, category, base) turns a category into menu rows (list =
  {count, capacity, items}, items 24 bytes {value, flags, unlock, text pointer at +12});
  sub_828829D8(list, value, text, 0, 0, unlock) appends one. Motion categories: 2 singles, 3 / 4
  teams, 16 champion, 17 multi-title (values = entrance numbers); 15, 7, 8, 9, 10 the scenes'
  parts. Flags 1 hides a row (no visible header row: the "MOD: " prefix names the group).
- The rows are appended after the game's fill (the hook in user_movies.cpp calls CaeModRows); a
  stored value the editors look up (sub_828823E0(list, value)) and miss gets its row there
  (CaeMissingRow).
- Scripts exist by number anywhere (the game tries every Nyujyo group); EntranceScriptExists reads
  the EVP names of `pac\evt\*.pac` once plus the packs' pacentry numbers.

## Tested (2026-10-13, test install with 6 mod entrances)

EASY CREATION (a downloaded superstar's page): rows on the last page, a MOD row picked and saved,
reopened with it shown. ADVANCED CREATION (a created superstar): the 5 scene lists with 6 MOD rows
each, a MOD part picked for INTRO, FULL PREVIEW played. MOVIE -> USER MOVIES unchanged. The mod
disabled: the page showed `MISSING MOD (498)`, the match getter fell back (log line above).
