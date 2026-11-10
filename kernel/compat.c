/*
 * AxisOS compatibility layer.
 *
 * Loads small 32-bit ELF programs built for Linux or FreeBSD and runs them
 * in the kernel's address space (ring 0, no paging - the simple way),
 * emulating a useful subset of each system's `int 0x80` system calls.
 *
 * A program's own system (from its ELF header) decides which syscall table
 * and which OS name it sees - that is the "name converter". High Power mode
 * just gives a program a much larger memory arena so heavier programs fit.
 *
 * Only position-independent executables (gcc -pie) are supported, so the
 * loader can place them anywhere in RAM. Windows .exe files are not handled.
 */
#include "axis.h"

/* from entry.asm */
extern int compat_enter(void *entry, void *stacktop);
extern void compat_exit(int code) __attribute__((noreturn));
extern uint32_t isr128;

/* ---- ELF32 ------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_hdr_t;
typedef struct __attribute__((packed)) {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} elf_ph_t;
typedef struct __attribute__((packed)) {
    int32_t tag;
    uint32_t val;
} elf_dyn_t;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define ET_DYN 3
#define EM_386 3
#define DT_REL 17
#define DT_RELSZ 18
#define DT_RELENT 19
#define R_386_RELATIVE 8

enum { ABI_LINUX, ABI_FREEBSD };

/* ---- running program state -------------------------------------------- */
typedef struct {
    int abi;
    uint8_t *arena;
    uint32_t arena_size;
    uint32_t brk_start, brk_cur, brk_max;
    bool running;
    int exit_code;
    char name[32];
} app_t;

static app_t *cur;

int compat_report_os = -1; /* -1 = use the app's own system; else forced */

/* ---- open files ------------------------------------------------------- */
#define MAXFD 16
typedef struct {
    bool used;
    int node;      /* AxisFS node, or -1 for the console streams */
    uint32_t off;
    bool writable, append;
} fdesc_t;
static fdesc_t fds[MAXFD];

static int fd_alloc(void) {
    for (int i = 3; i < MAXFD; i++)
        if (!fds[i].used) return i;
    return -1;
}

/* ---- syscall helpers -------------------------------------------------- */
#define EBADF -9
#define ENOENT -2
#define EINVAL -22
#define ENOSYS -38
#define EFAULT -14
#define ENOMEM -12

static bool valid_ptr(const void *p) {
    uint32_t a = (uint32_t)p;
    return a >= 0x1000 && a < mem_total_kb() * 1024;
}

static int sys_write(int fd, const char *buf, uint32_t n) {
    if (!valid_ptr(buf)) return EFAULT;
    if (fd == 1 || fd == 2) {
        con_writen(buf, n);
        return (int)n;
    }
    if (fd < 0 || fd >= MAXFD || !fds[fd].used) return EBADF;
    if (!fds[fd].writable) return EBADF;
    fdesc_t *f = &fds[fd];
    if (f->append) f->off = fs_nodes[f->node].size;
    int r = fs_append(f->node, buf, n); /* grows file; simple model */
    if (f->off != fs_nodes[f->node].size - n) {
        /* overwrite case: rewrite whole buffer region */
    }
    f->off += n;
    return r < 0 ? EINVAL : (int)n;
}

static int sys_read(int fd, char *buf, uint32_t n) {
    if (!valid_ptr(buf)) return EFAULT;
    if (fd == 0) {
        irq_on(); /* ensure the keyboard/timer can wake us */
        /* line-buffered keyboard input */
        uint32_t i = 0;
        while (i < n) {
            int k = kbd_getkey();
            if (k == KEY_CTRL('d')) break;
            if (k == KEY_CTRL('c')) {
                compat_exit(130);
            }
            if (k == '\b') {
                if (i) {
                    i--;
                    con_write("\b \b");
                }
                continue;
            }
            if (k > 255) continue;
            con_putc((char)k);
            buf[i++] = (char)k;
            if (k == '\n') break;
        }
        return (int)i;
    }
    if (fd < 0 || fd >= MAXFD || !fds[fd].used) return EBADF;
    fdesc_t *f = &fds[fd];
    fs_node_t *node = fs_get(f->node);
    if (!node) return EBADF;
    if (f->off >= node->size) return 0;
    uint32_t avail = node->size - f->off;
    if (n > avail) n = avail;
    memcpy(buf, node->data + f->off, n);
    f->off += n;
    return (int)n;
}

