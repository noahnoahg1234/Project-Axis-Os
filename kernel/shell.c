/*
 * axsh - the AxisOS command shell.
 *
 * Understands Linux, Windows and FreeBSD command names, is fully case
 * insensitive, autocorrects mistyped commands and paths, and supports
 * pipes (|), redirection (> >> <), command chaining (; && ||),
 * environment variables ($VAR, ${VAR}, %VAR%), aliases and history.
 */
#include "axis.h"

const char *shell_stdin;
size_t shell_stdin_len;
const char *cur_cmd_name = "axsh";
static int last_rc;

/* ---- environment ------------------------------------------------------ */
#define MAX_ENV 32
static char env_names[MAX_ENV][32];
static char env_vals[MAX_ENV][128];

static int env_index(const char *n) {
    for (int i = 0; i < MAX_ENV; i++)
        if (env_names[i][0] && !strcasecmp(env_names[i], n)) return i;
    return -1;
}

const char *env_get(const char *name) {
    static char dyn[260];
    int i = env_index(name);
    if (i >= 0) return env_vals[i];
    if (!strcasecmp(name, "HOME") || !strcasecmp(name, "USERPROFILE")) {
        fs_path(fs_home(), dyn, sizeof dyn);
        return dyn;
    }
    if (!strcasecmp(name, "PWD") || !strcasecmp(name, "CD")) {
        fs_path(fs_cwd, dyn, sizeof dyn);
        return dyn;
    }
    if (!strcasecmp(name, "USER") || !strcasecmp(name, "USERNAME") || !strcasecmp(name, "LOGNAME"))
        return adv_mode ? (os_win_style() ? "Administrator" : "root") : cfg.username;
    if (!strcasecmp(name, "HOSTNAME") || !strcasecmp(name, "COMPUTERNAME")) return cfg.hostname;
    if (!strcasecmp(name, "SHELL") || !strcasecmp(name, "COMSPEC"))
        return cur_os == OS_DOS ? "C:\\COMMAND.COM" : cur_os == OS_WINDOWS ? "C:\\Windows\\System32\\cmd.exe" : (cur_os == OS_FREEBSD ? "/bin/csh" : "/bin/bash");
    if (!strcasecmp(name, "OS") || !strcasecmp(name, "OSTYPE")) return os_name(cur_os);
    if (!strcasecmp(name, "PATH"))
        return cur_os == OS_DOS ? "C:\\DOS;C:\\" : cur_os == OS_WINDOWS ? "C:\\Windows\\System32;C:\\Windows;C:\\Program Files" : "/bin:/usr/bin:/usr/local/bin";
    if (!strcasecmp(name, "?") || !strcasecmp(name, "ERRORLEVEL")) {
        snprintf(dyn, sizeof dyn, "%d", last_rc);
        return dyn;
    }
    if (!strcasecmp(name, "RANDOM")) {
        static uint32_t seed = 12345;
        seed = seed * 1103515245u + 12345u + ticks;
        snprintf(dyn, sizeof dyn, "%u", (seed >> 16) % 32768);
        return dyn;
    }
    if (!strcasecmp(name, "DATE") || !strcasecmp(name, "TIME")) {
        rtc_time_t t;
        rtc_local(&t);
        if (!strcasecmp(name, "DATE")) snprintf(dyn, sizeof dyn, "%04d-%02d-%02d", t.year, t.month, t.day);
        else snprintf(dyn, sizeof dyn, "%02d:%02d:%02d", t.hour, t.min, t.sec);
        return dyn;
    }
    return NULL;
}

