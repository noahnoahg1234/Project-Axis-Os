/*
 * AxisOS - baseline JPEG decoder (JFIF / SOF0 sequential).
 *
 * Decodes ordinary .jpg images to RGB: marker parsing, Huffman decode,
 * dequantise, a fixed-point separable integer IDCT, chroma upsampling and
 * YCbCr->RGB. No floating point. Used by the image viewer, the file explorer,
 * ClassicTube (real YouTube thumbnails and MJPEG video) and anything else.
 *
 * Supported: baseline sequential, 1 component (grey) or 3 (YCbCr), any
 * sampling factors, restart markers. Not supported: progressive (SOF2),
 * arithmetic coding, CMYK - jpeg_decode() returns an error for those.
 */
#include "axis.h"

/* IDCT basis, Q13: spatial[x] = sum_u M[u][x] * freq[u]  (normalisation folded in) */
static const int idct_m[8][8] = {
  {  2896,   2896,   2896,   2896,   2896,   2896,   2896,   2896},
  {  4017,   3406,   2276,    799,   -799,  -2276,  -3406,  -4017},
  {  3784,   1567,  -1567,  -3784,  -3784,  -1567,   1567,   3784},
  {  3406,   -799,  -4017,  -2276,   2276,   4017,    799,  -3406},
  {  2896,  -2896,  -2896,   2896,   2896,  -2896,  -2896,   2896},
  {  2276,  -4017,    799,   3406,  -3406,   -799,   4017,  -2276},
  {  1567,  -3784,   3784,  -1567,  -1567,   3784,  -3784,   1567},
  {   799,  -2276,   3406,  -4017,   4017,  -3406,   2276,   -799},
};
static const uint8_t zz[64] = {
  0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

typedef struct { uint8_t bits[17]; uint8_t vals[256]; int mincode[17], maxcode[18], valptr[17]; } huff_t;
typedef struct {
    const uint8_t *d; uint32_t len, pos;
    int W, H, ncomp;
    uint16_t qt[4][64];                 /* quant tables, zigzag order */
    huff_t hdc[4], hac[4];
    int hpresent_dc[4], hpresent_ac[4];
    struct { int id, h, v, tq, td, ta; } comp[3];
    int hmax, vmax, dri;
    /* entropy bit reader */
    uint32_t bits; int nbits; int eoi;
    int dcpred[3];
} jpg_t;

static int rd8(jpg_t *j) { return j->pos < j->len ? j->d[j->pos++] : -1; }
static int rd16(jpg_t *j) { int a = rd8(j), b = rd8(j); return (a < 0 || b < 0) ? -1 : ((a << 8) | b); }

static void build_huff(huff_t *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code;
        code += h->bits[l]; k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
}

/* entropy-coded bit input with 0xFF stuffing + restart/EOI detection */
static int nextbit(jpg_t *j) {
    if (j->nbits == 0) {
        if (j->eoi) return 0;
        int b = rd8(j);
        if (b < 0) { j->eoi = 1; return 0; }
        if (b == 0xFF) {
            int m = rd8(j);
            if (m != 0x00) {                /* a marker: stop consuming entropy data */
                j->pos -= 2; j->eoi = 1; return 0;
            }
        }
        j->bits = b; j->nbits = 8;
    }
    j->nbits--;
    return (j->bits >> j->nbits) & 1;
}
static int receive(jpg_t *j, int n) { int v = 0; while (n--) v = (v << 1) | nextbit(j); return v; }
static int extend(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }
static int huffdec(jpg_t *j, huff_t *h) {
    int code = nextbit(j), l = 1;
    while (code > h->maxcode[l]) { code = (code << 1) | nextbit(j); l++; if (l > 16) return 0; }
    return h->vals[h->valptr[l] + code - h->mincode[l]];
}

/* separable fixed-point IDCT of an 8x8 block -> out[64] clamped 0..255 (+128 shift) */
static void idct(const int *in, uint8_t *out, int ostride) {
    int tmp[64];
    for (int v = 0; v < 8; v++) {               /* rows: freq -> spatial */
        const int *row = in + v * 8;
        if (!(row[1]|row[2]|row[3]|row[4]|row[5]|row[6]|row[7])) {
            int dc = row[0] * idct_m[0][0] >> 8;
            for (int x = 0; x < 8; x++) tmp[v*8+x] = dc;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            int s = 0; for (int u = 0; u < 8; u++) s += idct_m[u][x] * row[u];
            tmp[v*8+x] = s >> 8;
        }
    }
    for (int x = 0; x < 8; x++) {                /* columns */
        for (int y = 0; y < 8; y++) {
            int s = 0; for (int v = 0; v < 8; v++) s += idct_m[v][y] * tmp[v*8+x];
            s = (s >> 18) + 128;
            if (s < 0) s = 0; else if (s > 255) s = 255;
            out[y * ostride + x] = (uint8_t)s;
        }
    }
}

int jpeg_decode(const uint8_t *data, uint32_t len, int *w, int *h, uint8_t **rgb_out) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return -1;  /* SOI */
    jpg_t *j = kmalloc(sizeof *j);
    if (!j) return -1;
    memset(j, 0, sizeof *j);
    j->d = data; j->len = len; j->pos = 2;
    uint8_t *planes[3] = {0,0,0};
    int pw[3] = {0,0,0}, ph[3] = {0,0,0};
    int rc = -1;

    for (;;) {
        int m = rd8(j);
        if (m < 0) goto done;
        if (m != 0xFF) continue;
        while (m == 0xFF) m = rd8(j);           /* skip fill bytes */
        if (m == 0xD9) goto done;               /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue; /* standalone */
        int seglen = rd16(j);
        if (seglen < 2) goto fail;
        uint32_t segend = j->pos + seglen - 2;

        if (m == 0xDB) {                        /* DQT */
            while (j->pos < segend) {
                int pq = rd8(j); int id = pq & 15; pq >>= 4;
                if (id > 3) goto fail;
                for (int i = 0; i < 64; i++) j->qt[id][i] = pq ? rd16(j) : rd8(j);
            }
        } else if (m == 0xC0 || m == 0xC1) {    /* SOF0/1 baseline */
            int prec = rd8(j); (void)prec;
            j->H = rd16(j); j->W = rd16(j); j->ncomp = rd8(j);
            if (j->ncomp != 1 && j->ncomp != 3) goto fail;
            if (j->W <= 0 || j->H <= 0 || j->W > 2048 || j->H > 2048) goto fail;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].id = rd8(j); int hv = rd8(j);
                j->comp[c].h = hv >> 4; j->comp[c].v = hv & 15; j->comp[c].tq = rd8(j);
                if (j->comp[c].h > j->hmax) j->hmax = j->comp[c].h;
                if (j->comp[c].v > j->vmax) j->vmax = j->comp[c].v;
            }
        } else if (m == 0xC2) {                 /* progressive - unsupported */
            goto fail;
        } else if (m == 0xC4) {                 /* DHT */
            while (j->pos < segend) {
                int tc = rd8(j); int id = tc & 15; tc >>= 4;
                if (id > 3 || tc > 1) goto fail;
                huff_t *ht = tc ? &j->hac[id] : &j->hdc[id];
                int total = 0;
                for (int i = 1; i <= 16; i++) { ht->bits[i] = rd8(j); total += ht->bits[i]; }
                for (int i = 0; i < total; i++) ht->vals[i] = rd8(j);
                build_huff(ht);
                if (tc) j->hpresent_ac[id] = 1; else j->hpresent_dc[id] = 1;
            }
        } else if (m == 0xDD) {                 /* DRI */
            j->dri = rd16(j);
        } else if (m == 0xDA) {                 /* SOS - start of scan */
            int ns = rd8(j);
            if (ns != j->ncomp) goto fail;      /* non-interleaved scans unsupported */
            for (int i = 0; i < ns; i++) {
                int cid = rd8(j), t = rd8(j);
                for (int c = 0; c < j->ncomp; c++) if (j->comp[c].id == cid) { j->comp[c].td = t >> 4; j->comp[c].ta = t & 15; }
            }
            rd8(j); rd8(j); rd8(j);             /* Ss, Se, Ah/Al */
            goto scan;
        } else {
            j->pos = segend;                    /* skip APPn, COM, etc. */
        }
        if (j->pos != segend) j->pos = segend;
        continue;

    scan:;
        /* allocate per-component planes padded to MCU size */
        int mcux = (j->W + 8 * j->hmax - 1) / (8 * j->hmax);
        int mcuy = (j->H + 8 * j->vmax - 1) / (8 * j->vmax);
        for (int c = 0; c < j->ncomp; c++) {
            pw[c] = mcux * j->comp[c].h * 8; ph[c] = mcuy * j->comp[c].v * 8;
            planes[c] = kmalloc((uint32_t)pw[c] * ph[c]);
            if (!planes[c]) goto fail;
        }
        j->nbits = 0; j->eoi = 0; j->dcpred[0] = j->dcpred[1] = j->dcpred[2] = 0;
        int rst = j->dri, mcucount = 0;
        for (int my = 0; my < mcuy; my++) {
            for (int mx = 0; mx < mcux; mx++) {
                for (int c = 0; c < j->ncomp; c++) {
                    huff_t *dc = &j->hdc[j->comp[c].td], *ac = &j->hac[j->comp[c].ta];
                    uint16_t *q = j->qt[j->comp[c].tq];
                    for (int by = 0; by < j->comp[c].v; by++) {
                        for (int bx = 0; bx < j->comp[c].h; bx++) {
                            int F[64]; memset(F, 0, sizeof F);
                            int t = huffdec(j, dc);
                            int diff = t ? extend(receive(j, t), t) : 0;
                            j->dcpred[c] += diff;
                            F[0] = j->dcpred[c] * q[0];
                            int k = 1;
                            while (k < 64) {
                                int rs = huffdec(j, ac); int r = rs >> 4, s = rs & 15;
                                if (s == 0) { if (r == 15) { k += 16; continue; } break; }
                                k += r; if (k > 63) break;
                                F[zz[k]] = extend(receive(j, s), s) * q[k]; k++;
                            }
                            int px = (mx * j->comp[c].h + bx) * 8;
                            int py = (my * j->comp[c].v + by) * 8;
                            idct(F, planes[c] + py * pw[c] + px, pw[c]);
                        }
                    }
                }
                if (j->dri && --rst == 0 && !(my == mcuy-1 && mx == mcux-1)) {
                    /* align to byte, consume RSTn marker, reset predictors */
                    j->nbits = 0;
                    int b = rd8(j); while (b == 0xFF) { int mm = rd8(j); if (mm >= 0xD0 && mm <= 0xD7) break; if (mm < 0) break; b = mm; }
                    j->eoi = 0; j->dcpred[0] = j->dcpred[1] = j->dcpred[2] = 0; rst = j->dri;
                }
                mcucount++;
            }
        }
        (void)mcucount;
        break;
    }

    /* compose RGB */
    *rgb_out = kmalloc((uint32_t)j->W * j->H * 3);
    if (!*rgb_out) goto fail;
    for (int y = 0; y < j->H; y++) {
        for (int x = 0; x < j->W; x++) {
            uint8_t *o = *rgb_out + ((uint32_t)y * j->W + x) * 3;
            if (j->ncomp == 1) {
                int Y = planes[0][y * pw[0] + x];
                o[0] = o[1] = o[2] = (uint8_t)Y;
            } else {
                int yx = x * j->comp[0].h / j->hmax, yy = y * j->comp[0].v / j->vmax;
                int cbx = x * j->comp[1].h / j->hmax, cby = y * j->comp[1].v / j->vmax;
                int crx = x * j->comp[2].h / j->hmax, cry = y * j->comp[2].v / j->vmax;
                int Y = planes[0][yy * pw[0] + yx];
                int Cb = planes[1][cby * pw[1] + cbx] - 128;
                int Cr = planes[2][cry * pw[2] + crx] - 128;
                int r = Y + ((91881 * Cr) >> 16);
                int g = Y - ((22554 * Cb + 46802 * Cr) >> 16);
                int b = Y + ((116130 * Cb) >> 16);
                o[0] = r < 0 ? 0 : r > 255 ? 255 : r;
                o[1] = g < 0 ? 0 : g > 255 ? 255 : g;
                o[2] = b < 0 ? 0 : b > 255 ? 255 : b;
            }
        }
    }
    *w = j->W; *h = j->H; rc = 0;
    goto done;

fail:
    rc = -1;
done:
    for (int c = 0; c < 3; c++) kfree(planes[c]);
    uint32_t endpos = j->pos; (void)endpos;
    kfree(j);
    return rc;
}

/* how many bytes the first JPEG in the buffer occupies (SOI..EOI), for MJPEG */
uint32_t jpeg_frame_len(const uint8_t *data, uint32_t len) {
    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) return 0;
    uint32_t p = 2;
    while (p + 1 < len) {
        if (data[p] != 0xFF) { p++; continue; }
        int m = data[p + 1];
        if (m == 0xD9) return p + 2;             /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { p += 2; continue; }
        if (p + 3 >= len) break;
        uint32_t sl = (data[p + 2] << 8) | data[p + 3];
        p += 2 + sl;
        if (m == 0xDA) {                         /* entropy data until next marker */
            while (p + 1 < len) {
                if (data[p] == 0xFF && data[p+1] != 0x00 && !(data[p+1] >= 0xD0 && data[p+1] <= 0xD7)) break;
                p++;
            }
        }
    }
    return 0;
}
