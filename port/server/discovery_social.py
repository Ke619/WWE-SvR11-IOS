"""Discovery comments and star ratings (docs/DISCOVERY_API.md, "Comments and ratings").

Players signed in to an online account comment on a mod's page (plain text, flat, newest first) and rate it 1-5
stars (one rating per account per mod, changeable; not their own mod). The server checks every comment itself
(server.py discovery_comment_why: links, the name filter, the comment word list, rate limits); the dashboard's
Discovery comments tab hides / deletes comments, mutes accounts and clears a mod's ratings.

A comment's state: 0 shown, 1 hidden by the dashboard, 2 deleted by its writer or the mod's owner. Deleted ones are
kept KEEP_DELETED (the dashboard still sees a reported comment its writer deleted), then removed by prune().
The mods table keeps rating_sum / rating_count up to date with each rating (the list and its sort stay cheap).
"""
import collections
import re
import time
import unicodedata

MAX_COMMENT = 500
MAX_LINES = 8
PAGE = 20
ADMIN_PAGE = 50
KEEP_DELETED = 30 * 86400
SHOWN, HIDDEN, DELETED = 0, 1, 2
STATES = {SHOWN: "shown", HIDDEN: "hidden", DELETED: "deleted"}
MUTE_SCOPES = ("comments", "all")  # (comments only; comments and ratings)

# The comment word list's seed (comment_words.json in the data folder, edited in the dashboard's Names tab): talk of
# game copies. The name filter's rules: a short term (under 5 letters) only as a whole word or spelled out letter by
# letter ("i s o", "i.s.o", "game.iso" - not "isolated"); a long one inside words too.
COMMENT_WORDS_SEED = [
    "# Discovery comments: talk of game copies (namefilter.py rules - a short term: whole words only)",
    "iso", "isos", "xex", "rom", "roms", "romset", "nsp", "xci", "nsz", "xcz", "pkg", "wbfs", "wad", "cia",
    "torrent", "magnet link", "utorrent", "qbittorrent", "piratebay", "rutracker", "nopaystation",
    "download the game", "download game", "game download", "free download", "full game download",
    "cracked", "warez",
]
COMMENT_WORDS_ALLOW = ["# words never flagged though a term is inside them"]

# Split archives and disc image file names that the word list's words alone don't read (".7z.001", ".part1.rar")
_ARCHIVE = re.compile(r"\.(?:7z|zip|rar)\.\d{2,3}\b|\.part\d{1,3}\.rar\b|\.[zr]\d{2}\b", re.I)
# Invite links has_url doesn't count as an address (no www., no known ending): "discord.gg/x", ".gg/x", "/invite/x"
_INVITE = re.compile(r"(?:discord(?:app)?\s*\.\s*(?:gg|com|me|io)|\.gg\s*/|/invite/|\bt\s*\.\s*me\s*/)", re.I)

COMMENT_WHY = {
    "comments_off": "Comments are off for now.",
    "ratings_off": "Ratings are off for now.",
    "sign_in_needed": "Sign in to an online account first: the launcher's Online tab.",
    "muted": "Your account can't comment on Discovery.",
    "muted_ratings": "Your account can't rate mods on Discovery.",
    "empty": "Write something first.",
    "too_long": "Comments are %d characters at most." % MAX_COMMENT,
    "too_many_lines": "Comments are %d lines at most." % MAX_LINES,
    "link": "Comments can't have links, web addresses or invites.",
    "word": "That comment has a word that isn't allowed.",
    "account_name": "Your account name isn't allowed on Community Creations.",
    "piracy": "Comments can't talk about copies of games or where to get them (ISO, XEX, ROM, torrents...).",
    "repeat": "You posted that already.",
    "too_fast": "You're commenting too fast - wait a few minutes.",
    "rate_too_fast": "Too many ratings - wait a while.",
    "report_too_fast": "Too many reports - wait a while.",
    "own_mod": "You can't rate your own mod.",
    "bad_stars": "Ratings are 1 to 5 stars (0 takes yours back).",
    "no_mod": "No such mod.",
    "no_comment": "No such comment.",
    "not_yours": "Only its writer or the mod's uploader can delete it.",
}


