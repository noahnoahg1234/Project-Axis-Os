/*
 * AxisOS networking: Intel e1000 driver + a small polled TCP/IP stack
 * (Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP) and an HTTP/1.0 client.
 *
 * Everything is polled (no NIC interrupts). Only one TCP connection is open
 * at a time, which is all the browser, wget and the app store need.
 */
#include "axis.h"

/* ---- byte order ------------------------------------------------------- */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t htonl(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
#define ntohs htons
#define ntohl htonl

uint8_t net_mac[6];
uint32_t net_ip, net_mask, net_gw, net_dns; /* host byte order */
static bool nic_ok, configured;
static char nic_name[32];
static uint32_t rx_packets, tx_packets;

/* ======================================================= e1000 driver === */
#define E_CTRL 0x0000
#define E_STATUS 0x0008
#define E_EERD 0x0014
#define E_IMC 0x00D8
#define E_RCTL 0x0100
#define E_TCTL 0x0400
#define E_TIPG 0x0410
#define E_RDBAL 0x2800
#define E_RDBAH 0x2804
#define E_RDLEN 0x2808
#define E_RDH 0x2810
#define E_RDT 0x2818
#define E_TDBAL 0x3800
#define E_TDBAH 0x3804
#define E_TDLEN 0x3808
#define E_TDH 0x3810
#define E_TDT 0x3818
#define E_MTA 0x5200
#define E_RAL 0x5400
#define E_RAH 0x5404

#define NRX 32
#define NTX 16
#define BUFSZ 2048

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t len, csum;
    uint8_t status, errors;
    uint16_t special;
} rx_desc_t;
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t len;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} tx_desc_t;

static volatile uint8_t *mmio;
static rx_desc_t *rxd;
static tx_desc_t *txd;
static uint8_t *rxbuf, *txbuf;
static uint32_t rx_cur, tx_cur;

