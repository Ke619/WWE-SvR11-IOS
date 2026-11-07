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

## Public

- `GET /api/mods?q=&type=&tag=&sort=&page=`: one page of 24 mods, as `{"total", "page", "per_page", "mods": [row]}`.
  - `type`: a comma list of the four types (a mod must have all of them).
  - `sort`: one of
    - `new` (the default): newest upload first;
    - `updated`: the most recently updated first, by the time of its last file update or else its upload;
    - `popular`: most downloaded first;
    - `name`.
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
```

## Clients

- **The "my mod" buttons (Edit / Update / Delete):** show them only when the detail answer (fetched with the token) says `mine: true`.
  - Re-check every time the page is shown again, for example after switching launcher tabs.
  - Hide them when signed out.
  - The server refuses anyone else, but the buttons must not show either.
- **Updating a mod:** ask for the new `.svrmod` and a new version; fill in the other fields from the current row.
- **Lists:** mark rows with `updated` set as "Updated (date)". Offer `sort=updated` as an "Updated" tab or sort entry.
