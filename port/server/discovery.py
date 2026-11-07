"""Discovery: players' .svrmod mods for everyone (the launcher's Mods -> Discovery).

A mod is one .svrmod (a zip with a manifest.txt: docs/SVRMOD_FORMAT.md) and four pictures, 1024 x 768 JPEG
(the launcher makes them), with a name, types, tags, a version and a description. The uploader is a signed-in
account: its online name is the author. Uploads are public at once; the dashboard hides or deletes them, and
owners may edit (name, version, tags, description, types, the pictures), update the file (the same id and
download count: "updated") or delete their own (docs/DISCOVERY_API.md). The bytes are blobs of the Community Creations store (gamespy.Store: here, or on
the storage node), so they share its de-duplication and its node.
"""
import io
import json
import re
import struct
import time
import zipfile

TYPES = {"entrance": 1, "superstar": 2, "arena": 4, "moveset": 8}
MAX_MOD = 250 << 20          # (the user's limit, 20 Oct)
IMG_W, IMG_H, IMAGES = 1024, 768, 4
MAX_IMG = 1 << 20            # (a 1024 x 768 JPEG of the launcher's: ~150-400 KB)
MAX_TEXT = {"name": 60, "version": 20, "tags": 200, "description": 2000}
PAGE = 24


def init(store):
    with store.lock:
        store.db.executescript("""
            CREATE TABLE IF NOT EXISTS mods (
                id INTEGER PRIMARY KEY AUTOINCREMENT, owner INTEGER, author TEXT, name TEXT, types INTEGER,
                tags TEXT, version TEXT, description TEXT, size INTEGER, sha TEXT, images TEXT, file_name TEXT,
                hidden INTEGER DEFAULT 0, created REAL, downloads INTEGER DEFAULT 0);
            CREATE INDEX IF NOT EXISTS mods_created ON mods (created);
        """)
        # (owner edits: updated = the last file update, edited = the last info / picture edit,
        # pics = a counter of picture changes for the clients' image URLs)
        have = {r[1] for r in store.db.execute("PRAGMA table_info(mods)")}
        # (needs: the manifest's needs=, the oldest game version that plays the file - docs/SVRMOD_FORMAT.md)
        for col, kind in (("updated", "REAL"), ("edited", "REAL"), ("pics", "INTEGER DEFAULT 0"), ("needs", "TEXT")):
            if col not in have:
                store.db.execute("ALTER TABLE mods ADD COLUMN %s %s" % (col, kind))
        store.db.commit()


def jpeg_size(data):
    """A JPEG's (width, height) from its SOF marker, or None."""
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            return None
        marker = data[i + 1]
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        length = struct.unpack(">H", data[i + 2:i + 4])[0]
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + length
    return None


def check_svrmod(data):
    """(manifest dict, None) for a .svrmod with a manifest.txt (top level or one folder down), else (None, why)."""
    if len(data) > MAX_MOD:
        return None, "larger than %d MB" % (MAX_MOD >> 20)
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
        names = [n for n in z.namelist() if n.replace("\\", "/").count("/") <= 1 and
                 n.replace("\\", "/").rsplit("/", 1)[-1].lower() == "manifest.txt"]
        if not names:
            return None, "not a .svrmod: no manifest.txt in it"
        text = z.read(sorted(names, key=len)[0]).decode("utf-8", "replace")
    except (zipfile.BadZipFile, OSError, RuntimeError, ValueError):
        return None, "not a .svrmod: not a zip"
    manifest = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            manifest[k.strip().lower()] = v.strip()
    if not manifest.get("type"):
        return None, "not a .svrmod: its manifest has no type="
    return manifest, None


def clean_tags(text):
    tags = []
    for t in re.split(r"[,;]", text or ""):
        t = re.sub(r"\s+", " ", t).strip().lower()[:24]
        if t and t not in tags:
            tags.append(t)
    return ",".join(tags[:10])


