/* S2b on bare metal: the bytecode compiler and VM for Verum Script.
 *
 * Mirrors verum/bytecode.py: forms are compiled once, at spawn, into a flat
 * instruction list; variables resolve at compile time to (depth, slot) with
 * static-link frames; fuel is charged per instruction and the timer's
 * preempt flag is checked at the same point; every instruction carries its
 * source line, so an error says where it happened. The only effect is still
 * CALLCAP, which goes through the kernel's syscall function unchanged. */
#include "vs.h"

enum { CONST, LOCAL, SETLOCAL, GLOBAL, SETGLOBAL, JUMP, JUMPIFNOT, JUMPIF, POP, DUP, CALL, CALLCAP, CLOSURE, RET };

typedef struct { u8 op; u16 line; i32 a, b; Val *cache; u32 gen; } Ins;   /* cache: inline cache for GLOBAL */
static u32 bc_gen = 1;                                /* bumped whenever a global is (re)defined */

typedef struct Code {
    const char *name;
    int nparams, nslots;
    Ins *ins; int n, cap;
    Val **consts; int nconsts, ccap;
    struct Code **codes; int ncodes, kcap;
} Code;

typedef struct Frame { Val **slots; struct Frame *parent; } Frame;

extern volatile bool vm_preempt;
void task_yield(void);

/* ---------------------------------------------------------------- code objects */

static Code *code_new(const char *name, int nparams) {
    Code *c = kalloc(sizeof *c);
    c->name = name; c->nparams = nparams; c->nslots = nparams;
    c->cap = 32; c->ins = kalloc(sizeof(Ins) * c->cap);
    c->ccap = 8; c->consts = kalloc(sizeof(Val *) * c->ccap);
    c->kcap = 4; c->codes = kalloc(sizeof(Code *) * c->kcap);
    return c;
}
static int emit(Code *c, int op, i32 a, i32 b, int line) {
    if (c->n == c->cap) { Ins *ni = kalloc(sizeof(Ins) * c->cap * 2); memcpy(ni, c->ins, sizeof(Ins) * c->n); c->ins = ni; c->cap *= 2; }
    c->ins[c->n].op = (u8)op; c->ins[c->n].a = a; c->ins[c->n].b = b; c->ins[c->n].line = (u16)line;
    return c->n++;
}
static int konst(Code *c, Val *v) {
    if (c->nconsts == c->ccap) { Val **nc = kalloc(sizeof(Val *) * c->ccap * 2); memcpy(nc, c->consts, sizeof(Val *) * c->nconsts); c->consts = nc; c->ccap *= 2; }
    c->consts[c->nconsts] = v; return c->nconsts++;
}
static int add_code(Code *c, Code *child) {
    if (c->ncodes == c->kcap) { Code **nk = kalloc(sizeof(Code *) * c->kcap * 2); memcpy(nk, c->codes, sizeof(Code *) * c->ncodes); c->codes = nk; c->kcap *= 2; }
    c->codes[c->ncodes] = child; return c->ncodes++;
}
static void patch(Code *c, int at, int target) { c->ins[at].a = target; }

/* ---------------------------------------------------------------- compiler */

#define MAX_BLOCK_NAMES 64
#define MAX_BLOCKS 16
typedef struct { const char *names[MAX_BLOCK_NAMES]; int slots[MAX_BLOCK_NAMES]; int n; } Block;
typedef struct Scope { Code *code; struct Scope *parent; bool module; Block blocks[MAX_BLOCKS]; int nblocks; } Scope;

static Interp *cit;                                  /* the interpreter compiling right now (for errors) */
static void cerr(const char *msg, int line) {
    char *m = kalloc(160); strcpy(m, msg);
    if (line) { strcat(m, " at line "); utoa((u32)line, m + strlen(m)); }
    vs_error(cit, m);
}

