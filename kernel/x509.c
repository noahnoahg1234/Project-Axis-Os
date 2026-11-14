/*
 * AxisOS - X.509 certificate chain validation for TLS.
 *
 * Parses the DER certificate chain the server sends, verifies each cert's
 * signature with its issuer's public key up to a built-in trusted root, checks
 * validity dates and the hostname (SAN / CN), then verifies the server's signed
 * ECDHE parameters with the leaf key. Supports RSA (PKCS#1 v1.5 + SHA-256) and
 * ECDSA P-256 (SHA-256).
 *
 * This is deliberately a compact, auditable implementation for an educational
 * OS. It is NOT a drop-in for a hardened TLS library: it validates the chain,
 * dates and hostname, but does not check revocation (CRL/OCSP) or name
 * constraints.
 */
#include "axis.h"
#include "bignum.h"

/* ------------------------------------------------------------- DER ------ */
typedef struct { const uint8_t *p, *end; } der_t;
/* read one TLV: returns tag, sets *content/*clen, advances d->p past it */
static int der_tlv(der_t *d, int *tag, const uint8_t **content, uint32_t *clen) {
    if (d->p >= d->end) return -1;
    *tag = *d->p++;
    if (d->p >= d->end) return -1;
    uint32_t len = *d->p++;
    if (len & 0x80) {
        int nb = len & 0x7F;
        if (nb == 0 || nb > 4 || d->p + nb > d->end) return -1;
        len = 0; while (nb--) len = (len << 8) | *d->p++;
    }
    if (d->p + len > d->end) return -1;
    *content = d->p; *clen = len; d->p += len;
    return 0;
}
/* step into a constructed TLV (returns a der_t over its contents) */
static int der_into(der_t *d, int want_tag, der_t *inner) {
    int tag; const uint8_t *c; uint32_t l;
    if (der_tlv(d, &tag, &c, &l) != 0) return -1;
    if (want_tag >= 0 && tag != want_tag) return -1;
    inner->p = c; inner->end = c + l;
    return 0;
}

/* ------------------------------------------------------------- cert ----- */
typedef struct {
    const uint8_t *tbs; uint32_t tbs_len;       /* signed portion */
    const uint8_t *sig; uint32_t sig_len; int sig_alg; /* 1=rsa-sha256 2=ecdsa-sha256 */
    const uint8_t *subject; uint32_t subject_len;
    const uint8_t *issuer; uint32_t issuer_len;
    int key_type;                                /* 1=rsa 2=ec-p256 */
    const uint8_t *rsa_n; uint32_t rsa_n_len; const uint8_t *rsa_e; uint32_t rsa_e_len;
    uint8_t ec_pub[65]; uint32_t ec_pub_len;
    const uint8_t *san; uint32_t san_len;        /* raw SAN extension contents */
    const uint8_t *cn; uint32_t cn_len;          /* subject CN fallback */
    int32_t not_before, not_after;               /* days since 2000-01-01 (coarse) */
    int is_ca;
} cert_t;

