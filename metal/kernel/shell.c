/* The shell: the human's seat. Not a terminal over a filesystem - there is none.
   Row 0 of the screen is a live status bar; `dash` opens a full-screen panel view.
   `k_me` names the seat currently at the keyboard; `switch` changes it. */
#include "verum.h"

#define LINE 256
#define MAXARGS 12

static const char *HELP =
"intents:\n"
"  note <text>              store a note        find <tag>        objects by tag\n"
"  code                     built-in code       show <hash>       inspect an object\n"
"  why <hash>               provenance tree     tags              all tags\n"
"  spawn <cell> <code>      new cell, NO caps   kill <cell>\n"
"  grant <who> <cap> [r,r]  narrowed copy of your cap to a cell or seat   e.g. grant wc store get,put\n"
"  link <a> <b> [label]     let a send to b     revoke <who> <label>\n"
"  send <cell> <expr|@tag>  queue a message     run [ticks]       drive the scheduler\n"
"  cells / caps / mem       state               eval <expr>       Verum Script, your caps\n"
"  seat <name> / seats      another human       switch <name>     take that seat   whoami\n"
"  ledger [n] / verify      tamper-evident history (persisted to disk)\n"
"  history / rewind <n>     timeline; go back to entry n (asks; the rest goes to the attic)\n"
"  attic / restore <k>      branches cut off by rewind; bring one back\n"
"  dash                     panel view          demo              guided tour\n"
"  uptime                   timer since boot    trap              raise a CPU exception (halts)\n";

static void say(const char *s) { vga_puts(s); vga_putc('\n'); }
static void refuse(const char *s) { vga_color(C_LRED, C_BLACK); kprintf("refused: %s\n", s); vga_color(C_GREY, C_BLACK); }
static void head(const char *s) { vga_color(C_WHITE, C_BLACK); vga_puts(s); vga_color(C_GREY, C_BLACK); }

/* ---------------------------------------------------------------- status bar */

static void status(void) {
    char bar[100]; char n[12];
    Cell *cs[MAX_CELLS]; int ncell = cell_count(cs, MAX_CELLS);
    strcpy(bar, " VERUM  "); strcat(bar, k_me);
    strcat(bar, "  objects "); utoa((u32)store_count(), n); strcat(bar, n);
    strcat(bar, "  cells "); utoa((u32)ncell, n); strcat(bar, n);
    strcat(bar, "  ledger "); utoa((u32)ledger_count(), n); strcat(bar, n);
    strcat(bar, "  tick "); utoa(k_tick, n); strcat(bar, n);
    strcat(bar, "  up "); utoa(uptime_ms() / 1000, n); strcat(bar, n); strcat(bar, "s");
    strcat(bar, "  disk: ");
    if (disk_present()) { utoa(disk_entries(), n); strcat(bar, n); strcat(bar, " saved"); if (disk_attic_branches()) { strcat(bar, " +"); utoa(disk_attic_branches(), n); strcat(bar, n); strcat(bar, " attic"); } }
    else strcat(bar, "none");
    vga_status(bar);
}

/* ---------------------------------------------------------------- helpers */

static int split_args(char *line, char **argv) {
    int n = 0; char *p = line;
    while (*p && n < MAXARGS) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { p++; argv[n++] = p; while (*p && *p != '"') p++; }
        else { argv[n++] = p; while (*p && *p != ' ') p++; }
        if (*p) *p++ = 0;
    }
    return n;
}

static int resolve_obj(const char *ref) {
    int i = store_resolve(ref);
    if (i < 0) i = store_find_code(ref);
    return i;
}

static void preview(Val *v, char *out, usize cap) {
    v_show(v, out, cap);
    for (char *p = out; *p; p++) if (*p == '\n') *p = ' ';
    if (strlen(out) >= cap - 1) { out[cap - 4] = '.'; out[cap - 3] = '.'; out[cap - 2] = '.'; }
}

static void short_hash(const char *h, char *out, int n) { memcpy(out, h, (usize)n); out[n] = 0; }

static int split_fields(char *line, char **f, int n) {   /* mirrors kernel.c */
    int k = 0; f[k++] = line;
    for (char *p = line; *p && k < n; p++) if (*p == '|') { *p = 0; f[k++] = p + 1; }
    return k;
}

