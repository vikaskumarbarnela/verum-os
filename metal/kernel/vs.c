#include "vs.h"

/* ---------------------------------------------------------------- values */

static Val *mk(VType t) { Val *v = kalloc(sizeof *v); v->t = t; return v; }
static Val NIL = { T_NIL, { 0 } };
static Val TRUE_ = { T_BOOL, { .b = 1 } };
static Val FALSE_ = { T_BOOL, { .b = 0 } };

Val *v_nil(void) { return &NIL; }
Val *v_bool(bool b) { return b ? &TRUE_ : &FALSE_; }
#define SMALL_INTS 4096
static Val small_ints[SMALL_INTS];                 /* 0..4095 preboxed: loop counters and sums stop allocating */
Val *v_int(int i) {
    if (i >= 0 && i < SMALL_INTS) {
        Val *s = &small_ints[i];
        if (s->t != T_INT) { s->t = T_INT; s->i = i; }
        return s;
    }
    Val *v = mk(T_INT); v->i = i; return v;
}
Val *v_str(const char *s) { Val *v = mk(T_STR); v->s = kstrdup(s); return v; }
Val *v_sym(const char *s) { Val *v = mk(T_SYM); v->s = kstrdup(s); return v; }
Val *v_list(int cap) {
    Val *v = mk(T_LIST);
    v->list.items = kalloc(sizeof(Val *) * (cap < 4 ? 4 : cap));
    v->list.n = 0;
    return v;
}
void list_push(Val *l, Val *x) {
    /* capacity is tracked implicitly: grow by doubling when n hits a power of two >= 4 */
    int n = l->list.n;
    if (n >= 4 && (n & (n - 1)) == 0) {
        Val **ni = kalloc(sizeof(Val *) * n * 2);
        memcpy(ni, l->list.items, sizeof(Val *) * n);
        l->list.items = ni;
    }
    l->list.items[n] = x;
    l->list.n = n + 1;
}
Val *v_dict(void) {
    Val *v = mk(T_DICT);
    v->dict.keys = kalloc(sizeof(char *) * 16);
    v->dict.vals = kalloc(sizeof(Val *) * 16);
    return v;
}
Val *dict_get(Val *d, const char *k) {
    for (int i = 0; i < d->dict.n; i++) if (strcmp(d->dict.keys[i], k) == 0) return d->dict.vals[i];
    return 0;
}
void dict_set(Val *d, const char *k, Val *v) {
    for (int i = 0; i < d->dict.n; i++) if (strcmp(d->dict.keys[i], k) == 0) { d->dict.vals[i] = v; return; }
    int n = d->dict.n;
    if (n >= 16 && (n & (n - 1)) == 0) {
        char **nk = kalloc(sizeof(char *) * n * 2); Val **nv = kalloc(sizeof(Val *) * n * 2);
        memcpy(nk, d->dict.keys, sizeof(char *) * n); memcpy(nv, d->dict.vals, sizeof(Val *) * n);
        d->dict.keys = nk; d->dict.vals = nv;
    }
    d->dict.keys[n] = kstrdup(k); d->dict.vals[n] = v; d->dict.n = n + 1;
}
Val *v_cap(const char *token, const char *label) {
    Val *v = mk(T_CAP);
    memcpy(v->cap.token, token, 32); v->cap.token[32] = 0;
    v->cap.label = label;
    return v;
}
bool truthy(Val *v) { return !(v->t == T_NIL || (v->t == T_BOOL && !v->b)); }
bool v_equal(Val *a, Val *b) {
    if (a->t != b->t) return false;
    switch (a->t) {
    case T_NIL: return true;
    case T_BOOL: return a->b == b->b;
    case T_INT: return a->i == b->i;
    case T_STR: case T_SYM: return strcmp(a->s, b->s) == 0;
    case T_LIST:
        if (a->list.n != b->list.n) return false;
        for (int i = 0; i < a->list.n; i++) if (!v_equal(a->list.items[i], b->list.items[i])) return false;
        return true;
    default: return a == b;
    }
}

/* bounded string builder */
typedef struct { char *p; usize n, cap; } SB;
static void sb_put(SB *b, const char *s) {
    while (*s && b->n + 1 < b->cap) b->p[b->n++] = *s++;
    b->p[b->n] = 0;
}
static void sb_putc(SB *b, char c) { char t[2] = { c, 0 }; sb_put(b, t); }

