/*
 * AxisOS - animated GIF decoder (GIF87a / GIF89a).
 *
 * Decodes real moving pictures: LZW-compressed, palette-indexed frames with
 * per-frame delays and disposal, composited onto an RGB canvas. ClassicTube
 * uses it to actually load and play video (as multi-colour half-block text),
 * from a file on disk or fetched over HTTP/HTTPS. No floating point.
 *
 * Supported: GIF87a/89a, interlace, transparency, disposal methods 0-3,
 * local and global colour tables. Not supported: the (rarely used) Plain Text
 * extension, which is skipped.
 */
#include "axis.h"

#define GIF_MAXDIM 800              /* reject absurd canvases */

struct gif {
    const uint8_t *d; uint32_t len, pos;
    int W, H;
    uint8_t gct[768]; int gct_sz; int has_gct; int bg;
    uint8_t *canvas;                /* W*H*3 RGB, current composited frame */
    uint8_t *prev;                  /* W*H*3 backup for disposal method 3 */
    uint8_t bgr, bgg, bgb;          /* background colour (disposal method 2) */
    uint32_t first_pos;             /* block stream start, for gif_reset */
    /* disposal bookkeeping for the previously drawn frame */
    int last_disposal, lx, ly, lw, lh;
    int started;
};

/* ---- little helpers over the byte stream ------------------------------- */
static int rd_u8(gif_t *g) { return g->pos < g->len ? g->d[g->pos++] : -1; }
static int rd_u16(gif_t *g) { int a = rd_u8(g), b = rd_u8(g); return (a < 0 || b < 0) ? -1 : (a | (b << 8)); }
static void skip_subblocks(gif_t *g) {
    int n; while ((n = rd_u8(g)) > 0) { if (g->pos + n > g->len) { g->pos = g->len; return; } g->pos += n; }
}

gif_t *gif_open(const uint8_t *data, uint32_t len) {
    if (len < 13 || memcmp(data, "GIF8", 4) != 0) return NULL;
    gif_t *g = kmalloc(sizeof *g);
    if (!g) return NULL;
    memset(g, 0, sizeof *g);
    g->d = data; g->len = len; g->pos = 6;              /* skip "GIF89a" */
    g->W = rd_u16(g); g->H = rd_u16(g);
    int packed = rd_u8(g); g->bg = rd_u8(g); rd_u8(g);  /* bg, aspect */
    if (g->W <= 0 || g->H <= 0 || g->W > GIF_MAXDIM || g->H > GIF_MAXDIM) { kfree(g); return NULL; }
    g->has_gct = (packed & 0x80) != 0;
    g->gct_sz = 1 << ((packed & 7) + 1);
    if (g->has_gct) { for (int i = 0; i < g->gct_sz * 3; i++) { int b = rd_u8(g); g->gct[i] = b < 0 ? 0 : b; } }
    g->canvas = kmalloc((uint32_t)g->W * g->H * 3);
    g->prev = kmalloc((uint32_t)g->W * g->H * 3);
    if (!g->canvas || !g->prev) { gif_close(g); return NULL; }
    /* background colour from the global colour table (used by disposal method 2) */
    if (g->has_gct && g->bg < g->gct_sz) { g->bgr = g->gct[g->bg*3]; g->bgg = g->gct[g->bg*3+1]; g->bgb = g->gct[g->bg*3+2]; }
    for (uint32_t i = 0; i < (uint32_t)g->W * g->H; i++) { g->canvas[i*3] = g->bgr; g->canvas[i*3+1] = g->bgg; g->canvas[i*3+2] = g->bgb; }
    g->first_pos = g->pos;
    g->last_disposal = 0;
    return g;
}
int gif_width(gif_t *g) { return g->W; }
int gif_height(gif_t *g) { return g->H; }
const uint8_t *gif_canvas(gif_t *g) { return g->canvas; }   /* current frame, W*H*3 RGB */

