"""Problem reports from the launcher, the Mod Maker, the game and Android (POST /api/reports, docs/REPORTS_API.md).

Each one is kept where the Community Creations bytes live (here, or the storage node: the user's PC), readable
there as plain files:
    <data>/reports/<yyyy-mm-dd>/<id>/report.json, report.zip, image0.png ...
    <data>/reports/index.jsonl     (one line per report: the dashboard's list, and the duplicates' counts)
    <data>/reports/status.json     (id -> done / duplicate / looking, {status: fixed, note, commit, version, date},
                                    {status: needs_info / replied, question, date}: the dashboard's marks;
                                    `python reports.py mark ...` writes them too - see main())
    <data>/reports/<yyyy-mm-dd>/<id>/replies.jsonl, reply<n>_<k>.png   (the player's answers to a question)
    <data>/reports/<yyyy-mm-dd>/<id>/match.zip   (the game's match folder: logs/matches/<date>_<people>/)
    <data>/reports/mods/<content hash>.svrmod    (the mods used in that match, each kept once for every report)
The sender's address is never stored (the core keeps it in memory for its rate limit only).

Receipts: each report gets a random secret, returned once to the client that sent it (it keeps id + secret in
UserData\reports.json); only its SHA-256 is stored. With id + secret a client asks for its reports' public state
(POST /api/reports/status: state, the fixed note and version, a question) and answers a question
(POST /api/reports/reply) - never another report, never the private parts (logs, zip, pictures, contact).

Mods: the client asks first which it needs to send (POST /api/reports/mods/check), then sends only those, each as
a zip of its folder (an .svrmod) named by its CONTENT hash: SHA-256 over the folder's files sorted by their
lower-cased relative path ('/'), each file as path (UTF-8, lower case), a 0 byte, its size (8 bytes little-endian)
and its bytes - the same folder gives the same hash however it was zipped. The server checks the hash on arrival.

Duplicates: a signature from app + version + problem + the first crash / error line in the zip's logs, made
general (times, addresses, thread ids and other numbers taken out); the client's own hint stands in when the
zip has none. seen_before = the reports before this one with the same signature.
"""
import hashlib
import io
import json
import os
import re
import secrets
import shutil
import threading
import time
import zipfile

APPS = ("launcher", "modmaker", "game", "android")
MAX_ZIP = 25 << 20
MAX_IMAGE = 8 << 20
MAX_IMAGES = 5
TEXT_CAP = {"what": 4000, "before": 4000, "updater": 600}  # (every other text field: 200; updater: the updater's last result line)
MAX_MATCH = 5 << 20          # the match folder's zip
MAX_MOD = 300 << 20          # one mod
MAX_NEW_MOD_BYTES = 300 << 20  # the mods sent with one report
MAX_MODS = 64                # mods listed in one report
MOD_QUOTA = 20 << 30         # every report's mods together (the node's --report-mod-quota-gb)
FIELDS = ("format", "app", "version", "platform", "os", "problem", "happens", "what", "before", "contact", "updater")
STATUSES = ("", "done", "duplicate", "fixed", "looking", "needs_info", "replied")
FIX_FIELDS = {"note": 1000, "commit": 40, "version": 20, "date": 10}
INFO_FIELDS = {"question": 1000, "date": 10}
MAX_STATUS_IDS = 100          # receipts asked about in one status call
MAX_REPLY = 2000              # a player's answer's text
MAX_REPLY_IMAGES = 3
MAX_REPLIES = 5               # answers kept for one report
# (the player's words for each state - the launcher / Mod Maker / Android lists show these)
STATES = {"": "received", "looking": "looking", "fixed": "fixed", "needs_info": "needs_info", "replied": "replied",
          "done": "done", "duplicate": "duplicate"}


def secret_hash(secret):
    return hashlib.sha256(str(secret or "").encode("utf-8")).hexdigest()


def clean_fix(raw):
    """A fixed mark's details -> {note, commit, version, date} (date: today when not given)."""
    raw = raw if isinstance(raw, dict) else {}
    fix = {k: str(raw.get(k) or "").replace("\r\n", "\n").strip()[:cap] for k, cap in FIX_FIELDS.items()}
    if not re.fullmatch(r"\d{4}-\d\d-\d\d", fix["date"]):
        fix["date"] = time.strftime("%Y-%m-%d")
    return fix


def clean_info(raw):
    """A needs-info mark's details -> {question, date} (date: today)."""
    raw = raw if isinstance(raw, dict) else {}
    info = {k: str(raw.get(k) or "").replace("\r\n", "\n").strip()[:cap] for k, cap in INFO_FIELDS.items()}
    if not re.fullmatch(r"\d{4}-\d\d-\d\d", info["date"]):
        info["date"] = time.strftime("%Y-%m-%d")
    return info


def _mark_of(value):
    """A status.json value -> (status, details or None): fixed {note, commit, version, date}, needs_info / replied
    {question, date}; old marks are plain strings."""
    if isinstance(value, dict):
        status = str(value.get("status", ""))
        fields = INFO_FIELDS if status in ("needs_info", "replied") else FIX_FIELDS
        return status, {k: value.get(k, "") for k in fields}
    return str(value or ""), None


def image_ext(data):
    """png / jpg / webp by their magic bytes, else None."""
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:3] == b"\xff\xd8\xff":
        return "jpg"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    return None


def clean_meta(raw):
    """The client's report.json -> (the fields kept, None) or (None, why it's refused)."""
    if not isinstance(raw, dict):
        return None, "report: not a JSON object"
    meta = {}
    for k in FIELDS:
        v = raw.get(k)
        if v is None:
            continue
        if k == "format":
            meta[k] = v if isinstance(v, int) else 1
            continue
        meta[k] = str(v).replace("\r\n", "\n").strip()[:TEXT_CAP.get(k, 200)]
    meta["app"] = meta.get("app", "").lower()
    if meta["app"] not in APPS:
        return None, "app must be one of: " + ", ".join(APPS)
    if not meta.get("version") or not meta.get("problem"):
        return None, "A version and a problem are needed."
    hint = str(raw.get("signature") or "").strip()[:300]
    if hint:
        meta["client_signature"] = hint
    return meta, None


# -- the signature ---------------------------------------------------------------------------------------------

