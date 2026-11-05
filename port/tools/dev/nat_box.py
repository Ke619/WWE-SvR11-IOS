#!/usr/bin/env python3
"""A NAT simulator for the online tests (standard library only, this PC's loopback only). For us: tools/dev is never
packaged.

Two games on one PC reach each other whatever their routers would do, so this puts each game behind a simulated
router. A game started with SVR2011_TEST_NAT_BOX=127.0.0.1:<inside> wraps everything on its P2P socket for the box
(6 bytes: the real far end's IPv4 and port, then the packet); the box sends it on from the router's "public" address -
a 127.x address of its own - by that router's rules, and wraps what the rules let back in. The outside is real UDP
between those addresses, so a test server's UDP relay and the box's STUN servers see real source addresses.

    py -3 nat_box.py --config nat.json [--log box.log]

The config: {"stun": ["127.30.0.1:3478", "127.30.0.2:3478"], "nats": [
    {"name": "A", "inside": 37100, "outside": "127.10.0.1",
     "mapping": "eim" | "adm" | "apdm",   (a new outside port: never / per far address / per far address and port)
     "filter": "eif" | "adf" | "apf",     (who gets in: anyone / addresses it sent to / addresses and ports it sent to)
     "ports": "preserve" | "seq" | "random",  (the outside port: the game's own if free / next one / any)
     "lease": 60,                         (seconds a quiet mapping lives; a packet out keeps it)
     "hairpin": false,                    (a packet to its own outside address comes back in)
     "lan": {"37700": "10.1.0.2"},        (client UDP port -> its LAN address: clients here reach each other's LAN address)
     "lan_reach": true,                   (false: LAN addresses go nowhere - separate subnets, a shared CGNAT)
     "natpmp": 37151,                     (optional: answers NAT-PMP there, SVR2011_TEST_NATPMP=127.0.0.1:37151)
     "natpmp_outside": "192.168.0.2"}]}   (optional: the outside address it claims - a router behind another router;
                                           its mappings then do nothing)

Names as in RFC 4787: eim + eif = full cone, eim + adf = restricted cone, eim + apf = port-restricted cone,
apdm = "symmetric" (apdm + preserve is what most Linux routers do: the same port while it's free).
"""
import argparse
import json
import random
import selectors
import socket
import struct
import sys
import time

MAGIC = 0x2112A442


def is_lan(ip):
    b = socket.inet_aton(ip)
    return b[0] == 10 or (b[0] == 172 and 16 <= b[1] < 32) or (b[0] == 192 and b[1] == 168) or (b[0] == 100 and 64 <= b[1] < 128)


class Mapping:
    def __init__(self, nat, client, sock, port):
        self.nat, self.client, self.sock, self.port = nat, client, sock, port
        self.used = time.time()
        self.addrs, self.ports = set(), set()
        self.forward = False  # (a NAT-PMP mapping: anyone gets in, it doesn't expire)


