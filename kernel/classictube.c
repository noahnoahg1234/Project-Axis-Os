/*
 * ClassicTube - a YouTube-style video app for AxisOS, entirely in text.
 *
 * Every picture is converted to "multi-coloured text": each character cell is
 * an upper-half-block (0xDF) with the top pixel as the foreground colour and
 * the bottom pixel as the background colour, so one cell shows two coloured
 * pixels and the screen becomes a 16-colour image. The browsable video list
 * shows names; the player and the vertical Shorts feed convert each frame of
 * the (procedurally generated) videos into colour text in real time.
 */
#include "axis.h"

/* integer sine: a(0..255)=full circle -> -127..127 (parabola approximation) */
static int isin(int a) {
    a &= 255;
    int neg = a >= 128;
    int x = a & 127;            /* 0..127 = 0..180 deg */
    int v = (x * (128 - x)) >> 5; /* peak 128 at x=64 */
    v -= v >> 3;               /* ~ *0.875 so peak ~112, keep < 127 */
    return neg ? -v : v;
}
static int icos(int a) { return isin(a + 64); }
static int iabs(int v) { return v < 0 ? -v : v; }

static uint32_t cseed = 0x1234abcd;
static uint32_t crnd(void) { cseed ^= cseed << 13; cseed ^= cseed >> 17; cseed ^= cseed << 5; return cseed; }

/* ---- the "converter": pixel buffer (VGA colour indices) -> colour text --- */
#define PW 72
#define PH 44
static uint8_t pix[PH * PW];
static void put_px(int x, int y, uint8_t c) { if (x >= 0 && y >= 0 && x < PW && y < PH) pix[y * PW + x] = c; }

/* draw the top (pw x ph) of pix as colour text at (ox,oy), ph rows -> ph/2 cells */
static void blit(int ox, int oy, int pw, int ph) {
    for (int cy = 0; cy * 2 + 1 < ph; cy++)
        for (int x = 0; x < pw; x++) {
            uint8_t top = pix[(cy * 2) * PW + x];
            uint8_t bot = pix[(cy * 2 + 1) * PW + x];
            scr_put(ox + x, oy + cy, (char)0xDF, ATTR(top & 15, bot & 15));
        }
}

/* ======================= video frame generators ======================== */
/* each fills pix[0..ph*pw] with colour indices for frame time t */

static void g_plasma(int t, int pw, int ph) {
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int v = isin(x * 8 + t * 2) + isin(y * 10 - t * 2) + isin((x + y) * 6 + t) + icos((x - y) * 7 - t * 3);
            static const uint8_t pal[8] = {C_BLUE, C_LBLUE, C_LCYAN, C_LGREEN, C_YELLOW, C_LRED, C_LMAGENTA, C_WHITE};
            put_px(x, y, pal[((v + 512) >> 7) & 7]);
        }
}

static void g_starfield(int t, int pw, int ph) {
    for (int i = 0; i < pw * ph; i++) pix[i] = C_BLACK;
    int cx = pw / 2, cy = ph / 2;
    for (int i = 0; i < 90; i++) {
        int ang = (i * 37) & 255;
        int z = 120 - ((t * 3 + i * 13) % 120);
        if (z < 1) z = 1;
        int r = 1400 / z;
        int x = cx + icos(ang) * r / 110;
        int y = cy + isin(ang) * r / 110;
        uint8_t c = z > 80 ? C_DGRAY : z > 50 ? C_LGRAY : z > 25 ? C_WHITE : C_LCYAN;
        put_px(x, y, c);
        if (z < 40) { put_px(x + 1, y, c); }
    }
}

static void g_fire(int t, int pw, int ph) {
    (void)t;
    static uint8_t fb[PH * PW];
    static int fw, fh, init;
    if (!init || fw != pw || fh != ph) { memset(fb, 0, sizeof fb); fw = pw; fh = ph; init = 1; }
    for (int x = 0; x < pw; x++) fb[(ph - 1) * pw + x] = (crnd() & 1) ? 36 : 0; /* hot source */
    for (int y = 0; y < ph - 1; y++)
        for (int x = 0; x < pw; x++) {
            int below = fb[(y + 1) * pw + x];
            int l = x > 0 ? fb[(y + 1) * pw + x - 1] : below;
            int r = x < pw - 1 ? fb[(y + 1) * pw + x + 1] : below;
            int v = (below + l + r + fb[(y + 1) * pw + x]) / 4;
            v -= (crnd() & 1);
            if (v < 0) v = 0;
            fb[y * pw + x] = (uint8_t)v;
        }
    static const uint8_t ramp[10] = {C_BLACK, C_RED, C_RED, C_LRED, C_LRED, C_BROWN, C_YELLOW, C_YELLOW, C_WHITE, C_WHITE};
    for (int i = 0; i < pw * ph; i++) { int v = fb[i]; if (v > 36) v = 36; put_px(i % pw, i / pw, ramp[v * 9 / 36]); }
}

