/* Interrupts: IDT, PIC remap, PIT timer, keyboard IRQ, CPU exceptions.
 *
 * The timer gives the kernel two things it did not have while fully polled:
 *   - a clock (uptime in 10 ms ticks) shown in the status bar and `uptime`
 *   - preemption: a cell's run gets a time slice; when it expires the timer
 *     sets `vm_preempt` and the VM aborts the run on its next step, so a
 *     cell can no longer hold the machine even inside its fuel budget.
 * CPU exceptions print a register panel and halt instead of triple-faulting. */
#include "lib.h"

typedef struct { u16 lo; u16 sel; u8 zero; u8 flags; u16 hi; } __attribute__((packed)) IdtEntry;
typedef struct { u16 limit; u32 base; } __attribute__((packed)) IdtPtr;

typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 vector, err;
    u32 eip, cs, eflags;
} Regs;

static IdtEntry idt[256];
static IdtPtr idt_ptr;

extern void idt_load(IdtPtr *p);
#define ISR(n) extern void isr##n(void);
ISR(0) ISR(1) ISR(2) ISR(3) ISR(4) ISR(5) ISR(6) ISR(7) ISR(8) ISR(9) ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23) ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)
ISR(32) ISR(33) ISR(34) ISR(35) ISR(36) ISR(37) ISR(38) ISR(39) ISR(40) ISR(41) ISR(42) ISR(43) ISR(44) ISR(45) ISR(46) ISR(47)
static void (*const STUBS[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39, isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47 };

static const char *const EXC[32] = {
    "divide by zero", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode", "no FPU",
    "double fault", "coprocessor overrun", "bad TSS", "segment not present", "stack fault", "general protection",
    "page fault", "reserved", "x87 error", "alignment check", "machine check", "SIMD error", "virtualization",
    "control protection", "r", "r", "r", "r", "r", "r", "hypervisor", "VMM", "security", "r" };

/* ---------------------------------------------------------------- shared state */

volatile u32 uptime_ticks = 0;          /* 100 Hz since boot */
volatile u32 vm_deadline = 0;           /* tick at which the running cell is preempted; 0 = none */
volatile bool vm_preempt = false;       /* set by the timer, consumed by the VM */

#define KBUF 64
static volatile u8 kbuf[KBUF];
static volatile u32 khead = 0, ktail = 0;

/* ---------------------------------------------------------------- PIC / PIT */

#define PIC1 0x20
#define PIC2 0xA0
#define EOI  0x20

static void pic_remap(void) {
    outb(PIC1, 0x11); outb(PIC2, 0x11);        /* ICW1: init, expect ICW4 */
    outb(PIC1 + 1, 32); outb(PIC2 + 1, 40);    /* ICW2: vector offsets */
    outb(PIC1 + 1, 4);  outb(PIC2 + 1, 2);     /* ICW3: cascade on IRQ2 */
    outb(PIC1 + 1, 1);  outb(PIC2 + 1, 1);     /* ICW4: 8086 mode */
    outb(PIC1 + 1, 0xFC);                      /* mask all but IRQ0 (timer) and IRQ1 (keyboard) */
    outb(PIC2 + 1, 0xFF);
}

static void pit_init(u32 hz) {
    u32 div = 1193182 / hz;
    outb(0x43, 0x36);                          /* channel 0, lo/hi byte, square wave */
    outb(0x40, (u8)div);
    outb(0x40, (u8)(div >> 8));
}

/* ---------------------------------------------------------------- handler */

static void exception_panel(Regs *r) {
    __asm__ volatile("cli");
    vga_color(C_WHITE, C_RED);
    kprintf("\n CPU EXCEPTION %u: %s   err=%x\n", r->vector, r->vector < 32 ? EXC[r->vector] : "?", r->err);
    kprintf(" eip=%x cs=%x eflags=%x\n", r->eip, r->cs, r->eflags);
    kprintf(" eax=%x ebx=%x ecx=%x edx=%x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf(" esi=%x edi=%x ebp=%x\n", r->esi, r->edi, r->ebp);
    if (r->vector == 14) { u32 cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2)); kprintf(" cr2=%x (faulting address)\n", cr2); }
    kprintf(" The ledger on disk is intact up to the last recorded event. Halted.\n");
    for (;;) __asm__ volatile("hlt");
}

void isr_handler(Regs *r) {
    if (r->vector < 32) { exception_panel(r); return; }
    u32 irq = r->vector - 32;
    if (irq == 0) {
        uptime_ticks++;
        if (vm_deadline && uptime_ticks >= vm_deadline) vm_preempt = true;
    } else if (irq == 1) {
        u8 sc = inb(0x60);
        u32 next = (khead + 1) % KBUF;
        if (next != ktail) { kbuf[khead] = sc; khead = next; }   /* drop on overflow */
    }
    if (irq >= 8) outb(PIC2, EOI);
    outb(PIC1, EOI);
}

/* ---------------------------------------------------------------- public API */

void irq_init(void) {
    for (int i = 0; i < 48; i++) {
        u32 a = (u32)STUBS[i];
        idt[i].lo = (u16)a; idt[i].hi = (u16)(a >> 16);
        idt[i].sel = 0x08; idt[i].zero = 0; idt[i].flags = 0x8E;   /* present, ring 0, 32-bit interrupt gate */
    }
    idt_ptr.limit = sizeof idt - 1;
    idt_ptr.base = (u32)idt;
    idt_load(&idt_ptr);
    pic_remap();
    pit_init(100);
    __asm__ volatile("sti");
}

/* Next raw scancode from the keyboard IRQ ring. While the ring is empty the
   kernel gets the CPU for background work, then the CPU halts until an IRQ. */
extern void kernel_idle(void);
u8 kbd_scancode(void) {
    for (;;) {
        __asm__ volatile("cli");
        if (ktail != khead) { u8 sc = kbuf[ktail]; ktail = (ktail + 1) % KBUF; __asm__ volatile("sti"); return sc; }
        __asm__ volatile("sti");
        kernel_idle();
        __asm__ volatile("hlt");
    }
}

u32 uptime_ms(void) { return uptime_ticks * 10; }
