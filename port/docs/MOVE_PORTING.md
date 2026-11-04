# Porting moves to SvR 2011

How a move from another game becomes a move SvR 2011 plays, and the rules each fix taught. The converter is
`port/tools/moves_port.py` (`convert --game G --src <disc> --item <move id> --out <dir>`); the game merges its
packs into `Mods/PacOverlay` at start (`port/src/move_packs.cpp`). The Mod Maker's modder how-to builds on this
page (`port/docs/MOVE_MODDING.md`).

## 1. What a 2011 move is

- **Motions** in YMKs banks, keyed `(id, x, y)`: `x` the phase or variant (0 the move; 90-92 reversal branches),
  `y` the track - 0 the attacker, 1 the victim, 17/18 ropes, 30-32 extra parts, 50-57 cameras, 60 props.
  Every move has a keyframe copy in `m.pac` (`MOT/BM..`, `MOT/RR..`, the `MVMT/<category>` menu banks) and a
  **packed copy in `mpsp.pac MOTP/GAME`** - the one a match plays.
- **Records** in `misc.pac`: `MOVS/WAZE` (160 bytes: the first 16 the category bits that make it selectable),
  `WAZA/DATA` EXH (per-key move data), sound events, MBD.
- **Events** inside the motion frames (an op byte, its length from the bank's 256-byte table at 0x10). They carry
  hits, the face, the follow-up motion (op 49), timing and synchronisation. A ported move takes its events
  from its **template** (a 2011 move), retimed to the new length.

## 2. Rules

### 2.1 Frames and keys

- **Keyframe copies (BM/RR/MVMT): a key every frame, count 0** - the key at frame f is the pose of frame f+1.
- **Packed copies (MOTP/GAME): a key every 5 frames, count 4** - the key at frame f is the pose of frame f+5
  (2011's own: Attitude Adjustment 5010, 265 frames, 2971 bytes). A packed copy keyed every frame (4x the
  size) froze both people about 60 frames in while the motion's frame ran on past its end (2K15 F-5).
- A bank directory's `frames` = the motion stream's frame count.

### 2.2 Time

- **2011 plays a motion key every 2 match frames** (30 keys a second: the forced Last Ride, 265 keys, ran 526 match
  frames; the person's motion frame at +24 counts match frames). **2K14/2K15 YANM blocks have twice 2011's keys**:
  2K15's Attitude Adjustment 2 is 532 source frames, 2011's 265 - the converter samples every second one.

### 2.3 Where the two people are (2K15, WWE '12, WWE '13, 2K14, 2K16, 2K17)

- **The victim (block person 1) starts at the origin; the attacker (person 0) is stored in its own frame:
  turned 180 degrees about the vertical and 1500 root units down -z.** Turned into the victim's frame
  (`w13_retarget.facing_frame`, root x -> -x, z -> -1500 - z, facing and IK together), 2K15's AA 2 - a copy
  of 2011's 5010 - matches 2011's attacker to a few units. Unturned, the victim played its part metres away.
- **WWE '12 is laid out the same way** (found 25 Oct): its moves that 2011 also has start, turned, as 2011's do -
  Gory Bomb 2 0 units apart (2011: 100), Attitude Adjustment 2 142 (229), Tombstone Piledriver 1 376 (329) -
  and about 1500 apart unturned. Every WWE '12 grapple ported before this started its pair metres apart.
- **2K14, 2K16 and 2K17 too** (26 Oct, porter-2k17 6e95c654): before, their grapples started the pair metres apart
  (a player's 2K17 Blue Thunder Bomb, Sami Zayn). A move ported with 2.1.5 or older from these games has to be
  ported again.

### 2.4 Ids

- **Below every template bank's last id.** A bank's directory is sorted and ends at its highest id (16000 in
  `MOT/BM00/0` and its kin); the merge inserts only below it. 2K15's AA on record 17013 went into no bank and
  never started. Before 2.1.6 the game then skipped the **whole bank** - every pack's motions in it; now only
  that motion, with "id past this bank's last ... left out" in the log.
- **Not 9000-9999 for a keyframe move.** The game treats 9000-9779 as streamed (YMBs) motions by its flag
  table; a keyframe move there plays through the wrong path.
- The converter picks a free record (one 2011 keeps without motions) within those limits; `--id` chooses.
- **Not 16000 and up outside the template's thousand** (the game never starts it: move_packs.cpp NeverStarts),
  **nor a DLC move**: 2011's DLC brings 282 records the disc keeps cut back to life with its own motions and adds
  18 (`tools/dlc_move_ids.json`, 303 ids) - 273 of the disc's "free" records. One rule for every picker:
  `Game11.free_ids` (free_record, moveset_port, the Mod Doctor's renumber); a passed `--id` outside it is picked
  again (4 Nov, reports db3291 / d97b88 / e47af7 / 3b5056).
- **A move on YMBs banks (a submission) takes minutes**: 2K15's Kimura Lock solves 5 hold phases in ~172 s. The
  move-set port's limit (120 s a move) gives such a move `YMBS_EXTRA` (480 s) more - before, it was stopped and the
  base move kept ("missing from CREATE A MOVE-SET").

### 2.5 The template

- **The template brings its events, cameras, props and rope tracks.** Pick one of the same situation: a rope
  move's events wait on the ropes - 2K15's F-5 on Throat Thrust 4 (a rope move) froze in its carry.
- Coded categories (2K14+: `C000`, `E125`, ...) carry no meaning by name: the converter maps each to the 2011
  categories its moves' 2011 namesakes sit in (a vote - 2K15 E125/E127 standing front grapples, E136 behind,
  E160 diving, M050 ropes, M060 apron, M090 tag team), and uses the 2011 move of the same name when there is one.
- **Ids: not 16000 and up** outside the template's own thousand - a move there never starts (WWE '12's Octopus
  Stretch on 17071 dropped both people to stance; on 5198 it played).
- **The template is never a reversal unless the source is one** (WWE '12's G.T.S., filed under its STAT, got
  Ankle Lock Reversal). A two-person move filed under STAT (strikes and reversals), or a poor fit in its
  category, also looks among the standing grapples STN1 / STN2; the better fit wins (G.T.S.: Powerbomb 4).
- **The template's situation must match** (WAZE +0x73: 0 standing front, 9 dives, 10/11 corner, 13/14 the
  victim seated on the turnbuckle): a standing grapple doesn't get a dive or corner template unless its source
  category says corner (`CORN`), nor one filed under the rope, apron or corner groups (MVMT ROPE / APRO / CORN)
  unless asked for. Before, the Blue Thunder Bomb got DDT 14 and the Dragon Special a rope template; now Gory
  Neckbreaker 5015. Moves ported before keep their templates (16 checked).
- **Dives onto a lying victim are one-person moves in 2011.** The Frog Splash 3555 and its kind are the
  attacker's motion alone (x 0 y 0; the victim's x 50 y 1 is the miss) - the victim takes the hit with the
  game's reaction. A source whose attacker starts high (root y < -1150, the top rope) and whose victim starts
  lying (root y > -400) gets a one-person CORN template, picked by where the attacker ends, and its victim
  track is left out (2K15's Superfly Splash: Leap of Faith 3591; before, Corner Head Scissors Flip - a
  two-person corner move - and it was called off in 5 frames). Such a move can't be forced with
  `SVR2011_TEST_FORCE_MOVE` (it starts a pair: the victim waits in a motion with no track of his; a start with
  no target crashed the game - a null target write).
