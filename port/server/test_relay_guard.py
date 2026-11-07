"""Checks the relay's guards (29 Oct) on this PC: the tickets with the account in
them, a ban reaching the relay and the edge, the match network p2p kept by the
relay itself (no match data through it, whatever the game was told), and the
dashboard's Relay now rows - never a bare "?". Starts a relay edge and a server
with --relay-edge on test ports and a throwaway data folder.

    python test_relay_guard.py [--ports 8545]   (uses that port, the next one and +10)
"""
import argparse
import json
import os
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

import relay_link as rl

HERE = Path(__file__).resolve().parent
PY = sys.executable
HDR = b"SVRR\x01"
REGISTER, WELCOME, FORWARD, FROM = 1, 2, 3, 4
BROADCAST = b"\xff" * 6


def call(server, method, path, body=None, token=None):
    h = {"Content-Type": "application/json"}
    if token:
        h["Authorization"] = "Bearer " + token
    data = json.dumps(body).encode() if body is not None else (b"" if method == "POST" else None)
    req = urllib.request.Request(server + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def wait_up(url, proc, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            urllib.request.urlopen(url + "/api/status", timeout=1).read()
            return True
        except Exception:
            time.sleep(0.3)
    return False


class UdpGame:
    """A game on the edge's UDP: its id, its socket, what it got."""

    def __init__(self, port, gid=None):
        self.port = port
        self.gid = gid or bytes([2]) + secrets.token_bytes(5)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.settimeout(0.3)

    def register(self, ticket, wait=1.0):
        """True when welcomed."""
        self.last_ticket = ticket
        self.drain()
        self.s.sendto(HDR + bytes([REGISTER]) + self.gid + ticket, ("127.0.0.1", self.port))
        end = time.time() + wait
        while time.time() < end:
            try:
                d = self.s.recvfrom(4096)[0]
            except socket.timeout:
                continue
            if d[:6] == HDR + bytes([WELCOME]):
                return True
        return False

    def send(self, to, data):
        self.s.sendto(HDR + bytes([FORWARD]) + to + data, ("127.0.0.1", self.port))

    def drain(self, wait=0.0):
        """The packets that came (FROM bodies), for `wait` seconds."""
        got = []
        end = time.time() + wait
        while True:
            try:
                d = self.s.recvfrom(4096)[0]
                if d[:6] == HDR + bytes([FROM]):
                    got.append(d[12:])
            except socket.timeout:
                if time.time() >= end:
                    return got


def units(check):
    secret = secrets.token_bytes(32)
    gid, other = bytes(range(6)), bytes(range(1, 7))
    t = rl.make_ticket(secret, gid, 4242)
    got = rl.read_ticket(secret, gid, t)
    check(got and got[0] == 4242 and abs(got[1] - time.time()) < 3, "ticket: the account in it (%s)" % (got,))
    check(rl.read_ticket(secret, other, t) is None, "ticket: another game's id refused")
    bad = t[:4] + struct.pack(">I", 7) + t[8:]
    check(rl.read_ticket(secret, gid, bad) is None, "ticket: another account written in refused")
    expiry = struct.pack(">I", int(time.time() + rl.TICKET_LIFE))
    import hashlib
    import hmac
    old = expiry + hmac.new(secret, b"ticket" + gid + expiry, hashlib.sha256).digest()[:12]
    got = rl.read_ticket(secret, gid, old)
    check(got and got[0] == 0, "ticket: a first-format one passes, as account 0")
    check(rl.read_ticket(secret, gid, rl.make_ticket(secret, gid, 1, now=time.time() - 2 * rl.TICKET_LIFE)) is None,
          "ticket: an expired one refused")
    ids = [gid, other]
    games = {gid: (5, socket.inet_aton("1.2.3.4") + struct.pack(">H", 36500), 70), other: (0, bytes(6), 0)}
    body = rl.state_body(9, ids, games)
    check(rl.read_state(body) == (9, ids), "state: an older home reads the ids of the new body")
    relayed, ids2, extra = rl.read_state_full(body)
    check(relayed == 9 and ids2 == ids and extra[gid] == games[gid] and extra[other] == games[other],
          "state: account, address, sent round trip")
    check(rl.read_state_full(rl.state_body(3, ids))[2] == {}, "state: an older edge's body - no extra columns")
    check(rl.read_home_state(b"") == rl.AUTO and rl.read_home_state(rl.home_state(rl.P2P)) == rl.P2P,
          "home state: the match network (auto from an older home)")
    check(rl.read_kick(rl.kick_body([3, 70000])) == [3, 70000], "kick: round trip")
    lim = rl.PairLimit()
    now = 1000.0
    first = sum(lim.allowed(gid, other, now) for _ in range(40))
    check(first == rl.PAIR_BURST, "pair limit: %d at once (burst %d)" % (first, rl.PAIR_BURST))
    check(lim.allowed(other, gid, now), "pair limit: the other way is its own")
    later = sum(lim.allowed(gid, other, now + 1.0) for _ in range(40))
    check(later == int(rl.PAIR_RATE), "pair limit: then %d a second" % later)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", type=int, default=8545)
    args = ap.parse_args()
    p = args.ports
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    print("--- units")
    units(check)

    tmp = Path(tempfile.mkdtemp(prefix="relay_guard_"))
    secret_file = tmp / "secret.txt"
    secret_file.write_text(secrets.token_hex(32))
    secret = rl.read_secret(secret_file)
    edge_log, server_log = tmp / "edge.log", tmp / "server.log"
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    procs = {}

    def start_edge():
        procs["edge"] = subprocess.Popen([PY, str(HERE / "relay_edge.py"), "--udp-port", str(p), "--link", "127.0.0.1:%d" % (p + 1),
                                          "--secret", str(secret_file), "--log", str(edge_log), "--report", "1"],
                                         creationflags=flags)

    def start_server():
        procs["server"] = subprocess.Popen(
            [PY, str(HERE / "server.py"), "--port", str(p + 10), "--data", str(tmp / "data"), "--test-open-accounts",
             "--local-admin", "--relay-udp-port", str(p), "--relay-edge", "127.0.0.1:%d" % (p + 1),
             "--relay-secret", str(secret_file), "--log", str(server_log)],
            creationflags=flags, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    def stop(name):
        procs[name].terminate()
        procs[name].wait(10)

    url = "http://127.0.0.1:%d" % (p + 10)
    try:
        start_edge()
        start_server()
        check(wait_up(url, procs["server"]), "server with an edge up (pids edge %d, server %d)" % (
            procs["edge"].pid, procs["server"].pid))
        time.sleep(1.5)
        names = ["rg%s%d" % (secrets.token_hex(3), i) for i in range(3)]
        pw = {n: secrets.token_hex(8) for n in names}
        tokens = {n: call(url, "POST", "/api/register", {"name": n, "password": pw[n]})[1].get("token") for n in names}
        check(all(tokens.values()), "three throwaway accounts")
        users = {u["name"]: u["id"] for u in call(url, "GET", "/api/admin/users")[1]}
        a, b, c = [UdpGame(p) for _ in names]
        games = dict(zip(names, (a, b, c)))

        def ticket(n):
            st, t = call(url, "GET", "/api/relay/%s/udp" % games[n].gid.hex(), token=tokens[n])
            return bytes.fromhex(t["ticket"]) if st == 200 else None
        for n, g in games.items():
            t = ticket(n)
            check(t and rl.read_ticket(secret, g.gid, t)[0] == users[n], "%s: its ticket has its account (%d)" % (n, users[n]))
            check(g.register(t), "%s: welcomed at the edge" % n)
        time.sleep(1.5)
        live = call(url, "GET", "/api/admin/live")[1]
        rows = {r["id"]: r for r in live.get("relay_games", [])}
        check(all(g.gid.hex() in rows and rows[g.gid.hex()]["name"] == n and rows[g.gid.hex()]["known"] and
                  rows[g.gid.hex()]["ip"] == "127.0.0.1" and rows[g.gid.hex()]["link"] == "UDP (edge)"
                  for n, g in games.items()), "dashboard: the edge's games by name, address and link")

        # an older ticket (no account): its row says so, never "?"
        d = UdpGame(p)
        expiry = struct.pack(">I", int(time.time() + rl.TICKET_LIFE))
        import hashlib
        import hmac
        check(d.register(expiry + hmac.new(secret, b"ticket" + d.gid + expiry, hashlib.sha256).digest()[:12]),
              "an older ticket still welcomed")
        time.sleep(1.5)
        rows = {r["id"]: r for r in call(url, "GET", "/api/admin/live")[1].get("relay_games", [])}
        r = rows.get(d.gid.hex(), {})
        check(r and not r["known"] and "not known" in r["name"] and r["name"] != "?" and not r["account"],
              "dashboard: an older ticket's game - '%s'" % r.get("name"))

        # auto: match data passes
        for i in range(100):
            b.send(c.gid, b"m%03d" % i)
        got = c.drain(1.0)
        check(len(got) == 100, "auto: 100 packets b -> c (%d)" % len(got))
        time.sleep(1.2)
        rows = {r["id"]: r for r in call(url, "GET", "/api/admin/live")[1].get("relay_games", [])}
        check(rows.get(b.gid.hex(), {}).get("rate", 0) > 0, "dashboard: b's packets a second (%s)" %
              rows.get(b.gid.hex(), {}).get("rate"))

        # p2p: the relay itself holds match data back
        st, _ = call(url, "POST", "/api/admin/settings", {"match_network": "p2p"})
        check(st == 200, "match network p2p set")
        time.sleep(1.5)
        for i in range(100):
            b.send(c.gid, b"p%03d" % i)
            time.sleep(0.01)
        got = c.drain(1.0)
        check(rl.PAIR_BURST <= len(got) <= rl.PAIR_BURST + 4, "p2p: of 100 packets b -> c in a second, %d passed" % len(got))
        a.send(c.gid, b"hello from a")
        check(c.drain(0.5) == [b"hello from a"], "p2p: another pair's hello still passes")
        a.send(BROADCAST, b"search")
        check(b.drain(0.5) == [b"search"] and c.drain(0.2) == [b"search"], "p2p: searches still reach everyone")
        check("match network p2p" in edge_log.read_text(), "edge log: match network p2p")
        call(url, "POST", "/api/admin/settings", {"match_network": "auto"})
        time.sleep(1.5)
        for i in range(30):
            b.send(c.gid, b"q%03d" % i)
        check(len(c.drain(0.8)) == 30, "auto again: 30 packets b -> c")

        # a ban: off the relay at once, its tickets refused
        cn = names[2]
        old_c_ticket = ticket(cn)
        st, _ = call(url, "POST", "/api/admin/user/%d/ban" % users[cn])
        check(st == 200, "c banned from the dashboard")
        time.sleep(0.5)
        c.send(b.gid, b"after the ban")
        check(b.drain(0.6) == [], "ban: c's packets no longer relayed")
        check(not c.register(old_c_ticket), "ban: c's ticket refused")
        st, _ = call(url, "GET", "/api/relay/%s/udp" % c.gid.hex(), token=tokens[cn])
        check(st == 401, "ban: no new ticket (%d)" % st)
        check("account %d banned" % users[cn] in edge_log.read_text(), "edge log: the ban")
        time.sleep(1.2)
        rows = {r["id"]: r for r in call(url, "GET", "/api/admin/live")[1].get("relay_games", [])}
        check(c.gid.hex() not in rows, "dashboard: c's game gone")

        # an edge restart forgets the ban: the server tells it again
        stop("edge")
        start_edge()
        time.sleep(2.0)
        for g, n in ((a, names[0]), (b, names[1])):
            g.register(ticket(n))
        welcomed = c.register(old_c_ticket)
        time.sleep(2.5)
        c.send(b.gid, b"after the edge restart")
        check(b.drain(0.6) == [] and not c.register(old_c_ticket),
              "edge restart: the banned game (welcomed: %s) dropped again by the server" % welcomed)

        # unban + sign in again: a new ticket works
        call(url, "POST", "/api/admin/user/%d/unban" % users[cn])
        time.sleep(1.2)
        st, body = call(url, "POST", "/api/login", {"name": cn, "password": pw[cn]})
        tokens[cn] = body.get("token")
        check(st == 200 and tokens[cn], "unban: c signs in again")
        check(c.register(ticket(cn)), "unban: c's new ticket welcomed")

        # a server restart: the edge's games are still known by name
        stop("server")
        start_server()
        check(wait_up(url, procs["server"]), "server restarted")
        # (the edge takes a new server once the old link is quiet 5 s; the games keep registering with the
        # tickets the old run made - their names are lost from its memory, not from the tickets)
        rows = {}
        for _ in range(12):
            for g in games.values():
                g.s.sendto(HDR + bytes([REGISTER]) + g.gid + g.last_ticket, ("127.0.0.1", p))
            time.sleep(1.0)
            rows = {r["id"]: r for r in call(url, "GET", "/api/admin/live")[1].get("relay_games", [])}
            if all(g.gid.hex() in rows for g in games.values()):
                break
        check(all(rows.get(g.gid.hex(), {}).get("name") == n for n, g in games.items()),
              "server restart: the edge's games still by name (%s)" % [rows.get(g.gid.hex(), {}).get("name") for g in games.values()])
        slog = server_log.read_text(encoding="utf-8", errors="replace")
        check("(?)" not in slog, "server log: no '(?)' line")
    finally:
        for proc in procs.values():
            proc.terminate()
        for proc in procs.values():
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
    print("relay guard: all ok" if ok else "relay guard: FAILED")
    print("(logs in %s)" % tmp)
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
