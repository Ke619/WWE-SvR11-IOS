# CAW parts - research (step 1)

Status (2026-10-26): research only, nothing built. Goal: Create-a-Superstar items (masks, clothing, ...) made from an
OBJ/FBX and added by a mod, next to the game's own. Sources: `Game Files\pac\edit` (read-only), hidden CAW editor runs
with the debug build. The survey scripts are throw-away; the facts are below.

## Where parts live

- `pac\edit\edNN.pac`: one EPAC per part category, NN = the category number. Its PACH entries:
  - `Mnnn`: a part with a model. `Tnnn`: a texture-only part (face paint, tattoos, make-up: ed06-08, 12/13/15,
    37-43, 58-63).
  - Inside each entry (PACH child ids):
    - `10xx`: colour texture (colour-mapped TGA, see Textures). Some parts have two (`1025` + `1026`).
    - `11xx`: normal map (colour-mapped TGA, flat-normal palette). `14xx`: a small extra map on a few parts.
    - `20xx`: the JBOY model, xx = the slot type. `21xx`: the same mesh bound to the other skeleton
      (bone palette 37 instead of 79 on ed44, same vertices). `22xx`: per-vertex table
      (`ffffffff`, u32 count = vertex count, then count x 16 B `0, 0, index, cccccccc`; looks like a weld/seam map).
    - `23xx`: `;CAS` sway text (bone + spring values); `2233` = hair chain data.
- `pac\edit\edit.pac`, EPAC group `EDIT`:
  - `MENU`: PACH child 0 holds 116 children, one per category id: the editor's part lists (below).
  - `EXSS` (12 preset tables), `SSTL` (Superstar Threads, not parts), `SSCP` / `MOT` (editor poses), `IK`
    (15 x 1648 B), `TEX` (shared textures), `FACE`.
- The editor looks `/EDIT/MOT`, `MENU`, `EXSS`, `FACE`, `TEX` up through the resolver sub_826B87B0
  (seen with `SVR2011_TEST_VFS_TRACE`). The edNN pacs are read by index, not through the resolver.

## MENU records

Each category child: a 12-byte header, then `total` records of 60 bytes (little-endian).

Header: `u16 total`, `u16 male`, `u16 female` (or `total, total, 0` when both share it), then flags.

| Offset | Size | Meaning |
|---|---|---|
| +0 | 4 x u8 | **colour band ends** (palette index, ascending, 0 = unused): `ff` = one band 0-255; `7f ff` = 0-127 / 128-255; `3f 7f bf ff` = four bands |
| +4 | 4 x u8 | the same again (all 3082 records have +4 = +0) |
| +8 | 4 x u8 | small flags (layer / draw order; not decoded) |
| +12 | u16 | model index -> `Mnnn` (`ffff` = none) |
| +14 | u16 | texture index -> `Tnnn` |
| +16 | u32 | `ffffffff` |
| +20 | u8 | **colour slots** the editor offers; **0 = fixed colours** (the palette is shown as stored) |
| +21 | u8 | the same again (23 of 3082 records differ) |
| +23 | u8 | 1 / 2 (gender list the record belongs to) |
| +24..+31 | | per-category extras (e.g. `02 02`, `1e 1e`: default shine / pattern values, not decoded) |
| +32 | u16 | style group (records sharing a shape) |
| +40.. | | hair sway values (hair categories only) |

The record index is what a CAW save stores, so new parts must be **appended** (never inserted).

## Textures and tinting

Part textures are colour-mapped TGAs: 18-byte header, 256 x 32-bit BGRA palette, 8-bit indices.

- **Tintable part:** the palette is grey. The index image is split into the record's **bands** (MENU +0): each band
  is one colour region, and its grey ramp is the shading inside that region. The editor colour for a region
  multiplies that band's greys. Checked on ed16 / ed19 / ed31 / ed33: the bands match the used index ranges
  (e.g. ed31 record 4, `3f 7f bf ff`: 19.6 % / 52 % / 10.8 % / 17.5 % of the pixels, each band its own grey ramp).
