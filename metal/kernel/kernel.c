#include "verum.h"

/* Every state change is an event line in the ledger. Booting replays the
 * ledger from disk; `rewind N` rebuilds from its first N lines and parks the
 * rest in the attic. Event forms ('|'-separated, data last so it may contain '|'):
 *
 *   genesis|storeTok|consoleTok|clockTok|kernelTok
 *   seat|name
 *   put|kind|by|op|tick|tag,tag|input,input|<data as Verum Script>
 *   spawn|cell|codeHash|fuel
 *   grant|parentTok|by|rights|holder|label|tok      holder: a cell or a seat
 *   mint|holder|target|label|tok                     a cell->target[send] capability
 *   revoke|tok|by
 *   send|from|to|<body as Verum Script>
 *   ran|cell|ok|fuel|tick|error
 *   kill|cell
 */

u32 k_tick = 0;
bool k_replaying = false;
static Cell *current = 0;          /* the cell whose stack we are on; 0 = the shell */
static u32 *sched_sp = 0;          /* the scheduler's saved stack pointer while a cell runs */
const char *k_me = SHELL;
static Cell cells[MAX_CELLS];
static Seat seats[MAX_SEATS];

static void copy_name(char *dst, const char *src) {
    usize n = strlen(src); if (n >= NAME_LEN) n = NAME_LEN - 1;
    memcpy(dst, src, n); dst[n] = 0;
}

static void record(const char *text) { if (!k_replaying) ledger_append(text); }

/* small line builder */
typedef struct { char *p; usize n, cap; } LB;
static void lb_add(LB *b, const char *s) { while (*s && b->n + 1 < b->cap) b->p[b->n++] = *s++; b->p[b->n] = 0; }
static void lb_sep(LB *b) { lb_add(b, "|"); }
static void lb_u(LB *b, u32 v) { char t[12]; utoa(v, t); lb_add(b, t); }
static LB lb_new(usize cap) { LB b = { kalloc(cap), 0, cap }; return b; }

static int split_fields(char *line, char **f, int n) {
    int k = 0; f[k++] = line;
    for (char *p = line; *p && k < n; p++) if (*p == '|') { *p = 0; f[k++] = p + 1; }
    return k;
}

/* ---------------------------------------------------------------- seats */

Seat *seat_find(const char *name) {
    for (int i = 0; i < MAX_SEATS; i++) if (seats[i].used && strcmp(seats[i].name, name) == 0) return &seats[i];
    return 0;
}

int seat_count(Seat **out, int cap) {
    int n = 0;
    for (int i = 0; i < MAX_SEATS && n < cap; i++) if (seats[i].used) out[n++] = &seats[i];
    return n;
}

static Seat *seat_internal(const char *name, const char **err) {
    if (seat_find(name) || cell_find(name)) { *err = "that name is taken"; return 0; }
    if (!name[0] || strlen(name) >= NAME_LEN) { *err = "bad seat name"; return 0; }
    for (int i = 0; i < MAX_SEATS; i++) if (!seats[i].used) {
        memset(&seats[i], 0, sizeof seats[i]);
        copy_name(seats[i].name, name); seats[i].used = true;
        for (int k = 0; k < K_COUNT; k++) seats[i].caps[k] = -1;
        return &seats[i];
    }
    *err = "too many seats"; return 0;
}

Seat *seat_create(const char *name, const char **err) {
    if (!i_may(K_KERNEL, R_MINT)) { *err = "you hold no kernel[mint] capability"; return 0; }
    Seat *s = seat_internal(name, err);
    if (!s) return 0;
    LB b = lb_new(64); lb_add(&b, "seat"); lb_sep(&b); lb_add(&b, name); record(b.p);
    return s;
}

int my_cap(int kind) { Seat *s = seat_find(k_me); return s ? s->caps[kind] : -1; }
bool i_may(int kind, u32 right) {
    Cap *c = cap_at(my_cap(kind));
    return c && !c->revoked && (c->rights & right);
}