static int sys_open(const char *path, int flags) {
    if (!valid_ptr(path)) return EFAULT;
    int fd = fd_alloc();
    if (fd < 0) return -24; /* EMFILE */
    bool wr = (flags & 3) != 0;     /* O_WRONLY|O_RDWR */
    bool creat = (flags & 0100);    /* O_CREAT */
    bool trunc = (flags & 01000);   /* O_TRUNC */
    bool append = (flags & 02000);  /* O_APPEND */
    int id = fs_resolve(path, fs_cwd);
    if (id < 0) {
        if (!creat) return ENOENT;
        id = fs_mkfile(path, fs_cwd);
        if (id < 0) return EINVAL;
    }
    if (fs_nodes[id].type != FT_FILE) return EINVAL;
    if (trunc && wr) fs_write(id, "", 0);
    fds[fd] = (fdesc_t){true, id, append ? fs_nodes[id].size : 0, wr, append};
    return fd;
}

/* minimal uname: the name converter in action */
struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
static const char *app_os_name(void) {
    if (compat_report_os == OS_LINUX) return "Linux";
    if (compat_report_os == OS_WINDOWS) return "Windows_NT";
    if (compat_report_os == OS_FREEBSD) return "FreeBSD";
    return cur->abi == ABI_FREEBSD ? "FreeBSD" : "Linux";
}
static int sys_uname(struct utsname *u) {
    if (!valid_ptr(u)) return EFAULT;
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, app_os_name());
    strcpy(u->nodename, cfg.hostname);
    snprintf(u->release, 65, "%s-axis", AXIS_VERSION);
    snprintf(u->version, 65, "AxisOS compat #1");
    strcpy(u->machine, cur->abi == ABI_FREEBSD ? "i386" : "i686");
    return 0;
}

static uint32_t sys_brk(uint32_t addr) {
    if (addr == 0) return cur->brk_cur;
    if (addr < cur->brk_start || addr > cur->brk_max) return cur->brk_cur;
    cur->brk_cur = addr;
    return cur->brk_cur;
}

