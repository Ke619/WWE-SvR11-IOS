# Match recordings

Every match writes a folder of its own:

```
logs\matches\<YYYYMMDD-HHMMSS>_<people>\
```

The folder sits next to the log file, the same place `matchbox_N.txt` goes. `<people>` is the wrestlers' names
joined by `_vs_`, using letters, digits and `-` only, and cut at 80 characters. The newest
`match_recordings` folders are kept (cvar, group Logs: 20 by default, 0 turns recordings off). Older ones are
deleted when a new match starts.

The recording starts at the bell: the first world update after the clock resets, with a referee in the ring, after
a match's load (sub_823EEB98). The menus' attract matches never load that way (one of them has a referee), so they
are not recorded. The files are written while the match runs, so a crash
leaves whatever was written up to that point. `done.txt` is written last, once the match's end task starts.

Code: `port/src/match_record.cpp` (`match_record.h`).

## Files

| File | What it holds |
|---|---|
| `match.json` | What the match was: enough to set it up again (below). `complete:false` until the end. |
| `inputs.bin` | Every pad's state as the game read it, one record each time a pad changes. |
| `bell.bin` | The state at the bell for exact replays: each character's first 8 KB, the random table and its indexes (about 40-80 KB). |
| `box.txt` | The black box (`docs/LOG_DIAGNOSTICS.md`): each case as it is logged, then the full box at the end. |
| `trail.txt` | Once a second of the match clock (60 world updates) from the bell: each person's position and motion. |
| `loaded.txt` | At the bell: fallback memory, weapon slots (slot:model), the random table counters. At the end: fallback memory and its peak. |
| `mods.txt` | All installed mods, in load order, with on/off, used in this match, and a files hash. |
| `done.txt` | Empty. Written last: the folder is complete. |

A normal match comes to a few KB in total, well under 1 MB. In a 30 s CPU-vs-CPU test the folder was 8 KB,
with `inputs.bin` at 192 bytes.

## match.json

```json
{
  "format": 1,
  "complete": true,
  "game": {"version": "2.1.5", "commit": "87786a66 2026-10-25"},
  "start": "2026-10-26T18:47:03",
  "length_s": 34,
  "result": "ended",
  "rule": {"id": 0, "name": "SINGLE"},
  "arena": {"id": 1, "name": "RAW", "mod": null},
  "people": [
    {"index": 0, "person": 0, "name": "JOHN CENA", "superstar": 57, "cpu": false, "attire": 2, "port": 0, "mod": null,
     "caw": null}
  ],
  "frame_rate": 60,
  "online": false,
  "entrances": true,
  "renderer": "any",
  "cpu_tab": {"reversal_chance": -1, "reversal_rates": -1, "damage_level": -1, "pin_level": -1, "momentum_level": -1},
  "weapons": {"picks": "mod:light_tube:3,4:1"},
  "inputs": {"file": "inputs.bin", "version": 1},
  "frames": 1769,
  "seeds": {"rand": "AC303B9C", "rand_sites": {"82302C40": 578, "82C2E2F4+": 85}, "table_load": [0, 0, 0, 0],
            "table_bell": [0, 0, 1, 60], "table_steps": [[], [87, 117, 147], [], [380, 385, 390]], "frames_load_to_bell": 857}
}
```

- `result` is one of:
  - `running`: the match is still going, or the game was closed during it;
  - `ended`: the match's end task started;
  - `crash`: the crash report was written while the match ran.
- `people` lists every character in the match, the referee (`person` 6) and the announcers included.
- `cpu` is true when the CPU plays the wrestler, that is, when no local player's pad plays them (the character's
  +1152 is not a connected pad; the select screen's CPU picks have 6). The pick slot's controller byte (-8) is not
  used: the select screen's own CPU picks leave it 0.
- `attire` is the picked attire, 1-4.
- `port` is the controller (0-3) of the local player who plays the wrestler, or `null` for the CPU.
- `cpu`, `attire` and `port` are `null` for the referee and the announcers.
- `frames` is the number of frames from the bell to the last write (the end, or the crash).
- `entrances` is the live rules' ENTRANCES (+38) at the bell. A replay sets them the same (the pre-bell part
  differs with and without them).
- `mod` is `null` for stock content. For a mod it is `{type, id, version, files}`, where `files` is the hash used
  in mods.txt.
- Rule and arena names are the game's own strings, in the game's language.
  - The rule's name comes from its rule record (+56, a string.pac string id).
  - A stock arena's name is string 0x9C42 + its id.
  - A mod arena uses the name from its manifest.
  - The fallbacks are `rule XX` and `arena N`. `port/server/match_names.py` has English names, tidied in places,
    for the dashboard.
- `cpu_tab` gives -1 for anything left at the game's own setting.

The launcher's report form (HCTP) and the server (Extras) read these keys. Tell both before renaming a key or
changing a separator.

