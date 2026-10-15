/* Framebuffer console: 800x600x32 through the Bochs/QEMU VBE interface.
 *
 * The VGA text mode has 256 glyphs of 8x16 and cannot show Devanagari. This
 * console draws into a linear framebuffer instead: ASCII from an 8x16 bitmap
 * font, and Devanagari word by word from an atlas that fontgen.py shaped at
 * build time with a real text engine (conjuncts, reordered vowel signs).
 * UTF-8 bytes are collected until the word ends (an ASCII byte arrives), then
 * the word is looked up and blitted. Unknown Devanagari shows as a box per
 * code point. Text-mode geometry is kept in spirit: 100 columns, 30 rows of
 * 20 px, row 0 is the status bar. If no VBE device is found, lib.c keeps the
 * VGA text console. */
#include "lib.h"
#include "font.h"

#define VBE_INDEX 0x1CE
#define VBE_DATA  0x1CF
#define FB_W 800
#define FB_H 600
#define COLS (FB_W / FONT_ASCII_W)        /* 100 */
#define ROWS (FB_H / FONT_CELL_H)         /* 30 */
#define TOP 1

static volatile u32 *fb = 0;
static int cur_x = 0, cur_y = TOP;
static u32 fg = 0xAAAAAA, bg = 0x000000;
static char pend[64]; static int pend_n = 0;   /* bytes of a Devanagari word being assembled */
static int px = 0;                              /* pixels already used in the current cell by Devanagari glyphs */
static int cursor_x = -1, cursor_y = -1;

static const u32 PALETTE[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };

/* ---------------------------------------------------------------- PCI: find the VGA device's framebuffer */

static u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static u32 find_lfb(void) {
    for (u32 dev = 0; dev < 32; dev++) {
        u32 id = pci_read(0, dev, 0, 0);
        if (id == 0xFFFFFFFFu) continue;
        u32 cls = pci_read(0, dev, 0, 8) >> 24;
        if (cls == 0x03 || id == 0x11111234u) {           /* display class, or Bochs/QEMU std VGA */
            u32 bar0 = pci_read(0, dev, 0, 0x10);
            if (bar0 & 1) continue;                      /* I/O BAR, not memory */
            return bar0 & 0xFFFFFFF0u;
        }
    }
    return 0;
}

static void vbe_write(u16 index, u16 value) { outw(VBE_INDEX, index); outw(VBE_DATA, value); }
static u16 vbe_read(u16 index) { outw(VBE_INDEX, index); return inw(VBE_DATA); }

bool fb_init(void) {
    if (vbe_read(0) < 0xB0C0) return false;               /* no Bochs VBE */
    u32 lfb = find_lfb();
    if (!lfb) return false;
    vbe_write(4, 0);                                      /* disable while changing mode */
    vbe_write(1, FB_W); vbe_write(2, FB_H); vbe_write(3, 32);
    vbe_write(4, 0x41);                                   /* enable + linear framebuffer */
    fb = (volatile u32 *)lfb;
    for (u32 i = 0; i < FB_W * FB_H; i++) fb[i] = 0;
    cur_x = 0; cur_y = TOP;
    return true;
}

bool fb_active(void) { return fb != 0; }

/* ---------------------------------------------------------------- drawing */

static void fill(int x, int y, int w, int h, u32 color) {
    for (int yy = y; yy < y + h && yy < FB_H; yy++)
        for (int xx = x; xx < x + w && xx < FB_W; xx++) fb[yy * FB_W + xx] = color;
}

static void draw_ascii(int col, int row, char c) {
    int x = col * FONT_ASCII_W, y = row * FONT_CELL_H;
    fill(x, y, FONT_ASCII_W, FONT_CELL_H, bg);
    if (c < 32 || c > 126) return;
    const unsigned char *g = FONT_ASCII[c - 32];
    for (int yy = 0; yy < FONT_ASCII_H; yy++)
        for (int xx = 0; xx < FONT_ASCII_W; xx++)
            if (g[yy] & (0x80 >> xx)) fb[(y + 2 + yy) * FB_W + x + xx] = fg;
}