static void render(SB *b, Val *v, bool canon) {
    char num[16];
    switch (v->t) {
    case T_NIL: sb_put(b, canon ? "null" : "nil"); break;
    case T_BOOL: sb_put(b, v->b ? "true" : "false"); break;
    case T_INT: itoa(v->i, num); sb_put(b, num); break;
    case T_STR:
        if (!canon) { sb_put(b, v->s); break; }
        sb_putc(b, '"');
        for (const char *s = v->s; *s; s++) { if (*s == '"' || *s == '\\') sb_putc(b, '\\'); sb_putc(b, *s); }
        sb_putc(b, '"');
        break;
    case T_SYM: sb_put(b, v->s); break;
    case T_LIST:
        sb_putc(b, canon ? '[' : '(');
        for (int i = 0; i < v->list.n; i++) { if (i) sb_put(b, canon ? "," : " "); render(b, v->list.items[i], canon); }
        sb_putc(b, canon ? ']' : ')');
        break;
    case T_DICT: {
        /* sorted key order so identical dicts hash identically */
        int n = v->dict.n, idx[64];
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) idx[i] = i;
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && strcmp(v->dict.keys[idx[j-1]], v->dict.keys[idx[j]]) > 0; j--) { int t = idx[j]; idx[j] = idx[j-1]; idx[j-1] = t; }
        sb_putc(b, '{');
        for (int i = 0; i < n; i++) {
            if (i) sb_put(b, canon ? "," : ", ");
            if (canon) sb_putc(b, '"');
            sb_put(b, v->dict.keys[idx[i]]);
            sb_put(b, canon ? "\":" : ": ");
            render(b, v->dict.vals[idx[i]], canon);
        }
        sb_putc(b, '}');
        break;
    }
    case T_FN: sb_put(b, "<fn "); sb_put(b, v->fn.name); sb_putc(b, '>'); break;
    case T_PRIM: sb_put(b, "<builtin "); sb_put(b, v->prim.name); sb_putc(b, '>'); break;
    case T_CAP: sb_put(b, "<cap "); sb_put(b, v->cap.label); sb_putc(b, '>'); break;   /* token never shown */
    }
}
/* Verum Script source form: what the ledger stores, so replay can read it back with the VM's own reader. */
static void ser(SB *b, Val *v) {
    char num[16];
    switch (v->t) {
    case T_NIL: sb_put(b, "nil"); break;
    case T_BOOL: sb_put(b, v->b ? "true" : "false"); break;
    case T_INT: itoa(v->i, num); sb_put(b, num); break;
    case T_STR: case T_SYM:
        sb_putc(b, '"');
        for (const char *s = v->s; *s; s++) {
            if (*s == '"' || *s == '\\') { sb_putc(b, '\\'); sb_putc(b, *s); }
            else if (*s == '\n') sb_put(b, "\\n");
            else sb_putc(b, *s);
        }
        sb_putc(b, '"');
        break;
    case T_LIST:
        sb_put(b, "(list");
        for (int i = 0; i < v->list.n; i++) { sb_putc(b, ' '); ser(b, v->list.items[i]); }
        sb_putc(b, ')');
        break;
    case T_DICT:
        sb_put(b, "(dict");
        for (int i = 0; i < v->dict.n; i++) { sb_put(b, " \""); sb_put(b, v->dict.keys[i]); sb_put(b, "\" "); ser(b, v->dict.vals[i]); }
        sb_putc(b, ')');
        break;
    default: sb_put(b, "nil");
    }
}
void v_serialize(Val *v, char *out, usize cap) { SB b = { out, 0, cap }; out[0] = 0; ser(&b, v); }
Val *v_deserialize(const char *src) {
    const char *err; Val *forms = vs_parse(src, &err);
    if (!forms || forms->list.n != 1) return 0;
    Interp it = { .fuel = 200000, .syscall = 0 };
    return vs_run(&it, forms, env_new(env_builtins()));
}

static Val *persist_rec(Val *v) {
    switch (v->t) {
    case T_NIL: case T_BOOL: return v;                       /* static singletons */
    case T_INT: return v_int(v->i);
    case T_STR: return v_str(v->s);
    case T_SYM: return v_sym(v->s);
    case T_LIST: { Val *l = v_list(v->list.n); for (int i = 0; i < v->list.n; i++) list_push(l, persist_rec(v->list.items[i])); return l; }
    case T_DICT: { Val *d = v_dict(); for (int i = 0; i < v->dict.n; i++) dict_set(d, v->dict.keys[i], persist_rec(v->dict.vals[i])); return d; }
    default: return v_nil();                                  /* capability handles, closures and builtins do not travel */
    }
}
Val *v_persist(Val *v) { heap_persist_begin(); Val *r = persist_rec(v); heap_persist_end(); return r; }

void v_show(Val *v, char *out, usize cap) { SB b = { out, 0, cap }; out[0] = 0; render(&b, v, false); }
void v_canon(Val *v, char *out, usize cap) { SB b = { out, 0, cap }; out[0] = 0; render(&b, v, true); }

