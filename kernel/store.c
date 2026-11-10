/*
 * AxisOS App Store, installed-app registry and the script interpreter.
 *
 * Apps come from two places:
 *   - the built-in AxisOS repository (native apps + bundled script apps)
 *   - online repositories: http:// index files listing script apps, added
 *     with the A key in the store and refreshed with R
 *
 * Installed apps live in /Apps/<id>/ (app.info + main.sh for script apps)
 * and run by typing their id at the prompt.
 */
#include "axis.h"

int app_calculator(void);
int app_minesweeper(void);
int app_paint(void);
int app_snake(void);
int app_clock(void);
int app_monitor(void);
int app_tictactoe(void);
int app_2048(void);
int app_tetris(void);
int app_pong(void);
int app_hangman(void);

/* ----------------------------------------------------- bundled scripts -- */
static const char *script_guess =
    "# Guess the number - an AxisScript app\n"
    "random N 100\n"
    "let TRIES = 0\n"
    "echo I am thinking of a number from 1 to 100.\n"
    ":loop\n"
    "input G Your guess:\n"
    "if \"%G%\" == \"\" goto loop\n"
    "let TRIES = %TRIES% + 1\n"
    "if %G% < %N% then echo Higher!\n"
    "if %G% > %N% then echo Lower!\n"
    "if %G% != %N% goto loop\n"
    "echo Correct! You found %N% in %TRIES% tries.\n";

static const char *script_todo =
    "# Todo list - keeps your tasks in ~/todo.txt\n"
    "touch ~/todo.txt\n"
    ":menu\n"
    "echo\n"
    "echo ===== Todo list =====\n"
    "cat -n ~/todo.txt\n"
    "echo ---------------------\n"
    "input C 1) add  2) clear all  3) quit :\n"
    "if %C% == 1 goto add\n"
    "if %C% == 2 goto clear\n"
    "if %C% == 3 exit\n"
    "goto menu\n"
    ":add\n"
    "input T New task:\n"
    "if \"%T%\" == \"\" goto menu\n"
    "echo %T% >> ~/todo.txt\n"
    "goto menu\n"
    ":clear\n"
    "rm -f ~/todo.txt\n"
    "touch ~/todo.txt\n"
    "goto menu\n";

static const char *script_convert =
    "# Unit converter (whole numbers)\n"
    ":menu\n"
    "echo\n"
    "echo 1) km to miles   2) miles to km   3) C to F   4) F to C   5) kg to lb   6) quit\n"
    "input C Choose:\n"
    "if %C% == 6 exit\n"
    "input V Value:\n"
    "if \"%V%\" == \"\" goto menu\n"
    "if %C% == 1 then let R = %V% * 1000 / 1609\n"
    "if %C% == 2 then let R = %V% * 1609 / 1000\n"
    "if %C% == 3 then let R = %V% * 9 / 5 + 32\n"
    "if %C% == 4 then let R = (%V% - 32) * 5 / 9\n"
    "if %C% == 5 then let R = %V% * 2205 / 1000\n"
    "echo Result: %R%\n"
    "goto menu\n";

static const char *script_fortune =
    "# Fortune - a random saying\n"
    "random N 8\n"
    "if %N% == 1 then echo A clean desk is a sign of a full trash folder.\n"
    "if %N% == 2 then echo Today is a good day to back up your files.\n"
    "if %N% == 3 then echo The best error message is the one that never shows up.\n"
    "if %N% == 4 then echo Three systems walked into a bar. They all said: dir, ls, ls.\n"
    "if %N% == 5 then echo Small steps every day add up to big things.\n"
    "if %N% == 6 then echo Curiosity is the engine of learning.\n"
    "if %N% == 7 then echo Every expert was once a beginner.\n"
    "if %N% == 8 then echo Have you tried turning it off and on again?\n";

