/*
 * MiniJS - a tiny JavaScript interpreter for the AxisBrowser.
 *
 * Runs a practical subset of JavaScript: numbers (integer), strings, booleans,
 * var/let/const, + - * / %, comparisons, && || !, ?:, if/else, while, for,
 * functions (with recursion) and return. Built-ins: document.write/writeln,
 * console.log, alert, parseInt, String, Number, Math.* and string .length.
 *
 * It is NOT a full engine - no floats, objects, arrays-of-note, closures over
 * the DOM, timers or events - so modern web apps won't run, but classic inline
 * scripts (document.write pages, calculators, loops) do.
 */
#include "axis.h"

static bool js_err;
/* ---- arena ------------------------------------------------------------- */
#define JS_ARENA (1024u * 1024u)   /* fixed, never moves: pointers stay valid */
static uint8_t *arena;
static uint32_t arena_len, arena_cap;
static uint8_t js_dummy[16];
static void *amalloc(uint32_t n) {
    n = (n + 7) & ~7u;
    if (!arena) { arena = kmalloc(JS_ARENA); arena_cap = arena ? JS_ARENA : 0; arena_len = 0; }
    if (!arena || arena_len + n > arena_cap) { js_err = true; return js_dummy; }
    void *p = arena + arena_len;
    arena_len += n;
    return p;
}
static char *astrdup(const char *s, int len) {
    char *d = amalloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}

/* ---- output buffers ---------------------------------------------------- */
static char *js_html;
static uint32_t js_html_len, js_html_cap;
static char js_alert[256];
static bool js_had_alert;
static char js_errmsg[96];

static void out_str(const char *s) {
    uint32_t l = strlen(s);
    if (js_html_len + l + 1 > js_html_cap) {
        uint32_t nc = js_html_cap ? js_html_cap * 2 : 4096;
        while (nc < js_html_len + l + 1) nc *= 2;
        char *n = krealloc(js_html, nc);
        if (!n) return;
        js_html = n;
        js_html_cap = nc;
    }
    memcpy(js_html + js_html_len, s, l);
    js_html_len += l;
    js_html[js_html_len] = 0;
}

/* ---- tokens ------------------------------------------------------------ */
enum { T_EOF, T_NUM, T_STR, T_ID, T_PUNCT };
typedef struct { uint8_t t; long num; char *s; } tok_t;
static tok_t *toks;
static int ntok, tcap, tp;

static void push_tok(uint8_t t, long num, const char *s, int len) {
    if (ntok >= tcap) {
        tcap = tcap ? tcap * 2 : 256;
        toks = krealloc(toks, sizeof(tok_t) * tcap);
    }
    toks[ntok].t = t;
    toks[ntok].num = num;
    toks[ntok].s = s ? astrdup(s, len) : NULL;
    ntok++;
}

