/*
 * AxisOS built-in commands.
 *
 * Every command is reachable through its Linux, Windows and FreeBSD name
 * (plus extra aliases), in every system. Option parsing accepts both
 * Unix-style (-l) and Windows-style (/w) switches where it makes sense.
 */
#include "axis.h"

extern const char *cur_cmd_name;
extern const char *theme_names[];
extern const int theme_count;
bool storage_ready(void);
void shell_banner(void);

#define PRINTC(attr, ...) do { uint8_t _s = con_get_attr(); con_set_attr(attr); kprintf(__VA_ARGS__); con_set_attr(_s); } while (0)

static bool is_flag(const char *a) { return (a[0] == '-' && a[1]) || (a[0] == '/' && a[1] && !a[2]); }
static bool has_flag(int argc, char **argv, char f) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '-') {
            for (const char *p = argv[i] + 1; *p; p++)
                if (tolower(*p) == tolower(f)) return true;
        } else if (argv[i][0] == '/' && argv[i][1] && !argv[i][2] && tolower(argv[i][1]) == tolower(f))
            return true;
    }
    return false;
}

/* resolve an existing path, applying autocorrect and reporting it */
static int resolve_existing(const char *path) {
    char corr[256];
    int id = fs_resolve_fuzzy(path, fs_cwd, corr, sizeof corr);
    if (id >= 0 && strcasecmp(corr, path)) PRINTC(theme.accent, "autocorrect: %s -> %s\n", path, corr);
    if (id < 0) {
        shell_print_error(id, path);
        if (id == E_NOENT && cfg.autocorrect == AC_SUGGEST && !adv_mode && strcasecmp(corr, path))
            PRINTC(theme.accent, "Did you mean: %s?\n", corr);
    }
    return id;
}

/* input text: from a file argument or from the pipe */
static const char *input_text(const char *path, size_t *len) {
    if (path) {
        int id = resolve_existing(path);
        if (id < 0) return NULL;
        if (fs_nodes[id].type != FT_FILE) {
            shell_print_error(E_ISDIR, path);
            return NULL;
        }
        *len = fs_nodes[id].size;
        return fs_nodes[id].data ? (const char *)fs_nodes[id].data : "";
    }
    if (shell_stdin) {
        *len = shell_stdin_len;
        return shell_stdin;
    }
    *len = 0;
    return "";
}

static void fmt_time(uint32_t ts, char *buf, size_t n, bool windows) {
    rtc_time_t t;
    rtc_from_unix((uint32_t)((int32_t)ts + cfg.tz_hours * 3600), &t);
    if (windows) {
        if (cfg.clock24) snprintf(buf, n, "%02d/%02d/%04d  %02d:%02d", t.month, t.day, t.year, t.hour, t.min);
        else snprintf(buf, n, "%02d/%02d/%04d  %02d:%02d %s", t.month, t.day, t.year, (t.hour + 11) % 12 + 1, t.min, t.hour < 12 ? "AM" : "PM");
    } else
        snprintf(buf, n, "%s %2d %02d:%02d", rtc_month_name(t.month), t.day, t.hour, t.min);
}

/* ======================================================= help / info ==== */
static int c_help(int argc, char **argv) {
    if (argc > 1) {
        const command_t *c = cmd_find(argv[1]);
        if (!c) {
            shell_print_error(E_NOENT, argv[1]);
            return 1;
        }
        PRINTC(theme.accent, "%s\n", c->desc);
        kprintf("  Usage:   %s\n", c->usage);
        kprintf("  Linux:   %s\n  Windows: %s\n  FreeBSD: %s\n", c->lin[0] ? c->lin : "-", c->win[0] ? c->win : "-", c->bsd[0] ? c->bsd : "-");
        if (c->aliases[0]) kprintf("  Aliases: %s\n", c->aliases);
        if (c->flags & CMD_ADVANCED) PRINTC(theme.error, "  (Advanced mode only)\n");
        return 0;
    }
    if (adv_mode) {
        /* advanced mode: terse, names only */
        int col = 0;
        for (int i = 0; i < command_count; i++) {
            const command_t *c = &commands[i];
            if (c->flags & CMD_HIDDEN) continue;
            const char *n = cur_os == OS_WINDOWS && c->win[0] ? c->win : (cur_os == OS_FREEBSD && c->bsd[0] ? c->bsd : (c->lin[0] ? c->lin : c->win));
            kprintf("%-10s", n);
            if (++col % 8 == 0) kprintf("\n");
        }
        if (col % 8) kprintf("\n");
        return 0;
    }
    PRINTC(theme.accent, "AxisOS commands - every name works in every system, in any letter case.\n");
    PRINTC(theme.prompt, "  %-11s %-11s %-11s %s\n", "Linux", "Windows", "FreeBSD", "Description");
    for (int i = 0; i < command_count; i++) {
        const command_t *c = &commands[i];
        if (c->flags & (CMD_HIDDEN | CMD_ADVANCED)) continue;
        kprintf("  %-11s %-11s %-11s %s\n", c->lin[0] ? c->lin : "-", c->win[0] ? c->win : "-", c->bsd[0] ? c->bsd : "-", c->desc);
    }
    PRINTC(theme.dim, "Type 'help <command>' or '<command> /?' for details. Advanced mode adds expert tools.\n");
    PRINTC(theme.dim, "Use '| more' to page long output, e.g. help | more\n");
    return 0;
}

static int c_uname(int argc, char **argv) {
    const char *n = argv[0];
    if (!strcasecmp(n, "ver")) {
        kprintf("\nAxisOS [Version %s] (%s personality)\n", AXIS_VERSION, os_name(cur_os));
        return 0;
    }
    if (!strcasecmp(n, "freebsd-version")) {
        kprintf("%s-AXIS\n", AXIS_VERSION);
        return 0;
    }
    bool all = has_flag(argc, argv, 'a');
    const char *sys = cur_os == OS_WINDOWS ? "AxisOS-Windows" : (cur_os == OS_FREEBSD ? "AxisOS-FreeBSD" : "AxisOS-Linux");
    if (all)
        kprintf("%s %s %s-axis #1 %s i686 %s\n", sys, cfg.hostname, AXIS_VERSION, cur_os == OS_FREEBSD ? "AXIS" : "SMP", os_name(cur_os));
    else if (has_flag(argc, argv, 'r'))
        kprintf("%s-axis\n", AXIS_VERSION);
    else if (has_flag(argc, argv, 'n'))
        kprintf("%s\n", cfg.hostname);
    else if (has_flag(argc, argv, 'm') || has_flag(argc, argv, 'p'))
        kprintf("i686\n");
    else
        kprintf("%s\n", sys);
    return 0;
}

static int c_fetch(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char brand[49], cpu_v[13];
    cpu_brand(brand);
    cpu_vendor(cpu_v);
    uint32_t total, used;
    heap_stats(&total, &used, NULL);
    uint32_t up = ticks / TICK_HZ;
    int nodes;
    uint32_t bytes;
    fs_stats(&nodes, &bytes);
    if (!strcasecmp(argv[0], "systeminfo")) {
        kprintf("\nHost Name:                 %s\n", cfg.hostname);
        kprintf("OS Name:                   AxisOS %s\n", os_name(cur_os));
        kprintf("OS Version:                %s\n", AXIS_VERSION);
        kprintf("System Type:               x86-based PC\n");
        kprintf("Processor(s):              %s\n", brand);
        kprintf("Total Physical Memory:     %u MB\n", mem_total_kb() / 1024);
        kprintf("Kernel Heap:               %u KB used of %u KB\n", used / 1024, total / 1024);
        kprintf("Boot Device:               BIOS drive 0x%x\n", boot.drive);
        kprintf("System Up Time:            %u h %u min %u s\n", up / 3600, up / 60 % 60, up % 60);
        kprintf("Disk:                      %s\n", ata_present() ? ata_model() : "none (no persistence)");
        return 0;
    }
    static const char *logo[3][7] = {
        {"     .--.     ", "    |o_o |    ", "    |:_/ |    ", "   //   \\ \\   ", "  (|     | )  ", " /'\\_   _/`\\  ", " \\___)=(___/  "},
        {" ##### ##### ", " ##### ##### ", " ##### ##### ", "             ", " ##### ##### ", " ##### ##### ", " ##### ##### "},
        {" /\\,-'''''-,/\\", " \\_)       (_/", " |           |", " |           |", "  ;         ; ", "   '-_____-'  ", "              "}};
    uint8_t lc = cur_os == OS_WINDOWS ? theme.accent : (cur_os == OS_FREEBSD ? ATTR(C_LRED, theme.normal >> 4) : theme.accent);
    char lines[8][80];
    snprintf(lines[0], 80, "%s@%s", env_get("USER"), cfg.hostname);
    snprintf(lines[1], 80, "OS:      AxisOS %s (%s)", AXIS_VERSION, os_name(cur_os));
    snprintf(lines[2], 80, "Kernel:  axis %s i686", AXIS_VERSION);
    snprintf(lines[3], 80, "Uptime:  %u min %u s", up / 60, up % 60);
    snprintf(lines[4], 80, "Shell:   axsh (%s)", cur_os == OS_WINDOWS ? "cmd" : (cur_os == OS_FREEBSD ? "csh" : "bash"));
    snprintf(lines[5], 80, "CPU:     %.40s", brand);
    snprintf(lines[6], 80, "Memory:  %u MB, files: %d (%u bytes)", mem_total_kb() / 1024, nodes, bytes);
    for (int i = 0; i < 7; i++) {
        PRINTC(lc, "%s ", logo[cur_os][i]);
        PRINTC(i == 0 ? theme.prompt : theme.normal, "%s\n", lines[i]);
    }
    return 0;
}

static int c_date(int argc, char **argv) {
    (void)argc;
    rtc_time_t t;
    rtc_local(&t);
    int h12 = (t.hour + 11) % 12 + 1;
    if (!strcasecmp(argv[0], "time")) {
        if (cfg.clock24) kprintf("The current time is: %02d:%02d:%02d\n", t.hour, t.min, t.sec);
        else kprintf("The current time is: %d:%02d:%02d %s\n", h12, t.min, t.sec, t.hour < 12 ? "AM" : "PM");
        return 0;
    }
    if (cur_os == OS_WINDOWS) {
        kprintf("The current date is: %s %02d/%02d/%04d\n", rtc_wday_name(t.wday), t.month, t.day, t.year);
        return 0;
    }
    char tz[16];
    if (cfg.tz_hours) snprintf(tz, sizeof tz, "UTC%c%d", cfg.tz_hours > 0 ? '+' : '-', cfg.tz_hours > 0 ? cfg.tz_hours : -cfg.tz_hours);
    else strcpy(tz, "UTC");
    if (cfg.clock24)
        kprintf("%s %s %2d %02d:%02d:%02d %s %d\n", rtc_wday_name(t.wday), rtc_month_name(t.month), t.day, t.hour, t.min, t.sec, tz, t.year);
    else
        kprintf("%s %s %2d %d:%02d:%02d %s %s %d\n", rtc_wday_name(t.wday), rtc_month_name(t.month), t.day, h12, t.min, t.sec,
                t.hour < 12 ? "AM" : "PM", tz, t.year);
    return 0;
}

