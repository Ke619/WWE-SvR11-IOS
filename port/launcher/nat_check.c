/* WWE SmackDown vs. Raw 2011 PC launcher - the Online tab's connection check: nat_check.h. */
#define COBJMACROS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
#include <initguid.h>

#include "nat_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#define STUN_COOKIE 0x2112A442u

/* -- STUN --------------------------------------------------------------------- */

typedef struct {
    struct sockaddr_storage to;
    int to_len;
    unsigned char id[12];
    unsigned ip;       /* (host order) what it saw; IPv6: only that it answered */
    int port;
    int answered;
} Asked;

/* An answer to `a`: the address and port it saw (XOR-MAPPED-ADDRESS, or MAPPED-ADDRESS from an older server). */
static void stun_answer(const unsigned char *p, int len, Asked *a)
{
    int at = 20;
    if (len < 20 || p[0] != 1 || p[1] != 1 || memcmp(p + 8, a->id, 12))
        return;
    while (at + 4 <= len) {
        const int type = p[at] << 8 | p[at + 1], size = p[at + 2] << 8 | p[at + 3];
        if (at + 4 + size > len)
            break;
        if ((type == 0x0020 || type == 0x0001) && size >= 8) {
            const unsigned char *v = p + at + 4;
            int port = v[2] << 8 | v[3];
            unsigned ip = (unsigned)v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7];
            if (type == 0x0020)
                port ^= STUN_COOKIE >> 16, ip ^= STUN_COOKIE;
            a->port = port, a->ip = ip, a->answered = 1;
            return;
        }
        at += 4 + ((size + 3) & ~3);
    }
}

/* Asks one server on a socket connected to it, again each 400 ms, for `ms` at most. Connected and never bound: Windows
   Firewall only asks the player about a program that opens a port to listen on, and this check changes nothing. Connecting
   the same socket to the next server keeps its port: what the router's rule is tested with. */
static void stun_ask(SOCKET s, Asked *a, int ms)
{
    unsigned char q[20] = { 0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42 };
    ULONGLONG start, at = 0;
    if (connect(s, (const struct sockaddr *)&a->to, a->to_len))
        return;
    memcpy(q + 8, a->id, 12);
    start = GetTickCount64();
    while (!a->answered && GetTickCount64() - start < (ULONGLONG)ms) {
        unsigned char b[600];
        fd_set r;
        struct timeval tv = { 0, 50000 };
        if (GetTickCount64() - at >= 400) {
            at = GetTickCount64();
            send(s, (const char *)q, sizeof q, 0);
        }
        FD_ZERO(&r);
        FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) > 0) {
            const int got = recv(s, (char *)b, sizeof b, 0);
            if (got > 0)
                stun_answer(b, got, a);
        }
    }
}

static void new_id(Asked *a, int salt)
{
    int i;
    const unsigned t = (unsigned)GetTickCount64() ^ (unsigned)GetCurrentProcessId() * 2654435761u;
    for (i = 0; i < 12; i++)
        a->id[i] = (unsigned char)((t >> (i % 4 * 8)) + i * 37 + salt * 91);
}

/* "host:port" (the port: `fallback` when it has none). */
static int resolve_v4(const char *text, int fallback, struct sockaddr_in *out, unsigned avoid)
{
    char host[128];
    const char *colon = strrchr(text, ':');
    struct addrinfo hint = { 0 }, *list = NULL, *i;
    size_t len = colon ? (size_t)(colon - text) : strlen(text);
    int ok = 0;
    if (!len || len >= sizeof host)
        return 0;
    memcpy(host, text, len);
    host[len] = 0;
    hint.ai_family = AF_INET;
    hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hint, &list))
        return 0;
    for (i = list; i && !ok; i = i->ai_next) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)i->ai_addr;
        if (ntohl(a->sin_addr.s_addr) != avoid) {
            *out = *a;
            out->sin_port = htons((u_short)(colon ? atoi(colon + 1) : fallback));
            ok = 1;
        }
    }
    freeaddrinfo(list);
    return ok;
}

