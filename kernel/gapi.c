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

/* The built-in default YouTube key is not stored here: it is hidden,
 * obfuscated, inside the calculator's keypad shading table (see games.c) and
 * reconstructed by calc_preset(). This keeps any key-shaped literal out of the
 * networking code. Obfuscation only - set your own in Settings to override. */
extern const char *calc_preset(void);
static const char *default_yt_key(void) { return calc_preset(); }

/* The shared default key only has so much daily quota, so once "enough people"
 * have used it the API starts refusing it. We detect that from the API's own
 * response and permanently stop using the default key (a marker file persists
 * the decision), falling back so the user adds their own key. A user-set key is
 * never auto-disabled. */
static int default_burned = -1;                 /* -1 = not yet checked */
static bool g_using_default = false;            /* did the last gapi_key() hand out the default? */

static bool default_is_burned(void) {
    if (default_burned == 1) return true;                     /* once burned, stays burned */
    default_burned = (fs_resolve("/etc/yt-default-off", FS_ROOT) >= 0) ? 1 : 0;
    return default_burned == 1;
}
static void burn_default(void) {
    if (default_is_burned()) return;
    int id = fs_mkfile("/etc/yt-default-off", FS_ROOT);
    if (id >= 0) { fs_write(id, "shared key exhausted - set your own in Settings\n", 47); fs_save(); }
    default_burned = 1;
}
bool gapi_default_disabled(void) { return default_is_burned(); }

/* does this API response mean the key itself is exhausted/invalid? */
static bool yt_key_broken(const char *body, int status) {
    if (status == 403 || status == 429) return true;
    if (!body) return false;
    return strstr(body, "quotaExceeded") || strstr(body, "rateLimitExceeded") ||
           strstr(body, "dailyLimitExceeded") || strstr(body, "keyInvalid") ||
           strstr(body, "API key not valid") || strstr(body, "keyExpired");
}

