/*
 * AxisRAR - the AxisOS archiver. Packs files and folders into one .axar
 * archive and extracts them again, with a simple run-length compression.
 *
 * Command line:  axar a archive.axar file... | x archive.axar [dest] | l archive.axar
 * Also opens a small interactive screen when run with no arguments.
 *
 * Archive format (all little-endian):
 *   "AXAR" ver=1 count
 *   per entry: type(1) pathlen(2) path origsize(4) packsize(4) packed-bytes
 * Compression: RLE - 0x00 marks a run (0x00, byte, count 1..255);
 *   a literal 0x00 is stored as 0x00,0x00,0x01.
 */
#include "axis.h"

#define AXAR_MAGIC "AXAR"

static uint32_t rle_pack(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t outcap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n;) {
        uint8_t b = in[i];
        uint32_t run = 1;
        while (i + run < n && in[i + run] == b && run < 255) run++;
        if (run >= 4 || b == 0) {
            if (o + 3 > outcap) return 0xFFFFFFFF;
            out[o++] = 0;
            out[o++] = b;
            out[o++] = (uint8_t)run;
            i += run;
        } else {
            if (o + 1 > outcap) return 0xFFFFFFFF;
            out[o++] = b;
            i++;
        }
    }
    return o;
}

static uint32_t rle_unpack(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t outcap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n;) {
        if (in[i] == 0) {
            if (i + 3 > n) break;
            uint8_t b = in[i + 1], run = in[i + 2];
            if (o + run > outcap) return 0xFFFFFFFF;
            for (int r = 0; r < run; r++) out[o++] = b;
            i += 3;
        } else {
            if (o + 1 > outcap) return 0xFFFFFFFF;
            out[o++] = in[i++];
        }
    }
    return o;
}

/* ---- buffer that grows ------------------------------------------------- */
typedef struct { uint8_t *d; uint32_t len, cap; } buf_t;
static bool buf_put(buf_t *b, const void *p, uint32_t n) {
    if (b->len + n > b->cap) {
        uint32_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n) nc *= 2;
        uint8_t *nd = krealloc(b->d, nc);
        if (!nd) return false;
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    return true;
}
static bool buf_u32(buf_t *b, uint32_t v) { return buf_put(b, &v, 4); }
static bool buf_u16(buf_t *b, uint16_t v) { return buf_put(b, &v, 2); }

/* ---- create ------------------------------------------------------------ */
static int add_node(buf_t *b, int id, const char *relpath, uint32_t *count) {
    fs_node_t *n = fs_get(id);
    if (!n) return E_NOENT;
    char path[256];
    if (n->type == FT_DIR) {
        uint8_t t = FT_DIR;
        buf_put(b, &t, 1);
        buf_u16(b, (uint16_t)strlen(relpath));
        buf_put(b, relpath, strlen(relpath));
        (*count)++;
        int kids[FS_MAX_NODES];
        int k = fs_children(id, kids, FS_MAX_NODES, true);
        for (int i = 0; i < k; i++) {
            snprintf(path, sizeof path, "%s/%s", relpath, fs_nodes[kids[i]].name);
            int r = add_node(b, kids[i], path, count);
            if (r < 0) return r;
        }
        return E_OK;
    }
    uint8_t *packed = kmalloc(n->size + 16);
    if (!packed) return E_NOSPC;
    uint32_t psize = rle_pack(n->data ? n->data : (uint8_t *)"", n->size, packed, n->size + 16);
    bool store = psize == 0xFFFFFFFF || psize >= n->size;
    buf_put(b, store ? (const void *)"\x02" : (const void *)"\x03", 1); /* 2=stored,3=packed */
    buf_u16(b, (uint16_t)strlen(relpath));
    buf_put(b, relpath, strlen(relpath));
    buf_u32(b, n->size);
    if (store) {
        buf_u32(b, n->size);
        buf_put(b, n->data ? n->data : (uint8_t *)"", n->size);
    } else {
        buf_u32(b, psize);
        buf_put(b, packed, psize);
    }
    kfree(packed);
    (*count)++;
    return E_OK;
}

