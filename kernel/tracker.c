/*
 * AxisOS - music tracker & editor (OPL2 FM).
 *
 * A classic pattern tracker: a grid of rows x 4 channels, notes typed on a
 * piano-style keyboard, instruments from the OPL2 synth, multiple patterns and
 * an order list, playback of a pattern or the whole song, and save/load of
 * songs to .trk files. Falls back to the PC speaker when no FM chip is present.
 *
 *   Keys (in the grid):
 *     Arrows        move cursor (Up/Down rows, Left/Right channels)
 *     piano keys    enter a note:
 *                     low octave  Z S X D C V G B H N J M  (C C# D .. B)
 *                     high octave Q 2 W 3 E R 5 T 6 Y 7 U, I (C of next)
 *     1             note OFF        Del/Bksp/.  clear cell
 *     [ ]           octave down/up  - =         tempo slower/faster
 *     F1 F2         instrument -/+  PgUp/PgDn    pattern -/+
 *     O / P         add current pattern to / remove from the order list
 *     Space         play / stop this pattern (loop)
 *     Enter         play the whole song (order list)
 *     F5 / F6       save / load a .trk song     F8  clear pattern
 *     Esc           quit
 */
#include "axis.h"

int tui_prompt(const char *title, const char *label, char *buf, size_t n);

#define TCH   4
#define TROWS 64
#define TPAT  8
#define ORDMAX 32
#define NINSTR 5

typedef struct { uint8_t note; uint8_t inst; } cell_t;   /* note: 0 empty, 1..96 pitch, 97 OFF */
static cell_t pat[TPAT][TROWS][TCH];
static uint8_t order[ORDMAX];
static int order_len;
static int speed;                 /* ticks per row */

static const int semi_f4[12] = {262,277,294,311,330,349,370,392,415,440,466,494};
static const char *semi_nm[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};

static int note_hz(int v) {
    if (v < 1 || v > 96) return 0;
    int idx = v - 1, oct = idx / 12, semi = idx % 12, f = semi_f4[semi], d = oct - 4;
    while (d-- > 0) f *= 2;
    while (++d < 0) f /= 2;
    return f;
}
/* write a 5-char cell label like "C-4 0" into buf */
static void cell_label(const cell_t *c, char *buf) {
    if (c->note == 0) { memcpy(buf, "--- .", 6); return; }
    if (c->note == 97) { memcpy(buf, "=== .", 6); return; }
    int idx = c->note - 1, oct = idx / 12, semi = idx % 12;
    buf[0] = semi_nm[semi][0]; buf[1] = semi_nm[semi][1];
    buf[2] = (char)('0' + (oct & 7)); buf[3] = ' ';
    buf[4] = (char)('0' + c->inst); buf[5] = 0;
}
/* map a key to a semitone offset + octave offset for note entry; -1 if none */
static int key_semi(int k, int *octoff) {
    *octoff = 0;
    switch (k) {
        case 'z': return 0;  case 's': return 1;  case 'x': return 2;  case 'd': return 3;
        case 'c': return 4;  case 'v': return 5;  case 'g': return 6;  case 'b': return 7;
        case 'h': return 8;  case 'n': return 9;  case 'j': return 10; case 'm': return 11;
        case 'q': *octoff = 1; return 0;  case '2': *octoff = 1; return 1;
        case 'w': *octoff = 1; return 2;  case '3': *octoff = 1; return 3;
        case 'e': *octoff = 1; return 4;  case 'r': *octoff = 1; return 5;
        case '5': *octoff = 1; return 6;  case 't': *octoff = 1; return 7;
        case '6': *octoff = 1; return 8;  case 'y': *octoff = 1; return 9;
        case '7': *octoff = 1; return 10; case 'u': *octoff = 1; return 11;
        case 'i': *octoff = 2; return 0;
    }
    return -1;
}

static void clear_pattern(int p) { memset(pat[p], 0, sizeof pat[p]); }
static void new_song(void) {
    memset(pat, 0, sizeof pat);
    order[0] = 0; order_len = 1; speed = 7;
}

