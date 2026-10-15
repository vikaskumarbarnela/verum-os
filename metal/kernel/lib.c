#include "lib.h"

/* ---------------------------------------------------------------- memory */

extern char _end[];                     /* from linker.ld: end of kernel image */
static char *heap_ptr = 0;
static char *heap_end = 0;
#define HEAP_SIZE (112u * 1024u * 1024u)

static u8 *arena_base = 0; static usize arena_size = 0; static usize *arena_used = 0;
static int persist_depth = 0;
void heap_arena_use(u8 *base, usize size, usize *used) { arena_base = base; arena_size = size; arena_used = used; }
void heap_persist_begin(void) { persist_depth++; }
void heap_persist_end(void) { persist_depth--; }

static void heap_init(void) {
    u32 p = ((u32)_end + 15u) & ~15u;
    heap_ptr = (char *)p;
    heap_end = heap_ptr + HEAP_SIZE;
}

void *kalloc(usize n) {
    if (!heap_ptr) heap_init();
    n = (n + 15u) & ~15u;
    if (arena_base && persist_depth == 0) {
        if (*arena_used + n > arena_size) arena_overflow();     /* does not return */
        void *p = arena_base + *arena_used;
        *arena_used += n;
        memset(p, 0, n);
        return p;
    }
    if (heap_ptr + n > heap_end) panic("out of kernel memory");
    void *p = heap_ptr;
    heap_ptr += n;
    memset(p, 0, n);
    return p;
}

usize heap_used(void) { return heap_ptr ? (usize)(heap_ptr - _end) : 0; }