/* ---- the big dispatcher ----------------------------------------------- */
static int32_t linux_syscall(uint32_t nr, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    switch (nr) {
    case 1: /* exit */
    case 252: /* exit_group */
        compat_exit((int)a);
    case 3: return sys_read((int)a, (char *)b, c);
    case 4: return sys_write((int)a, (const char *)b, c);
    case 5: return sys_open((const char *)a, (int)b);
    case 6: /* close */
        if (a >= 3 && a < MAXFD) fds[a].used = false;
        return 0;
    case 13: { /* time */
        rtc_time_t t;
        rtc_read(&t);
        uint32_t s = rtc_unix(&t);
        if (a && valid_ptr((void *)a)) *(uint32_t *)a = s;
        return (int32_t)s;
    }
    case 19: /* lseek */
        if (a < MAXFD && fds[a].used) {
            if (c == 0) fds[a].off = b;
            else if (c == 1) fds[a].off += b;
            else fds[a].off = fs_nodes[fds[a].node].size + b;
            return (int32_t)fds[a].off;
        }
        return EBADF;
    case 20: return 1;    /* getpid */
    case 24: return 0;    /* getuid */
    case 47: return 0;    /* getgid */
    case 45: return (int32_t)sys_brk(a); /* brk */
    case 54: return 0;    /* ioctl - pretend success (isatty etc.) */
    case 33: /* access */
        return fs_resolve((const char *)a, fs_cwd) >= 0 ? 0 : ENOENT;
    case 122: return sys_uname((struct utsname *)a); /* uname */
    case 78: { /* gettimeofday */
        if (a && valid_ptr((void *)a)) {
            rtc_time_t t;
            rtc_read(&t);
            ((uint32_t *)a)[0] = rtc_unix(&t);
            ((uint32_t *)a)[1] = (ticks % TICK_HZ) * (1000000 / TICK_HZ);
        }
        return 0;
    }
    case 146: { /* writev */
        const uint32_t *iov = (const uint32_t *)b;
        if (!valid_ptr(iov)) return EFAULT;
        int total = 0;
        for (uint32_t i = 0; i < c; i++) {
            int r = sys_write((int)a, (const char *)iov[i * 2], iov[i * 2 + 1]);
            if (r < 0) return r;
            total += r;
        }
        return total;
    }
    case 162: { /* nanosleep */
        const uint32_t *ts = (const uint32_t *)a;
        if (valid_ptr(ts)) sleep_ms(ts[0] * 1000 + ts[1] / 1000000);
        return 0;
    }
    case 192: /* mmap2 - satisfy small anonymous maps from brk */
        if (b && b < cur->brk_max - cur->brk_cur) {
            uint32_t p = (cur->brk_cur + 0xFFF) & ~0xFFFu;
            cur->brk_cur = p + b;
            memset((void *)p, 0, b);
            return (int32_t)p;
        }
        return ENOMEM;
    case 91: return 0;    /* munmap */
    case 243: return 0;   /* set_thread_area */
    case 174: case 175: case 173: return 0; /* rt_sig* */
    case 55: return 0;    /* fcntl */
    case 197: case 108: { /* fstat64/fstat - just say it's a char device */
        if (valid_ptr((void *)b)) memset((void *)b, 0, 96);
        return 0;
    }
    case 265: { /* clock_gettime */
        if (valid_ptr((void *)b)) {
            rtc_time_t t;
            rtc_read(&t);
            ((uint32_t *)b)[0] = rtc_unix(&t);
            ((uint32_t *)b)[1] = (ticks % TICK_HZ) * (1000000000 / TICK_HZ);
        }
        return 0;
    }
    case 355: { /* getrandom */
        uint8_t *buf = (uint8_t *)a;
        if (!valid_ptr(buf)) return EFAULT;
        static uint32_t s = 0x12345;
        for (uint32_t i = 0; i < b; i++) {
            s = s * 1103515245 + 12345 + ticks;
            buf[i] = (uint8_t)(s >> 16);
        }
        return (int32_t)b;
    }
    case 41: return (int32_t)a; /* dup - share fd number (good enough) */
    default:
        return ENOSYS;
    }
    (void)d;
    (void)e;
}

/* FreeBSD i386: syscall number in eax, args on the user stack */
static int32_t freebsd_syscall(uint32_t nr, const uint32_t *args) {
    switch (nr) {
    case 1: compat_exit((int)args[0]);           /* exit */
    case 3: return sys_read((int)args[0], (char *)args[1], args[2]);
    case 4: return sys_write((int)args[0], (const char *)args[1], args[2]);
    case 5: return sys_open((const char *)args[0], (int)args[1]);
    case 6:
        if (args[0] >= 3 && args[0] < MAXFD) fds[args[0]].used = false;
        return 0;
    case 17: return (int32_t)sys_brk(args[0]);   /* obreak */
    case 20: return 1;                            /* getpid */
    case 24: return 0;                            /* getuid */
    case 13: return 0;                            /* sigprocmask-ish noop */
    case 202: { /* __sysctl: answer kern.ostype / kern.osrelease */
        const int *name = (const int *)args[0];
        uint32_t namelen = args[1];
        char *oldp = (char *)args[2];
        uint32_t *oldlenp = (uint32_t *)args[3];
        if (namelen >= 2 && name[0] == 1 /*CTL_KERN*/ && oldp && oldlenp) {
            const char *v = NULL;
            if (name[1] == 1) v = app_os_name();           /* KERN_OSTYPE */
            else if (name[1] == 2) v = AXIS_VERSION;        /* KERN_OSRELEASE */
            else if (name[1] == 10) v = cfg.hostname;       /* KERN_HOSTNAME */
            if (v) {
                uint32_t l = strlen(v) + 1;
                if (*oldlenp >= l) memcpy(oldp, v, l);
                *oldlenp = l;
                return 0;
            }
        }
        return EINVAL;
    }
    case 116: { /* gettimeofday */
        uint32_t *tv = (uint32_t *)args[0];
        if (valid_ptr(tv)) {
            rtc_time_t t;
            rtc_read(&t);
            tv[0] = rtc_unix(&t);
            tv[1] = 0;
        }
        return 0;
    }
    default:
        return ENOSYS;
    }
}

