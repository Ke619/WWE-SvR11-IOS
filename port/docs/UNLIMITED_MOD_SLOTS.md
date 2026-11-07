# Unlimited superstar mod slots / 00mods.dat - research (D14)

User's list: "unlimited slots for modded characters, using a custom save file for
mods (00mods.dat) instead of saving them to 00savedata.dat". Research only,
nothing built. Today (0c88905) mods take 137 game ids: 72 blank + 65 absent.

Confidence: **H** read in the code, **M** inferred, **L** a guess.

## Where ids are bounded

### Id tables (static, in the XEX image)

| Table | Address | Read by | Notes |
|---|---|---|---|
| id -> index, u16[322] | 0x82DB3610 | only `sub_828B7038` (id >= 322 -> 512) | 242 ids indexed (0-241); CAW 0-49 and 70-99 not (512). **H** (checked in the image) |
| index -> id, u16[242] | 0x82DB3898 | `sub_828B7060` (+ `sub_828B83B0`, `sub_828B84F8`) | **H** |
| present, u8[322] | 0x82DB3AE0 | only `sub_828B7078` | 70-99 present but no index, purpose unknown. **H** |

All three go through small accessors, so the tables themselves are easy to
replace. Everything that stores data per index is not.

### The save is the roster (fixed size)

The live roster *is* the save's memory: two static blocks, written whole.

- Block A at 0x82E407C0, 0x3C154 bytes (`sub_8257AE40` sets `*(0x82E407B4)`). **H**
  - +0: records, 242 x 260 (`sub_8257AF18`). Stats and abilities live here.
  - +0xF5C8: 70 x 270 (`sub_8257B758`): not per superstar (titles?). **L**
  - +0x13F9C: CAWs, 50 x 0x6BC (record, profile at +260, 40 B at +1316). **H**
  - +0x29054: 216-byte table (`sub_8257A470`). **M**
  - +0x2FE04 byte table, +0x364AC u16 table: possibly per id, not confirmed. **L**
  - +0x364B4: Universe. **H**
- Block B at 0x82E7C918, 0x45150 bytes (`sub_8257A4A8`). **H**
  - +8: profiles, 242 x 1056 (`sub_8257BB48`): moveset, entrance, theme, movie.
  - +0x3E648: 242 x 40 (`sub_8257BBD8`).
  - +0x40C18: title holders (= save + 0x7CD70).
- Save writer `sub_827235D8`: `[0x1C header][A][B][28 B]`, 0x812C0 bytes,
  checksummed over the whole file. **H** (sizes checked in the code)

### Other fixed per-index arrays

- Roster snapshot `*(0x82E3DB34)` (`sub_8243B6F0`, `sub_8243A380` / `A488` / `A9E0`,
  `sub_8243AD48`, ~60 `sub_82BB0xxx` users): 242 x 18, 242 x 40, 242 x 260 records,
  50 CAWs. **H** that it exists, **M** that it is the online / default copy.
- 242 x 980 table in a heap object (`sub_82626EB0`, `sub_82626F48`), near the
  commentary code. **M**
- Select screen lists (`sub_82736C68`): 322 u32 ids + 322 flags per list, stride
  2580. Portrait / tile cache `sub_82741FC0`: 268 entries (0-241 index, 242+ none / CAW). **H**
- `cmpwi / cmplwi ..., 242`: 25 compares (CHAR/DAT and CHAR/PRO loaders
  `sub_82594C78` / `sub_82595240`, getters `sub_8259C7D0`-`CBF8`, the snapshot
  functions ...). `sub_8257A230` returns 242. The 322 bound / sentinel shows up in
  about 211 functions. **H**
- `sub_828B70E0` returns a list count of 222 or 272 (+20 DLC); the 272 + 20 case
  would run past the index table. Unresolved. **L**
- Attires: misc.pac COS table (80 records, by id). Universe roster: 150 places,
  mods already kept out (ModOf). **H**

## Bigger table vs remap

### A bigger id table: large, not practical

