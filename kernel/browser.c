/*
 * AxisBrowser - a text-mode web browser.
 *
 * Fetches pages over http://, renders HTML as text (headings, paragraphs,
 * lists, links, preformatted text) and lets you follow links with the
 * keyboard. https:// pages and web searches go through FrogFind
 * (frogfind.com), which serves simplified http:// versions of any site.
 */
#include "axis.h"

#define PW 78       /* text width */
#define VIEW_TOP 1
#define VIEW_ROWS 22
#define MAX_LINKS 600
#define HIST 32

typedef struct {
    char c;
    uint8_t a;
    int16_t link;
} cell_t;

static cell_t *page;
static int nlines, cap_lines;
static char *links[MAX_LINKS];
static int nlinks;
static char title[96];
static char cur_url[512];
static int top, sel = -1;
static char status[120];
static char *history[HIST];
static int hist_n, hist_pos;

/* attributes, from the theme */
static uint8_t A_TEXT, A_HEAD, A_LINK, A_BOLD, A_DIM;

/* ---------------------------------------------------------- page model -- */
static void page_clear(void) {
    for (int i = 0; i < nlinks; i++) kfree(links[i]);
    nlinks = 0;
    nlines = 0;
    title[0] = 0;
    top = 0;
    sel = -1;
}

static cell_t *line_at(int l) {
    if (l >= cap_lines) {
        int nc = cap_lines ? cap_lines * 2 : 512;
        while (nc <= l) nc *= 2;
        if (nc > 20000) return NULL;
        cell_t *np = krealloc(page, sizeof(cell_t) * PW * nc);
        if (!np) return NULL;
        page = np;
        cap_lines = nc;
    }
    while (nlines <= l) {
        cell_t *c = page + nlines * PW;
        for (int i = 0; i < PW; i++) c[i] = (cell_t){' ', A_TEXT, -1};
        nlines++;
    }
    return page + l * PW;
}

/* ------------------------------------------------------------ renderer -- */
static int ln, col, indent;
static bool pre, line_has_text, pending_space;
static uint8_t cur_attr;
static int cur_link;
static int blank_lines; /* consecutive empty lines emitted */
typedef struct { char c; uint8_t a; int16_t link; } wch_t;
static wch_t word[PW];
static int wlen;

static void newline(void) {
    if (!line_has_text) blank_lines++;
    else blank_lines = 0;
    ln++;
    col = indent;
    line_has_text = false;
    pending_space = false;
}

static void put_cell(char c, uint8_t a, int link) {
    if (col >= PW) newline();
    cell_t *l = line_at(ln);
    if (!l) return;
    l[col++] = (cell_t){c, a, (int16_t)link};
    line_has_text = true;
}

static void flush_word(void) {
    if (!wlen) return;
    int need = wlen + (pending_space && col > indent ? 1 : 0);
    if (col + need > PW && col > indent) newline();
    if (pending_space && col > indent) {
        /* keep the link colour across the space inside a link */
        put_cell(' ', word[0].link >= 0 && cur_link == word[0].link ? word[0].a : A_TEXT, word[0].link >= 0 && cur_link == word[0].link ? word[0].link : -1);
    }
    for (int i = 0; i < wlen; i++) put_cell(word[i].c, word[i].a, word[i].link);
    wlen = 0;
    pending_space = false;
}

static void out_char(char c) {
    if (pre) {
        if (c == '\n') {
            line_has_text = true;
            newline();
        } else if (c == '\t') {
            do put_cell(' ', cur_attr, cur_link);
            while (col % 8);
        } else if ((uint8_t)c >= 32)
            put_cell(c, cur_attr, cur_link);
        return;
    }
    if (isspace(c)) {
        flush_word();
        if (line_has_text) pending_space = true;
        return;
    }
    if ((uint8_t)c < 32) return;
    if (wlen >= PW - indent - 1) flush_word();
    word[wlen++] = (wch_t){c, cur_attr, (int16_t)cur_link};
}

static void block_break(int blanks) {
    flush_word();
    if (line_has_text) newline();
    while (blank_lines < blanks) {
        line_at(ln);
        newline();
    }
}

static void out_str(const char *s, uint8_t a) {
    uint8_t save = cur_attr;
    cur_attr = a;
    while (*s) out_char(*s++);
    cur_attr = save;
}

