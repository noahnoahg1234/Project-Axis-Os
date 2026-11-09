/*
 * AxisFS - the unified, case-insensitive, in-memory filesystem.
 *
 * One tree serves all three systems: Linux and FreeBSD see "/home/user",
 * Windows sees "C:\Users\user". Both '/' and '\' separate paths, a drive
 * prefix like "C:" is accepted, and every lookup ignores case (while the
 * original case of each name is preserved). The tree is persisted to the
 * AxisOS disk (LBA 2048..) when saving is possible.
 */
#include "axis.h"

fs_node_t fs_nodes[FS_MAX_NODES];
int fs_cwd = FS_ROOT;
bool fs_dirty;

bool storage_ready(void);

static uint32_t now(void) {
    rtc_time_t t;
    rtc_read(&t);
    return rtc_unix(&t);
}

const char *fs_strerror(int e) {
    switch (e) {
    case E_OK: return "Success";
    case E_NOENT: return "No such file or directory";
    case E_EXIST: return "File exists";
    case E_NOTDIR: return "Not a directory";
    case E_ISDIR: return "Is a directory";
    case E_INVAL: return "Invalid argument";
    case E_NOSPC: return "No space left on device";
    case E_NOTEMPTY: return "Directory not empty";
    case E_ROFS: return "File is read-only";
    case E_PERM: return "Operation not permitted";
    case E_IO: return "Input/output error";
    }
    return "Unknown error";
}

fs_node_t *fs_get(int id) {
    if (id < 0 || id >= FS_MAX_NODES || fs_nodes[id].type == FT_FREE) return NULL;
    return &fs_nodes[id];
}