void env_set(const char *name, const char *value) {
    int i = env_index(name);
    if (i < 0)
        for (i = 0; i < MAX_ENV && env_names[i][0]; i++) {}
    if (i >= MAX_ENV) return;
    strlcpy(env_names[i], name, 32);
    strlcpy(env_vals[i], value, 128);
}
void env_unset(const char *name) {
    int i = env_index(name);
    if (i >= 0) env_names[i][0] = 0;
}
int env_list(char names[][32], char values[][128], int max) {
    static const char *dyn[] = {"HOME", "USER", "HOSTNAME", "SHELL", "OS", "PATH", "PWD"};
    int n = 0;
    for (unsigned d = 0; d < sizeof dyn / sizeof *dyn && n < max; d++) {
        if (env_index(dyn[d]) >= 0) continue;
        const char *nm = dyn[d];
        if (cur_os == OS_WINDOWS) {
            if (!strcmp(nm, "HOME")) nm = "USERPROFILE";
            else if (!strcmp(nm, "USER")) nm = "USERNAME";
            else if (!strcmp(nm, "HOSTNAME")) nm = "COMPUTERNAME";
            else if (!strcmp(nm, "SHELL")) nm = "COMSPEC";
            else if (!strcmp(nm, "PWD")) nm = "CD";
        }
        strlcpy(names[n], nm, 32);
        strlcpy(values[n], env_get(nm), 128);
        n++;
    }
    for (int i = 0; i < MAX_ENV && n < max; i++)
        if (env_names[i][0]) {
            strlcpy(names[n], env_names[i], 32);
            strlcpy(values[n], env_vals[i], 128);
            n++;
        }
    return n;
}

/* ---- aliases ------------------------------------------------------------ */
#define MAX_ALIAS 24
static char alias_names[MAX_ALIAS][24];
static char alias_vals[MAX_ALIAS][96];

static const char *alias_get(const char *n) {
    for (int i = 0; i < MAX_ALIAS; i++)
        if (alias_names[i][0] && !strcasecmp(alias_names[i], n)) return alias_vals[i];
    return NULL;
}
int alias_set(const char *name, const char *value) {
    int free_i = -1;
    for (int i = 0; i < MAX_ALIAS; i++) {
        if (alias_names[i][0] && !strcasecmp(alias_names[i], name)) {
            if (!value || !*value) alias_names[i][0] = 0;
            else strlcpy(alias_vals[i], value, 96);
            return 0;
        }
        if (!alias_names[i][0] && free_i < 0) free_i = i;
    }
    if (!value || !*value) return 0;
    if (free_i < 0) return -1;
    strlcpy(alias_names[free_i], name, 24);
    strlcpy(alias_vals[free_i], value, 96);
    return 0;
}
void alias_list(void) {
    for (int i = 0; i < MAX_ALIAS; i++)
        if (alias_names[i][0]) {
            if (os_win_style()) kprintf("%s=%s\n", alias_names[i], alias_vals[i]);
            else kprintf("alias %s='%s'\n", alias_names[i], alias_vals[i]);
        }
}

/* ---- history ------------------------------------------------------------ */
#define HIST 32
static char history[HIST][256];
static int hist_count;
static void hist_add(const char *l) {
    if (!*l) return;
    if (hist_count && !strcmp(history[(hist_count - 1) % HIST], l)) return;
    strlcpy(history[hist_count % HIST], l, 256);
    hist_count++;
}
int shell_history_count(void) { return hist_count < HIST ? hist_count : HIST; }
const char *shell_history_get(int i) {
    int first = hist_count < HIST ? 0 : hist_count - HIST;
    return history[(first + i) % HIST];
}

/* ---- prompt ------------------------------------------------------------- */
static void cwd_display(char *buf, size_t n) {
    char p[256], h[256];
    fs_path(fs_cwd, p, sizeof p);
    if (os_win_style()) {
        strlcpy(buf, p, n);
        return;
    }
    fs_path(fs_home(), h, sizeof h);
    size_t hl = strlen(h);
    if (!strncasecmp(p, h, hl) && (p[hl] == 0 || p[hl] == '/')) snprintf(buf, n, "~%s", p + hl);
    else strlcpy(buf, p, n);
}

void shell_prompt_string(char *buf, size_t n) {
    char d[256];
    cwd_display(d, sizeof d);
    if (os_win_style()) snprintf(buf, n, "%s%s>", adv_mode ? "[Admin] " : "", d);
    else if (cur_os == OS_FREEBSD) snprintf(buf, n, "%s@%s:%s %c ", env_get("USER"), cfg.hostname, d, adv_mode ? '#' : '%');
    else snprintf(buf, n, "%s@%s:%s%c ", env_get("USER"), cfg.hostname, d, adv_mode ? '#' : '$');
}

