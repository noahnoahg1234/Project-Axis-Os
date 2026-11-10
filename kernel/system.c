/* AxisOS - system personality, power management, kernel log, CPU info */
#include "axis.h"

int cur_os = OS_LINUX;
bool adv_mode = false;
bool compat_high_power = false;

static const char *names[OS_COUNT] = {"Axis/Linux", "Axis/Windows", "Axis/FreeBSD", "Axis/DOS", "Axis/Unix"};
static const char *shorts[OS_COUNT] = {"linux", "windows", "freebsd", "dos", "unix"};
const char *os_name(int os) { return names[os % OS_COUNT]; }
bool os_win_style(void) { return cur_os == OS_WINDOWS || cur_os == OS_DOS; }
bool os_dos(void) { return cur_os == OS_DOS; }
bool os_unix_style(void) { return cur_os == OS_LINUX || cur_os == OS_FREEBSD || cur_os == OS_UNIX; }
const char *os_short(int os) { return shorts[os % OS_COUNT]; }

void sys_reboot(void) {
    irq_off();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE); /* pulse reset line */
    for (int i = 0; i < 1000000; i++) io_wait();
    /* triple fault fallback */
    struct __attribute__((packed)) { uint16_t l; uint32_t b; } z = {0, 0};
    __asm__ volatile("lidt %0; int $3" ::"m"(z));
    for (;;) cpu_hlt();
}

void sys_poweroff(void) {
    irq_off();
    outw(0x604, 0x2000);  /* QEMU (q35/piix ACPI) */
    outw(0xB004, 0x2000); /* Bochs / old QEMU */
    outw(0x4004, 0x3400); /* VirtualBox */
    for (;;) cpu_hlt();
}

/* ---- kernel log (dmesg) ---------------------------------------------- */
static char logbuf[4096];
static size_t loglen;
void klog(const char *fmt, ...) {
    char line[160];
    int n = snprintf(line, sizeof line, "[%5u.%02u] ", ticks / TICK_HZ, ticks % TICK_HZ);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    size_t l = strlen(line);
    if (loglen + l + 2 >= sizeof logbuf) return;
    memcpy(logbuf + loglen, line, l);
    loglen += l;
    logbuf[loglen++] = '\n';
    logbuf[loglen] = 0;
}
const char *klog_buffer(void) { return logbuf; }

/* ---- CPU ------------------------------------------------------------- */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
void cpu_vendor(char *out) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(out, &b, 4);
    memcpy(out + 4, &d, 4);
    memcpy(out + 8, &c, 4);
    out[12] = 0;
}
void cpu_brand(char *out) {
    uint32_t a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000004) {
        cpu_vendor(out);
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, &a, &b, &c, &d);
        memcpy(out + i * 16, &a, 4);
        memcpy(out + i * 16 + 4, &b, 4);
        memcpy(out + i * 16 + 8, &c, 4);
        memcpy(out + i * 16 + 12, &d, 4);
    }
    out[48] = 0;
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}
uint32_t mem_total_kb(void) { return boot.mem_kb + 1024; }