_ERR = re.compile(r"\b(fatal|error|exception|crash(ed)?|assert(ion)?|unhandled|access violation|traceback)\b", re.I)
_NORMALISE = [
    (re.compile(r"^\s*\[?\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:[.,]\d+)?\]?\s*"), ""),  # (a leading date + time)
    (re.compile(r"^\s*\[?\d\d:\d\d:\d\d(?:[.,]\d+)?\]?\s*"), ""),                       # (a leading time)
    (re.compile(r"\b\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:[.,]\d+)?"), "<time>"),
    (re.compile(r"\b\d\d:\d\d:\d\d(?:[.,]\d+)?\b"), "<time>"),
    (re.compile(r"\b(thread|tid|pid)[\s:=#]*(0x)?[0-9a-f]+\b", re.I), r"\1 <n>"),
    (re.compile(r"\b0x[0-9a-f]+\b", re.I), "<addr>"),
    (re.compile(r"\b[0-9a-f]{8,16}\b", re.I), "<addr>"),
    (re.compile(r"[A-Za-z]:[\\/][^\s:\"']*[\\/]"), "<path>/"),  # (a player's own folders: only the file name counts)
    (re.compile(r"\d+"), "<n>"),
    (re.compile(r"\s+"), " "),
]


def normalise(line):
    s = line.strip()
    for rx, to in _NORMALISE:
        s = rx.sub(to, s)
    return s.strip()[:200]


_STAMP = re.compile(r"(20\d{6})[_-]?(\d{6})")


def _stamp(name):
    """The time in a file's name (crash_20261022_142646.txt, modmaker_20261024-044512.log) as an int, or 0."""
    m = _STAMP.search(name)
    return int(m.group(1) + m.group(2)) if m else 0


def _stale(stamp, newest):
    """A crash file from before this problem: a day older than the newest file in the zip, or two days older
    than now (the player's clock and time zone may be off a little)."""
    import datetime
    try:
        a = datetime.datetime.strptime(str(stamp), "%Y%m%d%H%M%S")
        b = datetime.datetime.strptime(str(newest), "%Y%m%d%H%M%S") if newest else a
    except ValueError:
        return False
    return b - a > datetime.timedelta(days=1) or datetime.datetime.now() - a > datetime.timedelta(days=2)


# Lines every session logs and that say nothing about a problem: the emulator's start-up heap probing, the
# game's own look-ups of optional files (DLC, SeasonVO voice banks), the XMA / kernel stubs (30 Oct: five live
# reports were all signed by an old "BaseHeap::Alloc failed" line).
_HARMLESS = re.compile(
    r"BaseHeap::Alloc|PhysicalHeap::Alloc|ResolvePath\(|NtCreateFile\]? FAILED|XMA: Write to unknown register|"
    r"VFS: '|IoDismountVolume|\bStub\b|\bSTUB\b|SeasonVO|\bdlc\d:\\|catalog_hd\.arc|gamecontrollerdb|"
    r"Failed to find vkGetDeviceProcAddr", re.I)
_LEVEL = re.compile(r"\[(critical|error)\]", re.I)
_LOG_TIME = re.compile(r"^\[(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)")
_GAME_LOG = re.compile(r"(?:^|/)svr2011_(\d+)(?:\.(\d+))?\.log$", re.I)


def _useful(line):
    """A line that can sign a report: not one of the harmless ones, not a converter's JSON output."""
    t = line.strip()
    return bool(t) and not _HARMLESS.search(t) and not t.startswith(("[{", "{", "\"", "[\""))


def _first_time(text):
    """A log's first line's time as YYYYMMDDhhmmss (int), 0 if none."""
    for line in text.splitlines()[:5]:
        m = _LOG_TIME.match(line)
        if m:
            return int("".join(m.groups()))
    return 0


def _session(z, names, app):
    """The reported session's logs, oldest part first, as [(name, text)]: the game's newest log (its highest
    number, all its parts - report.c puts the newest game's parts and the last part of two older ones in), or
    for the Mod Maker its newest modmaker_<time>.log."""
    def text(i):
        try:
            return z.read(i).decode("utf-8", "replace")
        except Exception:
            return ""
    if app == "modmaker":
        own = sorted((i for i in names if re.search(r"(^|/)modmaker_[^/]*\.log$", i.filename, re.I)),
                     key=lambda i: (_stamp(i.filename), i.filename))
        if own:
            return [(own[-1].filename, text(own[-1]))]
    games = {}
    for i in names:
        m = _GAME_LOG.search(i.filename.replace("\\", "/"))
        if m:
            games.setdefault(int(m.group(1)), []).append((int(m.group(2) or 0), i))
    if not games:
        return []
    parts = sorted(games[max(games)], key=lambda kp: -kp[0])  # (the highest part number is the oldest)
    return [(i.filename, text(i)) for _, i in parts]


def first_error_line(zip_bytes, app=""):
    """The line a report is about, '' when there's none (or no zip). Only the reported session counts - its
    logs (_session) and the crash reports written while it ran - and the harmless lines never do. In order:
    a crash report's first error line (else its first line), the session's first [critical] line, its LAST
    [error] line (the one nearest the problem), then for the Mod Maker its log's last failed / error line."""
    if not zip_bytes:
        return ""
    try:
        z = zipfile.ZipFile(io.BytesIO(zip_bytes))
    except (zipfile.BadZipFile, ValueError):
        return ""
    names = [i for i in z.infolist() if not i.is_dir() and i.file_size <= (64 << 20)]
    session = _session(z, names, app)
    start = min([t for t in (_first_time(txt) for _, txt in session) if t] or [0])
    # crash reports written during the session (empty ones - the game closed while writing - say nothing)
    crashes = sorted((i for i in names if re.search(r"(^|/)crash[^/]*\.txt$", i.filename, re.I) and i.file_size
                      and _stamp(i.filename) and (not start or _stamp(i.filename) >= start - 60)),
                     key=lambda i: -_stamp(i.filename))
    for info in crashes:
        try:
            lines = [l.strip() for l in z.read(info).decode("utf-8", "replace").splitlines() if l.strip()]
        except Exception:
            continue
        hit = next((l for l in lines if _ERR.search(l) and _useful(l)), None) or (lines[0] if lines else None)
        if hit:
            return hit[:500]
    lines = [l for _, txt in session for l in txt.splitlines()]
    hit = next((l for l in lines if "[critical]" in l.lower() and _useful(l)), None)
    if hit is None:
        hit = next((l for l in reversed(lines) if _LEVEL.search(l) and _useful(l)), None)
    if hit is None and app == "modmaker":
        hit = next((l for l in reversed(lines) if re.search(r"\b(failed|error|traceback)\b", l, re.I) and _useful(l)),
                   None)
    return (hit or "")[:500]