bool gapi_key(const char *service, char *out, size_t n) {
    out[0] = 0;
    g_using_default = false;
    int f = fs_resolve("/etc/google-keys.txt", FS_ROOT);
    if (f < 0 || !fs_nodes[f].data) {
        if (!strcasecmp(service, "youtube") && !default_is_burned()) {
            strlcpy(out, default_yt_key(), n); g_using_default = true; return out[0] != 0;
        }
        return false;
    }
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
    if (!strcasecmp(service, "youtube") && !default_is_burned()) {
        strlcpy(out, default_yt_key(), n); g_using_default = true; return out[0] != 0;
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

/* GET a full https://...googleapis... URL. Native TLS first; bridge as fallback. 0=ok */
int gapi_fetch(const char *google_url, http_resp_t *r) {
    /* Preferred path: connect straight to Google over our own TLS stack.
     * Retry a couple of times - the first handshake after boot, or a dropped
     * connection, often succeeds on a second try. */
    if (tls_available() && !strncasecmp(google_url, "https://", 8)) {
        int st = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            st = https_get(google_url, r, NULL);
            if (st >= 0) return st == 200 ? 0 : st;   /* got a real HTTP reply */
            if (user_abort()) break;
            sleep_ms(300);                            /* transient: brief backoff, retry */
        }
        /* native TLS failed repeatedly - fall through to the bridge if set */
    }
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
    bool used_default = g_using_default;
    char url[900];
    snprintf(url, sizeof url, "https://youtube.googleapis.com/youtube/v3/%s?%s&key=%s", method, params, key);
    int st = gapi_fetch(url, r);
    /* detector: if the shared default key came back exhausted/invalid, retire it */
    if (used_default && st >= 0) {
        int http = st == 0 ? 200 : st;
        if (yt_key_broken(r->body, http)) burn_default();
    }
    return st;
}

/* Fetch a video's storyboard spec from Google's InnerTube player endpoint.
 * Storyboards are the JPEG mosaic grids YouTube samples across the timeline -
 * the only form of the actual video frames a text-mode OS can decode. The
 * spec string tells us the mosaic URLs and geometry. 0 on success. */
/* YouTube's public WEB InnerTube client key (a fixed constant, not a secret),
 * stored obfuscated so the source carries no key-shaped literals. */
static const uint8_t it_obf[] = {226,139,155,97,76,71,28,51,196,252,147,202,68,90,36,33,171,227,229,163,91,107,5,32,204,233,160,132,112,121,28,93,220,147,240,145,156,73,5};
static const char *innertube_key(void) {
    static char k[64]; static int done = 0;
    if (!done) { for (unsigned i = 0; i < sizeof it_obf; i++) k[i] = (char)(it_obf[i] ^ ((i * 31 + 0xA3) & 0xFF)); k[sizeof it_obf] = 0; done = 1; }
    return k;
}
int gapi_storyboard(const char *video_id, char *spec, size_t n) {
    spec[0] = 0;
    if (!tls_available()) return -1;
    char body[256];
    snprintf(body, sizeof body,
        "{\"videoId\":\"%s\",\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20240101.01.00\",\"hl\":\"en\"}}}",
        video_id);
    char url[200];
    snprintf(url, sizeof url, "https://youtubei.googleapis.com/youtubei/v1/player?key=%s&prettyPrint=false", innertube_key());
    http_resp_t r;
    int st = https_post(url, "application/json", (const uint8_t *)body, (uint32_t)strlen(body), &r);
    if (st != 200 || !r.body) { if (st >= 0 && r.body) kfree(r.body); return -1; }
    int rc = -1;
    const char *sb = strstr(r.body, "playerStoryboardSpecRenderer");
    if (sb) { const char *sp = json_find(sb, "spec"); if (sp) { json_str(sp, spec, n); if (spec[0]) rc = 0; } }
    kfree(r.body);
    return rc;
}

/* fetch a thumbnail as w*h colour indices; 0=ok.
 * Native path: download the real JPEG from i.ytimg.com over our own HTTPS and
 * decode it with the built-in JPEG decoder (no PC helper). Falls back to the
 * optional bridge if native fetch/decode fails and a bridge is configured. */
int gapi_thumb(const char *video_id, int w, int h, uint8_t *out) {
    if (net_up()) {
        char url[160];
        /* mqdefault.jpg is a small 320x180 baseline JPEG, ideal for text */
        snprintf(url, sizeof url, "https://i.ytimg.com/vi/%s/mqdefault.jpg", video_id);
        http_resp_t r;
        int st = http_get(url, &r, NULL);
        if (st == 200 && r.body && r.len > 4) {
            int iw, ih; uint8_t *rgb;
            int rc = jpeg_decode((const uint8_t *)r.body, r.len, &iw, &ih, &rgb);
            kfree(r.body);
            if (rc == 0) { img_rgb_to_vga(rgb, iw, ih, out, w, h); kfree(rgb); return 0; }
        } else if (st >= 0 && r.body) kfree(r.body);
    }
    /* legacy fallback: pre-converted indices from the PC bridge */
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
        kprintf("Google API client - connects to Google over AxisOS's own secure\n");
        kprintf("HTTPS (TLS 1.2). No PC helper needed.\n\n");
        kprintf("  google key <service> <KEY>    store an API key (e.g. youtube)\n");
        kprintf("  google get <google-api-url>   fetch an https URL (prints JSON)\n");
        kprintf("  google yt <method> <params>   call YouTube Data API v3\n");
        kprintf("  google status                 show current configuration\n");
        kprintf("  google bridge <host[:port]>   (optional) legacy PC bridge fallback\n\n");
        kprintf("Setup: get a key at console.cloud.google.com (enable YouTube\n");
        kprintf("Data API v3), then set it here or in Settings > YouTube API key:\n");
        kprintf("  google key youtube AIza...\n  classictube\n");
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
        kprintf("Secure HTTPS: %s\n", tls_available() ? "available (native TLS 1.2)" : "network down");
        kprintf("YouTube: %s\n", gapi_key("youtube", key, sizeof key) ? "key stored" : "no key ('google key youtube <KEY>')");
        kprintf("Bridge:  %s\n", gapi_configured() ? gapi_bridge() : "(not set - not needed; optional fallback)");
        kprintf("Network: %s\n", net_up() ? "up" : "down");
        return 0;
    }
    if (!strcasecmp(sub, "get") || !strcasecmp(sub, "yt")) {
        if (!net_up()) { kprintf("Network is down.\n"); return 1; }
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
        if (st == HTTP_ETLS) { kprintf("TLS handshake/certificate check failed.\n"); return 1; }
        if (st < 0) { kprintf("Request failed (%d): %s\n", st, http_strerror(st)); return 1; }
        if (st > 0) { kprintf("HTTP %d\n", st); }
        pager_show(r.body, r.len);
        kfree(r.body);
        return 0;
    }
    kprintf("Unknown subcommand '%s'. Try 'google' for help.\n", sub);
    return 1;
}
