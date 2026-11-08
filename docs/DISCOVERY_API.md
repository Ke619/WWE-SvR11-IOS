# Discovery API

Discovery is the launcher's Mods -> Discovery page: players' `.svrmod` mods (docs/SVRMOD_FORMAT.md) for everyone.
The server side is `port/server/discovery.py` plus the `api_mod*` handlers in `port/server/server.py`.

All paths are under the online server's base: `https://sho-ti.me/svr` (a local test server: `http://127.0.0.1:<port>/svr`).
Signed-in requests carry the account's token: `Authorization: Bearer <token>`, the same token the launcher's
Online page gets at sign-in. Errors are JSON `{"ok": false, "error": "<text to show the player>"}` where a
text helps.

## A mod row

Every list and detail answer uses this shape:

| Field | Meaning |
|---|---|
| `id` | the mod's id (it stays the same across owner edits and file updates) |
| `owner` | the uploader's account id |
| `mine` | detail answer only, when asked with a Bearer token: `true` when it's the asker's own mod. **Use this** (not names) for the "Edit / Update / Delete my mod" buttons |
| `author` | the uploader's online name at upload time (for display) |
| `name`, `version` | text |
| `types` | a list of `entrance`, `superstar`, `arena`, `moveset` |
| `tags` | a list of lower-case tags |
| `size` | the .svrmod's size in bytes |
| `downloads` | download count; kept across file updates |
| `created` | upload time (Unix seconds) |
| `updated` | the time of the owner's last **file** update, or `null`. Show "Updated" when it's set, "New" when not |
| `edited` | the time of the owner's last info or picture edit, or `null` |
| `pics` | a counter that goes up on every picture change; put it in the picture URL (`img/{n}?v=<pics>`) |
| `hidden` | the dashboard hid it (only its owner and the dashboard ever see a hidden row) |
| `description`, `file_name` | in the detail answer only |
| `rating` | `{"avg": 4.33, "count": 3}`: the mod's star rating (avg 0 when count is 0). Left out while ratings are off (`ratings_on: false`) |

## Public