/* ---------------------------------------------------------------- environments */

Env *env_new(Env *outer) { Env *e = kalloc(sizeof *e); e->outer = outer; return e; }
void env_define(Env *e, const char *name, Val *v) {
    for (Env *n = e->next; n; n = n->next) if (strcmp(n->name, name) == 0) { n->val = v; return; }
    Env *n = kalloc(sizeof *n); n->name = name; n->val = v; n->next = e->next; e->next = n;   /* names live as long as the code or label that owns them */
}
Val *env_lookup(Env *e, const char *name) {
    for (; e; e = e->outer)
        for (Env *n = e->next; n; n = n->next) if (strcmp(n->name, name) == 0) return n->val;
    return 0;
}

/* ---------------------------------------------------------------- errors */

void vs_error(Interp *it, const char *msg) { it->error = msg; vs_longjmp(it->jmp, 1); }
static void burn(Interp *it, u32 n) {
    it->used += n;
    if (it->used > it->fuel) vs_error(it, "fuel exhausted");
    if (vm_preempt) { vm_preempt = false; task_yield(); }   /* the timer asked for a switch: let another cell run */
}
static void guard(Interp *it, int n) { if (n > VS_MAX_SIZE) vs_error(it, "value exceeds size budget"); }

static Val *need(Interp *it, Val *v, VType t, const char *what) {
    if (v->t != t) vs_error(it, what);
    return v;
}
#define NEED_INT(v)  need(it, v, T_INT, "expected a number")
#define NEED_STR(v)  need(it, v, T_STR, "expected a string")
#define NEED_LIST(v) need(it, v, T_LIST, "expected a list")
#define ARGS(n) if (argc != (n)) vs_error(it, "wrong number of arguments")

/* ---------------------------------------------------------------- संस्कृतलिपि (UTF-8 byte strings) */

static const struct { const char *sk, *en; } SANSKRIT[] = {
    {"परिभाषा", "define"}, {"कार्यम्", "lambda"}, {"अस्तु", "let"}, {"यदि", "if"}, {"विकल्पः", "cond"}, {"अन्यथा", "else"},
    {"क्रमेण", "do"}, {"च", "and"}, {"वा", "or"}, {"उद्धृतम्", "quote"}, {"आह्वानम्", "call"}, {"सन्देशे", "on-message"},
    {"योगः", "+"}, {"वियोगः", "-"}, {"गुणनम्", "*"}, {"भागः", "/"}, {"शेषफलम्", "mod"},
    {"समम्", "="}, {"न्यूनम्", "<"}, {"अधिकम्", ">"}, {"न्यूनसमम्", "<="}, {"अधिकसमम्", ">="}, {"न", "not"},
    {"शून्यम्?", "nil?"}, {"सूची", "list"}, {"प्रथमः", "head"}, {"शेषः", "tail"}, {"स्थानम्", "nth"}, {"परिमाणम्", "len"},
    {"योजय", "append"}, {"पुरतः", "cons"}, {"श्रेणी", "range"}, {"विपर्ययः", "reverse"},
    {"कोशः", "dict"}, {"गृहाण", "get"}, {"कुञ्चिकाः", "keys"}, {"संयोजय", "assoc"},
    {"वाक्यम्", "str"}, {"विभज", "split"}, {"संयुज", "join"}, {"बृहत्", "upper"}, {"लघु", "lower"}, {"अन्तर्गतम्", "contains"},
    {"खण्डः", "slice"}, {"दोषः", "error"}, {"प्रत्येकम्", "map"}, {"चयनम्", "filter"}, {"संकलनम्", "reduce"},
    {"भाण्डारः", "store"}, {"पटलः", "console"}, {"घटिका", "clock"}, {"यन्त्रम्", "kernel"}, {"अग्रिमः", "next"},
    {"प्रेषकः", "from"}, {"विषयः", "body"}, {"सत्यम्", "true"}, {"असत्यम्", "false"}, {"शून्यम्", "nil"},
};
static const struct { const char *sk, *en; } SANSKRIT_OPS[] = {
    {"प्राप्", "get"}, {"स्थापय", "put"}, {"अन्विष्य", "find"}, {"वंशः", "lineage"}, {"मुद्रय", "print"},
    {"अधुना", "now"}, {"प्रेषय", "send"}, {"समर्पय", "delegate"}, {"कोष्ठाः", "cells"},
};

