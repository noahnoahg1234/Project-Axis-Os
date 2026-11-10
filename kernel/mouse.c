/* AxisOS - PS/2 mouse driver (IRQ12), reported in text-cell coordinates */
#include "axis.h"

static volatile int mx, my;      /* in character cells */
static volatile uint8_t buttons; /* bit0 left, bit1 right, bit2 middle */
static volatile bool moved, present;
static uint8_t packet[3];
static int phase;

static void wait_in(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1) return;
}
static void wait_out(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) return;
}
static void mouse_cmd(uint8_t cmd) {
    wait_out();
    outb(0x64, 0xD4);
    wait_out();
    outb(0x60, cmd);
    wait_in();
    inb(0x60); /* ACK */
}

static void mouse_irq(regs_t *r) {
    (void)r;
    uint8_t st = inb(0x64);
    if (!(st & 1) || !(st & 0x20)) return;
    uint8_t v = inb(0x60);
    switch (phase) {
    case 0:
        if (!(v & 0x08)) return; /* resync: bit3 always set in byte 0 */
        packet[0] = v;
        phase = 1;
        break;
    case 1:
        packet[1] = v;
        phase = 2;
        break;
    case 2: {
        packet[2] = v;
        phase = 0;
        buttons = packet[0] & 7;
        int dx = packet[1], dy = packet[2];
        if (packet[0] & 0x10) dx |= 0xFFFFFF00;
        if (packet[0] & 0x20) dy |= 0xFFFFFF00;
        if (packet[0] & 0xC0) break; /* overflow */
        /* 8 mouse units per character cell feels about right */
        static int ax, ay;
        ax += dx;
        ay -= dy;
        while (ax >= 8) ax -= 8, mx++;
        while (ax <= -8) ax += 8, mx--;
        while (ay >= 8) ay -= 8, my++;
        while (ay <= -8) ay += 8, my--;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= SCREEN_W) mx = SCREEN_W - 1;
        if (my >= SCREEN_H) my = SCREEN_H - 1;
        moved = true;
        break;
    }
    }
}

void mouse_init(void) {
    wait_out();
    outb(0x64, 0xA8); /* enable aux device */
    wait_out();
    outb(0x64, 0x20); /* read config */
    wait_in();
    uint8_t cfg = inb(0x60);
    cfg |= 2;         /* enable IRQ12 */
    cfg &= ~0x20;     /* enable mouse clock */
    wait_out();
    outb(0x64, 0x60);
    wait_out();
    outb(0x60, cfg);
    mouse_cmd(0xF6); /* defaults */
    mouse_cmd(0xF4); /* enable reporting */
    irq_set_handler(12, mouse_irq);
    mx = SCREEN_W / 2;
    my = SCREEN_H / 2;
    present = true;
}

bool mouse_present(void) { return present; }
void mouse_pos(int *x, int *y) {
    *x = mx;
    *y = my;
}
uint8_t mouse_buttons(void) { return buttons; }
bool mouse_poll_moved(void) {
    bool m = moved;
    moved = false;
    return m;
}
void mouse_set(int x, int y) {
    mx = x;
    my = y;
}