/* called from isr128 with the saved register frame */
void syscall_dispatch(regs_t *r) {
    if (!cur || !cur->running) {
        r->eax = (uint32_t)ENOSYS;
        return;
    }
    int32_t ret;
    if (cur->abi == ABI_FREEBSD) {
        /* args sit just above the int-0x80 return frame on the app stack */
        const uint32_t *stk = (const uint32_t *)((uint8_t *)&r->eip + 16);
        ret = freebsd_syscall(r->eax, stk);
        /* FreeBSD reports errors via carry flag; we just return -errno in eax,
           which its libc checks as (unsigned) >= -4095. Good enough. */
    } else {
        ret = linux_syscall(r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi);
    }
    r->eax = (uint32_t)ret;
}

/* ---- loader ----------------------------------------------------------- */
static bool apply_relocs(uint8_t *base, const elf_ph_t *dyn_ph) {
    const elf_dyn_t *d = (const elf_dyn_t *)(base + dyn_ph->vaddr);
    uint32_t rel = 0, relsz = 0, relent = 8;
    for (; d->tag != 0; d++) {
        if (d->tag == DT_REL) rel = d->val;
        else if (d->tag == DT_RELSZ) relsz = d->val;
        else if (d->tag == DT_RELENT) relent = d->val;
    }
    for (uint32_t off = 0; off < relsz; off += relent) {
        const uint32_t *r = (const uint32_t *)(base + rel + off);
        uint32_t where = r[0];
        uint8_t type = r[1] & 0xFF;
        if (type == R_386_RELATIVE) *(uint32_t *)(base + where) += (uint32_t)base;
    }
    return true;
}