- `GET /api/mods?q=&type=&tag=&sort=&page=`: one page of 24 mods, as `{"total", "page", "per_page", "mods": [row]}`.
  - `type`: a comma list of the four types (a mod must have all of them).
  - `sort`: one of
    - `new` (the default): newest upload first;
    - `updated`: the most recently updated first, by the time of its last file update or else its upload;
    - `popular`: most downloaded first;
    - `name`;
    - `rating`: mods with at least `discovery_rating_min` ratings (a server setting, 3 by default) first, by their
      average and then by how many ratings they have; the rest after them, the same way. So one 5-star rating
      doesn't top the list. While ratings are off, `rating` sorts as `new`.
  - The answer also has `ratings_on` and `comments_on` (the server's two switches).
- `GET /api/mods/{id}`: the row with `description` and `file_name`; 404 when it doesn't exist or is hidden. Send the
  Bearer token when signed in and the answer has `mine` (see the row). Sign-in (`/api/login`, `/api/register`) also
  answers the account's `id` now.
- `GET /api/mods/{id}/file`: the `.svrmod`. Each download counts.
  - Big files stream with `Content-Length`. The launcher shows progress from it and checks that every byte came.
  - When a download breaks, the launcher tries again (6 tries in all, 2 / 5 / 10 / 30 s apart) with
    `Range: bytes=<have>-`. It appends on `206` with a matching `Content-Range`. On `200` it starts again from 0.
  - The server answers `Range: bytes=N-` and `bytes=N-M` with `206`, `Content-Range` and `Accept-Ranges: bytes`
    (a range past the end: `416`).
    - Stored blobs are read from N: on the core's disk, or through the storage node's own Range.
    - Packed (xz) blobs are unpacked and their first N bytes dropped.
  - Only a request without a Range, or with a Range from 0, counts as a download.
  - Only a complete file is installed.
- `GET /api/mods/{id}/img/{n}` (n 0-3): picture n, a 1024 x 768 JPEG.
  - It carries an `ETag`, so you can send `If-None-Match` and get a 304.
  - Ask for `img/{n}?v=<pics>` so a replaced picture is a new URL.

## Signed in

### Upload

- `POST /api/mods`, multipart form data.
- Parts:
  - `name` (≤ 60 characters), `version` (≤ 20), `types` (e.g. `arena,entrance`), `tags` (≤ 200, comma-separated), `description` (≤ 2000);
  - `svrmod`: the file, with a `.svrmod` file name, at most 250 MB;
  - `img0` to `img3`: four 1024 x 768 JPEGs, ≤ 1 MB each.
- All are needed except `tags` and `description`.
- One upload is stored at a time server-wide: while another is stored you get a 503, so try again in a minute.
- Answer: `{"ok": true, "id", "size"}`, plus `"warning": "<text>"` when the mod is larger than the dashboard's
  `discovery_warn_mb` (default 50 MB). It is stored all the same; show the text to the uploader.
- One account's mods may take `discovery_quota_mb` in all (default 1024 MB). An upload or a file update over it is
  refused (400, the error says how much it would take).

### Owner-only

These check that the token's account **is the mod's uploader** (`owner`). The answers are the same for all of them:

| Status | Meaning |
|---|---|
| 401 | no token, or not a valid one |
| 403 | signed in, but not the uploader |
| 404 | no such mod |
| 429 | more than 30 changes in an hour from this account |

A change answers `{"ok": true, "mod": <row with description>}`.

- `PATCH /api/mods/{id}`, a JSON object with any of:
  - `name`, `version`, `tags`, `description` (the same limits as upload);
  - `types` (a list or a comma string);
  - `picture_order`: a permutation of `[0, 1, 2, 3]`. Element i is the old index of the picture that becomes picture i, so `[3, 2, 1, 0]` reverses them.

  An empty name or version, an empty `types`, a bad `picture_order` or nothing to change is a 400.
- `PUT /api/mods/{id}/file`, multipart, a **new version of the mod**:
  - Parts: `svrmod` (the new file) and `version`, which is required and must differ from the current version.
  - Optional parts: `name`, `tags`, `description`, `types`, to change them in the same request.
  - Size limit and the one-at-a-time slot: the same as upload (413 / 503).
  - The new file is checked as an upload is (a zip with a `manifest.txt` that has `type=`). It is stored before the mod points at it, and only then is the old file dropped. A refused or failed update leaves the old file in place.
  - The id and the download count stay the same; `updated` becomes now.
  - The same quota (counted without the mod's old file) and `warning` as an upload.
- `PUT /api/mods/{id}/pictures/{n}` (n 0-3): the request body **is** the new picture, a 1024 x 768 JPEG ≤ 1 MB (`Content-Type: image/jpeg`). It replaces picture n, and `pics` goes up.
- `DELETE /api/mods/{id}`: deletes the mod and its file and pictures for everyone.

The dashboard can still hide, show or delete any mod. A hidden mod stays hidden when its owner edits or updates it.

An admin can store a new version of any mod (the 30 Oct bulk .bik -> .mp4 movie shrink): the same `PUT /api/mods/{id}/file` with the storage node's key (`X-Storage-Key`, a core with a node only) instead of a Bearer token, or `PUT /api/admin/mods/{id}/file` from the dashboard. The checks are the owner's (a new version, the file checked, `needs` from its manifest) without the quota. The owner, author, id and downloads stay; `updated` becomes now, so players see UPDATE. An optional `why` part goes in the log. The key route goes through the `/svr/api/mods/{id}/file` nginx location, which takes big bodies; `/svr-admin/` takes 1 MB.

## Comments and ratings

The server side is `port/server/discovery_social.py` plus the `api_mod_comment*` / `api_mod_rating` handlers in
`server.py`. Comments are plain text, flat (no replies), newest first. Ratings are 1-5 stars: one per account per
mod, which the player can change or take back. A mod's uploader can't rate their own mod.

Reading needs no account. Posting, deleting, reporting and rating need a signed-in account: the Bearer token, the
same as uploads. An account that `account_checks` keeps off online can still comment; a banned account's sessions are
gone, so it gets `401`.

Every refusal is `{"ok": false, "code": "<code>", "error": "<text to show the player, as it is>"}`. Show `error`.
Use `code` only to decide what to do next (for example, `sign_in_needed`: offer the Online tab).

### The mod's page

`GET /api/mods/{id}` (with the Bearer token when signed in) also has:

| Field | Meaning |
|---|---|
| `comments_on`, `ratings_on` | the server's switches. Off: hide that part of the page |
| `comments` | how many comments are shown on it |
| `rating` | see the row (left out while ratings are off) |
| `my_rating` | with a token: the asker's own stars, 0 = not rated |
| `can_rate` | with a token: ratings on, not the asker's own mod, not muted from ratings |
| `can_comment` | with a token: comments on, not muted |

### Endpoints

- `GET /api/mods/{id}/comments?page=N`: 20 a page, newest first:
  `{"ok": true, "enabled": true, "total", "page", "per_page": 20, "comments": [comment], "can_comment"}`.
  - A comment: `{"id", "author" (the account's online name), "text", "created" (Unix seconds)}`. With a token, also
    `"mine"` (the asker wrote it) and `"can_delete"` (the asker wrote it, or uploaded the mod).
  - `can_comment` is there only with a token.
  - Comments off: `{"ok": true, "enabled": false, "comments": [], ...}`.
  - Text keeps its new lines (8 lines at most). Show it as plain text, never as markup or links.
- `POST /api/mods/{id}/comments` `{"text": "..."}` -> `{"ok": true, "comment": comment, "total"}`.
- `DELETE /api/mods/{id}/comments/{cid}` -> `{"ok": true}`. Allowed for the comment's writer, or the mod's uploader on
  their own mod. Players no longer see it. The server keeps it 30 days for the dashboard, then removes it.
- `POST /api/mods/{id}/comments/{cid}/report` `{"why": "optional, 200 characters"}` -> `{"ok": true, "new": true|false}`.
  It goes to the dashboard. The same account reporting the same comment again isn't counted twice; `new: false` is
  still a success: say "Reported".
- `POST /api/mods/{id}/rating` `{"stars": 1-5}` (0 takes the rating back) -> `{"ok": true, "rating": {"avg", "count"},
  "my_rating"}`. `stars` must be a JSON number, not a string.

A hidden or missing mod is `404` (`no_mod`) on all of these; a missing, hidden or deleted comment is `404`
(`no_comment`).

### The server's checks on a comment (in this order)

| `code` | Status | When |
|---|---|---|
| `comments_off` | 403 | the dashboard turned comments off |
| `sign_in_needed` | 401 | no token, an ended session or a banned account |
| `muted` | 403 | the dashboard muted the account from comments |
| `empty` | 400 | nothing but spaces |
| `too_long` | 400 | over 500 characters (after trimming) |
| `too_many_lines` | 400 | over 8 lines |
| `link` | 400 | a web address, a link, a Discord / Telegram invite (`discord.gg/...`, `.gg/...`, `/invite/...`, `t.me/...`) |
| `word` | 400 | the name filter's list (the dashboard's Names tab) |
| `account_name` | 400 | the account's own name is on the name filter's list |
| `piracy` | 400 | the comment word list (the Names tab's second list: ISO, XEX, ROM, NSP, XCI, PKG, torrents, "download the game"...; also spelled out, "i s o", "i.s.o", "game.iso") or split archive names (`.7z.001`, `.part1.rar`, `.z01`) |
| `repeat` | 400 | the same text (any capitals) from this account in the last 10 minutes |
| `too_fast` | 429 | over 5 comments a minute or 30 an hour per account, or 60 an hour per address |

Ratings: `ratings_off` 403, `sign_in_needed` 401, `muted_ratings` 403 (muted from comments and ratings),
`own_mod` 403, `bad_stars` 400, `rate_too_fast` 429 (60 an hour). Reports: `report_too_fast` 429 (20 an hour).
Deleting: `not_yours` 403.

Clients may check the length (500) and lines (8) before sending. Leave every other check to the server and show its
`error`.

### Clients

- **The page:** under the description, the stars (`rating.avg` to one decimal, and `rating.count`), then the
  comments. Hide each part when its switch is off.
- **Rating:** five stars the player taps; theirs shows from `my_rating`. Show them only when `can_rate`. A signed-out
  player sees the average only, with "Sign in to rate".
- **Writing:** a box with a 500-character counter and Send, shown when `can_comment`. Signed out: "Sign in to
  comment" (the Online tab). After a send, put the returned `comment` at the top and use the returned `total`.
- **Each comment:** the author, the age ("3 h ago"), the text; Delete when `can_delete` (ask first); Report on other
  people's comments when signed in.
- **Paging:** "More comments" asks for the next page while `(page + 1) * per_page < total`.
- **Lists:** the row's stars and count (for example "4.3 (12)"); a "Top rated" sort entry (`sort=rating`), shown
  while `ratings_on`.
- Never put comment text into anything that runs it: no markup, no links. It is plain text.

### Dashboard

- The **Comments** tab (`GET /api/admin/discovery/comments?page&q&state&mod&account`): reported comments first, then
  the latest, each with its mod, writer, state and reports (who and why). It also lists the mutes.
  - Actions: `POST /api/admin/discovery/comment/{cid}/{hide|show|delete|clear-reports|mute-comments|mute-all}`.
    `delete` removes it for good. A writer's or uploader's delete only takes it from the players' view.
  - `POST /api/admin/discovery/mute` `{"account": id, "scope": "comments" | "all" | ""}` (`""` lifts the mute).
- The **Discovery** tab: each mod's rating, and **Clear ratings** (`POST /api/admin/mod/{id}/clear-ratings`).
- The **Names** tab: the comment word list, below the name filter's (`/api/admin/names?list=comments`, the same
  rules). It's used at once.
- **Settings:** `discovery_comments` and `discovery_ratings` (on / off, at once; nothing stored is deleted) and
  `discovery_rating_min` (1-100, default 3).

## Server log lines

```
discovery: mod 12 "Name" 1.0 (12345 bytes, arena) from Player
discovery: mod 12 updated by its owner (info): description, name
discovery: mod 12 updated by its owner (pictures)
discovery: mod 12 updated by its owner (pictures): picture 2
discovery: mod 12 updated by its owner (file) 1.0 -> 1.1 (12345 -> 23456 bytes, arena)
discovery: mod 12 updated by admin (movie shrink) (file) 1.1 -> 1.1.1 (23456 -> 3456 bytes, arena)
discovery: update of mod 12 from Player refused: <why>
discovery: mod 12 deleted by its owner Player
discovery: comment 40 on mod 12 by Player (57 characters)
discovery: comment on mod 12 from Player refused (piracy)
discovery: comment 40 on mod 12 deleted by its writer Player      (or: its owner Uploader)
discovery: comment 40 on mod 12 reported by Player
discovery: mod 12 rated 4 by Player                               (or: rated (taken back))
discovery: 3 deleted comment(s) over 30 days old removed
admin: discovery comment 40 (mod 12, by Player): hide
admin: discovery mute of Player: comments
admin: mod 12: its 5 rating(s) cleared
```

## Clients

- **The "my mod" buttons (Edit / Update / Delete):** show them only when the detail answer (fetched with the token) says `mine: true`.
  - Re-check every time the page is shown again, for example after switching launcher tabs.
  - Hide them when signed out.
  - The server refuses anyone else, but the buttons must not show either.
- **Updating a mod:** ask for the new `.svrmod` and a new version; fill in the other fields from the current row.
- **Lists:** mark rows with `updated` set as "Updated (date)". Offer `sort=updated` as an "Updated" tab or sort entry.