static void ew(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static uint32_t er(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }

static uint32_t pci_read(int bus, int dev, int fn, int off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void pci_write(int bus, int dev, int fn, int off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

static void *aligned_alloc16(size_t n, size_t align) {
    uint8_t *p = kzalloc(n + align);
    if (!p) return NULL;
    return (void *)(((uint32_t)p + align - 1) & ~(align - 1));
}

static bool e1000_probe(void) {
    static const uint16_t ids[] = {0x100E, 0x100F, 0x1004, 0x10D3, 0x153A, 0x10EA, 0x1502};
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read(bus, dev, 0, 0);
            if ((id & 0xFFFF) != 0x8086) continue;
            uint16_t did = id >> 16;
            for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
                if (did != ids[i]) continue;
                uint32_t bar0 = pci_read(bus, dev, 0, 0x10);
                if (bar0 & 1) continue; /* need MMIO */
                mmio = (volatile uint8_t *)(bar0 & 0xFFFFFFF0);
                uint32_t cmd = pci_read(bus, dev, 0, 0x04);
                pci_write(bus, dev, 0, 0x04, cmd | 0x6); /* memory space + bus master */
                snprintf(nic_name, sizeof nic_name, "e1000 (8086:%04x)", did);
                return true;
            }
        }
    return false;
}

static bool e1000_init(void) {
    if (!e1000_probe()) return false;
    ew(E_IMC, 0xFFFFFFFF);
    ew(E_CTRL, er(E_CTRL) | (1u << 26)); /* reset */
    for (int i = 0; i < 100000 && (er(E_CTRL) & (1u << 26)); i++) io_wait();
    sleep_ms(20);
    ew(E_IMC, 0xFFFFFFFF);
    ew(E_CTRL, (er(E_CTRL) | (1 << 6) | (1 << 5)) & ~((1u << 3) | (1u << 31) | (1u << 7)));

    uint32_t lo = er(E_RAL), hi = er(E_RAH);
    if (!lo && !(hi & 0xFFFF)) {
        /* read the MAC from the EEPROM */
        for (int w = 0; w < 3; w++) {
            ew(E_EERD, 1 | ((uint32_t)w << 8));
            uint32_t v = 0;
            for (int t = 0; t < 100000; t++)
                if ((v = er(E_EERD)) & (1 << 4)) break;
            net_mac[w * 2] = (v >> 16) & 0xFF;
            net_mac[w * 2 + 1] = (v >> 24) & 0xFF;
        }
    } else {
        for (int i = 0; i < 4; i++) net_mac[i] = (lo >> (i * 8)) & 0xFF;
        net_mac[4] = hi & 0xFF;
        net_mac[5] = (hi >> 8) & 0xFF;
    }
    ew(E_RAL, net_mac[0] | net_mac[1] << 8 | net_mac[2] << 16 | (uint32_t)net_mac[3] << 24);
    ew(E_RAH, net_mac[4] | net_mac[5] << 8 | (1u << 31));
    for (int i = 0; i < 128; i++) ew(E_MTA + i * 4, 0);

    rxd = aligned_alloc16(sizeof(rx_desc_t) * NRX, 128);
    txd = aligned_alloc16(sizeof(tx_desc_t) * NTX, 128);
    rxbuf = aligned_alloc16(BUFSZ * NRX, 16);
    txbuf = aligned_alloc16(BUFSZ * NTX, 16);
    if (!rxd || !txd || !rxbuf || !txbuf) return false;
    for (int i = 0; i < NRX; i++) rxd[i].addr = (uint32_t)(rxbuf + i * BUFSZ);
    for (int i = 0; i < NTX; i++) {
        txd[i].addr = (uint32_t)(txbuf + i * BUFSZ);
        txd[i].status = 1; /* DD: free */
    }
    ew(E_RDBAL, (uint32_t)rxd);
    ew(E_RDBAH, 0);
    ew(E_RDLEN, sizeof(rx_desc_t) * NRX);
    ew(E_RDH, 0);
    ew(E_RDT, NRX - 1);
    rx_cur = 0;
    /* EN | UPE | MPE | BAM | BSIZE 2048 | SECRC */
    ew(E_RCTL, (1 << 1) | (1 << 3) | (1 << 4) | (1 << 15) | (1 << 26));

    ew(E_TDBAL, (uint32_t)txd);
    ew(E_TDBAH, 0);
    ew(E_TDLEN, sizeof(tx_desc_t) * NTX);
    ew(E_TDH, 0);
    ew(E_TDT, 0);
    tx_cur = 0;
    ew(E_TCTL, (1 << 1) | (1 << 3) | (0x10 << 4) | (0x40 << 12));
    ew(E_TIPG, 0x0060200A);
    return true;
}

static void nic_send(const void *frame, uint32_t len) {
    if (!nic_ok) return;
    tx_desc_t *d = &txd[tx_cur];
    for (int i = 0; i < 1000000 && !(d->status & 1); i++) {}
    if (len < 60) {
        memset(txbuf + tx_cur * BUFSZ, 0, 60);
    }
    memcpy(txbuf + tx_cur * BUFSZ, frame, len);
    d->len = (uint16_t)(len < 60 ? 60 : len);
    d->cmd = (1 << 0) | (1 << 1) | (1 << 3); /* EOP | IFCS | RS */
    d->status = 0;
    tx_cur = (tx_cur + 1) % NTX;
    ew(E_TDT, tx_cur);
    tx_packets++;
}

/* ===================================================== protocol layer === */
typedef struct __attribute__((packed)) {
    uint8_t dst[6], src[6];
    uint16_t type;
} eth_t;
typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} arp_t;
typedef struct __attribute__((packed)) {
    uint8_t vihl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} ip_t;
typedef struct __attribute__((packed)) {
    uint16_t sport, dport, len, csum;
} udp_t;
typedef struct __attribute__((packed)) {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, csum, urg;
} tcp_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

static uint16_t csum16(const void *data, uint32_t len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}
static uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len) {
    /* src, dst in network order */
    uint32_t s = 0;
    s += ntohl(src) >> 16;
    s += ntohl(src) & 0xFFFF;
    s += ntohl(dst) >> 16;
    s += ntohl(dst) & 0xFFFF;
    s += proto;
    s += len;
    return s;
}

/* ---- ARP --------------------------------------------------------------- */
static struct { uint32_t ip; uint8_t mac[6]; } arp_cache[16];
static int arp_next;
static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void arp_store(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < 16; i++)
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    arp_cache[arp_next].ip = ip;
    memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % 16;
}

