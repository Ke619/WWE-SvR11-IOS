# Problem reports

We don't ask players for their saves or mods. What a report needs has to be in
the "Report a problem" zip (launcher `report.c`, `SvR2011 Launcher.exe --report
<game folder> [description]`) and in the logs.

## The zip

| file | |
|---|---|
| `report.txt` | the player's words, then the launcher version, Windows version, date |
| `logs/` | the newest game's log parts (repeated fault lines condensed) and the two games before |
| `crashes/` | the three newest crash reports (host stack, guest registers) |
| `logs/updater.log` | the updater's runs (each step of an update / repair and how it ended) and the launcher's update and file-check messages (`update_log.h`; its last result also goes in report.json as `updater`) |
| `svr2011.toml` | the settings, account token and password removed |
| `saves_check.txt` | each `NNCreateSuperStar.cas`: size, save slot, the attires with a picture, its three checksums and SaveData.dat's copy for its slot, "agree" or "DON'T AGREE"; SaveData.dat's own checksum. Never the saves |
| `mods_text.txt` | each superstar mod's `manifest.txt`, `moves.txt` and `threads.txt` (64 KB each, 512 KB in all). Never models or pictures |

IP addresses are blanked in the logs (x.x.x.x).

## Log lines for reports

- `caw saves: NNCreateSuperStar.cas - slot N, attires with a picture 1+2, checksums agree`
  (`src/caw_check.cpp`, at start, one line per Created Superstar, then a count). A
  warning when the three copies or SaveData.dat's record differ, or the size is
  wrong: the game calls such a CAW damaged or leaves it out (at start it stops on a
  dialog). The value itself isn't checked: the port's logo cache keeps part of a
  CAW outside the file, so the on-disk byte sum doesn't match for every good save.
- `match picks: person 0 CAW slot 3 attire 2, person 1 id 110 attire 1`
  (`src/test_match.cpp`, as the match is built): who was picked, with which attire.
- `match: rule .., arena .. - <names> (person n, team t)`: the match's people.
- `match state (<why>): ...` then one line per person: name and superstar id, its
  mod folder or "not a mod", and its last move (id, moveset slot, whether a move
  pack's / the game's / missing / without a move record, how long ago)
  (`src/match_diag.cpp`). Logged once when the world update is stuck for 3 s
  (with the guest threads' back chains), or when the match clock runs 30 s with no
  wrestler asking for a move.
- Mod Maker session log (`<game>\logs\modmaker_*.log`, part of the Mod Maker's own
  report): `CAW pictures: <file> (slot N) attire 2: ... checksum A -> B; SaveData.dat
  record ... file sum ...` for every CAW pictures save (its first versions go to
  `Saves\.backup`), and `model preview: N models (...), M meshes, V vertices, T
  textures; textures the meshes name but the model lacks (drawn black): ...` for
  every model the 3D preview loads.

## A signed-in account and a Discord name (2.2.1)

Every report and suggestion needs the player's online account and their Discord name: the launcher's Report a
problem and Suggestions tab, the Mod Maker's Report a problem, the game's REPORT A PROBLEM (PC and Android), and the
app's report and idea forms. The game's crash messages send players to these forms; there is no other sender.

- The forms send the saved sign-in as `Authorization: Bearer <token>`: the launcher's `s_online_token`, the game's
  `online_token` setting, the Mod Maker reads `online_token` from `<game>\svr2011.toml`, the app from its settings
  (the same file). The token never goes into a report, its zip or a log (the zip's `svr2011.toml` says
  `(removed)`).
- The Discord name goes in report.json's `contact` (private: only the dashboard shows it, as "<account> (Discord:
  <name>)" in the From column, and `reports.py list/show`; never `/api/reports/status` or the receipts). Discord's
  rule, checked by the forms and by the server (`reports.discord_name`): 2-32 characters, lowercase letters, digits,
  `_` and `.`, no two dots in a row, no `@`, no links; capitals become lower case. The forms remember it in
  `<game>\UserData\report_discord.txt` (every PC form reads and writes it; the app too). The game's on-screen
  keyboard types `_` with SHIFT + `'`.
