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

/* ================= real YouTube (YouTube Data API over native HTTPS) === */
#define RT_MAX 20
static char rt_title[RT_MAX][72], rt_chan[RT_MAX][40], rt_id[RT_MAX][16], rt_views[RT_MAX][16];
static int rt_dur[RT_MAX], rt_n;
static bool online;
static uint8_t thumb_cache[PH * PW];
static char thumb_id[16];
static int thumb_w, thumb_h, thumb_ok;

static void fmt_views(const char *n, char *out) {
    long v = strtol(n, NULL, 10);
    if (v >= 1000000000) snprintf(out, 16, "%ld.%ldB views", v / 1000000000, (v / 100000000) % 10);
    else if (v >= 1000000) snprintf(out, 16, "%ld.%ldM views", v / 1000000, (v / 100000) % 10);
    else if (v >= 1000) snprintf(out, 16, "%ldK views", v / 1000);
    else snprintf(out, 16, "%ld views", v);
}
static int parse_dur(const char *s) { /* ISO 8601 PT#M#S / PT#H#M#S -> seconds */
    int sec = 0, num = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') num = num * 10 + (*s - '0');
        else if (*s == 'H') { sec += num * 3600; num = 0; }
        else if (*s == 'M') { sec += num * 60; num = 0; }
        else if (*s == 'S') { sec += num; num = 0; }
    }
    return sec;
}

/* parse a videos.list (mostPopular) or search response into rt_* */
static int parse_items(const char *json, uint32_t len, bool is_search) {
    const char *p = json, *end = json + len;
    rt_n = 0;
    while (rt_n < RT_MAX) {
        char idbuf[32];
        /* id: search uses "videoId", videos.list uses "id" */
        if (!json_field(&p, end, is_search ? "videoId" : "id", idbuf, sizeof idbuf)) break;
        strlcpy(rt_id[rt_n], idbuf, sizeof rt_id[0]);
        const char *sp = p;
        if (!json_field(&sp, end, "title", rt_title[rt_n], sizeof rt_title[0])) break;
        const char *cp = sp;
        if (!json_field(&cp, end, "channelTitle", rt_chan[rt_n], sizeof rt_chan[0])) rt_chan[rt_n][0] = 0;
        rt_views[rt_n][0] = 0;
        rt_dur[rt_n] = 0;
        if (!is_search) {
            const char *vp = cp;
            char vc[20];
            if (json_field(&vp, end, "viewCount", vc, sizeof vc)) fmt_views(vc, rt_views[rt_n]);
            const char *dp = cp;
            char du[20];
            if (json_field(&dp, end, "duration", du, sizeof du)) rt_dur[rt_n] = parse_dur(du);
        }
        p = cp;
        rt_n++;
    }
    return rt_n;
}

static void status_line(const char *msg) {
    scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    scr_puts(1, 24, msg, ATTR(C_BLACK, C_LGRAY));
}

static bool load_trending(void) {
    http_resp_t r;
    status_line("Loading trending from YouTube...");
    int st = gapi_youtube("videos",
        "part=snippet,statistics,contentDetails&chart=mostPopular&maxResults=18&regionCode=US", &r);
    if (st != 0) return false;
    int n = parse_items(r.body, r.len, false);
    kfree(r.body);
    return n > 0;
}
static bool load_search(const char *q) {
    char enc[128], params[220];
    size_t o = 0;
    for (const char *s = q; *s && o + 4 < sizeof enc; s++) {
        unsigned char c = *s;
        if (isalnum(c)) enc[o++] = c;
        else if (c == ' ') enc[o++] = '+';
        else { snprintf(enc + o, 4, "%%%02X", c); o += 3; }
    }
    enc[o] = 0;
    snprintf(params, sizeof params, "part=snippet&type=video&maxResults=18&q=%s", enc);
    http_resp_t r;
    status_line("Searching YouTube...");
    int st = gapi_youtube("search", params, &r);
    if (st != 0) return false;
    int n = parse_items(r.body, r.len, true);
    kfree(r.body);
    return n > 0;
}