/* ------------------------------------------------------------- catalog -- */
typedef struct {
    char id[24];
    char name[32];
    char category[20];
    char desc[96];
    int (*native)(void);  /* native app */
    const char *script;   /* bundled script */
    char url[200];        /* online script */
    char source[40];
} app_t;

static const app_t builtin[] = {
    {"calculator", "Calculator", "Windows-style", "Desktop calculator with a history of results.", app_calculator, NULL, "", "AxisOS repository"},
    {"minesweeper", "Minesweeper", "Windows-style", "Clear the field without hitting a mine. Beginner and Expert boards.", app_minesweeper, NULL, "", "AxisOS repository"},
    {"paint", "Paint", "Windows-style", "Draw coloured pictures with blocks and save them as .pnt files.", app_paint, NULL, "", "AxisOS repository"},
    {"taskmgr", "Task Monitor", "Windows-style", "Live CPU, memory, disk and network use (like Task Manager / top).", app_monitor, NULL, "", "AxisOS repository"},
    {"snake", "Snake", "Linux-style", "The classic terminal snake game. Eat, grow, don't crash.", app_snake, NULL, "", "AxisOS repository"},
    {"tetris", "Tetris", "Games", "Stack the falling blocks and clear full lines.", app_tetris, NULL, "", "AxisOS repository"},
    {"2048", "2048", "Games", "Slide and join numbered tiles to reach 2048.", app_2048, NULL, "", "AxisOS repository"},
    {"tictactoe", "Tic-Tac-Toe", "Games", "Noughts and crosses against the computer.", app_tictactoe, NULL, "", "AxisOS repository"},
    {"pong", "Pong", "Games", "The classic paddle game against the computer.", app_pong, NULL, "", "AxisOS repository"},
    {"hangman", "Hangman", "Games", "Guess the hidden word one letter at a time.", app_hangman, NULL, "", "AxisOS repository"},
    {"xclock", "Big Clock", "Linux-style", "A large digital clock for your screen.", app_clock, NULL, "", "AxisOS repository"},
    {"fortune", "Fortune", "Linux-style", "Prints a random saying, like the Unix fortune program.", NULL, NULL, "", "AxisOS repository"},
    {"guess", "Guess the Number", "Games", "Find the secret number from 1 to 100 in as few tries as you can.", NULL, NULL, "", "AxisOS repository"},
    {"todo", "Todo List", "Tools", "A simple task list stored in ~/todo.txt.", NULL, NULL, "", "AxisOS repository"},
    {"convert", "Unit Converter", "Tools", "Convert km/miles, Celsius/Fahrenheit and kg/lb.", NULL, NULL, "", "AxisOS repository"},
};
#define NBUILTIN ((int)(sizeof builtin / sizeof *builtin))

static const char *builtin_script(const char *id) {
    if (!strcmp(id, "guess")) return script_guess;
    if (!strcmp(id, "todo")) return script_todo;
    if (!strcmp(id, "convert")) return script_convert;
    if (!strcmp(id, "fortune")) return script_fortune;
    return NULL;
}

#define MAX_ONLINE 64
static app_t online[MAX_ONLINE];
static int nonline;

static const app_t *find_builtin(const char *id) {
    for (int i = 0; i < NBUILTIN; i++)
        if (!strcasecmp(builtin[i].id, id)) return &builtin[i];
    return NULL;
}

/* ------------------------------------------------------------ registry -- */
static int apps_dir(bool create) {
    int d = fs_lookup(FS_ROOT, "Apps");
    if (d < 0 && create) d = fs_create(FS_ROOT, "Apps", FT_DIR);
    return d;
}
static int app_dir(const char *id) {
    int d = apps_dir(false);
    return d < 0 ? -1 : fs_lookup(d, id);
}
static bool installed(const char *id) { return app_dir(id) >= 0; }

static int write_file(int dir, const char *name, const char *text, uint32_t len) {
    int f = fs_lookup(dir, name);
    if (f < 0) f = fs_create(dir, name, FT_FILE);
    return f < 0 ? f : fs_write(f, text, len);
}

