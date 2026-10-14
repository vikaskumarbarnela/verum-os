#include "verum.h"

void kmain(u32 magic, u32 mbinfo) {
    (void)mbinfo;
    vga_init();
    if (magic != 0x2BADB002) panic("not booted by a Multiboot loader");
    irq_init();
    kernel_boot();
    shell_main();
    for (;;) __asm__ volatile("cli; hlt");
}
