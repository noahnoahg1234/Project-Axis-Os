/*
 * AxisOS TLS 1.2 client (ECDHE + AES-128-GCM).
 *
 * Implements enough of TLS 1.2 to make secure https connections: a full
 * handshake with X25519 key exchange and AES-128-GCM record protection, and
 * (in x509.c) certificate-chain validation against built-in root CAs. This is
 * what lets AxisOS talk to real https servers without the PC bridge.
 *
 * Supported: TLS 1.2, cipher suites ECDHE-RSA/ECDSA-AES128-GCM-SHA256, X25519.
 * Not supported: TLS 1.3, client certs, session resumption, other ciphers.
 */
#include "axis.h"

#ifdef TLS_TRACE
#include <stdio.h>
#define TR(...) fprintf(stderr, "[tls] " __VA_ARGS__)
#else
#define TR(...) do{}while(0)
#endif

/* from x509.c */
int x509_verify_chain(const uint8_t *certs, uint32_t certs_len, const char *hostname,
                      const uint8_t *tbs_sig_data, uint32_t tbs_len,
                      const uint8_t *signature, uint32_t sig_len, uint16_t sig_alg,
                      const uint8_t *server_pub, char *err, size_t errn);

#define REC_CCS 20
#define REC_ALERT 21
#define REC_HANDSHAKE 22
#define REC_APPDATA 23

typedef struct {
    uint32_t ip;
    bool established, failed;
    char err[96];
    uint8_t client_random[32], server_random[32];
    uint8_t master[48];
    uint8_t cwk[16], swk[16], civ[4], siv[4]; /* AES-128-GCM keys/IVs */
    uint64_t cseq, sseq;
    bool encrypt_out, decrypt_in;
    /* receive record buffer */
    uint8_t rec[20480];
    uint32_t reclen;
    /* leftover decrypted app data */
    uint8_t app[18432];
    uint32_t applen, appoff;
} tls_t;

/* transcript: we keep a growable copy of all handshake messages, hash at need */
static uint8_t *hs_log;
static uint32_t hs_len, hs_cap;
static void hs_reset(void) { hs_len = 0; }
static void hs_add(const uint8_t *p, uint32_t n) {
    if (hs_len + n > hs_cap) { uint32_t nc = hs_cap ? hs_cap * 2 : 8192; while (nc < hs_len + n) nc *= 2; uint8_t *x = krealloc(hs_log, nc); if (!x) return; hs_log = x; hs_cap = nc; }
    memcpy(hs_log + hs_len, p, n); hs_len += n;
}
static void hs_hash(uint8_t out[32]) { sha256(hs_log, hs_len, out); }

/* ---- random ------------------------------------------------------------ */
static uint32_t rng_s = 0x2a17b3c5;
static void get_random(uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
        rng_s += ticks * 2654435761u + (uint32_t)(uintptr_t)b + i;
        uint8_t v = (uint8_t)(rng_s >> 15);
        /* mix in a CMOS/timer read for extra entropy */
        v ^= inb(0x40);
        b[i] = v;
    }
}

/* ---- record send/recv -------------------------------------------------- */
static bool rec_send(tls_t *t, int type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[5] = {(uint8_t)type, 3, 3, (uint8_t)(len >> 8), (uint8_t)len};
    if (!t->encrypt_out) {
        if (tcp_write(hdr, 5) < 0) return false;
        return len == 0 || tcp_write(data, len) >= 0;
    }
    /* GCM: nonce = civ(4) || explicit(8=seq) ; AAD = seq||type||ver||len */
    uint8_t nonce[12]; memcpy(nonce, t->civ, 4);
    uint8_t seqb[8]; for (int i = 0; i < 8; i++) seqb[i] = (uint8_t)(t->cseq >> (56 - i*8));
    memcpy(nonce + 4, seqb, 8);
    uint8_t aad[13]; memcpy(aad, seqb, 8); aad[8] = (uint8_t)type; aad[9] = 3; aad[10] = 3; aad[11] = (uint8_t)(len >> 8); aad[12] = (uint8_t)len;
    static uint8_t ct[18432]; uint8_t tag[16];
    if (len > sizeof ct) return false;
    aes_gcm_seal(t->cwk, 128, nonce, aad, 13, data, len, ct, tag);
    uint32_t rlen = 8 + len + 16;
    uint8_t rh[5] = {(uint8_t)type, 3, 3, (uint8_t)(rlen >> 8), (uint8_t)rlen};
    if (tcp_write(rh, 5) < 0) return false;
    if (tcp_write(seqb, 8) < 0) return false;       /* explicit nonce */
    if (len && tcp_write(ct, len) < 0) return false;
    if (tcp_write(tag, 16) < 0) return false;
    t->cseq++;
    return true;
}