/* real Shorts: short-duration videos, most-viewed first */
static bool load_shorts(void) {
    http_resp_t r;
    status_line("Loading Shorts from YouTube...");
    int st = gapi_youtube("search",
        "part=snippet&type=video&videoDuration=short&order=viewCount&maxResults=18&q=%23shorts", &r);
    if (st != 0) return false;
    int n = parse_items(r.body, r.len, true);
    kfree(r.body);
    return n > 0;
}

/* fetch + cache the thumbnail for rt_id[idx] at (w x h) colour indices */
static bool get_thumb(int idx, int w, int h) {
    if (thumb_ok && !strcmp(thumb_id, rt_id[idx]) && thumb_w == w && thumb_h == h) return true;
    status_line("Loading thumbnail...");
    int st = gapi_thumb(rt_id[idx], w, h, thumb_cache);
    if (st != 0) { thumb_ok = 0; return false; }
    strlcpy(thumb_id, rt_id[idx], sizeof thumb_id);
    thumb_w = w;
    thumb_h = h;
    thumb_ok = 1;
    return true;
}
/* copy the cached thumbnail into pix for blit */
static void thumb_to_pix(int w, int h) {
    for (int i = 0; i < w * h && i < PH * PW; i++) pix[i] = thumb_cache[i];
}

static void draw_progress(int y, int cur, int total, uint8_t fg);

/* ---- real video frames via YouTube storyboards ------------------------ */
/* A storyboard is a set of JPEG mosaics: a grid of small frames sampled across
 * the whole video. We fetch them from Google, decode with the JPEG decoder,
 * and play the tiles in sequence - the actual video, as a fast timelapse in
 * colour text. (The H.264/VP9 stream itself can't be decoded in text mode.) */

/* build a storyboard image URL from the base template (substituting $L level
 * and $M/$N image index) plus the level's sigh token. */
static void sb_url(char *out, size_t n, const char *base, int level, int img, const char *sigh) {
    size_t o = 0;
    for (const char *p = base; *p && o + 1 < n; ) {
        if (p[0] == '$' && (p[1] == 'L' || p[1] == 'M' || p[1] == 'N')) {
            int v = p[1] == 'L' ? level : img;
            char t[12]; int tn = 0; if (v == 0) t[tn++] = '0';
            while (v > 0) { t[tn++] = '0' + v % 10; v /= 10; }
            while (tn-- > 0 && o + 1 < n) out[o++] = t[tn];
            p += 2;
        } else out[o++] = *p++;
    }
    if (sigh && *sigh) { const char *s = "&sigh="; while (*s && o + 1 < n) out[o++] = *s++; while (*sigh && o + 1 < n) out[o++] = *sigh++; }
    out[o] = 0;
}

