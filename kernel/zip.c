/*
 * AxisZip - the AxisOS ZIP tool (7-Zip-style).
 *
 * Reads and writes real .zip files: lists them, extracts them (supporting
 * both STORED and DEFLATE compression via a built-in inflate decoder), and
 * creates them (STORED, i.e. uncompressed). Command: zip / unzip / 7z.
 *
 * Only the common cases are handled - no encryption, no ZIP64, no split
 * archives - which covers the great majority of everyday .zip files.
 */
#include "axis.h"

/* ============================ INFLATE (RFC 1951) ======================== */
typedef struct {
    const uint8_t *in;
    uint32_t inlen, inpos;
    uint32_t bitbuf, bitcnt;
    uint8_t *out;
    uint32_t outlen, outcap;
    bool error;
} inf_t;

static int inf_bit(inf_t *s) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) { s->error = true; return 0; }
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}
static uint32_t inf_bits(inf_t *s, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)inf_bit(s) << i;
    return v;
}
static void inf_out(inf_t *s, uint8_t b) {
    if (s->outlen >= s->outcap) {
        uint32_t nc = s->outcap ? s->outcap * 2 : 4096;
        uint8_t *n = krealloc(s->out, nc);
        if (!n) { s->error = true; return; }
        s->out = n;
        s->outcap = nc;
    }
    s->out[s->outlen++] = b;
}

/* canonical Huffman: build from code lengths */
typedef struct { uint16_t count[16]; uint16_t sym[288]; } huff_t;
static void huff_build(huff_t *h, const uint8_t *lens, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    uint16_t offs[16];
    offs[0] = offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
}
static int huff_decode(inf_t *s, huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= inf_bit(s);
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->error) return -1;
    }
    s->error = true;
    return -1;
}

static const uint16_t len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void inf_block(inf_t *s, huff_t *lh, huff_t *dh) {
    for (;;) {
        int sym = huff_decode(s, lh);
        if (s->error) return;
        if (sym < 256) inf_out(s, (uint8_t)sym);
        else if (sym == 256) return;
        else {
            sym -= 257;
            if (sym >= 29) { s->error = true; return; }
            int length = len_base[sym] + (int)inf_bits(s, len_extra[sym]);
            int dsym = huff_decode(s, dh);
            if (dsym < 0 || dsym >= 30) { s->error = true; return; }
            int dist = dist_base[dsym] + (int)inf_bits(s, dist_extra[dsym]);
            if ((uint32_t)dist > s->outlen) { s->error = true; return; }
            for (int i = 0; i < length; i++) { inf_out(s, s->out[s->outlen - dist]); if (s->error) return; }
        }
    }
}