class Nat:
    def __init__(self, box, c):
        self.box, self.c = box, c
        self.name, self.outside = c["name"], c["outside"]
        self.mapping_kind, self.filter = c.get("mapping", "eim"), c.get("filter", "apf")
        self.port_kind = c.get("ports", "preserve")
        self.lease, self.hairpin = c.get("lease", 60), c.get("hairpin", False)
        self.lan = {int(k): v for k, v in c.get("lan", {}).items()}
        self.lan_reach = c.get("lan_reach", True)
        self.next_port = 40000
        self.maps = {}  # key -> Mapping
        self.by_port = {}
        self.clients = {}  # client's UDP port -> address it sent from
        self.stats = {"out": 0, "in": 0, "dropped_filter": 0, "dropped_lan": 0, "dropped_hairpin": 0, "mappings": 0}
        self.inside = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.inside.bind(("127.0.0.1", c["inside"]))
        box.sel.register(self.inside, selectors.EVENT_READ, ("inside", self))
        if c.get("natpmp"):
            self.pmp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.pmp.bind(("127.0.0.1", c["natpmp"]))
            box.sel.register(self.pmp, selectors.EVENT_READ, ("natpmp", self))

    def key(self, client, dst):
        # (per flow but the same port while it's free - Linux routers: seen from outside, one mapping; the filter
        # still goes by who it sent to)
        if self.mapping_kind == "eim" or self.port_kind == "preserve":
            return (client,)
        if self.mapping_kind == "adm":
            return (client, dst[0])
        return (client, dst[0], dst[1])

    def alloc(self, want):
        if self.port_kind == "preserve":
            tries = [want]
        elif self.port_kind == "seq":
            tries = []
        else:
            tries = [random.randint(20000, 60000) for _ in range(40)]
        for p in tries + list(range(self.next_port, self.next_port + 2000)):
            if p in self.by_port:
                continue
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.bind((self.outside, p))
            except OSError:
                s.close()
                continue
            if p >= self.next_port:
                self.next_port = p + 1
            return s, p
        raise RuntimeError("no outside port")

    def get_mapping(self, client, dst):
        k = self.key(client, dst)
        m = self.maps.get(k)
        if m:
            return m
        s, p = self.alloc(client[1])
        m = Mapping(self, client, s, p)
        self.maps[k] = m
        self.by_port[p] = m
        self.box.sel.register(s, selectors.EVENT_READ, ("outside", m))
        self.stats["mappings"] += 1
        self.box.log("%s: map %s:%d -> %s:%d (first to %s:%d)" % (self.name, client[0], client[1], self.outside, p, dst[0], dst[1]))
        return m

    def from_inside(self, data, client):
        if len(data) < 6:
            return
        dst = (socket.inet_ntoa(data[:4]), struct.unpack(">H", data[4:6])[0])
        payload = data[6:]
        self.clients[client[1]] = client
        if dst[0] == "255.255.255.255" or is_lan(dst[0]):
            self.to_lan(dst, payload, client)
            return
        if dst[0] == self.outside and not self.hairpin:
            self.stats["dropped_hairpin"] += 1
            return
        m = self.get_mapping(client, dst)
        m.used = time.time()
        m.addrs.add(dst[0])
        m.ports.add(dst)
        self.stats["out"] += 1
        try:
            m.sock.sendto(payload, dst)
        except OSError:
            pass

    def to_lan(self, dst, payload, client):
        src_ip = self.lan.get(client[1])
        if not self.lan_reach or not src_ip:
            self.stats["dropped_lan"] += 1
            return
        for port, ip in self.lan.items():
            if port == client[1] or port not in self.clients:
                continue
            if dst[0] == "255.255.255.255" or (dst[0] == ip):
                self.inside.sendto(socket.inet_aton(src_ip) + struct.pack(">H", client[1]) + payload, self.clients[port])
                return
        self.stats["dropped_lan"] += 1

    def from_outside(self, m, data, src):
        if not m.forward:
            ok = self.filter == "eif" or (self.filter == "adf" and src[0] in m.addrs) or (self.filter == "apf" and src in m.ports)
            if not ok:
                self.stats["dropped_filter"] += 1
                return
        self.stats["in"] += 1
        self.inside.sendto(socket.inet_aton(src[0]) + struct.pack(">H", src[1]) + data, m.client)

    def natpmp(self, data, src):
        claimed = self.c.get("natpmp_outside", self.outside)
        now = int(time.time()) & 0xFFFFFFFF
        if len(data) >= 2 and data[1] == 0:  # the outside address
            self.pmp.sendto(struct.pack(">BBHI", 0, 128, 0, now) + socket.inet_aton(claimed), src)
        elif len(data) >= 12 and data[1] == 1:  # UDP mapping: inside port, wanted outside port, lease
            inside, wanted, lease = struct.unpack(">HHI", data[4:12])
            self.pmp.sendto(struct.pack(">BBHIHHI", 0, 129, 0, now, inside, wanted, lease), src)
            self.box.log("%s: NAT-PMP UDP %d -> %d%s" % (self.name, inside, wanted, " (claims %s: does nothing)" % claimed if "natpmp_outside" in self.c else ""))
            if "natpmp_outside" in self.c or not lease:
                return
            client = ("127.0.0.1", inside)
            m = self.by_port.get(wanted)
            if not m:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.bind((self.outside, wanted))
                m = Mapping(self, client, s, wanted)
                self.maps[(client,)] = m
                self.by_port[wanted] = m
                self.box.sel.register(s, selectors.EVENT_READ, ("outside", m))
            m.forward = True

    def expire(self):
        now = time.time()
        for k, m in list(self.maps.items()):
            if not m.forward and now - m.used > self.lease:
                self.box.sel.unregister(m.sock)
                m.sock.close()
                del self.maps[k]
                del self.by_port[m.port]
                self.box.log("%s: mapping %s:%d expired" % (self.name, self.outside, m.port))


class Box:
    def __init__(self, cfg, log=None):
        self.sel = selectors.DefaultSelector()
        self.logf = open(log, "a", buffering=1) if log else None
        self.nats = [Nat(self, c) for c in cfg["nats"]]
        self.stun = []
        for a in cfg.get("stun", []):
            ip, port = a.split(":")
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((ip, int(port)))
            self.sel.register(s, selectors.EVENT_READ, ("stun", s))
            self.stun.append(s)

    def log(self, t):
        if self.logf:
            self.logf.write(time.strftime("%H:%M:%S ") + t + "\n")

    def answer_stun(self, s, data, src):
        if len(data) < 20 or struct.unpack(">H", data[:2])[0] != 1:
            return
        ip = struct.unpack(">I", socket.inet_aton(src[0]))[0] ^ MAGIC
        attr = struct.pack(">HHBBHI", 0x0020, 8, 0, 1, src[1] ^ (MAGIC >> 16), ip)
        s.sendto(struct.pack(">HH", 0x0101, len(attr)) + data[4:20] + attr, src)

    def step(self, timeout=0.2):
        for key, _ in self.sel.select(timeout):
            kind, obj = key.data
            try:
                data, src = key.fileobj.recvfrom(65536)
            except OSError:
                continue  # (Windows: a port unreachable comes back as an error)
            if kind == "inside":
                obj.from_inside(data, src)
            elif kind == "outside":
                obj.nat.from_outside(obj, data, src)
            elif kind == "stun":
                self.answer_stun(obj, data, src)
            elif kind == "natpmp":
                obj.natpmp(data, src)

    def run(self, until=None):
        last = time.time()
        while until is None or time.time() < until:
            self.step()
            if time.time() - last >= 1:
                last = time.time()
                for n in self.nats:
                    n.expire()

    def stats(self):
        return {n.name: n.stats for n in self.nats}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", required=True)
    ap.add_argument("--log")
    a = ap.parse_args()
    with open(a.config, encoding="utf-8") as f:
        cfg = json.load(f)
    box = Box(cfg, a.log)
    box.log("nat box up: " + ", ".join("%s %s %s %s" % (n.name, n.mapping_kind, n.filter, n.port_kind) for n in box.nats))
    try:
        last = time.time()
        while True:
            box.step()
            if time.time() - last >= 1:
                last = time.time()
                for n in box.nats:
                    n.expire()
                box.log("stats " + json.dumps(box.stats()))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
