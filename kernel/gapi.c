/*
 * AxisOS Google API client.
 *
 * AxisOS cannot speak https (which every Google API needs), so requests go
 * through a small bridge the user runs on their PC (tools/google-bridge.py):
 * AxisOS -> http -> bridge -> https -> Google. This file stores the bridge
 * address and per-service API keys ("service codes"), builds Google API
 * requests and returns the JSON, and offers a tiny JSON field scanner so both
 * native apps (like ClassicTube) and scripts (via the `google` command) can
 * use real Google services.
 *
 * Config files:
 *   /etc/google.conf        one line: bridge host[:port]   (default port 8099)
 *   /etc/google-keys.txt    lines of  service=APIKEY   (e.g. youtube=AIza...)
 */
#include "axis.h"

/* ---- config ------------------------------------------------------------ */
static char bridge_host[96];
static bool cfg_loaded;

static void gapi_load(void) {
    cfg_loaded = true;
    bridge_host[0] = 0;
    int f = fs_resolve("/etc/google.conf", FS_ROOT);
    if (f >= 0 && fs_nodes[f].size) {
        uint32_t n = fs_nodes[f].size;
        if (n > 95) n = 95;
        memcpy(bridge_host, fs_nodes[f].data, n);
        bridge_host[n] = 0;
        char *nl = bridge_host;
        while (*nl && *nl != '\n' && *nl != '\r' && *nl != ' ') nl++;
        *nl = 0;
    }
}

const char *gapi_bridge(void) {
    if (!cfg_loaded) gapi_load();
    return bridge_host;
}
bool gapi_configured(void) { return gapi_bridge()[0] != 0; }

void gapi_set_bridge(const char *host) {
    strlcpy(bridge_host, host, sizeof bridge_host);
    int f = fs_mkfile("/etc/google.conf", FS_ROOT);
    if (f >= 0) { fs_write(f, bridge_host, (uint32_t)strlen(bridge_host)); if (cfg.autosave) fs_save(); }
}

bool gapi_key(const char *service, char *out, size_t n) {
    out[0] = 0;
    int f = fs_resolve("/etc/google-keys.txt", FS_ROOT);
    if (f < 0 || !fs_nodes[f].data) return false;
    const char *t = (const char *)fs_nodes[f].data, *e = t + fs_nodes[f].size;
    size_t sl = strlen(service);
    while (t < e) {
        const char *nl = memchr(t, '\n', (size_t)(e - t));
        if (!nl) nl = e;
        if ((size_t)(nl - t) > sl && !strncasecmp(t, service, sl) && t[sl] == '=') {
            const char *v = t + sl + 1;
            size_t l = (size_t)(nl - v);
            while (l && (v[l - 1] == '\r' || v[l - 1] == ' ')) l--;
            if (l >= n) l = n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return out[0] != 0;
        }
        t = nl + 1;
    }
    return false;
}

void gapi_set_key(const char *service, const char *key) {
    int f = fs_resolve("/etc/google-keys.txt", FS_ROOT);
    char *buf = kmalloc(4096);
    if (!buf) return;
    buf[0] = 0;
    uint32_t len = 0;
    /* copy existing lines except the one for this service */
    if (f >= 0 && fs_nodes[f].data) {
        const char *t = (const char *)fs_nodes[f].data, *e = t + fs_nodes[f].size;
        size_t sl = strlen(service);
        while (t < e) {
            const char *nl = memchr(t, '\n', (size_t)(e - t));
            const char *next = nl ? nl + 1 : e;
            size_t ll = (nl ? nl : e) - t;
            bool same = ll > sl && !strncasecmp(t, service, sl) && t[sl] == '=';
            if (!same && ll && len + ll + 1 < 4000) { memcpy(buf + len, t, ll); len += ll; buf[len++] = '\n'; }
            t = next;
        }
    }
    len += snprintf(buf + len, 4096 - len, "%s=%s\n", service, key);
    f = fs_mkfile("/etc/google-keys.txt", FS_ROOT);
    if (f >= 0) { fs_write(f, buf, len); fs_nodes[f].attr |= FA_HIDDEN; if (cfg.autosave) fs_save(); }
    kfree(buf);
}

/* ---- url encode -------------------------------------------------------- */
static void urlenc(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr("-_.~", c)) out[o++] = c;
        else { snprintf(out + o, 4, "%%%02X", c); o += 3; }
    }
    out[o] = 0;
}

/* GET a full https://...googleapis... URL through the bridge. 0=ok */
int gapi_fetch(const char *google_url, http_resp_t *r) {
    if (!gapi_configured()) return -100;
    char enc[1024];
    urlenc(google_url, enc, sizeof enc);
    char url[1200];
    const char *b = gapi_bridge();
    if (strchr(b, ':')) snprintf(url, sizeof url, "http://%s/g?url=%s", b, enc);
    else snprintf(url, sizeof url, "http://%s:8099/g?url=%s", b, enc);
    int st = http_get(url, r, NULL);
    if (st < 0) return st;
    return st == 200 ? 0 : st;
}

/* convenience: call a YouTube Data API v3 method, adding the key */
int gapi_youtube(const char *method, const char *params, http_resp_t *r) {
    char key[64];
    if (!gapi_key("youtube", key, sizeof key)) return -101;
    char url[900];
    snprintf(url, sizeof url, "https://youtube.googleapis.com/youtube/v3/%s?%s&key=%s", method, params, key);
    return gapi_fetch(url, r);
}

