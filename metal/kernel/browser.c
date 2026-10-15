/* The VERUM browser on bare metal: पत्र pages rendered on the framebuffer console.
 *
 * A page is an object of kind "patra"; its hash is its address. A link is a
 * hash, tag:<name> or page:<title>, and resolves to exact content or not at
 * all. A page may carry "@cell <code> cap[rights] ..." - the browser shows the
 * request and runs the cell only on `allow`, with exactly those capabilities. */
#include "verum.h"

#define MAX_LINKS 32
typedef struct { char target[80]; char label[120]; } BLink;
static BLink links[MAX_LINKS]; static int nlinks = 0;
static int cur_obj = -1;                         /* store index of the open page, -1 for a synthesized one */
static char *cur_src = 0;
static int hist[32]; static int nhist = 0;       /* store indexes; -1 marks a synthesized page we cannot return to */
static char script_code[48]; static char script_caps[4][48]; static int script_ncaps = 0;

static void line_out(u8 color, const char *s) { vga_color(color, C_BLACK); vga_puts(s); vga_color(C_GREY, C_BLACK); vga_putc('\n'); }

/* ---------------------------------------------------------------- rendering */

static void render(const char *src) {
    nlinks = 0; script_ncaps = 0; script_code[0] = 0;
    bool pre = false; char line[400];
    const char *p = src;
    while (*p) {
        usize n = 0; while (*p && *p != '\n' && n + 1 < sizeof line) line[n++] = *p++;
        if (*p == '\n') p++;
        line[n] = 0;
        while (n && line[n - 1] == ' ') line[--n] = 0;
        if (strcmp(line, "```") == 0) { pre = !pre; continue; }
        if (pre) { vga_color(C_DGREY, C_BLACK); vga_puts("    "); vga_color(C_GREY, C_BLACK); say_line(line); continue; }
        if (starts_with(line, "###")) { const char *t = line + 3; while (*t == ' ') t++; line_out(C_LCYAN, t); }
        else if (starts_with(line, "##")) { const char *t = line + 2; while (*t == ' ') t++; vga_putc('\n'); line_out(C_YELLOW, t); }
        else if (starts_with(line, "#")) { const char *t = line + 1; while (*t == ' ') t++; vga_putc('\n'); line_out(C_WHITE, t); }
        else if (starts_with(line, "* ")) { vga_color(C_LCYAN, C_BLACK); vga_puts("  * "); vga_color(C_GREY, C_BLACK); say_line(line + 2); }
        else if (starts_with(line, "> ")) { vga_color(C_DGREY, C_BLACK); vga_puts("  | "); vga_puts(line + 2); vga_color(C_GREY, C_BLACK); vga_putc('\n'); }
        else if (starts_with(line, "=>")) {
            const char *t = line + 2; while (*t == ' ') t++;
            if (nlinks < MAX_LINKS) {
                BLink *l = &links[nlinks]; usize i = 0;
                while (*t && *t != ' ' && i + 1 < sizeof l->target) l->target[i++] = *t++;
                l->target[i] = 0; while (*t == ' ') t++;
                i = 0; while (*t && i + 1 < sizeof l->label) l->label[i++] = *t++;
                l->label[i] = 0; if (!l->label[0]) strcpy(l->label, l->target);
                nlinks++;
                vga_color(C_LCYAN, C_BLACK); kprintf("  [%d] ", nlinks); vga_puts(l->label); vga_color(C_GREY, C_BLACK); vga_putc('\n');
            }
        }
        else if (starts_with(line, "@cell")) {
            const char *t = line + 5; while (*t == ' ') t++;
            usize i = 0; while (*t && *t != ' ' && i + 1 < sizeof script_code) script_code[i++] = *t++;
            script_code[i] = 0;
            while (*t && script_ncaps < 4) {
                while (*t == ' ') t++;
                i = 0; while (*t && *t != ' ' && i + 1 < sizeof script_caps[0]) script_caps[script_ncaps][i++] = *t++;
                script_caps[script_ncaps][i] = 0; if (i) script_ncaps++;
            }
        }
        else if (!line[0]) vga_putc('\n');
        else say_line(line);
    }
    if (script_code[0]) {
        vga_putc('\n'); vga_color(C_YELLOW, C_BLACK);
        kprintf("  this page would like to run a cell (%s) with:", script_code);
        for (int i = 0; i < script_ncaps; i++) kprintf(" %s", script_caps[i]);
        vga_puts("  - `allow` to run it"); vga_color(C_GREY, C_BLACK); vga_putc('\n');
    }
}

void say_line(const char *s) { vga_puts(s); vga_putc('\n'); }

static void footer(int obj) {
    if (obj < 0) { line_out(C_DGREY, "-- a page made for you now; it is not stored"); return; }
    Obj *o = store_get(obj); char h[13]; memcpy(h, o->hash, 12); h[12] = 0;
    vga_color(C_DGREY, C_BLACK); kprintf("-- %s . by %s . tick %u . %d link(s)\n", h, o->by, o->tick, nlinks); vga_color(C_GREY, C_BLACK);
}

/* ---------------------------------------------------------------- resolving targets */