static int axar_create(const char *archive, char **files, int nfiles) {
    buf_t b = {0};
    buf_put(&b, AXAR_MAGIC, 4);
    uint8_t ver = 1;
    buf_put(&b, &ver, 1);
    uint32_t count_pos = b.len;
    buf_u32(&b, 0);
    uint32_t count = 0;
    for (int i = 0; i < nfiles; i++) {
        int id = fs_resolve(files[i], fs_cwd);
        if (id < 0) {
            shell_print_error(E_NOENT, files[i]);
            kfree(b.d);
            return 1;
        }
        int r = add_node(&b, id, fs_nodes[id].name, &count);
        if (r < 0) {
            shell_print_error(r, files[i]);
            kfree(b.d);
            return 1;
        }
    }
    memcpy(b.d + count_pos, &count, 4);
    int id = fs_mkfile(archive, fs_cwd);
    int w = id < 0 ? id : fs_write(id, b.d, b.len);
    uint32_t total = b.len;
    kfree(b.d);
    if (w < 0) {
        shell_print_error(w, archive);
        return 1;
    }
    if (cfg.autosave) fs_save();
    kprintf("Created %s: %u item%s, %u bytes.\n", archive, count, count == 1 ? "" : "s", total);
    return 0;
}

/* ---- read -------------------------------------------------------------- */
typedef struct {
    const uint8_t *p, *end;
    uint32_t count;
} rd_t;

static bool axar_open(const char *archive, rd_t *rd, const uint8_t **base) {
    int id = fs_resolve(archive, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE) {
        shell_print_error(id < 0 ? id : E_ISDIR, archive);
        return false;
    }
    const uint8_t *d = fs_nodes[id].data;
    if (!d || fs_nodes[id].size < 9 || memcmp(d, AXAR_MAGIC, 4) || d[4] != 1) {
        kprintf("%s: not an AxisRAR (.axar) archive.\n", archive);
        return false;
    }
    *base = d;
    rd->p = d + 5;
    memcpy(&rd->count, rd->p, 4);
    rd->p += 4;
    rd->end = d + fs_nodes[id].size;
    return true;
}

/* iterate; fills type/path/orig/pack and advances; returns false at end */
static bool axar_next(rd_t *rd, uint8_t *type, char *path, size_t pn, uint32_t *orig, const uint8_t **data, uint32_t *packsz) {
    if (rd->p >= rd->end) return false;
    *type = *rd->p++;
    uint16_t pl;
    memcpy(&pl, rd->p, 2);
    rd->p += 2;
    if (pl >= pn) pl = pn - 1;
    memcpy(path, rd->p, pl);
    path[pl] = 0;
    rd->p += pl;
    if (*type == FT_DIR) {
        *orig = 0;
        *packsz = 0;
        *data = NULL;
        return true;
    }
    memcpy(orig, rd->p, 4);
    rd->p += 4;
    memcpy(packsz, rd->p, 4);
    rd->p += 4;
    *data = rd->p;
    rd->p += *packsz;
    return true;
}

static int axar_list(const char *archive) {
    rd_t rd;
    const uint8_t *base;
    if (!axar_open(archive, &rd, &base)) return 1;
    kprintf("Archive: %s\n\n  %-40s %10s %10s\n", archive, "Name", "Size", "Packed");
    uint8_t type;
    char path[256];
    uint32_t orig, packsz, total = 0, tpack = 0;
    const uint8_t *data;
    int n = 0;
    while (axar_next(&rd, &type, path, sizeof path, &orig, &data, &packsz)) {
        if (type == FT_DIR) kprintf("  %-40s %10s\n", path, "<DIR>");
        else {
            kprintf("  %-40s %10u %10u\n", path, orig, packsz);
            total += orig;
            tpack += packsz;
        }
        n++;
    }
    kprintf("\n  %d item%s, %u bytes -> %u packed", n, n == 1 ? "" : "s", total, tpack);
    if (total) kprintf(" (%u%%)", tpack * 100 / total);
    kprintf("\n");
    return 0;
}