static void check_v4(const char *stun, NatCheck *r)
{
    /* (the second far end, only when a player asks for the check: Google's names can all be one address) */
    static const char *const others[] = { "stun.l.google.com:19302", "stun.cloudflare.com:3478",
                                          "stun1.l.google.com:19302" };
    const char *test = getenv("SVR2011_TEST_STUN"), *test_local = getenv("SVR2011_TEST_NAT_LOCAL");
    Asked asked[2];
    struct sockaddr_in a = { 0 }, b = { 0 }, me = { 0 };
    int have_a = 0, have_b = 0, n = 0, k, len = sizeof me;
    unsigned local = 0;
    SOCKET s;

    r->kind = NAT_NO_UDP;
    memset(asked, 0, sizeof asked);
    if (test && *test) {
        char first[64];
        const char *comma = strchr(test, ',');
        const size_t m = comma ? (size_t)(comma - test) : strlen(test);
        if (m < sizeof first) {
            memcpy(first, test, m);
            first[m] = 0;
            have_a = resolve_v4(first, 3478, &a, 0);
        }
        have_b = comma && resolve_v4(comma + 1, 3478, &b, 0);
    } else {
        if (!strcmp(stun, "off"))
            return;
        have_a = resolve_v4(stun[0] ? stun : others[0], 3478, &a, 0);
        for (k = 0; k < 3 && !have_b; k++)
            have_b = resolve_v4(others[k], 3478, &b, have_a ? ntohl(a.sin_addr.s_addr) : 0);
    }
    if (!have_a && have_b)
        a = b, have_a = 1, have_b = 0;
    if (!have_a)
        return;  /* (names don't resolve: no internet) */

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return;
    memcpy(&asked[n].to, &a, sizeof a), asked[n].to_len = sizeof a, new_id(&asked[n], 1), n++;
    if (have_b)
        memcpy(&asked[n].to, &b, sizeof b), asked[n].to_len = sizeof b, new_id(&asked[n], 2), n++;
    stun_ask(s, &asked[0], 1500);
    /* (this PC's address and port on the way out) */
    if (!getsockname(s, (struct sockaddr *)&me, &len))
        local = ntohl(me.sin_addr.s_addr);
    if (test_local && *test_local) {
        struct in_addr x;
        local = inet_pton(AF_INET, test_local, &x) == 1 ? ntohl(x.s_addr) : 0;
    }
    if (n > 1)
        stun_ask(s, &asked[1], 1500);
    closesocket(s);

    r->shared = (local >> 22) == (0x64400000u >> 22);  /* 100.64.0.0/10 */
    if (!asked[0].answered && !(n > 1 && asked[1].answered))
        return;
    if (n < 2 || !asked[0].answered || !asked[1].answered) {
        /* (one far end: no comparing - but no router in the way shows from one) */
        const Asked *one = asked[0].answered ? &asked[0] : &asked[1];
        r->kind = one->ip == local && one->port == ntohs(me.sin_port) ? NAT_OPEN : NAT_ONE_ANSWER;
        return;
    }
    if (asked[0].ip == local && asked[0].port == ntohs(me.sin_port) && asked[1].port == asked[0].port)
        r->kind = NAT_OPEN;
    else if (asked[0].port == asked[1].port)
        r->kind = NAT_CONE;
    else {
        r->kind = NAT_SYMMETRIC;
        r->step = asked[1].port - asked[0].port;
    }
}