def add(store, owner, author, fields, data, file_name, images):
    """Stores a checked upload; returns its id."""
    with store.lock:
        sha = store._put_blob(data, compress=False)  # (a zip: compressed already)
        shas = [store._put_blob(img, compress=False) for img in images]
        cur = store.db.execute(
            "INSERT INTO mods (owner, author, name, types, tags, version, description, size, sha, images, file_name,"
            " created, needs) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (owner, author, fields["name"], fields["types"], fields["tags"], fields["version"],
             fields["description"], len(data), sha, json.dumps(shas), file_name, time.time(), fields.get("needs", "")))
        store.db.commit()
        return cur.lastrowid


def _row(r, full=False):
    d = {"id": r[0], "owner": r[1], "author": r[2], "name": r[3],
         "types": [k for k, v in TYPES.items() if r[4] & v], "tags": [t for t in (r[5] or "").split(",") if t],
         "version": r[6], "size": r[8], "hidden": bool(r[12]), "created": r[13], "downloads": r[14],
         "updated": r[15], "edited": r[16], "pics": r[17] or 0, "needs": r[18] or ""}
    if full:
        d["description"] = r[7]
        d["file_name"] = r[11]
    return d


COLS = ("id, owner, author, name, types, tags, version, description, size, sha, images, file_name, hidden, created,"
        " downloads, updated, edited, pics, needs")


def search(store, q="", types=0, tag="", sort="new", page=0, with_hidden=False):
    where, args = [], []
    if not with_hidden:
        where.append("hidden = 0")
    for word in (q or "").lower().split()[:6]:
        where.append("(lower(name) LIKE ? OR lower(author) LIKE ? OR tags LIKE ?)")
        args += ["%" + word + "%"] * 3
    if types:
        where.append("(types & ?) = ?")
        args += [types, types]
    if tag:
        where.append("(',' || tags || ',') LIKE ?")
        args.append("%," + tag.lower().strip() + ",%")
    sql_where = (" WHERE " + " AND ".join(where)) if where else ""
    order = {"new": "created DESC", "popular": "downloads DESC, created DESC", "name": "lower(name)",
             "updated": "COALESCE(updated, created) DESC"}.get(sort, "created DESC")
    with store.lock:
        total = store.db.execute("SELECT COUNT(*) FROM mods" + sql_where, args).fetchone()[0]
        rows = store.db.execute("SELECT %s FROM mods%s ORDER BY %s LIMIT ? OFFSET ?" % (COLS, sql_where, order),
                                args + [PAGE, max(0, page) * PAGE]).fetchall()
    return {"total": total, "page": page, "per_page": PAGE, "mods": [_row(r) for r in rows]}


def get(store, mod_id, with_hidden=False):
    with store.lock:
        r = store.db.execute("SELECT %s FROM mods WHERE id = ?" % COLS, (mod_id,)).fetchone()
    if not r or (r[12] and not with_hidden):
        return None, None
    return _row(r, full=True), {"sha": r[9], "images": json.loads(r[10] or "[]"), "file_name": r[11]}


def counted(store, mod_id):
    with store.lock:
        store.db.execute("UPDATE mods SET downloads = downloads + 1 WHERE id = ?", (mod_id,))
        store.db.commit()


def set_hidden(store, mod_id, hidden):
    with store.lock:
        n = store.db.execute("UPDATE mods SET hidden = ? WHERE id = ?", (1 if hidden else 0, mod_id)).rowcount
        store.db.commit()
    return n > 0


def delete(store, mod_id):
    with store.lock:
        r = store.db.execute("SELECT sha, images FROM mods WHERE id = ?", (mod_id,)).fetchone()
        if not r:
            return False
        store._drop_blob(r[0])
        for s in json.loads(r[1] or "[]"):
            store._drop_blob(s)
        store.db.execute("DELETE FROM mods WHERE id = ?", (mod_id,))
        store.db.commit()
    return True