- Inside the 322 id range only ~32 more ids exist (70-99 and the cut 108, 109),
  so about 169 slots at most.
- Past 322: the ~211 bound / sentinel checks, ids stored as u16 / u8, the 322-id
  select lists, the 268-entry portrait cache, the snapshot, the 980-stride table,
  the CHAR loaders.
- Past 242 indices: move the records out of block A (which also holds CAWs and
  Universe at fixed offsets) and the profiles out of block B (hard-coded base),
  and a new save format.

### Remap: medium-large, practical (recommended)

Keep the 137 live ids and give a mod an id only when it's needed: when it's
picked on the select screen, reusing the least recently used id.

- **What swaps per mod:**
  - record (260 B), profile (1056 B) and the 40 B entry;
  - the ch.pac / SSF overlay (id stamped into the EMD names): remount, or
    prebuilt copies per id;
  - name strings, attire (COS) hooks, sign ids (id x 10 + n), theme, movie,
    announcer / voice tables, height, sort keys, the select tile.
  - Most of this already happens in `ApplyRecords` / `Remember`
    (src/superstar_mods.cpp); new is doing it at runtime, not only at start.
- **00mods.dat:** hook the save write (`sub_827235D8`) and load (`sub_8257A218`).
  Copy each live mod's record, profile and 40 B entry into 00mods.dat keyed by
  mod folder, and put the blank placeholders back before the game writes.
  00savedata.dat then never holds mod data, whatever the slot count.
- **Safe to swap:** only outside matches and loading, outside CREATE A MOVESET
  and the other editors, before `sub_82736C68` builds a list, and not inside an
  online lobby (the D15 key translation must use the current map).

## What a player would see

- **Select screen:** the M (EXTRA) tile opens our own paged list of every mod;
  ids are given on pick. The game's own lists only show ids that hold a record.
  This is the main UI work. One match uses at most ~30 ids (30-man Rumble), so
  137 live ids are plenty.
- **Things that would point at the wrong mod after a swap:** tag teams and Story
  Designer stories saved with a mod id (store those by folder in 00mods.dat, or
  clear them); replays / highlight reels; the snapshot copy (refresh after a
  swap); title holders (mods are already kept off).
- **Universe:** unchanged (mods stay out). **Superstar Threads:** COS ids only, as now.
- **Online:** works if the lobby translation looks up the current map.

## Cost and risk

| Part | Cost |
|---|---|
| 00mods.dat split (save write / load hooks, migration) | small-medium |
| Runtime id assignment + swap (overlay remount, caches) | medium-large |
| Paged mod list on the select screen | medium |
| Save-held mod ids (teams, stories) | medium |
| A bigger id table instead | large |

Riskiest:
1. Remounting overlays at runtime: the VFS directory list, pac caches, and the
   HostPathDevice lists its folders only once. **M**
2. Stale caches: the portrait cache, the snapshot, the 980-stride table. **M**
3. Existing 00savedata.dat files that already hold edited mod slots need a
   one-time move into 00mods.dat. **H**

A possible first step: the 00mods.dat split alone, which keeps 137 slots but
gets mod data out of the main save.

## The save's mod ids (step 0, 3 Nov) and key refs (part 1)

