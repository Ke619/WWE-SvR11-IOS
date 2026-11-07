# Victory scenes: what a superstar does after winning

Research and the `victory=` key (26 Oct 2026). Game side: `src/superstar_mods.cpp` (hook on sub_8275DEE0).
Mod Maker: Superstar page, More: entrance ... -> **Victory** with **Preview** (the Full preview).

## 1. Where they are [C]

A victory scene is a scene script like an entrance (an EVP script and its EVT track bank, the same format,
docs/HCTP_ENTRANCES.md), in `pac\evt\Winning*.pac`:

| pac | groups | scenes |
|---|---|---|
| Winning0 | EVPA / EVTA | **6000 + id**: the superstars' own (26: 6100 The Rock, 6101 Steve Austin, 6102 Triple H, 6103 Undertaker, 6104 Jericho, 6107 Kane, 6110 Edge, 6112 Matt Hardy, 6123 Rey Mysterio, 6125 Big Show, 6131 CM Punk, 6135 MVP, 6139 John Cena, 6145 Shawn Michaels, 6160 Batista, 6161 Randy Orton, 6175 John Morrison, 6176 Kofi Kingston, 6179 Mark Henry, 6219 Zack Ryder, 6262 Kozlov, 6272 R-Truth, 6277 Goldust, 6279 Sheamus), 6700 DX, 6702 Brothers of Destruction |
| WinningA | EVPP / EVTP | 6050 Chris Masters |
| Winning1 | EVPB / EVTB | generic: 7000-7015 singles (with the referee, loser in / out of the ring, outside), 7100-7107 divas, 7150-7158 with a manager, 7200-7208 tag, 7300-7301 divas tag, 7400-7404 mixed tag, 7500-7605 trios |
| Winning2 | EVPC / EVTC | match types: 8000 Royal Rumble, 8020 / 8030 Chamber, 8040 First Blood, 8050-8062 Hell in a Cell, 8100+ Ladder ... |
| Winning3 | EVPD / EVTD | titles: 9000-9005 singles, 9100 tag, 9200 women's, 9400 Championship Scramble |

The game looks a script up by number in every group EVP0..EVPZ (sub_82751C60 formats `/EVP%c/%d`), so a
move pack's `pacentry <pac> EVPx <n> <file>` adds a scene of any number.

## 2. How the game picks one [C/L]

- After the match the scene's number sits in the entrance queue (`*(0x82E3DDFC) + 5872`, the queue entrances
  use) at **+416**; sub_8275DEE0 (called by sub_8275E1F8) opens `/EVPx/<n>` and starts it. sub_82753C28 tells a
  victory scene by its number (6000-9999).
- [C] John Cena (139) winning a singles match: 6139. [L] a superstar without a scene of his own gets a generic
  one; title and match-type wins use 8000+ / 9000+.
- Not traced: where +416 is written (26 Oct: not in the 65 callers of the queue getter sub_82573EF8, not stored
  at *(0x82E3DDFC) + 6288 directly; the 6288 stores in sub_824324C0 / sub_82431A40 are another object). In the test matches (SVR2011_TEST_LOSE, an attract match before) Matt
  Striker (149) winning also got 6139 - probably a stale value from the attract match's Cena, not a rule.

## 3. `victory=<n>` [C]

A superstar mod's manifest, or an attire mod's (for that attire of a stock superstar; one without a model
works too: `type=attire`, `superstar=`, `attire=1-4`, `victory=` and nothing else). When the winner of a
singles match (the queued scene is 6000-7107: own or generic singles / divas) is that superstar, the hook puts
`n` in its place if script `n` exists (`EntranceScriptExists`); log `victory: superstar <id> attire <a>: scene
<n> instead of <old>`. Tag, trio, manager, title and match-type scenes stay the game's. The winner: the fighter
who lost (`+447`) names the winner's person (`+448`); the person's match slot (`*(0x82E3DDFC) + 132 + p * 52`,
+16 = id * 100 + attire * 10 + kind) gives the superstar.

Proof (26 Oct, hidden match, `runs\vic4_sheet.png`): the test Matt Striker mod with `victory=6176` beat Cena
(`SVR2011_TEST_LOSE=1,12`) and did Kofi Kingston's celebration; with 6139 (vic2) John Cena's rope climb.

## 4. Victories from other games [C]

SvR 2008, 2009, 2010 and Legends of WrestleMania keep their win scenes as 2011 does (`pac/evt/Winning0-3.pac`,
6000 + their own ids: SvR 2008 has WIN_Sabu 6070 and WIN_Sandman 6071, SvR 2010 Jeff Hardy 6111). The entrance
converters convert them unchanged with `--kind victory`:

    python tools/entrance_port.py list --kind victory --game svr08 --src <SvR 2008 disc or folder>
    python tools/entrance_port.py free-numbers --kind victory            (6300-6999 not taken)
    python tools/entrance_port.py convert --kind victory --game svr08 --src <...> --entrance 6070 --number 6300 --out <dir>