static int c_uptime(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint32_t s = ticks / TICK_HZ;
    rtc_time_t t;
    rtc_local(&t);
    kprintf(" %02d:%02d:%02d up %u:%02u:%02u, 1 user, load average: 0.00, 0.00, 0.00\n", t.hour, t.min, t.sec, s / 3600, s / 60 % 60, s % 60);
    return 0;
}

static int c_whoami(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (cur_os == OS_WINDOWS) kprintf("%s\\%s\n", cfg.hostname, env_get("USERNAME"));
    else kprintf("%s\n", env_get("USER"));
    return 0;
}

static int c_hostname(int argc, char **argv) {
    if (argc > 1 && !is_flag(argv[1])) {
        if (!adv_mode && cur_os == OS_WINDOWS) {
            shell_print_error(E_PERM, "use settings to change the computer name");
            return 1;
        }
        strlcpy(cfg.hostname, argv[1], sizeof cfg.hostname);
        config_save();
        return 0;
    }
    kprintf("%s\n", cfg.hostname);
    return 0;
}

static int c_ps(int argc, char **argv) {
    (void)argc;
    uint32_t used;
    heap_stats(NULL, &used, NULL);
    uint32_t s = ticks / TICK_HZ;
    if (!strcasecmp(argv[0], "tasklist")) {
        kprintf("\nImage Name                     PID Session Name        Mem Usage\n");
        kprintf("========================= ======== ================ ============\n");
        kprintf("%-25s %8d %-16s %10u K\n", "System (axiskrnl)", 0, "Services", 512u);
        kprintf("%-25s %8d %-16s %10u K\n", "axsh.exe", 1, "Console", used / 1024);
        return 0;
    }
    kprintf("  PID TTY          TIME CMD\n");
    kprintf("    0 ?        %02u:%02u:%02u [axis-kernel]\n", s / 3600, s / 60 % 60, s % 60);
    kprintf("    1 tty0     %02u:%02u:%02u axsh\n", s / 3600, s / 60 % 60, s % 60);
    return 0;
}

static int c_free(int argc, char **argv) {
    uint32_t total, used, blocks;
    heap_stats(&total, &used, &blocks);
    uint32_t mem = mem_total_kb();
    if (!strcasecmp(argv[0], "mem")) {
        kprintf("\n%10u K total memory\n%10u K kernel heap\n%10u K heap in use\n%10u K heap free\n", mem, total / 1024, used / 1024, (total - used) / 1024);
        return 0;
    }
    if (!strcasecmp(argv[0], "vmstat")) {
        kprintf(" procs    memory (KB)\n r b w     avm     fre\n 1 0 0 %7u %7u\n", used / 1024, (total - used) / 1024);
        return 0;
    }
    bool h = has_flag(argc, argv, 'h');
    uint32_t kused = used / 1024 + 640;
    if (h) {
        kprintf("               total        used        free\n");
        kprintf("Mem:     %10uM %10uM %10uM\n", mem / 1024, kused / 1024, (mem - kused) / 1024);
        kprintf("Heap:    %10uK %10uK %10uK  (%u blocks)\n", total / 1024, used / 1024, (total - used) / 1024, blocks);
    } else {
        kprintf("               total        used        free\n");
        kprintf("Mem:     %11u %11u %11u\n", mem, kused, mem - kused);
        kprintf("Heap:    %11u %11u %11u\n", total / 1024, used / 1024, (total - used) / 1024);
    }
    return 0;
}

static int c_df(int argc, char **argv) {
    (void)argc;
    int nodes;
    uint32_t bytes;
    fs_stats(&nodes, &bytes);
    uint32_t cap = FS_SECTORS * 512;
    if (!strcasecmp(argv[0], "vol")) {
        kprintf(" Volume in drive C is AXISFS\n Volume Serial Number is AX15-0001\n");
        kprintf(" %u bytes used, %u bytes free, %d of %d entries\n", bytes, cap > bytes ? cap - bytes : 0, nodes, FS_MAX_NODES);
        return 0;
    }
    kprintf("Filesystem     1K-blocks   Used  Avail Capacity  Mounted on\n");
    kprintf("axisfs         %9u %6u %6u %7u%%   %s\n", cap / 1024, (bytes + 1023) / 1024, (cap - bytes) / 1024,
            bytes * 100 / cap, cur_os == OS_WINDOWS ? "C:\\" : "/");
    kprintf("  inodes: %d used of %d, persistence: %s\n", nodes, FS_MAX_NODES, storage_ready() ? "disk" : "RAM only");
    return 0;
}

static int c_lspci(int argc, char **argv) {
    bool bsd = !strcasecmp(argv[0], "pciconf");
    (void)argc;
    int n = 0;
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t addr = 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8);
                outl(0xCF8, addr);
                uint32_t id = inl(0xCFC);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                outl(0xCF8, addr | 8);
                uint32_t cls = inl(0xCFC);
                uint8_t c = cls >> 24, sc = (cls >> 16) & 0xFF;
                const char *desc = "Device";
                switch (c) {
                case 1: desc = sc == 1 ? "IDE interface" : "Mass storage controller"; break;
                case 2: desc = "Ethernet controller"; break;
                case 3: desc = "VGA compatible controller"; break;
                case 4: desc = "Multimedia controller"; break;
                case 6: desc = sc == 0 ? "Host bridge" : (sc == 1 ? "ISA bridge" : (sc == 4 ? "PCI bridge" : "Bridge")); break;
                case 0xC: desc = sc == 3 ? "USB controller" : "Serial bus controller"; break;
                }
                if (bsd) kprintf("none%d@pci0:%d:%d:%d: class=0x%06x vendor=0x%04x device=0x%04x  %s\n", n, bus, dev, fn, cls >> 8, id & 0xFFFF, id >> 16, desc);
                else kprintf("%02x:%02x.%d %s [%04x:%04x]\n", bus, dev, fn, desc, id & 0xFFFF, id >> 16);
                n++;
            }
    if (!n) kprintf("no PCI devices found\n");
    return 0;
}

static int c_sysctl(int argc, char **argv) {
    char v[64];
    struct { const char *k; } keys[] = {{"kern.ostype"}, {"kern.osrelease"}, {"kern.hostname"}, {"kern.personality"},
                                        {"kern.boottime"}, {"hw.machine"}, {"hw.model"}, {"hw.ncpu"}, {"hw.physmem"},
                                        {"axis.autocorrect"}, {"axis.advanced"}, {"axis.theme"}};
    int nk = sizeof keys / sizeof *keys;
    bool all = argc < 2 || has_flag(argc, argv, 'a');
    for (int a = 1; a <= argc; a++) {
        const char *want = a < argc ? argv[a] : NULL;
        if (want && is_flag(want)) continue;
        if (!want && !all) break;
        if (want) {
            char *eq = strchr(want, '=');
            if (eq) {
                char k[48];
                int kl = (int)(eq - want);
                if (kl >= 48) kl = 47;
                memcpy(k, want, kl);
                k[kl] = 0;
                if (!strcasecmp(k, "kern.hostname")) {
                    kprintf("kern.hostname: %s -> %s\n", cfg.hostname, eq + 1);
                    strlcpy(cfg.hostname, eq + 1, sizeof cfg.hostname);
                    config_save();
                } else if (!strcasecmp(k, "axis.autocorrect")) {
                    cfg.autocorrect = (uint8_t)(atoi(eq + 1) % 3);
                    config_save();
                    kprintf("axis.autocorrect: %d\n", cfg.autocorrect);
                } else {
                    shell_print_error(E_PERM, k);
                    return 1;
                }
                continue;
            }
        }
        bool found = false;
        for (int i = 0; i < nk; i++) {
            const char *k = keys[i].k;
            if (want && strncasecmp(k, want, strlen(want))) continue;
            found = true;
            if (!strcmp(k, "kern.ostype")) strcpy(v, "AxisOS");
            else if (!strcmp(k, "kern.osrelease")) snprintf(v, sizeof v, "%s-AXIS", AXIS_VERSION);
            else if (!strcmp(k, "kern.hostname")) strlcpy(v, cfg.hostname, sizeof v);
            else if (!strcmp(k, "kern.personality")) strlcpy(v, os_short(cur_os), sizeof v);
            else if (!strcmp(k, "kern.boottime")) snprintf(v, sizeof v, "%u seconds ago", ticks / TICK_HZ);
            else if (!strcmp(k, "hw.machine")) strcpy(v, "i386");
            else if (!strcmp(k, "hw.model")) cpu_brand(v);
            else if (!strcmp(k, "hw.ncpu")) strcpy(v, "1");
            else if (!strcmp(k, "hw.physmem")) snprintf(v, sizeof v, "%u", mem_total_kb() * 1024);
            else if (!strcmp(k, "axis.autocorrect")) snprintf(v, sizeof v, "%d", cfg.autocorrect);
            else if (!strcmp(k, "axis.advanced")) snprintf(v, sizeof v, "%d", adv_mode);
            else snprintf(v, sizeof v, "%s", theme_names[cfg.theme % theme_count]);
            kprintf("%s: %s\n", k, v);
        }
        if (want && !found) {
            kprintf("sysctl: unknown oid '%s'\n", want);
            return 1;
        }
        if (!want) break;
    }
    return 0;
}

/* ======================================================== files ========= */
static void ls_entry(int id, bool lng, bool windows) {
    fs_node_t *n = &fs_nodes[id];
    char t[40];
    if (windows) {
        fmt_time(n->mtime, t, sizeof t, true);
        if (n->type == FT_DIR) kprintf("%s    <DIR>          ", t);
        else kprintf("%s    %14u ", t, n->size);
        PRINTC(n->type == FT_DIR ? theme.path : theme.normal, "%s\n", n->name);
        return;
    }
    if (lng) {
        fmt_time(n->mtime, t, sizeof t, false);
        char perm[11] = "-rw-r--r--";
        if (n->type == FT_DIR) memcpy(perm, "drwxr-xr-x", 10);
        if (n->attr & FA_READONLY) perm[2] = perm[5] = perm[8] = '-';
        kprintf("%s 1 %-6s %-6s %7u %s ", perm, cfg.username, cur_os == OS_FREEBSD ? "wheel" : cfg.username, n->size, t);
    }
    PRINTC(n->type == FT_DIR ? theme.path : theme.normal, "%s%s", n->name, (n->type == FT_DIR && cur_os == OS_FREEBSD) ? "/" : "");
    if (lng) kprintf("\n");
}