- **Fixed-colour part:** MENU +20 = 0 and a real colour palette (e.g. all coloured parts in ed27, ed22 rec 8).
- Not the region source (all checked): mesh count, palette alpha, vertex colours (all `ffffffff`), `22xx`, `21xx`.
- At run time the editor builds its own textures (the dumps show unnamed DXT textures, `tex_1024x1024_dxt1_*`),
  so the tint is baked on the CPU, not done in the shader. The shader parameters stay 1.0 (`g_f4MatDifCol`).

What this means for a converter:
- A model's own texture -> quantise to 256 colours, MENU +20 = 0: shows as made, not recolourable.
- A recolourable part -> greyscale, 1-4 bands with band ends in MENU +0, MENU +20 = band count.

## Models

- JBOY, one or more meshes, shader `yBumpMap` / `yBumpMapSS` / `yReflect` / `yDefault`.
- **Parts without their own nodes** (`nodes 0`, e.g. ed05, ed09, ed18 glasses, ed25-28, ed31-35, ed44): the mesh bone
  palette points straight into the shared CAW skeleton (ed44: palette `[79]`; pants: `[25,24,1,15,14,13,4,3,2]`),
  so they can still bend with several bones. These are the easy kind to convert.
- **Parts with their own node tree** (35-115 nodes: ed04 faces, ed10 hair, ed16 hats, ed19 horns, ed20 other headwear, ed14, ed24, ed45, ed48):
  they carry a face / hair rig. Converting these is the skinned-clothing step.
- Coordinates: y points down; head about -8.5, boots +5..9.6 (game units).

## Thumbnails

`pac\edit\casThumb.pac`, EPAC group `CAST`: one entry per category (`"%04X"` of the category id), a PACH with one
DDS per MENU record, in record order (256 x 128 DXT1, 16512 bytes). A new part needs its thumbnail appended there
too (an overlay, like MENU).

## Categories

The category id is the same number everywhere: MENU child, `edNN.pac`, casThumb `"%04X"`. Names from the thumbnails
and a hidden editor tour (svrtest `--dbg_boot=caw`, scripted presses). `m / f` = the header's men's / women's counts.

Editor menus: HEAD (Templates, Skin, Hair, Face, Facial Hair, Makeup, Face Paint, Tattoos); BODY (Height, Size,
Shape, Body Hair, Scars, Tattoos); CLOTHING (Templates, Headwear, Upper Body, Lower Body); the last tab (attires,
crowd signs, pose, abilities). CLOTHING -> HEADWEAR: Wrestling Masks (Masks, Mask Designs), Piercings, Glasses, Hats,
Horns, Other, Edit Layers.

| id | m / f | model | nodes | what |
|---|---|---|---|---|
| 02 | 11 / 4 | yes | 0 | bodies (body templates) |
| 04 | 36 / 27 | yes | 113 | faces (heads: eyelashes, teeth, blood) |
| 05 | 40 / 40 | yes | 0 | eyes |
| 06, 07, 08 | | texture | - | eyebrows, eyelashes, lips |
| 09 | 14 / 14 | yes | 0 | teeth |
| 10 | 102 / 102 | yes | 73 | hair |
| 11 | 0 / 13 | yes | 0 | women's tops |
| 12, 13, 15 | | texture | - | sideburns, moustaches, beards |
| 14 | 23 / 23 | yes | 42 | chin beards |
| 16 | 68 / 68 | yes | 37 | **hats** |
| 17 | 31 / 31 | **texture** | - | **wrestling masks** (a head texture layer) |
| 18 | 42 / 42 | yes | 0 | **glasses** (rigid, 2 meshes) |
| 19 | 17 / 17 | yes | 113 | **horns** |
| 20 | 53 / 53 | yes | 37 | **other headwear** (eye patch, gas mask) |
| 21, 22, 24, 25 | | yes | 0 / 115 | shirts, T-shirts, jackets, singlets |
| 26 | 15 / 15 | yes | 0 | elbow pads |
| 27 | 29 / 29 | yes | 0 | wristbands, watches |
| 28 | 33 / 33 | yes | 0 | gloves |
| 29, 30, 31, 32 | | yes | 0 | trunks, tights, shorts, pants |
| 33 | 14 / 14 | yes | 0 | knee pads |
| 34, 35 | | yes | 0 | socks / shoes, boots |
| 36-43, 58-61, 63 | | texture | - | scars, face marks, eye make-up, face paint, tattoos, body hair |
| 44, 47 | 59 / 59, 3 / 1 | yes | 0 | **piercings** (ears; face) |
| 45 | 41 / 38 | yes | 37 | necklaces, ties |
| 48 | 28 / 28 | yes | 35 | belts |
| 49, 50, 51, 69 | | (Paint Tool) | - | logos, shapes, flags, patterns |
| 62 | 74 / 74 | texture | - | mask designs (over a mask) |