/* "put note by shell 3c1f..." style summary of an event line, for humans. Always fits in `cap`. */
static void summarize(const char *text, char *out, usize cap) {
    char tmp[256]; char *line = kstrdup(text); char *f[8]; usize n;
    if (starts_with(line, "put|")) {
        split_fields(line, f, 8);
        strcpy(tmp, "put "); strcat(tmp, f[1]); strcat(tmp, " by "); strcat(tmp, f[2]); strcat(tmp, ": ");
        n = strlen(tmp); Val *d = v_deserialize(f[7]);
        if (d) preview(d, tmp + n, sizeof tmp - n); else strcat(tmp, "?");
    } else if (starts_with(line, "genesis|")) strcpy(tmp, "genesis: shell seated with store console clock kernel");
    else if (starts_with(line, "seat|")) { split_fields(line, f, 2); strcpy(tmp, "seat "); strcat(tmp, f[1]); }
    else if (starts_with(line, "grant|")) { split_fields(line, f, 7); strcpy(tmp, "grant "); strcat(tmp, f[4]); strcat(tmp, " "); strcat(tmp, f[5]); strcat(tmp, "["); strcat(tmp, f[3]); strcat(tmp, "] by "); strcat(tmp, f[2]); }
    else if (starts_with(line, "mint|")) { split_fields(line, f, 5); strcpy(tmp, "mint cell->"); strcat(tmp, f[2]); strcat(tmp, "[send] for "); strcat(tmp, f[1]); }
    else if (starts_with(line, "spawn|")) { split_fields(line, f, 4); strcpy(tmp, "spawn "); strcat(tmp, f[1]); strcat(tmp, " code "); char h[13]; short_hash(f[2], h, 12); strcat(tmp, h); }
    else if (starts_with(line, "send|")) { split_fields(line, f, 4); strcpy(tmp, "send "); strcat(tmp, f[1]); strcat(tmp, " -> "); strcat(tmp, f[2]); strcat(tmp, ": "); n = strlen(tmp); Val *d = v_deserialize(f[3]); if (d) preview(d, tmp + n, sizeof tmp - n); }
    else if (starts_with(line, "ran|")) { split_fields(line, f, 6); strcpy(tmp, "ran "); strcat(tmp, f[1]); strcat(tmp, f[2][0] == '1' ? " ok fuel " : " ERROR fuel "); strcat(tmp, f[3]); if (f[5][0]) { strcat(tmp, ": "); n = strlen(tmp); usize m = strlen(f[5]); if (n + m >= sizeof tmp) m = sizeof tmp - 1 - n; memcpy(tmp + n, f[5], m); tmp[n + m] = 0; } }
    else if (starts_with(line, "revoke|")) { split_fields(line, f, 3); strcpy(tmp, "revoke by "); strcat(tmp, f[2]); }
    else if (starts_with(line, "kill|")) { split_fields(line, f, 2); strcpy(tmp, "kill "); strcat(tmp, f[1]); }
    else { n = strlen(text); if (n >= sizeof tmp) n = sizeof tmp - 1; memcpy(tmp, text, n); tmp[n] = 0; }
    n = strlen(tmp); if (n >= cap) { n = cap - 1; if (n >= 3) { tmp[n - 1] = '.'; tmp[n - 2] = '.'; tmp[n - 3] = '.'; } }
    memcpy(out, tmp, n); out[n] = 0;
}

static void show_cells(void) {
    Cell *cs[MAX_CELLS]; int n = cell_count(cs, MAX_CELLS);
    if (!n) say("no cells");
    for (int i = 0; i < n; i++) {
        Cell *c = cs[i];
        kprintf("  %s  runs %u errors %u fuel %u inbox %d%s%s\n", c->name, c->runs, c->errors, c->fuel_used, c->in_count,
                c->mid_run ? "  PAUSED mid-run" : "", c->slices ? "" : "");
        if (c->slices) kprintf("      preempted %u time(s)\n", c->slices);
        for (Env *b = c->env->next; b; b = b->next)
            if (b->val->t == T_CAP) {
                Cap *cap = cap_by_token(b->val->cap.token); char d[96];
                if (cap) { cap_describe(cap, d, sizeof d); kprintf("      %s=%s\n", b->name, d); }
            }
    }
}