static int install_app(const app_t *a, char *msg, size_t mn) {
    int root = apps_dir(true);
    if (root < 0) {
        snprintf(msg, mn, "Cannot create /Apps");
        return -1;
    }
    char *script = NULL;
    uint32_t slen = 0;
    bool fetched = false, is_program = false;
    if (!a->native) {
        const char *b = builtin_script(a->id);
        if (b) {
            script = (char *)b;
            slen = (uint32_t)strlen(b);
        } else if (a->url[0]) {
            http_resp_t r;
            int st = http_get(a->url, &r, NULL);
            if (st != 200) {
                snprintf(msg, mn, "Download failed: %s", st < 0 ? http_strerror(st) : "server error");
                if (st >= 0) kfree(r.body);
                return -1;
            }
            script = r.body;
            slen = r.len;
            fetched = true;
            is_program = r.len >= 4 && !memcmp(r.body, "\x7f""ELF", 4);
        } else {
            snprintf(msg, mn, "App has no download address");
            return -1;
        }
    }
    int d = fs_lookup(root, a->id);
    if (d < 0) d = fs_create(root, a->id, FT_DIR);
    if (d < 0) {
        if (fetched) kfree(script);
        snprintf(msg, mn, "Install failed: %s", fs_strerror(d));
        return -1;
    }
    const char *type = a->native ? "native" : (is_program ? "program" : "script");
    char info[400];
    snprintf(info, sizeof info, "name=%s\ncategory=%s\ntype=%s\nsource=%s\ndescription=%s\n", a->name, a->category,
             type, a->url[0] ? a->url : a->source, a->desc);
    write_file(d, "app.info", info, (uint32_t)strlen(info));
    if (script) write_file(d, is_program ? "main.elf" : "main.sh", script, slen);
    if (fetched) kfree(script);
    if (cfg.autosave) fs_save();
    snprintf(msg, mn, "Installed %s. Type '%s' at the prompt to run it.", a->name, a->id);
    return 0;
}

static int uninstall_app(const char *id) {
    int d = app_dir(id);
    if (d < 0) return E_NOENT;
    int r = fs_delete(d, true);
    if (cfg.autosave) fs_save();
    return r;
}

static void info_get(int dir, const char *key, char *out, size_t n) {
    out[0] = 0;
    int f = fs_lookup(dir, "app.info");
    if (f < 0 || !fs_nodes[f].data) return;
    const char *t = (const char *)fs_nodes[f].data, *e = t + fs_nodes[f].size;
    size_t kl = strlen(key);
    while (t < e) {
        const char *nl = memchr(t, '\n', (size_t)(e - t));
        if (!nl) nl = e;
        if ((size_t)(nl - t) > kl && !strncmp(t, key, kl) && t[kl] == '=') {
            size_t l = (size_t)(nl - t - kl - 1);
            if (l >= n) l = n - 1;
            memcpy(out, t + kl + 1, l);
            out[l] = 0;
            return;
        }
        t = nl + 1;
    }
}

int run_installed(const char *id, int argc, char **argv) {
    int d = app_dir(id);
    if (d < 0) return -1;
    char type[16];
    info_get(d, "type", type, sizeof type);
    if (!strcmp(type, "native")) {
        const app_t *a = find_builtin(fs_nodes[d].name);
        if (!a || !a->native) return -1;
        static uint16_t saved[SCREEN_W * SCREEN_H];
        scr_save(saved);
        con_show_cursor(false);
        a->native();
        con_show_cursor(true);
        scr_restore(saved);
        return 0;
    }
    char path[96];
    if (!strcmp(type, "program")) {
        snprintf(path, sizeof path, "/Apps/%s/main.elf", fs_nodes[d].name);
        return compat_run(path, argc, argv);
    }
    snprintf(path, sizeof path, "/Apps/%s/main.sh", fs_nodes[d].name);
    return script_run(path, argc, argv);
}