/* ---------------------------------------------------------------- cells (internal) */

Cell *cell_find(const char *name) {
    for (int i = 0; i < MAX_CELLS; i++) if (cells[i].alive && strcmp(cells[i].name, name) == 0) return &cells[i];
    return 0;
}

int cell_count(Cell **out, int cap) {
    int n = 0;
    for (int i = 0; i < MAX_CELLS && n < cap; i++) if (cells[i].alive) out[n++] = &cells[i];
    return n;
}

static Cell *spawn_internal(const char *name, int code, u32 fuel, const char **err) {
    Obj *o = store_get(code);
    if (!o || strcmp(o->kind, "code") != 0) { *err = "not a code object"; return 0; }
    if (cell_find(name) || seat_find(name)) { *err = "that name is taken"; return 0; }
    if (strlen(name) >= NAME_LEN || !name[0]) { *err = "bad cell name"; return 0; }
    Cell *c = 0;
    for (int i = 0; i < MAX_CELLS; i++) if (!cells[i].alive) { c = &cells[i]; break; }
    if (!c) { *err = "too many cells"; return 0; }
    memset(c, 0, sizeof *c);
    copy_name(c->name, name);
    c->code = code; c->fuel = fuel;
    c->env = env_new(env_builtins());
    const char *perr; Val *forms = vs_parse(o->data->s, &perr);
    if (!forms) { *err = perr; return 0; }
    Interp it = { .fuel = fuel, .syscall = kernel_syscall, .ctx = c->name };
    if (!vs_run(&it, forms, c->env)) { *err = it.error; return 0; }
    c->alive = true;
    return c;
}

/* bind a capability into a holder: a cell's environment, or a seat's slot of that kind */
static bool bind_cap(const char *holder, const char *label, int cap_idx) {
    Cell *c = cell_find(holder);
    if (c) {
        heap_persist_begin();
        const char *name = kstrdup(label);             /* the caller's label may be a transient buffer */
        env_define(c->env, name, v_cap(cap_at(cap_idx)->token, name));
        heap_persist_end();
        return true;
    }
    Seat *s = seat_find(holder);
    if (s) { s->caps[cap_at(cap_idx)->kind] = cap_idx; return true; }
    return false;
}

static void inbox_push(Cell *c, const char *from, Val *body) {
    int slot = (c->in_head + c->in_count) % INBOX;
    copy_name(c->inbox[slot].from, from); c->inbox[slot].body = v_persist(body); c->in_count++;
}

void arena_overflow(void) {
    if (current && current->it) vs_error(current->it, "memory budget exceeded");
    panic("arena overflow outside a cell");
}

/* ---------------------------------------------------------------- public ops (record events) */

int kernel_put(const char *kind, Val *data, const char **tags, int ntags, const char *by,
               const char *op, const char (*inputs)[65], int ninputs) {
    int before = store_count();
    int idx = store_put(kind, data, tags, ntags, by, op, inputs, ninputs, k_tick);
    if (idx < 0 || idx < before) return idx;
    if (!k_replaying) {
        LB b = lb_new(VS_MAX_SIZE + 512);
        lb_add(&b, "put"); lb_sep(&b); lb_add(&b, kind); lb_sep(&b); lb_add(&b, by); lb_sep(&b); lb_add(&b, op);
        lb_sep(&b); lb_u(&b, k_tick); lb_sep(&b);
        for (int i = 0; i < ntags; i++) { if (i) lb_add(&b, ","); lb_add(&b, tags[i]); }
        lb_sep(&b);
        for (int i = 0; i < ninputs; i++) { if (i) lb_add(&b, ","); lb_add(&b, inputs[i]); }
        lb_sep(&b);
        v_serialize(data, b.p + b.n, b.cap - b.n); b.n = strlen(b.p);
        record(b.p);
    }
    return idx;
}

