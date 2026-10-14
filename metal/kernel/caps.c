#include "verum.h"

static Cap caps[MAX_CAPS];
static int ncaps = 0;

static const char *KIND_NAMES[K_COUNT] = { "store", "console", "clock", "cell", "kernel" };
static const u32 KIND_RIGHTS[K_COUNT] = {
    R_GET | R_PUT | R_FIND | R_LINEAGE | R_DELEGATE, R_PRINT | R_DELEGATE, R_NOW | R_DELEGATE,
    R_SEND | R_DELEGATE, R_SPAWN | R_MINT | R_REVOKE | R_CELLS };
static const struct { const char *name; u32 bit; } RIGHT_NAMES[] = {
    {"get", R_GET}, {"put", R_PUT}, {"find", R_FIND}, {"lineage", R_LINEAGE}, {"print", R_PRINT},
    {"now", R_NOW}, {"send", R_SEND}, {"spawn", R_SPAWN}, {"mint", R_MINT}, {"revoke", R_REVOKE}, {"cells", R_CELLS},
    {"delegate", R_DELEGATE} };
#define N_RIGHTS (sizeof RIGHT_NAMES / sizeof RIGHT_NAMES[0])

const char *cap_kind_name(int kind) { return KIND_NAMES[kind]; }
u32 cap_rights_of_kind(int kind) { return KIND_RIGHTS[kind]; }
void caps_reset(void) { ncaps = 0; }
int cap_count(void) { return ncaps; }

u32 cap_parse_rights(const char *csv, int kind) {
    u32 r = 0; char name[16]; usize n = 0;
    for (const char *p = csv;; p++) {
        if (*p == ',' || !*p) {
            name[n] = 0; n = 0; bool ok = false;
            for (usize i = 0; i < N_RIGHTS; i++)
                if (strcmp(RIGHT_NAMES[i].name, name) == 0 && (RIGHT_NAMES[i].bit & KIND_RIGHTS[kind])) { r |= RIGHT_NAMES[i].bit; ok = true; }
            if (!ok) return 0;
            if (!*p) return r;
        } else if (n + 1 < sizeof name) name[n++] = *p;
    }
}

void cap_rights_csv(u32 rights, char *out) {
    out[0] = 0;
    for (usize i = 0; i < N_RIGHTS; i++)
        if (rights & RIGHT_NAMES[i].bit) { if (out[0]) strcat(out, ","); strcat(out, RIGHT_NAMES[i].name); }
}

static u32 rdtsc_lo(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return lo ^ hi; }

static void new_token(char out[33]) {
    static u32 counter = 0;
    u32 seed[4] = { rdtsc_lo(), (u32)heap_used(), ++counter, 0x5eed5eed };
    u8 d[32]; sha256((const u8 *)seed, sizeof seed, d);
    hex_encode(d, 16, out);
}

static void copy_name(char *dst, const char *src) {
    usize n = src ? strlen(src) : 0; if (n >= NAME_LEN) n = NAME_LEN - 1;
    if (n) memcpy(dst, src, n); dst[n] = 0;
}

int cap_mint(int kind, u32 rights, const char *holder, const char *target, int parent, const char *token) {
    if (ncaps >= MAX_CAPS) return -1;
    Cap *c = &caps[ncaps];
    if (token) { memcpy(c->token, token, 32); c->token[32] = 0; } else new_token(c->token);
    c->kind = kind; c->rights = rights & KIND_RIGHTS[kind];
    copy_name(c->holder, holder);
    copy_name(c->target, target);
    c->parent = parent; c->revoked = false; c->used = true;
    return ncaps++;
}

Cap *cap_by_token(const char *token) {
    for (int i = 0; i < ncaps; i++) if (strcmp(caps[i].token, token) == 0) return &caps[i];
    return 0;
}
Cap *cap_at(int idx) { return (idx >= 0 && idx < ncaps) ? &caps[idx] : 0; }
int cap_index(Cap *c) { return (int)(c - caps); }

Cap *cap_check(const char *token, const char *holder, u32 right, const char **err) {
    Cap *c = cap_by_token(token);
    if (!c) { *err = "invalid capability"; return 0; }
    if (c->revoked) { *err = "capability has been revoked"; return 0; }
    if (strcmp(c->holder, holder) != 0) { *err = "capability is not held by the caller"; return 0; }
    if (right && !(c->rights & right)) { *err = "capability lacks that right"; return 0; }
    return c;
}

int cap_attenuate(int parent, const char *by, u32 rights, const char *holder, const char *token, const char **err) {
    Cap *src = cap_check(caps[parent].token, by, 0, err);
    if (!src) return -1;
    if (rights & ~src->rights) { *err = "cannot grant rights you do not hold"; return -1; }
    if (!rights) { *err = "no rights given"; return -1; }
    return cap_mint(src->kind, rights, holder, src->target[0] ? src->target : 0, parent, token);
}

int cap_revoke(int idx, const char *by, const char **err) {
    if (idx < 0 || idx >= ncaps) { *err = "no such capability"; return -1; }
    bool controls = false;
    for (int i = idx; i >= 0; i = caps[i].parent) if (strcmp(caps[i].holder, by) == 0) { controls = true; break; }
    if (!controls) { *err = "you do not control this capability"; return -1; }
    int n = 0;
    for (int i = 0; i < ncaps; i++)
        for (int j = i; j >= 0; j = caps[j].parent)
            if (j == idx) { if (!caps[i].revoked) { caps[i].revoked = true; n++; } break; }
    return n;
}

void cap_describe(Cap *c, char *out, usize cap) {
    (void)cap;
    strcpy(out, KIND_NAMES[c->kind]);
    if (c->target[0]) { strcat(out, "->"); strcat(out, c->target); }
    strcat(out, "[");
    char r[80]; cap_rights_csv(c->rights, r); strcat(out, r);
    strcat(out, "] held by "); strcat(out, c->holder);
    if (c->revoked) strcat(out, " (REVOKED)");
}
