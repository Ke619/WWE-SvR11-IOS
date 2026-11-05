/* WWE SmackDown vs. Raw 2011 PC launcher - the Online tab's connection check (nat_check.c): what this PC's network
   does to a direct connection, read-only: nothing on the router or in Windows Firewall changes, no address is shown
   or saved, and our server isn't asked anything.
     - UDP to two STUN servers (the p2p_stun setting's, and another at a different address): the same port seen by both
       = a router that keeps one port for all players; different = "symmetric". A connected socket, never bound: a
       program that opens a port to listen on makes Windows Firewall ask the player, and add a rule;
     - STUN over IPv6: an answer means IPv6 reaches the internet from here;
     - Windows Firewall's rules for the game (or its port), read through its COM interface.
   Whether UPnP worked comes from the game's log (the caller).
   It can't see the other player's router: a result says what this side allows. */
#pragma once

#include <windows.h>

enum { NAT_NO_UDP, NAT_ONE_ANSWER, NAT_OPEN, NAT_CONE, NAT_SYMMETRIC };
enum { NAT_V6_NONE, NAT_V6_WORKS };
enum { NAT_FW_UNKNOWN, NAT_FW_OFF, NAT_FW_ALLOWED, NAT_FW_NO_RULE, NAT_FW_BLOCKED };

typedef struct {
    int kind;       /* NAT_NO_UDP ... */
    int step;       /* NAT_SYMMETRIC: second port - first port */
    int shared;     /* this PC's address is in 100.64.0.0/10: the provider's own NAT (a forwarded port can't help) */
    int v6;         /* NAT_V6_... */
    int fw;         /* NAT_FW_... */
} NatCheck;

/* Takes a few seconds: not on the window's thread. stun: the p2p_stun setting ("" : the default; "off": no STUN);
   port: p2p_port; exe: the game's full path (for the firewall's rules). Test aids: SVR2011_TEST_STUN=<ip:port>,<ip:port>,
   SVR2011_TEST_STUN6=<port> (::1), SVR2011_TEST_NAT_LOCAL=<this PC's IPv4>, SVR2011_TEST_NAT_FW=off|allowed|none|blocked. */
void nat_check_run(const char *stun, int port, const WCHAR *exe, NatCheck *r);

/* The check's words, for a player. mapped: the game's last log says the router opened its port (UPnP / NAT-PMP) 1,
   didn't -1, no such line 0. */
void nat_check_text(const NatCheck *r, int port, int mapped, WCHAR *out, size_t n);