def signature(meta, zip_bytes, match_bytes=b""):
    """(signature, the line it was made from). No line from the session: the client's hint (when it's a useful
    one), else the problem with the match's rule and arena mod (the match folder), else the problem alone."""
    line = first_error_line(zip_bytes, meta["app"])
    if not line and _useful(meta.get("client_signature", "")):
        line = meta.get("client_signature", "")
    if not line:
        m = match_summary(match_bytes) if match_bytes else None
        line = "no error line - match rule %s, arena %s" % ((m or {}).get("rule") or "-",
                                                             (m or {}).get("arena_mod") or (m or {}).get("arena") or "-")
    key = "|".join((meta["app"], meta["version"].lower(), meta["problem"].lower(), normalise(line)))
    return hashlib.sha1(key.encode("utf-8")).hexdigest()[:16], line


# -- mods and the match folder ---------------------------------------------------------------------------------

def content_hash(zip_file):
    """The clients' content hash of a mod zip (a path or a file): its files (a single top folder taken off), sorted
    by the lower-cased relative path, each as path + 0 byte + 8-byte little-endian size + bytes. None: not a zip."""
    try:
        z = zipfile.ZipFile(zip_file)
    except (zipfile.BadZipFile, ValueError, OSError):
        return None
    with z:
        files = [i for i in z.infolist() if not i.is_dir()]
        names = [i.filename.replace("\\", "/") for i in files]
        tops = {n.split("/", 1)[0] for n in names}
        strip = len(tops) == 1 and all("/" in n for n in names)
        entries = []
        for n, info in zip(names, files):
            rel = n.split("/", 1)[1] if strip else n
            parts = rel.split("/")
            # (as the launcher: no file or folder starting with '.', no "disabled" file)
            if any(p.startswith(".") for p in parts) or parts[-1].lower() == "disabled":
                continue
            entries.append((rel.encode("utf-8").lower(), info))  # (bytes.lower(): ASCII letters only, as the launcher)
        entries.sort(key=lambda e: e[0])
        h = hashlib.sha256()
        for rel, info in entries:
            h.update(rel + b"\0" + info.file_size.to_bytes(8, "little"))
            with z.open(info) as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    h.update(chunk)
        return h.hexdigest()


def clean_mods(raw):
    """The report's "mods" list -> [{hash, name, type, id, version, size, sent}] (64 at most, bad entries out)."""
    out, seen = [], set()
    for m in raw if isinstance(raw, list) else []:
        if not isinstance(m, dict):
            continue
        h = str(m.get("hash") or "").lower()
        if not re.fullmatch(r"[0-9a-f]{64}", h) or h in seen:
            continue
        seen.add(h)
        try:
            size = max(0, int(m.get("size") or 0))
        except (TypeError, ValueError):
            size = 0
        out.append({"hash": h, "name": str(m.get("name") or "")[:80], "type": str(m.get("type") or "")[:20],
                    "id": str(m.get("id") or "")[:80], "version": str(m.get("version") or "")[:20], "size": size,
                    "sent": bool(m.get("sent"))})
        if len(out) >= MAX_MODS:
            break
    return out


def match_summary(match_bytes):
    """The dashboard's line for a match folder zip (match.json, Arena Porting's format 1) -> dict or None."""
    try:
        z = zipfile.ZipFile(io.BytesIO(match_bytes))
        name = next((n for n in z.namelist() if re.fullmatch(r"(?:[^/]+/)?match\.json", n.replace("\\", "/"))), None)
        if not name or z.getinfo(name).file_size > (4 << 20):
            return None
        m = json.loads(z.read(name).decode("utf-8-sig"))
    except Exception:
        return None
    if not isinstance(m, dict):
        return None
    d = lambda v: v if isinstance(v, dict) else {}
    rule, arena = d(m.get("rule")), d(m.get("arena"))
    try:
        rule_hex = "%02X" % int(rule.get("id"))
    except (TypeError, ValueError):
        rule_hex = ""
    people = []
    for p in m.get("people") if isinstance(m.get("people"), list) else []:
        p = d(p)
        mod = d(p.get("mod"))
        try:  # ("person": the match's place - 6 the referee, 7 / 8 the announcers: not the wrestlers)
            if int(p.get("person", 0)) >= 6:
                continue
        except (TypeError, ValueError):
            pass
        people.append({"name": str(p.get("name") or p.get("superstar") or "?")[:60], "cpu": bool(p.get("cpu")),
                       "mod": str(mod.get("id") or "")[:80] if p.get("mod") else "", "caw": p.get("caw") is not None})
    amod = d(arena.get("mod"))
    t = lambda v, n=60: str(v if v is not None else "")[:n]
    # (names: the dashboard's English ones by id - match_names.py, as on the other tabs - else the game's own, which
    # come in the player's language; a custom arena keeps its manifest name)
    import match_names
    stock_arena = match_names.ARENAS.get(arena.get("id")) if isinstance(arena.get("id"), int) else None
    return {"version": t(d(m.get("game")).get("version"), 20), "rule": rule_hex,
            "rule_name": t((match_names.rule_name(rule_hex) if rule_hex else None) or rule.get("name")),
            "arena": t(arena.get("id"), 10),
            "arena_name": t(arena.get("name") if arena.get("mod") else stock_arena or arena.get("name")),
            "arena_mod": t(amod.get("id"), 80) if arena.get("mod") else "", "people": people[:16],
            "fps": t(m.get("frame_rate"), 4), "online": bool(m.get("online")), "started": t(m.get("start"), 30),
            "length_s": t(m.get("length_s"), 10), "result": t(m.get("result"), 120), "complete": bool(m.get("complete"))}


# -- where they are kept ---------------------------------------------------------------------------------------

def _day(rid):
    return "%s-%s-%s" % (rid[0:4], rid[4:6], rid[6:8])


def _valid_id(rid):
    return bool(re.fullmatch(r"\d{8}-\d{6}-[0-9a-f]{6}", rid or ""))


