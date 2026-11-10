/*
 * Axis Assistant - a small offline help assistant built into AxisOS.
 *
 * It is not a large language model and needs no internet or API key. It
 * matches your question against what it knows about AxisOS - commands, apps,
 * the three systems, the browser, the store, networking - and answers, and
 * it can run commands for you when you ask ("open the browser", "list files").
 */
#include "axis.h"

typedef struct {
    const char *keywords; /* '|' separated; any match counts */
    const char *answer;
} kb_t;

static const kb_t kb[] = {
    {"hello|hi|hey|greet", "Hello! I'm the Axis Assistant. Ask me how to do things in AxisOS - like 'how do I copy a file', 'what is advanced mode', or 'open the browser'. Type 'bye' to leave."},
    {"who are you|what are you|your name", "I'm the Axis Assistant - a small built-in helper. I run entirely offline inside AxisOS, so I'm not a big AI from the internet, but I know AxisOS well."},
    {"copy|duplicate", "To copy a file use cp (Linux), copy (Windows) or cp (FreeBSD):  cp notes.txt backup.txt . Copy a folder with -r:  cp -r Documents Backup . In the file explorer, press C then V."},
    {"move|rename", "Move with mv (or 'move' in Windows), rename with mv or 'ren'.  mv old.txt new.txt  moves/renames. In the explorer press R to rename, X then V to move."},
    {"delete|remove|erase", "Delete a file with rm (or 'del' in Windows):  rm file.txt . Delete a folder and its contents with rm -r folder . In the explorer, select and press Delete."},
    {"list|show files|directory|dir|ls", "List files with ls (Linux/FreeBSD) or dir (Windows). Add -l for details, -a to show hidden files. Or open the Files app (explorer)."},
    {"folder|directory|mkdir|make dir", "Make a folder with mkdir name (or 'md' in Windows). Enter it with cd name , go up with cd .. , go home with cd ~ ."},
    {"edit|editor|write|text", "Edit text with the editor: nano file.txt (also 'edit' or 'ee'). Ctrl+S saves, Ctrl+X exits. You can also open files from the file explorer."},
    {"browser|web|internet|website|surf", "Open the web browser with 'browser' (also msedge or lynx). Press S to search the web, G to type an address. It shows pages as text. Secure https sites open through FrogFind."},
    {"store|install|app|download app|package", "Open the App Store with 'store' (also winget, apt or pkg). Pick an app and press Enter to install, then type its name to run it. Press A to add an online app source."},
    {"advanced|expert|power user|root|admin|sudo", "Advanced mode unlocks expert commands (peek, poke, sector, cpuid, heap...) and turns off autocorrect and confirmations. Turn it on with 'advanced on' and off with 'exit'. The prompt changes to #."},
    {"high power|highpower|run fast|more memory|performance", "High Power mode gives a running program a much larger memory arena so heavier programs fit. Turn it on with 'highpower on'. It can't make the CPU itself faster - programs already use the whole processor."},
    {"windows|linux|freebsd|switch system|personality|os ", "AxisOS has three personalities. Switch with: os linux , os windows , os freebsd (or wsl, cmd, bsd). Each has its own commands, prompt and look, but every command works in all three."},
    {"run program|run app|exe|linux program|compat|elf", "AxisOS can run small 32-bit Linux and FreeBSD command-line programs. Download one (wget http://...), then type ./program . Build them with gcc -pie . Windows .exe files are not supported."},
    {"network|ip|wifi|ethernet|dhcp|static ip", "See your network with ifconfig (or ipconfig). Set it up in Network settings ('netconf' or the Network icon): choose automatic (DHCP) or a static IP. Test with ping, download with wget."},
    {"ping|test connection", "Check a connection with: ping 10.0.2.2 (or any address). Look up a name with nslookup example.com ."},
    {"archive|zip|rar|compress|winrar|extract|unzip", "Use the Archiver (winrar / axar). Create an archive:  axar a backup.axar file1 folder2 . Extract one:  axar x backup.axar . List it:  axar l backup.axar ."},
    {"desktop|gui|mouse|windows 3|icons", "Open the retro desktop with 'desktop' (or from Shello). Click an icon with the mouse, or use the arrow keys and Enter. The Start menu is top-left."},
    {"shello|retro shell", "Shello is the retro shell. Start it with 'shello'. It has short commands (LIST, GO, SHOW, NEW, TIME, BEEP, TINT) and a green phosphor look. Type BYE to leave. 'shello desktop' opens the desktop."},
    {"settings|configure|theme|preferences|color", "Open Settings with 'settings' (or control / bsdconfig). You can change the theme, default system, autocorrect, timezone, your name and more. Network has its own page (netconf)."},
    {"save|persist|lose files|reboot", "Files and settings are saved to the AxisOS disk automatically, so they survive a reboot. Force a save with 'sync'. This needs the disk image (it works in QEMU/VirtualBox)."},
    {"pipe|redirect|output to file|combine", "Pipe output with | and save it with > . Example:  ls | grep txt > list.txt . Chain commands with ; && || . Works in all three systems."},
    {"autocorrect|typo|did you mean|spelling", "AxisOS fixes typos in commands and paths automatically (sl becomes ls). Change this in Settings: Off, Suggest, or Automatic. It's off in advanced mode."},
    {"help|commands|what can|list commands", "Type 'help' for the full command list (Linux, Windows and FreeBSD names side by side), or 'help <command>' for one command. Try the apps: explorer, browser, store, settings, desktop."},
    {"game|play|fun|snake|mines", "AxisOS has games in the App Store: Snake, Minesweeper, Guess the Number, and more. Install them from 'store', then type their name to play."},
    {"thank|thanks|cool|awesome|great", "You're welcome! Happy exploring. Type 'bye' when you're done."},
};
#define NKB ((int)(sizeof kb / sizeof *kb))