static void lex(const char *p, const char *end) {
    while (p < end && !js_err) {
        if (isspace((uint8_t)*p)) { p++; continue; }
        if (p[0] == '/' && p + 1 < end && p[1] == '/') { while (p < end && *p != '\n') p++; continue; }
        if (p[0] == '/' && p + 1 < end && p[1] == '*') { p += 2; while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++; p += 2; continue; }
        if (isdigit((uint8_t)*p)) {
            long v = 0;
            if (p[0] == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) { p += 2; while (p < end && isxdigit((uint8_t)*p)) { v = v * 16 + (isdigit((uint8_t)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); p++; } }
            else { while (p < end && isdigit((uint8_t)*p)) v = v * 10 + (*p++ - '0'); if (p < end && *p == '.') { p++; while (p < end && isdigit((uint8_t)*p)) p++; } }
            push_tok(T_NUM, v, NULL, 0);
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            char buf[1024]; int l = 0;
            while (p < end && *p != q && l < 1023) {
                char c = *p++;
                if (c == '\\' && p < end) { char e = *p++; c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e; }
                buf[l++] = c;
            }
            if (p < end) p++;
            push_tok(T_STR, 0, buf, l);
            continue;
        }
        if (isalpha((uint8_t)*p) || *p == '_' || *p == '$') {
            const char *s = p;
            while (p < end && (isalnum((uint8_t)*p) || *p == '_' || *p == '$')) p++;
            push_tok(T_ID, 0, s, (int)(p - s));
            continue;
        }
        /* punctuation: two-char ops first */
        static const char *two[] = {"==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "=>", NULL};
        static const char *three[] = {"===", "!==", NULL};
        bool got = false;
        for (int i = 0; three[i]; i++) if (p + 3 <= end && !strncmp(p, three[i], 3)) { push_tok(T_PUNCT, 0, three[i], 3); p += 3; got = true; break; }
        if (got) continue;
        for (int i = 0; two[i]; i++) if (p + 2 <= end && !strncmp(p, two[i], 2)) { push_tok(T_PUNCT, 0, two[i], 2); p += 2; got = true; break; }
        if (got) continue;
        char o[2] = {*p, 0};
        push_tok(T_PUNCT, 0, o, 1);
        p++;
    }
    push_tok(T_EOF, 0, NULL, 0);
}

/* ---- AST --------------------------------------------------------------- */
enum { N_NUM, N_STR, N_ID, N_BOOL, N_NULL, N_UNDEF, N_BIN, N_UN, N_ASSIGN, N_CALL,
       N_MEMBER, N_INDEX, N_COND, N_IF, N_WHILE, N_FOR, N_BLOCK, N_VAR, N_FUNC,
       N_RETURN, N_EXPR, N_BREAK, N_CONT, N_FUNCEXPR };
typedef struct node {
    uint8_t k;
    long num;
    char *str;        /* id name / op / string literal */
    struct node *a, *b, *c, *d;
    struct node **list; int n;
} node;
static node *mknode(uint8_t k) {
    node *n = amalloc(sizeof(node));
    if (!n) { js_err = true; return NULL; }
    memset(n, 0, sizeof *n);
    n->k = k;
    return n;
}

static tok_t *cur(void) { return &toks[tp]; }
static bool is_p(const char *s) { return cur()->t == T_PUNCT && !strcmp(cur()->s, s); }
static bool is_kw(const char *s) { return cur()->t == T_ID && !strcmp(cur()->s, s); }
static void adv(void) { if (tp < ntok - 1) tp++; }
static bool eat_p(const char *s) { if (is_p(s)) { adv(); return true; } return false; }
static bool eat_kw(const char *s) { if (is_kw(s)) { adv(); return true; } return false; }
static void err(const char *m) { if (!js_err) { js_err = true; strlcpy(js_errmsg, m, sizeof js_errmsg); } }

static node *parse_expr(void);
static node *parse_postfix(void);
static node *parse_stmt(void);

static node *parse_primary(void) {
    tok_t *t = cur();
    if (t->t == T_NUM) { node *n = mknode(N_NUM); n->num = t->num; adv(); return n; }
    if (t->t == T_STR) { node *n = mknode(N_STR); n->str = t->s; adv(); return n; }
    if (t->t == T_ID) {
        if (!strcmp(t->s, "true")) { adv(); node *n = mknode(N_BOOL); n->num = 1; return n; }
        if (!strcmp(t->s, "false")) { adv(); node *n = mknode(N_BOOL); n->num = 0; return n; }
        if (!strcmp(t->s, "null")) { adv(); return mknode(N_NULL); }
        if (!strcmp(t->s, "undefined")) { adv(); return mknode(N_UNDEF); }
        if (!strcmp(t->s, "function")) {
            adv();
            node *f = mknode(N_FUNCEXPR);
            if (cur()->t == T_ID) adv(); /* optional name */
            eat_p("(");
            node *params[16]; int np = 0;
            while (!is_p(")") && cur()->t != T_EOF) { if (cur()->t == T_ID && np < 16) { node *p = mknode(N_ID); p->str = cur()->s; params[np++] = p; adv(); } eat_p(","); }
            eat_p(")");
            f->list = amalloc(sizeof(node *) * (np + 1));
            for (int i = 0; i < np; i++) f->list[i] = params[i];
            f->n = np;
            f->a = parse_stmt(); /* body block */
            return f;
        }
        node *n = mknode(N_ID); n->str = t->s; adv(); return n;
    }
    if (eat_p("(")) { node *n = parse_expr(); eat_p(")"); return n; }
    if (eat_p("!")) { node *n = mknode(N_UN); n->str = "!"; n->a = parse_primary(); return n; }
    if (eat_p("-")) { node *n = mknode(N_UN); n->str = "-"; n->a = parse_primary(); return n; }
    if (eat_p("+")) { return parse_primary(); }
    if (is_p("++") || is_p("--")) { node *a = mknode(N_ASSIGN); a->str = is_p("++") ? "+=" : "-="; adv(); a->a = parse_postfix(); a->b = mknode(N_NUM); a->b->num = 1; return a; }
    err("unexpected token");
    adv();
    return mknode(N_UNDEF);
}

static node *parse_postfix(void) {
    node *n = parse_primary();
    for (;;) {
        if (eat_p(".")) {
            node *m = mknode(N_MEMBER);
            m->a = n;
            m->str = cur()->s ? cur()->s : (char *)"";
            adv();
            n = m;
        } else if (is_p("(")) {
            adv();
            node *c = mknode(N_CALL);
            c->a = n;
            node *args[16]; int na = 0;
            while (!is_p(")") && cur()->t != T_EOF) { if (na < 16) args[na++] = parse_expr(); else parse_expr(); if (!eat_p(",")) break; }
            eat_p(")");
            c->list = amalloc(sizeof(node *) * (na + 1));
            for (int i = 0; i < na; i++) c->list[i] = args[i];
            c->n = na;
            n = c;
        } else if (eat_p("[")) {
            node *ix = mknode(N_INDEX);
            ix->a = n;
            ix->b = parse_expr();
            eat_p("]");
            n = ix;
        } else break;
    }
    /* postfix ++ / -- : treat as  x += 1  /  x -= 1 */
    if (is_p("++") || is_p("--")) {
        node *a = mknode(N_ASSIGN);
        a->str = is_p("++") ? "+=" : "-=";
        adv();
        a->a = n;
        a->b = mknode(N_NUM);
        a->b->num = 1;
        return a;
    }
    return n;
}

static int prec(const char *op) {
    if (!strcmp(op, "*") || !strcmp(op, "/") || !strcmp(op, "%")) return 7;
    if (!strcmp(op, "+") || !strcmp(op, "-")) return 6;
    if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) return 5;
    if (!strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "===") || !strcmp(op, "!==")) return 4;
    if (!strcmp(op, "&&")) return 3;
    if (!strcmp(op, "||")) return 2;
    return -1;
}
static node *parse_binop(int minp) {
    node *left = parse_postfix();
    for (;;) {
        if (cur()->t != T_PUNCT) break;
        int p = prec(cur()->s);
        if (p < minp || p < 0) break;
        char *op = cur()->s;
        adv();
        node *right = parse_binop(p + 1);
        node *b = mknode(N_BIN);
        b->str = op; b->a = left; b->b = right;
        left = b;
    }
    return left;
}
static node *parse_assign(void) {
    node *l = parse_binop(1);
    if (is_p("?")) {
        adv();
        node *c = mknode(N_COND);
        c->a = l; c->b = parse_assign();
        eat_p(":");
        c->c = parse_assign();
        return c;
    }
    if (cur()->t == T_PUNCT && (is_p("=") || is_p("+=") || is_p("-=") || is_p("*=") || is_p("/="))) {
        char *op = cur()->s;
        adv();
        node *a = mknode(N_ASSIGN);
        a->str = op; a->a = l; a->b = parse_assign();
        return a;
    }
    return l;
}
static node *parse_expr(void) { return parse_assign(); }