def init(store):
    with store.lock:
        store.db.executescript("""
            CREATE TABLE IF NOT EXISTS mod_comments (
                id INTEGER PRIMARY KEY AUTOINCREMENT, mod INTEGER, account INTEGER, text TEXT, created REAL,
                state INTEGER DEFAULT 0, changed REAL, changed_by TEXT, reports INTEGER DEFAULT 0, ip TEXT);
            CREATE INDEX IF NOT EXISTS mod_comments_mod ON mod_comments (mod, state, created);
            CREATE INDEX IF NOT EXISTS mod_comments_account ON mod_comments (account, created);
            CREATE TABLE IF NOT EXISTS mod_ratings (
                mod INTEGER, account INTEGER, stars INTEGER, at REAL, PRIMARY KEY (mod, account));
            CREATE INDEX IF NOT EXISTS mod_ratings_account ON mod_ratings (account);
            CREATE TABLE IF NOT EXISTS mod_comment_reports (
                comment INTEGER, account INTEGER, why TEXT, at REAL, PRIMARY KEY (comment, account));
            CREATE TABLE IF NOT EXISTS discovery_mutes (
                account INTEGER PRIMARY KEY, scope TEXT, note TEXT, at REAL);
        """)
        have = {r[1] for r in store.db.execute("PRAGMA table_info(mods)")}
        for col in ("rating_sum", "rating_count"):
            if col not in have:
                store.db.execute("ALTER TABLE mods ADD COLUMN %s INTEGER DEFAULT 0" % col)
        store.db.commit()


class Limiter:
    """Sliding windows in memory; check() looks at every window first and counts only when none is full."""

    def __init__(self):
        self.q = collections.defaultdict(collections.deque)

    def check(self, *rules):
        """rules: (key, limit, window seconds). True = refused (nothing counted)."""
        t = time.time()
        for key, limit, window in rules:
            q = self.q[key]
            while q and q[0] < t - window:
                q.popleft()
            if len(q) >= limit:
                return True
        for key, _, _ in rules:
            self.q[key].append(t)
        return False


# -- the text ------------------------------------------------------------------------------------------------------

def clean_text(text):
    """A player's comment as kept: NFC, control characters out (new lines kept), no runs of blank lines, trimmed.
    -> (text, code) - code "" when it may be checked further."""
    text = unicodedata.normalize("NFC", str(text or "")).replace("\r\n", "\n").replace("\r", "\n")
    text = "".join(c for c in text if c == "\n" or (unicodedata.category(c) not in ("Cc", "Cf", "Co", "Cs")))
    text = re.sub(r"[ \t ]+\n", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text).strip()
    if not text:
        return "", "empty"
    if len(text) > MAX_COMMENT:
        return text, "too_long"
    if text.count("\n") + 1 > MAX_LINES:
        return text, "too_many_lines"
    return text, ""


def has_invite(text):
    return bool(_INVITE.search(text or ""))


def has_archive_name(text):
    return bool(_ARCHIVE.search(text or ""))


# -- comments ------------------------------------------------------------------------------------------------------

_CCOLS = "c.id, c.mod, c.account, COALESCE(a.name, '?'), c.text, c.created, c.state, c.changed, c.changed_by, c.reports"


def _comment(r):
    return {"id": r[0], "mod": r[1], "account": r[2], "author": r[3], "text": r[4], "created": r[5],
            "state": STATES.get(r[6], "shown"), "changed": r[7], "changed_by": r[8] or "", "reports": r[9] or 0}


def add_comment(store, mod_id, account_id, text, ip=""):
    with store.lock:
        cur = store.db.execute("INSERT INTO mod_comments (mod, account, text, created, ip) VALUES (?, ?, ?, ?, ?)",
                               (mod_id, account_id, text, time.time(), ip))
        store.db.commit()
        return get_comment(store, cur.lastrowid)


