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

/* ============================================================ Tic-Tac-Toe */
static int ttt_winner(const char *b) {
    static const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for (int i = 0; i < 8; i++)
        if (b[L[i][0]] != ' ' && b[L[i][0]] == b[L[i][1]] && b[L[i][1]] == b[L[i][2]]) return b[L[i][0]];
    for (int i = 0; i < 9; i++) if (b[i] == ' ') return 0;
    return 'D'; /* draw */
}
static int ttt_find(char *b, char me, char opp, bool win) {
    static const int L[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    char look = win ? me : opp;
    for (int i = 0; i < 8; i++) {
        int c = 0, empty = -1;
        for (int j = 0; j < 3; j++) {
            if (b[L[i][j]] == look) c++;
            else if (b[L[i][j]] == ' ') empty = L[i][j];
        }
        if (c == 2 && empty >= 0) return empty;
    }
    return -1;
}
static void ttt_ai(char *b) {
    int m = ttt_find(b, 'O', 'X', true);        /* win */
    if (m < 0) m = ttt_find(b, 'O', 'X', false); /* block */
    if (m < 0 && b[4] == ' ') m = 4;             /* center */
    if (m < 0) { int cn[4] = {0,2,6,8}; for (int i=0;i<4;i++) if (b[cn[i]]==' ') { m=cn[i]; break; } }
    if (m < 0) for (int i = 0; i < 9; i++) if (b[i]==' ') { m=i; break; }
    if (m >= 0) b[m] = 'O';
}
int app_tictactoe(void) {
    char b[9]; int cur = 4, wins = 0, losses = 0;
new:;
    for (int i = 0; i < 9; i++) b[i] = ' ';
    cur = 4; int st = 0;
    for (;;) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', theme.normal);
        title_bar("Tic-Tac-Toe  (you are X)", "Arrows move  Space/Enter place  N new  Q quit");
        int ox = 33, oy = 7;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                int i = r*3+c, x = ox + c*6, y = oy + r*3;
                uint8_t a = (i == cur && !st) ? theme.select : theme.normal;
                scr_fill(x, y, 5, 2, ' ', a);
                char ch = b[i];
                scr_put(x+2, y, ch, b[i]=='X'?ATTR(C_LCYAN,a>>4):b[i]=='O'?ATTR(C_LRED,a>>4):a);
                if (c < 2) { scr_put(x+5, y, (char)0xB3, theme.dim); scr_put(x+5, y+1, (char)0xB3, theme.dim); }
            }
        for (int c = 0; c < 17; c++) { scr_put(ox+c, oy+2, (char)0xC4, theme.dim); scr_put(ox+c, oy+5, (char)0xC4, theme.dim); }
        scr_printf(30, oy+10, theme.accent, "Wins %d   Losses %d", wins, losses);
        if (st) {
            const char *m = st=='X'?"You win!":st=='O'?"Computer wins.":"It's a draw.";
            scr_puts(34, oy+12, m, theme.prompt);
            scr_puts(28, oy+13, "Press N for a new game.", theme.dim);
        }
        int k = kbd_getkey();
        if (k=='q'||k=='Q'||k==KEY_ESC) break;
        if (k=='n'||k=='N') goto new;
        if (st) continue;
        if (k==KEY_LEFT && cur%3) cur--;
        else if (k==KEY_RIGHT && cur%3!=2) cur++;
        else if (k==KEY_UP && cur>=3) cur-=3;
        else if (k==KEY_DOWN && cur<6) cur+=3;
        else if ((k==' '||k=='\n') && b[cur]==' ') {
            b[cur]='X';
            st = ttt_winner(b);
            if (!st) { ttt_ai(b); st = ttt_winner(b); }
            if (st=='X') wins++; else if (st=='O') losses++;
        } else if (k>='1'&&k<='9' && b[k-'1']==' ') {
            b[k-'1']='X'; st=ttt_winner(b);
            if (!st){ ttt_ai(b); st=ttt_winner(b);} if(st=='X')wins++;else if(st=='O')losses++;
        }
    }
    return 0;
}