Cell *cell_spawn(const char *name, int code, const char **err) {
    if (!i_may(K_KERNEL, R_SPAWN)) { *err = "you hold no kernel[spawn] capability"; return 0; }
    Cell *c = spawn_internal(name, code, DEFAULT_FUEL, err);
    if (!c) return 0;
    LB b = lb_new(128);
    lb_add(&b, "spawn"); lb_sep(&b); lb_add(&b, name); lb_sep(&b); lb_add(&b, store_get(code)->hash); lb_sep(&b); lb_u(&b, DEFAULT_FUEL);
    record(b.p);
    return c;
}

bool cell_kill(const char *name) {
    if (!i_may(K_KERNEL, R_SPAWN)) return false;
    Cell *c = cell_find(name);
    if (!c) return false;
    c->alive = false;
    LB b = lb_new(64); lb_add(&b, "kill"); lb_sep(&b); lb_add(&b, name); record(b.p);
    return true;
}

bool kernel_grant(int parent_cap, const char *by, u32 rights, const char *holder, const char *label, const char **err) {
    if (!cell_find(holder) && !seat_find(holder)) { *err = "no such cell or seat"; return false; }
    int idx = cap_attenuate(parent_cap, by, rights, holder, 0, err);
    if (idx < 0) return false;
    bind_cap(holder, label, idx);
    char r[80]; cap_rights_csv(rights, r);
    LB b = lb_new(256);
    lb_add(&b, "grant"); lb_sep(&b); lb_add(&b, cap_at(parent_cap)->token); lb_sep(&b); lb_add(&b, by); lb_sep(&b);
    lb_add(&b, r); lb_sep(&b); lb_add(&b, holder); lb_sep(&b); lb_add(&b, label); lb_sep(&b); lb_add(&b, cap_at(idx)->token);
    record(b.p);
    return true;
}

bool cell_link(const char *src, const char *target, const char *label, const char **err) {
    if (!i_may(K_KERNEL, R_MINT)) { *err = "you hold no kernel[mint] capability"; return false; }
    if (!cell_find(src) || !cell_find(target)) { *err = "no such cell"; return false; }
    int mint = cap_mint(K_CELL, R_SEND | R_DELEGATE, k_me, target, -1, 0);
    if (mint < 0) { *err = "capability table full"; return false; }
    LB b = lb_new(160);
    lb_add(&b, "mint"); lb_sep(&b); lb_add(&b, k_me); lb_sep(&b); lb_add(&b, target); lb_sep(&b); lb_add(&b, label); lb_sep(&b); lb_add(&b, cap_at(mint)->token);
    record(b.p);
    return kernel_grant(mint, k_me, R_SEND, src, label, err);
}

bool kernel_revoke(int cap_idx, const char *by, const char **err) {
    const char *tok = cap_at(cap_idx) ? cap_at(cap_idx)->token : "";
    if (cap_revoke(cap_idx, by, err) < 0) return false;
    LB b = lb_new(96); lb_add(&b, "revoke"); lb_sep(&b); lb_add(&b, tok); lb_sep(&b); lb_add(&b, by); record(b.p);
    return true;
}

bool cell_send(const char *to, Val *body, const char *from, const char **err) {
    Cell *c = cell_find(to);
    if (!c) { *err = "no such cell"; return false; }
    if (c->in_count >= INBOX) { *err = "inbox full"; return false; }
    inbox_push(c, from, body);
    if (!k_replaying) {
        LB b = lb_new(VS_MAX_SIZE + 128);
        lb_add(&b, "send"); lb_sep(&b); lb_add(&b, from); lb_sep(&b); lb_add(&b, to); lb_sep(&b);
        v_serialize(body, b.p + b.n, b.cap - b.n); b.n = strlen(b.p);
        record(b.p);
    }
    return true;
}

/* ---------------------------------------------------------------- the syscall gate */