static void g_matrix(int t, int pw, int ph) {
    static int head[PW], spd[PW], init, iw;
    if (!init || iw != pw) { for (int x = 0; x < pw; x++) { head[x] = crnd() % ph; spd[x] = 1 + (crnd() % 3); } init = 1; iw = pw; }
    for (int i = 0; i < pw * ph; i++) pix[i] = C_BLACK;
    for (int x = 0; x < pw; x++) {
        if ((t % spd[x]) == 0) head[x] = (head[x] + 1) % (ph + 12);
        for (int k = 0; k < 10; k++) {
            int y = head[x] - k;
            if (y < 0 || y >= ph) continue;
            put_px(x, y, k == 0 ? C_WHITE : k < 3 ? C_LGREEN : k < 6 ? C_GREEN : C_GREEN);
        }
    }
}

static void g_logo(int t, int pw, int ph) {
    static const uint8_t bg = C_BLACK;
    for (int i = 0; i < pw * ph; i++) pix[i] = bg;
    int lw = 14, lh = 6;
    int rx = pw - lw, ry = ph - lh;
    int x = iabs(((t * 2) % (2 * rx)) - rx);
    int y = iabs(((t * 1) % (2 * ry)) - ry);
    int col = ((t / rx) + (t / ry)) % 6;
    static const uint8_t cs[6] = {C_LRED, C_LGREEN, C_LCYAN, C_YELLOW, C_LMAGENTA, C_WHITE};
    for (int j = 0; j < lh; j++)
        for (int i = 0; i < lw; i++) {
            /* a blocky "play" triangle */
            int inside = (i > 2 && i < lw - 2) && (j > 0 && j < lh - 1) && ((i - 3) <= (j < lh / 2 ? j * 3 : (lh - 1 - j) * 3) + 3);
            put_px(x + i, y + j, inside ? cs[col] : C_DGRAY);
        }
}

static void g_tunnel(int t, int pw, int ph) {
    int cx = pw / 2, cy = ph / 2;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int dx = x - cx, dy = (y - cy) * 2;
            int d = dx * dx + dy * dy;
            int ring = ((d / 12) - t) & 7;
            int ang = (isin(dx * 8) + dy) & 7;
            static const uint8_t pal[8] = {C_BLUE, C_LBLUE, C_LCYAN, C_WHITE, C_LMAGENTA, C_MAGENTA, C_BLUE, C_LBLUE};
            put_px(x, y, pal[(ring ^ ang) & 7]);
        }
}

static void g_fireworks(int t, int pw, int ph) {
    for (int i = 0; i < pw * ph; i++) pix[i] = C_BLACK;
    for (int b = 0; b < 3; b++) {
        int phase = (t + b * 40) % 120;
        int bx = ((b * 53 + 20) % pw);
        int by = ph / 3 + (b * 7) % (ph / 2);
        if (phase < 40) { put_px(bx, ph - 1 - phase * (ph - by) / 40, C_WHITE); continue; }
        int age = phase - 40;
        static const uint8_t cols[3] = {C_LRED, C_LGREEN, C_YELLOW};
        for (int p = 0; p < 24; p++) {
            int a = (p * 256 / 24);
            int r = age;
            int x = bx + icos(a) * r / 90;
            int y = by + isin(a) * r / 180;
            uint8_t c = age > 50 ? C_DGRAY : cols[(b + p) % 3];
            put_px(x, y, c);
        }
    }
}

static void g_badapple(int t, int pw, int ph) {
    /* a morphing black silhouette on white - a nod to a famous text-video */
    for (int i = 0; i < pw * ph; i++) pix[i] = C_WHITE;
    int cx = pw / 2, cy = ph / 2;
    int rr = 8 + isin(t * 4) / 12;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int dx = (x - cx), dy = (y - cy);
            int wob = isin(x * 12 + t * 6) / 20;
            if (dx * dx / 2 + dy * dy < (rr + wob) * (rr + wob)) put_px(x, y, C_BLACK);
        }
    /* two spinning dots */
    int a = t * 5;
    put_px(cx + icos(a) * 16 / 110, cy + isin(a) * 10 / 110, C_DGRAY);
    put_px(cx - icos(a) * 16 / 110, cy - isin(a) * 10 / 110, C_DGRAY);
}