/* returns malloc'd output (caller frees) or NULL; *outlen set */
static uint8_t *inflate(const uint8_t *in, uint32_t inlen, uint32_t expected, uint32_t *outlen) {
    inf_t s;
    memset(&s, 0, sizeof s);
    s.in = in; s.inlen = inlen;
    if (expected) { s.out = kmalloc(expected + 16); s.outcap = expected ? expected + 16 : 0; }
    int final;
    do {
        final = inf_bit(&s);
        int type = (int)inf_bits(&s, 2);
        if (type == 0) { /* stored */
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) { s.error = true; break; }
            uint32_t len = s.in[s.inpos] | (s.in[s.inpos + 1] << 8);
            s.inpos += 4;
            for (uint32_t i = 0; i < len; i++) { if (s.inpos >= s.inlen) { s.error = true; break; } inf_out(&s, s.in[s.inpos++]); }
        } else if (type == 1) { /* fixed Huffman (built fresh each block) */
            huff_t fl, fd;
            uint8_t l[288];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            huff_build(&fl, l, 288);
            uint8_t d[30]; for (int i = 0; i < 30; i++) d[i] = 5;
            huff_build(&fd, d, 30);
            inf_block(&s, &fl, &fd);
        } else if (type == 2) { /* dynamic Huffman */
            int hlit = (int)inf_bits(&s, 5) + 257;
            int hdist = (int)inf_bits(&s, 5) + 1;
            int hclen = (int)inf_bits(&s, 4) + 4;
            static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t cl[19]; memset(cl, 0, sizeof cl);
            for (int i = 0; i < hclen; i++) cl[ord[i]] = (uint8_t)inf_bits(&s, 3);
            huff_t clh; huff_build(&clh, cl, 19);
            uint8_t lens[288 + 32]; int n = 0;
            while (n < hlit + hdist) {
                int sym = huff_decode(&s, &clh);
                if (s.error) break;
                if (sym < 16) lens[n++] = (uint8_t)sym;
                else if (sym == 16) { int r = (int)inf_bits(&s, 2) + 3; uint8_t p = n ? lens[n-1] : 0; while (r-- && n < hlit+hdist) lens[n++] = p; }
                else if (sym == 17) { int r = (int)inf_bits(&s, 3) + 3; while (r-- && n < hlit+hdist) lens[n++] = 0; }
                else { int r = (int)inf_bits(&s, 7) + 11; while (r-- && n < hlit+hdist) lens[n++] = 0; }
            }
            huff_t lh, dh;
            huff_build(&lh, lens, hlit);
            huff_build(&dh, lens + hlit, hdist);
            inf_block(&s, &lh, &dh);
        } else { s.error = true; }
    } while (!final && !s.error);
    if (s.error) { kfree(s.out); return NULL; }
    *outlen = s.outlen;
    return s.out ? s.out : (uint8_t *)kzalloc(1);
}