static void show_seats(void) {
    Seat *ss[MAX_SEATS]; int n = seat_count(ss, MAX_SEATS);
    for (int i = 0; i < n; i++) {
        kprintf("  %s%s\n", ss[i]->name, strcmp(ss[i]->name, k_me) == 0 ? "  (you)" : "");
        for (int k = 0; k < K_COUNT; k++) if (ss[i]->caps[k] >= 0) { char d[96]; cap_describe(cap_at(ss[i]->caps[k]), d, sizeof d); kprintf("      %s\n", d); }
    }
}

static Val *eval_pure(const char *src, const char **err) {
    Val *forms = vs_parse(src, err);
    if (!forms) return 0;
    Interp it = { .fuel = 20000, .syscall = 0, .ctx = (void *)k_me };
    Val *r = vs_run(&it, forms, env_new(env_builtins()));
    if (!r) { *err = it.error; return 0; }
    return r;
}

static int cap_kind_by_name(const char *s) {
    for (int k = 0; k < K_COUNT; k++) if (strcmp(cap_kind_name(k), s) == 0) return k;
    return -1;
}

static void cmd_why(int idx, int depth) {
    Obj *o = store_get(idx);
    if (!o || depth > 8) return;
    char pv[40]; preview(o->data, pv, sizeof pv);
    for (int i = 0; i < depth; i++) vga_puts("    ");
    char h[13]; short_hash(o->hash, h, 12);
    kprintf("  %s %s <- %s via %s @tick %u: %s\n", h, o->kind, o->by, o->op, o->tick, pv);
    for (int i = 0; i < o->ninputs; i++) cmd_why(store_resolve(o->inputs[i]), depth + 1);
}

/* the capability bound under `label` in a cell or seat, as a table index; -1 if none */
static int holder_cap(const char *holder, const char *label) {
    Cell *c = cell_find(holder);
    if (c) { Val *v = env_lookup(c->env, label); return (v && v->t == T_CAP && cap_by_token(v->cap.token)) ? cap_index(cap_by_token(v->cap.token)) : -1; }
    Seat *s = seat_find(holder); int k = cap_kind_by_name(label);
    return (s && k >= 0) ? s->caps[k] : -1;
}

/* ---------------------------------------------------------------- dash: the panel view */

static void dash(void) {
    vga_clear();
    head("  CELLS                                 |  LEDGER (newest last)\n");
    Cell *cs[MAX_CELLS]; int n = cell_count(cs, MAX_CELLS);
    int row = 2;
    int total = ledger_count(), first = total > 18 ? total - 18 : 0;
    for (int i = 0; i < 18; i++) {
        vga_at(row + i, 0);
        if (i < n) {
            Cell *c = cs[i];
            vga_color(c->errors ? C_YELLOW : C_LGREEN, C_BLACK);
            kprintf("  %s", c->name);
            vga_color(C_GREY, C_BLACK);
            vga_at(row + i, 14); kprintf("runs %u err %u", c->runs, c->errors);
            char caps[40] = ""; int k = 0;
            for (Env *b = c->env->next; b; b = b->next)
                if (b->val->t == T_CAP && k < 3) { if (k++) strcat(caps, ","); Cap *cp = cap_by_token(b->val->cap.token); if (cp && cp->revoked) strcat(caps, "!"); strcat(caps, b->name); }
            vga_at(row + i, 29); char cut[11]; memcpy(cut, caps, 10); cut[10] = 0; vga_puts(cut);
        } else if (i == n && n == 0) { vga_at(row, 2); vga_puts("(no cells)"); }
        vga_at(row + i, 40); vga_puts("|");
        if (first + i < total) {
            Entry *e = ledger_entry(first + i); char s[40]; summarize(e->text, s, 36);
            vga_at(row + i, 42); kprintf("#%u %s", e->seq, s);
        }
    }
    vga_at(21, 0);
    head("  STORE  "); kprintf("%d objects  ", store_count());
    int hits[64]; int nn = store_find("note", hits, 64); kprintf("notes %d  ", nn);
    nn = store_find("stats", hits, 64); kprintf("stats %d  ", nn);
    nn = store_find("code", hits, 64); kprintf("code %d  ", nn);
    Seat *ss[MAX_SEATS];
    kprintf("caps %d  seats %d  heap %u KiB\n", cap_count(), seat_count(ss, MAX_SEATS), (u32)(heap_used() / 1024));
    vga_at(22, 0);
    head("  DISK   ");
    if (disk_present()) kprintf("%u entries saved (%u bytes); attic %u branch(es); ledger head %c%c%c%c%c%c%c%c\n", disk_entries(), disk_bytes(), disk_attic_branches(),
        ledger_head()[0], ledger_head()[1], ledger_head()[2], ledger_head()[3], ledger_head()[4], ledger_head()[5], ledger_head()[6], ledger_head()[7]);
    else kprintf("no disk attached; state is lost at power-off\n");
    vga_at(24, 0);
    vga_color(C_DGREY, C_BLACK); vga_puts("  press any key to return to the shell"); vga_color(C_GREY, C_BLACK);
    kbd_getc();
    vga_clear();
}

