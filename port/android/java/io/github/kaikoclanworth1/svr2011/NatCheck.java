// WWE SmackDown vs. Raw 2011 - the launcher's connection check (the PC launcher's launcher/nat_check.c): what this
// network does to a direct connection, read-only. No address is shown or saved, and our server isn't asked anything.
//   - UDP to two STUN servers at different addresses: the same port seen by both = a router that keeps one port for
//     all players; different = "symmetric";
//   - STUN over IPv6: an answer means IPv6 reaches the internet from here.
// It can't see the other player's router: a result says what this side allows. No Android classes: tests run it on a PC.

package io.github.kaikoclanworth1.svr2011;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.util.Arrays;
import java.util.Random;

final class NatCheck {
    static final int kNoUdp = 0, kOneAnswer = 1, kOpen = 2, kCone = 3, kSymmetric = 4;
    static final int kCookie = 0x2112A442;
    // (the second far end: Google's names can all be one address)
    static final String[] kServers = {"stun.l.google.com:19302", "stun.cloudflare.com:3478", "stun1.l.google.com:19302"};

    int kind = kNoUdp, step;
    boolean shared, ipv6;

    private static final class Seen {
        boolean answered;
        byte[] ip;
        int port;
    }

    private static String[] hostPort(String text, int fallback) {
        int colon = text.lastIndexOf(':');
        return colon > 0 ? new String[] {text.substring(0, colon), text.substring(colon + 1)}
                         : new String[] {text, Integer.toString(fallback)};
    }

    // An address of that name, not `avoid`, of that family (null: none).
    private static InetSocketAddress resolve(String text, Class<?> family, byte[] avoid) {
        try {
            String[] hp = hostPort(text, 3478);
            for (InetAddress a : InetAddress.getAllByName(hp[0]))
                if (family.isInstance(a) && (avoid == null || !Arrays.equals(a.getAddress(), avoid)))
                    return new InetSocketAddress(a, Integer.parseInt(hp[1]));
        } catch (Exception e) {
        }
        return null;
    }

    // Asks on the socket, connected to that server, again each 400 ms, for `ms` at most.
    private static Seen ask(DatagramSocket s, InetSocketAddress to, int ms) throws Exception {
        Seen seen = new Seen();
        byte[] q = new byte[20], id = new byte[12], b = new byte[600];
        new Random().nextBytes(id);
        q[1] = 1;
        q[4] = 0x21; q[5] = 0x12; q[6] = (byte) 0xA4; q[7] = 0x42;
        System.arraycopy(id, 0, q, 8, 12);
        s.disconnect();
        s.connect(to);
        s.setSoTimeout(100);
        long start = System.currentTimeMillis(), at = 0;
        while (!seen.answered && System.currentTimeMillis() - start < ms) {
            if (System.currentTimeMillis() - at >= 400) {
                at = System.currentTimeMillis();
                s.send(new DatagramPacket(q, q.length));
            }
            DatagramPacket p = new DatagramPacket(b, b.length);
            try {
                s.receive(p);
            } catch (SocketTimeoutException e) {
                continue;
            } catch (java.io.IOException e) {  // (a closed port's answer, on a connected socket)
                continue;
            }
            read(b, p.getLength(), id, seen);
        }
        return seen;
    }

    // An answer: the address and port it saw (XOR-MAPPED-ADDRESS, or MAPPED-ADDRESS from an older server).
    private static void read(byte[] p, int len, byte[] id, Seen seen) {
        if (len < 20 || p[0] != 1 || p[1] != 1) return;
        for (int i = 0; i < 12; i++)
            if (p[8 + i] != id[i]) return;
        for (int at = 20; at + 4 <= len; ) {
            int type = (p[at] & 255) << 8 | (p[at + 1] & 255), size = (p[at + 2] & 255) << 8 | (p[at + 3] & 255);
            if (at + 4 + size > len) return;
            if ((type == 0x0020 || type == 0x0001) && size >= 8) {
                int v = at + 4;
                int port = (p[v + 2] & 255) << 8 | (p[v + 3] & 255);
                byte[] ip = Arrays.copyOfRange(p, v + 4, v + 4 + (p[v + 1] == 2 && size >= 20 ? 16 : 4));
                if (type == 0x0020) {
                    port ^= kCookie >>> 16;
                    for (int i = 0; i < 4 && i < ip.length; i++) ip[i] ^= (byte) (kCookie >>> (24 - 8 * i));
                }
                seen.port = port;
                seen.ip = ip;
                seen.answered = true;
                return;
            }
            at += 4 + ((size + 3) & ~3);
        }
    }

