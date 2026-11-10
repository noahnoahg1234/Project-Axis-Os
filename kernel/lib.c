/* AxisOS - freestanding C library subset */
#include "axis.h"

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a == b || n == 0) return d;
    if (a < b) {
        while (n--) *a++ = *b++;
    } else {
        a += n;
        b += n;
        while (n--) *--a = *--b;
    }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++)
        if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isxdigit(int c) { return isdigit(c) || (tolower(c) >= 'a' && tolower(c) <= 'f'); }

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) a++, b++;
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (tolower(*a) != tolower(*b)) return tolower((uint8_t)*a) - tolower((uint8_t)*b);
        if (!*a) return 0;
    }
    return 0;
}
char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
void strlcpy(char *d, const char *s, size_t n) {
    if (!n) return;
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}
char *strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++)
        if (!strncasecmp(h, n, l)) return (char *)h;
    return NULL;
}
char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++)
        if (!strncmp(h, n, l)) return (char *)h;
    return NULL;
}
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = s;
    for (; n; n--, p++)
        if (*p == (uint8_t)c) return (void *)p;
    return NULL;
}
char *kstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
void str_lower(char *s) {
    for (; *s; s++) *s = tolower(*s);
}

long strtol(const char *s, const char **end, int base) {
    long v = 0;
    int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') neg = 1, s++;
    else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    }
    if (base == 0) base = 10;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (isalpha(*s)) d = tolower(*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = s;
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* ---- printf ---------------------------------------------------------- */
typedef struct { char *buf; size_t n, pos; } outbuf_t;
static void ob_put(outbuf_t *o, char c) {
    if (o->pos + 1 < o->n) o->buf[o->pos] = c;
    o->pos++;
}

static void ob_num(outbuf_t *o, uint32_t v, int base, int width, bool zero, bool left, bool neg, bool upper) {
    char tmp[16];
    int i = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[i++] = dig[v % base];
        v /= base;
    } while (v);
    int len = i + (neg ? 1 : 0);
    if (!left && !zero)
        for (; width > len; width--) ob_put(o, ' ');
    if (neg) ob_put(o, '-');
    if (!left && zero)
        for (; width > len; width--) ob_put(o, '0');
    while (i) ob_put(o, tmp[--i]);
    if (left)
        for (; width > len; width--) ob_put(o, ' ');
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    outbuf_t o = {buf, n, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            ob_put(&o, *fmt);
            continue;
        }
        fmt++;
        bool left = false, zero = false;
        int width = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) left = true, width = -width;
            fmt++;
        } else
            while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else
                while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = va_arg(ap, int);
            ob_num(&o, v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v, 10, width, zero, left, v < 0, false);
            break;
        }
        case 'u': ob_num(&o, va_arg(ap, uint32_t), 10, width, zero, left, false, false); break;
        case 'x': ob_num(&o, va_arg(ap, uint32_t), 16, width, zero, left, false, false); break;
        case 'X': ob_num(&o, va_arg(ap, uint32_t), 16, width, zero, left, false, true); break;
        case 'p':
            ob_put(&o, '0');
            ob_put(&o, 'x');
            ob_num(&o, va_arg(ap, uint32_t), 16, 8, true, false, false, false);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) for (; width > 1; width--) ob_put(&o, ' ');
            ob_put(&o, c);
            if (left) for (; width > 1; width--) ob_put(&o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            if (!left) for (; width > len; width--) ob_put(&o, ' ');
            for (int i = 0; i < len; i++) ob_put(&o, s[i]);
            if (left) for (; width > len; width--) ob_put(&o, ' ');
            break;
        }
        case '%': ob_put(&o, '%'); break;
        case 0: fmt--; break;
        default:
            ob_put(&o, '%');
            ob_put(&o, *fmt);
        }
    }
    if (n) o.buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* case-insensitive glob with '*' and '?' */
bool wildcard_match(const char *p, const char *s) {
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '?' || (*p != '*' && tolower(*p) == tolower(*s))) {
            p++, s++;
        } else if (*p == '*') {
            star = p++;
            ss = s;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else
            return false;
    }
    while (*p == '*') p++;
    return *p == 0;
}

void format_size(uint32_t b, char *out, size_t n) {
    if (b < 1024) snprintf(out, n, "%uB", b);
    else if (b < 1024 * 1024) snprintf(out, n, "%u.%uK", b / 1024, (b % 1024) * 10 / 1024);
    else snprintf(out, n, "%u.%uM", b / (1024 * 1024), (b % (1024 * 1024)) * 10 / (1024 * 1024));
}

/* split on spaces (no quotes); like strtok(s, " ") */
char *strtok_simple(char *s) {
    static char *next;
    if (s) next = s;
    if (!next) return NULL;
    while (*next == ' ' || *next == '\t') next++;
    if (!*next) return next = NULL;
    char *start = next;
    while (*next && *next != ' ' && *next != '\t') next++;
    if (*next) *next++ = 0;
    else next = NULL;
    return start;
}