static int c_ls(int argc, char **argv) {
    bool windows = !strcasecmp(argv[0], "dir");
    bool lng = has_flag(argc, argv, 'l') || !strcasecmp(argv[0], "ll");
    bool all = cfg.show_hidden || has_flag(argc, argv, 'a') || !strcasecmp(argv[0], "la") || !strcasecmp(argv[0], "ll");
    bool wide = windows && has_flag(argc, argv, 'w');
    bool bare = windows && has_flag(argc, argv, 'b');
    if (windows && wide) windows = true;
    int targets[16], nt = 0;
    char pattern[FS_NAME_MAX] = "";
    for (int i = 1; i < argc && nt < 16; i++) {
        if (is_flag(argv[i])) continue;
        const char *a = argv[i];
        if (strchr(a, '*') || strchr(a, '?')) {
            char leaf[FS_NAME_MAX];
            const char *s1 = strrchr(a, '/'), *s2 = strrchr(a, '\\');
            const char *sep = s1 > s2 ? s1 : s2;
            int dir = fs_cwd;
            if (sep) {
                char d[256];
                int l = (int)(sep - a);
                memcpy(d, a, l);
                d[l] = 0;
                dir = l ? resolve_existing(d) : FS_ROOT;
                if (dir < 0) return 1;
            }
            strlcpy(leaf, sep ? sep + 1 : a, sizeof leaf);
            strlcpy(pattern, leaf, sizeof pattern);
            targets[nt++] = dir;
            continue;
        }
        int id = resolve_existing(a);
        if (id < 0) return 1;
        targets[nt++] = id;
    }
    if (!nt) targets[nt++] = fs_cwd;
    for (int t = 0; t < nt; t++) {
        int id = targets[t];
        if (fs_nodes[id].type == FT_FILE) {
            ls_entry(id, lng, windows);
            if (!lng && !windows) kprintf("\n");
            continue;
        }
        static int kids[FS_MAX_NODES];
        int k = fs_children(id, kids, FS_MAX_NODES, all);
        char p[256];
        fs_path(id, p, sizeof p);
        if (windows && !bare) kprintf("\n Volume in drive C is AXISFS\n Directory of %s\n\n", p);
        else if (nt > 1) kprintf("%s:\n", p);
        int files = 0, dirs = 0, col = 0;
        uint32_t bytes = 0;
        if (windows && !bare && !wide && id != FS_ROOT && !pattern[0]) {
            char tt[40];
            fmt_time(fs_nodes[id].mtime, tt, sizeof tt, true);
            kprintf("%s    <DIR>          .\n%s    <DIR>          ..\n", tt, tt);
        }
        if (lng && !windows) {
            uint32_t tot = 0;
            for (int i = 0; i < k; i++) tot += (fs_nodes[kids[i]].size + 1023) / 1024;
            kprintf("total %u\n", tot);
        }
        for (int i = 0; i < k; i++) {
            fs_node_t *n = &fs_nodes[kids[i]];
            if (pattern[0] && !wildcard_match(pattern, n->name)) continue;
            if (n->type == FT_DIR) dirs++;
            else files++, bytes += n->size;
            if (bare) {
                kprintf("%s\n", n->name);
            } else if (wide) {
                char nm[48];
                snprintf(nm, sizeof nm, n->type == FT_DIR ? "[%s]" : "%s", n->name);
                PRINTC(n->type == FT_DIR ? theme.path : theme.normal, "%-20s", nm);
                if (++col % 4 == 0) kprintf("\n");
            } else if (windows || lng) {
                ls_entry(kids[i], lng, windows);
            } else {
                int w = (int)strlen(n->name) + 2;
                int cw = w < 20 ? 20 : (w < 40 ? 40 : 80);
                if (con_capturing()) {
                    ls_entry(kids[i], false, false);
                    kprintf("\n");
                    continue;
                }
                int x, y;
                con_get_cursor(&x, &y);
                if (x + cw > SCREEN_W && x) kprintf("\n");
                ls_entry(kids[i], false, false);
                con_get_cursor(&x, &y);
                int pad = cw - (int)strlen(n->name) - (n->type == FT_DIR && cur_os == OS_FREEBSD);
                while (pad-- > 0 && x < SCREEN_W - 1) kprintf(" "), x++;
                col++;
            }
        }
        if (wide && col % 4) kprintf("\n");
        if (!windows && !lng && !bare && !con_capturing()) {
            int x, y;
            con_get_cursor(&x, &y);
            if (x) kprintf("\n");
        }
        if (windows && !bare) {
            int nodes;
            uint32_t used;
            fs_stats(&nodes, &used);
            kprintf("%16d File(s) %14u bytes\n%16d Dir(s)  %14u bytes free\n", files, bytes, dirs + (id != FS_ROOT ? 2 : 0),
                    FS_SECTORS * 512 > used ? FS_SECTORS * 512 - used : 0);
        }
    }
    return 0;
}

static int c_cd(int argc, char **argv) {
    const char *target = NULL;
    for (int i = 1; i < argc; i++)
        if (!(argv[i][0] == '/' && (argv[i][1] == 'd' || argv[i][1] == 'D') && !argv[i][2])) target = argv[i];
    if (!target) {
        if (cur_os == OS_WINDOWS) {
            char p[256];
            fs_path(fs_cwd, p, sizeof p);
            kprintf("%s\n", p);
        } else
            fs_cwd = fs_home();
        return 0;
    }
    if (!strcmp(target, "-")) {
        const char *old = env_get("OLDPWD");
        if (!old) return 1;
        target = old;
    }
    int id = resolve_existing(target);
    if (id < 0) return 1;
    if (fs_nodes[id].type != FT_DIR) {
        shell_print_error(E_NOTDIR, target);
        return 1;
    }
    char p[256];
    fs_path(fs_cwd, p, sizeof p);
    env_set("OLDPWD", p);
    fs_cwd = id;
    return 0;
}

static int c_pwd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char p[256];
    fs_path(fs_cwd, p, sizeof p);
    kprintf("%s\n", p);
    return 0;
}

static int c_cat(int argc, char **argv) {
    bool number = has_flag(argc, argv, 'n');
    bool any = false;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i]) && strcmp(argv[i], "-")) continue;
        any = true;
        size_t len;
        const char *t = input_text(argv[i], &len);
        if (!t) {
            rc = 1;
            continue;
        }
        if (number) {
            int ln = 1;
            kprintf("%6d  ", ln++);
            for (size_t k = 0; k < len; k++) {
                con_putc(t[k]);
                if (t[k] == '\n' && k + 1 < len) kprintf("%6d  ", ln++);
            }
        } else
            con_writen(t, len);
        if (len && t[len - 1] != '\n' && !con_capturing()) kprintf("\n");
    }
    if (!any) {
        size_t len;
        const char *t = input_text(NULL, &len);
        con_writen(t, len);
    }
    return rc;
}

static int c_more(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++)
        if (!is_flag(argv[i])) path = argv[i];
    size_t len;
    const char *t = input_text(path, &len);
    if (!t) return 1;
    pager_show(t, len);
    return 0;
}

static int c_echo(int argc, char **argv) {
    int i = 1;
    bool nl = true;
    if (argc > 1 && !strcmp(argv[1], "-n")) nl = false, i++;
    if (argc == 2 && cur_os == OS_WINDOWS && (!strcasecmp(argv[1], "on") || !strcasecmp(argv[1], "off"))) return 0;
    if (argc == 1 && cur_os == OS_WINDOWS) {
        kprintf("ECHO is on.\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], ".")) {
        kprintf("\n");
        return 0;
    }
    for (; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (nl) kprintf("\n");
    return 0;
}

static int c_mkdir(int argc, char **argv) {
    bool parents = has_flag(argc, argv, 'p') || cur_os == OS_WINDOWS;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i])) continue;
        if (parents) {
            /* create every missing component */
            char buf[256];
            strlcpy(buf, argv[i], sizeof buf);
            for (char *p = buf + 1; ; p++) {
                if (*p == '/' || *p == '\\' || !*p) {
                    char c = *p;
                    *p = 0;
                    if (!(isalpha(buf[0]) && buf[1] == ':' && !buf[2]) && fs_resolve(buf, fs_cwd) < 0) {
                        char leaf[FS_NAME_MAX];
                        int par = fs_resolve_parent(buf, fs_cwd, leaf);
                        int r = par < 0 ? par : fs_create(par, leaf, FT_DIR);
                        if (r < 0) {
                            shell_print_error(r, buf);
                            rc = 1;
                            break;
                        }
                    } else if (!c && fs_resolve(buf, fs_cwd) >= 0 && cur_os == OS_WINDOWS && !has_flag(argc, argv, 'p')) {
                        shell_print_error(E_EXIST, argv[i]);
                        rc = 1;
                    }
                    *p = c;
                    if (!c) break;
                }
            }
            continue;
        }
        char leaf[FS_NAME_MAX];
        int par = fs_resolve_parent(argv[i], fs_cwd, leaf);
        int r = par < 0 ? par : fs_create(par, leaf, FT_DIR);
        if (r < 0) {
            shell_print_error(r, argv[i]);
            rc = 1;
        }
    }
    return rc;
}

static int c_rmdir(int argc, char **argv) {
    bool rec = has_flag(argc, argv, 's');
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i])) continue;
        int id = resolve_existing(argv[i]);
        if (id < 0) { rc = 1; continue; }
        if (fs_nodes[id].type != FT_DIR) {
            shell_print_error(E_NOTDIR, argv[i]);
            rc = 1;
            continue;
        }
        if (rec && !has_flag(argc, argv, 'q')) {
            char q[300];
            snprintf(q, sizeof q, "%s, Are you sure", argv[i]);
            if (!shell_confirm(q)) continue;
        }
        int r = fs_delete(id, rec);
        if (r < 0) {
            shell_print_error(r, argv[i]);
            rc = 1;
        }
    }
    return rc;
}

static int delete_match(const char *arg, bool rec, bool force, int *count) {
    if (strchr(arg, '*') || strchr(arg, '?')) {
        const char *s1 = strrchr(arg, '/'), *s2 = strrchr(arg, '\\');
        const char *sep = s1 > s2 ? s1 : s2;
        int dir = fs_cwd;
        if (sep) {
            char d[256];
            int l = (int)(sep - arg);
            memcpy(d, arg, l);
            d[l] = 0;
            dir = l ? fs_resolve(d, fs_cwd) : FS_ROOT;
            if (dir < 0) {
                shell_print_error(dir, d);
                return 1;
            }
        }
        const char *pat = sep ? sep + 1 : arg;
        if (!strcmp(pat, "*") || !strcmp(pat, "*.*")) {
            char p[256], q[300];
            fs_path(dir, p, sizeof p);
            snprintf(q, sizeof q, "%s%s*, Are you sure", p, cur_os == OS_WINDOWS ? "\\" : "/");
            if (!force && !shell_confirm(q)) return 0;
        }
        static int kids[FS_MAX_NODES];
        int k = fs_children(dir, kids, FS_MAX_NODES, true);
        for (int i = 0; i < k; i++) {
            fs_node_t *n = &fs_nodes[kids[i]];
            if (!wildcard_match(pat, n->name)) continue;
            if (n->type == FT_DIR && !rec) continue;
            if (fs_delete(kids[i], rec) == 0) (*count)++;
        }
        return 0;
    }
    int id = force ? fs_resolve(arg, fs_cwd) : resolve_existing(arg);
    if (id < 0) return force ? 0 : 1;
    if (fs_nodes[id].type == FT_DIR && !rec) {
        shell_print_error(E_ISDIR, arg);
        return 1;
    }
    if (fs_nodes[id].type == FT_DIR && !force) {
        char q[300];
        snprintf(q, sizeof q, "Remove directory '%s' and everything in it?", arg);
        if (!shell_confirm(q)) return 0;
    }
    int r = fs_delete(id, rec);
    if (r < 0) {
        shell_print_error(r, arg);
        return 1;
    }
    (*count)++;
    return 0;
}

