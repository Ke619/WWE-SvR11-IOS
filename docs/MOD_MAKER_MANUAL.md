# SvR2011 Mod Maker - manual

The Mod Maker makes mods for the WWE SmackDown vs. Raw 2011 PC port: new
arenas, backstage rooms, superstars, move packs, crowd signs, main menu backgrounds and media packs,
all as `.svrmod` files the launcher's Mods tab installs. It also shows what is
inside the game's files, plays the game's animations, and swaps the pictures
of your Created Superstars.

The game's own files are never changed. Mods are kept in `<game>\Mods` and
laid over the game while it runs.

## Getting around

- **Title bar:** New (a fresh project of one type), Open (a project or a
  mod), Save / Save as (the project), the recent list, and the game folder
  button on the right (where the arenas, superstars and pictures are read
  from - normally set by the launcher).
- **Sidebar:** MAKE pages each build one kind of mod: Superstar, Arena, Moves
  & entrances, Media and Match types. A page with more than one part shows
  its tabs in a row at the top (Superstar: Superstar / Attires / From another
  game; Arena: Arena / Backstage / 3D editor / From another game; ...).
  PORTING is My game discs, your other WWE games set once. SAVES edits your
  Created Superstars' pictures. LOOK pages show the game's assets, animations
  and pictures. HELP is this manual.
- **From another game:** the Superstar, Arena, Moves & entrances and Media
  pages each have a From another game tab: it brings that kind of thing from
  your other WWE games (a disc from My game discs, or one file). The old Port
  from a disc and Other games pages are these tabs now.
- **Status line:** the last message, what a long job is doing, the problem
  count, and Log, which folds the full log out.
- **Problems:** every mod page checks its work as you go. Red problems keep
  Save and Install off until fixed (hover the greyed buttons to read them);
  yellow ones are warnings you confirm once.
- **Save as mod / Install into game / Test in game:** the row at the bottom of
  every mod page. Save writes a `.svrmod` to share; Install writes the mod
  straight into the game folder; Test installs it and starts the game.
- Shortcuts: Ctrl+S saves the project, Ctrl+O opens one.

## Projects

A project (`.svrproj`) is the work on one mod page: every setting and a copy
of every file you picked, in one file you can come back to, move or send to
someone. A `.svrmod` you made earlier opens too (Open accepts both), so a mod
can always be reopened and changed - even by someone else.

Each MAKE page (and tab) is its own project; switching them switches projects. The
title bar shows which project is current, with `*` when it has unsaved
changes. Closing the app asks to save.

## Arena

A new arena on the arena select pages, made from one of the game's own.

- **Start tab:** pick the arena to start from. Double-click it or press
  **Open in the 3D editor** to edit it; **Start an empty arena** keeps only
  the ring, the floor and the ringside parts so you can build the rest;
  **Export to Blender** / **Import from Blender** go through an FBX file
  (1 Blender unit = 1 metre; textures come along as PNG).
- **Details tab:** the name shown on the select page, the author and
  version, the banner (any picture, shown at 256 x 128) and the loading
  screen (1024 x 512) shown while a match in it loads.
- **VS screen tab:** the pictures of the screen behind the two superstars
  before the match. Your arena uses the theme of the arena it plays in place
  of; replace any of its pictures.

Your arena plays in the place of the arena you started from (its room, its
VS screen style); on the select pages it sits after the game's arenas.

Limits: each arena file must stay within the room the game gives the
original; the Mod Maker shrinks the biggest textures when needed (the Size
budget panel in the editor shows how full it is). Ringside and entrance parts
move at most 50 cm because the game's collision is not rebuilt; rigged parts
(ropes, flags) can't move.

### 3D editor tab

The arena in 3D. Right drag orbits, middle drag pans, the wheel zooms, F
focuses the selection, Ctrl+Z undoes.

- **Tools:** Select (Q), Move (W), Turn (E), Scale (R); Snap rounds the
  moves. Ctrl+D duplicates, Delete hides a game object or removes one you
  added.