static int entity(const char *p, char *out) {
    /* returns length consumed (after '&'), writes one char */
    static const struct { const char *n; char c; } ents[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '},
        {"copy;", 'c'}, {"reg;", 'R'}, {"mdash;", '-'}, {"ndash;", '-'}, {"hellip;", '.'}, {"laquo;", '<'},
        {"raquo;", '>'}, {"middot;", '.'}, {"bull;", '*'}, {"rsquo;", '\''}, {"lsquo;", '\''}, {"rdquo;", '"'},
        {"ldquo;", '"'}, {"times;", 'x'}, {"deg;", (char)0xF8}, {"eacute;", (char)0x82}, {"uuml;", (char)0x81},
        {"ouml;", (char)0x94}, {"auml;", (char)0x84}};
    if (p[0] == '#') {
        int v = (p[1] == 'x' || p[1] == 'X') ? (int)strtol(p + 2, NULL, 16) : atoi(p + 1);
        const char *e = strchr(p, ';');
        if (!e || e - p > 8) return 0;
        *out = v == 160 ? ' ' : (v >= 32 && v < 127) ? (char)v : (v == 8211 || v == 8212) ? '-' : (v == 8217 || v == 8216) ? '\'' : (v == 8220 || v == 8221) ? '"' : '?';
        return (int)(e - p) + 1;
    }
    for (unsigned i = 0; i < sizeof ents / sizeof *ents; i++) {
        size_t l = strlen(ents[i].n);
        if (!strncasecmp(p, ents[i].n, l)) {
            *out = ents[i].c;
            return (int)l;
        }
    }
    return 0;
}

static bool get_attr(const char *attrs, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    const char *p = attrs;
    while (*p) {
        while (*p && (isspace(*p) || *p == '/')) p++;
        if (!*p) break;
        const char *ns = p;
        while (*p && !isspace(*p) && *p != '=' && *p != '>') p++;
        size_t l = (size_t)(p - ns);
        while (isspace(*p)) p++;
        const char *val = "";
        size_t vl = 0;
        if (*p == '=') {
            p++;
            while (isspace(*p)) p++;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                val = p;
                while (*p && *p != q) p++;
                vl = (size_t)(p - val);
                if (*p) p++;
            } else {
                val = p;
                while (*p && !isspace(*p) && *p != '>') p++;
                vl = (size_t)(p - val);
            }
        }
        if (l == nl && !strncasecmp(ns, name, nl)) {
            if (vl >= n) vl = n - 1;
            /* decode &amp; in attribute values */
            size_t o = 0;
            for (size_t i = 0; i < vl && o + 1 < n; i++) {
                if (val[i] == '&' && !strncasecmp(val + i, "&amp;", 5)) {
                    out[o++] = '&';
                    i += 4;
                } else
                    out[o++] = val[i];
            }
            out[o] = 0;
            return true;
        }
    }
    return false;
}

static int add_link(const char *href) {
    if (nlinks >= MAX_LINKS) return -1;
    links[nlinks] = kstrdup(href);
    return links[nlinks] ? nlinks++ : -1;
}