class LocalReports:
    """The reports in <data>/reports (this machine: --role all, or the storage node)."""

    def __init__(self, folder):
        self.dir = os.path.join(folder, "reports")
        os.makedirs(self.dir, exist_ok=True)
        self.index_path = os.path.join(self.dir, "index.jsonl")
        self.status_path = os.path.join(self.dir, "status.json")
        self.lock = threading.Lock()
        self.mods_dir = os.path.join(self.dir, "mods")
        os.makedirs(self.mods_dir, exist_ok=True)
        self.mod_quota = MOD_QUOTA
        self.counts = {}
        for row in self._rows():
            self.counts[row.get("signature", "")] = self.counts.get(row.get("signature", ""), 0) + 1
        self.mod_bytes = sum(e.stat().st_size for e in os.scandir(self.mods_dir) if e.name.endswith(".svrmod"))

    def _rows(self):
        try:
            with open(self.index_path, encoding="utf-8") as f:
                return [json.loads(l) for l in f if l.strip()]
        except FileNotFoundError:
            return []

    def _status(self):
        try:
            with open(self.status_path, encoding="utf-8") as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return {}

    def add(self, meta, zip_bytes, images, match_bytes=b""):
        """meta: clean_meta's (its "mods": clean_mods', each marked stored here or not); images: [bytes] (checked);
        match_bytes: the match folder's zip -> {id, signature, seen_before}"""
        sig, line = signature(meta, zip_bytes, match_bytes)
        received = time.time()
        secret = secrets.token_urlsafe(18)  # (the sender's receipt: only its hash is kept)
        with self.lock:
            rid = time.strftime("%Y%m%d-%H%M%S", time.localtime(received)) + "-" + secrets.token_hex(3)
            folder = os.path.join(self.dir, _day(rid), rid)
            os.makedirs(folder)
            seen = self.counts.get(sig, 0)
            names = []
            for n, img in enumerate(images):
                name = "image%d.%s" % (n, image_ext(img))
                with open(os.path.join(folder, name), "wb") as f:
                    f.write(img)
                names.append(name)
            if zip_bytes:
                with open(os.path.join(folder, "report.zip"), "wb") as f:
                    f.write(zip_bytes)
            summary = None
            if match_bytes:
                with open(os.path.join(folder, "match.zip"), "wb") as f:
                    f.write(match_bytes)
                summary = match_summary(match_bytes) or {}
            mods = [dict(m, stored=self.has_mod(m["hash"])) for m in meta.get("mods", [])]
            meta = dict(meta, mods=mods)
            rec = dict(meta, id=rid, received=time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(received)),
                       received_at=round(received), signature=sig, signature_line=line, seen_before=seen,
                       zip_size=len(zip_bytes or b""), images=names, match=summary,
                       match_size=len(match_bytes or b""), secret_hash=secret_hash(secret))
            with open(os.path.join(folder, "report.json"), "w", encoding="utf-8") as f:
                json.dump(rec, f, indent=2, ensure_ascii=False)
            row = {k: rec.get(k) for k in ("id", "received", "received_at", "app", "version", "platform", "problem",
                                           "happens", "signature", "signature_line", "seen_before", "zip_size",
                                           "images", "account", "match", "match_size", "secret_hash")}
            row["mods"] = [{k: m[k] for k in ("hash", "name", "type", "id", "size", "stored")} for m in mods]
            row["what"] = meta.get("what", "")[:200]
            if meta.get("updater"):
                row["updater"] = meta["updater"][:300]  # (the updater's last result: the dashboard's What column)
            with open(self.index_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(row, ensure_ascii=False) + "\n")
            self.counts[sig] = seen + 1
        return {"id": rid, "signature": sig, "seen_before": seen, "secret": secret}

    def list(self, limit=500):
        """The newest reports first (index lines + their dashboard marks + their signature's count)."""
        with self.lock:
            rows, marks, counts = self._rows(), self._status(), dict(self.counts)
        rows = rows[-limit:][::-1]
        for r in rows:
            r.pop("secret_hash", None)
            r["status"], detail = _mark_of(marks.get(r["id"]))
            if detail and r["status"] == "fixed":
                r["fix"] = detail
            elif detail:
                r["info"] = detail
            r["count"] = counts.get(r.get("signature", ""), 1)
            r["replies"] = len(self._replies(r["id"])) if r["status"] in ("needs_info", "replied") else 0
        return rows

    # (the players' side: receipts)
    def _replies(self, rid):
        try:
            with open(os.path.join(self.dir, _day(rid), rid, "replies.jsonl"), encoding="utf-8") as f:
                return [json.loads(l) for l in f if l.strip()]
        except (OSError, ValueError):
            return []

    def _public(self, row, mark):
        """A report's public state for its sender: never its logs, zip, pictures, contact or signature."""
        status, detail = _mark_of(mark)
        out = {"id": row["id"], "state": STATES.get(status, "received"), "received": row.get("received", ""),
               "app": row.get("app", ""), "problem": row.get("problem", ""), "what": (row.get("what") or "")[:160]}
        if status == "fixed" and detail:
            out.update(note=detail.get("note", ""), version=detail.get("version", ""), date=detail.get("date", ""))
        elif status in ("needs_info", "replied") and detail:
            out.update(question=detail.get("question", ""), date=detail.get("date", ""))
        if status in ("needs_info", "replied"):
            out["replies"] = len(self._replies(row["id"]))
        return out

    def status(self, receipts, account=None):
        """[{id, secret}] (and, signed in, the account's name) -> the public state of each report whose secret
        matches ({id, state: unknown} for the rest), then the account's other reports (from_account)."""
        with self.lock:
            rows = {r.get("id"): r for r in self._rows()}
            marks = self._status()
        out, seen = [], set()
        for rc in (receipts if isinstance(receipts, list) else [])[:MAX_STATUS_IDS]:
            if not isinstance(rc, dict):
                continue
            rid = str(rc.get("id") or "")
            row = rows.get(rid) if _valid_id(rid) else None
            if rid in seen:
                continue
            seen.add(rid)
            if not row or not row.get("secret_hash") or not secrets.compare_digest(
                    row["secret_hash"], secret_hash(rc.get("secret"))):
                out.append({"id": rid[:40], "state": "unknown"})
                continue
            out.append(self._public(row, marks.get(rid)))
        if account:
            for rid, row in rows.items():
                if rid not in seen and row.get("account") == account:
                    out.append(dict(self._public(row, marks.get(rid)), from_account=True))
        return out

    def reply(self, rid, secret, text, images, account=None):
        """The sender's answer to a question (needs_info / replied): kept in replies.jsonl (+ its pictures), the
        mark becomes replied -> (True, "") or (False, why)."""
        with self.lock:
            row = next((r for r in self._rows() if r.get("id") == rid), None) if _valid_id(rid) else None
            ok = bool(row and row.get("secret_hash") and secrets.compare_digest(row["secret_hash"], secret_hash(secret)))
            if not ok and not (row and account and row.get("account") == account):
                return False, "No such report."
            marks = self._status()
            status, detail = _mark_of(marks.get(rid))
            if status not in ("needs_info", "replied"):
                return False, "This report isn't waiting for an answer."
            folder = os.path.join(self.dir, _day(rid), rid)
            replies = self._replies(rid)
            if len(replies) >= MAX_REPLIES:
                return False, "This report has all the answers it can take - post on our Discord instead."
            n = len(replies)
            names = []
            for k, img in enumerate(images[:MAX_REPLY_IMAGES]):
                name = "reply%d_%d.%s" % (n, k, image_ext(img))
                with open(os.path.join(folder, name), "wb") as f:
                    f.write(img)
                names.append(name)
            with open(os.path.join(folder, "replies.jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps({"date": time.strftime("%Y-%m-%d %H:%M:%S"), "text": text[:MAX_REPLY],
                                    "images": names}, ensure_ascii=False) + "\n")
            marks[rid] = dict(detail or clean_info({}), status="replied")
            self._write_status(marks)
        return True, ""

    def file(self, rid, name):
        """One of a report's files (report.json, report.zip, imageN.ext) -> bytes or None."""
        if not _valid_id(rid) or not re.fullmatch(r"(report|match)\.zip|report\.json|image[0-4]\.(png|jpg|webp)|"
                                                  r"replies\.jsonl|reply\d_\d\.(png|jpg|webp)", name or ""):
            return None
        try:
            with open(os.path.join(self.dir, _day(rid), rid, name), "rb") as f:
                return f.read()
        except OSError:
            return None

    # (the mods: each kept once, named by its content hash)
    def mod_path(self, h):
        return os.path.join(self.mods_dir, h + ".svrmod") if re.fullmatch(r"[0-9a-f]{64}", h or "") else None

    def has_mod(self, h):
        path = self.mod_path(h)
        return bool(path) and os.path.isfile(path)

    def has_mods(self, hashes):
        return [h for h in hashes if self.has_mod(h)]

    def mod_usage(self):
        return {"bytes": self.mod_bytes, "quota": self.mod_quota,
                "count": sum(1 for e in os.scandir(self.mods_dir) if e.name.endswith(".svrmod"))}

    def put_mod(self, h, src_path):
        """A mod sent with a report (a zip at src_path, taken) -> (stored, why not)."""
        path = self.mod_path(h)
        try:
            size = os.path.getsize(src_path)
            if not path:
                return False, "bad hash"
            if self.has_mod(h):
                return True, ""
            if size > MAX_MOD:
                return False, "too large"
            if self.mod_bytes + size > self.mod_quota:
                return False, "server mod store full"
            got = content_hash(src_path)
            if got is None:
                return False, "not a zip"
            if got != h:
                return False, "hash mismatch"
            with self.lock:
                if not self.has_mod(h):
                    shutil.move(src_path, path)
                    self.mod_bytes += size
            return True, ""
        finally:
            if os.path.exists(src_path):
                os.remove(src_path)

    def open_mod(self, h):
        """-> (an open file, its size, None) or None"""
        path = self.mod_path(h)
        if not path or not os.path.isfile(path):
            return None
        return open(path, "rb"), os.path.getsize(path), None

    def open_replay(self, rid):
        """-> (the replay pack open, its size, its temporary path: the caller removes it once sent) or None"""
        path = self.replay_pack(rid)
        return (open(path, "rb"), os.path.getsize(path), path) if path else None

    def replay_pack(self, rid):
        """One zip for replaying a report's match: match/ (the match folder), mods/<type>_<id>_<hash>.svrmod (each
        stored mod), report.json and mods.txt -> the path of a temporary file (the caller removes it), or None."""
        if not _valid_id(rid):
            return None
        folder = os.path.join(self.dir, _day(rid), rid)
        try:
            with open(os.path.join(folder, "report.json"), encoding="utf-8") as f:
                rec = json.load(f)
        except (OSError, ValueError):
            return None
        tmp_dir = os.path.join(self.dir, "tmp")
        os.makedirs(tmp_dir, exist_ok=True)
        out = os.path.join(tmp_dir, "replay_%s_%s.zip" % (rid, secrets.token_hex(4)))
        lines = []
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
            z.writestr("report.json", json.dumps(rec, indent=2, ensure_ascii=False))
            match = os.path.join(folder, "match.zip")
            if os.path.isfile(match):
                try:
                    with zipfile.ZipFile(match) as m:
                        for info in m.infolist():
                            if not info.is_dir() and ".." not in info.filename.replace("\\", "/").split("/"):
                                z.writestr("match/" + info.filename.replace("\\", "/"), m.read(info))
                except zipfile.BadZipFile:
                    z.write(match, "match.zip")
            for m in rec.get("mods", []):
                path = self.mod_path(m.get("hash", ""))
                if path and os.path.isfile(path):
                    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", "%s_%s" % (m.get("type") or "mod", m.get("id") or m.get("name") or "mod"))
                    z.write(path, "mods/%s_%s.svrmod" % (name, m["hash"][:12]))
                    lines.append("%s\t%s\t%s\t%s\tincluded" % (m.get("type"), m.get("id"), m.get("version"), m["hash"]))
                else:
                    lines.append("%s\t%s\t%s\t%s\tnot sent" % (m.get("type"), m.get("id"), m.get("version"), m.get("hash")))
            z.writestr("mods.txt", "\n".join(lines) + "\n")
        return out

    def _write_status(self, marks):
        tmp = self.status_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(marks, f, indent=1, ensure_ascii=False)
        os.replace(tmp, self.status_path)

    def mark(self, rid, status, fix=None):
        """One report: done / duplicate / looking / fixed (fix: {note, commit, version, date}) / needs_info (fix:
        {question}) / '' (open again)."""
        return bool(self.mark_many(rid, status, fix))

    def mark_many(self, target, status, fix=None):
        """A report id, or a signature (every report with it) -> the ids marked ([] when none)."""
        if status not in STATUSES or status == "replied":  # (replied: only a player's answer sets it)
            return []
        if status == "needs_info" and not clean_info(fix)["question"]:
            return []
        value = (dict(clean_fix(fix), status="fixed") if status == "fixed" else
                 dict(clean_info(fix), status="needs_info") if status == "needs_info" else status)
        with self.lock:
            if _valid_id(target):
                ids = [target] if os.path.isdir(os.path.join(self.dir, _day(target), target)) else []
            elif re.fullmatch(r"[0-9a-f]{16}", target or ""):
                ids = [r["id"] for r in self._rows() if r.get("signature") == target]
            else:
                ids = []
            if not ids:
                return []
            marks = self._status()
            for rid in ids:
                if value:
                    marks[rid] = value
                else:
                    marks.pop(rid, None)
            self._write_status(marks)
        return ids

    def resign(self, write=False):
        """Every report signed again with the current rule -> [(id, old signature, new, old line, new line)]. write:
        report.json and index.jsonl get the new signature / line, seen_before counted again in received order."""
        out, counts = [], {}
        rows = sorted(self._rows(), key=lambda r: r.get("id", ""))
        for row in rows:
            rid = row.get("id", "")
            folder = os.path.join(self.dir, _day(rid), rid)
            try:
                with open(os.path.join(folder, "report.json"), encoding="utf-8") as f:
                    rec = json.load(f)
            except (OSError, ValueError):
                continue
            read = lambda n: open(os.path.join(folder, n), "rb").read() if os.path.exists(os.path.join(folder, n)) else b""
            meta = {"app": rec.get("app", ""), "version": rec.get("version", ""), "problem": rec.get("problem", ""),
                    "client_signature": rec.get("client_signature", "")}
            sig, line = signature(meta, read("report.zip"), read("match.zip"))
            out.append((rid, rec.get("signature", ""), sig, rec.get("signature_line", ""), line))
            if write:
                seen = counts.get(sig, 0)
                counts[sig] = seen + 1
                rec.update(signature=sig, signature_line=line, seen_before=seen)
                with open(os.path.join(folder, "report.json"), "w", encoding="utf-8") as f:
                    json.dump(rec, f, indent=2, ensure_ascii=False)
                row.update(signature=sig, signature_line=line, seen_before=seen)
        if write:
            with self.lock:
                tmp = self.index_path + ".tmp"
                with open(tmp, "w", encoding="utf-8") as f:
                    for row in rows:
                        f.write(json.dumps(row, ensure_ascii=False) + "\n")
                os.replace(tmp, self.index_path)
                self.counts = {}
                for row in rows:
                    self.counts[row.get("signature", "")] = self.counts.get(row.get("signature", ""), 0) + 1
        return out

    def delete(self, rid):
        """A report gone for good: its folder, its index line and its mark."""
        if not _valid_id(rid):
            return False
        with self.lock:
            folder = os.path.join(self.dir, _day(rid), rid)
            rows = self._rows()
            keep = [r for r in rows if r.get("id") != rid]
            if len(keep) == len(rows) and not os.path.isdir(folder):
                return False
            shutil.rmtree(folder, ignore_errors=True)
            try:
                os.rmdir(os.path.dirname(folder))  # (the day's folder, once empty)
            except OSError:
                pass
            tmp = self.index_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                for r in keep:
                    f.write(json.dumps(r, ensure_ascii=False) + "\n")
            os.replace(tmp, self.index_path)
            marks = self._status()
            if marks.pop(rid, None) is not None:
                self._write_status(marks)
            self.counts = {}
            for r in keep:
                self.counts[r.get("signature", "")] = self.counts.get(r.get("signature", ""), 0) + 1
            # (its mods, when no other report has them)
            used = {m.get("hash") for r in keep for m in r.get("mods") or []}
            gone = [m.get("hash") for r in rows if r.get("id") == rid for m in r.get("mods") or []]
            for h in gone:
                path = self.mod_path(h or "")
                if path and h not in used and os.path.isfile(path):
                    self.mod_bytes -= os.path.getsize(path)
                    os.remove(path)
        return True