void gif_reset(gif_t *g) {
    g->pos = g->first_pos;
    g->last_disposal = 0; g->started = 0;
    for (uint32_t i = 0; i < (uint32_t)g->W * g->H; i++) { g->canvas[i*3] = g->bgr; g->canvas[i*3+1] = g->bgg; g->canvas[i*3+2] = g->bgb; }
}

/* apply disposal of the frame we drew last, before drawing the next one */
static void apply_disposal(gif_t *g) {
    if (g->last_disposal == 2) {                        /* restore to background colour */
        for (int y = g->ly; y < g->ly + g->lh && y < g->H; y++)
            for (int x = g->lx; x < g->lx + g->lw && x < g->W; x++) {
                uint8_t *p = g->canvas + ((uint32_t)y * g->W + x) * 3; p[0] = g->bgr; p[1] = g->bgg; p[2] = g->bgb;
            }
    } else if (g->last_disposal == 3) {                 /* restore previous */
        memcpy(g->canvas, g->prev, (uint32_t)g->W * g->H * 3);
    }
}

/* decode one LZW image of w*h indices into out (caller-sized w*h).
 * Gathers the sub-blocks into one buffer first, so g->pos is left exactly
 * after the terminating zero-length sub-block, ready for the next block. */
static int lzw_decode(gif_t *g, int w, int h, uint8_t *out) {
    int mincode = rd_u8(g);
    if (mincode < 2 || mincode > 8) return -1;
    /* gather the image's data sub-blocks into one contiguous buffer */
    uint8_t *data = NULL; uint32_t dlen = 0, dcap = 0; int n;
    while ((n = rd_u8(g)) > 0) {
        if (g->pos + n > g->len) break;
        if (dlen + n > dcap) { dcap = dcap ? dcap * 2 : 1024; while (dcap < dlen + n) dcap *= 2;
            uint8_t *nd = krealloc(data, dcap); if (!nd) { kfree(data); return -1; } data = nd; }
        memcpy(data + dlen, g->d + g->pos, n); dlen += n; g->pos += n;
    }
    static uint16_t prefix[4096]; static uint8_t suffix[4096]; static uint8_t stack[4096];
    int clear = 1 << mincode, end = clear + 1;
    int width = mincode + 1, next = end + 1;
    uint32_t acc = 0; int bits = 0, bytepos = 0;
    long need = (long)w * h, got = 0;
    int prev_code = -1, first_byte = 0, sp = 0;
    for (;;) {
        while (bits < width && bytepos < (int)dlen) { acc |= (uint32_t)data[bytepos++] << bits; bits += 8; }
        if (bits < width) break;                        /* out of data */
        int code = acc & ((1 << width) - 1); acc >>= width; bits -= width;
        if (code == clear) { width = mincode + 1; next = end + 1; prev_code = -1; continue; }
        if (code == end) break;
        sp = 0;
        int cur = code;
        if (code >= next) { stack[sp++] = first_byte; cur = prev_code; }  /* KwKwK */
        if (cur < 0 || cur >= 4096) break;
        while (cur >= clear) { stack[sp++] = suffix[cur]; cur = prefix[cur]; if (sp >= 4095) break; }
        first_byte = cur;
        stack[sp++] = (uint8_t)cur;
        while (sp > 0 && got < need) out[got++] = stack[--sp];
        if (prev_code >= 0 && next < 4096) {
            prefix[next] = (uint16_t)prev_code; suffix[next] = (uint8_t)first_byte; next++;
            if (next == (1 << width) && width < 12) width++;
        }
        prev_code = code;
    }
    kfree(data);
    return got >= need ? 0 : -1;
}

