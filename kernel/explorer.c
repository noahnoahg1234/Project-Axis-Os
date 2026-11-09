/*
 * AxisOS Explorer - full-screen file manager.
 *
 * Normal mode: a friendly two-pane view with a text preview and a key bar.
 * Advanced mode: no key bar, inode/attribute columns and a hex preview.
 */
#include "axis.h"

int tui_prompt(const char *title, const char *label, char *buf, size_t n);

#define LIST_TOP 2
#define LIST_ROWS 20
#define LIST_W 50

static int dir, sel, top, nitems;
static int items[FS_MAX_NODES];
static bool show_hidden;
static int clip = -1;
static bool clip_cut;
static char status[96];

static void reload(void) {
    nitems = fs_children(dir, items, FS_MAX_NODES, show_hidden);
    if (sel >= nitems) sel = nitems ? nitems - 1 : 0;
    if (sel < 0) sel = 0;
}

static void select_name(const char *name) {
    for (int i = 0; i < nitems; i++)
        if (!strcasecmp(fs_nodes[items[i]].name, name)) sel = i;
}

static void draw_preview(int id) {
    int x = LIST_W + 1, w = SCREEN_W - x - 1;
    scr_fill(LIST_W, 1, SCREEN_W - LIST_W, LIST_ROWS + 1, ' ', theme.normal);
    for (int y = 1; y <= LIST_ROWS + 1; y++) scr_put(LIST_W, y, (char)0xB3, theme.dim);
    if (id < 0) {
        scr_puts(x + 1, 3, "(empty folder)", theme.dim);
        return;
    }
    fs_node_t *n = &fs_nodes[id];
    char t[48];
    rtc_time_t tm;
    rtc_from_unix((uint32_t)((int32_t)n->mtime + cfg.tz_hours * 3600), &tm);
    snprintf(t, sizeof t, "%04d-%02d-%02d %02d:%02d", tm.year, tm.month, tm.day, tm.hour, tm.min);
    scr_putsn(x + 1, 1, n->name, w - 1, theme.accent);
    if (n->type == FT_DIR) {
        int kids[FS_MAX_NODES];
        int k = fs_children(id, kids, FS_MAX_NODES, show_hidden);
        scr_printf(x + 1, 2, theme.dim, "Folder, %d item%s", k, k == 1 ? "" : "s");
        scr_printf(x + 1, 3, theme.dim, "%s", t);
        for (int i = 0; i < k && i < LIST_ROWS - 4; i++) {
            fs_node_t *c = &fs_nodes[kids[i]];
            char nm[48];
            snprintf(nm, sizeof nm, "%c %s", c->type == FT_DIR ? '+' : ' ', c->name);
            scr_putsn(x + 1, 5 + i, nm, w - 1, c->type == FT_DIR ? theme.path : theme.normal);
        }
        return;
    }
    char sz[16];
    format_size(n->size, sz, sizeof sz);
    scr_printf(x + 1, 2, theme.dim, "File, %s%s", sz, n->attr & FA_READONLY ? ", read-only" : "");
    scr_printf(x + 1, 3, theme.dim, "%s", t);
    if (adv_mode) {
        scr_printf(x + 1, 4, theme.dim, "inode %d attr 0x%02x", id, n->attr);
        for (uint32_t row = 0; row < (uint32_t)LIST_ROWS - 4 && row * 8 < n->size; row++) {
            char hx[40] = "";
            for (uint32_t i = 0; i < 8 && row * 8 + i < n->size; i++) {
                char b[4];
                snprintf(b, sizeof b, "%02x ", n->data[row * 8 + i]);
                strcat(hx, b);
            }
            scr_printf(x + 1, 6 + row, theme.normal, "%04x %s", row * 8, hx);
        }
        return;
    }
    /* text preview */
    uint32_t i = 0;
    for (int row = 0; row < LIST_ROWS - 4 && i < n->size; row++) {
        int col = 0;
        while (i < n->size && n->data[i] != '\n') {
            char c = (char)n->data[i++];
            if (col < w - 1) scr_put(x + 1 + col++, 5 + row, (c >= 32 && c < 127) ? c : '.', theme.normal);
        }
        i++;
    }
}