def edit(store, mod_id, fields, order=None):
    """The owner's info edit: fields (name / version / tags / description / types, checked already) and / or a
    new picture order (a permutation of 0-3)."""
    sets, args = [], []
    for k in ("name", "version", "tags", "description", "types"):
        if k in fields:
            sets.append("%s = ?" % k)
            args.append(fields[k])
    with store.lock:
        if order is not None:
            r = store.db.execute("SELECT images FROM mods WHERE id = ?", (mod_id,)).fetchone()
            shas = json.loads(r[0] or "[]") if r else []
            if len(shas) == len(order):
                sets.append("images = ?")
                args.append(json.dumps([shas[i] for i in order]))
                sets.append("pics = COALESCE(pics, 0) + 1")
        if not sets:
            return False
        sets.append("edited = ?")
        args.append(time.time())
        store.db.execute("UPDATE mods SET %s WHERE id = ?" % ", ".join(sets), args + [mod_id])
        store.db.commit()
    return True


def replace_file(store, mod_id, data, file_name, fields):
    """The owner's update: the new .svrmod (checked already) is stored first, then the row points at it and the
    old file goes; the id and the download count stay. fields: version (and maybe name / tags / description /
    types). Returns the old version, or None (no such mod)."""
    with store.lock:
        r = store.db.execute("SELECT sha, version FROM mods WHERE id = ?", (mod_id,)).fetchone()
        if not r:
            return None
        sha = store._put_blob(data, compress=False)
        sets, args = (["sha = ?", "size = ?", "file_name = ?", "updated = ?", "needs = ?"],
                      [sha, len(data), file_name, time.time(), fields.get("needs", "")])
        for k in ("name", "version", "tags", "description", "types"):
            if k in fields:
                sets.append("%s = ?" % k)
                args.append(fields[k])
        store.db.execute("UPDATE mods SET %s WHERE id = ?" % ", ".join(sets), args + [mod_id])
        store.db.commit()
        store._drop_blob(r[0])  # (after the new one is in: refcounted, so the same file again is fine)
        store.db.commit()
        return r[1]


def replace_picture(store, mod_id, n, img):
    """The owner's new picture n (a checked 1024 x 768 JPEG)."""
    with store.lock:
        r = store.db.execute("SELECT images FROM mods WHERE id = ?", (mod_id,)).fetchone()
        shas = json.loads(r[0] or "[]") if r else []
        if not 0 <= n < len(shas):
            return False
        old, shas[n] = shas[n], store._put_blob(img, compress=False)
        store.db.execute("UPDATE mods SET images = ?, pics = COALESCE(pics, 0) + 1, edited = ? WHERE id = ?",
                         (json.dumps(shas), time.time(), mod_id))
        store.db.commit()
        store._drop_blob(old)
        store.db.commit()
    return True


def account_bytes(store, owner, but=None):
    """The size of an account's Discovery mods (but: leave that mod out - the one being updated)."""
    with store.lock:
        r = store.db.execute("SELECT COALESCE(SUM(size), 0) FROM mods WHERE owner = ? AND id != ?",
                             (owner, but if but is not None else -1)).fetchone()
    return r[0]


def blob_labels(store):
    """sha -> what refers to it in Discovery (the audit's names): "mod 12 'Name' (hidden)" / "... picture 2"."""
    out = {}
    with store.lock:
        rows = store.db.execute("SELECT id, name, hidden, sha, images FROM mods").fetchall()
    for mid, name, hidden, sha, images in rows:
        tag = "mod %d '%s'%s" % (mid, name, " (hidden)" if hidden else "")
        out[sha] = tag
        for n, s in enumerate(json.loads(images or "[]")):
            out.setdefault(s, "%s picture %d" % (tag, n + 1))
    return out


def owner_of(store, mod_id):
    with store.lock:
        r = store.db.execute("SELECT owner FROM mods WHERE id = ?", (mod_id,)).fetchone()
    return r[0] if r else None
