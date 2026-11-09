/* AxisOS - COM1 serial port (mirrors console output, accepts input) */
#include "axis.h"

#define COM1 0x3F8
static bool present;

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* no interrupts */
    outb(COM1 + 3, 0x80); /* DLAB */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xC7); /* FIFO */
    outb(COM1 + 4, 0x1E); /* loopback test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) {
        present = false;
        return;
    }
    outb(COM1 + 4, 0x0F);
    present = true;
}
bool serial_present(void) { return present; }

void serial_putc(char c) {
    if (!present) return;
    for (int i = 0; i < 2000000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (uint8_t)c);
}
void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}
int serial_getc(void) {
    if (!present || !(inb(COM1 + 5) & 1)) return -1;
    return inb(COM1);
}
