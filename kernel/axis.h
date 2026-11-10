/*
 * AxisOS - shared kernel header
 */
#ifndef AXIS_H
#define AXIS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define AXIS_VERSION "1.0.0"

typedef int bool;
#define true 1
#define false 0

/* ------------------------------------------------------------ port I/O -- */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt"); }
static inline void irq_on(void) { __asm__ volatile("sti"); }
static inline void irq_off(void) { __asm__ volatile("cli"); }

/* ------------------------------------------------------------- lib.c ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
void strlcpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strcasestr(const char *h, const char *n);
char *strstr(const char *h, const char *n);
void *memchr(const void *s, int c, size_t n);
char *kstrdup(const char *s);
int tolower(int c);
int toupper(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isxdigit(int c);
void str_lower(char *s);
char *strtok_simple(char *s);
long strtol(const char *s, const char **end, int base);
int atoi(const char *s);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
bool wildcard_match(const char *pat, const char *s); /* case-insensitive * and ? */
void format_size(uint32_t bytes, char *out, size_t n);

/* ------------------------------------------------------------- mem.c ---- */
void heap_init(uint32_t start, uint32_t end);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void kfree(void *p);
void *krealloc(void *p, size_t n);
void heap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks);

/* --------------------------------------------------------- console.c ---- */
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
    C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE
};
#define ATTR(fg, bg) ((uint8_t)(((bg) << 4) | (fg)))
#define SCREEN_W 80
#define SCREEN_H 25

void con_init(void);
void con_clear(void);
void con_putc(char c);
void con_write(const char *s);
void con_writen(const char *s, size_t n);
int kprintf(const char *fmt, ...);
void con_set_attr(uint8_t a);
uint8_t con_get_attr(void);
void con_set_cursor(int x, int y);
void con_get_cursor(int *x, int *y);
void con_update_hw_cursor(void);
void con_show_cursor(bool on);
uint32_t con_scroll_count(void);
void con_capture_begin(void);
char *con_capture_end(size_t *len); /* returns kmalloc'd buffer */
bool con_capturing(void);
void con_serial_mirror(bool on);
/* low level screen access used by full-screen apps */
void scr_put(int x, int y, char c, uint8_t attr);
void scr_puts(int x, int y, const char *s, uint8_t attr);
void scr_putsn(int x, int y, const char *s, int maxw, uint8_t attr);
void scr_fill(int x, int y, int w, int h, char c, uint8_t attr);
void scr_box(int x, int y, int w, int h, uint8_t attr, const char *title);
void scr_save(uint16_t *buf);
void scr_restore(const uint16_t *buf);
int scr_printf(int x, int y, uint8_t attr, const char *fmt, ...);

/* theme (colours) */
typedef struct {
    const char *name;
    uint8_t normal, prompt, path, accent, error, dim, bar, select;
} theme_t;
extern theme_t theme;
void theme_apply(void);
extern const char *theme_names[];
extern const int theme_count;

/* ---------------------------------------------------------- serial.c ---- */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
int serial_getc(void); /* -1 if none */
bool serial_present(void);

/* ------------------------------------------------------------- idt.c ---- */
typedef struct {
    uint32_t ds, edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err;
    uint32_t eip, cs, eflags;
} regs_t;
void idt_init(void);
void irq_set_handler(int irq, void (*h)(regs_t *));
extern volatile uint32_t ticks; /* 100 Hz */
#define TICK_HZ 100
void sleep_ms(uint32_t ms);

/* ------------------------------------------------------------ kbd.c ----- */
enum {
    KEY_ESC = 27, KEY_ENTER = '\n', KEY_BACKSPACE = '\b', KEY_TAB = '\t',
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12
};
#define KEY_CTRL(c) ((c) & 0x1F)
void kbd_init(void);
void mouse_init(void);
bool mouse_present(void);
void mouse_pos(int *x, int *y);
uint8_t mouse_buttons(void);
bool mouse_poll_moved(void);
void mouse_set(int x, int y);
int desktop_run(void);
int kbd_getkey(void);  /* blocking */
int kbd_trygetkey(void); /* -1 if none */
bool kbd_ctrl_c_pending(void);

/* ------------------------------------------------------------- ata.c ---- */
bool ata_init(void);
bool ata_present(void);
bool ata_read(uint32_t lba, uint32_t count, void *buf);
bool ata_write(uint32_t lba, uint32_t count, const void *buf);
const char *ata_model(void);
uint32_t ata_sectors(void);