def get_comment(store, comment_id):
    with store.lock:
        r = store.db.execute("SELECT %s FROM mod_comments c LEFT JOIN accounts a ON a.id = c.account WHERE c.id = ?"
                             % _CCOLS, (comment_id,)).fetchone()
    return _comment(r) if r else None


def posted_lately(store, account_id, text, window=600):
    """Has this account posted this same text (any mod) in the last window seconds?"""
    with store.lock:
        r = store.db.execute("SELECT 1 FROM mod_comments WHERE account = ? AND created > ? AND lower(text) = lower(?)"
                             " LIMIT 1", (account_id, time.time() - window, text)).fetchone()
    return bool(r)


def public(c, viewer=None, mod_owner=None):
    """A comment as players get it (no account id, state, IP or reports); can_delete for the viewer."""
    d = {"id": c["id"], "author": c["author"], "text": c["text"], "created": c["created"]}
    if viewer is not None:
        d["mine"] = c["account"] == viewer
        d["can_delete"] = c["account"] == viewer or (mod_owner is not None and mod_owner == viewer)
    return d


def comments(store, mod_id, page=0, viewer=None, mod_owner=None):
    """A mod's shown comments, newest first, PAGE a page."""
    page = max(0, page)
    with store.lock:
        total = store.db.execute("SELECT COUNT(*) FROM mod_comments WHERE mod = ? AND state = 0",
                                 (mod_id,)).fetchone()[0]
        rows = store.db.execute(
            "SELECT %s FROM mod_comments c LEFT JOIN accounts a ON a.id = c.account WHERE c.mod = ? AND c.state = 0"
            " ORDER BY c.created DESC, c.id DESC LIMIT ? OFFSET ?" % _CCOLS, (mod_id, PAGE, page * PAGE)).fetchall()
    return {"total": total, "page": page, "per_page": PAGE,
            "comments": [public(_comment(r), viewer, mod_owner) for r in rows]}


def comment_count(store, mod_id):
    with store.lock:
        return store.db.execute("SELECT COUNT(*) FROM mod_comments WHERE mod = ? AND state = 0",
                                (mod_id,)).fetchone()[0]


def set_state(store, comment_id, state, by):
    with store.lock:
        n = store.db.execute("UPDATE mod_comments SET state = ?, changed = ?, changed_by = ? WHERE id = ?",
                             (state, time.time(), by, comment_id)).rowcount
        store.db.commit()
    return n > 0


def report_comment(store, comment_id, account_id, why):
    """One report per account per comment. True when it is a new one."""
    with store.lock:
        cur = store.db.execute("INSERT OR IGNORE INTO mod_comment_reports VALUES (?, ?, ?, ?)",
                               (comment_id, account_id, (why or "")[:200], time.time()))
        if cur.rowcount:
            store.db.execute("UPDATE mod_comments SET reports = reports + 1 WHERE id = ?", (comment_id,))
        store.db.commit()
        return cur.rowcount > 0


def clear_reports(store, comment_id):
    with store.lock:
        store.db.execute("DELETE FROM mod_comment_reports WHERE comment = ?", (comment_id,))
        n = store.db.execute("UPDATE mod_comments SET reports = 0 WHERE id = ?", (comment_id,)).rowcount
        store.db.commit()
    return n > 0


def delete_comment(store, comment_id):
    """The dashboard's Delete: gone for good, its reports too."""
    with store.lock:
        store.db.execute("DELETE FROM mod_comment_reports WHERE comment = ?", (comment_id,))
        n = store.db.execute("DELETE FROM mod_comments WHERE id = ?", (comment_id,)).rowcount
        store.db.commit()
    return n > 0