static void render_html(const char *h, uint32_t len) {
    ln = 0;
    col = indent = 0;
    pre = line_has_text = pending_space = false;
    cur_attr = A_TEXT;
    cur_link = -1;
    blank_lines = 1;
    wlen = 0;
    int skip = 0, list_depth = 0, ol_num[8] = {0}, heading = 0, bold = 0;
    bool in_title = false;
    int tlen = 0;
    const char *p = h, *end = h + len;
    while (p < end) {
        if (*p == '<') {
            if (p + 4 <= end && !strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : end;
                continue;
            }
            const char *e = memchr(p, '>', (size_t)(end - p));
            if (!e) break;
            char tag[640];
            size_t tl = (size_t)(e - p - 1);
            if (tl >= sizeof tag) tl = sizeof tag - 1;
            memcpy(tag, p + 1, tl);
            tag[tl] = 0;
            p = e + 1;
            bool close = tag[0] == '/';
            char *name = tag + (close ? 1 : 0);
            char *attrs = name;
            while (*attrs && !isspace(*attrs) && *attrs != '/') attrs++;
            char nm[16];
            size_t nml = (size_t)(attrs - name);
            if (nml >= sizeof nm) nml = sizeof nm - 1;
            memcpy(nm, name, nml);
            nm[nml] = 0;
            str_lower(nm);

            if (!strcmp(nm, "script") || !strcmp(nm, "style") || !strcmp(nm, "noscript") || !strcmp(nm, "svg") ||
                !strcmp(nm, "select") || !strcmp(nm, "template")) {
                if (!close) {
                    /* jump to the matching close tag */
                    char endtag[24];
                    snprintf(endtag, sizeof endtag, "</%s", nm);
                    const char *q = p;
                    while (q < end && strncasecmp(q, endtag, strlen(endtag))) q++;
                    p = q;
                }
                continue;
            }
            if (skip) continue;
            if (!strcmp(nm, "title")) {
                in_title = !close;
                continue;
            }
            if (!strcmp(nm, "br")) {
                flush_word();
                line_has_text = true;
                newline();
            } else if (!strcmp(nm, "p") || !strcmp(nm, "div") || !strcmp(nm, "section") || !strcmp(nm, "article") ||
                       !strcmp(nm, "header") || !strcmp(nm, "footer") || !strcmp(nm, "nav") || !strcmp(nm, "form") ||
                       !strcmp(nm, "table") || !strcmp(nm, "blockquote") || !strcmp(nm, "main") || !strcmp(nm, "aside") ||
                       !strcmp(nm, "figure") || !strcmp(nm, "dl")) {
                block_break(!strcmp(nm, "p") || !strcmp(nm, "blockquote") ? 1 : 0);
                if (!strcmp(nm, "blockquote")) indent = close ? (indent >= 4 ? indent - 4 : 0) : indent + 4, col = indent;
            } else if (!strcmp(nm, "tr") || !strcmp(nm, "dt") || !strcmp(nm, "dd")) {
                block_break(0);
            } else if (!strcmp(nm, "td") || !strcmp(nm, "th")) {
                if (!close) {
                    flush_word();
                    if (line_has_text) pending_space = true;
                }
            } else if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) {
                block_break(1);
                if (!close) {
                    heading++;
                    cur_attr = A_HEAD;
                    if (nm[1] <= '2') out_str(nm[1] == '1' ? "# " : "## ", A_HEAD);
                } else {
                    if (heading) heading--;
                    cur_attr = bold ? A_BOLD : A_TEXT;
                }
            } else if (!strcmp(nm, "ul") || !strcmp(nm, "ol") || !strcmp(nm, "menu")) {
                block_break(list_depth ? 0 : 1);
                if (!close) {
                    if (list_depth < 7) ol_num[list_depth + 1] = !strcmp(nm, "ol") ? 1 : 0;
                    list_depth++;
                } else if (list_depth)
                    list_depth--;
                indent = list_depth * 3 > 30 ? 30 : list_depth * 3;
                col = indent;
            } else if (!strcmp(nm, "li")) {
                block_break(0);
                if (!close) {
                    int d = list_depth > 7 ? 7 : list_depth;
                    col = indent > 2 ? indent - 2 : 0;
                    char b[8];
                    if (d && ol_num[d]) snprintf(b, sizeof b, "%d.", ol_num[d]++);
                    else strcpy(b, "*");
                    for (char *q = b; *q; q++) put_cell(*q, A_DIM, -1);
                    col = indent > col ? indent : col + 1;
                    line_has_text = false;
                }
            } else if (!strcmp(nm, "pre")) {
                block_break(close ? 1 : 0);
                pre = !close;
            } else if (!strcmp(nm, "hr")) {
                block_break(0);
                for (int i = 0; i < PW; i++) put_cell((char)0xC4, A_DIM, -1);
                newline();
            } else if (!strcmp(nm, "b") || !strcmp(nm, "strong") || !strcmp(nm, "em") || !strcmp(nm, "i")) {
                bold += close ? -1 : 1;
                if (bold < 0) bold = 0;
                if (cur_link < 0 && !heading) cur_attr = bold ? A_BOLD : A_TEXT;
            } else if (!strcmp(nm, "a")) {
                if (!close) {
                    char href[512];
                    if (get_attr(attrs, "href", href, sizeof href) && href[0] && href[0] != '#' &&
                        strncasecmp(href, "javascript:", 11) && strncasecmp(href, "mailto:", 7)) {
                        cur_link = add_link(href);
                        if (cur_link >= 0) cur_attr = A_LINK;
                    }
                } else {
                    flush_word();
                    cur_link = -1;
                    cur_attr = heading ? A_HEAD : (bold ? A_BOLD : A_TEXT);
                }
            } else if (!strcmp(nm, "img")) {
                char alt[80];
                if (get_attr(attrs, "alt", alt, sizeof alt) && alt[0]) {
                    char b[96];
                    snprintf(b, sizeof b, "[%s]", alt);
                    out_str(b, cur_link >= 0 ? A_LINK : A_DIM);
                }
            } else if (!strcmp(nm, "input") && !close) {
                char type[16] = "text", val[64] = "";
                get_attr(attrs, "type", type, sizeof type);
                if (!strcasecmp(type, "text") || !strcasecmp(type, "search")) out_str("[____ press S to search ____]", A_DIM);
                else if ((!strcasecmp(type, "submit") || !strcasecmp(type, "button")) && get_attr(attrs, "value", val, sizeof val)) {
                    char b[80];
                    snprintf(b, sizeof b, "[%s]", val);
                    out_str(b, A_DIM);
                }
            }
            continue;
        }
        char c = *p++;
        if (c == '&') {
            char d;
            int n = entity(p, &d);
            if (n) {
                p += n;
                c = d;
            }
        }
        if (in_title) {
            if (tlen < (int)sizeof title - 1 && !(isspace(c) && (!tlen || title[tlen - 1] == ' '))) title[tlen++] = isspace(c) ? ' ' : c;
            title[tlen] = 0;
            continue;
        }
        if (skip) continue;
        out_char(c);
    }
    flush_word();
    if (line_has_text) newline();
    nlines = ln > 0 ? ln : 1;
    line_at(0);
}