/* ================================================================== 2048 == */
int app_2048(void) {
    int g[4][4], score = 0, best = 0;
    #define SPAWN() do { int e=0,px[16],py[16]; for(int y=0;y<4;y++)for(int x=0;x<4;x++) if(!g[y][x]){px[e]=x;py[e]=y;e++;} if(e){int s=rnd()%e; g[py[s]][px[s]]=(rnd()%10)?2:4;} } while(0)
newg:;
    memset(g,0,sizeof g); score=0; SPAWN(); SPAWN();
    for (;;) {
        scr_fill(0,0,SCREEN_W,SCREEN_H,' ',theme.normal);
        title_bar("2048  -  join the tiles!", "Arrows move   N new   Q quit");
        int ox=28, oy=4;
        for (int y=0;y<4;y++) for (int x=0;x<4;x++) {
            int v=g[y][x];
            static const uint8_t col[12]={C_DGRAY,C_LGRAY,C_WHITE,C_LCYAN,C_LGREEN,C_YELLOW,C_BROWN,C_LRED,C_LMAGENTA,C_LBLUE,C_GREEN,C_CYAN};
            int idx=0,t=v; while(t>1){t>>=1;idx++;} if(idx>11)idx=11;
            uint8_t a=ATTR(C_BLACK, v?col[idx]:C_DGRAY);
            scr_fill(ox+x*6, oy+y*3, 5, 2, ' ', a);
            if (v){ char s[8]; snprintf(s,sizeof s,"%d",v); scr_puts(ox+x*6+(5-(int)strlen(s))/2, oy+y*3, s, a);}        }
        scr_printf(ox, oy+13, theme.accent, "Score %d    Best %d", score, best);
        int moved=0, won=0;
        int k=kbd_getkey();
        if (k=='q'||k=='Q'||k==KEY_ESC) break;
        if (k=='n'||k=='N') goto newg;
        int dx=0,dy=0;
        if (k==KEY_LEFT)dx=-1; else if(k==KEY_RIGHT)dx=1; else if(k==KEY_UP)dy=-1; else if(k==KEY_DOWN)dy=1; else continue;
        /* process moves */
        for (int iter=0;iter<4;iter++) {
            for (int y=0;y<4;y++) for (int x=0;x<4;x++) {
                int cx = dx>0?3-x:x, cy = dy>0?3-y:y;
                int nx=cx+dx, ny=cy+dy;
                if (nx<0||ny<0||nx>3||ny>3) continue;
                if (g[cy][cx]==0) continue;
                if (g[ny][nx]==0){ g[ny][nx]=g[cy][cx]; g[cy][cx]=0; moved=1; }
                else if (g[ny][nx]==g[cy][cx]){ g[ny][nx]*=2; score+=g[ny][nx]; if(g[ny][nx]==2048)won=1; g[cy][cx]=0; moved=1; }
            }
        }
        if (score>best) best=score;
        if (moved) SPAWN();
        if (won) { tui_message("2048","You made 2048! You win! (Keep going or press N.)"); }
        /* game over check */
        int full=1; for(int y=0;y<4;y++)for(int x=0;x<4;x++) if(!g[y][x])full=0;
        if (full){ int mv=0; for(int y=0;y<4;y++)for(int x=0;x<4;x++){ if(x<3&&g[y][x]==g[y][x+1])mv=1; if(y<3&&g[y][x]==g[y+1][x])mv=1;} if(!mv){ tui_message("2048","No moves left - game over! Press N for a new game."); } }
    }
    return 0;
}