/* Devanagari digit U+0966..U+096F is the UTF-8 sequence E0 A5 A6..AF */
static void sanskrit_canonical(char *tok, usize cap) {
    for (usize i = 0; i < sizeof SANSKRIT / sizeof SANSKRIT[0]; i++)
        if (strcmp(tok, SANSKRIT[i].sk) == 0) { strcpy(tok, SANSKRIT[i].en); return; }
    char out[64]; usize n = 0; bool digits = tok[0] != 0;
    for (const u8 *p = (const u8 *)tok; *p && n + 1 < sizeof out;) {
        if (p[0] == 0xE0 && p[1] == 0xA5 && p[2] >= 0xA6 && p[2] <= 0xAF) { out[n++] = (char)('0' + (p[2] - 0xA6)); p += 3; }
        else if (*p == '-' && p == (const u8 *)tok) { out[n++] = '-'; p++; }
        else { digits = false; break; }
    }
    if (digits && n) { out[n] = 0; if (strlen(out) < cap) strcpy(tok, out); }
}

/* Is this the English spelling of a Verum Script word (keyword, builtin, label, op)? Such words stay Latin under lipi. */
bool sanskrit_is_english(const char *word) {
    for (usize i = 0; i < sizeof SANSKRIT / sizeof SANSKRIT[0]; i++) if (strcmp(word, SANSKRIT[i].en) == 0) return true;
    for (usize i = 0; i < sizeof SANSKRIT_OPS / sizeof SANSKRIT_OPS[0]; i++) if (strcmp(word, SANSKRIT_OPS[i].en) == 0) return true;
    static const char *const EXTRA[] = { "on", "off", "yes", "nil?", "number?", "string?", "list?", "dict?", "sort", "trim", "mod" };
    for (usize i = 0; i < sizeof EXTRA / sizeof EXTRA[0]; i++) if (strcmp(word, EXTRA[i]) == 0) return true;
    return false;
}

const char *sanskrit_op(const char *op) {
    for (usize i = 0; i < sizeof SANSKRIT_OPS / sizeof SANSKRIT_OPS[0]; i++)
        if (strcmp(op, SANSKRIT_OPS[i].sk) == 0) return SANSKRIT_OPS[i].en;
    return op;
}

/* ---------------------------------------------------------------- reader */

static const char *rd_err;
static const char *rd;

static void skip_ws(void) {
    for (;;) {
        while (*rd == ' ' || *rd == '\t' || *rd == '\n' || *rd == '\r') rd++;
        if (*rd == ';') { while (*rd && *rd != '\n') rd++; } else return;
    }
}
static Val *read_form(void) {
    skip_ws();
    if (!*rd) { rd_err = "unexpected end of input"; return 0; }
    if (*rd == '(') {
        rd++;
        Val *l = v_list(4);
        for (;;) {
            skip_ws();
            if (*rd == ')') { rd++; return l; }
            if (!*rd) { rd_err = "missing )"; return 0; }
            Val *x = read_form();
            if (!x) return 0;
            list_push(l, x);
        }
    }
    if (*rd == ')') { rd_err = "unexpected )"; return 0; }
    if (*rd == '"') {
        rd++;
        char buf[VS_MAX_SIZE]; usize n = 0;
        while (*rd && *rd != '"' && n + 1 < sizeof buf) {
            char c = *rd++;
            if (c == '\\' && *rd) { char e = *rd++; c = e == 'n' ? '\n' : e == 't' ? '\t' : e; }
            buf[n++] = c;
        }
        if (*rd != '"') { rd_err = "unterminated string"; return 0; }
        rd++;
        buf[n] = 0;
        return v_str(buf);
    }
    char tok[256]; usize n = 0;
    while (*rd && *rd != ' ' && *rd != '\t' && *rd != '\n' && *rd != '\r' && *rd != '(' && *rd != ')' && *rd != ';' && n + 1 < sizeof tok)
        tok[n++] = *rd++;
    tok[n] = 0;
    sanskrit_canonical(tok, sizeof tok);          /* संस्कृतलिपि: Sanskrit words and digits become canonical */
    n = strlen(tok);                              /* a Devanagari digit shrank from 3 bytes to 1 */
    if (strcmp(tok, "true") == 0) return v_bool(true);
    if (strcmp(tok, "false") == 0) return v_bool(false);
    if (strcmp(tok, "nil") == 0) return v_nil();
    bool numeric = n > 0; usize i = (tok[0] == '-' && n > 1) ? 1 : 0;
    for (; i < n; i++) if (tok[i] < '0' || tok[i] > '9') numeric = false;
    if (numeric) return v_int(atoi(tok));
    return v_sym(tok);
}
Val *vs_parse(const char *src, const char **err) {
    rd = src; rd_err = 0;
    Val *forms = v_list(8);
    for (;;) {
        skip_ws();
        if (!*rd) break;
        Val *f = read_form();
        if (!f) { *err = rd_err; return 0; }
        list_push(forms, f);
    }
    *err = 0;
    return forms;
}