- Signed out, a form says so and offers the Online tab / page (Sign in or Create account). The Mod Maker and the
  game can't sign in themselves: they say "sign in on the launcher's Online tab" (the app's Online page on Android).
- The server setting `reports_need_account` (dashboard, live; on by default) refuses a send otherwise, with a
  `code`: `update_needed` 426 (a release below 2.2.1; dev builds and odd version strings pass), `sign_in_needed` 401
  (no token, or an ended one - a banned account too), `discord_needed` 400. 2.2.0 and older clients show the error
  text as it is: "Reports need 2.2.1: update, sign in to an online account and add your Discord name." With it on,
  `/api/reports/mods/check` asks a client without a sign-in for no mods (its report would be refused). Answers to
  questions, edits and the receipts' status stay keyed by the receipt (2.2.0's open questions can be answered).
  SvR Anthology's apps aren't affected. Off: as 2.2.0.
- Test aids: the launcher's `--report-send` and the Mod Maker's `--test-report-send` answers files take `token=`
  (a test account's sign-in; else the game folder's), `SVR2011_REPORT_ANY_CONTACT=1` skips the form's Discord check
  (to see the server's refusal); `SVR2011_SUGGEST_DISCORD` fills the Suggestions tab's field for `--capture`
  sends; the app's `report_send` answers file takes `token=`. Server tests: `test_reports.py` `accounts()` (a
  private server with `--test-open-accounts`).

## Report a problem inside the game (2.1.9)

MY WWE -> OPTIONS -> REPORT A PROBLEM, and in a match the pause menu's REPORT A PROBLEM (a row above QUIT MATCH;
not in online matches). `src/report_page.cpp`: the launcher's questions and rule ("what happened" needed), a
screenshot of the game from before the menu (or "add pictures later in the launcher": the receipt gets
`"pictures_later":1`), the match being played (from the menus: the last one) with its mods, the main save. Sent
by the launcher's own code (`launcher/report_flow.c` - the zip, the match and mod parts, report.json, the
pictures; the launcher uses it too), app `game`; on Android the app's `GameReport.java` -> `ReportForm.send`.
The receipt goes into `<game>\UserData\reports.json` (the launcher's Reports tab: "In the game").

Pause menu: the screen `sub_824271E8` makes (vtable 0x820183E0) gets a copy of QUIT MATCH's entry with the label
0x0FA0B202 before it, its row count and the cursor's (+36 -> +28) one more; `sub_824068A8(screens, row)` opens
row - 1 for the rows after ours, and A on ours never reaches the game (`ReportPagePad`, XamInputGetState).

Test aids: `SVR2011_TEST_REPORT_TYPE=<file>` types its lines into the text being edited; test runs
(`SVR2011_INPUT_FILE`) send only to a server on this PC (`SVR2011_ALLOW_LIVE=1` lifts it).

## Test aids

- `SVR2011_TEST_MATCH_STATE=<s>`: the `match state` lines once, `<s>` seconds into
  the match clock.
- `SVR2011_TEST_ROSTER_DUMP=1`: each match person's roster record (260 bytes; +0x20
  is the superstar id, +0x22 the name).

## Universe teams and the character select (21 Oct)

- `universe teams: N in this save: ...` when a save comes in (slot, name, member superstar ids, mods by folder),
  then `team K "NAME" made / changed / dissolved - it had ..., now ... (during a match | outside a match)`.
  Universe keeps 70 teams of 270 bytes at save + 0xF5C8 (+0 u16 members, +2 five u16 ids, 0x200 none, +0x50 name);
  a team is also an ally pair (Universe asks "added as an Ally of all the current Team Members").
- `character select: open (screen ...)` when a select screen sets its players up, and
  `character select: player P A on the tile at column C row R` for every A on a tile - where a "stuck select screen"
  report was.

## Two wrestlers frozen in a hold (21 Oct)

- `match state (two wrestlers held in one motion frame for 10 s while the match clock runs (motion M))` - once a
  match, when two wrestlers (people 0-5) keep the same motion (+212) and frame (+24, a float) for 10 s of the match
  clock (checked once a second). Every `match state:` person line now also shows `motion M frame F`.
