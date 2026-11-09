/* AxisOS - persistent settings shared with the AxisBoot boot manager */
#include "axis.h"

config_t cfg;
bootinfo_t boot;
static bool storage_checked, storage_ok;

/* Only ever write to a disk that carries the AxisOS MBR signature. */
bool storage_ready(void) {
    if (storage_checked) return storage_ok;
    storage_checked = true;
    storage_ok = false;
    if (!ata_present()) return false;
    static uint8_t mbr[512];
    if (!ata_read(0, 1, mbr)) return false;
    storage_ok = !memcmp(mbr + 0x1B0, "AXISDISK", 8) && mbr[510] == 0x55 && mbr[511] == 0xAA;
    return storage_ok;
}

void config_defaults(void) {
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.magic, "AXCF", 4);
    cfg.version = 1;
    cfg.default_os = OS_LINUX;
    cfg.timeout = 5;
    cfg.theme = 0;
    cfg.autocorrect = AC_AUTO;
    cfg.advanced = 0;
    cfg.autosave = 1;
    cfg.show_hidden = 0;
    cfg.clock24 = 1;
    cfg.tz_hours = 0;
    cfg.confirm = 1;
    strcpy(cfg.hostname, "axis");
    strcpy(cfg.username, "user");
}

static bool memchr_ok(const char *s, int n) {
    for (int i = 0; i < n; i++)
        if (!s[i]) return true;
    return false;
}

static bool sane(const config_t *c) {
    return !memcmp(c->magic, "AXCF", 4) && c->version == 1 && c->default_os < OS_COUNT &&
           c->hostname[0] && c->username[0] && memchr_ok(c->hostname, 32) && memchr_ok(c->username, 32);
}

void config_load(void) {
    const config_t *fromboot = (const config_t *)0x600;
    if (sane(fromboot)) {
        memcpy(&cfg, fromboot, sizeof cfg);
        return;
    }
    static uint8_t sec[512];
    if (storage_ready() && ata_read(CONFIG_LBA, 1, sec) && sane((config_t *)sec)) {
        memcpy(&cfg, sec, sizeof cfg);
        return;
    }
    config_defaults();
}

bool config_save(void) {
    if (!storage_ready()) return false;
    static uint8_t sec[512];
    memset(sec, 0, sizeof sec);
    memcpy(sec, &cfg, sizeof cfg);
    memcpy((void *)0x600, &cfg, sizeof cfg);
    return ata_write(CONFIG_LBA, 1, sec);
}