static void render_text(const char *t, uint32_t len) {
    ln = col = indent = 0;
    line_has_text = false;
    cur_attr = A_TEXT;
    cur_link = -1;
    pre = true;
    for (uint32_t i = 0; i < len; i++) out_char(t[i]);
    if (line_has_text) newline();
    pre = false;
    nlines = ln > 0 ? ln : 1;
    line_at(0);
}

/* ---------------------------------------------------------------- URLs -- */
static void url_encode(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        if (isalnum(*in) || strchr("-_.~", *in)) out[o++] = *in;
        else if (*in == ' ') out[o++] = '+';
        else {
            snprintf(out + o, 4, "%%%02X", (uint8_t)*in);
            o += 3;
        }
    }
    out[o] = 0;
}

static void via_frogfind(const char *https_url, char *out, size_t n) {
    snprintf(out, n, "http://frogfind.com/read.php?a=%s", https_url);
}

/* resolve href against the current page */
static void resolve_url(const char *base, const char *href, char *out, size_t n) {
    if (!strncasecmp(href, "http://", 7) || !strncasecmp(href, "about:", 6) || !strncasecmp(href, "file:", 5)) {
        strlcpy(out, href, n);
        return;
    }
    if (!strncasecmp(href, "https://", 8)) {
        via_frogfind(href, out, n);
        return;
    }
    if (!strncmp(href, "//", 2)) {
        snprintf(out, n, "http:%s", href);
        return;
    }
    if (!strncasecmp(base, "file:", 5)) {
        /* local pages: resolve within AxisFS */
        char dir[400];
        strlcpy(dir, base + 5, sizeof dir);
        char *s = strrchr(dir, '/');
        if (href[0] == '/') snprintf(out, n, "file:%s", href);
        else {
            if (s) s[1] = 0;
            snprintf(out, n, "file:%s%s", dir, href);
        }
        return;
    }
    const char *hs = strstr(base, "://");
    hs = hs ? hs + 3 : base;
    const char *path = strchr(hs, '/');
    size_t origin_len = path ? (size_t)(path - base) : strlen(base);
    if (href[0] == '/') {
        snprintf(out, n, "%.*s%s", (int)origin_len, base, href);
        return;
    }
    if (href[0] == '?') {
        const char *q = strchr(base, '?');
        size_t bl = q ? (size_t)(q - base) : strlen(base);
        snprintf(out, n, "%.*s%s", (int)bl, base, href);
        return;
    }
    /* relative to the directory of base (ignore its query string) */
    char b[512];
    strlcpy(b, base, sizeof b);
    char *q = strchr(b, '?');
    if (q) *q = 0;
    char *last = strrchr(b, '/');
    if (!last || last < b + origin_len) snprintf(out, n, "%s/%s", b, href);
    else {
        last[1] = 0;
        snprintf(out, n, "%s%s", b, href);
    }
}

/* ------------------------------------------------------------- loading -- */
static const char *home_html =
    "<title>AxisBrowser start page</title>"
    "<h1>AxisBrowser</h1>"
    "<p>A text web browser for AxisOS. Press <b>S</b> to search the web or <b>G</b> to type an address.</p>"
    "<h2>Start here</h2><ul>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search engine that turns any site into simple pages</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - headlines from around the world</li>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first website</a> (CERN, 1991)</li>"
    "<li><a href=\"http://text.npr.org/\">NPR text-only news</a></li>"
    "<li><a href=\"http://wiby.me/\">wiby.me</a> - search engine for simple, classic websites</li>"
    "<li><a href=\"about:bookmarks\">Your bookmarks</a></li>"
    "</ul><h2>Keys</h2><ul>"
    "<li><b>Up/Down PgUp/PgDn</b> scroll, <b>Tab</b> or <b>Right</b> next link, <b>`</b> previous link, <b>Enter</b> open link</li>"
    "<li><b>Backspace</b> or <b>Left</b> back, <b>F</b> forward, <b>R</b> reload, <b>H</b> home</li>"
    "<li><b>G</b> go to address, <b>S</b> search, <b>B</b> bookmark page, <b>D</b> save page text, <b>W</b> download a link, <b>J</b> toggle JavaScript, <b>L</b> link by number, <b>Q</b> quit</li>"
    "</ul><h2>Notes</h2>"
    "<p>AxisOS speaks plain http://. Secure https:// pages are opened through FrogFind automatically, which "
    "shows a simplified, readable version. Local files work too: try <a href=\"file:/usr/share/doc/axis-guide.txt\">the AxisOS guide</a>.</p>";

