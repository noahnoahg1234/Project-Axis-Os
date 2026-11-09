/* AxisOS - ATA PIO driver (primary master, LBA28) */
#include "axis.h"

#define ATA_DATA 0x1F0
#define ATA_ERR 0x1F1
#define ATA_COUNT 0x1F2
#define ATA_LBA0 0x1F3
#define ATA_LBA1 0x1F4
#define ATA_LBA2 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_CMD 0x1F7
#define ATA_CTRL 0x3F6

static bool present;
static char model[41];
static uint32_t nsect;

static bool wait_not_busy(void) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_CMD);
        if (s == 0xFF) return false;
        if (!(s & 0x80)) return true;
    }
    return false;
}
static bool wait_drq(void) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_CMD);
        if (s & 0x01 || s & 0x20) return false; /* ERR / DF */
        if (!(s & 0x80) && (s & 0x08)) return true;
    }
    return false;
}
static void delay400(void) {
    for (int i = 0; i < 4; i++) inb(ATA_CTRL);
}

bool ata_init(void) {
    present = false;
    outb(ATA_CTRL, 0x02); /* no interrupts */
    if (inb(ATA_CMD) == 0xFF) return false;
    outb(ATA_DRIVE, 0xA0);
    delay400();
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC); /* IDENTIFY */
    if (inb(ATA_CMD) == 0) return false;
    if (!wait_not_busy()) return false;
    if (inb(ATA_LBA1) || inb(ATA_LBA2)) return false; /* not ATA (ATAPI/SATA) */
    if (!wait_drq()) return false;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    for (int i = 0; i < 20; i++) {
        model[i * 2] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;
    nsect = id[60] | ((uint32_t)id[61] << 16);
    present = true;
    return true;
}
bool ata_present(void) { return present; }
const char *ata_model(void) { return model; }
uint32_t ata_sectors(void) { return nsect; }

static bool setup(uint32_t lba, uint8_t count, uint8_t cmd) {
    if (!wait_not_busy()) return false;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    delay400();
    outb(ATA_COUNT, count);
    outb(ATA_LBA0, lba & 0xFF);
    outb(ATA_LBA1, (lba >> 8) & 0xFF);
    outb(ATA_LBA2, (lba >> 16) & 0xFF);
    outb(ATA_CMD, cmd);
    return true;
}

bool ata_read(uint32_t lba, uint32_t count, void *buf) {
    if (!present) return false;
    uint16_t *p = buf;
    while (count) {
        uint8_t n = count > 128 ? 128 : (uint8_t)count;
        if (!setup(lba, n, 0x20)) return false;
        for (int s = 0; s < n; s++) {
            delay400();
            if (!wait_drq()) return false;
            for (int i = 0; i < 256; i++) *p++ = inw(ATA_DATA);
        }
        lba += n;
        count -= n;
    }
    return true;
}

bool ata_write(uint32_t lba, uint32_t count, const void *buf) {
    if (!present) return false;
    const uint16_t *p = buf;
    while (count) {
        uint8_t n = count > 128 ? 128 : (uint8_t)count;
        if (!setup(lba, n, 0x30)) return false;
        for (int s = 0; s < n; s++) {
            delay400();
            if (!wait_drq()) return false;
            for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
        }
        lba += n;
        count -= n;
    }
    outb(ATA_CMD, 0xE7); /* cache flush */
    wait_not_busy();
    return true;
}
