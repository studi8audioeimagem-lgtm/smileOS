#include "kernel.h"
#include "string.h"
#include "types.h"
#include "net.h"
#include "e1000.h"

/* Minimal IPv4 stack:
   - static config for QEMU user networking (10.0.2.15/24 gw 10.0.2.2)
   - ARP (resolve gateway MAC)
   - tiny TCP client for HTTP/1.0 GET (no retransmit, best-effort)

   This is intentionally small and not production-safe. */

typedef struct {
    u8 dst[6];
    u8 src[6];
    u16 ethertype;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    u16 htype;
    u16 ptype;
    u8 hlen;
    u8 plen;
    u16 oper;
    u8 sha[6];
    u32 spa;
    u8 tha[6];
    u32 tpa;
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    u8 ver_ihl;
    u8 tos;
    u16 total_len;
    u16 id;
    u16 frag_off;
    u8 ttl;
    u8 proto;
    u16 hdr_checksum;
    u32 src;
    u32 dst;
} __attribute__((packed)) ipv4_hdr_t;

typedef struct {
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    u8 data_off;
    u8 flags;
    u16 window;
    u16 checksum;
    u16 urg;
} __attribute__((packed)) tcp_hdr_t;

enum { ETH_P_ARP = 0x0806, ETH_P_IP = 0x0800 };

static net_state_t g_state;
static u8 g_gw_mac[6];
static bool g_gw_mac_ok;

static u16 bswap16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static u32 bswap32(u32 x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) << 8)  |
           ((x & 0x00FF0000U) >> 8)  |
           ((x & 0xFF000000U) >> 24);
}
static u16 htons(u16 x) { return bswap16(x); }
static u16 ntohs(u16 x) { return bswap16(x); }
static u32 htonl(u32 x) { return bswap32(x); }
static u32 ntohl(u32 x) { return bswap32(x); }

static u16 csum16(const void *data, usize len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    for (usize i = 0; i + 1 < len; i += 2) {
        sum += (u32)((p[i] << 8) | p[i + 1]);
    }
    if (len & 1) {
        sum += (u32)(p[len - 1] << 8);
    }
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (u16)(~sum);
}

static u32 sum16(const void *data, usize len) {
    const u8 *p = (const u8 *)data;
    u32 sum = 0;
    for (usize i = 0; i + 1 < len; i += 2) {
        sum += (u32)((p[i] << 8) | p[i + 1]);
    }
    if (len & 1) {
        sum += (u32)(p[len - 1] << 8);
    }
    return sum;
}

static void req_append_u8(char *req, usize cap, usize *io, u8 v) {
    if (!req || !io) return;
    char d[4];
    usize di = 0;
    if (v >= 100) { d[di++] = (char)('0' + (v / 100)); v %= 100; }
    if (di || v >= 10) { d[di++] = (char)('0' + (v / 10)); v %= 10; }
    d[di++] = (char)('0' + v);
    for (usize k = 0; k < di && *io + 1 < cap; k++) {
        req[(*io)++] = d[k];
    }
}

static bool mac_is_zero(const u8 m[6]) {
    for (u32 i = 0; i < 6; i++) if (m[i] != 0) return false;
    return true;
}

static void arp_send_request(u32 target_ip) {
    u8 frame[64];
    kmemset(frame, 0, sizeof(frame));

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    for (u32 i = 0; i < 6; i++) eth->dst[i] = 0xFF;
    for (u32 i = 0; i < 6; i++) eth->src[i] = g_state.mac[i];
    eth->ethertype = htons(ETH_P_ARP);

    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(*eth));
    arp->htype = htons(1);
    arp->ptype = htons(ETH_P_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(1); /* request */
    for (u32 i = 0; i < 6; i++) arp->sha[i] = g_state.mac[i];
    arp->spa = htonl(g_state.ip);
    kmemset(arp->tha, 0, 6);
    arp->tpa = htonl(target_ip);

    (void)e1000_send(frame, (u16)(sizeof(*eth) + sizeof(*arp)));
}