bool fs_valid_name(const char *n) {
    size_t l = strlen(n);
    if (!l || l >= FS_NAME_MAX) return false;
    if (!strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (; *n; n++)
        if (strchr("/\\:*?\"<>|", *n) || (uint8_t)*n < 32) return false;
    return true;
}

int fs_lookup(int dir, const char *name) {
    for (int i = 1; i < FS_MAX_NODES; i++)
        if (fs_nodes[i].type != FT_FREE && fs_nodes[i].parent == dir && !strcasecmp(fs_nodes[i].name, name))
            return i;
    return E_NOENT;
}

static int alloc_node(void) {
    for (int i = 1; i < FS_MAX_NODES; i++)
        if (fs_nodes[i].type == FT_FREE) return i;
    return E_NOSPC;
}

int fs_create(int parent, const char *name, int type) {
    fs_node_t *p = fs_get(parent);
    if (!p) return E_NOENT;
    if (p->type != FT_DIR) return E_NOTDIR;
    if (!fs_valid_name(name)) return E_INVAL;
    if (fs_lookup(parent, name) >= 0) return E_EXIST;
    int id = alloc_node();
    if (id < 0) return id;
    fs_node_t *n = &fs_nodes[id];
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type;
    n->parent = (int16_t)parent;
    strlcpy(n->name, name, FS_NAME_MAX);
    if (name[0] == '.') n->attr |= FA_HIDDEN;
    n->mtime = now();
    fs_dirty = true;
    return id;
}

static int reserve(fs_node_t *n, uint32_t len) {
    if (len <= n->cap) return E_OK;
    uint32_t nc = n->cap ? n->cap : 64;
    while (nc < len) nc *= 2;
    uint8_t *d = krealloc(n->data, nc);
    if (!d) return E_NOSPC;
    n->data = d;
    n->cap = nc;
    return E_OK;
}

int fs_write(int id, const void *data, uint32_t len) {
    fs_node_t *n = fs_get(id);
    if (!n) return E_NOENT;
    if (n->type != FT_FILE) return E_ISDIR;
    if (n->attr & FA_READONLY) return E_ROFS;
    int r = reserve(n, len);
    if (r) return r;
    if (len) memcpy(n->data, data, len);
    n->size = len;
    n->mtime = now();
    fs_dirty = true;
    return E_OK;
}

int fs_append(int id, const void *data, uint32_t len) {
    fs_node_t *n = fs_get(id);
    if (!n) return E_NOENT;
    if (n->type != FT_FILE) return E_ISDIR;
    if (n->attr & FA_READONLY) return E_ROFS;
    int r = reserve(n, n->size + len);
    if (r) return r;
    memcpy(n->data + n->size, data, len);
    n->size += len;
    n->mtime = now();
    fs_dirty = true;
    return E_OK;
}

bool fs_is_ancestor(int anc, int node) {
    while (node > 0) {
        if (node == anc) return true;
        node = fs_nodes[node].parent;
    }
    return node == anc;
}

static void free_node(int id) {
    for (int i = 1; i < FS_MAX_NODES; i++)
        if (fs_nodes[i].type != FT_FREE && fs_nodes[i].parent == id) free_node(i);
    kfree(fs_nodes[id].data);
    memset(&fs_nodes[id], 0, sizeof fs_nodes[id]);
}

int fs_delete(int id, bool recursive) {
    fs_node_t *n = fs_get(id);
    if (!n) return E_NOENT;
    if (id == FS_ROOT) return E_PERM;
    if (n->attr & FA_READONLY) return E_ROFS;
    if (n->type == FT_DIR && !recursive)
        for (int i = 1; i < FS_MAX_NODES; i++)
            if (fs_nodes[i].type != FT_FREE && fs_nodes[i].parent == id) return E_NOTEMPTY;
    if (fs_is_ancestor(id, fs_cwd)) fs_cwd = n->parent;
    free_node(id);
    fs_dirty = true;
    return E_OK;
}

int fs_copy(int src, int dstdir, const char *name) {
    fs_node_t *s = fs_get(src);
    if (!s) return E_NOENT;
    if (s->type == FT_DIR && fs_is_ancestor(src, dstdir)) return E_INVAL;
    int ex = fs_lookup(dstdir, name);
    int id;
    if (ex >= 0) {
        if (ex == src) return E_EXIST;
        if (fs_nodes[ex].type != s->type) return E_EXIST;
        if (s->type == FT_DIR) id = ex; /* merge into existing directory */
        else return fs_write(ex, s->data, s->size);
    } else {
        id = fs_create(dstdir, name, s->type);
        if (id < 0) return id;
    }
    if (s->type == FT_FILE) {
        int r = fs_write(id, s->data, s->size);
        fs_nodes[id].attr = s->attr & ~FA_SYSTEM;
        return r;
    }
    int kids[FS_MAX_NODES];
    int k = fs_children(src, kids, FS_MAX_NODES, true);
    for (int i = 0; i < k; i++) {
        int r = fs_copy(kids[i], id, fs_nodes[kids[i]].name);
        if (r < 0) return r;
    }
    return E_OK;
}

int fs_move(int id, int newparent, const char *newname) {
    fs_node_t *n = fs_get(id);
    fs_node_t *p = fs_get(newparent);
    if (!n || !p) return E_NOENT;
    if (id == FS_ROOT) return E_PERM;
    if (p->type != FT_DIR) return E_NOTDIR;
    if (!fs_valid_name(newname)) return E_INVAL;
    if (n->type == FT_DIR && fs_is_ancestor(id, newparent)) return E_INVAL;
    int ex = fs_lookup(newparent, newname);
    if (ex >= 0 && ex != id) {
        if (fs_nodes[ex].type == FT_DIR) return E_EXIST;
        if (n->type == FT_DIR) return E_EXIST;
        fs_delete(ex, false); /* overwrite file */
    }
    n->parent = (int16_t)newparent;
    strlcpy(n->name, newname, FS_NAME_MAX);
    n->mtime = now();
    fs_dirty = true;
    return E_OK;
}

static int cmp_nodes(int a, int b) {
    fs_node_t *x = &fs_nodes[a], *y = &fs_nodes[b];
    if (x->type != y->type) return x->type == FT_DIR ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int fs_children(int dir, int *out, int max, bool hidden) {
    int n = 0;
    for (int i = 1; i < FS_MAX_NODES && n < max; i++) {
        fs_node_t *c = &fs_nodes[i];
        if (c->type == FT_FREE || c->parent != dir) continue;
        if (!hidden && (c->attr & FA_HIDDEN)) continue;
        int j = n++;
        while (j > 0 && cmp_nodes(out[j - 1], i) > 0) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return n;
}

void fs_path_unix(int id, char *buf, size_t n) {
    int chain[64], depth = 0;
    while (id > 0 && depth < 64) {
        chain[depth++] = id;
        id = fs_nodes[id].parent;
    }
    if (!depth) {
        strlcpy(buf, "/", n);
        return;
    }
    buf[0] = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (strlen(buf) + strlen(fs_nodes[chain[i]].name) + 2 >= n) break;
        strcat(buf, "/");
        strcat(buf, fs_nodes[chain[i]].name);
    }
}

void fs_path(int id, char *buf, size_t n) {
    if (cur_os != OS_WINDOWS) {
        fs_path_unix(id, buf, n);
        return;
    }
    char tmp[256];
    fs_path_unix(id, tmp, sizeof tmp);
    snprintf(buf, n, "C:%s", tmp);
    for (char *p = buf; *p; p++)
        if (*p == '/') *p = '\\';
}

int fs_home(void) {
    const char *base = cur_os == OS_WINDOWS ? "Users" : "home";
    int b = fs_lookup(FS_ROOT, base);
    if (b < 0) b = fs_create(FS_ROOT, base, FT_DIR);
    if (b < 0) return FS_ROOT;
    int h = fs_lookup(b, cfg.username);
    if (h < 0) h = fs_create(b, cfg.username, FT_DIR);
    return h < 0 ? FS_ROOT : h;
}

/* split helpers ------------------------------------------------------- */
static const char *skip_drive(const char *p, bool *abs) {
    *abs = false;
    if (isalpha(p[0]) && p[1] == ':') {
        p += 2;
        *abs = true; /* "C:" and "C:\x" are both rooted */
    }
    if (*p == '/' || *p == '\\') *abs = true;
    return p;
}

static int walk(const char *path, int cwd, bool fuzzy, char *corr, size_t corrn) {
    bool abs;
    const char *p = skip_drive(path, &abs);
    int cur = abs ? FS_ROOT : cwd;
    if (corr) {
        corr[0] = 0;
        size_t pre = (size_t)(p - path);
        if (pre && pre < corrn) {
            memcpy(corr, path, pre);
            corr[pre] = 0;
        }
    }
    if (!abs && p[0] == '~' && (p[1] == 0 || p[1] == '/' || p[1] == '\\')) {
        cur = fs_home();
        p++;
        if (corr) strlcpy(corr, "~", corrn);
    }
    while (*p) {
        while (*p == '/' || *p == '\\') {
            if (corr) {
                size_t l = strlen(corr);
                if (l + 1 < corrn) corr[l] = *p, corr[l + 1] = 0;
            }
            p++;
        }
        if (!*p) break;
        char comp[FS_NAME_MAX + 1];
        int l = 0;
        while (*p && *p != '/' && *p != '\\') {
            if (l < FS_NAME_MAX) comp[l++] = *p;
            p++;
        }
        comp[l] = 0;
        if (fs_nodes[cur].type != FT_DIR) return E_NOTDIR;
        const char *use = comp;
        if (!strcmp(comp, ".")) {
        } else if (!strcmp(comp, "..")) {
            cur = cur == FS_ROOT ? FS_ROOT : fs_nodes[cur].parent;
        } else {
            int nx = fs_lookup(cur, comp);
            if (nx < 0 && fuzzy) {
                int best = -1, bd = 1000, ties = 0;
                for (int i = 1; i < FS_MAX_NODES; i++) {
                    if (fs_nodes[i].type == FT_FREE || fs_nodes[i].parent != cur) continue;
                    int d = edit_distance(comp, fs_nodes[i].name);
                    if (d < bd) bd = d, best = i, ties = 0;
                    else if (d == bd) ties++;
                }
                if (best >= 0 && !ties && bd <= ac_threshold(comp)) {
                    nx = best;
                    use = fs_nodes[best].name;
                }
            }
            if (nx < 0) return E_NOENT;
            cur = nx;
        }
        if (corr && strlen(corr) + strlen(use) + 1 < corrn) strcat(corr, use);
    }
    return cur;
}

int fs_resolve(const char *path, int cwd) {
    if (!path || !*path) return cwd;
    return walk(path, cwd, false, NULL, 0);
}

int fs_resolve_fuzzy(const char *path, int cwd, char *corrected, size_t n) {
    int r = fs_resolve(path, cwd);
    if (corrected) strlcpy(corrected, path, n);
    if (r >= 0 || adv_mode || cfg.autocorrect == AC_OFF) return r;
    char tmp[256];
    int f = walk(path, cwd, true, tmp, sizeof tmp);
    if (f < 0) return r;
    if (cfg.autocorrect == AC_AUTO) {
        if (corrected) strlcpy(corrected, tmp, n);
        return f;
    }
    /* suggest only */
    if (corrected) strlcpy(corrected, tmp, n);
    return r;
}

int fs_resolve_parent(const char *path, int cwd, char *leaf) {
    char buf[256];
    strlcpy(buf, path, sizeof buf);
    size_t l = strlen(buf);
    while (l > 1 && (buf[l - 1] == '/' || buf[l - 1] == '\\')) buf[--l] = 0;
    char *s1 = strrchr(buf, '/'), *s2 = strrchr(buf, '\\');
    char *sep = s1 > s2 ? s1 : s2;
    const char *name;
    int parent;
    if (sep) {
        *sep = 0;
        name = sep + 1;
        parent = buf[0] ? fs_resolve(buf, cwd) : FS_ROOT;
        if (sep == buf + 2 && isalpha(buf[0]) && buf[1] == ':') parent = FS_ROOT;
    } else if (isalpha(buf[0]) && buf[1] == ':') {
        name = buf + 2;
        parent = FS_ROOT;
    } else {
        name = buf;
        parent = cwd;
    }
    if (parent < 0) return parent;
    if (fs_nodes[parent].type != FT_DIR) return E_NOTDIR;
    if (!fs_valid_name(name)) return E_INVAL;
    strlcpy(leaf, name, FS_NAME_MAX);
    return parent;
}

int fs_mkfile(const char *path, int cwd) {
    int id = fs_resolve(path, cwd);
    if (id >= 0) return fs_nodes[id].type == FT_FILE ? id : E_ISDIR;
    char leaf[FS_NAME_MAX];
    int p = fs_resolve_parent(path, cwd, leaf);
    if (p < 0) return p;
    return fs_create(p, leaf, FT_FILE);
}

void fs_stats(int *nodes, uint32_t *bytes) {
    int c = 0;
    uint32_t b = 0;
    for (int i = 0; i < FS_MAX_NODES; i++)
        if (fs_nodes[i].type != FT_FREE) c++, b += fs_nodes[i].size;
    if (nodes) *nodes = c;
    if (bytes) *bytes = b;
}

/* ---- default tree ---------------------------------------------------- */
static int mkdirp(const char *path) {
    int cur = FS_ROOT;
    char comp[FS_NAME_MAX];
    while (*path) {
        while (*path == '/') path++;
        int l = 0;
        while (*path && *path != '/') comp[l++] = *path++;
        comp[l] = 0;
        if (!l) break;
        int nx = fs_lookup(cur, comp);
        if (nx < 0) nx = fs_create(cur, comp, FT_DIR);
        if (nx < 0) return nx;
        cur = nx;
    }
    return cur;
}
static void mkf(const char *dir, const char *name, const char *text, uint8_t attr) {
    int d = mkdirp(dir);
    if (d < 0) return;
    int f = fs_create(d, name, FT_FILE);
    if (f < 0) return;
    fs_write(f, text, strlen(text));
    fs_nodes[f].attr |= attr;
}

void fs_format(void) {
    for (int i = 0; i < FS_MAX_NODES; i++) {
        kfree(fs_nodes[i].data);
        memset(&fs_nodes[i], 0, sizeof fs_nodes[i]);
    }
    fs_nodes[0].type = FT_DIR;
    fs_nodes[0].parent = 0;
    fs_nodes[0].mtime = now();
    fs_cwd = FS_ROOT;

    char home[64], users[64];
    snprintf(home, sizeof home, "/home/%s", cfg.username);
    snprintf(users, sizeof users, "/Users/%s", cfg.username);

    const char *dirs[] = {"/bin", "/boot", "/dev", "/etc", "/tmp", "/usr/bin", "/usr/local/bin",
                          "/usr/local/etc", "/usr/share/doc", "/var/log", "/Program Files",
                          "/Windows/System32/drivers/etc"};
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) mkdirp(dirs[i]);
    char b[96];
    const char *sub[] = {"Desktop", "Documents", "Downloads", "Music", "Pictures"};
    for (int i = 0; i < 5; i++) {
        snprintf(b, sizeof b, "%s/%s", users, sub[i]);
        mkdirp(b);
    }
    snprintf(b, sizeof b, "%s/Documents", home);
    mkdirp(b);
    snprintf(b, sizeof b, "%s/Downloads", home);
    mkdirp(b);

    mkf("/etc", "motd", "Welcome to AxisOS - one system, three personalities.\n", 0);
    mkf("/etc", "os-release",
        "NAME=\"AxisOS\"\nVERSION=\"" AXIS_VERSION "\"\nID=axis\nPRETTY_NAME=\"AxisOS " AXIS_VERSION "\"\n"
        "PERSONALITIES=\"linux windows freebsd\"\n", 0);
    mkf("/etc", "rc.conf",
        "# FreeBSD-style rc.conf (informational)\nhostname=\"axis\"\nautocorrect_enable=\"YES\"\n", 0);
    mkf("/etc", "hosts", "127.0.0.1   localhost\n::1         localhost\n", 0);
    mkf("/boot", "README.txt",
        "AxisBoot - the AxisOS boot manager\n\n"
        "The boot menu lets you start Axis/Linux, Axis/Windows or Axis/FreeBSD\n"
        "and toggle Advanced mode with the A key.\n\n"
        "Default system, menu timeout and advanced mode are changed in the\n"
        "settings app (settings / control / bsdconfig) and stored in the\n"
        "settings sector on disk, which AxisBoot reads on every start.\n", FA_SYSTEM);
    mkf("/Windows", "win.ini", "; for 16-bit app support\n[fonts]\n[extensions]\n[mci extensions]\n[files]\n", FA_SYSTEM);
    mkf("/Windows/System32/drivers/etc", "hosts", "127.0.0.1       localhost\n", 0);
    mkf("/usr/share/doc", "axis-guide.txt",
        "AxisOS quick guide\n==================\n\n"
        "Every command works in every system - pick the names you know:\n\n"
        "  Linux        Windows      FreeBSD      What it does\n"
        "  ls           dir          ls           list a directory\n"
        "  cat          type         cat          print a file\n"
        "  cp / mv      copy / move  cp / mv      copy / move\n"
        "  rm           del          rm           delete files\n"
        "  clear        cls          clear        clear the screen\n"
        "  nano         edit         ee           text editor\n"
        "  files        explorer     files        file explorer\n"
        "  settings     control      bsdconfig    settings\n\n"
        "Commands and file names are not case-sensitive. Mistyped commands\n"
        "and paths are autocorrected (see settings).\n\n"
        "Pipes and redirection: ls | grep txt > list.txt\n"
        "Chain commands with ; or &&\n"
        "Switch system at any time: os linux | os windows | os freebsd\n"
        "Advanced mode: advanced on (expert tools: peek, poke, hexdump,\n"
        "  inb, outb, sector, cpuid, heap, panic ...)\n", 0);

    char welcome[640];
    snprintf(welcome, sizeof welcome,
             "Hello %s, welcome to AxisOS!\n\n"
             "Things to try:\n"
             "  help              list every command (Linux, Windows and FreeBSD names)\n"
             "  explorer          the AxisOS file explorer\n"
             "  settings          change theme, default system, autocorrect...\n"
             "  os windows        switch to the Windows personality\n"
             "  sl                typo? autocorrect fixes it to 'ls'\n"
             "  cat /usr/share/doc/axis-guide.txt | more\n", cfg.username);
    mkf(home, "welcome.txt", welcome, 0);
    mkf(users, "welcome.txt", welcome, 0);
    mkf(home, ".profile", "# login shell profile\nexport EDITOR=nano\n", 0);
    snprintf(b, sizeof b, "%s/Documents", users);
    mkf(b, "notes.txt", "My notes\n--------\n", 0);
    mkf("/var/log", "messages", "", 0);
    fs_dirty = true;
}

/* ---- persistence ----------------------------------------------------- */
#define FS_IMG_BYTES (FS_SECTORS * 512)
typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t version, count, length, checksum;
} fs_hdr_t;

