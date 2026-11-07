"""
The online match relay, part of the Community Creations server (server.py),
for players who can't reach each other directly (VPNs, mobile networks,
strict NATs).

Games play peer to peer (port/src/p2p.cpp). A signed-in game keeps two
streams open with the server, so it works anywhere the server's https does
(the game's UDP never has to reach it):
    GET  <base>/api/relay/<id>   the server's packets for this game, as they come
    POST <base>/api/relay/<id>   the game's packets (one chunked request)
<id> is the game's 6-byte hardware id (12 hex digits). Both carry frames:
a 16-bit big-endian length, then a packet ("SVRR" + version 1 + type):
    1 register  game -> relay   id[6]                 (keeps the upload alive)
      2 welcome relay -> game   ip[4] port[2]         (its address as seen here)
    3 forward   game -> relay   to[6] packet          (to ff..ff: every game here)
      4 from    relay -> game   ip[4] port[2] packet  (the sender's address)
A zero-length frame is a keepalive. The relay never reads the game's
packets and keeps nothing on disk.

UDP (--relay-udp-port): the same packets, one per datagram, no length -
over TCP one lost segment held back every later packet, and online matches
step only once every player's input is in. A game asks for a ticket over
https,
    GET  <base>/api/relay/<id>/udp   -> {"port": p, "ticket": hex}
registers from its P2P socket with it every few seconds,
    1 register  game -> relay   id[6] ticket[16]
and is answered with a welcome; from then on its forwards come over UDP and
packets for it go there (the https download keeps working: a game whose UDP
goes quiet for 20 s gets its packets there again).

With an edge (--relay-edge, relay_edge.py on the VPS that takes the UDP):
the tickets are signed (relay_link.py) and the UDP half runs there; this
server keeps the https streams and passes packets to and from the edge over
the link - between two UDP players they never come here.
"""
import asyncio
import os
import re
import socket
import struct
import time

from aiohttp import web

import relay_link as rl

MAGIC = b"SVRR"
VERSION = 1
REGISTER, WELCOME, FORWARD, FROM = 1, 2, 3, 4
BROADCAST = b"\xff" * 6
MAX_GAMES = 2048
MAX_BROADCAST = 256    # the most recently seen games a search reaches
RATE = 400             # packets per second one game may send (a match: ~60)
QUEUE = 1024           # packets waiting for a slow game (then dropped)
KEEPALIVE = 5.0        # seconds between keepalives on a quiet download
TICKET_LIFE = 3600.0   # seconds a UDP ticket may register
UDP_QUIET = 20.0       # a UDP game not heard from for this long: back to its https download


def packet(kind, body):
    return MAGIC + bytes([VERSION, kind]) + body


def frame(data):
    return struct.pack(">H", len(data)) + data


class Rate:
    """A game's forwards a second, over the last few seconds (the dashboard)."""
    SPAN = 5

    def __init__(self):
        self.secs = {}  # whole second -> packets

    def add(self, n=1, now=None):
        s = int(now or time.monotonic())
        self.secs[s] = self.secs.get(s, 0) + n
        if len(self.secs) > 2 * self.SPAN:
            for k in [k for k in self.secs if k < s - self.SPAN]:
                del self.secs[k]

    def per_second(self, now=None):
        s = int(now or time.monotonic())
        return round(sum(n for k, n in self.secs.items() if s - self.SPAN <= k < s) / self.SPAN, 1)


class Game:
    def __init__(self, gid, account, ip):
        self.id, self.account, self.ip = gid, account, ip
        self.queue = asyncio.Queue(QUEUE)
        self.seen = time.monotonic()
        self.since = time.time()
        self.window, self.count = 0.0, 0
        self.rate = Rate()

    def put(self, data):
        try:
            self.queue.put_nowait(data)
        except asyncio.QueueFull:
            pass  # (a game that stopped reading: it loses packets, not the server's memory)

    def allowed(self):
        now = time.monotonic()
        if now - self.window >= 1.0:
            self.window, self.count = now, 0
        self.count += 1
        return self.count <= RATE


class UdpGame:
    def __init__(self, gid, account, addr):
        self.id, self.account, self.addr = gid, account, addr
        self.seen = time.monotonic()
        self.since = time.time()
        self.window, self.count = 0.0, 0
        self.rate = Rate()

    def fresh(self):
        return time.monotonic() - self.seen < UDP_QUIET

    allowed = Game.allowed