/* ---------------------------------------------------------------- commands */

static void demo(void);

static bool dispatch(char *line) {
    char raw[LINE]; strcpy(raw, line);
    char *argv[MAXARGS]; int argc = split_args(line, argv);
    if (!argc) return true;
    const char *cmd = argv[0], *err = "";
    const char *rest = raw; while (*rest && *rest != ' ') rest++; while (*rest == ' ') rest++;

    if (!strcmp(cmd, "help")) { vga_puts(HELP); return true; }
    if (!strcmp(cmd, "demo")) { demo(); return true; }
    if (!strcmp(cmd, "dash")) { dash(); return true; }
    if (!strcmp(cmd, "whoami")) { kprintf("%s\n", k_me); return true; }
    if (!strcmp(cmd, "uptime")) { kprintf("up %u ms (%u timer ticks at 100 Hz); cells get %d ms per message\n", uptime_ms(), uptime_ticks, SLICE_TICKS * 10); return true; }
    if (!strcmp(cmd, "trap")) {
        say("raising a divide-by-zero in the kernel to show the exception handler...");
        __asm__ volatile("xor %%eax, %%eax; div %%eax" ::: "eax", "edx");
        return true;
    }
    if (!strcmp(cmd, "note")) {
        if (!*rest) { refuse("note needs text"); return true; }
        if (!i_may(K_STORE, R_PUT)) { refuse("you hold no store[put] capability"); return true; }
        const char *tags[1] = { "note" };
        int i = kernel_put("note", v_str(rest), tags, 1, k_me, "note", 0, 0);
        if (i < 0) refuse("store full"); else { char h[13]; short_hash(store_get(i)->hash, h, 12); kprintf("stored note %s\n", h); }
        return true;
    }
    if (!strcmp(cmd, "code")) {
        for (int i = 0; i < N_BUILTIN_CELLS; i++) {
            int idx = store_find_code(BUILTIN_CELLS[i].name); char h[13];
            if (idx >= 0) { short_hash(store_get(idx)->hash, h, 12); kprintf("  %s  code  %s\n", h, BUILTIN_CELLS[i].name); }
        }
        return true;
    }
    if (!strcmp(cmd, "find")) {
        if (argc < 2) { refuse("find <tag>"); return true; }
        if (!i_may(K_STORE, R_FIND)) { refuse("you hold no store[find] capability"); return true; }
        int hits[64]; int n = store_find(argv[1], hits, 64);
        if (!n) kprintf("nothing tagged '%s'\n", argv[1]);
        for (int i = 0; i < n; i++) {
            Obj *o = store_get(hits[i]); char h[13]; short_hash(o->hash, h, 12); char pv[44]; preview(o->data, pv, sizeof pv);
            kprintf("  %s  %s  by %s  %s\n", h, o->kind, o->by, pv);
        }
        return true;
    }
    if (!strcmp(cmd, "tags")) {
        for (int i = 0; i < store_count(); i++) {
            Obj *o = store_get(i);
            for (int t = 0; t < o->ntags; t++) {
                bool seen = false;
                for (int j = 0; j < i && !seen; j++) for (int u = 0; u < store_get(j)->ntags; u++) if (!strcmp(store_get(j)->tags[u], o->tags[t])) seen = true;
                for (int u = 0; u < t && !seen; u++) if (!strcmp(o->tags[u], o->tags[t])) seen = true;
                if (!seen) { int hits[64]; kprintf("  %s  %d\n", o->tags[t], store_find(o->tags[t], hits, 64)); }
            }
        }
        return true;
    }
    if (!strcmp(cmd, "show")) {
        if (!i_may(K_STORE, R_GET)) { refuse("you hold no store[get] capability"); return true; }
        int i = argc > 1 ? resolve_obj(argv[1]) : -1; Obj *o = store_get(i);
        if (!o) { refuse("no unique object with that prefix"); return true; }
        char buf[400]; v_show(o->data, buf, sizeof buf);
        kprintf("object %s\n  kind %s  by %s via %s at tick %u\n  tags", o->hash, o->kind, o->by, o->op, o->tick);
        for (int t = 0; t < o->ntags; t++) kprintf(" %s", o->tags[t]);
        kprintf("\n  inputs %d\n  data %s\n", o->ninputs, buf);
        return true;
    }
    if (!strcmp(cmd, "why")) {
        if (!i_may(K_STORE, R_GET)) { refuse("you hold no store[get] capability"); return true; }
        int i = argc > 1 ? resolve_obj(argv[1]) : -1;
        if (i < 0) refuse("no unique object with that prefix"); else cmd_why(i, 0);
        return true;
    }
    if (!strcmp(cmd, "spawn")) {
        if (argc < 3) { refuse("spawn <cell> <code>"); return true; }
        int code = resolve_obj(argv[2]);
        if (code < 0) { refuse("no such code object"); return true; }
        if (!cell_spawn(argv[1], code, &err)) refuse(err);
        else kprintf("cell %s spawned with NO capabilities. grant %s <cap> [rights]\n", argv[1], argv[1]);
        return true;
    }
    if (!strcmp(cmd, "kill")) { if (argc < 2 || !cell_kill(argv[1])) refuse("no such cell, or no kernel[spawn] capability"); else say("removed"); return true; }
    if (!strcmp(cmd, "grant")) {
        if (argc < 3) { refuse("grant <cell|seat> <cap> [rights]"); return true; }
        int kind = cap_kind_by_name(argv[2]);
        int mine = kind >= 0 ? my_cap(kind) : -1;
        if (mine < 0) { refuse("you hold no such capability"); return true; }
        u32 rights = argc > 3 ? cap_parse_rights(argv[3], kind) : cap_at(mine)->rights;
        if (!rights) { refuse("unknown right for that capability"); return true; }
        if (!kernel_grant(mine, k_me, rights, argv[1], argv[2], &err)) refuse(err);
        else { char d[96]; cap_describe(cap_at(holder_cap(argv[1], argv[2])), d, sizeof d); kprintf("granted %s\n", d); }
        return true;
    }
    if (!strcmp(cmd, "link")) {
        if (argc < 3) { refuse("link <cell> <target> [label]"); return true; }
        const char *label = argc > 3 ? argv[3] : argv[2];
        if (!cell_link(argv[1], argv[2], label, &err)) refuse(err); else kprintf("%s may now send to %s (bound as %s)\n", argv[1], argv[2], label);
        return true;
    }
    if (!strcmp(cmd, "revoke")) {
        if (argc < 3) { refuse("revoke <cell|seat> <label>"); return true; }
        int idx = holder_cap(argv[1], argv[2]);
        if (idx < 0) { refuse("no such capability there"); return true; }
        if (!kernel_revoke(idx, k_me, &err)) refuse(err);
        else kprintf("revoked %s.%s and everything derived from it\n", argv[1], argv[2]);
        return true;
    }
    if (!strcmp(cmd, "seat")) {
        if (argc < 2) { refuse("seat <name>"); return true; }
        if (!seat_create(argv[1], &err)) refuse(err);
        else kprintf("seat %s created with NO capabilities. grant %s <cap> [rights], then: switch %s\n", argv[1], argv[1], argv[1]);
        return true;
    }
    if (!strcmp(cmd, "seats")) { show_seats(); return true; }
    if (!strcmp(cmd, "switch")) {
        if (argc < 2) { refuse("switch <seat>"); return true; }
        Seat *s = seat_find(argv[1]);
        if (!s) { refuse("no such seat"); return true; }
        k_me = s->name;
        kprintf("you are now %s\n", k_me);
        return true;
    }
    if (!strcmp(cmd, "send")) {
        if (argc < 2) { refuse("send <cell> <expr|@tag>"); return true; }
        const char *e = rest; while (*e && *e != ' ') e++; while (*e == ' ') e++;
        Val *body;
        if (*e == '@') {
            int hits[64]; int n = store_find(e + 1, hits, 64);
            if (!n) { refuse("nothing has that tag"); return true; }
            body = v_str(store_get(hits[n - 1])->hash);
        } else body = *e ? eval_pure(e, &err) : v_nil();
        if (!body) { refuse(err); return true; }
        if (!cell_send(argv[1], body, k_me, &err)) refuse(err);
        else { char pv[60]; preview(body, pv, sizeof pv); kprintf("queued for %s: %s\n", argv[1], pv); }
        return true;
    }
    if (!strcmp(cmd, "run")) {
        int t = kernel_run(argc > 1 ? atoi(argv[1]) : 100);
        Cell *cs[MAX_CELLS]; int n = cell_count(cs, MAX_CELLS), paused = 0;
        for (int i = 0; i < n; i++) if (cs[i]->running) paused++;
        kprintf("ran %d round(s); now at tick %u%s\n", t, k_tick, paused ? " - some cells are paused mid-run; `run` again to continue" : "");
        return true;
    }
    if (!strcmp(cmd, "cells")) { show_cells(); return true; }
    if (!strcmp(cmd, "caps")) {
        for (int k = 0; k < K_COUNT; k++) if (my_cap(k) >= 0) { char d[96]; cap_describe(cap_at(my_cap(k)), d, sizeof d); kprintf("  %s\n", d); }
        return true;
    }
    if (!strcmp(cmd, "mem")) { kprintf("heap used %u KiB, %d objects, %d caps, %d ledger entries, disk %u entries, attic %u branches (%u bytes)\n", (u32)(heap_used() / 1024), store_count(), cap_count(), ledger_count(), disk_entries(), disk_attic_branches(), disk_attic_bytes()); return true; }
    if (!strcmp(cmd, "eval")) {
        Val *forms = vs_parse(rest, &err);
        if (!forms) { refuse(err); return true; }
        Env *env = env_new(env_builtins());
        for (int k = 0; k < K_COUNT; k++) if (my_cap(k) >= 0) env_define(env, cap_kind_name(k), v_cap(cap_at(my_cap(k))->token, cap_kind_name(k)));
        Interp it = { .fuel = 200000, .syscall = kernel_syscall, .ctx = (void *)k_me };
        Val *r = vs_run(&it, forms, env);
        if (!r) refuse(it.error); else { char buf[400]; v_show(r, buf, sizeof buf); kprintf("=> %s   (fuel %u)\n", buf, it.used); }
        return true;
    }
    if (!strcmp(cmd, "ledger") || !strcmp(cmd, "history")) {
        int total = ledger_count();
        int n = argc > 1 ? atoi(argv[1]) : (!strcmp(cmd, "history") ? total : 12);
        for (int i = total - n < 0 ? 0 : total - n; i < total; i++) {
            Entry *e = ledger_entry(i); char h[11]; short_hash(e->hash, h, 10); char s[64]; summarize(e->text, s, sizeof s);
            kprintf("  #%u %s  %s\n", e->seq, h, s);
        }
        if (!strcmp(cmd, "history")) kprintf("rewind <n> keeps the first n entries; the rest goes to the attic (see: attic)\n");
        return true;
    }
    if (!strcmp(cmd, "verify")) { char why[80]; bool ok = ledger_verify(why, sizeof why); char h[13]; short_hash(ledger_head(), h, 12); kprintf("%s  %s, %d entries, head %s%s\n", ok ? "OK  " : "FAIL", why, ledger_count(), h, disk_present() ? " (on disk)" : " (RAM only)"); return true; }
    if (!strcmp(cmd, "rewind")) {
        if (argc < 2) { refuse("rewind <n>  (see: history)"); return true; }
        if (!i_may(K_KERNEL, R_REVOKE)) { refuse("rewind needs the kernel[revoke] capability"); return true; }
        int n = atoi(argv[1]), total = ledger_count();
        if (n < 1 || n > total) { refuse("entry number out of range"); return true; }
        if (n == total) { say("already there: nothing to discard"); return true; }
        vga_color(C_YELLOW, C_BLACK);
        kprintf("rewind to %d entries moves %d newer entries to the attic (restorable with: restore):\n", n, total - n);
        for (int i = n; i < total && i < n + 6; i++) { char s[60]; summarize(ledger_entry(i)->text, s, sizeof s); kprintf("    #%d %s\n", i, s); }
        if (total - n > 6) kprintf("    ... and %d more\n", total - n - 6);
        vga_puts("type yes to confirm: "); vga_color(C_GREY, C_BLACK);
        char ans[16]; kbd_readline(ans, sizeof ans);
        if (strcmp(ans, "yes") != 0) { say("cancelled"); return true; }
        if (!kernel_rewind(n, &err)) refuse(err);
        else kprintf("rewound: %d objects, %d ledger entries, tick %u; attic now holds %u branch(es)\n", store_count(), ledger_count(), k_tick, disk_attic_branches());
        return true;
    }
    if (!strcmp(cmd, "attic")) {
        Branch br[32]; int n = attic_list(br, 32);
        if (!n) { say("the attic is empty: nothing has been rewound"); return true; }
        for (int i = 0; i < n; i++) {
            char s[50]; summarize(br[i].first, s, sizeof s);
            kprintf("  branch %d: %u entries cut at #%u (uptime %us)  first: %s%s\n", i, br[i].count, br[i].from, br[i].uptime / 1000, s,
                    (u32)ledger_count() == br[i].from ? "   <- restorable now" : "");
        }
        return true;
    }
    if (!strcmp(cmd, "restore")) {
        if (argc < 2) { refuse("restore <branch>  (see: attic)"); return true; }
        if (!i_may(K_KERNEL, R_REVOKE)) { refuse("restore needs the kernel[revoke] capability"); return true; }
        if (!attic_restore(atoi(argv[1]), &err)) refuse(err);
        else kprintf("restored: %d objects, %d ledger entries, tick %u\n", store_count(), ledger_count(), k_tick);
        return true;
    }
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) return false;
    refuse("unknown intent - type help");
    return true;
}