bool app_try_run(int argc, char **argv) {
    if (!argc) return false;
    if (!installed(argv[0])) return false;
    run_installed(argv[0], argc, argv);
    return true;
}

int apps_list(void) {
    int d = apps_dir(false);
    int kids[FS_MAX_NODES];
    int k = d < 0 ? 0 : fs_children(d, kids, FS_MAX_NODES, true);
    if (!k) {
        kprintf("No apps installed yet. Open the App Store with 'store' (or 'winget', 'apt', 'pkg').\n");
        return 0;
    }
    kprintf("%-14s %-20s %-14s %s\n", "Command", "Name", "Category", "Type");
    for (int i = 0; i < k; i++) {
        char name[40], cat[24], type[12];
        info_get(kids[i], "name", name, sizeof name);
        info_get(kids[i], "category", cat, sizeof cat);
        info_get(kids[i], "type", type, sizeof type);
        kprintf("%-14s %-20s %-14s %s\n", fs_nodes[kids[i]].name, name, cat, type);
    }
    return 0;
}

/* ------------------------------------------------------ online repos ---- */
static const char *repos_path = "/etc/axis-repos.txt";

static int repo_file(bool create) {
    int f = fs_resolve(repos_path, FS_ROOT);
    if (f < 0 && create) f = fs_mkfile(repos_path, FS_ROOT);
    return f;
}

/* index lines: id|Name|Category|Description|script-url (relative to index) */
static void parse_index(const char *idx_url, const char *t, uint32_t len, int *added) {
    const char *e = t + len;
    while (t < e && nonline < MAX_ONLINE) {
        const char *nl = memchr(t, '\n', (size_t)(e - t));
        if (!nl) nl = e;
        char line[512];
        size_t l = (size_t)(nl - t);
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, t, l);
        line[l] = 0;
        if (l && line[l - 1] == '\r') line[l - 1] = 0;
        t = nl + 1;
        if (!line[0] || line[0] == '#') continue;
        char *f[5];
        int nf = 0;
        char *p = line;
        f[nf++] = p;
        while (*p && nf < 5)
            if (*p++ == '|') {
                p[-1] = 0;
                f[nf++] = p;
            }
        if (nf < 5 || !fs_valid_name(f[0]) || find_builtin(f[0])) continue;
        app_t *a = &online[nonline];
        memset(a, 0, sizeof *a);
        strlcpy(a->id, f[0], sizeof a->id);
        str_lower(a->id);
        strlcpy(a->name, f[1], sizeof a->name);
        strlcpy(a->category, f[2], sizeof a->category);
        strlcpy(a->desc, f[3], sizeof a->desc);
        if (!strncasecmp(f[4], "http://", 7)) strlcpy(a->url, f[4], sizeof a->url);
        else {
            /* relative to the index file */
            char base[200];
            strlcpy(base, idx_url, sizeof base);
            char *s = strrchr(base, '/');
            if (s) s[1] = 0;
            snprintf(a->url, sizeof a->url, "%s%s", base, f[4]);
        }
        char host[64], path[8];
        uint16_t port;
        url_split(idx_url, host, sizeof host, &port, path, sizeof path);
        snprintf(a->source, sizeof a->source, "online: %.30s", host);
        nonline++;
        (*added)++;
    }
}