/* ------------------------------------------------------------- rtc.c ---- */
typedef struct { int year, month, day, hour, min, sec, wday; } rtc_time_t;
void rtc_read(rtc_time_t *t);
void rtc_local(rtc_time_t *t); /* with configured timezone */
uint32_t rtc_unix(const rtc_time_t *t);
void rtc_from_unix(uint32_t ts, rtc_time_t *t);
const char *rtc_wday_name(int w);
const char *rtc_month_name(int m);

/* --------------------------------------------------------- config.c ----- */
#define CONFIG_LBA 17
#define FS_LBA 2048
#define FS_SECTORS 512
enum { OS_LINUX = 0, OS_WINDOWS = 1, OS_FREEBSD = 2, OS_DOS = 3, OS_UNIX = 4, OS_COUNT = 5 };
enum { AC_OFF = 0, AC_SUGGEST = 1, AC_AUTO = 2 };
typedef struct __attribute__((packed)) {
    char magic[4];      /* "AXCF" */
    uint8_t version;    /* 1 */
    uint8_t default_os; /* boot menu default */
    uint8_t timeout;    /* boot menu seconds, 0 = wait */
    uint8_t theme;
    uint8_t autocorrect;
    uint8_t advanced;   /* advanced mode on boot */
    uint8_t autosave;
    uint8_t show_hidden;
    uint8_t clock24;
    int8_t tz_hours;
    uint8_t confirm;    /* ask before destructive operations */
    uint8_t reserved;
    char hostname[32];
    char username[32];
} config_t;
extern config_t cfg;
void config_load(void);
bool config_save(void);
void config_defaults(void);

typedef struct {
    uint8_t os;
    uint8_t advanced;
    uint8_t drive;
    uint32_t mem_kb; /* above 1MB */
} bootinfo_t;
extern bootinfo_t boot;

/* --------------------------------------------------------- system.c ----- */
extern int cur_os;
extern bool adv_mode;
const char *os_name(int os);
bool os_win_style(void);   /* Windows or DOS: C:\ and backslashes, dir/type/del */
bool os_dos(void);         /* MS-DOS personality */
bool os_unix_style(void);  /* Linux or FreeBSD */
const char *os_short(int os);
void sys_reboot(void);
void sys_poweroff(void);
void klog(const char *fmt, ...);
const char *klog_buffer(void);
void cpu_vendor(char *out13);
void cpu_brand(char *out49);
uint32_t mem_total_kb(void);

/* ------------------------------------------------------------- fs.c ----- */
#define FS_MAX_NODES 512
#define FS_NAME_MAX 40
enum { FT_FREE = 0, FT_DIR = 1, FT_FILE = 2 };
#define FA_HIDDEN 1
#define FA_READONLY 2
#define FA_SYSTEM 4
typedef struct {
    uint8_t type;
    uint8_t attr;
    int16_t parent;
    char name[FS_NAME_MAX];
    uint8_t *data;
    uint32_t size, cap;
    uint32_t mtime;
} fs_node_t;
enum { E_OK = 0, E_NOENT = -2, E_EXIST = -17, E_NOTDIR = -20, E_ISDIR = -21,
       E_INVAL = -22, E_NOSPC = -28, E_NOTEMPTY = -39, E_ROFS = -30, E_PERM = -1, E_IO = -5 };
extern fs_node_t fs_nodes[FS_MAX_NODES];
extern int fs_cwd;
extern bool fs_dirty;
#define FS_ROOT 0
void fs_init(void);
void fs_format(void);
bool fs_save(void);
bool fs_load(void);
fs_node_t *fs_get(int id);
int fs_lookup(int dir, const char *name);
int fs_resolve(const char *path, int cwd);
int fs_resolve_fuzzy(const char *path, int cwd, char *corrected, size_t n);
int fs_resolve_parent(const char *path, int cwd, char *leaf);
int fs_create(int parent, const char *name, int type);
int fs_mkfile(const char *path, int cwd); /* create or open existing */
int fs_write(int id, const void *data, uint32_t len);
int fs_append(int id, const void *data, uint32_t len);
int fs_delete(int id, bool recursive);
int fs_copy(int src, int dstdir, const char *name);
int fs_move(int id, int newparent, const char *newname);
void fs_path(int id, char *buf, size_t n);  /* in the current OS style */
void fs_path_unix(int id, char *buf, size_t n);
int fs_children(int dir, int *out, int max, bool hidden); /* sorted */
bool fs_is_ancestor(int anc, int node);
int fs_home(void);
void fs_stats(int *nodes, uint32_t *bytes);
const char *fs_strerror(int e);
bool storage_ready(void);
bool fs_valid_name(const char *n);

