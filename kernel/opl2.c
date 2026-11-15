/*
 * AxisOS - OPL2 (Yamaha YM3812 / AdLib) FM synthesiser.
 *
 * The classic early-90s PC music chip at I/O ports 0x388/0x389. Two-operator
 * FM voices across 9 channels. AxisOS uses it for richer music than the PC
 * speaker (the Radio app) and anything else that wants real synthesised tones.
 * QEMU emulates it with -device adlib; it is also built into Sound Blaster Pro
 * and later cards on real hardware.
 */
#include "axis.h"

#define OPL_ADDR 0x388
#define OPL_DATA 0x389

static int opl_ok = 0;
static uint8_t b0_shadow[9];     /* last 0xB0..0xB8 value (block/fnum/key) */
static uint8_t car_ksl[9];       /* carrier KSL bits, so volume can keep them */

/* per-channel operator register offsets (classic AdLib 2-op layout) */
static const uint8_t mod_off[9] = {0x00,0x01,0x02,0x08,0x09,0x0A,0x10,0x11,0x12};
static const uint8_t car_off[9] = {0x03,0x04,0x05,0x0B,0x0C,0x0D,0x13,0x14,0x15};

typedef struct {
    uint8_t m_avekm, m_ksltl, m_ad, m_sr, m_wave;   /* modulator */
    uint8_t c_avekm, c_ksltl, c_ad, c_sr, c_wave;   /* carrier   */
    uint8_t fbconn;
} opl_instr_t;

/* a small palette of timbres (register values chosen for clear musical tones) */
static const opl_instr_t instruments[] = {
    /* 0: bright synth lead  */ {0x01,0x10,0xF2,0x53,0x00, 0x01,0x00,0xF2,0x13,0x00, 0x0A},
    /* 1: square-ish chiptune*/ {0x01,0x08,0xF4,0x24,0x03, 0x01,0x00,0xF4,0x14,0x03, 0x00},
    /* 2: mellow organ       */ {0x01,0x00,0xA0,0x02,0x00, 0x01,0x00,0xA0,0x02,0x00, 0x01},
    /* 3: hard 8-bit pulse   */ {0x02,0x05,0xF8,0x45,0x02, 0x01,0x00,0xF8,0x15,0x02, 0x06},
    /* 4: soft pad/bass      */ {0x01,0x12,0x51,0x33,0x00, 0x01,0x00,0x61,0x33,0x00, 0x08},
};
#define OPL_NINSTR ((int)(sizeof instruments / sizeof *instruments))

static void opl_w(uint8_t reg, uint8_t val) {
    outb(OPL_ADDR, reg);
    for (int i = 0; i < 6; i++) inb(OPL_ADDR);     /* address settle (~4us) */
    outb(OPL_DATA, val);
    for (int i = 0; i < 35; i++) inb(OPL_ADDR);    /* data settle (~23us)   */
}

static int opl_detect(void) {
    opl_w(0x04, 0x60); opl_w(0x04, 0x80);          /* reset both timers */
    uint8_t s1 = inb(OPL_ADDR) & 0xE0;
    opl_w(0x02, 0xFF); opl_w(0x04, 0x21);          /* start timer 1 */
    for (int i = 0; i < 1000; i++) inb(OPL_ADDR);
    uint8_t s2 = inb(OPL_ADDR) & 0xE0;
    opl_w(0x04, 0x60); opl_w(0x04, 0x80);
    return (s1 == 0x00 && s2 == 0xC0);
}

void opl_init(void) {
    opl_ok = opl_detect();
    for (int r = 0x01; r <= 0xF5; r++) opl_w(r, 0x00);   /* clear everything */
    opl_w(0x01, 0x20);                                   /* enable waveform select (OPL2) */
    opl_w(0x08, 0x00);
    opl_w(0xBD, 0x00);                                   /* no rhythm / AM-vib depth */
    for (int c = 0; c < 9; c++) { b0_shadow[c] = 0; car_ksl[c] = 0; }
}
bool opl_available(void) { return opl_ok; }

void opl_set_instrument(int ch, int idx) {
    if (ch < 0 || ch > 8) return;
    const opl_instr_t *in = &instruments[idx < 0 || idx >= OPL_NINSTR ? 0 : idx];
    uint8_t m = mod_off[ch], c = car_off[ch];
    opl_w(0x20 + m, in->m_avekm); opl_w(0x40 + m, in->m_ksltl);
    opl_w(0x60 + m, in->m_ad);    opl_w(0x80 + m, in->m_sr);  opl_w(0xE0 + m, in->m_wave);
    opl_w(0x20 + c, in->c_avekm); opl_w(0x40 + c, in->c_ksltl);
    opl_w(0x60 + c, in->c_ad);    opl_w(0x80 + c, in->c_sr);  opl_w(0xE0 + c, in->c_wave);
    opl_w(0xC0 + ch, in->fbconn);
    car_ksl[ch] = in->c_ksltl & 0xC0;
}

/* key a note on. freq in Hz; atten 0 (loudest) .. 63 (silent) on the carrier */
void opl_note_on(int ch, int freq, int atten) {
    if (ch < 0 || ch > 8) return;
    if (freq <= 0) { opl_note_off(ch); return; }
    if (atten < 0) atten = 0; if (atten > 63) atten = 63;
    opl_w(0x40 + car_off[ch], car_ksl[ch] | (uint8_t)atten);
    uint32_t fnum = 0; int block = 0;
    for (block = 0; block < 8; block++) {
        fnum = ((uint32_t)freq * (1u << (20 - block))) / 49716u;
        if (fnum < 1024) break;
    }
    opl_w(0xA0 + ch, fnum & 0xFF);
    b0_shadow[ch] = (uint8_t)(0x20 | ((block & 7) << 2) | ((fnum >> 8) & 3));
    opl_w(0xB0 + ch, b0_shadow[ch]);
}
void opl_note_off(int ch) {
    if (ch < 0 || ch > 8) return;
    opl_w(0xB0 + ch, b0_shadow[ch] & ~0x20);       /* clear key-on, keep pitch */
}
void opl_silence(void) { for (int c = 0; c < 9; c++) opl_note_off(c); }
