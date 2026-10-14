/* Freestanding runtime for VERUM: no libc exists here. */
#ifndef VERUM_LIB_H
#define VERUM_LIB_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;
typedef unsigned int       usize;
typedef int                bool;
#define true  1
#define false 0
#define NULL  ((void *)0)

/* memory */
void  *memcpy(void *d, const void *s, usize n);
void  *memset(void *d, int c, usize n);
int    memcmp(const void *a, const void *b, usize n);
void  *kalloc(usize n);                 /* bump allocator; persistent heap, or the current arena */
/* Arenas: while a cell runs, kalloc serves from that cell's arena, which is reset per message.
   Kernel code that must outlive the run wraps its allocations in persist_begin/persist_end. */
void   heap_arena_use(u8 *base, usize size, usize *used);   /* base NULL: persistent heap */
void   heap_persist_begin(void);
void   heap_persist_end(void);
void   arena_overflow(void);            /* kernel.c: aborts the running cell with "memory budget exceeded" */
char  *kstrdup(const char *s);
usize  heap_used(void);

/* strings */
usize  strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, usize n);
char  *strcpy(char *d, const char *s);
char  *strcat(char *d, const char *s);
bool   starts_with(const char *s, const char *prefix);
char  *strstr(const char *h, const char *n);
int    atoi(const char *s);
void   itoa(int v, char *out);
void   utoa(u32 v, char *out);
void   hex_encode(const u8 *in, usize n, char *out);   /* out needs 2n+1 */

/* x86 I/O */
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8   inb(u16 port) { u8 r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r; }

/* VGA console */
enum { C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_GREY,
       C_DGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_PINK, C_YELLOW, C_WHITE };
void vga_init(void);
void vga_clear(void);                      /* clears the console area (rows 1..24) */
void vga_status(const char *s);            /* row 0: inverse-video status bar */
void vga_at(int row, int col);             /* move the cursor (dash screens) */
void vga_color(u8 fg, u8 bg);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_puti(int v);
void kprintf(const char *fmt, ...);       /* %s %d %u %c %x %% only */

/* keyboard */
char kbd_getc(void);                       /* blocking, polled */
void kbd_readline(char *buf, usize cap);   /* with echo and backspace */

/* ATA PIO disk (primary master, LBA28). Sectors are 512 bytes. */
bool ata_present(void);
bool ata_read(u32 lba, u8 *buf, u32 count);
bool ata_write(u32 lba, const u8 *buf, u32 count);

/* interrupts (irq.c) */
void irq_init(void);                       /* IDT + PIC + PIT at 100 Hz; enables interrupts */
u8   kbd_scancode(void);                   /* next raw scancode from the IRQ ring; halts CPU while idle */
u32  uptime_ms(void);
extern volatile u32 uptime_ticks;
extern volatile u32 vm_deadline;           /* timer tick at which the running cell is preempted */
extern volatile bool vm_preempt;           /* set by the timer, consumed by the VM */
void task_yield(void);                     /* kernel.c: give the CPU to the next runnable cell (no-op outside a cell) */
void ctx_switch(u32 **save_sp, u32 *load_sp);

/* sha256 */
void sha256(const u8 *data, usize n, u8 out[32]);

/* panic */
void panic(const char *msg);

#endif