static void print_prompt(void) {
    char d[256];
    cwd_display(d, sizeof d);
    uint8_t save = con_get_attr();
    if (os_win_style()) {
        if (adv_mode) {
            con_set_attr(theme.error);
            con_write("[Admin] ");
        }
        con_set_attr(theme.prompt);
        kprintf("%s>", d);
    } else {
        con_set_attr(adv_mode ? theme.error : theme.prompt);
        kprintf("%s@%s", env_get("USER"), cfg.hostname);
        con_set_attr(theme.normal);
        con_write(":");
        con_set_attr(theme.path);
        con_write(d);
        con_set_attr(theme.normal);
        if (cur_os == OS_FREEBSD) kprintf(" %c ", adv_mode ? '#' : '%');
        else kprintf("%c ", adv_mode ? '#' : '$');
    }
    con_set_attr(save);
}

/* ---- line editor --------------------------------------------------------- */
static int le_x0, le_y0;
static uint32_t le_s0;
static void (*le_reprompt)(void);

static void le_begin(void) {
    con_get_cursor(&le_x0, &le_y0);
    le_s0 = con_scroll_count();
}
static void le_place(int pos) {
    int row = le_y0 - (int)(con_scroll_count() - le_s0);
    int lin = row * SCREEN_W + le_x0 + pos;
    if (lin < 0) lin = 0;
    con_set_cursor(lin % SCREEN_W, lin / SCREEN_W);
}
static void le_draw(const char *buf, int len, int pos, int oldlen) {
    con_serial_mirror(false);
    le_place(0);
    con_writen(buf, len);
    for (int i = len; i < oldlen; i++) con_putc(' ');
    /* if writing scrolled the screen, the start row moved: le_place handles it */
    le_place(pos);
    con_serial_mirror(true);
}

static int collect_completions(const char *word, bool cmd, char out[][FS_NAME_MAX], int max, int *dirpart) {
    int n = 0;
    *dirpart = 0;
    if (cmd) {
        for (int i = 0; i < command_count && n < max; i++) {
            const command_t *c = &commands[i];
            if ((c->flags & CMD_ADVANCED) && !adv_mode) continue;
            const char *names[3] = {c->lin, c->win, c->bsd};
            for (int k = 0; k < 3; k++) {
                if (!names[k][0] || strncasecmp(names[k], word, strlen(word))) continue;
                bool dup = false;
                for (int j = 0; j < n; j++)
                    if (!strcasecmp(out[j], names[k])) dup = true;
                if (!dup && n < max) strlcpy(out[n++], names[k], FS_NAME_MAX);
            }
        }
        return n;
    }
    /* file name completion */
    const char *s1 = strrchr(word, '/'), *s2 = strrchr(word, '\\');
    const char *sep = s1 > s2 ? s1 : s2;
    int dir = fs_cwd;
    const char *base = word;
    if (sep) {
        char d[256];
        int l = (int)(sep - word);
        memcpy(d, word, l);
        d[l] = 0;
        dir = l ? fs_resolve(d, fs_cwd) : FS_ROOT;
        if (l == 2 && word[1] == ':') dir = FS_ROOT;
        base = sep + 1;
        *dirpart = l + 1;
    } else if (word[0] == '~' && !word[1]) {
        return 0;
    }
    if (dir < 0 || fs_nodes[dir].type != FT_DIR) return 0;
    int kids[FS_MAX_NODES];
    int k = fs_children(dir, kids, FS_MAX_NODES, true);
    for (int i = 0; i < k && n < max; i++)
        if (!strncasecmp(fs_nodes[kids[i]].name, base, strlen(base))) {
            strlcpy(out[n], fs_nodes[kids[i]].name, FS_NAME_MAX - 1);
            if (fs_nodes[kids[i]].type == FT_DIR) strcat(out[n], os_win_style() ? "\\" : "/");
            n++;
        }
    return n;
}

