#!/usr/bin/env python3
"""WWE SmackDown vs. Raw 2011 (PC port) - the Community Creations server.

One aiohttp service with two halves:

  <base>/...        what the game and the launchers use (the port relays the
                    game's GameSpy traffic here over HTTP(S), see
                    port/src/online.cpp); behind Project Index this is its
                    public mount, e.g. https://sho-ti.me/svr
      api/register, api/login, api/logout     accounts (name + password; 2.1.6 sign-up needs an invite
                                              code - signup.py)
      api/account, api/reactivate, api/password/{reset,change}
                                              the launcher's account steps (signup.py: codes only)
      api/session                             the game's login (profile, ticket)
      api/logo/<hash>, api/logos/wanted       the port's extra Superstar logos
      api/status                              is the server up
      game/<GameSpy path>                     the game's own requests (gamespy.py)
      api/relay/<id>                          the online match relay (relay.py)
  /                 the admin dashboard: connections, bandwidth, storage,
                    players, uploads and accounts (Project Index admins, or
                    anyone on this PC when run on its own); the name filter's
                    list (namefilter.py, docs/NAME_FILTER.md)

Everything a player does is tied to their account (a bearer token from
api/login): the game's own identities - its XUID, login ticket - are not
trusted.

    python server.py [--host 127.0.0.1] [--port 8101] [--data ./data] [--base /svr]

Needs Python 3.9+ and aiohttp.
"""

import argparse
import asyncio
import base64
import collections
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import sqlite3
import sys
import threading
import time
import traceback
from pathlib import Path

os.environ.setdefault("AIOHTTP_NOSENDFILE", "1")

from aiohttp import web

import admin_auth as aa
import gamespy as gs
import discovery
import leaderboards
import match_names
import media
import namefilter
import relay
import reports
import signup
import thumbs

HERE = Path(__file__).resolve().parent
LOOPBACK = frozenset({"127.0.0.1", "::1", "::ffff:127.0.0.1"})
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.\-]{2,14}$")  # 3-15, what the game can show
XUID_BASE = 0x0009000100000000  # + account id: the player's XUID on every device
PBKDF2_ROUNDS = 240000
MAX_FRIENDS = 200  # (a player's list)
NEWS_MAX = 600  # (the NEWS panel shows about 14 lines of 30 characters, then scrolls)
# The game's menu font (every Latin-1 character drawn on the NEWS panel, 21 Oct
# 2026): ASCII, the Latin-1 letters (but x and / signs), the signs in NEWS_LATIN,
# curly double quotes, the ellipsis and TM; anything else drew as "*" (an emoji
# as "????", "o" ordinal as nothing). Look-alikes become ASCII, the rest goes.
NEWS_SAME = {"\u2018": "'", "\u2019": "'", "\u201a": "'", "\u2032": "'", "\u00b4": "'", "\u201e": '"',
             "\u2033": '"', "\u00ab": '"', "\u00bb": '"', "\u2039": "'", "\u203a": "'",
             "\u2013": "-", "\u2014": "-", "\u2012": "-", "\u2015": "-", "\u2212": "-", "\u2022": "-",
             "\u00b7": "-", "\u25cf": "-", "\u2605": "*", "\u2606": "*", "\u00d7": "x", "\u00f7": "/",
             "\u00bd": " 1/2", "\u00bc": " 1/4", "\u00be": " 3/4", "\u00b9": "1", "\u00b2": "2", "\u00b3": "3",
             "\u00ba": "o", "\u00aa": "a", "\u00b1": "+/-", "\u00b5": "u", "\u00a6": "|", "\u00a5": "YEN",
             "\u20ac": "EUR", "\t": " ", "\u00a0": " ", "\u2009": " ", "\u200a": " ", "\u202f": " "}
NEWS_LATIN = set("\u00a1\u00a2\u00a3\u00a9\u00ae\u00b0\u00bf")
NEWS_EXTRA = set("\u201c\u201d\u2026\u2122")


def news_char(ch):
    return (ch == "\n" or " " <= ch <= "~" or ch in NEWS_LATIN or ch in NEWS_EXTRA
            or ("\u00c0" <= ch <= "\u00ff" and ch not in "\u00d7\u00f7"))


def news_clean(text):
    """The dashboard's news as the game can show it: plain text, new lines kept
    (at most two in a row), characters the font lacks replaced or left out."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    out = []
    for c in text:
        c = NEWS_SAME.get(c, c)
        out.extend(ch for ch in c if news_char(ch))
    text = "".join(out)
    text = "\n".join(" ".join(line.split()) for line in text.split("\n"))  # (no runs of spaces)
    while "\n\n\n" in text:
        text = text.replace("\n\n\n", "\n\n")
    return text.strip()


def news_fields(text, parts=5, size=NEWS_MAX // 5 + 20):
    """The NEWS record's Text1-5 for the dashboard's news (the game joins them as
    they are: cut after a space or a new line), or None (empty: the stored record)."""
    if not text:
        return None
    out = []
    while text and len(out) < parts - 1:
        if len(text) <= size:
            break
        cut = max(text.rfind(" ", 0, size), text.rfind("\n", 0, size))
        cut = cut + 1 if cut > 0 else size
        out.append(text[:cut])
        text = text[cut:]
    out.append(text)
    return out + [""] * (parts - len(out))


# Settings the dashboard edits (data/settings.json, read at start; "live" ones
# apply at once, the others at the next start).
SETTINGS_SPEC = {
    "presence_window": {"default": 60, "min": 30, "max": 600, "live": True,
                        "help": "seconds a game stays online after its last presence request (it asks every 20 s)"},
    "invite_life": {"default": 600, "min": 60, "max": 3600, "live": True,
                    "help": "seconds an invite to a match stays"},
    "mods_online": {"default": True, "live": True,
                    "help": "mods in online sessions: off = every game plays online without its mods (no mod sets compared)"},
    "match_network": {"default": "auto", "choices": ["auto", "relay", "p2p"], "live": True,
                      "help": "how online matches connect: auto = directly, through the relay when that fails; "
                              "relay = every match through this server's relay; p2p = matches directly only (the "
                              "relay just helps games find each other) - for when the server struggles. New "
                              "sessions follow it (games ask every 20 s); running matches keep theirs"},
    # (signup.py: off until 2.1.6 is out - every account plays as before, old launchers sign up as before; the
    # codes work, to get accounts ready. On: invite sign-up only, and online needs an active account - its sign-up
    # or re-activation code used)
    "account_checks": {"default": "off", "choices": ["off", "on"], "live": True,
                       "help": "the 2.1.6 account rules: off = nobody is blocked (2.1.5 launchers and games as "
                               "before; codes can be made ready); on = new accounts need an invite code, and "
                               "accounts from before need their re-activation code to play online - 2.1.5 "
                               "launchers are told to update. At once"},
    # (the Online Rollback Netcode chat's: in every /api/friends config; the host latches it into session property
    # 0x1000F005 at session creation - 0 lockstep, 2 lockstep_vclock, 1 rollback; joiners follow the session's,
    # none or unknown = lockstep. Games before that ignore it)
    "netcode": {"default": "lockstep", "choices": ["lockstep", "lockstep_vclock", "rollback"], "live": True,
                "help": "online matches' netcode for new sessions (the host's game latches it per match): lockstep "
                        "= today's; lockstep_vclock = lockstep with a fixed 1/60 s clock per world update (both "
                        "games step alike); rollback = the rollback netcode. Games without one play lockstep"},
    "online_news": {"default": "", "text": True, "max_len": NEWS_MAX, "live": True,
                    "help": "the NEWS panel on the game's ONLINE screen (after the players-online count): plain "
                            "text, new lines kept; characters the game's font lacks are replaced or left out. "
                            "Empty = the built-in welcome. Games show it the next time they open ONLINE"},
    # (2.2.1: reports and suggestions only from a signed-in account with a Discord name; older clients are told to
    # update. Answers and changes stay keyed by the receipt. Off = as 2.2.0: anyone, any contact)
    "reports_need_account": {"default": "on", "choices": ["on", "off"], "live": True,
                             "help": "reports and suggestions: on = only signed in to an online account, with a "
                                     "Discord name (2.2.1 forms; older clients are told to update); off = anyone, "
                                     "as 2.2.0. Answers to questions and changes work either way. At once"},
    "cc_unmarked": {"default": "refuse", "choices": ["refuse", "log"], "live": True,
                    "help": "Community Creations uploads from builds that don't check for mods (no 'checked' "
                            "mark): refuse = refused (once latest_release is 2.1.5 or newer - before, only "
                            "logged), log = allowed and logged"},
    "latest_release": {"default": "", "text": True, "max_len": 16, "live": True,
                       "help": "the newest published release (e.g. 2.1.5): unmarked uploads are refused only "
                               "once it is 2.1.5 or newer (the release with the check). Empty = not yet"},
    "discovery_quota_mb": {"default": 1024, "min": 50, "max": 102400, "live": True,
                           "help": "Discovery: the most one account's mods may take in all (MB); an upload or an "
                                   "update over it is refused"},
    "discovery_warn_mb": {"default": 50, "min": 5, "max": 1024, "live": True,
                          "help": "Discovery: mods larger than this (MB) are stored, with a warning in the answer "
                                  "(big entrance movies: an .mp4 is ~10x smaller than a Bink one)"},
    "max_connections": {"default": 100, "min": 10, "max": 1000, "live": False,
                        "help": "game requests at once (more wait up to 10 s, then 503)"},
}
SETTINGS = {k: v["default"] for k, v in SETTINGS_SPEC.items()}
MODFREE_SINCE = (2, 1, 5)  # (the release whose game marks Community Creations uploads checked for mods)
REPORTS_ACCOUNT_SINCE = (2, 2, 1)  # (the release whose report forms send the account and a Discord name)
# (shown as it is by 2.2.0 and older launchers / Mod Makers / apps: it has to say everything on its own)
REPORTS_OLD_CLIENT = "Reports need 2.2.1: update, sign in to an online account and add your Discord name."


def older_release(text, want):
    """True only for a plain release number below want ('2.2.0' < (2, 2, 1)); dev builds and odd strings: False."""
    m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)(?:\.\d+)?", (text or "").strip())
    return bool(m) and tuple(int(x) for x in m.groups()) < want


def release_at_least(text, want):
    """'2.1.5' (or 'v2.1.5.1') >= want, a tuple; False when empty or not a version."""
    parts = re.findall(r"\d+", text or "")
    return bool(parts) and tuple(int(p) for p in parts[:len(want)]) >= want
# Where a player is (the game's presence poll, 2.1.7 on: &place=) - the dashboard's names for the tokens.
PLACE_NAMES = {
    "match_online": "Online match", "match_offline": "Offline match",
    "lobby_host": "Lobby (host)", "lobby_join": "Lobby (joined)",
    "matchmaking": "Matchmaking", "online_menu": "Online menu",
    "cc": "Community Creations", "cc_download": "CC: downloading", "cc_upload": "CC: uploading",
    "leaderboards": "Leaderboards",
    "menus": "Offline menus", "match_setup": "Match setup", "universe": "WWE Universe", "rtwm": "Road to WrestleMania",
    "shop": "Shop", "my_wwe": "My WWE", "practice": "Practice arena", "options": "Options",
    "create_superstar": "Create a Superstar", "paint_tool": "Paint Tool", "threads": "Superstar Threads",
    "create_story": "Story Designer", "create_finisher": "Create a Finisher", "create_entrance": "Create an Entrance",
    "highlight_reel": "Highlight Reel", "create_moveset": "Create a Move-Set", "create_menu": "Create modes",
}

LIVE_LIFE = 60      # seconds a session host's report stays (it reports every 20 s)
RESTART_CODE = 75
# A storage node stopped from the dashboard: its exit code (the supervisor ends too, not a restart) and the
# marker in its data folder that keeps it stopped (it refuses to start; the watchdog, pc_watchdog.ps1, leaves
# it) until the dashboard's Start (the core's wanted state, which the watchdog asks for).
STOP_CODE = 76
NODE_STOPPED = "node.stopped"
# the dashboard's own pages (admin_auth.py): never framed, cached or sent on as a referrer
ADMIN_HEADERS = {"X-Frame-Options": "DENY", "Cache-Control": "no-store", "Referrer-Policy": "no-referrer",
                 "X-Content-Type-Options": "nosniff"}   # the exit code that asks the supervisor (--supervise) to start the server again


def load_settings(path):
    try:
        with open(path, encoding="utf-8") as f:
            stored = json.load(f)
    except (OSError, ValueError):
        return
    for k, v in stored.items():
        ok, value = check_setting(k, v)
        if ok:
            SETTINGS[k] = value


def check_setting(k, v):
    spec = SETTINGS_SPEC.get(k)
    if not spec:
        return False, None
    if isinstance(spec["default"], bool):
        return (True, v) if isinstance(v, bool) else (False, None)
    if "choices" in spec:
        return (True, v) if v in spec["choices"] else (False, None)
    if spec.get("text"):
        return (True, news_clean(v)[:spec["max_len"]]) if isinstance(v, str) else (False, None)
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return False, None
    v = int(v)
    return (spec["min"] <= v <= spec["max"]), v


def now():
    return time.time()


# -- Accounts ------------------------------------------------------------------

class Accounts:
    """Players' accounts and their sessions (bearer tokens), in the store's
    database. Passwords are kept as PBKDF2 hashes; tokens as SHA-256."""

    def __init__(self, store):
        self.store = store
        self.db = store.db
        self.lock = store.lock
        with self.lock:
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS accounts (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE COLLATE NOCASE,
                    salt TEXT, pwhash TEXT, profileid INTEGER, created REAL, last_seen REAL,
                    banned INTEGER DEFAULT 0, ip TEXT);
                CREATE TABLE IF NOT EXISTS sessions (
                    token TEXT PRIMARY KEY, account INTEGER, created REAL, last_used REAL, ip TEXT);
                CREATE INDEX IF NOT EXISTS sessions_account ON sessions (account);
                CREATE TABLE IF NOT EXISTS friends (
                    account INTEGER, friend INTEGER, created REAL, PRIMARY KEY (account, friend));
                CREATE INDEX IF NOT EXISTS friends_friend ON friends (friend);
            """)
            self.db.commit()
        self.cache = {}  # token hash -> (account row, cached at)
        self.names = None  # (the name filter - the service's)

    @staticmethod
    def hash_password(password, salt):
        return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt), PBKDF2_ROUNDS).hex()

    @staticmethod
    def xuid(account):
        return XUID_BASE + account["id"]

    def _row(self, row):
        if not row:
            return None
        keys = ("id", "name", "profileid", "created", "last_seen", "banned", "ip")
        return dict(zip(keys, row))

    def get(self, account_id):
        with self.lock:
            return self._row(self.db.execute(
                "SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts WHERE id = ?",
                (account_id,)).fetchone())

    def by_profile(self, profileid):
        with self.lock:
            return self._row(self.db.execute(
                "SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts WHERE profileid = ?",
                (profileid,)).fetchone())

    def register(self, name, password, ip):
        name = name.strip()
        if not NAME_RE.match(name):
            return None, "Names are 3-15 letters, digits, spaces, dots, dashes or underscores."
        if self.names and self.names.check(name):
            gs.log("accounts: sign-up as %r from %s refused: the name filter" % (name, ip))
            return None, "That account name isn't allowed - pick another."
        if len(password) < 6:
            return None, "Passwords need at least 6 characters."
        salt = secrets.token_hex(16)
        pwhash = self.hash_password(password, salt)
        with self.lock:
            if self.db.execute("SELECT 1 FROM accounts WHERE name = ?", (name,)).fetchone():
                return None, "That name is taken."
            cur = self.db.execute(
                "INSERT INTO accounts (name, salt, pwhash, created, last_seen, ip) VALUES (?, ?, ?, ?, ?, ?)",
                (name, salt, pwhash, now(), now(), ip))
            account_id = cur.lastrowid
            profileid = self.store.profile("account:%d" % account_id, name)
            self.db.execute("UPDATE accounts SET profileid = ? WHERE id = ?", (profileid, account_id))
            self.db.commit()
        gs.log("accounts: %s registered (account %d) from %s" % (name, account_id, ip))
        return self.get(account_id), None

    def check(self, name, password):
        with self.lock:
            row = self.db.execute("SELECT id, salt, pwhash, banned FROM accounts WHERE name = ? COLLATE NOCASE",
                                  (name.strip(),)).fetchone()
        if not row or not hmac.compare_digest(self.hash_password(password, row[1]), row[2]):
            return None, "Wrong name or password."
        if row[3]:
            return None, "This account is banned."
        return self.get(row[0]), None

    def new_token(self, account, ip):
        token = secrets.token_urlsafe(32)
        with self.lock:
            self.db.execute("INSERT INTO sessions VALUES (?, ?, ?, ?, ?)",
                            (hashlib.sha256(token.encode()).hexdigest(), account["id"], now(), now(), ip))
            self.db.commit()
        return token

    def from_token(self, token):
        """The account behind a bearer token (not banned), or None."""
        if not token:
            return None
        key = hashlib.sha256(token.encode()).hexdigest()
        hit = self.cache.get(key)
        if hit and now() - hit[1] < 60:
            return hit[0]
        with self.lock:
            row = self.db.execute("SELECT account FROM sessions WHERE token = ?", (key,)).fetchone()
            if not row:
                return None
            account = self.get(row[0])
            if not account or account["banned"]:
                return None
            self.db.execute("UPDATE sessions SET last_used = ? WHERE token = ?", (now(), key))
            self.db.execute("UPDATE accounts SET last_seen = ? WHERE id = ?", (now(), account["id"]))
            self.db.commit()
        self.cache[key] = (account, now())
        return account

    def logout(self, token):
        with self.lock:
            self.db.execute("DELETE FROM sessions WHERE token = ?", (hashlib.sha256(token.encode()).hexdigest(),))
            self.db.commit()
        self.cache.clear()

    def set_banned(self, account_id, banned):
        with self.lock:
            self.db.execute("UPDATE accounts SET banned = ? WHERE id = ?", (1 if banned else 0, account_id))
            if banned:
                self.db.execute("DELETE FROM sessions WHERE account = ?", (account_id,))
            self.db.commit()
        self.cache.clear()

    def reset_password(self, account_id):
        password = secrets.token_urlsafe(9)
        salt = secrets.token_hex(16)
        with self.lock:
            self.db.execute("UPDATE accounts SET salt = ?, pwhash = ? WHERE id = ?",
                            (salt, self.hash_password(password, salt), account_id))
            self.db.execute("DELETE FROM sessions WHERE account = ?", (account_id,))
            self.db.commit()
        self.cache.clear()
        return password

    def delete(self, account_id):
        account = self.get(account_id)
        if not account:
            return 0
        removed = 0
        for table in list(self.store.records):
            for rec in [r for r in self.store.all(table) if r["ownerid"] == account["profileid"]]:
                self.store.delete(table, rec["recordid"])
                removed += 1
        with self.lock:
            self.db.execute("DELETE FROM sessions WHERE account = ?", (account_id,))
            self.db.execute("DELETE FROM friends WHERE account = ? OR friend = ?", (account_id, account_id))
            self.db.execute("DELETE FROM lb_rows WHERE account = ?", (account_id,))  # (its leaderboard rows)
            self.db.execute("DELETE FROM accounts WHERE id = ?", (account_id,))
            self.db.commit()
        self.cache.clear()
        return removed

    # friends: each player's own list (adding someone needs no answer from them;
    # they see who added them, to add them back)

    def by_name(self, name):
        with self.lock:
            return self._row(self.db.execute(
                "SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts WHERE name = ?",
                (name,)).fetchone())

    def friends(self, account_id):
        """(the accounts this one added, the accounts that added this one)"""
        cols = "a.id, a.name, a.profileid, a.created, a.last_seen, a.banned, a.ip"
        with self.lock:
            mine = self.db.execute("SELECT %s FROM friends f JOIN accounts a ON a.id = f.friend "
                                   "WHERE f.account = ? ORDER BY a.name COLLATE NOCASE" % cols, (account_id,)).fetchall()
            theirs = self.db.execute("SELECT %s FROM friends f JOIN accounts a ON a.id = f.account "
                                     "WHERE f.friend = ? ORDER BY a.name COLLATE NOCASE" % cols,
                                     (account_id,)).fetchall()
        return [self._row(r) for r in mine], [self._row(r) for r in theirs]

    def add_friend(self, account_id, friend_id):
        with self.lock:
            if self.db.execute("SELECT COUNT(*) FROM friends WHERE account = ?", (account_id,)).fetchone()[0] >= MAX_FRIENDS:
                return False
            self.db.execute("INSERT OR IGNORE INTO friends VALUES (?, ?, ?)", (account_id, friend_id, now()))
            self.db.commit()
        return True

    def remove_friend(self, account_id, friend_id):
        with self.lock:
            self.db.execute("DELETE FROM friends WHERE account = ? AND friend = ?", (account_id, friend_id))
            self.db.commit()

    def all(self):
        with self.lock:
            rows = self.db.execute("SELECT id, name, profileid, created, last_seen, banned, ip FROM accounts "
                                   "ORDER BY id").fetchall()
        return [self._row(r) for r in rows]


# -- Statistics ----------------------------------------------------------------

