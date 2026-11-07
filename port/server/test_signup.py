"""Checks the account codes (signup.py, 2.1.6 on: codes only, no email) on a server of its own: a temporary data
folder with accounts from before (2.1.5's table), the dashboard on loopback.

  account_checks off (the default): 2.1.5 launchers and games as before, 2.1.6 sign-ups with an invite code;
  then on (live): invite sign-up (one use, revoked codes refused), re-activation codes bound to one account, the
  game's online sign-in blocked until then, old launchers' texts, reset codes from the dashboard (24 h, once,
  sessions ended), password change, rate limits, --activate, the migration's backup, codes kept as hashes.

    python test_signup.py [--port 8561] [--keep]
"""
import argparse
import glob
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def call(server, method, path, value=None, token=None, ip=""):
    """(the player's calls: each from its own address - X-Forwarded-For, as nginx sets it - unless ip; the
    dashboard's: none, loopback)"""
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    if "/api/admin/" not in path:
        headers["X-Forwarded-For"] = ip or "10.9.%d.%d" % tuple(os.urandom(2))
    data = json.dumps(value).encode() if value is not None else (b"{}" if method == "POST" else None)
    req = urllib.request.Request(server + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            body, status = r.read(), r.status
    except urllib.error.HTTPError as e:
        body, status = e.read(), e.code
    try:
        return status, json.loads(body or b"{}")
    except ValueError:
        return status, {"text": body.decode(errors="replace")}


def old_database(folder):
    """community.db as 2.1.5 left it: two accounts (old_one, Old_Two; password 'oldpass1'), no activation."""
    import hashlib
    db = sqlite3.connect(os.path.join(folder, "community.db"))
    db.executescript("""
        CREATE TABLE accounts (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT UNIQUE COLLATE NOCASE,
            salt TEXT, pwhash TEXT, profileid INTEGER, created REAL, last_seen REAL, banned INTEGER DEFAULT 0, ip TEXT);
        CREATE TABLE sessions (token TEXT PRIMARY KEY, account INTEGER, created REAL, last_used REAL, ip TEXT);
    """)
    for name in ("old_one", "Old_Two"):
        salt = os.urandom(16).hex()
        h = hashlib.pbkdf2_hmac("sha256", b"oldpass1", bytes.fromhex(salt), 240000).hex()
        db.execute("INSERT INTO accounts (name, salt, pwhash, created, last_seen) VALUES (?, ?, ?, ?, ?)",
                   (name, salt, h, time.time(), time.time()))
    db.commit()
    db.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8561)  # (outside 8460-8499: the test tools' free-port range)
    ap.add_argument("--keep", action="store_true", help="leave the data folder")
    args = ap.parse_args()
    data = tempfile.mkdtemp(prefix="svr_signup_")
    old_database(data)
    server = "http://127.0.0.1:%d" % args.port
    proc = subprocess.Popen([sys.executable, os.path.join(HERE, "server.py"), "--port", str(args.port), "--data", data,
                             "--local-admin", "--admin-account", "Old_Two"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    def users():
        return {u["name"]: u for u in call(server, "GET", "/api/admin/users")[1]}

    try:
        for _ in range(100):
            try:
                if call(server, "GET", "/api/status")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        check(glob.glob(os.path.join(data, "backups", "community-*-before-codes.db")), "migration: a backup first")
        cols = {r[1] for r in sqlite3.connect(os.path.join(data, "community.db")).execute("PRAGMA table_info(accounts)")}
        check("activated" in cols and "email" not in cols, "migration: the activated column (no email columns)")

        # -- account_checks off (the default): every account and client as before 2.1.6 --
        st, r = call(server, "POST", "/api/login", {"name": "old_one", "password": "oldpass1"})
        check(st == 200 and r.get("token") and not r.get("needs_code"), "checks off: a 2.1.5 launcher signs in")
        tok = r.get("token", "")
        st, r = call(server, "GET", "/api/session", token=tok)
        check(st == 200 and r.get("ticket"), "checks off: the game's online sign-in works")
        st, r = call(server, "GET", "/api/friends", token=tok)
        check(st == 200 and r.get("config", {}).get("netcode") == "lockstep", "checks off: presence; config netcode lockstep")
        st, r = call(server, "POST", "/api/register", {"name": "oldstyle", "password": "pw1234"})
        check(st == 200 and r.get("token"), "checks off: a 2.1.5 launcher's sign-up as before")
        st, r = call(server, "POST", "/api/register", {"name": "newstyle", "password": "longpass1", "client": 2,
                                                        "invite": "WRONG-CODE"})
        check(st == 403 and "invite" in r.get("error", ""), "checks off: a wrong invite code typed is still refused")
        st, r = call(server, "GET", "/api/status")
        check(st == 200 and r.get("invite_needed") is False, "checks off: status says no invite code needed")
        st, m = call(server, "GET", "/api/admin/checks")
        check(st == 200 and m["checks"] == "off" and m["would_block"] == 2 and m["accounts"] == 3,
              "the ON switch's numbers: 2 of 3 accounts would be blocked (Old_Two: --admin-account)")
        st, r = call(server, "POST", "/api/admin/settings", {"netcode": "sideways"})
        check(st == 400, "netcode: an unknown value refused")
        st, r = call(server, "POST", "/api/admin/settings", {"account_checks": "on"})
        check(st == 200, "account_checks on (live, no restart)")
        st, r = call(server, "POST", "/api/login", {"name": "oldstyle", "password": "pw1234"})
        check(st == 403 and "re-activation" in r.get("error", ""), "checks on: the account made while off needs a code")

        # -- invite sign-up --
        st, inv = call(server, "POST", "/api/admin/invites", {"note": "for Bob", "count": 2})
        check(st == 200 and len(inv.get("codes", [])) == 2, "dashboard: 2 invite codes made")
        code, code2 = inv["codes"][0]["code"], inv["codes"][1]["code"]
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1"})
        check(st == 403 and "2.1.6" in r.get("error", "") and "invite" in r.get("error", ""),
              "sign-up: an old launcher (no client) is told to update")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1",
                                                       "invite": "AAAA-BBBB-CCCC", "client": 2})
        check(st == 403 and "invite code" in r.get("error", ""), "sign-up: a wrong invite code refused")
        st, r = call(server, "POST", "/api/register", {"name": "OLD_ONE", "password": "longpass1", "invite": code,
                                                       "client": 2})
        check(st == 403 and "taken" in r.get("error", ""), "sign-up: a name taken in other case refused")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "short", "invite": code,
                                                       "client": 2})
        check(st == 403 and "8 characters" in r.get("error", ""), "sign-up: a short password refused")
        st, r = call(server, "POST", "/api/register", {"name": "newbie", "password": "longpass1",
                                                       "invite": code.lower().replace("-", " "), "client": 2})
        check(st == 200 and r.get("token") and r.get("online_ok") and not r.get("needs_code"),
              "sign-up: with the code (any case / spacing) - signed in, online ok")
        bob = r.get("token", "")
        st, r = call(server, "POST", "/api/register", {"name": "newbie2", "password": "longpass1", "invite": code,
                                                       "client": 2})
        check(st == 403, "sign-up: the same invite code twice refused")
        st, r = call(server, "GET", "/api/session", token=bob)
        check(st == 200 and r.get("ticket"), "game: a new account's online sign-in works")

        # -- an account from before: its re-activation code --
        st, r = call(server, "POST", "/api/login", {"name": "old_one", "password": "oldpass1"})
        check(st == 403 and "re-activation code" in r.get("error", "") and "2.1.6" in r.get("error", ""),
              "old account, old launcher: told to update and get the code")
        st, r = call(server, "POST", "/api/login", {"name": "old_one", "password": "oldpass1", "client": 2})
        check(st == 200 and r.get("needs_code"), "old account, 2.1.6 launcher: signed in, needs its code")
        old = r.get("token", "")
        st, r = call(server, "GET", "/api/session", token=old)
        check(st == 403 and "2.1.6" in r.get("error", ""), "old account: online refused (a 2.1.5 game's text)")
        st, r = call(server, "GET", "/api/session?client=2", token=old)
        check(st == 403 and "re-activation" in r.get("error", ""), "old account: online refused (the 2.1.6 game's text)")
        st, r = call(server, "GET", "/api/friends?client=2", token=old)
        check(st == 403, "old account: friends / presence refused too")
        ids = {n: u["id"] for n, u in users().items()}
        st, rc_bob = call(server, "POST", "/api/admin/user/%d/reactivation-code" % ids["newbie"])
        st, rc = call(server, "POST", "/api/admin/user/%d/reactivation-code" % ids["old_one"])
        check(st == 200 and rc.get("code"), "dashboard: a re-activation code for old_one")
        st, r = call(server, "POST", "/api/reactivate", {"code": rc_bob.get("code", "")}, token=old)
        check(st == 403, "re-activation: another account's code refused")
        st, r = call(server, "POST", "/api/register", {"name": "sneaky", "password": "longpass1", "invite": rc["code"],
                                                       "client": 2})
        check(st == 403, "re-activation code refused for a sign-up")
        st, r = call(server, "POST", "/api/reactivate", {"code": rc["code"]}, token=old)
        check(st == 200 and not r.get("needs_code") and r.get("online_ok"), "re-activation: its own code - online ok")
        st, r = call(server, "POST", "/api/reactivate", {"code": rc["code"]}, token=old)
        check(st == 403, "re-activation: the code works once")
        st, r = call(server, "GET", "/api/session", token=old)
        check(st == 200, "old account: online again")
        st, r = call(server, "POST", "/api/login", {"name": "old_two", "password": "oldpass1", "client": 2})
        check(st == 200 and not r.get("needs_code") and r.get("online_ok"), "--admin-account: no code needed")

        # -- reset codes (the dashboard's): a forgotten password --
        st, other = call(server, "POST", "/api/login", {"name": "newbie", "password": "longpass1", "client": 2})
        st, rs = call(server, "POST", "/api/admin/user/%d/reset-code" % ids["newbie"])
        check(st == 200 and rs.get("code"), "dashboard: a reset code for newbie")
        st, rs_old = call(server, "POST", "/api/admin/user/%d/reset-code" % ids["old_one"])
        st, r = call(server, "POST", "/api/password/reset", {"name": "newbie", "code": rs_old["code"],
                                                             "password": "newpass99"})
        check(st == 403, "reset: another account's code refused")
        st, r = call(server, "POST", "/api/password/reset", {"name": "newbie", "code": rs["code"], "password": "short"})
        check(st == 403 and "8 characters" in r.get("error", ""), "reset: a short password refused")
        st, r = call(server, "POST", "/api/password/reset", {"name": "NEWBIE", "code": rs["code"],
                                                             "password": "newpass99"})
        check(st == 200, "reset: the code sets the password (the name in any case)")
        check(call(server, "GET", "/api/account", token=bob)[0] == 401 and
              call(server, "GET", "/api/account", token=other.get("token", ""))[0] == 401,
              "reset: every session of the account ended")
        st, r = call(server, "POST", "/api/password/reset", {"name": "newbie", "code": rs["code"], "password": "again999"})
        check(st == 403, "reset: the code works once")
        st, r = call(server, "POST", "/api/admin/user/%d/reset-code" % ids["old_one"])
        st2, r2 = call(server, "POST", "/api/password/reset", {"name": "old_one", "code": rs_old["code"],
                                                               "password": "newpass99"})
        check(st == 200 and st2 == 403, "reset: a newer reset code replaces the older")
        st, r = call(server, "POST", "/api/login", {"name": "newbie", "password": "newpass99", "client": 2})
        check(st == 200 and r.get("online_ok"), "sign in with the new password")
        bob = r.get("token", "")
        st, r2 = call(server, "POST", "/api/login", {"name": "newbie", "password": "newpass99", "client": 2})
        other = r2.get("token", "")

        # -- password change: the other sessions end --
        st, r = call(server, "POST", "/api/password/change", {"password": "wrong", "new_password": "x" * 9}, token=bob)
        check(st == 403, "change: the old password is asked")
        st, r = call(server, "POST", "/api/password/change", {"password": "newpass99", "new_password": "third999"},
                     token=bob)
        check(st == 200, "change: done")
        check(call(server, "GET", "/api/account", token=bob)[0] == 200, "change: this session stays")
        check(call(server, "GET", "/api/account", token=other)[0] == 401, "change: the other session ended")

        # -- revoke, the Invites tab, limits, hashes --
        st, inv = call(server, "POST", "/api/admin/invites", {"note": "revoked"})
        iid, rcode = inv["codes"][0]["id"], inv["codes"][0]["code"]
        call(server, "POST", "/api/admin/invite/%d/revoke" % iid)
        st, r = call(server, "POST", "/api/register", {"name": "late", "password": "longpass1", "invite": rcode,
                                                       "client": 2})
        check(st == 403, "a revoked invite refused")
        listed = call(server, "GET", "/api/admin/invites")[1].get("invites", [])
        used = [i for i in listed if i["kind"] == "signup" and i["used_by_name"] == "newbie"]
        check(used and used[0]["note"] == "for Bob", "Invites tab: who used each sign-up code")
        check(any(i["kind"] == "reactivate" and i["used_by_name"] == "old_one" for i in listed) and
              any(i["kind"] == "reset" and i["used_by_name"] == "newbie" for i in listed),
              "Invites tab: the re-activation and reset codes too")
        db_codes = [r[0] for r in sqlite3.connect(os.path.join(data, "community.db")).execute("SELECT hash FROM invites")]
        check(all(len(h) == 64 and "-" not in h for h in db_codes), "codes kept as hashes only")
        st, rc2 = call(server, "POST", "/api/admin/user/%d/reactivation-code" % ids["oldstyle"])
        st, r = call(server, "POST", "/api/login", {"name": "oldstyle", "password": "pw1234", "client": 2})
        tries = [call(server, "POST", "/api/reactivate", {"code": "3333-4444-5555"}, token=r.get("token", ""))[1]
                 for _ in range(10)]
        check("Too many" in tries[-1].get("error", ""), "re-activation: rate limited per account")
        u = users()
        check(u["old_one"]["activated"] and not u["oldstyle"]["activated"], "dashboard: the accounts' state")
        check(call(server, "GET", "/api/admin/checks")[1]["would_block"] == 1, "the ON switch's numbers follow")
        st, r = call(server, "POST", "/api/admin/settings", {"account_checks": "off"})
        st, r = call(server, "POST", "/api/login", {"name": "oldstyle", "password": "pw1234"})
        check(st == 200, "account_checks off again: the blocked account plays as before (live)")
        st, r = call(server, "POST", "/api/register", {"name": "nocode216", "password": "longpass1", "client": 2})
        check(st == 200 and r.get("token"), "checks off: a 2.1.6 sign-up with no invite code is made")
    finally:
        proc.terminate()
        proc.wait(10)
    r = subprocess.run([sys.executable, os.path.join(HERE, "server.py"), "--data", data, "--activate", "nobody_here"],
                       capture_output=True, text=True)
    check(r.returncode == 1, "--activate: an unknown name says so")
    r = subprocess.run([sys.executable, os.path.join(HERE, "server.py"), "--data", data, "--activate", "oldstyle"],
                       capture_output=True, text=True)
    check(r.returncode == 0, "--activate: an account let in")
    if args.keep:
        print("data:", data)
    else:
        shutil.rmtree(data, ignore_errors=True)
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