static void refresh_online(char *msg, size_t mn) {
    nonline = 0;
    int f = repo_file(false);
    if (f < 0 || !fs_nodes[f].size) {
        snprintf(msg, mn, "No online repositories yet - press A to add one (an http:// index.txt).");
        return;
    }
    if (!net_up()) {
        snprintf(msg, mn, "No network: cannot reach online repositories.");
        return;
    }
    int repos = 0, ok = 0, added = 0;
    const char *t = (const char *)fs_nodes[f].data, *e = t + fs_nodes[f].size;
    while (t < e) {
        const char *nl = memchr(t, '\n', (size_t)(e - t));
        if (!nl) nl = e;
        char url[200];
        size_t l = (size_t)(nl - t);
        if (l >= sizeof url) l = sizeof url - 1;
        memcpy(url, t, l);
        url[l] = 0;
        t = nl + 1;
        if (!url[0] || url[0] == '#') continue;
        repos++;
        scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
        scr_printf(1, 23, theme.accent, "Contacting %.60s ...", url);
        http_resp_t r;
        int st = http_get(url, &r, NULL);
        if (st == 200) {
            ok++;
            parse_index(url, r.body, r.len, &added);
        }
        if (st >= 0) kfree(r.body);
    }
    snprintf(msg, mn, "Checked %d repositor%s (%d reachable): %d online app%s found.", repos, repos == 1 ? "y" : "ies", ok, added,
             added == 1 ? "" : "s");
}

/* ------------------------------------------------------------ store UI -- */
static const char *cats[] = {"All", "Programs", "Games", "Tools", "Windows-style", "Linux-style", "Online", "Installed"};
#define NCATS 8

static int build_list(int cat, const char *filter, const app_t **out, int max) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass ? nonline : NBUILTIN;
        for (int i = 0; i < cnt && n < max; i++) {
            const app_t *a = pass ? &online[i] : &builtin[i];
            bool ok;
            switch (cat) {
            case 0: ok = true; break;
            case 1: ok = strcasestr(a->category, "Linux") || strcasestr(a->category, "Windows") || strcasestr(a->category, "FreeBSD") || strcasestr(a->category, "Program"); break;
            case 6: ok = pass == 1; break;
            case 7: ok = installed(a->id); break;
            default: ok = !strcasecmp(a->category, cats[cat]);
            }
            if (ok && filter[0] && !strcasestr(a->name, filter) && !strcasestr(a->desc, filter) && !strcasestr(a->id, filter)) ok = false;
            if (ok) out[n++] = a;
        }
    }
    return n;
}