/* ================================================================ Tetris == */
int app_tetris(void) {
    enum { W=10, H=20 };
    static const int shapes[7][4] = {
        {0x0F00,0x2222,0x0F00,0x2222}, /* I */
        {0x0E20,0x44C0,0x8E00,0x6440}, /* J */
        {0x0E80,0xC440,0x2E00,0x4460}, /* L */
        {0x6600,0x6600,0x6600,0x6600}, /* O */
        {0x06C0,0x4620,0x06C0,0x4620}, /* S */
        {0x0E40,0x4640,0x4E00,0x4C40}, /* T */
        {0x0C60,0x2640,0x0C60,0x2640}, /* Z */
    };
    static const uint8_t col[7]={C_LCYAN,C_LBLUE,C_BROWN,C_YELLOW,C_LGREEN,C_LMAGENTA,C_LRED};
    static uint8_t well[H][W];
    int piece, rot, px, py, score=0, lines=0, level=1;
    #define CELL(sh,r,i) (((shapes[sh][(r)&3])>>(15-(i)))&1)
    #define FITS(sh,r,ox,oy) ({ int ok=1; for(int i=0;i<16;i++){ if(!CELL(sh,r,i))continue; int cx=(ox)+(i%4), cy=(oy)+(i/4); if(cx<0||cx>=W||cy>=H|| (cy>=0&&well[cy][cx])) ok=0; } ok; })
    memset(well,0,sizeof well);
    #define NEWPIECE() do { piece=rnd()%7; rot=0; px=3; py=0; } while(0)
    NEWPIECE();
    uint32_t drop = ticks;
    for (;;) {
        /* draw */
        scr_fill(0,0,SCREEN_W,SCREEN_H,' ',theme.normal);
        title_bar("Tetris", "Left/Right move  Up rotate  Down soft-drop  Space hard-drop  Q quit");
        int ox=30, oy=2;
        scr_box(ox-1, oy-1, W*2+2, H+2, theme.dim, NULL);
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
            uint8_t c = well[y][x];
            if (c){ scr_put(ox+x*2,oy+y,(char)0xDB,ATTR(col[c-1],theme.normal>>4)); scr_put(ox+x*2+1,oy+y,(char)0xDB,ATTR(col[c-1],theme.normal>>4)); }
        }
        for (int i=0;i<16;i++) if (CELL(piece,rot,i)) { int cx=px+(i%4),cy=py+(i/4); if(cy>=0){ scr_put(ox+cx*2,oy+cy,(char)0xDB,ATTR(col[piece],theme.normal>>4)); scr_put(ox+cx*2+1,oy+cy,(char)0xDB,ATTR(col[piece],theme.normal>>4)); } }
        scr_printf(ox+W*2+4, oy+2, theme.accent, "Score");
        scr_printf(ox+W*2+4, oy+3, theme.normal, "%d", score);
        scr_printf(ox+W*2+4, oy+5, theme.accent, "Lines");
        scr_printf(ox+W*2+4, oy+6, theme.normal, "%d", lines);
        scr_printf(ox+W*2+4, oy+8, theme.accent, "Level");
        scr_printf(ox+W*2+4, oy+9, theme.normal, "%d", level);
        /* timing */
        uint32_t speed = 30 - level*2; if ((int)speed<4) speed=4;
        int k=-1,kk; while ((int32_t)(drop+speed-ticks)>0){ if((kk=kbd_trygetkey())>=0){k=kk;break;} cpu_hlt(); }
        int drophard=0;
        if (k=='q'||k=='Q'||k==KEY_ESC) break;
        else if (k==KEY_LEFT){ if(FITS(piece,rot,px-1,py))px--; }
        else if (k==KEY_RIGHT){ if(FITS(piece,rot,px+1,py))px++; }
        else if (k==KEY_UP){ if(FITS(piece,(rot+1)&3,px,py))rot=(rot+1)&3; }
        else if (k==KEY_DOWN){ if(FITS(piece,rot,px,py+1))py++; }
        else if (k==' '){ while(FITS(piece,rot,px,py+1))py++; drophard=1; }
        if (k<0 || k==KEY_DOWN || drophard) {
            drop=ticks;
            if (FITS(piece,rot,px,py+1) && !drophard) py++;
            else {
                for (int i=0;i<16;i++) if (CELL(piece,rot,i)){ int cx=px+(i%4),cy=py+(i/4); if(cy>=0&&cy<H&&cx>=0&&cx<W) well[cy][cx]=piece+1; }
                /* clear lines */
                for (int y=H-1;y>=0;y--){ int full=1; for(int x=0;x<W;x++) if(!well[y][x])full=0; if(full){ lines++; score+=100*level; if(lines%10==0&&level<14)level++; for(int yy=y;yy>0;yy--) memcpy(well[yy],well[yy-1],W); memset(well[0],0,W); y++; } }
                NEWPIECE();
                if (!FITS(piece,rot,px,py)) { tui_message("Tetris","Game over! Final score below."); char m[40]; snprintf(m,sizeof m,"Score %d, %d lines.",score,lines); tui_message("Tetris",m); break; }
            }
        }
    }
    return 0;
}

