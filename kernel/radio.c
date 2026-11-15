/*
 * AxisOS - Radio: a retro FM tuner that plays chiptune "stations" through the
 * OPL2 FM synth (falls back to the PC speaker if no FM chip is present).
 * Tune across the dial, pick presets, change volume, watch the signal meter.
 */
#include "axis.h"

typedef struct { const char *name, *genre; int instr, tempo; const char *tune; } station_t;

/* note tune format: letter A-G (+ # / b), octave 2-6, ":n" eighth-note length;
   'R' is a rest. Same simple format as the music player. */
static const int f4[12] = {262,277,294,311,330,349,370,392,415,440,466,494};
static int note_freq(const char *s, int *dur) {
    *dur = 2;
    if (*s == 'R' || *s == 'r') { const char *c = strchr(s, ':'); if (c) *dur = atoi(c + 1); if (*dur < 1) *dur = 1; return 0; }
    int n;
    switch (*s | 0x20) {
        case 'c': n = 0; break; case 'd': n = 2; break; case 'e': n = 4; break;
        case 'f': n = 5; break; case 'g': n = 7; break; case 'a': n = 9; break;
        case 'b': n = 11; break; default: return -1;
    }
    s++;
    if (*s == '#') { n++; s++; } else if (*s == 'b') { n--; s++; }
    int oct = 4; if (*s >= '2' && *s <= '6') { oct = *s - '0'; s++; }
    if (*s == ':') *dur = atoi(s + 1);
    if (*dur < 1) *dur = 1;
    int f = f4[(n + 12) % 12], d = oct - 4;
    while (d-- > 0) f *= 2;
    while (++d < 0) f /= 2;
    return f;
}

static const station_t stations[] = {
    {"Axis FM",    "Synthwave", 0, 120,
     "C4 E4 G4 C5 B4 G4 E4 G4 A4 C5 E5 C5 A4 G4 E4 C4 "
     "F4 A4 C5 F5 E5 C5 A4 F4 G4 B4 D5 G5 D5 B4 G4 R"},
    {"Retro Gold", "Chiptune",  1, 150,
     "C5 C5 G4 G4 A4 A4 G4:4 F4 F4 E4 E4 D4 D4 C4:4 "
     "E5 E5 D5 C5 D5 E5 E5 E5 D5 D5 E5 D5 C5:4"},
    {"Hall FM",    "Classical", 2, 110,
     "E4 E4 F4 G4 G4 F4 E4 D4 C4 C4 D4 E4 E4:3 D4 D4:4 "
     "E4 E4 F4 G4 G4 F4 E4 D4 C4 C4 D4 E4 D4:3 C4 C4:4"},
    {"Night Jazz", "Jazz",      4, 100,
     "C4 Eb4 G4 Bb4 A4 G4 Eb4 C4 D4 F4 A4 C5 Bb4 A4 F4 D4 R "
     "G3 Bb3 D4 F4 Eb4 D4 Bb3 G3 R"},
    {"Game Over",  "8-bit",     3, 160,
     "C5 G4 E4 C4 E4 G4 C5 G4 F5 C5 A4 F4 A4 C5 F5 C5 "
     "G5 D5 B4 G4 B4 D5 G5 B5 C6:4"},
    {"Pulsewave",  "Electro",   3, 135,
     "A4 A4 E5 A4 C5 A4 E5 A4 G4 G4 D5 G4 B4 G4 D5 G4 "
     "F4 F4 C5 F4 A4 F4 C5 F4 E4 E4 B4 E4 G4 B4 E5 R"},
};
#define NSTATION ((int)(sizeof stations / sizeof *stations))