/* ---------------------------------------------------------------- builtins */

#define PRIM(name) static Val *name(Interp *it, Val **a, int argc)

PRIM(p_add) { int r = 0; for (int i = 0; i < argc; i++) r += NEED_INT(a[i])->i; return v_int(r); }
PRIM(p_sub) { if (!argc) vs_error(it, "-: needs arguments"); int r = NEED_INT(a[0])->i; if (argc == 1) return v_int(-r); for (int i = 1; i < argc; i++) r -= NEED_INT(a[i])->i; return v_int(r); }
PRIM(p_mul) { int r = 1; for (int i = 0; i < argc; i++) r *= NEED_INT(a[i])->i; return v_int(r); }
PRIM(p_div) { ARGS(2); int d = NEED_INT(a[1])->i; if (!d) vs_error(it, "division by zero"); return v_int(NEED_INT(a[0])->i / d); }
PRIM(p_mod) { ARGS(2); int d = NEED_INT(a[1])->i; if (!d) vs_error(it, "mod by zero"); return v_int(NEED_INT(a[0])->i % d); }
PRIM(p_eq) { ARGS(2); return v_bool(v_equal(a[0], a[1])); }
PRIM(p_lt) { ARGS(2); return v_bool(NEED_INT(a[0])->i <  NEED_INT(a[1])->i); }
PRIM(p_gt) { ARGS(2); return v_bool(NEED_INT(a[0])->i >  NEED_INT(a[1])->i); }
PRIM(p_le) { ARGS(2); return v_bool(NEED_INT(a[0])->i <= NEED_INT(a[1])->i); }
PRIM(p_ge) { ARGS(2); return v_bool(NEED_INT(a[0])->i >= NEED_INT(a[1])->i); }
PRIM(p_not) { ARGS(1); return v_bool(!truthy(a[0])); }
PRIM(p_nilp) { ARGS(1); return v_bool(a[0]->t == T_NIL); }
PRIM(p_list) { Val *l = v_list(argc); for (int i = 0; i < argc; i++) list_push(l, a[i]); return l; }
PRIM(p_head) { ARGS(1); Val *l = NEED_LIST(a[0]); return l->list.n ? l->list.items[0] : v_nil(); }
PRIM(p_tail) { ARGS(1); Val *l = NEED_LIST(a[0]); Val *r = v_list(l->list.n); for (int i = 1; i < l->list.n; i++) list_push(r, l->list.items[i]); return r; }
PRIM(p_nth) { ARGS(2); Val *l = NEED_LIST(a[0]); int i = NEED_INT(a[1])->i; if (i < 0 || i >= l->list.n) vs_error(it, "nth: index out of range"); return l->list.items[i]; }
PRIM(p_len) {
    ARGS(1);
    if (a[0]->t == T_STR) return v_int((int)strlen(a[0]->s));
    if (a[0]->t == T_LIST) return v_int(a[0]->list.n);
    if (a[0]->t == T_DICT) return v_int(a[0]->dict.n);
    vs_error(it, "len: not sized");
}
PRIM(p_append) {
    int total = 0;
    for (int i = 0; i < argc; i++) total += NEED_LIST(a[i])->list.n;
    guard(it, total);
    Val *r = v_list(total);
    for (int i = 0; i < argc; i++) for (int j = 0; j < a[i]->list.n; j++) list_push(r, a[i]->list.items[j]);
    return r;
}
PRIM(p_cons) { ARGS(2); Val *l = NEED_LIST(a[1]); guard(it, l->list.n + 1); Val *r = v_list(l->list.n + 1); list_push(r, a[0]); for (int i = 0; i < l->list.n; i++) list_push(r, l->list.items[i]); return r; }
PRIM(p_range) { ARGS(1); int n = NEED_INT(a[0])->i; if (n < 0) n = 0; guard(it, n); burn(it, (u32)n); Val *r = v_list(n); for (int i = 0; i < n; i++) list_push(r, v_int(i)); return r; }
PRIM(p_reverse) { ARGS(1); Val *l = NEED_LIST(a[0]); Val *r = v_list(l->list.n); for (int i = l->list.n - 1; i >= 0; i--) list_push(r, l->list.items[i]); return r; }
PRIM(p_dict) {
    if (argc % 2) vs_error(it, "dict: odd number of arguments");
    Val *d = v_dict();
    for (int i = 0; i < argc; i += 2) dict_set(d, NEED_STR(a[i])->s, a[i + 1]);
    return d;
}
PRIM(p_get) {
    if (argc < 2 || argc > 3) vs_error(it, "get: (get d key [default])");
    Val *dflt = argc == 3 ? a[2] : v_nil();
    if (a[0]->t == T_DICT) { Val *v = dict_get(a[0], NEED_STR(a[1])->s); return v ? v : dflt; }
    if (a[0]->t == T_LIST) { int i = NEED_INT(a[1])->i; return (i >= 0 && i < a[0]->list.n) ? a[0]->list.items[i] : dflt; }
    vs_error(it, "get: cannot index value");
}
PRIM(p_keys) { ARGS(1); Val *d = need(it, a[0], T_DICT, "keys: not a dict"); Val *r = v_list(d->dict.n); for (int i = 0; i < d->dict.n; i++) list_push(r, v_str(d->dict.keys[i])); return r; }
PRIM(p_assoc) {
    ARGS(3); Val *d = need(it, a[0], T_DICT, "assoc: not a dict");
    Val *r = v_dict();
    for (int i = 0; i < d->dict.n; i++) dict_set(r, d->dict.keys[i], d->dict.vals[i]);
    dict_set(r, NEED_STR(a[1])->s, a[2]);
    return r;
}
PRIM(p_str) {
    char buf[VS_MAX_SIZE]; usize n = 0; buf[0] = 0;
    for (int i = 0; i < argc; i++) { v_show(a[i], buf + n, sizeof buf - n); n = strlen(buf); }
    return v_str(buf);
}
PRIM(p_split) {
    if (argc < 1 || argc > 2) vs_error(it, "split: (split s [sep])");
    const char *s = NEED_STR(a[0])->s, *sep = argc == 2 ? NEED_STR(a[1])->s : " ";
    usize sl = strlen(sep); if (!sl) vs_error(it, "split: empty separator");
    Val *r = v_list(8); char buf[VS_MAX_SIZE]; usize n = 0;
    for (;;) {
        if (!*s || strncmp(s, sep, sl) == 0) {
            if (n) { buf[n] = 0; list_push(r, v_str(buf)); n = 0; guard(it, r->list.n); }
            if (!*s) break;
            s += sl;
        } else if (n + 1 < sizeof buf) buf[n++] = *s++;
        else s++;
    }
    return r;
}
PRIM(p_join) {
    if (argc < 1 || argc > 2) vs_error(it, "join: (join l [sep])");
    Val *l = NEED_LIST(a[0]); const char *sep = argc == 2 ? NEED_STR(a[1])->s : "";
    char buf[VS_MAX_SIZE]; usize n = 0; buf[0] = 0;
    for (int i = 0; i < l->list.n; i++) {
        if (i && n + strlen(sep) + 1 < sizeof buf) { strcpy(buf + n, sep); n += strlen(sep); }
        v_show(l->list.items[i], buf + n, sizeof buf - n); n = strlen(buf);
    }
    return v_str(buf);
}
static Val *map_case(Interp *it, Val *v, int delta) {
    const char *s = NEED_STR(v)->s; char *r = kstrdup(s);
    for (char *p = r; *p; p++) {
        if (delta > 0 && *p >= 'a' && *p <= 'z') *p -= 32;
        if (delta < 0 && *p >= 'A' && *p <= 'Z') *p += 32;
    }
    Val *o = kalloc(sizeof *o); o->t = T_STR; o->s = r; return o;
}
PRIM(p_upper) { ARGS(1); return map_case(it, a[0], 1); }
PRIM(p_lower) { ARGS(1); return map_case(it, a[0], -1); }
PRIM(p_contains) { ARGS(2); return v_bool(strstr(NEED_STR(a[0])->s, NEED_STR(a[1])->s) != 0); }
PRIM(p_slice) {
    if (argc < 2 || argc > 3) vs_error(it, "slice: (slice v from [to])");
    int from = NEED_INT(a[1])->i;
    if (a[0]->t == T_STR) {
        int len = (int)strlen(a[0]->s), to = argc == 3 ? NEED_INT(a[2])->i : len;
        if (from < 0) from = 0; if (to > len) to = len; if (to < from) to = from;
        char buf[VS_MAX_SIZE]; memcpy(buf, a[0]->s + from, (usize)(to - from)); buf[to - from] = 0;
        return v_str(buf);
    }
    Val *l = NEED_LIST(a[0]); int to = argc == 3 ? NEED_INT(a[2])->i : l->list.n;
    if (from < 0) from = 0; if (to > l->list.n) to = l->list.n;
    Val *r = v_list(to - from > 0 ? to - from : 1);
    for (int i = from; i < to; i++) list_push(r, l->list.items[i]);
    return r;
}
PRIM(p_error) { ARGS(1); char *m = kalloc(256); v_show(a[0], m, 256); vs_error(it, m); }
PRIM(p_map) {
    ARGS(2); Val *l = NEED_LIST(a[1]); Val *r = v_list(l->list.n);
    for (int i = 0; i < l->list.n; i++) { Val *x = l->list.items[i]; list_push(r, vs_apply(it, a[0], &x, 1)); }
    return r;
}
PRIM(p_filter) {
    ARGS(2); Val *l = NEED_LIST(a[1]); Val *r = v_list(l->list.n);
    for (int i = 0; i < l->list.n; i++) { Val *x = l->list.items[i]; if (truthy(vs_apply(it, a[0], &x, 1))) list_push(r, x); }
    return r;
}
PRIM(p_reduce) {
    ARGS(3); Val *l = NEED_LIST(a[2]); Val *acc = a[1];
    for (int i = 0; i < l->list.n; i++) { Val *args[2] = { acc, l->list.items[i] }; acc = vs_apply(it, a[0], args, 2); }
    return acc;
}

