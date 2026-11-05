// WWE SmackDown vs. Raw 2011 - online test aids and packet counts for the
// match tunnel (net_sim.cpp; p2p.cpp calls these on its game-packet path).
//
//   SVR2011_TEST_NET_DELAY="ms[,jitter_ms[,loss_pct]]"  this game's outgoing
//       game packets wait ms plus up to jitter_ms more (kept in order) and
//       loss_pct of them are dropped: an internet link between two games on
//       one PC or LAN.
//   SVR2011_TEST_NET_TUNNEL=1  peers on this PC / LAN go through the tunnel
//       (proxies + kData) too, so the delay above applies to them.
//   SVR2011_NET_STATS=2  per-peer packet counts in the log every 5 s.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace svr2011 {

// The simulated link is on (SVR2011_TEST_NET_DELAY).
bool NetSimActive();
// Sends now, later or never, as the simulated link would.
void NetSimSend(std::function<void()> send);
// Every peer through the tunnel (SVR2011_TEST_NET_TUNNEL).
bool NetForceTunnel();
// Tests: the old tunnel packets only, no repeats (SVR2011_TEST_NET_V1=1): before/after.
bool NetDataV1();
// A game packet to / from the peer `peer` (its 10.64.x.y address), relayed or not.
void NetPacket(bool out, uint32_t peer, size_t size, bool relayed);

// Round trips (p2p.cpp pings the peers it exchanges game packets with, once a
// second): the latest per peer, for the log and the ping indicator.
// how: the path's word for the ping indicator ("direct", "IPv6", "LAN", "relay").
void NetRtt(uint32_t peer, double ms, bool relayed, const char* how);
struct NetLink {
  uint32_t peer = 0;
  float rtt_ms = 0;     // the latest
  float worst_ms = 0;   // the worst of the last 5 s
  bool relayed = false;
  const char* how = "";  // (the latest ping's path: NetRtt)
  std::string name;     // (its player's name, when its pings carry it)
};
std::vector<NetLink> NetLinks();  // (peers answered within the last 5 s)
void NetPeerName(uint32_t peer, const std::string& name);

}  // namespace svr2011
