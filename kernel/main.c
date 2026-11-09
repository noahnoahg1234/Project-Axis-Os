/* AxisOS - kernel main */
#include "axis.h"

bool storage_ready(void);

static void boot_line(const char *msg) {
    if (cur_os == OS_LINUX) {
        uint8_t s = con_get_attr();
        kprintf("[");
        con_set_attr(ATTR(C_LGREEN, C_BLACK));
        kprintf("  OK  ");
        con_set_attr(s);
        kprintf("] %s\n", msg);
    } else if (cur_os == OS_FREEBSD) {
        kprintf("%s\n", msg);
    }
    klog("%s", msg);
}

static void windows_splash(void) {
    con_set_attr(ATTR(C_WHITE, C_BLACK));
    con_clear();
    con_show_cursor(false);
    uint8_t lc[4] = {ATTR(C_LRED, C_BLACK), ATTR(C_LGREEN, C_BLACK), ATTR(C_LBLUE, C_BLACK), ATTR(C_YELLOW, C_BLACK)};
    for (int q = 0; q < 4; q++)
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 6; x++) scr_put(34 + (q % 2) * 7 + x, 6 + (q / 2) * 4 + y, (char)0xDB, lc[q]);
    scr_puts(35, 15, "AxisOS", ATTR(C_WHITE, C_BLACK));
    scr_puts(31, 16, "Windows personality", ATTR(C_LGRAY, C_BLACK));
    for (int i = 0; i < 5; i++) {
        for (int d = 0; d < 5; d++) scr_put(37 + d, 19, d == i ? (char)0x07 : ' ', ATTR(C_WHITE, C_BLACK));
        sleep_ms(80);
    }
    con_show_cursor(true);
}

void shell_banner(void) {
    uint8_t s = con_get_attr();
    if (cur_os == OS_WINDOWS) {
        kprintf("AxisOS [Version %s]  -  Windows personality\n", AXIS_VERSION);
        kprintf("Commands like dir, type, copy, del, cls work here - and so do ls, cat, rm.\n\n");
    } else if (cur_os == OS_FREEBSD) {
        con_set_attr(theme.accent);
        kprintf("AxisOS %s-AXIS (FreeBSD personality)\n", AXIS_VERSION);
        con_set_attr(s);
        kprintf("Welcome to Axis/FreeBSD! sysctl, ee, chflags, pciconf and every Linux and\n");
        kprintf("Windows command are available too.\n\n");
    } else {
        con_set_attr(theme.accent);
        kprintf("AxisOS %s (Axis/Linux) tty0\n", AXIS_VERSION);
        con_set_attr(s);
        int m = fs_resolve("/etc/motd", FS_ROOT);
        if (m >= 0 && fs_nodes[m].size) con_writen((char *)fs_nodes[m].data, fs_nodes[m].size);
        kprintf("\n");
    }
    con_set_attr(theme.dim);
    kprintf("Type 'help' for commands, 'explorer' for files, 'settings' to customise.\n");
    if (!storage_ready()) kprintf("Note: no AxisOS disk detected - files are kept in RAM only.\n");
    con_set_attr(s);
    if (adv_mode) {
        con_set_attr(theme.error);
        kprintf("ADVANCED MODE: no autocorrect, no confirmations, expert commands unlocked.\n");
        con_set_attr(s);
    }
    kprintf("\n");
}

void kmain(void) {
    uint8_t bi[16];
    for (int i = 0; i < 16; i++) bi[i] = ((volatile const uint8_t *)0x500)[i];
    if (!memcmp(bi, "AXBI", 4)) {
        boot.os = bi[4] < OS_COUNT ? bi[4] : 0;
        boot.advanced = bi[5] & 1;
        boot.drive = bi[6];
        memcpy(&boot.mem_kb, bi + 8, 4);
    } else {
        boot.os = OS_LINUX;
        boot.mem_kb = 31 * 1024;
    }
    if (boot.mem_kb < 4096) boot.mem_kb = 31 * 1024;

    serial_init();
    cur_os = boot.os;
    adv_mode = boot.advanced;
    con_set_attr(ATTR(C_LGRAY, C_BLACK));
    con_init();

    uint32_t heap_end = (boot.mem_kb + 1024) * 1024;
    if (heap_end > 256u * 1024 * 1024) heap_end = 256u * 1024 * 1024;
    heap_init(0x200000, heap_end);
    idt_init();
    kbd_init();
    irq_on();

    if (cur_os == OS_WINDOWS) windows_splash();
    con_set_attr(ATTR(C_LGRAY, C_BLACK));
    con_clear();
    if (cur_os == OS_FREEBSD) {
        kprintf("AxisBoot: loaded kernel /boot/kernel/axis (FreeBSD personality)\n");
        kprintf("AxisOS %s-AXIS i386\n", AXIS_VERSION);
    }
    klog("AxisOS %s booting as %s", AXIS_VERSION, os_name(cur_os));

    char line[96];
    snprintf(line, sizeof line, "Memory: %u KB available, kernel heap at 0x200000", mem_total_kb());
    boot_line(line);
    boot_line("Interrupts, PIC and 100 Hz timer initialised");
    boot_line("PS/2 keyboard ready");
    if (serial_present()) boot_line("Serial console on COM1 (115200 8N1)");
    if (ata_init()) {
        snprintf(line, sizeof line, "Disk: %s (%u MB)", ata_model(), ata_sectors() / 2048);
        boot_line(line);
    } else
        boot_line("No ATA disk found - running from RAM");
    config_load();
    boot_line(storage_ready() ? "Settings loaded from AxisOS disk" : "Using default settings");
    fs_init();
    boot_line("AxisFS mounted (case-insensitive, unified Linux/Windows/FreeBSD tree)");
    boot_line("Starting axsh");
    if (cur_os != OS_WINDOWS) sleep_ms(250);

    theme_apply();
    con_clear();
    shell_banner();
    shell_main();
}