class UdpEndpoint(asyncio.DatagramProtocol):
    def __init__(self, relay, on_error=None):
        self.relay, self.on_error = relay, on_error

    def connection_made(self, transport):
        self.relay.transport = transport

    def datagram_received(self, data, addr):
        try:
            self.relay.on_datagram(data, addr)
        except Exception as e:  # (one bad datagram never stops the relay)
            self.relay.log("relay: udp packet from %s:%d dropped (%s)" % (addr[0], addr[1], e))

    def error_received(self, exc):
        if self.on_error:
            self.on_error()  # (Windows: opened again - rl.reopen_on_windows)


class LinkEndpoint(asyncio.DatagramProtocol):
    def __init__(self, relay, on_error=None):
        self.relay, self.on_error = relay, on_error

    def connection_made(self, transport):
        self.relay.link = transport

    def datagram_received(self, data, addr):
        try:
            self.relay.on_link(data)
        except Exception as e:
            self.relay.log("relay: link packet dropped (%s)" % e)

    def error_received(self, exc):
        # (the edge not up yet: the keepalive tries again; Windows: opened again - rl.reopen_on_windows)
        if self.on_error:
            self.on_error()


class EdgeGame:
    """One of the edge's UDP games, as its state says (an older edge: only the id)."""

    def __init__(self, gid):
        self.id, self.account, self.addr = gid, 0, None
        self.since = time.time()
        self.rate = Rate()