static node *parse_block(void) {
    node *b = mknode(N_BLOCK);
    node *st[128]; int n = 0;
    eat_p("{");
    while (!is_p("}") && cur()->t != T_EOF && !js_err) { if (n < 128) st[n++] = parse_stmt(); else parse_stmt(); }
    eat_p("}");
    b->list = amalloc(sizeof(node *) * (n + 1));
    for (int i = 0; i < n; i++) b->list[i] = st[i];
    b->n = n;
    return b;
}
static node *parse_stmt(void) {
    if (is_p("{")) return parse_block();
    if (eat_p(";")) return mknode(N_BLOCK);
    if (is_kw("var") || is_kw("let") || is_kw("const")) {
        adv();
        node *v = mknode(N_VAR);
        node *decls[16]; int n = 0;
        do {
            node *d = mknode(N_ASSIGN);
            d->str = "="; d->a = mknode(N_ID); d->a->str = cur()->s; adv();
            if (eat_p("=")) d->b = parse_assign();
            if (n < 16) decls[n++] = d;
        } while (eat_p(","));
        eat_p(";");
        v->list = amalloc(sizeof(node *) * (n + 1));
        for (int i = 0; i < n; i++) v->list[i] = decls[i];
        v->n = n;
        return v;
    }
    if (eat_kw("if")) {
        node *n = mknode(N_IF);
        eat_p("("); n->a = parse_expr(); eat_p(")");
        n->b = parse_stmt();
        if (eat_kw("else")) n->c = parse_stmt();
        return n;
    }
    if (eat_kw("while")) {
        node *n = mknode(N_WHILE);
        eat_p("("); n->a = parse_expr(); eat_p(")");
        n->b = parse_stmt();
        return n;
    }
    if (eat_kw("for")) {
        node *n = mknode(N_FOR);
        eat_p("(");
        if (!is_p(";")) n->a = parse_stmt(); else eat_p(";"); /* init (parse_stmt eats ;) */
        if (!is_p(";")) n->b = parse_expr();
        eat_p(";");
        if (!is_p(")")) n->c = parse_expr();
        eat_p(")");
        n->d = parse_stmt();
        return n;
    }
    if (eat_kw("function")) {
        node *f = mknode(N_FUNC);
        f->str = cur()->s; adv();
        eat_p("(");
        node *params[16]; int np = 0;
        while (!is_p(")") && cur()->t != T_EOF) { if (cur()->t == T_ID && np < 16) { node *p = mknode(N_ID); p->str = cur()->s; params[np++] = p; adv(); } eat_p(","); }
        eat_p(")");
        f->list = amalloc(sizeof(node *) * (np + 1));
        for (int i = 0; i < np; i++) f->list[i] = params[i];
        f->n = np;
        f->a = parse_block();
        return f;
    }
    if (eat_kw("return")) {
        node *n = mknode(N_RETURN);
        if (!is_p(";") && !is_p("}") && cur()->t != T_EOF) n->a = parse_expr();
        eat_p(";");
        return n;
    }
    if (eat_kw("break")) { eat_p(";"); return mknode(N_BREAK); }
    if (eat_kw("continue")) { eat_p(";"); return mknode(N_CONT); }
    node *e = mknode(N_EXPR);
    e->a = parse_expr();
    eat_p(";");
    return e;
}