static Val *obj_to_val(Obj *o) {
    Val *d = v_dict();
    dict_set(d, "hash", v_str(o->hash)); dict_set(d, "kind", v_str(o->kind)); dict_set(d, "data", o->data);
    Val *tags = v_list(o->ntags); for (int i = 0; i < o->ntags; i++) list_push(tags, v_str(o->tags[i]));
    dict_set(d, "tags", tags); dict_set(d, "by", v_str(o->by));
    return d;
}

static void deny(Interp *it, const char *why) {
    char *m = kalloc(96); strcpy(m, "denied: "); strcat(m, why); vs_error(it, m);
}

Val *kernel_syscall(Interp *it, Val *cap, const char *op, Val **argv, int argc) {
    const char *caller = it->ctx, *err;
    u32 right = 0;
    for (int k = 0; k < K_COUNT; k++) right |= cap_parse_rights(op, k);
    if (!right) deny(it, "no such operation");
    Cap *c = cap_check(cap->cap.token, caller, right, &err);
    if (!c) deny(it, err);

    if (right == R_DELEGATE) {
        /* (call <cap> "delegate" <link-cap> "rights" "label"): hand a narrower copy to the linked cell */
        if (argc != 3 || argv[0]->t != T_CAP || argv[1]->t != T_STR || argv[2]->t != T_STR)
            vs_error(it, "delegate: (call cap \"delegate\" link \"rights\" \"label\")");
        Cap *link = cap_check(argv[0]->cap.token, caller, R_SEND, &err);
        if (!link) deny(it, err);
        if (link->kind != K_CELL) deny(it, "second argument must be a cell capability");
        u32 rights = cap_parse_rights(argv[1]->s, c->kind);
        if (!rights) deny(it, "unknown right for that capability");
        if (!kernel_grant(cap_index(c), caller, rights, link->target, argv[2]->s, &err)) deny(it, err);
        return v_nil();
    }
    switch (c->kind) {
    case K_CONSOLE: {
        vga_color(C_LCYAN, C_BLACK); kprintf("[%s] ", caller); vga_color(C_GREY, C_BLACK);
        char buf[200];
        for (int i = 0; i < argc; i++) { v_show(argv[i], buf, sizeof buf); kprintf("%s%s", i ? " " : "", buf); }
        kprintf("\n");
        return v_nil();
    }
    case K_CLOCK: return v_int((int)k_tick);
    case K_CELL:
        if (argc != 1) vs_error(it, "send: one argument");
        if (!cell_send(c->target, argv[0], caller, &err)) vs_error(it, err);
        return v_nil();
    case K_KERNEL: {
        if (right != R_CELLS) vs_error(it, "that kernel operation is only available from the shell");
        Cell *cs[MAX_CELLS]; int n = cell_count(cs, MAX_CELLS); Val *l = v_list(n);
        for (int i = 0; i < n; i++) list_push(l, v_str(cs[i]->name));
        return l;
    }
    case K_STORE:
        if (right == R_GET) {
            if (argc != 1 || argv[0]->t != T_STR) vs_error(it, "get: (call store \"get\" hash)");
            Obj *o = store_get(store_resolve(argv[0]->s));
            if (!o) vs_error(it, "no such object");
            return obj_to_val(o);
        }
        if (right == R_PUT) {
            if (argc < 2 || argv[0]->t != T_STR) vs_error(it, "put: (call store \"put\" kind data [tags] [inputs])");
            const char *tags[MAX_TAGS]; int ntags = 0;
            if (argc > 2 && argv[2]->t == T_LIST)
                for (int i = 0; i < argv[2]->list.n && ntags < MAX_TAGS; i++)
                    if (argv[2]->list.items[i]->t == T_STR) tags[ntags++] = argv[2]->list.items[i]->s;
            char inputs[MAX_INPUTS][65]; int nin = 0;
            if (argc > 3 && argv[3]->t == T_LIST)
                for (int i = 0; i < argv[3]->list.n && nin < MAX_INPUTS; i++) {
                    Val *h = argv[3]->list.items[i];
                    int idx = h->t == T_STR ? store_resolve(h->s) : -1;
                    if (idx < 0) vs_error(it, "put: unknown input object");
                    strcpy(inputs[nin++], store_get(idx)->hash);
                }
            int idx = kernel_put(argv[0]->s, argv[1], tags, ntags, caller, "run", inputs, nin);
            if (idx < 0) vs_error(it, "store full");
            return v_str(store_get(idx)->hash);
        }
        if (right == R_FIND) {
            if (argc != 1 || argv[0]->t != T_STR) vs_error(it, "find: one tag");
            int hits[64]; int n = store_find(argv[0]->s, hits, 64); Val *l = v_list(n);
            for (int i = 0; i < n; i++) list_push(l, v_str(store_get(hits[i])->hash));
            return l;
        }
        vs_error(it, "store: unknown operation");
    }
    vs_error(it, "unknown capability kind");
}