/* returns true if it played a storyboard (even briefly); false = unavailable */
static bool play_storyboard(const char *video_id, const char *title) {
    static char spec[1200];
    status_line("Fetching real video frames from Google...");
    if (gapi_storyboard(video_id, spec, sizeof spec) != 0) return false;

    /* tokenize spec on '|': [0]=base URL, [1..]=level descriptors */
    char *tok[16]; int ntok = 0;
    for (char *p = spec; *p && ntok < 16; ) {
        tok[ntok++] = p;
        char *bar = strchr(p, '|');
        if (!bar) break;
        *bar = 0; p = bar + 1;
    }
    if (ntok < 2) return false;
    const char *base = tok[0];
    int level = ntok - 2;                       /* highest available level */
    char *desc = tok[ntok - 1];
    /* level descriptor: w#h#count#cols#rows#interval#name#sigh */
    int f[6] = {0,0,0,0,0,0}; char sigh[64] = "";
    { char *s = desc; int fi = 0;
      while (fi < 6) { f[fi++] = atoi(s); char *h = strchr(s, '#'); if (!h) { s = NULL; break; } s = h + 1; }
      /* sigh is the last '#' field */
      char *last = strrchr(desc, '#'); if (last) strlcpy(sigh, last + 1, sizeof sigh);
    }
    int cols = f[3], rows = f[4], count = f[2];
    if (cols < 1 || rows < 1 || count < 1) return false;
    int per = cols * rows, nimg = (count + per - 1) / per;

    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));
    int pw = PW, ph = 40;
    bool paused = false, played = false;
    uint32_t next = ticks;
    int frame = 0;
    for (;;) {                                  /* loop the whole storyboard */
        for (int m = 0; m < nimg; m++) {
            char url[700];
            sb_url(url, sizeof url, base, level, m, sigh);
            http_resp_t r;
            int st = http_get(url, &r, NULL);
            if (st != 200 || !r.body) { if (st >= 0 && r.body) kfree(r.body); if (played) { frame = 0; continue; } return false; }
            int W, H; uint8_t *rgb;
            int rc = jpeg_decode((const uint8_t *)r.body, r.len, &W, &H, &rgb);
            kfree(r.body);
            if (rc != 0) { if (played) continue; return false; }
            int tw = W / cols, th = H / rows;
            for (int t = 0; t < per && frame < count; t++, frame++) {
                int tx = (t % cols) * tw, ty = (t / cols) * th;
                while (paused) {
                    int k = kbd_trygetkey();
                    if (k == ' ') paused = false;
                    if (k == 'q' || k == 'Q' || k == KEY_ESC) { kfree(rgb); return true; }
                    cpu_hlt();
                }
                /* sample the tile sub-rectangle into pix */
                for (int oy = 0; oy < ph; oy++) {
                    int sy = ty + oy * th / ph;
                    for (int ox = 0; ox < pw; ox++) {
                        int sx = tx + ox * tw / pw;
                        uint8_t *p = rgb + ((uint32_t)sy * W + sx) * 3;
                        pix[oy * pw + ox] = img_vga_nearest(p[0], p[1], p[2]);
                    }
                }
                blit(4, 1, pw, ph);
                played = true;
                scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
                scr_puts(1, 0, " ClassicTube  REAL VIDEO (storyboard)", ATTR(C_WHITE, C_LRED));
                scr_fill(0, 22, SCREEN_W, 1, ' ', ATTR(C_LGRAY, C_BLACK));
                scr_putsn(1, 22, title, SCREEN_W - 2, ATTR(C_WHITE, C_BLACK));
                draw_progress(23, frame, count, C_LRED);
                scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
                scr_puts(1, 24, "Real frames from YouTube  -  Space pause  Q back", ATTR(C_BLACK, C_LGRAY));
                next = ticks + 8;               /* ~12 fps timelapse */
                while ((int32_t)(ticks - next) < 0) {
                    int k = kbd_trygetkey();
                    if (k == ' ') { paused = true; break; }
                    if (k == 'q' || k == 'Q' || k == KEY_ESC) { kfree(rgb); return true; }
                    cpu_hlt();
                }
                if (user_abort()) { kfree(rgb); return true; }
            }
            kfree(rgb);
        }
        frame = 0;                              /* loop */
    }
}