/* ---- values & scope ---------------------------------------------------- */
enum { V_UNDEF, V_NULL, V_NUM, V_BOOL, V_STR, V_FN, V_NATIVE, V_OBJ };
typedef struct { int type; long num; char *str; node *fn; void *env; int native; } val;
static val V_undef(void) { val v = {V_UNDEF, 0, NULL, NULL, NULL, 0}; return v; }
static val V_num(long n) { val v = {V_NUM, n, NULL, NULL, NULL, 0}; return v; }
static val V_bool(long b) { val v = {V_BOOL, b != 0, NULL, NULL, NULL, 0}; return v; }
static val V_str(char *s) { val v = {V_STR, 0, s, NULL, NULL, 0}; return v; }

typedef struct var { char *name; val v; struct var *next; } var;
typedef struct scope { var *vars; struct scope *parent; } scope;
static scope *global;

static var *find_var(scope *s, const char *name) {
    for (; s; s = s->parent)
        for (var *v = s->vars; v; v = v->next)
            if (!strcmp(v->name, name)) return v;
    return NULL;
}
static void set_var(scope *s, const char *name, val v, bool decl) {
    if (!decl) { var *e = find_var(s, name); if (e) { e->v = v; return; } }
    var *nv = amalloc(sizeof(var));
    if (!nv) return;
    nv->name = (char *)name; nv->v = v; nv->next = s->vars; s->vars = nv;
}