/* returns 0 on success, negative on error (message in *err) */
int compat_run(const char *path, int argc, char **argv) {
    int id = fs_resolve(path, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE) {
        shell_print_error(id < 0 ? id : E_ISDIR, path);
        return 1;
    }
    fs_node_t *node = &fs_nodes[id];
    if (node->size < sizeof(elf_hdr_t)) {
        kprintf("%s: not an executable program\n", path);
        return 1;
    }
    const elf_hdr_t *eh = (const elf_hdr_t *)node->data;
    if (memcmp(eh->ident, "\x7f""ELF", 4) || eh->ident[4] != 1 /*32-bit*/) {
        if (eh->ident[0] == 'M' && eh->ident[1] == 'Z')
            kprintf("%s: this is a Windows program; AxisOS cannot run Windows .exe files.\n", path);
        else
            kprintf("%s: not a 32-bit ELF program.\n", path);
        return 1;
    }
    if (eh->machine != EM_386) {
        kprintf("%s: not built for 32-bit x86 (i386).\n", path);
        return 1;
    }
    if (eh->type != ET_DYN) {
        kprintf("%s: must be a position-independent executable (build with gcc -pie).\n", path);
        return 1;
    }
    int abi = (eh->ident[7] == 9) ? ABI_FREEBSD : ABI_LINUX; /* EI_OSABI 9 = FreeBSD */

    /* measure the image */
    const elf_ph_t *ph = (const elf_ph_t *)((const uint8_t *)node->data + eh->phoff);
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    const elf_ph_t *dyn = NULL;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) dyn = &ph[i];
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo) {
        kprintf("%s: no loadable code.\n", path);
        return 1;
    }
    uint32_t image = (hi - lo + 0xFFF) & ~0xFFFu;
    bool hp = adv_mode || compat_high_power;
    uint32_t heap_room = hp ? 16u * 1024 * 1024 : 1024 * 1024;
    uint32_t stack_room = hp ? 1024 * 1024 : 256 * 1024;
    uint32_t total = image + heap_room + stack_room + 0x4000;

    uint8_t *raw = kmalloc(total + 0x1000);
    if (!raw) {
        kprintf("%s: not enough memory to run this program.\n", path);
        return 1;
    }
    uint8_t *base = (uint8_t *)(((uint32_t)raw + 0xFFF) & ~0xFFFu);
    memset(base, 0, total);

    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        memcpy(base + ph[i].vaddr - lo, (const uint8_t *)node->data + ph[i].offset, ph[i].filesz);
    }
    /* base is where vaddr 0 lands; since lo may be >0, adjust so base+vaddr works */
    uint8_t *load = base - lo;
    if (dyn) apply_relocs(load, dyn);

    app_t app;
    memset(&app, 0, sizeof app);
    app.abi = abi;
    app.arena = raw;
    app.arena_size = total;
    app.brk_start = app.brk_cur = ((uint32_t)base + image + 0xFFF) & ~0xFFFu;
    app.brk_max = app.brk_start + heap_room;
    app.running = true;
    strlcpy(app.name, argv ? argv[0] : path, sizeof app.name);

    /* build the initial stack: argc, argv[], NULL, envp NULL, auxv */
    uint32_t sp_top = (uint32_t)base + total;
    sp_top &= ~0xFu;
    uint8_t *sp = (uint8_t *)sp_top;
    /* argv strings */
    int n = argc > 0 ? argc : 1;
    char *defname = (char *)path;
    char *av[8];
    for (int i = 0; i < n && i < 8; i++) av[i] = (i < argc && argv) ? argv[i] : defname;
    uint32_t argp[8];
    for (int i = n - 1; i >= 0; i--) {
        uint32_t l = strlen(av[i]) + 1;
        sp -= l;
        memcpy(sp, av[i], l);
        argp[i] = (uint32_t)sp;
    }
    uint8_t *rnd = (sp -= 16);
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(ticks + i * 7);
    sp = (uint8_t *)((uint32_t)sp & ~0xFu);
    uint32_t *st = (uint32_t *)sp;
    /* push from the top down; leave room then fill */
    uint32_t items = 1 + n + 1 + 1 + 6 + 1; /* argc, argv, 0, envp 0, 3 aux pairs, pad */
    st -= items;
    st = (uint32_t *)((uint32_t)st & ~0xFu);
    uint32_t *w = st;
    *w++ = (uint32_t)n;                 /* argc */
    for (int i = 0; i < n; i++) *w++ = argp[i];
    *w++ = 0;                           /* argv NULL */
    *w++ = 0;                           /* envp NULL */
    *w++ = 6; *w++ = 4096;              /* AT_PAGESZ */
    *w++ = 25; *w++ = (uint32_t)rnd;    /* AT_RANDOM */
    *w++ = 0; *w++ = 0;                 /* AT_NULL */

    app_t *prev = cur;
    cur = &app;
    for (int i = 3; i < MAXFD; i++) fds[i].used = false;

    void *entry = load + eh->entry;
    con_serial_mirror(true);
    int code = compat_enter(entry, st);
    app.running = false;
    cur = prev;
    kfree(raw);
    return code;
}

bool compat_is_program(const char *path) {
    int id = fs_resolve(path, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE || fs_nodes[id].size < 4) return false;
    const uint8_t *d = fs_nodes[id].data;
    return d && !memcmp(d, "\x7f""ELF", 4);
}
