"""Checks the relay split (relay_link.py) on this PC: starts a relay edge
(relay_edge.py) and a server with --relay-edge on test ports and a
throwaway data folder, runs test_relay.py against them, and checks that
packets between UDP games stayed on the edge, that a forged ticket and a
link packet without the secret are refused, and that the server without an
edge still works.

    python test_relay_edge.py [--ports 37650]   (uses that port and the next 12)
"""
import argparse
import asyncio
import os
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import relay_link as rl
import test_relay

HERE = Path(__file__).resolve().parent
PY = sys.executable


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", type=int, default=37650)
    args = ap.parse_args()
    p = args.ports
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok    " if cond else "FAIL  ") + what)
        ok &= bool(cond)

    tmp = Path(tempfile.mkdtemp(prefix="relay_edge_"))
    secret_file = tmp / "secret.txt"
    secret_file.write_text(secrets.token_hex(32))
    secret = rl.read_secret(secret_file)
    edge_log, server_log = tmp / "edge.log", tmp / "server.log"
    procs = []
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    try:
        edge = subprocess.Popen([PY, str(HERE / "relay_edge.py"), "--udp-port", str(p), "--link", "127.0.0.1:%d" % (p + 1),
                                 "--secret", str(secret_file), "--log", str(edge_log), "--report", "1"],
                                creationflags=flags)
        procs.append(edge)
        server = subprocess.Popen([PY, str(HERE / "server.py"), "--port", str(p + 10), "--data", str(tmp / "data"), "--test-open-accounts",
                                   "--relay-udp-port", str(p), "--relay-edge", "127.0.0.1:%d" % (p + 1),
                                   "--relay-secret", str(secret_file), "--log", str(server_log)],
                                  creationflags=flags, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        procs.append(server)
        url = "http://127.0.0.1:%d" % (p + 10)
        check(wait_up(url, server), "server with an edge up")
        time.sleep(1.5)  # (linked)
        print("--- test_relay.py against it")
        check(asyncio.run(test_relay.main(url)), "test_relay.py with the edge")
        time.sleep(2.5)  # (the edge's counts)
        log = edge_log.read_text()
        here = sum(int(m) for m in re.findall(r"relayed (\d+) here", log))
        check(here >= 51, "edge: %d packets between UDP games relayed there, not through the server" % here)
        slog = server_log.read_text(encoding="utf-8", errors="replace")
        check("on UDP at the edge" in slog and "edge linked" in slog, "server: linked, saw the edge's games")

        # refused: a forged ticket, a link packet without the secret
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1)
        gid = bytes([2]) + secrets.token_bytes(5)
        good = rl.make_ticket(secret, gid)
        forged = good[:4] + bytes(12)
        s.sendto(b"SVRR\x01\x01" + gid + forged, ("127.0.0.1", p))
        try:
            s.recvfrom(64)
            check(False, "edge: a forged ticket is refused")
        except socket.timeout:
            check(True, "edge: a forged ticket is refused")
        expired = rl.make_ticket(secret, gid, now=time.time() - 2 * rl.TICKET_LIFE)
        s.sendto(b"SVRR\x01\x01" + gid + expired, ("127.0.0.1", p))
        try:
            s.recvfrom(64)
            check(False, "edge: an expired ticket is refused")
        except socket.timeout:
            check(True, "edge: an expired ticket is refused")
        s.sendto(b"SVRR\x01\x01" + gid + good, ("127.0.0.1", p))
        try:
            check(s.recvfrom(64)[0][:6] == b"SVRR\x01\x02", "edge: a good ticket is welcomed")
        except socket.timeout:
            check(False, "edge: a good ticket is welcomed")
        intruder = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        intruder.settimeout(1.5)
        intruder.sendto(rl.link_packet(b"not the secret at all!", rl.STATE, b""), ("127.0.0.1", p + 1))
        try:
            intruder.recvfrom(4096)
            check(False, "edge: a link packet without the secret is ignored")
        except socket.timeout:
            check(True, "edge: a link packet without the secret is ignored")
        # a second server with the secret (an old one left running, 22 Oct): while
        # the real one is linked the edge doesn't take it as home (no state back)
        second = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        second.settimeout(2.5)
        for _ in range(3):
            second.sendto(rl.link_packet(secret, rl.STATE, b""), ("127.0.0.1", p + 1))
            time.sleep(0.3)
        try:
            second.recvfrom(4096)
            check(False, "edge: a second server doesn't take the link while the first is linked")
        except socket.timeout:
            check(True, "edge: a second server doesn't take the link while the first is linked")
        for proc in procs:
            proc.terminate()
        for proc in procs:
            proc.wait(10)
        procs = []

        # the server on its own (no edge) still relays UDP itself
        server = subprocess.Popen([PY, str(HERE / "server.py"), "--port", str(p + 11), "--data", str(tmp / "data2"), "--test-open-accounts",
                                   "--relay-udp-port", str(p + 2)],
                                  creationflags=flags, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        procs.append(server)
        url = "http://127.0.0.1:%d" % (p + 11)
        check(wait_up(url, server), "server without an edge up")
        print("--- test_relay.py against it")
        check(asyncio.run(test_relay.main(url)), "test_relay.py without an edge")
    finally:
        for proc in procs:
            proc.terminate()
    print("relay edge: all ok" if ok else "relay edge: FAILED")
    print("(logs in %s)" % tmp)
    return ok


if __name__ == "__main__":
    sys.exit(0 if main() else 1)
