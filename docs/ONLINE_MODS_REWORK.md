# Online mods rework (brief, 27 Oct 2026)

Owner: the Online chat. Main merges. Work on your own branch and worktree, branched from main.

## What the user wants

When a player joins a session that uses mods:

1. The game checks which of the host's mods the joiner is missing, or has in a different version.
2. It asks: "This match uses N mods you don't have (X MB). Download them?" If the answer is No, the player doesn't join.
3. If the answer is Yes, it downloads them **directly from the host** (P2P only, no server copy and no relay for files: user's choice) into a temp folder. Then it enables them while the game loads. Show a progress bar at the bottom right, or reuse the Community Creations download screen.
4. The player then plays the match with those mods.
5. When the player leaves online mode, or closes the game, the temp folder is cleared. Also clear any leftovers at the next start, in case the game crashed.

Only allow safe files in a transfer: an **allow-list** of the game and mod file types (.pac, .bik, .mp4, .png, .dds, manifest/txt/toml, .svrmod parts...). Refuse .exe, .dll, .bat, .ps1, .iso and everything else. Check this on the host before sending, and on the joiner before writing. Also check:
- paths: no `..`, no absolute paths, nothing outside the temp folder;
- a size cap per mod and per session;
- CRCs.

## What exists today (read first)

- online_mods.cpp/.h: D15 per-lobby mod keys (folder\tversion\tCRC), id translation, refusal notes, OnlineModsMayStart. Memory: svr2011-online-mods.
- The "You have mods enabled, relaunch with Start without mods" block and the Mod Online setting. The rework replaces the block with the download offer.
- p2p.cpp: the transport (kMods = 14 message). It needs a chunked file stream with resume and back-pressure, so a big .bik doesn't stall the lockstep or the lobby pings.
- Community Creations download UI: the download screen and the CAW temp handling.
- Move packs, backstage and match types are currently reduced per lobby (331eda8, 8810d4a). Decide whether they join the download too.

## The hard part (a mod-loading rework)

Mods are found and mounted at start-up today: slots.txt, the VFS overlay, arena pages, move packs. To enable them mid-session we need:
- a second mod root, `<user data>\OnlineMods\<session>`, that the mod scan can add and remove at runtime, or a narrow "hot add" path for each kind (superstar slot, arena, entrance/media, move pack);
- slot ids for temp superstars that never collide with, or get saved over, the player's own mods (the virtual-slot or side-table work in svr2011-universe-limit may help);
- nothing from a temp mod written into saves, the CAW list, the Universe or Community Creations uploads;
- unloading cleanly on leave.

Research first: which mod kinds can be added at runtime, and which need a restart. Send Main a short plan before building. Per kind, it should say: add at runtime, or "rejoin needed".

## Rules

- Test games hidden and muted, with scripted input only; at most 4 across all chats.
- Use a private test server and --p2p_port 365xx+. Never use the live server or the VPS.
- Use a private test data copy, with no junctions to Game Files.
- Run the NDK syntax check before committing game code.
- Don't push. Main merges.

## Status (27 Oct)

- Step 2 (transport, mod_transfer.cpp) and step 3 (question, progress bar, launcher settings, Mod Maker line,
  shared allow-list svrfmt/mod_travel) are in main.
- After DOWNLOAD the mods sit checked in `<user data>\OnlineMods\<kind>\<folder>@<crc>\`, but the running
  game doesn't load them yet. That is step 4.

## Step 4 plan: load the downloaded mods (restart and rejoin)

Facts from the code:
- The mod scans read `GetExecutableFolder()/Mods/<kind>` directly in about 7 places: superstar_mods,
  online_files, online_mods, match_types, crowd_signs, backgrounds_page, mod_transfer.
- The superstar scan writes `Mods/Superstars/slots.txt` and `Mods/SuperstarOverlay`. Both are the player's
  own files, so a temp mod must never land there.
- The invite relaunch already exists (online_overlay.cpp SaveRelaunch / relaunch.bin: launch data plus the
  invite's XSESSION_INFO and inviter). The new copy rejoins through the invite walk.

Pieces, each small and testable on its own:
1. A second mod root for one run: `--online_mods` (a cvar; the folder is always `<user data>\OnlineMods`).
   - Add one `ModRoots(kind)` in online_files: the game's Mods plus the online root, for Superstars, Moves,
     Arenas and Backstage only.
   - Point the hard-coded sites at it. Cosmetic kinds stay on Mods only.
2. Superstars from the online root:
   - Slot ids come from the pool ids the player's slots.txt doesn't use, recorded in
     `OnlineMods\slots.txt`. The player's slots.txt is never written.
   - The overlay output goes to `OnlineMods\SuperstarOverlay`, mounted the way smods: is.
   - Mark these ids as temp (an `IsOnlineTempSlot(id)`).
3. Nothing temp is written into the player's data:
   - The save build hook (sub_827235D8, already hooked in superstar_threads.cpp) blanks temp slots while the
     save is built and puts them back after.
   - The CAW list, the Universe pools and Community Creations uploads leave temp ids out (the CC mod-free
     check already refuses mod uploads).
4. Restart and rejoin:
   - On "THE MODS ARE DOWNLOADED", the OK becomes "RESTART AND REJOIN".
   - Save relaunch.bin with this session as an accepted invite from its host, leave the session cleanly,
     start a new copy with `--online_mods`, and let this one end (as the invite relaunch does).
   - The new copy: invite walk, then into the lobby. The lobby's mod tables now match, so the picks work.
5. Lifetime: the online mods stay loaded for that run.
   - Leaving online shows a notice ("online mods stay until you restart the game").
   - The folder is cleared at exit and at the next start (as today).
   - A normal start (no `--online_mods`) never scans it.

Lean test (one two-game run): download, restart, rejoin, then in the lobby the fetched superstar can be
picked (no "lacks" CHAT line). Then:
- A save written in that run has no temp slot: compare its roster bytes with the save before.
- After exit, OnlineMods is gone.