/* returns 1 if a frame was decoded (canvas updated), 0 at end of file */
int gif_next_frame(gif_t *g, int *delay_cs) {
    int disposal = 0, trans_flag = 0, trans_idx = -1, delay = 10;
    for (;;) {
        int blk = rd_u8(g);
        if (blk < 0 || blk == 0x3B) return 0;           /* trailer / EOF */
        if (blk == 0x21) {                              /* extension */
            int label = rd_u8(g);
            if (label == 0xF9) {                        /* graphic control */
                int sz = rd_u8(g);                      /* == 4 */
                int packed = rd_u8(g);
                delay = rd_u16(g);
                trans_idx = rd_u8(g);
                rd_u8(g);                               /* block terminator */
                disposal = (packed >> 2) & 7;
                trans_flag = packed & 1;
                (void)sz;
            } else { skip_subblocks(g); }
            continue;
        }
        if (blk != 0x2C) { /* unknown block: bail to avoid desync */ return 0; }

        /* image descriptor */
        int ix = rd_u16(g), iy = rd_u16(g), iw = rd_u16(g), ih = rd_u16(g);
        int packed = rd_u8(g);
        if (ix < 0 || iy < 0 || iw <= 0 || ih <= 0 || ix + iw > g->W || iy + ih > g->H) return 0;
        uint8_t lct[768]; int has_lct = packed & 0x80, lct_sz = 1 << ((packed & 7) + 1);
        int interlace = packed & 0x40;
        const uint8_t *pal = g->gct; int pal_sz = g->gct_sz;
        if (has_lct) { for (int i = 0; i < lct_sz * 3; i++) { int b = rd_u8(g); lct[i] = b < 0 ? 0 : b; } pal = lct; pal_sz = lct_sz; }

        apply_disposal(g);                              /* dispose the previous frame */
        if (disposal == 3) memcpy(g->prev, g->canvas, (uint32_t)g->W * g->H * 3);

        uint8_t *idx = kmalloc((uint32_t)iw * ih);
        if (!idx) return 0;
        if (lzw_decode(g, iw, ih, idx) != 0) { kfree(idx); return 0; }

        /* composite onto the canvas (handle interlace + transparency) */
        static const int i_start[4] = {0, 4, 2, 1}, i_step[4] = {8, 8, 4, 2};
        int row = 0;
        for (int pass = 0; pass < (interlace ? 4 : 1); pass++) {
            int sy = interlace ? i_start[pass] : 0, step = interlace ? i_step[pass] : 1;
            for (int y = sy; y < ih; y += step, row++) {
                for (int x = 0; x < iw; x++) {
                    int ci = idx[row * iw + x];
                    if (trans_flag && ci == trans_idx) continue;
                    if (ci >= pal_sz) continue;
                    uint8_t *dst = g->canvas + ((uint32_t)(iy + y) * g->W + (ix + x)) * 3;
                    dst[0] = pal[ci * 3]; dst[1] = pal[ci * 3 + 1]; dst[2] = pal[ci * 3 + 2];
                }
            }
        }
        kfree(idx);
        g->last_disposal = disposal; g->lx = ix; g->ly = iy; g->lw = iw; g->lh = ih;
        g->started = 1;
        if (delay_cs) *delay_cs = delay > 0 ? delay : 10;
        return 1;
    }
}

/* nearest of the 16 VGA text colours */
static const uint8_t vga[16][3] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}
};
static uint8_t nearest(int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        int dr = r - vga[i][0], dg = g - vga[i][1], db = b - vga[i][2];
        int d = dr*dr + dg*dg + db*db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

/* downscale the whole canvas into outw*outh VGA colour indices */
void gif_sample(gif_t *g, uint8_t *out, int outw, int outh) {
    for (int y = 0; y < outh; y++) {
        int sy = y * g->H / outh;
        for (int x = 0; x < outw; x++) {
            int sx = x * g->W / outw;
            uint8_t *p = g->canvas + ((uint32_t)sy * g->W + sx) * 3;
            out[y * outw + x] = nearest(p[0], p[1], p[2]);
        }
    }
}

void gif_close(gif_t *g) {
    if (!g) return;
    kfree(g->canvas); kfree(g->prev); kfree(g);
}