static int c_rm(int argc, char **argv) {
    bool rec = has_flag(argc, argv, 'r') || has_flag(argc, argv, 's');
    bool force = has_flag(argc, argv, 'f') || has_flag(argc, argv, 'q');
    int rc = 0, count = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i])) continue;
        n++;
        rc |= delete_match(argv[i], rec, force, &count);
    }
    if (!n) {
        kprintf("usage: %s [-rf] file ...\n", argv[0]);
        return 1;
    }
    if (has_flag(argc, argv, 'v')) kprintf("%d removed\n", count);
    return rc;
}

/* copy/move common: last arg is destination */
static int copy_or_move(int argc, char **argv, bool move) {
    char *args[MAX_ARGS];
    int n = 0;
    for (int i = 1; i < argc; i++)
        if (!is_flag(argv[i])) args[n++] = argv[i];
    if (n < 2) {
        if (n == 1 && cur_os == OS_WINDOWS && !move) {
            args[n++] = (char *)".";
        } else {
            kprintf("usage: %s source ... destination\n", argv[0]);
            return 1;
        }
    }
    bool rec = has_flag(argc, argv, 'r') || has_flag(argc, argv, 's') || has_flag(argc, argv, 'e') || !strcasecmp(argv[0], "xcopy");
    const char *dst = args[n - 1];
    int dstid = fs_resolve(dst, fs_cwd);
    bool dst_is_dir = dstid >= 0 && fs_nodes[dstid].type == FT_DIR;
    if (n > 2 && !dst_is_dir) {
        shell_print_error(E_NOTDIR, dst);
        return 1;
    }
    int rc = 0, done = 0;
    for (int i = 0; i < n - 1; i++) {
        int src = resolve_existing(args[i]);
        if (src < 0) { rc = 1; continue; }
        if (fs_nodes[src].type == FT_DIR && !move && !rec) {
            kprintf("%s: %s is a directory (use -r)\n", argv[0], args[i]);
            rc = 1;
            continue;
        }
        int pdir;
        char leaf[FS_NAME_MAX];
        if (dst_is_dir) {
            pdir = dstid;
            strlcpy(leaf, fs_nodes[src].name, sizeof leaf);
        } else {
            pdir = fs_resolve_parent(dst, fs_cwd, leaf);
            if (pdir < 0) {
                shell_print_error(pdir, dst);
                rc = 1;
                continue;
            }
        }
        int r = move ? fs_move(src, pdir, leaf) : fs_copy(src, pdir, leaf);
        if (r < 0) {
            shell_print_error(r, args[i]);
            rc = 1;
        } else
            done++;
    }
    if (cur_os == OS_WINDOWS && !con_capturing())
        kprintf("%9d file(s) %s.\n", done, move ? "moved" : "copied");
    return rc;
}
static int c_cp(int argc, char **argv) { return copy_or_move(argc, argv, false); }
static int c_mv(int argc, char **argv) { return copy_or_move(argc, argv, true); }

static int c_ren(int argc, char **argv) {
    if (argc != 3) {
        kprintf("usage: %s file newname\n", argv[0]);
        return 1;
    }
    int id = resolve_existing(argv[1]);
    if (id < 0) return 1;
    int r = fs_move(id, fs_nodes[id].parent, argv[2]);
    if (r < 0) shell_print_error(r, argv[2]);
    return r < 0;
}

static int c_touch(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i])) continue;
        int id = fs_resolve(argv[i], fs_cwd);
        if (id >= 0) {
            rtc_time_t t;
            rtc_read(&t);
            fs_nodes[id].mtime = rtc_unix(&t);
            fs_dirty = true;
            continue;
        }
        id = fs_mkfile(argv[i], fs_cwd);
        if (id < 0) {
            shell_print_error(id, argv[i]);
            rc = 1;
        }
    }
    return rc;
}

static int c_stat(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int id = resolve_existing(argv[i]);
        if (id < 0) { rc = 1; continue; }
        fs_node_t *n = &fs_nodes[id];
        char p[256], t[40];
        fs_path(id, p, sizeof p);
        fmt_time(n->mtime, t, sizeof t, cur_os == OS_WINDOWS);
        kprintf("  File: %s\n  Size: %u\t\tType: %s\n Inode: %d\tParent: %d\n Attrs: %s%s%s%s\nModify: %s\n", p, n->size,
                n->type == FT_DIR ? "directory" : "regular file", id, n->parent, n->attr & FA_HIDDEN ? "hidden " : "",
                n->attr & FA_READONLY ? "read-only " : "", n->attr & FA_SYSTEM ? "system " : "", n->attr ? "" : "none", t);
    }
    return rc;
}

static int c_attrib(int argc, char **argv) {
    /* attrib +h -r file | chmod 444 file | chflags hidden,uchg file */
    uint8_t set = 0, clr = 0;
    int rc = 0;
    const char *files[MAX_ARGS];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '+' || a[0] == '-') && a[1] && !a[2] && strchr("hrsHRS", a[1])) {
            uint8_t f = tolower(a[1]) == 'h' ? FA_HIDDEN : (tolower(a[1]) == 'r' ? FA_READONLY : FA_SYSTEM);
            if (a[0] == '+') set |= f;
            else clr |= f;
        } else if (isdigit(a[0]) && strlen(a) == 3 && nf == 0) {
            if ((a[0] - '0') & 2) clr |= FA_READONLY;
            else set |= FA_READONLY;
        } else if (!strcasecmp(argv[0], "chflags") && nf == 0 && (strcasestr(a, "hidden") || strcasestr(a, "uchg") || strcasestr(a, "schg"))) {
            if (strcasestr(a, "nohidden")) clr |= FA_HIDDEN;
            else if (strcasestr(a, "hidden")) set |= FA_HIDDEN;
            if (strcasestr(a, "nouchg") || strcasestr(a, "noschg")) clr |= FA_READONLY;
            else if (strcasestr(a, "uchg") || strcasestr(a, "schg")) set |= FA_READONLY;
        } else
            files[nf++] = a;
    }
    if (!nf) {
        static int kids[FS_MAX_NODES];
        int k = fs_children(fs_cwd, kids, FS_MAX_NODES, true);
        for (int i = 0; i < k; i++) {
            fs_node_t *n = &fs_nodes[kids[i]];
            char p[256];
            fs_path(kids[i], p, sizeof p);
            kprintf("%c  %c%c  %s\n", n->type == FT_DIR ? 'D' : 'A', n->attr & FA_HIDDEN ? 'H' : ' ', n->attr & FA_READONLY ? 'R' : ' ', p);
        }
        return 0;
    }
    for (int i = 0; i < nf; i++) {
        int id = resolve_existing(files[i]);
        if (id < 0) { rc = 1; continue; }
        if (!set && !clr) {
            fs_node_t *n = &fs_nodes[id];
            kprintf("%c%c%c  %s\n", n->attr & FA_HIDDEN ? 'H' : '-', n->attr & FA_READONLY ? 'R' : '-', n->attr & FA_SYSTEM ? 'S' : '-', files[i]);
            continue;
        }
        fs_nodes[id].attr = (uint8_t)((fs_nodes[id].attr | set) & ~clr);
        fs_dirty = true;
    }
    return rc;
}

static int c_tree_rec(int id, char *prefix, int depth, bool files, int *nd, int *nf) {
    static int kids[16][FS_MAX_NODES / 4];
    if (depth >= 16) return 0;
    int k = fs_children(id, kids[depth], FS_MAX_NODES / 4, cfg.show_hidden);
    int shown[FS_MAX_NODES / 4], ns = 0;
    for (int i = 0; i < k; i++)
        if (files || fs_nodes[kids[depth][i]].type == FT_DIR) shown[ns++] = kids[depth][i];
    for (int i = 0; i < ns; i++) {
        bool last = i == ns - 1;
        fs_node_t *n = &fs_nodes[shown[i]];
        kprintf("%s%s", prefix, last ? "\xC0\xC4\xC4 " : "\xC3\xC4\xC4 ");
        PRINTC(n->type == FT_DIR ? theme.path : theme.normal, "%s\n", n->name);
        if (n->type == FT_DIR) {
            (*nd)++;
            size_t pl = strlen(prefix);
            if (pl + 5 < 128) {
                strcat(prefix, last ? "    " : "\xB3   ");
                c_tree_rec(shown[i], prefix, depth + 1, files, nd, nf);
                prefix[pl] = 0;
            }
        } else
            (*nf)++;
    }
    return 0;
}
static int c_tree(int argc, char **argv) {
    bool files = cur_os != OS_WINDOWS || has_flag(argc, argv, 'f');
    int id = fs_cwd;
    for (int i = 1; i < argc; i++)
        if (!is_flag(argv[i])) {
            id = resolve_existing(argv[i]);
            if (id < 0) return 1;
        }
    char p[256], prefix[128] = "";
    fs_path(id, p, sizeof p);
    PRINTC(theme.path, "%s\n", p);
    int nd = 0, nf = 0;
    c_tree_rec(id, prefix, 0, files, &nd, &nf);
    if (cur_os != OS_WINDOWS) kprintf("\n%d directories, %d files\n", nd, nf);
    return 0;
}

static int c_grep(int argc, char **argv) {
    bool icase = has_flag(argc, argv, 'i') || !strcasecmp(argv[0], "findstr");
    bool invert = has_flag(argc, argv, 'v');
    bool count = has_flag(argc, argv, 'c');
    bool number = has_flag(argc, argv, 'n');
    icase = true; /* AxisOS: everything is case-insensitive */
    (void)icase;
    const char *pat = NULL;
    const char *files[MAX_ARGS];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i])) continue;
        if (!pat) pat = argv[i];
        else files[nf++] = argv[i];
    }
    if (!pat) {
        kprintf("usage: %s [-vcn] pattern [file ...]\n", argv[0]);
        return 2;
    }
    int matches = 0;
    for (int f = 0; f < (nf ? nf : 1); f++) {
        size_t len;
        const char *t = input_text(nf ? files[f] : NULL, &len);
        if (!t) continue;
        int fc = 0, ln = 0;
        size_t i = 0;
        while (i < len) {
            size_t s = i;
            while (i < len && t[i] != '\n') i++;
            char line[512];
            size_t l = i - s < 511 ? i - s : 511;
            memcpy(line, t + s, l);
            line[l] = 0;
            i++;
            ln++;
            bool hit = strcasestr(line, pat) != NULL;
            if (hit == invert) continue;
            fc++;
            if (count) continue;
            if (nf > 1) PRINTC(theme.path, "%s:", files[f]);
            if (number) PRINTC(theme.accent, "%d:", ln);
            if (!invert && !con_capturing()) {
                /* highlight the match */
                char *m = strcasestr(line, pat);
                size_t pl = strlen(pat);
                con_writen(line, (size_t)(m - line));
                PRINTC(theme.error, "%.*s", (int)pl, m);
                kprintf("%s\n", m + pl);
            } else
                kprintf("%s\n", line);
        }
        if (count) kprintf("%d\n", fc);
        matches += fc;
    }
    return matches ? 0 : 1;
}

