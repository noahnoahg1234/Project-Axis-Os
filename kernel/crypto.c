/*
 * AxisOS cryptography primitives for TLS: SHA-256, HMAC-SHA256, the TLS 1.2
 * PRF, AES-128/256, AES-GCM, and X25519 (Curve25519) key exchange.
 *
 * These are compact, well-known constructions (X25519 is the public-domain
 * TweetNaCl implementation). They are verified against standard test vectors
 * in tools/crypto_test (run on the host).
 */
#include "axis.h"

/* ============================= SHA-256 ================================== */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha256_t;
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void sha256_init(sha256_t *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, 32); s->len = 0; s->n = 0;
}
static void sha256_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i*4]<<24 | p[i*4+1]<<16 | p[i*4+2]<<8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6)^ror(e,11)^ror(e,25);
        uint32_t ch = (e&f)^(~e&g);
        uint32_t t1 = h+S1+ch+K256[i]+w[i];
        uint32_t S0 = ror(a,2)^ror(a,13)^ror(a,22);
        uint32_t mj = (a&b)^(a&c)^(b&c);
        uint32_t t2 = S0+mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha256_update(sha256_t *s, const void *data, uint32_t len) {
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        uint32_t take = 64 - s->n; if (take > len) take = len;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; len -= take;
        if (s->n == 64) { sha256_block(s, s->buf); s->n = 0; }
    }
}
static void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha256_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(s, lb, 8);
    for (int i = 0; i < 8; i++) { out[i*4]=s->h[i]>>24; out[i*4+1]=s->h[i]>>16; out[i*4+2]=s->h[i]>>8; out[i*4+3]=s->h[i]; }
}
void sha256(const void *data, uint32_t len, uint8_t out[32]) {
    sha256_t s; sha256_init(&s); sha256_update(&s, data, len); sha256_final(&s, out);
}

/* ============================= HMAC / PRF =============================== */
void hmac_sha256(const uint8_t *key, uint32_t klen, const uint8_t *msg, uint32_t mlen, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], kh[32];
    if (klen > 64) { sha256(key, klen, kh); key = kh; klen = 32; }
    memset(k, 0, 64); memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i]^0x36; opad[i] = k[i]^0x5c; }
    sha256_t s; sha256_init(&s); sha256_update(&s, ipad, 64); sha256_update(&s, msg, mlen);
    uint8_t inner[32]; sha256_final(&s, inner);
    sha256_init(&s); sha256_update(&s, opad, 64); sha256_update(&s, inner, 32); sha256_final(&s, out);
}
/* TLS 1.2 PRF with SHA-256: P_hash(secret, label+seed) */
void tls_prf(const uint8_t *secret, uint32_t slen, const char *label,
             const uint8_t *seed, uint32_t seedlen, uint8_t *out, uint32_t outlen) {
    uint8_t lseed[256]; uint32_t ll = strlen(label);
    memcpy(lseed, label, ll); memcpy(lseed + ll, seed, seedlen); uint32_t ls = ll + seedlen;
    uint8_t a[32]; hmac_sha256(secret, slen, lseed, ls, a);
    uint32_t done = 0;
    while (done < outlen) {
        uint8_t in[32 + 256]; memcpy(in, a, 32); memcpy(in + 32, lseed, ls);
        uint8_t block[32]; hmac_sha256(secret, slen, in, 32 + ls, block);
        uint32_t take = outlen - done; if (take > 32) take = 32;
        memcpy(out + done, block, take); done += take;
        hmac_sha256(secret, slen, a, 32, a);
    }
}

