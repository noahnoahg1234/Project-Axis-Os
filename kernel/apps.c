/* AxisOS - text UI helpers, the pager and the Settings app */
#include "axis.h"

extern const char *theme_names[];
extern const int theme_count;
bool storage_ready(void);

/* ---- helpers ------------------------------------------------------------ */
int tui_input(int x, int y, int w, char *buf, size_t n, uint8_t attr) {
    size_t len = strlen(buf);
    con_show_cursor(true);
    for (;;) {
        int off = (int)len >= w ? (int)len - w + 1 : 0;
        scr_putsn(x, y, buf + off, w, attr);
        con_set_cursor(x + (int)len - off, y);
        int k = kbd_getkey();
        if (k == '\n') return 1;
        if (k == KEY_ESC || k == KEY_CTRL('c')) return 0;
        if (k == '\b') {
            if (len) buf[--len] = 0;
        } else if (k == KEY_CTRL('u')) {
            len = 0;
            buf[0] = 0;
        } else if (k >= 32 && k < 127 && len + 1 < n) {
            buf[len++] = (char)k;
            buf[len] = 0;
        }
    }
}

static void center_box(int w, int h, const char *title, int *x, int *y) {
    *x = (SCREEN_W - w) / 2;
    *y = (SCREEN_H - h) / 2;
    scr_box(*x, *y, w, h, theme.bar, title);
}

bool tui_confirm(const char *title, const char *msg) {
    int w = (int)strlen(msg) + 6;
    if (w < 30) w = 30;
    if (w > 76) w = 76;
    int x, y;
    center_box(w, 6, title, &x, &y);
    scr_putsn(x + 3, y + 2, msg, w - 6, theme.bar);
    scr_puts(x + (w - 18) / 2, y + 4, "[Y] Yes    [N] No", theme.bar);
    for (;;) {
        int k = kbd_getkey();
        if (k == 'y' || k == 'Y' || k == '\n') return true;
        if (k == 'n' || k == 'N' || k == KEY_ESC) return false;
    }
}

void tui_message(const char *title, const char *msg) {
    int w = (int)strlen(msg) + 6;
    if (w < 30) w = 30;
    if (w > 76) w = 76;
    int x, y;
    center_box(w, 6, title, &x, &y);
    scr_putsn(x + 3, y + 2, msg, w - 6, theme.bar);
    scr_puts(x + (w - 14) / 2, y + 4, "[ OK - any key ]", theme.bar);
    kbd_getkey();
}

/* prompt in a box; returns 1 on Enter */
int tui_prompt(const char *title, const char *label, char *buf, size_t n) {
    int x, y;
    center_box(60, 6, title, &x, &y);
    scr_puts(x + 2, y + 2, label, theme.bar);
    scr_fill(x + 2, y + 3, 56, 1, ' ', theme.select);
    return tui_input(x + 2, y + 3, 56, buf, n, theme.select);
}

/* ---- pager -------------------------------------------------------------- */
void pager_show(const char *t, size_t len) {
    if (con_capturing()) {
        con_writen(t, len);
        return;
    }
    int rows = 0, col = 0;
    for (size_t i = 0; i < len; i++) {
        con_putc(t[i]);
        if (t[i] == '\n' || ++col >= SCREEN_W) {
            col = 0;
            rows++;
        }
        if (rows >= SCREEN_H - 1 && i + 1 < len) {
            uint8_t s = con_get_attr();
            con_set_attr(theme.bar);
            con_serial_mirror(false);
            int pct = (int)((i + 1) * 100 / len);
            kprintf(" -- More -- (%d%%)  Space: page  Enter: line  Q: quit ", pct);
            con_set_attr(s);
            int k = kbd_getkey();
            int x, y;
            con_get_cursor(&x, &y);
            for (int c = 0; c < SCREEN_W; c++) scr_put(c, y, ' ', s);
            con_set_cursor(0, y);
            con_serial_mirror(true);
            if (k == 'q' || k == 'Q' || k == KEY_ESC || k == KEY_CTRL('c')) return;
            rows = (k == '\n' || k == KEY_DOWN) ? SCREEN_H - 2 : 0;
        }
    }
    if (len && t[len - 1] != '\n') con_putc('\n');
}

/* ---- settings ----------------------------------------------------------- */
enum {
    S_DEFOS, S_TIMEOUT, S_THEME, S_AUTOCORRECT, S_ADVANCED, S_CONFIRM, S_AUTOSAVE, S_HIDDEN,
    S_CLOCK, S_TZ, S_HOST, S_USER, S_SYSNOW, S_SAVE, S_RESETFS, S_CANCEL, S_COUNT
};
static const char *labels[S_COUNT] = {
    "Default system at boot", "Boot menu timeout", "Colour theme", "Autocorrect", "Advanced mode at boot",
    "Ask before deleting", "Save files to disk automatically", "Show hidden files", "Clock format", "Time zone",
    "Computer name", "User name", "Current system (this session)", "> Save and exit", "> Reset files to factory defaults",
    "> Exit without saving"};