/* ---------------------------------------------------------------- scheduler
 *
 * Every cell with work runs on its own kernel stack. The scheduler (on the
 * shell's stack) hands each runnable cell a time slice with ctx_switch; when
 * the timer sees the slice expire it sets vm_preempt, and the VM calls
 * task_yield() at its next step, which switches back here. Switches therefore
 * happen only between VM steps: a syscall is never interleaved with another
 * cell's, so the ledger records effects in the exact order they occurred and
 * replay stays deterministic. A cell paused mid-message simply resumes on the
 * next slice, or on the next `run`. */

void task_yield(void) {
    Cell *c = current;
    if (!c) return;                                    /* shell-side eval: nothing to switch to */
    c->slices++;
    current = 0;
    ctx_switch(&c->sp, sched_sp);                      /* ...and we come back here when rescheduled */
}

static void run_one_message(Cell *c) {
    int slot = c->in_head; c->in_head = (c->in_head + 1) % INBOX; c->in_count--;
    c->arena_used = 0;                                 /* everything the previous message allocated is gone */
    Val *handler = env_lookup(c->env, "on-message");
    Interp it = { .fuel = c->fuel, .syscall = kernel_syscall, .ctx = c->name };
    c->it = &it;
    Val *args[2] = { v_str(c->inbox[slot].from), c->inbox[slot].body };
    bool ok = true; const char *err = "";
    c->mid_run = true;
    if (!handler || handler->t != T_FN) { ok = false; err = "cell defines no (on-message from body)"; }
    else if (!vs_call(&it, handler, args, 2)) { ok = false; err = it.error; }
    c->mid_run = false; c->it = 0;
    c->runs++; c->fuel_used += it.used;
    if (!ok) {
        c->errors++;
        vga_color(C_YELLOW, C_BLACK); kprintf("[kernel] cell %s stopped: %s\n", c->name, err); vga_color(C_GREY, C_BLACK);
    }
    heap_persist_begin();                              /* the arena may be full (that may be why the run ended) */
    LB b = lb_new(256);
    lb_add(&b, "ran"); lb_sep(&b); lb_add(&b, c->name); lb_sep(&b); lb_add(&b, ok ? "1" : "0"); lb_sep(&b);
    lb_u(&b, it.used); lb_sep(&b); lb_u(&b, k_tick); lb_sep(&b); lb_add(&b, err);
    record(b.p);
    heap_persist_end();
    c->arena_used = 0;
}

/* Entry point of a cell's stack: drain the inbox, then park forever. */
static void task_entry(void) {
    for (;;) {
        Cell *c = current;
        while (c->in_count) run_one_message(c);
        c->running = false;
        current = 0;
        ctx_switch(&c->sp, sched_sp);                  /* done: back to the scheduler, never resumed */
    }
}

