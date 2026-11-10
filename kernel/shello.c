/*
 * Shello - a retro, minimal shell for AxisOS.
 *
 * Old-school look (phosphor colours, upper case, "READY." prompt) and a
 * tiny set of short commands. Enter it with "shello", leave with BYE.
 */
#include "axis.h"

static const uint8_t phosphor[3] = {ATTR(C_LGREEN, C_BLACK), ATTR(C_YELLOW, C_BLACK), ATTR(C_WHITE, C_BLACK)};
static int tint;

static void up(const char *s) {
    for (; *s; s++) con_putc((char)toupper(*s));
    con_update_hw_cursor();
}
static void upf(const char *fmt, ...) {
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    up(b);
}
static void err(const char *what) { upf("?%s\n", what); }

static void beep(void) {
    uint32_t div = 1193180 / 880;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);
    sleep_ms(120);
    outb(0x61, inb(0x61) & 0xFC);
}

static void banner(void) {
    con_set_attr(phosphor[tint]);
    con_clear();
    up("\n    ****  SHELLO RETRO SHELL  V1.0  ****\n\n");
    uint32_t total, used;
    heap_stats(&total, &used, NULL);
    upf(" %uK RAM SYSTEM   %u BYTES FREE\n\n", mem_total_kb(), total - used);
    up("TYPE HELP FOR COMMANDS, BYE TO LEAVE.\n\nREADY.\n");
}

static void help(void) {
    up("COMMANDS:\n");
    up(" LIST         FILES HERE        GO <DIR>   CHANGE DIR\n");
    up(" UP           PARENT DIR        HOME       HOME DIR\n");
    up(" SHOW <F>     PRINT A FILE      NEW <F>    TYPE A NEW FILE\n");
    up(" ADD <F>      ADD LINES         DEL <F>    DELETE\n");
    up(" MAKE <DIR>   NEW DIR           NAME <A> <B>  RENAME\n");
    up(" COPY <A> <B> COPY A FILE       WHERE      CURRENT DIR\n");
    up(" TIME         DATE AND TIME     MEM        MEMORY\n");
    up(" RUN <CMD>    RUN AXSH COMMAND  BEEP       SPEAKER BEEP\n");
    up(" DESKTOP      RETRO DESKTOP     CLS        CLEAR SCREEN\n");
    up(" TINT         CHANGE COLOUR     CLS        CLEAR SCREEN\n");
    up(" BYE          BACK TO AXSH\n");
}

static void list(void) {
    int kids[FS_MAX_NODES];
    int k = fs_children(fs_cwd, kids, FS_MAX_NODES, false);
    int col = 0;
    uint32_t total = 0;
    for (int i = 0; i < k; i++) {
        fs_node_t *n = &fs_nodes[kids[i]];
        char nm[24];
        snprintf(nm, sizeof nm, "%.15s%s", n->name, n->type == FT_DIR ? "/" : "");
        upf("%-17s%-6s", nm, n->type == FT_DIR ? "DIR" : "");
        if (n->type == FT_FILE) total += n->size;
        if (++col % 3 == 0) up("\n");
    }
    if (col % 3) up("\n");
    upf("%d ENTRIES, %u BYTES\n", k, total);
}

/* NEW / ADD: type lines until a line with a single "." */
static void enter_lines(const char *name, bool append) {
    int id = fs_mkfile(name, fs_cwd);
    if (id < 0) {
        err("BAD NAME");
        return;
    }
    if (!append) fs_write(id, "", 0);
    up("ENTER LINES. END WITH A LINE HOLDING ONLY .\n");
    char buf[200];
    int n = 0;
    for (;;) {
        con_write(". ");
        int r = line_edit(buf, sizeof buf, false, false);
        if (r < 0 || !strcmp(buf, ".")) break;
        fs_append(id, buf, (uint32_t)strlen(buf));
        fs_append(id, "\n", 1);
        n++;
    }
    upf("%d LINES SAVED\n", n);
}

