#!/usr/bin/env python3
"""The online match relay's UDP half on its own (see relay_link.py): run on
the VPS that takes the players' UDP, so packets between UDP players go
player -> VPS -> player instead of through the PC with the server.

    python relay_edge.py --udp-port 36500 --link 10.8.0.1:36501 --secret relay_secret.txt [--log edge.log]

--udp-port: where the players' UDP comes in (the port the server tells the
game, its --relay-udp-port). --link: this end of the link to the server
(the tunnel address; never a public one). The server runs with
--relay-edge <that address> and the same --relay-secret file.

Python 3.8+, nothing else. Keeps nothing on disk.
"""
import argparse
import asyncio
import socket
import struct
import sys
import time

import relay_link as rl

MAGIC = b"SVRR"
VERSION = 1
REGISTER, WELCOME, FORWARD, FROM = 1, 2, 3, 4
BROADCAST = b"\xff" * 6
MAX_GAMES = 2048
MAX_BROADCAST = 256
RATE = 400          # packets per second one game may send (a match: ~60)
UDP_QUIET = 20.0    # a game not heard from for this long: its packets go home (its https download)
HOME_QUIET = 5.0    # no keepalive from home for this long: nothing goes home


def packet(kind, body):
    return MAGIC + bytes([VERSION, kind]) + body


def address(ip, port):
    try:
        return socket.inet_aton(ip) + struct.pack(">H", port)
    except OSError:
        return b"\0" * 6


class UdpGame:
    def __init__(self, gid, addr, account=0):
        self.id, self.addr, self.account = gid, addr, account  # (account 0: a first-format ticket)
        self.seen = time.monotonic()
        self.window, self.count = 0.0, 0
        self.sent = 0  # (forwards since the last state home)

    def fresh(self):
        return time.monotonic() - self.seen < UDP_QUIET

    def allowed(self):
        now = time.monotonic()
        if now - self.window >= 1.0:
            self.window, self.count = now, 0
        self.count += 1
        return self.count <= RATE


class Endpoint(asyncio.DatagramProtocol):
    def __init__(self, handler, log, on_error=None):
        self.handler, self.log, self.on_error = handler, log, on_error

    def datagram_received(self, data, addr):
        try:
            self.handler(data, addr)
        except Exception as e:  # (one bad datagram never stops the relay)
            self.log("edge: packet from %s:%d dropped (%s)" % (addr[0], addr[1], e))

    def error_received(self, exc):
        if self.on_error:
            self.on_error()