static uint32_t checksum(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

bool fs_save(void) {
    if (!storage_ready()) return false;
    uint8_t *img = kmalloc(FS_IMG_BYTES);
    if (!img) return false;
    uint32_t pos = sizeof(fs_hdr_t), count = 0;
    bool fit = true;
    for (int i = 0; i < FS_MAX_NODES && fit; i++) {
        fs_node_t *n = &fs_nodes[i];
        if (n->type == FT_FREE) continue;
        uint32_t nl = (uint32_t)strlen(n->name);
        uint32_t need = 2 + 2 + 1 + 1 + 1 + nl + 4 + 4 + n->size;
        if (pos + need > FS_IMG_BYTES) {
            fit = false;
            break;
        }
        uint16_t id = (uint16_t)i;
        memcpy(img + pos, &id, 2); pos += 2;
        memcpy(img + pos, &n->parent, 2); pos += 2;
        img[pos++] = n->type;
        img[pos++] = n->attr;
        img[pos++] = (uint8_t)nl;
        memcpy(img + pos, n->name, nl); pos += nl;
        memcpy(img + pos, &n->mtime, 4); pos += 4;
        memcpy(img + pos, &n->size, 4); pos += 4;
        if (n->size) memcpy(img + pos, n->data, n->size);
        pos += n->size;
        count++;
    }
    if (!fit) {
        kfree(img);
        return false;
    }
    fs_hdr_t *h = (fs_hdr_t *)img;
    memcpy(h->magic, "AXFS", 4);
    h->version = 1;
    h->count = count;
    h->length = pos - sizeof(fs_hdr_t);
    h->checksum = checksum(img + sizeof(fs_hdr_t), h->length);
    uint32_t secs = (pos + 511) / 512;
    memset(img + pos, 0, secs * 512 - pos);
    bool ok = ata_write(FS_LBA, secs, img);
    kfree(img);
    if (ok) fs_dirty = false;
    return ok;
}

bool fs_load(void) {
    if (!storage_ready()) return false;
    uint8_t *img = kmalloc(FS_IMG_BYTES);
    if (!img) return false;
    bool ok = false;
    if (!ata_read(FS_LBA, 1, img)) goto out;
    fs_hdr_t h;
    memcpy(&h, img, sizeof h);
    if (memcmp(h.magic, "AXFS", 4) || h.version != 1 || h.length + sizeof h > FS_IMG_BYTES || h.count > FS_MAX_NODES)
        goto out;
    uint32_t secs = (uint32_t)(sizeof h + h.length + 511) / 512;
    if (!ata_read(FS_LBA, secs, img)) goto out;
    if (checksum(img + sizeof h, h.length) != h.checksum) goto out;

    for (int i = 0; i < FS_MAX_NODES; i++) {
        kfree(fs_nodes[i].data);
        memset(&fs_nodes[i], 0, sizeof fs_nodes[i]);
    }
    uint32_t pos = sizeof h, end = sizeof h + h.length;
    for (uint32_t k = 0; k < h.count && pos < end; k++) {
        uint16_t id;
        int16_t parent;
        memcpy(&id, img + pos, 2); pos += 2;
        memcpy(&parent, img + pos, 2); pos += 2;
        uint8_t type = img[pos++], attr = img[pos++], nl = img[pos++];
        if (id >= FS_MAX_NODES || nl >= FS_NAME_MAX) goto bad;
        fs_node_t *n = &fs_nodes[id];
        n->type = type;
        n->attr = attr;
        n->parent = parent;
        memcpy(n->name, img + pos, nl);
        n->name[nl] = 0;
        pos += nl;
        memcpy(&n->mtime, img + pos, 4); pos += 4;
        uint32_t size;
        memcpy(&size, img + pos, 4); pos += 4;
        if (pos + size > end) goto bad;
        if (size) {
            n->data = kmalloc(size);
            if (!n->data) goto bad;
            memcpy(n->data, img + pos, size);
            n->cap = n->size = size;
        }
        pos += size;
    }
    if (fs_nodes[0].type != FT_DIR) goto bad;
    for (int i = 1; i < FS_MAX_NODES; i++)
        if (fs_nodes[i].type != FT_FREE && !fs_get(fs_nodes[i].parent)) fs_nodes[i].parent = FS_ROOT;
    ok = true;
    fs_dirty = false;
    goto out;
bad:
    fs_format();
out:
    kfree(img);
    return ok;
}

void fs_init(void) {
    if (!fs_load()) {
        fs_format();
        klog("axisfs: created a fresh filesystem");
    } else
        klog("axisfs: filesystem loaded from disk");
    fs_cwd = fs_home();
}