static void task_start(Cell *c) {
    if (!c->stack) c->stack = kalloc(TASK_STACK);
    if (!c->arena) c->arena = kalloc(ARENA_SIZE);
    u32 *sp = (u32 *)(c->stack + TASK_STACK);
    *--sp = (u32)task_entry;                           /* ctx_switch's `ret` lands here */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;        /* ebp ebx esi edi */
    c->sp = sp;
    c->running = true;
}

int kernel_run(int max_rounds) {
    int rounds = 0;
    for (;;) {
        int runnable = 0;
        for (int i = 0; i < MAX_CELLS; i++) {
            Cell *c = &cells[i];
            if (!c->alive) continue;
            if (!c->running && c->in_count) task_start(c);
            if (c->running) runnable++;
        }
        if (!runnable || rounds >= max_rounds) return rounds;
        k_tick++; rounds++;
        for (int i = 0; i < MAX_CELLS; i++) {
            Cell *c = &cells[i];
            if (!c->alive || !c->running) continue;
            current = c;
            heap_arena_use(c->arena, ARENA_SIZE, &c->arena_used);
            vm_preempt = false; vm_deadline = uptime_ticks + SLICE_TICKS;
            ctx_switch(&sched_sp, c->sp);              /* run the cell until it yields or finishes */
            vm_deadline = 0; vm_preempt = false;
            heap_arena_use(0, 0, 0);
            if (c->running && runnable > 1) {          /* paused mid-run with others waiting: show the switch */
                vga_color(C_DGREY, C_BLACK); kprintf("[sched] %s paused after its slice (round %d)\n", c->name, rounds); vga_color(C_GREY, C_BLACK);
            }
        }
    }
}

/* ---------------------------------------------------------------- genesis, replay, rewind, attic */

static void genesis(void) {
    const char *err;
    Seat *sh = seat_internal(SHELL, &err);
    LB b = lb_new(200); lb_add(&b, "genesis");
    for (int k = 0; k < K_COUNT; k++) {
        if (k == K_CELL) continue;
        sh->caps[k] = cap_mint(k, cap_rights_of_kind(k), SHELL, 0, -1, 0);
        lb_sep(&b); lb_add(&b, cap_at(sh->caps[k])->token);
    }
    record(b.p);
    for (int i = 0; i < N_BUILTIN_CELLS; i++) {
        const char *tags[2] = { "code", BUILTIN_CELLS[i].name };
        kernel_put("code", v_str(BUILTIN_CELLS[i].src), tags, 2, SHELL, "code", 0, 0);
    }
}