/* ============================== AES ==================================== */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
typedef struct { uint8_t rk[240]; int nr; } aes_t;
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
void aes_init(aes_t *a, const uint8_t *key, int bits) {
    int nk = bits / 32; a->nr = nk + 6;
    uint8_t *rk = a->rk;
    memcpy(rk, key, nk * 4);
    static const uint8_t rcon[11] = {0,1,2,4,8,16,32,64,128,0x1b,0x36};
    int total = 4 * (a->nr + 1);
    for (int i = nk; i < total; i++) {
        uint8_t t[4]; memcpy(t, rk + (i-1)*4, 4);
        if (i % nk == 0) {
            uint8_t tmp = t[0]; t[0]=sbox[t[1]]^rcon[i/nk]; t[1]=sbox[t[2]]; t[2]=sbox[t[3]]; t[3]=sbox[tmp];
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) rk[i*4+j] = rk[(i-nk)*4+j] ^ t[j];
    }
}
void aes_encrypt(const aes_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16]; memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= a->rk[i];
    for (int round = 1; round <= a->nr; round++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        /* ShiftRows */
        uint8_t sh[16] = {t[0],t[5],t[10],t[15], t[4],t[9],t[14],t[3], t[8],t[13],t[2],t[7], t[12],t[1],t[6],t[11]};
        if (round < a->nr) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = sh + c*4;
                uint8_t a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                col[0]=xtime(a0)^(xtime(a1)^a1)^a2^a3;
                col[1]=a0^xtime(a1)^(xtime(a2)^a2)^a3;
                col[2]=a0^a1^xtime(a2)^(xtime(a3)^a3);
                col[3]=(xtime(a0)^a0)^a1^a2^xtime(a3);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = sh[i] ^ a->rk[round*16 + i];
    }
    memcpy(out, s, 16);
}

/* ============================= AES-GCM ================================= */
static void ghash_mul(uint8_t *x, const uint8_t *h) {
    uint8_t z[16] = {0}, v[16]; memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i>>3] & (0x80 >> (i&7))) for (int j = 0; j < 16; j++) z[j] ^= v[j];
        uint8_t lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (v[j] >> 1) | (v[j-1] << 7);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}
typedef struct { aes_t aes; uint8_t H[16], J0[16]; uint8_t acc[16]; uint64_t aad_len, ct_len; } gcm_t;
static void gcm_start(gcm_t *g, const uint8_t *key, int bits, const uint8_t iv[12]) {
    aes_init(&g->aes, key, bits);
    uint8_t zero[16] = {0}; aes_encrypt(&g->aes, zero, g->H);
    memcpy(g->J0, iv, 12); g->J0[12]=0; g->J0[13]=0; g->J0[14]=0; g->J0[15]=1;
    memset(g->acc, 0, 16); g->aad_len = 0; g->ct_len = 0;
}
static void gcm_aad(gcm_t *g, const uint8_t *aad, uint32_t len) {
    g->aad_len += len;
    while (len) { uint8_t b[16] = {0}; uint32_t t = len > 16 ? 16 : len; memcpy(b, aad, t);
        for (int i = 0; i < 16; i++) g->acc[i] ^= b[i]; ghash_mul(g->acc, g->H); aad += t; len -= t; }
}
static void incr32(uint8_t *ctr) { for (int i = 15; i >= 12; i--) if (++ctr[i]) break; }
/* encrypt (enc=1) or decrypt (enc=0) in place-ish: out may equal in */
static void gcm_crypt(gcm_t *g, const uint8_t *in, uint8_t *out, uint32_t len, int enc) {
    uint8_t ctr[16]; memcpy(ctr, g->J0, 16);
    g->ct_len += len;
    while (len) {
        incr32(ctr);
        uint8_t ks[16]; aes_encrypt(&g->aes, ctr, ks);
        uint32_t t = len > 16 ? 16 : len;
        uint8_t cblock[16] = {0};
        if (enc) { for (uint32_t i = 0; i < t; i++) out[i] = in[i] ^ ks[i]; memcpy(cblock, out, t); }
        else { memcpy(cblock, in, t); for (uint32_t i = 0; i < t; i++) out[i] = in[i] ^ ks[i]; }
        for (int i = 0; i < 16; i++) g->acc[i] ^= cblock[i]; ghash_mul(g->acc, g->H);
        in += t; out += t; len -= t;
    }
}
static void gcm_tag(gcm_t *g, uint8_t tag[16]) {
    uint8_t lenblk[16];
    uint64_t ab = g->aad_len * 8, cb = g->ct_len * 8;
    for (int i = 0; i < 8; i++) lenblk[i] = (uint8_t)(ab >> (56 - i*8));
    for (int i = 0; i < 8; i++) lenblk[8+i] = (uint8_t)(cb >> (56 - i*8));
    for (int i = 0; i < 16; i++) g->acc[i] ^= lenblk[i]; ghash_mul(g->acc, g->H);
    uint8_t ej0[16]; aes_encrypt(&g->aes, g->J0, ej0);
    for (int i = 0; i < 16; i++) tag[i] = g->acc[i] ^ ej0[i];
}
/* one-shot AEAD seal/open. key bits 128/256. iv is 12 bytes. tag 16 bytes. */
void aes_gcm_seal(const uint8_t *key, int bits, const uint8_t iv[12],
                  const uint8_t *aad, uint32_t aadlen, const uint8_t *pt, uint32_t ptlen,
                  uint8_t *ct, uint8_t tag[16]) {
    gcm_t g; gcm_start(&g, key, bits, iv); gcm_aad(&g, aad, aadlen); gcm_crypt(&g, pt, ct, ptlen, 1); gcm_tag(&g, tag);
}
int aes_gcm_open(const uint8_t *key, int bits, const uint8_t iv[12],
                 const uint8_t *aad, uint32_t aadlen, const uint8_t *ct, uint32_t ctlen,
                 const uint8_t tag[16], uint8_t *pt) {
    gcm_t g; gcm_start(&g, key, bits, iv); gcm_aad(&g, aad, aadlen);
    uint8_t want[16]; gcm_t gc = g; gcm_crypt(&gc, ct, pt, ctlen, 0); gcm_tag(&gc, want);
    int diff = 0; for (int i = 0; i < 16; i++) diff |= want[i] ^ tag[i];
    return diff == 0 ? 0 : -1;
}

