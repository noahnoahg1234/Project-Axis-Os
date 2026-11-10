/*
 * AxisOS Desktop - a retro, text-mode desktop with mouse support,
 * in the style of the DOS Shell / Windows 3.1. Shello opens it.
 *
 * Click an icon (or use the arrow keys and Enter) to launch an app. A
 * Start menu, a clock and the current system are shown on the bars.
 */
#include "axis.h"

int browser_run(const char *url);
int store_run(void);
int settings_run(void);
int explorer_run(const char *p);
int editor_run(const char *p);
int shello_run(void);
int ai_run(void);
int archiver_run(void);
int app_calculator(void);
int app_minesweeper(void);
int app_paint(void);
int app_snake(void);
int app_clock(void);
int app_monitor(void);
static int act_terminal(void) { return -999; } /* sentinel: leave to shell */
static int act_netconf(void);

typedef struct {
    const char *label;
    const char *glyph;   /* 3 chars */
    int (*run)(void);
    uint8_t color;
} icon_t;

static int run_browser(void) { return browser_run(NULL); }
static int run_explorer(void) { return explorer_run(NULL); }
static int run_editor(void) { return editor_run(NULL); }

static const icon_t icons[] = {
    {"Files", "[=]", run_explorer, C_YELLOW},
    {"Browser", "(o)", run_browser, C_LCYAN},
    {"App Store", "[S]", store_run, C_LGREEN},
    {"Editor", "[/]", run_editor, C_WHITE},
    {"AI Helper", "<*>", ai_run, C_LMAGENTA},
    {"Archiver", "[#]", archiver_run, C_BROWN},
    {"Calculator", "[+]", app_calculator, C_LGRAY},
    {"Paint", "<#>", app_paint, C_LRED},
    {"Minesweeper", "[*]", app_minesweeper, C_LRED},
    {"Snake", "~@~", app_snake, C_LGREEN},
    {"Clock", "(:)", app_clock, C_LCYAN},
    {"Monitor", "[#]", app_monitor, C_LGREEN},
    {"Network", "<~>", act_netconf, C_LBLUE},
    {"Settings", "[o]", settings_run, C_WHITE},
    {"Shell", ">_ ", act_terminal, C_LGREEN},
};
#define NICONS ((int)(sizeof icons / sizeof *icons))

#define COLS 5
#define IW 14
#define IH 4
#define OX 2
#define OY 3

static int icon_x(int i) { return OX + (i % COLS) * IW; }
static int icon_y(int i) { return OY + (i / COLS) * IH; }

static void draw_icon(int i, bool sel) {
    int x = icon_x(i), y = icon_y(i);
    uint8_t bg = sel ? C_LGRAY : C_CYAN;
    uint8_t fg = sel ? C_BLACK : C_WHITE;
    scr_fill(x, y, IW - 1, 3, ' ', ATTR(fg, bg));
    scr_puts(x + (IW - 1 - 3) / 2, y, icons[i].glyph, ATTR(icons[i].color == C_CYAN ? C_WHITE : icons[i].color, bg));
    int ll = (int)strlen(icons[i].label);
    scr_putsn(x + (IW - 1 - ll) / 2, y + 1, icons[i].label, ll, ATTR(fg, bg));
}

static bool start_open;
static const char *start_items[] = {"Files...", "App Store...", "AI Helper...", "Settings...", "Shello shell...", "Switch system >", "About AxisOS", "Leave desktop"};
#define NSTART 8

static void draw_desktop(int sel) {
    scr_fill(0, 0, SCREEN_W, SCREEN_H, (char)0xB0, ATTR(C_BLUE, C_BLACK));
    /* top bar */
    scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    scr_puts(1, 0, " Start ", start_open ? ATTR(C_WHITE, C_BLACK) : ATTR(C_BLACK, C_LGRAY));
    scr_puts(10, 0, "AxisOS Desktop", ATTR(C_BLACK, C_LGRAY));
    char rt[40];
    rtc_time_t t;
    rtc_local(&t);
    snprintf(rt, sizeof rt, "%s  %02d:%02d", os_name(cur_os), t.hour, t.min);
    scr_puts(SCREEN_W - (int)strlen(rt) - 1, 0, rt, ATTR(C_BLACK, C_LGRAY));
    /* icons */
    scr_puts(OX, 2, "Programs", ATTR(C_WHITE, C_BLACK));
    for (int i = 0; i < NICONS; i++) draw_icon(i, i == sel);
    /* bottom hint bar */
    scr_fill(0, 24, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    scr_puts(1, 24, mouse_present() ? "Click an icon, or use arrows + Enter. Start menu top-left. Esc or right-click: menu."
                                    : "Arrows move, Enter opens, S: Start menu, Esc: leave.",
             ATTR(C_BLACK, C_LGRAY));
    if (start_open) {
        int w = 22, x = 0, y = 1;
        scr_box(x, y, w, NSTART + 2, ATTR(C_WHITE, C_BLACK), "Start");
        for (int i = 0; i < NSTART; i++) scr_puts(x + 2, y + 1 + i, start_items[i], ATTR(C_WHITE, C_BLACK));
    }
}

static int start_menu_sel = 0;

static int icon_at(int cx, int cy) {
    for (int i = 0; i < NICONS; i++) {
        int x = icon_x(i), y = icon_y(i);
        if (cx >= x && cx < x + IW - 1 && cy >= y && cy < y + 3) return i;
    }
    return -1;
}

static void launch(int i, bool *leave) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    int (*fn)(void) = icons[i].run;
    if (fn == act_terminal) {
        *leave = true;
        return;
    }
    scr_save(saved);
    con_show_cursor(false);
    fn();
    con_show_cursor(false);
    scr_restore(saved);
}