static void find_rec(int dir, const char *pat, int *n, bool dirs_too) {
    static int kids[FS_MAX_NODES];
    int ids[FS_MAX_NODES];
    int k = fs_children(dir, kids, FS_MAX_NODES, true);
    memcpy(ids, kids, k * sizeof(int));
    for (int i = 0; i < k; i++) {
        fs_node_t *c = &fs_nodes[ids[i]];
        if (wildcard_match(pat, c->name) && (dirs_too || c->type == FT_FILE)) {
            char p[256];
            fs_path(ids[i], p, sizeof p);
            kprintf("%s\n", p);
            (*n)++;
        }
        if (c->type == FT_DIR) find_rec(ids[i], pat, n, dirs_too);
    }
}
static int c_find(int argc, char **argv) {
    int start = fs_cwd;
    const char *pat = "*";
    bool where = !strcasecmp(argv[0], "where");
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "-name") || !strcasecmp(argv[i], "-iname")) {
            if (i + 1 < argc) pat = argv[++i];
        } else if (!strcasecmp(argv[i], "/r") && i + 1 < argc) {
            start = resolve_existing(argv[++i]);
            if (start < 0) return 1;
        } else if (!is_flag(argv[i])) {
            if (where || (i == argc - 1 && i > 1) || strchr(argv[i], '*')) pat = argv[i];
            else {
                start = resolve_existing(argv[i]);
                if (start < 0) return 1;
            }
        }
    }
    char pp[FS_NAME_MAX + 2];
    if (where && !strchr(pat, '*') && !strchr(pat, '?')) {
        snprintf(pp, sizeof pp, "%s*", pat);
        pat = pp;
    }
    int n = 0;
    find_rec(start, pat, &n, !where);
    if (!n && where) {
        kprintf("INFO: Could not find files for the given pattern(s).\n");
        return 1;
    }
    return 0;
}

static int c_wc(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++)
        if (!is_flag(argv[i])) path = argv[i];
    size_t len;
    const char *t = input_text(path, &len);
    if (!t) return 1;
    uint32_t lines = 0, words = 0;
    bool inw = false;
    for (size_t i = 0; i < len; i++) {
        if (t[i] == '\n') lines++;
        if (isspace(t[i])) inw = false;
        else if (!inw) inw = true, words++;
    }
    if (has_flag(argc, argv, 'l')) kprintf("%u\n", lines);
    else if (has_flag(argc, argv, 'w')) kprintf("%u\n", words);
    else if (has_flag(argc, argv, 'c')) kprintf("%u\n", (uint32_t)len);
    else kprintf("%7u %7u %7u %s\n", lines, words, (uint32_t)len, path ? path : "");
    return 0;
}

static int c_headtail(int argc, char **argv) {
    bool tail = !strcasecmp(argv[0], "tail");
    int n = 10;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (argv[i][0] == '-' && isdigit(argv[i][1])) n = atoi(argv[i] + 1);
        else path = argv[i];
    }
    size_t len;
    const char *t = input_text(path, &len);
    if (!t) return 1;
    int total = 0;
    for (size_t i = 0; i < len; i++)
        if (t[i] == '\n') total++;
    if (len && t[len - 1] != '\n') total++;
    int skip = tail ? (total > n ? total - n : 0) : 0;
    int line = 0;
    size_t i = 0;
    while (i < len && line < skip)
        if (t[i++] == '\n') line++;
    int shown = 0;
    for (; i < len && shown < n; i++) {
        con_putc(t[i]);
        if (t[i] == '\n') shown++;
    }
    if (len && t[len - 1] != '\n' && i >= len) kprintf("\n");
    return 0;
}

static int c_sort(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++)
        if (!is_flag(argv[i])) path = argv[i];
    bool rev = has_flag(argc, argv, 'r');
    size_t len;
    const char *t = input_text(path, &len);
    if (!t) return 1;
    char *copy = kmalloc(len + 1);
    if (!copy) return 1;
    memcpy(copy, t, len);
    copy[len] = 0;
    char *lines[512];
    int n = 0;
    char *p = copy;
    while (*p && n < 512) {
        lines[n++] = p;
        char *e = strchr(p, '\n');
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    for (int i = 1; i < n; i++) {
        char *x = lines[i];
        int j = i;
        while (j > 0 && (rev ? strcasecmp(lines[j - 1], x) < 0 : strcasecmp(lines[j - 1], x) > 0)) lines[j] = lines[j - 1], j--;
        lines[j] = x;
    }
    for (int i = 0; i < n; i++) kprintf("%s\n", lines[i]);
    kfree(copy);
    return 0;
}

static int c_hexdump(int argc, char **argv) {
    const char *path = argc > 1 ? argv[argc - 1] : NULL;
    if (path && is_flag(path)) path = NULL;
    size_t len;
    const char *t = input_text(path, &len);
    if (!t) return 1;
    for (size_t off = 0; off < len; off += 16) {
        kprintf("%08x  ", (uint32_t)off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len) kprintf("%02x ", (uint8_t)t[off + i]);
            else kprintf("   ");
            if (i == 7) kprintf(" ");
        }
        kprintf(" |");
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            char c = t[off + i];
            con_putc(c >= 32 && c < 127 ? c : '.');
        }
        kprintf("|\n");
    }
    kprintf("%08x\n", (uint32_t)len);
    return 0;
}

/* ===================================================== apps & system ==== */
static int c_clear(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!con_capturing()) {
        con_set_attr(theme.normal);
        con_clear();
        serial_write("\x1b[2J\x1b[H");
    }
    return 0;
}

static int c_edit(int argc, char **argv) {
    if (con_capturing()) return 1;
    return editor_run(argc > 1 ? argv[1] : NULL);
}
static int c_explorer(int argc, char **argv) {
    if (con_capturing()) return 1;
    return explorer_run(argc > 1 ? argv[1] : NULL);
}
static int c_settings(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (con_capturing()) return 1;
    return settings_run();
}

static int c_os(int argc, char **argv) {
    int target = -1;
    const char *n = argv[0];
    if (!strcasecmp(n, "wsl") || !strcasecmp(n, "bash")) target = OS_LINUX;
    else if (!strcasecmp(n, "cmd") || !strcasecmp(n, "powershell")) target = OS_WINDOWS;
    else if (!strcasecmp(n, "bsd") || !strcasecmp(n, "csh") || !strcasecmp(n, "tcsh")) target = OS_FREEBSD;
    else if (argc > 1) {
        const char *a = argv[1];
        if (!strncasecmp(a, "lin", 3) || !strcasecmp(a, "l") || !strcasecmp(a, "1")) target = OS_LINUX;
        else if (!strncasecmp(a, "win", 3) || !strcasecmp(a, "w") || !strcasecmp(a, "2")) target = OS_WINDOWS;
        else if (strcasestr(a, "bsd") || !strncasecmp(a, "free", 4) || !strcasecmp(a, "f") || !strcasecmp(a, "3")) target = OS_FREEBSD;
        else {
            /* autocorrect the system name too */
            const char *names[] = {"linux", "windows", "freebsd"};
            int best = -1, bd = 100;
            for (int i = 0; i < 3; i++) {
                int d = edit_distance(a, names[i]);
                if (d < bd) bd = d, best = i;
            }
            if (bd <= ac_threshold(a) && !adv_mode && cfg.autocorrect) target = best;
        }
    }
    if (target < 0) {
        kprintf("Current system: %s%s\n", os_name(cur_os), adv_mode ? " (advanced mode)" : "");
        kprintf("usage: os linux | windows | freebsd   (shortcuts: wsl, cmd, bsd)\n");
        return argc > 1;
    }
    cur_os = target;
    theme_apply();
    if (fs_nodes[fs_cwd].parent >= 0) {
        /* move to the new system's home if we were at the old one */
        int lh = fs_lookup(FS_ROOT, "home"), wh = fs_lookup(FS_ROOT, "Users");
        int par = fs_nodes[fs_cwd].parent;
        if ((par == lh || par == wh) && !strcasecmp(fs_nodes[fs_cwd].name, cfg.username)) fs_cwd = fs_home();
    }
    con_clear();
    shell_banner();
    return 0;
}

static int c_advanced(int argc, char **argv) {
    bool on;
    if (!strcasecmp(argv[0], "su") || !strcasecmp(argv[0], "sudo")) {
        if (!strcasecmp(argv[0], "sudo") && argc > 1) {
            /* sudo <cmd>: run one command in advanced mode */
            bool prev = adv_mode;
            adv_mode = true;
            char line[256] = "";
            for (int i = 1; i < argc; i++) {
                strlcpy(line + strlen(line), argv[i], sizeof line - strlen(line));
                strlcpy(line + strlen(line), " ", sizeof line - strlen(line));
            }
            int rc = shell_exec_line(line);
            adv_mode = prev;
            return rc;
        }
        on = true;
    } else if (argc > 1)
        on = !strcasecmp(argv[1], "on") || !strcmp(argv[1], "1") || !strcasecmp(argv[1], "enable");
    else
        on = !adv_mode;
    if (on && !adv_mode) {
        PRINTC(theme.error, "WARNING: Advanced mode. Autocorrect, confirmations and hints are off.\n");
        PRINTC(theme.error, "Expert tools enabled: peek poke inb outb sector cpuid heap dmesg hexdump panic.\n");
        PRINTC(theme.dim, "Leave with 'exit' or 'advanced off'.\n");
    } else if (!on && adv_mode)
        PRINTC(theme.accent, "Advanced mode off.\n");
    adv_mode = on;
    return 0;
}

static int c_exit(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (adv_mode) {
        adv_mode = false;
        PRINTC(theme.accent, "Left advanced mode.\n");
        return 0;
    }
    kprintf("There is nothing to log out to. Use 'shutdown' to power off or 'reboot' to restart.\n");
    return 0;
}

static int c_history(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "-c"))) return 0;
    int n = shell_history_count();
    for (int i = 0; i < n; i++) kprintf("%5d  %s\n", i + 1, shell_history_get(i));
    return 0;
}