static bool apply(const char *text) {
    char *line = kstrdup(text); char *f[8]; const char *err;
    if (starts_with(line, "genesis|")) {
        if (split_fields(line, f, 5) != 5) return false;
        Seat *sh = seat_internal(SHELL, &err);
        if (!sh) return false;
        int kinds[4] = { K_STORE, K_CONSOLE, K_CLOCK, K_KERNEL };
        for (int i = 0; i < 4; i++) sh->caps[kinds[i]] = cap_mint(kinds[i], cap_rights_of_kind(kinds[i]), SHELL, 0, -1, f[1 + i]);
        return true;
    }
    if (starts_with(line, "seat|")) {
        if (split_fields(line, f, 2) != 2) return false;
        return seat_internal(f[1], &err) != 0;
    }
    if (starts_with(line, "put|")) {
        if (split_fields(line, f, 8) != 8) return false;
        const char *tags[MAX_TAGS]; int ntags = 0;
        for (char *p = f[5]; *p && ntags < MAX_TAGS;) { tags[ntags++] = p; while (*p && *p != ',') p++; if (*p) *p++ = 0; }
        char inputs[MAX_INPUTS][65]; int nin = 0;
        for (char *p = f[6]; *p && nin < MAX_INPUTS;) { char *s = p; while (*p && *p != ',') p++; if (*p) *p++ = 0; if (strlen(s) == 64) strcpy(inputs[nin++], s); }
        Val *data = v_deserialize(f[7]);
        if (!data) return false;
        return store_put(f[1], data, tags, ntags, f[2], f[3], inputs, nin, (u32)atoi(f[4])) >= 0;
    }
    if (starts_with(line, "spawn|")) {
        if (split_fields(line, f, 4) != 4) return false;
        return spawn_internal(f[1], store_resolve(f[2]), (u32)atoi(f[3]), &err) != 0;
    }
    if (starts_with(line, "grant|")) {
        if (split_fields(line, f, 7) != 7) return false;
        Cap *parent = cap_by_token(f[1]);
        if (!parent) return false;
        int idx = cap_attenuate(cap_index(parent), f[2], cap_parse_rights(f[3], parent->kind), f[4], f[6], &err);
        if (idx < 0) return false;
        return bind_cap(f[4], f[5], idx);
    }
    if (starts_with(line, "mint|")) {
        if (split_fields(line, f, 5) != 5) return false;
        return cap_mint(K_CELL, R_SEND | R_DELEGATE, f[1], f[2], -1, f[4]) >= 0;
    }
    if (starts_with(line, "revoke|")) {
        if (split_fields(line, f, 3) != 3) return false;
        Cap *c = cap_by_token(f[1]);
        return c && cap_revoke(cap_index(c), f[2], &err) >= 0;
    }
    if (starts_with(line, "send|")) {
        if (split_fields(line, f, 4) != 4) return false;
        Cell *c = cell_find(f[2]); Val *body = v_deserialize(f[3]);
        if (!c || !body || c->in_count >= INBOX) return false;
        inbox_push(c, f[1], body);
        return true;
    }
    if (starts_with(line, "ran|")) {
        if (split_fields(line, f, 6) != 6) return false;
        Cell *c = cell_find(f[1]);
        if (!c) return false;
        if (c->in_count) { c->in_head = (c->in_head + 1) % INBOX; c->in_count--; }
        c->runs++; c->fuel_used += (u32)atoi(f[3]); if (f[2][0] == '0') c->errors++;
        k_tick = (u32)atoi(f[4]);
        return true;
    }
    if (starts_with(line, "kill|")) {
        if (split_fields(line, f, 2) != 2) return false;
        Cell *c = cell_find(f[1]); if (c) c->alive = false;
        return true;
    }
    return false;
}

static void reset_state(void) {
    store_reset(); caps_reset(); ledger_reset();
    for (int i = 0; i < MAX_CELLS; i++) cells[i].alive = false;
    for (int i = 0; i < MAX_SEATS; i++) seats[i].used = false;
    k_tick = 0; k_me = SHELL;
}

/* Replay "\n"-separated lines. `persist`: also write each line to the disk ledger (used by restore). */
static int replay_text(const char *lines, int max_entries, bool persist) {
    int n = 0;
    k_replaying = true; ledger_set_loading(!persist);
    const char *p = lines;
    while (*p && n < max_entries) {
        const char *e = p; while (*e && *e != '\n') e++;
        usize len = (usize)(e - p);
        char *line = kalloc(len + 1); memcpy(line, p, len); line[len] = 0;
        ledger_append(line);
        if (!apply(line)) { vga_color(C_LRED, C_BLACK); kprintf("[ledger] entry %d could not be applied: %s\n", n, line); vga_color(C_GREY, C_BLACK); }
        n++;
        p = *e ? e + 1 : e;
    }
    k_replaying = false; ledger_set_loading(false);
    return n;
}

void kernel_boot(void) {
    reset_state();
    bool disk = disk_init();
    if (disk && disk_entries() > 0) {
        char *all = disk_read_all();
        if (all) {
            int n = replay_text(all, MAX_LEDGER, false);
            char why[80]; ledger_verify(why, sizeof why);
            kprintf("[disk] replayed %d ledger entries: %s; attic holds %u branch(es)\n", n, why, disk_attic_branches());
            return;
        }
        kprintf("[disk] present but unreadable; starting from genesis\n");
    } else if (disk) {
        kprintf("[disk] blank image formatted; the ledger will persist\n");
    } else {
        kprintf("[disk] no disk; state lives in RAM for this session\n");
    }
    genesis();
}

