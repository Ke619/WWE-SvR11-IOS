# Title mods

A title mod replaces one of the game's championships: its name, its belt's textures and its picture
in the menus. It changes no ids, so saves, Universe holders and online play are untouched (title
mods are cosmetic: they don't count as mods for online matches).

## New titles (in progress)

Not available to players yet: the game ignores `new=1` mods (one log line) unless `SVR2011_TEST_NEW_TITLES_ON` is set,
and the doctor says so. The gate comes out when new titles ship.

A mod with `new=1` adds a title beyond the game's own instead of replacing one: `type=title`, `new=1`,
`name=`, `champion=`, `belt=<key>` (the game's belt model the holder carries: default IC, a tag title WWE_TAG), `short=` (the entrance's banner, where the game adds "CHAMPION" after it; without it the
holder's form less that word) and `kind=tag` for a tag title. Up to 32.

**Works so far:** Championship Scramble's title list and a match on the title; the holder's banner in an entrance;
holders kept beside the save.

**Not yet:** other screens' lists (Universe, title select), a belt or art of its own (the holder carries a stock belt model and the list shows no art for it), announcer lines, online rules, a Mod Maker page.

Outside Universe titles don't change hands in the game: every holder write is Universe, title settings or the save's
set-up (`sub_825B7EB0`, `sub_825B1488`, `sub_8243D680`, `sub_8257B268`).

- The game's table is at 0x82EDEB50, 14 rows x 16 B, all 14 in use: holder a, holder b, +8 a group, +12 kind (2 = tag),
  +13 display order. Names: `sub_82588698(title, kind)`, kind 0 the name, 1 the short name.
- A new title's index is 32 + its place in folder order, for that run only: nothing stores the index. (Not 14 up: Universe's lists use 14 for "no title".)
- Holders are in `Saves	itles.txt`, one row per mod folder: holder a, holder b, each an id, `m:<mod folder>` or `-`.
  The file is written once the save file has changed (the game builds its save before it asks, so a save turned down
  at its prompt doesn't count). The save itself is never changed, so removing the mod takes only its holders. The row
  stays while the mod is off or gone, and the title comes back with its holders if the mod returns.
- Hooks: `src/title_new.cpp`, the title module's functions in 828BD488-828BDEF8 plus 828C22E0 and 8243B0A8, the name
  above, the list below. Each is the game's own with no new title installed.
- Championship Scramble's list (`sub_8244C9B0`): 11 rows on screen, the game's own 11 first. With new titles the rows
  scroll in the list's on-move function (`sub_8244CF50`). The art of a row is a widget (`sub_82452A48`: row -> widget
  1342-1352); its picture is the widget's +0x4C, so a scrolled row's widget gets its title's picture.
- `sub_82732D68` (select screen: is this pick allowed) marks titles in a 14-entry stack list. With a new title in the
  picks the same rules run in the hook, so nothing writes past the list. Written from the game's code; no run has
  confirmed that path was taken.
- Universe's per-title store (`sub_825B6BE0`, P+20776) is 8 rows of 4 B, only for the titles in the list at 0x8206D6BC
  (`sub_828BDA98`). It returns a pointer, so a new title can have its row in a side buffer.
- Universe (in progress). `brand=raw|smackdown|both` puts a new title in Universe, `like=<key>` picks its rank class
  (`sub_825B74D0`: 0 world, 1 tag, 2 midcard, 3 women, 4 none). Universe's titles are 0, 1, 4, 5, 7, 11, 12. Its loops over
  titles run on past 13 to the new ones (`NewTitleLoop_*` in `svr2011_config.toml`). A title's Universe row is in a side
  buffer, not yet saved. The rankings screens (`sub_8249B208`) show 4 fixed rows per brand, the third with two faces for
  the tag title, so the last row carries the brand's new titles: moving past it shows the next one (`sub_8249B600`), and
  picking it opens that title's page (`sub_8249CAC8`, from +2944). Tag titles have no row yet. No art: the row hides it;
  the title's page still shows the row's stock belt.
- Universe's cards (save + 0x364B4 + 14942, 66 x 88 B: +1 the title, +2 people, +4 bit 0 a title match). A card on a new
  title is saved as a plain match, with a `#card` row in `Saves\titles.txt`; it comes back on a load if the mod is there,
  and stays a plain match if not. A Universe title with no holder gets one the first time Universe is asked: of the
  brand, in the roster, holding nothing, lowest id first (to improve: by rank). Contenders are saved in the row.
- Universe's match screen shows a title match's picture from a stack list by title; a new title has its `belt=` title's.
- To check: `sub_825C1EA0` lists holders in a 96 B heap block, which many new titles could fill; `sub_824AC5B8` reads a
  string table by title with no limit on its +296 path.
- Test aids: `SVR2011_TEST_NEW_TITLES` (the table in the log; =<id> gives the first new title that holder, in memory).

## The mod

`Mods\Titles\<folder>\`:

- `manifest.txt`:
  - `type=title`
  - `title=<key>`: one of `IC`, `US`, `WWE`, `WHC`, `WWE_TAG`, `WORLD_TAG`, `ECW`, `DIVAS`, `WOMENS`,
    `HARDCORE`, `CRUISER`, `MILLION`, `WCW` (WCW has no belt textures in the tables: name and picture only)
  - `name=<the new name>` (optional: without it the game's name stays)
  - `champion=<the holder's title>` (optional: "WWE Champion" for "WWE Championship"; without it the name
    less its "ship", or the name and "Champion") - the Universe rankings' "INTERCONTINENTAL CHAMPION"
  - `short=`, `author=`, `version=` as other mods