static const struct { const char *name; PrimFn f; } PRIMS[] = {
    {"+", p_add}, {"-", p_sub}, {"*", p_mul}, {"/", p_div}, {"mod", p_mod},
    {"=", p_eq}, {"<", p_lt}, {">", p_gt}, {"<=", p_le}, {">=", p_ge}, {"not", p_not}, {"nil?", p_nilp},
    {"list", p_list}, {"head", p_head}, {"tail", p_tail}, {"nth", p_nth}, {"len", p_len},
    {"append", p_append}, {"cons", p_cons}, {"range", p_range}, {"reverse", p_reverse},
    {"dict", p_dict}, {"get", p_get}, {"keys", p_keys}, {"assoc", p_assoc},
    {"str", p_str}, {"split", p_split}, {"join", p_join}, {"upper", p_upper}, {"lower", p_lower},
    {"contains", p_contains}, {"slice", p_slice}, {"error", p_error},
    {"map", p_map}, {"filter", p_filter}, {"reduce", p_reduce},
};

Env *env_builtins(void) {
    Env *e = env_new(0);
    for (usize i = 0; i < sizeof PRIMS / sizeof PRIMS[0]; i++) {
        Val *v = mk(T_PRIM); v->prim.name = PRIMS[i].name; v->prim.f = PRIMS[i].f;
        env_define(e, PRIMS[i].name, v);
    }
    return e;
}