static void eth_send(const uint8_t *dst, uint16_t type, const void *payload, uint32_t len) {
    static uint8_t frame[1600];
    eth_t *e = (eth_t *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, net_mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof(eth_t), payload, len);
    nic_send(frame, sizeof(eth_t) + len);
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa_host) {
    arp_t a;
    a.htype = htons(1);
    a.ptype = htons(0x0800);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, net_mac, 6);
    a.spa = htonl(net_ip);
    memcpy(a.tha, tha, 6);
    a.tpa = htonl(tpa_host);
    eth_send(op == 1 ? bcast : tha, 0x0806, &a, sizeof a);
}

static bool arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < 16; i++)
        if (arp_cache[i].ip == ip && ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    return false;
}

/* ---- receive path ------------------------------------------------------ */
static volatile int ping_reply_seq = -1;
static uint16_t udp_wait_port;
static uint8_t *udp_buf;
static uint32_t udp_len;
static bool udp_got;

/* TCP connection state */
enum { T_CLOSED, T_SYN_SENT, T_EST, T_FIN_WAIT, T_CLOSE_WAIT };
static struct {
    int state;
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t snd_nxt, snd_una, rcv_nxt;
    uint8_t *rx;
    uint32_t rxlen, rxcap;
    bool remote_closed, reset;
    uint32_t rx_limit;
} tc;

static void ip_send(uint32_t dst_host, uint8_t proto, const void *payload, uint32_t len);

static void tcp_send_seg(uint8_t flags, const void *data, uint32_t len) {
    static uint8_t seg[1600];
    tcp_t *t = (tcp_t *)seg;
    uint32_t hl = sizeof(tcp_t);
    memset(t, 0, sizeof *t);
    t->sport = htons(tc.lport);
    t->dport = htons(tc.rport);
    t->seq = htonl(tc.snd_nxt);
    t->ack = htonl(tc.rcv_nxt);
    t->flags = flags;
    t->win = htons(64240);
    if (flags & TCP_SYN) {
        /* MSS option 1460 */
        seg[hl] = 2;
        seg[hl + 1] = 4;
        seg[hl + 2] = 1460 >> 8;
        seg[hl + 3] = 1460 & 0xFF;
        hl += 4;
    }
    t->off = (uint8_t)((hl / 4) << 4);
    if (len) memcpy(seg + hl, data, len);
    uint32_t total = hl + len;
    t->csum = 0;
    t->csum = csum16(seg, total, pseudo_sum(htonl(net_ip), htonl(tc.rip), 6, (uint16_t)total));
    ip_send(tc.rip, 6, seg, total);
}