/* play a real video: storyboard frames if available, else the thumbnail */
static void play_real(int idx) {
    if (play_storyboard(rt_id[idx], rt_title[idx])) return;
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));
    int pw = PW, ph = 40;
    bool have = get_thumb(idx, pw, ph);
    uint32_t t0 = ticks;
    int marquee = 0;
    for (;;) {
        if (have) { thumb_to_pix(pw, ph);
            /* subtle "playing" shimmer on the border */
            int b = (ticks / 4) % pw;
            for (int x = 0; x < pw; x++) { pix[x] = (x == b) ? C_WHITE : pix[x]; pix[(ph - 1) * pw + x] = (x == (pw - 1 - b)) ? C_WHITE : pix[(ph - 1) * pw + x]; }
            blit(4, 1, pw, ph);
        } else {
            scr_fill(4, 1, pw, ph / 2, (char)0xB0, ATTR(C_DGRAY, C_BLACK));
            scr_puts(24, 10, "(thumbnail unavailable - check the connection)", ATTR(C_LGRAY, C_BLACK));
        }
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
        scr_puts(1, 0, " ClassicTube  LIVE", ATTR(C_WHITE, C_LRED));
        /* scrolling title */
        int tl = (int)strlen(rt_title[idx]);
        char line[120];
        if (tl > 44) { for (int i = 0; i < 44; i++) line[i] = rt_title[idx][(marquee + i) % tl]; line[44] = 0; if ((ticks / 8) != (uint32_t)marquee) marquee = (ticks / 8) % tl; }
        else strlcpy(line, rt_title[idx], sizeof line);
        scr_putsn(21, 0, line, 44, ATTR(C_WHITE, C_LRED));
        scr_fill(0, 22, SCREEN_W, 3, ' ', ATTR(C_LGRAY, C_BLACK));
        scr_printf(1, 22, ATTR(C_LCYAN, C_BLACK), "%s   %s", rt_chan[idx], rt_views[idx][0] ? rt_views[idx] : "");
        scr_printf(1, 23, ATTR(C_DGRAY, C_BLACK), "youtu.be/%s   %u s watched", rt_id[idx], (ticks - t0) / TICK_HZ);
        status_line("Real YouTube video - text mode can't decode the stream, so this is the live thumbnail. Q back");
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        if (k < 0) cpu_hlt();
    }
}

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