- The list keeps the singles scenes (below 7150); the pack is `pacentry evt\Winning0.pac EVPA|EVTA <n>` with
  `victory/<n>_evp.bin` / `_evt.bin`, for a superstar mod's moves\ or Mods\Moves.
- Every character object gets the generic model 13902, as in 2011's own scenes (the game puts the winner,
  loser and referee in; the entrance rule's converted ids name other superstars). No CAE parts, pyro or
  titantron.
- Only the asked scene is BPE-decoded (a convert ~2.5 s, a list ~3 s; decoding every entry took ~20 s).
- The game counts pack scripts 6000-9999 as scenes, not as mod entrances (Create An Entrance lists stay as they
  were; move_packs.cpp SetModEntrances).
- Mod Maker: the Victory list's "From another game" part lists the scenes of the discs in My game discs;
  picking one converts it into the mod's moves (a free 6300+ number) and sets victory=; Preview plays it from
  the mod's pack. Test aid `--star-victory-port <game>,<n>`.
- WWE '12 / '13 / 2K14+ (YANM scenes) aren't converted yet.

Proof (26 Oct, hidden, `runsic6_sheet.png`): Matt Striker with SvR 2008's WIN_Sabu as 6300 beat Cena.

## 5. Victory tracks [C]

A superstar's own music for the win, in place of the theme (suggestion 0bbd61).

- Superstar mod: `victory_song=<file>`, `victory_gain=<dB>` (-20..+6). Mod Maker: Superstar page, More, under Victory,
  with Measure / Match the game.
- Media pack, for a stock superstar: `victory_theme.<id>=<file>`, `victory_theme_gain.<id>=<dB>`. Mod Maker: Media,
  the Audio tab.

How the game starts the win's music [C, event logs and stacks, 6 Nov]:

- The match's end task (sub_82322DA0, `match end: step` in the log) posts the winner's theme right after step 1, the
  result: before the replay of the finish and about 8 s before the scene. A stock theme is the entrance's event
  `Play_MUS_<nnnn>_a_b` (via 825F7BA8), a USER PLAYLIST the entrance's `XMPPlayUserPlaylist` (via 825F7690).
- The scene (sub_82768270 -> 8277CD88 -> 825F2D34, the path an entrance's music takes) then starts its music again. A
  stock theme isn't posted a second time. A USER PLAYLIST is: that was the **double start**, the mods' and CAWs' song
  jumping back to its start as the scene came up. The port now lets the song go on (`user music: the win's song
  started again: it goes on`), with or without a track.

Hooks: `WinMusicOn` from the end task's step 1 (replays.cpp), off when a match sets up a wrestler (sub_828B5BC0) or at
`Play_Menu_Music`. While it's on, the music the game starts goes through media_mods.cpp `WinnerTrack`: the game asks by
theme, so the owner is the person in the match with that theme in the entrance block his match record copied, a person
who beat someone first (a mirror match, or a mod on a stock theme). A theme that's nobody's (a tag team's own) or
whose owner has no track plays as it did. An event track plays on a host voice tied to the event's object, as a media
pack's `sound.` does; a playlist's on the user music player.

Online: the music is this PC's alone. `victory_song` / `victory_gain` / `victory_theme` lines and the sound files stay
out of a mod's key (svrfmt/mod_travel), so two players with different tracks should still match, each hearing their own
(from the code; not played with two different folders). A LAN match with a track on the host only stayed in lockstep
at clocks 600 and 800 [C]. With `SVR2011_TEST_LOSE` online the end task's step 1 never came and no win music started,
so a track never played there. A real online pin hasn't been run. CAWs have no way to set a track yet, and a song has no
start point: both left for later.

Seen [C, logs, 7 Nov]: singles (mod and media pack), tornado tag 0x2E, a Universe title defence (saved card: `nt_test
after the match`; a CAW won, whose theme has no track). 14 entrants' themes in a 15-man Rumble played as they were.
Not seen: a Rumble's end, a win in Road to WrestleMania (rule 0x54 story match, forced loss: no end task, nothing
swapped). Muted tests: how it sounds is a hand check.

## Test aids

- `SVR2011_TEST_VICTORY_LOG=1`: each match person's theme as the owner lookup sees it (`victory track: person ...`).
- `SVR2011_TEST_NO_VICTORY=1`: this game plays without tracks (two test games share one Mods folder).
- Mod Maker: `--star-victory-song <file>` (-3 dB), `--media-victory <id>,<file>`.
- `SVR2011_TEST_LOSE=<person>,<s>`: at <s> seconds of the match clock the person loses to person 0.
- `SVR2011_TEST_EVP_LOG=1`: every scene script the game opens (`evp: /EVPA/6139 (lr ...)`).
- Frame shots stop with the match clock: after the bell use timed shots (`SVR2011_TEST_SHOT_REQUEST`).
- Mod Maker: `--full-preview 6139,139,<key>,1` plays a victory scene (all of it is slot 0, drawn - an
  entrance's slot 0 is the hidden intro).
