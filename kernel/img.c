/*
 * AxisOS - shared image facility, usable by any app.
 *
 * Turns image bytes (JPEG or GIF) into RGB, quantises RGB to the 16 VGA text
 * colours, and provides a full-screen viewer that renders an image as
 * multi-colour half-block text. Loads from a file on disk or an http(s):// URL.
 *   view <file-or-url>      (shell command)
 * Also used by the file explorer and ClassicTube.
 */
#include "axis.h"

static const uint8_t vga[16][3] = {
    {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
    {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}
};
uint8_t img_vga_nearest(int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        int dr = r - vga[i][0], dg = g - vga[i][1], db = b - vga[i][2];
        int d = dr*dr + dg*dg + db*db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}
/* downscale RGB (W x H) into ow x oh VGA colour indices */
void img_rgb_to_vga(const uint8_t *rgb, int W, int H, uint8_t *out, int ow, int oh) {
    for (int y = 0; y < oh; y++) {
        int sy = H > 0 ? y * H / oh : 0;
        for (int x = 0; x < ow; x++) {
            int sx = W > 0 ? x * W / ow : 0;
            const uint8_t *p = rgb + ((uint32_t)sy * W + sx) * 3;
            out[y * ow + x] = img_vga_nearest(p[0], p[1], p[2]);
        }
    }
}
/* decode image bytes to RGB (JPEG, or the first frame of a GIF). 0 ok, rgb kmalloc'd */
int img_decode_rgb(const uint8_t *data, uint32_t len, int *w, int *h, uint8_t **rgb) {
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) return jpeg_decode(data, len, w, h, rgb);
    if (len >= 4 && !memcmp(data, "GIF8", 4)) {
        gif_t *g = gif_open(data, len);
        if (!g) return -1;
        int delay;
        if (!gif_next_frame(g, &delay)) { gif_close(g); return -1; }
        int W = gif_width(g), H = gif_height(g);
        uint8_t *out = kmalloc((uint32_t)W * H * 3);
        if (!out) { gif_close(g); return -1; }
        memcpy(out, gif_canvas(g), (uint32_t)W * H * 3);
        *w = W; *h = H; *rgb = out; gif_close(g);
        return 0;
    }
    return -1;
}

/* read a file or URL into a kmalloc'd buffer; returns length (0 on failure) */
uint8_t *img_load(const char *src, uint32_t *out_len) {
    if (!strncasecmp(src, "http://", 7) || !strncasecmp(src, "https://", 8)) {
        http_resp_t r;
        int st = http_get(src, &r, NULL);
        if (st < 0 || !r.body) return NULL;
        *out_len = r.len;
        return (uint8_t *)r.body;              /* caller frees */
    }
    int id = fs_resolve(src, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE || !fs_nodes[id].data) return NULL;
    uint8_t *buf = kmalloc(fs_nodes[id].size);
    if (!buf) return NULL;
    memcpy(buf, fs_nodes[id].data, fs_nodes[id].size);
    *out_len = fs_nodes[id].size;
    return buf;
}

/* ---- full-screen viewer / media player -------------------------------- */
#define VW 80
#define VH 46          /* 23 text rows x 2 half-pixels (row 23 = status) */
static uint8_t vbuf[VW * VH];

/* render one RGB frame into the half-block area, centred and aspect-fit */
static void render_frame(const uint8_t *rgb, int W, int H, const char *label, int extra) {
    int dw = VW, dh = VH;
    if (W * VH > H * VW) dh = H * VW / W; else dw = W * VH / H;
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    if (dh & 1) dh++; if (dh > VH) dh = VH;
    memset(vbuf, 0, sizeof vbuf);
    int ox = (VW - dw) / 2, oy = (VH - dh) / 2;
    uint8_t *tmp = kmalloc((uint32_t)dw * dh);
    if (tmp) {
        img_rgb_to_vga(rgb, W, H, tmp, dw, dh);
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) vbuf[(oy + y) * VW + (ox + x)] = tmp[y * dw + x];
        kfree(tmp);
    }
    for (int cy = 0; cy < VH / 2; cy++)
        for (int x = 0; x < VW; x++) {
            uint8_t top = vbuf[(cy*2) * VW + x], bot = vbuf[(cy*2+1) * VW + x];
            scr_put(x, cy, (char)0xDF, ATTR(top & 15, bot & 15));
        }
    scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    scr_printf(1, 24, ATTR(C_BLACK, C_LGRAY), "%s   %dx%d   %s   Q back", label, W, H, extra ? "Space pause" : "");
}
static void wait_quit(void) { for (;;) { int k = kbd_getkey(); if (k == 'q' || k == 'Q' || k == KEY_ESC) break; } }

