/*
 * AxisOS - sound for old PCs.
 *
 * Two layers, both chosen for maximum vintage-hardware compatibility:
 *  - PC speaker (every PC ever) for tones and beeps, via the 8254 timer.
 *  - Sound Blaster 16 / Pro / original SB for real 8-bit PCM playback, via
 *    classic ISA DMA on channel 1 (the mode all Sound Blasters share). We use
 *    the DSP "time constant" rate command and single-cycle DMA, and detect the
 *    end of a block by polling the 8237 DMA terminal-count status - so no IRQ
 *    wiring is needed, which keeps it simple and robust on real old machines
 *    and in QEMU (-device sb16).
 *
 * WAV files (8- or 16-bit, mono or stereo PCM) are down-converted to 8-bit
 * unsigned mono and played. No floating point anywhere.
 */
#include "axis.h"

/* ---- PC speaker (8254 channel 2 + port 0x61) --------------------------- */
void snd_tone(int hz) {
    if (hz <= 0) { outb(0x61, inb(0x61) & 0xFC); return; }
    uint32_t d = 1193182u / (uint32_t)hz;
    outb(0x43, 0xB6);
    outb(0x42, d & 0xFF);
    outb(0x42, (d >> 8) & 0xFF);
    outb(0x61, inb(0x61) | 3);
}
void snd_beep(int hz, int ms) { snd_tone(hz); sleep_ms(ms); snd_tone(0); }

/* ---- Sound Blaster 16 (base 0x220, 8-bit DMA channel 1) ---------------- */
#define SB_BASE   0x220
#define SB_RESET  (SB_BASE + 0x6)
#define SB_READ   (SB_BASE + 0xA)
#define SB_WRITE  (SB_BASE + 0xC)
#define SB_WSTAT  (SB_BASE + 0xC)
#define SB_RSTAT  (SB_BASE + 0xE)

static int sb_present = 0;
static uint8_t *dma_base = 0;      /* 64KB-aligned DMA window (no boundary crossing) */

static void dsp_write(uint8_t v) { int t = 100000; while ((inb(SB_WSTAT) & 0x80) && t--) {} outb(SB_WRITE, v); }

static int sb_reset(void) {
    outb(SB_RESET, 1);
    for (volatile int i = 0; i < 20000; i++) {}
    outb(SB_RESET, 0);
    int t = 200000;
    while (t--) { if (inb(SB_RSTAT) & 0x80) { if (inb(SB_READ) == 0xAA) return 1; } }
    return 0;
}

void sound_init(void) {
    opl_init();                                    /* OPL2 FM synth (AdLib) */
    sb_present = sb_reset();
    if (sb_present) {
        uint8_t *raw = kmalloc(0x20000);           /* 128KB: enough slack to 64KB-align */
        if (raw) {
            uint32_t a = ((uint32_t)raw + 0xFFFF) & ~0xFFFFu;  /* 64KB-aligned */
            dma_base = (uint8_t *)a;
        } else sb_present = 0;
        if (sb_present) dsp_write(0xD1);           /* turn the DAC speaker on */
    }
}
bool sound_available(void) { return sb_present; }

static void dma8_setup(uint32_t addr, uint32_t n) {
    outb(0x0A, 0x05);                 /* mask channel 1                     */
    outb(0x0C, 0x00);                 /* clear byte-pointer flip-flop       */
    outb(0x0B, 0x49);                 /* single, read (mem->device), chan 1 */
    outb(0x02, addr & 0xFF);          /* base address low                   */
    outb(0x02, (addr >> 8) & 0xFF);   /* base address high                  */
    outb(0x83, (addr >> 16) & 0xFF);  /* page register (channel 1)          */
    outb(0x0C, 0x00);
    outb(0x03, (n - 1) & 0xFF);       /* count low                          */
    outb(0x03, ((n - 1) >> 8) & 0xFF);/* count high                         */
    outb(0x0A, 0x01);                 /* unmask channel 1                   */
}

