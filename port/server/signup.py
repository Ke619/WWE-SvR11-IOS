"""Account security for the Community Creations server (server.py, 2.1.6 on): codes only, no email.

  - Sign-up needs a one-use INVITE code (the dashboard makes them, one per person): a name, a password, the code.
  - Accounts from before 2.1.6 are blocked online until the player enters the RE-ACTIVATION code the server
    owner made for that one account (the dashboard's account row).
  - A forgotten password: the server owner makes a one-use RESET code for that account (the dashboard's account
    row, it works for 24 hours); the player types it in the launcher's Forgot password with a new password.
  - Online play (the game's /api/session, presence, matches, Community Creations) needs an active account (its
    invite or re-activation code used); the launcher's own account requests work before that.
  - Codes are kept only as SHA-256 hashes; each works once, for its own account only (a sign-up code for a new
    account only); a newer re-activation or reset code replaces the account's older one. A password reset ends
    every session of the account, a change the others.
  - Rate limits per IP and per account (memory only).
  - The dashboard's account_checks setting switches the blocking: off (the default, until 2.1.6 is out) - every
    account plays online as before, old launchers sign up as before; the codes work, to get accounts ready. On -
    the rules above, at once.
"""

import collections
import hashlib
import os
import re
import secrets
import sqlite3
import time

ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"  # (no 0/O, 1/I: read out or typed from a message)
RESET_LIFE = 24 * 3600
MIN_PASSWORD = 8
KINDS = ("signup", "reactivate", "reset")

# The text an old (2.1.5) launcher or game shows - its sign-in's error, as it is.
OLD_NEEDS_CODE = ("Online needs the 2.1.6 launcher now: update to 2.1.6, then sign in there with the re-activation "
                  "code the server owner gives you.")
NEEDS_CODE = "Your account needs its re-activation code (from the server owner) to play online again."


def now():
    return time.time()


def normalize(code):
    return re.sub(r"[^0-9A-Za-z]", "", code or "").upper()


def new_code(n):
    c = "".join(secrets.choice(ALPHABET) for _ in range(n))
    return "-".join(c[i:i + 4] for i in range(0, n, 4))


def code_hash(kind, code):
    return hashlib.sha256(("%s:%s" % (kind, normalize(code))).encode()).hexdigest()


class Limiter:
    """At most `limit` tries per `window` seconds for each key (memory only)."""

    def __init__(self):
        self.q = collections.defaultdict(collections.deque)

    def hit(self, key, limit, window):
        q = self.q[key]
        while q and q[0] < now() - window:
            q.popleft()
        if len(q) >= limit:
            return True
        q.append(now())
        return False