static char *bookmarks_html(void) {
    char path[300];
    char home[256];
    fs_path_unix(fs_home(), home, sizeof home);
    snprintf(path, sizeof path, "%s/.bookmarks", home);
    int id = fs_resolve(path, FS_ROOT);
    size_t cap = 1024 + (id >= 0 ? fs_nodes[id].size * 2 : 0);
    char *o = kmalloc(cap);
    if (!o) return NULL;
    strcpy(o, "<title>Bookmarks</title><h1>Bookmarks</h1>");
    if (id < 0 || !fs_nodes[id].size) {
        strcat(o, "<p>No bookmarks yet. Press B on any page to add one.</p>");
        return o;
    }
    strcat(o, "<ul>");
    const char *t = (const char *)fs_nodes[id].data, *e = t + fs_nodes[id].size;
    while (t < e) {
        const char *nl = memchr(t, '\n', (size_t)(e - t));
        if (!nl) nl = e;
        const char *sp = memchr(t, ' ', (size_t)(nl - t));
        size_t used = strlen(o);
        if (sp && used + (nl - t) + 40 < cap)
            snprintf(o + used, cap - used, "<li><a href=\"%.*s\">%.*s</a></li>", (int)(sp - t), t, (int)(nl - sp - 1), sp + 1);
        t = nl + 1;
    }
    strcat(o, "</ul><p>Bookmarks are stored in ~/.bookmarks</p>");
    return o;
}

static void draw(void);

static void show_loading(const char *url, uint32_t got, uint32_t total) {
    scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
    if (total) scr_printf(1, 23, theme.accent, "Loading %.50s  %u%%  (Esc cancels)", url, got * 100 / total);
    else if (got) scr_printf(1, 23, theme.accent, "Loading %.50s  %u KB  (Esc cancels)", url, got / 1024);
    else scr_printf(1, 23, theme.accent, "Connecting to %.50s  (Esc cancels)", url);
}
static const char *loading_url;
static void progress_cb(uint32_t got, uint32_t total) {
    static uint32_t last;
    if (ticks - last < 10) return;
    last = ticks;
    show_loading(loading_url, got, total);
}

static void error_page(const char *url, const char *msg) {
    char *h = kmalloc(2048);
    if (!h) return;
    snprintf(h, 2048,
             "<title>Problem loading page</title><h1>Could not open this page</h1><p>%s</p><p>Address: %s</p>"
             "<h2>Things to try</h2><ul><li>Check the address for typos.</li>"
             "<li>AxisOS needs a network card (QEMU's default e1000 works) - see <b>ifconfig</b> in the shell.</li>"
             "<li>Press <b>S</b> to search the web through FrogFind instead.</li></ul>",
             msg, url);
    page_clear();
    render_html(h, (uint32_t)strlen(h));
    kfree(h);
}

/* run the page's <script> blocks and splice their document.write output in */
static bool js_enabled = true;
static char js_alert_msg[256];
static bool js_alert_pending;

static char *preprocess_js(const char *h, uint32_t len, uint32_t *outlen) {
    uint32_t cap = len * 2 + 256, o = 0;
    char *out = kmalloc(cap);
    if (!out) return NULL;
    js_begin();
    const char *p = h, *end = h + len;
    while (p < end) {
        if ((*p == '<') && (p + 7 <= end) && !strncasecmp(p, "<script", 7) &&
            (p[7] == '>' || p[7] == ' ' || p[7] == '\t' || p[7] == '\n')) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            if (!gt) break;
            /* src="..."? */
            char srcurl[256] = "";
            const char *a = p + 7;
            const char *sp = strcasestr(a, "src");
            if (sp && sp < gt) {
                const char *q = sp + 3;
                while (q < gt && (*q == ' ' || *q == '=' )) q++;
                char quote = (*q == '"' || *q == '\'') ? *q : 0;
                if (quote) q++;
                int l = 0;
                while (q < gt && *q != quote && *q != ' ' && *q != '>' && l < 255) srcurl[l++] = *q++;
                srcurl[l] = 0;
            }
            const char *endtag = strcasestr(gt + 1, "</script");
            const char *body = gt + 1;
            uint32_t blen = endtag ? (uint32_t)(endtag - body) : 0;
            uint32_t before = js_output_len();
            if (srcurl[0]) {
                char full[300];
                resolve_url(cur_url, srcurl, full, sizeof full);
                if (!strncasecmp(full, "http://", 7)) {
                    http_resp_t r;
                    if (http_get(full, &r, NULL) == 200) { js_run(r.body, (int)r.len); kfree(r.body); }
                }
            } else if (blen) {
                js_run(body, (int)blen);
            }
            /* splice any document.write output here */
            const char *w = js_output();
            uint32_t after = js_output_len();
            for (uint32_t i = before; i < after; i++) {
                if (o + 1 >= cap) { cap *= 2; char *n = krealloc(out, cap); if (!n) break; out = n; }
                out[o++] = w[i];
            }
            p = endtag ? (strchr(endtag, '>') ? strchr(endtag, '>') + 1 : end) : end;
            continue;
        }
        if (o + 1 >= cap) { cap *= 2; char *n = krealloc(out, cap); if (!n) break; out = n; }
        out[o++] = *p++;
    }
    out[o] = 0;
    *outlen = o;
    if (js_get_alert(js_alert_msg, sizeof js_alert_msg)) js_alert_pending = true;
    return out;
}

