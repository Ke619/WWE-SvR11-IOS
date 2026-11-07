"""
What the Community Creations server (relay.py, "home") and a relay edge
(relay_edge.py, on the VPS that takes the players' UDP) share: the signed UDP
tickets and the link between the two. No aiohttp: the edge needs only Python.

Why an edge: with the server on a PC behind a VPS, every relayed packet went
player -> VPS -> PC -> VPS -> player; the edge relays UDP players on the VPS
itself (player -> VPS -> player) while accounts, storage and the https
relay streams stay on the PC.

Tickets: home signs them with the shared secret, the edge checks them - it
never needs the accounts. 16 bytes, as the game sends them:
    expiry[4] (unix seconds, big-endian) + account[4] + HMAC-SHA256(secret, "ticket2" + id[6] + expiry[4] + account[4])[:8]
(29 Oct: the account in it, so the edge knows whose game it relays - for the
dashboard, after a restart of either end too - and can refuse a banned one's.
The first tickets, expiry[4] + HMAC(secret, "ticket" + id + expiry)[:12], still
pass, as account 0: an older home's.)

The link: UDP datagrams over the private tunnel, home -> edge first (the
edge answers wherever home's last good datagram came from), each
    "SVRL" kind[1] body  HMAC-SHA256(secret, "link" + kind + body)[:16]
    1 state   home -> edge: a keepalive, every second: nothing (an older home), or
                            1, network[1] (0 auto, 1 relay, 2 p2p: the server's
                            match network; p2p: no match data relayed, PairLimit)
              edge -> home: relayed[4] (since the last), n[2], n x id[6]
                            (its UDP games now; also at once when one comes on),
                            then n x account[4], n x address[6], n x sent[2]
                            (whose, from where, packets since the last; an older
                            home stops reading after the ids)
    2 forward to[6] packet   deliver `packet` (a relay "from", already built) to game `to`
    3 search  from[6] packet deliver to every game there but `from`
    4 kick    home -> edge: n[2], n x account[4]: banned - their games dropped,
                            their tickets made until now refused (sent again
                            while one is still there: an edge restart)
A forward the edge can't deliver (not one of its UDP games) goes back to
home, which delivers on the https download only - a packet never loops.
"""
import asyncio
import hashlib
import hmac
import struct
import sys
import time

LINK_MAGIC = b"SVRL"
STATE, FORWARD, SEARCH, KICK = 1, 2, 3, 4
MAC = 16
TICKET_LIFE = 3600
AUTO, RELAY, P2P = 0, 1, 2
NETWORKS = {"auto": AUTO, "relay": RELAY, "p2p": P2P}
# p2p (no match data through the relay): what one game may send one other game
# through it - hellos, searches' answers and QoS asks come a few at a time, a
# match's packets 60 and more a second (PAIR_BURST at once, then PAIR_RATE a second)
PAIR_BURST = 12
PAIR_RATE = 2.0


def read_secret(path):
    with open(path, "rb") as f:
        secret = f.read().strip()
    if len(secret) < 16:
        raise SystemExit("relay secret %s: too short (at least 16 characters)" % path)
    return secret


def make_ticket(secret, gid, account=0, now=None):
    expiry = struct.pack(">I", int((now or time.time()) + TICKET_LIFE))
    aid = struct.pack(">I", account & 0xFFFFFFFF)
    return expiry + aid + hmac.new(secret, b"ticket2" + gid + expiry + aid, hashlib.sha256).digest()[:8]


def read_ticket(secret, gid, ticket, now=None):
    """(account, made at) of a good ticket for game `gid` (account 0: a first-format one), else None."""
    if len(ticket) != 16:
        return None
    expiry, aid = ticket[:4], ticket[4:8]
    t = struct.unpack(">I", expiry)[0]
    if t < (now or time.time()):
        return None
    if hmac.compare_digest(hmac.new(secret, b"ticket2" + gid + expiry + aid, hashlib.sha256).digest()[:8], ticket[8:]):
        return struct.unpack(">I", aid)[0], t - TICKET_LIFE
    if hmac.compare_digest(hmac.new(secret, b"ticket" + gid + expiry, hashlib.sha256).digest()[:12], ticket[4:]):
        return 0, t - TICKET_LIFE
    return None


def check_ticket(secret, gid, ticket, now=None):
    return read_ticket(secret, gid, ticket, now) is not None


def link_packet(secret, kind, body):
    head = LINK_MAGIC + bytes([kind])
    return head + body + hmac.new(secret, b"link" + bytes([kind]) + body, hashlib.sha256).digest()[:MAC]


def read_link(secret, data):
    """(kind, body) of a good link datagram, else None."""
    if len(data) < 5 + MAC or data[:4] != LINK_MAGIC:
        return None
    kind, body, mac = data[4], data[5:-MAC], data[-MAC:]
    if not hmac.compare_digest(mac, hmac.new(secret, b"link" + bytes([kind]) + body, hashlib.sha256).digest()[:MAC]):
        return None
    return kind, body