/* ---- to-string / to-number / truthiness -------------------------------- */
static char *num_to_str(long n) { char *b = amalloc(16); snprintf(b, 16, "%ld", n); return b; }
static const char *val_str(val v) {
    switch (v.type) {
    case V_STR: return v.str ? v.str : "";
    case V_NUM: return num_to_str(v.num);
    case V_BOOL: return v.num ? "true" : "false";
    case V_NULL: return "null";
    case V_FN: case V_NATIVE: return "function";
    default: return "undefined";
    }
}
static long val_num(val v) {
    if (v.type == V_NUM) return v.num;
    if (v.type == V_BOOL) return v.num;
    if (v.type == V_STR) return v.str ? strtol(v.str, NULL, 0) : 0;
    return 0;
}
static bool truthy(val v) {
    switch (v.type) {
    case V_NUM: return v.num != 0;
    case V_BOOL: return v.num != 0;
    case V_STR: return v.str && v.str[0];
    case V_UNDEF: case V_NULL: return false;
    default: return true;
    }
}

/* ---- evaluator --------------------------------------------------------- */
static int flow; /* 0 normal, 1 return, 2 break, 3 continue */
static val retval;
static int call_depth;
static int steps;

static val eval(node *n, scope *s);

static char *concat(const char *a, const char *b) {
    char *r = amalloc(strlen(a) + strlen(b) + 1);
    if (!r) return (char *)"";
    strcpy(r, a); strcat(r, b);
    return r;
}

static val call_native(const char *name, val *args, int na);

static val eval_call(node *n, scope *s) {
    /* resolve callee; member calls route to natives */
    node *callee = n->a;
    val args[16]; int na = n->n < 16 ? n->n : 16;
    for (int i = 0; i < na; i++) args[i] = eval(n->list[i], s);
    if (callee->k == N_MEMBER) {
        /* object.method(...) -> native dispatch by "object.method" or "method" */
        char key[64];
        if (callee->a->k == N_ID) snprintf(key, sizeof key, "%s.%s", callee->a->str, callee->str);
        else snprintf(key, sizeof key, ".%s", callee->str);
        /* string methods need the receiver */
        if (callee->a->k != N_ID) {
            val recv = eval(callee->a, s);
            if (recv.type == V_STR) {
                if (!strcmp(callee->str, "charAt")) { long i = na ? val_num(args[0]) : 0; char *c = amalloc(2); c[0] = (i >= 0 && i < (long)strlen(recv.str)) ? recv.str[i] : 0; c[1] = 0; return V_str(c); }
                if (!strcmp(callee->str, "toUpperCase")) { char *c = astrdup(recv.str, strlen(recv.str)); for (char *p = c; *p; p++) *p = toupper(*p); return V_str(c); }
                if (!strcmp(callee->str, "toLowerCase")) { char *c = astrdup(recv.str, strlen(recv.str)); str_lower(c); return V_str(c); }
            }
        }
        return call_native(key, args, na);
    }
    val f = eval(callee, s);
    if (f.type == V_NATIVE) return call_native(f.str, args, na);
    if (f.type == V_FN && f.fn) {
        if (++call_depth > 64) { err("recursion too deep"); call_depth--; return V_undef(); }
        scope local = {NULL, (scope *)f.env ? (scope *)f.env : global};
        node *fn = f.fn;
        for (int i = 0; i < fn->n; i++) set_var(&local, fn->list[i]->str, i < na ? args[i] : V_undef(), true);
        int save = flow; flow = 0;
        eval(fn->a, &local);
        val r = (flow == 1) ? retval : V_undef();
        flow = save;
        call_depth--;
        return r;
    }
    err("not a function");
    return V_undef();
}

