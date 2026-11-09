/* AxisEdit - full-screen text editor (nano / edit / ee) */
#include "axis.h"

int tui_prompt(const char *title, const char *label, char *buf, size_t n);

#define ED_TOP 1
#define ED_ROWS 22

static char *text;
static uint32_t len, cap, cur;
static int top_line, left_col, want_col = -1;
static bool modified;
static char fname[256];
static char msg[96];
static char *cutbuf;
static uint32_t cutlen;

static bool ensure(uint32_t n) {
    if (n <= cap) return true;
    uint32_t nc = cap ? cap : 256;
    while (nc < n) nc *= 2;
    char *t = krealloc(text, nc);
    if (!t) return false;
    text = t;
    cap = nc;
    return true;
}
static void insert(const char *s, uint32_t n) {
    if (!ensure(len + n + 1)) {
        strlcpy(msg, "Out of memory", sizeof msg);
        return;
    }
    memmove(text + cur + n, text + cur, len - cur);
    memcpy(text + cur, s, n);
    len += n;
    cur += n;
    modified = true;
}
static void erase(uint32_t at, uint32_t n) {
    if (at + n > len) n = len - at;
    memmove(text + at, text + at + n, len - at - n);
    len -= n;
    if (cur > at) cur = cur >= at + n ? cur - n : at;
    modified = true;
}

static uint32_t line_start(uint32_t p) {
    while (p > 0 && text[p - 1] != '\n') p--;
    return p;
}
static uint32_t line_end(uint32_t p) {
    while (p < len && text[p] != '\n') p++;
    return p;
}
static int line_of(uint32_t p) {
    int l = 0;
    for (uint32_t i = 0; i < p; i++)
        if (text[i] == '\n') l++;
    return l;
}
static uint32_t pos_of_line(int line) {
    uint32_t p = 0;
    for (int l = 0; l < line && p < len; p++)
        if (text[p] == '\n') l++;
    return p;
}
static int total_lines(void) { return line_of(len) + 1; }

static void move_to_col(uint32_t ls, int col) {
    uint32_t le = line_end(ls);
    cur = ls + (uint32_t)col;
    if (cur > le) cur = le;
}

static void draw(void) {
    int cl = line_of(cur);
    int cc = (int)(cur - line_start(cur));
    if (cl < top_line) top_line = cl;
    if (cl >= top_line + ED_ROWS) top_line = cl - ED_ROWS + 1;
    if (cc < left_col) left_col = cc;
    if (cc >= left_col + SCREEN_W) left_col = cc - SCREEN_W + 1;

    scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
    const char *title = cur_os == OS_WINDOWS ? "AxisEdit" : (cur_os == OS_FREEBSD ? "ee (AxisEdit)" : "GNU-style nano (AxisEdit)");
    scr_printf(1, 0, theme.bar, "%s   %s%s", title, fname[0] ? fname : "[New file]", modified ? "  [Modified]" : "");
    scr_printf(SCREEN_W - 18, 0, theme.bar, "Ln %d, Col %d", cl + 1, cc + 1);

    uint32_t p = pos_of_line(top_line);
    bool done = false;
    for (int r = 0; r < ED_ROWS; r++) {
        int y = ED_TOP + r;
        scr_fill(0, y, SCREEN_W, 1, ' ', theme.normal);
        if (done) {
            scr_put(0, y, '~', theme.dim);
            continue;
        }
        uint32_t e = line_end(p);
        for (uint32_t i = p + left_col; i < e && (int)(i - p - left_col) < SCREEN_W; i++) {
            char c = text[i];
            scr_put((int)(i - p - left_col), y, (c >= 32 && c < 127) ? c : (c == '\t' ? ' ' : '?'), theme.normal);
        }
        if (e >= len) done = true;
        else p = e + 1;
    }
    scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
    if (msg[0]) scr_putsn(1, 23, msg, 78, theme.accent);
    msg[0] = 0;
    scr_fill(0, 24, SCREEN_W, 1, ' ', theme.bar);
    if (adv_mode) scr_puts(1, 24, "^S ^X ^F ^K ^U ^G", theme.bar);
    else scr_puts(1, 24, "^S Save  ^X/Esc Exit  ^F Find  ^K Cut line  ^U Paste  ^G Go to line  ^A/^E Home/End", theme.bar);
    con_set_cursor(cc - left_col, ED_TOP + cl - top_line);
}

static bool save(void) {
    if (!fname[0]) {
        char n[200] = "";
        if (!tui_prompt("Save as", "File name:", n, sizeof n) || !n[0]) return false;
        strlcpy(fname, n, sizeof fname);
    }
    int id = fs_mkfile(fname, fs_cwd);
    if (id < 0) {
        snprintf(msg, sizeof msg, "Cannot save %s: %s", fname, fs_strerror(id));
        return false;
    }
    int r = fs_write(id, text, len);
    if (r < 0) {
        snprintf(msg, sizeof msg, "Cannot save %s: %s", fname, fs_strerror(r));
        return false;
    }
    modified = false;
    if (cfg.autosave) fs_save();
    snprintf(msg, sizeof msg, "Wrote %u bytes, %d lines to %s", len, total_lines(), fname);
    return true;
}