static void draw(void) {
    char p[256];
    fs_path(dir, p, sizeof p);
    scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
    scr_printf(1, 0, theme.bar, "%s  %.60s", adv_mode ? "Explorer [adv]" : "AxisOS Explorer", p);
    scr_fill(0, 1, LIST_W, 1, ' ', theme.normal);
    if (adv_mode) scr_printf(1, 1, theme.accent, "%-5s %-24s %8s %4s", "inode", "Name", "Size", "Attr");
    else scr_printf(1, 1, theme.accent, "  %-26s %10s  %s", "Name", "Size", "Type");
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = top + r, y = LIST_TOP + r;
        scr_fill(0, y, LIST_W, 1, ' ', theme.normal);
        if (i >= nitems) continue;
        fs_node_t *n = &fs_nodes[items[i]];
        uint8_t a = i == sel ? theme.select : (n->type == FT_DIR ? theme.path : theme.normal);
        if (i == sel) scr_fill(0, y, LIST_W, 1, ' ', a);
        char sz[16], line[80];
        if (n->type == FT_DIR) strcpy(sz, "");
        else format_size(n->size, sz, sizeof sz);
        if (adv_mode)
            snprintf(line, sizeof line, "%5d %-24.24s %8u %c%c%c%c", items[i], n->name, n->size, n->type == FT_DIR ? 'd' : '-',
                     n->attr & FA_HIDDEN ? 'h' : '-', n->attr & FA_READONLY ? 'r' : '-', n->attr & FA_SYSTEM ? 's' : '-');
        else
            snprintf(line, sizeof line, "%c %-26.26s %10s  %s", n->type == FT_DIR ? '+' : ' ', n->name, sz,
                     n->type == FT_DIR ? "Folder" : "File");
        scr_putsn(1, y, line, LIST_W - 2, a);
        if (clip == items[i]) scr_put(0, y, clip_cut ? 'x' : 'c', a);
    }
    draw_preview(nitems ? items[sel] : -1);
    scr_fill(0, 22, SCREEN_W, 1, ' ', theme.normal);
    if (status[0]) scr_putsn(1, 22, status, 78, theme.accent);
    else scr_printf(1, 22, theme.dim, "%d item%s%s%s", nitems, nitems == 1 ? "" : "s", show_hidden ? "  (showing hidden)" : "",
                    clip >= 0 ? (clip_cut ? "  [1 item cut]" : "  [1 item copied]") : "");
    scr_fill(0, 23, SCREEN_W, 2, ' ', adv_mode ? theme.normal : theme.bar);
    if (!adv_mode) {
        scr_puts(1, 23, "Enter Open  Bksp Up  E Edit  N New file  M New folder  R Rename  Del Delete", theme.bar);
        scr_puts(1, 24, "C Copy  X Cut  V Paste  H Hidden  G Go to path  F Find  S Shell here  Q Quit", theme.bar);
    }
    status[0] = 0;
}

static void view_file(int id) {
    fs_node_t *n = &fs_nodes[id];
    int first = 0;
    for (;;) {
        scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
        scr_printf(1, 0, theme.bar, "Viewing %s   (Up/Down/PgUp/PgDn scroll, E edit, Q back)", n->name);
        scr_fill(0, 1, SCREEN_W, SCREEN_H - 1, ' ', theme.normal);
        int line = 0, row = 0, col = 0;
        for (uint32_t i = 0; i < n->size && row < SCREEN_H - 1; i++) {
            char c = (char)n->data[i];
            if (line >= first) {
                if (c == '\n') {
                    row++;
                    col = 0;
                } else if (col < SCREEN_W)
                    scr_put(col++, 1 + row, (c >= 32 && c < 127) ? c : (c == '\t' ? ' ' : '.'), theme.normal);
            }
            if (c == '\n') line++;
        }
        int k = kbd_getkey();
        if (k == KEY_DOWN || k == '\n') first++;
        else if (k == KEY_UP && first) first--;
        else if (k == KEY_PGDN || k == ' ') first += SCREEN_H - 2;
        else if (k == KEY_PGUP) first = first > SCREEN_H - 2 ? first - (SCREEN_H - 2) : 0;
        else if (k == KEY_HOME) first = 0;
        else if (k == 'e' || k == 'E') {
            char p[256];
            fs_path_unix(id, p, sizeof p);
            editor_run(p);
            con_show_cursor(false);
            return;
        } else if (k == 'q' || k == 'Q' || k == KEY_ESC || k == '\b')
            return;
    }
}