static void tcp_input(const ip_t *ip, const uint8_t *p, uint32_t len) {
    if (len < sizeof(tcp_t)) return;
    const tcp_t *t = (const tcp_t *)p;
    if (tc.state == T_CLOSED || ntohs(t->dport) != tc.lport || ntohs(t->sport) != tc.rport || ntohl(ip->src) != tc.rip)
        return;
    uint32_t hl = (t->off >> 4) * 4;
    if (hl > len) return;
    const uint8_t *data = p + hl;
    uint32_t dlen = len - hl;
    uint32_t seq = ntohl(t->seq), ack = ntohl(t->ack);
    if (t->flags & TCP_RST) {
        tc.reset = true;
        tc.state = T_CLOSED;
        return;
    }
    if (tc.state == T_SYN_SENT) {
        if ((t->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == tc.snd_nxt) {
            tc.rcv_nxt = seq + 1;
            tc.snd_una = ack;
            tc.state = T_EST;
            tcp_send_seg(TCP_ACK, NULL, 0);
        }
        return;
    }
    if (t->flags & TCP_ACK) {
        if ((int32_t)(ack - tc.snd_una) > 0) tc.snd_una = ack;
    }
    bool need_ack = false;
    if (dlen) {
        if (seq == tc.rcv_nxt) {
            if (tc.rxlen + dlen > tc.rxcap) {
                uint32_t nc = tc.rxcap ? tc.rxcap : 16384;
                while (nc < tc.rxlen + dlen) nc *= 2;
                uint8_t *nb = krealloc(tc.rx, nc + 1);
                if (!nb) {
                    tc.reset = true;
                    return;
                }
                tc.rx = nb;
                tc.rxcap = nc;
            }
            memcpy(tc.rx + tc.rxlen, data, dlen);
            tc.rxlen += dlen;
            tc.rcv_nxt += dlen;
        }
        need_ack = true; /* re-ack out-of-order data so the sender retransmits */
    }
    if ((t->flags & TCP_FIN) && seq + dlen == tc.rcv_nxt) {
        tc.rcv_nxt++;
        tc.remote_closed = true;
        need_ack = true;
    }
    if (need_ack) tcp_send_seg(TCP_ACK, NULL, 0);
}

static void ip_input(const uint8_t *p, uint32_t len) {
    if (len < sizeof(ip_t)) return;
    const ip_t *ip = (const ip_t *)p;
    if ((ip->vihl >> 4) != 4) return;
    uint32_t hl = (ip->vihl & 0xF) * 4;
    uint32_t tot = ntohs(ip->len);
    if (tot > len || hl > tot) return;
    if (configured && ntohl(ip->dst) != net_ip && ip->dst != 0xFFFFFFFF) return;
    const uint8_t *pl = p + hl;
    uint32_t plen = tot - hl;
    if (ip->proto == 1 && plen >= 8) { /* ICMP */
        if (pl[0] == 0) {
            ping_reply_seq = (pl[6] << 8) | pl[7];
        } else if (pl[0] == 8) { /* echo request: answer it */
            uint8_t *r = kmalloc(plen);
            if (r) {
                memcpy(r, pl, plen);
                r[0] = 0;
                r[2] = r[3] = 0;
                uint16_t c = csum16(r, plen, 0);
                memcpy(r + 2, &c, 2);
                ip_send(ntohl(ip->src), 1, r, plen);
                kfree(r);
            }
        }
    } else if (ip->proto == 17 && plen >= sizeof(udp_t)) {
        const udp_t *u = (const udp_t *)pl;
        if (udp_wait_port && ntohs(u->dport) == udp_wait_port && !udp_got) {
            udp_len = ntohs(u->len) - sizeof(udp_t);
            if (udp_len > 1500) udp_len = 1500;
            memcpy(udp_buf, pl + sizeof(udp_t), udp_len);
            udp_got = true;
        }
    } else if (ip->proto == 6) {
        tcp_input(ip, pl, plen);
    }
}

void net_poll(void) {
    if (!nic_ok) return;
    for (int n = 0; n < NRX; n++) {
        rx_desc_t *d = &rxd[rx_cur];
        if (!(d->status & 1)) break;
        uint8_t *f = rxbuf + rx_cur * BUFSZ;
        uint32_t len = d->len;
        rx_packets++;
        if (len >= sizeof(eth_t)) {
            eth_t *e = (eth_t *)f;
            uint16_t type = ntohs(e->type);
            uint8_t *pl = f + sizeof(eth_t);
            uint32_t plen = len - sizeof(eth_t);
            if (type == 0x0806 && plen >= sizeof(arp_t)) {
                arp_t *a = (arp_t *)pl;
                arp_store(ntohl(a->spa), a->sha);
                if (ntohs(a->op) == 1 && configured && ntohl(a->tpa) == net_ip) arp_send(2, a->sha, ntohl(a->spa));
            } else if (type == 0x0800) {
                ip_input(pl, plen);
            }
        }
        d->status = 0;
        ew(E_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

static bool resolve_mac(uint32_t ip, uint8_t *mac) {
    if (ip == 0xFFFFFFFF || !configured) {
        memcpy(mac, bcast, 6);
        return true;
    }
    uint32_t hop = ((ip & net_mask) == (net_ip & net_mask)) ? ip : net_gw;
    if (arp_lookup(hop, mac)) return true;
    for (int attempt = 0; attempt < 4; attempt++) {
        arp_send(1, (const uint8_t *)"\0\0\0\0\0\0", hop);
        uint32_t t = ticks;
        while (ticks - t < 50) {
            net_poll();
            if (arp_lookup(hop, mac)) return true;
        }
    }
    return false;
}

static void ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len) {
    static uint8_t pkt[1600];
    static uint16_t ip_id = 1;
    uint8_t mac[6];
    if (!resolve_mac(dst, mac)) return;
    ip_t *ip = (ip_t *)pkt;
    ip->vihl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)(sizeof(ip_t) + len));
    ip->id = htons(ip_id++);
    ip->frag = htons(0x4000);
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = htonl(net_ip);
    ip->dst = htonl(dst);
    ip->csum = csum16(ip, sizeof(ip_t), 0);
    memcpy(pkt + sizeof(ip_t), payload, len);
    eth_send(mac, 0x0800, pkt, sizeof(ip_t) + len);
}

static void udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, uint32_t len) {
    static uint8_t buf[1600];
    udp_t *u = (udp_t *)buf;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(sizeof(udp_t) + len));
    u->csum = 0;
    memcpy(buf + sizeof(udp_t), data, len);
    u->csum = csum16(buf, sizeof(udp_t) + len, pseudo_sum(htonl(net_ip), htonl(dst), 17, (uint16_t)(sizeof(udp_t) + len)));
    if (!u->csum) u->csum = 0xFFFF;
    ip_send(dst, 17, buf, sizeof(udp_t) + len);
}