/* ================================================================== Pong == */
int app_pong(void) {
    enum { FW=60, FH=18 };
    int ox=10, oy=3;
    int py=FH/2-2, ay=FH/2-2; /* paddles (len 4) */
    int bx=FW/2, by=FH/2, dx=1, dy=1, ps=0, as=0;
    for (;;) {
        scr_fill(0,0,SCREEN_W,SCREEN_H,' ',theme.normal);
        title_bar("Pong  (you are the left paddle)", "Up/Down move paddle   Q quit");
        scr_box(ox-1,oy-1,FW+2,FH+2,theme.dim,NULL);
        for (int i=0;i<4;i++){ scr_put(ox,oy+py+i,(char)0xDB,theme.accent); scr_put(ox+FW-1,oy+ay+i,(char)0xDB,theme.error); }
        scr_put(ox+bx,oy+by,'O',theme.prompt);
        scr_printf(ox+FW/2-6,oy+FH+2,theme.accent,"You %d    CPU %d",ps,as);
        int k=-1,kk; uint32_t w=ticks+3; while((int32_t)(w-ticks)>0){ if((kk=kbd_trygetkey())>=0){k=kk;} cpu_hlt(); }
        if (k=='q'||k=='Q'||k==KEY_ESC) break;
        if (k==KEY_UP && py>0) py--;
        else if (k==KEY_DOWN && py<FH-4) py++;
        /* AI follows ball */
        if (ay+2 < by && ay<FH-4) ay++; else if (ay+2 > by && ay>0) ay--;
        bx+=dx; by+=dy;
        if (by<=0||by>=FH-1) dy=-dy;
        if (bx<=1){ if (by>=py&&by<py+4){ dx=1; } else { as++; bx=FW/2; by=FH/2; dx=1; dy=(rnd()&1)?1:-1; } }
        if (bx>=FW-2){ if (by>=ay&&by<ay+4){ dx=-1; } else { ps++; bx=FW/2; by=FH/2; dx=-1; dy=(rnd()&1)?1:-1; } }
        if (ps>=7||as>=7){ tui_message("Pong", ps>as?"You win the match!":"The computer wins."); ps=as=0; }
    }
    return 0;
}

/* =============================================================== Hangman == */
int app_hangman(void) {
    static const char *words[] = {"computer","keyboard","network","program","penguin","desktop","browser",
        "command","compile","monitor","internet","password","terminal","software","hardware","variable",
        "function","operating","processor","megabyte"};
    int nw = sizeof words/sizeof *words;
newword:;
    const char *w = words[rnd()%nw];
    int len=(int)strlen(w); char guessed[27]={0}; int wrong=0, left=len;
    for (;;) {
        scr_fill(0,0,SCREEN_W,SCREEN_H,' ',theme.normal);
        title_bar("Hangman", "Type a letter to guess   N new word   Q quit");
        /* gallows */
        static const char *pics[7][5] = {
            {"  +---+","      |","      |","      |","======="},
            {"  +---+","  O   |","      |","      |","======="},
            {"  +---+","  O   |","  |   |","      |","======="},
            {"  +---+","  O   |"," /|   |","      |","======="},
            {"  +---+","  O   |"," /|\\  |","      |","======="},
            {"  +---+","  O   |"," /|\\  |"," /    |","======="},
            {"  +---+","  O   |"," /|\\  |"," / \\  |","======="},
        };
        for (int i=0;i<5;i++) scr_puts(8,5+i,pics[wrong][i],theme.normal);
        int x=30;
        for (int i=0;i<len;i++){ char c=w[i]; char sh=guessed[tolower(c)-'a']?c:'_'; scr_put(x+i*2,8,sh,theme.accent); }
        scr_puts(30,11,"Guessed:",theme.dim); int gx=39; for(int c=0;c<26;c++) if(guessed[c]) scr_put(gx++,11,'a'+c,theme.dim);
        scr_printf(30,13,theme.normal,"Wrong: %d of 6",wrong);
        if (wrong>=6){ char m[40]; snprintf(m,sizeof m,"The word was: %s",w); scr_puts(30,15,"You lost!",theme.error); scr_puts(30,16,m,theme.dim); scr_puts(30,17,"Press N for a new word.",theme.dim); }
        else if (left==0){ scr_puts(30,15,"You got it! Well done!",theme.prompt); scr_puts(30,16,"Press N for a new word.",theme.dim); }
        int k=kbd_getkey();
        if (k=='q'||k=='Q'||k==KEY_ESC) break;
        if (k=='n'||k=='N') goto newword;
        if (wrong>=6||left==0) continue;
        if (isalpha(k)) { int c=tolower(k)-'a'; if(guessed[c])continue; guessed[c]=1;
            int found=0; for(int i=0;i<len;i++) if(tolower(w[i])==('a'+c)){found++;} if(found)left-=found; else wrong++; }
    }
    return 0;
}
