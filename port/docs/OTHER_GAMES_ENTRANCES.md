# Entrances from other games -> SvR 2011

One section per game: where its entrances are, how they differ from 2011, what converts, and
the state of the converter. Linked from docs/OTHER_GAMES.md (Superstar Porting's overview).

- **Entry point:** `tools/entrance_port.py` (`games`, `list`, `convert`, `free-numbers`; JSON on
  stdout, `progress <pct> <text>` on stderr, exit 0/2/3). `--src` is an extract folder or a disc
  image (`tools/discfs.py`). The Mod Maker calls it (docs/MOD_MAKER_3_PLAN.md).
- **Output:** a move pack (`pack.txt` with `pacentry evt\NyujyoN.pac EVPx|EVTx <n> <file>` lines,
  docs/MOVE_PACKS.md) holding the entrance and its 5 Create-An-Entrance parts
  (10000 + 1000k + n). Put it in a superstar mod's `moves/` (with `entrance=<n>`, or
  `attire<N>_entrance=<n>`: docs/ENTRANCE_ATTIRES.md) or in `Mods/Moves/<name>/`.
- **The 2011 format** (target of every converter): docs/SVR08_ENTRANCES.md section 1-2, 6
  (EVP script: 32-byte objects, 21 x 212-byte slots, segments at slots 0/5/10/15/20, tail;
  EVT: a YMKs bank, track key (25000 + object, x = slot, y = sub-track); `pac/evt/NyujyoN.pac`
  by number range; 2011's segment 0 is the hidden intro).
- **Themes and titantrons** are not part of an entrance in any of these games: the entrance's
  tail says "music / movie from the profile", so they go into the superstar mod as `song=` /
  `movie=` (docs/SUPERSTAR_MODS.md, docs/MEDIA_MODS.md).

| game | disc id | entrances | converter | status |
|---|---|---|---|---|
| SmackDown! Here Comes the Pain (PS2) | SLES_520.36 / SLUS_207.87 | 185 | `hctp_entrance.py` | ready, played in game (Goldberg) |
| SmackDown! vs. Raw (PS2, 2004) | SLES_527.81 / SLUS_210.60 | 96 | `entrance_port.py` (HCTP path, game svr04) | ready, played in game (Andre 3066 as 490) |
| SmackDown! Shut Your Mouth (PS2) | SLES_512.83 / SLUS_204.83 | 114 | `sym_entrance.py` (game sym) | ready, played in game (Hollywood Hogan 3044 as 491); all 114 convert offline (Hollywood Hogan 3044, DDP 3020, X-Pac 3067, Raven 3058) |
| SmackDown! vs. Raw 2006 (PS2) | SLES_536.76 / SLUS_212.86 | 93 | `svr06_entrance.py` (game svr06) | ready, played in game (JYD 4080 as 492); all 93 convert offline (JYD 4080, La Resistance 4202, Team Angle 4252); 2/3 also in SvR 2007 |
| WWF SmackDown! Just Bring It (PS2) | SLES_504.77 / SLUS_203.16 | 348 | `jbi_entrance.py` (game jbi; SYM's back end) | ready, played in game (Chris Benoit, JBI 1, as 499; again with the mapped fingers); all 348 convert offline, named from the ELF |
| SvR 2007-2010 (PS2) | SLES_544.89 / 548.79 / 552.51 / 555.45, SLUS_214.27 / 216.45 / 218.10 / 219.01 | same as 360 | `ps2_entrance.py` (games svr07-svr10 read PS2 discs) | ready: 2008 / 2009 output byte-identical to the 360's |
| WWE 2K15 / 2K16 / 2K17 (X360) | 545408B6 / 315A07D2 / 545408BA | 138 / 236 / 315 | `wwe13_entrance.py --game wwe2k15/16/17` | ready, played in game (2K15 Rollins 369, 2K16 Owens 458, 2K17 Asuka 292); 2K15 one-takes warned |
| SvR 2007 (X360) | 545107E0 | 150 (58 in 2011) | `entrance_port.py` (2008 path) | ready, played in game (Andre 4109 as 496) |
| SvR 2008 (X360) | 5451080B | 136 (78 in 2011) | `svr08_entrance.py` | ready, played in game (Sandman) |
| SvR 2009 (X360) | 54510826 | 168 (149 in 2011) | `entrance_port.py` (2010 rules) | ready, played in game (Mr. Kennedy 4062 as 495) |
| SvR 2010 (X360) | 54510844 | 203 (191 in 2011) | `svr10_entrance.py` | ready, played in game (Mr. Kennedy) |
| Legends of WrestleMania (X360) | 54510838 | 157 (89 not in 2009 / 2011) | `entrance_port.py` (2009 path) | ready, played in game (Andre 4019 as 493); gondola entrances not supported |
| WWE '12 (X360) | 54510890 | 247 (149 in 2011) | `wwe13_entrance.py --game wwe12` | ready, validated offline (Rey 123, Austin 101: 1-2 cm) |
| WWE '13 (X360) | 545108B4 | 317 (150 in 2011) | `wwe13_entrance.py` | ready, played in game (Mankind 246 as 497); titantrons need Bink 2 |
| WWE 2K14 (X360) | 545408B2 | 347 | `wwe13_entrance.py --game wwe2k14` | ready, validated offline on segments 2K14 did not re-cut (1-2 cm); titantrons need Bink 2 |

## SmackDown! Here Comes the Pain (PS2)

Details: docs/HCTP_ENTRANCES.md (+ HCTP_ENTRANCE_EVENTS.md).

- **Where:** `PAC/EVTENT.PAC` (DPAC): `EVET/<n>` motion banks 3000-3533 and `ENTR/SEVP` (all
  scripts); names in `PAC/MOVES.PAC` LIST sub-entry 2. Placeholders (512-byte banks) are skipped.
- **Format:** an older script (40-byte slots, 5 segments, little-endian) and CEKey banks in
  HCTP's wide bone form; same motion engine generation as 2011 (no retarget).
- **Rules:** HCTP's segment 0 (intro + stage) is cut at the first camera cut after the reveal;
  HCTP's raised stage and sloped ramp are brought down to 2011's floor level; camera keys
  13 -> 29 bytes (same rotation convention); events kept / dropped per both executables; ring-rope
  objects kept; prop 2019 (folded WWE Title) -> 2014.
- **Pyro:** none in the data (HCTP's flashes are op 18 events, kept). **Titantron/theme:** from
  the profile.

## SmackDown! vs. Raw (PS2, 2004)

- **Where:** HCTP's files: `PAC/EVTENT.PAC` (DPAC) `EVET/<n>` motion banks 3000-3201 and `ENTR/SEVP`
  (`hctp_entrance.parse_sevp` reads it unchanged); names in `PAC/MOVES.PAC` `MOVS/LIST` sub-entry 2
  (BPE-packed, HCTP's {u16 number, name[30]} records). 96 entrances (The Rock, Andre, Mankind, Bret
  Hart, Brutus Beefcake, Mr. McMahon...).
- **Differences from HCTP:** 20-24 sub-scenes per object instead of 5: by position 0-3 hidden intro,
  4-8 stage, 9-14 ramp, 15-18 ringside, 19-23 in the ring = 2011's 5 segments (whole sub-scenes
  joined, no intro cut); camera keys of 19 bytes (f32 position, s16 rotation); most entrances carry
  2 partner / valet characters (generic bodies). Same motion format, wide bone table, raised stage
  (HCTP's ground fix applies).
- **Converter:** `hctp_entrance.build(..., game='svr04')` (GAMES: segment groups, 24 slots, the
  19-byte camera key) via `entrance_port.py --game svr04`. HCTP's output is unchanged (3047
  byte-identical). Converted offline: 3066 Andre, 3077 Bret Hart. Not played yet.
- **Media:** titantrons MPEG in AFS archives, themes CRI ADX (not converted). No pyro (flash ops only).

## SmackDown! Shut Your Mouth, SvR 2006, Just Bring It (PS2) - research

- **Shut Your Mouth converter:** `tools/sym_entrance.py` (game `sym`; formats and rules in its docstring):
  own DPAC reader (doubled row counts), SYDT -> parse_sevp's dicts, halfword records re-framed to HCTP's,
  slot-relative roots / cameras baked to world (yaw turns the root), ground fix per slot, SYM's takes
  placed into 2011's 5 segments by where the entrant starts (alternates of equal length dropped; a
  1-frame stand-in for an empty segment), camera 0 of 2 (`camera=1` to switch), prop 172 -> 2014, other
  props dropped. All 114 convert offline; Kane 3021 vs HCTP 3007: heights and root paths line up.
  Played in game: Hollywood Hogan 3044 as 491 (test star first, stage -> ramp -> ring, no crash).
- **Shut Your Mouth:** `PAC/EVTENT.PAC` motion banks 3000-3113 (HCTP's format, but frame records sized
  in halfwords: a byte < 0x80 skips frames, else (b & 0x7f) x 2 bytes) and only the compact `SYDT`
  script (a reader reproduces HCTP's and SvR 2004's full scripts 534/534 and 100/100); 2-7 segments
  with alternates, 2 cameras, character roots relative to the slot (bake to absolute), own prop ids.
  Unique: Hollywood Hogan, DDP, X-Pac, Raven, Lawler, JR, Earl Hebner. Plan: SYDT reader in front of
  HCTP's back end.
- **SvR 2006 converter:** `tools/svr06_entrance.py` (game `svr06`; its docstring has the formats and
  rules): 2007's rules on the little-endian script (PS2 byte-order traps as 2007-2011), belt 2021 ->
  5009, bodies re-encoded (`hctp_entrance.conv_char_pose`), face/hand tracks, pyro and ops 24/65/185
  dropped, a 1-frame intro made for the 4 entrances without one, 4077's extra slots dropped. Andre
  4066 vs the svr07 path's 4109: same frames, roots within 0.4, bones ~1 degree except the foot pitch
  2006 does not store. `list` gives `in_svr07` (prefer the 2007 version when there is one). Played in
  game: JYD 4080 as 492 (test star first, stage -> ramp -> ring, no crash).
- **SvR 2006:** `PAC/EVT.PAC` (a DPAC variant with doubled group counts), 95 entrances 4000-4500; the
  script is 2007/2008's layout little-endian, cameras already 2011's 29-byte keys, floor-level stage;
  bodies need HCTP-style re-encoding; ops 24, 65, 185 differ in size. About two thirds are in SvR
  2007 (use svr07 for those: Andre 4066 = 2007 4109). Unique: JYD, Heidenreich, Rene Dupree, tag
  entrances, Team Angle.
- **Just Bring It converter:** `tools/jbi_entrance.py` (game `jbi`; formats in its docstring). The
  motion is SYM's, so the module only reads JBI's container and script into sym_entrance's dicts and
  SYM's pipeline does the rest (no ELF study needed):
  - `EN3.PAC`: DPAC with one group `ENMT` {u16 0x8000 | 349, u16 0}, rows {u16 BCD number, u16 size /
    0x100}, data from 0x4000, sector aligned. Each row is a `PAC ` {u32 n; n x (u16 id, u24 offset, u24
    size)}: item 0 the motion bank (CEKey, SYM's halfword framing), item 1 the script (CRLF text).
    Entrances 0-78 and 1000-1268 (348); 9999 is a shared bank, not an entrance.
  - Track keys are SYM's transposed: id 9000 + take, x 0, y = cast index (characters); id 25384 + take,
    x 0, y 50 (camera). 1500 x 0-10 (4-frame static objects), 9010 and 9999 (1 record): dropped.
  - Bodies: the same 21-bone records as SYM / HCTP (`hctp_motion.decode_body`, H_TYPES_WIDE, identical
    channel masks). Camera: SYM's 13-byte key `[c][s16 pos x3][s16 rot x3]` + pad, with c = frames to
    the next key - 1; JBI keys sparsely (8 keys for 539 frames) and without an event block, so a span
    >= 0x80 would read as one: the reader puts an empty event block in front and splits such spans with
    an interpolated key (2 keys in all of JBI).
  - Script: line 1 the character count, line 2 the 6 cast model ids (255 = none), line 7 the take
    count, then per take 6 rows `x,y,z,yaw` (one per cast index; units 1/100 of SYM's, yaw in degrees).
    Character roots are relative to their row (SYM's slots; yaw sign checked by take-to-take
    continuity, 0007 takes 6 -> 7 at 88 degrees). The camera has no row: its keys are world positions
    (0007: 7-49 units from the entrant in every take; relative to a row they'd be 120-260 off).
  - Floor: a JBI take row can sit at the stage while its roots already walk the arena (0007 takes 3-4,
    row z 244, entrant from the ramp foot into the ring), so the stage floor fix goes by where the
    entrant starts (`at_z`), not by the row.
  - Hands: op 16 as SYM (`10 ss iiii`), but the ids are JBI's own library, not 2011's: M.PAC
    `MOT/PTS`, ids 0-170 left / 200-370 right, one 60-byte record per pose = per finger (thumb ..
    pinky) a base joint `[k][0][s16 x y z]` and a bend `[k][0][s16]` (k 0x3b-0x44 left, 0x45-0x4e
    right). Only 23 ids exist in both libraries and they don't look alike, so every JBI id is mapped
    by its finger angles (`hand_map`): per finger a curl (fingers: z of every joint; thumb: y of every
    joint + z of the base), scaled per library and finger to 0 straight .. 1 (95th percentile), then
    the nearest 2011 pose of the same hand (thumb weighted 0.5). Checked on 2011's labelled shapes
    (fist ~ 1 1 1 1 1, open ~ 0, point / V / horns read right) and on JBI: pose 1 -> fist 3, 2 -> a
    pointing hand, 11 -> open 11, 126 -> horns 64, 108 -> relaxed 2. 1746 hand events mapped; the
    finger tracks now use 1305 events (477 before, and those were wrong shapes).
  - Names from the ELF's tables (`elf_names`): the roster (84-byte records by cast model id, 0 The
    Rock .. 45 Fred Durst) and the tag / trio entrance lists (36-byte `{u16 EN3 number, s16 -1,
    name[32]}`). Tag / trio entrances get the team name (generic ones plus the cast: "Tag Champion:
    Triple H & Steve Austin (JBI 3)"), others the cast ("Chris Benoit (JBI 1)", story scenes "...
    (JBI scene 1001)"); all 348 unique. The alphabetical 0-45 list next to them is the theme list,
    and the singles motion list's ids are not EN3 numbers (not used).
  All 348 convert and self-check offline. Played in game: JBI 0001 as 499 (test star first, stage ->
  ramp -> ring, no crash).

## SvR 2007-2011 (PS2) - research

The PS2 discs carry the same entrances as the 360 versions (2007 150, 2008 140, 2009 168, 2010 211,
2011 238 - none PS2-only). Containers are the 360's in little-endian (DPAC 2007-2009, DPK8 2010-2011),
scripts are the 360 EVP in PS2 byte order (two-u16 fields, raw type bytes: a little-endian parser +
`build_evp` rebuilds the 360 bytes for 935/935), tracks are the same YMKs banks (2008 138/140 and 2009
167/168 byte-identical; 2010/2011 character tracks are key-reduced). A ~110-line front end (container
reader + `parse_evp_le`) lets the svr07-10 converters take a PS2 disc; 2008/2009 output then equals
the 360-based one - done: `tools/ps2_entrance.py` (`entrance_port.pac_entries` reads DPAC / DPK8 and turns
PS2 scripts into the 360 bytes), so games svr07-svr10 take a PS2 disc; `games` lists the PS2 serials in
`disc_hints` (PAL discs checked; NTSC-U SLUS serials listed too, their data layouts untested). Checked: 2008 Sandman 4132 and 2009
Kennedy 4062 byte-identical to the 360-based conversion, 2007 Andre 4109 script identical (one minor track
differs, as on the disc), 2010 Kennedy 162 converts. Titantrons: Sofdec MPEG-1 224x192 in `MOVIE/NMV*.AFS`; themes CRI ADX; pyro sets are
PS2 textures (not portable). The 360 discs are the better source.

## WWE 2K15 / 2K16 / 2K17 (Xbox 360)

- **Converter:** `wwe13_entrance.py --game wwe2k15 / wwe2k16 / wwe2k17` (entrance_port games of the same
  names): `scene_entries` reads 2K16/2K17's per-scene pacs (EPAC 20-byte entries or HSPC + SHDC),
  `evp_788_to_212` the 788-byte slots, mode-4 YANM blocks skipped, a one-take entrance (only segment 0)
  is converted but warned. Pyro: 2K15 `prtcls.pac`, 2K16/2K17 `pac/particle/pce6NNNN.pac`. Converted
  offline: 2K15 369 Seth Rollins, 466 Cesaro (one-take, warned), 2K16 458 Kevin Owens, 2K17 292 Asuka
  (HSPC). Not played yet. Prop 2755 (in nearly every new entrance) and 159 are not in 2011 (warned).

- **2K15:** `pac/evt/Entrance00-10.pac`, xcompressed EPK8 (groups `EVP2`, `EVZ `): 138 entrances (a
  cut-down 2K14: 291 numbers shared, 42 new). **2K16/2K17:** one pac per scene `pac/evt/NNNNN.pac`
  (entrances 100-899, CAE parts 10000-14999), 20-byte table entries with 4-character names (number
  from the file name); 2K17 has 115 in a new HSPC container (SHDC block instead of PACH). 236 / 315
  entrances.
- **Script:** 2K14's EVP (cm positions, segments 0/5/10/15/20) with **788-byte slots**: words 0-50 +
  195-196 = 2K14's slot (56,297 / 56,442 shared slots equal); the rest are longer rope / turnbuckle key
  lists (up to 32 keys, 2011 holds 8: resample 26 / 124 / 199 slots).
- **Motion:** same records at 60 Hz, same Biped skeleton; 2K15 YANM v4, 2K16/2K17 v5 (`yanm_decode`
  reads it: same pose within 0-1.7 cm); mode-4 YANM blocks (boned props only) must be skipped; y1 is a
  32-track face rig (dropped). Checked: 2K15 Austin vs 2011's own 0-2 cm on segments not re-cut.
- **One-take entrances (2K15):** ~61 put the whole entrance in segment 0 (2011 hides segment 0): use
  2K16's re-cut of the same take (37 of 61) or an HCTP-style split.
- **Other:** lights object moved to `pac/evt/light/elNNNNN.pac` (2011 donor used anyway); the event
  table covers 82% (2K15) / 95% (2K16-17); new prop 2755 in nearly every new entrance, 2K17 uses 63
  props 2011 lacks; pyro PCE6 (2K16-17 in `pac/particle/pce6NNNN.pac`); titantrons Bink 2.

## SvR 2007 (Xbox 360)

- **Where:** `pac/evt/evt1.pac` (EVP1/EVT1, 4000-4046) and `evt2.pac` (EVP2/EVT2, 4056-4362),
  EPAC: 150 entrances (89 singles, 11 generic styles, 10 champion versions, 11 tag, 4 trios, 25
  match intros). evt0 = the entrance editor and props, evt3 = CAE parts, evt4-8 = season and win
  scenes. **The EPAC group count is the u16 at +4** (/3); 2007 keeps other data in the u16 at +6,
  so a u32 read fails (`entrance_port.pac_entries` reads the u16).
- **Not in 2008 or 2011:** 29 singles, e.g. Mankind, Cactus Jack, Dude Love, Andre, Bulldog,
  Bigelow, Lesnar, Eugene, Lawler, D-X. Same-named ones differ too (2008 re-recorded most tracks).
  Yuke's converted one itself: 4252 nWo -> 2011 772 "NWO Trio" (the reference for the rules).
- **Format:** 2008's (28-byte objects, 212-byte slots, headerless track bank); `parse_evp(d,
  '2008')` round-trips 631 of 636 scripts; the rest have a 2007-only **type-9 tail record** (a
  music cue). Event sizes as 2008 except ops 127 and 172-175 (not used by entrances).
- **Rules** (`entrance_port.svr07_convert`): the 2008 converter, plus extra character models ->
  generic 3902 (-> 13902; 772 uses 13902), the type-9 tail record dropped, **op 79** (a loop
  marker on the face/hand tracks y 1-3, ~700 per entrance) removed, as in 772.
- **Props:** 21 of 28 exist in 2011; missing 2040, 2054, 2075, 2078-2080, 3001 (warned).
  **Pyro:** `prtcls.pac` group PCEN by number (format vs 2011 not checked; not installed).
  **Titantrons:** Bink 1 320x320, usable as `movie=`. **Themes:** `music/DVD_Music.xwb` (XACT,
  XMA; ffmpeg decodes them wrapped as RIFF 0x166); the cue -> wave map is still open.

## SvR 2008 (Xbox 360)

Details: docs/SVR08_ENTRANCES.md.

- **Where:** `pac/evt/Nyujyo0-3.pac` (EPAC) groups EVP1-4 / EVT1-4, numbers 4000+.
- **Format:** 28-byte object entries (2011: 32), the same 212-byte slots; the EVT is a 2011 YMKs
  bank without its 16-byte header, with 2008's own event-size table.
- **Rules (validated on 4071 -> 506):** objects to 32 bytes, track ids + 25000, 2011's size
  table, op-193 face events with params 0x30-0x3F dropped from y0 tracks, belt 2029 -> 5009,
  entrant model -> 16102 (preview body), generic 3902 -> 13902, x 10..14 effect tracks copied
  to x + 50.
- **Props:** the shinai, can and microphone exist in 2011 with the same ids. **Pyro:** the global
  effect object's tracks (converted). **Titantron/theme:** from the profile (the superstar mod).

## SvR 2009 (Xbox 360)

- **Where:** `pac/evt/Nyujyo0-3.pac` (EPAC, EVP1-4 / EVT1-4): 168 entrances 4002-4424;
  `TNyujyo.pac` = 18 test scripts (skipped); `Create00-20.pac` = CAE parts (not used). Profile
  CHAR/PRO (496 bytes): +0xE4 music, +0xE6 movie, +0xE8 entrance, +0xEA champion; attire 2 at
  +0xEC.
- **In 2011:** 149 (36 rebuilt by Yuke's). Only in 2009: Undertaker / HBK / Rey PPV versions,
  Big Daddy V champion, General Champ A, Cruiser champion, Undertaker & Zombie.
- **Format:** 2008's 28-byte object entries but 2011's model ids and tail; the EVT is 2008-style
  (no header) and every op used has 2011's size; all character records decode with 2011's codec.
- **Rules** (`entrance_port.svr09_convert`): parse as 2008, then the 2010 rules
  (`svr10_entrance.convert_evp`), plus extras 13902 -> 16102 when object 0 is the preview body
  (as Yuke's); tracks through `svr08_entrance.convert_evt`. Offline: 20-22 of 142 shared scripts
  byte-identical, 77% of tracks. Scripts with fewer slots (4317 Undertaker & Zombie) get no CAE
  parts (warned).
- **Props:** 31 of 37 exist; missing 2116, 2152, 2262 (warned). **Pyro:** PCEN/LPEN by number,
  an older particle format (not portable). **Titantrons:** Bink 1. **Themes:** `music1/2.yaf`,
  record = music id x 100, XMA1.

Shared parser fixes found on 2007/2009 (`svr08_entrance.parse_evp`): type-9 tail records carry
one item and keep their count field; 30-character names have no NUL, so the object count is the
low 16 bits of +0x40 (the high half is kept). This also round-trips 2011's own 400/401/523/700.

## SvR 2010 (Xbox 360)

- **Where:** `pac/evt/Nyujyo0-3.pac` - EPK8 like 2011, groups EVP1-4 / EVT1-4; 211 entrances:
  singles 100-303, alternates, champion versions 450-478, CAE styles 480-487, legends 500-563,
  tag teams 700-773, match intros 800-840. CAE parts in separate `Create00..20.pac` (not used:
  the converter makes 2011-style parts). 2010 profile entrance block at +0x178 (attire 1) and
  +0x180 (attire 2): music, movie, entrance, champion.
- **In 2011:** 184 by name (some renumbered: 111 Jeff Hardy -> 535, 114 JBL -> 536,
  127 Maria -> 539, 157 Umaga -> 537, 166 Khali -> 547, 168 Dreamer -> 526, 503 Masters -> 50,
  504 RVD -> 306, champion versions 450-464 -> 600-614; 35 were rebuilt by Yuke's). Not in 2011:
  118 Carlito, 162 Mr. Kennedy, 214 Festus, 542 Layla, 403 JBL 2P, 411 Cena PPV, 412 HBK PPV,
  some champion versions.
- **Format:** the EVP already is the 2011 layout (32-byte objects, 212-byte slots, 2011 tail;
  `parse_evp`/`build_evp` round-trip 203 of 211 exactly); the EVT is a full YMKs bank whose ops
  all have 2011's sizes (op 208 shrank 6 -> 2 but no 2010 entrance uses it).
- **Rules** (`svr10_entrance.py`; 67 of 183 shipped pairs byte-identical, the rest were
  re-authored): track ids + 25000, 2011's size table, belt 2029 -> 5009, the global effect
  object's segment parents 9999 -> 0, object 6's position templates from 2011's own entrances,
  preview body unless 2011 uses the 2010 entrant model.
- **Props:** all exist in 2011 except 2403 (JBL 2P limo extra) and 2405 (Miz) - the converter
  warns (2011's own 703 references 2405 too, so a missing prop is tolerated [L]).
- **Pyro:** `prtcls.pac` PCE6/LPE6 packs keyed by entrance number (`0NNN`); ids 0-23 are the
  entrance's own emitters (byte-identical 2010 -> 2011). Of the 2011-missing entrances only 411
  and 412 have their own pyro. Not installed by the converter yet: `prtcls.pac` is EPAC and the
  move-pack overlay's `pacentry` is EPK8-only.
- **Sounds:** op-193 sound ids 4000-4499 are the same in both games; only Kennedy's 4300-4304
  (and 9012, 9349) are missing in 2011 (silent).
- **Titantron:** Bink 1 320x320 (same as 2011): copy as the mod's `movie=`. **Theme:**
  `music1.yaf` XMA2 records -> mp3 (`svr10_superstar.py` already does it).

## WWE Legends of WrestleMania (Xbox 360)

- **Where:** `pac/evt/Nyujyo0-3.pac` (EPAC, EVP1-4 / EVT1-4), 157 entrances 4001-4359: singles 4000 +
  (superstar id - 100), champion versions 4200-4217, tag teams 4300-4329, and 29 "Gondola" (ring cart)
  entrances 4170-4186 / 4230-4233 / 4350-4359. Profile CHAR/PRO 504 bytes: +0xE4 music, movie,
  entrance, champion, gondola, gondola champion (attire 2 at +0xF0).
- **Format:** SvR 2009's (2008's 28-byte objects, 2011 model ids and tail; headerless YMKs bank, every
  op used has 2011's size, all body records decode); ~60% of the tracks are byte-identical to 2009's.
- **Not in 2009 or 2011:** 89, mostly legends (Andre, Piper, Rude, Slaughter, Bundy, Yokozuna, Bulldog,
  Studd, Road Warriors, Honky Tonk Man, Iron Sheik, Volkoff, Beefcake, Bossman, Duggan, Kamala, Bigelow,
  Arn Anderson, Ultimate Warrior...), legend tag teams and champion versions.
- **Rules** (`entrance_port.low_convert` = `svr09_convert(low=True)`): LoW's generic extras 10902 /
  24802 / 24902 (and 11902 / 15902 except a tag partner at object 4) -> 13902 before the 2009 rules;
  object 6's positions always from 2011's template; the inert last ring-cart prop (2552 / 2565) and its
  tracks dropped. Gondola entrances are refused (exit 2) until the carts are ported.
- **Props:** 30 of 43 in 2011; missing 2116, 2152, 2262, the carts and 2554/2555/2557/2562-2564/2568/2569
  (warned). **Pyro:** 2009's older PCEN format (not converted). **Titantrons:** Bink 1 by superstar
  id: `--titantron <id>`. **Themes:** `sound/music1.yaf` (2009-style).

## WWE '12 (Xbox 360)

- **Where:** `pac/evt/Nyujyo0-9,A.pac` (EPK8), groups EVP and EVZ, 2011's pac letters and number
  ranges: 247 entrances + 685 CAE parts. 149 share number and name with 2011, 34 sit on numbers
  free in 2011 (Lesnar, Bryan, Del Rio, Nash, Luger, Warrior, Demolition...). Profile CHAR/PRO
  (1072 bytes): +0x1D4 music, movie, entrance, alternate; +0x1DC champion.
- **Script (EVP):** 2011's layout, but **positions in cm (10 x 2011)**, the same rule as WWE '13;
  generic models 10402 / 51200 / 12502 -> 16102 / 13902; f44 -> 0; standard tail.
- **Tracks (EVZ):** WWE '13's records (YANM + YAT2) at 60 Hz; the body is plain FK (no IK calves:
  `w13_retarget` with IK off); cameras as WWE '13 (exact); 87 of 90 event kinds as in '13.
- **Props:** 35 of 44 exist in 2011; missing Del Rio's car 2636, 2649, DLC 2653-2658. **Pyro:**
  2011's format. **Titantrons:** Bink 1 (21 at 640x640, not tried). **Themes:** Wwise `Music.pck`.
- **Converter:** `wwe13_entrance.py --game wwe12` (`entrance_port.py` game `wwe12`; plain pacs, no
  svrmod needed; `Retarget(ik=False)`). Validated against 2011's own 123 Rey Mysterio and 101 Steve
  Austin: root exact, head / hands / feet median 0.3-2.2 cm (95%: 1-8 cm), EVP slots 162-167 of
  168 equal. WWE '12-only event kinds (char 31/t1 ...) are dropped and reported.

## WWE '13 (Xbox 360)

- **Where:** `pac/evt/Entrance00..08.pac` + `Entrance_dlc01.pac` - xcompress around EPK8 with BPE
  entries; groups `EVP ` (script) and `EVZ ` (tracks); 317 entrances with CAE parts like 2011.
  150 have the same number and superstar in 2011, 39 use a number 2011 gives to someone else
  (50 DDP, 511 Matt Hardy, ...), 128 use numbers free in 2011 (Mankind 246, Cactus Jack 245,
  Attitude Era 310-316, legends 553-569, ...). Profile entrance block at +0x230 (same layout as
  2011's).
- **Script (EVP):** the 2011 format byte for byte (`parse_evp(d, '2011')` reads it); 310 of 317
  have no tail (write 2011's standard tail); generic extras use 19202/10402 (2011: 13902).
- **Tracks (EVZ):** WWE '13 motion records (YANM animation + YAT2 events) per object, named by
  the 2011 track key, at 60 Hz (2011: 30). The character needs the WWE '13 skeleton retarget
  (as for WWE '13 moves) into a 2011 body track; cameras convert exactly (eye/target/fov,
  position x 0.1, a fixed axis swap); events map by a table derived from the 150 shared
  entrances; the lights/effects object is not fully decoded (first version: a donor).
- **Props:** 34 of 58 exist in 2011; 24 need porting or a stand-in (Godfather stick, Lincoln,
  Lowrider, Kane mask, Cena chain/caps...). **Pyro:** `prtcls.pac` same format, by number.
  **Titantron:** Bink 2 (2011 plays Bink 1) - needs RAD's Bink 2 decoder (a download, the user's
  call) before the project's own Bink 1 encoder. **Theme:** Wwise `Music.pck` (vgmstream, as
  for the WWE '13 Mankind port).
- **Correction:** EVP positions are in cm (10 x 2011): divide by 10, also the rope words 34-36,
  40-42, 46-48.
- **Converter:** `wwe13_entrance.py` (list / convert / validate; `entrance_port.py` game `wwe13`,
  `--svrmod` = the Mod Maker's `svrmod.exe` for xcompress) with `w13_retarget.py` (pose -> 2011
  world -> the 21 YMKs bones, parents first, per-bone axis limits, hand twist into the forearm
  roll), `w13_skeletons.py`, `yanm_decode.py`. **2011's motion bind is the HCTP rest pose**, not
  the model's A-pose (with the model's own bind hands and feet were 15-23 cm off).
- **Validation (offline, 135 shared entrances, 642 segments):** median error hands 1.1-1.2 cm,
  feet 0.7-0.9 cm, head 0.6 cm, root 0; 595 of 642 segments under 3 cm everywhere (the rest were
  hand-edited by Yuke's for 2011). Cameras 0.01-0.04 cm / 0.01 deg. Events: a table from the
  shared entrances. Lights/effects object: a stock 2011 donor's tracks. Not converted: pyro, the
  Bink 2 titantron, the theme. Mankind 246 -> 497 played in game (lighting, walk, crawl under the rope).

## WWE 2K14 (Xbox 360)

- **Where:** `pac/evt/Entrance00-08.pac`: xcompress like '13, but the inside is **EPAC** with
  '13-style group tables (8-byte names: count field / 4; 4-byte names: / 3); 347 entrances (228
  singles 100-599 with CAE parts, 62 in the 600s, 58 tag/trio, 20 intros).
- **Format:** WWE '13's (238 match by number and name; decoded motion identical; 22,444 of 23,728
  shared event blocks identical, 76 new event ids). Differences: motion leaves carry an id string
  (use the leaf index), 40 EVPs have a new tail type (write the standard tail), count flags on
  676/677/731/757/777, a new generic extra 25500 -> 13902, numbers moved from '13 (Mankind 246 ->
  497, Cactus Jack 245 -> 496...).
- **Not in 2011 or '13:** ~38 singles (Goldberg, Ryback, Shield, Cesaro, Sandow, Usos, Andre,
  Yokozuna, Piper, Razor...).
- **Props:** 65 used: 32 in 2011, 9 from '13, 23 new (boned props would be static). **Pyro:** 444
  sets, copy into PCE6 / LPE6. **Titantrons:** 116 Bink 2 (blocked as for '13). **Themes:**
  probably `Game_Sound_M.pck` (map unknown).
- **Converter:** `wwe13_entrance.py --game wwe2k14` (`entrance_port.py` game `wwe2k14`; the shared
  reader takes EPK8 and EPAC, kind-0 groups / 3 in EPAC; generic 25500 -> 13902; odd tails replaced
  by the standard tail; leaf id strings ignored). Validated against 2011's 101 / 123: segments 2K14
  kept as they were match within 1-2 cm (feet 0.6-0.9 cm); re-cut segments (other frame counts,
  e.g. Austin x5, Rey x5-x20) differ, as expected. Not converted: boned/cloth prop motion, pyro,
  Bink 2 titantrons, themes.

## Tag and trio entrances (9 Nov, suggestion b9a775)

- **2011:** numbers 700-799 - tag 700-756 (mixed tags 750-756, generics 720-723), trios 770-775. CREATE AN
  ENTRANCE's MOTION lists split them by category (2 singles, 3 tag, 4 trio). Every script has 8 character objects;
  a team's members are objects **0, 4 and 5** (Hardyz 701: 0 Matt, 4 Jeff; MNM 770: 0, 4, 5), the rest are extras
  without tracks. The scripts keep stand-in models on 4 / 5 (General Tag 720: Edge's).
- **A team's entrance:** the save's team table (+0xF5C8, 70 x 270 B) has a tag block (+0x14 entrance, +0x16 /
  +0x18) and a trio block (+0x1A, +0x1C / +0x1E), set in TEAM MANAGER (ENTRANCE SETTING). The match slot record
  (`*(0x82E3DDFC) + 132 + idx * 52`) has a team index at +48 (-1 none; the select screen's slot +40). With it set,
  the slot asks no singles number (sub_82756538) - but `sub_823373A8`, which reads the team's entrance, was never
  called in a test match: +48 seems to index the match's own team list (sub_823370D0 -> sub_828B9520), which the
  select screen fills when a team is picked. Not worked out yet.
- **The sources:** 2010, '12-2K17 use 2011's 700-799 (2010 36, '12 42, '13 50, 2K14 58, 2K15 14, 2K16 31, 2K17 48);
  SvR 2007 / 2008 4200-4259, 2009 / Legends 4300-4359. Every converter keeps the members on objects 0 / 4 / 5 with
  their tracks (checked: '13 Too Cool 741 and The Brood 778, 2007 MNM 4250, 2008 MNM Trio 4230 and Cade & Murdoch
  4211, 2009 4313, 2010 MNM 770).
- **entrance_port `--team singles|tag|trio|auto`** (convert, free-numbers): the members played and the number's
  range (singles never 700-799; tag 700-769, trio 770-799). Without it the number decides. `auto`: the source's kind
  (its tracked members), the number re-picked when it doesn't fit. A team motion as a singles entrance (or a trio
  as a tag one) doesn't draw the other members - their slots' draw flag off, their tracks kept (the bank's entries
  share data offsets). List rows carry `team`. The Mod Maker's From another game -> Entrances has an Entrance kind
  (Automatic / Singles / Tag team / Trios); the superstar paths (superstar_port, the superstar page, Simple) stay
  singles.
- **Game:** a mod's tag number shows in CREATE AN ENTRANCE's tag list, a trio number in the trio list
  (cae_mod_motions `TeamKind`; before, both lists showed every mod 7xx number).
- **Test aid** `SVR2011_TEST_TEAM_ENTRANCE=<team>,<tag>[,<trio>]` (test games only - the exe in runs\svrtest\, its
  private UserData; memory only): the team's entrance blocks, and the test match's slots of its members as that team.
  `SVR2011_TEST_CAE_LOG=1` logs each slot's entrance number and, when asked, a team's (sub_823373A8).
- **Seen in game (9 Nov):** The Brood (WWE '13 778, a trio) at singles number 699 as a superstar mod's entrance:
  the entrant walked out alone, stage to ring, no stand-ins. **Not seen yet:** a ported tag or trio entrance played
  by a team in a tag / 6-man match, and the TEAM MANAGER / CREATE AN ENTRANCE lists showing the MOD rows.

## Per-attire entrances and entrance outfits (game side)

docs/ENTRANCE_ATTIRES.md: `attire<N>_entrance=<n>` (Rey Mysterio's "Evil" style), entrance
outfits (EMD kind 4 per attire, already working for mods).

## One entrance file (no disc, no folder)

`entrance_port.py` takes a single entrance pac copied off a disc as `--src` (any file name), for
`detect`, `list` and `convert`; `--game` is optional (detected from the format, `--game` wins):

```
python entrance_port.py detect  --src <file>
python entrance_port.py list    --src <file> [--game-pac <2011 pac dir>]
python entrance_port.py convert --src <file> --entrance <n> --number <2011 n> --out <dir> [--game-pac ...]
```

`detect` returns `{game, name, platform, entrances, file, detected, limits[]}`; `convert`'s report
carries `lone: {file, detected, limits[], warning?}` and the limits again as notes.

| Files | Games |
|---|---|
| `pac/evt/evt1.pac` / `evt2.pac` | SvR 2007 |
| `pac/evt/Nyujyo*.pac` | SvR 2008, 2009, 2010, LoW, WWE '12 |
| `pac/evt/Entrance*.pac` (xcompressed: svrmod) | WWE '13, 2K14, 2K15 |
| `pac/evt/<entrance>.pac` (keep the name, or give `--entrance`) | 2K16, 2K17 |
| `PAC/EVTENT.PAC` | HCTP, SvR 2004, Shut Your Mouth |
| `PAC/EVT.PAC` / `EN3.PAC` | SvR 2006 / Just Bring It |

Why one file is enough: every game keeps an entrance's script and all of its tracks - camera,
props, other people - in the same pac (checked on every stock pac: 2007-2K15 EVP and EVT / EVZ of
each number sit in one file; 2K16 / 2K17 are one pac per entrance; the PS2 games one file for all).
The converted EVP / EVT are byte for byte a disc conversion's (one entrance per game, all 16 games),
except the PS2 names below.

What a lone file can't give:
- the theme music (no converter reads it: the mod picks the song);
- the titantron (`movies/titantron` on the disc): `--titantron` is skipped;
- pyro, for the games that have it (2010 on): `pac/prtcls.pac`, 2K16 / 2K17
  `pac/particle/pce6NNNN.pac`;
- PS2 names: HCTP / SvR 2004 / SYM names are in `PAC/MOVES.PAC`, JBI's in the ELF -> "HCTP 3000";
- JBI's hand poses (`M.PAC`): its pose ids are its own, so the hand events are left out and the
  fingers keep the ring pose.
Props are 2011's own objects (mapped by id), so nothing else is needed for them.

Detection (`detect_lone`, all stock entrance pacs of all games checked; 2K16 / 2K17 sampled):
- PS2: the DPAC's first group - `ENMT` JBI, `EVNT` SvR 2006, `EVET` with named rows SYM; numbered
  `EVET` then `ENTR` with an `EVTM` row SvR 2004, else HCTP.
- X360, after svrmod for xcompressed pacs: `HSPC` 2K17; an EPAC scene pac 2K16 (some 2K17 pacs are
  the same format, converted the same way).
- EVZ tracks: an EVP with ~16.6 K bytes per cast object (788-byte slots), or an xcompressed EPK8 whose
  script group is `EVP2`, is 2K15; other xcompressed pacs WWE '13 (EPK8) / 2K14 (EPAC); plain ones
  WWE '12.
- EVT tracks: the EVP size % 4480 (one object = 21 x 212-byte slots + 28) is the header size: 240 or
  a YMKs bank SvR 2010; 160 SvR 2007 / 2008, split by the EVT event-size table (op 127 = 0 in 2007);
  176 SvR 2009 / LoW (LoW sizes op 72).

## Testing a converted entrance

Put the pack in a test superstar mod's `moves/` with `entrance=<n>` and run a 1-on-1 test match
(SVR2011_TEST_MATCH). Check the log for `match profile: ... entrance <n>`. Whoever enters **second**
gets a shortened entrance in this test route (seen with stock Randy Orton too), so judge the mod
only when it enters first (re-run if the order flipped).

## Pyro

`entrance_port.py convert` adds the entrance's own particle set for svr10 / wwe12 / wwe13 / wwe2k14
(`pac/prtcls.pac` PCE6 / LPE6 `<0NNN>`, plain PACH; 2K14 has no LPE6: PCE6 is used twice) as
`pacentry prtcls.pac PCE6|LPE6 <0nnn> pyro/...`; `--no-pyro` leaves it out. The game's move-pack
overlay (src/move_packs.cpp BuildEntryPac, format "movepacks 10") writes pacentry lines into EPAC
pacs too (4-character names, u32 count x 3, entries packed only when the group's own are).
Checked in game: 2010 411 (Cena PPV) as 494 - `move packs: prtcls.pac: 2 entries added`, the
copied prtcls.pac loads, the entrance plays, no crash. A visible difference against the same run without the set was not
caught (screenshots every 1.5 s looked the same; short bursts fall between them).

How the game finds it (code, so no table is needed): `sub_8275F548` (entrance setup) loads, for a
wrestler without a created entrance, particle kind 0 with the number from the loaded entrance
script (`lhz 0(*(entrance + 12))`, the EVP's own number, which the converters set to the new
number); created entrances load kind 5 (`/PCCH/%04d`) per part number (`sub_82756880`), and kind 7
the arena's set. `sub_8275F490(kind, n)` -> `sub_82152518` builds the request, `sub_82152B20`
formats the name from the table at 0x82D60EC0 (kind 0 = `/PCE6/%04d`, 1 `/PCOT`, 7 `/PCCH` ...),
`sub_826A17D0` checks the entry exists, `sub_826A31B8` loads it. So an added `PCE6/<0nnn>` is
loaded for entrance nnn; whether every source game's emitter ids match the converted EVT's
events is checked per game (2010 -> 2011 sets are byte-identical for shared entrances).

## Hands (fingers)

2011's entrances do not switch hand shapes by event (no op 21 in any stock entrance): the entrant's
fingers have their own tracks beside the body y0 - y2 = right hand, y3 = left hand (y1 = face) - one
record per frame in the hand-pose library's layout (15 sub-records [k][count][s8 x][s8 y][s8 z], k 0-14
left / 15-29 right; docs/HAND_POSES.md), the first with the event block `40 05 00 00 00 00 89 00 01 00 00`.

| converter | fingers |
|---|---|
| SvR 2007-2010, LoW (and their PS2 discs) | the source's y1-y3 tracks are kept |
| HCTP, SvR 2004, Shut Your Mouth, Just Bring It | made from the source's hand events: op 16 `10 ss iiii` (ss 1 left / 2 right, pose ids = 2011's 0-105 / 200-305; ss 0 = face) -> `tools/hand_tracks.py` builds y2/y3 per 2011 segment from the 2011 game's pose library (m.pac MVMT/STAT/99, `--game-pac` needed), 5-frame blends. Goldberg 3047: 10 tracks from 8 events, played in game. Just Bring It: its own pose library (M.PAC MOT/PTS) mapped to the nearest 2011 pose by finger curl |
| SvR 2006 | its y2/y3 keys are 2011's finger values in joint-mask groups (`0x78` right / `0x79` left `[op][count][u16 mask][s8 xyz per bit]`, plain 5-byte sub-records otherwise): `tools/svr06_hands.py` expands them losslessly (812 / 812 tracks of the 93 entrances; vs SvR 2007's re-releases ~1 step) |
| WWE '12 - 2K17 | the Biped finger bones retargeted onto 2011's 30 finger joints: `tools/finger_retarget.py` (hand-frame chain, fixed ch246 -> ch104 hand alignment, `bind * Rz Ry Rx` as the library, solved parent first on the quantised chain); vs 2011's own 107 Kane / 101 Austin: fist / open agree 97-99% of frames, median 1 degree. Bind tables are WWE '13's (assumed for '12 / 2K14-2K17) |

The Mod Maker's future entrance preview reads y2/y3 the same way (ModMakerUIRework).

## The global object (lights and effects)

Tracks x 10..14 go with slot 0, 60..64 with slot 5, 110..114 with slot 10; y 52..59 are moving lights, y 50 holds op 137. WWE 2K14 conversions now
use the entrance's own tracks (`wwe13_entrance.global_track`), not a stock donor's. A donor changes the look of the whole entrance: with Triple H 102's, Goldberg
361 goes nearly black. The other games keep the donor until they are played (`OWN_GLOBAL_GAMES`).

Record: u8 hold (that many skipped frames follow; at most 0x5f), 3 f32 LE position, 3 s16 LE angles, 3 u16 LE colour. Position = source x 100, from y 50 x 10;
colour = the source's second channel x 4096; angles: zyx, 2011's matrix = the source's x `GLOBAL_AXES`. Source events: 87 starts (nothing in 2011), 88 / 256 end (op 1, 00), 208 = op 137.
Checked against the entrances 2K14 and 2011 share, tracks of equal length only: position exact to the 90th percentile, angles within 115 / 65536; other differences look re-authored.