static void assign_to(node *tgt, val v, scope *s) {
    if (tgt->k == N_ID) set_var(s, tgt->str, v, false);
    /* member/index assignment (e.g. el.innerHTML=..) is accepted but not applied */
}

static val eval(node *n, scope *s) {
    if (!n || js_err) return V_undef();
    if (++steps > 2000000) { err("script ran too long"); return V_undef(); }
    switch (n->k) {
    case N_NUM: return V_num(n->num);
    case N_STR: return V_str(n->str);
    case N_BOOL: return V_bool(n->num);
    case N_NULL: { val v = {V_NULL, 0, NULL, NULL, NULL, 0}; return v; }
    case N_UNDEF: return V_undef();
    case N_ID: {
        var *v = find_var(s, n->str);
        if (v) return v->v;
        /* known globals as native refs */
        if (!strcmp(n->str, "document") || !strcmp(n->str, "console") || !strcmp(n->str, "Math") || !strcmp(n->str, "window")) { val r = {V_OBJ, 0, n->str, NULL, NULL, 0}; return r; }
        if (!strcmp(n->str, "alert") || !strcmp(n->str, "parseInt") || !strcmp(n->str, "String") || !strcmp(n->str, "Number") || !strcmp(n->str, "parseFloat") || !strcmp(n->str, "isNaN") || !strcmp(n->str, "prompt")) { val r = {V_NATIVE, 0, n->str, NULL, NULL, 0}; return r; }
        return V_undef();
    }
    case N_FUNCEXPR: { val v = {V_FN, 0, NULL, n, s, 0}; return v; }
    case N_UN: {
        val a = eval(n->a, s);
        if (n->str[0] == '!') return V_bool(!truthy(a));
        return V_num(-val_num(a));
    }
    case N_BIN: {
        const char *op = n->str;
        if (!strcmp(op, "&&")) { val a = eval(n->a, s); return truthy(a) ? eval(n->b, s) : a; }
        if (!strcmp(op, "||")) { val a = eval(n->a, s); return truthy(a) ? a : eval(n->b, s); }
        val a = eval(n->a, s), b = eval(n->b, s);
        if (!strcmp(op, "+")) {
            if (a.type == V_STR || b.type == V_STR) return V_str(concat(val_str(a), val_str(b)));
            return V_num(val_num(a) + val_num(b));
        }
        if (!strcmp(op, "-")) return V_num(val_num(a) - val_num(b));
        if (!strcmp(op, "*")) return V_num(val_num(a) * val_num(b));
        if (!strcmp(op, "/")) { long d = val_num(b); return V_num(d ? val_num(a) / d : 0); }
        if (!strcmp(op, "%")) { long d = val_num(b); return V_num(d ? val_num(a) % d : 0); }
        if (!strcmp(op, "<")) return V_bool(val_num(a) < val_num(b));
        if (!strcmp(op, ">")) return V_bool(val_num(a) > val_num(b));
        if (!strcmp(op, "<=")) return V_bool(val_num(a) <= val_num(b));
        if (!strcmp(op, ">=")) return V_bool(val_num(a) >= val_num(b));
        if (!strcmp(op, "==") || !strcmp(op, "===")) {
            if (a.type == V_STR || b.type == V_STR) return V_bool(!strcmp(val_str(a), val_str(b)));
            return V_bool(val_num(a) == val_num(b));
        }
        if (!strcmp(op, "!=") || !strcmp(op, "!==")) {
            if (a.type == V_STR || b.type == V_STR) return V_bool(strcmp(val_str(a), val_str(b)) != 0);
            return V_bool(val_num(a) != val_num(b));
        }
        return V_undef();
    }
    case N_COND: return truthy(eval(n->a, s)) ? eval(n->b, s) : eval(n->c, s);
    case N_ASSIGN: {
        val rv = eval(n->b ? n->b : n->a, s);
        if (n->b) {
            if (strcmp(n->str, "=")) { /* compound */
                val old = eval(n->a, s);
                if (n->str[0] == '+') rv = (old.type == V_STR || rv.type == V_STR) ? V_str(concat(val_str(old), val_str(rv))) : V_num(val_num(old) + val_num(rv));
                else if (n->str[0] == '-') rv = V_num(val_num(old) - val_num(rv));
                else if (n->str[0] == '*') rv = V_num(val_num(old) * val_num(rv));
                else if (n->str[0] == '/') { long d = val_num(rv); rv = V_num(d ? val_num(old) / d : 0); }
            }
            assign_to(n->a, rv, s);
            return rv;
        }
        return rv;
    }
    case N_MEMBER: {
        val a = eval(n->a, s);
        if (a.type == V_STR && !strcmp(n->str, "length")) return V_num((long)strlen(a.str));
        if (a.type == V_OBJ && !strcmp(a.str, "Math")) { val r = {V_NATIVE, 0, concat("Math.", n->str), NULL, NULL, 0}; return r; }
        if (a.type == V_OBJ) { val r = {V_NATIVE, 0, concat(a.str ? a.str : "", concat(".", n->str)), NULL, NULL, 0}; return r; }
        return V_undef();
    }
    case N_INDEX: {
        val a = eval(n->a, s), i = eval(n->b, s);
        if (a.type == V_STR) { long ix = val_num(i); char *c = amalloc(2); c[0] = (ix >= 0 && ix < (long)strlen(a.str)) ? a.str[ix] : 0; c[1] = 0; return V_str(c); }
        return V_undef();
    }
    case N_CALL: return eval_call(n, s);
    case N_BLOCK: for (int i = 0; i < n->n && !flow && !js_err; i++) eval(n->list[i], s); return V_undef();
    case N_VAR: for (int i = 0; i < n->n; i++) { node *d = n->list[i]; val v = d->b ? eval(d->b, s) : V_undef(); set_var(s, d->a->str, v, true); } return V_undef();
    case N_EXPR: return eval(n->a, s);
    case N_IF: if (truthy(eval(n->a, s))) eval(n->b, s); else if (n->c) eval(n->c, s); return V_undef();
    case N_WHILE: {
        int guard = 0;
        while (truthy(eval(n->a, s)) && !js_err) {
            eval(n->b, s);
            if (flow == 2) { flow = 0; break; }
            if (flow == 3) flow = 0;
            if (flow == 1) break;
            if (++guard > 1000000) { err("loop ran too long"); break; }
        }
        return V_undef();
    }
    case N_FOR: {
        scope ls = {NULL, s};
        if (n->a) eval(n->a, &ls);
        int guard = 0;
        while ((!n->b || truthy(eval(n->b, &ls))) && !js_err) {
            eval(n->d, &ls);
            if (flow == 2) { flow = 0; break; }
            if (flow == 3) flow = 0;
            if (flow == 1) break;
            if (n->c) eval(n->c, &ls);
            if (++guard > 1000000) { err("loop ran too long"); break; }
        }
        return V_undef();
    }
    case N_FUNC: { val v = {V_FN, 0, NULL, n, s, 0}; v.fn = n; set_var(global, n->str, v, true); return V_undef(); }
    case N_RETURN: retval = n->a ? eval(n->a, s) : V_undef(); flow = 1; return retval;
    case N_BREAK: flow = 2; return V_undef();
    case N_CONT: flow = 3; return V_undef();
    }
    return V_undef();
}