- **Outliner:** every object by zone (Ring, Ringside, Entrance, Free); tick
  to show or hide; type to find one.
- **Object:** position, turn and scale; the texture, or a picture of yours.
- **Add:** a box, or an object from an OBJ / FBX file (in game units, 10 cm).
- **Library:** any object from any of the 20 arenas, placed where it was or
  at the view centre.
- **Ring Kit:** each rope's colour, picture or absence, the turnbuckles and
  corner pads, the low rope's height and the gap between ropes. Fewer ropes
  change the gameplay (the game adapts: no rope breaks, no rebounds with
  none).
- **Lighting:** presets and a colour / strength, and the crowd in the seats.
- **Size budget:** file and memory against the original arena.
- **Test in game:** installs the mod and starts the game.

### Backstage tab

A backstage brawl room rebuilt. The game's seven rooms are in one file; your
mod rebuilds one and the game plays it in that room's backstage brawls. Keep
the floor and walls where they are (the game's cameras expect them); the
cars, crates and weapons are placed by the game and are not shown.

**An area of its own** instead gives the mod a row in ONE ON ONE ->
BACKSTAGE (after the room's row) and plays it only there: give it a row
name, a fight box (centre x, z and half sizes in game units), a camera
distance cap and minimum height for small areas, and optionally a gimmick
pac (the area's own cars and props, as `tools/svr08_gimmick.py` makes).

### From another game tab

An arena or a backstage room from your other WWE games: pick a disc (My game
discs) or one arena file (`pac\bg\bgNN.pac`), the arena and the slot. It opens
on the Arena tab (a room: in `Mods\Backstage`, on the Backstage tab) to finish.
Below, from another game's extracted files:

- **WWE '12 - 2K17 arena (extracted file):** a WWE '12 to 2K17 `pac\bg\bgNN.pac` (compressed or not) rebuilt
  as a 2011 arena in the place of a host arena, which gives the room, the
  ring and the aprons. Options: the host's ring / aprons / crowd, barrier
  corners, the full rope, pruning unused pictures, role lights and the stand
  light group, the canvas tone, and meshes to drop (a model over about 430 KB
  crashes the load; the report names the heavy ones). The result opens in the
  3D editor; the Arena page then takes the banner and the name. RAW IS WAR,
  SmackDown 1999, Royal Rumble 1998 and King of the Ring 1998 were made so.
- **SvR 2008 backstage:** the Parking Lot stage and its cars as 2011's parking
  room and an area of its own, with the bundled mod's settings.

## Superstar

A new playable character under the M tile of the character select (up to 137
mods).

- **Name** (up to 31 letters) and a short name for the match screens.
- **Fighting style:** decides the move-set, the entrance motions and the
  starting attributes (from one of the game's superstars underneath). Move
  the attribute sliders as you like; the moves can be changed in the game's
  CREATE A MOVE-SET.
- **Theme volume:** under the theme song, a volume in dB the game applies (left
  out at 0). Measure reads the song's loudness; Match the game sets the volume
  so it sits with the game's own themes (about -24 LUFS).
- **Entrances:** under More, the entrance by number and name - every one the
  game has and every one an installed move pack adds (the From another game tab makes them
  from other games); attires 2-4 can each have their own. A number that isn't
  installed warns (no entrance would play), and so does a model that carries
  an entrance outfit (Undertaker's, Punk's, Cena's, HBK's, Morrison's coats).
- **Moveset:** the style's moves slot by slot; pick another for any slot by
  name (each slot lists the game's moves of its kind; "all moves" lists the
  rest, a move pack's moves go by number). Saved as the mod's moves.txt.
- **Height (scale):** the superstar's size against the base's (1.00 = the
  base's own size, left out of the mod; 1.05 = 5% taller), for converted or
  older models that come out too tall or short. The preview shows the size the
  game uses; about 0.90 - 1.15 plays best.
- **Name call:** what the announcer and commentators call the superstar: one
  of the Created Superstar nicknames, or a recording of the name.
- **More:** the entrance (any superstar's, or Jeff Hardy's SvR 2010 one the
  game ships unused), an announcer name the game's sound banks have clips of
  (JEFFHARDY), the abilities, a moves list (`0xOFF=<move id>` lines over the
  style's move-set) and a move pack folder carried inside the mod.
- **Files:** the model (`ch.pac`, required - one of the game's `pac\ch\chNNN.pac`
  or a converted one), the theme song, the entrance movie (320 x 320 Bink from
  the launcher's Movies tab), up to three more attires (another pac's first
  attire each, with a name), up to four crowd signs the fans hold, a name
  recording, and the select picture (a PNG with a transparent background is
  best: the render, the bust and the face icon are made from it).
- The preview on the right shows the model playing its idle stance - a quick
  check that an imported model works. Drag to turn it.
- **Make my first superstar (Simple):** the same superstar step by step - name
  and style, look, attires, music, entrance, moves, taunts, pictures, extras,
  finish - each step one choice with a line on what it is. Whatever you skip
  comes from the fighting style, so the mod is valid from the first step. Full
  editor goes back to this page; nothing is lost either way.
  Every step from Attires on starts with two cards: the game's own / your
  files, or **From another game**. On Entrance, Moves and Taunts that is the
  porting menu in the step (a disc from My game discs, a folder of the game's
  files, or one entrance file; then the superstar - already picked when the
  Look came from that game - and Watch / choose), and "Use ... for my
  superstar" puts it in this superstar: the entrance number, its titantron
  video and its move pack, or the moves / taunts in the move-set with their
  pack. On Attires, Music, Pictures and Extras it brings that superstar over
  with the Look's converter and takes only that part (their look as attire
  2-4; their theme and video; their pictures, made from that game's renders;
  their strength, height, abilities and announcer lines).
- **Model from an FBX:** a rigged model from Blender, Unity / VRChat, Mixamo,
  Rigify, 3ds Max Biped or the UE Mannequin. Its bones are matched to the
  game's by their names (Bone mapping: change any), its meshes listed (on /
  off, kind, picture), and Make the model poses it into the game's stance,
  moves its weights onto the game's bones, packs its pictures into one sheet,
  brings it within the game's memory and builds the `ch.pac`.
- **Under the 3D view:** Reset view, Blood (off at first) and its Damage level
  - the blood a superstar gets in a match.
- **Model checks:** besides the memory budget, a model with broken points
  (not-a-number positions: the game crashes) is an error, and eyes more than
  1.5 cm from their eye bones (they pop out when the face blinks) a warning.
  These two come from robert32's SvR Port Doctor findings - thanks.

Theme songs play at full level while the game's own are about 7 dB quieter:
make yours about -24 LUFS.

### Attires tab

Attires for a superstar mod or for one of 2011's own superstars.

### From another game tab

A whole superstar or attires from your other WWE games (SvR 2007 to 2K17 and
the PS2 games): pick a disc (My game discs) or one character file (`chNNN.pac`,
a PS2 `CH.PAC`). The superstar opens on the Superstar tab to finish. Below,
from another game's extracted files:

- **Superstar file (any game):** one character file read by the disc steps.
- **WWE '13 superstar:** a WWE '13 `chNNN.pac` as a 2011 model, with a 2011
  model as the template (John Cena's by default) and an optional portrait.
  The model goes on the Superstar page.
- **SvR 2010 superstar:** a whole mod from a 2010 id - model, renders, theme
  (through ffmpeg), titantron, ratings, abilities, the move-set with the
  missing moves ported, entrance and announcer when 2011 has them. The 20
  bundled 2010 superstars were made so. Survey lists the ids.

## Moves & entrances

Moves, taunts and entrances from your other WWE games, in a few steps each:

- **Moves / Taunts / Entrance tabs:** the game (My game discs, one file or a
  folder of its files), the superstar, the moves, where they go (a superstar
  mod of yours, or a move pack of their own) and finish. An entrance becomes a
  move pack the game's Create an Entrance lists as `MOD: <name>`.
- **Move pack tab:** a move pack folder you have or made (below).
- **From another game tab:** the same converters with every option (a
  superstar's whole move-set, an entrance with its own number), and SvR 2010
  moves from its extracted files:

- **SvR 2010 moves:** chosen 2010 move ids ported into a move pack, opened on
  the Move pack tab.

### Move pack tab

Moves the game doesn't have, in a move pack: the motions (attacker, victim,
props and cameras) and the move's table records. Open a pack folder
(`pack.txt` + `motions\`), check it move by move - each move's name from the
game's table, its tracks, frames and whether it can go in a move-set - and
save it as a mod of its own (installed into `Mods\Moves`) or hand the folder
to a superstar mod (Superstar > More > Move pack folder).

`tools\svr10_moves.py` ports moves from SvR 2010 and `tools\movepack.py`
writes the folder. The game merges every enabled pack into copies of its
files when it starts (a few seconds the first time). Format: docs/MOVE_PACKS.md.

## For Fun

The whole arena or a whole superstar out to FBX and PNG, from your game into a folder of yours. Each list starts on
what's open on the Arena or Superstar page and can pick any other.

- **Full arena:** every object where the game shows it, with the ring put together and the light rigs in place, the
  default lights, and every texture as a PNG with normal maps linked. Video screens are plain white: the game fills
  them with video. It's for other programs; to edit an arena and bring it back, use Export to Blender on the Arena page.
- **Full superstar:** skeleton, skin weights, one attire, and colour and normal maps. It comes back in: on the
  Superstar page, pick the FBX as the model and Make it.

## Media

Crowd signs, menu backgrounds and the game's media, one tab each; Belts &
weapons brings title belts' and weapons' looks from another game (a texture
pack); Titles replaces a championship; CAW glasses adds your own glasses to Create
A Superstar.

### CAW glasses tab

Your own glasses from an OBJ model and a picture: give them a name, pick both, then
Install or Save. Glasses only for now. The model's first object is the frame and a second
one the lenses; it's sized to the stock glasses' width and put in front of the eyes, facing the way
Blender's front view does. They come last in Create A Superstar's Glasses list. They're fixed to the
head, so they don't follow the face's shape sliders and can sit wrong on a heavily reshaped face. A
Created Superstar wearing them shows the first stock glasses on a game without the mod. The page runs the
Mod Maker tools (Python): Install what's missing fetches them.

### Titles tab

One of the game's 13 titles with a new name, new pictures and new belt
textures. Pick the title, then fill in only what should change: the name (and
what its holder is called), the championship art beside the holder, the belt
picture, and any of the belt's textures. Holders and title matches stay as
they are, and without the mod the game's title comes back. Two mods on one
title: the first by folder name wins, and the doctor warns.

### Crowd signs tab

Pictures the crowd holds up in every match, with the game's own. Add any
pictures: each is fitted onto a white 128 x 64 board. A superstar's own
signs (held by that superstar's fans) go on the Superstar page instead.

### Menu backgrounds tab

Pictures behind the main menu, after the game's own 12 (MY WWE -> OPTIONS ->
BACKGROUNDS turns each on or off in the game). Add any pictures: each is
fitted to the screen without squashing (the edges that don't fit are cut),
and the preview shows how the main menu will show it. Give each a name (the
game's BACKGROUNDS page shows it). "Show the game's logo" puts the game's
"SmackDown vs RAW 2011" logo over them; leave it off for fan art with its own
logo. One picture, or a pack of many. (The launcher's Mods tab can also turn
a single picture into a background: Add background picture.)

### Titantron videos, Menus & renders and Audio tabs

One pack that replaces the game's own media; every item is optional (the
three tabs fill the same pack).

- **Titantron videos:** a superstar's entrance video (320 x 320 Bink), and
  an arena's screen pictures between entrances (10 frames from a video, or
  pictures shown in turn).
- **Menus & renders:** a superstar's render, bust and face icon from one
  picture.
- **Audio:** a superstar's entrance theme, the menu music, and any game sound
  by its event name (without `Play_`; e.g. `SVR10_Chant_Sena_001`).

### Belts & weapons tab

Title belts' and weapons' looks from another game's disc, as a texture pack.

## Match types

Match type mods (`type=matchtype`, a manifest only). Two kinds:

- **Port match type (switch):** the switch for one of the match types the
  port adds (Falls Count Anywhere, Championship Scramble, Lumberjack, ...);
  the player turns it on or off in the launcher's Mods tab.
- **Custom match type:** one of the game's matches as the base (picked by
  name from the 119 rules), under a new row in a menu list (and submenu),
  with its own people, slots (index, team, kind), arena, option rules with
  locks, placed weapons and Match Creator rows. The game doesn't play
  custom match types yet: the page saves them in the format a later update
  will read, and warns until then.
- **The game's matches:** every rule record decoded - participants, arena,
  option rules - as reference, with "Use as the base".

## My game discs

Your other WWE games, set once: a disc image or an extracted folder per game
(Find my discs looks through a folder). Every From another game tab and the
Moves & entrances tabs read them; a game's row says what it can bring and,
for a partial folder, which files are missing.

The WWE '12 - 2K17 arena conversion is built in. The other conversions run the port's Python
tools from the `Mod Maker Tools` folder next to the Mod Maker and need Python
3 with numpy and Pillow (and ffmpeg for the 2010 themes); the tab says what
is missing (and Install what's missing gets it). Only the other games' extracted files are read.

Also built in: **From a video...** on the Superstar and Media pages makes a
320 x 320 Bink entrance movie from any video or picture; the 3D editor's
**Pictures from a folder...** replaces every arena texture that has a picture
of the same name in the folder; the CAW pictures page's **Import a .cas...**
brings someone else's Created Superstar into a free slot.

## CAW pictures

The picture the game shows for a Created Superstar in its lists (the one
FINALIZE made) and the head the Community Creations preview uses, per attire.
Replace either from a picture of yours (a transparent background fits the
lists best). The change goes into the save in the game's Saves folder with its
checksums redone; a `.bak` copy of the save is kept the first time. Close the
game first.

## Game assets

Everything in the game's pac files, read-only. Expand a folder, a file, a
group and an entry; nothing is read until you look at it. Each thing shows as
what it is: textures (with zoom), texture bundles, models (meshes, nodes,
textures), motion banks (every motion, with the move's name), string tables
(every text by id), roster records, text, or bytes. Export writes any of them
out (textures as PNG too).

## Animations

A superstar playing the game's motions. Pick who (a superstar, or any
`ch.pac`), optionally a dummy for the other person, then:

- **Watch here:** the menu motions - stances, taunts, the entrance's ring
  part, finisher previews, abilities - play in the view. Play / pause, step,
  scrub, speed, loop; the camera turns (left drag), tilts (right drag), pans
  (middle drag) and zooms (wheel).
- **Match moves:** pick one by name (grapples, strikes, finishers ...).
  **Watch here** plays it in the view - the superstar does it, the dummy
  takes it - from the game's own motion banks (m.pac, the 2,800 moves the
  game ships). **Play in game** starts a test match instead - the superstar
  against the dummy, both computer-controlled - where the move is done every
  few seconds; that is the way to see a move pack's moves. Close the game
  when done.

## Icons & renders

The game's pictures by what they are for: each superstar's renders and face
icon, the arena banners, VS screen themes and loading screens, and the
crowd's signs. Export any as PNG; the pages that replace them are named.

## Installing and sharing

A `.svrmod` is a zip (store it, send it, put it on the Project Index). The
launcher's Mods tab installs it (+), lists it with a tick to turn it on or
off, and removes it (-). Mods also install from the Mod Maker directly
(Install into game). The game picks mods up when it starts.

Formats: docs/SVRMOD_FORMAT.md. The game side of every mod type:
port/docs/SUPERSTAR_MODS.md, BACKSTAGE_MODS.md, MEDIA_MODS.md,
CROWD_SIGNS.md, MOVE_PACKS.md, docs/ARENA_MOD_MAKER_PLAN.md.