class Signup:
    def __init__(self, store, accounts, log, open_accounts=False, exempt="", checks=lambda: True):
        self.store, self.db, self.lock = store, store.db, store.lock
        self.accounts, self.log = accounts, log
        self.open = open_accounts          # (--test-open-accounts: a private test server - no codes)
        self.checks = checks               # (the dashboard's account_checks: on - the rules block)
        self.limits = Limiter()
        self.migrate(exempt)

    # -- the database ----------------------------------------------------------------------------------

    def migrate(self, exempt):
        with self.lock:
            cols = {r[1] for r in self.db.execute("PRAGMA table_info(accounts)")}
            if "activated" not in cols:
                self.backup("before-codes")
                n = self.db.execute("SELECT COUNT(*) FROM accounts").fetchone()[0]
                # (the accounts from before: not active until their re-activation code - once account_checks is on.
                # A database from the email plan's test servers has email / email_ok too: unused, left as they are)
                self.db.execute("ALTER TABLE accounts ADD COLUMN activated INTEGER DEFAULT 0")
                self.log("accounts: added activation (%d account(s) from before: once account_checks is on, blocked "
                         "online until their re-activation code)" % n)
            self.db.executescript("""
                CREATE TABLE IF NOT EXISTS invites (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, hash TEXT UNIQUE, note TEXT,
                    account INTEGER, created REAL, used_by INTEGER, used_at REAL, revoked REAL);
            """)
            if exempt:  # (--admin-account: the server owner's own account never needs a re-activation code)
                if self.db.execute("UPDATE accounts SET activated = 1 WHERE name = ? AND activated = 0",
                                   (exempt,)).rowcount:
                    self.log("accounts: %s active without a re-activation code (--admin-account)" % exempt)
            self.db.commit()

    def backup(self, why):
        folder = os.path.join(self.store.folder, "backups")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "community-%s-%s.db" % (time.strftime("%Y%m%d-%H%M%S"), why))
        dst = sqlite3.connect(path)
        try:
            self.db.backup(dst)
        finally:
            dst.close()
        self.log("accounts: database backed up to %s" % path)
        return path

    # -- the account's state ---------------------------------------------------------------------------

    def activated(self, account_id):
        with self.lock:
            row = self.db.execute("SELECT activated FROM accounts WHERE id = ?", (account_id,)).fetchone()
        return bool(row and row[0])

    def relaxed(self):
        """Nobody blocked: a test server, or account_checks off."""
        return self.open or not self.checks()

    def blocked_count(self):
        """(the accounts account_checks on would keep off online now, all accounts) - banned ones left out."""
        with self.lock:
            rows = self.db.execute("SELECT activated FROM accounts WHERE banned = 0").fetchall()
        return sum(1 for (a,) in rows if not a), len(rows)

    def online_ok(self, account):
        return self.relaxed() or self.activated(account["id"])

    def blocked_why(self, account, old_client):
        """'' when it may play online, else what the player has to do (an old client's text: update)."""
        if self.online_ok(account):
            return ""
        return OLD_NEEDS_CODE if old_client else NEEDS_CODE

    def account_json(self, account):
        strict = not self.relaxed()  # (checks off: no steps - the launchers show none, nothing is blocked)
        return {"name": account["name"], "needs_code": strict and not self.activated(account["id"]),
                "online_ok": self.online_ok(account), "checks": strict}

    # -- the codes (the dashboard) ------------------------------------------------------------------------

    def make_code(self, kind, note="", account=None):
        """A new one-use code (shown once: only its hash is kept). Re-activation and reset codes: bound to the
        account, a new one replaces its older open one."""
        assert kind in KINDS
        code = new_code(12)
        with self.lock:
            if kind != "signup":
                self.db.execute("UPDATE invites SET revoked = ? WHERE kind = ? AND account = ? AND used_by IS NULL "
                                "AND revoked IS NULL", (now(), kind, account))
            cur = self.db.execute("INSERT INTO invites (kind, hash, note, account, created) VALUES (?, ?, ?, ?, ?)",
                                  (kind, code_hash("invite", code), note[:80], account, now()))
            self.db.commit()
        return cur.lastrowid, code

    def revoke(self, invite_id):
        with self.lock:
            n = self.db.execute("UPDATE invites SET revoked = ? WHERE id = ? AND used_by IS NULL AND revoked IS NULL",
                                (now(), invite_id)).rowcount
            self.db.commit()
        return n > 0

    def invites(self):
        with self.lock:
            rows = self.db.execute(
                "SELECT i.id, i.kind, i.note, i.account, a.name, i.created, i.used_by, u.name, i.used_at, i.revoked "
                "FROM invites i LEFT JOIN accounts a ON a.id = i.account LEFT JOIN accounts u ON u.id = i.used_by "
                "ORDER BY i.id DESC").fetchall()
        keys = ("id", "kind", "note", "account", "account_name", "created", "used_by", "used_by_name", "used_at",
                "revoked")
        out = [dict(zip(keys, r)) for r in rows]
        for i in out:  # (a reset code past its 24 hours: expired)
            i["expired"] = i["kind"] == "reset" and not i["used_by"] and now() - (i["created"] or 0) > RESET_LIFE
        return out

    def _take(self, kind, code, account_id=None):
        """The live code's row id (under the lock), or None."""
        row = self.db.execute("SELECT id, account, created FROM invites WHERE hash = ? AND kind = ? AND used_by IS "
                              "NULL AND revoked IS NULL", (code_hash("invite", code), kind)).fetchone()
        if not row or (kind != "signup" and row[1] != account_id):
            return None
        if kind == "reset" and now() - (row[2] or 0) > RESET_LIFE:
            return None
        return row[0]

    def reactivate(self, account, code, ip):
        if self.limits.hit(("code", account["id"]), 8, 1800) or self.limits.hit(("code-ip", ip), 20, 1800):
            return "Too many tries - wait half an hour."
        with self.lock:
            iid = self._take("reactivate", code, account["id"])
            if not iid:
                return "That code isn't right for this account (or it was used or replaced)."
            self.db.execute("UPDATE invites SET used_by = ?, used_at = ? WHERE id = ?", (account["id"], now(), iid))
            self.db.execute("UPDATE accounts SET activated = 1 WHERE id = ?", (account["id"],))
            self.db.commit()
        self.accounts.cache.clear()
        self.log("accounts: %s re-activated (code %d) from %s" % (account["name"], iid, ip))
        return ""

    def set_active(self, account_id):
        with self.lock:
            self.db.execute("UPDATE accounts SET activated = 1 WHERE id = ?", (account_id,))
            self.db.commit()
        self.accounts.cache.clear()

    # -- sign-up -----------------------------------------------------------------------------------------

    def register(self, name, password, invite, ip):
        """(account, error) - an active account (its invite code used)."""
        if not self.open and self.limits.hit(("signup-ip", ip), 5, 3600):  # (test servers make many)
            return None, "Too many sign-ups from here - wait an hour."
        if not self.open and len(password) < MIN_PASSWORD:
            return None, "Passwords need at least %d characters." % MIN_PASSWORD
        with self.lock:
            iid = None
            if not self.open:
                iid = self._take("signup", invite)
                if not iid:
                    return None, "That invite code isn't right (or it was used)."
            account, error = self.accounts.register(name, password, ip)
            if not account:
                return None, error
            self.db.execute("UPDATE accounts SET activated = 1 WHERE id = ?", (account["id"],))
            if iid:
                self.db.execute("UPDATE invites SET used_by = ?, used_at = ? WHERE id = ?", (account["id"], now(), iid))
            self.db.commit()
        return account, None

    # -- passwords ----------------------------------------------------------------------------------------------

    def reset(self, name, code, password, ip):
        """A new password with the account's reset code (the dashboard's); every session of the account ends."""
        name = (name or "").strip()
        if self.limits.hit(("reset-ip", ip), 20, 1800) or self.limits.hit(("reset", name.lower()), 10, 1800):
            return "Too many tries - wait half an hour."
        if len(password) < MIN_PASSWORD:
            return "Passwords need at least %d characters." % MIN_PASSWORD
        with self.lock:
            acc = self.db.execute("SELECT id, name FROM accounts WHERE name = ? COLLATE NOCASE", (name,)).fetchone()
            iid = acc and self._take("reset", code, acc[0])
            if not iid:
                return "That reset code isn't right for this account (or it was used, replaced or is over 24 hours old)."
            self.db.execute("UPDATE invites SET used_by = ?, used_at = ? WHERE id = ?", (acc[0], now(), iid))
            self.db.commit()
        self.set_password(acc[0], password)
        self.log("accounts: %s set a new password with its reset code (from %s)" % (acc[1], ip))
        return ""

    def set_password(self, account_id, password, keep_token=None):
        """A new password; every session of the account ends (but keep_token's: the one changing it)."""
        salt = secrets.token_hex(16)
        with self.lock:
            self.db.execute("UPDATE accounts SET salt = ?, pwhash = ? WHERE id = ?",
                            (salt, self.accounts.hash_password(password, salt), account_id))
            keep = hashlib.sha256(keep_token.encode()).hexdigest() if keep_token else ""
            self.db.execute("DELETE FROM sessions WHERE account = ? AND token != ?", (account_id, keep))
            self.db.commit()
        self.accounts.cache.clear()