static val call_native(const char *name, val *args, int na) {
    val a0 = na > 0 ? args[0] : V_undef();
    if (!strcmp(name, "document.write") || !strcmp(name, "document.writeln")) {
        for (int i = 0; i < na; i++) out_str(val_str(args[i]));
        if (strstr(name, "writeln")) out_str("\n");
        return V_undef();
    }
    if (!strcmp(name, "console.log") || !strcmp(name, "window.console.log")) {
        out_str("<br>[log] ");
        for (int i = 0; i < na; i++) { out_str(val_str(args[i])); out_str(" "); }
        out_str("<br>");
        return V_undef();
    }
    if (!strcmp(name, "alert") || !strcmp(name, "window.alert")) {
        if (!js_had_alert) { strlcpy(js_alert, val_str(a0), sizeof js_alert); js_had_alert = true; }
        return V_undef();
    }
    if (!strcmp(name, "document.getElementById") || !strcmp(name, "document.querySelector")) { val r = {V_OBJ, 0, "element", NULL, NULL, 0}; return r; }
    if (!strcmp(name, "parseInt") || !strcmp(name, "Number") || !strcmp(name, "parseFloat")) return V_num(val_num(a0));
    if (!strcmp(name, "isNaN")) return V_bool(a0.type == V_STR && !isdigit((uint8_t)(a0.str ? a0.str[0] : 0)));
    if (!strcmp(name, "String")) return V_str((char *)val_str(a0));
    if (!strcmp(name, "prompt")) return V_str("");
    if (!strcmp(name, "Math.floor") || !strcmp(name, "Math.round") || !strcmp(name, "Math.ceil") || !strcmp(name, "Math.trunc")) return V_num(val_num(a0));
    if (!strcmp(name, "Math.abs")) { long v = val_num(a0); return V_num(v < 0 ? -v : v); }
    if (!strcmp(name, "Math.max")) { long m = na ? val_num(args[0]) : 0; for (int i = 1; i < na; i++) if (val_num(args[i]) > m) m = val_num(args[i]); return V_num(m); }
    if (!strcmp(name, "Math.min")) { long m = na ? val_num(args[0]) : 0; for (int i = 1; i < na; i++) if (val_num(args[i]) < m) m = val_num(args[i]); return V_num(m); }
    if (!strcmp(name, "Math.pow")) { long b = val_num(a0), e = na > 1 ? val_num(args[1]) : 0, r = 1; while (e-- > 0) r *= b; return V_num(r); }
    if (!strcmp(name, "Math.sqrt")) { long v = val_num(a0), r = 0; while (r * r <= v) r++; return V_num(r - 1); }
    if (!strcmp(name, "Math.random")) { static uint32_t sd = 0x1234567; sd = sd * 1103515245u + 12345u + ticks; return V_num((sd >> 16) % 1000); } /* 0..999 (no floats) */
    /* unknown .method on element/window: no-op */
    return V_undef();
}

