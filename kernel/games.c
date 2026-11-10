/*
 * AxisOS native apps offered by the App Store:
 * Calculator, Minesweeper, Paint, Snake, Big Clock and Task Monitor.
 */
#include "axis.h"

static int abs_i(int v) { return v < 0 ? -v : v; }
static uint32_t rng_state = 2463534242u;
static uint32_t rnd(void) {
    rng_state ^= ticks;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void title_bar(const char *t, const char *keys) {
    scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
    scr_puts(1, 0, t, theme.bar);
    scr_fill(0, 24, SCREEN_W, 1, ' ', theme.bar);
    if (keys) scr_puts(1, 24, keys, theme.bar);
}

/* =========================================================== Calculator == */
int app_calculator(void) {
    char expr[64] = "";
    char hist[8][80];
    int nh = 0;
    static const char *pad[4] = {" 7  8  9  / ", " 4  5  6  * ", " 1  2  3  - ", " 0  (  )  + "};
    for (;;) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
        title_bar("Calculator", "Type a sum   Enter =   Backspace delete   C clear   Esc quit");
        scr_box(20, 2, 40, 5, theme.accent, "Display");
        char shown[40];
        int l = (int)strlen(expr);
        strlcpy(shown, l > 36 ? expr + l - 36 : expr, sizeof shown);
        scr_printf(57 - (int)strlen(shown), 4, theme.prompt, "%s", shown[0] ? shown : "0");
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                char key[6];
                snprintf(key, sizeof key, "[%.2s]", pad[r] + c * 3 + 1);
                scr_puts(26 + c * 7, 8 + r * 2, key, c == 3 ? theme.accent : theme.normal);
            }
        scr_puts(26, 16, "[ C]   [ =]   ( %  = remainder )", theme.dim);
        scr_puts(20, 18, "History", theme.accent);
        for (int i = 0; i < nh; i++) scr_puts(20, 19 + i, hist[i], theme.dim);
        con_set_cursor(57, 4);
        int k = kbd_getkey();
        if (k == KEY_ESC || k == 'q' || k == 'Q') break;
        if (k == 'c' || k == 'C') expr[0] = 0;
        else if (k == '\b') {
            if (l) expr[l - 1] = 0;
        } else if (k == '\n' || k == '=') {
            long v;
            char line[80];
            if (!expr[0]) continue;
            if (calc_eval(expr, &v)) {
                snprintf(line, sizeof line, "%s = %d", expr, (int)v);
                snprintf(expr, sizeof expr, "%d", (int)v);
            } else {
                snprintf(line, sizeof line, "%s = error", expr);
                expr[0] = 0;
            }
            if (nh == 5) {
                memmove(hist, hist + 1, sizeof hist[0] * 4);
                nh--;
            }
            strlcpy(hist[nh++], line, 80);
        } else if ((isdigit(k) || strchr("+-*/%() ", k)) && k && l < 60) {
            expr[l] = (char)k;
            expr[l + 1] = 0;
        }
    }
    return 0;
}

/* ========================================================== Minesweeper == */
#define MS_MAX 16
static uint8_t ms_mine[MS_MAX][MS_MAX], ms_open[MS_MAX][MS_MAX], ms_flag[MS_MAX][MS_MAX];
static int ms_w, ms_h, ms_n;

static int ms_count(int x, int y) {
    int c = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < ms_w && ny < ms_h && ms_mine[ny][nx]) c++;
        }
    return c;
}
static void ms_reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= ms_w || y >= ms_h || ms_open[y][x] || ms_flag[y][x]) return;
    ms_open[y][x] = 1;
    if (!ms_mine[y][x] && !ms_count(x, y))
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) ms_reveal(x + dx, y + dy);
}
static void ms_place(int sx, int sy) {
    int placed = 0;
    while (placed < ms_n) {
        int x = (int)(rnd() % ms_w), y = (int)(rnd() % ms_h);
        if (ms_mine[y][x] || (abs_i(x - sx) <= 1 && abs_i(y - sy) <= 1)) continue;
        ms_mine[y][x] = 1;
        placed++;
    }
}