static void arp_handle(const u8 *pkt, u16 len) {
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return;
    const arp_pkt_t *arp = (const arp_pkt_t *)(pkt + sizeof(eth_hdr_t));
    if (ntohs(arp->oper) != 2) return; /* reply */
    const u32 spa = ntohl(arp->spa);
    if (spa == g_state.gw) {
        for (u32 i = 0; i < 6; i++) g_gw_mac[i] = arp->sha[i];
        g_gw_mac_ok = !mac_is_zero(g_gw_mac);
    }
}

static void ip_send_tcp(u32 dst_ip, const u8 dst_mac[6], const void *tcp, u16 tcp_len) {
    u8 frame[1514];
    if (tcp_len + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) > sizeof(frame)) return;

    eth_hdr_t *eth = (eth_hdr_t *)frame;
    for (u32 i = 0; i < 6; i++) eth->dst[i] = dst_mac[i];
    for (u32 i = 0; i < 6; i++) eth->src[i] = g_state.mac[i];
    eth->ethertype = htons(ETH_P_IP);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
    kmemset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = 6; /* TCP */
    ip->total_len = htons((u16)(sizeof(*ip) + tcp_len));
    ip->id = htons((u16)(scheduler_ticks() & 0xFFFFU));
    ip->frag_off = htons(0);
    ip->src = htonl(g_state.ip);
    ip->dst = htonl(dst_ip);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = csum16(ip, sizeof(*ip));

    u8 *tp = frame + sizeof(*eth) + sizeof(*ip);
    kmemcpy(tp, tcp, tcp_len);

    /* TCP checksum */
    {
        /* Pseudo header: src, dst, zero, proto, tcp len */
        u8 ph[12];
        kmemset(ph, 0, sizeof(ph));
        *(u32 *)&ph[0] = ip->src;
        *(u32 *)&ph[4] = ip->dst;
        ph[8] = 0;
        ph[9] = 6;
        *(u16 *)&ph[10] = htons(tcp_len);

        tcp_hdr_t *th = (tcp_hdr_t *)tp;
        th->checksum = 0;

        u32 sum = 0;
        sum += sum16(ph, sizeof(ph));
        sum += sum16(tp, tcp_len);
        while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
        th->checksum = (u16)(~sum);
    }

    (void)e1000_send(frame, (u16)(sizeof(*eth) + sizeof(*ip) + tcp_len));
}

typedef struct {
    bool in_use;
    u32 dst_ip;
    u16 src_port;
    u16 dst_port;
    u32 seq;
    u32 ack;
    bool established;
    bool fin_seen;
    char *out;
    usize out_cap;
    usize out_len;
} tcp_conn_t;

static tcp_conn_t g_http;

static void tcp_handle(const u8 *pkt, u16 len) {
    if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t)) return;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    const u32 ihl = (u32)(ip->ver_ihl & 0x0F) * 4U;
    if (ihl < sizeof(ipv4_hdr_t)) return;
    if (len < sizeof(eth_hdr_t) + ihl + sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t *th = (const tcp_hdr_t *)(pkt + sizeof(eth_hdr_t) + ihl);
    const u32 doff = (u32)((th->data_off >> 4) & 0x0F) * 4U;
    if (doff < sizeof(tcp_hdr_t)) return;
    if (len < sizeof(eth_hdr_t) + ihl + doff) return;

    const u16 dst_port = ntohs(th->dst_port);
    const u16 src_port = ntohs(th->src_port);
    if (!g_http.in_use) return;
    if (dst_port != g_http.src_port || src_port != g_http.dst_port) return;

    const u32 seq = ntohl(th->seq);
    const u32 ack = ntohl(th->ack);
    const u8 flags = th->flags;
    const u32 plen = (u32)len - (u32)(sizeof(eth_hdr_t) + ihl + doff);
    const u8 *payload = pkt + sizeof(eth_hdr_t) + ihl + doff;

    if ((flags & 0x12) == 0x12) { /* SYN+ACK */
        g_http.ack = seq + 1;
        g_http.seq = ack;
        g_http.established = true;
        return;
    }

    if (!g_http.established) {
        return;
    }

    /* Data */
    if (plen && payload && seq == g_http.ack) {
        const usize to_copy = (g_http.out_len + plen < g_http.out_cap) ? plen : (g_http.out_cap - g_http.out_len);
        if (to_copy) {
            kmemcpy(g_http.out + g_http.out_len, payload, to_copy);
            g_http.out_len += to_copy;
        }
        g_http.ack += plen;
    }

    if (flags & 0x01) { /* FIN */
        g_http.fin_seen = true;
        g_http.ack += 1;
    }
}