def admin_comments(store, page=0, q="", state="", mod_id=None, account_id=None):
    """The dashboard's list: reported ones (not deleted for good) first, then the latest."""
    where, args = [], []
    if state in ("shown", "hidden", "deleted"):
        where.append("c.state = ?")
        args.append({"shown": SHOWN, "hidden": HIDDEN, "deleted": DELETED}[state])
    elif state == "reported":
        where.append("c.reports > 0")
    if mod_id is not None:
        where.append("c.mod = ?")
        args.append(mod_id)
    if account_id is not None:
        where.append("c.account = ?")
        args.append(account_id)
    for word in (q or "").lower().split()[:6]:
        where.append("(lower(c.text) LIKE ? OR lower(COALESCE(a.name, '')) LIKE ?)")
        args += ["%" + word + "%"] * 2
    sql_where = (" WHERE " + " AND ".join(where)) if where else ""
    page = max(0, page)
    with store.lock:
        total = store.db.execute("SELECT COUNT(*) FROM mod_comments c LEFT JOIN accounts a ON a.id = c.account" +
                                 sql_where, args).fetchone()[0]
        rows = store.db.execute(
            "SELECT %s, COALESCE(m.name, '?'), c.ip FROM mod_comments c LEFT JOIN accounts a ON a.id = c.account"
            " LEFT JOIN mods m ON m.id = c.mod%s ORDER BY (c.reports > 0) DESC, c.created DESC LIMIT ? OFFSET ?"
            % (_CCOLS, sql_where), args + [ADMIN_PAGE, page * ADMIN_PAGE]).fetchall()
        mutes = {r[0]: r[1] for r in store.db.execute("SELECT account, scope FROM discovery_mutes")}
    out = []
    for r in rows:
        c = _comment(r)
        c["mod_name"], c["ip"] = r[10], r[11] or ""
        c["muted"] = mutes.get(c["account"], "")
        out.append(c)
    return {"total": total, "page": page, "per_page": ADMIN_PAGE, "comments": out}


def comment_reports(store, comment_id):
    with store.lock:
        rows = store.db.execute(
            "SELECT COALESCE(a.name, '?'), r.why, r.at FROM mod_comment_reports r LEFT JOIN accounts a"
            " ON a.id = r.account WHERE r.comment = ? ORDER BY r.at", (comment_id,)).fetchall()
    return [{"by": r[0], "why": r[1], "at": r[2]} for r in rows]


# -- ratings -------------------------------------------------------------------------------------------------------

def rating_json(rsum, rcount):
    rcount = rcount or 0
    return {"avg": round((rsum or 0) / rcount, 2) if rcount else 0, "count": rcount}


def rating(store, mod_id):
    with store.lock:
        r = store.db.execute("SELECT rating_sum, rating_count FROM mods WHERE id = ?", (mod_id,)).fetchone()
    return rating_json(*(r or (0, 0)))


def my_rating(store, mod_id, account_id):
    with store.lock:
        r = store.db.execute("SELECT stars FROM mod_ratings WHERE mod = ? AND account = ?",
                             (mod_id, account_id)).fetchone()
    return r[0] if r else 0


def _recount(store, mod_id):
    store.db.execute("UPDATE mods SET rating_sum = (SELECT COALESCE(SUM(stars), 0) FROM mod_ratings WHERE mod = ?),"
                     " rating_count = (SELECT COUNT(*) FROM mod_ratings WHERE mod = ?) WHERE id = ?",
                     (mod_id, mod_id, mod_id))


def rate(store, mod_id, account_id, stars):
    """stars 1-5 (the account's rating, new or changed) or 0 (taken back). -> the mod's rating."""
    with store.lock:
        if stars:
            store.db.execute("INSERT INTO mod_ratings VALUES (?, ?, ?, ?) ON CONFLICT (mod, account) DO UPDATE SET"
                             " stars = excluded.stars, at = excluded.at", (mod_id, account_id, stars, time.time()))
        else:
            store.db.execute("DELETE FROM mod_ratings WHERE mod = ? AND account = ?", (mod_id, account_id))
        _recount(store, mod_id)
        store.db.commit()
    return rating(store, mod_id)