static void find(void) {
    static char q[64];
    if (!tui_prompt("Find", "Search for (case-insensitive):", q, sizeof q) || !q[0]) return;
    size_t ql = strlen(q);
    for (uint32_t k = 1; k <= len; k++) {
        uint32_t i = (cur + k) % (len ? len : 1);
        if (i + ql <= len && !strncasecmp(text + i, q, ql)) {
            cur = i;
            return;
        }
    }
    snprintf(msg, sizeof msg, "\"%s\" not found", q);
}

int editor_run(const char *path) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    len = cur = 0;
    top_line = left_col = 0;
    modified = false;
    fname[0] = msg[0] = 0;
    ensure(256);
    if (path) {
        char corr[256];
        int id = fs_resolve_fuzzy(path, fs_cwd, corr, sizeof corr);
        if (id >= 0 && fs_nodes[id].type == FT_DIR) {
            scr_restore(saved);
            shell_print_error(E_ISDIR, path);
            return 1;
        }
        if (id >= 0) {
            fs_path(id, fname, sizeof fname);
            fs_node_t *n = &fs_nodes[id];
            if (ensure(n->size + 1)) {
                memcpy(text, n->data, n->size);
                len = n->size;
            }
            if (n->attr & FA_READONLY) strlcpy(msg, "File is read-only - changes cannot be saved", sizeof msg);
            else snprintf(msg, sizeof msg, "Read %d lines", total_lines());
        } else {
            strlcpy(fname, path, sizeof fname);
            strlcpy(msg, "New file", sizeof msg);
        }
    }
    con_show_cursor(true);
    for (;;) {
        draw();
        int k = kbd_getkey();
        int col_keep = want_col;
        want_col = -1;
        if (k == KEY_CTRL('s')) save();
        else if (k == KEY_CTRL('x') || k == KEY_ESC || k == KEY_CTRL('q')) {
            if (modified) {
                scr_fill(0, 23, SCREEN_W, 1, ' ', theme.bar);
                scr_puts(1, 23, "Save changes?  Y = yes   N = no   C = cancel", theme.bar);
                int a = kbd_getkey();
                if (a == 'y' || a == 'Y') {
                    if (!save()) continue;
                } else if (a != 'n' && a != 'N')
                    continue;
            }
            break;
        } else if (k == KEY_CTRL('f')) find();
        else if (k == KEY_CTRL('g')) {
            char b[12] = "";
            if (tui_prompt("Go to line", "Line number:", b, sizeof b) && b[0]) cur = pos_of_line(atoi(b) > 0 ? atoi(b) - 1 : 0);
        } else if (k == KEY_CTRL('k')) {
            uint32_t s = line_start(cur), e = line_end(cur);
            if (e < len) e++;
            kfree(cutbuf);
            cutlen = e - s;
            cutbuf = kmalloc(cutlen + 1);
            if (cutbuf) memcpy(cutbuf, text + s, cutlen);
            erase(s, e - s);
            cur = s;
        } else if (k == KEY_CTRL('u')) {
            if (cutbuf && cutlen) insert(cutbuf, cutlen);
        } else if (k == KEY_LEFT) {
            if (cur) cur--;
        } else if (k == KEY_RIGHT) {
            if (cur < len) cur++;
        } else if (k == KEY_UP || k == KEY_DOWN || k == KEY_PGUP || k == KEY_PGDN) {
            uint32_t ls = line_start(cur);
            int col = col_keep >= 0 ? col_keep : (int)(cur - ls);
            int steps = (k == KEY_PGUP || k == KEY_PGDN) ? ED_ROWS - 1 : 1;
            for (int s = 0; s < steps; s++) {
                if (k == KEY_UP || k == KEY_PGUP) {
                    if (ls == 0) break;
                    ls = line_start(ls - 1);
                } else {
                    uint32_t e = line_end(ls);
                    if (e >= len) break;
                    ls = e + 1;
                }
            }
            move_to_col(ls, col);
            want_col = col;
        } else if (k == KEY_HOME || k == KEY_CTRL('a')) cur = line_start(cur);
        else if (k == KEY_END || k == KEY_CTRL('e')) cur = line_end(cur);
        else if (k == '\b') {
            if (cur) erase(cur - 1, 1);
        } else if (k == KEY_DELETE) {
            if (cur < len) erase(cur, 1);
        } else if (k == '\n') insert("\n", 1);
        else if (k == '\t') insert("    ", 4);
        else if (k >= 32 && k < 127) {
            char c = (char)k;
            insert(&c, 1);
        }
    }
    scr_restore(saved);
    return 0;
}