/* ============================= X25519 ================================== */
/* public-domain TweetNaCl crypto_scalarmult (Curve25519) */
typedef int64_t gf[16];
static const gf _121665 = {0xDB41, 1};
static void car25519(gf o) {
    for (int i = 0; i < 16; i++) { o[i] += (1LL<<16); int64_t c = o[i] >> 16; o[(i+1)*(i<15)] += c - 1 + 37*(c-1)*(i==15); o[i] -= c << 16; }
}
static void sel25519(gf p, gf q, int b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t *o, const gf n) {
    gf m, t; for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i-1]>>16)&1); m[i-1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14]>>16)&1);
        int b = (m[15]>>16)&1; m[14] &= 0xffff; sel25519(t, m, 1-b);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = t[i] & 0xff; o[2*i+1] = t[i] >> 8; }
}
static void unpack25519(gf o, const uint8_t *n) { for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8); o[15] &= 0x7fff; }
static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void M(gf o, const gf a, const gf b) {
    int64_t t[31]; for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i+j] += a[i]*b[j];
    for (int i = 0; i < 15; i++) t[i] += 38*t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void S(gf o, const gf a) { M(o, a, a); }
static void inv25519(gf o, const gf i) {
    gf c; for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
void x25519(uint8_t *out, const uint8_t *scalar, const uint8_t *point) {
    uint8_t z[32]; gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i]; z[31] = (scalar[31] & 127) | 64; z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {
        int bit = (z[i>>3] >> (i&7)) & 1;
        sel25519(a, b, bit); sel25519(c, d, bit);
        A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d); S(d, e); S(f, a);
        M(a, c, a); M(c, b, e); A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
        M(a, c, _121665); A(a, a, d); M(c, c, a); M(a, d, f); M(d, b, x); S(b, e);
        sel25519(a, b, bit); sel25519(c, d, bit);
    }
    inv25519(c, c); M(a, a, c); pack25519(out, a);
}
/* X25519 base point multiply (generate public key from private) */
void x25519_base(uint8_t *pub, const uint8_t *priv) {
    static const uint8_t base[32] = {9};
    x25519(pub, priv, base);
}