void *memcpy(void *d, const void *s, usize n) {
    char *dd = d; const char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, usize n) {
    char *dd = d;
    while (n--) *dd++ = (char)c;
    return d;
}
int memcmp(const void *a, const void *b, usize n) {
    const u8 *x = a, *y = b;
    for (usize i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
char *kstrdup(const char *s) {
    usize n = strlen(s);
    char *p = kalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* ---------------------------------------------------------------- strings */

usize strlen(const char *s) { usize n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, usize n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
bool starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
char *strstr(const char *h, const char *n) {
    usize ln = strlen(n);
    for (; *h; h++) if (strncmp(h, n, ln) == 0) return (char *)h;
    return 0;
}
int atoi(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
void utoa(u32 v, char *out) {
    char tmp[12]; int i = 0;
    do { tmp[i++] = '0' + v % 10; v /= 10; } while (v);
    while (i) *out++ = tmp[--i];
    *out = 0;
}
void itoa(int v, char *out) {
    if (v < 0) { *out++ = '-'; utoa((u32)(-v), out); } else utoa((u32)v, out);
}
void hex_encode(const u8 *in, usize n, char *out) {
    static const char *hx = "0123456789abcdef";
    for (usize i = 0; i < n; i++) { out[2 * i] = hx[in[i] >> 4]; out[2 * i + 1] = hx[in[i] & 15]; }
    out[2 * n] = 0;
}

/* ---------------------------------------------------------------- VGA text */

#define VGA ((volatile u16 *)0xB8000)
#define COLS 80
#define ROWS 25
static int cur_x = 0, cur_y = 0;
static u8 attr = 0x07;

static void vga_cursor(void) {
    u16 pos = (u16)(cur_y * COLS + cur_x);
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos & 0xFF);
}
#define TOP 1                                   /* row 0 is the status bar */
static bool use_fb = false;                     /* the framebuffer console takes over when VBE exists */
void vga_init(void) {
    attr = (u8)((C_BLACK << 4) | C_GREY);
    for (int i = 0; i < COLS * ROWS; i++) VGA[i] = (u16)(attr << 8) | ' ';
    cur_x = 0; cur_y = TOP;
    vga_cursor();
    use_fb = fb_init();
    if (use_fb) fb_color(C_GREY, C_BLACK);
}
void vga_clear(void) {
    if (use_fb) { fb_clear(); return; }
    for (int i = TOP * COLS; i < COLS * ROWS; i++) VGA[i] = (u16)(attr << 8) | ' ';
    cur_x = 0; cur_y = TOP;
    vga_cursor();
}
void vga_status(const char *s) {
    if (use_fb) { fb_status(s); return; }
    u8 bar = (u8)((C_CYAN << 4) | C_BLACK);
    for (int i = 0; i < COLS; i++) VGA[i] = (u16)(bar << 8) | (u8)(*s ? *s++ : ' ');
}
void vga_at(int row, int col) { if (use_fb) { fb_at(row, col); return; } cur_y = row; cur_x = col; vga_cursor(); }
void vga_color(u8 fg, u8 bg) { attr = (u8)((bg << 4) | fg); if (use_fb) fb_color(fg, bg); }
static void scroll(void) {
    for (int i = TOP * COLS; i < (ROWS - 1) * COLS; i++) VGA[i] = VGA[i + COLS];
    for (int i = (ROWS - 1) * COLS; i < ROWS * COLS; i++) VGA[i] = (u16)(attr << 8) | ' ';
    cur_y = ROWS - 1;
}
void vga_putc(char c) {
    if (use_fb) { fb_putc(c); return; }
    if (c == '\n') { cur_x = 0; cur_y++; }
    else if (c == '\r') cur_x = 0;
    else if (c == '\b') { if (cur_x) { cur_x--; VGA[cur_y * COLS + cur_x] = (u16)(attr << 8) | ' '; } }
    else if (c == '\t') { cur_x = (cur_x + 4) & ~3; }
    else {
        VGA[cur_y * COLS + cur_x] = (u16)(attr << 8) | (u8)c;
        if (++cur_x >= COLS) { cur_x = 0; cur_y++; }
    }
    if (cur_y >= ROWS) scroll();
    vga_cursor();
}
void vga_puts(const char *s) { while (*s) vga_putc(*s++); }
void vga_puti(int v) { char b[12]; itoa(v, b); vga_puts(b); }

void kprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { vga_putc(*fmt); continue; }
        fmt++;
        switch (*fmt) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *); vga_puts(s ? s : "(null)"); break; }
        case 'd': vga_puti(__builtin_va_arg(ap, int)); break;
        case 'u': { char b[12]; utoa(__builtin_va_arg(ap, u32), b); vga_puts(b); break; }
        case 'c': vga_putc((char)__builtin_va_arg(ap, int)); break;
        case 'x': { u32 v = __builtin_va_arg(ap, u32); char b[9]; u8 raw[4] = { v >> 24, v >> 16, v >> 8, v };
                    hex_encode(raw, 4, b); vga_puts(b); break; }
        case '%': vga_putc('%'); break;
        default: vga_putc('%'); vga_putc(*fmt);
        }
    }
    __builtin_va_end(ap);
}

void panic(const char *msg) {
    vga_color(C_WHITE, C_RED);
    kprintf("\nKERNEL PANIC: %s\n", msg);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------------------------------------------------------- keyboard (PS/2, polled) */

static const char keymap[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0,'*', 0,' ', 0,
};
static const char keymap_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b', '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0,'|','Z','X','C','V',
    'B','N','M','<','>','?', 0,'*', 0,' ', 0,
};

char kbd_getc(void) {
    static bool shift = false;
    for (;;) {
        u8 sc = kbd_scancode();                       /* interrupt-driven: the CPU sleeps between keys */
        if (sc == 0x2A || sc == 0x36) { shift = true; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = false; continue; }
        if (sc & 0x80) continue;                       /* key release */
        char c = shift ? keymap_shift[sc] : keymap[sc];
        if (c) return c;
    }
}