/* ------------------------------------------------------- autocorrect.c -- */
int edit_distance(const char *a, const char *b);
int ac_threshold(const char *word);

/* ----------------------------------------------------------- shell.c ---- */
#define MAX_ARGS 32
typedef int (*cmd_fn)(int argc, char **argv);
typedef struct {
    const char *lin, *win, *bsd; /* native name in each system ("" = none) */
    const char *aliases;         /* extra names, '|' separated */
    cmd_fn fn;
    const char *usage;
    const char *desc;
    uint8_t flags;
} command_t;
#define CMD_ADVANCED 1 /* only available in advanced mode */
#define CMD_HIDDEN 2   /* not listed by help */
extern const command_t commands[];
extern int command_count;
const command_t *cmd_find(const char *name);
bool cmd_name_matches(const command_t *c, const char *name);
void shell_main(void);
int shell_exec_line(const char *line);
void shell_prompt_string(char *buf, size_t n);
extern const char *shell_stdin; /* piped input, or NULL */
extern size_t shell_stdin_len;
const char *env_get(const char *name);
void env_set(const char *name, const char *value);
void env_unset(const char *name);
int env_list(char names[][32], char values[][128], int max);
void shell_print_error(int code, const char *what);
void shell_expand(const char *in, char *out, size_t n);
bool calc_eval(const char *expr, long *out);
bool shell_confirm(const char *question);
int shell_history_count(void);
const char *shell_history_get(int i);
int alias_set(const char *name, const char *value);
void alias_list(void);
int line_edit(char *buf, size_t n, bool history, bool complete);

/* ------------------------------------------------------------- net.c ---- */
typedef struct {
    int status;
    char *body; /* kmalloc'd, NUL terminated */
    uint32_t len;
    char location[512];
    char ctype[64];
} http_resp_t;
enum { HTTP_ENONET = -1, HTTP_EURL = -2, HTTP_EDNS = -3, HTTP_ECONN = -4, HTTP_EPROTO = -5, HTTP_EABORT = -6 };
extern uint8_t net_mac[6];
extern uint32_t net_ip, net_mask, net_gw, net_dns;
bool net_init(void);
bool net_renew(void);
bool net_is_dhcp(void);
void net_set_dhcp(void);
void net_set_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
int netconf_run(void);
int ai_run(void);
int archiver_run(void);
int axar_cmd(int argc, char **argv);
int desktop_run(void);
bool net_present(void);
bool net_up(void);
bool net_online(void);
bool net_link_up(void);
const char *net_nic_name(void);
void net_stats(uint32_t *rx, uint32_t *tx);
void net_poll(void);
int net_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ms);
bool net_resolve(const char *host, uint32_t *ip);
void ip_to_str(uint32_t ip, char *out16);
bool str_to_ip(const char *s, uint32_t *ip);
bool url_split(const char *url, char *host, size_t hn, uint16_t *port, char *path, size_t pn);
int http_get(const char *url, http_resp_t *r, void (*progress)(uint32_t got, uint32_t total));
const char *http_strerror(int e);

/* --------------------------------------------------------- apps --------- */
int browser_run(const char *url);
int store_run(void);
int shello_run(void);
int script_run(const char *path, int argc, char **argv);
bool app_try_run(int argc, char **argv); /* installed app by name */
int apps_list(void);
/* compat.c - run Linux/FreeBSD ELF programs */
int compat_run(const char *path, int argc, char **argv);
bool compat_is_program(const char *path);
extern bool compat_high_power;
extern int compat_report_os;
int explorer_run(const char *start);
int editor_run(const char *path);
int settings_run(void);
void pager_show(const char *text, size_t len);
int tui_input(int x, int y, int w, char *buf, size_t n, uint8_t attr);
int tui_menu(const char *title, const char **items, int count, int sel);
bool tui_confirm(const char *title, const char *msg);
void tui_message(const char *title, const char *msg);
int tui_prompt(const char *title, const char *label, char *buf, size_t n);
void shell_banner(void);

#endif