static int scope_lookup(Scope *s, const char *name) {
    for (int b = s->nblocks - 1; b >= 0; b--)
        for (int i = s->blocks[b].n - 1; i >= 0; i--)
            if (strcmp(s->blocks[b].names[i], name) == 0) return s->blocks[b].slots[i];
    return -1;
}
static int scope_declare(Scope *s, const char *name) {
    Block *b = &s->blocks[s->nblocks - 1];
    if (b->n == MAX_BLOCK_NAMES) cerr("too many names in one block", 0);
    b->names[b->n] = name; b->slots[b->n] = s->code->nslots++;
    return b->slots[b->n++];
}
static Scope *scope_new(Code *code, Scope *parent, bool module) {
    Scope *s = kalloc(sizeof *s); s->code = code; s->parent = parent; s->module = module; s->nblocks = 1; return s;
}
/* (depth, slot) through the static chain; false if global */
static bool resolve(Scope *s, const char *name, int *depth, int *slot) {
    int d = 0;
    for (; s; s = s->parent) {
        if (!s->module) {
            int sl = scope_lookup(s, name);
            if (sl >= 0) { *depth = d; *slot = sl; return true; }
            d++;
        }
    }
    return false;
}

static void expr(Val *x, Scope *s, Code *c);
static void body(Val *forms, int start, Scope *s, Code *c) {
    if (start >= forms->list.n) { emit(c, CONST, konst(c, v_nil()), 0, 0); return; }
    for (int i = start; i < forms->list.n; i++) { expr(forms->list.items[i], s, c); if (i < forms->list.n - 1) emit(c, POP, 0, 0, 0); }
}
static Code *lambda_code(Val *params, Val *forms, int start, Scope *s, const char *name) {
    if (params->t != T_LIST) cerr("lambda: parameters must be a list", params->line);
    Code *fn = code_new(name, params->list.n);
    Scope *inner = scope_new(fn, s, false);
    for (int i = 0; i < params->list.n; i++) {
        if (params->list.items[i]->t != T_SYM) cerr("lambda: parameter is not a symbol", params->line);
        Block *b = &inner->blocks[0]; b->names[b->n] = params->list.items[i]->s; b->slots[b->n] = i; b->n++;
    }
    body(forms, start, inner, fn);
    emit(fn, RET, 0, 0, 0);
    return fn;
}