static const char *helps[S_COUNT] = {
    "The system AxisBoot selects by default. You can still pick another one in the boot menu.",
    "Seconds AxisBoot waits before starting the default system. 0 waits for a key press.",
    "Auto uses the colours of the running system. The others apply everywhere.",
    "Off: exact commands only. Suggest: show 'Did you mean'. Automatic: fix typos in commands and paths.",
    "Start in Advanced mode: expert commands, no autocorrect, no confirmations. Harder, more powerful.",
    "Ask for confirmation before deleting directories or wildcard matches.",
    "Write the filesystem to the AxisOS disk after every command that changes files.",
    "Show files marked hidden (and dot-files) in ls, dir and the explorer.",
    "Show times in 12-hour (AM/PM) or 24-hour format.",
    "Offset from UTC applied to date, time and file times.",
    "Name of this computer (hostname). Enter to edit.",
    "Your user name. Home folders /home/<name> and C:\\Users\\<name> are created as needed.",
    "Switch between Axis/Linux, Axis/Windows and Axis/FreeBSD without rebooting.",
    "Store these settings in the settings sector so AxisBoot and AxisOS use them next time.",
    "Erase all files and recreate the default folders. This cannot be undone.",
    "Close without changing anything."};

static void value_str(int i, const config_t *c, int os, char *out, size_t n) {
    static const char *onoff[] = {"Off", "On"};
    static const char *ac[] = {"Off", "Suggest", "Automatic"};
    switch (i) {
    case S_DEFOS: strlcpy(out, os_name(c->default_os), n); break;
    case S_TIMEOUT:
        if (c->timeout) snprintf(out, n, "%u s", c->timeout);
        else strlcpy(out, "wait for key", n);
        break;
    case S_THEME: strlcpy(out, theme_names[c->theme % theme_count], n); break;
    case S_AUTOCORRECT: strlcpy(out, ac[c->autocorrect % 3], n); break;
    case S_ADVANCED: strlcpy(out, onoff[c->advanced & 1], n); break;
    case S_CONFIRM: strlcpy(out, onoff[c->confirm & 1], n); break;
    case S_AUTOSAVE: strlcpy(out, onoff[c->autosave & 1], n); break;
    case S_HIDDEN: strlcpy(out, onoff[c->show_hidden & 1], n); break;
    case S_CLOCK: strlcpy(out, c->clock24 ? "24-hour" : "12-hour", n); break;
    case S_TZ:
        if (c->tz_hours) snprintf(out, n, "UTC%c%d", c->tz_hours > 0 ? '+' : '-', c->tz_hours > 0 ? c->tz_hours : -c->tz_hours);
        else strlcpy(out, "UTC", n);
        break;
    case S_HOST: strlcpy(out, c->hostname, n); break;
    case S_USER: strlcpy(out, c->username, n); break;
    case S_SYSNOW: strlcpy(out, os_name(os), n); break;
    default: out[0] = 0;
    }
}

static void change(int i, config_t *c, int *os, int d) {
    switch (i) {
    case S_DEFOS: c->default_os = (uint8_t)((c->default_os + OS_COUNT + d) % OS_COUNT); break;
    case S_TIMEOUT: {
        int t = c->timeout + d;
        c->timeout = (uint8_t)(t < 0 ? 30 : (t > 30 ? 0 : t));
        break;
    }
    case S_THEME: c->theme = (uint8_t)((c->theme + theme_count + d) % theme_count); break;
    case S_AUTOCORRECT: c->autocorrect = (uint8_t)((c->autocorrect + 3 + d) % 3); break;
    case S_ADVANCED: c->advanced ^= 1; break;
    case S_CONFIRM: c->confirm ^= 1; break;
    case S_AUTOSAVE: c->autosave ^= 1; break;
    case S_HIDDEN: c->show_hidden ^= 1; break;
    case S_CLOCK: c->clock24 ^= 1; break;
    case S_TZ: {
        int t = c->tz_hours + d;
        c->tz_hours = (int8_t)(t < -12 ? 14 : (t > 14 ? -12 : t));
        break;
    }
    case S_SYSNOW: *os = (*os + OS_COUNT + d) % OS_COUNT; break;
    }
}

static void preview_theme(const config_t *c, int os) {
    config_t save = cfg;
    int so = cur_os;
    cfg.theme = c->theme;
    cur_os = os;
    theme_apply();
    cfg = save;
    cur_os = so;
}