/* wait for a UDP reply on a port; returns length or -1 */
static int udp_wait(uint16_t port, uint8_t *buf, uint32_t timeout_ms) {
    udp_wait_port = port;
    udp_buf = buf;
    udp_got = false;
    uint32_t end = ticks + timeout_ms / 10;
    while ((int32_t)(end - ticks) > 0) {
        net_poll();
        if (udp_got) {
            udp_wait_port = 0;
            return (int)udp_len;
        }
    }
    udp_wait_port = 0;
    return -1;
}

/* ---- DHCP -------------------------------------------------------------- */
static int dhcp_build(uint8_t *b, uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t server) {
    memset(b, 0, 300);
    b[0] = 1;
    b[1] = 1;
    b[2] = 6;
    memcpy(b + 4, &xid, 4);
    b[10] = 0x80; /* broadcast flag */
    memcpy(b + 28, net_mac, 6);
    b[236] = 99, b[237] = 130, b[238] = 83, b[239] = 99;
    int o = 240;
    b[o++] = 53, b[o++] = 1, b[o++] = type;
    if (req_ip) {
        uint32_t n = htonl(req_ip);
        b[o++] = 50, b[o++] = 4;
        memcpy(b + o, &n, 4);
        o += 4;
    }
    if (server) {
        uint32_t n = htonl(server);
        b[o++] = 54, b[o++] = 4;
        memcpy(b + o, &n, 4);
        o += 4;
    }
    b[o++] = 55, b[o++] = 3, b[o++] = 1, b[o++] = 3, b[o++] = 6;
    b[o++] = 255;
    return o < 300 ? 300 : o;
}

static bool dhcp_parse(const uint8_t *b, int len, uint32_t xid, uint8_t *type, uint32_t *yi, uint32_t *mask,
                       uint32_t *gw, uint32_t *dns, uint32_t *srv) {
    if (len < 240 || memcmp(b + 4, &xid, 4)) return false;
    uint32_t v;
    memcpy(&v, b + 16, 4);
    *yi = ntohl(v);
    int o = 240;
    *type = 0;
    while (o < len && b[o] != 255) {
        if (b[o] == 0) {
            o++;
            continue;
        }
        uint8_t code = b[o], l = b[o + 1];
        const uint8_t *d = b + o + 2;
        if (l >= 4) memcpy(&v, d, 4);
        if (code == 53) *type = d[0];
        else if (code == 1 && l >= 4) *mask = ntohl(v);
        else if (code == 3 && l >= 4) *gw = ntohl(v);
        else if (code == 6 && l >= 4) *dns = ntohl(v);
        else if (code == 54 && l >= 4) *srv = ntohl(v);
        o += 2 + l;
    }
    return true;
}

static bool dhcp(void) {
    uint8_t *b = kmalloc(1600);
    if (!b) return false;
    uint32_t xid = 0xA715000 ^ ticks ^ ((uint32_t)net_mac[5] << 24);
    uint32_t yi = 0, mask = 0xFFFFFF00, gw = 0, dns = 0, srv = 0;
    uint8_t type;
    bool ok = false;
    net_ip = 0;
    configured = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        int n = dhcp_build(b, 1, xid, 0, 0);
        udp_send(0xFFFFFFFF, 68, 67, b, n);
        int r = udp_wait(68, b, 1500);
        if (r < 0 || !dhcp_parse(b, r, xid, &type, &yi, &mask, &gw, &dns, &srv) || type != 2) continue;
        uint32_t offered = yi;
        n = dhcp_build(b, 3, xid, offered, srv);
        udp_send(0xFFFFFFFF, 68, 67, b, n);
        r = udp_wait(68, b, 1500);
        if (r < 0 || !dhcp_parse(b, r, xid, &type, &yi, &mask, &gw, &dns, &srv) || type != 5) continue;
        net_ip = yi ? yi : offered;
        net_mask = mask;
        net_gw = gw;
        net_dns = dns ? dns : gw;
        ok = true;
    }
    kfree(b);
    configured = ok;
    return ok;
}