**Wrestling masks are not models.** A mask record (17) has no model (`ffff`): it is a texture drawn over the head,
like face paint, with the mask designs (62) on top. A mask from an OBJ / FBX would have to be baked into the head's
texture layout. The model categories for the "rigid part" prototype are glasses (18, no own nodes) and piercings (44);
hats (16), horns (19) and other headwear (20, the gas mask) carry their own head rig.

## Making a part (`tools/caw_part.py`, `src/caw_parts.cpp`)

`Mods/CawParts/<id>/`: `manifest.txt` (`type=cawpart`, `part=<category> <m|f|mf> <stem>`), `<stem>.m.pach`, `.t.pach`, `.thumb.dds`, `.rec`.

    python caw_part.py make --game-pac <pac> --obj part.obj --png part.png --out <mod folder> --id <id> --name "<text>"
    python caw_part.py export --game-pac <pac> --out <folder>      (the template as OBJ + PNG, in the game's axes)

An OBJ is taken as Blender writes it (y up, the front towards +z); `--no-fit` keeps the game's own numbers (y down, the front towards -z).
Glasses (18) only so far. Piercings (44) list with their thumbnail but don't draw (one mesh, not looked into): the tool refuses them.

What the game needs of a part, each found by a crash or a part that didn't show:

- **Settings rows:** a second table per category, 28-byte rows by texture number (`sub_828DDC98` / `sub_828DDCC8`), as long as the stock textures.
  A mod part's number is past it, so it gets its own row, a copy of the row of the texture its `.rec` names at +14 (the template).
- **Colour slots:** a glasses `.rec` needs its +20; with 0 the part doesn't draw.
- **Entries are BPE-packed:** the model's and the colour texture's entries go through the game's decoder (`sub_826AEF10`).
- **Two meshes:** the game walks a model's meshes past the last (`sub_82700FB0`), so the template's count is kept: an unused mesh is one tiny triangle.
- **The model's third entry (22xx):** for each editor vertex (20xx), the in-game model's (21xx) (mesh, 0, vertex) that places it, once skinned (`sub_828F1050`).
  Stock: exact to 1e-6. With one vertex order in both models: (mesh, 0, i).
- **Bones:** the in-game model's weight blocks aren't plain weights (a vertex's add up to anything from 0 to 2.8), and copying them to other vertices threw
  the part off the head. A part is rigid: one bone at 1.0, the in-game template's main one (33; the editor's 79 isn't in a match, and the part vanishes).
  So a part doesn't follow the face's shape sliders the way stock glasses do.

**A saved CAW keeps the part's numbers:** with no overlay (mod and index gone, another machine) `/ED18/T054` was in no pac and the game crashed. A name no pac answers now gets the category's first part
(`CawPartsMissing`), and a record past the list the first record (`sub_828DDC00`, in caw_logos.cpp).

## Open

- Other categories: hats, horns and hair have their own rig and chains (`SIMPLE` in the tool).
- Online: a CAW wearing a mod part on a game without it falls back as above; not run.
- Android: the app's Mods page adds the .svrmod (A51: listed and drawn in the editor). The Mod Maker page is CAW glasses (Media tab).
- Multi-texture parts (two `10xx`, e.g. ed33 knee pads): which band set goes with which.
