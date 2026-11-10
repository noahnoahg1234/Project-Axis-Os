/* AxisOS - VGA text console, output capture and colour themes */
#include "axis.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static int cx, cy;
static uint8_t attr = 0x07;
static uint32_t scrolls;
static bool mirror = true;

static char *cap_buf;
static size_t cap_len, cap_cap;
static int cap_depth;

theme_t theme;

void con_update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(cy * SCREEN_W + cx);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

void con_show_cursor(bool on) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, on ? 0x0D : 0x20);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0E);
}

void con_init(void) {
    cx = cy = 0;
    con_show_cursor(true);
    con_clear();
}

void con_clear(void) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) VGA[i] = (uint16_t)(attr << 8 | ' ');
    cx = cy = 0;
    con_update_hw_cursor();
}

static void scroll(void) {
    memmove((void *)VGA, (const void *)(VGA + SCREEN_W), SCREEN_W * (SCREEN_H - 1) * 2);
    for (int i = 0; i < SCREEN_W; i++) VGA[(SCREEN_H - 1) * SCREEN_W + i] = (uint16_t)(attr << 8 | ' ');
    cy = SCREEN_H - 1;
    scrolls++;
}

static void cap_put(char c) {
    if (cap_len + 2 > cap_cap) {
        size_t nc = cap_cap ? cap_cap * 2 : 1024;
        char *nb = krealloc(cap_buf, nc);
        if (!nb) return;
        cap_buf = nb;
        cap_cap = nc;
    }
    cap_buf[cap_len++] = c;
    cap_buf[cap_len] = 0;
}

static void vga_putc(char c) {
    switch (c) {
    case '\n': cx = 0; cy++; break;
    case '\r': cx = 0; break;
    case '\b':
        if (cx > 0) cx--;
        else if (cy > 0) cx = SCREEN_W - 1, cy--;
        break;
    case '\t':
        do VGA[cy * SCREEN_W + cx++] = (uint16_t)(attr << 8 | ' ');
        while (cx % 8 && cx < SCREEN_W);
        break;
    default:
        VGA[cy * SCREEN_W + cx] = (uint16_t)(attr << 8 | (uint8_t)c);
        cx++;
    }
    if (cx >= SCREEN_W) cx = 0, cy++;
    if (cy >= SCREEN_H) scroll();
}

void con_putc(char c) {
    if (cap_depth) {
        cap_put(c);
        return;
    }
    vga_putc(c);
    if (mirror) {
        if (c == '\n') serial_putc('\r');
        uint8_t u = (uint8_t)c;
        if (u == 0xB3 || u == 0xBA) c = '|';
        else if (u == 0xC4 || u == 0xCD) c = '-';
        else if (u >= 0xB4 && u <= 0xDA) c = '+';
        serial_putc(c);
    }
}

void con_write(const char *s) {
    while (*s) con_putc(*s++);
    if (!cap_depth) con_update_hw_cursor();
}
void con_writen(const char *s, size_t n) {
    while (n--) con_putc(*s++);
    if (!cap_depth) con_update_hw_cursor();
}

int kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_write(buf);
    return r;
}

void con_set_attr(uint8_t a) { attr = a; }
uint8_t con_get_attr(void) { return attr; }
void con_set_cursor(int x, int y) {
    cx = x < 0 ? 0 : (x >= SCREEN_W ? SCREEN_W - 1 : x);
    cy = y < 0 ? 0 : (y >= SCREEN_H ? SCREEN_H - 1 : y);
    con_update_hw_cursor();
}
void con_get_cursor(int *x, int *y) {
    *x = cx;
    *y = cy;
}
uint32_t con_scroll_count(void) { return scrolls; }
void con_serial_mirror(bool on) { mirror = on; }

void con_capture_begin(void) {
    if (cap_depth++ == 0) {
        cap_len = 0;
        if (cap_buf) cap_buf[0] = 0;
    }
}
char *con_capture_end(size_t *len) {
    if (!cap_depth) return NULL;
    char *r = NULL;
    if (--cap_depth == 0) {
        r = kmalloc(cap_len + 1);
        if (r) {
            if (cap_len) memcpy(r, cap_buf, cap_len);
            r[cap_len] = 0;
        }
        if (len) *len = cap_len;
        cap_len = 0;
    }
    return r;
}
bool con_capturing(void) { return cap_depth > 0; }