def state_body(relayed, ids, games=None):
    """games: id -> (account, address[6], sent): the columns after the ids."""
    ids = list(ids)[:4096]
    body = struct.pack(">IH", relayed, len(ids)) + b"".join(ids)
    if games is not None:
        rows = [games.get(i, (0, bytes(6), 0)) for i in ids]
        body += b"".join(struct.pack(">I", a & 0xFFFFFFFF) for a, _, _ in rows)
        body += b"".join(addr[:6].ljust(6, b"\0") for _, addr, _ in rows)
        body += b"".join(struct.pack(">H", min(sent, 0xFFFF)) for _, _, sent in rows)
    return body


def read_state(body):
    return read_state_full(body)[:2]


def read_state_full(body):
    """relayed, ids, {id: (account, address[6], sent)} ({} from an older edge)."""
    if len(body) < 6:
        return 0, [], {}
    relayed, n = struct.unpack(">IH", body[:6])
    ids = [body[6 + 6 * i:12 + 6 * i] for i in range(n) if 12 + 6 * i <= len(body)]
    extra = {}
    at = 6 + 6 * n
    if len(ids) == n and len(body) >= at + 12 * n:
        for i, gid in enumerate(ids):
            aid = struct.unpack(">I", body[at + 4 * i:at + 4 * i + 4])[0]
            addr = body[at + 4 * n + 6 * i:at + 4 * n + 6 * i + 6]
            sent = struct.unpack(">H", body[at + 10 * n + 2 * i:at + 10 * n + 2 * i + 2])[0]
            extra[gid] = (aid, addr, sent)
    return relayed, ids, extra


def home_state(network):
    return bytes([1, network & 0xFF])


def read_home_state(body):
    """The server's match network from its keepalive (auto from an older one)."""
    return body[1] if len(body) >= 2 and body[0] == 1 and body[1] in (AUTO, RELAY, P2P) else AUTO


def kick_body(accounts):
    accounts = list(accounts)[:300]
    return struct.pack(">H", len(accounts)) + b"".join(struct.pack(">I", a & 0xFFFFFFFF) for a in accounts)


def read_kick(body):
    if len(body) < 2:
        return []
    n = struct.unpack(">H", body[:2])[0]
    return [struct.unpack(">I", body[2 + 4 * i:6 + 4 * i])[0] for i in range(n) if 6 + 4 * i <= len(body)]


class PairLimit:
    """p2p: what one game may send one other game through the relay (PAIR_BURST, then PAIR_RATE a second)."""

    def __init__(self, burst=PAIR_BURST, rate=PAIR_RATE):
        self.burst, self.rate = burst, rate
        self.pairs = {}  # (from, to) -> [tokens, at]
        self.dropped = 0

    def allowed(self, sender, to, now=None):
        now = now or time.monotonic()
        b = self.pairs.get((sender, to))
        if b is None:
            if len(self.pairs) >= 65536:
                self.forget(now)
            b = self.pairs[(sender, to)] = [float(self.burst), now]
        b[0] = min(float(self.burst), b[0] + (now - b[1]) * self.rate)
        b[1] = now
        if b[0] >= 1.0:
            b[0] -= 1.0
            return True
        self.dropped += 1
        return False

    def forget(self, now=None):
        now = now or time.monotonic()
        full = self.burst / self.rate  # (a pair quiet this long is full again anyway)
        for k in [k for k, b in self.pairs.items() if now - b[1] > full]:
            del self.pairs[k]


def reopen_on_windows(open_endpoint, log, what):
    """An error_received callback. On Windows a UDP socket hears "port unreachable" for a datagram it sent to a
    closed port (a game or server gone), and asyncio's Proactor loop stops reading that socket after it (Python
    3.11, 29 Oct): there the endpoint is opened again, at most once a second (logged once a minute). Linux reads on."""
    state = {"at": 0.0, "busy": False, "logged": 0.0}

    def on_error():
        if sys.platform != "win32" or state["busy"]:
            return
        state["busy"] = True  # (until it's open again: the dead socket reports nothing more)

        async def again():
            try:
                await asyncio.sleep(max(0.0, state["at"] + 1.0 - time.monotonic()))
                state["at"] = time.monotonic()
                for tries in range(20):  # (the old socket closes on the loop's next turn: its port free a moment later)
                    try:
                        await open_endpoint()
                        if time.monotonic() - state["logged"] >= 60:
                            state["logged"] = time.monotonic()
                            log("%s: UDP socket opened again (Windows stops reading one after a port-unreachable)" % what)
                        return
                    except OSError as e:
                        if tries == 19:
                            log("%s: UDP socket not opened again (%s)" % (what, e))
                        await asyncio.sleep(0.1)
            finally:
                state["busy"] = False
        asyncio.ensure_future(again())
    return on_error