static void draw_dial(int sel, int vol, bool playing, int bar) {
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_LGRAY, C_BLACK));
    scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_YELLOW));
    scr_puts(2, 0, "\xF0\xF0 AXIS RADIO", ATTR(C_BLACK, C_YELLOW));
    scr_puts(SCREEN_W - 18, 0, playing ? "\x10 ON AIR" : "\xFE PAUSED", ATTR(C_BLACK, C_YELLOW));

    /* frequency dial 88 .. 108 MHz, stations spread across it */
    scr_box(3, 3, SCREEN_W - 6, 5, ATTR(C_DGRAY, C_BLACK), " TUNER ");
    for (int x = 0; x < SCREEN_W - 10; x++) {
        int tick = (x % 6 == 0);
        scr_put(5 + x, 5, tick ? (char)0xB3 : (char)0xC4, ATTR(C_DGRAY, C_BLACK));
    }
    int span = SCREEN_W - 12;
    for (int i = 0; i < NSTATION; i++) {
        int px = 5 + i * span / (NSTATION - 1);
        scr_put(px, 4, (char)0x1F, ATTR(i == sel ? C_YELLOW : C_DGRAY, C_BLACK));
    }
    int needle = 5 + sel * span / (NSTATION - 1);
    scr_put(needle, 5, (char)0xD1, ATTR(C_LRED, C_BLACK));
    scr_put(needle, 6, (char)0x1E, ATTR(C_LRED, C_BLACK));
    int mhz = 881 + sel * (1079 - 881) / (NSTATION - 1);
    scr_printf(6, 6, ATTR(C_LGREEN, C_BLACK), "%d.%d FM", mhz / 10, mhz % 10);

    /* station info */
    scr_box(3, 9, 40, 7, ATTR(C_LGRAY, C_BLACK), " NOW PLAYING ");
    scr_printf(5, 11, ATTR(C_WHITE, C_BLACK), "%s", stations[sel].name);
    scr_printf(5, 12, ATTR(C_LCYAN, C_BLACK), "%s", stations[sel].genre);
    scr_printf(5, 14, ATTR(C_DGRAY, C_BLACK), "Station %d of %d", sel + 1, NSTATION);

    /* presets */
    scr_box(45, 9, SCREEN_W - 48, 7, ATTR(C_LGRAY, C_BLACK), " PRESETS ");
    for (int i = 0; i < NSTATION; i++)
        scr_printf(47, 11 + i % 5, i == sel ? ATTR(C_BLACK, C_LGRAY) : ATTR(C_WHITE, C_BLACK), "%d %-14.14s", i + 1, stations[i].name);

    /* volume + a little signal/spectrum meter */
    scr_puts(5, 18, "VOL", ATTR(C_WHITE, C_BLACK));
    for (int i = 0; i < 16; i++) scr_put(9 + i, 18, (char)0xDB, ATTR(i < vol ? C_LGREEN : C_DGRAY, C_BLACK));
    scr_puts(5, 20, "SIG", ATTR(C_WHITE, C_BLACK));
    for (int i = 0; i < 40; i++) {
        int h = playing ? ((bar + i * 5) % 7) : 0;
        scr_put(9 + i, 20, h > 3 ? (char)0xB2 : (h ? (char)0xB1 : (char)0xB0),
                ATTR(h > 4 ? C_LRED : h > 2 ? C_YELLOW : C_LGREEN, C_BLACK));
    }

    scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    scr_puts(1, 24, "Left/Right tune   1-6 presets   +/- volume   Space play/pause   Q quit",
             ATTR(C_BLACK, C_LGRAY));
}

int radio_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    int sel = 0, vol = 12, bar = 0;
    bool playing = true, use_fm = opl_available();
    const char *scan = stations[sel].tune;
    uint32_t note_end = 0;
    int cur_hz = 0;

    if (use_fm) opl_set_instrument(0, stations[sel].instr);

    for (;;) {
        /* advance the tune */
        if (playing && ticks >= note_end) {
            while (scan && *scan && (*scan == ' ' || *scan == '\n' || *scan == '\t' || *scan == '\r')) scan++;
            if (!scan || !*scan) scan = stations[sel].tune;   /* loop the station */
            char tok[16]; int o = 0;
            while (*scan && *scan != ' ' && *scan != '\n' && *scan != '\t' && *scan != '\r' && o < 15) tok[o++] = *scan++;
            tok[o] = 0;
            int dur, hz = note_freq(tok, &dur);
            cur_hz = hz;
            int atten = (15 - vol) * 4;                        /* volume -> carrier attenuation */
            if (use_fm) { if (hz > 0) opl_note_on(0, hz, atten); else opl_note_off(0); }
            else snd_tone(hz > 0 ? hz : 0);                    /* PC-speaker fallback */
            note_end = ticks + (uint32_t)dur * (600 / (stations[sel].tempo / 10)) / 10 + 1;
            bar = hz > 0 ? 1 + (hz / 80) % 6 : 0;
        }
        draw_dial(sel, vol, playing, bar + (int)(ticks / 3));

        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_RIGHT || k == KEY_DOWN) { sel = (sel + 1) % NSTATION; goto tune; }
        else if (k == KEY_LEFT || k == KEY_UP) { sel = (sel + NSTATION - 1) % NSTATION; goto tune; }
        else if (k >= '1' && k <= '0' + NSTATION) { sel = k - '1'; goto tune; }
        else if (k == '+' || k == '=') { if (vol < 16) vol++; }
        else if (k == '-' || k == '_') { if (vol > 0) vol--; }
        else if (k == ' ') { playing = !playing; if (!playing) { if (use_fm) opl_silence(); else snd_tone(0); } else note_end = 0; }
        if (k < 0) cpu_hlt();
        continue;
    tune:
        scan = stations[sel].tune; note_end = 0;
        if (use_fm) opl_set_instrument(0, stations[sel].instr);
        continue;
    }
    if (use_fm) opl_silence(); else snd_tone(0);
    con_show_cursor(true);
    scr_restore(saved);
    (void)cur_hz;
    return 0;
}