/* render an HTML page, running its JavaScript first (when enabled) */
static void render_page(const char *h, uint32_t len) {
    if (!js_enabled || !strstr(h, "<script")) { render_html(h, len); return; }
    uint32_t nl = 0;
    char *expanded = preprocess_js(h, len, &nl);
    if (expanded) { render_html(expanded, nl); kfree(expanded); }
    else render_html(h, len);
}

static bool is_html(const char *ctype, const char *body) {
    if (ctype[0]) return strcasestr(ctype, "html") != NULL;
    return strcasestr(body, "<html") || strcasestr(body, "<body") || strcasestr(body, "<p>") || strcasestr(body, "<a ");
}

static void load(const char *input, bool push) {
    char url[512];
    /* decide what the user typed */
    if (!strncasecmp(input, "https://", 8)) via_frogfind(input, url, sizeof url);
    else if (!strncasecmp(input, "http://", 7) || !strncasecmp(input, "about:", 6) || !strncasecmp(input, "file:", 5))
        strlcpy(url, input, sizeof url);
    else if (input[0] == '/' || input[0] == '~' || input[0] == '\\' || (isalpha(input[0]) && input[1] == ':')) {
        int id = fs_resolve(input, fs_cwd);
        char p[400];
        if (id >= 0) fs_path_unix(id, p, sizeof p);
        else strlcpy(p, input, sizeof p);
        snprintf(url, sizeof url, "file:%s", p);
    } else if (!strchr(input, ' ') && strchr(input, '.'))
        snprintf(url, sizeof url, "http://%s", input);
    else {
        char q[300];
        url_encode(input, q, sizeof q);
        snprintf(url, sizeof url, "http://frogfind.com/?q=%s", q);
    }

    page_clear();
    if (!strcasecmp(url, "about:home") || !strcasecmp(url, "about:blank")) {
        render_html(home_html, (uint32_t)strlen(home_html));
    } else if (!strcasecmp(url, "about:bookmarks")) {
        char *h = bookmarks_html();
        if (h) {
            render_html(h, (uint32_t)strlen(h));
            kfree(h);
        }
    } else if (!strncasecmp(url, "file:", 5)) {
        int id = fs_resolve(url + 5, FS_ROOT);
        if (id < 0 || fs_nodes[id].type != FT_FILE) error_page(url, "That file does not exist on this computer.");
        else {
            fs_node_t *n = &fs_nodes[id];
            const char *nm = n->name;
            size_t l = strlen(nm);
            bool html = (l > 4 && !strcasecmp(nm + l - 4, ".htm")) || (l > 5 && !strcasecmp(nm + l - 5, ".html"));
            if (html) render_page((const char *)n->data, n->size);
            else render_text(n->data ? (const char *)n->data : "", n->size);
            if (!title[0]) strlcpy(title, nm, sizeof title);
        }
    } else {
        http_resp_t r;
        int st = 0;
        loading_url = url;
        show_loading(url, 0, 0);
        for (int hops = 0; hops < 6; hops++) {
            st = http_get(url, &r, progress_cb);
            if (st >= 300 && st < 400 && r.location[0]) {
                char next[512];
                resolve_url(url, r.location, next, sizeof next);
                kfree(r.body);
                strlcpy(url, next, sizeof url);
                continue;
            }
            break;
        }
        if (st < 0) {
            error_page(url, http_strerror(st));
        } else {
            if (is_html(r.ctype, r.body)) render_page(r.body, r.len);
            else render_text(r.body, r.len);
            if (st >= 400 && !title[0]) snprintf(title, sizeof title, "HTTP error %d", st);
            kfree(r.body);
        }
    }
    strlcpy(cur_url, url, sizeof cur_url);
    if (!title[0]) strlcpy(title, url, sizeof title);
    if (push) {
        /* drop forward history */
        for (int i = hist_pos + 1; i < hist_n; i++) kfree(history[i]);
        if (hist_n) hist_n = hist_pos + 1;
        if (hist_n == HIST) {
            kfree(history[0]);
            memmove(history, history + 1, sizeof(char *) * (HIST - 1));
            hist_n--;
        }
        history[hist_n] = kstrdup(url);
        hist_pos = hist_n++;
    }
    /* select the first link on screen */
    sel = -1;
    for (int l = 0; l < nlines && l < VIEW_ROWS && sel < 0; l++)
        for (int c = 0; c < PW; c++)
            if (page[l * PW + c].link >= 0) {
                sel = page[l * PW + c].link;
                break;
            }
}