/* ---- public API -------------------------------------------------------- */
static uint32_t ip4(int a, int b, int c, int d) { return (uint32_t)a << 24 | b << 16 | c << 8 | d; }

void ip_to_str(uint32_t ip, char *out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}
bool str_to_ip(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit(*s)) return false;
        long n = strtol(s, &s, 10);
        if (n > 255) return false;
        v = v << 8 | (uint32_t)n;
        if (i < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (*s) return false;
    *ip = v;
    return true;
}

bool net_present(void) { return nic_ok; }
bool net_up(void) { return nic_ok && configured; }
const char *net_nic_name(void) { return nic_name; }
void net_stats(uint32_t *rx, uint32_t *tx) {
    *rx = rx_packets;
    *tx = tx_packets;
}

bool net_init(void) {
    nic_ok = e1000_init();
    if (!nic_ok) return false;
    /* wait briefly for link */
    for (int i = 0; i < 50 && !(er(E_STATUS) & 2); i++) sleep_ms(10);
    if (!dhcp()) {
        /* QEMU user-mode networking defaults */
        net_ip = ip4(10, 0, 2, 15);
        net_mask = ip4(255, 255, 255, 0);
        net_gw = ip4(10, 0, 2, 2);
        net_dns = ip4(10, 0, 2, 3);
        configured = true;
        klog("net: no DHCP answer, using static 10.0.2.15");
        return true;
    }
    return true;
}

bool net_renew(void) {
    if (!nic_ok) return false;
    return dhcp();
}

int net_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ms) {
    if (!net_up()) return -1;
    uint8_t pkt[40];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 8;
    pkt[4] = 0xA7;
    pkt[5] = 1;
    pkt[6] = seq >> 8;
    pkt[7] = seq & 0xFF;
    for (int i = 8; i < 40; i++) pkt[i] = (uint8_t)('a' + (i - 8) % 23);
    uint16_t c = csum16(pkt, sizeof pkt, 0);
    memcpy(pkt + 2, &c, 2);
    ping_reply_seq = -1;
    uint32_t start = ticks;
    ip_send(ip, 1, pkt, sizeof pkt);
    while ((ticks - start) * 10 < timeout_ms) {
        net_poll();
        if (ping_reply_seq == seq) return (int)((ticks - start) * 10);
    }
    return -1;
}

bool net_resolve(const char *host, uint32_t *ip) {
    if (str_to_ip(host, ip)) return true;
    if (!strcasecmp(host, "localhost")) {
        *ip = ip4(127, 0, 0, 1);
        return true;
    }
    if (!net_up()) return false;
    uint8_t *b = kmalloc(1600);
    if (!b) return false;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        uint16_t id = (uint16_t)(ticks * 7919 + attempt);
        int o = 0;
        b[o++] = id >> 8, b[o++] = id & 0xFF;
        b[o++] = 0x01, b[o++] = 0x00; /* recursion desired */
        b[o++] = 0, b[o++] = 1, b[o++] = 0, b[o++] = 0, b[o++] = 0, b[o++] = 0, b[o++] = 0, b[o++] = 0;
        const char *p = host;
        while (*p && o < 250) {
            const char *dot = strchr(p, '.');
            int l = dot ? (int)(dot - p) : (int)strlen(p);
            if (l == 0 || l > 63) break;
            b[o++] = (uint8_t)l;
            memcpy(b + o, p, l);
            o += l;
            p += l;
            if (*p == '.') p++;
        }
        b[o++] = 0;
        b[o++] = 0, b[o++] = 1, b[o++] = 0, b[o++] = 1; /* A, IN */
        uint16_t sport = (uint16_t)(40000 + (ticks % 20000));
        udp_send(net_dns, sport, 53, b, o);
        int r = udp_wait(sport, b, 2000);
        if (r < 12 || ((b[0] << 8) | b[1]) != id) continue;
        int qd = (b[4] << 8) | b[5], an = (b[6] << 8) | b[7];
        int q = 12;
        for (int i = 0; i < qd && q < r; i++) {
            while (q < r && b[q]) {
                if ((b[q] & 0xC0) == 0xC0) {
                    q++;
                    break;
                }
                q += b[q] + 1;
            }
            q += 5;
        }
        for (int i = 0; i < an && q + 10 < r; i++) {
            while (q < r && b[q]) {
                if ((b[q] & 0xC0) == 0xC0) {
                    q++;
                    break;
                }
                q += b[q] + 1;
            }
            q++;
            uint16_t type = (b[q] << 8) | b[q + 1];
            uint16_t rdl = (b[q + 8] << 8) | b[q + 9];
            q += 10;
            if (type == 1 && rdl == 4 && q + 4 <= r) {
                *ip = (uint32_t)b[q] << 24 | b[q + 1] << 16 | b[q + 2] << 8 | b[q + 3];
                ok = true;
                break;
            }
            q += rdl;
        }
        if (!ok) break; /* got an answer without A record */
    }
    kfree(b);
    return ok;
}

