#include "verum.h"

/* ---------------------------------------------------------------- store */

static Obj objs[MAX_OBJS];
static int nobjs = 0;

static void obj_hash(Obj *o, char out[65]) {
    /* canonical form: kind|data|tags|by|op|inputs|tick  (the content IS the name) */
    char *buf = kalloc(VS_MAX_SIZE + 1024);
    strcpy(buf, o->kind); strcat(buf, "|");
    v_canon(o->data, buf + strlen(buf), VS_MAX_SIZE);
    strcat(buf, "|");
    for (int i = 0; i < o->ntags; i++) { strcat(buf, o->tags[i]); strcat(buf, ","); }
    strcat(buf, "|"); strcat(buf, o->by); strcat(buf, "|"); strcat(buf, o->op); strcat(buf, "|");
    for (int i = 0; i < o->ninputs; i++) { strcat(buf, o->inputs[i]); strcat(buf, ","); }
    char t[12]; utoa(o->tick, t); strcat(buf, "|"); strcat(buf, t);
    u8 d[32]; sha256((const u8 *)buf, strlen(buf), d); hex_encode(d, 32, out);
}

int store_put(const char *kind, Val *data, const char **tags, int ntags, const char *by,
              const char *op, const char (*inputs)[65], int ninputs, u32 tick) {
    if (nobjs >= MAX_OBJS || ntags > MAX_TAGS || ninputs > MAX_INPUTS) return -1;
    heap_persist_begin();                               /* objects outlive the run that made them */
    Obj *o = &objs[nobjs];
    o->kind = kstrdup(kind); o->data = v_persist(data); o->by = kstrdup(by); o->op = kstrdup(op); o->tick = tick;
    o->ntags = ntags; for (int i = 0; i < ntags; i++) o->tags[i] = kstrdup(tags[i]);
    o->ninputs = ninputs; for (int i = 0; i < ninputs; i++) strcpy(o->inputs[i], inputs[i]);
    obj_hash(o, o->hash);
    heap_persist_end();
    for (int i = 0; i < nobjs; i++) if (strcmp(objs[i].hash, o->hash) == 0) return i;   /* same content, same name */
    return nobjs++;
}

Obj *store_get(int idx) { return (idx >= 0 && idx < nobjs) ? &objs[idx] : 0; }
int store_count(void) { return nobjs; }
void store_reset(void) { nobjs = 0; }

int store_resolve(const char *prefix) {
    usize n = strlen(prefix);
    if (n < 4) return -1;
    int hit = -1;
    for (int i = 0; i < nobjs; i++)
        if (strncmp(objs[i].hash, prefix, n) == 0) { if (hit >= 0) return -1; hit = i; }
    return hit;
}

int store_find(const char *tag, int *out, int cap) {
    int n = 0;
    for (int i = 0; i < nobjs && n < cap; i++)
        for (int t = 0; t < objs[i].ntags; t++)
            if (strcmp(objs[i].tags[t], tag) == 0) { out[n++] = i; break; }
    return n;
}

int store_find_code(const char *name) {
    for (int i = nobjs - 1; i >= 0; i--)
        if (strcmp(objs[i].kind, "code") == 0)
            for (int t = 0; t < objs[i].ntags; t++) if (strcmp(objs[i].tags[t], name) == 0) return i;
    return -1;
}

/* ---------------------------------------------------------------- ledger */

static Entry led[MAX_LEDGER];
static int nled = 0;
static bool loading = false;
static const char GENESIS[65] = "0000000000000000000000000000000000000000000000000000000000000000";

static void entry_hash(Entry *e, char out[65]) {
    usize n = strlen(e->text);
    char *buf = kalloc(n + 96); char s[12]; utoa(e->seq, s);
    strcpy(buf, s); strcat(buf, "|"); strcat(buf, e->prev); strcat(buf, "|"); strcat(buf, e->text);
    u8 d[32]; sha256((const u8 *)buf, strlen(buf), d); hex_encode(d, 32, out);
}

void ledger_set_loading(bool l) { loading = l; }
void ledger_reset(void) { nled = 0; }

int ledger_append(const char *text) {
    if (nled >= MAX_LEDGER) panic("ledger full");
    Entry *e = &led[nled];
    e->seq = (u32)nled;
    strcpy(e->prev, nled ? led[nled - 1].hash : GENESIS);
    heap_persist_begin();
    e->text = kstrdup(text);
    entry_hash(e, e->hash);
    heap_persist_end();
    e->bytes = (nled ? led[nled - 1].bytes : 0) + (u32)strlen(text) + 1;
    nled++;
    if (!loading && disk_present()) {
        if (!disk_append(text)) { vga_color(C_LRED, C_BLACK); kprintf("[disk] write failed\n"); vga_color(C_GREY, C_BLACK); }
    }
    return nled - 1;
}

bool ledger_verify(char *why, usize cap) {
    const char *prev = GENESIS; char h[65];
    (void)cap;
    for (int i = 0; i < nled; i++) {
        Entry *e = &led[i];
        if (e->seq != (u32)i || strcmp(e->prev, prev) != 0) { strcpy(why, "broken chain at entry "); utoa((u32)i, why + strlen(why)); return false; }
        entry_hash(e, h);
        if (strcmp(h, e->hash) != 0) { strcpy(why, "entry does not match its hash: "); utoa((u32)i, why + strlen(why)); return false; }
        prev = e->hash;
    }
    strcpy(why, "chain intact");
    return true;
}

int ledger_count(void) { return nled; }
Entry *ledger_entry(int i) { return (i >= 0 && i < nled) ? &led[i] : 0; }
const char *ledger_head(void) { return nled ? led[nled - 1].hash : GENESIS; }