class Edge:
    def __init__(self, secret, log):
        self.secret, self.log = secret, log
        self.games = {}     # id -> UdpGame
        self.by_addr = {}   # (ip, port) -> id
        self.players = None  # the players' UDP socket
        self.link = None     # the link socket
        self.home = None     # home's address (its last good datagram)
        self.home_seen = 0.0
        self.other_logged = 0.0  # (a second server's link attempts: logged once a minute)
        self.relayed = 0     # since the last state sent home
        self.total_local = self.total_home = self.total_from_home = 0
        self.network = rl.AUTO   # the server's match network (its keepalives)
        self.pairs = rl.PairLimit()
        self.kicked = {}     # account -> when (unix): banned - its tickets made until then are refused

    # -- home
    def home_up(self):
        return self.home and time.monotonic() - self.home_seen < HOME_QUIET

    def to_home(self, kind, body):
        if self.home_up():
            self.link.sendto(rl.link_packet(self.secret, kind, body), self.home)
            return True
        return False

    def send_state(self):
        fresh = [g for g in self.games.values() if g.fresh()]
        games = {g.id: (g.account, address(*g.addr), g.sent) for g in fresh}
        if self.to_home(rl.STATE, rl.state_body(self.relayed, [g.id for g in fresh], games)):
            self.relayed = 0
            for g in fresh:
                g.sent = 0

    def kick(self, accounts):
        now = time.time()
        for aid in accounts:
            if not aid:
                continue
            first = aid not in self.kicked
            self.kicked[aid] = now
            gone = [g for g in self.games.values() if g.account == aid]
            for g in gone:
                self.games.pop(g.id, None)
                self.by_addr.pop(g.addr, None)
            if first or gone:
                self.log("edge: account %d banned - %d game(s) dropped, its tickets refused" % (aid, len(gone)))
        for aid in [a for a, at in self.kicked.items() if now - at > 2 * rl.TICKET_LIFE]:
            del self.kicked[aid]  # (every ticket made before it has expired)

    def on_link(self, data, addr):
        got = rl.read_link(self.secret, data)
        if not got:
            return
        kind, body = got
        # One server: while it's linked (keepalives every second), another one -
        # an old server left running with --relay-edge (22 Oct: the PC's, next to
        # the VPS core) - is ignored; taking whichever spoke last flipped home every
        # second and sent half the players' packets and searches to the wrong
        # server. (A restarted core takes over once the old link is HOME_QUIET.)
        if self.home_up() and addr != self.home:
            now = time.monotonic()
            if now - self.other_logged >= 60:
                self.other_logged = now
                self.log("edge: a second server tried to link from %s:%d - ignored (linked: %s:%d)" % (addr + self.home))
            return
        if not self.home_up() or addr != self.home:
            self.log("edge: server linked from %s:%d" % addr)
        self.home, self.home_seen = addr, time.monotonic()
        if kind == rl.STATE:
            network = rl.read_home_state(body)
            if network != self.network:
                self.log("edge: match network %s" % {rl.AUTO: "auto", rl.RELAY: "relay", rl.P2P: "p2p"}[network] +
                         (" - one game to another: %d packets, then %g a second (no match data)" %
                          (rl.PAIR_BURST, rl.PAIR_RATE) if network == rl.P2P else ""))
                self.network = network
            self.send_state()
        elif kind == rl.KICK:
            self.kick(rl.read_kick(body))
        elif kind == rl.FORWARD and len(body) > 6:
            to, out = body[:6], body[6:]
            g = self.games.get(to)
            if g and g.fresh():
                self.players.sendto(out, g.addr)
                self.total_from_home += 1
            else:
                self.to_home(rl.FORWARD, body)  # (home: its https download)
        elif kind == rl.SEARCH and len(body) > 6:
            self.search_local(body[:6], body[6:])

    # -- players
    def search_local(self, sender, out):
        fresh = sorted((g for g in self.games.values() if g.fresh() and g.id != sender), key=lambda g: -g.seen)
        for g in fresh[:MAX_BROADCAST]:
            self.players.sendto(out, g.addr)

    def on_player(self, data, addr):
        if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION:
            return
        kind = data[5]
        if kind == REGISTER and len(data) >= 6 + 6 + 16:
            gid, ticket = data[6:12], data[12:28]
            t = rl.read_ticket(self.secret, gid, ticket)
            if not t:
                return
            account, made = t
            if account in self.kicked and made <= self.kicked[account]:
                return  # (banned since: a new ticket needs a sign-in it can't have)
            g = self.games.get(gid)
            if not g or g.addr != addr or g.account != account:
                if not g and len(self.games) >= MAX_GAMES:
                    self.forget_quiet()
                    if len(self.games) >= MAX_GAMES:
                        return
                if g:
                    self.by_addr.pop(g.addr, None)
                g = UdpGame(gid, addr, account)
                self.games[gid] = g
                self.by_addr[addr] = gid
                self.log("edge: %s (account %s) on UDP from %s:%d, %d game(s)" % (
                    gid.hex(), account or "? - an older ticket", addr[0], addr[1], len(self.games)))
                g.seen = time.monotonic()
                self.send_state()  # (home sends its packets here from now on - before the welcome)
            g.seen = time.monotonic()
            self.players.sendto(packet(WELCOME, address(addr[0], addr[1])), addr)
        elif kind == FORWARD and len(data) > 12:
            gid = self.by_addr.get(addr)
            g = self.games.get(gid) if gid else None
            if not g or g.addr != addr or not g.allowed():
                return
            g.seen = time.monotonic()
            to, inner = data[6:12], data[12:]
            out = packet(FROM, address(addr[0], addr[1]) + inner)
            if to == BROADCAST:
                self.search_local(gid, out)
                self.to_home(rl.SEARCH, gid + out)
                return
            g.sent += 1  # (what it tries: in p2p a game sending match data shows on the dashboard, held back or not)
            if self.network == rl.P2P and not self.pairs.allowed(gid, to):
                return  # (p2p: no match data through the relay - whatever the game was told)
            t = self.games.get(to)
            if t and t.fresh():
                self.players.sendto(out, t.addr)
                self.total_local += 1
            elif self.to_home(rl.FORWARD, to + out):
                self.total_home += 1
            else:
                return
            self.relayed += 1

    def forget_quiet(self):
        for gid in [gid for gid, g in self.games.items() if time.monotonic() - g.seen > 120]:
            self.by_addr.pop(self.games.pop(gid).addr, None)

    async def run(self, udp_port, link_addr, report=60):
        loop = asyncio.get_running_loop()

        async def open_players():
            if self.players:
                self.players.close()
            self.players, _ = await loop.create_datagram_endpoint(
                lambda: Endpoint(self.on_player, self.log, again_players), local_addr=("0.0.0.0", udp_port))

        async def open_link():
            if self.link:
                self.link.close()
            self.link, _ = await loop.create_datagram_endpoint(
                lambda: Endpoint(self.on_link, self.log, again_link), local_addr=link_addr)
        again_players = rl.reopen_on_windows(open_players, self.log, "edge: players' port")
        again_link = rl.reopen_on_windows(open_link, self.log, "edge: link")
        await open_players()
        await open_link()
        self.log("edge: players' UDP on port %d, link on %s:%d" % (udp_port, link_addr[0], link_addr[1]))
        last_report = time.monotonic()
        while True:
            await asyncio.sleep(1.0)
            self.send_state()
            if time.monotonic() - last_report >= report:
                last_report = time.monotonic()
                self.forget_quiet()
                if self.total_local or self.total_home or self.total_from_home:
                    self.log("edge: relayed %d here, %d to the server, %d from it; %d game(s)%s" % (
                        self.total_local, self.total_home, self.total_from_home,
                        sum(g.fresh() for g in self.games.values()), "" if self.home_up() else "; server not linked"))
                    self.total_local = self.total_home = self.total_from_home = 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--udp-port", type=int, required=True)
    ap.add_argument("--link", required=True, help="host:port of this end of the link (the tunnel address)")
    ap.add_argument("--secret", required=True, help="the file with the secret shared with the server")
    ap.add_argument("--log", default=None)
    ap.add_argument("--report", type=float, default=60, help="seconds between the relayed counts in the log")
    args = ap.parse_args()
    out = open(args.log, "a", encoding="utf-8", buffering=1) if args.log else sys.stdout

    def log(msg):
        out.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
        out.flush()
    host, port = args.link.rsplit(":", 1)
    try:
        asyncio.run(Edge(rl.read_secret(args.secret), log).run(args.udp_port, (host, int(port)), args.report))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