/* returns a store index, or -1 with *synth set to a page made on the spot; false if nothing matches */
static bool resolve(const char *target, int *obj, char **synth, const char **err) {
    *obj = -1; *synth = 0;
    if (starts_with(target, "tag:")) {
        const char *tag = target + 4;
        int hits[64]; int n = store_find(tag, hits, 64);
        char *buf = kalloc(8192); usize len = 0;
        len += (usize)0; strcpy(buf, "# objects tagged "); strcat(buf, tag); strcat(buf, "\n\n");
        for (int i = 0; i < n; i++) {
            Obj *o = store_get(hits[i]); char pv[50]; v_show(o->data, pv, sizeof pv);
            for (char *c = pv; *c; c++) if (*c == '\n') *c = ' ';
            len = strlen(buf); if (len + 200 > 8192) break;
            strcat(buf, "=> "); strcat(buf, o->hash); strcat(buf, " "); strcat(buf, o->kind); strcat(buf, ": "); strcat(buf, pv); strcat(buf, "\n");
        }
        if (!n) { strcat(buf, "nothing is tagged "); strcat(buf, tag); strcat(buf, ".\n"); }
        *synth = buf; return true;
    }
    if (starts_with(target, "page:")) {
        int hits[64]; int n = store_find(target + 5, hits, 64);
        for (int i = 0; i < n; i++) if (strcmp(store_get(hits[i])->kind, "patra") == 0) { *obj = hits[i]; return true; }
        *err = "no page with that title"; return false;
    }
    int i = store_resolve(target);
    if (i < 0) { *err = "no unique object matching that"; return false; }
    Obj *o = store_get(i);
    if (strcmp(o->kind, "patra") == 0) { *obj = i; return true; }
    char *buf = kalloc(VS_MAX_SIZE + 256);                        /* any object shows as a page */
    strcpy(buf, "# "); strcat(buf, o->kind); strcat(buf, " "); strncat_safe(buf, o->hash, 12); strcat(buf, "\n\n```\n");
    v_show(o->data, buf + strlen(buf), VS_MAX_SIZE); strcat(buf, "\n```\n\nby "); strcat(buf, o->by); strcat(buf, "\n");
    *synth = buf; *obj = -1; return true;
}

void strncat_safe(char *d, const char *s, usize n) { usize l = strlen(d); memcpy(d + l, s, n); d[l + n] = 0; }

static void show(int obj, char *synth) {
    vga_clear();
    cur_obj = obj; cur_src = synth ? synth : store_get(obj)->data->s;
    render(cur_src);
    footer(obj);
}

/* ---------------------------------------------------------------- public */

bool browser_open(const char *target, const char **err) {
    int obj; char *synth;
    if (!resolve(target, &obj, &synth, err)) return false;
    if (cur_src && nhist < 32) hist[nhist++] = cur_obj;
    show(obj, synth);
    return true;
}

bool browser_follow(int n, const char **err) {
    if (!cur_src) { *err = "no page is open (open page:home)"; return false; }
    if (n < 1 || n > nlinks) { *err = "no such link on this page"; return false; }
    return browser_open(links[n - 1].target, err);
}

bool browser_back(const char **err) {
    if (!nhist) { *err = "no earlier page"; return false; }
    int obj = hist[--nhist];
    if (obj < 0) { *err = "the earlier page was made on the spot; open it again by name"; cur_src = 0; return false; }
    show(obj, 0);
    return true;
}

void browser_links(void) {
    if (!cur_src) { say_line("no page is open"); return; }
    for (int i = 0; i < nlinks; i++) kprintf("  [%d] %s  ->  %s\n", i + 1, links[i].label, links[i].target);
}

/* run the open page's cell with exactly the declared capabilities, and hand it the page's hash */
bool browser_allow(const char **err) {
    if (!cur_src || !script_code[0]) { *err = "the open page has no cell to run"; return false; }
    if (cur_obj < 0) { *err = "only a stored page can run a cell"; return false; }
    char name[NAME_LEN]; strcpy(name, "page-"); strncat_safe(name, store_get(cur_obj)->hash, 6);
    if (cell_find(name)) cell_kill(name);
    int code = store_resolve(script_code); if (code < 0) code = store_find_code(script_code);
    if (code < 0) { *err = "the page names a program that is not here"; return false; }
    if (!cell_spawn(name, code, err)) return false;
    for (int i = 0; i < script_ncaps; i++) {
        char kind[16]; const char *spec = script_caps[i]; usize k = 0;
        while (*spec && *spec != '[' && k + 1 < sizeof kind) kind[k++] = *spec++;
        kind[k] = 0;
        char rights[48]; usize r = 0; if (*spec == '[') spec++;
        while (*spec && *spec != ']' && r + 1 < sizeof rights) rights[r++] = *spec++;
        rights[r] = 0;
        int kidx = -1; for (int j = 0; j < K_COUNT; j++) if (strcmp(cap_kind_name(j), kind) == 0) kidx = j;
        if (kidx < 0 || my_cap(kidx) < 0) { *err = "the page asks for a capability you do not hold"; return false; }
        u32 bits = r ? cap_parse_rights(rights, kidx) : cap_at(my_cap(kidx))->rights;
        if (!bits || !kernel_grant(my_cap(kidx), k_me, bits, name, kind, err)) return false;
        char d[96]; cap_describe(cap_at(my_cap(kidx)), d, sizeof d);
    }
    kprintf("cell %s runs with:", name);
    for (int i = 0; i < script_ncaps; i++) kprintf(" %s", script_caps[i]);
    vga_putc('\n');
    if (!cell_send(name, v_str(store_get(cur_obj)->hash), k_me, err)) return false;
    kernel_run(100);
    return true;
}

bool browser_has_page(void) { return cur_src != 0; }