int store_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    int cat = 0, sel = 0, top = 0;
    char msg[120] = "", filter[40] = "";
    static bool refreshed;
    if (!refreshed) {
        refreshed = true;
        int f = repo_file(false);
        if (f >= 0 && fs_nodes[f].size && net_up()) refresh_online(msg, sizeof msg);
    }
    for (;;) {
        const app_t *list[NBUILTIN + MAX_ONLINE];
        int n = build_list(cat, filter, list, NBUILTIN + MAX_ONLINE);
        if (sel >= n) sel = n ? n - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + 12) top = sel - 11;

        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
        scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
        scr_puts(1, 0, "AxisOS App Store", theme.bar);
        scr_printf(SCREEN_W - 30, 0, theme.bar, "%s", net_up() ? "online" : "offline (built-in apps only)");
        int x = 1;
        for (int c = 0; c < NCATS; c++) {
            uint8_t a = c == cat ? theme.select : theme.dim;
            scr_printf(x, 2, a, " %s ", cats[c]);
            x += (int)strlen(cats[c]) + 3;
        }
        if (filter[0]) scr_printf(1, 3, theme.accent, "Search: %s   (/ to change, empty to clear)", filter);
        scr_printf(2, 4, theme.accent, "%-20s %-15s %-24s %s", "App", "Category", "Source", "Status");
        for (int r = 0; r < 12; r++) {
            int i = top + r;
            if (i >= n) break;
            const app_t *a = list[i];
            uint8_t at = i == sel ? theme.select : theme.normal;
            scr_fill(1, 5 + r, 78, 1, ' ', at);
            bool inst = installed(a->id);
            scr_printf(2, 5 + r, at, "%-20.20s %-15.15s %-24.24s %s", a->name, a->category, a->source, inst ? "Installed" : "Get");
        }
        if (!n)
            scr_puts(2, 6, cat == 5 ? "No online apps. Press A to add a repository, R to refresh." : "Nothing here.", theme.dim);
        if (n) {
            const app_t *a = list[sel];
            scr_box(1, 17, 78, 5, theme.dim, a->name);
            scr_putsn(3, 18, a->desc, 74, theme.normal);
            scr_printf(3, 19, theme.dim, "Command: %s   Type: %s   %s", a->id, a->native ? "native app" : "AxisScript app",
                       a->url[0] ? a->url : "");
            scr_puts(3, 20, installed(a->id) ? "Enter: open   U: uninstall" : "Enter: install", theme.accent);
        }
        scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
        if (msg[0]) scr_putsn(1, 23, msg, 78, theme.accent);
        msg[0] = 0;
        scr_fill(0, 24, SCREEN_W, 1, ' ', theme.bar);
        scr_puts(1, 24, "Left/Right category  Enter install/open  U remove  / search  R refresh  A add repo  Q", theme.bar);

        int k = kbd_getkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        if (k == KEY_UP && sel) sel--;
        else if (k == KEY_DOWN && sel + 1 < n) sel++;
        else if (k == KEY_LEFT) cat = (cat + NCATS - 1) % NCATS, sel = top = 0;
        else if (k == KEY_RIGHT || k == '\t') cat = (cat + 1) % NCATS, sel = top = 0;
        else if (k == '/') {
            tui_prompt("Search apps", "Find apps by name or description:", filter, sizeof filter);
            con_show_cursor(false);
            sel = top = 0;
        } else if (k == 'r' || k == 'R') {
            refresh_online(msg, sizeof msg);
        } else if (k == 'a' || k == 'A') {
            char url[200] = "http://";
            if (tui_prompt("Add repository", "Address of an index.txt (http:// only):", url, sizeof url) && strlen(url) > 7) {
                con_show_cursor(false);
                if (strncasecmp(url, "http://", 7)) snprintf(msg, sizeof msg, "Only http:// repositories are supported.");
                else {
                    int f = repo_file(true);
                    if (f >= 0) {
                        fs_append(f, url, (uint32_t)strlen(url));
                        fs_append(f, "\n", 1);
                        if (cfg.autosave) fs_save();
                    }
                    refresh_online(msg, sizeof msg);
                    cat = 6;
                    sel = top = 0;
                }
            }
            con_show_cursor(false);
        } else if ((k == 'u' || k == 'U') && n && installed(list[sel]->id)) {
            char q[80];
            snprintf(q, sizeof q, "Remove %s?", list[sel]->name);
            if (adv_mode || tui_confirm("Uninstall", q)) {
                uninstall_app(list[sel]->id);
                snprintf(msg, sizeof msg, "%s removed.", list[sel]->name);
            }
        } else if (k == '\n' && n) {
            const app_t *a = list[sel];
            if (installed(a->id)) {
                char *argv[2] = {(char *)a->id, NULL};
                if (a->native) run_installed(a->id, 1, argv);
                else {
                    /* scripts print to the console: run them on a clean screen */
                    con_set_attr(theme.normal);
                    con_clear();
                    con_show_cursor(true);
                    run_installed(a->id, 1, argv);
                    kprintf("\n[%s finished - press any key to return to the store]", a->name);
                    kbd_getkey();
                    con_show_cursor(false);
                }
            } else {
                scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
                scr_printf(1, 23, theme.accent, "Installing %s ...", a->name);
                install_app(a, msg, sizeof msg);
            }
        }
    }
    scr_restore(saved);
    con_show_cursor(true);
    return 0;
}

/* ===================================================== AxisScript ====== */
/*
 * A script is a list of shell commands plus a few extras:
 *   # / rem / ::     comment          :label / goto label
 *   if A op B [then] command          (op: == != < > <= >=, numeric when both are numbers)
 *   input VAR prompt   (also: read VAR, set /p VAR=prompt)
 *   let VAR = expression  (also: set /a VAR=expression)
 *   random VAR max     pause     exit
 * Arguments are available as $1 $2 ... (and %1 %2 ...).
 */