/* ---- save / load (.trk text) ------------------------------------------ */
static void put_int(char **o, int v) {
    char t[12]; int n = 0; if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = '0' + v % 10; v /= 10; }
    while (n--) *(*o)++ = t[n];
}
static int save_song(const char *path) {
    uint32_t cap = 32768; char *buf = kmalloc(cap), *o = buf;
    if (!buf) return -1;
    const char *hdr = "AXISTRACKER 1\nspeed "; while (*hdr) *o++ = *hdr++;
    put_int(&o, speed); *o++ = '\n';
    const char *os = "order "; while (*os) *o++ = *os++;
    put_int(&o, order_len); for (int i = 0; i < order_len; i++) { *o++ = ' '; put_int(&o, order[i]); }
    *o++ = '\n';
    for (int p = 0; p < TPAT; p++)
        for (int r = 0; r < TROWS; r++) {
            for (int c = 0; c < TCH; c++) { put_int(&o, pat[p][r][c].note); *o++ = ','; put_int(&o, pat[p][r][c].inst); *o++ = ' '; }
            *o++ = '\n';
        }
    int id = fs_mkfile(path, fs_cwd);
    int rc = -1;
    if (id >= 0) { rc = fs_write(id, buf, (uint32_t)(o - buf)); fs_save(); }
    kfree(buf);
    return rc < 0 ? -1 : 0;
}
static int scan_int(const char **p) {
    while (**p && (**p < '0' || **p > '9')) (*p)++;
    int v = 0; while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0');
    return v;
}
static int load_song(const char *path) {
    int id = fs_resolve(path, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE || !fs_nodes[id].data) return -1;
    const char *s = (const char *)fs_nodes[id].data;
    if (strncmp(s, "AXISTRACKER", 11)) return -1;
    const char *sp = strstr(s, "speed"); if (!sp) return -1; sp += 5; speed = scan_int(&sp); if (speed < 2) speed = 7;
    const char *op = strstr(s, "order"); if (!op) return -1; op += 5;
    order_len = scan_int(&op); if (order_len < 1) order_len = 1; if (order_len > ORDMAX) order_len = ORDMAX;
    const char *q = op;
    for (int i = 0; i < order_len; i++) { int v = scan_int(&q); order[i] = (uint8_t)(v % TPAT); }
    memset(pat, 0, sizeof pat);
    for (int p = 0; p < TPAT; p++)
        for (int r = 0; r < TROWS; r++)
            for (int c = 0; c < TCH; c++) {
                int nn = scan_int(&q); int ins = scan_int(&q);
                pat[p][r][c].note = (uint8_t)nn; pat[p][r][c].inst = (uint8_t)(ins % NINSTR);
            }
    return 0;
}

/* ---- the app ----------------------------------------------------------- */
static void trig_row(int p, int r, bool fm, int atten) {
    int spk = 0;
    for (int c = 0; c < TCH; c++) {
        cell_t *cell = &pat[p][r][c];
        if (cell->note == 97) { if (fm) opl_note_off(c); }
        else if (cell->note) {
            int hz = note_hz(cell->note);
            if (fm) { opl_set_instrument(c, cell->inst); opl_note_on(c, hz, atten); }
            else if (!spk) { snd_tone(hz); spk = 1; }
        }
    }
}

