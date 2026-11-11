/*
 * AxisPlayer - a small media player for AxisOS.
 *
 * Plays music through the PC speaker (built-in public-domain tunes, plus your
 * own .sng files from /Music) with a now-playing screen and an animated bar
 * visualizer. On real hardware (and QEMU with audio enabled) you hear it; the
 * visualizer always shows what's playing.
 *
 * .sng format: whitespace-separated notes like  C4 D4 E4:2 R G4
 *   note = letter A-G, optional # or b, octave 2-6; ":N" sets length (eighths)
 *   R = rest. First line beginning with ';' is the title.
 */
#include "axis.h"

/* ---- PC speaker --------------------------------------------------------- */
static void tone(int hz) {
    if (hz <= 0) { outb(0x61, inb(0x61) & 0xFC); return; }
    uint32_t d = 1193182u / (uint32_t)hz;
    outb(0x43, 0xB6);
    outb(0x42, d & 0xFF);
    outb(0x42, (d >> 8) & 0xFF);
    outb(0x61, inb(0x61) | 3);
}

/* base frequencies for octave 4 (C..B) */
static const int f4[12] = {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494};
static int note_freq(const char *s, int *dur) {
    *dur = 2; /* default quarter note = 2 eighths */
    if (*s == 'R' || *s == 'r') { const char *c = strchr(s, ':'); if (c) *dur = atoi(c + 1); return 0; }
    int n;
    switch (*s | 0x20) {
    case 'c': n = 0; break; case 'd': n = 2; break; case 'e': n = 4; break;
    case 'f': n = 5; break; case 'g': n = 7; break; case 'a': n = 9; break;
    case 'b': n = 11; break; default: return -1;
    }
    s++;
    if (*s == '#') { n++; s++; }
    else if (*s == 'b') { n--; s++; }
    int oct = 4;
    if (*s >= '2' && *s <= '6') { oct = *s - '0'; s++; }
    if (*s == ':') *dur = atoi(s + 1);
    if (*dur < 1) *dur = 1;
    int f = f4[(n + 12) % 12];
    int d = oct - 4;
    while (d > 0) { f *= 2; d--; }
    while (d < 0) { f /= 2; d++; }
    return f;
}

/* ---- built-in songs (traditional / public-domain melodies) ------------- */
typedef struct { const char *title; const char *notes; } song_t;
static const song_t builtin[] = {
    {"Ode to Joy (Beethoven)",
     "E4 E4 F4 G4 G4 F4 E4 D4 C4 C4 D4 E4 E4:3 D4 D4:4 "
     "E4 E4 F4 G4 G4 F4 E4 D4 C4 C4 D4 E4 D4:3 C4 C4:4"},
    {"Twinkle Twinkle",
     "C4 C4 G4 G4 A4 A4 G4:4 F4 F4 E4 E4 D4 D4 C4:4 "
     "G4 G4 F4 F4 E4 E4 D4:4 G4 G4 F4 F4 E4 E4 D4:4"},
    {"Mary Had a Little Lamb",
     "E4 D4 C4 D4 E4 E4 E4:4 D4 D4 D4:4 E4 G4 G4:4 "
     "E4 D4 C4 D4 E4 E4 E4 E4 D4 D4 E4 D4 C4:4"},
    {"Jingle Bells",
     "E4 E4 E4:4 E4 E4 E4:4 E4 G4 C4 D4 E4:4 "
     "F4 F4 F4 F4 F4 E4 E4 E4 G4 G4 F4 D4 C4:4"},
    {"Happy Scale",
     "C4 D4 E4 F4 G4 A4 B4 C5 C5 B4 A4 G4 F4 E4 D4 C4"},
    {"Axis Theme",
     "C4 E4 G4 C5 G4 E4 C4 R G4 B4 D5 G5 D5 B4 G4 R "
     "A4 C5 E5 A5 E5 C5 A4 R C5:4 G4:4 C4:4"},
};
#define NBUILTIN ((int)(sizeof builtin / sizeof *builtin))

/* ---- player ------------------------------------------------------------ */
#define NBARS 32
static int bar[NBARS];