static void net_handle_frame(const u8 *frame, u16 len) {
    if (len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    const u16 et = ntohs(eth->ethertype);
    if (et == ETH_P_ARP) {
        arp_handle(frame, len);
        return;
    }
    if (et != ETH_P_IP) {
        return;
    }
    if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    if ((ip->ver_ihl >> 4) != 4) return;
    if (ip->proto == 6) {
        tcp_handle(frame, len);
    }
}

void net_init(void) {
    kmemset(&g_state, 0, sizeof(g_state));
    kmemset(g_gw_mac, 0, sizeof(g_gw_mac));
    g_gw_mac_ok = false;
    kmemset(&g_http, 0, sizeof(g_http));
    g_state.up = false;
    log_write("net: initialized (disconnected)");
}

void net_poll(void) {
    if (!g_state.up) return;
    u8 buf[1600];
    for (u32 i = 0; i < 4; i++) {
        const u16 n = e1000_recv(buf, sizeof(buf));
        if (!n) break;
        net_handle_frame(buf, n);
    }
}

const net_state_t *net_state(void) {
    return &g_state;
}

bool net_connect(void) {
    if (g_state.up) {
        return true;
    }
    if (!e1000_init()) {
        g_state.up = false;
        return false;
    }
    e1000_get_mac(g_state.mac);

    /* Static QEMU user-net defaults (works with -netdev user,... in QEMU). */
    g_state.ip = (10U << 24) | (0U << 16) | (2U << 8) | 15U; /* 10.0.2.15 */
    g_state.netmask = 0xFFFFFF00U; /* /24 */
    g_state.gw = (10U << 24) | (0U << 16) | (2U << 8) | 2U; /* 10.0.2.2 */
    g_state.up = true;
    g_gw_mac_ok = false;
    kmemset(g_gw_mac, 0, sizeof(g_gw_mac));
    log_write("net: connected (static 10.0.2.15 gw 10.0.2.2)");
    return true;
}

void net_disconnect(void) {
    /* Driver doesn't support clean shutdown yet; just mark down and clear ARP. */
    g_state.up = false;
    g_gw_mac_ok = false;
    kmemset(g_gw_mac, 0, sizeof(g_gw_mac));
    log_write("net: disconnected");
}

static bool ensure_gw_mac(void) {
    if (g_gw_mac_ok) return true;
    /* ARP a few times. */
    for (u32 tries = 0; tries < 8; tries++) {
        arp_send_request(g_state.gw);
        for (u32 spin = 0; spin < 20000U; spin++) {
            net_poll();
            if (g_gw_mac_ok) return true;
            __asm__ volatile ("pause");
        }
    }
    return false;
}

static void tcp_send(u32 dst_ip, u16 src_port, u16 dst_port, u32 seq, u32 ack, u8 flags, const void *payload, u16 payload_len) {
    u8 seg[1400];
    const u16 hlen = sizeof(tcp_hdr_t);
    if ((u32)hlen + (u32)payload_len > sizeof(seg)) return;

    tcp_hdr_t *th = (tcp_hdr_t *)seg;
    kmemset(th, 0, sizeof(*th));
    th->src_port = htons(src_port);
    th->dst_port = htons(dst_port);
    th->seq = htonl(seq);
    th->ack = htonl(ack);
    th->data_off = (u8)((hlen / 4U) << 4);
    th->flags = flags;
    th->window = htons(4096);
    th->checksum = 0;
    th->urg = 0;
    if (payload_len && payload) {
        kmemcpy(seg + hlen, payload, payload_len);
    }

    ip_send_tcp(dst_ip, g_gw_mac, seg, (u16)(hlen + payload_len));
}

usize net_http_get(u32 ip, const char *path, char *out, usize out_cap) {
    if (!g_state.up || !out || out_cap == 0) return 0;
    if (!ensure_gw_mac()) {
        return 0;
    }
    if (!path || path[0] == 0) path = "/";

    /* Setup connection slot. */
    kmemset(&g_http, 0, sizeof(g_http));
    g_http.in_use = true;
    g_http.dst_ip = ip;
    g_http.src_port = (u16)(49152U + (u16)(scheduler_ticks() & 0x0FFFU));
    g_http.dst_port = 80;
    g_http.seq = (u32)(scheduler_ticks() * 1103515245U + 12345U);
    g_http.ack = 0;
    g_http.out = out;
    g_http.out_cap = out_cap;
    g_http.out_len = 0;
    g_http.established = false;
    g_http.fin_seen = false;

    /* SYN */
    tcp_send(ip, g_http.src_port, g_http.dst_port, g_http.seq, 0, 0x02, 0, 0);
    const u32 syn_seq = g_http.seq;
    g_http.seq = syn_seq + 1;

    /* Wait SYN-ACK */
    for (u32 spin = 0; spin < 400000U; spin++) {
        net_poll();
        if (g_http.established) break;
        __asm__ volatile ("pause");
    }
    if (!g_http.established) {
        g_http.in_use = false;
        return 0;
    }

    /* ACK */
    tcp_send(ip, g_http.src_port, g_http.dst_port, g_http.seq, g_http.ack, 0x10, 0, 0);

    /* HTTP request */
    char req[256];
    kmemset(req, 0, sizeof(req));
    /* "GET <path> HTTP/1.0\r\nHost: <ip>\r\n\r\n" */
    usize o = 0;
    const char *a = "GET ";
    for (usize i = 0; a[i] && o + 1 < sizeof(req); i++) req[o++] = a[i];
    for (usize i = 0; path[i] && o + 1 < sizeof(req); i++) req[o++] = path[i];
    const char *b = " HTTP/1.0\r\nHost: ";
    for (usize i = 0; b[i] && o + 1 < sizeof(req); i++) req[o++] = b[i];
    /* host as dotted quad */
    {
        const u32 ipb = ip;
        const u8 p1 = (u8)((ipb >> 24) & 0xFF);
        const u8 p2 = (u8)((ipb >> 16) & 0xFF);
        const u8 p3 = (u8)((ipb >> 8) & 0xFF);
        const u8 p4 = (u8)(ipb & 0xFF);
        req_append_u8(req, sizeof(req), &o, p1); if (o + 1 < sizeof(req)) req[o++] = '.';
        req_append_u8(req, sizeof(req), &o, p2); if (o + 1 < sizeof(req)) req[o++] = '.';
        req_append_u8(req, sizeof(req), &o, p3); if (o + 1 < sizeof(req)) req[o++] = '.';
        req_append_u8(req, sizeof(req), &o, p4);
    }
    const char *c = "\r\nUser-Agent: smileOS\r\nConnection: close\r\n\r\n";
    for (usize i = 0; c[i] && o + 1 < sizeof(req); i++) req[o++] = c[i];

    tcp_send(ip, g_http.src_port, g_http.dst_port, g_http.seq, g_http.ack, 0x18, req, (u16)o); /* PSH|ACK */
    g_http.seq += (u32)o;

    /* Receive data until FIN or timeout. */
    for (u32 spin = 0; spin < 1200000U; spin++) {
        net_poll();
        if (g_http.fin_seen) break;
        __asm__ volatile ("pause");
    }

    /* Final ACK */
    tcp_send(ip, g_http.src_port, g_http.dst_port, g_http.seq, g_http.ack, 0x10, 0, 0);
    g_http.in_use = false;

    if (g_http.out_len < g_http.out_cap) {
        g_http.out[g_http.out_len] = 0;
    } else if (g_http.out_cap) {
        g_http.out[g_http.out_cap - 1] = 0;
    }
    return g_http.out_len;
}