def pack(meta, zip_bytes, images, match_bytes=b""):
    """The core -> storage node body: a stored zip of report.json + report.zip + imageN.ext."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("meta.json", json.dumps(meta, ensure_ascii=False))
        if zip_bytes:
            z.writestr("report.zip", zip_bytes)
        for n, img in enumerate(images):
            z.writestr("image%d" % n, img)
        if match_bytes:
            z.writestr("match.zip", match_bytes)
    return out.getvalue()


def unpack(body):
    with zipfile.ZipFile(io.BytesIO(body)) as z:
        names = z.namelist()
        meta = json.loads(z.read("meta.json").decode("utf-8"))
        zip_bytes = z.read("report.zip") if "report.zip" in names else b""
        images = [z.read("image%d" % n) for n in range(MAX_IMAGES) if "image%d" % n in names]
        match_bytes = z.read("match.zip") if "match.zip" in names else b""
    return meta, zip_bytes, images, match_bytes


class RemoteReports:
    """The reports on the storage node (--role core): POST /reports, GET /reports, GET /reports/<id>/<name>,
    POST /reports/<id>/mark with the shared key. Writes raise when the node is down."""

    def __init__(self, url, key, timeout=60):
        self.url = url.rstrip("/")
        self.key = key
        self.timeout = timeout

    def _req(self, method, path, data=None, ctype=None):
        import urllib.request
        headers = {"X-Storage-Key": self.key}
        if ctype:
            headers["Content-Type"] = ctype
        r = urllib.request.Request(self.url + path, data=data, method=method, headers=headers)
        return urllib.request.urlopen(r, timeout=self.timeout)

    def add(self, meta, zip_bytes, images, match_bytes=b""):
        with self._req("POST", "/reports", pack(meta, zip_bytes, images, match_bytes), "application/zip") as f:
            return json.loads(f.read())

    def has_mods(self, hashes):
        with self._req("POST", "/reports/mods/check", json.dumps({"hashes": hashes}).encode(), "application/json") as f:
            return json.loads(f.read()).get("have", [])

    def status(self, receipts, account=None):
        with self._req("POST", "/reports/status", json.dumps({"receipts": receipts, "account": account}).encode(),
                       "application/json") as f:
            return json.loads(f.read()).get("reports", [])

    def reply(self, rid, secret, text, images, account=None):
        body = pack({"id": rid, "secret": secret, "text": text, "account": account}, b"", images)
        with self._req("POST", "/reports/reply", body, "application/zip") as f:
            d = json.loads(f.read())
        return bool(d.get("ok")), d.get("why", "")

    def mod_usage(self):
        with self._req("GET", "/reports/mods/usage") as f:
            return json.loads(f.read())

    def put_mod(self, h, src_path):
        """Streams the file to the node (it checks the hash and the quota) -> (stored, why not)."""
        import urllib.request
        try:
            size = os.path.getsize(src_path)
            with open(src_path, "rb") as body:
                r = urllib.request.Request(self.url + "/reports/mods/" + h, data=body, method="POST",
                                           headers={"X-Storage-Key": self.key, "Content-Length": str(size),
                                                    "Content-Type": "application/zip"})
                with urllib.request.urlopen(r, timeout=max(self.timeout, 600)) as f:
                    d = json.loads(f.read())
            return bool(d.get("ok")), d.get("why", "")
        except Exception as e:
            return False, "the storage node didn't take it (%s)" % e.__class__.__name__
        finally:
            if os.path.exists(src_path):
                os.remove(src_path)

    def open_mod(self, h):
        try:
            f = self._req("GET", "/reports/mods/" + h)
            return f, int(f.headers.get("Content-Length") or 0), None
        except Exception:
            return None

    def open_replay(self, rid):
        try:
            f = self._req("GET", "/reports/%s/replay.zip" % rid)
            return f, int(f.headers.get("Content-Length") or 0), None
        except Exception:
            return None

    def list(self, limit=500):
        with self._req("GET", "/reports?limit=%d" % limit) as f:
            return json.loads(f.read())

    def file(self, rid, name):
        try:
            with self._req("GET", "/reports/%s/%s" % (rid, name)) as f:
                return f.read()
        except Exception:
            return None

    def mark(self, rid, status, fix=None):
        return bool(self.mark_many(rid, status, fix))

    def mark_many(self, target, status, fix=None):
        try:
            with self._req("POST", "/reports/%s/mark" % target, json.dumps({"status": status, "fix": fix}).encode(),
                           "application/json") as f:
                return json.loads(f.read() or b"{}").get("ids", [])
        except Exception:
            return []

    def delete(self, rid):
        try:
            with self._req("POST", "/reports/%s/delete" % rid) as f:
                return f.status == 200
        except Exception:
            return False


def node_routes(app, folder, key_ok, mod_quota=MOD_QUOTA):
    """The storage node's side (server.py storage_app)."""
    import asyncio
    from aiohttp import web
    reports = LocalReports(folder)
    reports.mod_quota = mod_quota

    async def mods_check(request):
        if not key_ok(request):
            return web.Response(status=403)
        try:
            hashes = [str(h) for h in (await request.json()).get("hashes", [])][:MAX_MODS]
        except (ValueError, AttributeError):
            return web.Response(status=400)
        return web.json_response({"have": reports.has_mods(hashes)})

    async def mods_usage(request):
        if not key_ok(request):
            return web.Response(status=403)
        return web.json_response(reports.mod_usage())

    async def mod_put(request):
        if not key_ok(request):
            return web.Response(status=403)
        h = request.match_info["hash"]
        if not reports.mod_path(h):
            return web.json_response({"ok": False, "why": "bad hash"}, status=400)
        if (request.content_length or 0) > MAX_MOD:
            return web.json_response({"ok": False, "why": "too large"}, status=413)
        tmp_dir = os.path.join(reports.dir, "tmp")
        os.makedirs(tmp_dir, exist_ok=True)
        tmp = os.path.join(tmp_dir, "in_%s_%s.zip" % (h[:16], secrets.token_hex(4)))
        got = 0
        try:
            with open(tmp, "wb") as f:
                while True:
                    chunk = await request.content.read(1 << 20)
                    if not chunk:
                        break
                    got += len(chunk)
                    if got > MAX_MOD:
                        return web.json_response({"ok": False, "why": "too large"}, status=413)
                    f.write(chunk)
        except Exception:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise
        if got > MAX_MOD:
            os.remove(tmp)
        ok, why = await asyncio.get_running_loop().run_in_executor(None, reports.put_mod, h, tmp)
        return web.json_response({"ok": ok, "why": why})

    async def mod_get(request):
        if not key_ok(request):
            return web.Response(status=403)
        path = reports.mod_path(request.match_info["hash"])
        if not path or not os.path.isfile(path):
            return web.Response(status=404)
        return web.FileResponse(path, headers={"Content-Type": "application/zip"})

    async def replay(request):
        if not key_ok(request):
            return web.Response(status=403)
        path = await asyncio.get_running_loop().run_in_executor(None, reports.replay_pack, request.match_info["id"])
        if not path:
            return web.Response(status=404)
        resp = web.StreamResponse(headers={"Content-Type": "application/zip",
                                           "Content-Length": str(os.path.getsize(path))})
        await resp.prepare(request)
        try:
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    await resp.write(chunk)
        finally:
            os.remove(path)
        return resp

    async def add(request):
        if not key_ok(request):
            return web.Response(status=403)
        body = await request.read()
        loop = asyncio.get_running_loop()
        try:
            meta, zip_bytes, images, match_bytes = unpack(body)
        except Exception:
            return web.json_response({"ok": False, "error": "bad body"}, status=400)
        return web.json_response(dict({"ok": True}, **await loop.run_in_executor(None, reports.add, meta, zip_bytes,
                                                                                  images, match_bytes)))

    async def listing(request):
        if not key_ok(request):
            return web.Response(status=403)
        limit = max(1, min(5000, int(request.query.get("limit", "500") or 500)))
        return web.json_response(await asyncio.get_running_loop().run_in_executor(None, reports.list, limit))

    async def one(request):
        if not key_ok(request):
            return web.Response(status=403)
        data = await asyncio.get_running_loop().run_in_executor(
            None, reports.file, request.match_info["id"], request.match_info["name"])
        return web.Response(status=404) if data is None else web.Response(body=data,
                                                                           content_type="application/octet-stream")

    async def mark(request):
        if not key_ok(request):
            return web.Response(status=403)
        try:
            body = await request.json()
            status, fix = str(body.get("status", "")), body.get("fix")
        except (ValueError, AttributeError):
            return web.Response(status=400)
        ids = reports.mark_many(request.match_info["id"], status, fix)
        return web.json_response({"ok": bool(ids), "ids": ids}, status=200 if ids else 404)

    async def delete(request):
        if not key_ok(request):
            return web.Response(status=403)
        done = await asyncio.get_running_loop().run_in_executor(None, reports.delete, request.match_info["id"])
        return web.Response(status=200 if done else 404)

    async def status(request):
        if not key_ok(request):
            return web.Response(status=403)
        try:
            body = await request.json()
        except ValueError:
            return web.Response(status=400)
        rows = await asyncio.get_running_loop().run_in_executor(None, reports.status, body.get("receipts"),
                                                                body.get("account"))
        return web.json_response({"reports": rows})

    async def reply(request):
        if not key_ok(request):
            return web.Response(status=403)
        try:
            meta, _, images, _ = unpack(await request.read())
        except Exception:
            return web.json_response({"ok": False, "why": "bad body"}, status=400)
        ok, why = await asyncio.get_running_loop().run_in_executor(
            None, reports.reply, str(meta.get("id", "")), meta.get("secret"), str(meta.get("text", "")), images,
            meta.get("account"))
        return web.json_response({"ok": ok, "why": why})

    app.router.add_post("/reports/mods/check", mods_check)
    app.router.add_post("/reports/status", status)
    app.router.add_post("/reports/reply", reply)
    app.router.add_get("/reports/mods/usage", mods_usage)
    app.router.add_post("/reports/mods/{hash}", mod_put)
    app.router.add_get("/reports/mods/{hash}", mod_get)
    app.router.add_get("/reports/{id}/replay.zip", replay)
    app.router.add_post("/reports", add)
    app.router.add_get("/reports", listing)
    app.router.add_post("/reports/{id}/mark", mark)
    app.router.add_post("/reports/{id}/delete", delete)
    app.router.add_get("/reports/{id}/{name}", one)


