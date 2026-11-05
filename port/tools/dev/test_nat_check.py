#!/usr/bin/env python3
"""The launcher's connection check (launcher/nat_check.c): every result, from fake STUN servers on loopback.

  python test_nat_check.py "<SvR2011 Launcher.exe>"

Each case: two STUN servers (127.30.0.1, 127.30.0.2) that report the addresses the case says, or stay quiet; the
launcher's --nat-check in a folder of its own. What the check says of Windows Firewall comes from its test aid here (the
real rules are read-only and depend on this PC); UPnP, from a log in the folder. Nothing leaves this PC.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading

COOKIE = 0x2112A442
A, B, V6_PORT = ("127.30.0.1", 34780), ("127.30.0.2", 34781), 34782


def answer(request, ip, port):
    """A STUN binding answer with XOR-MAPPED-ADDRESS."""
    value = struct.pack(">BBH", 0, 1, port ^ (COOKIE >> 16)) + struct.pack(">I", struct.unpack(">I", socket.inet_aton(ip))[0] ^ COOKIE)
    x = struct.pack(">HH", 0x0020, len(value)) + value
    return struct.pack(">HHI", 0x0101, len(x), COOKIE) + request[8:20] + x


class Fake(threading.Thread):
    """seen(from_address) -> (ip, port) to report, or None: quiet."""

    def __init__(self, family, address, seen):
        super().__init__(daemon=True)
        self.sock = socket.socket(family, socket.SOCK_DGRAM)
        self.sock.settimeout(0.1)
        self.sock.bind(address)
        self.seen, self.stop, self.asked = seen, False, 0

    def run(self):
        while not self.stop:
            try:
                data, frm = self.sock.recvfrom(600)
            except (socket.timeout, ConnectionResetError):
                continue
            if len(data) != 20 or data[:2] != b"\x00\x01" or struct.unpack(">I", data[4:8])[0] != COOKIE:
                continue
            self.asked += 1
            r = self.seen(frm)
            if r:
                self.sock.sendto(answer(data, *r), frm)

    def close(self):
        self.stop = True
        self.join()
        self.sock.close()


def run_check(exe, folder, a, b, v6=False, local="", fw="none", log=""):
    fakes = [Fake(socket.AF_INET, A, a), Fake(socket.AF_INET, B, b)]
    if v6:
        fakes.append(Fake(socket.AF_INET6, ("::1", V6_PORT), lambda f: ("1.2.3.4", 1)))
    logs = os.path.join(folder, "logs")
    os.makedirs(logs, exist_ok=True)
    with open(os.path.join(logs, "svr2011_test.log"), "w") as f:
        f.write(log)
    for f in fakes:
        f.start()
    env = dict(os.environ, SVR2011_TEST_STUN="%s:%d,%s:%d" % (A + B), SVR2011_TEST_NAT_FW=fw,
               SVR2011_TEST_NAT_LOCAL=local, SVR2011_TEST_STUN6=str(V6_PORT) if v6 else "")
    # (no IPv6 test server: p2p_stun "off" in the folder's settings keeps the check from asking the real one)
    try:
        out = subprocess.run([exe, "--nat-check", folder], env=env, capture_output=True, timeout=60).stdout
    finally:
        for f in fakes:
            f.close()
    text = out.decode("utf-16-le", "replace") if out[:2].count(b"\0") else out.decode("utf-8", "replace")
    fields = dict(x.split("=") for x in text.splitlines()[0].split())
    return {k: int(v) for k, v in fields.items()}, text


def main():
    exe = os.path.abspath(sys.argv[1])
    bad = 0
    pub = lambda port: lambda f: ("203.0.113.9", port)
    quiet = lambda f: None
    with tempfile.TemporaryDirectory() as folder:
        with open(os.path.join(folder, "svr2011.toml"), "w") as f:
            f.write('p2p_port = 36911\np2p_stun = "off"\n')
        cases = [
            ("no UDP", dict(a=quiet, b=quiet), dict(kind=0, v6=0), ["nothing answered", "relay", "another network"]),
            ("no UDP, IPv6", dict(a=quiet, b=quiet, v6=True), dict(kind=0, v6=1), ["only with players on IPv6"]),
            ("one answer", dict(a=pub(40000), b=quiet), dict(kind=1), ["only one test server", "can't tell"]),
            ("cone", dict(a=pub(40000), b=pub(40000)), dict(kind=3, step=0, mapped=0), ["one port for all", "most players"]),
            ("symmetric", dict(a=pub(40000), b=pub(40007)), dict(kind=4, step=7), ["new port for each", "forward UDP 36911"]),
            ("symmetric, IPv6", dict(a=pub(40000), b=pub(41000), v6=True), dict(kind=4, step=1000, v6=1), ["on IPv6 or with an open port"]),
            ("symmetric, shared address", dict(a=pub(40000), b=pub(40001), local="100.64.3.4"), dict(kind=4, shared=1), ["shares its address", "can't help"]),
            ("100.63 isn't shared", dict(a=pub(40000), b=pub(40001), local="100.63.255.1"), dict(kind=4, shared=0), ["forward UDP 36911"]),
            ("open", dict(a=lambda f: ("127.0.0.1", f[1]), b=lambda f: ("127.0.0.1", f[1]), local="127.0.0.1"), dict(kind=2), ["none in the way", "every player"]),
            ("symmetric, UPnP opened it", dict(a=pub(40000), b=pub(40001), log="i> 1 p2p: router port mapping UDP 36911 -> 192.0.2.5:36911 (UPnP)\ni> 2 p2p: router port mapping removed (UDP 36911)\n"), dict(kind=4, mapped=1), ["opened the game's port", "every player"]),
            ("symmetric, UPnP refused", dict(a=pub(40000), b=pub(40001), log="i> 1 p2p: router port mapping: none (no UPnP / NAT-PMP router answered, or it refused)\n"), dict(kind=4, mapped=-1), ["didn't open", "forward UDP 36911"]),
            ("firewall blocks", dict(a=pub(40000), b=pub(40000), fw="blocked"), dict(kind=3, fw=4), ["blocks the game", "Windows Security"]),
            ("firewall off", dict(a=pub(40000), b=pub(40000), fw="off"), dict(fw=1), ["Firewall: off"]),
            ("firewall allows", dict(a=pub(40000), b=pub(40000), fw="allowed"), dict(fw=2), ["lets the game in"]),
            ("firewall no rule", dict(a=pub(40000), b=pub(40000), fw="none"), dict(fw=3), ["no rule for the game"]),
        ]
        for name, args, want, words in cases:
            got, text = run_check(exe, folder, **args)
            wrong = [k for k, v in want.items() if got.get(k) != v] + [w for w in words if w not in text]
            if "203.0.113" in text:
                wrong.append("shows the public address")
            print(("ok    " if not wrong else "WRONG ") + name + ("" if not wrong else ": %s\n%s" % (wrong, text)))
            bad += bool(wrong)
    print("OK" if not bad else "%d WRONG" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