- **A dive laid out as 2011's** (WWE '12's Shooting Star Press 4 / Frog Splash 1, 2K15's Superfly Splash): block
  0000 0000 is the attacker's flight alone, 0030 / 0040 / 0050 the pair's branches (hit / miss ...). For a corner
  move the flight goes to x 0 and each branch to the template's x 30 / 40 / 50 (before, the first pair block -
  0030, a landing - was taken as the move).

### 2.6 Face events

- Op 68 (`[68, n, frames]`) picks face n-1 from an 18-entry table; anything past 17 read wild memory and killed
  the match thread - the "frozen pair" (report 001255). The game skips such events since 2.1.6.

- **In-game each person's root plays in its own placed frame**: the bases (+288) stay where the paired start put
  them (the F-5: 1.5 m apart, unmoved), and a shared-frame viewer has to place the attacker's root turned to face
  the victim to match what the game draws.

### 2.7 Phases (multi-part moves)

- A source move with several block pairs (a submission: hold, struggle loops, escape) keeps one phase per pair:
  2K15 phase n (decimal in the block name; 0090-0092 are reversals) feeds 2011's x n of the template. Before,
  every x got phase 0's motion.

### 2.8 Submissions (YMBs templates)

- 2011's submissions (Ankle Lock 3 21305, STF 21350, Sharpshooter 21302) keep their body tracks in YMBs banks
  (MOT/BM02/4, MVMT/ANAS/17, MOTP/GAME/1a); the converter solves the source into YMBs channels per x
  (ymbs_retarget). The stock hold starts from a lying victim (the attacker walks to the feet, sits in, HOLD
  prompt) and the game's submission state runs the loops after x 0.