/* ---------------------------------------------------------------- evaluator */

static Val *eval_body(Interp *it, Val *forms, int from, Env *env) {
    Val *r = v_nil();
    for (int i = from; i < forms->list.n; i++) r = vs_eval(it, forms->list.items[i], env);
    return r;
}

Val *vs_apply(Interp *it, Val *fn, Val **argv, int argc) {
    if (fn->t == T_PRIM) return fn->prim.f(it, argv, argc);
    if (fn->t != T_FN) vs_error(it, "not callable");
    Val *params = fn->fn.params;
    if (params->list.n != argc) vs_error(it, "wrong number of arguments to function");
    if (++it->depth > VS_MAX_DEPTH) vs_error(it, "call depth exceeded");
    Env *local = env_new(fn->fn.env);
    for (int i = 0; i < argc; i++) env_define(local, params->list.items[i]->s, argv[i]);
    Val *r = eval_body(it, fn->fn.body, 0, local);
    it->depth--;
    return r;
}

static Val *make_fn(Interp *it, Val *params, Val *forms, int from, Env *env, const char *name) {
    if (params->t != T_LIST) vs_error(it, "lambda: parameters must be a list");
    for (int i = 0; i < params->list.n; i++) if (params->list.items[i]->t != T_SYM) vs_error(it, "lambda: parameter is not a symbol");
    Val *body = v_list(forms->list.n);
    for (int i = from; i < forms->list.n; i++) list_push(body, forms->list.items[i]);
    Val *f = mk(T_FN); f->fn.params = params; f->fn.body = body; f->fn.env = env; f->fn.name = name;
    return f;
}