## mods.txt

One line per mod, separated by tabs:

```
Type	id	version	on|off	used|-	files	name
```

- `files` is an FNV-1a 64-bit hash (16 hex digits). It covers the mod folder's files: the lower-case relative
  path with `/` separators, the size and the modification time, taken in path order.
  - Dot files are skipped.
  - Generated folders are skipped: `disabled`, ArenaOverlay, ArenaCrowdless, PacOverlay and ArenaTest.
  - It is a quick check for "the same mod files", not a content hash. The launcher computes its own SHA-256
    for uploads.
- `used` marks the match's superstar mods, its arena mod, and the weapon picks (`mod:<folder>`).

The list is built on a thread of its own, so a large Mods folder does not hold up the bell.

## inputs.bin (version 1)

All values are little-endian.

- Head, 16 bytes: `"SVRI"`, u32 version (1), u32 the frame at the bell (counted from the game's start), u32 0.
- Records, 22 bytes each:
  - u32: frames since the bell. A frame runs one world update, or two at 30 fps or on a slow PC;
  - u8: the user (0-7), or 0xFF for a frame's world updates (version 1);
  - for a pad: u8 0 if the pad is connected, 1 if not, then 16 bytes, the XINPUT_STATE as the game got it (guest
    byte order);
  - for 0xFF: u8 0, then the number of world updates this frame ran (1, or 2 at 30 fps or on a slow frame; 0 while
    the free camera holds the match), then 15 bytes of 0. Written when the number changes; a recording starts at 1.

  The packet number (the state's first 4 bytes) is written as 0. It goes up on every read, not only when the
  pad changes.

A record is written only when a pad's state changes. Every user gets one record at the bell. Extra world updates
at 30 fps or on slow frames reuse the cached state, so they add nothing.

The file is flushed once a second of the match clock, with trail.txt. A game that hangs and is killed keeps its
pads up to that second (before 27 Oct it was empty until the end).

## bell.bin

All values are little-endian.

- Head: `"SVRB"`, u32 version (1), u32 block count.
- Each block: u32 kind, u32 index, u32 person, u32 size, then the bytes, which are guest memory as it was (big-endian
  inside).
  - Kind 1: a character. The index is its slot in the character table (0x82E3CC50), the person is +1156, and the
    bytes are its first 8 KB.
  - Kind 2: the random table (0x82D9DB10, 256 bytes).
  - Kind 3: its four draw indexes (0x82E3689C).
  - Kind 4: the seed table's two counters (0x82EC6A2C).

It is written only for real matches, at the same bell the recording starts at. Only a replay reads it.

When a replay's bell finds every wrestler and the referee in the recorded motions, it writes bell.bin back. If they
are in other motions, the bell came from another pre-bell intro, so nothing is written and the tool tries again.

- The data blocks are written back as they are.
- A character is written back only where its slot holds the same person. Only the words that differ are written,
  and only if one of these holds:
  - the word is a float on both sides (0, or 1e-9 to 1e7, either sign);
  - it is one of the frame counters in `kBellCounters`.
- A word that is or was a guest pointer (4-aligned and pointing into readable guest memory) is never written.
- Handles and indexes stay as this run has them. Writing every differing word crashed a replay.
- The log line `replay: bell.bin written back` says how many words were written and how many were left, per
  person.

## seeds

What an exact replay will need later (see Replay below). The replay tool uses `rand` only.

- `rand`: the match's rand() seed. From the match's load, every rand() call site draws from a stream of its own,
  seeded from this seed and the site, so the order threads call in doesn't change the draws. The streams start
  again from the seed at the bell.
- `rand_sites`: draws per call site since the bell. A `+` marks calls from a thread other than the logic thread
  (those sites have a second stream of their own).
- `table_load` and `table_bell`: the game's random table's four draw indexes (0x82E3689C) at the match's load and
  at the bell. Inlined draws all over the game advance them.
- `table_steps`: for each index, the frames after the bell of its first eight steps. Index 1 steps on a timer
  (about every 30 frames) that starts before the bell, so its phase shows here.
- `frames_load_to_bell`: frames from the match's load to the bell. The people load at disk speed, so this differs
  from run to run, and so does the pre-bell intro (a sneak attack or standing).

## The crash report

When the game crashes during a match, the crash report gets this line:

```
match recording: <folder> (unfinished: no done.txt)
```

match.json is rewritten with `result: "crash"`. The black box goes into the crash report as before.

## Replay

```
port\tools\replay_match.ps1 -Recording <a recording's folder> -GameDir <game folder> [-UserData <test saves>]
```

The tool reads match.json and starts a hidden test game that goes through the scripted route to the same match:
- the same wrestlers (CAWs by save slot), attires, controllers, rule and arena (`SVR2011_TEST_MATCH`,
  `SVR2011_TEST_RULE`);
- the same rand() seed (`SVR2011_TEST_RAND_SEED`);
- the entrances as match.json's `entrances` has them (`-Entrances auto`, the default). Recordings from before
  27 Oct have no `entrances`: off. `-Entrances 0|1|keep` overrides it (`keep`: the game's setting).

Before it starts the game, the tool checks that the recording's mods (a superstar's or the arena's `mod`) have a
manifest with that id in the game folder's Mods. A missing mod is a clean error. Without this check the mod's
superstar id was no roster wrestler and the match load hung. Another version only gets a note.

Each try gets its own log (`runs\replay_<name>.tryN.log`). A stopped game can hold its log for seconds, and one
shared log gave the next try the last game's lines. The test-game guard runs before every try.

`SVR2011_REPLAY=<folder>` then takes over at the bell:
- Every frame gets the recording's pads (users 0-7, as recorded; a user with no record yet keeps its own).
- It gets the recorded number of world updates, so 30 fps and slow frames come out as they were.
- After the recording's `frames` the pads are the players' again.

The replay records itself like any match. Its folder gets `replay.txt`, and the tool adds the frame shots:
- `replay.txt` heads with three lines:
  - the second where the replay parts from the recording's `trail.txt` (a wrestler 2.0 or more from the recorded
    place);
  - the first second anyone parts by 0.05 or more, the referee and announcers included (the referee parted
    seconds before the wrestlers in a 27 Oct replay);
  - how the two ended: the replay's match ended at frame N (and the recording's at M), or it was still on when
    the recording's frames ran out.

  Below that, second by second, the farthest anyone is (`*`: a wrestler's motion differs).
- The frame shots are `shot_NN.png` (`-Shots`, one every `-ShotEvery` seconds of the match).

**Approximate.** The player's side is replayed: the pads, the setup and the rand() seed. From format 1 with
bell.bin, the people's state at the bell is replayed too. Measured on 26 Oct: exact for the first 27 world updates,
then 0.003 apart at update 28. The next differences are values that are the same in every character: +3040 at
clock 3 (one float step) and the block +2800-2952 at clock 5. They come from global state outside the characters,
which still has to be found.

- **The pre-bell intro:** it is a sneak attack or a standing start, with or without entrances. It comes out either
  way, run to run, because the people load at disk pace. When the bell finds other motions than the recording's,
  the tool stops the game and starts again (`-Tries`, 4). The last try is replayed all the same, approximate from
  the start (before 27 Oct no replay came out). On a busy disk one intro can come out every time (27 Oct: 6 tries
  in a row).
- **The CPU's first moves:** they part within a second or two. The game's random table steps on a timer that
  started before the bell, and bell.bin writes back the indexes but not the timer's phase. On 27 Oct index 1
  stepped at 92/122/152 frames in the recording and 87/117/147 in its replay.
- **Measured 27 Oct** (full CPU matches of 4 and 3.5 minutes, pinfalls): the replays part at second 1 and end
  differently in time (the same winner and finish, at 43 s instead of 241 s, and 98 s instead of 201 s). A
  player-vs-CPU replay: the wrestlers were exact for 5 s; the referee parted first (0.1 at second 2).

Test aids for replays and replay research:

| Switch | What it does |
|---|---|
| `SVR2011_TEST_SEED_LOG` | Logs each time the random table is seeded. |
| `SVR2011_TEST_RAND_LOG` | Logs who calls rand(), every 10 s. |
| `SVR2011_TEST_RAND_SEED=<n>` | Uses this rand() seed for every match. |
| `SVR2011_TEST_ENTRANCES=0\|1` | Turns the match's entrances off or on (live rules +38). |
| `SVR2011_TEST_MATCH=... ports=0,-1` | Each person's controller: a player's 0-3, or -1 for the CPU. A CPU (and `cpu=all`) also gets pad bits 6 (slot -4), as the select screen's CPU picks have. Before 27 Oct person 0 stayed on pad 0: an idle player in "CPU vs CPU" tests. |
| `SVR2011_TEST_MATCH=... exact=1` | Only the listed people: empties a later slot that the route's presses picked. |
| `SVR2011_TEST_PRE_BELL_SLOW_MS=<ms>` | Makes each frame from a match's load to its bell this much longer. |
| `SVR2011_TEST_REPLAY_DUMP=<file>` | For the first 40 clock values of a recorded match, writes persons 0, 1 and the referee's first 8 KB in hex, one line each (diff two runs). |
| `SVR2011_TEST_TRAIL=<file>` | Writes a line per frame: the clock (`pre N` before the bell), the random table's indexes, then each person's position, motion and motion frame. |

## Testing

`SVR2011_TEST_BOX=crash,600` makes the game fault 10 s after the bell. The folder is left without done.txt,
match.json says `"result": "crash"`, and the crash report names the folder. This test aid now counts from the
bell, not from the entrances.