int shello_run(void) {
    tint = 0;
    banner();
    char line[200];
    for (;;) {
        con_set_attr(phosphor[tint]);
        con_write("> ");
        int r = line_edit(line, sizeof line, false, false);
        if (r < 0) continue;
        char *argv[8];
        char store[200];
        strlcpy(store, line, sizeof store);
        int argc = 0;
        for (char *p = strtok_simple(store); p && argc < 8; p = strtok_simple(NULL)) argv[argc++] = p;
        if (!argc) continue;
        const char *c = argv[0];
        bool ok = true;
        if (!strcasecmp(c, "bye") || !strcasecmp(c, "exit") || !strcasecmp(c, "quit") || !strcasecmp(c, "q")) break;
        else if (!strcasecmp(c, "help") || !strcmp(c, "?")) help();
        else if (!strcasecmp(c, "list") || !strcasecmp(c, "l") || !strcasecmp(c, "dir")) list();
        else if (!strcasecmp(c, "cls")) {
            con_clear();
            ok = false;
        } else if (!strcasecmp(c, "where")) {
            char p[256];
            fs_path(fs_cwd, p, sizeof p);
            upf("%s\n", p);
        } else if (!strcasecmp(c, "go") || !strcasecmp(c, "g")) {
            if (argc < 2) err("WHICH DIR");
            else {
                int id = fs_resolve(argv[1], fs_cwd);
                if (id < 0 || fs_nodes[id].type != FT_DIR) err("NOT FOUND");
                else fs_cwd = id;
            }
        } else if (!strcasecmp(c, "up")) {
            fs_cwd = fs_nodes[fs_cwd].parent;
        } else if (!strcasecmp(c, "home")) {
            fs_cwd = fs_home();
        } else if (!strcasecmp(c, "show") || !strcasecmp(c, "s")) {
            int id = argc > 1 ? fs_resolve(argv[1], fs_cwd) : -1;
            if (id < 0 || fs_nodes[id].type != FT_FILE) err("NOT FOUND");
            else {
                pager_show(fs_nodes[id].data ? (const char *)fs_nodes[id].data : "", fs_nodes[id].size);
            }
        } else if (!strcasecmp(c, "new") || !strcasecmp(c, "add")) {
            if (argc < 2) err("WHICH FILE");
            else enter_lines(argv[1], !strcasecmp(c, "add"));
        } else if (!strcasecmp(c, "del")) {
            int id = argc > 1 ? fs_resolve(argv[1], fs_cwd) : -1;
            if (id < 0) err("NOT FOUND");
            else if (fs_delete(id, false) < 0) err("CANNOT DELETE");
        } else if (!strcasecmp(c, "make")) {
            char leaf[FS_NAME_MAX];
            int par = argc > 1 ? fs_resolve_parent(argv[1], fs_cwd, leaf) : -1;
            if (par < 0 || fs_create(par, leaf, FT_DIR) < 0) err("CANNOT MAKE");
        } else if (!strcasecmp(c, "name") || !strcasecmp(c, "copy")) {
            int id = argc > 2 ? fs_resolve(argv[1], fs_cwd) : -1;
            char leaf[FS_NAME_MAX];
            int par = argc > 2 ? fs_resolve_parent(argv[2], fs_cwd, leaf) : -1;
            if (id < 0 || par < 0) err("NOT FOUND");
            else {
                int rr = !strcasecmp(c, "name") ? fs_move(id, par, leaf) : fs_copy(id, par, leaf);
                if (rr < 0) err("FAILED");
            }
        } else if (!strcasecmp(c, "time")) {
            rtc_time_t t;
            rtc_local(&t);
            upf("%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
        } else if (!strcasecmp(c, "mem")) {
            uint32_t total, used;
            heap_stats(&total, &used, NULL);
            upf("%u BYTES FREE OF %u\n", total - used, total);
        } else if (!strcasecmp(c, "desktop") || !strcasecmp(c, "gui")) {
            desktop_run();
            banner();
            ok = false;
        } else if (!strcasecmp(c, "beep")) {
            beep();
        } else if (!strcasecmp(c, "tint")) {
            tint = (tint + 1) % 3;
            con_set_attr(phosphor[tint]);
            ok = false;
        } else if (!strcasecmp(c, "run")) {
            const char *rest = line;
            while (*rest == ' ') rest++;
            rest += 3;
            while (*rest == ' ') rest++;
            if (!*rest) err("RUN WHAT");
            else {
                con_set_attr(phosphor[tint]);
                shell_exec_line(rest);
            }
        } else {
            err("WHAT");
            ok = false;
        }
        if (ok) up("READY.\n");
        if (fs_dirty && cfg.autosave) fs_save();
    }
    theme_apply();
    con_set_attr(theme.normal);
    con_clear();
    shell_banner();
    return 0;
}