Val *vs_eval(Interp *it, Val *x, Env *env) {
    burn(it, 1);
    if (x->t == T_SYM) { Val *v = env_lookup(env, x->s); if (!v) { char *m = kalloc(80); strcpy(m, "unbound symbol "); strcat(m, x->s); vs_error(it, m); } return v; }
    if (x->t != T_LIST) return x;
    if (x->list.n == 0) return x;
    Val *head = x->list.items[0];
    int n = x->list.n;
    if (head->t == T_SYM) {
        const char *s = head->s;
        if (strcmp(s, "quote") == 0) { if (n != 2) vs_error(it, "quote: one argument"); return x->list.items[1]; }
        if (strcmp(s, "if") == 0) {
            if (n != 3 && n != 4) vs_error(it, "if: (if cond then [else])");
            if (truthy(vs_eval(it, x->list.items[1], env))) return vs_eval(it, x->list.items[2], env);
            return n == 4 ? vs_eval(it, x->list.items[3], env) : v_nil();
        }
        if (strcmp(s, "cond") == 0) {
            for (int i = 1; i < n; i++) {
                Val *c = x->list.items[i];
                if (c->t != T_LIST || c->list.n == 0) vs_error(it, "cond: bad clause");
                Val *test = c->list.items[0];
                bool hit = (test->t == T_SYM && strcmp(test->s, "else") == 0) || truthy(vs_eval(it, test, env));
                if (hit) return eval_body(it, c, 1, env);
            }
            return v_nil();
        }
        if (strcmp(s, "do") == 0) return eval_body(it, x, 1, env);
        if (strcmp(s, "and") == 0) { Val *v = v_bool(true); for (int i = 1; i < n; i++) { v = vs_eval(it, x->list.items[i], env); if (!truthy(v)) return v; } return v; }
        if (strcmp(s, "or") == 0) { Val *v = v_bool(false); for (int i = 1; i < n; i++) { v = vs_eval(it, x->list.items[i], env); if (truthy(v)) return v; } return v; }
        if (strcmp(s, "let") == 0) {
            if (n < 3 || x->list.items[1]->t != T_LIST) vs_error(it, "let: (let ((name value) ...) body...)");
            Env *local = env_new(env);
            Val *binds = x->list.items[1];
            for (int i = 0; i < binds->list.n; i++) {
                Val *b = binds->list.items[i];
                if (b->t != T_LIST || b->list.n != 2 || b->list.items[0]->t != T_SYM) vs_error(it, "let: bad binding");
                env_define(local, b->list.items[0]->s, vs_eval(it, b->list.items[1], local));
            }
            return eval_body(it, x, 2, local);
        }
        if (strcmp(s, "lambda") == 0) { if (n < 3) vs_error(it, "lambda: (lambda (params) body...)"); return make_fn(it, x->list.items[1], x, 2, env, "lambda"); }
        if (strcmp(s, "define") == 0) {
            if (n < 3) vs_error(it, "define: (define name value) or (define (name params) body...)");
            Val *target = x->list.items[1];
            if (target->t == T_LIST) {
                if (target->list.n == 0 || target->list.items[0]->t != T_SYM) vs_error(it, "define: bad function head");
                Val *params = v_list(target->list.n);
                for (int i = 1; i < target->list.n; i++) list_push(params, target->list.items[i]);
                const char *name = target->list.items[0]->s;
                env_define(env, name, make_fn(it, params, x, 2, env, name));
                return v_nil();
            }
            if (target->t != T_SYM) vs_error(it, "define: name must be a symbol");
            env_define(env, target->s, vs_eval(it, x->list.items[2], env));
            return v_nil();
        }
        if (strcmp(s, "call") == 0) {
            if (n < 3) vs_error(it, "call: (call <capability> \"op\" args...)");
            Val *cap = vs_eval(it, x->list.items[1], env);
            if (cap->t != T_CAP) vs_error(it, "call: first argument must be a capability");
            Val *op = vs_eval(it, x->list.items[2], env);
            if (op->t != T_STR) vs_error(it, "call: operation must be a string");
            Val *argv[8]; int argc = n - 3;
            if (argc > 8) vs_error(it, "call: too many arguments");
            for (int i = 0; i < argc; i++) argv[i] = vs_eval(it, x->list.items[3 + i], env);
            burn(it, 10);
            if (!it->syscall) vs_error(it, "no capabilities here");
            return it->syscall(it, cap, op->s, argv, argc);
        }
    }
    Val *fn = vs_eval(it, head, env);
    Val *argv[16]; int argc = n - 1;
    if (argc > 16) vs_error(it, "too many arguments");
    for (int i = 0; i < argc; i++) argv[i] = vs_eval(it, x->list.items[1 + i], env);
    return vs_apply(it, fn, argv, argc);
}

Val *vs_run(Interp *it, Val *forms, Env *env) {
    it->error = 0; it->depth = 0;
    if (vs_setjmp(it->jmp)) return 0;
    return eval_body(it, forms, 0, env);
}

Val *vs_call(Interp *it, Val *fn, Val **argv, int argc) {
    it->error = 0; it->depth = 0;
    if (vs_setjmp(it->jmp)) return 0;
    return vs_apply(it, fn, argv, argc);
}