static int c_set(int argc, char **argv) {
    bool csh = !strcasecmp(argv[0], "setenv");
    if (argc == 1) {
        static char names[64][32], vals[64][128];
        int n = env_list(names, vals, 64);
        for (int i = 0; i < n; i++) kprintf("%s=%s\n", names[i], vals[i]);
        return 0;
    }
    if (csh) {
        env_set(argv[1], argc > 2 ? argv[2] : "");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && tolower(argv[i][1]) == 'a') {
            /* set /a expr */
            continue;
        }
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = env_get(argv[i]);
            if (v) kprintf("%s=%s\n", argv[i], v);
            else if (!strcasecmp(argv[0], "export")) env_set(argv[i], "");
            else {
                kprintf("Environment variable %s not defined\n", argv[i]);
                return 1;
            }
            continue;
        }
        *eq = 0;
        if (!eq[1]) env_unset(argv[i]);
        else env_set(argv[i], eq + 1);
    }
    return 0;
}
static int c_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) env_unset(argv[i]);
    return 0;
}

static int c_alias(int argc, char **argv) {
    if (argc == 1) {
        alias_list();
        return 0;
    }
    /* alias ll='ls -l'  |  doskey ll=ls -l  |  alias ll ls -l (csh) */
    char line[256] = "";
    for (int i = 1; i < argc; i++) {
        strlcpy(line + strlen(line), argv[i], sizeof line - strlen(line));
        if (i + 1 < argc) strlcpy(line + strlen(line), " ", sizeof line - strlen(line));
    }
    char *eq = strchr(line, '=');
    char *name = line, *val;
    if (eq) {
        *eq = 0;
        val = eq + 1;
    } else {
        char *sp = strchr(line, ' ');
        if (!sp) {
            kprintf("usage: %s name=command\n", argv[0]);
            return 1;
        }
        *sp = 0;
        val = sp + 1;
    }
    size_t vl = strlen(val);
    if (vl >= 2 && (val[0] == '\'' || val[0] == '"') && val[vl - 1] == val[0]) {
        val[vl - 1] = 0;
        val++;
    }
    if (alias_set(name, val) < 0) {
        shell_print_error(E_NOSPC, "alias table full");
        return 1;
    }
    return 0;
}
static int c_unalias(int argc, char **argv) {
    for (int i = 1; i < argc; i++) alias_set(argv[i], NULL);
    return 0;
}

static int c_color(int argc, char **argv) {
    if (argc < 2) {
        theme_apply();
        return 0;
    }
    const char *a = argv[1];
    static const char *names[16] = {"black", "blue", "green", "cyan", "red", "magenta", "brown", "gray",
                                    "darkgray", "lightblue", "lightgreen", "lightcyan", "lightred", "pink", "yellow", "white"};
    if (strlen(a) == 2 && isxdigit(a[0]) && isxdigit(a[1])) {
        int bg = isdigit(a[0]) ? a[0] - '0' : tolower(a[0]) - 'a' + 10;
        int fg = isdigit(a[1]) ? a[1] - '0' : tolower(a[1]) - 'a' + 10;
        if (bg == fg) return 1;
        theme.normal = ATTR(fg, bg);
        theme.prompt = theme.path = theme.accent = ATTR(fg, bg);
        theme.error = ATTR(bg == C_RED ? C_YELLOW : C_LRED, bg);
        theme.dim = ATTR(fg, bg);
        con_set_attr(theme.normal);
        con_clear();
        return 0;
    }
    for (int i = 0; i < 16; i++)
        if (!strcasecmp(a, names[i])) {
            theme.normal = ATTR(i, theme.normal >> 4);
            con_set_attr(theme.normal);
            return 0;
        }
    kprintf("usage: color <bg><fg> (hex, e.g. 0a or 1f) or color <name>\n");
    return 1;
}

static int c_sleep(int argc, char **argv) {
    int s = 1;
    for (int i = 1; i < argc; i++)
        if (isdigit(argv[i][0])) s = atoi(argv[i]);
        else if (!strcasecmp(argv[i], "/t") && i + 1 < argc) s = atoi(argv[++i]);
    uint32_t end = ticks + (uint32_t)s * TICK_HZ;
    while ((int32_t)(end - ticks) > 0) {
        int k = kbd_trygetkey();
        if (k == KEY_CTRL('c') || (k >= 0 && !strcasecmp(argv[0], "timeout"))) break;
        cpu_hlt();
    }
    return 0;
}

static void power_prepare(void) {
    if (fs_dirty) {
        kprintf("Syncing disks... ");
        kprintf(fs_save() ? "done\n" : "skipped (no AxisOS disk)\n");
    }
}
static int c_reboot(int argc, char **argv) {
    (void)argc;
    (void)argv;
    power_prepare();
    kprintf("Rebooting...\n");
    sleep_ms(300);
    sys_reboot();
    return 0;
}
static int c_shutdown(int argc, char **argv) {
    if (has_flag(argc, argv, 'r')) return c_reboot(argc, argv);
    power_prepare();
    if (cur_os == OS_WINDOWS) kprintf("Shutting down...\n");
    else kprintf("The system is going down for power-off NOW!\n");
    sleep_ms(300);
    sys_poweroff();
    return 0;
}
static int c_sync(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!storage_ready()) {
        shell_print_error(E_IO, "no AxisOS disk found - files live in RAM only");
        return 1;
    }
    if (!fs_save()) {
        shell_print_error(E_NOSPC, "filesystem too large to save");
        return 1;
    }
    if (!con_capturing() && argv[0][0] != 's') kprintf("Filesystem saved.\n");
    return 0;
}

/* tiny integer calculator: + - * / % ( ) */
static const char *cp;
static int calc_err;
static long calc_expr(void);
static void calc_ws(void) { while (*cp == ' ') cp++; }
static long calc_atom(void) {
    calc_ws();
    if (*cp == '(') {
        cp++;
        long v = calc_expr();
        calc_ws();
        if (*cp == ')') cp++;
        else calc_err = 1;
        return v;
    }
    if (*cp == '-') {
        cp++;
        return -calc_atom();
    }
    if (!isdigit(*cp)) {
        calc_err = 1;
        return 0;
    }
    return strtol(cp, &cp, 0);
}
static long calc_term(void) {
    long v = calc_atom();
    for (;;) {
        calc_ws();
        char op = *cp;
        if (op != '*' && op != '/' && op != '%') return v;
        cp++;
        long r = calc_atom();
        if ((op == '/' || op == '%') && r == 0) {
            calc_err = 2;
            return 0;
        }
        v = op == '*' ? v * r : (op == '/' ? v / r : v % r);
    }
}
static long calc_expr(void) {
    long v = calc_term();
    for (;;) {
        calc_ws();
        if (*cp == '+') cp++, v += calc_term();
        else if (*cp == '-') cp++, v -= calc_term();
        else return v;
    }
}
bool calc_eval(const char *expr, long *out) {
    cp = expr;
    calc_err = 0;
    long v = calc_expr();
    calc_ws();
    if (*cp || calc_err) return false;
    *out = v;
    return true;
}

static int c_calc(int argc, char **argv) {
    char e[256] = "";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && tolower(argv[i][1]) == 'a') continue;
        strlcpy(e + strlen(e), argv[i], sizeof e - strlen(e));
    }
    if (!e[0]) {
        kprintf("usage: %s 2+3*(4-1)\n", argv[0]);
        return 1;
    }
    cp = e;
    calc_err = 0;
    long v = calc_expr();
    calc_ws();
    if (*cp) calc_err = 1;
    if (calc_err) {
        shell_print_error(E_INVAL, calc_err == 2 ? "division by zero" : "syntax error");
        return 1;
    }
    kprintf("%d\n", (int)v);
    return 0;
}

/* ================================================== advanced mode ======= */
static uint32_t parse_num(const char *s) { return (uint32_t)strtol(s, NULL, 0); }

static int c_peek(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: peek <addr> [count]\n");
        return 1;
    }
    uint32_t a = parse_num(argv[1]);
    uint32_t n = argc > 2 ? parse_num(argv[2]) : 64;
    if (n > 4096) n = 4096;
    if (a >= mem_total_kb() * 1024 && a < 0xA0000) return 1;
    const uint8_t *p = (const uint8_t *)a;
    for (uint32_t off = 0; off < n; off += 16) {
        kprintf("%08x: ", a + off);
        for (uint32_t i = 0; i < 16 && off + i < n; i++) kprintf("%02x ", p[off + i]);
        kprintf("\n");
    }
    return 0;
}
static int c_poke(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: poke <addr> <byte> [byte ...]\n");
        return 1;
    }
    uint8_t *p = (uint8_t *)parse_num(argv[1]);
    for (int i = 2; i < argc; i++) *p++ = (uint8_t)parse_num(argv[i]);
    return 0;
}
static int c_inb(int argc, char **argv) {
    if (argc < 2) return 1;
    kprintf("0x%02x\n", inb((uint16_t)parse_num(argv[1])));
    return 0;
}
static int c_outb(int argc, char **argv) {
    if (argc < 3) return 1;
    outb((uint16_t)parse_num(argv[1]), (uint8_t)parse_num(argv[2]));
    return 0;
}
static int c_sector(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: sector <lba>   (dump one disk sector)\n");
        return 1;
    }
    static uint8_t buf[512];
    if (!ata_read(parse_num(argv[1]), 1, buf)) {
        shell_print_error(E_IO, argv[1]);
        return 1;
    }
    for (int off = 0; off < 512; off += 16) {
        kprintf("%03x: ", off);
        for (int i = 0; i < 16; i++) kprintf("%02x", buf[off + i]);
        kprintf("  ");
        for (int i = 0; i < 16; i++) con_putc(buf[off + i] >= 32 && buf[off + i] < 127 ? buf[off + i] : '.');
        kprintf("\n");
    }
    return 0;
}
static int c_cpuid(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char v[13], b[49];
    cpu_vendor(v);
    cpu_brand(b);
    uint32_t a, bb, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(bb), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    kprintf("vendor %s\nbrand  %s\nfamily %u model %u stepping %u\nfeatures edx=%08x ecx=%08x\n", v, b, (a >> 8) & 0xF,
            (a >> 4) & 0xF, a & 0xF, d, c);
    kprintf("flags ");
    static const char *f[] = {"fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce", "cx8", "apic", "", "sep", "mtrr", "pge",
                              "mca", "cmov", "pat", "pse36", "", "", "", "", "", "mmx", "fxsr", "sse", "sse2"};
    for (int i = 0; i < 27; i++)
        if ((d >> i) & 1 && f[i][0]) kprintf("%s ", f[i]);
    kprintf("\n");
    return 0;
}
static int c_heap(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint32_t t, u, b;
    heap_stats(&t, &u, &b);
    extern char __bss_end[];
    kprintf("kernel   0x00010000-0x%08x\nheap     0x00200000-0x%08x  %u KB, %u KB used, %u blocks\n", (uint32_t)__bss_end,
            0x200000 + t, t / 1024, u / 1024, b);
    kprintf("vga      0x000b8000\nbootinfo 0x00000500  settings copy 0x00000600\n");
    return 0;
}
static int c_dmesg(int argc, char **argv) {
    (void)argc;
    (void)argv;
    con_write(klog_buffer());
    return 0;
}
static int c_panic(int argc, char **argv) {
    (void)argc;
    (void)argv;
    kprintf("Triggering a kernel exception...\n");
    /* a real divide-by-zero (C code would be optimised away) */
    __asm__ volatile("xor %%ecx, %%ecx\n\tmov $1, %%eax\n\txor %%edx, %%edx\n\tdiv %%ecx" ::: "eax", "ecx", "edx");
    return 0;
}
static int c_format(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!shell_confirm("Erase every file and recreate the default filesystem?")) return 0;
    fs_format();
    fs_cwd = fs_home();
    fs_save();
    kprintf("Filesystem formatted.\n");
    return 0;
}