static int axar_extract(const char *archive, const char *dest) {
    rd_t rd;
    const uint8_t *base;
    if (!axar_open(archive, &rd, &base)) return 1;
    int destdir = fs_cwd;
    if (dest) {
        destdir = fs_resolve(dest, fs_cwd);
        if (destdir < 0) {
            char leaf[FS_NAME_MAX];
            int par = fs_resolve_parent(dest, fs_cwd, leaf);
            if (par >= 0) destdir = fs_create(par, leaf, FT_DIR);
        }
        if (destdir < 0 || fs_nodes[destdir].type != FT_DIR) {
            shell_print_error(E_NOTDIR, dest);
            return 1;
        }
    }
    uint8_t type;
    char path[256];
    uint32_t orig, packsz;
    const uint8_t *data;
    int n = 0, errs = 0;
    while (axar_next(&rd, &type, path, sizeof path, &orig, &data, &packsz)) {
        /* make parent folders */
        char full[300];
        snprintf(full, sizeof full, "%s/%s", "", path);
        /* create directories along the path under destdir */
        char acc[300] = "";
        int cur = destdir;
        const char *seg = path;
        char comp[FS_NAME_MAX];
        while (*seg) {
            const char *slash = strchr(seg, '/');
            int l = slash ? (int)(slash - seg) : (int)strlen(seg);
            if (l <= 0 || l >= FS_NAME_MAX) break;
            memcpy(comp, seg, l);
            comp[l] = 0;
            bool last = !slash;
            if (last && type != FT_DIR) {
                int f = fs_lookup(cur, comp);
                if (f < 0) f = fs_create(cur, comp, FT_FILE);
                if (f < 0) {
                    errs++;
                    break;
                }
                uint8_t *out = kmalloc(orig + 1);
                if (!out) {
                    errs++;
                    break;
                }
                uint32_t got = (type == 3) ? rle_unpack(data, packsz, out, orig) : (memcpy(out, data, packsz), packsz);
                fs_write(f, out, got == 0xFFFFFFFF ? 0 : got);
                kfree(out);
                n++;
            } else {
                int dderr = fs_lookup(cur, comp);
                if (dderr < 0) dderr = fs_create(cur, comp, FT_DIR);
                if (dderr < 0) {
                    errs++;
                    break;
                }
                cur = dderr;
                if (last) n++;
            }
            seg = slash ? slash + 1 : seg + l;
        }
        (void)full;
        (void)acc;
    }
    if (cfg.autosave) fs_save();
    kprintf("Extracted %d item%s%s.\n", n, n == 1 ? "" : "s", errs ? " (some errors)" : "");
    return errs ? 1 : 0;
}

/* command entry: axar a|x|l ... */
int axar_cmd(int argc, char **argv) {
    if (argc < 3) {
        kprintf("AxisRAR - the AxisOS archiver\n");
        kprintf("  %s a <archive.axar> <file|folder>...   create an archive\n", argv[0]);
        kprintf("  %s x <archive.axar> [folder]           extract\n", argv[0]);
        kprintf("  %s l <archive.axar>                    list contents\n", argv[0]);
        return argc < 2 ? 1 : 0;
    }
    const char *op = argv[1];
    if (!strcasecmp(op, "a") || !strcasecmp(op, "add") || !strcasecmp(op, "c")) return axar_create(argv[2], argv + 3, argc - 3);
    if (!strcasecmp(op, "x") || !strcasecmp(op, "e") || !strcasecmp(op, "extract")) return axar_extract(argv[2], argc > 3 ? argv[3] : NULL);
    if (!strcasecmp(op, "l") || !strcasecmp(op, "t") || !strcasecmp(op, "list")) return axar_list(argv[2]);
    kprintf("%s: unknown operation '%s' (use a, x or l)\n", argv[0], op);
    return 1;
}

/* interactive screen (desktop icon / 'archiver' with no args) */
int archiver_run(void) {
    con_set_attr(theme.normal);
    con_clear();
    uint8_t s = con_get_attr();
    con_set_attr(theme.accent);
    kprintf("  AxisRAR - the AxisOS Archiver\n");
    con_set_attr(theme.dim);
    kprintf("  Pack files into one .axar archive, or extract one.\n\n");
    con_set_attr(s);
    kprintf("  1) Create an archive\n  2) Extract an archive\n  3) List an archive\n  4) Quit\n\n  Choose: ");
    int k = kbd_getkey();
    kprintf("%c\n\n", (k >= 32 && k < 127) ? k : ' ');
    char a[128] = "", b[200] = "";
    if (k == '1') {
        con_write("  Archive name (e.g. backup.axar): ");
        if (line_edit(a, sizeof a, false, false) <= 0) return 0;
        con_write("  Files/folders to add (space separated): ");
        if (line_edit(b, sizeof b, false, false) <= 0) return 0;
        char *argv[18];
        int n = 0;
        argv[n++] = (char *)"axar";
        argv[n++] = (char *)"a";
        argv[n++] = a;
        for (char *t = strtok_simple(b); t && n < 18; t = strtok_simple(NULL)) argv[n++] = t;
        axar_cmd(n, argv);
    } else if (k == '2') {
        con_write("  Archive to extract: ");
        if (line_edit(a, sizeof a, false, false) <= 0) return 0;
        con_write("  Destination folder (blank = here): ");
        line_edit(b, sizeof b, false, false);
        char *argv[4] = {(char *)"axar", (char *)"x", a, b[0] ? b : NULL};
        axar_cmd(b[0] ? 4 : 3, argv);
    } else if (k == '3') {
        con_write("  Archive to list: ");
        if (line_edit(a, sizeof a, false, false) <= 0) return 0;
        char *argv[3] = {(char *)"axar", (char *)"l", a};
        axar_cmd(3, argv);
    } else
        return 0;
    con_set_attr(theme.dim);
    kprintf("\n  Press any key...");
    con_set_attr(s);
    kbd_getkey();
    return 0;
}
