"""Checks a running Community Creations server's friends list (server.py
/api/friends): adding and removing friends, who added you, and presence -
offline, online (the game's requests) and playing (the match relay). It makes
three throwaway accounts: run it against a test server.

    python test_friends.py [--server http://127.0.0.1:8411]
"""
import argparse
import json
import secrets
import sys
import threading
import time
import urllib.error
import urllib.request


def request(server, method, path, body=None, token=None, timeout=15):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(server.rstrip("/") + path, method=method, headers=headers,
                                 data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="http://127.0.0.1:8411", help="its address, with its base (e.g. .../svr)")
    server = ap.parse_args().server
    failures = []

    def check(cond, what):
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    names = ["fr%s%d" % (secrets.token_hex(3), k) for k in range(3)]
    tokens = []
    for n in names:
        _, a = request(server, "POST", "/api/register", {"name": n, "password": "test-" + n})
        tokens.append(a.get("token"))
    check(all(tokens), "three accounts")
    a, b, c = tokens

    status, _ = request(server, "GET", "/api/friends")
    check(status == 401, "signed out: refused")
    status, r = request(server, "GET", "/api/friends", token=a)
    check(status == 200 and r.get("friends") == [], "an empty list")
    status, r = request(server, "POST", "/api/friends", {"add": "nobody-" + secrets.token_hex(4)}, a)
    check(status == 404, "an unknown name refused: %s" % r.get("error"))
    status, r = request(server, "POST", "/api/friends", {"add": names[0]}, a)
    check(status == 400, "adding yourself refused: %s" % r.get("error"))
    status, r = request(server, "POST", "/api/friends", {"add": names[1].upper()}, a)
    f = r.get("friends", [])
    check(status == 200 and len(f) == 1 and f[0]["name"] == names[1], "added (any case)")
    check(f and f[0]["status"] == "offline" and not f[0]["mutual"], "offline, not mutual")
    _, r = request(server, "GET", "/api/friends", token=b)
    check(r.get("added_you") == [names[0]], "the other player sees who added them")
    request(server, "POST", "/api/friends", {"add": names[0]}, b)
    _, r = request(server, "GET", "/api/friends", token=a)
    check(r["friends"][0]["mutual"], "added back: mutual")

    # the friend in the game's ONLINE menu (a game request), then in a match (the relay)
    request(server, "GET", "/api/friends?here=1&mode=online", token=b)  # (a 2.1.4+ game in online mode)
    _, r = request(server, "GET", "/api/friends", token=a)
    check(r["friends"][0]["status"] == "online", "online after a game in online mode asked")
    gid = secrets.token_hex(6)

    def hold():
        req = urllib.request.Request(server.rstrip("/") + "/api/relay/" + gid,
                                     headers={"Authorization": "Bearer " + b})
        try:
            with urllib.request.urlopen(req, timeout=6) as resp:
                resp.read()
        except Exception:
            pass
    t = threading.Thread(target=hold, daemon=True)
    t.start()
    time.sleep(1.5)
    _, r = request(server, "GET", "/api/friends", token=a)
    check(r["friends"][0]["status"] == "playing", "playing while its game has the relay open")
    # the launcher's own polling doesn't make anyone online
    request(server, "GET", "/api/friends", token=c)
    request(server, "POST", "/api/friends", {"add": names[2]}, a)
    _, r = request(server, "GET", "/api/friends", token=a)
    check(next(x for x in r["friends"] if x["name"] == names[2])["status"] == "offline",
          "the launcher's requests don't count as online")
    _, r = request(server, "POST", "/api/friends", {"remove": names[1]}, a)
    check([x["name"] for x in r["friends"]] == [names[2]], "removed")
    t.join(8)

    # invites to a match session (a's friends now: c; b still has a)
    import base64
    info = base64.b64encode(bytes(range(60))).decode()
    status, r = request(server, "POST", "/api/invite", {"to": names[1], "session": info, "kind": "ROYAL RUMBLE"}, a)
    check(status == 403, "only friends can be invited: %s" % r.get("error"))
    status, _ = request(server, "POST", "/api/invite", {"to": names[2], "session": "bm9wZQ==", "kind": "x"}, a)
    check(status == 400, "not a session: refused")
    status, _ = request(server, "POST", "/api/invite", {"to": names[2], "session": info, "kind": "ROYAL RUMBLE"}, a)
    check(status == 200, "invited")
    _, r = request(server, "GET", "/api/friends?here=1&mode=online", token=c)
    inv = r.get("invites", [])
    check(len(inv) == 1 and inv[0]["from"] == names[0] and inv[0]["session"] == info and inv[0]["kind"] == "ROYAL RUMBLE",
          "the invite reaches the friend, with its session")
    request(server, "POST", "/api/invite", {"decline": names[0]}, c)
    _, r = request(server, "GET", "/api/friends", token=c)
    check(r.get("invites") == [], "declined: gone")
    _, r = request(server, "GET", "/api/friends", token=a)
    check(next(x for x in r["friends"] if x["name"] == names[2])["status"] == "online",
          "a game asking (?here=1&mode=online) counts as online")
    request(server, "GET", "/api/friends?bye=1", token=c)
    _, r = request(server, "GET", "/api/friends", token=a)
    check(next(x for x in r["friends"] if x["name"] == names[2])["status"] == "offline",
          "a game closing (?bye=1) is offline at once")
    request(server, "GET", "/api/friends?here=1", token=c)
    _, r = request(server, "GET", "/api/friends", token=a)
    check(next(x for x in r["friends"] if x["name"] == names[2])["status"] == "ingame",
          "a game in the offline menus (?here=1, no mode) is in the game: it can be invited")
    status, _ = request(server, "POST", "/api/invite", {"to": names[2], "session": info, "kind": "Match"}, a)
    _, r = request(server, "GET", "/api/friends?here=1", token=c)
    check(status == 200 and len(r.get("invites", [])) == 1, "the invite reaches a friend outside online mode")
    print("friends: " + ("all ok" if not failures else "%d failed" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