static void about(void) {
    tui_message("About AxisOS", "AxisOS " AXIS_VERSION " - one system, three personalities. Desktop, browser, App Store and more.");
}

int desktop_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    int sel = 0, pmx = -1, pmy = -1;
    uint8_t pbtn = 0;
    bool leave = false;
    uint32_t last_clock = 0;
    start_open = false;
    while (!leave) {
        draw_desktop(sel);
        /* draw mouse pointer */
        if (mouse_present()) {
            int msx, msy;
            mouse_pos(&msx, &msy);
            scr_put(msx, msy, (char)0xDB, ATTR(C_WHITE, C_BLACK));
        }
        /* input loop: redraw on mouse move or every second for the clock */
        for (;;) {
            int k = kbd_trygetkey();
            if (k >= 0) {
                start_open = false;
                if (k == KEY_ESC || k == 'q' || k == 'Q') {
                    if (start_open) start_open = false;
                    else {
                        leave = true;
                    }
                    break;
                } else if (k == 's' || k == 'S') {
                    start_open = true;
                    start_menu_sel = 0;
                    break;
                } else if (k == KEY_LEFT) {
                    if (sel > 0) sel--;
                    break;
                } else if (k == KEY_RIGHT) {
                    if (sel < NICONS - 1) sel++;
                    break;
                } else if (k == KEY_UP) {
                    if (sel >= COLS) sel -= COLS;
                    break;
                } else if (k == KEY_DOWN) {
                    if (sel + COLS < NICONS) sel += COLS;
                    break;
                } else if (k == '\n') {
                    launch(sel, &leave);
                    break;
                }
            }
            if (mouse_present() && mouse_poll_moved()) break;
            uint8_t b = mouse_present() ? mouse_buttons() : 0;
            if (b != pbtn) {
                int cx, cy;
                mouse_pos(&cx, &cy);
                if ((b & 1) && !(pbtn & 1)) { /* left click down */
                    if (cy == 0 && cx < 8) {
                        start_open = !start_open;
                        pbtn = b;
                        break;
                    }
                    if (start_open) {
                        if (cx >= 1 && cx < 23 && cy >= 2 && cy < 2 + NSTART) {
                            int it = cy - 2;
                            start_open = false;
                            pbtn = b;
                            /* handle start item */
                            if (it == 0) { launch(0, &leave); }
                            else if (it == 1) { launch(2, &leave); }
                            else if (it == 2) { launch(4, &leave); }
                            else if (it == 3) { settings_run(); con_show_cursor(false); }
                            else if (it == 4) { shello_run(); con_show_cursor(false); return 0; }
                            else if (it == 5) { cur_os = (cur_os + 1) % OS_COUNT; theme_apply(); }
                            else if (it == 6) about();
                            else if (it == 7) leave = true;
                            break;
                        }
                        start_open = false;
                        pbtn = b;
                        break;
                    }
                    int ic = icon_at(cx, cy);
                    if (ic >= 0) {
                        sel = ic;
                        pbtn = b;
                        launch(ic, &leave);
                        break;
                    }
                }
                if ((b & 2) && !(pbtn & 2)) { /* right click: toggle start */
                    start_open = !start_open;
                    pbtn = b;
                    break;
                }
                pbtn = b;
                break;
            }
            if (ticks - last_clock > TICK_HZ) {
                last_clock = ticks;
                break;
            }
            cpu_hlt();
        }
        (void)pmx;
        (void)pmy;
    }
    con_show_cursor(true);
    scr_restore(saved);
    return 0;
}

/* ---- network settings (also used by the 'netconf' command) ------------ */
extern bool net_up(void);
static int act_netconf(void) {
    extern int netconf_run(void);
    return netconf_run();
}