static bool cond_true(const char *a, const char *op, const char *b) {
    bool num = true;
    for (const char *p = a; *p; p++)
        if (!isdigit(*p) && !(p == a && *p == '-')) num = false;
    for (const char *p = b; *p; p++)
        if (!isdigit(*p) && !(p == b && *p == '-')) num = false;
    if (!*a || !*b) num = false;
    int c = num ? (atoi(a) > atoi(b)) - (atoi(a) < atoi(b)) : strcasecmp(a, b);
    if (!strcmp(op, "==") || !strcmp(op, "=")) return c == 0;
    if (!strcmp(op, "!=") || !strcasecmp(op, "neq")) return c != 0;
    if (!strcmp(op, "<") || !strcasecmp(op, "lss")) return c < 0;
    if (!strcmp(op, ">") || !strcasecmp(op, "gtr")) return c > 0;
    if (!strcmp(op, "<=") || !strcasecmp(op, "leq")) return c <= 0;
    if (!strcmp(op, ">=") || !strcasecmp(op, "geq")) return c >= 0;
    return false;
}

/* read one token (quotes allowed) from *p into out */
static void token(const char **p, char *out, size_t n) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    size_t o = 0;
    char q = 0;
    if (*s == '"' || *s == '\'') q = *s++;
    while (*s && (q ? *s != q : (*s != ' ' && *s != '\t'))) {
        if (o + 1 < n) out[o++] = *s;
        s++;
    }
    if (q && *s == q) s++;
    out[o] = 0;
    *p = s;
}