def clear_ratings(store, mod_id):
    with store.lock:
        n = store.db.execute("DELETE FROM mod_ratings WHERE mod = ?", (mod_id,)).rowcount
        _recount(store, mod_id)
        store.db.commit()
    return n


# -- mutes ---------------------------------------------------------------------------------------------------------

def muted(store, account_id):
    """The account's mute scope ("comments" / "all") or ""."""
    with store.lock:
        r = store.db.execute("SELECT scope FROM discovery_mutes WHERE account = ?", (account_id,)).fetchone()
    return r[0] if r else ""


def set_mute(store, account_id, scope, note=""):
    """scope "comments" / "all", or "" to unmute."""
    with store.lock:
        if scope:
            store.db.execute("INSERT INTO discovery_mutes VALUES (?, ?, ?, ?) ON CONFLICT (account) DO UPDATE SET"
                             " scope = excluded.scope, note = excluded.note, at = excluded.at",
                             (account_id, scope, (note or "")[:200], time.time()))
        else:
            store.db.execute("DELETE FROM discovery_mutes WHERE account = ?", (account_id,))
        store.db.commit()


def mutes(store):
    with store.lock:
        rows = store.db.execute("SELECT m.account, COALESCE(a.name, '?'), m.scope, m.note, m.at FROM discovery_mutes m"
                                " LEFT JOIN accounts a ON a.id = m.account ORDER BY m.at DESC").fetchall()
    return [{"account": r[0], "name": r[1], "scope": r[2], "note": r[3] or "", "at": r[4]} for r in rows]


# -- clean-up ------------------------------------------------------------------------------------------------------

def drop_mod(store, mod_id):
    """A deleted mod's comments (their reports) and ratings. Under the store's lock (an RLock)."""
    with store.lock:
        store.db.execute("DELETE FROM mod_comment_reports WHERE comment IN (SELECT id FROM mod_comments WHERE mod = ?)",
                         (mod_id,))
        store.db.execute("DELETE FROM mod_comments WHERE mod = ?", (mod_id,))
        store.db.execute("DELETE FROM mod_ratings WHERE mod = ?", (mod_id,))


def drop_account(store, account_id):
    """A deleted account's comments, ratings (the mods' counts again), reports and mute. Under the store's lock."""
    with store.lock:
        rated = [r[0] for r in store.db.execute("SELECT mod FROM mod_ratings WHERE account = ?", (account_id,))]
        store.db.execute("DELETE FROM mod_comment_reports WHERE comment IN (SELECT id FROM mod_comments WHERE"
                         " account = ?)", (account_id,))
        for cid in [r[0] for r in store.db.execute("SELECT comment FROM mod_comment_reports WHERE account = ?",
                                                   (account_id,))]:
            store.db.execute("UPDATE mod_comments SET reports = MAX(reports - 1, 0) WHERE id = ?", (cid,))
        store.db.execute("DELETE FROM mod_comment_reports WHERE account = ?", (account_id,))
        store.db.execute("DELETE FROM mod_comments WHERE account = ?", (account_id,))
        store.db.execute("DELETE FROM mod_ratings WHERE account = ?", (account_id,))
        store.db.execute("DELETE FROM discovery_mutes WHERE account = ?", (account_id,))
        for mid in rated:
            _recount(store, mid)


def prune(store, keep=KEEP_DELETED):
    """Comments deleted by their writer / the mod's owner over keep seconds ago: removed. -> how many."""
    with store.lock:
        old = [r[0] for r in store.db.execute("SELECT id FROM mod_comments WHERE state = ? AND changed < ?",
                                              (DELETED, time.time() - keep))]
        for cid in old:
            store.db.execute("DELETE FROM mod_comment_reports WHERE comment = ?", (cid,))
            store.db.execute("DELETE FROM mod_comments WHERE id = ?", (cid,))
        store.db.commit()
    return len(old)
