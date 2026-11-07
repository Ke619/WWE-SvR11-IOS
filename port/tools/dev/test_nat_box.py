#!/usr/bin/env python3
"""The NAT box's own check, and what each pair of routers can do: two small clients behind two simulated routers each
learn their public address from STUN, tell the other (the relay's job in the game), then both send to it every 100 ms
for a few seconds, as the game's punching does. Prints the matrix; fails if a cell differs from what those NAT rules
must give. Standard library only, this PC's loopback only. py -3 test_nat_box.py [--seconds 1.5]"""
import argparse
import os
import random
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nat_box  # noqa: E402

MAGIC = 0x2112A442
STUN = ("127.30.0.1", 3478)

# (mapping, filter, ports): name
KINDS = {
    "full": ("eim", "eif", "random"),
    "restricted": ("eim", "adf", "random"),
    "port-restricted": ("eim", "apf", "random"),
    "linux": ("apdm", "apf", "preserve"),  # (per flow, but the same port while it's free: most home routers)
    "symmetric": ("apdm", "apf", "random"),
    "symmetric-open": ("apdm", "eif", "random"),
}


def expected(a, b):
    """Direct with plain punching to the port STUN reported, and answers to whoever was heard from?"""
    sym = lambda k: k in ("symmetric", "symmetric-open")  # (its port towards the peer isn't the one STUN saw)
    flt = lambda k: KINDS[k][1]

    def passes(x, y):  # (x's packet, sent to the port y's STUN reported, gets in)
        if flt(y) == "eif":
            return True
        if sym(y):
            return False  # (that mapping only ever sent to STUN)
        return flt(y) == "adf" or not sym(x)

    def answered(x, y):  # (y heard x: its answer, from y's port towards x, gets through x's router)
        return not sym(y) or flt(x) in ("eif", "adf")

    return (passes(a, b) and answered(a, b)) or (passes(b, a) and answered(b, a))


class Client:
    def __init__(self, inside, port):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", port))
        self.s.setblocking(False)
        self.inside = ("127.0.0.1", inside)
        self.public = None
        self.heard_from = None

    def send(self, to, data):
        self.s.sendto(socket.inet_aton(to[0]) + struct.pack(">H", to[1]) + data, self.inside)

    def poll(self):
        while True:
            try:
                data, _ = self.s.recvfrom(4096)
            except (BlockingIOError, ConnectionResetError, OSError):
                return
            src = (socket.inet_ntoa(data[:4]), struct.unpack(">H", data[4:6])[0])
            data = data[6:]
            if len(data) >= 32 and struct.unpack(">H", data[:2])[0] == 0x0101:
                port = struct.unpack(">H", data[26:28])[0] ^ (MAGIC >> 16)
                ip = struct.unpack(">I", data[28:32])[0] ^ MAGIC
                self.public = (socket.inet_ntoa(struct.pack(">I", ip)), port)
            elif data == b"punch":
                self.heard_from = src
                self.send(src, b"reply")  # (the way the game answers a hello)
            elif data == b"reply":
                self.got_reply = True
                self.heard_from = src
    got_reply = False


def cell(a, b, seconds, base):
    cfg = {"stun": [], "nats": []}
    for name, kind, outside, inside in (("A", a, "127.10.0.1", base), ("B", b, "127.20.0.1", base + 1)):
        m, f, p = KINDS[kind]
        cfg["nats"].append({"name": name, "inside": inside, "outside": outside, "mapping": m, "filter": f, "ports": p,
                            "lease": 30, "hairpin": False})
    box = nat_box.Box(cfg)
    stun = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stun.bind(STUN)
    stun.setblocking(False)
    ca, cb = Client(base, base + 10), Client(base + 1, base + 11)
    req = struct.pack(">HHI", 1, 0, MAGIC) + bytes(random.getrandbits(8) for _ in range(12))
    try:
        t0 = time.time()
        ca.send(STUN, req)
        cb.send(STUN, req)
        while time.time() - t0 < 0.5 and not (ca.public and cb.public):
            box.step(0.01)
            try:
                d, src = stun.recvfrom(4096)
                box.answer_stun(stun, d, src)
            except (BlockingIOError, ConnectionResetError, OSError):
                pass
            ca.poll(), cb.poll()
        assert ca.public and cb.public, "no STUN answer"
        end, last = time.time() + seconds, 0
        while time.time() < end and not (ca.got_reply and cb.got_reply):
            if time.time() - last >= 0.1:
                last = time.time()
                for me, other in ((ca, cb), (cb, ca)):
                    me.send(other.public, b"punch")
                    if me.heard_from and me.heard_from != other.public:  # (as the game: it follows who it heard)
                        me.send(me.heard_from, b"punch")
            box.step(0.01)
            ca.poll(), cb.poll()
        return ca.got_reply and cb.got_reply
    finally:
        for s in (ca.s, cb.s, stun):
            s.close()
        for key in list(box.sel.get_map().values()):
            key.fileobj.close()
        box.sel.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=float, default=1.5)
    a = ap.parse_args()
    names = list(KINDS)
    bad = 0
    width = max(len(n) for n in names) + 2
    print(" " * width + "".join(n.ljust(width) for n in names))
    base = 37300
    for x in names:
        row = x.ljust(width)
        for y in names:
            got = cell(x, y, a.seconds, base)
            want = expected(x, y)
            row += (("direct" if got else "-") + ("" if got == want else " WRONG")).ljust(width)
            bad += got != want
        print(row)
    print("OK" if not bad else "%d cell(s) differ from the rules" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