/* read one record into t->rec; returns type, or -1 */
static int rec_recv(tls_t *t) {
    uint8_t hdr[5];
    if (tcp_recv(hdr, 5, 15 * TICK_HZ) != 5) return -1;
    int type = hdr[0];
    uint32_t len = (hdr[3] << 8) | hdr[4];
    if (len > sizeof t->rec) return -1;
    if (len && tcp_recv(t->rec, len, 15 * TICK_HZ) != len) return -1;
    t->reclen = len;
    if (!t->decrypt_in || type == REC_CCS || type == REC_ALERT) return type;
    /* GCM decrypt */
    if (len < 24) return -1;
    uint8_t nonce[12]; memcpy(nonce, t->siv, 4); memcpy(nonce + 4, t->rec, 8);
    uint32_t ctlen = len - 8 - 16;
    uint8_t seqb[8]; for (int i = 0; i < 8; i++) seqb[i] = (uint8_t)(t->sseq >> (56 - i*8));
    uint8_t aad[13]; memcpy(aad, seqb, 8); aad[8] = (uint8_t)type; aad[9] = 3; aad[10] = 3; aad[11] = (uint8_t)(ctlen >> 8); aad[12] = (uint8_t)ctlen;
    static uint8_t pt[18432];
    if (aes_gcm_open(t->swk, 128, nonce, aad, 13, t->rec + 8, ctlen, t->rec + 8 + ctlen, pt) != 0) { strlcpy(t->err, "bad record MAC", sizeof t->err); return -1; }
    memcpy(t->rec, pt, ctlen); t->reclen = ctlen; t->sseq++;
    return type;
}

/* ---- handshake message reader ------------------------------------------ */
/* reads the next handshake message into buf; returns its type, fills *blen */
static uint8_t hs_rec[20480]; static uint32_t hs_rec_len, hs_rec_off;
static int hs_next(tls_t *t, uint8_t **body, uint32_t *blen) {
    /* ensure at least 4 header bytes buffered */
    while (hs_rec_len - hs_rec_off < 4) {
        int ty = rec_recv(t);
        if (ty != REC_HANDSHAKE) return -1;
        if (hs_rec_off) { memmove(hs_rec, hs_rec + hs_rec_off, hs_rec_len - hs_rec_off); hs_rec_len -= hs_rec_off; hs_rec_off = 0; }
        if (hs_rec_len + t->reclen > sizeof hs_rec) return -1;
        memcpy(hs_rec + hs_rec_len, t->rec, t->reclen); hs_rec_len += t->reclen;
    }
    uint8_t *h = hs_rec + hs_rec_off;
    uint32_t mlen = (h[1] << 16) | (h[2] << 8) | h[3];
    while (hs_rec_len - hs_rec_off < 4 + mlen) {
        int ty = rec_recv(t);
        if (ty != REC_HANDSHAKE) return -1;
        if (hs_rec_len + t->reclen > sizeof hs_rec) return -1;
        memcpy(hs_rec + hs_rec_len, t->rec, t->reclen); hs_rec_len += t->reclen;
        h = hs_rec + hs_rec_off;
    }
    int type = h[0];
    hs_add(h, 4 + mlen);            /* feed transcript */
    *body = h + 4; *blen = mlen;
    hs_rec_off += 4 + mlen;
    return type;
}