Found by dumping the in-memory save image during WWE Universe shows (test aid SVR2011_TEST_SAVE_DUMP) and diffing
for u16 fields holding character ids (0-321, 0x200 = nobody). save = the image (*0x82E407B4); in SaveData.dat
block A (image < 0x3C158) is at image + 0x1C and block B (image >= 0x3C15C; its first 4 bytes aren't written) at
image + 0x18; P = save + 0x364B4 (Universe).

| Field | Where | Kept by |
|---|---|---|
| Records, profiles, 40 B entries | save + 0, + 0x3C160, + 0x7A7A0 | per-mod files (.mods\<folder>.dat) |
| Teams / stables (70 x 270, members u16 at +2..+10) | save + 0xF5C8 | refs.bin |
| Title holders (14 x 2 u16) | save + 0x7CD70 | refs.bin |
| Universe cards (88 B: 16 B header, 6 people x 12 B, id at +0) | P + 14942 .. P + 20808 | refs.bin |
| Universe list (240 x 12 B, ids at +0 and +2) | P + 20808 | refs.bin |
| Universe entries / rivalries | P + 16, P + 3616 | by place (Universe side table) |
| Universe id cursor (walks the ids between shows) | P + 0x5C92 | left as is (a position) |
| A 26168 B block (match setup state?; ids at +0x2B02, +0x2EB8, +0x652A..) | save + 0x2FE54 | open |
| Story Designer stories | not found yet | open |

Not ids: save + 0x29054 (130 x 216, Create-a-Finisher), the CAW area, block B after the titles (flags), the DDS
pictures after the image in SaveData.dat. 00SceneDat.scn = highlight reels.

Key refs (per_mod_saves on): while the save is built every field above that holds a mod gets a stand-in - the mod's
base superstar when that is a stock one, else 0x200 - and Saves\.mods\refs.bin keeps them:
"SVRMREFS", u32 1, u32 CRC-32 of the rest, u16 folder count, folders (u16 byte count + UTF-8 bytes), u32 ref count, refs
{u32 image offset, u16 folder index, u16 stand-in} (big-endian). The last one is kept as refs.bin.bak. After a load a
field that still holds its stand-in gets its mod's id back, whatever id the mod has this run; a mod that's gone
keeps the stand-in.

## Part 2: the per-index tables (3 Nov)

Three numberings: the character id (0-321), the record index (sub_828B7038, 0x82DB3610: id -> index, 242 indexed)
and the list position (sub_828B7060, 0x82DB3898: position -> id, 292 entries = the 242 indexed ones in index order,
then the 50 CAWs; sub_828B70E0 gives the list length 222 / 272 (+20)). For positions 0-241 position = index.

Tables with one entry per index (242), and who builds their addresses:

| Table | Where | Sites |
|---|---|---|
| Records 260 B | save + 0 | 8257A238 / A2C0 (by index), 8257AF18 (by id, 42 callers) |
| Profiles 1056 B | block B + 8 | 8257BB48 (by id, 17 callers) |
| 40 B entries | block B + 0x3E648 | 8257BBD8, 8257BDB8 |
| Snapshot *(0x82E3DB34): 18 B at +2338, 40 B at +26548, 260 B at +0x9554 | heap | 8243A380 / A488 / A590 / A920 / A9E0 / AAA0 / AB60 / AC98 / AD48, 8243C068 |
| Loaded CHAR/DAT (+4 + i x 260) and CHAR/PRO (i x 1056) tables | *(o), 4(o) | 8259C7D0 / C838 / C8A8 / C9A8 / CAA8 / CBF8; loaders 82594C78 / 82595240 |
| 1056 B at *(o + 1968) + 48, 260 B at o + 1976 | object | 824C3B48, 824C3BA0 |
| 64 B at o + 52 | object | 82438BA0, 82438C30 |
| 980 B | heap object | 82626EB0, 82626F48 |
| Portrait / tile cache (242 + CAWs) | object | 82741FC0 |

Per id (322): id -> index, position -> id, present (0x82DB3AE0), and a 36 B table at block B + 0x420E4
(8257B210 / B240 / B858 / BC70 / BCD0).

How they grow: through mid-instruction hooks ([[midasm_hook]] in svr2011_config.toml, regenerated by the build).
Indices 0-241 stay where they are, so the stock game and SaveData.dat are unchanged; an index from 242 up goes to
an extension buffer per table (found by the table's start, so every site of one table shares it). Three kinds of
site: an address (after base + i x size: i >= 242 -> the extension), a walk (a pointer stepping through the table:
at 242 it jumps to the extension) and a bound (a compare with 242 -> the live index count). Behind the cvar
big_roster (off: every hook returns at once); each site logs once when it first sends an index to an extension.