int tracker_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    if (order_len == 0) new_song();

    int er = 0, ec = 0, curpat = 0, octave = 4, inst = 0;
    bool playing = false, songmode = false;
    int playrow = 0, ordpos = 0, playpat = 0;
    uint32_t next_tick = 0, audition_off = 0;
    bool fm = opl_available();
    int atten = 10;
    char msg[48] = "";

    for (;;) {
        /* playback clock */
        if (playing && (int32_t)(ticks - next_tick) >= 0) {
            trig_row(playpat, playrow, fm, atten);
            next_tick = ticks + speed;
            playrow++;
            if (playrow >= TROWS) {
                playrow = 0;
                if (songmode) { ordpos++; if (ordpos >= order_len) ordpos = 0; playpat = order[ordpos]; }
            }
        }
        if (audition_off && (int32_t)(ticks - audition_off) >= 0 && fm) { opl_note_off(ec); audition_off = 0; }

        /* ---- draw ---- */
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_LGRAY, C_BLACK));
        scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LCYAN));
        scr_puts(2, 0, "\xF0 AXIS TRACKER", ATTR(C_BLACK, C_LCYAN));
        scr_printf(SCREEN_W - 40, 0, ATTR(C_BLACK, C_LCYAN), "Oct %d  Ins %d  Spd %d  Pat %d  %s",
                   octave, inst, speed, curpat, playing ? (songmode ? "SONG" : "PLAY") : "EDIT");

        int top = er - 9; if (top < 0) top = 0; if (top > TROWS - 20) top = TROWS - 20; if (top < 0) top = 0;
        for (int i = 0; i < 20 && top + i < TROWS; i++) {
            int r = top + i, y = 2 + i;
            bool prow = playing && r == playrow && curpat == playpat;
            uint8_t ra = prow ? ATTR(C_WHITE, C_BLUE) : ATTR(C_DGRAY, C_BLACK);
            scr_printf(1, y, ra, "%02d", r);
            for (int c = 0; c < TCH; c++) {
                char lb[8]; cell_label(&pat[curpat][r][c], lb);
                uint8_t a = (r == er && c == ec) ? ATTR(C_BLACK, C_YELLOW)
                           : prow ? ATTR(C_WHITE, C_BLUE)
                           : (pat[curpat][r][c].note ? ATTR(C_LGREEN, C_BLACK) : ATTR(C_DGRAY, C_BLACK));
                scr_fill(5 + c * 10, y, 9, 1, ' ', a);
                scr_puts(6 + c * 10, y, lb, a);
            }
        }
        for (int c = 0; c < TCH; c++) scr_printf(6 + c * 10, 1, ATTR(C_LCYAN, C_BLACK), "CH%d", c + 1);

        /* order list */
        scr_printf(1, 23, ATTR(C_WHITE, C_BLACK), "Order:");
        for (int i = 0; i < order_len && i < 24; i++)
            scr_printf(8 + i * 2, 23, i == ordpos && playing && songmode ? ATTR(C_BLACK, C_LGREEN) : ATTR(C_LGRAY, C_BLACK), "%d", order[i]);
        if (msg[0]) scr_puts(SCREEN_W - (int)strlen(msg) - 1, 23, msg, ATTR(C_YELLOW, C_BLACK));

        scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
        scr_puts(1, 24, "Keys: notes ZSXDC.. / QWE..  1 off  Del clear  []oct -=spd F1/2 ins PgUp/Dn pat O/P order  Spc/Enter play  F5/6 save/load  Esc",
                 ATTR(C_BLACK, C_LGRAY));

        int k = kbd_trygetkey();
        if (k < 0) { cpu_hlt(); continue; }
        msg[0] = 0;
        int oo, semi = key_semi(k, &oo);
        if (semi >= 0) {                                   /* enter a note */
            int v = (octave + oo) * 12 + semi + 1;
            if (v >= 1 && v <= 96) {
                pat[curpat][er][ec].note = (uint8_t)v;
                pat[curpat][er][ec].inst = (uint8_t)inst;
                if (fm && !playing) { opl_set_instrument(ec, inst); opl_note_on(ec, note_hz(v), atten); audition_off = ticks + 20; }
                er = (er + 1) % TROWS;
            }
            continue;
        }
        switch (k) {
            case KEY_UP: er = (er + TROWS - 1) % TROWS; break;
            case KEY_DOWN: er = (er + 1) % TROWS; break;
            case KEY_LEFT: ec = (ec + TCH - 1) % TCH; break;
            case KEY_RIGHT: ec = (ec + 1) % TCH; break;
            case '1': pat[curpat][er][ec].note = 97; er = (er + 1) % TROWS; break;
            case KEY_DELETE: case KEY_BACKSPACE: case '.':
                pat[curpat][er][ec].note = 0; pat[curpat][er][ec].inst = 0; er = (er + 1) % TROWS; break;
            case '[': if (octave > 0) octave--; break;
            case ']': if (octave < 7) octave++; break;
            case '-': if (speed < 30) speed++; break;
            case '=': case '+': if (speed > 2) speed--; break;
            case KEY_F1: inst = (inst + NINSTR - 1) % NINSTR; break;
            case KEY_F2: inst = (inst + 1) % NINSTR; break;
            case KEY_PGUP: curpat = (curpat + TPAT - 1) % TPAT; break;
            case KEY_PGDN: curpat = (curpat + 1) % TPAT; break;
            case 'o': case 'O': if (order_len < ORDMAX) { order[order_len++] = (uint8_t)curpat; strlcpy(msg, "added to order", sizeof msg); } break;
            case 'p': case 'P': if (order_len > 1) { order_len--; strlcpy(msg, "removed from order", sizeof msg); } break;
            case ' ':
                playing = !playing; songmode = false; playpat = curpat; playrow = er; next_tick = 0;
                if (!playing) { if (fm) opl_silence(); else snd_tone(0); }
                break;
            case KEY_ENTER:
                playing = true; songmode = true; ordpos = 0; playpat = order[0]; playrow = 0; next_tick = 0;
                break;
            case KEY_F8: clear_pattern(curpat); strlcpy(msg, "pattern cleared", sizeof msg); break;
            case KEY_F5: {
                char fn[64]; strlcpy(fn, "song.trk", sizeof fn);
                if (tui_prompt("Save song", "File name:", fn, sizeof fn) && fn[0])
                    strlcpy(msg, save_song(fn) == 0 ? "saved" : "save failed", sizeof msg);
                con_show_cursor(false);
                break;
            }
            case KEY_F6: {
                char fn[64]; strlcpy(fn, "song.trk", sizeof fn);
                if (tui_prompt("Load song", "File name:", fn, sizeof fn) && fn[0])
                    strlcpy(msg, load_song(fn) == 0 ? "loaded" : "load failed", sizeof msg);
                con_show_cursor(false);
                if (er >= TROWS) er = 0;
                break;
            }
            case KEY_ESC:
                if (fm) opl_silence(); else snd_tone(0);
                con_show_cursor(true); scr_restore(saved); return 0;
        }
    }
}