/* draws a shaped glyph (a word or an akshara) at pixel x; returns its width in pixels */
static int draw_glyph(int x, int row, const FontWord *wd) {
    int nbytes = (wd->width + 7) / 8, y = row * FONT_CELL_H;
    fill(x, y, wd->width, FONT_CELL_H, bg);
    for (int yy = 0; yy < FONT_CELL_H; yy++)
        for (int b = 0; b < nbytes; b++) {
            unsigned char byte = wd->bits[yy * nbytes + b];
            for (int i = 0; i < 8; i++)
                if (byte & (0x80 >> i) && x + b * 8 + i < FB_W) fb[(y + yy) * FB_W + x + b * 8 + i] = fg;
        }
    return wd->width;
}

static void draw_box(int col, int row) {                 /* an unknown code point */
    int x = col * FONT_ASCII_W, y = row * FONT_CELL_H;
    fill(x, y, FONT_ASCII_W, FONT_CELL_H, bg);
    fill(x + 1, y + 3, 6, 1, fg); fill(x + 1, y + 16, 6, 1, fg); fill(x + 1, y + 3, 1, 14, fg); fill(x + 6, y + 3, 1, 14, fg);
}

static void cursor_hide(void) {
    if (cursor_x >= 0) fill(cursor_x * FONT_ASCII_W, cursor_y * FONT_CELL_H + 18, FONT_ASCII_W, 2, bg);
    cursor_x = -1;
}
static void cursor_show(void) {
    cursor_x = cur_x; cursor_y = cur_y;
    fill(cur_x * FONT_ASCII_W, cur_y * FONT_CELL_H + 18, FONT_ASCII_W, 2, fg);
}

static void scroll(void) {
    u32 *base = (u32 *)fb;
    memcpy(base + TOP * FONT_CELL_H * FB_W, base + (TOP + 1) * FONT_CELL_H * FB_W, (usize)(ROWS - TOP - 1) * FONT_CELL_H * FB_W * 4);
    fill(0, (ROWS - 1) * FONT_CELL_H, FB_W, FONT_CELL_H, bg);
    cur_y = ROWS - 1;
}

/* ---- aksharas: words the atlas has never seen are drawn syllable by syllable ---- */

static u32 cp_at(const char *s, int *len) {          /* decode one UTF-8 code point */
    u8 c = (u8)s[0];
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *len = 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) { *len = 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    *len = 4; return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
}
static bool is_consonant(u32 c) { return c >= 0x0915 && c <= 0x0939; }
static bool is_sign(u32 c) { return (c >= 0x093E && c <= 0x094C) || c == 0x0962 || c == 0x0963; }
static bool is_mark(u32 c) { return c >= 0x0901 && c <= 0x0903; }
#define VIRAMA_CP 0x094D