static void g_cat(int t, int pw, int ph) {
    for (int i = 0; i < pw * ph; i++) pix[i] = C_LCYAN;
    int cx = pw / 2, base = ph - 4;
    /* body */
    for (int y = base - 10; y < base; y++) for (int x = cx - 8; x < cx + 8; x++) put_px(x, y, C_BROWN);
    /* head */
    for (int y = base - 18; y < base - 9; y++) for (int x = cx - 6; x < cx + 6; x++) put_px(x, y, C_BROWN);
    /* ears */
    put_px(cx - 6, base - 19, C_BROWN); put_px(cx - 5, base - 20, C_BROWN);
    put_px(cx + 5, base - 19, C_BROWN); put_px(cx + 4, base - 20, C_BROWN);
    /* eyes blink */
    int blink = (t % 50) < 4;
    put_px(cx - 3, base - 15, blink ? C_BROWN : C_LGREEN);
    put_px(cx + 3, base - 15, blink ? C_BROWN : C_LGREEN);
    put_px(cx, base - 13, C_LRED); /* nose */
    /* tail wag */
    int tx = cx + 8 + icos(t * 8) * 6 / 110;
    for (int y = base - 8; y < base; y++) put_px(tx, y, C_BROWN);
}

static void g_bars(int t, int pw, int ph) {
    for (int i = 0; i < pw * ph; i++) pix[i] = C_BLACK;
    int nb = 16, bw = pw / nb;
    static const uint8_t cs[8] = {C_LGREEN, C_GREEN, C_YELLOW, C_BROWN, C_LRED, C_LMAGENTA, C_LCYAN, C_LBLUE};
    for (int b = 0; b < nb; b++) {
        int h = (isin(t * 6 + b * 40) + 127) * ph / 300 + 2;
        for (int y = ph - 1; y > ph - 1 - h && y >= 0; y--)
            for (int x = 0; x < bw - 1; x++) put_px(b * bw + x, y, cs[(ph - y) * 8 / ph % 8]);
    }
}

static void g_heart(int t, int pw, int ph) {
    for (int i = 0; i < pw * ph; i++) pix[i] = C_MAGENTA;
    int cx = pw / 2, cy = ph / 2;
    int beat = isin(t * 10) / 30; /* pulse */
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int dx = (x - cx) * 10 / (9 + beat), dy = (y - cy) * 10 / (9 + beat);
            /* heart implicit curve (scaled ints) */
            int a = dx * dx + dy * dy - 60;
            if (a * a * a - dx * dx * dy * dy * 3 < 0) put_px(x, y, C_LRED);
        }
}

/* ----------------------------------------------------------------------- */
typedef void (*genfn)(int t, int pw, int ph);
typedef struct { const char *title, *channel, *views; int dur; genfn gen; } vid_t;

static const vid_t videos[] = {
    {"Lava Lamp Plasma (4 hours)",   "ChillVibes",   "2.3M views", 240, g_plasma},
    {"Warp Speed Starfield",         "SpaceCadet",   "881K views",  60, g_starfield},
    {"Cozy Fireplace 10 hours",      "RelaxTV",      "14M views",  600, g_fire},
    {"Falling Code (Matrix)",        "Cyberdeck",    "609K views",  90, g_matrix},
    {"Bouncing Logo (will it hit?)", "RetroScreens", "41M views",  120, g_logo},
    {"Hyperspace Tunnel Loop",       "DemoScene",    "1.1M views",  80, g_tunnel},
    {"Fireworks Finale",             "NightSky",     "5.6M views",  45, g_fireworks},
    {"The Famous Silhouette Video",  "AnimeClassic", "99M views",   75, g_badapple},
    {"Cute Cat Does Nothing",        "CatCam",       "33M views",  180, g_cat},
};
#define NVID ((int)(sizeof videos / sizeof *videos))