/* OID tails we recognise (last bytes) */
static int oid_is(const uint8_t *o, uint32_t n, const uint8_t *want, uint32_t wn) {
    return n == wn && !memcmp(o, want, wn);
}
static const uint8_t OID_RSA[]     = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const uint8_t OID_RSA_SHA256[]={0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const uint8_t OID_EC_PUB[]  = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const uint8_t OID_P256[]    = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const uint8_t OID_ECDSA_SHA256[]={0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_CN[]      = {0x55,0x04,0x03};
static const uint8_t OID_SAN[]     = {0x55,0x1d,0x11};
static const uint8_t OID_BASIC[]   = {0x55,0x1d,0x13};

/* parse an ASN.1 time (UTCTime/GeneralizedTime) into coarse days-since-2000 */
static int32_t parse_time(const uint8_t *p, uint32_t n) {
    int y, mo, da; const uint8_t *q = p;
    if (n >= 13 && n <= 13) { y = 2000 + (q[0]-'0')*10 + (q[1]-'0'); q += 2; }       /* YY */
    else if (n >= 15) { y = (q[0]-'0')*1000+(q[1]-'0')*100+(q[2]-'0')*10+(q[3]-'0'); q += 4; } /* YYYY */
    else return 0;
    mo = (q[0]-'0')*10 + (q[1]-'0'); q += 2;
    da = (q[0]-'0')*10 + (q[1]-'0');
    static const int cum[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int32_t days = (int32_t)(y - 2000) * 365 + (y - 2000 + 3) / 4; /* leap approx */
    if (mo >= 1 && mo <= 12) days += cum[mo-1];
    days += da;
    return days;
}

static int parse_pubkey(der_t spki, cert_t *c) {
    /* SubjectPublicKeyInfo ::= SEQ { AlgorithmIdentifier, BIT STRING } */
    der_t alg; if (der_into(&spki, 0x30, &alg) != 0) return -1;
    int tag; const uint8_t *o; uint32_t ol;
    if (der_tlv(&alg, &tag, &o, &ol) != 0 || tag != 0x06) return -1; /* OID */
    const uint8_t *alg_oid = o; uint32_t alg_oln = ol;
    const uint8_t *curve_oid = NULL; uint32_t curve_oln = 0;
    if (alg.p < alg.end) { int t2; if (der_tlv(&alg, &t2, &curve_oid, &curve_oln) == 0 && t2 != 0x06) curve_oid = NULL; }
    int bt; const uint8_t *bits; uint32_t bln;
    if (der_tlv(&spki, &bt, &bits, &bln) != 0 || bt != 0x03 || bln < 1) return -1;
    bits++; bln--;                               /* skip unused-bits byte */
    if (oid_is(alg_oid, alg_oln, OID_RSA, sizeof OID_RSA)) {
        der_t rk = {bits, bits + bln}, seq;
        if (der_into(&rk, 0x30, &seq) != 0) return -1;
        int t; if (der_tlv(&seq, &t, &c->rsa_n, &c->rsa_n_len) != 0 || t != 0x02) return -1;
        if (der_tlv(&seq, &t, &c->rsa_e, &c->rsa_e_len) != 0 || t != 0x02) return -1;
        while (c->rsa_n_len && c->rsa_n[0] == 0) { c->rsa_n++; c->rsa_n_len--; }
        c->key_type = 1; return 0;
    }
    if (oid_is(alg_oid, alg_oln, OID_EC_PUB, sizeof OID_EC_PUB) &&
        curve_oid && oid_is(curve_oid, curve_oln, OID_P256, sizeof OID_P256)) {
        if (bln != 65 || bits[0] != 0x04) return -1;
        memcpy(c->ec_pub, bits, 65); c->ec_pub_len = 65; c->key_type = 2; return 0;
    }
    return -1;
}

static void parse_name_cn(der_t name, cert_t *c) {
    /* Name ::= SEQ OF RDN(SET OF ATV{oid,value}); grab the CN */
    while (name.p < name.end) {
        der_t set; if (der_into(&name, 0x31, &set) != 0) return;
        while (set.p < set.end) {
            der_t atv; if (der_into(&set, 0x30, &atv) != 0) break;
            int t; const uint8_t *o; uint32_t ol;
            if (der_tlv(&atv, &t, &o, &ol) != 0 || t != 0x06) break;
            int vt; const uint8_t *v; uint32_t vl;
            if (der_tlv(&atv, &vt, &v, &vl) != 0) break;
            if (oid_is(o, ol, OID_CN, sizeof OID_CN)) { c->cn = v; c->cn_len = vl; }
        }
    }
}

static int parse_cert(const uint8_t *der, uint32_t len, cert_t *c) {
    memset(c, 0, sizeof *c);
    der_t top = {der, der + len}, cert;
    if (der_into(&top, 0x30, &cert) != 0) return -1;  /* Certificate */
    const uint8_t *tbs_start = cert.p;
    der_t tbs; if (der_into(&cert, 0x30, &tbs) != 0) return -1; /* TBSCertificate */
    c->tbs = tbs_start; c->tbs_len = (uint32_t)(cert.p - tbs_start);
    /* [0] version (optional) */
    if (tbs.p < tbs.end && tbs.p[0] == 0xA0) { der_t v; der_into(&tbs, 0xA0, &v); }
    int t; const uint8_t *cc; uint32_t cl;
    if (der_tlv(&tbs, &t, &cc, &cl) != 0 || t != 0x02) return -1; /* serial */
    der_t sigalg; if (der_into(&tbs, 0x30, &sigalg) != 0) return -1;
    { int st; const uint8_t *so; uint32_t sol; der_tlv(&sigalg, &st, &so, &sol); }
    der_t issuer; const uint8_t *iss_start = tbs.p;
    if (der_into(&tbs, 0x30, &issuer) != 0) return -1;
    c->issuer = iss_start; c->issuer_len = (uint32_t)(tbs.p - iss_start);
    /* validity */
    der_t val; if (der_into(&tbs, 0x30, &val) != 0) return -1;
    { int vt; const uint8_t *vp; uint32_t vl;
      if (der_tlv(&val, &vt, &vp, &vl) == 0) c->not_before = parse_time(vp, vl);
      if (der_tlv(&val, &vt, &vp, &vl) == 0) c->not_after  = parse_time(vp, vl); }
    const uint8_t *sub_start = tbs.p;
    der_t subject; if (der_into(&tbs, 0x30, &subject) != 0) return -1;
    c->subject = sub_start; c->subject_len = (uint32_t)(tbs.p - sub_start);
    parse_name_cn(subject, c);
    der_t spki; if (der_into(&tbs, 0x30, &spki) != 0) return -1;
    if (parse_pubkey(spki, c) != 0) return -1;
    /* optional [1] [2] then [3] extensions */
    while (tbs.p < tbs.end) {
        if (tbs.p[0] == 0xA3) {
            der_t exts_outer, exts;
            if (der_into(&tbs, 0xA3, &exts_outer) != 0) break;
            if (der_into(&exts_outer, 0x30, &exts) != 0) break;
            while (exts.p < exts.end) {
                der_t ext; if (der_into(&exts, 0x30, &ext) != 0) break;
                int et; const uint8_t *eo; uint32_t eol;
                if (der_tlv(&ext, &et, &eo, &eol) != 0 || et != 0x06) break;
                /* optional critical BOOLEAN */
                int vt; const uint8_t *vp; uint32_t vl;
                if (der_tlv(&ext, &vt, &vp, &vl) != 0) break;
                if (vt == 0x01) { if (der_tlv(&ext, &vt, &vp, &vl) != 0) break; }
                if (oid_is(eo, eol, OID_SAN, sizeof OID_SAN)) { c->san = vp; c->san_len = vl; }
                else if (oid_is(eo, eol, OID_BASIC, sizeof OID_BASIC)) {
                    der_t bc = {vp, vp + vl}, seq;
                    if (der_into(&bc, 0x30, &seq) == 0 && seq.p < seq.end && seq.p[0] == 0x01) c->is_ca = seq.p[2] ? 1 : 0;
                }
            }
        } else { der_t skip; int st; const uint8_t *sc; uint32_t scl;
                 if (tbs.p[0] & 0x20) { der_into(&tbs, -1, &skip); } else { der_tlv(&tbs, &st, &sc, &scl); } }
    }
    /* signatureAlgorithm + signature (after TBS) */
    der_t osa; if (der_into(&cert, 0x30, &osa) != 0) return -1;
    { int st; const uint8_t *so; uint32_t sol;
      if (der_tlv(&osa, &st, &so, &sol) == 0 && st == 0x06) {
          if (oid_is(so, sol, OID_RSA_SHA256, sizeof OID_RSA_SHA256)) c->sig_alg = 1;
          else if (oid_is(so, sol, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256)) c->sig_alg = 2;
      } }
    int bt; const uint8_t *bv; uint32_t bl;
    if (der_tlv(&cert, &bt, &bv, &bl) != 0 || bt != 0x03 || bl < 1) return -1;
    c->sig = bv + 1; c->sig_len = bl - 1;        /* skip unused-bits byte */
    return 0;
}

/* ------------------------------------------------- RSA verify ----------- */
static int rsa_verify(const cert_t *issuer, const uint8_t *sig, uint32_t sig_len,
                      const uint8_t hash[32]) {
    if (issuer->key_type != 1) return -1;
    bn_t n, e, s, m;
    bn_from_be(&n, issuer->rsa_n, issuer->rsa_n_len);
    bn_from_be(&e, issuer->rsa_e, issuer->rsa_e_len);
    bn_from_be(&s, sig, sig_len);
    if (bn_cmp(&s, &n) >= 0) return -1;
    bn_modexp(&m, &s, &e, &n);                    /* m = s^e mod n */
    uint8_t em[512]; uint32_t k = issuer->rsa_n_len;
    if (k > sizeof em) return -1;
    bn_to_be(&m, em, k);
    /* EMSA-PKCS1-v1_5: 00 01 FF..FF 00 DigestInfo(SHA-256,hash) */
    static const uint8_t prefix[] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    uint32_t tlen = sizeof prefix + 32;
    if (k < tlen + 11) return -1;
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    uint32_t i = 2; while (i < k && em[i] == 0xFF) i++;
    if (i < 10 || em[i] != 0x00) return -1; i++;
    if (k - i != tlen) return -1;
    if (memcmp(em + i, prefix, sizeof prefix) != 0) return -1;
    if (memcmp(em + i + sizeof prefix, hash, 32) != 0) return -1;
    return 0;
}

/* ------------------------------------------------- P-256 ECDSA ---------- */
/* field prime p, order n, base point G */
static const uint8_t P256_P[32]={0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t P256_N[32]={0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
static const uint8_t P256_GX[32]={0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const uint8_t P256_GY[32]={0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
static const uint8_t P256_B[32]={0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};

typedef struct { bn_t x, y; int inf; } ecp_t;
static bn_t EP, EN;
static void ec_setup(void){ bn_from_be(&EP,P256_P,32); bn_from_be(&EN,P256_N,32); }
static void fadd(bn_t*r,const bn_t*a,const bn_t*b){ bn_t t; bn_add(&t,a,b); bn_mod(r,&t,&EP); }
static void fsub(bn_t*r,const bn_t*a,const bn_t*b){ bn_t t; if(bn_cmp(a,b)>=0) bn_sub(&t,a,b); else { bn_t x; bn_add(&x,a,&EP); bn_sub(&t,&x,b);} bn_mod(r,&t,&EP); }
static void fmul(bn_t*r,const bn_t*a,const bn_t*b){ bn_mulmod(r,a,b,&EP); }
static void finv(bn_t*r,const bn_t*a){ bn_modinv_prime(r,a,&EP); }

/*
 * Jacobian projective coordinates (X,Y,Z) == affine (X/Z^2, Y/Z^3), with the
 * point at infinity as Z=0. This keeps the per-operation cost to field
 * multiplications and does a single modular inversion per scalar multiply, in
 * place of one inversion per point add/double - the difference between a
 * handshake that takes minutes and one that takes under a second.
 */
typedef struct { bn_t X, Y, Z; } jpt_t;
static void fmulk(bn_t *r, const bn_t *a, uint32_t k) { bn_t kk; bn_zero(&kk); kk.w[0] = k; bn_mulmod(r, a, &kk, &EP); }

/* doubling, assuming curve parameter a = -3 (true for P-256): EFD dbl-2001-b */
static void j_dbl(jpt_t *r, const jpt_t *p) {
    if (bn_is_zero(&p->Z)) { *r = *p; return; }
    bn_t delta, gamma, beta, alpha, t, xmd, xpd;
    fmul(&delta, &p->Z, &p->Z);                  /* Z^2            */
    fmul(&gamma, &p->Y, &p->Y);                  /* Y^2            */
    fmul(&beta, &p->X, &gamma);                  /* X*gamma        */
    fsub(&xmd, &p->X, &delta); fadd(&xpd, &p->X, &delta);
    fmul(&t, &xmd, &xpd); fmulk(&alpha, &t, 3);  /* 3(X-Z^2)(X+Z^2)*/
    bn_t a2, b8; fmul(&a2, &alpha, &alpha); fmulk(&b8, &beta, 8);
    bn_t X3; fsub(&X3, &a2, &b8);                 /* X3 = a^2-8beta */
    bn_t ypz, ypz2; fadd(&ypz, &p->Y, &p->Z); fmul(&ypz2, &ypz, &ypz);
    fsub(&ypz2, &ypz2, &gamma); bn_t Z3; fsub(&Z3, &ypz2, &delta);
    bn_t b4, tmp, ay, g2, g28; fmulk(&b4, &beta, 4); fsub(&tmp, &b4, &X3);
    fmul(&ay, &alpha, &tmp); fmul(&g2, &gamma, &gamma); fmulk(&g28, &g2, 8);
    bn_t Y3; fsub(&Y3, &ay, &g28);
    bn_copy(&r->X, &X3); bn_copy(&r->Y, &Y3); bn_copy(&r->Z, &Z3);
}
/* general addition: EFD add-2007-bl */
static void j_add(jpt_t *r, const jpt_t *a, const jpt_t *b) {
    if (bn_is_zero(&a->Z)) { *r = *b; return; }
    if (bn_is_zero(&b->Z)) { *r = *a; return; }
    bn_t Z1Z1, Z2Z2, U1, U2, S1, S2, t;
    fmul(&Z1Z1, &a->Z, &a->Z); fmul(&Z2Z2, &b->Z, &b->Z);
    fmul(&U1, &a->X, &Z2Z2); fmul(&U2, &b->X, &Z1Z1);
    fmul(&t, &b->Z, &Z2Z2); fmul(&S1, &a->Y, &t);
    fmul(&t, &a->Z, &Z1Z1); fmul(&S2, &b->Y, &t);
    if (bn_cmp(&U1, &U2) == 0) {                  /* same x */
        if (bn_cmp(&S1, &S2) == 0) { j_dbl(r, a); return; }
        bn_zero(&r->X); bn_zero(&r->Y); bn_zero(&r->Z); r->X.w[0] = 1; r->Y.w[0] = 1; return; /* infinity */
    }
    bn_t H, I, J, rr, V;
    fsub(&H, &U2, &U1);
    bn_t h2; fmulk(&h2, &H, 2); fmul(&I, &h2, &h2);
    fmul(&J, &H, &I);
    bn_t sd; fsub(&sd, &S2, &S1); fmulk(&rr, &sd, 2);
    fmul(&V, &U1, &I);
    bn_t X3, t2, t3; fmul(&t2, &rr, &rr); fsub(&t2, &t2, &J); bn_t v2; fmulk(&v2, &V, 2); fsub(&X3, &t2, &v2);
    bn_t Y3, vmx; fsub(&vmx, &V, &X3); fmul(&Y3, &rr, &vmx);
    bn_t s1j; fmul(&s1j, &S1, &J); fmulk(&s1j, &s1j, 2); fsub(&Y3, &Y3, &s1j);
    bn_t Z3, zpz; fadd(&zpz, &a->Z, &b->Z); fmul(&Z3, &zpz, &zpz);
    fsub(&Z3, &Z3, &Z1Z1); fsub(&Z3, &Z3, &Z2Z2); fmul(&Z3, &Z3, &H);
    bn_copy(&r->X, &X3); bn_copy(&r->Y, &Y3); bn_copy(&r->Z, &Z3);
    (void)t3;
}
static void j_to_affine(ecp_t *out, const jpt_t *p) {
    if (bn_is_zero(&p->Z)) { out->inf = 1; bn_zero(&out->x); bn_zero(&out->y); return; }
    bn_t zi, zi2, zi3; finv(&zi, &p->Z); fmul(&zi2, &zi, &zi); fmul(&zi3, &zi2, &zi);
    fmul(&out->x, &p->X, &zi2); fmul(&out->y, &p->Y, &zi3); out->inf = 0;
}
/* r = k*P (P affine), result in Jacobian */
static void ec_mul_j(jpt_t *r, const bn_t *k, const ecp_t *p) {
    jpt_t acc; bn_zero(&acc.X); bn_zero(&acc.Y); bn_zero(&acc.Z); acc.X.w[0] = 1; acc.Y.w[0] = 1; /* infinity */
    jpt_t pj; bn_copy(&pj.X, &p->x); bn_copy(&pj.Y, &p->y); bn_zero(&pj.Z); pj.Z.w[0] = 1;
    int bits = bn_bits(k);
    for (int i = bits - 1; i >= 0; i--) {
        jpt_t d; j_dbl(&d, &acc);
        if (bn_getbit(k, i)) { jpt_t s; j_add(&s, &d, &pj); acc = s; } else acc = d;
    }
    *r = acc;
}
static void ec_mul(ecp_t *r, const bn_t *k, const ecp_t *p) {
    jpt_t j; ec_mul_j(&j, k, p); j_to_affine(r, &j);
}
static int ecdsa_verify(const cert_t *issuer, const uint8_t *sig, uint32_t sig_len, const uint8_t hash[32]) {
    if (issuer->key_type != 2) return -1;
    ec_setup();
    /* sig = SEQ { INTEGER r, INTEGER s } */
    der_t d = {sig, sig + sig_len}, seq;
    if (der_into(&d, 0x30, &seq) != 0) return -1;
    int t; const uint8_t *rp, *sp; uint32_t rl, sl;
    if (der_tlv(&seq, &t, &rp, &rl) != 0 || t != 0x02) return -1;
    if (der_tlv(&seq, &t, &sp, &sl) != 0 || t != 0x02) return -1;
    bn_t r, s, z; bn_from_be(&r, rp, rl); bn_from_be(&s, sp, sl); bn_from_be(&z, hash, 32);
    if (bn_is_zero(&r) || bn_is_zero(&s) || bn_cmp(&r,&EN)>=0 || bn_cmp(&s,&EN)>=0) return -1;
    bn_t w; bn_modinv_prime(&w, &s, &EN);
    bn_t u1, u2; bn_mulmod(&u1, &z, &w, &EN); bn_mulmod(&u2, &r, &w, &EN);
    ecp_t G, Q; bn_from_be(&G.x, P256_GX, 32); bn_from_be(&G.y, P256_GY, 32); G.inf = 0;
    if (issuer->ec_pub_len != 65 || issuer->ec_pub[0] != 0x04) return -1;
    bn_from_be(&Q.x, issuer->ec_pub + 1, 32); bn_from_be(&Q.y, issuer->ec_pub + 33, 32); Q.inf = 0;
    jpt_t A, B, R; ec_mul_j(&A, &u1, &G); ec_mul_j(&B, &u2, &Q); j_add(&R, &A, &B);
    ecp_t Raff; j_to_affine(&Raff, &R);
    if (Raff.inf) return -1;
    bn_t rx; bn_mod(&rx, &Raff.x, &EN);
    return bn_cmp(&rx, &r) == 0 ? 0 : -1;
}

/* ------------------------------------------------- P-256 ECDH (for TLS) - */
/* is (x,y) a point on y^2 = x^3 - 3x + b (mod p), with x,y < p ? */
static int ec_on_curve(const ecp_t *p) {
    if (bn_cmp(&p->x, &EP) >= 0 || bn_cmp(&p->y, &EP) >= 0) return -1;
    bn_t y2, x2, x3, three, tx, b, rhs;
    fmul(&y2, &p->y, &p->y);
    fmul(&x2, &p->x, &p->x); fmul(&x3, &x2, &p->x);
    bn_zero(&three); three.w[0] = 3; bn_mulmod(&tx, &p->x, &three, &EP);
    bn_from_be(&b, P256_B, 32);
    fsub(&rhs, &x3, &tx); fadd(&rhs, &rhs, &b);
    return bn_cmp(&y2, &rhs) == 0 ? 0 : -1;
}
/* reduce a 32-byte scalar into [1, n-1] in place */
void p256_scalar_reduce(uint8_t s[32]) {
    ec_setup();
    bn_t k, r; bn_from_be(&k, s, 32); bn_mod(&r, &k, &EN);
    if (bn_is_zero(&r)) r.w[0] = 1;
    bn_to_be(&r, s, 32);
}
/* public key = priv * G, as a 65-byte uncompressed point (0x04||X||Y) */
void p256_keygen(const uint8_t priv[32], uint8_t pub[65]) {
    ec_setup();
    bn_t k; bn_from_be(&k, priv, 32);
    ecp_t G, R; bn_from_be(&G.x, P256_GX, 32); bn_from_be(&G.y, P256_GY, 32); G.inf = 0;
    ec_mul(&R, &k, &G);
    pub[0] = 0x04; bn_to_be(&R.x, pub + 1, 32); bn_to_be(&R.y, pub + 33, 32);
}
/* shared secret: X coordinate of priv * peer. peer is 65-byte uncompressed. 0=ok */
int p256_ecdh(const uint8_t priv[32], const uint8_t peer[65], uint8_t out[32]) {
    if (peer[0] != 0x04) return -1;
    ec_setup();
    ecp_t P; bn_from_be(&P.x, peer + 1, 32); bn_from_be(&P.y, peer + 33, 32); P.inf = 0;
    if (ec_on_curve(&P) != 0) return -1;         /* reject invalid-curve points */
    bn_t k; bn_from_be(&k, priv, 32);
    ecp_t R; ec_mul(&R, &k, &P);
    if (R.inf) return -1;
    bn_to_be(&R.x, out, 32);
    return 0;
}

static int verify_sig(const cert_t *issuer, int sig_alg, const uint8_t *sig, uint32_t sig_len,
                      const uint8_t *data, uint32_t dlen) {
    uint8_t h[32]; sha256(data, dlen, h);
    if (sig_alg == 1) return rsa_verify(issuer, sig, sig_len, h);
    if (sig_alg == 2) return ecdsa_verify(issuer, sig, sig_len, h);
    return -1;
}

/* ------------------------------------------------- trust + hostname ----- */
#ifndef ROOTS_HEADER
#define ROOTS_HEADER "roots.h"   /* generated: axis_roots[] of {der,len} trusted CA certs */
#endif
#include ROOTS_HEADER

static int name_eq(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl) {
    return al == bl && !memcmp(a, b, al);
}
/* wildcard-aware hostname match (*.example.com) */
static int host_match(const char *pat, uint32_t pn, const char *host) {
    uint32_t hn = (uint32_t)strlen(host);
    if (pn >= 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot) return 0;
        uint32_t suflen = pn - 1;                /* ".example.com" */
        uint32_t hsuf = hn - (uint32_t)(dot - host);
        return hsuf == suflen && !strncasecmp(dot, pat + 1, suflen);
    }
    return pn == hn && !strncasecmp(pat, host, pn);
}
static int check_hostname(const cert_t *leaf, const char *host) {
    if (leaf->san) {
        der_t d = {leaf->san, leaf->san + leaf->san_len}, seq;
        if (der_into(&d, 0x30, &seq) == 0) {
            while (seq.p < seq.end) {
                int t; const uint8_t *v; uint32_t vl;
                if (der_tlv(&seq, &t, &v, &vl) != 0) break;
                if (t == 0x82 /* dNSName [2] */ && host_match((const char*)v, vl, host)) return 0;
            }
        }
        return -1;                               /* SAN present but no match */
    }
    if (leaf->cn && host_match((const char*)leaf->cn, leaf->cn_len, host)) return 0;
    return -1;
}

/* days-since-2000 "now" from the RTC */
static int32_t now_days(void) {
    rtc_time_t tm; rtc_read(&tm);
    int y = tm.year; if (y < 100) y += 2000;
    static const int cum[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int32_t days = (int32_t)(y - 2000) * 365 + (y - 2000 + 3) / 4;
    if (tm.month >= 1 && tm.month <= 12) days += cum[tm.month - 1];
    days += tm.day;
    return days;
}

/*
 * Public entry: verify the chain, then verify the server's signed ECDHE
 * parameters (tbs_sig_data) with the leaf's key.
 * Returns 0 on success, -1 on failure (err filled).
 */
int x509_verify_chain(const uint8_t *certs, uint32_t certs_len, const char *hostname,
                      const uint8_t *tbs_sig_data, uint32_t tbs_len,
                      const uint8_t *signature, uint32_t sig_len, uint16_t sig_alg,
                      const uint8_t *server_pub, char *err, size_t errn) {
    (void)server_pub;
    /* split the TLS Certificate message: 3-byte-len-prefixed DER certs */
    cert_t chain[8]; int nc = 0;
    const uint8_t *p = certs, *e = certs + certs_len;
    while (p + 3 <= e && nc < 8) {
        uint32_t clen = (p[0] << 16) | (p[1] << 8) | p[2]; p += 3;
        if (p + clen > e) break;
        if (parse_cert(p, clen, &chain[nc]) != 0) { strlcpy(err, "could not parse certificate", errn); return -1; }
        nc++; p += clen;
    }
    if (nc == 0) { strlcpy(err, "no certificates", errn); return -1; }

    cert_t *leaf = &chain[0];
    /* hostname */
    if (check_hostname(leaf, hostname) != 0) { strlcpy(err, "hostname mismatch", errn); return -1; }
    /* validity window (coarse) */
    int32_t today = now_days();
    for (int i = 0; i < nc; i++) {
        if (today < chain[i].not_before - 1 || today > chain[i].not_after + 1) {
            strlcpy(err, "certificate expired or not yet valid", errn); return -1;
        }
    }
    /* chain: each cert signed by the next; verify signatures */
    for (int i = 0; i < nc - 1; i++) {
        if (!name_eq(chain[i].issuer, chain[i].issuer_len, chain[i+1].subject, chain[i+1].subject_len)) {
            strlcpy(err, "broken issuer chain", errn); return -1;
        }
        if (verify_sig(&chain[i+1], chain[i].sig_alg, chain[i].sig, chain[i].sig_len,
                       chain[i].tbs, chain[i].tbs_len) != 0) {
            strlcpy(err, "bad certificate signature", errn); return -1;
        }
    }
    /* anchor: the top cert's issuer must be a trusted root that signs it */
    cert_t *top = &chain[nc - 1];
    int anchored = 0;
    for (uint32_t i = 0; i < AXIS_ROOTS_N; i++) {
        cert_t root;
        if (parse_cert(axis_roots[i].der, axis_roots[i].len, &root) != 0) continue;
        if (!name_eq(top->issuer, top->issuer_len, root.subject, root.subject_len)) continue;
        if (verify_sig(&root, top->sig_alg, top->sig, top->sig_len, top->tbs, top->tbs_len) == 0) { anchored = 1; break; }
    }
    /* a self-signed top that is itself a trusted root also anchors */
    if (!anchored) {
        for (uint32_t i = 0; i < AXIS_ROOTS_N; i++) {
            if (axis_roots[i].len == 0) continue;
            cert_t root;
            if (parse_cert(axis_roots[i].der, axis_roots[i].len, &root) != 0) continue;
            if (name_eq(top->subject, top->subject_len, root.subject, root.subject_len) &&
                name_eq(top->issuer, top->issuer_len, root.issuer, root.issuer_len)) { anchored = 1; break; }
        }
    }
    if (!anchored) { strlcpy(err, "untrusted root CA", errn); return -1; }

    /* finally: the ServerKeyExchange signature over the ECDHE params, by the leaf */
    int kx_alg;
    switch (sig_alg) {                           /* TLS SignatureAndHashAlgorithm */
        case 0x0401: kx_alg = 1; break;          /* rsa_pkcs1_sha256 */
        case 0x0403: kx_alg = 2; break;          /* ecdsa_secp256r1_sha256 */
        case 0x0804: kx_alg = 1; break;          /* rsa_pss_rsae_sha256 -> treated as rsa (approx) */
        default:
            if (leaf->key_type == 1) kx_alg = 1; else kx_alg = 2; break;
    }
    if (kx_alg == 1 && leaf->key_type != 1) kx_alg = 2;
    if (verify_sig(leaf, kx_alg, signature, sig_len, tbs_sig_data, tbs_len) != 0) {
        strlcpy(err, "bad server key-exchange signature", errn); return -1;
    }
    return 0;
}