int app_minesweeper(void) {
    int level = 0;
restart:
    ms_w = ms_h = level ? 16 : 9;
    ms_n = level ? 40 : 10;
    memset(ms_mine, 0, sizeof ms_mine);
    memset(ms_open, 0, sizeof ms_open);
    memset(ms_flag, 0, sizeof ms_flag);
    int cx = ms_w / 2, cy = ms_h / 2, state = 0; /* 0 play, 1 lost, 2 won */
    bool first = true;
    uint32_t start = ticks;
    for (;;) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
        title_bar("Minesweeper", "Arrows move  Space open  F flag  N new game  L level  Q quit");
        int ox = 40 - ms_w, oy = 3;
        int flags = 0, opened = 0;
        for (int y = 0; y < ms_h; y++)
            for (int x = 0; x < ms_w; x++) {
                flags += ms_flag[y][x];
                opened += ms_open[y][x];
                char ch = (char)0xFA;
                uint8_t a = ATTR(C_LGRAY, C_DGRAY);
                if (ms_open[y][x] || (state == 1 && ms_mine[y][x])) {
                    a = ATTR(C_BLACK, C_LGRAY);
                    if (ms_mine[y][x]) {
                        ch = '*';
                        a = ATTR(C_WHITE, C_RED);
                    } else {
                        int n = ms_count(x, y);
                        static const uint8_t nc[9] = {0, C_BLUE, C_GREEN, C_RED, C_MAGENTA, C_BROWN, C_CYAN, C_BLACK, C_DGRAY};
                        ch = n ? (char)('0' + n) : ' ';
                        a = ATTR(nc[n], C_LGRAY);
                    }
                } else if (ms_flag[y][x]) {
                    ch = 'F';
                    a = ATTR(C_LRED, C_DGRAY);
                }
                if (x == cx && y == cy && state == 0) a = ATTR(C_BLACK, C_YELLOW);
                scr_put(ox + x * 2, oy + y, ch, a);
                scr_put(ox + x * 2 + 1, oy + y, ' ', a);
            }
        uint32_t secs = (ticks - start) / TICK_HZ;
        scr_printf(ox, oy + ms_h + 1, theme.accent, "Mines: %d   Flags: %d   Time: %us   %s", ms_n, flags, first ? 0 : secs,
                   level ? "Expert 16x16" : "Beginner 9x9");
        if (state == 1) scr_puts(ox, oy + ms_h + 3, "BOOM! You hit a mine. Press N to play again.", theme.error);
        if (state == 2) scr_puts(ox, oy + ms_h + 3, "You cleared the field! Press N to play again.", theme.prompt);
        if (state == 0 && opened == ms_w * ms_h - ms_n && !first) state = 2;
        int k = kbd_getkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        if (k == 'n' || k == 'N') goto restart;
        if (k == 'l' || k == 'L') {
            level ^= 1;
            goto restart;
        }
        if (state) continue;
        if (k == KEY_LEFT && cx > 0) cx--;
        else if (k == KEY_RIGHT && cx < ms_w - 1) cx++;
        else if (k == KEY_UP && cy > 0) cy--;
        else if (k == KEY_DOWN && cy < ms_h - 1) cy++;
        else if ((k == 'f' || k == 'F') && !ms_open[cy][cx]) ms_flag[cy][cx] ^= 1;
        else if (k == ' ' || k == '\n') {
            if (first) {
                ms_place(cx, cy);
                first = false;
                start = ticks;
            }
            if (ms_flag[cy][cx]) continue;
            if (ms_mine[cy][cx]) state = 1;
            else ms_reveal(cx, cy);
        }
    }
    return 0;
}

