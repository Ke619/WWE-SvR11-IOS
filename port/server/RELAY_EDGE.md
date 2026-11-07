# The relay edge: online match UDP relayed on the VPS

Before: player -> VPS -> (tunnel) -> PC -> (tunnel) -> VPS -> player.
The PC <-> VPS round trip (~31 ms, measured 13 Oct 2026) was added to every
relayed packet.

After: between two UDP players, player -> VPS -> player. Only the small UDP
relay (`relay_edge.py`) runs on the VPS. It has no accounts, no files and no
database. Everything else stays on the PC: accounts, Community Creations
storage, leaderboards, the dashboard, logs, and the https relay streams that
games fall back to.

The game doesn't change. It already sends its relay UDP to the server's
address (sho-ti.me, which is the VPS) on the port the server tells it.

## How it fits together

- **Tickets:** a game asks the PC for a UDP ticket, the same as now. The PC
  checks the account and signs the ticket with a secret that only the PC and
  the VPS know. The edge checks the signature, and an hour later the ticket
  runs out.
- **The link:** the PC and the edge talk over the WireGuard tunnel (UDP, the
  edge's port 36501, every packet signed). The PC sends a keepalive every
  second. The edge answers with its list of UDP games and its packet count,
  so the dashboard still counts relayed packets.
- **Between a UDP player and an https (fallback) player:** packets cross the
  link, which is no worse than today.

## Guards (29 Oct)

- **Whose game it is:** a ticket now holds the account id (signed with it), so
  the edge's list tells the server whose every game is, even after either one
  restarts. The dashboard's Matches -> Relay now shows each game's account,
  address, link, packets a second and version, never a bare "?". The edge
  still takes the first tickets (no account) until they run out, an hour at
  most; their rows say "not known: an older ticket".
- **Bans reach the relay:** the dashboard's Ban (or Delete) ends the account's
  https relay streams, drops its UDP games, and tells the edge. The edge drops
  them and refuses every ticket made before the ban, and the server tells it
  again after an edge restart. A banned account can't get a new ticket.
- **The match network p2p is kept by the relay itself:** the server's
  keepalive carries the setting. On p2p, the edge and the server pass one game
  only 12 packets to any one other game at once, then 2 a second. That is
  enough for hellos, session answers and QoS, but not for a match (60 a second
  and more), whatever the game was told. Searches are not limited.
- **Either side can be updated first:** an older edge ignores the new
  keepalive and the kick. An older server reads only the ids from the new
  edge's list, and its tickets still pass.
- **Windows:** a UDP socket there stops reading after a "port unreachable" (a
  game or the other end gone) on asyncio's default loop. The endpoints open
  again by themselves. Linux, the VPS, never needs it.

## Setting it up (the user does this, or OKs each step)

1. **A secret.** On the PC, in the service folder:
   `python -c "import secrets; print(secrets.token_hex(32))" > relay_secret.txt`.
2. **The edge on the VPS.** Put `relay_edge.py`, `relay_link.py`,
   `relay_edge_install.sh`, `relay_edge_uninstall.sh` and the secret (named
   `secret.txt`) in one folder, copy it to the VPS, and run
   `sudo bash relay_edge_install.sh` there. It needs Python 3.8 or newer and
   nothing else. It checks that `wg0` is `10.8.0.1` (the link must never be on
   the public address), copies the files to `/opt/svr-relay/` (secret `chmod 600`),
   adds the systemd unit `svr-relay-edge` and does step 3. The unit runs with
   `DynamicUser=yes`, so it gets the root-only secret through
   `LoadCredential=secret:/opt/svr-relay/secret.txt` and `--secret %d/secret`.
3. **The VPS firewall.**
   - Remove the forward of UDP 36500 to 10.8.0.2. The masquerade stays if
     anything else uses it.
   - Open UDP 36500 to the public.
   - Allow UDP 36501 from the tunnel only (10.8.0.0/24).
4. **The PC server.** Add `--relay-edge 10.8.0.1:36501 --relay-secret relay_secret.txt`
   to its arguments, keep `--relay-udp-port 36500`, and restart it. Its log
   should show `relay: UDP on the edge ...` and then `relay: edge linked`.
   The PC firewall rule for inbound UDP 36500 is no longer needed: the link's
   replies come back to the connection the PC opened.
5. **Check.** Run `python test_relay.py --server https://sho-ti.me/svr`. It
   should report all ok, including the `udp: b (udp) -> c (udp)` lines.
   Delete the three throwaway accounts it prints. Once games have relayed,
   the edge's log (`journalctl -u svr-relay-edge`) shows
   `edge: relayed N here, ...`.

**Going back:** `sudo bash relay_edge_uninstall.sh` on the VPS (stops the edge and
puts the forward back), then remove the two server arguments and restart the server.

## Tests

`python test_relay_edge.py` runs on any PC. It starts an edge and a server
on test ports (37650-37661) with a throwaway data folder, then checks:

- all of `test_relay.py` through the edge
- that packets between UDP games stayed on the edge
- that forged and expired tickets are refused, and that link packets
  without the secret are ignored
- that the server without an edge still works

`python test_relay_guard.py [--ports 8545]` checks the guards the same way:

- tickets with the account, the older ones, the link's new parts
- the dashboard's rows
- p2p holding match data back while hellos and searches pass
- a ban, an edge restart after a ban, unban and sign in again
- a server restart (the edge's games still by name)