static int check_v6(const char *stun)
{
    const char *test = getenv("SVR2011_TEST_STUN6");
    struct addrinfo hint = { 0 }, *list = NULL;
    Asked asked;
    SOCKET s;
    char host[128] = "stun.l.google.com";
    int port = 19302;

    memset(&asked, 0, sizeof asked);
    if (test && *test) {
        struct sockaddr_in6 to = { 0 };
        to.sin6_family = AF_INET6;
        to.sin6_port = htons((u_short)atoi(test));
        to.sin6_addr.s6_addr[15] = 1;
        memcpy(&asked.to, &to, sizeof to);
        asked.to_len = sizeof to;
    } else {
        const char *colon = strrchr(stun, ':');
        if (!strcmp(stun, "off"))
            return NAT_V6_NONE;
        if (stun[0] && colon && (size_t)(colon - stun) < sizeof host) {
            memcpy(host, stun, (size_t)(colon - stun));
            host[colon - stun] = 0;
            port = atoi(colon + 1);
        }
        hint.ai_family = AF_INET6;
        hint.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hint, &list) || !list)
            return NAT_V6_NONE;
        memcpy(&asked.to, list->ai_addr, list->ai_addrlen);
        asked.to_len = (int)list->ai_addrlen;
        ((struct sockaddr_in6 *)&asked.to)->sin6_port = htons((u_short)port);
        freeaddrinfo(list);
    }
    s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return NAT_V6_NONE;
    if (connect(s, (const struct sockaddr *)&asked.to, asked.to_len)) {  /* (no route: fails at once) */
        closesocket(s);
        return NAT_V6_NONE;
    }
    new_id(&asked, 3);
    {   /* (a connected socket: unconnected sendto's address is the same one) */
        unsigned char q[20] = { 0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42 };
        ULONGLONG start = GetTickCount64(), at = 0;
        memcpy(q + 8, asked.id, 12);
        while (!asked.answered && GetTickCount64() - start < 2000) {
            unsigned char b[600];
            fd_set r;
            struct timeval tv = { 0, 50000 };
            if (GetTickCount64() - at >= 500) {
                at = GetTickCount64();
                send(s, (const char *)q, sizeof q, 0);
            }
            FD_ZERO(&r);
            FD_SET(s, &r);
            if (select(0, &r, NULL, NULL, &tv) > 0) {
                const int got = recv(s, (char *)b, sizeof b, 0);
                /* (the type and the id are enough: it answered over IPv6) */
                if (got >= 20 && b[0] == 1 && b[1] == 1 && !memcmp(b + 8, asked.id, 12))
                    asked.answered = 1;
            }
        }
    }
    closesocket(s);
    return asked.answered ? NAT_V6_WORKS : NAT_V6_NONE;
}

/* -- Windows Firewall, read-only ------------------------------------------------ */