static const FontWord *find_cluster(const char *s) {   /* FONT_CLUSTER is sorted by UTF-8 bytes */
    int lo = 0, hi = FONT_CLUSTERS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(FONT_CLUSTER[mid].utf8, s);
        if (c == 0) return &FONT_CLUSTER[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* Devanagari glyphs advance by their exact pixel width; the cell grid is caught up when the word ends */
static void place(const FontWord *g) {
    int x = cur_x * FONT_ASCII_W + px;
    if (x + g->width > FB_W) { cur_x = 0; px = 0; cur_y++; if (cur_y >= ROWS) scroll(); x = 0; }
    px += draw_glyph(x, cur_y, g);
}
static void end_word(void) {
    cur_x += (px + FONT_ASCII_W - 1) / FONT_ASCII_W; px = 0;
    if (cur_x >= COLS) { cur_x = 0; cur_y++; }
    if (cur_y >= ROWS) scroll();
}

/* Draw one akshara; if the atlas lacks it, peel it apart: trailing mark, then the conjunct chain. */
static void draw_akshara(const char *s) {
    const FontWord *g = find_cluster(s);
    if (g) { place(g); return; }
    usize n = strlen(s);
    if (n > 3) {
        int l; u32 last = cp_at(s + n - 3, &l);
        if (is_mark(last)) {                             /* "क्षं" -> "क्ष" + "ं" */
            char head[64]; memcpy(head, s, n - 3); head[n - 3] = 0;
            draw_akshara(head); draw_akshara(s + n - 3); return;
        }
    }
    for (usize i = 3; i + 3 <= n; i += 3) {              /* split an unknown conjunct after its first dead consonant */
        int l; if (cp_at(s + i, &l) == VIRAMA_CP && i + 3 < n) {
            char head[64]; memcpy(head, s, i + 3); head[i + 3] = 0;
            draw_akshara(head); draw_akshara(s + i + 3); return;
        }
    }
    draw_box((cur_x * FONT_ASCII_W + px) / FONT_ASCII_W, cur_y); px += FONT_ASCII_W;
}

static void draw_by_aksharas(const char *s) {
    while (*s) {
        int l; u32 c = cp_at(s, &l);
        const char *start = s; s += l;
        if (is_consonant(c)) {
            for (;;) {                                   /* C (् C)* */
                int l2; if (*s && cp_at(s, &l2) == VIRAMA_CP) {
                    int l3; if (s[l2] && is_consonant(cp_at(s + l2, &l3))) { s += l2 + l3; continue; }
                    s += l2;                             /* dead consonant at the end */
                }
                break;
            }
            int l4; if (*s && is_sign(cp_at(s, &l4))) s += l4;
        }
        int l5; if (*s && is_mark(cp_at(s, &l5))) s += l5;
        char cluster[64]; usize n = (usize)(s - start); if (n >= sizeof cluster) n = sizeof cluster - 1;
        memcpy(cluster, start, n); cluster[n] = 0;
        draw_akshara(cluster);
    }
}

static void flush_word(void) {
    if (!pend_n) return;
    pend[pend_n] = 0;
    const FontWord *hit = 0;
    for (int i = 0; i < FONT_WORDS; i++) if (strcmp(FONT_WORD[i].utf8, pend) == 0) { hit = &FONT_WORD[i]; break; }
    if (hit) place(hit);                                 /* a known word: the best shaping */
    else draw_by_aksharas(pend);                         /* anything else: syllable by syllable */
    end_word();
    pend_n = 0;
}

void fb_color(u8 f, u8 b) { fg = PALETTE[f & 15]; bg = PALETTE[b & 15]; }

void fb_putc(char c) {
    cursor_hide();
    if ((unsigned char)c >= 0x80) {                       /* part of a Devanagari word: collect */
        if (pend_n < (int)sizeof pend - 1) pend[pend_n++] = c;
        cursor_show();
        return;
    }
    flush_word();
    if (c == '\n') { cur_x = 0; cur_y++; }
    else if (c == '\r') cur_x = 0;
    else if (c == '\b') { if (cur_x) { cur_x--; draw_ascii(cur_x, cur_y, ' '); } }
    else if (c == '\t') cur_x = (cur_x + 4) & ~3;
    else { draw_ascii(cur_x, cur_y, c); cur_x++; }
    if (cur_x >= COLS) { cur_x = 0; cur_y++; }
    if (cur_y >= ROWS) scroll();
    cursor_show();
}

void fb_clear(void) {
    cursor_hide();
    fill(0, TOP * FONT_CELL_H, FB_W, FB_H - TOP * FONT_CELL_H, bg);
    cur_x = 0; cur_y = TOP; pend_n = 0; px = 0;
    cursor_show();
}

void fb_at(int row, int col) { cursor_hide(); flush_word(); cur_y = row; cur_x = col; cursor_show(); }

void fb_status(const char *s) {
    /* row 0, inverse: black on cyan, drawn with the same engine so a Devanagari seat name works */
    int sx = cur_x, sy = cur_y; u32 sfg = fg, sbg = bg;
    cursor_hide(); flush_word();
    fg = 0x000000; bg = 0x00AAAA;
    fill(0, 0, FB_W, FONT_CELL_H, bg);
    cur_x = 0; cur_y = 0;
    for (; *s && cur_y == 0; s++) fb_putc(*s);
    flush_word();
    cursor_hide();
    fg = sfg; bg = sbg; cur_x = sx; cur_y = sy;
    cursor_show();
}