/* ================================================================ Paint == */
#define CW 78
#define CH 21
int app_paint(void) {
    static uint16_t canvas[CH][CW];
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) canvas[y][x] = (uint16_t)(ATTR(C_WHITE, C_BLACK) << 8 | ' ');
    static const char brushes[] = {(char)0xDB, (char)0xB2, (char)0xB1, (char)0xB0, '*', '#', 'o', '+', '.'};
    int px = CW / 2, py = CH / 2, color = C_LRED, brush = 0;
    bool pen = false;
    char msg[80] = "";
    for (;;) {
        title_bar("Paint", "Arrows move  Space paint  P pen up/down  1-9/0 colour  B brush  E erase  S save  L load  Q");
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) {
                uint16_t c = canvas[y][x];
                scr_put(1 + x, 2 + y, (char)(c & 0xFF), (uint8_t)(c >> 8));
            }
        scr_fill(0, 1, SCREEN_W, 1, ' ', theme.normal);
        scr_printf(1, 1, theme.normal, "Colour ");
        scr_put(8, 1, (char)0xDB, (uint8_t)color);
        scr_printf(10, 1, theme.normal, "Brush %c   Pen %s   %s", brushes[brush], pen ? "DOWN (drawing)" : "up", msg);
        msg[0] = 0;
        uint16_t under = canvas[py][px];
        scr_put(1 + px, 2 + py, pen ? 'X' : '+', ATTR(C_BLACK, C_YELLOW));
        (void)under;
        scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
        int k = kbd_getkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        if (k == KEY_LEFT && px > 0) px--;
        else if (k == KEY_RIGHT && px < CW - 1) px++;
        else if (k == KEY_UP && py > 0) py--;
        else if (k == KEY_DOWN && py < CH - 1) py++;
        else if (k >= '1' && k <= '9') color = k - '0' + 6; /* 7..15 bright colours */
        else if (k == '0') color = C_BLACK;
        else if (k == 'b' || k == 'B') brush = (brush + 1) % (int)sizeof brushes;
        else if (k == 'p' || k == 'P') pen = !pen;
        else if (k == 'e' || k == 'E') canvas[py][px] = (uint16_t)(ATTR(C_WHITE, C_BLACK) << 8 | ' ');
        else if (k == 's' || k == 'S' || k == 'l' || k == 'L') {
            bool save = (k == 's' || k == 'S');
            char name[FS_NAME_MAX] = "picture.pnt";
            if (!tui_prompt(save ? "Save picture" : "Open picture", "File name:", name, sizeof name) || !name[0]) continue;
            con_show_cursor(false);
            if (save) {
                int id = fs_mkfile(name, fs_cwd);
                int r = id < 0 ? id : fs_write(id, canvas, sizeof canvas);
                snprintf(msg, sizeof msg, r < 0 ? "Save failed: %s" : "Saved %s", r < 0 ? fs_strerror(r) : name);
            } else {
                int id = fs_resolve(name, fs_cwd);
                if (id < 0 || fs_nodes[id].size != sizeof canvas) snprintf(msg, sizeof msg, "Not a Paint picture: %s", name);
                else memcpy(canvas, fs_nodes[id].data, sizeof canvas);
            }
            continue;
        }
        if (k == ' ' || (pen && (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_UP || k == KEY_DOWN)))
            canvas[py][px] = (uint16_t)(ATTR(color, C_BLACK) << 8 | (uint8_t)brushes[brush]);
    }
    return 0;
}