/* ---- real video playback (animated GIF, decoded to colour text) -------- */
static void gif_error_screen(const char *msg) {
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_LGRAY, C_BLACK));
    scr_puts(4, 11, msg, ATTR(C_LRED, C_BLACK));
    status_line("Q back");
    for (;;) { int k = kbd_getkey(); if (k == 'q' || k == 'Q' || k == KEY_ESC) break; }
}
static void play_gif(const uint8_t *data, uint32_t len, const char *title) {
    gif_t *g = gif_open(data, len);
    if (!g) { gif_error_screen("Could not decode that video (needs a valid animated GIF)."); return; }
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));
    int pw = PW, ph = 40;
    int delay = 10, have = gif_next_frame(g, &delay), fcount = 0;
    uint32_t next = ticks;
    bool paused = false;
    for (;;) {
        if (!paused && ticks >= next) {
            if (!have) { gif_reset(g); fcount = 0; have = gif_next_frame(g, &delay); }
            if (have) {
                gif_sample(g, pix, pw, ph);
                blit(4, 1, pw, ph);
                int dl = delay * TICK_HZ / 100; if (dl < 2) dl = 2;
                next = ticks + dl; fcount++;
                have = gif_next_frame(g, &delay);
            }
        }
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
        scr_puts(1, 0, " ClassicTube  VIDEO", ATTR(C_WHITE, C_LRED));
        scr_putsn(21, 0, title, 44, ATTR(C_WHITE, C_LRED));
        scr_fill(0, 22, SCREEN_W, 1, ' ', ATTR(C_LGRAY, C_BLACK));
        scr_printf(1, 22, ATTR(C_LCYAN, C_BLACK), "%dx%d video  -  frame %d", gif_width(g), gif_height(g), fcount);
        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_puts(1, 24, paused ? "PAUSED  Space play  R restart  Q back" : "Playing  Space pause  R restart  Q back", ATTR(C_BLACK, C_LGRAY));
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == ' ') paused = !paused;
        else if (k == 'r' || k == 'R') { gif_reset(g); fcount = 0; have = gif_next_frame(g, &delay); next = ticks; }
        if (k < 0) cpu_hlt();
    }
    gif_close(g);
}
/* MJPEG (and single JPEG): concatenated JPEG frames, decoded to colour text */
static void play_mjpeg(const uint8_t *data, uint32_t len, const char *title) {
    if (jpeg_frame_len(data, len) == 0) { gif_error_screen("Not a valid JPEG / MJPEG video."); return; }
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));
    int pw = PW, ph = 40, fcount = 0, vw = 0, vh = 0, fps = 12;
    uint32_t pos = 0, next = ticks;
    bool paused = false;
    for (;;) {
        if (!paused && ticks >= next) {
            uint32_t fl = pos < len ? jpeg_frame_len(data + pos, len - pos) : 0;
            if (fl == 0) { pos = 0; fcount = 0; fl = jpeg_frame_len(data, len); }  /* loop */
            if (fl) {
                int w, h; uint8_t *rgb;
                if (jpeg_decode(data + pos, fl, &w, &h, &rgb) == 0) {
                    vw = w; vh = h;
                    img_rgb_to_vga(rgb, w, h, pix, pw, ph);
                    blit(4, 1, pw, ph);
                    kfree(rgb);
                }
                pos += fl; fcount++;
            }
            next = ticks + (TICK_HZ / fps > 0 ? TICK_HZ / fps : 1);
        }
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
        scr_puts(1, 0, " ClassicTube  VIDEO", ATTR(C_WHITE, C_LRED));
        scr_putsn(21, 0, title, 44, ATTR(C_WHITE, C_LRED));
        scr_fill(0, 22, SCREEN_W, 1, ' ', ATTR(C_LGRAY, C_BLACK));
        scr_printf(1, 22, ATTR(C_LCYAN, C_BLACK), "%dx%d MJPEG  -  frame %d", vw, vh, fcount);
        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_puts(1, 24, paused ? "PAUSED  Space play  R restart  Q back" : "Playing  Space pause  R restart  Q back", ATTR(C_BLACK, C_LGRAY));
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == ' ') paused = !paused;
        else if (k == 'r' || k == 'R') { pos = 0; fcount = 0; next = ticks; }
        if (k < 0) cpu_hlt();
    }
}
/* pick the player by content signature */
static void play_media(const uint8_t *data, uint32_t len, const char *title) {
    if (len >= 4 && !memcmp(data, "GIF8", 4)) play_gif(data, len, title);
    else if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) play_mjpeg(data, len, title);
    else gif_error_screen("Unsupported video format (use GIF or MJPEG).");
}
/* load a video from an http(s):// URL or a local file path, then play it */
static void load_and_play_video(const char *src) {
    if (!strncasecmp(src, "http://", 7) || !strncasecmp(src, "https://", 8)) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_BLACK, C_BLACK));
        status_line("Loading video over the network...");
        http_resp_t r;
        int st = http_get(src, &r, NULL);
        if (st < 0 || !r.body) { gif_error_screen("Could not download the video."); return; }
        play_media((const uint8_t *)r.body, r.len, src);
        kfree(r.body);
    } else {
        int id = fs_resolve(src, fs_cwd);
        if (id < 0 || fs_nodes[id].type != FT_FILE || !fs_nodes[id].data) { gif_error_screen("File not found."); return; }
        play_media(fs_nodes[id].data, fs_nodes[id].size, src);
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

/* real (online) Shorts: vertical thumbnail feed from a "#shorts" search */
static void real_shorts(void) {
    if (!load_shorts()) return;
    int idx = 0, sw = 30, sh = 42, ox = (SCREEN_W - sw) / 2, liked = 0;
    thumb_ok = 0;
    for (;;) {
        bool have = get_thumb(idx, sw, sh);
        scr_fill(0, 0, SCREEN_W, SCREEN_H, (char)0xB1, ATTR(C_DGRAY, C_BLACK));
        if (have) { thumb_to_pix(sw, sh); blit(ox, 2, sw, sh); }
        else scr_fill(ox, 2, sw, sh / 2, (char)0xB0, ATTR(C_DGRAY, C_BLACK));
        int rx = ox + sw + 2;
        scr_puts(rx, 12, "(^)", ATTR(C_LRED, C_BLACK));
        scr_puts(rx, 13, liked ? "Liked" : "Like", ATTR(C_WHITE, C_BLACK));
        scr_puts(rx, 15, "<->", ATTR(C_WHITE, C_BLACK));
        scr_puts(rx, 16, "Share", ATTR(C_LGRAY, C_BLACK));
        scr_fill(ox, 22, sw, 2, ' ', ATTR(C_WHITE, C_BLACK));
        scr_printf(ox, 22, ATTR(C_LCYAN, C_BLACK), "%s", rt_chan[idx]);
        scr_putsn(ox, 23, rt_title[idx], sw, ATTR(C_WHITE, C_BLACK));
        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_printf(1, 24, ATTR(C_BLACK, C_LGRAY), "Short %d/%d  Up/Down next  Enter play  L like  Q back", idx + 1, rt_n);
        int k = kbd_getkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_DOWN || k == ' ') { idx = (idx + 1) % rt_n; liked = 0; }
        else if (k == KEY_UP) { idx = (idx + rt_n - 1) % rt_n; liked = 0; }
        else if (k == '\n') { if (!play_storyboard(rt_id[idx], rt_title[idx])) play_real(idx); thumb_ok = 0; }
        else if (k == 'l' || k == 'L') liked = 1;
    }
}