static void run_script(const char *const *lines) {
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0] == '#') { vga_color(C_LGREEN, C_BLACK); kprintf("\n%s\n", lines[i]); vga_color(C_GREY, C_BLACK); continue; }
        head("verum> "); kprintf("%s\n", lines[i]);
        char buf[LINE]; strcpy(buf, lines[i]); dispatch(buf); status();
    }
}

static void demo(void) {
    static const char *const part1[] = {
        "# 1. no files: objects are named by content, found by tag",
        "note The quick brown fox jumps over the lazy dog",
        "# 2. cells are born with NO authority; 3. authority is granted, narrowed, visible",
        "spawn wc wordcount", "spawn shout shout",
        "grant wc store get,put", "grant wc console print", "grant shout store get,put", "link shout wc next",
        "cells",
        "# 4. work flows as messages; every result carries provenance",
        "send shout @note", "run", "find stats", 0 };
    run_script(part1);
    int st[4]; if (store_find("stats", st, 4)) { char w[20]; strcpy(w, "why "); memcpy(w + 4, store_get(st[0])->hash, 12); w[16] = 0; head("verum> "); kprintf("%s\n", w); dispatch(w); }
    static const char *const part3[] = {
        "# 5. a hostile cell tries five classic attacks - each is stopped by construction",
        "spawn mal malware", "grant mal console print",
        "send mal 1", "send mal 2", "send mal 3", "send mal 4", "send mal 5", "run",
        "# 6. revoke, and the authority is gone", "revoke wc console",
        "# 7. another human: a seat with its own, narrower capabilities",
        "seat alice", "grant alice store get,find", "switch alice", "spawn x wordcount", "switch shell",
        "# 8. the ledger is tamper-evident history on disk; try: history, rewind, attic, restore, dash", "verify", "ledger 6",
        "# done - type help to explore", 0 };
    run_script(part3);
}

void shell_main(void) {
    status();
    char h[13]; short_hash(ledger_head(), h, 12);
    head("VERUM OS  bare metal\n");
    kprintf("%d objects, %d ledger entries, head %s\n", store_count(), ledger_count(), h);
    say("no files. no root. no apps. state what you want: help, demo, or dash.");
    char line[LINE];
    for (;;) {
        status();
        head(k_me); head("> ");
        kbd_readline(line, sizeof line);
        if (!dispatch(line)) { say("halting."); return; }
    }
}