/* ======================================================= network ======== */
static int c_ifconfig(int argc, char **argv) {
    if (!net_present()) {
        kprintf("No network card found. In QEMU the default card (e1000) works.\n");
        return 1;
    }
    if (argc > 1 && (!strcasecmp(argv[1], "/renew") || !strcasecmp(argv[1], "renew") || !strcasecmp(argv[1], "-r"))) {
        kprintf(net_renew() ? "DHCP lease renewed.\n" : "No DHCP server answered.\n");
    }
    char ip[16], mask[16], gw[16], dns[16];
    ip_to_str(net_ip, ip);
    ip_to_str(net_mask, mask);
    ip_to_str(net_gw, gw);
    ip_to_str(net_dns, dns);
    uint32_t rx, tx;
    net_stats(&rx, &tx);
    if (!strcasecmp(argv[0], "ipconfig")) {
        kprintf("\nAxisOS IP Configuration\n\nEthernet adapter eth0:\n\n");
        kprintf("   Description . . . . . . . . . . : %s\n", net_nic_name());
        kprintf("   Physical Address. . . . . . . . : %02X-%02X-%02X-%02X-%02X-%02X\n", net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5]);
        kprintf("   IPv4 Address. . . . . . . . . . : %s\n   Subnet Mask . . . . . . . . . . : %s\n", ip, mask);
        kprintf("   Default Gateway . . . . . . . . : %s\n   DNS Servers . . . . . . . . . . : %s\n", gw, dns);
        return 0;
    }
    kprintf("%s: flags=8843<UP,BROADCAST,RUNNING,MULTICAST> mtu 1500\n", cur_os == OS_FREEBSD ? "em0" : "eth0");
    kprintf("        inet %s  netmask %s  gateway %s  dns %s\n", ip, mask, gw, dns);
    kprintf("        ether %02x:%02x:%02x:%02x:%02x:%02x  (%s)\n", net_mac[0], net_mac[1], net_mac[2], net_mac[3], net_mac[4], net_mac[5], net_nic_name());
    kprintf("        RX packets %u  TX packets %u\n", rx, tx);
    return 0;
}

static int c_ping(int argc, char **argv) {
    const char *host = NULL;
    int count = 4;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcasecmp(argv[i], "-n") || !strcasecmp(argv[i], "/n")) && i + 1 < argc) count = atoi(argv[++i]);
        else if (!is_flag(argv[i])) host = argv[i];
    }
    if (!host) {
        kprintf("usage: ping [-c count] host\n");
        return 1;
    }
    uint32_t ip;
    if (!net_resolve(host, &ip)) {
        shell_print_error(E_NOENT, host);
        return 1;
    }
    char ips[16];
    ip_to_str(ip, ips);
    bool win = cur_os == OS_WINDOWS;
    if (win) kprintf("\nPinging %s [%s] with 32 bytes of data:\n", host, ips);
    else kprintf("PING %s (%s): 32 data bytes\n", host, ips);
    int ok = 0;
    for (int i = 0; i < count; i++) {
        int rtt = net_ping(ip, (uint16_t)(i + 1), 2000);
        if (rtt >= 0) {
            ok++;
            if (win) kprintf("Reply from %s: bytes=32 time=%dms TTL=64\n", ips, rtt);
            else kprintf("40 bytes from %s: icmp_seq=%d ttl=64 time=%d ms\n", ips, i, rtt);
        } else
            kprintf(win ? "Request timed out.\n" : "Request timeout for icmp_seq %d\n", i);
        if (kbd_trygetkey() == KEY_CTRL('c')) break;
        if (i + 1 < count) sleep_ms(500);
    }
    kprintf("--- %s: %d sent, %d received, %d%% loss\n", host, count, ok, count ? (count - ok) * 100 / count : 0);
    return ok ? 0 : 1;
}

static int c_nslookup(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: nslookup host\n");
        return 1;
    }
    /* nslookup host [server] */
    uint32_t ip, saved = net_dns, server;
    const char *host = argv[1];
    if (argc > 2) {
        if (!str_to_ip(argv[2], &server)) {
            shell_print_error(E_INVAL, argv[2]);
            return 1;
        }
        net_dns = server;
    }
    char dns[16], ips[16];
    ip_to_str(net_dns, dns);
    kprintf("Server:  %s\n", dns);
    bool ok = net_resolve(host, &ip);
    net_dns = saved;
    if (!ok) {
        kprintf("** server can't find %s\n", host);
        return 1;
    }
    ip_to_str(ip, ips);
    kprintf("Name:    %s\nAddress: %s\n", host, ips);
    return 0;
}

static void dl_progress(uint32_t got, uint32_t total) {
    static uint32_t last;
    if (con_capturing() || ticks - last < 20) return;
    last = ticks;
    if (total) kprintf("\r  %u / %u bytes (%u%%)   ", got, total, total ? got * 100 / total : 0);
    else kprintf("\r  %u bytes   ", got);
}

static int c_wget(int argc, char **argv) {
    const char *url = NULL, *out = NULL;
    bool curl = !strcasecmp(argv[0], "curl");
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-O") || !strcmp(argv[i], "-o")) && i + 1 < argc) out = argv[++i];
        else if (!is_flag(argv[i])) url = argv[i];
    }
    if (!url) {
        kprintf("usage: %s http://host/path [-o file]\n", argv[0]);
        return 1;
    }
    if (!strncasecmp(url, "https://", 8)) {
        shell_print_error(E_INVAL, "https is not supported - use an http:// address");
        return 1;
    }
    char full[512];
    if (!strstr(url, "://")) snprintf(full, sizeof full, "http://%s", url);
    else strlcpy(full, url, sizeof full);
    http_resp_t r;
    int st = 0;
    for (int hops = 0; hops < 5; hops++) {
        if (!curl && !con_capturing()) kprintf("Fetching %s\n", full);
        st = http_get(full, &r, (curl || con_capturing()) ? NULL : dl_progress);
        if (st >= 300 && st < 400 && r.location[0] && strncasecmp(r.location, "https://", 8)) {
            if (r.location[0] == '/') {
                char host[128], path[512];
                uint16_t port;
                url_split(full, host, sizeof host, &port, path, sizeof path);
                snprintf(full, sizeof full, "http://%s:%u%s", host, port, r.location);
            } else
                strlcpy(full, r.location, sizeof full);
            kfree(r.body);
            continue;
        }
        break;
    }
    if (!curl && !con_capturing()) kprintf("\n");
    if (st < 0) {
        shell_print_error(E_IO, http_strerror(st));
        return 1;
    }
    if (st >= 300 && st < 400) {
        kfree(r.body);
        shell_print_error(E_INVAL, "redirected to an https:// address, which AxisOS cannot fetch");
        return 1;
    }
    if (curl && !out) {
        con_writen(r.body, r.len);
        kfree(r.body);
        return st >= 400;
    }
    char name[FS_NAME_MAX];
    if (out) strlcpy(name, out, sizeof name);
    else {
        const char *q = strrchr(full + 7, '/');
        strlcpy(name, q && q[1] ? q + 1 : "index.html", sizeof name);
        char *qq = strchr(name, '?');
        if (qq) *qq = 0;
        if (!fs_valid_name(name)) strcpy(name, "download.html");
    }
    int id = fs_mkfile(name, fs_cwd);
    int w = id < 0 ? id : fs_write(id, r.body, r.len);
    kfree(r.body);
    if (w < 0) {
        shell_print_error(w, name);
        return 1;
    }
    if (!con_capturing()) kprintf("HTTP %d - saved %u bytes to %s\n", st, r.len, name);
    return st >= 400;
}

static int c_browser(int argc, char **argv) {
    if (con_capturing()) return 1;
    return browser_run(argc > 1 ? argv[1] : NULL);
}
static int c_store(int argc, char **argv) {
    if (con_capturing()) return 1;
    return store_run();
}
static int c_shello(int argc, char **argv) {
    if (con_capturing()) return 1;
    return shello_run();
}
static int c_apps(int argc, char **argv) { return apps_list(); }
static int c_runscript(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: %s script [args]\n", argv[0]);
        return 1;
    }
    return script_run(argv[1], argc - 1, argv + 1);
}


static int c_netconf(int argc, char **argv) {
    (void)argc; (void)argv;
    if (con_capturing()) return 1;
    return netconf_run();
}
static int c_desktop(int argc, char **argv) {
    (void)argc; (void)argv;
    if (con_capturing()) return 1;
    return desktop_run();
}
static int c_ai(int argc, char **argv) {
    (void)argc; (void)argv;
    if (con_capturing()) return 1;
    return ai_run();
}
static int c_axar(int argc, char **argv) { return axar_cmd(argc, argv); }
static int c_highpower(int argc, char **argv) {
    if (argc > 1) compat_high_power = !strcasecmp(argv[1], "on") || !strcasecmp(argv[1], "1") || !strcasecmp(argv[1], "enable");
    else compat_high_power = !compat_high_power;
    PRINTC(theme.accent, "High Power mode is %s.\n", compat_high_power ? "ON (programs get a large memory arena)" : "off");
    if (compat_high_power) PRINTC(theme.dim, "Note: this raises the memory limit for programs; it cannot speed up the CPU itself.\n");
    return 0;
}
static int c_osname(int argc, char **argv) {
    static const char *names[] = {"the app's own system", "Linux", "Windows", "FreeBSD"};
    if (argc > 1) {
        if (!strcasecmp(argv[1], "auto") || !strcasecmp(argv[1], "app")) compat_report_os = -1;
        else if (strcasestr(argv[1], "lin")) compat_report_os = OS_LINUX;
        else if (strcasestr(argv[1], "win")) compat_report_os = OS_WINDOWS;
        else if (strcasestr(argv[1], "free") || strcasestr(argv[1], "bsd")) compat_report_os = OS_FREEBSD;
        else { kprintf("usage: osname auto|linux|windows|freebsd\n"); return 1; }
    }
    kprintf("Programs are told they are running on: %s\n", compat_report_os < 0 ? names[0] : names[compat_report_os + 1]);
    return 0;
}
static int c_runprog(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: %s program [args]\n", argv[0]); return 1; }
    if (con_capturing()) return 1;
    if (!compat_is_program(argv[1])) {
        /* maybe it's a path; let compat_run report the real reason */
    }
    return compat_run(argv[1], argc - 1, argv + 1);
}

