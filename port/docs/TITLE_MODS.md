# Title mods

A title mod replaces one of the game's championships: its name, its belt's textures and its picture
in the menus. It changes no ids, so saves, Universe holders and online play are untouched (title
mods are cosmetic: they don't count as mods for online matches).

## New titles (in progress)

A mod with `new=1` adds a title beyond the game's own instead of replacing one: `type=title`, `new=1`,
`name=`, `champion=` and `kind=tag` for a tag title. Up to 32. So far (piece 1) the game knows it and keeps its
holders, but no screen offers it yet.

- The game's table is at 0x82EDEB50, 14 rows x 16 B: holder a, holder b, +8 a group, +12 kind (2 = tag), +13
  display order. Its rows, by index: 0 and 1 the two world titles (holders 139, 123), 2 and 3 tag, 4 IC ...
- A new title's index is 14 + its place in folder order, for that run only: nothing stores the index.
- Holders are in `Saves	itles.txt`, one row per mod folder: holder a, holder b, each an id, `m:<mod folder>` or
  `-`. The save itself is never changed, so removing the mod takes only its holders. The row stays while the mod
  is off or gone, and the title comes back with its holders if the mod returns.
- Hooks: `src/title_new.cpp`, the title module's functions in 828BD488-828BDEF8 plus 828C22E0 and 8243B0A8. Each is
  the game's own for indices 0-13.
- Universe's per-title store (`sub_825B6BE0`, P+20776) is 8 rows of 4 B, and only for the titles in the list at
  0x8206D6BC (`sub_828BDA98`). It returns a pointer, so a new title can have its row in a side buffer.

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