static void settings_draw(const config_t *c, int os, int sel, const char *msg) {
    scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
    scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
    scr_puts(2, 0, adv_mode ? "AxisOS Settings  [advanced]" : "AxisOS Settings", theme.bar);
    scr_puts(SCREEN_W - 2 - (int)strlen(os_name(os)), 0, os_name(os), theme.bar);
    const char *heads[] = {"Boot", "Appearance & behaviour", "Identity", "Actions"};
    int heady[] = {2, 5, 14, 18};
    for (int h = 0; h < 4; h++) scr_puts(2, heady[h], heads[h], theme.accent);
    int rowmap[S_COUNT] = {3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 19, 20, 21};
    for (int i = 0; i < S_COUNT; i++) {
        int y = rowmap[i];
        uint8_t a = i == sel ? theme.select : theme.normal;
        scr_fill(3, y, 74, 1, ' ', a);
        scr_puts(5, y, labels[i], a);
        char v[48];
        value_str(i, c, os, v, sizeof v);
        if (v[0]) {
            bool cyc = i != S_HOST && i != S_USER;
            scr_printf(42, y, a, cyc ? "< %s >" : "[%s]", v);
        }
    }
    scr_fill(0, 22, SCREEN_W, 1, ' ', theme.normal);
    scr_putsn(2, 22, msg && *msg ? msg : helps[sel], 76, msg && *msg ? theme.accent : theme.dim);
    scr_fill(0, 24, SCREEN_W, 1, ' ', theme.bar);
    if (!adv_mode) scr_puts(1, 24, "Up/Down: choose   Left/Right: change   Enter: edit / select   S: save   Esc: exit", theme.bar);
    else scr_printf(1, 24, theme.bar, "cfg@LBA%d fs@LBA%d..%d  disk:%s", CONFIG_LBA, FS_LBA, FS_LBA + FS_SECTORS - 1, storage_ready() ? "axis" : "none");
}

int settings_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    config_t c = cfg;
    int os = cur_os, sel = 0;
    char msg[96] = "";
    int rc = 0;
    for (;;) {
        preview_theme(&c, os);
        settings_draw(&c, os, sel, msg);
        msg[0] = 0;
        int k = kbd_getkey();
        if (k == KEY_UP) sel = (sel + S_COUNT - 1) % S_COUNT;
        else if (k == KEY_DOWN || k == '\t') sel = (sel + 1) % S_COUNT;
        else if (k == KEY_HOME) sel = 0;
        else if (k == KEY_END) sel = S_COUNT - 1;
        else if (k == KEY_LEFT || k == '-') change(sel, &c, &os, -1);
        else if (k == KEY_RIGHT || k == '+' || k == ' ') change(sel, &c, &os, 1);
        else if (k == KEY_ESC || k == 'q' || k == 'Q') {
            rc = 0;
            break;
        } else if (k == 's' || k == 'S') {
            sel = S_SAVE;
            k = '\n';
        }
        if (k != '\n') continue;
        if (sel == S_HOST || sel == S_USER) {
            char buf[32];
            strlcpy(buf, sel == S_HOST ? c.hostname : c.username, sizeof buf);
            if (tui_prompt(labels[sel], "Type a new name and press Enter (Esc cancels):", buf, sizeof buf) && buf[0]) {
                if (!fs_valid_name(buf) || strchr(buf, ' ')) snprintf(msg, sizeof msg, "'%s' is not a valid name (no spaces or / \\ : * ? \" < > |)", buf);
                else strlcpy(sel == S_HOST ? c.hostname : c.username, buf, 32);
            }
            con_show_cursor(false);
        } else if (sel == S_SAVE) {
            bool user_changed = strcmp(c.username, cfg.username) != 0;
            cfg = c;
            cur_os = os;
            theme_apply();
            if (user_changed) fs_cwd = fs_home();
            bool ok = config_save();
            if (ok) tui_message("Settings", "Settings saved. AxisBoot will use them next time.");
            else tui_message("Settings", "Applied for this session (no AxisOS disk to save to).");
            rc = 1;
            break;
        } else if (sel == S_RESETFS) {
            if (tui_confirm("Reset files", "Erase ALL files and restore the default folders?")) {
                fs_format();
                fs_cwd = fs_home();
                fs_save();
                strlcpy(msg, "Files reset to factory defaults.", sizeof msg);
            }
        } else if (sel == S_CANCEL) {
            rc = 0;
            break;
        } else
            change(sel, &c, &os, 1);
    }
    theme_apply();
    if (rc) {
        /* system or colours may have changed: start with a clean screen */
        con_set_attr(theme.normal);
        con_clear();
        extern void shell_banner(void);
        shell_banner();
    } else
        scr_restore(saved);
    con_show_cursor(true);
    return 0;
}