    // Takes a few seconds: not on the UI thread. stun: the p2p_stun setting ("": the default; "off": no STUN).
    // servers: tests give their own (ip:port, ip:port), v6: their IPv6 one; local: their "this device's address".
    static NatCheck run(String stun, String[] servers, String v6, byte[] local) {
        NatCheck r = new NatCheck();
        if (servers == null && "off".equals(stun)) return r;
        InetSocketAddress a = null, b = null;
        if (servers != null) {
            a = resolve(servers[0], Inet4Address.class, null);
            if (servers.length > 1) b = resolve(servers[1], Inet4Address.class, null);
        } else {
            a = resolve(stun.isEmpty() ? kServers[0] : stun, Inet4Address.class, null);
            for (int k = 0; k < kServers.length && b == null; k++)
                b = resolve(kServers[k], Inet4Address.class, a != null ? a.getAddress().getAddress() : null);
        }
        if (a == null) {
            a = b;
            b = null;
        }
        if (a != null) {
            try (DatagramSocket s = new DatagramSocket()) {
                Seen first = ask(s, a, 1500);
                InetAddress mine = s.getLocalAddress();
                byte[] me = local != null ? local : mine != null ? mine.getAddress() : new byte[4];
                int port = s.getLocalPort();
                Seen second = b != null ? ask(s, b, 1500) : null;
                r.shared = me.length == 4 && (me[0] & 255) == 100 && (me[1] & 0xC0) == 64;  // (100.64.0.0/10)
                Seen one = first.answered ? first : second;
                if (one != null && one.answered) {
                    boolean open = Arrays.equals(one.ip, me) && one.port == port;
                    if (second == null || !first.answered || !second.answered) {
                        r.kind = open ? kOpen : kOneAnswer;
                    } else if (first.port != second.port) {
                        r.kind = kSymmetric;
                        r.step = second.port - first.port;
                    } else {
                        r.kind = open ? kOpen : kCone;
                    }
                }
            } catch (Exception e) {
            }
        }
        InetSocketAddress six = servers != null ? (v6 != null ? resolve(v6, Inet6Address.class, null) : null)
            : resolve(stun.isEmpty() ? kServers[0] : stun, Inet6Address.class, null);
        if (six != null) {
            try (DatagramSocket s = new DatagramSocket(new InetSocketAddress(java.net.InetAddress.getByName("::"), 0))) {
                r.ipv6 = ask(s, six, 2000).answered;
            } catch (Exception e) {  // (no route: nothing to send on)
            }
        }
        return r;
    }

    // The check's words, for a player.
    String text(int gamePort) {
        String router, direct, todo = "";
        switch (kind) {
            case kNoUdp: router = "Network: nothing answered over UDP. This network may block it."; break;
            case kOneAnswer: router = "Network: only one test server answered, so its kind is unknown. Check again in a minute."; break;
            case kOpen: router = "Network: no router in the way."; break;
            case kCone: router = "Network: keeps one port for all players. That's the good kind."; break;
            default: router = "Network: uses a new port for each player (strict, or \"symmetric\").";
        }
        if (kind == kOpen) direct = "Direct: yes, with every player.";
        else if (kind == kCone) direct = "Direct: yes with most players. Players on a strict network may go through the relay.";
        else if (kind == kSymmetric) direct = ipv6 ? "Direct: only with players on IPv6 or with an open port. With others, the relay (slower)."
            : "Direct: only with players who have an open port. With others, the relay (slower).";
        else if (kind == kNoUdp) direct = ipv6 ? "Direct: only with players on IPv6. With others, the relay (slower)."
            : "Direct: no. Matches go through the relay (slower).";
        else direct = "Direct: can't tell yet.";
        if (kind == kNoUdp) todo = "To do: try another network.\n";
        else if (kind == kSymmetric && shared) todo = "To do: your provider shares its address between customers, so a forwarded port can't help. Try Wi-Fi.\n";
        else if (kind == kSymmetric) todo = "To do: on Wi-Fi, turn on UPnP on the router or forward UDP " + gamePort
            + " to this phone. On mobile data this can't be changed.\n";
        return router + "\nIPv6: " + (ipv6 ? "works." : "not here.") + "\n" + direct + "\n" + todo
            + "(The other player's network matters too: this only sees your side.)";
    }
}