/* -------------------------------------------------------------- display -- */
static int link_line(int link) {
    for (int l = 0; l < nlines; l++)
        for (int c = 0; c < PW; c++)
            if (page[l * PW + c].link == link) return l;
    return -1;
}

static void draw(void) {
    scr_fill(0, 0, SCREEN_W, 1, ' ', theme.bar);
    scr_printf(1, 0, theme.bar, "AxisBrowser - %.62s", title);
    for (int r = 0; r < VIEW_ROWS; r++) {
        int l = top + r;
        scr_fill(0, VIEW_TOP + r, SCREEN_W, 1, ' ', A_TEXT);
        if (l >= nlines) continue;
        cell_t *c = page + l * PW;
        for (int i = 0; i < PW; i++) {
            uint8_t a = c[i].a;
            if (c[i].link >= 0 && c[i].link == sel) a = theme.select;
            scr_put(1 + i, VIEW_TOP + r, c[i].c, a);
        }
    }
    /* scroll indicator */
    if (nlines > VIEW_ROWS) {
        int pos = top * (VIEW_ROWS - 1) / (nlines - VIEW_ROWS > 0 ? nlines - VIEW_ROWS : 1);
        scr_put(SCREEN_W - 1, VIEW_TOP + (pos < VIEW_ROWS ? pos : VIEW_ROWS - 1), (char)0xDB, A_DIM);
    }
    scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
    if (status[0]) {
        scr_putsn(1, 23, status, 78, theme.accent);
        status[0] = 0;
    } else if (sel >= 0 && sel < nlinks) {
        char full[512];
        resolve_url(cur_url, links[sel], full, sizeof full);
        scr_printf(1, 23, A_DIM, "Link: %.72s", full);
    } else
        scr_printf(1, 23, A_DIM, "%.70s", cur_url);
    scr_fill(0, 24, SCREEN_W, 1, ' ', theme.bar);
    if (adv_mode) scr_printf(1, 24, theme.bar, "%d lines %d links  G S Tab Enter Bksp F R H B D Q", nlines, nlinks);
    else scr_puts(1, 24, "Tab Link  Enter Open  Bksp Back  G Go  S Search  W Download  B Bookmark  Q Quit", theme.bar);
}

static void ensure_visible(int link) {
    int l = link_line(link);
    if (l < 0) return;
    if (l < top) top = l;
    if (l >= top + VIEW_ROWS) top = l - VIEW_ROWS / 2;
    if (top < 0) top = 0;
}

static void save_page(void) {
    char name[FS_NAME_MAX] = "page.txt";
    if (!tui_prompt("Save page", "Save the page text as (in the current folder):", name, sizeof name) || !name[0]) return;
    size_t cap = (size_t)nlines * (PW + 1) + 1;
    char *t = kmalloc(cap);
    if (!t) return;
    size_t o = 0;
    for (int l = 0; l < nlines; l++) {
        int last = PW - 1;
        while (last >= 0 && page[l * PW + last].c == ' ') last--;
        for (int c = 0; c <= last; c++) t[o++] = page[l * PW + c].c;
        t[o++] = '\n';
    }
    int id = fs_mkfile(name, fs_cwd);
    int r = id < 0 ? id : fs_write(id, t, (uint32_t)o);
    kfree(t);
    snprintf(status, sizeof status, r < 0 ? "Could not save: %s" : "Saved to %s", r < 0 ? fs_strerror(r) : name);
}

/* download the current page, or the selected link's target, as a raw file */
static void download(void) {
    char url[512];
    if (sel >= 0 && sel < nlinks) resolve_url(cur_url, links[sel], url, sizeof url);
    else strlcpy(url, cur_url, sizeof url);
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) { snprintf(status, sizeof status, "Can only download http(s) links."); return; }
    /* default filename from the URL */
    char name[FS_NAME_MAX];
    const char *slash = strrchr(url + 7, '/');
    strlcpy(name, slash && slash[1] ? slash + 1 : "download.bin", sizeof name);
    char *q = strchr(name, '?'); if (q) *q = 0;
    if (!name[0] || !fs_valid_name(name)) strcpy(name, "download.bin");
    if (!tui_prompt("Download", "Save the linked file as:", name, sizeof name) || !name[0]) return;
    char full[520];
    if (!strncasecmp(url, "https://", 8)) via_frogfind(url, full, sizeof full);
    else strlcpy(full, url, sizeof full);
    scr_fill(0, 23, SCREEN_W, 1, ' ', theme.normal);
    scr_printf(1, 23, theme.accent, "Downloading %.60s ...", full);
    http_resp_t r;
    loading_url = full;
    int st = http_get(full, &r, progress_cb);
    if (st < 0) { snprintf(status, sizeof status, "Download failed: %s", http_strerror(st)); return; }
    int id = fs_mkfile(name, fs_cwd);
    int w = id < 0 ? id : fs_write(id, r.body, r.len);
    uint32_t n = r.len;
    kfree(r.body);
    if (w < 0) snprintf(status, sizeof status, "Could not save: %s", fs_strerror(w));
    else { snprintf(status, sizeof status, "Downloaded %u bytes to %s (HTTP %d)", n, name, st); if (cfg.autosave) fs_save(); }
}