/* ============================== ZIP ===================================== */
/* local file header sig 0x04034b50; central dir 0x02014b50; EOCD 0x06054b50 */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* CRC32 for writing */
static uint32_t crc_tab[256];
static bool crc_init;
static void crc_setup(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    crc_init = true;
}
static uint32_t crc32(const uint8_t *p, uint32_t n) {
    if (!crc_init) crc_setup();
    uint32_t c = 0xFFFFFFFF;
    for (uint32_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

static int zip_list(const char *path) {
    int id = fs_resolve(path, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE) { shell_print_error(id < 0 ? id : E_ISDIR, path); return 1; }
    const uint8_t *d = fs_nodes[id].data;
    uint32_t len = fs_nodes[id].size, p = 0, n = 0;
    uint32_t total = 0, comp = 0;
    kprintf("Archive: %s\n\n  %-36s %9s %9s %s\n", path, "Name", "Size", "Packed", "Method");
    while (p + 30 <= len && rd32(d + p) == 0x04034b50) {
        uint16_t method = rd16(d + p + 8);
        uint32_t csize = rd32(d + p + 18), usize = rd32(d + p + 22);
        uint16_t nlen = rd16(d + p + 26), elen = rd16(d + p + 28);
        char name[64];
        uint32_t nn = nlen < 63 ? nlen : 63;
        memcpy(name, d + p + 30, nn); name[nn] = 0;
        bool dir = nlen && name[nlen - 1] == '/';
        kprintf("  %-36.36s %9u %9u %s\n", name, usize, csize, dir ? "dir" : method == 0 ? "stored" : method == 8 ? "deflate" : "?");
        total += usize; comp += csize;
        p += 30 + nlen + elen + csize;
        n++;
    }
    kprintf("\n  %u file%s, %u bytes -> %u packed", n, n == 1 ? "" : "s", total, comp);
    if (total) kprintf(" (%u%%)", comp * 100 / total);
    kprintf("\n");
    return 0;
}

static int mkpath_dirs(int base, const char *rel, char *leaf) {
    /* create intermediate dirs in rel (which may contain '/'); return parent dir id, leaf name out */
    int cur = base;
    const char *s = rel;
    char comp[FS_NAME_MAX];
    leaf[0] = 0;
    while (*s) {
        const char *slash = strchr(s, '/');
        int l = slash ? (int)(slash - s) : (int)strlen(s);
        if (l <= 0 || l >= FS_NAME_MAX) { if (!slash) break; s = slash + 1; continue; }
        memcpy(comp, s, l); comp[l] = 0;
        if (!slash) { strlcpy(leaf, comp, FS_NAME_MAX); return cur; }
        int nx = fs_lookup(cur, comp);
        if (nx < 0) nx = fs_create(cur, comp, FT_DIR);
        if (nx < 0) return -1;
        cur = nx;
        s = slash + 1;
    }
    return leaf[0] ? cur : -1;
}

static int zip_extract(const char *path, const char *dest) {
    int id = fs_resolve(path, fs_cwd);
    if (id < 0 || fs_nodes[id].type != FT_FILE) { shell_print_error(id < 0 ? id : E_ISDIR, path); return 1; }
    int destdir = fs_cwd;
    if (dest) {
        destdir = fs_resolve(dest, fs_cwd);
        if (destdir < 0) { char leaf[FS_NAME_MAX]; int par = fs_resolve_parent(dest, fs_cwd, leaf); if (par >= 0) destdir = fs_create(par, leaf, FT_DIR); }
        if (destdir < 0 || fs_nodes[destdir].type != FT_DIR) { shell_print_error(E_NOTDIR, dest); return 1; }
    }
    const uint8_t *d = fs_nodes[id].data;
    uint32_t len = fs_nodes[id].size, p = 0;
    int nfiles = 0, errs = 0;
    while (p + 30 <= len && rd32(d + p) == 0x04034b50) {
        uint16_t method = rd16(d + p + 8);
        uint32_t csize = rd32(d + p + 18), usize = rd32(d + p + 22);
        uint16_t nlen = rd16(d + p + 26), elen = rd16(d + p + 28);
        char name[256];
        uint32_t nn = nlen < 255 ? nlen : 255;
        memcpy(name, d + p + 30, nn); name[nn] = 0;
        const uint8_t *data = d + p + 30 + nlen + elen;
        if (data + csize > d + len) break;
        bool isdir = nlen && name[nlen - 1] == '/';
        if (isdir) { name[nlen - 1] = 0; char leaf[FS_NAME_MAX]; int par = mkpath_dirs(destdir, name, leaf); if (par >= 0 && fs_lookup(par, leaf) < 0) fs_create(par, leaf, FT_DIR); }
        else {
            uint8_t *out = NULL; uint32_t olen = 0; bool freeout = false;
            if (method == 0) { out = (uint8_t *)data; olen = csize; }
            else if (method == 8) { out = inflate(data, csize, usize, &olen); freeout = true; if (!out) { errs++; p += 30 + nlen + elen + csize; continue; } }
            else { errs++; kprintf("  skip %s (unsupported compression)\n", name); p += 30 + nlen + elen + csize; continue; }
            char leaf[FS_NAME_MAX];
            int par = strchr(name, '/') ? mkpath_dirs(destdir, name, leaf) : (strlcpy(leaf, name, FS_NAME_MAX), destdir);
            if (par >= 0 && fs_valid_name(leaf)) {
                int f = fs_lookup(par, leaf);
                if (f < 0) f = fs_create(par, leaf, FT_FILE);
                if (f >= 0) { fs_write(f, out, olen); nfiles++; }
                else errs++;
            } else errs++;
            if (freeout) kfree(out);
        }
        p += 30 + nlen + elen + csize;
    }
    if (cfg.autosave) fs_save();
    kprintf("Extracted %d file%s%s.\n", nfiles, nfiles == 1 ? "" : "s", errs ? " (some entries skipped)" : "");
    return errs ? 1 : 0;
}

/* create a STORED (uncompressed) zip */
typedef struct { uint8_t *d; uint32_t len, cap; } buf_t;
static bool bput(buf_t *b, const void *p, uint32_t n) {
    if (b->len + n > b->cap) { uint32_t nc = b->cap ? b->cap : 4096; while (nc < b->len + n) nc *= 2; uint8_t *x = krealloc(b->d, nc); if (!x) return false; b->d = x; b->cap = nc; }
    memcpy(b->d + b->len, p, n); b->len += n; return true;
}
static void b16(buf_t *b, uint16_t v) { uint8_t t[2] = {v & 0xFF, v >> 8}; bput(b, t, 2); }
static void b32(buf_t *b, uint32_t v) { uint8_t t[4] = {v & 0xFF, (v>>8)&0xFF, (v>>16)&0xFF, v>>24}; bput(b, t, 4); }

typedef struct { char name[220]; uint32_t crc, size, off; } cent_t;

static void zip_add_node(buf_t *b, cent_t *cent, int *nc, int node, const char *rel) {
    fs_node_t *n = fs_get(node);
    if (!n) return;
    char full[220];
    if (n->type == FT_DIR) {
        snprintf(full, sizeof full, "%s/", rel);
        cent_t *c = &cent[(*nc)++];
        strlcpy(c->name, full, sizeof c->name); c->crc = 0; c->size = 0; c->off = b->len;
        b32(b, 0x04034b50); b16(b, 20); b16(b, 0); b16(b, 0); b16(b, 0); b16(b, 0);
        b32(b, 0); b32(b, 0); b32(b, 0); b16(b, (uint16_t)strlen(full)); b16(b, 0);
        bput(b, full, strlen(full));
        int kids[FS_MAX_NODES]; int k = fs_children(node, kids, FS_MAX_NODES, true);
        for (int i = 0; i < k && *nc < 250; i++) { char sub[220]; snprintf(sub, sizeof sub, "%s/%s", rel, fs_nodes[kids[i]].name); zip_add_node(b, cent, nc, kids[i], sub); }
        return;
    }
    uint32_t crc = crc32(n->data ? n->data : (uint8_t *)"", n->size);
    cent_t *c = &cent[(*nc)++];
    strlcpy(c->name, rel, sizeof c->name); c->crc = crc; c->size = n->size; c->off = b->len;
    b32(b, 0x04034b50); b16(b, 20); b16(b, 0); b16(b, 0); b16(b, 0); b16(b, 0);
    b32(b, crc); b32(b, n->size); b32(b, n->size); b16(b, (uint16_t)strlen(rel)); b16(b, 0);
    bput(b, rel, strlen(rel));
    if (n->size) bput(b, n->data, n->size);
}

static int zip_create(const char *archive, char **files, int nfiles) {
    if (!nfiles) { kprintf("usage: zip archive.zip file...\n"); return 1; }
    buf_t b = {0};
    cent_t *cent = kzalloc(sizeof(cent_t) * 256);
    if (!cent) return 1;
    int nc = 0;
    for (int i = 0; i < nfiles; i++) {
        int node = fs_resolve(files[i], fs_cwd);
        if (node < 0) { shell_print_error(E_NOENT, files[i]); kfree(cent); kfree(b.d); return 1; }
        zip_add_node(&b, cent, &nc, node, fs_nodes[node].name);
    }
    uint32_t cd_off = b.len;
    for (int i = 0; i < nc; i++) {
        cent_t *c = &cent[i];
        b32(&b, 0x02014b50); b16(&b, 20); b16(&b, 20); b16(&b, 0); b16(&b, 0); b16(&b, 0); b16(&b, 0);
        b32(&b, c->crc); b32(&b, c->size); b32(&b, c->size);
        b16(&b, (uint16_t)strlen(c->name)); b16(&b, 0); b16(&b, 0); b16(&b, 0); b16(&b, 0); b32(&b, 0);
        b32(&b, c->off);
        bput(&b, c->name, strlen(c->name));
    }
    uint32_t cd_size = b.len - cd_off;
    b32(&b, 0x06054b50); b16(&b, 0); b16(&b, 0); b16(&b, (uint16_t)nc); b16(&b, (uint16_t)nc);
    b32(&b, cd_size); b32(&b, cd_off); b16(&b, 0);
    int id = fs_mkfile(archive, fs_cwd);
    int r = id < 0 ? id : fs_write(id, b.d, b.len);
    uint32_t total = b.len;
    kfree(cent); kfree(b.d);
    if (r < 0) { shell_print_error(r, archive); return 1; }
    if (cfg.autosave) fs_save();
    kprintf("Created %s: %d entr%s, %u bytes (stored).\n", archive, nc, nc == 1 ? "y" : "ies", total);
    return 0;
}

int zip_cmd(int argc, char **argv) {
    const char *prog = argv[0];
    bool is_unzip = !strcasecmp(prog, "unzip");
    if (is_unzip) {
        if (argc < 2) { kprintf("usage: unzip archive.zip [dest]\n"); return 1; }
        return zip_extract(argv[1], argc > 2 ? argv[2] : NULL);
    }
    /* 7z / zip : need an operation unless it's 'zip archive files' */
    if (argc >= 3 && (!strcasecmp(argv[1], "a") || !strcasecmp(argv[1], "x") || !strcasecmp(argv[1], "l"))) {
        if (!strcasecmp(argv[1], "a")) return zip_create(argv[2], argv + 3, argc - 3);
        if (!strcasecmp(argv[1], "x") || !strcasecmp(argv[1], "e")) return zip_extract(argv[2], argc > 3 ? argv[3] : NULL);
        return zip_list(argv[2]);
    }
    if (!strcasecmp(prog, "zip") && argc >= 3) return zip_create(argv[1], argv + 2, argc - 2);
    kprintf("AxisZip (7-Zip style) - works with real .zip files\n");
    kprintf("  7z a <file.zip> <files...>   create (stored)\n");
    kprintf("  7z x <file.zip> [folder]     extract (stored or deflate)\n");
    kprintf("  7z l <file.zip>              list contents\n");
    kprintf("  zip <file.zip> <files...>    create\n");
    kprintf("  unzip <file.zip> [folder]    extract\n");
    return argc < 2 ? 1 : 0;
}

/* interactive (store icon) */
int sevenzip_run(void) {
    con_set_attr(theme.normal);
    con_clear();
    uint8_t s = con_get_attr();
    con_set_attr(theme.accent);
    kprintf("  AxisZip - the AxisOS ZIP tool (7-Zip style)\n");
    con_set_attr(theme.dim);
    kprintf("  Create and open real .zip files.\n\n");
    con_set_attr(s);
    kprintf("  1) Create a .zip\n  2) Extract a .zip\n  3) List a .zip\n  4) Quit\n\n  Choose: ");
    int k = kbd_getkey(); kprintf("%c\n\n", (k >= 32 && k < 127) ? k : ' ');
    char a[128] = "", b[200] = "";
    char *argv[18]; int n = 0;
    if (k == '1') {
        con_write("  Zip name (e.g. backup.zip): "); if (line_edit(a, sizeof a, false, false) <= 0) return 0;
        con_write("  Files/folders to add: "); if (line_edit(b, sizeof b, false, false) <= 0) return 0;
        argv[n++] = (char *)"7z"; argv[n++] = (char *)"a"; argv[n++] = a;
        for (char *t = strtok_simple(b); t && n < 18; t = strtok_simple(NULL)) argv[n++] = t;
        zip_cmd(n, argv);
    } else if (k == '2') {
        con_write("  Zip to extract: "); if (line_edit(a, sizeof a, false, false) <= 0) return 0;
        con_write("  Destination (blank = here): "); line_edit(b, sizeof b, false, false);
        char *av[4] = {(char *)"7z", (char *)"x", a, b[0] ? b : NULL};
        zip_cmd(b[0] ? 4 : 3, av);
    } else if (k == '3') {
        con_write("  Zip to list: "); if (line_edit(a, sizeof a, false, false) <= 0) return 0;
        char *av[3] = {(char *)"7z", (char *)"l", a}; zip_cmd(3, av);
    } else return 0;
    con_set_attr(theme.dim); kprintf("\n  Press any key..."); con_set_attr(s); kbd_getkey();
    return 0;
}