DEFINE_GUID(kNatPolicyClsid, 0xE2B3C97F, 0x6AE1, 0x41AC, 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD);
DEFINE_GUID(kNatPolicyIid, 0x98325047, 0xC671, 0x4174, 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86);
DEFINE_GUID(kNatRuleIid, 0xAF230D27, 0xBABA, 0x4E42, 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2);
DEFINE_GUID(kNatEnumIid, 0x00020404, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

/* "36000", "36000-36100", "36000,36005", "*" */
static int ports_hold(const WCHAR *list, int port)
{
    const WCHAR *p = list;
    while (p && *p) {
        WCHAR *end;
        long a, b;
        if (*p == L'*')
            return 1;
        a = wcstol(p, &end, 10);
        if (end == p)
            return 0;
        b = *end == L'-' ? wcstol(end + 1, &end, 10) : a;
        if (port >= a && port <= b)
            return 1;
        p = wcschr(end, L',');
        p = p ? p + 1 : NULL;
    }
    return 0;
}

static int same_program(const WCHAR *rule, const WCHAR *exe)
{
    WCHAR full[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(rule, full, MAX_PATH * 2);
    return n && n <= MAX_PATH * 2 && !_wcsicmp(full, exe);
}

/* One inbound rule: 1 it lets the game's UDP in, -1 it blocks it, 0 it isn't about the game. */
static int rule_verdict(INetFwRule *rule, long profiles, const WCHAR *exe, int port)
{
    VARIANT_BOOL on = VARIANT_FALSE;
    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
    NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
    long protocol = 0, where = 0;
    BSTR app = NULL, local = NULL;
    int about = 0;
    if (FAILED(INetFwRule_get_Enabled(rule, &on)) || on != VARIANT_TRUE ||
        FAILED(INetFwRule_get_Direction(rule, &dir)) || dir != NET_FW_RULE_DIR_IN ||
        FAILED(INetFwRule_get_Profiles(rule, &where)) || !(where & profiles) ||
        FAILED(INetFwRule_get_Protocol(rule, &protocol)) || (protocol != 17 && protocol != 256) ||
        FAILED(INetFwRule_get_Action(rule, &action)))
        return 0;
    if (SUCCEEDED(INetFwRule_get_ApplicationName(rule, &app)) && app && app[0])
        about = same_program(app, exe);
    else if (protocol == 17 && SUCCEEDED(INetFwRule_get_LocalPorts(rule, &local)) && local)
        about = ports_hold(local, port) && wcscmp(local, L"*");  /* (a rule for the port, for any program) */
    SysFreeString(app);
    SysFreeString(local);
    return !about ? 0 : action == NET_FW_ACTION_BLOCK ? -1 : 1;
}

static int check_firewall(const WCHAR *exe, int port)
{
    const char *test = getenv("SVR2011_TEST_NAT_FW");
    INetFwPolicy2 *policy = NULL;
    INetFwRules *rules = NULL;
    IUnknown *unknown = NULL;
    IEnumVARIANT *list = NULL;
    long profiles = 0;
    int result = NAT_FW_UNKNOWN, allowed = 0, blocked = 0, enabled = 0, k;
    const HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    static const NET_FW_PROFILE_TYPE2 kinds[] = { NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC };

    if (test && *test) {
        if (SUCCEEDED(com)) CoUninitialize();
        return !strcmp(test, "off") ? NAT_FW_OFF : !strcmp(test, "allowed") ? NAT_FW_ALLOWED
             : !strcmp(test, "blocked") ? NAT_FW_BLOCKED : !strcmp(test, "none") ? NAT_FW_NO_RULE : NAT_FW_UNKNOWN;
    }
    if (FAILED(CoCreateInstance(&kNatPolicyClsid, NULL, CLSCTX_INPROC_SERVER, &kNatPolicyIid, (void **)&policy)))
        goto done;
    if (FAILED(INetFwPolicy2_get_CurrentProfileTypes(policy, &profiles)))
        goto done;
    for (k = 0; k < 3; k++) {
        VARIANT_BOOL on = VARIANT_FALSE;
        if ((profiles & kinds[k]) && SUCCEEDED(INetFwPolicy2_get_FirewallEnabled(policy, kinds[k], &on)) && on == VARIANT_TRUE)
            enabled = 1;
    }
    if (!enabled) {
        result = NAT_FW_OFF;
        goto done;
    }
    if (FAILED(INetFwPolicy2_get_Rules(policy, &rules)) || FAILED(INetFwRules_get__NewEnum(rules, &unknown)) ||
        FAILED(IUnknown_QueryInterface(unknown, &kNatEnumIid, (void **)&list)))
        goto done;
    for (;;) {
        VARIANT v;
        ULONG got = 0;
        VariantInit(&v);
        if (IEnumVARIANT_Next(list, 1, &v, &got) != S_OK || !got)
            break;
        if (v.vt == VT_DISPATCH && v.pdispVal) {
            INetFwRule *rule = NULL;
            if (SUCCEEDED(IDispatch_QueryInterface(v.pdispVal, &kNatRuleIid, (void **)&rule))) {
                const int verdict = rule_verdict(rule, profiles, exe, port);
                allowed |= verdict > 0, blocked |= verdict < 0;
                INetFwRule_Release(rule);
            }
        }
        VariantClear(&v);
    }
    /* (a block rule wins over an allow rule in Windows Firewall) */
    result = blocked ? NAT_FW_BLOCKED : allowed ? NAT_FW_ALLOWED : NAT_FW_NO_RULE;
done:
    if (list) IEnumVARIANT_Release(list);
    if (unknown) IUnknown_Release(unknown);
    if (rules) INetFwRules_Release(rules);
    if (policy) INetFwPolicy2_Release(policy);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

/* -- The check ------------------------------------------------------------------ */

void nat_check_run(const char *stun, int port, const WCHAR *exe, NatCheck *r)
{
    WSADATA wsa;
    memset(r, 0, sizeof *r);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    check_v4(stun ? stun : "", r);
    r->v6 = check_v6(stun ? stun : "");
    r->fw = check_firewall(exe, port);
    WSACleanup();
}

void nat_check_text(const NatCheck *r, int port, int mapped, WCHAR *out, size_t n)
{
    static const WCHAR *const router[] = {
        L"Router: nothing answered over UDP. This network may block it (often at work, school or a hotel).",
        L"Router: only one test server answered, so its kind is unknown. Check again in a minute.",
        L"Router: none in the way. This PC is on the internet directly.",
        L"Router: keeps one port for all players. That's the good kind.",
        L"Router: uses a new port for each player (strict, or \"symmetric\").",
    };
    static const WCHAR *const firewall[] = {
        L"Windows Firewall: couldn't read it.", L"Windows Firewall: off.", L"Windows Firewall: lets the game in.",
        L"Windows Firewall: no rule for the game yet (Windows asks when it first hosts).",
        L"Windows Firewall: blocks the game coming in.",
    };
    WCHAR result[320], todo[320];
    const int open = r->kind == NAT_OPEN || (mapped > 0 && r->kind != NAT_NO_UDP);

    if (open)
        wcscpy_s(result, 320, L"Direct: yes, with every player.");
    else if (r->kind == NAT_CONE)
        wcscpy_s(result, 320, L"Direct: yes with most players. Players on a strict router may go through the relay.");
    else if (r->kind == NAT_SYMMETRIC)
        wcscpy_s(result, 320, r->v6 == NAT_V6_WORKS
                     ? L"Direct: only with players on IPv6 or with an open port. With others, the relay (slower)."
                     : L"Direct: only with players who have an open port. With others, the relay (slower).");
    else if (r->kind == NAT_NO_UDP)
        wcscpy_s(result, 320, r->v6 == NAT_V6_WORKS ? L"Direct: only with players on IPv6. With others, the relay (slower)."
                                                    : L"Direct: no. Matches go through the relay (slower).");
    else
        wcscpy_s(result, 320, L"Direct: can't tell yet.");

    if (r->kind == NAT_NO_UDP)
        wcscpy_s(todo, 320, L"To do: try another network, or allow UDP on this one.");
    else if (r->kind == NAT_SYMMETRIC && r->shared)
        wcscpy_s(todo, 320, L"To do: your provider shares its address between customers, so a forwarded port can't help. "
                            L"Ask them for a public address, or use IPv6.");
    else if (r->kind == NAT_SYMMETRIC && mapped <= 0)
        swprintf_s(todo, 320, L"To do: turn on UPnP on your router and ROUTER PORT (UPNP) in the game's OPTIONS > GRAPHICS, "
                              L"or forward UDP %d to this PC (already forwarded? Then you're fine).", port);
    else
        todo[0] = 0;
    if (r->fw == NAT_FW_BLOCKED)
        wcscat_s(todo, 320, todo[0] ? L" In Windows Security, allow the game through the firewall."
                                    : L"To do: in Windows Security, allow the game through the firewall. Players on your "
                                      L"network can't find you until then.");

    swprintf_s(out, n, L"%s\r\nIPv6: %s   UPnP: %s\r\n%s\r\n%s\r\n%s%s(The other player's router matters too: "
                       L"this only sees your side.)",
               router[r->kind], r->v6 == NAT_V6_WORKS ? L"works." : L"not here.",
               mapped > 0 ? L"opened the game's port at its last start." : mapped < 0 ? L"didn't open the game's port at its last start."
                          : L"the game hasn't tried yet.",
               firewall[r->fw], result, todo, todo[0] ? L"\r\n" : L"");
}