static const vid_t shorts[] = {
    {"POV: your CPU at 100%",     "techmemes",  "4.1M", 6, g_bars},
    {"this plasma is so satisfy", "oddlysat",   "9.9M", 6, g_plasma},
    {"little guy says hi",        "catcam",     "12M",  6, g_cat},
    {"i love you (loop)",         "wholesome",  "7.7M", 6, g_heart},
    {"stars go brrr",             "spacecadet", "2.2M", 6, g_starfield},
    {"fire = cozy #shorts",       "relaxtv",    "18M",  6, g_fire},
    {"matrix but short",          "cyberdeck",  "3.3M", 6, g_matrix},
};
#define NSHORT ((int)(sizeof shorts / sizeof *shorts))

/* ---- player ------------------------------------------------------------ */
static void draw_progress(int y, int cur, int total, uint8_t fg) {
    int w = SCREEN_W - 14;
    int fill = total ? cur * w / total : 0;
    scr_printf(1, y, ATTR(C_WHITE, C_BLACK), "%d:%02d", cur / 60, cur % 60);
    for (int i = 0; i < w; i++) scr_put(6 + i, y, i <= fill ? (char)0xC4 : (char)0xC4, ATTR(i <= fill ? fg : C_DGRAY, C_BLACK));
    scr_put(6 + fill, y, (char)0x07, ATTR(C_LRED, C_BLACK));
    scr_printf(SCREEN_W - 6, y, ATTR(C_LGRAY, C_BLACK), "%d:%02d", total / 60, total % 60);
}

static void play_video(const vid_t *v) {
    int frame = 0;
    bool paused = false;
    uint32_t last = ticks;
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));
    int fps_div = 4; /* advance every 4 ticks (~25 fps cap) */
    int total_frames = v->dur * TICK_HZ / fps_div;
    for (;;) {
        if (!paused && ticks - last >= (uint32_t)fps_div) { last = ticks; frame++; if (frame > total_frames) frame = 0; }
        v->gen(frame, PW, 40);
        blit(4, 1, PW, 40);
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
        scr_printf(1, 0, ATTR(C_WHITE, C_LRED), " ClassicTube ");
        scr_putsn(15, 0, v->title, 44, ATTR(C_WHITE, C_LRED));
        scr_fill(0, 22, SCREEN_W, 1, ' ', ATTR(C_LGRAY, C_BLACK));
        scr_printf(1, 22, ATTR(C_LCYAN, C_BLACK), "%s  -  %s", v->channel, v->views);
        draw_progress(23, frame * fps_div / TICK_HZ, v->dur, C_LRED);
        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_puts(1, 24, paused ? "PAUSED  Space play  Left/Right seek  Q back" : "Space pause  Left/Right seek  Q back", ATTR(C_BLACK, C_LGRAY));
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == ' ') paused = !paused;
        else if (k == KEY_RIGHT) frame += TICK_HZ * 5 / fps_div;
        else if (k == KEY_LEFT) { frame -= TICK_HZ * 5 / fps_div; if (frame < 0) frame = 0; }
        if (k < 0) cpu_hlt();
    }
}

/* ---- Shorts (vertical feed) ------------------------------------------- */
static void shorts_feed(int start) {
    int idx = start, frame = 0, likes_bonus = 0;
    uint32_t last = ticks;
    int sw = 30, sh = 42;           /* portrait: 30 wide x 21 cells */
    int ox = (SCREEN_W - sw) / 2;
    for (;;) {
        if (ticks - last >= 4) { last = ticks; frame++; }
        shorts[idx].gen(frame, sw, sh);
        scr_fill(0, 0, SCREEN_W, SCREEN_H, (char)0xB1, ATTR(C_DGRAY, C_BLACK));
        blit(ox, 2, sw, sh);
        /* side rail (like/comment/share) */
        int rx = ox + sw + 2;
        scr_puts(rx, 12, "(^)", ATTR(C_LRED, C_BLACK));
        scr_printf(rx, 13, ATTR(C_WHITE, C_BLACK), "%s", shorts[idx].views);
        scr_puts(rx, 15, "[c]", ATTR(C_WHITE, C_BLACK));
        scr_puts(rx, 16, "1.2K", ATTR(C_LGRAY, C_BLACK));
        scr_puts(rx, 18, "<->", ATTR(C_WHITE, C_BLACK));
        scr_puts(rx, 19, "Share", ATTR(C_LGRAY, C_BLACK));
        if (likes_bonus) scr_puts(rx, 11, "Liked!", ATTR(C_LGREEN, C_BLACK));
        /* title + channel over the bottom */
        scr_fill(ox, 22, sw, 2, ' ', ATTR(C_WHITE, C_BLACK));
        scr_printf(ox, 22, ATTR(C_LCYAN, C_BLACK), "@%s", shorts[idx].channel);
        scr_putsn(ox, 23, shorts[idx].title, sw, ATTR(C_WHITE, C_BLACK));
        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_printf(1, 24, ATTR(C_BLACK, C_LGRAY), "Short %d/%d   Up/Down next   L like   Q back", idx + 1, NSHORT);
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_DOWN || k == KEY_RIGHT || k == ' ') { idx = (idx + 1) % NSHORT; frame = 0; likes_bonus = 0; }
        else if (k == KEY_UP || k == KEY_LEFT) { idx = (idx + NSHORT - 1) % NSHORT; frame = 0; likes_bonus = 0; }
        else if (k == 'l' || k == 'L') likes_bonus = 1;
        if (k < 0) cpu_hlt();
    }
}