def memory_mb():
    """This process's resident memory (MB), or '?'."""
    try:
        if sys.platform == "win32":
            import ctypes
            from ctypes import wintypes

            class PMC(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                    (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                   "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                   "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
            c = PMC()
            c.cb = ctypes.sizeof(c)
            kernel32 = ctypes.WinDLL("kernel32")
            kernel32.GetCurrentProcess.restype = wintypes.HANDLE
            psapi = ctypes.WinDLL("psapi")
            psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
            if psapi.GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(c), c.cb):
                return "%d" % (c.WorkingSetSize >> 20)
        else:
            with open("/proc/self/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return "%d" % (int(line.split()[1]) >> 10)
    except Exception:
        pass
    return "?"


class Watchdog(threading.Thread):
    """The event loop stalling (a blocking call, a lock, a long query): when it
    hasn't run a tick for over 5 s, its thread's stack goes in the log; then how
    long the stall lasted."""

    def __init__(self, loop, limit=5.0):
        super().__init__(name="loop watchdog", daemon=True)
        self.loop = loop
        self.limit = limit
        self.beat = time.monotonic()
        self.loop_thread = threading.get_ident()

    def tick(self):
        self.beat = time.monotonic()

    def run(self):
        stalled = 0.0
        while True:
            time.sleep(1.0)
            try:
                self.loop.call_soon_threadsafe(self.tick)
            except RuntimeError:  # (the loop closed)
                return
            late = time.monotonic() - self.beat
            if late > self.limit and not stalled:
                stalled = late
                frame = sys._current_frames().get(self.loop_thread)
                gs.log("watchdog: the event loop hasn't run for %.0f s (%s); it is at:\n%s" % (
                    late, time.strftime("%Y-%m-%d %H:%M:%S"), "".join(traceback.format_stack(frame)) if frame else "?"))
            elif stalled and late < 2.0:
                gs.log("watchdog: the event loop runs again (stalled about %.0f s)" % stalled)
                stalled = 0.0
            elif stalled:
                stalled = max(stalled, late)


class Stats:
    """Per-minute counters of the game's traffic, kept in the database (the
    dashboard's history), plus the live gauges."""

    RELAY_COLUMNS = ("games", "sessions", "relayed")
    COLUMNS = ("requests", "bytes_in", "bytes_out", "peak", "players", "uploads", "downloads") + RELAY_COLUMNS

    def __init__(self, store):
        self.db = store.db
        self.lock = store.lock
        with self.lock:
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS stats (
                    t INTEGER PRIMARY KEY, requests INTEGER, bytes_in INTEGER, bytes_out INTEGER,
                    peak INTEGER, players INTEGER, uploads INTEGER, downloads INTEGER,
                    logins INTEGER, registrations INTEGER, refused INTEGER);
                CREATE TABLE IF NOT EXISTS player_days (day TEXT, account INTEGER, PRIMARY KEY (day, account));
                CREATE TABLE IF NOT EXISTS netreports (
                    day TEXT, version TEXT, path TEXT, nat TEXT, step INTEGER, rtt INTEGER, count INTEGER,
                    PRIMARY KEY (day, version, path, nat, step, rtt));
                -- (the dashboard's ping viewer and match history: per account, kept NET_KEEP_DAYS)
                CREATE TABLE IF NOT EXISTS net_events (
                    account INTEGER, at REAL, path TEXT, nat TEXT, rtt INTEGER, network TEXT, version TEXT);
                CREATE INDEX IF NOT EXISTS net_events_account ON net_events (account, at);
                CREATE TABLE IF NOT EXISTS match_rows (
                    account INTEGER, at REAL, view INTEGER, result TEXT, rated INTEGER);
                CREATE INDEX IF NOT EXISTS match_rows_account ON match_rows (account, at);
            """)
            # (online play through the relay, added later: games open at once, games
            # that came on, packets passed on)
            have = {r[1] for r in self.db.execute("PRAGMA table_info(stats)")}
            for column in self.RELAY_COLUMNS:
                if column not in have:
                    self.db.execute("ALTER TABLE stats ADD COLUMN %s INTEGER DEFAULT 0" % column)
            self.db.commit()
        self.active = 0
        self.peak = 0
        self.max_peak = 0
        self.minute = self._zero()
        self.recent = {}      # account id -> last presence poll (?here=1): its game runs (old clients: from start)
        self.online_mode = {}  # account id -> last poll marked &mode=online (2.1.4 on: only in online mode)
        self.places = {}  # account id -> (place, kind) of its last poll (2.1.7 on: &place=, &kind=; the dashboard's)
        self.days_counted = set()  # (day, account id) already in player_days
        self.total_in = 0
        self.total_out = 0
        self.started = now()
        self.games = 0        # relay games open now

    def relay_games(self, n, came_on=False):
        """The relay's games open now (and one more came on)."""
        self.games = n
        self.minute["games"] = max(self.minute["games"], n)
        if came_on:
            self.minute["sessions"] += 1

    @staticmethod
    def _zero():
        return collections.Counter()

    def begin(self):
        self.active += 1
        self.peak = max(self.peak, self.active)
        self.max_peak = max(self.max_peak, self.active)

    def end(self, bytes_in, bytes_out):
        self.active -= 1
        self.minute["requests"] += 1
        self.minute["bytes_in"] += bytes_in
        self.minute["bytes_out"] += bytes_out
        self.total_in += bytes_in
        self.total_out += bytes_out

    def count(self, what, n=1):
        self.minute[what] += n

    def seen(self, account_id, online_mode=False, place="", kind=""):
        """A game's presence poll: its game runs - in online mode when it says so (2.1.4 on), and where in the game
        it is (2.1.7 on: a token, PLACE_NAMES; "" from an older game)."""
        self.recent[account_id] = now()
        if online_mode:
            self.online_mode[account_id] = now()
        if place:
            self.places[account_id] = (place, kind)
        else:
            self.places.pop(account_id, None)
        self.day(account_id)


    def day(self, account_id):
        """The account played today (the dashboard's daily players) - any request of its game."""
        key = (time.strftime("%Y-%m-%d"), account_id)
        if key in self.days_counted:
            return
        if len(self.days_counted) > 100000:
            self.days_counted.clear()
        self.days_counted.add(key)
        with self.lock:
            self.db.execute("INSERT OR IGNORE INTO player_days VALUES (?, ?)", key)
            self.db.commit()

    def running_now(self, window=None):
        """Games running and signed in (presence polls; old clients poll from start)."""
        cutoff = now() - (window or SETTINGS["presence_window"])
        return sum(1 for t in list(self.recent.values()) if t >= cutoff)

    def playing_now(self, window=None):
        """Accounts whose game asked something lately (a running game, signed in,
        asks /api/friends?here=1 every 20 s - the launchers don't count)."""
        cutoff = now() - (window or SETTINGS["presence_window"])
        return sum(1 for t in list(self.recent.values()) if t >= cutoff)

    def players_online(self, window=300):
        cutoff = now() - window
        for k in [k for k, t in self.recent.items() if t < cutoff]:
            del self.recent[k]
        return len(self.recent)

    def flush(self):
        """The minute just ended, into the history."""
        m, self.minute = self.minute, self._zero()
        t = int(now() // 60 * 60) - 60
        row = (t, m["requests"], m["bytes_in"], m["bytes_out"], self.peak, self.players_online(),
               m["uploads"], m["downloads"], m["logins"], m["registrations"], m["refused"],
               max(m["games"], self.games), m["sessions"], m["relayed"])
        self.peak = self.active
        with self.lock:
            self.db.execute("INSERT OR REPLACE INTO stats (t, requests, bytes_in, bytes_out, peak, players, uploads, "
                            "downloads, logins, registrations, refused, games, sessions, relayed) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", row)
            self.db.commit()

    def history(self, span, buckets):
        """[(t, requests, bytes_in, bytes_out, peak, players, uploads, downloads, games, sessions,
        relayed)] over the last `span` seconds, in `buckets` equal steps (sums; peak, players and
        games: max)."""
        start = int(now() - span)
        step = max(60, span // buckets)
        with self.lock:
            rows = self.db.execute(
                "SELECT (t - ?) / ? AS b, SUM(requests), SUM(bytes_in), SUM(bytes_out), MAX(peak), "
                "MAX(players), SUM(uploads), SUM(downloads), MAX(games), SUM(sessions), SUM(relayed) "
                "FROM stats WHERE t >= ? GROUP BY b ORDER BY b",
                (start, step, start)).fetchall()
        got = {r[0]: r[1:] for r in rows}
        n = len(self.COLUMNS)
        out = []
        for b in range(buckets):
            r = got.get(b)
            out.append([start + b * step] + ([0] * n if r is None else [v or 0 for v in r]))
        return step, out

    def totals(self, since=0):
        """Sums over the history since `since` (all of it by default)."""
        with self.lock:
            r = self.db.execute("SELECT SUM(uploads), SUM(downloads), SUM(sessions), SUM(relayed), "
                                "MAX(games), MAX(players) FROM stats WHERE t >= ?", (since,)).fetchone()
        return {k: v or 0 for k, v in zip(("uploads", "downloads", "sessions", "relayed", "max_games",
                                             "max_players"), r)}

    def unique_players(self, days):
        with self.lock:
            return self.db.execute("SELECT COUNT(DISTINCT account) FROM player_days WHERE day >= ?",
                                   (time.strftime("%Y-%m-%d", time.localtime(now() - days * 86400)),)
                                   ).fetchone()[0]


# -- The service ---------------------------------------------------------------

class Service:
    def __init__(self, args):
        self.local_admin = bool(getattr(args, "local_admin", False))
        self.base = "/" + args.base.strip("/") if args.base.strip("/") else ""
        blobs = None
        if getattr(args, "role", "all") == "core":  # (the blob bytes on the storage node: the user's PC)
            blobs = gs.RemoteBlobs(args.storage_url, read_key(args.storage_key))
        self.store = gs.Store(args.data, max_file=args.max_file_mb << 20, quota=args.quota_mb << 20, blobs=blobs)
        # (the storage node's panel: its address and key; the wanted state - running / stopped - the PC's
        # watchdog asks for; the last time it asked)
        self.node_url = args.storage_url.rstrip("/") if blobs is not None else ""
        self.node_key = read_key(args.storage_key) if blobs is not None else ""
        self.node_state_path = os.path.join(args.data, "storage_node.json")
        try:
            self.node_state = json.load(open(self.node_state_path, encoding="utf-8"))
        except (OSError, ValueError):
            self.node_state = {}
        self.node_state.setdefault("wanted", "running")
        self.node_polled = 0.0
        # (problem reports: where the blobs are - the storage node's reports folder, readable on the PC)
        self.reports = (reports.RemoteReports(args.storage_url, read_key(args.storage_key)) if blobs is not None
                        else reports.LocalReports(args.data))
        if isinstance(self.reports, reports.LocalReports):
            self.reports.mod_quota = int(args.report_mod_quota_gb * (1 << 30))
        self.report_tmp = os.path.join(args.data, "tmp", "reports")  # (mods sent with a report, on their way)
        self.report_times = collections.defaultdict(collections.deque)  # ip / account -> report times (memory only)
        self.check_times = collections.defaultdict(collections.deque)   # ip -> mod checks (memory only)
        self.sake = gs.Sake(self.store)
        discovery.init(self.store)
        self.mod_upload = asyncio.Lock()  # (one .svrmod stored at a time: they are up to 250 MB, held in memory)
        self.accounts = Accounts(self.store)
        # (the name filter: Community Creations names, Discovery mods, entrances, account names - namefilter.py)
        self.names = namefilter.NameFilter(args.data, gs.log)
        self.accounts.names = self.names
        self.signup = signup.Signup(self.store, self.accounts, gs.log,
                                    open_accounts=bool(getattr(args, "test_open_accounts", False)),
                                    exempt=(getattr(args, "admin_account", None) or "").strip(),
                                    checks=lambda: SETTINGS["account_checks"] == "on")
        self.stats = Stats(self.store)
        self.slots = asyncio.Semaphore(args.max_connections)
        self.max_connections = args.max_connections
        self.busy_said = 0.0
        edge = getattr(args, "relay_edge", None)
        self.netreport_last = {}  # account id -> its last match report (memory only)
        self.relay = relay.Relay(self, gs.log, getattr(args, "relay_udp_port", 0),
                                 edge=(edge.rsplit(":", 1)[0], int(edge.rsplit(":", 1)[1])) if edge else None,
                                 secret=relay.rl.read_secret(args.relay_secret) if edge else None,
                                 network=lambda: SETTINGS["match_network"])
        self.leaderboards = leaderboards.Leaderboards(self.store)
        # the ONLINE menu's NEWS: how many look for a match now, first; then the dashboard's
        # text (online_news) when it has one, else the stored record's. (" online now. " stays in the line: the
        # game rewrites its news panel only over a text that has it - online.cpp SetOnlineNews)
        self.sake.news_prefix = lambda: (lambda n: "%d player%s looking for a match online now. " % (
            n, "" if n == 1 else "s"))(self.looking_for_match())
        self.sake.news_text = lambda: news_fields(SETTINGS["online_news"])
        self.sake.refuse_unmarked = lambda: SETTINGS["cc_unmarked"] == "refuse" and release_at_least(
            SETTINGS["latest_release"], MODFREE_SINCE)
        self.sake.name_check = self.sake_names
        self.invites = collections.defaultdict(dict)  # account id -> {inviter id: invite} (in memory)
        self.max_connections = args.max_connections
        self.attempts = collections.defaultdict(collections.deque)  # ip -> login/register times
        self.data_dir = Path(args.data)
        self.settings_path = self.data_dir / "settings.json"
        self.log_path = getattr(args, "log", None)
        self.supervised = bool(os.environ.get("SVR2011_SUPERVISOR_PID"))
        self.live = {}   # session id -> the host's last report (api_live)
        self.dashboard_html = (HERE / "dashboard.html").read_text(encoding="utf-8")
        self.admin_auth = aa.AdminAuth(getattr(args, "admin_password_file", None) or os.path.join(args.data, "admin_password"),
                                       gs.log, account=getattr(args, "admin_account", None), accounts=self.accounts)
        self.max_media = {"music": args.max_music_mb << 20, "movie": args.max_movie_mb << 20}
        self.media_quota = args.media_quota_mb << 20

    # who is asking

    @staticmethod
    def client_ip(request):
        peer = request.remote or ""
        if peer in LOOPBACK and request.headers.get("X-Forwarded-For"):
            return request.headers["X-Forwarded-For"].split(",")[0].strip()
        return peer

    def account(self, request):
        auth = request.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return None
        account = self.accounts.from_token(auth[7:].strip())
        if account and not self.signup.online_ok(account):  # (no re-activation code yet)
            return None
        if account:  # (not presence: a game outside online mode fetches logos / Community Creations too)
            self.stats.day(account["id"])
        return account

    def is_admin(self, request):
        """The dashboard: Project Index admins (through the portal: its headers on
        a loopback connection). A loopback request without the portal's headers
        is NOT an admin any more - behind a public proxy on the same host (nginx
        on a VPS) every request comes from loopback - unless the service runs
        with --local-admin (a developer's own server)."""
        if request.headers.get("X-Portal-Public"):
            return False
        if request.remote not in LOOPBACK:
            return False
        if request.headers.get("X-Forwarded-For"):  # through the portal
            return request.headers.get("X-Portal-Role") == "admin"
        return self.local_admin

    def throttled(self, ip, limit=10, window=600):
        q = self.attempts[ip]
        while q and q[0] < now() - window:
            q.popleft()
        if len(q) >= limit:
            return True
        q.append(now())
        return False

    # metering and the connection limit (the game's half only)

    @web.middleware
    async def meter(self, request, handler):
        if not request.path.startswith(self.base + "/") or request.path.startswith(self.base + "/api/relay/"):
            return await handler(request)  # (the relay's streams stay open all session: not requests' slots)
        try:
            await asyncio.wait_for(self.slots.acquire(), 10)
        except asyncio.TimeoutError:
            self.stats.count("refused")
            if now() - self.busy_said > 60:  # (once a minute: a hang's cause shows in the log)
                self.busy_said = now()
                gs.log("busy: all %d request slots taken for 10 s (%d running) - refusing (503)" %
                       (self.max_connections, self.stats.active))
            return web.Response(status=503, text="busy")
        self.stats.begin()
        bytes_in = request.content_length or 0
        bytes_out = 0
        try:
            response = await handler(request)
            if isinstance(response, web.Response) and response.body is not None:
                try:
                    bytes_out = len(response.body)
                except TypeError:
                    bytes_out = response.content_length or 0
            elif isinstance(response, web.StreamResponse):
                bytes_out = response.content_length or 0
            return response
        finally:
            self.stats.end(bytes_in, bytes_out)
            self.slots.release()

    # accounts

    async def api_register(self, request):
        return await self._sign_in(request, register=True)

    async def api_login(self, request):
        return await self._sign_in(request, register=False)

    async def _sign_in(self, request, register):
        ip = self.client_ip(request)
        if self.throttled(ip):
            return web.json_response({"ok": False, "error": "Too many tries - wait a few minutes."}, status=429)
        try:
            body = await request.json()
            name, password = str(body.get("name", "")), str(body.get("password", ""))
            client = int(body.get("client", 0) or 0)  # (2: a 2.1.6+ launcher - it walks the account steps)
        except (ValueError, AttributeError, TypeError):
            return web.json_response({"ok": False, "error": "Bad request."}, status=400)
        if not register and self.signup.limits.hit(("login", name.strip().lower()), 10, 600):
            return web.json_response({"ok": False, "error": "Too many tries - wait a few minutes."}, status=429)
        loop = asyncio.get_running_loop()
        if register and client < 2:  # (a 2.1.5 launcher: name and password only)
            if not self.signup.relaxed():
                return web.json_response({"ok": False, "error": "New accounts need the 2.1.6 launcher now: "
                                                                "update to 2.1.6 and sign up there with the invite "
                                                                "code the server owner gives you."}, status=403)
            # (account_checks off: as before 2.1.6 - an account without a code: it needs a re-activation code once on)
            account, error = await loop.run_in_executor(None, self.accounts.register, name, password, ip)
        elif register and self.signup.relaxed() and not str(body.get("invite", "")).strip():
            # (account_checks off and no code typed: an account as before 2.1.6 - it needs a re-activation code once on)
            account, error = await loop.run_in_executor(None, self.accounts.register, name, password, ip)
        elif register:
            account, error = await loop.run_in_executor(None, self.signup.register, name, password,
                                                        str(body.get("invite", "")), ip)
        else:
            account, error = await loop.run_in_executor(None, self.accounts.check, name, password)
        if not account:
            return web.json_response({"ok": False, "error": error}, status=403)
        why = self.signup.blocked_why(account, old_client=client < 2)
        if why and client < 2:  # (an old launcher: it can't do the steps - it shows this, as it is)
            gs.log("accounts: %s refused on an old launcher from %s (%s)" % (account["name"], ip, why))
            return web.json_response({"ok": False, "error": why}, status=403)
        token = self.accounts.new_token(account, ip)
        self.stats.count("registrations" if register else "logins")
        gs.log("accounts: %s %s from %s" % (account["name"], "registered" if register else "signed in", ip))
        return web.json_response(dict(self.signup.account_json(account), ok=True, token=token, id=account["id"],
                                      xuid="%016X" % Accounts.xuid(account), why=why))

    # the launcher's account steps (signup.py): a token's account, blocked or not

    def raw_account(self, request):
        auth = request.headers.get("Authorization", "")
        return self.accounts.from_token(auth[7:].strip()) if auth.startswith("Bearer ") else None

    async def _account_step(self, request, step):
        account = self.raw_account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        try:
            body = await request.json() if request.method == "POST" else {}
        except ValueError:
            return web.json_response({"ok": False, "error": "Bad request."}, status=400)
        ip = self.client_ip(request)
        error = await asyncio.get_running_loop().run_in_executor(None, step, account, body, ip)
        account = self.accounts.get(account["id"])
        return web.json_response(dict(self.signup.account_json(account), ok=not error, error=error or "",
                                      why=self.signup.blocked_why(account, False)), status=200 if not error else 403)

    async def api_account(self, request):
        return await self._account_step(request, lambda a, b, ip: "")

    async def api_reactivate(self, request):
        return await self._account_step(request, lambda a, b, ip: self.signup.reactivate(a, str(b.get("code", "")), ip))

    async def api_password_change(self, request):
        token = request.headers.get("Authorization", "")[7:].strip()

        def step(a, b, ip):
            if self.signup.limits.hit(("login", a["name"].lower()), 10, 600):
                return "Too many tries - wait a few minutes."
            if not self.accounts.check(a["name"], str(b.get("password", "")))[0]:
                return "Your password isn't right."
            new = str(b.get("new_password", ""))
            if len(new) < signup.MIN_PASSWORD:
                return "Passwords need at least %d characters." % signup.MIN_PASSWORD
            self.signup.set_password(a["id"], new, keep_token=token)
            gs.log("accounts: %s changed its password (its other sessions ended)" % a["name"])
            return ""
        return await self._account_step(request, step)

    async def api_password_reset(self, request):
        try:
            body = await request.json()
        except ValueError:
            return web.json_response({"ok": False, "error": "Bad request."}, status=400)
        error = await asyncio.get_running_loop().run_in_executor(
            None, self.signup.reset, str(body.get("name", "")), str(body.get("code", "")),
            str(body.get("password", "")), self.client_ip(request))
        if error:
            return web.json_response({"ok": False, "error": error}, status=403)
        return web.json_response({"ok": True, "message": "Your password is set: sign in with it."})

    async def api_logout(self, request):
        auth = request.headers.get("Authorization", "")
        if auth.startswith("Bearer "):
            self.accounts.logout(auth[7:].strip())
        return web.json_response({"ok": True})

    async def api_session(self, request):
        """The game's login: its profile, a login ticket, its name and XUID."""
        account = self.account(request)
        if not account:
            raw = self.raw_account(request)
            why = raw and self.signup.blocked_why(raw, old_client=request.query.get("client") != "2")
            return web.json_response({"ok": False, "error": why or "Not signed in."}, status=403 if why else 401)
        ticket = self.store.new_ticket(account["profileid"])
        self.stats.count("logins")
        return web.json_response({"ok": True, "name": account["name"], "profileid": account["profileid"],
                                  "ticket": ticket, "xuid": "%016X" % Accounts.xuid(account)})

    # friends and who's online (the launchers' Friends list)

    def in_online_mode(self):
        """Accounts in online mode now: games that say so (&mode=online, 2.1.4 on - older ones are refused
        online anyway and don't count)."""
        t = now()
        return {a for a, at in list(self.stats.online_mode.items()) if t - at < SETTINGS["presence_window"]}

    def in_session(self):
        """Players in a session or match now: the hosts' live reports (2.1.4+ hosts) - their names."""
        t = now()
        names = set()
        for v in list(self.live.values()):
            if t - v["seen"] <= LIVE_LIFE:
                names |= {p["name"] for p in v["players"] if p.get("name")}
        return len(names)

    def looking_for_match(self):
        """Players looking for a match now (the in-game NEWS line): searching or on the ONLINE MATCH screens (the
        presence poll's place "matchmaking", 2.1.7 on), or hosting a lobby others can find (place "lobby_host"; any
        game's: the live reports of sessions still in the lobby). Not players in a match - a started session isn't
        offered to searches - nor ones who joined a lobby."""
        t = now()
        ids = {a for a, (place, _) in list(self.stats.places.items())
               if place in ("matchmaking", "lobby_host") and t - self.stats.recent.get(a, 0) < SETTINGS["presence_window"]}
        ids |= {v["host_id"] for v in list(self.live.values())
                if v.get("host_id") and v.get("state") == "lobby" and t - v["seen"] <= LIVE_LIFE}
        return len(ids)

    def presence(self, account_id):
        """playing: in an online match or its lobby (its game has the relay open);
        online: its game in online mode - the ONLINE menus or an online match (it
        asks /api/friends?here=1 every 20 s then; gone presence_window s after the
        last, at once when it says bye);
        else offline. (The launchers' own requests don't count.)"""
        if any(g.account["id"] == account_id for g in self.relay.games.values()):
            return "playing"
        if account_id in self.in_online_mode():
            return "online"
        # ingame: its game runs, signed in, outside online mode (2.1.7 on: ?here=1 from the offline menus too) -
        # it can be invited (the game walks there); older games show it as offline but let it be invited
        if self.stats.recent.get(account_id, 0) >= now() - SETTINGS["presence_window"]:
            return "ingame"
        return "offline"

    def friends_json(self, account):
        mine, theirs = self.accounts.friends(account["id"])
        waiting = self.invites.get(account["id"], {})
        for k in [k for k, v in waiting.items() if now() - v["t"] > SETTINGS["invite_life"]]:
            del waiting[k]
        added = {a["id"] for a in mine}
        friends = [{"name": a["name"], "status": self.presence(a["id"]), "last_seen": a["last_seen"],
                    "mutual": any(t["id"] == a["id"] for t in theirs)} for a in mine if not a["banned"]]
        friends.sort(key=lambda f: ("playing", "online", "ingame", "offline").index(f["status"]))  # (online first)
        return {
            "ok": True,
            "config": {"mods_online": bool(SETTINGS["mods_online"]), "match_network": SETTINGS["match_network"],
                       "netcode": SETTINGS["netcode"],
                       # (the NEWS panel's text as the game's Sake fetch gets it: the game shows it at the
                       # next visit to ONLINE, without a restart)
                       "news": self.sake.news_panel()},
            "online_now": len(self.in_online_mode()),
            "friends": friends,
            "added_you": [a["name"] for a in theirs if a["id"] not in added and not a["banned"]],
            "invites": [{"from": v["from"], "xuid": v["xuid"], "session": v["session"], "kind": v["kind"],
                         "age": int(now() - v["t"])} for v in sorted(waiting.values(), key=lambda v: -v["t"])],
        }

    def token_account(self, request):
        """The signed-in account, without counting it as playing (the launchers) - one that may play online."""
        account = self.raw_account(request)
        return account if account and self.signup.online_ok(account) else None

    async def api_friends(self, request):
        account = self.token_account(request)
        if not account:
            raw = self.raw_account(request)  # (blocked: say why - an old launcher shows the error)
            why = raw and self.signup.blocked_why(raw, old_client=request.query.get("client") != "2")
            return web.json_response({"ok": False, "error": why or "Not signed in."}, status=403 if why else 401)
        if request.query.get("bye"):  # (the game closing / leaving online mode: offline now)
            self.stats.recent.pop(account["id"], None)
            self.stats.online_mode.pop(account["id"], None)
            self.stats.places.pop(account["id"], None)
            return web.json_response({"ok": True})
        if request.query.get("here"):  # (the game asking: it's running, signed in; 2.1.4 on: &mode=online)
            place = request.query.get("place", "")
            kind = request.query.get("kind", "")
            self.stats.seen(account["id"], request.query.get("mode") == "online",
                            place if place in PLACE_NAMES else "", kind if kind in ("ranked", "player") else "")
        if request.method == "POST":
            try:
                body = await request.json()
            except ValueError:
                body = {}
            name = str(body.get("add") or body.get("remove") or "").strip()
            other = self.accounts.by_name(name) if name else None
            if not other or other["banned"]:
                return web.json_response({"ok": False, "error": "There's no player called %s." % name[:20]},
                                         status=404)
            if other["id"] == account["id"]:
                return web.json_response({"ok": False, "error": "That's you."}, status=400)
            if body.get("add"):
                if not self.accounts.add_friend(account["id"], other["id"]):
                    return web.json_response({"ok": False, "error": "Your list is full (%d)." % MAX_FRIENDS},
                                             status=400)
                gs.log("friends: %s added %s" % (account["name"], other["name"]))
            else:
                self.accounts.remove_friend(account["id"], other["id"])
        return web.json_response(self.friends_json(account))

    async def api_live(self, request):
        """A session host's report (p2p.cpp, every 20 s while it hosts): {"session": hex id,
        "kind", "match_kind" ("ranked" / "player", 2.1.6+; "" = not known: an older game), "rule", "arena" ("arena N"),
        "arena_mod" (a custom arena's name, 2.1.6+), "state": "lobby" | "match",
        "players": [{"name", "path"}],
        "slots", "started"}. Kept LIVE_LIFE s; the dashboard's live view."""
        account = self.token_account(request)
        if not account:
            return web.json_response({"ok": False}, status=401)
        try:
            body = await request.json()
        except ValueError:
            return web.json_response({"ok": False}, status=400)
        sid = str(body.get("session") or "")[:20]
        if not re.fullmatch(r"[0-9A-Fa-f]{1,20}", sid):
            return web.json_response({"ok": False}, status=400)
        if body.get("ended"):
            self.live.pop(sid, None)
            return web.json_response({"ok": True})
        if len(self.live) >= 2000 and sid not in self.live:
            return web.json_response({"ok": False}, status=503)
        # (2.1.6 on: each peer's relay id too - a peer the host has no name for, "?" - an older game's, or one whose
        # first ping hasn't come - is named from the relay's account for it on the dashboard)
        players = [{"name": str(p.get("name") or "")[:32], "path": str(p.get("path") or "")[:8],
                    "id": str(p.get("id") or "")[:12].lower() if re.fullmatch(r"[0-9A-Fa-f]{12}", str(p.get("id") or "")) else ""}
                   for p in (body.get("players") or [])[:16] if isinstance(p, dict)]
        old = self.live.get(sid, {})
        state = str(body.get("state") or "lobby")[:12]
        self.live[sid] = {
            "session": sid, "host": account["name"], "host_id": account["id"], "kind": str(body.get("kind") or "")[:40],
            "rule": str(body.get("rule") or "")[:40],
            "rule_name": match_names.rule_name(str(body.get("rule") or "")) or "", "arena": str(body.get("arena") or "")[:40],
            "arena_name": match_names.arena_name(body.get("arena"), str(body.get("arena_mod") or "")[:60]),
            "arena_mod": bool(body.get("arena_mod")), "state": state,
            "match_kind": str(body.get("match_kind") or "") if body.get("match_kind") in ("ranked", "player") else "",
            "players": players, "slots": int(body.get("slots") or 0), "version": str(body.get("version") or "")[:16],
            "network": str(body.get("network") or "")[:8],
            "since": old.get("since", now()), "state_since": old.get("state_since", now())
            if old.get("state") == state else now(), "seen": now()}
        return web.json_response({"ok": True})

    async def api_invite(self, request):
        """An invite to the match session the player is in, for a friend's game
        (it accepts through the game's own Xbox LIVE invite path): {"to": name,
        "session": base64 XSESSION_INFO, "kind": text}; {"decline": name} drops
        one received."""
        account = self.token_account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        try:
            body = await request.json()
        except ValueError:
            body = {}
        if body.get("decline"):
            other = self.accounts.by_name(str(body["decline"]))
            if other:
                self.invites.get(account["id"], {}).pop(other["id"], None)
            return web.json_response({"ok": True})
        other = self.accounts.by_name(str(body.get("to") or "").strip())
        session = str(body.get("session") or "")
        try:
            info = base64.b64decode(session, validate=True)
        except ValueError:
            info = b""
        if not other or other["banned"]:
            return web.json_response({"ok": False, "error": "There's no such player."}, status=404)
        if len(info) != 60:
            return web.json_response({"ok": False, "error": "Not a session."}, status=400)
        mine, _ = self.accounts.friends(account["id"])
        if other["id"] not in {a["id"] for a in mine}:
            return web.json_response({"ok": False, "error": "Only friends can be invited."}, status=403)
        if self.throttled("invite:%d" % account["id"], limit=30, window=60):
            return web.json_response({"ok": False, "error": "Too many invites - wait a little."}, status=429)
        self.invites[other["id"]][account["id"]] = {"from": account["name"], "xuid": "%016X" % Accounts.xuid(account),
                                                    "session": session,
                                                    "kind": str(body.get("kind") or "")[:40], "t": now()}
        gs.log("friends: %s invited %s" % (account["name"], other["name"]))
        return web.json_response({"ok": True})

    # the game's leaderboards (leaderboards.py; the port's leaderboards.cpp)

    def lb_row(self, r, names):
        xuid, account, rating, cols, rank = r
        return {"xuid": "%016X" % xuid, "rank": rank, "rating": rating, "name": names.get(account, ""),
                "columns": json.loads(cols)}

    def lb_names(self):
        return {a["id"]: a["name"] for a in self.accounts.all()}

    # The games' per-match connection reports (p2p.cpp): how online matches
    # connected - only counts per day, nothing about who or from where.
    NET_PATHS = ("direct", "ipv6", "relay", "lan")
    NET_NATS = ("cone", "symmetric", "mapped", "open", "unknown")
    NET_RTT = (50, 100, 150, 250, 400, 100000)  # (upper bounds, ms)

    async def api_netreport(self, request):
        account = self.account(request)
        if not account:
            return web.json_response({"ok": False}, status=401)
        last = self.netreport_last.get(account["id"], 0)
        if now() - last < 20:  # (one a match: a few a minute at most)
            return web.json_response({"ok": False}, status=429)
        self.netreport_last[account["id"]] = now()
        try:
            body = await request.json()
            path, nat = str(body.get("path")), str(body.get("nat"))
            step = max(-1000, min(1000, int(body.get("step") or 0)))
            rtt = max(0, int(body.get("rtt") or 0))
            version = re.sub(r"[^0-9A-Za-z.\-]", "", str(body.get("version") or ""))[:24] or "?"
            network = str(body.get("network") or "")  # (2.1.4 on: the match network it played on)
            # (2.2.0 on: "ipv6": true beside path "direct". A server before that doesn't know the field and counts a
            # direct match; a path it doesn't know it would refuse, and the report would be lost)
            if path == "direct" and body.get("ipv6") is True:
                path = "ipv6"
        except (ValueError, TypeError, AttributeError):
            return web.json_response({"ok": False}, status=400)
        if path not in self.NET_PATHS or nat not in self.NET_NATS:
            return web.json_response({"ok": False}, status=400)
        step = step if abs(step) <= 16 else 99  # (a small steady step: port prediction can follow it)
        gs.log("netreport: %s played a match, path %s, match network %s (server %s)" % (
            account["name"], path, network if network in ("auto", "relay", "p2p") else "not sent (a game before 2.1.4)",
            SETTINGS["match_network"]))
        raw_rtt = min(rtt, 100000)
        rtt = next(b for b in self.NET_RTT if rtt <= b)
        with self.store.lock:
            self.store.db.execute(
                "INSERT INTO netreports VALUES (?, ?, ?, ?, ?, ?, 1) ON CONFLICT (day, version, path, nat, step, rtt) "
                "DO UPDATE SET count = count + 1", (time.strftime("%Y-%m-%d"), version, path, nat, step, rtt))
            # (the dashboard's ping viewer / a player's matches: this account's report)
            self.store.db.execute("INSERT INTO net_events VALUES (?, ?, ?, ?, ?, ?, ?)", (
                account["id"], now(), path, nat, raw_rtt, network if network in ("auto", "relay", "p2p") else "",
                version))
            self.store.db.commit()
        return web.json_response({"ok": True})

    def netreport_summary(self, days=30):
        since = time.strftime("%Y-%m-%d", time.localtime(time.time() - days * 86400))
        out = {"matches": 0, "path": {}, "nat": {}, "rtt": {}, "steps": {}}
        with self.store.lock:
            rows = self.store.db.execute("SELECT path, nat, step, rtt, SUM(count) FROM netreports WHERE day >= ? "
                                         "GROUP BY path, nat, step, rtt", (since,)).fetchall()
        for path, nat, step, rtt, n in rows:
            out["matches"] += n
            out["path"][path] = out["path"].get(path, 0) + n
            out["nat"][nat] = out["nat"].get(nat, 0) + n
            out["rtt"][str(rtt)] = out["rtt"].get(str(rtt), 0) + n
            if nat == "symmetric":
                out["steps"][str(step)] = out["steps"].get(str(step), 0) + n
        return out

    async def api_stats(self, request):
        account = self.account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Not signed in."}, status=401)
        try:
            body = await request.json()
        except ValueError:
            return web.json_response({"ok": False, "error": "Not JSON."}, status=400)
        what = request.match_info["what"]
        lb = self.leaderboards
        loop = asyncio.get_running_loop()
        if what == "write":
            # (a game writes its own player's stats only)
            xuid = Accounts.xuid(account)
            if int(str(body.get("xuid") or "0"), 16) != xuid:
                return web.json_response({"ok": False, "error": "Only your own stats."}, status=403)
            done = await loop.run_in_executor(None, lb.write, xuid, account["id"], body.get("views") or [])
            gs.log("stats: %s wrote %d view(s): %s" % (account["name"], done, ", ".join(
                self.view_label(v.get("view")) for v in (body.get("views") or [])[:8] if isinstance(v, dict))))
            await loop.run_in_executor(None, self.record_match, account["id"], body.get("views") or [])
            return web.json_response({"ok": True, "views": done})
        names = await loop.run_in_executor(None, self.lb_names)
        if what == "read":
            xuids = [int(str(x), 16) for x in (body.get("xuids") or [])][:100]
            out = []
            for vid in [int(v) for v in (body.get("views") or [])][:64]:
                rows = await loop.run_in_executor(None, lb.rows_for, vid, xuids)
                out.append({"view": vid, "total": await loop.run_in_executor(None, lb.total, vid),
                            "rows": [self.lb_row(r, names) for r in rows]})
            return web.json_response({"ok": True, "views": out})
        if what == "page":
            vid, count = int(body.get("view") or 0), max(1, min(100, int(body.get("count") or 10)))
            mode, pivot = body.get("mode"), body.get("pivot") or 0
            if mode == "xuid":
                rows = await loop.run_in_executor(None, lb.page_around, vid, int(str(pivot), 16), count)
            elif mode == "rating":
                rows = await loop.run_in_executor(None, lb.page_by_rating, vid, int(pivot), count)
            else:
                rows = await loop.run_in_executor(None, lb.page_by_rank, vid, int(pivot) or 1, count)
            return web.json_response({"ok": True, "total": await loop.run_in_executor(None, lb.total, vid),
                                      "rows": [self.lb_row(r, names) for r in rows]})
        return web.json_response({"ok": False}, status=404)

    async def api_status(self, request):
        return web.json_response({"ok": True, "server": "svr2011-community", "version": 1,
                                  "invite_needed": not self.signup.relaxed()})  # (sign-up asks for a code)

    # the port's logo store

    async def api_logo(self, request):
        if not self.account(request):
            return web.Response(status=401)
        hash16 = request.match_info["hash"].upper()
        if not re.fullmatch(r"[0-9A-F]{16}", hash16):
            return web.Response(status=400)
        loop = asyncio.get_running_loop()
        if request.method == "POST":
            data = await request.read()
            ok = await loop.run_in_executor(None, self.store.put_logo, hash16, data)
            return web.Response(text="ok" if ok else "", headers={"Svr2011-Result": "0" if ok else "6"})
        data = await loop.run_in_executor(None, self.store.get_logo, hash16)
        return web.Response(body=data or b"", content_type="application/octet-stream",
                            headers={"Svr2011-Result": "0" if data else "4"})

    # Created Superstars' entrance songs and movies (gamespy.Store.set_entrance)

    async def api_media_post(self, request):
        account = self.account(request)
        if not account:
            return web.Response(status=401)
        kind = request.query.get("kind", "")
        if kind not in self.max_media:
            return web.json_response({"ok": False, "error": "kind is music or movie"}, status=400)
        if (request.content_length or 0) > self.max_media[kind]:
            return web.json_response({"ok": False, "error": "too large"}, status=413)
        data = await request.read()
        if not data or len(data) > self.max_media[kind]:
            return web.json_response({"ok": False, "error": "too large"}, status=413)
        pid = account["profileid"]
        loop = asyncio.get_running_loop()
        sent = hashlib.sha256(data).hexdigest()
        if self.store.stored_media(sent):  # (sent before: as it was stored)
            return web.json_response({"ok": True, "sha": self.store.stored_media(sent)})
        # (any size: songs made MP3 at the game's rate, movies cut at 4 minutes - media.py)
        out, ext, what = await loop.run_in_executor(None, media.process, kind, data)
        if out is None:
            gs.log("media: %s from %s refused: %s" % (kind, account["name"], what))
            return web.json_response({"ok": False, "error": what}, status=415)
        if self.store.media_bytes(pid) + len(out) > self.media_quota:
            gs.log("media: %s from %s refused: over the quota" % (kind, account["name"]))
            return web.json_response({"ok": False, "error": "over the quota"}, status=507)
        sha = await loop.run_in_executor(None, self.store.put_media, pid, kind, out)
        if sha != sent:
            self.store.set_media_alias(sent, sha, ext)
        gs.log("media: %s %d bytes from %s -> %s, %d bytes: %s" % (kind, len(data), account["name"], sha[:12],
                                                                   len(out), what))
        return web.json_response({"ok": True, "sha": sha, "processed": what})

    STREAM_FROM = 4 << 20  # (smaller: read whole, through the cache)

    @staticmethod
    def byte_range(request, size):
        """The request's "Range: bytes=N-" / "bytes=N-M" (one range) against a file of `size` bytes:
        (start, end inclusive), None (no Range, or one we don't take - the whole file), or "bad" (past the end:
        416). A broken download asks for the rest with it (the launcher's Discovery resume)."""
        m = re.fullmatch(r"\s*bytes\s*=\s*(\d+)\s*-\s*(\d*)\s*", request.headers.get("Range", ""))
        if not m:
            return None
        start = int(m.group(1))
        end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        if start >= size:
            return "bad"
        return None if end < start else (start, end)

    async def send_blob(self, request, sha, headers=None, content_type="application/octet-stream"):
        """A stored file to the client: a small one whole; a big one passed on in pieces as they are read
        (and unpacked) here or on the storage node. A Range ("bytes=N-", "bytes=N-M") answers 206 with that
        part - a stored blob read from N, a packed one unpacked and its first N bytes dropped. None when it
        isn't there (the caller's 404)."""
        loop = asyncio.get_running_loop()
        size = self.store.blob_size(sha)
        rng = self.byte_range(request, size) if size else None
        headers = dict(headers or {}, **{"Accept-Ranges": "bytes"})
        if rng == "bad":
            return web.Response(status=416, headers={"Content-Range": "bytes */%d" % size, "Accept-Ranges": "bytes"})
        start, end = rng if rng else (0, (size or 1) - 1)
        if rng:
            headers["Content-Range"] = "bytes %d-%d/%d" % (start, end, size)
        if size is None or size <= self.STREAM_FROM:
            data = await loop.run_in_executor(None, self.store._get_blob, sha)
            if data is None:
                return None
            return web.Response(body=data[start:end + 1] if rng else data, status=206 if rng else 200,
                                content_type=content_type, headers=headers)
        try:
            pieces = await loop.run_in_executor(None, lambda: self.store.blob_chunks(sha, start=start))
        except gs.StorageBusy:   # (the storage node's disk too slow just now: the launcher tries again)
            return web.Response(status=503, text="storage busy, try again shortly", headers={"Retry-After": "10"})
        first = await loop.run_in_executor(None, next, pieces, None) if pieces is not None else None
        if first is None:
            return None
        response = web.StreamResponse(status=206 if rng else 200, headers=dict(headers, **{"Content-Type": content_type}))
        response.content_length = end - start + 1
        await response.prepare(request)
        sent, left = 0, end - start + 1
        try:
            piece = first
            while piece is not None and left > 0:
                if len(piece) > left:   # (bytes=N-M: up to M)
                    piece = piece[:left]
                await response.write(piece)
                sent += len(piece)
                left -= len(piece)
                piece = await loop.run_in_executor(None, next, pieces, None) if left > 0 else None
            if left <= 0:
                await loop.run_in_executor(None, pieces.close)
        except (ConnectionError, asyncio.CancelledError):  # (the client went away)
            await loop.run_in_executor(None, pieces.close)
            raise
        except Exception as e:  # (the node or the file broke part way: the client sees a short download)
            gs.log("download: %s broke after %d of %d bytes: %s" % (sha[:12], sent, size, e))
            await loop.run_in_executor(None, pieces.close)
            raise
        await response.write_eof()
        return response

    async def api_media_get(self, request):
        if not self.account(request):
            return web.Response(status=401)
        sha = request.match_info["sha"].lower()
        if not re.fullmatch(r"[0-9a-f]{64}", sha):
            return web.Response(status=400)
        if request.method == "HEAD":
            return web.Response(status=200 if self.store.stored_media(sha) else 404)
        sha = self.store.stored_media(sha) or sha
        response = await self.send_blob(request, sha) if self.store.has_media(sha) else None
        if response is None:
            return web.Response(status=404)
        self.stats.count("downloads")
        return response

    async def api_entrance(self, request):
        account = self.account(request)
        if not account:
            return web.Response(status=401)
        try:
            fileid = int(request.match_info["fileid"])
        except ValueError:
            return web.Response(status=400)
        if request.method == "GET":
            info = self.store.get_entrance(fileid)
            return web.json_response(info) if info is not None else web.Response(status=404)
        # PUT: the Superstar's owner says which song and movie its entrance uses
        if self.store.file_owner(fileid) != account["profileid"]:
            return web.json_response({"ok": False, "error": "not your upload"}, status=403)
        try:
            body = await request.json()
        except ValueError:
            return web.json_response({"ok": False, "error": "not JSON"}, status=400)
        info = {}
        for kind, names in (("music", ("playlist", "file")), ("movie", ("name",))):
            m = body.get(kind)
            if not isinstance(m, dict):
                continue
            # (the names land on the downloader's PC - the playlist's, the files': the name filter first)
            why = self.names_why(account, *(str(m.get(n, "")) for n in names))
            if why:
                gs.log("entrance: file %d by %s refused: %s (%s)" % (fileid, account["name"], why,
                                                                    ", ".join(str(m.get(n, "")) for n in names)))
                return web.json_response({"ok": False, "error": why}, status=400, headers={"X-Svr-Notice": why})
            sent = str(m.get("sha", "")).lower()
            sha = self.store.stored_media(sent)
            if not sha:
                return web.json_response({"ok": False, "error": "%s not uploaded" % kind}, status=400)
            _, new_ext = self.store.media_alias(sent)
            entry = {"sha": sha}
            for n in names:
                v = str(m.get(n, ""))
                # (a file name on the downloader's PC: no folders, nothing odd)
                if not v or len(v) > 80 or re.search(r'[\\/:*?"<>|\x00-\x1f]', v) or v.strip(". ") != v:
                    return web.json_response({"ok": False, "error": "bad %s %s" % (kind, n)}, status=400)
                entry[n] = v
            for n in ("frames",):
                if isinstance(m.get(n), int):
                    entry[n] = m[n]
            if kind == "music" and new_ext and "file" in entry:  # (the song became an MP3)
                entry["file"] = os.path.splitext(entry["file"])[0] + new_ext
            info[kind] = entry
        self.store.set_entrance(fileid, account["profileid"], info)
        gs.log("entrance: file %d by %s: %s" % (fileid, account["name"], ", ".join(
            "%s %s" % (k, v.get("playlist") or v.get("name")) for k, v in info.items()) or "nothing"))
        return web.json_response({"ok": True})

    async def api_logos_wanted(self, request):
        if not self.account(request):
            return web.Response(status=401)
        return web.Response(text="\n".join(self.store.wanted_logos()))

    # the game

    async def game(self, request):
        account = self.account(request)
        if not account:
            return web.Response(status=401, text="sign in first")
        path = request.match_info["tail"].lower()
        body = await request.read()
        loop = asyncio.get_running_loop()
        try:
            if path.endswith("sakefileserver/upload.aspx"):
                return await loop.run_in_executor(None, self.upload, account, request.headers, body)
            if path.endswith("sakefileserver/download.aspx"):
                fileid = int(request.query.get("fileid", "0") or 0)
                data = await loop.run_in_executor(None, self.store.read_file, fileid)
                if data is None:
                    return web.Response(body=b"", content_type="application/octet-stream",
                                        headers={"Sake-File-Result": "4"})
                self.stats.count("downloads")
                return web.Response(body=data, content_type="application/octet-stream",
                                    headers={"Sake-File-Result": "0"})
            if "dime" in request.headers.get("Content-Type", ""):
                body = gs.dime_soap(body)
            func, call = gs.soap_call(body)
            xuid = Accounts.xuid(account)
            if path.endswith("authservice/authservice.asmx"):
                xml = gs.login(self.store, func, call, account["profileid"], account["name"])
            elif path.endswith("sakestorageserver/storageserver.asmx"):
                modfree = request.headers.get("X-Svr-ModFree") == "1"  # (the game's relay: checked for mods)
                notes = []
                xml = await loop.run_in_executor(None, self.sake.handle, func, call, account["profileid"], xuid,
                                                 modfree, notes)
                result = re.search(rb"<%sResult>([^<]*)<" % func.encode(), xml)
                gs.log("sake: %s %s %s by %s -> %s" % (func, gs.text_of(call, "tableid"), gs.text_of(call, "recordid", ""),
                                                     account["name"], result.group(1).decode() if result else "?"))
                if notes:  # (the game's relay shows it on screen: 2.1.6 on)
                    return web.Response(body=xml, content_type="text/xml", charset="utf-8",
                                        headers={"X-Svr-Notice": notes[0]})
            elif path.endswith("competitionservice/competitionservice.asmx"):
                xml = gs.competition(func, call)
            else:
                gs.log("game: %s %s: not handled" % (request.method, path))
                return web.Response(body=b"")
            return web.Response(body=xml, content_type="text/xml", charset="utf-8")
        except Exception:
            gs.log("error:", traceback.format_exc())
            return web.Response(body=b"")

    def upload(self, account, headers, body):
        files = [f for f in gs.multipart_files(body, headers.get("Content-Type", "")) if f["filename"] is not None]
        if len(files) != 1:
            return web.Response(body=b"", headers={"Sake-File-Result": "2"})
        data = files[0]["data"]
        if len(data) > self.store.max_file:
            return web.Response(body=b"", headers={"Sake-File-Result": "5"})
        pid = account["profileid"]
        if self.store.owner_bytes(pid) + len(data) > self.store.quota:
            gs.log("files: upload from %s refused: over the quota" % account["name"])
            return web.Response(body=b"", headers={"Sake-File-Result": "5"})
        fileid = self.store.add_file(pid, data)
        self.stats.count("uploads")
        gs.log("files: upload %d bytes from %s -> file %d" % (len(data), account["name"], fileid))
        return web.Response(body=b"", headers={"Sake-File-Result": "0", "Sake-File-Id": str(fileid)})

    # the dashboard

    def admin_only(handler):
        """The dashboard's routes: under /svr-admin/ only a valid admin session (and on a POST its
        CSRF token) lets one in - no header does; elsewhere is_admin's rules (the portal, --local-admin)."""
        async def wrapped(self, request):
            if not request.path.startswith(aa.PREFIX + "/"):
                if not self.is_admin(request):
                    return web.Response(status=403, text="Admins only.")
                return await handler(self, request)
            s = self.admin_auth.session(request)
            if not s:
                if request.method == "GET" and request.path == aa.PREFIX + "/":
                    raise web.HTTPSeeOther(aa.PREFIX + "/login")
                return web.json_response({"ok": False, "error": "Sign in first."}, status=401)
            if request.method not in ("GET", "HEAD") and not self.admin_auth.csrf_ok(request, s):
                gs.log("admin: %s %s refused - no valid CSRF token (from %s)" % (
                    request.method, request.path, self.client_ip(request)))
                return web.json_response({"ok": False, "error": "Reload the page."}, status=403)
            request["admin_session"] = s
            return await handler(self, request)
        return wrapped

    # Discovery: players' .svrmod mods (discovery.py). Browsing, pictures and downloads need no account;
    # uploads need one (its name is the author); owners delete their own, the dashboard hides / deletes any.

    def _mod_id(self, request):
        try:
            return int(request.match_info["id"])
        except (KeyError, ValueError):
            return None

    async def api_mods(self, request):
        q = request.query
        try:
            page = max(0, int(q.get("page", "0")))
        except ValueError:
            page = 0
        types = sum(v for k, v in discovery.TYPES.items() if k in q.get("type", "").lower().split(","))
        res = await asyncio.get_running_loop().run_in_executor(
            None, discovery.search, self.store, q.get("q", "")[:100], types, q.get("tag", "")[:24],
            q.get("sort", "new"), page)
        return web.json_response(res)

    async def api_mod(self, request):
        mid = self._mod_id(request)
        if mid is None:
            return web.Response(status=400)
        if request.method == "DELETE":
            account = self.account(request)
            if not account:
                return web.Response(status=401)
            if discovery.owner_of(self.store, mid) != account["id"]:
                return web.json_response({"ok": False, "error": "Only its uploader can delete it."}, status=403)
            await asyncio.get_running_loop().run_in_executor(None, discovery.delete, self.store, mid)
            gs.log("discovery: mod %d deleted by its owner %s" % (mid, account["name"]))
            return web.json_response({"ok": True})
        if request.method == "PATCH":
            return await self.api_mod_edit(request, mid)
        if request.method not in ("GET", "HEAD"):
            return web.Response(status=405)
        info, _ = discovery.get(self.store, mid)
        if not info:
            return web.Response(status=404)
        # (asked with a Bearer token: is it the asker's own mod - the launchers' "my mod" buttons go by this)
        account = self.account(request) if request.headers.get("Authorization") else None
        info["mine"] = bool(account) and info["owner"] == account["id"]
        return web.json_response(info)

    # The owner's edits (docs/DISCOVERY_API.md): the same check as DELETE (the Bearer account is the uploader's),
    # OWNER_EDITS an hour per account; a file update also takes the upload slot.
    OWNER_EDITS = 30

    def mod_owner_check(self, request, mid):
        """(account, None) when the Bearer account owns mod mid, else (None, an error response)."""
        account = self.account(request)
        if not account:
            return None, web.json_response({"ok": False, "error": "Sign in online first."}, status=401)
        owner = discovery.owner_of(self.store, mid) if mid is not None else None
        if owner is None:
            return None, web.json_response({"ok": False, "error": "No such mod."}, status=404)
        if owner != account["id"]:
            return None, web.json_response({"ok": False, "error": "Only its uploader can change it."}, status=403)
        if self.throttled("modedit:%d" % account["id"], limit=self.OWNER_EDITS, window=3600):
            return None, web.json_response({"ok": False, "error": "Too many changes - wait a while."}, status=429)
        return account, None

    @staticmethod
    def mod_fields(raw):
        """The editable text fields of a request, limited as an upload's ({} entries left out); types -> bits.
        Returns (fields, why)."""
        fields = {}
        for k, limit in discovery.MAX_TEXT.items():
            if k in raw and raw[k] is not None:
                fields[k] = str(raw[k]).strip()[:limit]
        if "tags" in fields:
            fields["tags"] = discovery.clean_tags(fields["tags"])
        if "types" in raw and raw["types"] is not None:
            t = raw["types"]
            names = t if isinstance(t, list) else str(t).split(",")
            fields["types"] = sum(v for k, v in discovery.TYPES.items() if k in [str(n).strip().lower() for n in names])
            if not fields["types"]:
                return None, "Pick the mod's type."
        if "name" in fields and not fields["name"]:
            return None, "A name is needed."
        if "version" in fields and not fields["version"]:
            return None, "A version is needed."
        return fields, None

    def mod_quota_why(self, account, size, but=None):
        """Why an upload / update of `size` bytes is over the account's Discovery total, or None."""
        quota = SETTINGS["discovery_quota_mb"] << 20
        have = discovery.account_bytes(self.store, account["id"], but)
        if have + size <= quota:
            return None
        return ("Your Discovery mods would take %.0f MB; one account may have %d MB in all. Delete or update an "
                "older mod first." % ((have + size) / 1048576, SETTINGS["discovery_quota_mb"]))

    @staticmethod
    def mod_size_warning(size):
        """{"warning": text} for a big mod (stored all the same), else {}."""
        if size <= SETTINGS["discovery_warn_mb"] << 20:
            return {}
        return {"warning": "This mod is %.0f MB: players wait a while for it. A big entrance movie is usually why - "
                           "an .mp4 is about 10x smaller than a Bink .bik." % (size / 1048576)}

    async def api_mod_edit(self, request, mid):
        account, err = self.mod_owner_check(request, mid)
        if err:
            return err
        try:
            body = await request.json()
        except ValueError:
            body = None
        if not isinstance(body, dict):
            return web.json_response({"ok": False, "error": "Send a JSON object."}, status=400)
        fields, why = self.mod_fields(body)
        order = body.get("picture_order")
        if not why and order is not None:
            if not (isinstance(order, list) and sorted(order) == list(range(discovery.IMAGES)) and
                    all(isinstance(i, int) for i in order)):
                why = "picture_order: the numbers 0-3, each once."
            elif order == list(range(discovery.IMAGES)):
                order = None
        if not why and not fields and order is None:
            why = "Nothing to change."
        if not why:
            why = self.names_why(account, *fields.values())
            if why:
                gs.log("discovery: edit of mod %d from %s refused: %s" % (mid, account["name"], why))
        if why:
            return web.json_response({"ok": False, "error": why}, status=400)
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, discovery.edit, self.store, mid, fields, order)
        what = [w for w, on in (("info", fields), ("pictures", order is not None)) if on]
        gs.log("discovery: mod %d updated by its owner (%s)%s" % (
            mid, " + ".join(what), (": " + ", ".join(sorted(fields))) if fields else ""))
        info, _ = discovery.get(self.store, mid, True)
        return web.json_response({"ok": True, "mod": info})

    def mod_admin_key(self, request):
        """The storage node's key (X-Storage-Key) on a core that has a node: an admin's update of any mod from the
        PC (the bulk movie shrink) - through /svr/api/mods/{id}/file, whose nginx location takes big bodies."""
        key = request.headers.get("X-Storage-Key", "")
        return bool(self.node_key and key and hmac.compare_digest(key, self.node_key))

    async def api_mod_update_file(self, request):
        """PUT multipart: svrmod (the new file) + version (a new one); maybe name / tags / description / types.
        Its owner's (Bearer), or an admin's with the storage node's key (mod_admin_key)."""
        mid = self._mod_id(request)
        if self.mod_admin_key(request):
            if mid is None or discovery.owner_of(self.store, mid) is None:
                return web.json_response({"ok": False, "error": "No such mod."}, status=404)
            return await self.mod_update_file(request, mid, None)
        account, err = self.mod_owner_check(request, mid)
        if err:
            return err
        return await self.mod_update_file(request, mid, account)

    @admin_only
    async def admin_mod_update_file(self, request):
        """The dashboard's PUT of a mod's new version (as api_mod_update_file; the owner stays)."""
        mid = self._mod_id(request)
        if mid is None or discovery.owner_of(self.store, mid) is None:
            return web.json_response({"ok": False, "error": "No such mod."}, status=404)
        return await self.mod_update_file(request, mid, None)

    async def mod_update_file(self, request, mid, account):
        """A new version of mod mid from its owner (account), or from an admin (None): then no quota, and the
        owner, author, id and downloads stay all the same; the form's "why" goes in the log."""
        length = request.content_length
        if not length:
            return web.json_response({"ok": False, "error": "no length"}, status=411)
        if length > discovery.MAX_MOD + (8 << 20):
            return web.json_response({"ok": False, "error": "larger than %d MB" % (discovery.MAX_MOD >> 20)},
                                     status=413)
        if self.mod_upload.locked():
            return web.json_response({"ok": False, "error": "Another mod is being stored - try again in a minute."},
                                     status=503)
        async with self.mod_upload:
            raw, data, file_name, admin_why = {}, None, "", "admin"
            reader = await request.multipart()
            while True:
                part = await reader.next()
                if part is None:
                    break
                name = part.name or ""
                if name == "svrmod":
                    file_name = os.path.basename((part.filename or "").replace("\\", "/"))
                    data = await part.read()
                elif name in discovery.MAX_TEXT or name == "types":
                    raw[name] = (await part.text()).strip()[:discovery.MAX_TEXT.get(name, 100)]
                elif name == "why" and account is None:
                    admin_why = (await part.text()).strip()[:60] or admin_why
                else:
                    await part.release()
            fields, why = self.mod_fields(raw)
            old, _ = discovery.get(self.store, mid, True)
            loop = asyncio.get_running_loop()
            if not why:
                if not data or not file_name.lower().endswith(".svrmod"):
                    why = "Only a .svrmod file can be uploaded."
                elif not fields.get("version"):
                    why = "Give the update a version."
                elif fields["version"] == (old or {}).get("version"):
                    why = "Give the update a new version (it is %s now)." % old["version"]
            if not why:
                why = self.names_why(account, *fields.values())
            if not why:
                manifest, why = await loop.run_in_executor(None, discovery.check_svrmod, data)
            if not why and account:
                why = self.mod_quota_why(account, len(data), but=mid)
            by = "its owner" if account else "admin (%s)" % admin_why
            if why:
                gs.log("discovery: update of mod %d from %s refused: %s" % (
                    mid, account["name"] if account else by, why))
                return web.json_response({"ok": False, "error": why}, status=400)
            fields["needs"] = manifest.get("needs", "")[:20]  # (the new file's own)
            was = await loop.run_in_executor(None, discovery.replace_file, self.store, mid, data, file_name, fields)
            if was is None:
                return web.json_response({"ok": False, "error": "No such mod."}, status=404)
            gs.log("discovery: mod %d updated by %s (file) %s -> %s (%d -> %d bytes, %s)" % (
                mid, by, was, fields["version"], (old or {}).get("size", 0), len(data), manifest.get("type", "?")))
            info, _ = discovery.get(self.store, mid, True)
            return web.json_response(dict({"ok": True, "mod": info}, **self.mod_size_warning(len(data))))

    async def api_mod_update_picture(self, request):
        """PUT: the body is picture n (0-3), a 1024 x 768 JPEG."""
        mid = self._mod_id(request)
        try:
            n = int(request.match_info["n"])
        except ValueError:
            n = -1
        if not 0 <= n < discovery.IMAGES:
            return web.json_response({"ok": False, "error": "Pictures are 0-3."}, status=400)
        account, err = self.mod_owner_check(request, mid)
        if err:
            return err
        if (request.content_length or 0) > discovery.MAX_IMG:
            return web.json_response({"ok": False, "error": "A picture is at most 1 MB."}, status=413)
        img = await request.content.read(discovery.MAX_IMG + 1)
        if len(img) > discovery.MAX_IMG or discovery.jpeg_size(img) != (discovery.IMG_W, discovery.IMG_H):
            return web.json_response({"ok": False, "error": "Send a %d x %d JPEG." % (discovery.IMG_W, discovery.IMG_H)},
                                     status=400)
        await asyncio.get_running_loop().run_in_executor(None, discovery.replace_picture, self.store, mid, n, img)
        gs.log("discovery: mod %d updated by its owner (pictures): picture %d" % (mid, n + 1))
        info, _ = discovery.get(self.store, mid, True)
        return web.json_response({"ok": True, "mod": info})

    async def api_mod_file(self, request):
        mid = self._mod_id(request)
        info, blobs = discovery.get(self.store, mid) if mid is not None else (None, None)
        if not info:
            return web.Response(status=404)
        name = re.sub(r'[^A-Za-z0-9 ._()-]', "_", blobs["file_name"] or "mod.svrmod")
        # (counted before it is sent: a streamed download's handler returns only at its end; a resumed one -
        # a Range not from 0 - is the same download, not counted again)
        size = self.store.blob_size(blobs["sha"])
        if size is None:
            return web.Response(status=404)
        rng = self.byte_range(request, size)
        if not rng or (rng != "bad" and rng[0] == 0):
            discovery.counted(self.store, mid)
            self.stats.count("downloads")
        response = await self.send_blob(request, blobs["sha"],
                                        headers={"Content-Disposition": 'attachment; filename="%s"' % name})
        return response if response is not None else web.Response(status=404)

    async def api_mod_image(self, request):
        mid = self._mod_id(request)
        info, blobs = discovery.get(self.store, mid) if mid is not None else (None, None)
        try:
            n = int(request.match_info["n"])
        except ValueError:
            return web.Response(status=400)
        if not info or not 0 <= n < len(blobs["images"]):
            return web.Response(status=404)
        data = await asyncio.get_running_loop().run_in_executor(None, self.store._get_blob, blobs["images"][n])
        if data is None:
            return web.Response(status=404)
        etag = '"%s"' % blobs["images"][n][:32]
        if request.headers.get("If-None-Match") == etag:
            return web.Response(status=304, headers={"ETag": etag})
        # (clients ask for img/{n}?v=<pics>: a changed picture is a new URL)
        return web.Response(body=data, content_type="image/jpeg",
                            headers={"Cache-Control": "public, max-age=86400", "ETag": etag})

    async def api_mod_upload(self, request):
        """multipart: name, version, types (entrance,superstar,arena,moveset), tags, description; svrmod (the file);
        img0-img3 (1024 x 768 JPEG). Every one but tags and description is needed."""
        account = self.account(request)
        if not account:
            return web.json_response({"ok": False, "error": "Sign in online first."}, status=401)
        length = request.content_length
        if not length:
            return web.json_response({"ok": False, "error": "no length"}, status=411)
        if length > discovery.MAX_MOD + (8 << 20):
            return web.json_response({"ok": False, "error": "larger than %d MB" % (discovery.MAX_MOD >> 20)},
                                     status=413)
        if self.mod_upload.locked():
            return web.json_response({"ok": False, "error": "Another mod is being stored - try again in a minute."},
                                     status=503)
        async with self.mod_upload:
            fields, images, data, file_name = {}, {}, None, ""
            reader = await request.multipart()
            while True:
                part = await reader.next()
                if part is None:
                    break
                name = part.name or ""
                if name == "svrmod":
                    file_name = os.path.basename((part.filename or "").replace("\\", "/"))
                    data = await part.read()
                elif re.fullmatch(r"img[0-3]", name):
                    images[int(name[3])] = await part.read()
                elif name in discovery.MAX_TEXT or name == "types":
                    fields[name] = (await part.text()).strip()[:discovery.MAX_TEXT.get(name, 100)]
                else:
                    await part.release()
            types = sum(v for k, v in discovery.TYPES.items() if k in fields.get("types", "").lower().split(","))
            why = None
            if not fields.get("name") or not fields.get("version") or not types:
                why = "A name, a version and the mod's type are needed."
            elif not data or not file_name.lower().endswith(".svrmod"):
                why = "Only a .svrmod file can be uploaded."
            elif sorted(images) != list(range(discovery.IMAGES)):
                why = "Four pictures are needed."
            else:
                for n, img in sorted(images.items()):
                    if len(img) > discovery.MAX_IMG or discovery.jpeg_size(img) != (discovery.IMG_W, discovery.IMG_H):
                        why = "Picture %d isn't a %d x %d JPEG." % (n + 1, discovery.IMG_W, discovery.IMG_H)
                        break
            loop = asyncio.get_running_loop()
            if not why:
                why = self.names_why(account, *(fields.get(k, "") for k in discovery.MAX_TEXT))
            if not why:
                manifest, why = await loop.run_in_executor(None, discovery.check_svrmod, data)
            if not why:
                why = self.mod_quota_why(account, len(data))
            if why:
                gs.log("discovery: upload from %s refused: %s" % (account["name"], why))
                return web.json_response({"ok": False, "error": why}, status=400)
            fields["types"] = types
            fields["tags"] = discovery.clean_tags(fields.get("tags", ""))
            fields.setdefault("description", "")
            fields["needs"] = manifest.get("needs", "")[:20]
            mid = await loop.run_in_executor(None, discovery.add, self.store, account["id"], account["name"], fields,
                                             data, file_name, [images[n] for n in range(discovery.IMAGES)])
            gs.log("discovery: mod %d \"%s\" %s (%d bytes, %s) from %s" % (
                mid, fields["name"], fields["version"], len(data), manifest.get("type", "?"), account["name"]))
            return web.json_response(dict({"ok": True, "id": mid, "size": len(data)}, **self.mod_size_warning(len(data))))

    # -- the name filter (namefilter.py; the dashboard's Names tab) --------------------------------------------

    STRING_TYPES = ("asciiStringValue", "unicodeStringValue")
    NOT_NAMES = ("AuthorName", "UserID")  # (the game's ids, not text a player wrote)

    def names_why(self, account, *texts):
        """Why these texts (and the uploader's account name) are refused, or None."""
        why = self.names.check(*texts)
        if not why and account and self.names.check(account["name"]):
            why = namefilter.ACCOUNT_REASON
        return why

    def sake_names(self, owner, values):
        """A Community Creations record's texts - its title, tags and comments - and its uploader's name."""
        texts = [str(v) for k, (t, v) in values.items() if t in self.STRING_TYPES and k not in self.NOT_NAMES]
        return self.names_why(self.accounts.by_profile(owner), *texts)

    def names_flagged(self):
        """What is on the server now that the list flags (whatever the switch): uploads, Discovery mods,
        entrances, accounts. Nothing is changed - the dashboard's Delete buttons are the owner's say."""
        nf, out = self.names, []
        by_profile = {a["profileid"]: a for a in self.accounts.all()}

        def who(profileid):
            a = by_profile.get(profileid)
            return a["name"] if a else "profile %d" % profileid

        files = {}  # (an entrance's Superstar file -> its upload)
        for rec in self.store.all("UserContent"):
            f = rec["fields"]
            fid = int((f.get("F00") or [None, 0])[1] or 0)
            if fid:
                files[fid] = rec["recordid"]
            for k, (t, v) in sorted(f.items()):
                term = nf.match(v) if t in self.STRING_TYPES and k not in self.NOT_NAMES else None
                if term:
                    out.append({"kind": "upload", "id": rec["recordid"], "field": k, "text": str(v)[:200],
                                "term": term, "author": who(rec["ownerid"]),
                                "title": str((f.get("Title") or [None, ""])[1] or "")[:80]})
                    break
        with self.store.lock:
            mods = self.store.db.execute("SELECT id, name, tags, description, version, author FROM mods").fetchall()
            entrances = self.store.db.execute("SELECT fileid, ownerid, info FROM entrances").fetchall()
        for mid, *texts, author in mods:
            for k, v in zip(("name", "tags", "description", "version"), texts):
                term = nf.match(v)
                if term:
                    out.append({"kind": "mod", "id": mid, "field": k, "text": str(v)[:200], "term": term,
                                "author": author, "title": texts[0]})
                    break
        for fileid, ownerid, info in entrances:
            try:
                info = json.loads(info)
            except ValueError:
                continue
            hits = [(kind + " " + k, str(v)) for kind, m in sorted(info.items()) if isinstance(m, dict)
                    for k, v in sorted(m.items()) if k in ("playlist", "file", "name") and nf.match(v)]
            if hits:
                out.append({"kind": "entrance", "id": files.get(fileid), "file": fileid, "field": hits[0][0],
                            "text": hits[0][1][:200], "term": nf.match(hits[0][1]), "author": who(ownerid),
                            "title": ""})
        for a in by_profile.values():
            term = nf.match(a["name"])
            if term:
                out.append({"kind": "account", "id": a["id"], "field": "name", "text": a["name"], "term": term,
                            "author": a["name"], "title": "", "banned": bool(a["banned"])})
        return out

    @admin_only
    async def admin_names(self, request):
        """GET: the list, the allow list, the switch. POST {enabled?, words?, allow?}: saved, used at once."""
        if request.method == "POST":
            try:
                body = await request.json()
            except ValueError:
                body = None
            if not isinstance(body, dict):
                return web.json_response({"ok": False, "error": "Send a JSON object."}, status=400)
            lists = {}
            for k in ("words", "allow"):
                v = body.get(k)
                if v is None:
                    continue
                v = v.splitlines() if isinstance(v, str) else v
                if not isinstance(v, list) or len(v) > 5000 or any(len(str(w)) > 100 for w in v):
                    return web.json_response({"ok": False, "error": "%s: at most 5000 lines of 100 characters" % k},
                                             status=400)
                lists[k] = [str(w).strip() for w in v if str(w).strip()]
            await asyncio.get_running_loop().run_in_executor(
                None, lambda: self.names.save(body.get("enabled"), lists.get("words"), lists.get("allow")))
            gs.log("admin: name filter saved (%s, %d words, %d allowed)" % (
                "on" if self.names.enabled else "off", len(self.names.word_lines), len(self.names.allow_lines)))
        return web.json_response(dict({"ok": True}, **self.names.state()))

    @admin_only
    async def admin_names_test(self, request):
        """POST {text}: would this name pass? (the list as it is saved; the switch aside)"""
        try:
            body = await request.json()
        except ValueError:
            body = {}
        text = str((body or {}).get("text", ""))[:2000]
        term = self.names.match(text)
        return web.json_response({"ok": True, "passes": term is None, "term": term,
                                  "readings": [" ".join(r) for r in namefilter.words(text)]})

    @admin_only
    async def admin_names_flagged(self, request):
        rows = await asyncio.get_running_loop().run_in_executor(None, self.names_flagged)
        return web.json_response({"ok": True, "enabled": self.names.enabled, "flagged": rows})

    # Problem reports (reports.py, docs/REPORTS_API.md): anyone may send one (a Bearer account is noted);
    # 5 in 10 minutes and 20 a day per address, and per account.
    REPORT_LIMITS = ((600, 5), (86400, 20))

    def report_throttled(self, keys):
        t = now()
        for key in keys:
            q = self.report_times[key]
            while q and q[0] < t - self.REPORT_LIMITS[-1][0]:
                q.popleft()
            if any(sum(1 for x in q if x >= t - span) >= most for span, most in self.REPORT_LIMITS):
                return True
        for key in keys:
            self.report_times[key].append(t)
        return False

    CHECK_LIMIT = (600, 30)  # mod checks per IP: 30 in 10 minutes
    MAX_REPORT_BODY = (reports.MAX_ZIP + reports.MAX_IMAGES * reports.MAX_IMAGE + reports.MAX_MATCH +
                       reports.MAX_NEW_MOD_BYTES + (8 << 20))

    async def api_report_mods_check(self, request):
        """{"mods": [{"hash", "size", ...}]} -> {ok, missing: [the hashes to send], max_upload, max_mod, mods_off}"""
        t, q = now(), self.check_times["ip:" + self.client_ip(request)]
        while q and q[0] < t - self.CHECK_LIMIT[0]:
            q.popleft()
        if len(q) >= self.CHECK_LIMIT[1]:
            return web.json_response({"ok": False, "error": "Too many checks from here - try again later."}, status=429)
        q.append(t)
        try:
            body = await request.json()
            mods = reports.clean_mods(body.get("mods"))
        except (ValueError, AttributeError):
            return web.json_response({"ok": False, "error": "Send {\"mods\": [...]} as JSON."}, status=400)
        if (SETTINGS["reports_need_account"] == "on" and str(body.get("app") or "") not in reports.ANTHOLOGY_APPS
                and not self.raw_account(request)):
            # (no sign-in: the report will be refused - an older client, or one signed out - so no mods are asked
            # for; it's refused once with its pictures and zip, not after uploading every mod)
            return web.json_response({"ok": True, "missing": [], "max_upload": 0, "max_mod": reports.MAX_MOD,
                                      "mods_off": True})
        loop = asyncio.get_running_loop()
        try:
            have = set(await loop.run_in_executor(None, self.reports.has_mods, [m["hash"] for m in mods]))
            usage = await loop.run_in_executor(None, self.reports.mod_usage)
        except Exception:
            return web.json_response({"ok": False, "error": "The report store didn't answer - send the report "
                                                            "without mods."}, status=503)
        room = max(0, usage["quota"] - usage["bytes"])
        off = room < (16 << 20)
        return web.json_response({"ok": True, "missing": [] if off else [m["hash"] for m in mods if m["hash"] not in have],
                                  "max_upload": min(reports.MAX_NEW_MOD_BYTES, room), "max_mod": reports.MAX_MOD,
                                  "mods_off": off})

    def report_account(self, request, meta):
        """reports_need_account on: None when it may be sent, else (status, code, error). raw_account: a player
        that account_checks keep off online may still report."""
        if older_release(meta.get("version"), REPORTS_ACCOUNT_SINCE):
            return 426, "update_needed", REPORTS_OLD_CLIENT
        where = "the app's Online page" if meta.get("platform") == "android" else "the launcher's Online tab"
        if not request.headers.get("Authorization", "").startswith("Bearer "):
            return 401, "sign_in_needed", ("Sign in to an online account to send %s: %s (Sign in or Create "
                                           "account)." % ("a suggestion" if meta["kind"] == "suggestion" else
                                                          "a report", where))
        # (a banned account ends here too: banning deletes its sessions, from_token knows none - it isn't told why)
        if not self.raw_account(request):
            return 401, "sign_in_needed", "Your online sign-in has ended - sign in again on %s." % where
        meta["contact"], why = reports.discord_name(meta.get("contact"))
        if why:
            return 400, "discord_needed", why
        return None

    async def api_report(self, request):
        """multipart: report (JSON), zip (optional), image0-image4 (optional png / jpg / webp), match (the match
        folder's zip, optional), mod0..modN (a mod's zip each, named <content hash>.zip: only those the check
        asked for) -> {ok, id, secret (the sender's receipt: its Reports list asks with it), signature, seen_before,
        match, mods_stored, mods_refused}"""
        def no(status, why, code=""):
            return web.json_response(dict({"ok": False, "error": why}, **({"code": code} if code else {})),
                                     status=status)
        length = request.content_length
        if length and length > self.MAX_REPORT_BODY:
            return no(413, "The report is too large (the zip: %d MB at most, pictures: %d MB each, the mods: %d MB)." % (
                reports.MAX_ZIP >> 20, reports.MAX_IMAGE >> 20, reports.MAX_NEW_MOD_BYTES >> 20))
        ip = self.client_ip(request)
        account = self.raw_account(request)  # (noted with the report; banned: refused below when accounts are needed)
        meta, zip_bytes, images, match_bytes = None, b"", {}, b""
        mod_files = {}  # hash -> temporary file (a mod part, streamed to disk)
        tmp_dir = self.report_tmp
        try:
            try:
                reader = await request.multipart()
                mod_total = 0
                while True:
                    part = await reader.next()
                    if part is None:
                        break
                    name = part.name or ""
                    if re.fullmatch(r"mod\d+", name):
                        h = re.sub(r"\.zip$", "", os.path.basename(part.filename or "")).lower()
                        if not re.fullmatch(r"[0-9a-f]{64}", h) or h in mod_files:
                            await part.release()
                            continue
                        os.makedirs(tmp_dir, exist_ok=True)
                        path = os.path.join(tmp_dir, "%s_%s.zip" % (h[:16], secrets.token_hex(4)))
                        mod_files[h] = path
                        got = 0
                        with open(path, "wb") as f:
                            while True:
                                chunk = await part.read_chunk(1 << 20)
                                if not chunk:
                                    break
                                got += len(chunk)
                                mod_total += len(chunk)
                                if got > reports.MAX_MOD or mod_total > reports.MAX_NEW_MOD_BYTES:
                                    return no(413, "The mods are too large (%d MB each, %d MB a report at most)."
                                              % (reports.MAX_MOD >> 20, reports.MAX_NEW_MOD_BYTES >> 20))
                                f.write(chunk)
                        continue
                    cap = (64 << 10 if name in ("report", "report.json") else reports.MAX_ZIP if name == "zip"
                           else reports.MAX_MATCH if name == "match"
                           else reports.MAX_IMAGE if re.fullmatch(r"image\d+", name) else 0)
                    if not cap:
                        await part.release()
                        continue
                    data = bytearray()
                    while True:
                        chunk = await part.read_chunk(1 << 16)
                        if not chunk:
                            break
                        data += chunk
                        if len(data) > cap:
                            return no(413, "%s is too large (%d MB at most)." % (name, max(1, cap >> 20)))
                    if name in ("report", "report.json"):
                        try:
                            raw = json.loads(bytes(data).decode("utf-8-sig"))
                        except ValueError:
                            return no(400, "report: not valid JSON")
                        meta = raw
                    elif name == "zip":
                        zip_bytes = bytes(data)
                    elif name == "match":
                        match_bytes = bytes(data)
                    else:
                        n = int(name[5:])
                        if n >= reports.MAX_IMAGES:
                            return no(400, "%d pictures at most." % reports.MAX_IMAGES)
                        images[n] = bytes(data)
            except (ValueError, AssertionError, KeyError):
                return no(400, "Send it as multipart/form-data.")
            mods_listed = reports.clean_mods(meta.get("mods")) if isinstance(meta, dict) else []
            match_name = str(meta.get("match") or "")[:120] if isinstance(meta, dict) else ""
            meta, why = reports.clean_meta(meta)
            if why:
                return no(400, why)
            if SETTINGS["reports_need_account"] == "on" and meta["app"] not in reports.ANTHOLOGY_APPS:
                refused = self.report_account(request, meta)
                if refused:
                    gs.log("reports: refused from %s %s (%s)" % (meta["app"], meta["version"], refused[1]))
                    return no(refused[0], refused[2], refused[1])
            if zip_bytes and zip_bytes[:2] != b"PK":
                return no(400, "zip: not a zip file")
            if match_bytes and match_bytes[:2] != b"PK":
                return no(400, "match: not a zip file")
            for n, img in sorted(images.items()):
                if not reports.image_ext(img):
                    return no(400, "image%d: only PNG, JPEG or WebP pictures." % n)
            if self.report_throttled(["ip:" + ip] + (["acc:%d" % account["id"]] if account else [])):
                return no(429, "Too many reports from here - try again later.")
            if account:
                meta["account"] = account["name"]
            loop = asyncio.get_running_loop()
            # the mods first (each checked against its hash, within the quota), then the report that lists them
            listed = {m["hash"] for m in mods_listed}
            stored, refused = [], {}
            for h, path in list(mod_files.items()):
                if h not in listed:
                    refused[h] = "not in the report's mod list"
                    continue
                try:
                    ok, why = await loop.run_in_executor(None, self.reports.put_mod, h, path)
                except Exception as e:
                    ok, why = False, "the report store didn't take it (%s)" % e.__class__.__name__
                (stored.append(h) if ok else refused.__setitem__(h, why))
            meta["mods"] = mods_listed
            if match_name:
                meta["match_folder"] = match_name
            try:
                got = await loop.run_in_executor(None, self.reports.add, meta, zip_bytes,
                                                 [images[n] for n in sorted(images)], match_bytes)
            except Exception as e:
                gs.log("reports: storing one failed: %s" % e)
                return no(503, "The report couldn't be stored right now - try again later.")
            gs.log("reports: %s from %s %s (%s, %d KB zip, %d pictures, match %d KB, %d mods sent, %d stored): %s, "
                   "seen %d before" % (got["id"], meta["app"], meta["version"], meta["problem"], len(zip_bytes) >> 10,
                                       len(images), len(match_bytes) >> 10, len(mod_files), len(stored),
                                       got["signature"], got["seen_before"]))
            return web.json_response({"ok": True, "id": got["id"], "secret": got.get("secret", ""),
                                      "signature": got["signature"],
                                      "seen_before": got["seen_before"], "match": bool(match_bytes),
                                      "mods_stored": stored, "mods_refused": refused})
        finally:
            for path in mod_files.values():
                if os.path.exists(path):
                    os.remove(path)

    STATUS_LIMIT = (600, 60)  # receipt lookups per IP: 60 in 10 minutes (a launcher asks when its tab opens)

    async def api_report_status(self, request):
        """{"reports": [{"id", "secret"}, ...]} (100 at most; signed in: the account's other reports too) ->
        {ok, reports: [{id, state, received, app, problem, what, note, version, question, date, replies,
        from_account}]} - state: received / looking / fixed / needs_info / replied / done / duplicate / unknown
        (no such report, or not its secret). Never another report, never the logs, zip, pictures or contact."""
        t, q = now(), self.check_times["status:" + self.client_ip(request)]
        while q and q[0] < t - self.STATUS_LIMIT[0]:
            q.popleft()
        if len(q) >= self.STATUS_LIMIT[1]:
            return web.json_response({"ok": False, "error": "Too many lookups from here - try again later."}, status=429)
        q.append(t)
        try:
            receipts = (await request.json()).get("reports")
        except (ValueError, AttributeError):
            return web.json_response({"ok": False, "error": "Send {\"reports\": [{\"id\", \"secret\"}]} as JSON."},
                                     status=400)
        if not isinstance(receipts, list):
            return web.json_response({"ok": False, "error": "reports: a list"}, status=400)
        account = self.raw_account(request)  # (a blocked account still owns its reports)
        try:
            rows = await asyncio.get_running_loop().run_in_executor(
                None, self.reports.status, receipts[:reports.MAX_STATUS_IDS], account["name"] if account else None)
        except Exception:
            return web.json_response({"ok": False, "error": "The report store didn't answer - try again later."},
                                     status=503)
        return web.json_response({"ok": True, "reports": rows})

    async def api_report_reply(self, request):
        """multipart: report (JSON {id, secret, text}), image0-image2 (optional) -> {ok} / {ok: false, error}: the
        sender's answer to a question we asked (needs_info). Rate-limited as reports, through the name filter."""
        def no(status, why):
            return web.json_response({"ok": False, "error": why}, status=status)
        if (request.content_length or 0) > reports.MAX_REPLY_IMAGES * reports.MAX_IMAGE + (1 << 20):
            return no(413, "The answer is too large (pictures: %d MB each)." % (reports.MAX_IMAGE >> 20))
        meta, images = None, {}
        try:
            reader = await request.multipart()
            while True:
                part = await reader.next()
                if part is None:
                    break
                name = part.name or ""
                cap = (64 << 10 if name in ("report", "report.json") else
                       reports.MAX_IMAGE if re.fullmatch(r"image[0-%d]" % (reports.MAX_REPLY_IMAGES - 1), name) else 0)
                if not cap:
                    await part.release()
                    continue
                data = bytearray()
                while True:
                    chunk = await part.read_chunk(1 << 16)
                    if not chunk:
                        break
                    data += chunk
                    if len(data) > cap:
                        return no(413, "%s is too large." % name)
                if name in ("report", "report.json"):
                    try:
                        meta = json.loads(bytes(data).decode("utf-8-sig"))
                    except ValueError:
                        return no(400, "report: not valid JSON")
                else:
                    images[int(name[5:])] = bytes(data)
        except (ValueError, AssertionError, KeyError):
            return no(400, "Send it as multipart/form-data.")
        if not isinstance(meta, dict):
            return no(400, "No report part.")
        text = str(meta.get("text") or "").replace("\r\n", "\n").strip()[:reports.MAX_REPLY]
        if not text and not images:
            return no(400, "Write an answer first.")
        for n, img in sorted(images.items()):
            if not reports.image_ext(img):
                return no(400, "image%d: only PNG, JPEG or WebP pictures." % n)
        if reports.has_url(text):
            return no(400, reports.url_refusal())
        if text and self.names.check(text):
            return no(400, "Your answer has a word that isn't allowed - please write it without it.")
        ip = self.client_ip(request)
        account = self.raw_account(request)
        if self.report_throttled(["reply:" + ip] + (["reply-acc:%d" % account["id"]] if account else [])):
            return no(429, "Too many answers from here - try again later.")
        try:
            ok, why = await asyncio.get_running_loop().run_in_executor(
                None, self.reports.reply, str(meta.get("id") or ""), meta.get("secret"), text,
                [images[n] for n in sorted(images)], account["name"] if account else None)
        except Exception:
            return no(503, "The report store didn't answer - try again later.")
        if not ok:
            return no(404 if why == "No such report." else 409, why)
        gs.log("reports: %s answered (%d chars, %d pictures)" % (meta.get("id"), len(text), len(images)))
        return web.json_response({"ok": True})

    async def api_report_edit(self, request):
        """multipart: report (JSON {id, secret, what, before, contact}), image0-image4 (optional) -> {ok, edits,
        pictures} / {ok: false, error}: the sender adds to a report it sent - more of what happened / before, a new
        contact, pictures (5 a report in all) - until it's done or a duplicate. Kept as an edit history beside the
        report (reports.edit). Rate-limited as reports, through the name filter."""
        def no(status, why):
            return web.json_response({"ok": False, "error": why}, status=status)
        if (request.content_length or 0) > reports.MAX_IMAGES * reports.MAX_IMAGE + (1 << 20):
            return no(413, "The change is too large (pictures: %d MB each)." % (reports.MAX_IMAGE >> 20))
        meta, images = None, {}
        try:
            reader = await request.multipart()
            while True:
                part = await reader.next()
                if part is None:
                    break
                name = part.name or ""
                cap = (64 << 10 if name in ("report", "report.json") else
                       reports.MAX_IMAGE if re.fullmatch(r"image[0-%d]" % (reports.MAX_IMAGES - 1), name) else 0)
                if not cap:
                    await part.release()
                    continue
                data = bytearray()
                while True:
                    chunk = await part.read_chunk(1 << 16)
                    if not chunk:
                        break
                    data += chunk
                    if len(data) > cap:
                        return no(413, "%s is too large." % name)
                if name in ("report", "report.json"):
                    try:
                        meta = json.loads(bytes(data).decode("utf-8-sig"))
                    except ValueError:
                        return no(400, "report: not valid JSON")
                else:
                    images[int(name[5:])] = bytes(data)
        except (ValueError, AssertionError, KeyError):
            return no(400, "Send it as multipart/form-data.")
        if not isinstance(meta, dict):
            return no(400, "No report part.")
        what, before = (str(meta.get(k) or "").replace("\r\n", "\n").strip()[:reports.MAX_EDIT_TEXT]
                        for k in ("what", "before"))
        contact = str(meta.get("contact") or "").replace("\n", " ").strip()[:200]
        if not (what or before or contact or images):
            return no(400, "Write what to add, or add a picture.")
        for n, img in sorted(images.items()):
            if not reports.image_ext(img):
                return no(400, "image%d: only PNG, JPEG or WebP pictures." % n)
        if any(reports.has_url(t) for t in (what, before, contact)):
            return no(400, reports.url_refusal())
        if any(t and self.names.check(t) for t in (what, before, contact)):
            return no(400, "Your text has a word that isn't allowed - please write it without it.")
        ip = self.client_ip(request)
        account = self.raw_account(request)
        if self.report_throttled(["edit:" + ip] + (["edit-acc:%d" % account["id"]] if account else [])):
            return no(429, "Too many changes from here - try again later.")
        try:
            ok, result = await asyncio.get_running_loop().run_in_executor(
                None, lambda: self.reports.edit(str(meta.get("id") or ""), meta.get("secret"), what, before, contact,
                                                [images[n] for n in sorted(images)],
                                                account["name"] if account else None))
        except Exception:
            return no(503, "The report store didn't answer - try again later.")
        if not ok:
            return no(404 if result == "No such report." else 409, result)
        gs.log("reports: %s edited (%d + %d chars, %d pictures%s)" % (meta.get("id"), len(what), len(before),
                                                                     len(images), ", contact" if contact else ""))
        return web.json_response({"ok": True, "edits": result.get("edits", 0), "pictures": result.get("pictures", 0)})

    @admin_only
    async def admin_reports(self, request):
        try:
            rows = await asyncio.get_running_loop().run_in_executor(None, self.reports.list, 1000)
        except Exception as e:
            return web.json_response({"ok": False, "error": "The storage node didn't answer (%s)." % e}, status=503)
        try:
            usage = await asyncio.get_running_loop().run_in_executor(None, self.reports.mod_usage)
        except Exception:
            usage = None
        return web.json_response({"ok": True, "reports": rows, "mod_store": usage}, headers=ADMIN_HEADERS)

    async def stream_opened(self, request, opened, filename):
        """(file, size, temporary path or None) -> the download, read in pieces; the file closed (and removed)."""
        f, size, tmp = opened
        loop = asyncio.get_running_loop()
        try:
            response = web.StreamResponse(headers=dict(ADMIN_HEADERS, **{
                "Content-Type": "application/zip", "Content-Disposition": 'attachment; filename="%s"' % filename}))
            if size:
                response.content_length = size
            await response.prepare(request)
            while True:
                chunk = await loop.run_in_executor(None, f.read, 1 << 20)
                if not chunk:
                    break
                await response.write(chunk)
            await response.write_eof()
            return response
        finally:
            await loop.run_in_executor(None, f.close)
            if tmp and os.path.exists(tmp):
                os.remove(tmp)

    @admin_only
    async def admin_report_mod(self, request):
        h = request.match_info["hash"].lower()
        if not re.fullmatch(r"[0-9a-f]{64}", h):
            return web.Response(status=404)
        opened = await asyncio.get_running_loop().run_in_executor(None, self.reports.open_mod, h)
        if not opened:
            return web.Response(status=404)
        name = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.query.get("name", "") or "mod")[:60]
        return await self.stream_opened(request, opened, "%s_%s.svrmod" % (name, h[:12]))

    @admin_only
    async def admin_report_replay(self, request):
        rid = request.match_info["id"]
        opened = await asyncio.get_running_loop().run_in_executor(None, self.reports.open_replay, rid)
        if not opened:
            return web.Response(status=404)
        return await self.stream_opened(request, opened, "replay_%s.zip" % rid)

    @admin_only
    async def admin_report_file(self, request):
        rid, name = request.match_info["id"], request.match_info["name"]
        data = await asyncio.get_running_loop().run_in_executor(None, self.reports.file, rid, name)
        if data is None:
            return web.Response(status=404)
        ext = name.rsplit(".", 1)[-1]
        ctype = {"png": "image/png", "jpg": "image/jpeg", "webp": "image/webp", "json": "application/json",
                 "zip": "application/zip"}.get(ext, "application/octet-stream")
        headers = dict(ADMIN_HEADERS)
        if ext == "zip":
            headers["Content-Disposition"] = 'attachment; filename="%s.zip"' % rid
        return web.Response(body=data, content_type=ctype, headers=headers)

    @admin_only
    async def admin_report_mark(self, request):
        # (the id may be a signature: every report with it; status fixed takes fix: {note, commit, version, date})
        try:
            body = await request.json()
            status, fix = str(body.get("status", "")), body.get("fix")
        except (ValueError, AttributeError):
            return web.json_response({"ok": False}, status=400)
        ids = await asyncio.get_running_loop().run_in_executor(None, self.reports.mark_many, request.match_info["id"],
                                                               status, fix)
        return web.json_response({"ok": bool(ids), "ids": ids}, status=200 if ids else 404)

    @admin_only
    async def admin_report_delete(self, request):
        done = await asyncio.get_running_loop().run_in_executor(None, self.reports.delete, request.match_info["id"])
        return web.json_response({"ok": done}, status=200 if done else 404)

    # The blob audit: the files where the bytes live (here, or the storage node) against the blobs the
    # database refers to. Orphans (a file nothing refers to: a delete while the node was down) are listed;
    # one goes only with the dashboard's Delete on it.
    def blob_audit(self):
        files = self.store.blobs.list()
        with self.store.lock:
            refs = dict(self.store.db.execute("SELECT sha, refs FROM blobs").fetchall())
        labels = discovery.blob_labels(self.store)
        have = {f["sha"] for f in files}
        orphans = [dict(f, refs=refs.get(f["sha"])) for f in files if (refs.get(f["sha"]) or 0) <= 0]
        missing = [{"sha": s, "refs": r, "what": labels.get(s, "")} for s, r in refs.items() if r > 0 and s not in have]
        hidden = [dict(f, what=labels[f["sha"]]) for f in files
                  if f["sha"] in labels and "(hidden)" in labels[f["sha"]] and "picture" not in labels[f["sha"]]]
        return {"files": len(files), "bytes": sum(f["size"] for f in files),
                "packed": sum(1 for f in files if f["packed"]), "referenced": sum(1 for r in refs.values() if r > 0),
                "orphans": sorted(orphans, key=lambda f: -f["size"]),
                "orphan_bytes": sum(f["size"] for f in orphans), "missing": missing, "hidden_mods": hidden}

    @admin_only
    async def admin_blob_audit(self, request):
        try:
            res = await asyncio.get_running_loop().run_in_executor(None, self.blob_audit)
        except Exception as e:  # (the node down: say so)
            return web.json_response({"ok": False, "error": "The storage didn't answer: %s" % e}, status=502)
        gs.log("admin: blob audit - %d files, %d orphans (%.1f MB), %d missing" % (
            res["files"], len(res["orphans"]), res["orphan_bytes"] / 1e6, len(res["missing"])))
        return web.json_response(dict(ok=True, **res))

    @admin_only
    async def admin_blob_delete(self, request):
        """POST: deletes one orphan's file - only when the database still refers to nothing by it."""
        sha = request.match_info["sha"]
        if not re.fullmatch(r"[0-9a-f]{64}", sha):
            return web.json_response({"ok": False}, status=400)
        with self.store.lock:
            r = self.store.db.execute("SELECT refs FROM blobs WHERE sha = ?", (sha,)).fetchone()
            if r and r[0] > 0:
                return web.json_response({"ok": False, "error": "Something refers to it: not an orphan."},
                                         status=409)
            if sha in discovery.blob_labels(self.store):
                return web.json_response({"ok": False, "error": "A Discovery mod refers to it."}, status=409)
            if r:
                self.store.db.execute("DELETE FROM blobs WHERE sha = ?", (sha,))
                self.store.db.commit()
        await asyncio.get_running_loop().run_in_executor(None, self.store.blobs.delete, sha)
        gs.log("admin: orphan blob %s deleted" % sha[:16])
        return web.json_response({"ok": True})

    @admin_only
    async def admin_mods(self, request):
        q = request.query
        try:
            page = max(0, int(q.get("page", "0")))
        except ValueError:
            page = 0
        res = await asyncio.get_running_loop().run_in_executor(
            None, lambda: discovery.search(self.store, q.get("q", ""), 0, "", "new", page, True))
        return web.json_response(res)

    @admin_only
    async def admin_mod_image(self, request):
        mid = self._mod_id(request)
        _, blobs = discovery.get(self.store, mid, True) if mid is not None else (None, None)
        if not blobs or not blobs["images"]:
            return web.Response(status=404)
        data = await asyncio.get_running_loop().run_in_executor(None, self.store._get_blob, blobs["images"][0])
        return web.Response(body=data or b"", content_type="image/jpeg")

    @admin_only
    async def admin_mod_action(self, request):
        mid, action = self._mod_id(request), request.match_info["action"]
        if mid is None or discovery.owner_of(self.store, mid) is None:
            return web.json_response({"ok": False, "error": "No such mod."}, status=404)
        if action == "delete":
            await asyncio.get_running_loop().run_in_executor(None, discovery.delete, self.store, mid)
        elif action in ("hide", "show"):
            discovery.set_hidden(self.store, mid, action == "hide")
        else:
            return web.json_response({"ok": False}, status=400)
        gs.log("admin: mod %d %s" % (mid, action))
        return web.json_response({"ok": True})

    @admin_only
    async def dashboard(self, request):
        page = self.dashboard_html
        if "admin_session" in request:  # (signed in under /svr-admin/: its CSRF token for the page's POSTs)
            page = page.replace("<head>", '<head>\n<meta name="csrf-token" content="%s">' %
                                request["admin_session"]["csrf"], 1)
        return web.Response(text=page, content_type="text/html", charset="utf-8", headers=ADMIN_HEADERS)

    async def admin_slash(self, request):
        raise web.HTTPMovedPermanently(aa.PREFIX + "/")

    async def admin_login_page(self, request):
        if self.admin_auth.session(request):
            raise web.HTTPSeeOther(aa.PREFIX + "/")
        return web.Response(text=self.admin_auth.login_page(request.query.get("e", "")), content_type="text/html",
                            charset="utf-8", headers=ADMIN_HEADERS)

    async def admin_login(self, request):
        """The sign-in form: a session cookie and on to the dashboard, or back to the form."""
        ip = self.client_ip(request)
        try:
            form = await request.post()
        except ValueError:
            form = {}
        name, password = str(form.get("name", ""))[:64], str(form.get("password", ""))
        who, why = await asyncio.get_running_loop().run_in_executor(None, self.admin_auth.check, ip, name, password)
        if not who:
            raise web.HTTPSeeOther(aa.PREFIX + "/login?e=" + why, headers=ADMIN_HEADERS)
        token = self.admin_auth.start(ip, who)
        raise web.HTTPSeeOther(aa.PREFIX + "/", headers=dict(ADMIN_HEADERS, **{"Set-Cookie": aa.AdminAuth.cookie(token)}))

    async def admin_logout(self, request):
        s = self.admin_auth.session(request)
        if s and not self.admin_auth.csrf_ok(request, s):
            return web.json_response({"ok": False, "error": "Reload the page."}, status=403)
        self.admin_auth.end(request)
        raise web.HTTPSeeOther(aa.PREFIX + "/login", headers={"Set-Cookie": aa.AdminAuth.cookie("", 0)})

    @admin_only
    async def admin_stats(self, request):
        ranges = {"hour": (3600, 60), "day": (86400, 96), "week": (7 * 86400, 84), "month": (30 * 86400, 90)}
        span, buckets = ranges.get(request.query.get("range", "day"), ranges["day"])
        step, history = self.stats.history(span, buckets)
        files, raw, stored = self.store.stats()
        db_bytes = sum(p.stat().st_size for p in self.data_dir.glob("community.db*"))
        with self.store.lock:
            logos = self.store.db.execute("SELECT COUNT(*), COALESCE(SUM(b.stored), 0) FROM logos l "
                                          "JOIN blobs b ON b.sha = l.sha").fetchone()
        accounts = self.accounts.all()
        records = sum(len(t) for name, t in self.store.records.items() if name == "UserContent")
        with self.store.lock:
            downloads = sum(f[1] or 0 for f in self.store.files.values())
        today = time.mktime(time.strptime(time.strftime("%Y-%m-%d"), "%Y-%m-%d"))
        return web.json_response({
            "now": {
                "connections": self.stats.active, "max_connections": self.max_connections,
                "peak_since_start": self.stats.max_peak,
                "players_online": self.stats.players_online(),
                "online_mode": len(self.in_online_mode()),
                "in_session": self.in_session(),
                "game_running": self.stats.running_now(),
                "bytes_in": self.stats.total_in, "bytes_out": self.stats.total_out,
                "since": self.stats.started,
                "relay_games": len(self.relay.games), "relayed_packets": self.relay.relayed,
                "relay_sessions": self.relay.sessions,
            },
            "totals": {"all": self.stats.totals(), "today": self.stats.totals(today),
                       "file_downloads": downloads},
            "players": {
                "accounts": len(accounts),
                "banned": sum(1 for a in accounts if a["banned"]),
                "today": self.stats.unique_players(0), "week": self.stats.unique_players(7),
                "month": self.stats.unique_players(30),
            },
            "storage": {
                "files": files, "raw": raw, "stored": stored, "database": db_bytes,
                "logos": logos[0], "logo_bytes": logos[1], "uploads": records,
                "free": self._free_space(),
            },
            "history": {"step": step, "rows": history,
                        "columns": ["t"] + list(Stats.COLUMNS)},
            "net": self.netreport_summary(),
        })

    def _free_space(self):
        try:
            import shutil
            return shutil.disk_usage(self.data_dir).free
        except OSError:
            return 0

    @admin_only
    async def admin_uploads(self, request):
        out = []
        names = {a["profileid"]: a["name"] for a in self.accounts.all()}
        for rec in self.store.all("UserContent"):
            f = rec["fields"]
            fid = int((f.get("F00") or [None, 0])[1] or 0)
            size, downloads = self.store.file_info(fid)
            out.append({
                "id": rec["recordid"], "title": (f.get("Title") or [None, ""])[1],
                "type": (f.get("ContentType") or [None, -1])[1],
                "author": names.get(rec["ownerid"], "profile %d" % rec["ownerid"]),
                "size": size, "downloads": downloads, "created": rec["created"],
                "hidden": int((f.get("Moderated") or [None, 0])[1] or 0) != 0,
                "picture": int((f.get("ThumbSize") or [None, 0])[1] or 0) > 16,
                "deleted": int((f.get("Deleted") or [None, 0])[1] or 0) != 0,
            })
        out.sort(key=lambda r: -r["created"])
        return web.json_response(out)

    @staticmethod
    def thumb_blob(rec):
        """The picture the game keeps with an upload (ThumbData0, 1 ...)."""
        out = b""
        for k in range(16):
            v = rec["fields"].get("ThumbData%d" % k)
            if not v or v[0] != "binaryDataValue":
                break
            out += base64.b64decode(v[1] or "")
        return out

    @admin_only
    async def admin_upload_thumb(self, request):
        rec = self.store.get("UserContent", int(request.match_info["id"]))
        if not rec:
            return web.Response(status=404)
        data = await asyncio.get_running_loop().run_in_executor(None, thumbs.thumb_png, self.thumb_blob(rec))
        if not data:
            return web.Response(status=404, text="No picture.")
        return web.Response(body=data, content_type="image/png", headers={"Cache-Control": "private, max-age=3600"})

    @admin_only
    async def admin_upload_full(self, request):
        """The largest picture there is: a Paint Tool logo's own 256 x 256
        (from its file); others have only the game's thumbnail."""
        rec = self.store.get("UserContent", int(request.match_info["id"]))
        if not rec:
            return web.Response(status=404)
        loop = asyncio.get_running_loop()
        data = None
        f = rec["fields"]
        if int((f.get("ContentType") or [None, -1])[1] or -1) == 3:
            fid = int((f.get("F00") or [None, 0])[1] or 0)
            file_data = await loop.run_in_executor(None, lambda: self.store.read_file(fid, count=False))
            if file_data:
                data = await loop.run_in_executor(None, thumbs.logo_png, file_data)
        if not data:
            data = await loop.run_in_executor(None, thumbs.thumb_png, self.thumb_blob(rec))
        if not data:
            return web.Response(status=404, text="No picture.")
        return web.Response(body=data, content_type="image/png", headers={"Cache-Control": "private, max-age=3600"})

    @admin_only
    async def admin_leaderboard(self, request):
        lb = self.leaderboards
        vid = int(request.query.get("view", "1") or 1)
        count = max(1, min(500, int(request.query.get("count", "100") or 100)))
        loop = asyncio.get_running_loop()
        names = await loop.run_in_executor(None, self.lb_names)
        rows = await loop.run_in_executor(None, lb.page_by_rank, vid, 1, count)
        views = [{"id": k, "name": v["name"], "label": self.view_label(k), "weekly": v["reset"] == "Weekly", "rows": 0}
                 for k, v in sorted(lb.views.items())]
        for v in views:
            v["rows"] = await loop.run_in_executor(None, lb.total, v["id"])
        return web.json_response({"view": vid, "total": await loop.run_in_executor(None, lb.total, vid),
                                  "views": views, "rows": [self.lb_row(r, names) for r in rows]})

    @admin_only
    async def admin_upload_action(self, request):
        rid, action = int(request.match_info["id"]), request.match_info["action"]
        if not self.store.get("UserContent", rid):
            return web.json_response({"ok": False, "error": "No such upload."}, status=404)
        if action == "delete":
            self.store.delete("UserContent", rid)
        elif action in ("hide", "show"):
            self.store.update("UserContent", rid, {"Moderated": ["intValue", 1 if action == "hide" else 0]})
        else:
            return web.json_response({"ok": False}, status=400)
        gs.log("admin: upload %d %s" % (rid, action))
        return web.json_response({"ok": True})

    # The dashboard's ping viewer and match history. A ranked match is one stats
    # write of the player's own game (deltas per view: WIN / LOSS / DRAW / DQ /
    # DISCONNECT, the RATED change); its ping is that match's netreport (each game
    # sends one a match). Player Matches write no stats: not in the history yet.
    RESULT_COLUMNS = ("WIN", "LOSS", "DRAW", "DQ", "DISCONNECT")
    NET_KEEP_DAYS = 90

    def record_match(self, account_id, views):
        lb_views = self.leaderboards.views
        best = None  # (view, result, rated): the match type's own view before the totals
        for v in views if isinstance(views, list) else []:
            try:
                vid = int(v.get("view", 0))
            except (TypeError, ValueError, AttributeError):
                continue
            spec = lb_views.get(vid)
            if not spec:
                continue
            result, rated = None, 0
            for prop in v.get("props", []) if isinstance(v.get("props"), list) else []:
                try:
                    col = spec["columns"].get(int(prop[0]))
                    value = prop[2]
                except (TypeError, ValueError, IndexError):
                    continue
                if not col or not isinstance(value, (int, float)):
                    continue
                if col[0] == 65534:
                    rated = int(value)
                elif col[2] in self.RESULT_COLUMNS and value > 0 and not result:
                    result = col[2].lower()
            if not result:
                continue
            total = any(w in spec.get("name", "") for w in ("TOTAL", "PRESTIGE", "GAMESPY", "WEEKLY"))
            if best is None or (best[3] and not total):
                best = (vid, result, rated, total)
        if not best:
            return
        with self.store.lock:
            self.store.db.execute("INSERT INTO match_rows VALUES (?, ?, ?, ?, ?)",
                                  (account_id, now(), best[0], best[1], best[2]))
            cutoff = now() - self.NET_KEEP_DAYS * 86400
            self.store.db.execute("DELETE FROM match_rows WHERE at < ?", (cutoff,))
            self.store.db.execute("DELETE FROM net_events WHERE at < ?", (cutoff,))
            self.store.db.commit()

    def view_label(self, vid):
        """A leaderboard view's name as the game's LEADERBOARDS show it (match_names.py)."""
        try:
            vid = int(vid)
        except (TypeError, ValueError):
            return "view ?"
        name = match_names.view_name(vid)
        if name:
            return name
        name = self.leaderboards.views.get(vid, {}).get("name", "view %d" % vid)
        return re.sub(r"^LB_\d+_", "", name).replace("_RANKING", "").replace("_", " ")

    def relay_links(self):
        """Account name -> how its game is on the relay now (https / UDP / UDP at the edge)."""
        out = {}
        r = self.relay
        for g in list(r.games.values()):
            out[g.account["name"]] = "https"
        for g in list(getattr(r, "udp_games", {}).values()):
            out[g.account["name"]] = "UDP"
        for row in r.rows():
            if row["link"] == "UDP (edge)" and row["known"]:
                out[row["name"]] = "UDP (edge)"
        return out

    @admin_only
    async def admin_pings(self, request):
        """Players online now (presence) with their last match's network report (sent when a match ends), what
        they're in now (the hosts' live reports: session) and their relay link."""
        t = now()
        cutoff = t - SETTINGS["presence_window"]
        ids = [a for a, at in list(self.stats.recent.items()) if at >= cutoff]
        links = self.relay_links()
        # (a game sends its match report once a match ENDS: a player in a match now shows it from the hosts'
        # live session reports - in a match or a lobby, the path the host sees, since when)
        sessions = {}
        for v in list(self.live.values()):
            if t - v["seen"] > LIVE_LIFE:
                continue
            for p in v["players"]:
                key = p["name"].lower()
                if key and (key not in sessions or v["state"] == "match"):
                    sessions[key] = {"state": v["state"], "path": p["path"], "since": v["state_since"],
                                     "host": v["host"], "rule": v.get("rule", ""), "rule_name": v.get("rule_name", ""),
                                     "match_kind": v.get("match_kind", "")}

        def rows():
            out = []
            with self.store.lock:
                for aid in ids:
                    a = self.store.db.execute("SELECT name FROM accounts WHERE id = ?", (aid,)).fetchone()
                    if not a:
                        continue
                    e = self.store.db.execute(
                        "SELECT at, path, nat, rtt, network, version FROM net_events WHERE account = ? "
                        "ORDER BY at DESC LIMIT 1", (aid,)).fetchone()
                    out.append({"name": a[0], "seen": self.stats.recent.get(aid, 0), "link": links.get(a[0], ""),
                                "session": sessions.get(a[0].lower()),
                                "place": self.stats.places.get(aid, ("", ""))[0],
                                "kind": self.stats.places.get(aid, ("", ""))[1],
                                "online_mode": aid in self.stats.online_mode and
                                self.stats.online_mode[aid] >= cutoff,
                                "last": dict(zip(("at", "path", "nat", "rtt", "network", "version"), e)) if e else None})
            return sorted(out, key=lambda r: r["name"].lower())

        return web.json_response({"now": t, "place_names": PLACE_NAMES,
                                  "players": await asyncio.get_running_loop().run_in_executor(None, rows)})

    @admin_only
    async def admin_player(self, request):
        """?name=<online name>: wins, losses, win rate and the recent matches (with their ping)."""
        name = request.query.get("name", "").strip()[:40]

        def look():
            with self.store.lock:
                a = self.store.db.execute("SELECT id, name, created, last_seen FROM accounts WHERE lower(name) = lower(?)",
                                          (name,)).fetchone()
                if not a:
                    return None
                aid = a[0]
                totals = dict(self.store.db.execute(
                    "SELECT result, COUNT(*) FROM match_rows WHERE account = ? GROUP BY result", (aid,)).fetchall())
                matches = self.store.db.execute(
                    "SELECT at, view, result, rated FROM match_rows WHERE account = ? ORDER BY at DESC LIMIT 50",
                    (aid,)).fetchall()
                events = self.store.db.execute(
                    "SELECT at, path, nat, rtt, network, version FROM net_events WHERE account = ? "
                    "ORDER BY at DESC LIMIT 200", (aid,)).fetchall()
                lb = self.store.db.execute("SELECT rating, columns FROM lb_rows WHERE view = 1 AND account = ?",
                                           (aid,)).fetchone()
            out_matches = []
            for at, vid, result, rated in matches:
                near = min(events, key=lambda e: abs(e[0] - at), default=None)
                net = dict(zip(("at", "path", "nat", "rtt", "network", "version"), near)) \
                    if near and abs(near[0] - at) <= 300 else None
                out_matches.append({"at": at, "type": self.view_label(vid), "view": vid, "result": result, "rated": rated,
                                    "net": net})
            rtts = [e[3] for e in events if e[3]]
            wins, losses = totals.get("win", 0), totals.get("loss", 0)
            decided = wins + losses
            return {"name": a[1], "created": a[2], "last_seen": a[3], "totals": totals,
                    "win_rate": round(100.0 * wins / decided, 1) if decided else None,
                    "rating": lb[0] if lb else None,
                    "ping": {"matches": len(rtts), "avg": round(sum(rtts) / len(rtts)) if rtts else None,
                             "max": max(rtts) if rtts else None},
                    "paths": {p: sum(1 for e in events if e[1] == p) for p in {e[1] for e in events}},
                    "matches": out_matches, "recent_net": [
                        dict(zip(("at", "path", "nat", "rtt", "network", "version"), e)) for e in events[:20]]}

        if not name:
            return web.json_response({"ok": False, "error": "Give a name."}, status=400)
        res = await asyncio.get_running_loop().run_in_executor(None, look)
        if res is None:
            return web.json_response({"ok": False, "error": "No player called %s." % name}, status=404)
        return web.json_response(dict(ok=True, **res))

    @admin_only
    async def admin_live(self, request):
        """The sessions hosts reported in the last LIVE_LIFE s, and the relay's games now."""
        t = now()
        for k in [k for k, v in self.live.items() if t - v["seen"] > LIVE_LIFE]:
            del self.live[k]
        sessions = sorted(self.live.values(), key=lambda v: -v["since"])
        relay = [{"name": g.account["name"], "since": getattr(g, "since", 0)} for g in self.relay.games.values()]
        games = self.relay.rows()
        # (a host's peer without a name - "?" - named from the relay's account for its id, 2.1.6 hosts on)
        named = []
        for v in sessions:
            players = []
            for p in v["players"]:
                p = dict(p)
                aid = self.relay.account_of(p["id"]) if p.get("id") else 0
                if aid:
                    p["account"] = aid
                    if not p["name"] or p["name"] == "?":
                        p["name"], p["named_by"] = self.relay.name_of(aid) or "?", "relay"
                players.append(p)
            named.append(dict(v, players=players))
        hosts = {v["host"].lower(): v.get("version", "") for v in sessions}

        def versions():
            # (a game's version: its last match report's, else the session it hosts now; banned: the Ban button's state)
            out = {}
            with self.store.lock:
                for r in games:
                    if not r["account"] or r["account"] in out:
                        continue
                    e = self.store.db.execute("SELECT version FROM net_events WHERE account = ? AND version != '' "
                                              "ORDER BY at DESC LIMIT 1", (r["account"],)).fetchone()
                    b = self.store.db.execute("SELECT banned FROM accounts WHERE id = ?", (r["account"],)).fetchone()
                    out[r["account"]] = (e[0] if e else "", bool(b and b[0]))
            return out
        known = await asyncio.get_running_loop().run_in_executor(None, versions)
        for r in games:
            r["version"], r["banned"] = known.get(r["account"], ("", False))
            r["version"] = r["version"] or hosts.get(r["name"].lower(), "")
        return web.json_response({"now": t, "sessions": named, "relay": relay, "relay_games": games,
                                  "udp_games": len(getattr(self.relay, "udp_games", {})),
                                  "match_network": SETTINGS["match_network"]})

    @admin_only
    async def admin_log(self, request):
        """The end of the server's log (--log), filtered: ?filter=<text>&lines=<n>."""
        if not self.log_path:
            return web.json_response({"ok": False, "error": "This server writes no log file (--log)."}, status=404)
        want = max(10, min(int(request.query.get("lines", "300") or 300), 3000))
        needle = request.query.get("filter", "").lower()

        def tail():
            with open(self.log_path, "rb") as f:
                f.seek(0, 2)
                size = f.tell()
                chunk = 4 << 20 if needle else 512 << 10  # (a filter looks further back)
                f.seek(max(0, size - chunk))
                lines = f.read().decode("utf-8", "replace").splitlines()[1:]
            if needle:
                lines = [ln for ln in lines if needle in ln.lower()]
            return lines[-want:], size

        lines, size = await asyncio.get_running_loop().run_in_executor(None, tail)
        return web.json_response({"ok": True, "lines": lines, "size": size})

    @admin_only
    async def admin_settings(self, request):
        """GET: the settings and what they mean; POST {key: value}: saved to data/settings.json,
        applied at once where they're live (the rest at the next start)."""
        if request.method == "POST":
            try:
                body = await request.json()
            except ValueError:
                body = None
            if not isinstance(body, dict):
                return web.json_response({"ok": False, "error": "Send a JSON object."}, status=400)
            changed = {}
            for k, v in body.items():
                ok, value = check_setting(k, v)
                if not ok:
                    return web.json_response({"ok": False, "error": "%s: not a valid value." % k}, status=400)
                changed[k] = value
            try:
                stored = json.load(open(self.settings_path, encoding="utf-8"))
            except (OSError, ValueError):
                stored = {}
            stored.update(changed)
            tmp = str(self.settings_path) + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(stored, f, indent=1, sort_keys=True)
            os.replace(tmp, self.settings_path)
            for k, v in changed.items():
                if SETTINGS_SPEC[k]["live"]:
                    SETTINGS[k] = v
            gs.log("settings: %s (dashboard)" % ", ".join(
                "%s=%s" % (k, (repr(v[:60] + ("..." if len(v) > 60 else "")) + " (%d characters)" % len(v))
                           if isinstance(v, str) and SETTINGS_SPEC[k].get("text") else v)
                for k, v in sorted(changed.items())))
        try:
            stored = json.load(open(self.settings_path, encoding="utf-8"))
        except (OSError, ValueError):
            stored = {}
        return web.json_response({"ok": True, "supervised": self.supervised, "settings": [
            dict(key=k, value=SETTINGS[k], saved=stored.get(k, SETTINGS[k]), **{
                n: v for n, v in spec.items()}) for k, spec in SETTINGS_SPEC.items()]})

    @admin_only
    async def admin_restart(self, request):
        """The "Restart online" button: a clean exit the supervisor (--supervise) starts again."""
        if not self.supervised:
            return web.json_response({"ok": False, "error": "No supervisor runs this server (--supervise): "
                                                            "it would stay down."}, status=409)
        gs.log("restart: asked from the dashboard")

        def bye():
            gs.log("stopped %s: restart from the dashboard" % time.strftime("%Y-%m-%d %H:%M:%S"))
            os._exit(RESTART_CODE)

        asyncio.get_running_loop().call_later(0.5, bye)
        return web.json_response({"ok": True})

    @admin_only
    async def admin_users(self, request):
        out = []
        uploads = collections.Counter(r["ownerid"] for r in self.store.all("UserContent"))
        for a in self.accounts.all():
            out.append(dict(a, activated=self.signup.activated(a["id"]), uploads=uploads.get(a["profileid"], 0),
                            storage=self.store.owner_bytes(a["profileid"]),
                            online=a["id"] in self.stats.recent))
        return web.json_response(out)

    @admin_only
    async def admin_user_action(self, request):
        aid, action = int(request.match_info["id"]), request.match_info["action"]
        if not self.accounts.get(aid):
            return web.json_response({"ok": False, "error": "No such account."}, status=404)
        result = {"ok": True}
        if action == "ban":
            self.accounts.set_banned(aid, True)
            self.relay.ban(aid)  # (its relay streams, UDP and the edge's: not only its sign-ins)
        elif action == "unban":
            self.accounts.set_banned(aid, False)
            self.relay.unban(aid)
        elif action == "reset":
            result["password"] = self.accounts.reset_password(aid)
        elif action == "delete":
            result["removed"] = self.accounts.delete(aid)
            self.relay.ban(aid, "deleted")
        elif action == "reactivation-code":  # (shown once: only its hash is kept)
            result["id"], result["code"] = self.signup.make_code("reactivate", "", aid)
        elif action == "activate":  # (no code: the server owner's own say)
            self.signup.set_active(aid)
        elif action == "reset-code":  # (a forgotten password: the player types it in the launcher; 24 h, once)
            result["id"], result["code"] = self.signup.make_code("reset", "", aid)
        else:
            return web.json_response({"ok": False}, status=400)
        gs.log("admin: account %d %s" % (aid, action))
        return web.json_response(result)

    @admin_only
    async def admin_invites(self, request):
        if request.method == "POST":  # (new sign-up codes: shown once)
            try:
                body = await request.json()
            except ValueError:
                body = {}
            n = max(1, min(int(body.get("count", 1) or 1), 50))
            note = str(body.get("note", ""))
            codes = [self.signup.make_code("signup", note) for _ in range(n)]
            gs.log("admin: %d invite code(s) made%s" % (n, " (%s)" % note[:40] if note else ""))
            return web.json_response({"ok": True, "codes": [{"id": i, "code": c} for i, c in codes]})
        return web.json_response({"ok": True, "invites": self.signup.invites()})

    @admin_only
    async def admin_checks(self, request):
        """The account checks' numbers: how many accounts account_checks on would keep off online now."""
        blocked, total = await asyncio.get_running_loop().run_in_executor(None, self.signup.blocked_count)
        return web.json_response({"ok": True, "checks": SETTINGS["account_checks"], "would_block": blocked,
                                  "accounts": total, "test_open": self.signup.open})

    @admin_only
    async def admin_invite_revoke(self, request):
        ok = self.signup.revoke(int(request.match_info["id"]))
        gs.log("admin: invite %s revoked" % request.match_info["id"])
        return web.json_response({"ok": ok})

    async def health(self, request):
        return web.json_response({"ok": True})

    # -- the storage node (the user's PC behind the VPS): its state, Stop / Start ----------------

    def node_save(self):
        tmp = self.node_state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.node_state, f)
        os.replace(tmp, self.node_state_path)

    def node_call(self, method, path, timeout=6):
        """The node's answer (status, JSON or None); (0, None) when it doesn't answer."""
        import urllib.error
        import urllib.request
        r = urllib.request.Request(self.node_url + path, method=method, data=b"" if method == "POST" else None,
                                   headers={"X-Storage-Key": self.node_key})
        try:
            with urllib.request.urlopen(r, timeout=timeout) as f:
                return f.status, json.loads(f.read() or b"null")
        except urllib.error.HTTPError as e:
            return e.code, None
        except Exception:
            return 0, None

    @admin_only
    async def admin_storage(self, request):
        """GET: the node's state for the dashboard's panel. POST {"action": "stop" | "start"}: stop it (the core
        tells it - it stays stopped) or want it running (the PC's watchdog asks for that and starts it)."""
        if not self.node_url:
            return web.json_response({"ok": True, "node": False})
        loop = asyncio.get_running_loop()
        if request.method == "POST":
            try:
                body = await request.json()
            except ValueError:
                body = {}
            action = body.get("action")
            if action not in ("stop", "start"):
                return web.json_response({"ok": False, "error": "stop or start"}, status=400)
            self.node_state.update(wanted="stopped" if action == "stop" else "running", changed=now())
            self.node_save()
            gs.log("storage node: %s asked from the dashboard" % action)
            if action == "stop":
                status, _ = await loop.run_in_executor(None, self.node_call, "POST", "/__stop")
                if status != 200:  # (not answering: the watchdog stops it when it asks next)
                    gs.log("storage node: it didn't take the stop now (%s) - the watchdog stops it" % (status or
                                                                                                        "no answer"))
        status, info = await loop.run_in_executor(None, self.node_call, "GET", "/__health")
        up = status == 200 and bool(info and info.get("ok"))
        if up:
            self.node_state["answered"] = now()
        return web.json_response({"ok": True, "node": True, "up": up, "info": info if up else None,
                                  "wanted": self.node_state["wanted"], "changed": self.node_state.get("changed"),
                                  "answered": self.node_state.get("answered"), "watchdog_polled": self.node_polled or None,
                                  "now": now()})

    async def api_storage_wanted(self, request):
        """The PC's watchdog (pc_watchdog.ps1, every 2 minutes, with the node's key): running or stopped."""
        if not self.node_url or not hmac.compare_digest(request.headers.get("X-Storage-Key", ""), self.node_key):
            return web.Response(status=403)
        self.node_polled = now()
        return web.json_response({"wanted": self.node_state["wanted"]})

    def health_line(self):
        r = self.relay
        return ("health: %s requests running %d of %d, relay streams %d, UDP games %d (edge %d), threads %d, "
                "memory %s MB, players online %d" % (
                    time.strftime("%Y-%m-%d %H:%M:%S"), self.stats.active, self.max_connections, len(r.games),
                    len(getattr(r, "udp_games", {})), len(getattr(r, "edge_games", ())), threading.active_count(),
                    memory_mb(), self.stats.playing_now()))

    # background

    async def background(self, app):
        async def minutes():
            while True:
                await asyncio.sleep(60 - now() % 60 + 0.5)
                try:
                    self.stats.flush()
                except Exception:
                    gs.log("error:", traceback.format_exc())

        async def janitor():
            while True:
                await asyncio.sleep(3600)
                try:
                    await asyncio.get_running_loop().run_in_executor(None, self.store.collect_garbage)
                except Exception:
                    gs.log("error:", traceback.format_exc())

        async def health():  # (one line every 5 minutes: what a hang or a crash looked like before)
            while True:
                await asyncio.sleep(float(os.environ.get("SVR2011_HEALTH_EVERY", 300)))  # (tests: shorter)
                try:
                    gs.log(self.health_line())
                except Exception:
                    gs.log("error:", traceback.format_exc())

        loop = asyncio.get_running_loop()
        loop.set_exception_handler(lambda lp, ctx: gs.log(
            "error: unhandled in the event loop: %s\n%s" % (ctx.get("message"), "".join(traceback.format_exception(
                ctx["exception"])) if ctx.get("exception") else "")))
        Watchdog(loop).start()
        tasks = [asyncio.create_task(minutes()), asyncio.create_task(janitor()), asyncio.create_task(health())]
        yield
        for t in tasks:
            t.cancel()

    def build(self):
        app = web.Application(middlewares=[self.meter],
                              client_max_size=max([self.store.max_file] + list(self.max_media.values())) + (1 << 20))
        b = self.base
        app.router.add_post(b + "/api/register", self.api_register)
        app.router.add_post(b + "/api/login", self.api_login)
        app.router.add_post(b + "/api/logout", self.api_logout)
        app.router.add_get(b + "/api/account", self.api_account)
        app.router.add_post(b + "/api/reactivate", self.api_reactivate)
        app.router.add_post(b + "/api/password/reset", self.api_password_reset)
        app.router.add_post(b + "/api/password/change", self.api_password_change)
        app.router.add_route("*", b + "/api/session", self.api_session)
        app.router.add_get(b + "/api/status", self.api_status)
        app.router.add_get(b + "/api/friends", self.api_friends)
        app.router.add_post(b + "/api/friends", self.api_friends)
        app.router.add_post(b + "/api/invite", self.api_invite)
        app.router.add_post(b + "/api/live", self.api_live)
        app.router.add_post(b + "/api/stats/{what}", self.api_stats)
        app.router.add_post(b + "/api/netreport", self.api_netreport)
        app.router.add_route("*", b + "/api/logo/{hash}", self.api_logo)
        app.router.add_get(b + "/api/logos/wanted", self.api_logos_wanted)
        app.router.add_post(b + "/api/media", self.api_media_post)
        app.router.add_get(b + "/api/media/{sha}", self.api_media_get)  # (and HEAD)
        app.router.add_route("*", b + "/api/entrance/{fileid}", self.api_entrance)
        app.router.add_route("*", b + "/game/{tail:.*}", self.game)
        app.router.add_get(b + "/api/mods", self.api_mods)
        app.router.add_post(b + "/api/mods", self.api_mod_upload)
        app.router.add_route("*", b + "/api/mods/{id}", self.api_mod)  # (GET; PATCH / DELETE: its owner)
        app.router.add_get(b + "/api/mods/{id}/file", self.api_mod_file)
        app.router.add_put(b + "/api/mods/{id}/file", self.api_mod_update_file)  # (an update: its owner, or the node's key)
        app.router.add_put(b + "/api/mods/{id}/pictures/{n}", self.api_mod_update_picture)  # (its owner)
        app.router.add_get(b + "/api/mods/{id}/img/{n}", self.api_mod_image)
        app.router.add_post(b + "/api/reports", self.api_report)
        app.router.add_post(b + "/api/reports/mods/check", self.api_report_mods_check)
        app.router.add_post(b + "/api/reports/status", self.api_report_status)
        app.router.add_post(b + "/api/reports/reply", self.api_report_reply)
        app.router.add_post(b + "/api/reports/edit", self.api_report_edit)
        self.relay.routes(app, b)
        app.router.add_get("/__health", self.health)
        app.router.add_get(b + "/api/storage/wanted", self.api_storage_wanted)  # (the storage node's watchdog)
        # the dashboard: at / (the portal, --local-admin) and again under /svr-admin/ (its own
        # sign-in: admin_auth.py - what nginx passes on the VPS)
        app.router.add_get(aa.PREFIX, self.admin_slash)
        app.router.add_get(aa.PREFIX + "/login", self.admin_login_page)
        app.router.add_post(aa.PREFIX + "/login", self.admin_login)
        app.router.add_post(aa.PREFIX + "/logout", self.admin_logout)
        for p in ("", aa.PREFIX):
            app.router.add_get(p + "/", self.dashboard)
            app.router.add_get(p + "/api/admin/stats", self.admin_stats)
            app.router.add_get(p + "/api/admin/uploads", self.admin_uploads)
            app.router.add_get(p + "/api/admin/upload/{id}/thumb.png", self.admin_upload_thumb)
            app.router.add_get(p + "/api/admin/upload/{id}/full.png", self.admin_upload_full)
            app.router.add_get(p + "/api/admin/leaderboard", self.admin_leaderboard)
            app.router.add_post(p + "/api/admin/upload/{id}/{action}", self.admin_upload_action)
            app.router.add_get(p + "/api/admin/mods", self.admin_mods)
            app.router.add_get(p + "/api/admin/blobs/audit", self.admin_blob_audit)
            app.router.add_post(p + "/api/admin/blobs/{sha}/delete", self.admin_blob_delete)
            app.router.add_get(p + "/api/admin/mod/{id}/image.jpg", self.admin_mod_image)
            app.router.add_post(p + "/api/admin/mod/{id}/{action}", self.admin_mod_action)
            app.router.add_put(p + "/api/admin/mods/{id}/file", self.admin_mod_update_file)  # (a new version, owner kept)
            app.router.add_get(p + "/api/admin/users", self.admin_users)
            app.router.add_get(p + "/api/admin/reports", self.admin_reports)
            app.router.add_get(p + "/api/admin/report/{id}/replay.zip", self.admin_report_replay)
            app.router.add_get(p + "/api/admin/report-mod/{hash}", self.admin_report_mod)
            app.router.add_get(p + "/api/admin/report/{id}/{name}", self.admin_report_file)
            app.router.add_post(p + "/api/admin/report/{id}/mark", self.admin_report_mark)
            app.router.add_post(p + "/api/admin/report/{id}/delete", self.admin_report_delete)
            app.router.add_get(p + "/api/admin/live", self.admin_live)
            app.router.add_get(p + "/api/admin/pings", self.admin_pings)
            app.router.add_get(p + "/api/admin/player", self.admin_player)
            app.router.add_get(p + "/api/admin/log", self.admin_log)
            app.router.add_route("*", p + "/api/admin/settings", self.admin_settings)
            app.router.add_post(p + "/api/admin/restart", self.admin_restart)
            app.router.add_route("*", p + "/api/admin/storage", self.admin_storage)
            app.router.add_post(p + "/api/admin/user/{id}/{action}", self.admin_user_action)
            app.router.add_route("*", p + "/api/admin/invites", self.admin_invites)
            app.router.add_post(p + "/api/admin/invite/{id}/revoke", self.admin_invite_revoke)
            app.router.add_get(p + "/api/admin/checks", self.admin_checks)
            app.router.add_route("*", p + "/api/admin/names", self.admin_names)  # (GET; POST: save)
            app.router.add_post(p + "/api/admin/names/test", self.admin_names_test)
            app.router.add_get(p + "/api/admin/names/flagged", self.admin_names_flagged)
        app.cleanup_ctx.append(self.background)
        return app


def read_key(path):
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def storage_app(args):
    """--role storage: only the Community Creations files' bytes (the user's PC behind the VPS):
    GET / HEAD / PUT / DELETE /blob/<sha>?packed=0|1 with the core's key (X-Storage-Key), and /__health."""
    blobs = gs.LocalBlobs(os.path.join(args.data, "blobs"))
    key = read_key(args.storage_key)

    def ok(request):
        return hmac.compare_digest(request.headers.get("X-Storage-Key", ""), key)

    async def blob(request):
        if not ok(request):
            return web.Response(status=403)
        sha = request.match_info["sha"]
        if not re.fullmatch(r"[0-9a-f]{64}", sha):
            return web.Response(status=400)
        loop = asyncio.get_running_loop()
        if request.method == "PUT":
            data = await request.read()
            if not data:
                return web.Response(status=400)
            await loop.run_in_executor(None, blobs.put, sha, data, request.query.get("packed") != "0")
            return web.Response(text="ok")
        if request.method == "DELETE":
            await loop.run_in_executor(None, blobs.delete, sha)
            return web.Response(text="ok")
        if request.method == "HEAD":
            return web.Response(status=200 if blobs.exists(sha) else 404)
        for packed in (False, True):  # (sent from the disk as it is read: a big mod starts at once)
            path = blobs.path(sha, packed)
            if os.path.exists(path):
                return web.FileResponse(path, chunk_size=1 << 18, headers={
                    "X-Packed": "1" if packed else "0", "Content-Type": "application/octet-stream"})
        return web.Response(status=404)

    started = time.time()
    count = {"at": 0, "n": 0}

    def blob_count():  # (the blobs folder's files, counted at most once a minute)
        if time.time() - count["at"] > 60:
            try:
                count["n"] = sum(1 for d in os.scandir(blobs.folder) if d.is_dir()
                                 for e in os.scandir(d.path) if e.is_file())
            except OSError:
                pass
            count["at"] = time.time()
        return count["n"]

    async def health(request):
        out = {"ok": True, "role": "storage"}
        if ok(request):  # (the core asking: the details for the dashboard's panel)
            try:
                du = shutil.disk_usage(args.data)
                out.update(free_bytes=du.free, total_bytes=du.total)
            except OSError:
                pass
            out.update(up_s=int(time.time() - started), pid=os.getpid(), supervised=bool(os.environ.get(
                "SVR2011_SUPERVISOR_PID")), blobs=await asyncio.get_running_loop().run_in_executor(None, blob_count))
        return web.json_response(out)

    async def stop(request):  # (the dashboard's Stop: stopped until its Start - the marker, then a clean exit)
        if not ok(request):
            return web.Response(status=403)
        with open(os.path.join(args.data, NODE_STOPPED), "w", encoding="utf-8") as f:
            json.dump({"at": time.time(), "why": "the dashboard's Stop"}, f)
        gs.log("storage node: stop asked from the dashboard - stopped until its Start")

        def bye():
            gs.log("stopped %s: the dashboard's Stop" % time.strftime("%Y-%m-%d %H:%M:%S"))
            os._exit(STOP_CODE)

        asyncio.get_running_loop().call_later(0.5, bye)
        return web.json_response({"ok": True})

    async def blob_list(request):  # (the core's blob audit)
        if not ok(request):
            return web.Response(status=403)
        return web.json_response(await asyncio.get_running_loop().run_in_executor(None, blobs.list))

    app = web.Application(client_max_size=1 << 31)  # (any blob: songs, movies, mods - only the key's holder puts)
    app.router.add_route("*", "/blob/{sha}", blob)
    app.router.add_get("/__health", health)
    app.router.add_post("/__stop", stop)
    app.router.add_get("/blobs", blob_list)
    reports.node_routes(app, args.data, ok, int(args.report_mod_quota_gb * (1 << 30)))  # (problem reports: <data>/reports)
    return app


def supervise(args):
    """--supervise: the server as a child process, started again when it exits - at once after
    the dashboard's restart (exit code RESTART_CODE), else after a backoff (a crash: 2 s,
    doubling to 60 s while it keeps failing within a minute)."""
    import subprocess
    if args.log:
        gs.LOG = open(args.log, "a", encoding="utf-8")
    cmd = [sys.executable, os.path.abspath(__file__)] + [a for a in sys.argv[1:] if a != "--supervise"]
    env = dict(os.environ, SVR2011_SUPERVISOR_PID=str(os.getpid()))
    backoff = 2.0
    while True:
        started = time.time()
        gs.log("supervisor: starting the server %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
        try:
            code = subprocess.call(cmd, env=env)
        except KeyboardInterrupt:
            return
        ran = time.time() - started
        if code == RESTART_CODE:
            gs.log("supervisor: restart asked - starting it again")
            backoff = 2.0
            continue
        if code == STOP_CODE:  # (a storage node stopped from the dashboard: stays stopped)
            gs.log("supervisor: the server stopped on purpose (the dashboard's Stop) - not starting it again")
            return
        gs.log("supervisor: the server exited (code %s) after %.0f s - again in %.0f s" % (code, ran, backoff))
        time.sleep(backoff)
        backoff = 2.0 if ran > 60 else min(backoff * 2, 60.0)


def watch_supervisor():
    """A supervised server ends when its supervisor is gone (the portal stopped it), so it never
    holds the port on its own."""
    pid = int(os.environ.get("SVR2011_SUPERVISOR_PID") or 0)
    if not pid:
        return

    def alive():
        if sys.platform == "win32":
            import ctypes
            k = ctypes.WinDLL("kernel32")
            k.OpenProcess.restype = ctypes.c_void_p
            h = k.OpenProcess(0x00100000, False, pid)  # (SYNCHRONIZE)
            if not h:
                return False
            gone = k.WaitForSingleObject(ctypes.c_void_p(h), 0) == 0
            k.CloseHandle(ctypes.c_void_p(h))
            return not gone
        return os.getppid() == pid

    def watch():
        while True:
            time.sleep(2)
            if not alive():
                gs.log("stopped %s: the supervisor is gone" % time.strftime("%Y-%m-%d %H:%M:%S"))
                os._exit(0)

    threading.Thread(target=watch, name="supervisor watch", daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    # The defaults are the launcher's Custom server on this PC (127.0.0.1:8411);
    # Project Index passes its own (8101, its public mount /svr).
    ap.add_argument("--port", type=int, default=8411)
    ap.add_argument("--data", default=str(HERE / "data"))
    ap.add_argument("--report-mod-quota-gb", type=float, default=20,
                    help="(where reports are kept) the mods sent with problem reports, all together")
    ap.add_argument("--base", default="", help="where the game's half is (Project Index: its public mount)")
    ap.add_argument("--log", default=None, help="also write every event to this file")
    ap.add_argument("--max-connections", type=int, default=100, help="game requests at once")
    ap.add_argument("--max-file-mb", type=int, default=8, help="the largest upload")
    ap.add_argument("--quota-mb", type=int, default=256, help="what one player may store (uncompressed)")
    ap.add_argument("--max-music-mb", type=int, default=512, help="the largest entrance song sent (stored as MP3)")
    ap.add_argument("--max-movie-mb", type=int, default=1024, help="the largest entrance movie sent (MP4; kept 4 min)")
    ap.add_argument("--media-quota-mb", type=int, default=8192, help="entrance songs and movies one player may store")
    ap.add_argument("--relay-udp-port", type=int, default=0,
                    help="the match relay over UDP too (e.g. 36500; open it in the firewall); 0: https only")
    ap.add_argument("--relay-edge", default=None,
                    help="host:port of relay_edge.py's link (it takes the UDP; --relay-udp-port: the players' port there)")
    ap.add_argument("--relay-secret", default=None, help="the file with the secret shared with the edge")
    ap.add_argument("--role", choices=("all", "core", "storage"), default="all",
                    help="all: everything here (today's PC); core: everything but the Community Creations files' "
                         "bytes, which go to --storage-url (the VPS); storage: only those bytes (the PC behind it)")
    ap.add_argument("--storage-url", default=None, help="(core) the storage node, e.g. http://10.8.0.2:8102")
    ap.add_argument("--storage-key", default=None, help="(core, storage) the file with the key they share")
    ap.add_argument("--supervise", action="store_true",
                    help="run the server as a child process and start it again when it exits (a crash, or the "
                         "dashboard's Restart online)")
    ap.add_argument("--local-admin", action="store_true",
                    help="the dashboard for anyone on this PC talking to the service directly (a developer's own "
                         "server; never behind a public proxy on the same host)")
    ap.add_argument("--set-admin-password", action="store_true",
                    help="ask for the dashboard's password (/svr-admin/ sign-in) and store its hash in the data "
                         "folder (or --admin-password-file), then exit")
    ap.add_argument("--admin-password-file", default=None, help="where the dashboard password's hash is "
                    "(default: <data>/admin_password)")
    ap.add_argument("--admin-account", default=None,
                    help="the dashboard's sign-in is this online account's name and password instead (it never "
                         "needs a re-activation code online)")
    ap.add_argument("--test-open-accounts", action="store_true",
                    help="a private TEST server only: sign-up without an invite code, and no re-activation "
                         "code for online play (the accounts' old rules)")
    ap.add_argument("--activate", default=None, metavar="NAME",
                    help="let this account play online without a re-activation code, then exit")
    ap.add_argument("--prune-accounts", type=float, default=None, metavar="DAYS",
                    help="list the accounts made more than DAYS ago and not seen for DAYS (with --yes: delete them, "
                         "as the dashboard's Delete does), then exit - with the server stopped")
    ap.add_argument("--yes", action="store_true", help="(--prune-accounts) really delete")
    args = ap.parse_args()
    if args.set_admin_password:
        return aa.set_password_cli(args.admin_password_file or os.path.join(args.data, "admin_password"))
    if args.test_open_accounts and args.host not in ("127.0.0.1", "::1", "localhost"):
        ap.error("--test-open-accounts is for a private test server: only with --host 127.0.0.1")
    if args.activate and args.role == "storage":
        ap.error("--activate: a storage node has no accounts (run it where the core's data is)")
    if args.activate:
        store = gs.Store(args.data)
        signup.Signup(store, Accounts(store), print)  # (the column, if this database is from before)
        with store.lock:
            n = store.db.execute("UPDATE accounts SET activated = 1 WHERE name = ? COLLATE NOCASE",
                                 (args.activate,)).rowcount
            store.db.commit()
        print("%s: %s" % (args.activate, "active" if n else "no such account"))
        sys.exit(0 if n else 1)
    if args.prune_accounts is not None:
        if args.role == "storage":
            ap.error("--prune-accounts: a storage node has no accounts (run it where the core's data is)")
        store = gs.Store(args.data)
        accounts = Accounts(store)
        leaderboards.Leaderboards(store)  # (its tables: a delete clears the account's rows)
        cut = now() - args.prune_accounts * 86400
        with store.lock:
            rows = store.db.execute("SELECT id, name, created, last_seen, banned FROM accounts WHERE created < ? AND "
                                    "COALESCE(last_seen, 0) < ? ORDER BY last_seen", (cut, cut)).fetchall()
        day = lambda t: time.strftime("%Y-%m-%d", time.localtime(t)) if t else "never"
        for aid, name, created, seen, banned in rows:
            print("%6d  %-16s made %s  last seen %s%s" % (aid, name, day(created), day(seen),
                                                          "  (banned)" if banned else ""))
        print("%d account(s) made more than %g days ago and not seen since" % (len(rows), args.prune_accounts))
        if not args.yes:
            print("(nothing deleted: add --yes to delete them)")
            sys.exit(0)
        records = sum(accounts.delete(aid) for aid, *_ in rows)
        print("deleted %d account(s) and %d Community Creations record(s)" % (len(rows), records))
        sys.exit(0)
    if args.relay_edge and (not args.relay_secret or not args.relay_udp_port):
        ap.error("--relay-edge needs --relay-secret and --relay-udp-port")
    # (a storage node never relays: linked to the edge too, it took half the
    # players' packets from the core - relay_edge.py now keeps one server)
    if args.role == "storage" and (args.relay_edge or args.relay_udp_port):
        ap.error("--role storage doesn't relay: no --relay-edge / --relay-udp-port (the core does)")
    if args.supervise:
        return supervise(args)
    if args.role in ("core", "storage") and not args.storage_key:
        ap.error("--role %s needs --storage-key" % args.role)
    if args.role == "core" and not args.storage_url:
        ap.error("--role core needs --storage-url")
    if args.role == "storage":
        # (only the blobs: no accounts - no Service, so no signup.py migration or backup, no mail.json, no account
        # routes; its data folder's community.db, if one is left there, is never opened)
        if args.log:
            gs.LOG = open(args.log, "a", encoding="utf-8")
        if os.path.exists(os.path.join(args.data, NODE_STOPPED)):  # (stopped from the dashboard: stays so)
            gs.log("storage node: not starting - stopped from the dashboard (%s; its Start removes it)" %
                   os.path.join(args.data, NODE_STOPPED))
            sys.exit(STOP_CODE)
        watch_supervisor()
        gs.log("storage node on %s:%d, blobs in %s" % (args.host, args.port, os.path.abspath(args.data)))
        web.run_app(storage_app(args), host=args.host, port=args.port, print=None, access_log=None)
        return
    if args.log:
        gs.LOG = open(args.log, "a", encoding="utf-8")
    load_settings(os.path.join(args.data, "settings.json"))
    if SETTINGS["max_connections"] != SETTINGS_SPEC["max_connections"]["default"]:
        args.max_connections = SETTINGS["max_connections"]  # (the dashboard's, over the command line's)
    watch_supervisor()
    threading.excepthook = lambda a: gs.log("error: thread %s died:\n%s" % (
        a.thread.name if a.thread else "?", "".join(traceback.format_exception(a.exc_type, a.exc_value, a.exc_traceback))))
    service = Service(args)
    gs.log("started %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    gs.log("Community Creations server on %s:%d (game: %s/, dashboard: /), data in %s, up to %d connections" % (
        args.host, args.port, service.base, os.path.abspath(args.data), args.max_connections))
    try:
        web.run_app(service.build(), host=args.host, port=args.port, print=None, access_log=None)
    except BaseException as e:  # (why it stopped, in its own log - the portal's err.log starts empty each start)
        gs.log("stopped %s: %s\n%s" % (time.strftime("%Y-%m-%d %H:%M:%S"), type(e).__name__, traceback.format_exc()))
        raise
    gs.log("stopped %s" % time.strftime("%Y-%m-%d %H:%M:%S"))


if __name__ == "__main__":
    main()