/* play 8-bit unsigned mono PCM at `rate` Hz; returns 0 ok */
int sound_play_pcm8(const uint8_t *pcm, uint32_t n, int rate) {
    if (!sb_present || !dma_base) return -1;
    if (rate < 4000) rate = 4000;
    if (rate > 22050) rate = 22050;              /* time-constant method range */
    uint8_t tc = (uint8_t)(256 - (1000000 / rate));
    uint32_t off = 0;
    while (off < n) {
        uint32_t chunk = n - off;
        if (chunk > 0x8000) chunk = 0x8000;      /* 32KB chunks, within 64KB window */
        memcpy(dma_base, pcm + off, chunk);
        inb(SB_RSTAT);                           /* clear any pending 8-bit IRQ */
        inb(0x08);                               /* clear DMA1 status latch */
        dma8_setup((uint32_t)dma_base, chunk);
        dsp_write(0x40); dsp_write(tc);          /* set sample rate (time constant) */
        dsp_write(0x14);                         /* 8-bit single-cycle DMA output */
        dsp_write((chunk - 1) & 0xFF);
        dsp_write(((chunk - 1) >> 8) & 0xFF);
        /* wait for the DMA terminal count, with a time-based safety bound */
        uint32_t dur_ticks = (chunk * (uint32_t)TICK_HZ) / (uint32_t)rate;
        uint32_t deadline = ticks + dur_ticks + 3;
        for (;;) {
            if (inb(0x08) & 0x02) break;         /* channel-1 terminal count reached */
            if ((int32_t)(ticks - deadline) >= 0) break;
            if (user_abort()) { off = n; break; }
        }
        inb(SB_RSTAT);                           /* acknowledge the 8-bit DMA IRQ */
        off += chunk;
    }
    return 0;
}

/* ---- WAV (RIFF/WAVE PCM) ---------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1]<<8); }

/* parse a WAV into 8-bit unsigned mono PCM (kmalloc'd). 0 ok */
int wav_load(const uint8_t *d, uint32_t len, uint8_t **pcm8, uint32_t *nsamp, int *rate) {
    if (len < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    uint32_t p = 12; int channels = 1, bits = 8, sr = 11025;
    const uint8_t *data = NULL; uint32_t dlen = 0;
    while (p + 8 <= len) {
        const uint8_t *id = d + p; uint32_t sz = rd32(d + p + 4); p += 8;
        if (p + sz > len) sz = len - p;
        if (!memcmp(id, "fmt ", 4)) {
            if (rd16(d + p) != 1) return -2;      /* PCM only */
            channels = rd16(d + p + 2); sr = rd32(d + p + 4); bits = rd16(d + p + 14);
        } else if (!memcmp(id, "data", 4)) { data = d + p; dlen = sz; }
        p += sz + (sz & 1);
    }
    if (!data || (bits != 8 && bits != 16) || channels < 1 || channels > 2) return -3;
    uint32_t frame = (bits / 8) * channels;
    uint32_t frames = dlen / frame;
    uint8_t *out = kmalloc(frames ? frames : 1);
    if (!out) return -1;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *s = data + i * frame;
        int v;
        if (bits == 8) {                          /* already unsigned 8-bit */
            v = channels == 2 ? ((int)s[0] + s[1]) / 2 : s[0];
        } else {                                  /* signed 16-bit LE -> unsigned 8 */
            int l = (int16_t)rd16(s);
            int r = channels == 2 ? (int16_t)rd16(s + 2) : l;
            v = (((l + r) / 2) + 32768) >> 8;
        }
        out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    *pcm8 = out; *nsamp = frames; *rate = sr;
    return 0;
}

/* play a WAV from bytes; returns 0 ok */
int sound_play_wav(const uint8_t *d, uint32_t len) {
    uint8_t *pcm; uint32_t n; int rate;
    int rc = wav_load(d, len, &pcm, &n, &rate);
    if (rc != 0) return rc;
    rc = sound_play_pcm8(pcm, n, rate);
    kfree(pcm);
    return rc;
}
