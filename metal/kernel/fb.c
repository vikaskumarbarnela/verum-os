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

/* draws a shaped word; returns the number of 8px cells it used */
static int draw_word(int col, int row, const FontWord *wd) {
    int cells = (wd->width + FONT_ASCII_W - 1) / FONT_ASCII_W;
    int x = col * FONT_ASCII_W, y = row * FONT_CELL_H, nbytes = cells;
    fill(x, y, cells * FONT_ASCII_W, FONT_CELL_H, bg);
    for (int yy = 0; yy < FONT_CELL_H; yy++)
        for (int b = 0; b < nbytes; b++) {
            unsigned char byte = wd->bits[yy * nbytes + b];
            for (int i = 0; i < 8; i++)
                if (byte & (0x80 >> i) && x + b * 8 + i < FB_W) fb[(y + yy) * FB_W + x + b * 8 + i] = fg;
        }
    return cells;
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

static void advance(int cells) {
    cur_x += cells;
    if (cur_x >= COLS) { cur_x = 0; cur_y++; }
    if (cur_y >= ROWS) scroll();
}

static void flush_word(void) {
    if (!pend_n) return;
    pend[pend_n] = 0;
    const FontWord *hit = 0;
    for (int i = 0; i < FONT_WORDS; i++) if (strcmp(FONT_WORD[i].utf8, pend) == 0) { hit = &FONT_WORD[i]; break; }
    if (hit) {
        int cells = (hit->width + FONT_ASCII_W - 1) / FONT_ASCII_W;
        if (cur_x + cells > COLS) { cur_x = 0; cur_y++; if (cur_y >= ROWS) scroll(); }
        advance(draw_word(cur_x, cur_y, hit));
    } else {
        for (int i = 0; i < pend_n; i++)
            if ((pend[i] & 0xC0) != 0x80) { draw_box(cur_x, cur_y); advance(1); }   /* one box per code point */
    }
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
    cur_x = 0; cur_y = TOP; pend_n = 0;
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