- `textures\`: pictures named as in texture packs, `<anything>_<hash16>.dds` or `.png`, any size. The
  game shows each in place of the stock texture with that hash:
  - the belt's textures (`evtobj` and `bgEtc` `k20xx_*`): entrances, celebrations, cut scenes and the
    belt picked up in a match. Hashes: `tools/data/belt_weapon_textures.json`.
  - the title's menu picture, `belt_<title>` in `menuHD.pac` and `menuSD.pac` (title select,
    championship screens). Hashes: `tools/data/title_pictures.json`.
- `pack.txt` (optional): `pacentry evt\evtobj.pac SOBJ <id> <file>` lines replace whole belt models
  (move packs' pac overlay, `src/move_packs.cpp`). Not made by the tools yet.

Off with the launcher's switch (a `disabled` file in the folder) or Start Without Mods. Two mods on
one title: the first by folder name wins, the other is left out (the game log says so).

## Making one

```
py tools/title_port.py titles
py tools/title_port.py make --title IC --out <dir> --name "SvR PC Championship" --picture <png>
    --set 2002_01=<png> --set 2002_02=<png> ... --svrmod
```

`titles` lists each title's belt textures and menu pictures (names, sizes). `make` writes the mod
folder and, with `--svrmod`, the file to install (launcher Mods tab, or copy the folder into
`Mods\Titles`).

## How the game does it

- Name: `src/title_mods.cpp`. Each title has a fixed list of string ids, found by text in the English
  `string.pac`; the ids are the same in every language.
  - A string that is the name alone gets the mod's name, in capitals where the game wrote it so.
  - A phrase with the name in it ("INTERCONTINENTAL CHAMPIONSHIP OPPORTUNITY") gets the title's name
    in the language in use swapped for the mod's.
  - The string lookup (`sub_82153EF8`, `menu_hooks.cpp`) asks `TitleModString` first.
- Textures: `native/texture_packs.cpp` indexes the enabled title mods' `textures\` ahead of the
  launcher's texture packs.
- Belt models: `Mods\Titles` is one of move packs' roots (`pack.txt` pacentry lines).

Test aid: `tools/dev/svrtest.ps1` places `type=title` mods in `Mods\Titles` (`--mods <folder>`).