int script_run(const char *path, int argc, char **argv) {
    const char *p0 = !strncmp(path, "./", 2) ? path + 2 : path;
    int id = fs_resolve(p0, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE) {
        shell_print_error(id < 0 ? id : E_ISDIR, path);
        return 1;
    }
    uint32_t len = fs_nodes[id].size;
    char *src = kmalloc(len + 1);
    if (!src) return 1;
    memcpy(src, fs_nodes[id].data, len);
    src[len] = 0;
    /* split into lines */
    int nl = 1;
    for (uint32_t i = 0; i < len; i++)
        if (src[i] == '\n') nl++;
    char **lines = kmalloc(sizeof(char *) * nl);
    if (!lines) {
        kfree(src);
        return 1;
    }
    int n = 0;
    char *s = src;
    while (n < nl) {
        lines[n++] = s;
        char *e = strchr(s, '\n');
        if (!e) break;
        *e = 0;
        if (e > s && e[-1] == '\r') e[-1] = 0;
        s = e + 1;
    }
    /* arguments */
    char an[4];
    for (int i = 1; i < argc && i < 10; i++) {
        snprintf(an, sizeof an, "%d", i);
        env_set(an, argv[i]);
    }
    int rc = 0, steps = 0;
    for (int pc = 0; pc < n; pc++) {
        if (++steps % 64 == 0 && kbd_ctrl_c_pending()) {
            kprintf("^C\n");
            rc = 130;
            break;
        }
        const char *raw = lines[pc];
        while (*raw == ' ' || *raw == '\t') raw++;
        if (!*raw || *raw == '#' || *raw == ':' || !strncasecmp(raw, "rem ", 4) || !strcasecmp(raw, "rem") ||
            !strncasecmp(raw, "@echo off", 9))
            continue;
        char line[512], rest[512];
        shell_expand(raw, line, sizeof line);
        const char *exec_text = raw; /* what the shell runs (it expands variables itself) */
        const char *p = line;
        char w[64];
    dispatch:
        token(&p, w, sizeof w);

        if (!strcasecmp(w, "goto")) {
            char lbl[64];
            token(&p, lbl, sizeof lbl);
            int target = -1;
            for (int i = 0; i < n; i++) {
                const char *l = lines[i];
                while (*l == ' ' || *l == '\t') l++;
                if (*l == ':' && l[1] != ':' && !strcasecmp(l + 1, lbl)) target = i;
            }
            if (target < 0) {
                shell_print_error(E_NOENT, lbl);
                rc = 1;
                break;
            }
            pc = target;
            continue;
        }
        if (!strcasecmp(w, "exit")) break;
        if (!strcasecmp(w, "pause")) {
            kprintf("Press any key to continue . . . ");
            kbd_getkey();
            kprintf("\n");
            continue;
        }
        if (!strcasecmp(w, "input") || !strcasecmp(w, "read") ||
            (!strcasecmp(w, "set") && !strncasecmp(p, " /p ", 4))) {
            char var[32], prompt[200] = "";
            if (!strcasecmp(w, "set")) {
                p += 4;
                const char *eq = strchr(p, '=');
                size_t l = eq ? (size_t)(eq - p) : strlen(p);
                if (l >= sizeof var) l = sizeof var - 1;
                memcpy(var, p, l);
                var[l] = 0;
                if (eq) strlcpy(prompt, eq + 1, sizeof prompt);
            } else {
                token(&p, var, sizeof var);
                while (*p == ' ') p++;
                strlcpy(prompt, p, sizeof prompt);
            }
            if (prompt[0]) kprintf("%s ", prompt);
            char buf[200];
            int r = line_edit(buf, sizeof buf, false, false);
            if (r < 0) {
                rc = 130;
                break;
            }
            env_set(var, buf);
            continue;
        }
        if (!strcasecmp(w, "let") || (!strcasecmp(w, "set") && !strncasecmp(p, " /a ", 4))) {
            if (!strcasecmp(w, "set")) p += 4;
            const char *eq = strchr(p, '=');
            if (!eq) continue;
            char var[32];
            const char *v = p;
            while (*v == ' ') v++;
            size_t l = (size_t)(eq - v);
            while (l && v[l - 1] == ' ') l--;
            if (l >= sizeof var) l = sizeof var - 1;
            memcpy(var, v, l);
            var[l] = 0;
            long val;
            if (calc_eval(eq + 1, &val)) {
                char num[16];
                snprintf(num, sizeof num, "%d", (int)val);
                env_set(var, num);
            } else
                env_set(var, "0");
            continue;
        }
        if (!strcasecmp(w, "random")) {
            char var[32], mx[16];
            token(&p, var, sizeof var);
            token(&p, mx, sizeof mx);
            int m = atoi(mx) > 0 ? atoi(mx) : 100;
            static uint32_t seed = 0x2545F491;
            seed = seed * 1103515245u + 12345u + ticks;
            char num[16];
            snprintf(num, sizeof num, "%u", (seed >> 8) % (uint32_t)m + 1);
            env_set(var, num);
            continue;
        }
        if (!strcasecmp(w, "if")) {
            bool neg = false;
            char a[128], op[8], b[128];
            token(&p, a, sizeof a);
            if (!strcasecmp(a, "not")) {
                neg = true;
                token(&p, a, sizeof a);
            }
            token(&p, op, sizeof op);
            token(&p, b, sizeof b);
            bool t = cond_true(a, op, b) != neg;
            while (*p == ' ') p++;
            if (!strncasecmp(p, "then ", 5)) p += 5;
            if (!t || !*p) continue;
            /* the rest is a statement of its own (goto, let, echo ...) */
            strlcpy(rest, p, sizeof rest);
            p = rest;
            exec_text = rest;
            goto dispatch;
        }
        if (!strcasecmp(w, "print")) {
            while (*p == ' ') p++;
            kprintf("%s\n", p);
            continue;
        }
        rc = shell_exec_line(exec_text);
    }
    kfree(lines);
    kfree(src);
    return rc;
}