bool kernel_rewind(int upto, const char **err) {
    int total = ledger_count();
    if (upto < 1 || upto > total) { *err = "entry number out of range"; return false; }
    /* 1. park the tail in the attic so nothing is lost */
    if (disk_present()) {
        LB h = lb_new(96);
        lb_add(&h, "branch"); lb_sep(&h); lb_u(&h, (u32)upto); lb_sep(&h); lb_u(&h, (u32)(total - upto)); lb_sep(&h); lb_u(&h, uptime_ms());
        if (!disk_attic_append(h.p, true)) { *err = "attic is full; rewind refused so nothing is lost"; return false; }
        for (int i = upto; i < total; i++) if (!disk_attic_append(ledger_entry(i)->text, false)) { *err = "attic write failed"; return false; }
    }
    /* 2. rebuild memory from the kept prefix */
    usize bytes = 0;
    for (int i = 0; i < upto; i++) bytes += strlen(ledger_entry(i)->text) + 1;
    char *buf = kalloc(bytes + 1); usize n = 0;
    for (int i = 0; i < upto; i++) { const char *t = ledger_entry(i)->text; usize l = strlen(t); memcpy(buf + n, t, l); n += l; buf[n++] = '\n'; }
    buf[n] = 0;
    u32 disk_bytes_after = ledger_entry(upto - 1)->bytes;
    reset_state();
    replay_text(buf, upto, false);
    /* 3. shorten the on-disk ledger */
    if (disk_present() && !disk_truncate(disk_bytes_after, (u32)upto)) { *err = "disk truncate failed"; return false; }
    return true;
}

/* walk the attic stream; returns the branch headers and, for branch k, its lines */
static int attic_walk(Branch *out, int cap, int want, char **lines_out) {
    char *all = disk_attic_read_all();
    if (!all) return 0;
    int n = 0; char *p = all;
    while (*p) {
        char *e = p; while (*e && *e != '\n') e++;
        bool last = !*e;
        usize len = (usize)(e - p);
        char *line = kalloc(len + 1); memcpy(line, p, len); line[len] = 0;   /* copy: the stream itself must stay intact */
        if (starts_with(line, "branch|")) {
            char *f[4];
            if (split_fields(line, f, 4) == 4 && n < cap) {
                out[n].from = (u32)atoi(f[1]); out[n].count = (u32)atoi(f[2]); out[n].uptime = (u32)atoi(f[3]); out[n].first = "";
                if (n == want && lines_out) *lines_out = last ? e : e + 1;
                n++;
            }
        } else if (n > 0 && !out[n - 1].first[0]) out[n - 1].first = line;
        p = last ? e : e + 1;
    }
    return n;
}

int attic_list(Branch *out, int cap) { return attic_walk(out, cap, -1, 0); }

bool attic_restore(int k, const char **err) {
    Branch br[32]; char *lines = 0;
    int n = attic_walk(br, 32, k, &lines);
    if (k < 0 || k >= n || !lines) { *err = "no such branch"; return false; }
    if ((u32)ledger_count() != br[k].from) { *err = "the ledger is not at that branch's starting point (rewind there first)"; return false; }
    /* lines run until the next "branch|" header or the end of the attic */
    char *end = lines; u32 seen = 0;
    while (*end && seen < br[k].count) { while (*end && *end != '\n') end++; if (*end) end++; seen++; }
    char *buf = kalloc((usize)(end - lines) + 1); memcpy(buf, lines, (usize)(end - lines)); buf[end - lines] = 0;
    replay_text(buf, (int)br[k].count, true);         /* re-applied and re-appended to the disk ledger */
    return true;
}