void kbd_readline(char *buf, usize cap) {
    usize n = 0;
    for (;;) {
        char c = kbd_getc();
        if (c == '\n') { buf[n] = 0; vga_putc('\n'); return; }
        if (c == '\b') { if (n) { n--; vga_putc('\b'); } continue; }
        if (n + 1 < cap) { buf[n++] = c; vga_putc(c); }
    }
}

/* ---------------------------------------------------------------- ATA PIO */

#define ATA_DATA 0x1F0
#define ATA_SECCOUNT 0x1F2
#define ATA_LBA0 0x1F3
#define ATA_LBA1 0x1F4
#define ATA_LBA2 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_CMD 0x1F7
#define ATA_STATUS 0x1F7


static bool ata_wait(bool want_drq) {
    for (u32 i = 0; i < 1000000; i++) {
        u8 st = inb(ATA_STATUS);
        if (st == 0xFF) return false;                   /* no drive on the bus */
        if (st & 0x01) return false;                    /* ERR */
        if (!(st & 0x80)) {                             /* not BSY */
            if (!want_drq || (st & 0x08)) return true;  /* DRQ when asked */
        }
    }
    return false;
}

static void ata_setup(u32 lba, u8 count) {
    outb(ATA_DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA0, (u8)lba);
    outb(ATA_LBA1, (u8)(lba >> 8));
    outb(ATA_LBA2, (u8)(lba >> 16));
}

bool ata_present(void) {
    if (inb(ATA_STATUS) == 0xFF) return false;
    outb(ATA_DRIVE, 0xA0);
    outb(ATA_SECCOUNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                                /* IDENTIFY */
    u8 st = inb(ATA_STATUS);
    if (st == 0) return false;
    if (!ata_wait(true)) return false;
    for (int i = 0; i < 256; i++) (void)inw(ATA_DATA);
    return true;
}

bool ata_read(u32 lba, u8 *buf, u32 count) {
    for (u32 s = 0; s < count; s++) {
        if (!ata_wait(false)) return false;
        ata_setup(lba + s, 1);
        outb(ATA_CMD, 0x20);                            /* READ SECTORS */
        if (!ata_wait(true)) return false;
        u16 *w = (u16 *)(buf + s * 512);
        for (int i = 0; i < 256; i++) w[i] = inw(ATA_DATA);
    }
    return true;
}

bool ata_write(u32 lba, const u8 *buf, u32 count) {
    for (u32 s = 0; s < count; s++) {
        if (!ata_wait(false)) return false;
        ata_setup(lba + s, 1);
        outb(ATA_CMD, 0x30);                            /* WRITE SECTORS */
        if (!ata_wait(true)) return false;
        const u16 *w = (const u16 *)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(ATA_DATA, w[i]);
        if (!ata_wait(false)) return false;
        outb(ATA_CMD, 0xE7);                            /* FLUSH CACHE */
        if (!ata_wait(false)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------- SHA-256 */

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(u32 st[8], const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 | (u32)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        u32 s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    u32 a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + S1 + ch + K[i] + w[i];
        u32 S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        u32 mj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d; st[4]+=e; st[5]+=f; st[6]+=g; st[7]+=h;
}

void sha256(const u8 *data, usize n, u8 out[32]) {
    u32 st[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    usize i = 0;
    for (; i + 64 <= n; i += 64) sha_block(st, data + i);
    u8 tail[128]; usize r = n - i;
    memset(tail, 0, sizeof tail);
    memcpy(tail, data + i, r);
    tail[r] = 0x80;
    usize len = (r + 9 <= 64) ? 64 : 128;
    u64 bits = (u64)n * 8;
    for (int j = 0; j < 8; j++) tail[len - 1 - j] = (u8)(bits >> (8 * j));
    sha_block(st, tail);
    if (len == 128) sha_block(st, tail + 64);
    for (int j = 0; j < 8; j++) { out[4*j] = st[j] >> 24; out[4*j+1] = st[j] >> 16; out[4*j+2] = st[j] >> 8; out[4*j+3] = st[j]; }
}
