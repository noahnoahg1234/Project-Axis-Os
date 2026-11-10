/* AxisOS - PS/2 keyboard driver (scancode set 1) + serial input */
#include "axis.h"

#define KBUF 64
static volatile int kbuf[KBUF];
static volatile int khead, ktail;
static bool shift_l, shift_r, ctrl, alt, caps, e0;

static const char map_lo[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0};
static const char map_hi[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0};

static void push(int k) {
    int n = (khead + 1) % KBUF;
    if (n == ktail) return;
    kbuf[khead] = k;
    khead = n;
}

static void kbd_irq(regs_t *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) {
        e0 = true;
        return;
    }
    bool rel = sc & 0x80;
    sc &= 0x7F;
    if (e0) {
        e0 = false;
        if (sc == 0x1D) { ctrl = !rel; return; }
        if (sc == 0x38) { alt = !rel; return; }
        if (sc == 0x2A || sc == 0x36) return; /* fake shifts */
        if (rel) return;
        switch (sc) {
        case 0x48: push(KEY_UP); break;
        case 0x50: push(KEY_DOWN); break;
        case 0x4B: push(KEY_LEFT); break;
        case 0x4D: push(KEY_RIGHT); break;
        case 0x47: push(KEY_HOME); break;
        case 0x4F: push(KEY_END); break;
        case 0x49: push(KEY_PGUP); break;
        case 0x51: push(KEY_PGDN); break;
        case 0x52: push(KEY_INSERT); break;
        case 0x53: push(KEY_DELETE); break;
        case 0x1C: push('\n'); break;
        case 0x35: push('/'); break;
        }
        return;
    }
    switch (sc) {
    case 0x2A: shift_l = !rel; return;
    case 0x36: shift_r = !rel; return;
    case 0x1D: ctrl = !rel; return;
    case 0x38: alt = !rel; return;
    case 0x3A: if (!rel) caps = !caps; return;
    }
    if (rel) return;
    if (sc >= 0x3B && sc <= 0x44) { push(KEY_F1 + (sc - 0x3B)); return; }
    if (sc == 0x57) { push(KEY_F11); return; }
    if (sc == 0x58) { push(KEY_F12); return; }
    /* keypad without numlock handling: map to navigation keys */
    switch (sc) {
    case 0x48: push(KEY_UP); return;
    case 0x50: push(KEY_DOWN); return;
    case 0x4B: push(KEY_LEFT); return;
    case 0x4D: push(KEY_RIGHT); return;
    case 0x47: push(KEY_HOME); return;
    case 0x4F: push(KEY_END); return;
    case 0x49: push(KEY_PGUP); return;
    case 0x51: push(KEY_PGDN); return;
    case 0x53: push(KEY_DELETE); return;
    case 0x4A: push('-'); return;
    case 0x4E: push('+'); return;
    }
    bool shift = shift_l || shift_r;
    char c = shift ? map_hi[sc] : map_lo[sc];
    if (!c) return;
    if (caps && isalpha(c)) c = shift ? tolower(c) : toupper(c);
    if (ctrl && isalpha(c)) c = KEY_CTRL(c);
    push((uint8_t)c);
}

void kbd_init(void) {
    while (inb(0x64) & 1) inb(0x60);
    irq_set_handler(1, kbd_irq);
}

/* serial: translate ANSI escape sequences into key codes */
static int serial_key(void) {
    int c = serial_getc();
    if (c < 0) return -1;
    if (c == '\r') return '\n';
    if (c == '\n') return -1; /* swallow LF of CRLF; CR already produced newline */
    if (c == 0x7F) return '\b';
    if (c != 27) return c;
    uint32_t t = ticks;
    int c2;
    while ((c2 = serial_getc()) < 0)
        if (ticks - t > 5) return KEY_ESC;
    if (c2 != '[' && c2 != 'O') return KEY_ESC;
    int c3, num = 0;
    for (;;) {
        t = ticks;
        while ((c3 = serial_getc()) < 0)
            if (ticks - t > 5) return KEY_ESC;
        if (isdigit(c3)) { num = num * 10 + c3 - '0'; continue; }
        break;
    }
    switch (c3) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
    case 'S': return KEY_F4;
    case '~':
        switch (num) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 2: return KEY_INSERT;
        case 3: return KEY_DELETE;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        case 15: return KEY_F5;
        case 17: return KEY_F6;
        case 18: return KEY_F7;
        case 19: return KEY_F8;
        case 20: return KEY_F9;
        case 21: return KEY_F10;
        }
    }
    return -1;
}

int kbd_trygetkey(void) {
    if (khead != ktail) {
        int k = kbuf[ktail];
        ktail = (ktail + 1) % KBUF;
        return k;
    }
    return serial_key();
}

int kbd_getkey(void) {
    for (;;) {
        int k = kbd_trygetkey();
        if (k >= 0) return k;
        cpu_hlt();
    }
}

/* remove a pending Ctrl+C from the keyboard buffer; true if there was one */
bool kbd_ctrl_c_pending(void) {
    for (int i = ktail; i != khead; i = (i + 1) % KBUF)
        if (kbuf[i] == KEY_CTRL('c')) {
            khead = ktail = 0;
            return true;
        }
    return false;
}