/* ---- home -------------------------------------------------------------- */
int classictube_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    int sel = 0, top = 0;
    char filter[32] = "";
    int list[NVID], nl;
    uint32_t preview_t = 0, last = ticks;
    for (;;) {
        nl = 0;
        for (int i = 0; i < NVID; i++) if (!filter[0] || strcasestr(videos[i].title, filter) || strcasestr(videos[i].channel, filter)) list[nl++] = i;
        if (sel >= nl) sel = nl ? nl - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + 15) top = sel - 14;

        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
        scr_puts(1, 0, "\xDB\xDB ClassicTube", ATTR(C_WHITE, C_LRED));
        if (filter[0]) scr_printf(20, 0, ATTR(C_WHITE, C_LRED), "search: %s", filter);
        else scr_puts(SCREEN_W - 26, 0, "[ / search ] [ S Shorts ]", ATTR(C_WHITE, C_LRED));

        /* left: video list */
        for (int r = 0; r < 15 && top + r < nl; r++) {
            int i = list[top + r], y = 2 + r;
            uint8_t a = (top + r == sel) ? ATTR(C_BLACK, C_LGRAY) : ATTR(C_WHITE, C_BLACK);
            scr_fill(0, y, 40, 1, ' ', a);
            scr_put(1, y, (char)0x10, ATTR(C_LRED, a >> 4));
            scr_printf(3, y, a, "%-30.30s", videos[i].title);
        }
        if (nl == 0) scr_puts(2, 3, "No videos match your search.", ATTR(C_DGRAY, C_BLACK));

        /* right: live animated preview of the selected video */
        if (nl) {
            const vid_t *v = &videos[list[sel]];
            int px0 = 42, py0 = 3, ppw = 36, pph = 32;
            if (ticks - last >= 4) { last = ticks; preview_t++; }
            v->gen(preview_t, ppw, pph);
            scr_box(px0 - 1, py0 - 1, ppw + 2, pph / 2 + 2, ATTR(C_LGRAY, C_BLACK), " now playing ");
            blit(px0, py0, ppw, pph);
            scr_putsn(px0, py0 + pph / 2 + 1, v->title, ppw, ATTR(C_WHITE, C_BLACK));
            scr_printf(px0, py0 + pph / 2 + 2, ATTR(C_LCYAN, C_BLACK), "%s", v->channel);
            scr_printf(px0, py0 + pph / 2 + 3, ATTR(C_DGRAY, C_BLACK), "%s  -  %d:%02d", v->views, v->dur / 60, v->dur % 60);
            scr_puts(px0, py0 + pph / 2 + 5, "Press Enter to watch", ATTR(C_LGREEN, C_BLACK));
        }

        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_puts(1, 24, "Up/Down browse   Enter watch   S Shorts   / search   Q quit", ATTR(C_BLACK, C_LGRAY));

        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel + 1 < nl) sel++;
        else if (k == '\n' && nl) { play_video(&videos[list[sel]]); }
        else if (k == 's' || k == 'S') { shorts_feed(0); }
        else if (k == '/') {
            int tui_prompt(const char *, const char *, char *, size_t);
            tui_prompt("Search ClassicTube", "Find videos:", filter, sizeof filter);
            con_show_cursor(false);
            sel = top = 0;
        }
        if (k < 0) cpu_hlt();
    }
    con_show_cursor(true);
    scr_restore(saved);
    return 0;
}