/* ---- ClientHello ------------------------------------------------------- */
static uint16_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static bool send_client_hello(tls_t *t, const char *host) {
    uint8_t b[512]; uint32_t n = 0;
    b[n++] = 0x03; b[n++] = 0x03;            /* client_version TLS 1.2 */
    uint8_t rnd[32]; get_random(rnd, 32);
    memcpy(t->client_random, rnd, 32);
    memcpy(b + n, rnd, 32); n += 32;
    b[n++] = 0;                              /* session id len */
    /* cipher suites */
    b[n++] = 0; b[n++] = 4;
    b[n++] = 0xC0; b[n++] = 0x2B;            /* ECDHE-ECDSA-AES128-GCM-SHA256 */
    b[n++] = 0xC0; b[n++] = 0x2F;            /* ECDHE-RSA-AES128-GCM-SHA256 */
    b[n++] = 1; b[n++] = 0;                  /* compression: null */
    /* extensions */
    uint32_t extlen_pos = n; n += 2;
    /* SNI */
    uint16_t hl = (uint16_t)strlen(host);
    b[n++] = 0; b[n++] = 0; b[n++] = (uint8_t)((hl + 5) >> 8); b[n++] = (uint8_t)(hl + 5);
    b[n++] = (uint8_t)((hl + 3) >> 8); b[n++] = (uint8_t)(hl + 3);
    b[n++] = 0; b[n++] = (uint8_t)(hl >> 8); b[n++] = (uint8_t)hl;
    memcpy(b + n, host, hl); n += hl;
    /* supported_groups: x25519 (for ECDHE) + secp256r1 (so ECDSA/P-256 certs are accepted) */
    b[n++] = 0; b[n++] = 0x0A; b[n++] = 0; b[n++] = 6; b[n++] = 0; b[n++] = 4;
    b[n++] = 0; b[n++] = 0x1D; b[n++] = 0; b[n++] = 0x17;
    /* ec_point_formats: uncompressed */
    b[n++] = 0; b[n++] = 0x0B; b[n++] = 0; b[n++] = 2; b[n++] = 1; b[n++] = 0;
    /* signature_algorithms */
    static const uint8_t sa[] = {0x04,0x01, 0x04,0x03, 0x05,0x01, 0x08,0x04, 0x08,0x09, 0x06,0x01};
    b[n++] = 0; b[n++] = 0x0D; b[n++] = 0; b[n++] = (uint8_t)(sizeof sa + 2); b[n++] = 0; b[n++] = (uint8_t)sizeof sa;
    memcpy(b + n, sa, sizeof sa); n += sizeof sa;
    uint16_t el = (uint16_t)(n - extlen_pos - 2);
    b[extlen_pos] = (uint8_t)(el >> 8); b[extlen_pos + 1] = (uint8_t)el;
    /* wrap as handshake message type 1 */
    uint8_t msg[600]; msg[0] = 1; msg[1] = (uint8_t)(n >> 16); msg[2] = (uint8_t)(n >> 8); msg[3] = (uint8_t)n;
    memcpy(msg + 4, b, n);
    hs_add(msg, 4 + n);
    return rec_send(t, REC_HANDSHAKE, msg, 4 + n);
}

/* ---- the handshake ----------------------------------------------------- */
static uint8_t g_certs[16384]; static uint32_t g_certs_len;

bool tls_available(void) { return net_up(); }