/* ---- home -------------------------------------------------------------- */
int classictube_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    int sel = 0, top = 0;
    char filter[40] = "";
    uint32_t preview_t = 0, last = ticks;
    thumb_ok = 0;
    online = false;
    char ytkey[64];
    bool have_key = gapi_key("youtube", ytkey, sizeof ytkey);
    if (net_up() && have_key) {              /* real YouTube over native HTTPS */
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
        scr_puts(28, 11, "Connecting to YouTube...", ATTR(C_LRED, C_BLACK));
        online = load_trending();
    }
    for (;;) {
        int count = online ? rt_n : NVID;
        if (sel >= count) sel = count ? count - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + 15) top = sel - 14;

        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_LRED));
        scr_puts(1, 0, "\xDB\xDB ClassicTube", ATTR(C_WHITE, C_LRED));
        scr_puts(16, 0, online ? "* LIVE YouTube" : "* offline demo", ATTR(C_YELLOW, C_LRED));
        scr_puts(SCREEN_W - 26, 0, "[ / search ] [ S Shorts ]", ATTR(C_WHITE, C_LRED));
        if (!online)
            scr_puts(2, 23, have_key ? "No connection yet - showing the offline demo. Press R to retry YouTube."
                                     : gapi_default_disabled()
                                       ? "The free shared key is used up - add your own in Settings > YouTube API key."
                                       : "For real YouTube: set your API key in Settings > YouTube API key.",
                     ATTR(C_LCYAN, C_BLACK));

        for (int r = 0; r < 15 && top + r < count; r++) {
            int i = top + r, y = 2 + r;
            uint8_t a = (i == sel) ? ATTR(C_BLACK, C_LGRAY) : ATTR(C_WHITE, C_BLACK);
            scr_fill(0, y, 40, 1, ' ', a);
            scr_put(1, y, (char)0x10, ATTR(C_LRED, a >> 4));
            scr_printf(3, y, a, "%-36.36s", online ? rt_title[i] : videos[i].title);
        }
        if (count == 0) scr_puts(2, 3, "No results.", ATTR(C_DGRAY, C_BLACK));

        if (count) {
            int px0 = 42, py0 = 3, ppw = 36, pph = 32;
            scr_box(px0 - 1, py0 - 1, ppw + 2, pph / 2 + 2, ATTR(C_LGRAY, C_BLACK), online ? " thumbnail " : " now playing ");
            if (online) {
                if (get_thumb(sel, ppw, pph)) { thumb_to_pix(ppw, pph); blit(px0, py0, ppw, pph); }
                else scr_puts(px0 + 2, py0 + 6, "(no thumbnail)", ATTR(C_DGRAY, C_BLACK));
                scr_putsn(px0, py0 + pph / 2 + 1, rt_title[sel], ppw, ATTR(C_WHITE, C_BLACK));
                scr_printf(px0, py0 + pph / 2 + 2, ATTR(C_LCYAN, C_BLACK), "%s", rt_chan[sel]);
                scr_printf(px0, py0 + pph / 2 + 3, ATTR(C_DGRAY, C_BLACK), "%s%s%d:%02d", rt_views[sel], rt_views[sel][0] ? "  " : "", rt_dur[sel] / 60, rt_dur[sel] % 60);
            } else {
                const vid_t *v = &videos[sel];
                if (ticks - last >= 4) { last = ticks; preview_t++; }
                v->gen(preview_t, ppw, pph);
                blit(px0, py0, ppw, pph);
                scr_putsn(px0, py0 + pph / 2 + 1, v->title, ppw, ATTR(C_WHITE, C_BLACK));
                scr_printf(px0, py0 + pph / 2 + 2, ATTR(C_LCYAN, C_BLACK), "%s", v->channel);
                scr_printf(px0, py0 + pph / 2 + 3, ATTR(C_DGRAY, C_BLACK), "%s  -  %d:%02d", v->views, v->dur / 60, v->dur % 60);
            }
            scr_puts(px0, py0 + pph / 2 + 5, "Press Enter to watch", ATTR(C_LGREEN, C_BLACK));
        }

        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_puts(1, 24, online ? "Up/Dn browse  Enter watch  V play video  S Shorts  / search  R trending  Q quit"
                                : "Up/Dn browse  Enter watch  V play video  S Shorts  R retry live  Q quit",
                 ATTR(C_BLACK, C_LGRAY));

        int k = online ? kbd_getkey() : kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel + 1 < count) sel++;
        else if (k == '\n' && count) { if (online) play_real(sel); else play_video(&videos[sel]); thumb_ok = 0; }
        else if (k == 's' || k == 'S') { if (online) real_shorts(); else shorts_feed(0); sel = top = 0; thumb_ok = 0; }
        else if (k == 'v' || k == 'V') {
            int tui_prompt(const char *, const char *, char *, size_t);
            char src[256];
            strlcpy(src, "/usr/share/classictube/sample.gif", sizeof src);
            if (tui_prompt("Play video", "GIF file path, or http(s):// URL:", src, sizeof src) && src[0]) {
                con_show_cursor(false);
                load_and_play_video(src);
                thumb_ok = 0;
            }
            con_show_cursor(false);
        }
        else if (k == 'r' || k == 'R') {
            if (!online && net_up() && have_key) {   /* retry going live */
                scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
                scr_puts(28, 11, "Reconnecting to YouTube...", ATTR(C_LRED, C_BLACK));
                online = load_trending();
            } else if (online && load_trending()) { /* refresh trending */ }
            sel = top = 0; thumb_ok = 0;
        }
        else if (k == '/') {
            int tui_prompt(const char *, const char *, char *, size_t);
            if (tui_prompt(online ? "Search YouTube" : "Search", "Find videos:", filter, sizeof filter) && filter[0]) {
                con_show_cursor(false);
                if (online) { if (!load_search(filter)) load_trending(); }
                sel = top = 0;
                thumb_ok = 0;
            }
            con_show_cursor(false);
        }
        if (k < 0) cpu_hlt();
    }
    con_show_cursor(true);
    scr_restore(saved);
    return 0;
}