- The attacker of a ground hold is NOT turned (Patriot Lock: 1455 units as stored, 2955 turned): the turn is
  chosen per move by the closer start (2.3). Turned without the shift, a YMBs attacker held one pose.
- YMBs tracks are moved so the source attacker starts where the template's does (as the keyframe path).
- **A hold with phases is a submission**: a source with two-person phases 1, 2 and 4 (and more) under 30 (hold, struggle loops,
  escape, tap; 30 and up are a dive's branches, 90-92 reversals) gets a 2011 submission template (a YMBs move in MVMT/ANAS, most phases in common, then the
  closest length) before the category vote - a coded category mixes holds with other moves (2K15's M013 is
  mostly reversals: its vote said STAT and the Patriot Lock got Mule Kick Reversal 2; now Single Leg Boston
  Crab 21308).
- **Open: the game tells a submission by its id** (4 Nov): in a hold = the motion in 21000-21499 (sub_821B57F8 for
  an id, sub_821B58F8 a fighter's current one), standing 21000-21099 (sub_821B2540), the sub-kinds 21100s / 21200-249 /
  21250-299 / 21300-349 / 21350-399 / 21400-449 / 21450-499 (sub_821B2560 / 5818 / 5838 / 5858 / 5878 / 5898 / 58B8),
  a front grapple 4500-5549 ... (sub_821B9E80), submissions among the moveset's slots 101-103 / 107-109
  (sub_821BBAF0). Every 21000s record is a stock submission, so a ported hold sits elsewhere: its victim stands up when
  x 0 ends. Tried (branch kimura-ground, not merged): those four tests answering for the move's wazecopy source - a
  forced hold then stayed in its hold, but one the CPU applied (SVR2011_TEST_SLOT_MOVE + SVR2011_TEST_MOMENTUM) never
  ended (230 s; the stock Crossface the same way ended in 40 s) - a stuck match, worse than the stand-up: taken out.
  A forced start skips the game's submission state (stock holds loop forever too): taps and breaks need a CPU-applied
  hold.
- **Standing or on the mat**: choose_submission takes the template whose victim is as high as the source's at the
  start and in the hold (x 2; standing holds keep him at -7..-11 model units, mat holds -1..-5). 2K15's Kimura Lock
  (standing start, on the mat: -8.8 / -3.3) got Bearhug, a standing hold - now Crossface 21004 (-8.8 / -1.5).

## 3. Testing a ported move

- **A forced move only animates when someone in the match has it in their moveset** - the match loads the
  motions of its people's movesets. Put the new id in a test superstar's `moves.txt` first.
- `SVR2011_TEST_FORCE_MOVE=<attacker>,<victim>,<every s>,<motion>` starts the move; `SVR2011_TEST_MOTION_LOG=1`
  logs each person's motion changes; `SVR2011_TEST_FRAME_SHOT=<f.ppm>` with `_SHOTS=<n>`, `_SHOT_AT=<match frame>`
  and `_SHOT_EVERY=<k>` saves n frames k apart (contact sheets); `SVR2011_TEST_STATE_LOG=212,24` logs a person's
  motion and its frame (+24, a float) as they change.
- **Placing the attacker**: `SVR2011_TEST_FORCE_PLACE=<x>,<z>,<yaw>[,<y>]` puts him there in the ring (y -12 the
  mat, yaw in radians) before each forced start - rope, apron and corner moves (forced where the AI happens to
  stand, the Rope Hung Suplex ran outside the ring and both sank under the mat).
- **A forced move is never reversed**: `SVR2011_TEST_FORCE_MOVE` starts it with sub_82191AA8, past the point
  where the game decides a reversal (the stock F-5 5010, forced on a person tapping RT, was never reversed).
  Reversals can only be seen in moves the CPU picks itself, and even then rarely - see 5, reversal test aid.
- **CPU use (e) and reversals (d)**: no forced move - a CPU vs CPU match with the ported moves in the CPU
  superstar's slots, `SVR2011_TEST_MOTION_LOG=1` and `SVR2011_TEST_CPU_LOG=1` (reversal rolls). In
  `SVR2011_TEST_MATCH` only person 1 is a CPU when person 0 is the first named: put the test superstar second.
  Put front grapples in the front grapple groups C/D/E (move block 0x98-0xB2, not the fixed 0xB4-0xBA); the
  rear slots 0xBC-0xC2 are rarely picked. Damage per CPU hit is in the CPU log's `damage n in k hits`.
- **Dives**: `SVR2011_TEST_FORCE_DIVE=<every s>,<set-up motion>,<dive>[,<dive>...]` - the attacker floors the victim
  with the set-up (a paired takedown, e.g. 5000), then at +600 frames is put on the top turnbuckle (28.8, -21.9,
  -28.8, facing the victim) and starts the dive as a paired start, the victim restarted lying where he lies.
  Results 26 Oct: the stock Frog Splash 3555 dives into the ring onto the victim (the control); 2K15's Superfly
  Splash lands on him, WWE '12's Shooting Star Press across his legs; WWE '12's Springboard Moonsault 5094 rises
  and flips but lands about 2 m past the victim, who isn't hit (a springboard starts from the side ropes - the
  aid's corner start may not be its place; later: a side-rope start for springboards in the aid, then judge the landing again).
- **Where a forced move starts** (28 Oct): the paired start keeps or redoes the places by the move, and the facing
  it leaves can differ from the one written (0.17 for a written -0.79: the target then landed outside the ring), so
  `FORCE_PLACE` and the dive aid write the places and the facing again right after the start. The stock Diving
  Double Axe Handle from the turnbuckle onto a target 6 units in then lands on him - before, every corner and dive
  result depended on where the start happened to put the pair. `SVR2011_TEST_POS_LOG=<frames>` logs both people's
  base, body and facing every 4th frame after a motion change.
- **Ending a match**: `SVR2011_TEST_LOSE=<person>,<s>` - at <s> seconds the person loses to person 0 (victory
  scenes). `SVR2011_TEST_EVP_LOG=1` logs every scene script the game opens (`/EVPx/<n>`, with the caller).
- **Hidden test games**: `SVR2011_WINDOW_HIDDEN=1` (tools/test_guard.ps1 sets it) - the window is never shown;
  use frame shots, window screenshots of a hidden window are black.

## 4. Per game

### WWE 2K15
- Disc `WWE 2K15.iso`; moves in `mot07/mot08.pac` ZMVM (YANM records), records in `createmode.pac MOVS/WAZE`.
- Blocks `<phase><person>`: `0000 0000/0001` the move's attacker and victim for most grapples; `0030..` for
  springboard / diving moves.
- Pair layout 2.3; frames 2.2.
- Status of the hard moves (criteria a-g: sync, landing, damage, reversal, CPU use, no stuck pair / crash at
  60 fps, survives save / load):

  | Move | id | a-c | d reversal | e CPU picks it | f |
  |---|---|---|---|---|---|
  | Attitude Adjustment | 5743 | ok | pending tooling | yes (x2) | ok |
  | F-5 | 7564 | ok | pending tooling | not yet (forced: ok) | ok |
  | Triple German Suplex | 5188 | ok | pending tooling | yes (x3) | ok |
  | The Last Ride | 5872 | ok | pending tooling | yes | ok |

  (d) so far: each carries its template's reversal branches (x 90/91/92; the Last Ride's template has x 90
  only), and in 6 CPU picks at the default 55 % reversal chance none was reversed, none stuck (527-537 frames,
  the victim down after).
  (g) passed for the F-5: CREATE A MOVE-SET -> CREATED SUPERSTAR -> a CAW -> SPECIAL MOVES -> SIGNATURES ->
  `MOD: F-5 1`, ACCEPT CHANGES (saved); a new game on that save, a match with only that CAW holding the move
  (`SVR2011_TEST_MATCH=people=CAW0,...`): the F-5 played 9 times, the victim down each time, no stuck pair.
  The moveset keeps the id, so every pack move saves alike; menu shots of a hidden game come from
  `SVR2011_TEST_SHOT_REQUEST`.