static void le_complete(char *buf, int *len, int *pos, size_t cap) {
    int ws = *pos;
    while (ws > 0 && buf[ws - 1] != ' ') ws--;
    bool first = true;
    for (int i = 0; i < ws; i++)
        if (buf[i] != ' ') first = false;
    char word[128];
    int wl = *pos - ws;
    if (wl >= (int)sizeof word) return;
    memcpy(word, buf + ws, wl);
    word[wl] = 0;
    static char cands[64][FS_NAME_MAX];
    int dirpart;
    int n = collect_completions(word, first, cands, 64, &dirpart);
    if (!n) return;
    const char *typed = word + dirpart;
    int tl = (int)strlen(typed);
    /* longest common prefix (case-insensitive) */
    int lcp = (int)strlen(cands[0]);
    for (int i = 1; i < n; i++) {
        int j = 0;
        while (j < lcp && cands[i][j] && tolower(cands[i][j]) == tolower(cands[0][j])) j++;
        lcp = j;
    }
    if (lcp > tl || (n == 1)) {
        /* replace typed part with the candidate (fixes its case too) */
        char ins[FS_NAME_MAX + 1];
        memcpy(ins, cands[0], lcp);
        ins[lcp] = 0;
        if (n == 1 && first) strcat(ins, " ");
        int il = (int)strlen(ins);
        int start = ws + dirpart;
        int delta = il - tl;
        if (*len + delta >= (int)cap) return;
        int oldlen = *len;
        memmove(buf + start + il, buf + *pos, *len - *pos);
        memcpy(buf + start, ins, il);
        *len += delta;
        *pos = start + il;
        buf[*len] = 0;
        le_draw(buf, *len, *pos, oldlen);
        return;
    }
    /* ambiguous: list the candidates */
    le_place(*len);
    con_putc('\n');
    for (int i = 0; i < n; i++) kprintf("%-19s", cands[i]);
    con_putc('\n');
    if (le_reprompt) le_reprompt();
    le_begin();
    le_draw(buf, *len, *pos, 0);
}

int line_edit(char *buf, size_t cap, bool use_hist, bool complete) {
    int len = 0, pos = 0, hpos = shell_history_count();
    char saved[256] = "";
    buf[0] = 0;
    le_begin();
    for (;;) {
        int k = kbd_getkey();
        int oldlen = len;
        if (k == '\n') {
            le_place(len);
            con_serial_mirror(false);
            con_putc('\n');
            con_serial_mirror(true);
            serial_write("\n");
            buf[len] = 0;
            return len;
        } else if (k == KEY_CTRL('c')) {
            le_place(len);
            con_write("^C\n");
            buf[0] = 0;
            return -1;
        } else if (k == KEY_CTRL('l')) {
            con_clear();
            if (le_reprompt) le_reprompt();
            le_begin();
            le_draw(buf, len, pos, 0);
        } else if (k == '\b') {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--, len--;
                buf[len] = 0;
                if (pos == len) serial_write("\b \b");
                le_draw(buf, len, pos, oldlen);
            }
        } else if (k == KEY_DELETE || k == KEY_CTRL('d')) {
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos - 1);
                len--;
                buf[len] = 0;
                le_draw(buf, len, pos, oldlen);
            }
        } else if (k == KEY_LEFT) {
            if (pos > 0) le_place(--pos);
        } else if (k == KEY_RIGHT) {
            if (pos < len) le_place(++pos);
        } else if (k == KEY_HOME || k == KEY_CTRL('a')) {
            le_place(pos = 0);
        } else if (k == KEY_END || k == KEY_CTRL('e')) {
            le_place(pos = len);
        } else if (k == KEY_CTRL('u')) {
            len = pos = 0;
            buf[0] = 0;
            le_draw(buf, len, pos, oldlen);
        } else if ((k == KEY_UP || k == KEY_DOWN) && use_hist) {
            int hc = shell_history_count();
            if (k == KEY_UP && hpos > 0) {
                if (hpos == hc) strlcpy(saved, buf, sizeof saved);
                hpos--;
            } else if (k == KEY_DOWN && hpos < hc)
                hpos++;
            else
                continue;
            strlcpy(buf, hpos == hc ? saved : shell_history_get(hpos), cap);
            len = pos = (int)strlen(buf);
            le_draw(buf, len, pos, oldlen);
        } else if (k == '\t' && complete) {
            le_complete(buf, &len, &pos, cap);
        } else if (k >= 32 && k < 127 && len + 1 < (int)cap) {
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos++] = (char)k;
            len++;
            buf[len] = 0;
            if (pos == len) serial_putc((char)k);
            le_draw(buf, len, pos, oldlen);
        }
    }
}