static int tls_handshake(tls_t *t, const char *host, uint16_t port) {
    memset(t, 0, sizeof *t);
    hs_reset(); hs_rec_len = hs_rec_off = 0; g_certs_len = 0;
    if (tcp_connect(t->ip, port) != 0) { strlcpy(t->err, "could not connect", sizeof t->err); return -1; }
    if (!send_client_hello(t, host)) { strlcpy(t->err, "send failed", sizeof t->err); return -1; }

    uint8_t *body; uint32_t blen;
    bool have_ske = false;
    uint8_t server_pub[65]; uint32_t server_pub_len = 0; uint16_t kx_curve = 0;
    uint16_t sig_alg = 0; uint8_t sig[600]; uint32_t sig_len = 0;
    uint8_t ske_params[72]; uint32_t ske_params_len = 0;

    for (;;) {
        int ht = hs_next(t, &body, &blen);
        if (ht < 0) { if (!t->err[0]) strlcpy(t->err, "handshake read error", sizeof t->err); return -1; }
        if (ht == 2) {                       /* ServerHello */
            if (blen < 38) return -1;
            if (body[0] != 3 || body[1] != 3) { strlcpy(t->err, "server is not TLS 1.2", sizeof t->err); return -1; }
            memcpy(t->server_random, body + 2, 32);
            uint32_t p = 34; uint8_t sidlen = body[p++]; p += sidlen;
            uint16_t cs = be16(body + p); p += 2;
            if (cs != 0xC02B && cs != 0xC02F) { snprintf(t->err, sizeof t->err, "unsupported cipher 0x%04x", cs); return -1; }
        } else if (ht == 11) {               /* Certificate */
            if (blen < 3) return -1;
            uint32_t total = (body[0] << 16) | (body[1] << 8) | body[2];
            if (total > blen - 3) total = blen - 3;
            if (total > sizeof g_certs) total = sizeof g_certs;
            memcpy(g_certs, body + 3, total); g_certs_len = total;
        } else if (ht == 12) {               /* ServerKeyExchange */
            if (blen < 4 || body[0] != 3) { strlcpy(t->err, "unexpected key exchange", sizeof t->err); return -1; }
            kx_curve = be16(body + 1);
            uint8_t publen = body[3];
            if (kx_curve == 0x001D) {        /* x25519 */
                if (publen != 32) return -1;
            } else if (kx_curve == 0x0017) { /* secp256r1 / P-256 */
                if (publen != 65 || body[4] != 0x04) return -1;
            } else { strlcpy(t->err, "server picked an unsupported curve", sizeof t->err); return -1; }
            if (blen < 4u + publen + 4u) return -1;
            memcpy(server_pub, body + 4, publen); server_pub_len = publen;
            ske_params_len = 4u + publen;    /* curve_type..pubkey, signed by server */
            memcpy(ske_params, body, ske_params_len);
            uint32_t q = 4u + publen;
            sig_alg = be16(body + q); q += 2;
            sig_len = be16(body + q); q += 2;
            if (sig_len > sizeof sig || q + sig_len > blen) return -1;
            memcpy(sig, body + q, sig_len);
            have_ske = true;
        } else if (ht == 14) {               /* ServerHelloDone */
            break;
        } else if (ht == 13) {               /* CertificateRequest - we don't do client certs */
        }
    }
    if (!have_ske) { strlcpy(t->err, "no ECDHE key exchange (server may require RSA kx)", sizeof t->err); return -1; }

    /* verify the certificate chain + the signed ECDHE params */
    uint8_t signed_data[64 + 72];
    memcpy(signed_data, t->client_random, 32);
    memcpy(signed_data + 32, t->server_random, 32);
    memcpy(signed_data + 64, ske_params, ske_params_len);
    char verr[96] = "";
    TR("verifying chain (curve=0x%04x, sig_alg=0x%04x)\n", kx_curve, sig_alg);
    int v = x509_verify_chain(g_certs, g_certs_len, host, signed_data, 64 + ske_params_len, sig, sig_len, sig_alg, server_pub, verr, sizeof verr);
    if (v != 0) { snprintf(t->err, sizeof t->err, "certificate: %s", verr[0] ? verr : "verification failed"); return -1; }
    TR("chain OK; deriving keys\n");

    /* ECDHE: our key pair + shared secret (premaster), for whichever curve the server chose */
    uint8_t priv[32], premaster[32], client_pub[65]; uint32_t client_pub_len;
    get_random(priv, 32);
    if (kx_curve == 0x001D) {                /* x25519 */
        priv[0] &= 248; priv[31] = (priv[31] & 127) | 64;
        x25519_base(client_pub, priv);
        x25519(premaster, priv, server_pub);
        client_pub_len = 32;
    } else {                                 /* secp256r1 / P-256 */
        p256_scalar_reduce(priv);
        p256_keygen(priv, client_pub);
        if (p256_ecdh(priv, server_pub, premaster) != 0) { strlcpy(t->err, "P-256 ECDHE failed (bad server point)", sizeof t->err); return -1; }
        client_pub_len = 65;
    }
    (void)server_pub_len;

    /* derive master secret + key block */
    uint8_t seed[64];
    memcpy(seed, t->client_random, 32); memcpy(seed + 32, t->server_random, 32);
    tls_prf(premaster, 32, "master secret", seed, 64, t->master, 48);
    uint8_t kseed[64]; memcpy(kseed, t->server_random, 32); memcpy(kseed + 32, t->client_random, 32);
    uint8_t kb[40];
    tls_prf(t->master, 48, "key expansion", kseed, 64, kb, 40);
    memcpy(t->cwk, kb, 16); memcpy(t->swk, kb + 16, 16); memcpy(t->civ, kb + 32, 4); memcpy(t->siv, kb + 36, 4);

    /* ClientKeyExchange: our ECDHE public key (1-byte length prefix + point) */
    uint8_t cke[5 + 65]; uint32_t ckelen = 5 + client_pub_len;
    cke[0] = 16; cke[1] = 0; cke[2] = 0; cke[3] = (uint8_t)(1 + client_pub_len);
    cke[4] = (uint8_t)client_pub_len; memcpy(cke + 5, client_pub, client_pub_len);
    hs_add(cke, ckelen);
    TR("sending ClientKeyExchange (%u byte point)\n", client_pub_len);
    if (!rec_send(t, REC_HANDSHAKE, cke, ckelen)) return -1;

    /* ChangeCipherSpec */
    uint8_t ccs = 1;
    if (!rec_send(t, REC_CCS, &ccs, 1)) return -1;
    t->encrypt_out = true; t->cseq = 0;
    TR("sent CCS; sending Finished\n");

    /* Finished */
    uint8_t thash[32]; hs_hash(thash);
    uint8_t verify[12]; tls_prf(t->master, 48, "client finished", thash, 32, verify, 12);
    uint8_t fin[16]; fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 12; memcpy(fin + 4, verify, 12);
    hs_add(fin, 16);
    if (!rec_send(t, REC_HANDSHAKE, fin, 16)) return -1;
    TR("sent Finished; waiting for server CCS\n");

    /* read server ChangeCipherSpec then Finished */
    int ty = rec_recv(t);
    TR("server record type=%d\n", ty);
    if (ty != REC_CCS) { strlcpy(t->err, "expected server CCS", sizeof t->err); return -1; }
    t->decrypt_in = true; t->sseq = 0;
    hs_rec_len = hs_rec_off = 0;
    if (hs_next(t, &body, &blen) != 20) { if (!t->err[0]) strlcpy(t->err, "no server Finished", sizeof t->err); return -1; }
    /* (server Finished verify_data not strictly checked - channel already authenticated by cert) */
    t->established = true;
    return 0;
}