/* ================================================================ Snake == */
int app_snake(void) {
    enum { FW = 60, FH = 20, MAXLEN = 600 };
    static int sx[MAXLEN], sy[MAXLEN];
    static int best;
new_game:;
    int len = 4, dx = 1, dy = 0, score = 0;
    for (int i = 0; i < len; i++) sx[i] = 10 - i, sy[i] = 10;
    int fx = 30, fy = 8;
    bool dead = false;
    int ox = 10, oy = 2;
    for (;;) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
        title_bar("Snake", "Arrows steer   P pause   N new game   Q quit");
        scr_box(ox - 1, oy - 1, FW + 2, FH + 2, theme.dim, NULL);
        scr_put(ox + fx, oy + fy, (char)0x04, ATTR(C_LRED, theme.normal >> 4));
        for (int i = len - 1; i >= 0; i--) scr_put(ox + sx[i], oy + sy[i], i ? 'o' : '@', ATTR(i ? C_GREEN : C_LGREEN, theme.normal >> 4));
        scr_printf(ox, oy + FH + 1, theme.accent, "Score %d   Best %d", score, best);
        if (dead) {
            scr_puts(ox + 20, oy + FH / 2, " GAME OVER - press N ", theme.select);
            int k = kbd_getkey();
            if (k == 'n' || k == 'N') goto new_game;
            if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
            continue;
        }
        uint32_t wait = ticks + (uint32_t)(score > 30 ? 6 : 12 - score / 5);
        int k = -1, kk;
        while ((int32_t)(wait - ticks) > 0) {
            if ((kk = kbd_trygetkey()) >= 0) k = kk;
            cpu_hlt();
        }
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        if (k == 'n' || k == 'N') goto new_game;
        if (k == 'p' || k == 'P') kbd_getkey();
        if (k == KEY_UP && dy != 1) dx = 0, dy = -1;
        else if (k == KEY_DOWN && dy != -1) dx = 0, dy = 1;
        else if (k == KEY_LEFT && dx != 1) dx = -1, dy = 0;
        else if (k == KEY_RIGHT && dx != -1) dx = 1, dy = 0;
        int nx = sx[0] + dx, ny = sy[0] + dy;
        if (nx < 0 || ny < 0 || nx >= FW || ny >= FH) dead = true;
        for (int i = 0; i < len - 1 && !dead; i++)
            if (sx[i] == nx && sy[i] == ny) dead = true;
        if (dead) {
            if (score > best) best = score;
            continue;
        }
        bool eat = nx == fx && ny == fy;
        if (eat && len < MAXLEN) len++;
        for (int i = len - 1; i > 0; i--) sx[i] = sx[i - 1], sy[i] = sy[i - 1];
        sx[0] = nx;
        sy[0] = ny;
        if (eat) {
            score++;
            for (int t = 0; t < 100; t++) {
                fx = (int)(rnd() % FW);
                fy = (int)(rnd() % FH);
                bool on = false;
                for (int i = 0; i < len; i++)
                    if (sx[i] == fx && sy[i] == fy) on = true;
                if (!on) break;
            }
        }
    }
    return 0;
}