void image_view(const char *src) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_LGRAY, C_BLACK));
    scr_puts(1, 12, "Loading...", ATTR(C_LGRAY, C_BLACK));

    uint32_t len = 0;
    uint8_t *data = img_load(src, &len);
    if (!data) goto fail;
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));

    if (len >= 4 && !memcmp(data, "GIF8", 4)) {          /* animated GIF */
        gif_t *g = gif_open(data, len);
        if (!g) goto fail;
        int W = gif_width(g), H = gif_height(g), delay = 10, have = gif_next_frame(g, &delay);
        int frames = 0; uint32_t next = ticks; bool paused = false;
        for (;;) {
            if (!paused && (int32_t)(ticks - next) >= 0) {
                if (!have) { gif_reset(g); have = gif_next_frame(g, &delay); }
                if (have) {
                    render_frame(gif_canvas(g), W, H, src, 1);
                    int dl = delay * TICK_HZ / 100; if (dl < 2) dl = 2; next = ticks + dl; frames++;
                    have = gif_next_frame(g, &delay);
                }
            }
            int k = kbd_trygetkey();
            if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
            if (k == ' ') paused = !paused;
            if (k < 0) cpu_hlt();
        }
        gif_close(g); kfree(data); con_show_cursor(true); scr_restore(saved); return;
    }
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) { /* JPEG, possibly MJPEG */
        uint32_t f0 = jpeg_frame_len(data, len);
        if (f0 == 0) goto fail;
        int animated = f0 < len && jpeg_frame_len(data + f0, len - f0) != 0;
        if (!animated) {                                 /* single still image */
            int W, H; uint8_t *rgb;
            if (jpeg_decode(data, len, &W, &H, &rgb) != 0) goto fail;
            render_frame(rgb, W, H, src, 0); kfree(rgb); kfree(data);
            wait_quit(); con_show_cursor(true); scr_restore(saved); return;
        }
        uint32_t pos = 0; int frames = 0; uint32_t next = ticks; bool paused = false;
        for (;;) {
            if (!paused && (int32_t)(ticks - next) >= 0) {
                uint32_t fl = pos < len ? jpeg_frame_len(data + pos, len - pos) : 0;
                if (fl == 0) { pos = 0; fl = jpeg_frame_len(data, len); }
                if (fl) {
                    int W, H; uint8_t *rgb;
                    if (jpeg_decode(data + pos, fl, &W, &H, &rgb) == 0) { render_frame(rgb, W, H, src, 1); kfree(rgb); }
                    pos += fl; frames++;
                }
                next = ticks + TICK_HZ / 12;
            }
            int k = kbd_trygetkey();
            if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
            if (k == ' ') paused = !paused;
            if (k < 0) cpu_hlt();
        }
        kfree(data); con_show_cursor(true); scr_restore(saved); return;
    }
fail:
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_LGRAY, C_BLACK));
    scr_puts(2, 11, "Could not open that media.", ATTR(C_LRED, C_BLACK));
    scr_puts(2, 12, "Supported: JPEG (.jpg), GIF (.gif), MJPEG (.mjpeg).", ATTR(C_DGRAY, C_BLACK));
    scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    scr_puts(1, 24, "Q back", ATTR(C_BLACK, C_LGRAY));
    kfree(data);
    wait_quit();
    con_show_cursor(true); scr_restore(saved);
}