- Needs a tag match: Batista Bomb & RKO (a double-team move) - not tested in a singles match.

### WWE '12
- Disc `WWE 12.iso`; its categories are 2011's names (CATS). Its STAT group 60051-60067 (G.T.S., Pedigree, World's
  Strongest Slam ...) are special versions of finishers 2011 has (G.T.S. 1 = 4574): not new moves.
- Pair layout as 2K15 (2.3, turned): its Attitude Adjustment 2 ported onto 2011's own 5010 plays like the stock move
  turned (lock-up, lift, carry, drop) and metres apart unturned (contact sheets w12cal_on / w12cal_off).
- Dives laid out as 2011's (flight 0000 0000 + branches 0030/40/50): see 2.5.
- Hard moves, forced in the ring (hidden, 25 Oct): Koronco Buster 1, Tilt-A-Whirl Hurricanrana, Rolling Yoshi
  Tonic, Snake Eyes 1 play through, victim down, no stuck pair; LeBell Lock / Octopus Stretch (submissions) start
  and end without the hold loop (the parked submission issue, 2.8); Shooting Star Press 4 / Springboard Moonsault
  (dives) can't be forced (2.5). Sync looks right where the camera catches it; d / e / g not run yet.

### WWE '13
- Extract `WWE13 Extract` (xcompressed pacs: `--svrmod` = the Mod Maker's svrmod.exe). Most of its moves share names
  and keys with WWE '12 (same dives, Tilt-A-Whirl ...); the new ones are its apron group APR1, corner and BASE.
- **APR1 (grapples by the apron) are standing grapples in 2011** (CATS APR1 -> STN1, 27 Oct). On 2011's APRO
  templates (Apron Guillotine Drop 10885) the template's own events - the victim's fall from the apron to the floor
  - put both people at floor height: Canadian Backbreaker 1 sank under the ring wherever it started (its tracks
  have no drop: both keep normal heights). As standing grapples Canadian Backbreaker 1 (template 5118, 2011's
  Emerald Fusion: lift, rack across the shoulders, drop) and Emerald Fusion 1 (Spinning Side Slam 5037: lift onto
  the shoulder, slam) play in sync in mid-ring. APR2 stays an apron move (warned).
- **Dives (27 Oct)**, three porter fixes found here:
  - the flight goes the template's heading: the source is turned about the vertical so its attacker's root travel
    points the template attacker's way (`heading_turn`; Corkscrew Splash 180 degrees, WWE '12's Springboard
    Moonsault 161, Diving Double Axe Handle 1 -39 - a flip alone left that one leaving the ring at an angle);
  - **the game carries a dive to its target** (28 Oct, `SVR2011_TEST_POS_LOG`): in a stock dive the attacker's own
    root moves a few units (Diving Double Axe Handle 3538: 52 root units) and the game slides his base (+288)
    toward the target during the flight. A ported flight kept its own travel (1600-3100 root units) on top and
    landed metres past or beside the target. On a one-person dive template the flight's horizontal travel is
    scaled to the template's (`scale_travel`; the heights - the jump - kept), the branches likewise;
  - the template must have every branch the source has (`need_x`): Leap of Faith 3591 keeps its miss in x 110, so
    a dive the game judged a miss flew a second time with Leap of Faith's own miss flight (WWE '12's Shooting Star
    Press on Diving Headbutt 3 3570 flies once; the Corkscrew: see the name word below);
  - the flight-block path (2.5) only for a lying victim; a dive onto a standing one from up high takes 2011's move
    of the same name (Diving Double Axe Handle 1 -> 2011's Diving Double Axe Handle 3538; before Roll Up).
  - **a dive lands when its root reaches the mat** (28 Oct): a stock flight ends in a drop past the mat (3538's root
    -1579 at the top, then +435 ... +999 a frame to 7000 on the last key) and the game lands the dive when the root
    gets there. WWE '13's Diving Double Axe Handle 1 ends its flight as high as it left (its own engine brought the
    base down) and hung at turnbuckle height; 7000 copied onto its last key alone froze it in the air. A flight
    ending as high as it left (within 3 units) gets the template's drop after it, the last pose held (`fall_tail`);
    the template's events of the drop go with it. The packed copies' root is 12 bits x 2 (+-4096): past that a key
    is the bones without a root plus the s16 root on its own, as 3538's packed copy (`ymks.build_motion`).
  - **a dive's template by a rare name word** (`dive_kin`: a word at most 2 of 2011's dives have): WWE '13's
    Diving Corkscrew Splash -> Diving Corkscrew Body 3624. On 450 Splash 3559 (upright at the start; the corkscrew
    starts flipped) the game dropped its base to the mat in 16 frames, never carried it to the target, and it
    landed a torso short. "Splash" is in half of them: not a match (the 450 stays on 3570); nor "star" (a
    Shooting Star Press is no Five Star Frog Splash). WWE '12's Springboard Moonsault -> Diving Moonsault 2 3621.
  - **no floor dive for a dive in the ring** (`ringside=False`): Leap of Faith 3591 flies out to the floor (its
    flights x 110 / 111, no x 50). WWE '12's Springboard Moonsault had it (closest length), was turned 161 degrees to
    its heading and, from the turnbuckle, flipped away from the victim and out of the ring (Release Testing F11).
  - a dive given as the template (`--template`, the Mod Maker's pick) is ported as a dive: before, its flight,
    branches, turn and travel scaling were left out and the source's x 30 took x 0.
- Khali Bomb plays through: throat grab, lift, hold, drop - the victim lands about 2 m in front (TEST_VIEW didn't
  hold: the move's own camera).
- Open:
  - Stratusfaction 1 (situation 11 corner, template Springboard Bulldog 5757): forced, the stock 5757 puts its
    attacker at the corner first (28.8, 28.8) and he climbs and bulldogs the victim; the port isn't put there and
    the victim does the climb while the attacker stands - though the port's attacker track (y 0) is the flier
    like the template's. Not solved. (28 Oct, POS_LOG: the port's motion matches 5757's; at its start the game
    moves the *victim's* base to a corner (-28.6, -28.8, yaw 0.79) where the stock moves the attacker's. Every
    WAZE / WAZA record is 5757's (wazecopy): the free id, 3247 in a thousand of other moves, is the suspect - an
    A/B with 5007, in 5757's thousand, is the next run.)
  - Springboard Hurricanrana 4 (APRO, Cut Down) plays in sync but on the floor, not off the apron.
- **Placing for a forced move: place the target too.** The paired start puts the attacker relative to the
  target: a target the CPU had thrown out of the ring took the pair there (FORCE_PLACE's target fields).
- Test runs: `scratchpad mv13run5.ps1`-style batches run each again when the test match pick adds a third person
  (seen 3 times in 10 runs, 27 Oct), and wait while a build runs - a starved test game took 215 s to its test
  match and never reached the move. The porter's `MOVES_PORT_TRACE=1` logs both people's root paths and each
  phase's attacker heights.

### SvR 2007 / 2008 / 2009 (31 Oct, from the Anthology study)

Their banks are 2011's own format, their move ids and WAZE layout too (situation at +0x73, the same values), so
the porter copies the source's tracks instead of retargeting (`convert0789`). What a match needs, learnt by
comparing with Yumboy11's SVR Anthology (numbers only - `runs\anthology_moves_report.md`; nothing of it is in
the repo):

- **Every track goes into the match copy.** MOTP/GAME holds cameras (y 50-57), ropes / turnbuckles (10-21),
  parts (30-32) and props (60) as well as the people - 867 of 2011's 988 moves with a camera keep it there. Only
  the people went in before. Those other tracks keep the unpacked conventions in MOTP; only y 0 / 1 are packed.
- **SvR 2009 ships its own packed match copy** (`MOTP/GAME/<child>`), which 2011's packed codec reads, keyed
  every 1-4 frames (2011: 1-5): it goes into 2011's MOTP as it is. 2007 / 2008 have none (their `MOT/GAME` is
  keyframes): their packed people are built as before.
- **No pack without a match copy.** A template needs a keyframe (YMKs) bank in MOTP/GAME; the convert fails
  otherwise. Ankle Lock 1 on Powerful Knee Strike 5090 got a menu bank only: forced, both people stood frozen.
- **The move's own record.** All of the study's moves land on a record 2011 keeps without motions (a cut move):
  its situation, damage and ratings are the move's, only its 16 category bytes are empty. The pack sets those
  (`waze`, the template's bits) instead of copying the template's record (`wazecopy`), which started Frog Splash
  Pin as a running move (16 instead of 9), Iron Claw running too, Running STO in situation 1.
- **Template by situation and category first**, then start gap and length: the source's menu category (2009's
  STND / DIVI / ROPE ... -> STN1+STN2 / CORN / ROPE), the move's situation. Running STO: Spear 3 (was Roll Up).
- **Holds (x 101-103)** take banks and records from a keyframe hold - 2011 has three usable, all ground holds:
  Sleeper & Body Scissors 6019, Ankle Lock 2 6181, Cloverleaf Pain 6212 - and the category bits from the
  template of their own situation. Forced, Ankle Lock 1 on 6019 / 6181's banks takes Cena down and holds the
  ankle; on a standing grapple's banks the pair let go after the takedown.
- **Event ops 192 / 193 are kept**: 2007-2011 all size them 2 / 3 (the old "2008-only" drop was wrong). 2011
  ignores 192 (sub_8239D750 skips it); 193 is a sound / voice cue (u16 id), kept only for an id 2011's own match
  copies play (`Game11.cue_ids`; SvR 2009 has 6 of them in all its match copies, 3 of 4 ids shared).
- **An in-ring dive leaves out the source's floor flights x 110 / 111** (2011 keeps them for Leap of Faith alone
  and plays x 110 as the miss: SvR 2009's Frog Splash Pin flew out onto the floor) and takes the template's miss,
  x 50, which the source doesn't have (without it the attacker hung at the corner after the flight).

In-match (forced, hidden; `scratchpad anth\an_run1-5.ps1`, sheets `runs\an_*_sheet.png`, n = this porter, b = 2.1.8's):

| Move | Before | Now |
|---|---|---|
| Chokeslam 5 (corner, placed) | plays | plays the same, all 7 tracks in the match copy |
| Ankle Lock 1 (submission) | frozen, never animates | takedown + ankle hold, every start |
| DDT 8 (7 cameras) | pair frozen outside the ring after one start | plays in the ring with its cameras, every start |
| Atomic Leg Drop 1 (rope) | - | plays; its own low camera at the end |
| Frog Splash Pin (dive) | miss flies onto the floor | flies off the corner, ends in the ring - the dive aid's victim is far away (stock 3555 misses there too): not proven |
| Running STO (running) | - | forced from standing the attacker drops out after 16 frames (a running start is needed); the CPU didn't pick it in 180 s: not proven |

## 5. Parked: a reversal test aid

Wanted: `SVR2011_TEST_REVERSE=<person>` - the next move on that person goes into its reversal branch (x 90-92),
for a contact sheet of the switch from the ported x 0 into the template's branch (the likely bug: a pose pop
where a reversal starts from the ported pose).

Known:
- Reversal branches are tracks of the move's own id at x 90 / 91 / 92 (the porter copies the template's).
- `SVR2011_TEST_FORCE_MOVE` calls sub_82191AA8(attacker, victim, motion), which starts x 0 directly - the
  reversal decision is upstream of it (the grapple's move choice), so forced moves are never reversed.
- The CPU's reversal roll object is made by sub_82835150 (PROB at +8) and rolled by sub_82835200 (from
  sub_828513D8), but `SVR2011_TEST_CPU_LOG` never logged a taken roll for a grapple - stock moves included -
  so grapple reversals are decided elsewhere.
- The test match's pick is racy: the scripted pad often controls no one (both people CPU); setting the slot's
  controller byte (slot -8 = 0) did not make a person the player's.

Where to look: the code that reads a motion's x - the YMKs lookup by (id, x, y) and its callers; find the
caller that asks for x 90 (log every lookup with x >= 90 in a CPU match: stock reversals happen) and walk up to
the decision; then start that path from the test aid.