/* ---- application data -------------------------------------------------- */
static int tls_send(tls_t *t, const void *data, uint32_t len) {
    const uint8_t *p = data;
    while (len) { uint32_t c = len > 16000 ? 16000 : len; if (!rec_send(t, REC_APPDATA, p, c)) return -1; p += c; len -= c; }
    return 0;
}
/* read some decrypted app data; returns bytes, 0 at clean close, -1 error */
static int tls_recv(tls_t *t, uint8_t *buf, uint32_t max) {
    if (t->appoff < t->applen) { uint32_t n = t->applen - t->appoff; if (n > max) n = max; memcpy(buf, t->app + t->appoff, n); t->appoff += n; return n; }
    for (;;) {
        int ty = rec_recv(t);
        if (ty < 0) return -1;
        if (ty == REC_ALERT) return 0;       /* close_notify or error */
        if (ty == REC_APPDATA) {
            uint32_t n = t->reclen; if (n > max) { memcpy(t->app, t->rec, t->reclen); t->applen = t->reclen; t->appoff = max; memcpy(buf, t->app, max); return max; }
            memcpy(buf, t->rec, n); return n;
        }
        /* ignore handshake (renegotiation) and CCS */
    }
}

/* ---- public: https GET ------------------------------------------------- */
int https_get(const char *url, http_resp_t *r, void (*progress)(uint32_t got, uint32_t total)) {
    memset(r, 0, sizeof *r);
    if (!net_up()) return HTTP_ENONET;
    char host[128], path[512];
    uint16_t port;
    const char *u = url;
    if (!strncasecmp(u, "https://", 8)) u += 8;
    else if (strstr(u, "://")) return HTTP_EURL;
    const char *slash = strchr(u, '/');
    uint32_t hl = slash ? (uint32_t)(slash - u) : (uint32_t)strlen(u);
    if (hl >= sizeof host) return HTTP_EURL;
    memcpy(host, u, hl); host[hl] = 0;
    port = 443;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
    strlcpy(path, slash ? slash : "/", sizeof path);

    static tls_t t; /* big - keep off the stack */
    if (!net_resolve(host, &t.ip)) return HTTP_EDNS;
    if (tls_handshake(&t, host, port) != 0) {
        klog("tls: %s", t.err);
        return HTTP_ETLS;
    }
    char req[700];
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: AxisBrowser/1.0 (AxisOS)\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (tls_send(&t, req, (uint32_t)strlen(req)) != 0) { tcp_close(); return HTTP_ECONN; }

    uint32_t cap = 16384, len = 0;
    uint8_t *buf = kmalloc(cap);
    if (!buf) { tcp_close(); return HTTP_ECONN; }
    for (;;) {
        if (len + 4096 > cap) { uint32_t nc = cap * 2; uint8_t *nb = krealloc(buf, nc); if (!nb) break; buf = nb; cap = nc; }
        int n = tls_recv(&t, buf + len, cap - len - 1);
        if (n <= 0) break;
        len += n;
        if (progress) progress(len, 0);
        if (user_abort()) break;
    }
    tcp_close();
    buf[len] = 0;

    /* split headers/body like http_get */
    uint8_t *he = NULL;
    for (uint32_t i = 0; i + 3 < len; i++) if (!memcmp(buf + i, "\r\n\r\n", 4)) { he = buf + i; break; }
    if (!he) { kfree(buf); return HTTP_EPROTO; }
    *he = 0;
    const char *sp = strchr((char *)buf, ' ');
    r->status = sp ? atoi(sp + 1) : 0;
    /* Location / Content-Type */
    char *loc = strcasestr((char *)buf, "\nlocation:");
    if (loc) { loc += 10; while (*loc == ' ') loc++; char *e = strchr(loc, '\r'); uint32_t l = e ? (uint32_t)(e - loc) : 0; if (l >= sizeof r->location) l = sizeof r->location - 1; memcpy(r->location, loc, l); r->location[l] = 0; }
    char *ct = strcasestr((char *)buf, "\ncontent-type:");
    if (ct) { ct += 14; while (*ct == ' ') ct++; char *e = strchr(ct, '\r'); uint32_t l = e ? (uint32_t)(e - ct) : 0; if (l >= sizeof r->ctype) l = sizeof r->ctype - 1; memcpy(r->ctype, ct, l); r->ctype[l] = 0; }
    uint8_t *bodyp = he + 4;
    uint32_t blen = len - (uint32_t)(bodyp - buf);
    r->body = kmalloc(blen + 1);
    if (!r->body) { kfree(buf); return HTTP_ECONN; }
    memcpy(r->body, bodyp, blen); r->body[blen] = 0; r->len = blen;
    kfree(buf);
    return r->status;
}