/* ---- TCP --------------------------------------------------------------- */
static bool user_abort(void) {
    int k = kbd_trygetkey();
    return k == KEY_ESC || k == KEY_CTRL('c');
}

static void tcp_reset_state(void) {
    kfree(tc.rx);
    memset(&tc, 0, sizeof tc);
}

int tcp_connect(uint32_t ip, uint16_t port) {
    tcp_reset_state();
    static uint16_t next_port = 49152;
    next_port = (uint16_t)(next_port + 1 + (ticks & 7));
    if (next_port < 49152) next_port = 49152;
    tc.lport = next_port;
    tc.rport = port;
    tc.rip = ip;
    tc.snd_nxt = 0x1000 + ticks * 64000;
    tc.snd_una = tc.snd_nxt;
    tc.state = T_SYN_SENT;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t iss = tc.snd_nxt;
        tcp_send_seg(TCP_SYN, NULL, 0);
        tc.snd_nxt = iss + 1;
        uint32_t t = ticks;
        while (ticks - t < 150) {
            net_poll();
            if (tc.state == T_EST) return 0;
            if (tc.reset) return -1;
            if (user_abort()) return -2;
        }
        tc.snd_nxt = iss;
    }
    tc.state = T_CLOSED;
    return -1;
}

int tcp_write(const void *data, uint32_t len) {
    const uint8_t *p = data;
    while (len) {
        uint32_t n = len > 1400 ? 1400 : len;
        uint32_t seq = tc.snd_nxt;
        bool acked = false;
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            tc.snd_nxt = seq;
            tcp_send_seg(TCP_ACK | TCP_PSH, p, n);
            tc.snd_nxt = seq + n;
            uint32_t t = ticks;
            while (ticks - t < 100) {
                net_poll();
                if ((int32_t)(tc.snd_una - (seq + n)) >= 0) {
                    acked = true;
                    break;
                }
                if (tc.reset) return -1;
            }
        }
        if (!acked) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

void tcp_close(void) {
    if (tc.state == T_EST || tc.state == T_CLOSE_WAIT) {
        tcp_send_seg(TCP_FIN | TCP_ACK, NULL, 0);
        tc.snd_nxt++;
        uint32_t t = ticks;
        while (ticks - t < 20) net_poll();
    }
    tc.state = T_CLOSED;
}

/* ---- HTTP -------------------------------------------------------------- */
bool url_split(const char *url, char *host, size_t hn, uint16_t *port, char *path, size_t pn) {
    const char *p = url;
    if (!strncasecmp(p, "http://", 7)) p += 7;
    else if (strstr(p, "://")) return false;
    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (!hl || hl >= hn) return false;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = (uint16_t)atoi(colon + 1);
    }
    strlcpy(path, slash ? slash : "/", pn);
    return true;
}

static int find_header(const char *hdr, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    const char *p = hdr;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') v++;
            size_t l = (size_t)(eol - v);
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 1;
        }
        if (!*eol) break;
        p = eol + 2;
    }
    return 0;
}