static void do_paste(void) {
    if (clip < 0 || !fs_get(clip)) {
        strlcpy(status, "Clipboard is empty.", sizeof status);
        clip = -1;
        return;
    }
    char name[FS_NAME_MAX];
    strlcpy(name, fs_nodes[clip].name, sizeof name);
    int r;
    if (clip_cut) {
        r = fs_move(clip, dir, name);
        if (r == 0) clip = -1;
    } else {
        if (fs_lookup(dir, name) >= 0) {
            /* make a "copy of" name */
            char nn[FS_NAME_MAX];
            for (int i = 1; i < 100; i++) {
                if (i == 1) snprintf(nn, sizeof nn, "Copy of %.30s", name);
                else snprintf(nn, sizeof nn, "Copy %d of %.28s", i, name);
                if (fs_lookup(dir, nn) < 0) break;
            }
            strlcpy(name, nn, sizeof name);
        }
        r = fs_copy(clip, dir, name);
    }
    reload();
    if (r < 0) snprintf(status, sizeof status, "Paste failed: %s", fs_strerror(r));
    else {
        select_name(name);
        snprintf(status, sizeof status, "Pasted %s", name);
    }
}

int explorer_run(const char *start) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    dir = fs_cwd;
    if (start) {
        int id = fs_resolve(start, fs_cwd);
        if (id >= 0) dir = fs_nodes[id].type == FT_DIR ? id : fs_nodes[id].parent;
    }
    sel = top = 0;
    show_hidden = cfg.show_hidden;
    status[0] = 0;
    reload();
    bool shell_here = false;
    for (;;) {
        draw();
        int k = kbd_getkey();
        int cur = nitems ? items[sel] : -1;
        if (k == KEY_UP) { if (sel) sel--; }
        else if (k == KEY_DOWN) { if (sel + 1 < nitems) sel++; }
        else if (k == KEY_PGUP) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
        else if (k == KEY_PGDN) sel = sel + LIST_ROWS < nitems ? sel + LIST_ROWS : (nitems ? nitems - 1 : 0);
        else if (k == KEY_HOME) sel = 0;
        else if (k == KEY_END) sel = nitems ? nitems - 1 : 0;
        else if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == 's' || k == 'S') { shell_here = true; break; }
        else if ((k == '\n' || k == KEY_RIGHT) && cur >= 0) {
            if (fs_nodes[cur].type == FT_DIR) {
                dir = cur;
                sel = top = 0;
                reload();
            } else if (k == '\n')
                view_file(cur);
        } else if ((k == '\b' || k == KEY_LEFT) && dir != FS_ROOT) {
            char name[FS_NAME_MAX];
            strlcpy(name, fs_nodes[dir].name, sizeof name);
            dir = fs_nodes[dir].parent;
            sel = top = 0;
            reload();
            select_name(name);
        } else if ((k == 'e' || k == 'E' || k == KEY_F4) && cur >= 0 && fs_nodes[cur].type == FT_FILE) {
            char p[256];
            fs_path_unix(cur, p, sizeof p);
            editor_run(p);
            con_show_cursor(false);
            reload();
        } else if (k == 'h' || k == 'H') {
            show_hidden = !show_hidden;
            reload();
        } else if (k == 'n' || k == 'N' || k == 'm' || k == 'M' || k == KEY_F7) {
            bool folder = (k == 'm' || k == 'M' || k == KEY_F7);
            char name[FS_NAME_MAX] = "";
            if (tui_prompt(folder ? "New folder" : "New file", "Name:", name, sizeof name) && name[0]) {
                int r = fs_create(dir, name, folder ? FT_DIR : FT_FILE);
                reload();
                if (r < 0) snprintf(status, sizeof status, "Cannot create '%s': %s", name, fs_strerror(r));
                else select_name(name);
            }
            con_show_cursor(false);
        } else if ((k == 'r' || k == 'R' || k == KEY_F2) && cur >= 0) {
            char name[FS_NAME_MAX];
            strlcpy(name, fs_nodes[cur].name, sizeof name);
            if (tui_prompt("Rename", "New name:", name, sizeof name) && name[0]) {
                int r = fs_move(cur, dir, name);
                reload();
                if (r < 0) snprintf(status, sizeof status, "Cannot rename: %s", fs_strerror(r));
                else select_name(name);
            }
            con_show_cursor(false);
        } else if ((k == KEY_DELETE || k == 'd' || k == 'D' || k == KEY_F8) && cur >= 0) {
            char q[80];
            snprintf(q, sizeof q, "Delete '%.40s'%s?", fs_nodes[cur].name, fs_nodes[cur].type == FT_DIR ? " and its contents" : "");
            if (adv_mode || !cfg.confirm || tui_confirm("Delete", q)) {
                int r = fs_delete(cur, true);
                if (r < 0) snprintf(status, sizeof status, "Cannot delete: %s", fs_strerror(r));
                if (clip == cur) clip = -1;
                reload();
            }
        } else if ((k == 'c' || k == 'C' || k == KEY_CTRL('c') || k == KEY_F5) && cur >= 0) {
            clip = cur;
            clip_cut = false;
            snprintf(status, sizeof status, "Copied '%s' - go to a folder and press V to paste.", fs_nodes[cur].name);
        } else if ((k == 'x' || k == 'X' || k == KEY_CTRL('x') || k == KEY_F6) && cur >= 0) {
            clip = cur;
            clip_cut = true;
            snprintf(status, sizeof status, "Cut '%s' - go to a folder and press V to move it there.", fs_nodes[cur].name);
        } else if (k == 'v' || k == 'V' || k == 'p' || k == 'P' || k == KEY_CTRL('v')) {
            do_paste();
        } else if (k == 'g' || k == 'G' || k == '/' || k == '\\') {
            char path[200] = "";
            if (tui_prompt("Go to", "Path (any style: /home, C:\\Users, ~):", path, sizeof path) && path[0]) {
                char corr[256];
                int id = fs_resolve_fuzzy(path, dir, corr, sizeof corr);
                if (id < 0) snprintf(status, sizeof status, "Not found: %s", path);
                else {
                    if (strcasecmp(corr, path)) snprintf(status, sizeof status, "autocorrect: %s -> %s", path, corr);
                    if (fs_nodes[id].type == FT_DIR) dir = id, sel = top = 0;
                    else {
                        dir = fs_nodes[id].parent;
                        reload();
                        select_name(fs_nodes[id].name);
                    }
                    reload();
                }
            }
            con_show_cursor(false);
        } else if (k == 'f' || k == 'F') {
            char q[FS_NAME_MAX] = "";
            if (tui_prompt("Find", "Name or pattern (* and ? allowed):", q, sizeof q) && q[0]) {
                char pat[FS_NAME_MAX + 2];
                if (strchr(q, '*') || strchr(q, '?')) strlcpy(pat, q, sizeof pat);
                else snprintf(pat, sizeof pat, "*%s*", q);
                int found = -1;
                for (int i = 0; i < FS_MAX_NODES && found < 0; i++)
                    if (fs_nodes[i].type != FT_FREE && i && wildcard_match(pat, fs_nodes[i].name)) found = i;
                if (found < 0) snprintf(status, sizeof status, "No match for %s", q);
                else {
                    dir = fs_nodes[found].parent;
                    show_hidden = show_hidden || (fs_nodes[found].attr & FA_HIDDEN);
                    reload();
                    select_name(fs_nodes[found].name);
                    char p[200];
                    fs_path(found, p, sizeof p);
                    snprintf(status, sizeof status, "Found %s", p);
                }
            }
            con_show_cursor(false);
        } else if (k == KEY_F1 || k == '?') {
            tui_message("Explorer help", adv_mode ? "Keys: Enter Bksp E N M R Del C X V H G F S Q (same as normal mode)"
                                                  : "Arrows move, Enter opens, Backspace goes up. Letters on the bar run actions.");
        }
    }
    if (fs_dirty && cfg.autosave) fs_save();
    con_show_cursor(true);
    if (shell_here) {
        fs_cwd = dir;
        con_set_attr(theme.normal);
        con_clear();
    } else
        scr_restore(saved);
    return 0;
}