static void add_bookmark(void) {
    char home[256], path[300];
    fs_path_unix(fs_home(), home, sizeof home);
    snprintf(path, sizeof path, "%s/.bookmarks", home);
    int id = fs_mkfile(path, FS_ROOT);
    if (id < 0) return;
    char line[700];
    snprintf(line, sizeof line, "%s %s\n", cur_url, title);
    fs_append(id, line, (uint32_t)strlen(line));
    fs_nodes[id].attr |= FA_HIDDEN;
    snprintf(status, sizeof status, "Bookmarked: %.60s", title);
}

int browser_run(const char *start) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    A_TEXT = theme.normal;
    A_HEAD = theme.accent;
    A_LINK = theme.path;
    A_BOLD = theme.prompt;
    A_DIM = theme.dim;
    hist_n = hist_pos = 0;
    if (!net_up() && !(start && (start[0] == '/' || start[0] == '~'))) strlcpy(status, "No network: only local files and the start page work.", sizeof status);
    load(start && *start ? start : "about:home", true);
    for (;;) {
        con_show_cursor(false);
        if (js_alert_pending) { js_alert_pending = false; tui_message("JavaScript alert()", js_alert_msg); }
        draw();
        int k = kbd_getkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;
        else if (k == KEY_DOWN) { if (top + VIEW_ROWS < nlines) top++; }
        else if (k == KEY_UP) { if (top) top--; }
        else if (k == KEY_PGDN || k == ' ') {
            top += VIEW_ROWS - 1;
            if (top > nlines - VIEW_ROWS) top = nlines > VIEW_ROWS ? nlines - VIEW_ROWS : 0;
        } else if (k == KEY_PGUP) top = top > VIEW_ROWS - 1 ? top - (VIEW_ROWS - 1) : 0;
        else if (k == KEY_HOME) top = 0;
        else if (k == KEY_END) top = nlines > VIEW_ROWS ? nlines - VIEW_ROWS : 0;
        else if ((k == '\t' || k == KEY_RIGHT || k == '`') && nlinks) {
            bool back = (k == '`');
            sel = sel < 0 ? 0 : (back ? (sel + nlinks - 1) % nlinks : (sel + 1) % nlinks);
            ensure_visible(sel);
        } else if (k == '\n' && sel >= 0 && sel < nlinks) {
            char full[512];
            resolve_url(cur_url, links[sel], full, sizeof full);
            load(full, true);
        } else if (k == '\b' || k == KEY_LEFT) {
            if (hist_pos > 0) {
                hist_pos--;
                char u[512];
                strlcpy(u, history[hist_pos], sizeof u);
                load(u, false);
            } else
                strlcpy(status, "No previous page.", sizeof status);
        } else if (k == 'f' || k == 'F') {
            if (hist_pos + 1 < hist_n) {
                hist_pos++;
                char u[512];
                strlcpy(u, history[hist_pos], sizeof u);
                load(u, false);
            }
        } else if (k == 'r' || k == 'R') {
            char u[512];
            strlcpy(u, cur_url, sizeof u);
            int t = top;
            load(u, false);
            top = t < nlines ? t : 0;
        } else if (k == 'h' || k == 'H') load("about:home", true);
        else if (k == 'g' || k == 'G' || k == 'o' || k == 'O') {
            char u[300] = "";
            if (tui_prompt("Go to", "Address (http://..., https://..., a file path, or words to search):", u, sizeof u) && u[0]) load(u, true);
        } else if (k == 's' || k == 'S' || k == '/') {
            char q[200] = "";
            if (tui_prompt("Search the web", "Search FrogFind for:", q, sizeof q) && q[0]) {
                char enc[300], u[400];
                url_encode(q, enc, sizeof enc);
                snprintf(u, sizeof u, "http://frogfind.com/?q=%s", enc);
                load(u, true);
            }
        } else if (k == 'b' || k == 'B') add_bookmark();
        else if (k == 'd' || k == 'D') save_page();
        else if (k == 'w' || k == 'W') download();
        else if (k == 'l' || k == 'L') {
            char n[8] = "";
            if (tui_prompt("Link", "Link number (1 = first link on the page):", n, sizeof n) && atoi(n) > 0 && atoi(n) <= nlinks) {
                sel = atoi(n) - 1;
                ensure_visible(sel);
            }
        }
    }
    page_clear();
    js_free();
    for (int i = 0; i < hist_n; i++) kfree(history[i]);
    hist_n = 0;
    scr_restore(saved);
    con_show_cursor(true);
    return 0;
}