/* returns HTTP status, or a negative error */
int http_get(const char *url, http_resp_t *r, void (*progress)(uint32_t got, uint32_t total)) {
    memset(r, 0, sizeof *r);
    if (!net_up()) return HTTP_ENONET;
    char host[128], path[512];
    uint16_t port;
    if (!url_split(url, host, sizeof host, &port, path, sizeof path)) return HTTP_EURL;
    uint32_t ip;
    if (!net_resolve(host, &ip)) return HTTP_EDNS;
    int c = tcp_connect(ip, port);
    if (c == -2) return HTTP_EABORT;
    if (c < 0) return HTTP_ECONN;
    char *req = kmalloc(1024);
    if (!req) return HTTP_ECONN;
    snprintf(req, 1024,
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: AxisBrowser/1.0 (AxisOS; text)\r\n"
             "Accept: text/html, text/plain, */*\r\nConnection: close\r\n\r\n",
             path, host);
    int w = tcp_write(req, (uint32_t)strlen(req));
    kfree(req);
    if (w < 0) {
        tcp_close();
        return HTTP_ECONN;
    }
    uint32_t last = ticks, header_end = 0, content_len = 0;
    bool have_len = false;
    for (;;) {
        uint32_t before = tc.rxlen;
        net_poll();
        if (tc.rxlen != before) last = ticks;
        if (!header_end && tc.rxlen >= 4) {
            for (uint32_t i = 0; i + 3 < tc.rxlen; i++)
                if (!memcmp(tc.rx + i, "\r\n\r\n", 4)) {
                    header_end = i + 4;
                    char tmp[32];
                    tc.rx[i] = 0;
                    if (find_header((char *)tc.rx, "Content-Length", tmp, sizeof tmp)) {
                        content_len = (uint32_t)atoi(tmp);
                        have_len = true;
                    }
                    tc.rx[i] = '\r';
                    break;
                }
        }
        if (progress && header_end) progress(tc.rxlen - header_end, content_len);
        if (tc.remote_closed || tc.reset) break;
        if (have_len && header_end && tc.rxlen - header_end >= content_len) break;
        if (ticks - last > 15 * TICK_HZ) break; /* 15 s of silence */
        if (user_abort()) {
            tcp_close();
            return HTTP_EABORT;
        }
    }
    tcp_close();
    if (!header_end) {
        tcp_reset_state();
        return HTTP_EPROTO;
    }
    /* status line */
    tc.rx[header_end - 4] = 0;
    const char *sp = strchr((char *)tc.rx, ' ');
    r->status = sp ? atoi(sp + 1) : 0;
    find_header((char *)tc.rx, "Location", r->location, sizeof r->location);
    find_header((char *)tc.rx, "Content-Type", r->ctype, sizeof r->ctype);
    char te[32] = "";
    find_header((char *)tc.rx, "Transfer-Encoding", te, sizeof te);
    uint32_t blen = tc.rxlen - header_end;
    if (have_len && blen > content_len) blen = content_len;
    r->body = kmalloc(blen + 1);
    if (!r->body) {
        tcp_reset_state();
        return HTTP_ECONN;
    }
    memcpy(r->body, tc.rx + header_end, blen);
    r->body[blen] = 0;
    r->len = blen;
    if (strcasestr(te, "chunked")) {
        /* de-chunk in place */
        char *in = r->body, *out = r->body, *end = r->body + blen;
        while (in < end) {
            const char *e;
            long n = strtol(in, &e, 16);
            in = (char *)e;
            while (in < end && *in != '\n') in++;
            in++;
            if (n <= 0 || in + n > end) break;
            memmove(out, in, n);
            out += n;
            in += n + 2;
        }
        *out = 0;
        r->len = (uint32_t)(out - r->body);
    }
    tcp_reset_state();
    return r->status;
}

const char *http_strerror(int e) {
    switch (e) {
    case HTTP_ENONET: return "network is down (no network card found)";
    case HTTP_EURL: return "unsupported address (only http:// can be fetched directly)";
    case HTTP_EDNS: return "could not find that host name (DNS lookup failed)";
    case HTTP_ECONN: return "could not connect to the server";
    case HTTP_EPROTO: return "the server sent an invalid reply";
    case HTTP_EABORT: return "cancelled";
    }
    return "unknown error";
}