/* ---- direct screen access -------------------------------------------- */
void scr_put(int x, int y, char c, uint8_t a) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    VGA[y * SCREEN_W + x] = (uint16_t)(a << 8 | (uint8_t)c);
}
void scr_puts(int x, int y, const char *s, uint8_t a) {
    while (*s) scr_put(x++, y, *s++, a);
}
void scr_putsn(int x, int y, const char *s, int maxw, uint8_t a) {
    for (int i = 0; i < maxw; i++) scr_put(x + i, y, *s ? *s++ : ' ', a);
}
void scr_fill(int x, int y, int w, int h, char c, uint8_t a) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) scr_put(x + i, y + j, c, a);
}
void scr_box(int x, int y, int w, int h, uint8_t a, const char *title) {
    scr_fill(x, y, w, h, ' ', a);
    for (int i = 1; i < w - 1; i++) {
        scr_put(x + i, y, (char)0xC4, a);
        scr_put(x + i, y + h - 1, (char)0xC4, a);
    }
    for (int j = 1; j < h - 1; j++) {
        scr_put(x, y + j, (char)0xB3, a);
        scr_put(x + w - 1, y + j, (char)0xB3, a);
    }
    scr_put(x, y, (char)0xDA, a);
    scr_put(x + w - 1, y, (char)0xBF, a);
    scr_put(x, y + h - 1, (char)0xC0, a);
    scr_put(x + w - 1, y + h - 1, (char)0xD9, a);
    if (title && *title) {
        int tl = (int)strlen(title);
        int tx = x + (w - tl - 2) / 2;
        scr_put(tx, y, ' ', a);
        scr_puts(tx + 1, y, title, a);
        scr_put(tx + tl + 1, y, ' ', a);
    }
}
void scr_save(uint16_t *buf) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) buf[i] = VGA[i];
}
void scr_restore(const uint16_t *buf) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) VGA[i] = buf[i];
}
int scr_printf(int x, int y, uint8_t a, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    scr_puts(x, y, buf, a);
    return r;
}

/* ---- themes ------------------------------------------------------------ */
/*                    name            normal               prompt                path                  accent                error                 dim                  bar                         select */
static const theme_t os_themes[OS_COUNT] = {
    {"Linux",   ATTR(C_LGRAY, C_BLACK), ATTR(C_LGREEN, C_BLACK), ATTR(C_LBLUE, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_LRED, C_BLACK), ATTR(C_DGRAY, C_BLACK), ATTR(C_BLACK, C_GREEN), ATTR(C_WHITE, C_GREEN)},
    {"Windows", ATTR(C_WHITE, C_BLUE),  ATTR(C_WHITE, C_BLUE),   ATTR(C_YELLOW, C_BLUE), ATTR(C_LCYAN, C_BLUE),   ATTR(C_LRED, C_BLUE),  ATTR(C_LGRAY, C_BLUE), ATTR(C_BLACK, C_LGRAY), ATTR(C_WHITE, C_CYAN)},
    {"FreeBSD", ATTR(C_LGRAY, C_BLACK), ATTR(C_LRED, C_BLACK),   ATTR(C_WHITE, C_BLACK), ATTR(C_LRED, C_BLACK),   ATTR(C_YELLOW, C_BLACK), ATTR(C_DGRAY, C_BLACK), ATTR(C_WHITE, C_RED), ATTR(C_WHITE, C_RED)},
    {"DOS",     ATTR(C_LGRAY, C_BLACK), ATTR(C_WHITE, C_BLACK),   ATTR(C_LCYAN, C_BLACK), ATTR(C_WHITE, C_BLACK),  ATTR(C_LRED, C_BLACK),   ATTR(C_DGRAY, C_BLACK), ATTR(C_BLACK, C_LGRAY), ATTR(C_BLACK, C_LGRAY)},
};
static const theme_t extra_themes[] = {
    {"Classic",  ATTR(C_LGRAY, C_BLACK), ATTR(C_WHITE, C_BLACK),  ATTR(C_LGRAY, C_BLACK), ATTR(C_WHITE, C_BLACK),  ATTR(C_LRED, C_BLACK), ATTR(C_DGRAY, C_BLACK), ATTR(C_BLACK, C_LGRAY), ATTR(C_BLACK, C_LGRAY)},
    {"Matrix",   ATTR(C_GREEN, C_BLACK), ATTR(C_LGREEN, C_BLACK), ATTR(C_LGREEN, C_BLACK), ATTR(C_LGREEN, C_BLACK), ATTR(C_LRED, C_BLACK), ATTR(C_DGRAY, C_BLACK), ATTR(C_BLACK, C_GREEN), ATTR(C_BLACK, C_LGREEN)},
    {"Amber",    ATTR(C_BROWN, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_LRED, C_BLACK), ATTR(C_DGRAY, C_BLACK), ATTR(C_BLACK, C_BROWN), ATTR(C_BLACK, C_YELLOW)},
    {"Ocean",    ATTR(C_LCYAN, C_BLUE),  ATTR(C_WHITE, C_BLUE),   ATTR(C_YELLOW, C_BLUE), ATTR(C_LGREEN, C_BLUE),  ATTR(C_LRED, C_BLUE),  ATTR(C_LGRAY, C_BLUE), ATTR(C_BLUE, C_LCYAN), ATTR(C_BLUE, C_WHITE)},
    {"Contrast", ATTR(C_WHITE, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_LCYAN, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_LRED, C_BLACK), ATTR(C_LGRAY, C_BLACK), ATTR(C_BLACK, C_WHITE), ATTR(C_BLACK, C_YELLOW)},
};
const char *theme_names[] = {"Auto (per system)", "Classic", "Matrix", "Amber", "Ocean", "High contrast"};
const int theme_count = 6;

void theme_apply(void) {
    if (cfg.theme == 0 || cfg.theme >= theme_count) theme = os_themes[cur_os];
    else theme = extra_themes[cfg.theme - 1];
    attr = theme.normal;
}