/* ---- errors & prompts ------------------------------------------------ */
void shell_print_error(int code, const char *what) {
    uint8_t save = con_get_attr();
    con_set_attr(theme.error);
    if (adv_mode) {
        kprintf("E%d %s\n", -code, what ? what : "");
    } else if (os_win_style()) {
        switch (code) {
        case E_NOENT: kprintf("The system cannot find the file specified: %s\n", what); break;
        case E_NOTDIR: kprintf("The directory name is invalid: %s\n", what); break;
        case E_EXIST: kprintf("A subdirectory or file %s already exists.\n", what); break;
        case E_NOTEMPTY: kprintf("The directory is not empty: %s\n", what); break;
        case E_ROFS: case E_PERM: kprintf("Access is denied: %s\n", what); break;
        case E_INVAL: kprintf("The filename, directory name, or volume label syntax is incorrect: %s\n", what); break;
        case E_ISDIR: kprintf("%s is a directory.\n", what); break;
        default: kprintf("%s: %s.\n", what, fs_strerror(code));
        }
    } else {
        kprintf("%s: %s: %s\n", cur_cmd_name, what, fs_strerror(code));
    }
    con_set_attr(save);
}

bool shell_confirm(const char *q) {
    if (adv_mode || !cfg.confirm) return true;
    kprintf("%s %s ", q, os_win_style() ? "(Y/N)?" : "[y/N]");
    int k = kbd_getkey();
    bool yes = (k == 'y' || k == 'Y');
    kprintf("%c\n", (k >= 32 && k < 127) ? k : ' ');
    return yes;
}

/* ---- command lookup --------------------------------------------------- */
static bool in_list(const char *list, const char *name) {
    size_t nl = strlen(name);
    while (*list) {
        const char *e = strchr(list, '|');
        size_t l = e ? (size_t)(e - list) : strlen(list);
        if (l == nl && !strncasecmp(list, name, l)) return true;
        if (!e) break;
        list = e + 1;
    }
    return false;
}
bool cmd_name_matches(const command_t *c, const char *n) {
    return !strcasecmp(c->lin, n) || !strcasecmp(c->win, n) || !strcasecmp(c->bsd, n) || in_list(c->aliases, n);
}
const command_t *cmd_find(const char *name) {
    if (!*name) return NULL;
    for (int i = 0; i < command_count; i++)
        if (cmd_name_matches(&commands[i], name)) return &commands[i];
    return NULL;
}

/* best autocorrect candidates; returns how many are equally good.
 * Native command names win ties against extra aliases. */
static void consider(const char *word, const char *nm, bool alias, int *bd, char best[][32], int *n, int max) {
    if (!nm[0]) return;
    int d = edit_distance(word, nm);
    if (d > ac_threshold(word)) return;
    d = d * 2 + (alias ? 1 : 0);
    if (d > *bd) return;
    if (d < *bd) {
        *bd = d;
        *n = 0;
    }
    for (int j = 0; j < *n; j++)
        if (!strcasecmp(best[j], nm)) return;
    if (*n < max) strlcpy(best[(*n)++], nm, 32);
}

static int cmd_suggest(const char *word, char best[][32], int max) {
    int bd = 1000, n = 0;
    for (int i = 0; i < command_count; i++) {
        const command_t *c = &commands[i];
        if ((c->flags & CMD_ADVANCED) && !adv_mode) continue;
        if (c->flags & CMD_HIDDEN) continue;
        consider(word, c->lin, false, &bd, best, &n, max);
        consider(word, c->win, false, &bd, best, &n, max);
        consider(word, c->bsd, false, &bd, best, &n, max);
        const char *l = c->aliases;
        while (*l) {
            char nm[32];
            int k = 0;
            while (*l && *l != '|' && k < 31) nm[k++] = *l++;
            nm[k] = 0;
            if (*l == '|') l++;
            consider(word, nm, true, &bd, best, &n, max);
        }
    }
    for (int i = 0; i < MAX_ALIAS; i++)
        if (alias_names[i][0]) consider(word, alias_names[i], false, &bd, best, &n, max);
    return n;
}

