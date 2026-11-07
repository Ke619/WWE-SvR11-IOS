# Problem reports API

Report a problem in the launcher, the Mod Maker and the Android app (`ReportSend.java`) sends its report to the
online server - only sends: no client saves a zip for posting elsewhere, and a send that fails offers Try again.
Tests send only to a server on this PC (Android: this phone or its LAN, adb reverse) unless SVR2011_ALLOW_LIVE=1
(Android: the `svr2011_allow_live` extra). The server is `port/server/reports.py` + `server.py` (`api_report`); the clients are owned by the Log File
Reporting work.

## POST {online_server}/api/reports

`online_server` is the players' setting (live: `https://sho-ti.me/svr`, so `/svr/api/reports`). No sign-in is
needed; an `Authorization: Bearer <token>` (optional, not sent by today's clients) only adds the account's name.

`multipart/form-data` parts:

| part | type | |
|---|---|---|
| `report` (or `report.json`) | `application/json`, UTF-8 | required, 64 KB at most |
| `zip` | `application/zip` | optional, 25 MB at most; the client's report zip (already redacted) |
| `image0` .. `image4` | PNG / JPEG / WebP (checked by their bytes) | optional, 8 MB each |

`report` fields: `format` (1), `app` (`launcher` / `modmaker` / `game` / `android`, or SvR Anthology's `anthology` / `anthology-android`, shown on the dashboard's own SvR Anthology page; required), `version` (required),
`platform` (`windows` / `android`), `os`, `problem` (required), `happens`, `what`, `before`, `contact`, `signature`
(the client's hint), `updater` (PC: the last "result" line of `<game>\logs\updater.log` - the updater's last
update or repair and how it ended; `update_log.h`). `what` / `before` are cut at 4000 characters, `updater` at
600, the rest at 200. Other fields are dropped.

Answers (JSON):
- `200 {"ok": true, "id": "20261024-153012-ab12cd", "secret": "<receipt>", "signature": "<16 hex>",
  "seen_before": N}`: N = earlier reports with the same signature (0: new). A client can say "already reported N
  times, thanks". `secret` is the sender's receipt (below): the server keeps only its SHA-256.
- `400` bad input, `413` too large, `429` too many (5 in 10 minutes and 20 a day per address, and per account),
  `503` the storage node is down: `{"ok": false, "error": "<text to show>"}`. The client shows it with Try
  again (the zip is made again; it's deleted when the window closes).

## Receipts: the players' Reports list (2.1.9+)

Each client keeps a sent report's `id` + `secret` in `<game>\UserData\reports.json` (one object a line:
`id, secret, date, app, problem, what`, and `"pictures_later": 1` when the game's form sent it with "add pictures
later in the launcher" - the launcher, the Mod Maker, the game and Android share it; `report_receipts.c`,
`Receipts.java`). The launcher's **Reports** tab and Android's **My reports** list them and ask:

`POST {online_server}/api/reports/status` `{"reports": [{"id", "secret"}, ...]}` (100 at most; 60 lookups in 10
minutes per address; an `Authorization: Bearer <token>` adds the account's reports from other PCs,
`from_account: true`) -> `{"ok": true, "reports": [...]}`, each:
`{id, state, received, app, problem, what}` and, by state, `note`, `version`, `date` (fixed) or `question`, `date`,
`replies` (needs_info / replied), and `edits` (the player's changes), `pictures` (how many it has, sent + added),
`editable` (not done / duplicate). `state`: `received` (no mark), `looking`, `fixed`, `needs_info`, `replied`, `done`,
`duplicate`, or `unknown` (no such report, or a wrong secret - then only `{id, state}`). Never another report, never
its logs, zip, pictures, contact or signature. Reports from before receipts have no secret: they can't be listed.

`POST {online_server}/api/reports/reply` (multipart: `report` = `{id, secret, text}`, `image0`-`image2` PNG / JPEG /
WebP): the player's answer to a question; only while the report is `needs_info` / `replied`, 5 answers a report at
most, 2000 characters, rate-limited as reports, through the name filter. Kept beside the report as `replies.jsonl`
(+ `reply<n>_<k>.png`); the mark becomes `replied` (the dashboard shows it). `409` not waiting for an answer, `404`
no such report / wrong secret.

`POST {online_server}/api/reports/edit` (multipart: `report` = `{id, secret, what, before, contact}`, `image0`-`image4`
PNG / JPEG / WebP): the player adds to a report it sent - text to add to what happened and / or before (4000 each),
a new contact (200), pictures - until it's `done` or a `duplicate`; 5 pictures a report in all (the sent ones count),
10 changes a report, rate-limited as reports ("edit:" + address / account), through the name filter (all three
texts). Kept as a history beside the report: `edits.jsonl` (`{date, what, before, contact, images}`, only what was
given) + `edit<n>_<k>.png`; report.json stays as it was sent. -> `{ok, edits, pictures}`; `409` closed, too many
pictures or changes, `404` no such report / wrong secret, `400` nothing to add. The launcher's Reports tab ("Edit /
add pictures...", `--reports-edit`) and Android's My reports send it; reports waiting for their pictures
(`pictures_later`, none yet) say "Add your pictures".

The players' words: "Received", "We're looking at this one now.", "Fixed - in the next update (2.1.9): <note>",
"We need more information: <question>", "Thanks - we have your answer to: <question>", "Done", "Already reported".

## No web addresses in players' text (2.1.10+)

A report's problem, happens, what, before and contact, a report edit's what, before and contact, and an answer to a
question are refused with `400` "Please dont add urls to bug reports" ("... to suggestions" for a suggestion) when they
hold a web address. The rule is `reports.has_url`; `launcher/report_urls.h` and `ReportSend.hasUrl` (Android) repeat it so
the forms can say so first, and `test_reports.py` has the test lists. It counts a scheme (`ab://`), `www.<something>` and
a bare domain, which is a word ending in a web ending from `URL_ENDINGS` (`URL_WITH_PATH` ones only with a `/` after them,
plus the short-link names in `URL_SITES`). File names, versions, paths, e-mail addresses and "game.Net" (a missing space) don't count.

## Who sent a report

`reports.py list` and the dashboard's admin list give each row `who`: the online account, else the contact (with the
sender's last change), else `anonymous`, and `contact`. `reports.py list --who NAME` lists one person's; on the dashboard
a name in the From column does. Neither goes out in the receipts' status answer.

## The match folder and its mods (2.1.6+)

A report can carry the game's match folder (`logs\matches\<date>_<people>\`: match.json, inputs.bin, box.txt,
loaded.txt, mods.txt - Arena Porting's format 1) and the mods used in that match, so the match can be replayed.
The launcher (HCTP chat, report_match.c) sends them like this:

1. `POST /api/reports/mods/check` with `{"mods": [{"hash", "size", "name", "type", "id", "version"}]}` (64 at most;
   30 checks per 10 minutes per address) -> `{"ok": true, "missing": [hashes to send], "max_upload": <bytes for
   this report>, "max_mod": <bytes per mod>, "mods_off": <true: send none - the server's mod store is full>}`.
2. `POST /api/reports` as above, plus the parts `match` (`match.zip`, 5 MB at most) and `mod0` .. `modN` (each a zip
   of one mod folder, file name `<hash>.zip`, 300 MB each and 300 MB of mods a report; only the `missing` ones).
   `report` gains `"match": "<folder name>"` and `"mods": [{hash, name, type, id, version, size, sent}]` (every mod
   of the match, `sent` whether it came along). The answer gains `"match": true/false`, `"mods_stored": [hashes]`,
   `"mods_refused": {hash: why}` (`hash mismatch`, `too large`, `server mod store full`, `not in the report's mod
   list`); a refused mod never fails the report. `413` when a cap is passed: the launcher sends it again without
   mods.

The hash is the mod folder's CONTENT hash (the same folder gives the same hash however it is zipped): SHA-256 over
its files, sorted by their relative path as UTF-8 with '/' separators and ASCII letters lower-cased; each file adds
that path, a 0 byte, its size as 8 bytes little-endian, then its bytes. Files or folders whose name starts with '.'
and files named `disabled` are left out (of the hash and the zip). The zip holds paths relative to the mod folder
(one top folder is accepted and taken off). The server checks every mod it is sent against its hash.

Kept on the node: `reports/<day>/<id>/match.zip` and `reports/mods/<hash>.svrmod` (each mod once, whatever report
sent it; a mod goes when the last report listing it is deleted). The mods together are capped by the node's
`--report-mod-quota-gb` (default 20); the dashboard warns from 80 %. nginx: `client_max_body_size 400m` on
`/svr/api/reports`, request buffering off (the core writes mod parts to `<data>/tmp/reports` as they come).

Dashboard: under each report with them, the match line (people, mod / CAW / CPU, rule, arena or custom arena, version,
fps, result), its zip, the mods (a download each, "not sent" for the others, their sizes) and **Get replay pack**:
one zip with `match/` (the match folder), `mods/<type>_<id>_<hash>.svrmod`, `mods.txt` and `report.json`. Above the
list: the space the report mods use. Admin routes: `GET /api/admin/report/<id>/match.zip`,
`GET /api/admin/report/<id>/replay.zip`, `GET /api/admin/report-mod/<hash>`.

## The signature (duplicates)

`sha1(app | version | problem | line)[:16]`, where `line` is the first crash / error line in the zip (its
`crash*` files first, then `*.log` / `*.txt` lines with error / exception / fatal / crash / assert / unhandled /
traceback), made general: leading times, other times, `0x...` and 8-16 digit hex, thread / tid / pid numbers,
drive paths and the remaining numbers are taken out. No such line: the client's `signature` hint stands in.

## Where they are kept

On the machine that holds the Community Creations bytes (the storage node: the user's PC; `--role all`: here),
in its `--data` folder:

    reports/<yyyy-mm-dd>/<id>/report.json   the fields + id, received, signature, signature_line, seen_before,
                                            zip_size, images (never the sender's address)
    reports/<yyyy-mm-dd>/<id>/report.zip
    reports/<yyyy-mm-dd>/<id>/image0.png ...
    reports/index.jsonl                     one line per report (the list, and the duplicates' counts)
    reports/status.json                     the marks: id -> done / duplicate / looking, or
                                            {status: fixed, note, commit, version, date}, or
                                            {status: needs_info / replied, question, date}
    reports/<yyyy-mm-dd>/<id>/replies.jsonl  the player's answers ({date, text, images}) + reply<n>_<k>.png
    reports/<yyyy-mm-dd>/<id>/edits.jsonl    the player's changes ({date, what, before, contact, images}) + edit<n>_<k>.png
    reports/edits.json                      id -> {n, date, pictures}: the changes' counts for the lists

The core passes a report to the node as one stored zip (`POST /reports` with `X-Storage-Key`); the node also
answers `GET /reports`, `GET /reports/<id>/<file>`, `POST /reports/<id>/mark` (the id may be a signature: every
report with it; `{status, fix}`), `POST /reports/<id>/delete`, `POST /reports/status` (`{receipts, account}`) and
`POST /reports/reply` (a stored zip: meta.json `{id, secret, text, account}` + image0..2) and `POST /reports/edit` (the same,
meta.json `{id, secret, what, before, contact, account}` + image0..4). The sender's address stays in the core's memory,
for the rate limit only.

## Marking a fix

On the PC that keeps the reports (the storage node, or `--role all`), with the server running or not:

    python port/server/reports.py --data <the server's --data> mark <report id | signature> --fixed "how it was fixed" --commit abc1234 --version 2.1.6 [--date 2026-10-25]
    python port/server/reports.py --data <...> mark <id | signature> --needs-info "the question for the player"
    python port/server/reports.py --data <...> mark <id | signature> --looking | --done | --duplicate | --reopen
    python port/server/reports.py --data <...> delete <report id>
    python port/server/reports.py --data <...> list [--fixed]      (+ N edit(s) on the changed ones)
    python port/server/reports.py --data <...> show <report id>     (its fields, mark, answers and the player's edits)

The same from the dashboard's API (admin): `POST /api/admin/report/<id | signature>/mark` with
`{"status": "fixed", "fix": {"note", "commit", "version", "date"}}` (date: today when left out), `{"status":
"needs_info", "fix": {"question"}}`, `{"status": "looking"}` (`replied`: only a player's answer sets it) and
`POST /api/admin/report/<id>/delete`.

## Dashboard

The Reports tab:
- "Fixed while you were away" first: the fixed reports, newest fix first, each with its problem, what happened, how it
  was fixed, the version it ships in, the commit, the date, its pictures and Details. Delete (and "Delete all shown")
  asks in place (a second click within 5 s) and removes the report's folder, index line and mark.
- Then the open reports grouped by signature (newest first): their count, the error line, picture thumbnails,
  Details (report.json), the zip, and Looking / Ask... (a question: the player's Reports tab shows it) / Done /
  Duplicate / Reopen marks; an asked report shows its question and its answers (replies.jsonl). Fixed ones leave
  this list; the tab's count is the new and the answered ones. A report the player changed has an "edited" tag and a
  row under it with each change (its text, contact and pictures, from edits.jsonl).

## Deploying

- The VPS (core): `vps_install.sh` copies `reports.py` and writes the nginx locations `/svr/api/reports`
  (`client_max_body_size 400m`), `/svr/api/reports/reply` (30m) and `/svr/api/reports/edit` (45m; `/status` goes
  through `/svr/`).
- The PC storage node runs `server.py --role storage` from its own folder: `reports.py` must be beside it, and
  its server restarted, before the core's new code goes live (until then the core answers 503).

## Tests

`port/server/test_reports.py --url http://127.0.0.1:<core> --node-data <node --data>` against a private core
(`--role core --local-admin`) + node (`--role storage`) on scratch ports; never the live server.