/* ======================================================= table ========== */
const command_t commands[] = {
    /* linux      windows      freebsd      aliases                       fn            usage                          description */
    {"help",      "help",      "help",      "?|man|commands",             c_help,       "help [command]",              "Show commands or help for one command", 0},
    {"ls",        "dir",       "ls",        "ll|la",                    c_ls,         "ls [-la] [path] | dir [/w /b] [path]", "List directory contents", 0},
    {"cd",        "cd",        "cd",        "chdir",                      c_cd,         "cd [path | .. | ~ | -]",      "Change the current directory", 0},
    {"pwd",       "",          "pwd",       "",                           c_pwd,        "pwd",                         "Print the current directory", 0},
    {"cat",       "type",      "cat",       "",                           c_cat,        "cat [-n] file ...",           "Print files", 0},
    {"less",      "more",      "more",      "",                           c_more,       "more file | cmd | more",      "Show text one screen at a time", 0},
    {"head",      "",          "head",      "",                           c_headtail,   "head [-n N] [file]",          "Show the first lines", 0},
    {"tail",      "",          "tail",      "",                           c_headtail,   "tail [-n N] [file]",          "Show the last lines", 0},
    {"echo",      "echo",      "echo",      "print",                      c_echo,       "echo [-n] text",              "Print text", 0},
    {"touch",     "",          "touch",     "new",                        c_touch,      "touch file ...",              "Create an empty file / update time", 0},
    {"mkdir",     "md",        "mkdir",     "",                           c_mkdir,      "mkdir [-p] dir ...",          "Create directories", 0},
    {"rmdir",     "rd",        "rmdir",     "",                           c_rmdir,      "rmdir [/s] dir ...",          "Remove directories", 0},
    {"rm",        "del",       "rm",        "erase|delete",               c_rm,         "rm [-rf] file ... | del [/s /q] file", "Delete files (wildcards allowed)", 0},
    {"cp",        "copy",      "cp",        "xcopy",                      c_cp,         "cp [-r] src ... dest",        "Copy files and directories", 0},
    {"mv",        "move",      "mv",        "",                           c_mv,         "mv src ... dest",             "Move or rename", 0},
    {"",          "ren",       "",          "rename",                     c_ren,        "ren file newname",            "Rename a file in place", 0},
    {"stat",      "",          "stat",      "fileinfo",                   c_stat,       "stat file ...",               "Show file details", 0},
    {"chmod",     "attrib",    "chflags",   "",                           c_attrib,     "attrib [+h -h +r -r] file | chmod 444 file | chflags uchg file", "Change file attributes", 0},
    {"tree",      "tree",      "tree",      "",                           c_tree,       "tree [/f] [path]",            "Show the directory tree", 0},
    {"grep",      "findstr",   "grep",      "search",                     c_grep,       "grep [-vcn] pattern [file ...]", "Search text (case-insensitive)", 0},
    {"find",      "where",     "find",      "locate",                     c_find,       "find [path] -name pattern | where pattern", "Find files by name", 0},
    {"wc",        "",          "wc",        "",                           c_wc,         "wc [-lwc] [file]",            "Count lines, words and bytes", 0},
    {"sort",      "sort",      "sort",      "",                           c_sort,       "sort [-r] [file]",            "Sort lines", 0},
    {"nano",      "edit",      "ee",        "vi|vim|notepad|pico|editor", c_edit,       "nano [file]",                 "Full-screen text editor", 0},
    {"files",     "explorer",  "files",     "fm|mc|nautilus|filemanager", c_explorer,   "explorer [path]",             "AxisOS file explorer", 0},
    {"settings",  "control",   "bsdconfig", "config|setup|preferences",   c_settings,   "settings",                    "Open the settings app", 0},
    {"os",        "os",        "os",        "switch|wsl|cmd|bsd|bash|powershell|csh|tcsh", c_os, "os linux|windows|freebsd", "Switch the system personality", 0},
    {"advanced",  "advanced",  "advanced",  "adv|su|sudo|expert",         c_advanced,   "advanced [on|off] | sudo cmd", "Toggle Advanced mode", 0},
    {"ifconfig",  "ipconfig",  "ifconfig",  "ip|netstat",                 c_ifconfig,   "ifconfig | ipconfig [/renew]", "Show network settings", 0},
    {"ping",      "ping",      "ping",      "",                           c_ping,       "ping [-c N] host",            "Check that a host answers", 0},
    {"nslookup",  "nslookup",  "host",      "dig|resolve",                c_nslookup,   "nslookup host [server]",      "Look up a host name (DNS)", 0},
    {"wget",      "curl",      "fetch",     "download|curl",              c_wget,       "wget http://url [-o file]",   "Download a file from the web", 0},
    {"browser",   "msedge",    "browser",   "web|lynx|w3m|links|firefox|edge|iexplore|www", c_browser, "browser [url | search words]", "Text web browser", 0},
    {"store",     "store",     "pkg",       "appstore|apt|winget|ms-store|software", c_store, "store",                 "App Store: find and install apps", 0},
    {"apps",      "apps",      "apps",      "installed",                  c_apps,       "apps",                        "List installed apps", 0},
    {"desktop",   "desktop",   "desktop",   "gui|start|wm",               c_desktop,    "desktop",                     "Open the retro mouse desktop", 0},
    {"assistant", "assistant", "assistant", "ai|helper|help-me|clippy|cortana", c_ai,    "assistant",                   "Axis Assistant (offline helper)", 0},
    {"netconf",   "netsh",     "netconf",   "network-setup|wifi|netsetup", c_netconf,    "netconf",                     "Network settings (IP, DHCP)", 0},
    {"axar",      "winrar",    "axar",      "rar|archive|tar|zip|unzip|7z", c_axar,      "axar a|x|l archive files...", "AxisRAR archiver (pack/extract)", 0},
    {"highpower", "highpower", "highpower", "hp|turbo|powermode",         c_highpower,  "highpower [on|off]",          "Larger memory for heavy programs", 0},
    {"osname",    "osname",    "osname",    "reportos|nameconvert",       c_osname,     "osname auto|linux|windows|freebsd", "What OS programs think they're on", 0},
    {"runprog",   "runprog",   "runprog",   "linux|wine|exec|launch",     c_runprog,    "runprog ./program [args]",    "Run a Linux/FreeBSD program", 0},
    {"shello",    "shello",    "shello",    "",                           c_shello,     "shello",                      "Shello: the retro shell", 0},
    {"sh",        "call",      "sh",        "run|source|start",           c_runscript,  "sh script.sh | call script.bat", "Run a script file", 0},
    {"clear",     "cls",       "clear",     "reset",                      c_clear,      "clear",                       "Clear the screen", 0},
    {"uname",     "ver",       "uname",     "freebsd-version|version",    c_uname,      "uname [-a]",                  "Show system version", 0},
    {"neofetch",  "systeminfo","fetch",     "sysinfo|about|winver",       c_fetch,      "neofetch",                    "Show system information", 0},
    {"date",      "date",      "date",      "time",                       c_date,       "date | time",                 "Show date and time", 0},
    {"uptime",    "",          "uptime",    "",                           c_uptime,     "uptime",                      "Show how long the system has run", 0},
    {"whoami",    "whoami",    "whoami",    "id",                         c_whoami,     "whoami",                      "Show the current user", 0},
    {"hostname",  "hostname",  "hostname",  "",                           c_hostname,   "hostname [name]",             "Show or set the host name", 0},
    {"ps",        "tasklist",  "ps",        "top",                c_ps,         "ps",                          "List processes", 0},
    {"free",      "mem",       "vmstat",    "",                           c_free,       "free [-h]",                   "Show memory usage", 0},
    {"df",        "vol",       "df",        "du",                         c_df,         "df",                          "Show disk usage", 0},
    {"lspci",     "",          "pciconf",   "devices",                    c_lspci,      "lspci | pciconf -l",          "List PCI devices", 0},
    {"sysctl",    "",          "sysctl",    "",                           c_sysctl,     "sysctl [-a] [name[=value]]",  "Show or set kernel state", 0},
    {"env",       "set",       "setenv",    "export|printenv",            c_set,        "set NAME=value | export NAME=value | setenv NAME value", "Environment variables", 0},
    {"unset",     "",          "unsetenv",  "",                           c_unset,      "unset NAME",                  "Remove an environment variable", 0},
    {"alias",     "doskey",    "alias",     "",                           c_alias,      "alias ll='ls -l' | doskey ll=dir", "Define command aliases", 0},
    {"unalias",   "",          "unalias",   "",                           c_unalias,    "unalias name",                "Remove an alias", 0},
    {"history",   "",          "history",   "",                           c_history,    "history",                     "Show command history", 0},
    {"",          "color",     "",          "colour",                     c_color,      "color 0a | color yellow",     "Change console colours", 0},
    {"expr",      "calc",      "expr",      "bc",                         c_calc,       "calc 2+3*4",                  "Integer calculator", 0},
    {"sleep",     "timeout",   "sleep",     "wait",                       c_sleep,      "sleep N | timeout /t N",      "Wait N seconds", 0},
    {"sync",      "sync",      "sync",      "save",                       c_sync,       "sync",                        "Save the filesystem to disk", 0},
    {"reboot",    "",          "reboot",    "restart",                    c_reboot,     "reboot",                      "Restart the computer", 0},
    {"poweroff",  "shutdown",  "shutdown",  "halt",                       c_shutdown,   "shutdown [-r | /r]",          "Power off (or restart with -r)", 0},
    {"exit",      "exit",      "exit",      "logout",                     c_exit,       "exit",                        "Leave advanced mode", 0},
    /* advanced mode only */
    {"hexdump",   "format-hex","hexdump",   "xxd|od",                     c_hexdump,    "hexdump file",                "Hex dump a file", CMD_ADVANCED},
    {"peek",      "",          "",          "",                           c_peek,       "peek addr [count]",           "Dump physical memory", CMD_ADVANCED},
    {"poke",      "",          "",          "",                           c_poke,       "poke addr byte ...",          "Write physical memory", CMD_ADVANCED},
    {"inb",       "",          "",          "",                           c_inb,        "inb port",                    "Read an I/O port", CMD_ADVANCED},
    {"outb",      "",          "",          "",                           c_outb,       "outb port value",             "Write an I/O port", CMD_ADVANCED},
    {"sector",    "",          "",          "dd",                         c_sector,     "sector lba",                  "Dump a raw disk sector", CMD_ADVANCED},
    {"cpuid",     "",          "",          "lscpu",                      c_cpuid,      "cpuid",                       "Show CPU identification", CMD_ADVANCED},
    {"heap",      "",          "",          "memmap|kmem",                c_heap,       "heap",                        "Show the kernel memory map", CMD_ADVANCED},
    {"dmesg",     "",          "dmesg",     "eventvwr",                   c_dmesg,      "dmesg",                       "Show the kernel log", CMD_ADVANCED},
    {"mkfs",      "format",    "newfs",     "",                           c_format,     "format",                      "Erase and recreate the filesystem", CMD_ADVANCED},
    {"panic",     "",          "",          "crash|bsod",                 c_panic,      "panic",                       "Crash the kernel (test crash screen)", CMD_ADVANCED},
};
int command_count = sizeof commands / sizeof *commands;