/* fetch a thumbnail as w*h colour indices through the bridge; 0=ok */
int gapi_thumb(const char *video_id, int w, int h, uint8_t *out) {
    if (!gapi_configured()) return -100;
    char url[256];
    const char *b = gapi_bridge();
    char hostport[110];
    if (strchr(b, ':')) strlcpy(hostport, b, sizeof hostport);
    else snprintf(hostport, sizeof hostport, "%s:8099", b);
    snprintf(url, sizeof url, "http://%s/thumb?id=%s&w=%d&h=%d", hostport, video_id, w, h);
    http_resp_t r;
    int st = http_get(url, &r, NULL);
    if (st != 200) { if (st >= 0) kfree(r.body); return st < 0 ? st : st; }
    uint32_t need = (uint32_t)(w * h);
    if (r.len < need) { kfree(r.body); return -1; }
    memcpy(out, r.body, need);
    kfree(r.body);
    return 0;
}

/* ---- minimal JSON field scanner ---------------------------------------- */
/* find the next "key":"string" or "key":number starting at *pos; returns the
   value into out and advances *pos past it. Good enough for Google API JSON. */
const char *json_find(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return p;
}
/* copy a JSON string value (p points at the opening quote) with \uXXXX and
   basic escapes decoded to ASCII; returns pointer past the closing quote */
const char *json_str(const char *p, char *out, size_t n) {
    out[0] = 0;
    if (*p != '"') return p;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            if (e == 'n') c = ' ';
            else if (e == 't') c = ' ';
            else if (e == 'u') {
                int v = 0;
                for (int i = 0; i < 4 && isxdigit((uint8_t)*p); i++) { v = v * 16 + (isdigit((uint8_t)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); p++; }
                c = (v >= 32 && v < 127) ? (char)v : (v == 0x2019 || v == 0x2018 ? '\'' : v == 0x201c || v == 0x201d ? '"' : v == 0x2014 || v == 0x2013 ? '-' : '?');
            } else c = e;
        }
        if (o + 1 < n) out[o++] = c;
    }
    out[o] = 0;
    return *p == '"' ? p + 1 : p;
}

/* search the next item for key within [p, end): returns value string */
bool json_field(const char **p, const char *end, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = *p;
    while (q < end) {
        const char *f = strstr(q, pat);
        if (!f || f >= end) return false;
        f += strlen(pat);
        while (*f == ' ' || *f == ':' || *f == '\t') f++;
        if (*f == '"') { json_str(f, out, n); }
        else { size_t o = 0; while (*f && *f != ',' && *f != '}' && *f != ' ' && o + 1 < n) out[o++] = *f++; out[o] = 0; }
        *p = f;
        return true;
    }
    return false;
}

/* ---- the `google` command ---------------------------------------------- */
int google_cmd(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Google API client - use real Google services through a PC bridge.\n\n");
        kprintf("  google bridge <host[:port]>   set the PC running google-bridge.py\n");
        kprintf("  google key <service> <KEY>    store an API key (e.g. youtube)\n");
        kprintf("  google get <google-api-url>   fetch a URL (prints JSON)\n");
        kprintf("  google yt <method> <params>   call YouTube Data API v3\n");
        kprintf("  google status                 show current configuration\n\n");
        kprintf("Setup: run tools/google-bridge.py on your PC, get a key at\n");
        kprintf("console.cloud.google.com (enable YouTube Data API v3), then:\n");
        kprintf("  google bridge 192.168.1.20:8099\n  google key youtube AIza...\n  classictube\n");
        return 0;
    }
    const char *sub = argv[1];
    if (!strcasecmp(sub, "bridge")) {
        if (argc < 3) { kprintf("Bridge: %s\n", gapi_configured() ? gapi_bridge() : "(not set)"); return 0; }
        gapi_set_bridge(argv[2]);
        kprintf("Bridge set to %s\n", argv[2]);
        return 0;
    }
    if (!strcasecmp(sub, "key")) {
        if (argc < 4) { kprintf("usage: google key <service> <KEY>\n"); return 1; }
        gapi_set_key(argv[2], argv[3]);
        kprintf("Stored %s key (%d chars). Kept in /etc/google-keys.txt\n", argv[2], (int)strlen(argv[3]));
        return 0;
    }
    if (!strcasecmp(sub, "status")) {
        char key[64];
        kprintf("Bridge:  %s\n", gapi_configured() ? gapi_bridge() : "(not set - run 'google bridge <host>')");
        kprintf("YouTube: %s\n", gapi_key("youtube", key, sizeof key) ? "key stored" : "no key ('google key youtube <KEY>')");
        kprintf("Network: %s\n", net_up() ? "up" : "down");
        return 0;
    }
    if (!strcasecmp(sub, "get") || !strcasecmp(sub, "yt")) {
        if (!gapi_configured()) { kprintf("No bridge set. Run: google bridge <host:port>\n"); return 1; }
        http_resp_t r;
        int st;
        if (!strcasecmp(sub, "yt")) {
            if (argc < 3) { kprintf("usage: google yt <method> <params>\n"); return 1; }
            st = gapi_youtube(argv[2], argc > 3 ? argv[3] : "", &r);
        } else {
            if (argc < 3) { kprintf("usage: google get <url>\n"); return 1; }
            st = gapi_fetch(argv[2], &r);
        }
        if (st == -101) { kprintf("No YouTube key. Run: google key youtube <KEY>\n"); return 1; }
        if (st < 0) { kprintf("Request failed (%d). Is the bridge running and reachable?\n", st); return 1; }
        if (st > 0) { kprintf("HTTP %d\n", st); }
        pager_show(r.body, r.len);
        kfree(r.body);
        return 0;
    }
    kprintf("Unknown subcommand '%s'. Try 'google' for help.\n", sub);
    return 1;
}
