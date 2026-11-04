# Title mods

A title mod replaces one of the game's championships: its name, its belt's textures and its picture
in the menus. It changes no ids, so saves, Universe holders and online play are untouched (title
mods are cosmetic: they don't count as mods for online matches).

Adding new titles beyond the game's 13 is step 2 (not in yet).

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