class Relay:
    def __init__(self, service, log, udp_port=0, edge=None, secret=None, network=None):
        self.service, self.log = service, log
        self.edge, self.secret = edge, secret  # (host, port) of relay_edge.py's link; its shared secret
        self.network = network or (lambda: "auto")  # the server's match_network (p2p: no match data relayed)
        self.pairs = rl.PairLimit()
        self.kicked = {}                        # account id -> when it was banned (this run)
        self.checked = set()                    # the edge's accounts looked up this run (banned since?)
        self.names = {}                         # account id -> (name, when looked up)
        self.link = None                        # the link socket
        self.edge_games = set()                 # ids of the edge's UDP games (its last state)
        self.edge_info = {}                     # id -> EdgeGame
        self.edge_seen = 0.0
        self.ticket_names = {}                  # id -> account name (the edge's games in the log)
        self.games = {}  # id -> Game (with a download open)
        self.relayed = 0
        self.sessions = 0  # games that came on since the start
        self.udp_port = udp_port
        self.transport = None   # (the UDP socket, once open)
        self.tickets = {}       # ticket -> (game id, account, expiry)
        self.udp_games = {}     # id -> UdpGame
        self.udp_by_addr = {}   # (ip, port) -> id

    async def start_udp(self, app):
        if self.edge:
            loop = asyncio.get_running_loop()

            async def open_link():
                if self.link:
                    self.link.close()
                await loop.create_datagram_endpoint(lambda: LinkEndpoint(self, again), remote_addr=self.edge)
            again = rl.reopen_on_windows(open_link, self.log, "relay: the link")
            await open_link()
            keepalive = asyncio.ensure_future(self.keep_linked())
            self.log("relay: UDP on the edge %s:%d (players' port %d)" % (self.edge[0], self.edge[1], self.udp_port))
            yield
            keepalive.cancel()
            if self.link:
                self.link.close()
            return
        if self.udp_port:
            loop = asyncio.get_running_loop()

            async def open_udp():
                if self.transport:
                    self.transport.close()
                await loop.create_datagram_endpoint(lambda: UdpEndpoint(self, again), local_addr=("0.0.0.0", self.udp_port))
            again = rl.reopen_on_windows(open_udp, self.log, "relay: players' port")
            await open_udp()
            self.log("relay: UDP on port %d" % self.udp_port)
        yield
        if self.transport:
            self.transport.close()

    # -- the edge
    def edge_up(self):
        return self.link and time.monotonic() - self.edge_seen < 3.0

    def p2p(self):
        return self.network() == "p2p"

    def name_of(self, account_id):
        if not account_id:
            return None
        hit = self.names.get(account_id)
        if hit and time.monotonic() - hit[1] < 60:
            return hit[0]
        a = self.service.accounts.get(account_id)
        if len(self.names) > 4 * MAX_GAMES:
            self.names = {}
        self.names[account_id] = (a["name"] if a else None, time.monotonic())
        return self.names[account_id][0]

    def ban(self, account_id, why="banned"):
        """A banned (or deleted) account: its relay streams end, its UDP games go, the edge drops it and refuses
        its tickets."""
        self.kicked[account_id] = time.time()
        self.names.pop(account_id, None)
        gone = 0
        for g in [g for g in self.games.values() if g.account["id"] == account_id]:
            del self.games[g.id]
            g.put(None)
            gone += 1
        for g in [g for g in self.udp_games.values() if g.account["id"] == account_id]:
            self.udp_games.pop(g.id, None)
            self.udp_by_addr.pop(g.addr, None)
            gone += 1
        if gone:
            self.service.stats.relay_games(len(self.games))
        if self.edge and self.link:
            self.link.sendto(rl.link_packet(self.secret, rl.KICK, rl.kick_body([account_id])))
        self.log("relay: account %d %s - %d of its game(s) off the relay here%s" % (
            account_id, why, gone, "; the edge told (it drops its UDP games there)" if self.edge else ""))

    def unban(self, account_id):
        self.kicked.pop(account_id, None)  # (the edge refuses only tickets made before the ban: a new one works)

    async def keep_linked(self):
        while True:
            self.link.sendto(rl.link_packet(self.secret, rl.STATE, rl.home_state(rl.NETWORKS.get(self.network(), rl.AUTO))))
            await asyncio.sleep(1.0)
            if self.edge_games and not self.edge_up():
                self.log("relay: the edge went quiet")
                self.edge_games = set()
                self.edge_info = {}

    def edge_name(self, gid):
        """Whose edge game this is, for the log and the dashboard - never a bare "?"."""
        e = self.edge_info.get(gid)
        name = self.name_of(e.account) if e and e.account else None
        if name:
            return name
        if gid in self.ticket_names:
            return self.ticket_names[gid]
        if e and e.account:
            return "account %d (gone)" % e.account
        return "not known: an older ticket (from before this server started, or an older edge)"

    def on_link(self, data):
        got = rl.read_link(self.secret, data)
        if not got:
            return
        kind, body = got
        if not self.edge_up():
            self.log("relay: edge linked")
        self.edge_seen = time.monotonic()
        if kind == rl.STATE:
            relayed, ids, extra = rl.read_state_full(body)
            ids = set(ids)
            for gid in ids:
                e = self.edge_info.get(gid)
                if e is None:
                    e = self.edge_info[gid] = EdgeGame(gid)
                if gid in extra:
                    e.account, addr, sent = extra[gid]
                    e.addr = (socket.inet_ntoa(addr[:4]), struct.unpack(">H", addr[4:6])[0])
                    if sent:
                        e.rate.add(sent)
            for gid in [g for g in self.edge_info if g not in ids]:
                del self.edge_info[gid]
            for gid in ids - self.edge_games:
                e = self.edge_info[gid]
                self.log("relay: %s (%s) on UDP at the edge%s" % (
                    gid.hex(), self.edge_name(gid), " from %s:%d" % e.addr if e.addr else ""))
            self.edge_games = ids
            # (banned since its ticket was made: told again - an edge restart forgets; this server's own restart too)
            kick = set()
            for e in self.edge_info.values():
                if not e.account:
                    continue
                if e.account in self.kicked:
                    kick.add(e.account)
                elif e.account not in self.checked:
                    self.checked.add(e.account)
                    a = self.service.accounts.get(e.account)
                    if a and a["banned"]:
                        self.kicked[e.account] = time.time()
                        kick.add(e.account)
            if kick:
                self.link.sendto(rl.link_packet(self.secret, rl.KICK, rl.kick_body(kick)))
            if relayed:
                self.relayed += relayed
                self.service.stats.count("relayed", relayed)
        elif kind == rl.FORWARD and len(body) > 6:
            to, out = body[:6], body[6:]
            if to in self.games:
                self.games[to].put(out)  # (https only: the edge had it, or hasn't)
        elif kind == rl.SEARCH and len(body) > 6:
            self.broadcast(body[:6], body[6:], from_edge=True)

    def deliver(self, to, out):
        """A packet for game `to`: over UDP while it registers there, else its download."""
        if self.edge and to in self.edge_games and self.edge_up():
            self.link.sendto(rl.link_packet(self.secret, rl.FORWARD, to + out))
            return True
        g = self.udp_games.get(to)
        if g and g.fresh() and self.transport:
            self.transport.sendto(out, g.addr)
            return True
        if to in self.games:
            self.games[to].put(out)
            return True
        return False

    def broadcast(self, sender, out, from_edge=False):
        if self.edge and not from_edge and self.edge_up():
            self.link.sendto(rl.link_packet(self.secret, rl.SEARCH, sender + out))
        skip = self.edge_games if self.edge_up() else set()  # (the edge reaches those)
        recent = {}
        for g in list(self.games.values()) + [g for g in self.udp_games.values() if g.fresh()]:
            if g.id != sender and g.id not in skip and (g.id not in recent or g.seen > recent[g.id]):
                recent[g.id] = g.seen
        for gid in sorted(recent, key=lambda i: -recent[i])[:MAX_BROADCAST]:
            self.deliver(gid, out)

    async def udp_ticket(self, request):
        account = self.service.account(request)
        gid = self.game_id(request)
        if not account:
            return web.Response(status=401)
        if not gid or not self.udp_port:
            return web.Response(status=404)
        if self.edge:
            self.ticket_names[gid] = account["name"]
            if len(self.ticket_names) > 4 * MAX_GAMES:
                self.ticket_names = {gid: account["name"]}
            return web.json_response({"port": self.udp_port, "ticket": rl.make_ticket(self.secret, gid, account["id"]).hex()})
        now = time.monotonic()
        for t in [t for t, (_, _, exp) in self.tickets.items() if exp < now]:
            del self.tickets[t]
        ticket = os.urandom(16)
        self.tickets[ticket] = (gid, account, now + TICKET_LIFE)
        return web.json_response({"port": self.udp_port, "ticket": ticket.hex()})

    def on_datagram(self, data, addr):
        if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION:
            return
        kind = data[5]
        if kind == REGISTER and len(data) >= 6 + 6 + 16:
            gid, ticket = data[6:12], data[12:28]
            t = self.tickets.get(ticket)
            if not t or t[0] != gid or t[2] < time.monotonic() or t[1]["id"] in self.kicked:
                return
            g = self.udp_games.get(gid)
            if not g or g.addr != addr:
                if g:
                    self.udp_by_addr.pop(g.addr, None)
                g = UdpGame(gid, t[1], addr)
                self.udp_games[gid] = g
                self.udp_by_addr[addr] = gid
                self.log("relay: %s (%s) on UDP from %s:%d" % (gid.hex(), t[1]["name"], addr[0], addr[1]))
            g.seen = time.monotonic()
            self.transport.sendto(packet(WELCOME, self.address(addr[0], addr[1])), addr)
        elif kind == FORWARD and len(data) > 12:
            gid = self.udp_by_addr.get(addr)
            g = self.udp_games.get(gid) if gid else None
            if not g or g.addr != addr or not g.allowed():
                return
            g.seen = time.monotonic()
            to, inner = data[6:12], data[12:]
            out = packet(FROM, self.address(addr[0], addr[1]) + inner)
            if to == BROADCAST:
                self.broadcast(gid, out)
                return
            g.rate.add()  # (what it tries: in p2p a game sending match data shows on the dashboard, held back or not)
            if self.p2p() and not self.pairs.allowed(gid, to):
                return  # (p2p: no match data through the relay - whatever the game was told)
            if self.deliver(to, out):
                self.relayed += 1
                self.service.stats.count("relayed")

    @staticmethod
    def game_id(request):
        gid = request.match_info["id"].lower()
        return bytes.fromhex(gid) if re.fullmatch(r"[0-9a-f]{12}", gid) else None

    def address(self, ip, port=0):
        try:
            return socket.inet_aton(ip) + struct.pack(">H", port)
        except OSError:
            return b"\0" * 6

    async def download(self, request):
        account = self.service.account(request)
        gid = self.game_id(request)
        if not account:
            return web.Response(status=401)
        if not gid:
            return web.Response(status=400)
        if gid not in self.games and len(self.games) >= MAX_GAMES:
            return web.Response(status=503)
        ip = self.service.client_ip(request)
        old = self.games.get(gid)
        if old and old.account["id"] != account["id"]:
            return web.Response(status=409)  # (someone else's game id)
        game = Game(gid, account, ip)
        self.games[gid] = game
        if old:
            old.put(None)  # (a reconnect: the old download ends)
        resp = web.StreamResponse(headers={"Content-Type": "application/octet-stream",
                                           "Cache-Control": "no-store", "X-Accel-Buffering": "no"})
        await resp.prepare(request)
        self.log("relay: %s (%s) on from %s, %d game(s)" % (gid.hex(), account["name"], ip, len(self.games)))
        if not old:
            self.sessions += 1
        self.service.stats.relay_games(len(self.games), came_on=not old)
        try:
            await resp.write(frame(packet(WELCOME, self.address(ip))))
            while True:
                try:
                    data = await asyncio.wait_for(game.queue.get(), KEEPALIVE)
                except asyncio.TimeoutError:
                    data = b""
                if data is None:
                    break
                out = frame(data)
                while not game.queue.empty() and len(out) < 16384:  # (what's waiting goes together)
                    more = game.queue.get_nowait()
                    if more is None:
                        break
                    out += frame(more)
                await resp.write(out)
        except (ConnectionResetError, asyncio.CancelledError, RuntimeError):
            pass
        finally:
            if self.games.get(gid) is game:
                del self.games[gid]
                self.service.stats.relay_games(len(self.games))
                self.log("relay: %s off, %d game(s)" % (gid.hex(), len(self.games)))
        return resp

    async def upload(self, request):
        account = self.service.account(request)
        gid = self.game_id(request)
        if not account:
            return web.Response(status=401)
        if not gid:
            return web.Response(status=400)
        # (the answer comes when the upload ends: an answer started while the body
        # still streams made the Project Index portal end the request with a 502)
        content = request.content
        try:
            while True:
                size = struct.unpack(">H", await content.readexactly(2))[0]
                data = await content.readexactly(size) if size else b""
                game = self.games.get(gid)
                if not game or game.account["id"] != account["id"]:
                    continue  # (its download isn't open yet, or it's someone else's)
                game.seen = time.monotonic()
                if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION or not game.allowed():
                    continue
                if data[5] == FORWARD and len(data) > 12:
                    to, inner = data[6:12], data[12:]
                    out = packet(FROM, self.address(game.ip) + inner)
                    if to == BROADCAST:
                        self.broadcast(gid, out)
                        continue
                    game.rate.add()  # (what it tries: held back in p2p or not)
                    if self.p2p() and not self.pairs.allowed(gid, to):
                        continue  # (p2p: no match data through the relay - whatever the game was told)
                    if self.deliver(to, out):
                        self.relayed += 1
                        self.service.stats.count("relayed")
        except (asyncio.IncompleteReadError, ConnectionResetError, asyncio.CancelledError):
            pass
        return web.Response(status=200)

    def rows(self):
        """Every game on the relay now, for the dashboard: id, account, name, address, link, since, packets a second
        (its forwards to one game; searches not counted)."""
        out = {}
        for g in list(self.games.values()):
            out[g.id] = {"id": g.id.hex(), "account": g.account["id"], "name": g.account["name"], "ip": g.ip,
                         "port": 0, "link": "https", "since": g.since, "rate": g.rate.per_second()}
        for g in [g for g in list(self.udp_games.values()) if g.fresh()]:
            https = out.get(g.id, {})
            out[g.id] = {"id": g.id.hex(), "account": g.account["id"], "name": g.account["name"], "ip": g.addr[0],
                         "port": g.addr[1], "link": "UDP", "since": https.get("since", g.since),
                         "rate": round(https.get("rate", 0) + g.rate.per_second(), 1)}
        if self.edge_up():
            for gid in list(self.edge_games):
                e = self.edge_info.get(gid) or EdgeGame(gid)
                https = out.get(gid, {})
                name = self.name_of(e.account) if e.account else None
                out[gid] = {"id": gid.hex(), "account": e.account or https.get("account", 0),
                            "name": name or https.get("name") or self.edge_name(gid), "known": bool(name or https),
                            "ip": e.addr[0] if e.addr else https.get("ip", ""), "port": e.addr[1] if e.addr else 0,
                            "link": "UDP (edge)", "since": https.get("since", e.since),
                            "rate": round(https.get("rate", 0) + e.rate.per_second(), 1)}
        for r in out.values():
            r.setdefault("known", True)
        return sorted(out.values(), key=lambda r: -r["rate"])

    def account_of(self, gid_hex):
        """The account id behind relay game `gid_hex` now (a host's report names its peers by it), else 0."""
        try:
            gid = bytes.fromhex(gid_hex)
        except ValueError:
            return 0
        for table in (self.games, self.udp_games):
            g = table.get(gid)
            if g:
                return g.account["id"]
        e = self.edge_info.get(gid)
        return e.account if e else 0

    def routes(self, app, base):
        app.router.add_get(base + "/api/relay/{id}/udp", self.udp_ticket)
        app.router.add_get(base + "/api/relay/{id}", self.download)
        app.router.add_post(base + "/api/relay/{id}", self.upload)
        app.cleanup_ctx.append(self.start_udp)