def main():
    """On the machine that keeps the reports (the storage node's PC, or --role all), server running or not:
        python reports.py --data <data folder> mark <report id | signature> --fixed "how it was fixed"
                          --commit abc1234 --version 2.1.6 [--date 2026-10-25]
        python reports.py --data <data folder> mark <id | signature> --needs-info "the question for the player"
        python reports.py --data <data folder> mark <id | signature> --looking | --done | --duplicate | --reopen
        python reports.py --data <data folder> delete <report id>
        python reports.py --data <data folder> list [--fixed]
        python reports.py --data <data folder> resign [--write]   (every report signed again: before / after)
    A signature marks every report with it (the dashboard's groups). The dashboard shows the fixed ones under
    "Fixed while you were away". The players see the state in their launcher's Reports tab: received, looking,
    "Fixed - in the next update (<version>)" with the note, "We need more information: <question>" (they can
    answer it there), done, duplicate."""
    import argparse
    ap = argparse.ArgumentParser(description=main.__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="the server's --data folder (the one holding reports/)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mark")
    m.add_argument("target", help="a report id (20261025-101500-abc123) or a signature (16 hex)")
    how = m.add_mutually_exclusive_group(required=True)
    how.add_argument("--fixed", metavar="NOTE", help="fixed: a short plain-English note of how")
    how.add_argument("--needs-info", metavar="QUESTION", help="ask the player (their Reports tab shows it)")
    how.add_argument("--looking", action="store_true", help="we're looking at it")
    how.add_argument("--done", action="store_true")
    how.add_argument("--duplicate", action="store_true")
    how.add_argument("--reopen", action="store_true")
    m.add_argument("--commit", default="")
    m.add_argument("--version", default="", help="the version the fix ships in, e.g. 2.1.6")
    m.add_argument("--date", default="", help="YYYY-MM-DD (default: today)")
    d = sub.add_parser("delete")
    d.add_argument("id")
    ls = sub.add_parser("list")
    ls.add_argument("--fixed", action="store_true", help="only the fixed ones")
    rs = sub.add_parser("resign")
    rs.add_argument("--write", action="store_true", help="keep the new signatures (else only shown)")
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(a.data, "reports")):
        raise SystemExit("No reports folder in %s" % a.data)
    store = LocalReports(a.data)
    if a.cmd == "mark":
        status = ("fixed" if a.fixed is not None else "needs_info" if a.needs_info is not None else
                  "looking" if a.looking else "done" if a.done else "duplicate" if a.duplicate else "")
        fix = ({"note": a.fixed, "commit": a.commit, "version": a.version, "date": a.date} if status == "fixed" else
               {"question": a.needs_info, "date": a.date} if status == "needs_info" else None)
        ids = store.mark_many(a.target, status, fix)
        if not ids:
            raise SystemExit("No report with that id or signature.")
        print("%s: %d report(s): %s" % (status or "reopened", len(ids), ", ".join(ids)))
    elif a.cmd == "resign":
        for rid, old, new, old_line, new_line in store.resign(a.write):
            print("%s  %s -> %s%s\n    was: %s\n    now: %s" % (rid, old, new, "" if old != new else " (same)",
                                                              old_line[:150], new_line[:150]))
        print("written" if a.write else "(shown only: --write keeps them)")
    elif a.cmd == "delete":
        if not store.delete(a.id):
            raise SystemExit("No report %s." % a.id)
        print("deleted", a.id)
    else:
        for r in store.list(5000):
            if a.fixed and r["status"] != "fixed":
                continue
            fix, info = r.get("fix"), r.get("info")
            print("%s  %-10s %s %s  %s%s%s" % (r["id"], r["status"] or "open", r.get("app"), r.get("version"),
                                              r.get("problem"), "  -> %s (%s, %s)" % (fix["note"], fix["commit"],
                                                                                      fix["version"]) if fix else "",
                                              "  ? %s (%d answer(s))" % (info["question"], r["replies"]) if info else ""))


if __name__ == "__main__":
    main()