static void not_found(const char *name) {
    uint8_t save = con_get_attr();
    con_set_attr(theme.error);
    if (adv_mode) kprintf("?\n");
    else if (cur_os == OS_DOS)
        kprintf("Bad command or file name\n");
    else if (cur_os == OS_WINDOWS)
        kprintf("'%s' is not recognized as an internal or external command,\noperable program or batch file.\n", name);
    else if (cur_os == OS_FREEBSD) kprintf("%s: Command not found.\n", name);
    else kprintf("axsh: %s: command not found\n", name);
    con_set_attr(save);
}

/* ---- tokenizer ---------------------------------------------------------- */
static int tokenize(char *s, char **argv, int max) {
    int argc = 0;
    char *out = s;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (argc >= max - 1) break;
        argv[argc++] = out;
        char q = 0;
        while (*s) {
            if (q) {
                if (*s == q) { q = 0; s++; continue; }
            } else {
                if (*s == '"' || *s == '\'') { q = *s++; continue; }
                if (*s == ' ' || *s == '\t') break;
            }
            *out++ = *s++;
        }
        if (*s) s++;
        *out++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

/* expand $VAR ${VAR} %VAR% $? (not inside single quotes) */
void shell_expand(const char *in, char *out, size_t n);
static void expand_vars(const char *in, char *out, size_t n) {
    size_t o = 0;
    bool sq = false;
    while (*in && o + 1 < n) {
        if (*in == '\'' ) sq = !sq;
        const char *val = NULL;
        char name[32];
        if (!sq && *in == '$' && (in[1] == '{' || isalnum(in[1]) || in[1] == '_' || in[1] == '?')) {
            const char *p = in + 1;
            bool br = *p == '{';
            if (br) p++;
            int l = 0;
            if (*p == '?') name[l++] = *p++;
            else
                while ((isalnum(*p) || *p == '_') && l < 31) name[l++] = *p++;
            name[l] = 0;
            if (br && *p == '}') p++;
            val = env_get(name);
            in = p;
            if (!val) val = "";
        } else if (!sq && *in == '%') {
            const char *e = strchr(in + 1, '%');
            if (e && e - in - 1 > 0 && e - in - 1 < 31) {
                int l = (int)(e - in - 1);
                memcpy(name, in + 1, l);
                name[l] = 0;
                bool ok = true;
                for (int i = 0; i < l; i++)
                    if (!isalnum(name[i]) && name[i] != '_') ok = false;
                const char *v = ok ? env_get(name) : NULL;
                if (v) {
                    val = v;
                    in = e + 1;
                }
            }
        }
        if (val) {
            while (*val && o + 1 < n) out[o++] = *val++;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

void shell_expand(const char *in, char *out, size_t n) { expand_vars(in, out, n); }

/* ---- execution ---------------------------------------------------------- */
static int run_argv(int argc, char **argv) {
    if (!argc) return 0;
    char *name = argv[0];
    static char splitbuf[64];

    /* "cd.." / "cd\" (cmd.exe style, no space) */
    if (!cmd_find(name) && !strncasecmp(name, "cd", 2) && (name[2] == '.' || name[2] == '\\' || name[2] == '/')) {
        strlcpy(splitbuf, name + 2, sizeof splitbuf);
        static char *nargv[3];
        static char cdname[] = "cd";
        nargv[0] = cdname;
        nargv[1] = splitbuf;
        nargv[2] = NULL;
        return run_argv(2, nargv);
    }
    /* bare drive letter "C:" */
    if (isalpha(name[0]) && name[1] == ':' && !name[2] && argc == 1) {
        if (tolower(name[0]) != 'c') {
            shell_print_error(E_NOENT, name);
            return 1;
        }
        return 0;
    }

    const command_t *c = cmd_find(name);
    if (c && (c->flags & CMD_ADVANCED) && !adv_mode) {
        uint8_t s = con_get_attr();
        con_set_attr(theme.error);
        kprintf("%s: this is an Advanced mode command. Enable it with 'advanced on'.\n", name);
        con_set_attr(s);
        return 1;
    }
    if (!c && app_try_run(argc, argv)) return 0;
    if (!c) {
        /* a script file typed by name: ./x.sh, x.bat */
        size_t nl = strlen(name);
        bool dotslash = !strncmp(name, "./", 2);
        const char *rp = dotslash ? name + 2 : name;
        if ((nl > 3 && (!strcasecmp(name + nl - 3, ".sh") || !strcasecmp(name + nl - 4, ".bat") || !strcasecmp(name + nl - 4, ".cmd"))) || dotslash) {
            int id = fs_resolve(rp, fs_cwd);
            if (id >= 0 && fs_nodes[id].type == FT_FILE) {
                if (compat_is_program(rp)) return compat_run(rp, argc, argv);
                return script_run(name, argc, argv);
            }
        }
        if (compat_is_program(name)) return compat_run(name, argc, argv);
        if (adv_mode || cfg.autocorrect == AC_OFF) {
            not_found(name);
            return 127;
        }
        char best[6][32];
        int n = cmd_suggest(name, best, 6);
        if (n == 1 && cfg.autocorrect == AC_AUTO) {
            uint8_t s = con_get_attr();
            con_set_attr(theme.accent);
            kprintf("autocorrect: %s -> %s\n", name, best[0]);
            con_set_attr(s);
            const char *av = alias_get(best[0]);
            if (av) {
                char line[256];
                snprintf(line, sizeof line, "%s", best[0]);
                for (int i = 1; i < argc; i++) {
                    strlcpy(line + strlen(line), " \"", sizeof line - strlen(line));
                    strlcpy(line + strlen(line), argv[i], sizeof line - strlen(line));
                    strlcpy(line + strlen(line), "\"", sizeof line - strlen(line));
                }
                return shell_exec_line(line);
            }
            c = cmd_find(best[0]);
            static char fixed[32];
            strlcpy(fixed, best[0], sizeof fixed);
            argv[0] = fixed;
        } else {
            not_found(name);
            if (n) {
                uint8_t s = con_get_attr();
                con_set_attr(theme.accent);
                kprintf("Did you mean: ");
                for (int i = 0; i < n; i++) kprintf("%s%s", best[i], i + 1 < n ? ", " : "?\n");
                con_set_attr(s);
            }
            return 127;
        }
    }
    if (!c) return 127;
    if (argc >= 2 && (!strcmp(argv[1], "/?") || !strcasecmp(argv[1], "--help"))) {
        kprintf("%s\n\n  Usage: %s\n\n", c->desc, c->usage);
        kprintf("  Linux: %s   Windows: %s   FreeBSD: %s\n", c->lin[0] ? c->lin : "-", c->win[0] ? c->win : "-",
                c->bsd[0] ? c->bsd : "-");
        if (c->aliases[0]) kprintf("  Also:  %s\n", c->aliases);
        return 0;
    }
    const char *prev = cur_cmd_name;
    cur_cmd_name = argv[0];
    int rc = c->fn(argc, argv);
    cur_cmd_name = prev;
    return rc;
}

/* split one stage into argv, applying alias expansion; returns argc */
static int build_argv(const char *stage, char *store, size_t storen, char **argv) {
    strlcpy(store, stage, storen);
    int argc = tokenize(store, argv, MAX_ARGS);
    if (!argc) return 0;
    const char *av = alias_get(argv[0]);
    if (av) {
        /* rebuild: alias value + original remaining text */
        const char *rest = stage;
        while (*rest == ' ') rest++;
        while (*rest && *rest != ' ') rest++;
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s%s", av, rest);
        strlcpy(store, tmp, storen);
        argc = tokenize(store, argv, MAX_ARGS);
    }
    return argc;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
    return s;
}

static int run_pipeline(char *seg) {
    char exp[512];
    expand_vars(seg, exp, sizeof exp);

    /* split on '|' outside quotes; find redirections */
    char *stages[8];
    int ns = 0;
    char *redir_out = NULL, *redir_in = NULL;
    bool append = false;
    char q = 0;
    stages[ns++] = exp;
    for (char *p = exp; *p; p++) {
        if (q) {
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '|' && ns < 8) {
            *p = 0;
            stages[ns++] = p + 1;
        } else if (*p == '>') {
            *p = 0;
            if (p[1] == '>') {
                append = true;
                p++;
            }
            redir_out = p + 1;
        } else if (*p == '<') {
            *p = 0;
            redir_in = p + 1;
        }
    }
    char outname[256] = "", inname[256] = "";
    if (redir_out) {
        char *a[4];
        char t[256];
        strlcpy(t, redir_out, sizeof t);
        if (tokenize(t, a, 4) < 1) {
            shell_print_error(E_INVAL, "missing file name after '>'");
            return 2;
        }
        strlcpy(outname, a[0], sizeof outname);
    }
    if (redir_in) {
        char *a[4];
        char t[256];
        strlcpy(t, redir_in, sizeof t);
        if (tokenize(t, a, 4) >= 1) strlcpy(inname, a[0], sizeof inname);
    }

    char *input = NULL;
    size_t inlen = 0;
    if (inname[0]) {
        int id = fs_resolve(inname, fs_cwd);
        if (id < 0 || fs_nodes[id].type != FT_FILE) {
            shell_print_error(id < 0 ? id : E_ISDIR, inname);
            return 1;
        }
        input = kmalloc(fs_nodes[id].size + 1);
        if (!input) return 1;
        memcpy(input, fs_nodes[id].data, fs_nodes[id].size);
        input[fs_nodes[id].size] = 0;
        inlen = fs_nodes[id].size;
    }

    int rc = 0;
    for (int i = 0; i < ns; i++) {
        char store[512];
        char *argv[MAX_ARGS + 1];
        int argc = build_argv(trim(stages[i]), store, sizeof store, argv);
        bool last = (i == ns - 1);
        bool capture = !last || outname[0];
        shell_stdin = input;
        shell_stdin_len = inlen;
        if (capture) con_capture_begin();
        rc = argc ? run_argv(argc, argv) : 0;
        char *out = NULL;
        size_t outlen = 0;
        if (capture) out = con_capture_end(&outlen);
        kfree(input);
        input = out;
        inlen = outlen;
    }
    shell_stdin = NULL;
    shell_stdin_len = 0;
    if (outname[0] && input) {
        int id = fs_mkfile(outname, fs_cwd);
        if (id < 0) {
            shell_print_error(id, outname);
            rc = 1;
        } else {
            int r = append ? fs_append(id, input, (uint32_t)inlen) : fs_write(id, input, (uint32_t)inlen);
            if (r < 0) {
                shell_print_error(r, outname);
                rc = 1;
            }
        }
    }
    kfree(input);
    return rc;
}

int shell_exec_line(const char *line) {
    static int depth;
    if (depth > 8) return 1; /* alias loops */
    depth++;
    char buf[512];
    strlcpy(buf, line, sizeof buf);
    char *p = buf, *start = buf;
    int rc = 0;
    int op = 0; /* 0 = ;  1 = &&  2 = || */
    char q = 0;
    for (;; p++) {
        bool end = !*p;
        int nop = -1;
        if (!end) {
            if (q) {
                if (*p == q) q = 0;
                continue;
            }
            if (*p == '"' || *p == '\'') {
                q = *p;
                continue;
            }
            if (*p == ';') nop = 0;
            else if (*p == '&' && p[1] == '&') nop = 1;
            else if (*p == '&') nop = 0; /* single & : DOS/cmd sequential separator */
            else if (*p == '|' && p[1] == '|') nop = 2;
            else continue;
        }
        *p = 0;
        char *seg = trim(start);
        if (*seg) {
            bool run = op == 0 || (op == 1 && rc == 0) || (op == 2 && rc != 0);
            if (run) rc = run_pipeline(seg);
        }
        if (end) break;
        op = nop;
        if (nop > 0) p++;
        start = p + 1;
    }
    last_rc = rc;
    depth--;
    return rc;
}

/* ---- main loop ---------------------------------------------------------- */
void shell_banner(void);

void shell_main(void) {
    char line[256];
    le_reprompt = print_prompt;
    for (;;) {
        con_set_attr(theme.normal);
        print_prompt();
        int n = line_edit(line, sizeof line, true, true);
        if (n <= 0) continue;
        hist_add(line);
        shell_exec_line(line);
        if (fs_dirty && cfg.autosave) fs_save();
    }
}
