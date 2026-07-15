/*
 * nsfip.c -- IPv4 input, output and routing (see nsfip.h, spec ch. 11).
 *
 * Header fields are read and written BYTE BY BYTE (big-endian = native S/370),
 * never through a struct overlay or a USHORT/UINT cast: a cast round-trips green
 * on both host and target while emitting host-endian bytes on the little-endian
 * test box. Addresses live as UINTs (octet-1 in the MSB) and all routing/is-local
 * logic is UINT arithmetic; only the four header address bytes touch the wire,
 * byte-wise (see the header's ADDRESS CONVENTION note).
 */
#include "nsfip.h"
#include "nsficmp.h"            /* nsficmp_input (ICMP demux target) */
#include "nsfcksum.h"           /* in_cksum                          */
#include "nsfsts.h"
#include "nsftrc.h"
#include <string.h>

/* -- big-endian byte-wise accessors (see the file header) --------------------- */
static USHORT get16(const UCHAR *p)
{
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

static UINT get32(const UCHAR *p)
{
    return ((UINT)p[0] << 24) | ((UINT)p[1] << 16) |
           ((UINT)p[2] << 8)  |  (UINT)p[3];
}

static void put16(UCHAR *p, USHORT v)
{
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

static void put32(UCHAR *p, UINT v)
{
    p[0] = (UCHAR)(v >> 24);
    p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);
    p[3] = (UCHAR)v;
}

/* -- statistics (spec 11.7) ---------------------------------------------------
 * §11.7's minimum set, extended with `inaddrerr` (RFC 1213 ipInAddrErrors) for
 * a packet whose destination is not one of ours -- there is no §11.7 counter for
 * "not for this host" because NSF is a host, not a router (the set is a minimum,
 * spec 11.7). Pointers are cached once and NULL-guarded on every increment
 * (STS_INC(NULL) is a null-deref on MVS; sts_register returns NULL if the
 * registry is full). */
static STSCTR *ip_in, *ip_out, *ip_hdrerr, *ip_badcksum, *ip_badlen;
static STSCTR *ip_noproto, *ip_noroute, *ip_fragdrop, *ip_ttlexp, *ip_inaddrerr;
static int     ip_stats_ready;

static void ipc(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

static void ip_stats_init(void)
{
    if (ip_stats_ready) {
        return;
    }
    ip_in        = sts_register("NSFIP", "in");
    ip_out       = sts_register("NSFIP", "out");
    ip_hdrerr    = sts_register("NSFIP", "hdrerr");
    ip_badcksum  = sts_register("NSFIP", "badcksum");
    ip_badlen    = sts_register("NSFIP", "badlen");
    ip_noproto   = sts_register("NSFIP", "noproto");
    ip_noroute   = sts_register("NSFIP", "noroute");
    ip_fragdrop  = sts_register("NSFIP", "fragdrop");
    ip_ttlexp    = sts_register("NSFIP", "ttlexp");
    ip_inaddrerr = sts_register("NSFIP", "inaddrerr");
    ip_stats_ready = 1;
    (void)ip_ttlexp;    /* parsed but not a delivery gate; reserved for forward */
}

/* -- routing table (spec 11.4) ------------------------------------------------ */
typedef struct iproute {
    UINT    net;                /* network (already masked)                    */
    UINT    mask;              /* prefix mask; 0 == default                    */
    UINT    gw;               /* next hop, 0 == on-link (direct)               */
    NETDEV *dev;             /* outbound device                                */
} IPROUTE;

static IPROUTE g_route[NSFIP_MAX_ROUTES];
static UINT    g_nroute;
static UINT    g_local[NSFIP_MAX_LOCAL];
static UINT    g_nlocal;

/* -- transport demux handlers (spec 11.1) ------------------------------------
 * A tiny fixed table proto -> handler, consulted by nsfip_input BEFORE the
 * noproto/protocol-unreachable fallback. Deliberately SEPARATE from the routing
 * table: handler wiring is static (which C function services which protocol),
 * not per-config, so nsfip_init/nsfip_config do NOT reset it -- registration is
 * idempotent-replace and order-independent. Kept small (UDP now, TCP at M4). */
#define NSFIP_MAX_PROTO   4

typedef struct ipproto {
    UCHAR          inuse;
    UCHAR          proto;
    NSFIP_PROTO_FN fn;
} IPPROTO;

static IPPROTO g_proto[NSFIP_MAX_PROTO];

int nsfip_register_proto(UCHAR proto, NSFIP_PROTO_FN fn)
{
    UINT i;

    /* Replace an existing registration for the same proto (idempotent re-init). */
    for (i = 0u; i < NSFIP_MAX_PROTO; i++) {
        if (g_proto[i].inuse && g_proto[i].proto == proto) {
            g_proto[i].fn = fn;
            return 0;
        }
    }
    for (i = 0u; i < NSFIP_MAX_PROTO; i++) {
        if (!g_proto[i].inuse) {
            g_proto[i].inuse = 1u;
            g_proto[i].proto = proto;
            g_proto[i].fn    = fn;
            return 0;
        }
    }
    return -1;                          /* table full */
}

static NSFIP_PROTO_FN ip_proto_lookup(UCHAR proto)
{
    UINT i;

    for (i = 0u; i < NSFIP_MAX_PROTO; i++) {
        if (g_proto[i].inuse && g_proto[i].proto == proto) {
            return g_proto[i].fn;
        }
    }
    return NULL;
}

/* Classful mask for `ip` (used for on-link HOME routes and a maskless GATEWAY):
 * A/B/C by the first octet; class D/E collapses to a host route. */
static UINT classful_mask(UINT ip)
{
    UCHAR top = (UCHAR)(ip >> 24);

    if (top < 128u) {
        return 0xFF000000u;             /* class A: /8  */
    }
    if (top < 192u) {
        return 0xFFFF0000u;             /* class B: /16 */
    }
    if (top < 224u) {
        return 0xFFFFFF00u;             /* class C: /24 */
    }
    return 0xFFFFFFFFu;                 /* class D/E: host route */
}

void nsfip_init(void)
{
    ip_stats_init();
    g_nroute = 0u;
    g_nlocal = 0u;
    memset(g_route, 0, sizeof(g_route));
    memset(g_local, 0, sizeof(g_local));
}

int nsfip_route_add(UINT net, UINT mask, NETDEV *dev, UINT gw)
{
    if (g_nroute >= NSFIP_MAX_ROUTES) {
        return -1;
    }
    g_route[g_nroute].net  = net & mask;        /* normalize the network */
    g_route[g_nroute].mask = mask;
    g_route[g_nroute].gw   = gw;
    g_route[g_nroute].dev  = dev;
    g_nroute++;
    return 0;
}

int nsfip_local_add(UINT ip)
{
    UINT i;

    for (i = 0u; i < g_nlocal; i++) {
        if (g_local[i] == ip) {
            return 0;                   /* already recorded */
        }
    }
    if (g_nlocal >= NSFIP_MAX_LOCAL) {
        return -1;
    }
    g_local[g_nlocal++] = ip;
    return 0;
}

int nsfip_is_local(UINT ip)
{
    UINT i;

    for (i = 0u; i < g_nlocal; i++) {
        if (g_local[i] == ip) {
            return 1;
        }
    }
    return 0;
}

NETDEV *nsfip_route(UINT dst, UINT *nexthop)
{
    UINT i;
    INT  best     = -1;
    UINT bestmask = 0u;

    for (i = 0u; i < g_nroute; i++) {
        if (g_route[i].dev == NULL) {
            continue;                   /* unresolved route: unusable */
        }
        if ((dst & g_route[i].mask) == g_route[i].net) {
            if (best < 0 || g_route[i].mask > bestmask) {
                best     = (INT)i;      /* longest prefix (largest mask) wins */
                bestmask = g_route[i].mask;
            }
        }
    }
    if (best < 0) {
        return NULL;
    }
    if (nexthop != NULL) {
        /* on-link (gw 0) -> the destination is directly reachable (the peer on a
         * point-to-point link); otherwise the configured gateway. */
        *nexthop = (g_route[best].gw != 0u) ? g_route[best].gw : dst;
    }
    return g_route[best].dev;
}

int nsfip_config(const NSFCFG *cfg)
{
    UINT i;

    nsfip_init();

    /* HOME: our own address + an on-link (classful) network route out its link. */
    for (i = 0u; i < cfg->nhome; i++) {
        UINT    ip  = cfg->home[i].ip;
        NETDEV *dev = dev_find(cfg->home[i].link);

        (void)nsfip_local_add(ip);
        if (dev != NULL) {
            UINT mask = classful_mask(ip);
            (void)nsfip_route_add(ip & mask, mask, dev, 0u);
        }
    }

    /* GATEWAY: a default route (DEFAULTNET) or an explicit network route. */
    for (i = 0u; i < cfg->ngw; i++) {
        const NSFCFGGW *g   = &cfg->gw[i];
        NETDEV         *dev = dev_find(g->link);

        if (dev == NULL) {
            continue;                   /* link/device not present: skip */
        }
        if (g->is_default) {
            (void)nsfip_route_add(0u, 0u, dev, g->firsthop);
        } else {
            UINT mask = (g->mask != 0u) ? g->mask : classful_mask(g->net);
            (void)nsfip_route_add(g->net & mask, mask, dev, g->firsthop);
        }
    }
    return 0;
}

/* -- input -------------------------------------------------------------------- */
void nsfip_input(NETDEV *dev, PBUF *b)
{
    const UCHAR *p;
    USHORT       blen, total, frag;
    UCHAR        ver, ihl, hlen, proto;
    UINT         src, dst;

    if (b == NULL) {
        return;
    }
    ipc(ip_in);                                 /* every received IP packet */

    blen = buf_chain_len(b);
    if (blen < (USHORT)NSFIP_HDR_MIN || b->len < (USHORT)NSFIP_HDR_MIN) {
        ipc(ip_badlen);                         /* runt: no fixed header */
        buf_free(b);
        return;
    }
    p   = b->data;                              /* header is contiguous (drivers
                                                 * deliver one segment, <=256 B) */
    ver = (UCHAR)(p[0] >> 4);
    ihl = (UCHAR)(p[0] & 0x0Fu);
    if (ver != 4u || ihl < 5u) {
        ipc(ip_hdrerr);
        buf_free(b);
        return;
    }
    hlen = (UCHAR)(ihl * 4u);
    if (b->len < hlen) {                         /* header (incl. options) not
                                                  * contiguous in the first seg */
        ipc(ip_hdrerr);
        buf_free(b);
        return;
    }
    total = get16(p + 2);
    if (total < (USHORT)hlen || total > blen) {
        ipc(ip_badlen);
        buf_free(b);
        return;
    }
    /* Header checksum over the whole header (incl. options) must be zero. */
    if (in_cksum(b, 0u, (USHORT)hlen) != 0u) {
        ipc(ip_badcksum);
        buf_free(b);
        return;
    }
    /* Options (IHL > 5) are validated for length only -- bounded above by
     * b->len >= hlen -- and otherwise skipped (spec 11.1). */

    /* Fragments are not reassembled in v1 (spec 11.3): drop MF or nonzero
     * offset. */
    frag = get16(p + 6);
    if ((frag & 0x2000u) != 0u || (frag & 0x1FFFu) != 0u) {
        ipc(ip_fragdrop);
        buf_free(b);
        return;
    }

    /* Not-for-us: a host stack never forwards (spec 11.1). */
    src = get32(p + 12);
    dst = get32(p + 16);
    if (!nsfip_is_local(dst)) {
        ipc(ip_inaddrerr);
        buf_free(b);
        return;
    }
    /* TTL (p[8]) is PARSED but NOT a delivery gate: RFC 1122 §3.2.1.7 forbids a
     * destination host from discarding a datagram addressed to it on low TTL --
     * TTL expiry is a forwarding concern NSF does not have. ip_ttlexp stays 0 in
     * v1, reserved for a future forward path. */

    /* Trim any trailing padding beyond the IP total length so a transport's
     * length math (e.g. the ICMP checksum span) is exact. */
    if (blen > total) {
        (void)buf_trim_tail(b, (USHORT)(blen - total));
    }

    proto = p[9];
    TRC(IP, "IN %u.%u.%u.%u -> %u.%u.%u.%u proto %u len %u",
        (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
        (unsigned)((src >> 8) & 0xFFu),  (unsigned)(src & 0xFFu),
        (unsigned)((dst >> 24) & 0xFFu), (unsigned)((dst >> 16) & 0xFFu),
        (unsigned)((dst >> 8) & 0xFFu),  (unsigned)(dst & 0xFFu),
        (unsigned)proto, (unsigned)total);

    if (proto == (UCHAR)NSFIP_PROTO_ICMP) {
        nsficmp_input(dev, b, (const IPHDR *)p);    /* hands off ownership */
        return;
    }
    {
        NSFIP_PROTO_FN fn = ip_proto_lookup(proto);
        if (fn != NULL) {
            fn(dev, b, (const IPHDR *)p);           /* hands off ownership */
            return;
        }
    }
    /* No transport registered for this protocol: this stack has no listener for
     * it, full stop, so "protocol unreachable" (RFC 792, code 2) is accurate --
     * the LIVE M2-4 trigger for nsficmp_send_error. UDP/TCP register through
     * nsfip_register_proto when present (M3-3/M4); until then, and for genuinely
     * unknown protocols, this fallback fires. Closed-PORT "port unreachable"
     * (code 3) is a UDP/TCP concern raised from inside the transport handler, not
     * here (spec 11.2). send_error reads b (still ours) and does not take
     * ownership -- we free it right after, as every other drop does. */
    ipc(ip_noproto);
    nsficmp_send_error(b, (UCHAR)NSFICMP_DEST_UNREACH,
                       (UCHAR)NSFICMP_UNREACH_PROTO);
    buf_free(b);
}

/* -- output ------------------------------------------------------------------- */
int nsfip_output(PBUF *b, UINT src, UINT dst, UCHAR proto, UCHAR ttl)
{
    static USHORT ip_id;                /* monotonic identification counter */
    UCHAR        *p;
    USHORT        total;
    NETDEV       *dev;
    UINT          nexthop = 0u;

    if (b == NULL) {
        return -1;
    }
    dev = nsfip_route(dst, &nexthop);
    if (dev == NULL) {
        ipc(ip_noroute);
        buf_free(b);
        return -1;
    }
    /* Claim the 20-byte fixed header from the headroom the buffer reserves for
     * exactly this (spec 3.3). This cannot fail for our own allocations (64 B
     * headroom); guard it anyway rather than write out of bounds. */
    if (buf_prepend(b, (USHORT)NSFIP_HDR_MIN) != 0) {
        ipc(ip_hdrerr);
        buf_free(b);
        return -1;
    }
    p     = b->data;
    total = buf_chain_len(b);           /* header + payload */

    p[0] = (UCHAR)0x45u;                /* version 4, IHL 5 (no options) */
    p[1] = 0x00u;                       /* TOS */
    put16(p + 2, total);
    put16(p + 4, ++ip_id);              /* identification */
    put16(p + 6, 0x0000u);              /* flags 0, fragment offset 0 (no DF) */
    p[8] = ttl;
    p[9] = proto;
    put16(p + 10, 0x0000u);             /* checksum field zero before computing */
    put32(p + 12, src);
    put32(p + 16, dst);
    put16(p + 10, in_cksum(b, 0u, (USHORT)NSFIP_HDR_MIN));

    TRC(IP, "OUT %u.%u.%u.%u -> %u.%u.%u.%u proto %u len %u",
        (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
        (unsigned)((src >> 8) & 0xFFu),  (unsigned)(src & 0xFFu),
        (unsigned)((dst >> 24) & 0xFFu), (unsigned)((dst >> 16) & 0xFFu),
        (unsigned)((dst >> 8) & 0xFFu),  (unsigned)(dst & 0xFFu),
        (unsigned)proto, (unsigned)total);

    /* dev_send takes ownership unconditionally: on rejection it has already freed
     * b and counted the device error, so we never touch b afterwards. */
    if (dev_send(dev, b) != 0) {
        return -1;
    }
    ipc(ip_out);
    return 0;
}