static void draw_player(const char *title, const char *allnotes, int note_i, int total, bool playing, int tempo, const char **plist, int np, int sel) {
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
    scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LCYAN));
    scr_puts(2, 0, "AxisPlayer", ATTR(C_BLACK, C_LCYAN));
    scr_puts(SCREEN_W - 20, 0, "PC speaker music", ATTR(C_BLACK, C_LCYAN));
    /* playlist */
    scr_box(1, 2, 34, 20, ATTR(C_WHITE, C_BLUE), " Playlist ");
    for (int i = 0; i < np && i < 18; i++) {
        uint8_t a = i == sel ? ATTR(C_BLACK, C_LGRAY) : ATTR(C_WHITE, C_BLUE);
        scr_fill(2, 3 + i, 32, 1, ' ', a);
        scr_printf(3, 3 + i, a, "%c %.28s", i == sel ? (char)0x10 : ' ', plist[i]);
    }
    /* now playing */
    scr_box(37, 2, 41, 11, ATTR(C_WHITE, C_BLACK), " Now Playing ");
    scr_printf(39, 4, ATTR(C_LGREEN, C_BLACK), "%.36s", title ? title : "(nothing)");
    scr_printf(39, 5, ATTR(C_DGRAY, C_BLACK), "%s  note %d / %d", playing ? "> playing" : "|| paused", note_i, total);
    /* progress bar */
    int w = 36, fill = total ? note_i * w / total : 0;
    for (int i = 0; i < w; i++) scr_put(39 + i, 7, i < fill ? (char)0xDB : (char)0xB0, ATTR(i < fill ? C_LGREEN : C_DGRAY, C_BLACK));
    scr_printf(39, 9, ATTR(C_WHITE, C_BLACK), "Tempo: %d", tempo);
    /* visualizer */
    scr_box(37, 14, 41, 8, ATTR(C_WHITE, C_BLACK), " Visualizer ");
    for (int i = 0; i < NBARS; i++) {
        int h = bar[i];
        for (int y = 0; y < 6; y++) {
            bool on = (6 - y) <= h;
            scr_put(39 + i, 15 + y, on ? (char)0xDB : ' ', ATTR(h > 4 ? C_LRED : h > 2 ? C_YELLOW : C_LGREEN, C_BLACK));
        }
    }
    scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LCYAN));
    scr_puts(1, 24, "Up/Down pick  Enter play  Space pause  N/P next/prev  +/- tempo  Q quit", ATTR(C_BLACK, C_LCYAN));
    (void)allnotes;
}

int mediaplayer_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    /* gather playlist: built-ins plus .sng files in the Music folder */
    const char *titles[48];
    const char *notes[48];
    static char nbuf[16][1024];
    static char tbuf[16][48];
    int np = 0;
    for (int i = 0; i < NBUILTIN; i++) { titles[np] = builtin[i].title; notes[np] = builtin[i].notes; np++; }
    int md = fs_lookup(FS_ROOT, "Music");
    int userbuf = 0;
    if (md >= 0) {
        int kids[FS_MAX_NODES]; int k = fs_children(md, kids, FS_MAX_NODES, false);
        for (int i = 0; i < k && np < 48 && userbuf < 16; i++) {
            fs_node_t *f = &fs_nodes[kids[i]];
            if (f->type != FT_FILE || !f->data) continue;
            int l = strlen(f->name);
            if (l < 5 || strcasecmp(f->name + l - 4, ".sng")) continue;
            uint32_t sz = f->size < 1023 ? f->size : 1023;
            memcpy(nbuf[userbuf], f->data, sz); nbuf[userbuf][sz] = 0;
            strlcpy(tbuf[userbuf], f->name, 48);
            titles[np] = tbuf[userbuf]; notes[np] = nbuf[userbuf];
            np++; userbuf++;
        }
    }
    int sel = 0, cur = -1, note_i = 0, total = 0, tempo = 50;
    bool playing = false;
    uint32_t note_end = 0;
    char tok[16];
    const char *scan = NULL;
    for (int i = 0; i < NBARS; i++) bar[i] = (i * 7) % 5;
    for (;;) {
        /* note advance */
        if (playing && cur >= 0 && ticks >= note_end) {
            /* read next token from scan */
            while (scan && *scan && (*scan == ' ' || *scan == '\n' || *scan == '\r' || *scan == '\t')) scan++;
            if (!scan || !*scan) { tone(0); playing = false; }
            else {
                int o = 0; while (*scan && *scan != ' ' && *scan != '\n' && *scan != '\r' && *scan != '\t' && o < 15) tok[o++] = *scan++;
                tok[o] = 0;
                int dur; int hz = note_freq(tok, &dur);
                tone(hz > 0 ? hz : 0);
                note_end = ticks + (uint32_t)dur * tempo / 10;
                note_i++;
                /* visualizer reacts to pitch */
                for (int i = NBARS - 1; i > 0; i--) bar[i] = bar[i - 1];
                bar[0] = hz > 0 ? 1 + (hz / 90) % 6 : 0;
            }
        }
        /* idle visualizer decay */
        if (!playing) for (int i = 0; i < NBARS; i++) if (bar[i] > 0 && (ticks % 4 == 0)) bar[i]--;
        draw_player(cur >= 0 ? titles[cur] : NULL, cur >= 0 ? notes[cur] : NULL, note_i, total, playing, tempo, titles, np, sel);

        int k = kbd_trygetkey();
        if (k < 0) { cpu_hlt(); continue; }
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_UP) sel = (sel + np - 1) % np;
        else if (k == KEY_DOWN) sel = (sel + 1) % np;
        else if (k == '\n' || k == 'n' || k == 'N' || k == 'p' || k == 'P') {
            if (k == 'n' || k == 'N') sel = (sel + 1) % np;
            else if (k == 'p' || k == 'P') sel = (sel + np - 1) % np;
            cur = sel; scan = notes[cur]; note_i = 0; playing = true; note_end = 0;
            total = 1; for (const char *q = notes[cur]; *q; q++) if (*q == ' ') total++;
        }
        else if (k == ' ') { playing = !playing; if (!playing) tone(0); else note_end = 0; }
        else if (k == '+' || k == '=') { if (tempo > 10) tempo -= 5; }
        else if (k == '-' || k == '_') { if (tempo < 120) tempo += 5; }
    }
    tone(0);
    con_show_cursor(true);
    scr_restore(saved);
    return 0;
}
