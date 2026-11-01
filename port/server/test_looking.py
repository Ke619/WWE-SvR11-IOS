"""Checks the in-game NEWS line's count (server.py looking_for_match): only players searching / on the ONLINE MATCH
screens (presence place "matchmaking") or hosting a lobby (place "lobby_host", or any game's live session report
still in the lobby) - not players in a match, in a joined lobby or elsewhere online. Starts a private server on a
test port with a throwaway data folder.

    python test_looking.py [--port 8549]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
FAILS = []


def check(what, ok):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        FAILS.append(what)


def call(server, method, path, body=None, token=None):
    headers = {"Content-Type": "application/json"} if body is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(server + path, method=method, headers=headers,
                                 data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(req, timeout=30) as f:
            return f.status, json.loads(f.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8549)
    port = ap.parse_args().port
    data = Path(tempfile.mkdtemp(prefix="svr_looking_"))
    proc = subprocess.Popen([sys.executable, str(HERE / "server.py"), "--host", "127.0.0.1", "--port", str(port),
                             "--data", str(data), "--local-admin", "--test-open-accounts",
                             "--log", str(data / "server.log")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    server = "http://127.0.0.1:%d" % port
    try:
        for _ in range(60):
            try:
                urllib.request.urlopen(server + "/api/status", timeout=1).read()
                break
            except Exception:
                time.sleep(0.3)
        suffix, password = os.urandom(3).hex(), "pw-" + os.urandom(6).hex()
        acc = {}
        for who in ("search", "host", "match", "joined", "menu", "older"):
            st, a = call(server, "POST", "/api/register", {"name": "lk_%s_%s" % (who[:4], suffix), "password": password})
            acc[who] = a.get("token")
        check("private server up, six accounts", all(acc.values()))

        def here(who, place="", kind=""):
            q = "/api/friends?here=1&mode=online" + ("&place=" + place if place else "") + ("&kind=" + kind if kind else "")
            return call(server, "GET", q, token=acc[who])[1]

        def count():
            news = here("menu", "online_menu").get("config", {}).get("news", "")
            m = re.match(r"(\d+) players? looking for a match online now\. ", news)
            return int(m.group(1)) if m else -1, news

        n, news = count()
        check("the NEWS line: 'N players looking for a match online now. ' (%r)" % news[:60], n == 0)
        check("... still has ' online now. ' (2.1.6 / 2.1.7 games rewrite their panel only over it)", " online now. " in news)
        here("search", "matchmaking")
        check("searching (place matchmaking): 1", count()[0] == 1)
        n, news = count()
        check("one: 'player', not 'players' (%r)" % news[:45], news.startswith("1 player looking for a match"))
        here("host", "lobby_host", "ranked")
        check("hosting a ranked lobby (place lobby_host): 2", count()[0] == 2)
        here("match", "match_online", "player")
        here("joined", "lobby_join")
        check("in a match / joined to a lobby / on the ONLINE menu: not counted (2)", count()[0] == 2)
        # (an older game: no place - its host report says lobby)
        here("older")
        st, _ = call(server, "POST", "/api/live", {"session": "AB" + suffix, "state": "lobby", "slots": 2,
                                                  "players": [{"name": "lk_olde", "path": "host"}]}, acc["older"])
        check("an older game's host report in the lobby: counted (3)", st == 200 and count()[0] == 3)
        call(server, "POST", "/api/live", {"session": "AB" + suffix, "state": "match", "slots": 2,
                                           "players": [{"name": "lk_olde", "path": "host"}]}, acc["older"])
        check("... its match started (state match): not counted (2)", count()[0] == 2)
        # (the 2.1.7 host reports its lobby too: counted once)
        call(server, "POST", "/api/live", {"session": "CD" + suffix, "state": "lobby", "slots": 2, "match_kind": "ranked",
                                           "players": [{"name": "lk_host", "path": "host"}]}, acc["host"])
        check("a host with both a lobby place and a lobby report: counted once (2)", count()[0] == 2)
        call(server, "GET", "/api/friends?bye=1", token=acc["search"])
        check("the searcher's game closes (bye): 1", count()[0] == 1)
        call(server, "POST", "/api/live", {"session": "CD" + suffix, "ended": True}, acc["host"])
        here("host", "match_online", "ranked")
        check("the host's match starts (place match_online, session ended): 0", count()[0] == 0)
        st, d = call(server, "GET", "/api/admin/stats")
        # (still running: host, match, joined, menu, older - the dashboard counts every game, as before)
        online = d.get("now", {}).get("players_online")
        check("the dashboard's own count unchanged: every game running (%s)" % online, st == 200 and online == 5)
    finally:
        proc.kill()
        proc.wait(10)
    print("--- %s (%s)" % ("all ok" if not FAILS else "%d FAILED" % len(FAILS), data))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