static void expr(Val *x, Scope *s, Code *c) {
    int ln = x->line;
    if (x->t == T_SYM) {
        int d, sl;
        if (resolve(s, x->s, &d, &sl)) emit(c, LOCAL, d, sl, ln); else emit(c, GLOBAL, konst(c, x), 0, ln);
        return;
    }
    if (x->t != T_LIST) { emit(c, CONST, konst(c, x), 0, ln); return; }
    int n = x->list.n;
    if (n == 0) { emit(c, CONST, konst(c, x), 0, ln); return; }
    Val *head = x->list.items[0];
    if (head->t == T_SYM) {
        const char *h = head->s;
        if (strcmp(h, "quote") == 0) { if (n != 2) cerr("quote: one argument", ln); emit(c, CONST, konst(c, x->list.items[1]), 0, ln); return; }
        if (strcmp(h, "if") == 0) {
            if (n != 3 && n != 4) cerr("if: (if cond then [else])", ln);
            expr(x->list.items[1], s, c);
            int j1 = emit(c, JUMPIFNOT, 0, 0, ln);
            expr(x->list.items[2], s, c);
            int j2 = emit(c, JUMP, 0, 0, ln);
            patch(c, j1, c->n);
            if (n == 4) expr(x->list.items[3], s, c); else emit(c, CONST, konst(c, v_nil()), 0, ln);
            patch(c, j2, c->n);
            return;
        }
        if (strcmp(h, "cond") == 0) {
            int ends[64], ne = 0; bool had_else = false;
            for (int i = 1; i < n; i++) {
                Val *cl = x->list.items[i];
                if (cl->t != T_LIST || cl->list.n == 0) cerr("cond: bad clause", ln);
                Val *test = cl->list.items[0];
                if (test->t == T_SYM && strcmp(test->s, "else") == 0) { body(cl, 1, s, c); if (ne < 64) ends[ne++] = emit(c, JUMP, 0, 0, ln); had_else = true; break; }
                expr(test, s, c);
                int jn = emit(c, JUMPIFNOT, 0, 0, ln);
                body(cl, 1, s, c);
                if (ne < 64) ends[ne++] = emit(c, JUMP, 0, 0, ln);
                patch(c, jn, c->n);
            }
            if (!had_else) emit(c, CONST, konst(c, v_nil()), 0, ln);
            for (int i = 0; i < ne; i++) patch(c, ends[i], c->n);
            return;
        }
        if (strcmp(h, "do") == 0) { body(x, 1, s, c); return; }
        if (strcmp(h, "and") == 0 || strcmp(h, "or") == 0) {
            bool is_and = h[0] == 'a';
            if (n == 1) { emit(c, CONST, konst(c, v_bool(is_and)), 0, ln); return; }
            int ends[64], ne = 0;
            for (int i = 1; i < n; i++) {
                expr(x->list.items[i], s, c);
                if (i < n - 1) { emit(c, DUP, 0, 0, ln); if (ne < 64) ends[ne++] = emit(c, is_and ? JUMPIFNOT : JUMPIF, 0, 0, ln); emit(c, POP, 0, 0, ln); }
            }
            for (int i = 0; i < ne; i++) patch(c, ends[i], c->n);
            return;
        }
        if (strcmp(h, "let") == 0) {
            if (n < 3 || x->list.items[1]->t != T_LIST) cerr("let: (let ((name value) ...) body...)", ln);
            if (s->module) {                              /* module-level let: run inside a thunk so its slots have a frame */
                Code *thunk = code_new("let", 0);
                Scope *inner = scope_new(thunk, s, false);
                Val *binds = x->list.items[1];
                for (int i = 0; i < binds->list.n; i++) {
                    Val *b = binds->list.items[i];
                    if (b->t != T_LIST || b->list.n != 2 || b->list.items[0]->t != T_SYM) cerr("let: bad binding", ln);
                    expr(b->list.items[1], inner, thunk);
                    emit(thunk, SETLOCAL, 0, scope_declare(inner, b->list.items[0]->s), ln);
                }
                body(x, 2, inner, thunk); emit(thunk, RET, 0, 0, 0);
                emit(c, CLOSURE, add_code(c, thunk), 0, ln); emit(c, CALL, 0, 0, ln);
                return;
            }
            if (s->nblocks == MAX_BLOCKS) cerr("let: too deeply nested", ln);
            s->blocks[s->nblocks++].n = 0;
            Val *binds = x->list.items[1];
            for (int i = 0; i < binds->list.n; i++) {
                Val *b = binds->list.items[i];
                if (b->t != T_LIST || b->list.n != 2 || b->list.items[0]->t != T_SYM) cerr("let: bad binding", ln);
                expr(b->list.items[1], s, c);
                emit(c, SETLOCAL, 0, scope_declare(s, b->list.items[0]->s), ln);
            }
            body(x, 2, s, c);
            s->nblocks--;
            return;
        }
        if (strcmp(h, "lambda") == 0) {
            if (n < 3) cerr("lambda: (lambda (params) body...)", ln);
            emit(c, CLOSURE, add_code(c, lambda_code(x->list.items[1], x, 2, s, "lambda")), 0, ln);
            return;
        }
        if (strcmp(h, "define") == 0) {
            if (n < 3) cerr("define: (define name value) or (define (name params) body...)", ln);
            Val *target = x->list.items[1]; const char *name; int slot = -1;
            if (target->t == T_LIST) {
                if (target->list.n == 0 || target->list.items[0]->t != T_SYM) cerr("define: bad function head", ln);
                name = target->list.items[0]->s;
                if (!s->module) { slot = scope_lookup(s, name); if (slot < 0) slot = scope_declare(s, name); }
                Val *params = v_list(target->list.n);
                for (int i = 1; i < target->list.n; i++) list_push(params, target->list.items[i]);
                emit(c, CLOSURE, add_code(c, lambda_code(params, x, 2, s, name)), 0, ln);
            } else {
                if (target->t != T_SYM) cerr("define: name must be a symbol", ln);
                name = target->s;
                if (!s->module) { slot = scope_lookup(s, name); if (slot < 0) slot = scope_declare(s, name); }
                expr(x->list.items[2], s, c);
            }
            if (slot < 0) emit(c, SETGLOBAL, konst(c, target->t == T_LIST ? target->list.items[0] : target), 0, ln);
            else emit(c, SETLOCAL, 0, slot, ln);
            emit(c, CONST, konst(c, v_nil()), 0, ln);
            return;
        }
        if (strcmp(h, "call") == 0) {
            if (n < 3) cerr("call: (call <capability> \"op\" args...)", ln);
            for (int i = 1; i < n; i++) expr(x->list.items[i], s, c);
            emit(c, CALLCAP, n - 3, 0, ln);
            return;
        }
    }
    expr(head, s, c);
    for (int i = 1; i < n; i++) expr(x->list.items[i], s, c);
    emit(c, CALL, n - 1, 0, ln);
}