/* ---- public API -------------------------------------------------------- */
void js_begin(void) {
    if (js_err && !arena) {} /* keep */
    arena_len = 0;
    ntok = 0; tp = 0;
    js_html_len = 0;
    if (js_html) js_html[0] = 0;
    js_had_alert = false;
    js_alert[0] = 0;
    js_err = false;
    js_errmsg[0] = 0;
    flow = 0; call_depth = 0; steps = 0;
    global = amalloc(sizeof(scope));
    if (global) { global->vars = NULL; global->parent = NULL; }
}
void js_run(const char *src, int len) {
    if (!global) js_begin();
    int save_html = js_html_len;
    js_err = false;
    ntok = 0; tp = 0;
    lex(src, src + len);
    if (js_err) return;
    while (cur()->t != T_EOF && !js_err) {
        flow = 0;
        node *st = parse_stmt();
        eval(st, global);
    }
    if (js_err) {
        char m[160];
        snprintf(m, sizeof m, "<br><tt>[JavaScript error: %s]</tt><br>", js_errmsg);
        js_html_len = save_html; if (js_html) js_html[save_html] = 0;
        out_str(m);
    }
}
const char *js_output(void) { return js_html ? js_html : ""; }
uint32_t js_output_len(void) { return js_html_len; }
bool js_get_alert(char *out, size_t n) { if (!js_had_alert) return false; strlcpy(out, js_alert, n); js_had_alert = false; return true; }
void js_end(void) { /* arena kept for reuse; freed on browser exit via js_free */ }
void js_free(void) { kfree(arena); arena = NULL; arena_cap = arena_len = 0; kfree(toks); toks = NULL; tcap = ntok = 0; kfree(js_html); js_html = NULL; js_html_cap = js_html_len = 0; global = NULL; }
