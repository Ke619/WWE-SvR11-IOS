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

`report` fields: `format` (1), `app` (`launcher` / `modmaker` / `game` / `android`, required), `version` (required),
`platform` (`windows` / `android`), `os`, `problem` (required), `happens`, `what`, `before`, `contact`, `signature`
(the client's hint), `updater` (PC: the last "result" line of `<game>\logs\updater.log` - the updater's last
update or repair and how it ended; `update_log.h`). `what` / `before` are cut at 4000 characters, `updater` at
600, the rest at 200. Other fields are dropped.

Answers (JSON):
- `200 {"ok": true, "id": "20261024-153012-ab12cd", "signature": "<16 hex>", "seen_before": N}`:
  N = earlier reports with the same signature (0: new). A client can say "already reported N times, thanks".
- `400` bad input, `413` too large, `429` too many (5 in 10 minutes and 20 a day per address, and per account),
  `503` the storage node is down: `{"ok": false, "error": "<text to show>"}`. The client shows it with Try
  again (the zip is made again; it's deleted when the window closes).

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
    reports/status.json                     the marks: id -> done / duplicate, or
                                            {status: fixed, note, commit, version, date}

The core passes a report to the node as one stored zip (`POST /reports` with `X-Storage-Key`); the node also
answers `GET /reports`, `GET /reports/<id>/<file>`, `POST /reports/<id>/mark` (the id may be a signature: every
report with it; `{status, fix}`) and `POST /reports/<id>/delete`. The sender's address stays in the core's memory,
for the rate limit only.

## Marking a fix

On the PC that keeps the reports (the storage node, or `--role all`), with the server running or not:

    python port/server/reports.py --data <the server's --data> mark <report id | signature> --fixed "how it was fixed" --commit abc1234 --version 2.1.6 [--date 2026-10-25]
    python port/server/reports.py --data <...> mark <id | signature> --done | --duplicate | --reopen
    python port/server/reports.py --data <...> delete <report id>
    python port/server/reports.py --data <...> list [--fixed]

The same from the dashboard's API (admin): `POST /api/admin/report/<id | signature>/mark` with
`{"status": "fixed", "fix": {"note", "commit", "version", "date"}}` (date: today when left out) and
`POST /api/admin/report/<id>/delete`.

## Dashboard

The Reports tab:
- "Fixed while you were away" first: the fixed reports, newest fix first, each with its problem, what happened, how it
  was fixed, the version it ships in, the commit, the date, its pictures and Details. Delete (and "Delete all shown")
  asks in place (a second click within 5 s) and removes the report's folder, index line and mark.
- Then the open reports grouped by signature (newest first): their count, the error line, picture thumbnails,
  Details (report.json), the zip, and Done / Duplicate / Reopen marks. Fixed ones leave this list.

## Deploying

- The VPS (core): `vps_install.sh` copies `reports.py` and writes the nginx location `/svr/api/reports`
  (`client_max_body_size 70m`).
- The PC storage node runs `server.py --role storage` from its own folder: `reports.py` must be beside it, and
  its server restarted, before the core's new code goes live (until then the core answers 503).

## Tests

`port/server/test_reports.py --url http://127.0.0.1:<core> --node-data <node --data>` against a private core
(`--role core --local-admin`) + node (`--role storage`) on scratch ports; never the live server.