Code *bc_compile(Interp *it, Val *forms) {
    cit = it;
    Code *m = code_new("<module>", 0);
    Scope *s = scope_new(m, 0, true);
    if (forms->list.n == 0) emit(m, CONST, konst(m, v_nil()), 0, 0);
    for (int i = 0; i < forms->list.n; i++) { expr(forms->list.items[i], s, m); if (i < forms->list.n - 1) emit(m, POP, 0, 0, 0); }
    emit(m, RET, 0, 0, 0);
    return m;
}

/* ---------------------------------------------------------------- virtual machine */

#define STACK 256

static void at_line(Interp *it, const char *msg, int line) {
    char *m = kalloc(strlen(msg) + 24); strcpy(m, msg);
    if (line) { strcat(m, " at line "); utoa((u32)line, m + strlen(m)); }
    vs_error(it, m);
}

static Val *execute(Interp *it, Code *code, Frame *frame) {
    Val *stack[STACK]; int sp = 0, pc = 0;
    Ins *ins = code->ins; Val **consts = code->consts;
    for (;;) {
        if (++it->used > it->fuel) vs_error(it, "fuel exhausted");
        if (vm_preempt) { vm_preempt = false; task_yield(); }
        Ins *inp = &ins[pc++]; Ins in = *inp;
        switch (in.op) {
        case LOCAL: {
            Frame *f = frame; for (int d = 0; d < in.a; d++) f = f->parent;
            Val *v = f->slots[in.b];
            stack[sp++] = v ? v : v_nil();
            break;
        }
        case CONST: stack[sp++] = consts[in.a]; break;
        case CALL: {
            int argc = in.a;
            Val **argv = &stack[sp - argc];
            Val *fn = stack[sp - argc - 1];
            Val *r;
            if (fn->t == T_BFN) {
                Code *c2 = fn->bfn.code;
                if (argc != c2->nparams) at_line(it, "wrong number of arguments to function", in.line);
                Frame *f2 = kalloc(sizeof *f2); f2->slots = kalloc(sizeof(Val *) * (c2->nslots ? c2->nslots : 1)); f2->parent = fn->bfn.parent;
                for (int i = 0; i < argc; i++) f2->slots[i] = argv[i];
                if (++it->depth > VS_MAX_DEPTH) at_line(it, "call depth exceeded", in.line);
                r = execute(it, c2, f2);
                it->depth--;
            } else r = vs_apply(it, fn, argv, argc);
            sp -= argc + 1;
            stack[sp++] = r;
            break;
        }
        case GLOBAL: {
            Val *v;
            if (inp->cache && inp->gen == bc_gen) v = inp->cache;          /* resolved before, nothing redefined since */
            else {
                v = env_lookup(it->globals, consts[in.a]->s);
                if (!v) { char *m = kalloc(96); strcpy(m, "unbound symbol "); strcat(m, consts[in.a]->s); at_line(it, m, in.line); }
                inp->cache = v; inp->gen = bc_gen;
            }
            stack[sp++] = v;
            break;
        }
        case JUMPIFNOT: if (!truthy(stack[--sp])) pc = in.a; break;
        case JUMP: pc = in.a; break;
        case POP: sp--; break;
        case SETLOCAL: { Frame *f = frame; for (int d = 0; d < in.a; d++) f = f->parent; f->slots[in.b] = stack[--sp]; break; }
        case RET: return sp ? stack[sp - 1] : v_nil();
        case CALLCAP: {
            int argc = in.a;
            Val **argv = &stack[sp - argc];
            Val *op = stack[sp - argc - 1], *cap = stack[sp - argc - 2];
            if (cap->t != T_CAP) at_line(it, "call: first argument must be a capability", in.line);
            if (op->t != T_STR) at_line(it, "call: operation must be a string", in.line);
            if (!it->syscall) at_line(it, "no capabilities here", in.line);
            it->used += 10;
            Val *r = it->syscall(it, cap, op->s, argv, argc);
            sp -= argc + 2;
            stack[sp++] = r;
            break;
        }
        case CLOSURE: { Val *v = kalloc(sizeof *v); v->t = T_BFN; v->bfn.code = code->codes[in.a]; v->bfn.parent = frame; stack[sp++] = v; break; }
        case DUP: stack[sp] = stack[sp - 1]; sp++; break;
        case JUMPIF: if (truthy(stack[--sp])) pc = in.a; break;
        case SETGLOBAL: env_define(it->globals, consts[in.a]->s, stack[--sp]); bc_gen++; break;
        default: at_line(it, "bad instruction", in.line);
        }
        if (sp >= STACK - 2) at_line(it, "expression too deep", in.line);
    }
}