static bool kw_match(const char *keywords, const char *q) {
    char kwbuf[160];
    strlcpy(kwbuf, keywords, sizeof kwbuf);
    char *save = kwbuf;
    for (;;) {
        char *bar = strchr(save, '|');
        if (bar) *bar = 0;
        if (*save && strcasestr(q, save)) return true;
        if (!bar) break;
        save = bar + 1;
    }
    return false;
}

/* try to carry out an action the user asked for; return true if handled */
static bool try_action(const char *q) {
    struct { const char *kw; const char *cmd; const char *say; } acts[] = {
        {"open the browser|launch browser|start browser|go on the web|open browser", "browser", "Opening the browser..."},
        {"open the store|app store|open store", "store", "Opening the App Store..."},
        {"open settings|open the settings", "settings", "Opening Settings..."},
        {"open files|file explorer|open explorer|open the files", "explorer", "Opening the file explorer..."},
        {"open the desktop|show desktop|launch desktop", "desktop", "Opening the desktop..."},
        {"list files|show my files|what files|list my files", "ls", NULL},
        {"network settings|open network|configure network", "netconf", "Opening network settings..."},
        {"what time|the time|current time", "date", NULL},
        {"system info|about my system|system information", "neofetch", NULL},
        {"who am i", "whoami", NULL},
    };
    for (unsigned i = 0; i < sizeof acts / sizeof *acts; i++)
        if (kw_match(acts[i].kw, q)) {
            if (acts[i].say) {
                uint8_t s = con_get_attr();
                con_set_attr(theme.accent);
                kprintf("  %s\n", acts[i].say);
                con_set_attr(s);
            }
            shell_exec_line(acts[i].cmd);
            return true;
        }
    return false;
}

static void answer(const char *q) {
    if (try_action(q)) return;
    int best = -1, score = 0;
    for (int i = 0; i < NKB; i++) {
        if (!kw_match(kb[i].keywords, q)) continue;
        /* prefer the entry with the most keyword hits */
        int sc = 0;
        char kwbuf[160];
        strlcpy(kwbuf, kb[i].keywords, sizeof kwbuf);
        char *save = kwbuf, *bar;
        do {
            bar = strchr(save, '|');
            if (bar) *bar = 0;
            if (*save && strcasestr(q, save)) sc++;
            save = bar ? bar + 1 : save;
        } while (bar);
        if (sc > score) score = sc, best = i;
    }
    uint8_t s = con_get_attr();
    con_set_attr(theme.prompt);
    con_write("  ");
    con_set_attr(theme.normal);
    if (best >= 0) {
        /* word-wrap the answer at ~74 cols */
        const char *a = kb[best].answer;
        int col = 2;
        while (*a) {
            const char *sp = a;
            while (*sp && *sp != ' ') sp++;
            int wl = (int)(sp - a);
            if (col + wl > 76) {
                kprintf("\n  ");
                col = 2;
            }
            con_writen(a, wl);
            con_putc(' ');
            col += wl + 1;
            a = *sp ? sp + 1 : sp;
        }
        kprintf("\n");
    } else {
        kprintf("I'm not sure about that. I can help with files, apps, the browser, the App\n");
        kprintf("  Store, networking, the three systems, advanced mode and more. Try asking\n");
        kprintf("  'what can you do' or 'how do I install an app'. Or say 'open the browser'.\n");
    }
    con_set_attr(s);
}

int ai_run(void) {
    con_set_attr(theme.normal);
    con_clear();
    uint8_t s = con_get_attr();
    con_set_attr(theme.accent);
    kprintf("  +------------------------------------------------------+\n");
    kprintf("  |              A X I S   A S S I S T A N T             |\n");
    kprintf("  +------------------------------------------------------+\n");
    con_set_attr(theme.dim);
    kprintf("  Offline help built into AxisOS. Ask a question in plain words.\n");
    kprintf("  I can also do things: try \"open the browser\" or \"list files\".\n");
    kprintf("  Type 'bye' or press Esc to leave.\n\n");
    con_set_attr(s);
    char line[200];
    for (;;) {
        con_set_attr(theme.prompt);
        con_write("you> ");
        con_set_attr(theme.normal);
        int r = line_edit(line, sizeof line, true, false);
        if (r < 0) break;
        /* trim */
        char *q = line;
        while (*q == ' ') q++;
        if (!*q) continue;
        if (!strcasecmp(q, "bye") || !strcasecmp(q, "exit") || !strcasecmp(q, "quit")) break;
        if (!strcasecmp(q, "clear") || !strcasecmp(q, "cls")) {
            con_clear();
            continue;
        }
        answer(q);
        kprintf("\n");
    }
    con_set_attr(theme.accent);
    kprintf("Axis Assistant: goodbye!\n");
    con_set_attr(s);
    return 0;
}