/* ============================================================ Big clock == */
static const char *digits[11][5] = {
    {"###", "# #", "# #", "# #", "###"}, {"  #", "  #", "  #", "  #", "  #"}, {"###", "  #", "###", "#  ", "###"},
    {"###", "  #", "###", "  #", "###"}, {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
    {"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"}, {"###", "# #", "###", "# #", "###"},
    {"###", "# #", "###", "  #", "###"}, {"   ", " # ", "   ", " # ", "   "}};

int app_clock(void) {
    int last = -1;
    for (;;) {
        rtc_time_t t;
        rtc_local(&t);
        if (t.sec != last) {
            last = t.sec;
            scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
            title_bar("Clock", "Q quit");
            int h = cfg.clock24 ? t.hour : (t.hour + 11) % 12 + 1;
            int d[8] = {h / 10, h % 10, 10, t.min / 10, t.min % 10, 10, t.sec / 10, t.sec % 10};
            int x = 8;
            for (int i = 0; i < 8; i++) {
                for (int r = 0; r < 5; r++)
                    for (int c = 0; c < 3; c++)
                        if (digits[d[i]][r][c] == '#') {
                            scr_put(x + c * 2, 7 + r * 2, (char)0xDB, theme.accent);
                            scr_put(x + c * 2 + 1, 7 + r * 2, (char)0xDB, theme.accent);
                            scr_put(x + c * 2, 8 + r * 2, (char)0xDB, theme.accent);
                            scr_put(x + c * 2 + 1, 8 + r * 2, (char)0xDB, theme.accent);
                        }
                x += d[i] == 10 ? 6 : 8;
            }
            scr_printf(28, 19, theme.normal, "%s %d %s %d%s", rtc_wday_name(t.wday), t.day, rtc_month_name(t.month), t.year,
                       cfg.clock24 ? "" : (t.hour < 12 ? "  AM" : "  PM"));
        }
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        cpu_hlt();
    }
    return 0;
}

/* ========================================================= Task Monitor == */
static void bar(int x, int y, int w, uint32_t used, uint32_t total) {
    int f = total >= 1024 ? (int)((used / 1024) * (uint32_t)w / (total / 1024)) : 0;
    if (f > w) f = w;
    for (int i = 0; i < w; i++) scr_put(x + i, y, i < f ? (char)0xDB : (char)0xB0, i < f ? theme.accent : theme.dim);
}

int app_monitor(void) {
    uint32_t last = 0;
    uint32_t prx = 0, ptx = 0;
    for (;;) {
        if (ticks - last >= TICK_HZ || !last) {
            last = ticks;
            scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
            title_bar(cur_os == OS_WINDOWS ? "Task Manager" : "System Monitor (top)", "Updates every second   Q quit");
            uint32_t total, used, blocks;
            heap_stats(&total, &used, &blocks);
            int nodes;
            uint32_t bytes;
            fs_stats(&nodes, &bytes);
            uint32_t up = ticks / TICK_HZ;
            char brand[49];
            cpu_brand(brand);
            scr_printf(2, 2, theme.accent, "CPU");
            scr_printf(12, 2, theme.normal, "%s", brand);
            scr_printf(2, 4, theme.accent, "Memory");
            bar(12, 4, 40, used + 640 * 1024, mem_total_kb() * 1024);
            scr_printf(54, 4, theme.normal, "%u KB heap in use", used / 1024);
            scr_printf(2, 6, theme.accent, "Disk");
            bar(12, 6, 40, bytes, FS_SECTORS * 512);
            scr_printf(54, 6, theme.normal, "%u KB, %d files", bytes / 1024, nodes);
            scr_printf(2, 8, theme.accent, "Network");
            if (net_present()) {
                uint32_t rx, tx;
                net_stats(&rx, &tx);
                char ip[16];
                ip_to_str(net_ip, ip);
                scr_printf(12, 8, theme.normal, "%s  rx %u (+%u/s)  tx %u (+%u/s)", ip, rx, prx ? rx - prx : 0, tx, ptx ? tx - ptx : 0);
                prx = rx;
                ptx = tx;
            } else
                scr_puts(12, 8, "no network card", theme.dim);
            scr_printf(2, 10, theme.accent, "Uptime");
            scr_printf(12, 10, theme.normal, "%u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
            scr_printf(2, 12, theme.accent, "System");
            scr_printf(12, 12, theme.normal, "%s%s", os_name(cur_os), adv_mode ? "  (advanced mode)" : "");
            scr_printf(2, 14, theme.prompt, "%-6s %-24s %-10s %s", "PID", "Name", "State", "Memory");
            scr_printf(2, 15, theme.normal, "%-6d %-24s %-10s %u KB", 0, "axis-kernel", "running", 512u);
            scr_printf(2, 16, theme.normal, "%-6d %-24s %-10s %u KB", 1, "axsh", "waiting", 16u);
            scr_printf(2, 17, theme.normal, "%-6d %-24s %-10s %u KB", 2, cur_os == OS_WINDOWS ? "taskmgr" : "top", "running", used / 1024);
        }
        int k = kbd_trygetkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        cpu_hlt();
    }
    return 0;
}