Val *bc_apply(Interp *it, Val *closure, Val **argv, int argc) {
    Code *c = closure->bfn.code;
    if (argc != c->nparams) vs_error(it, "wrong number of arguments to function");
    Frame *f = kalloc(sizeof *f); f->slots = kalloc(sizeof(Val *) * (c->nslots ? c->nslots : 1)); f->parent = closure->bfn.parent;
    for (int i = 0; i < argc; i++) f->slots[i] = argv[i];
    if (++it->depth > VS_MAX_DEPTH) vs_error(it, "call depth exceeded");
    Val *r = execute(it, c, f);
    it->depth--;
    return r;
}

Val *bc_run_module(Interp *it, Code *code, Env *globals) {
    it->error = 0; it->depth = 0; it->globals = globals;
    if (vs_setjmp(it->jmp)) return 0;
    Frame *f = kalloc(sizeof *f); f->slots = kalloc(sizeof(Val *) * (code->nslots ? code->nslots : 1)); f->parent = 0;
    return execute(it, code, f);
}

Val *bc_run_source(Interp *it, const char *src, Env *globals) {
    const char *perr;
    Val *forms = vs_parse(src, &perr);
    if (!forms) { it->error = perr; return 0; }
    it->error = 0; it->depth = 0; it->globals = globals;
    if (vs_setjmp(it->jmp)) return 0;
    Code *code = bc_compile(it, forms);
    Frame *f = kalloc(sizeof *f); f->slots = kalloc(sizeof(Val *) * (code->nslots ? code->nslots : 1)); f->parent = 0;
    return execute(it, code, f);
}
