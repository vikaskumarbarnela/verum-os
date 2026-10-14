; VERUM bare-metal boot stub: Multiboot 1 header + 32-bit entry.
; QEMU (-kernel) or GRUB loads us at 1 MiB already in protected mode.

MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x00000003          ; page-align modules, provide memory map
MB_CHECK    equ -(MB_MAGIC + MB_FLAGS)

section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECK

section .bss
align 16
stack_bottom:
    resb 1024 * 1024                ; 1 MiB kernel stack (the VM uses stack buffers)
stack_top:

section .text
global _start
global vs_setjmp
global vs_longjmp
extern kmain

; int vs_setjmp(u32 *buf)   - saves ebx esi edi ebp esp eip
vs_setjmp:
    mov  eax, [esp + 4]
    mov  [eax + 0],  ebx
    mov  [eax + 4],  esi
    mov  [eax + 8],  edi
    mov  [eax + 12], ebp
    lea  ecx, [esp + 4]
    mov  [eax + 16], ecx
    mov  ecx, [esp]
    mov  [eax + 20], ecx
    xor  eax, eax
    ret

; void vs_longjmp(u32 *buf, int v)
vs_longjmp:
    mov  edx, [esp + 4]
    mov  eax, [esp + 8]
    mov  ebx, [edx + 0]
    mov  esi, [edx + 4]
    mov  edi, [edx + 8]
    mov  ebp, [edx + 12]
    mov  esp, [edx + 16]
    jmp  dword [edx + 20]

_start:
    cli
    mov  esp, stack_top
    lgdt [gdt_ptr]                  ; the loader's GDT is not guaranteed; install ours
    mov  cx, 0x10
    mov  ds, cx
    mov  es, cx
    mov  fs, cx
    mov  gs, cx
    mov  ss, cx
    jmp  0x08:.flush
.flush:
    push ebx                        ; multiboot info pointer
    push eax                        ; multiboot magic
    call kmain
.hang:
    cli
    hlt
    jmp  .hang

; ---------------------------------------------------------------- GDT: flat code + data
section .rodata
align 8
gdt:
    dq 0                                    ; null
    dq 0x00CF9A000000FFFF                   ; 0x08: code, base 0, limit 4G, 32-bit
    dq 0x00CF92000000FFFF                   ; 0x10: data
gdt_ptr:
    dw gdt_ptr - gdt - 1
    dd gdt

section .text
; ---------------------------------------------------------------- context switch
; void ctx_switch(u32 **save_sp, u32 *load_sp)
; Saves the callee-saved registers on the current stack, stores its esp,
; switches to the other stack and pops that side's registers. A fresh task
; stack is prepared by the kernel to look like it was saved here, with the
; entry function as the "return address".
global ctx_switch
ctx_switch:
    mov  eax, [esp + 4]
    mov  edx, [esp + 8]
    push ebp
    push ebx
    push esi
    push edi
    mov  [eax], esp
    mov  esp, edx
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    ret

; ---------------------------------------------------------------- IDT load
global idt_load
idt_load:
    mov  eax, [esp + 4]
    lidt [eax]
    ret

; ---------------------------------------------------------------- interrupt stubs
; Each stub pushes (fake) error code + vector, then shares one path that
; builds a `Regs` frame and calls isr_handler(Regs *).
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp  isr_common
%endmacro
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp  isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

extern isr_handler
isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    push esp                        ; Regs *
    call isr_handler
    add  esp, 4
    pop  gs
    pop  fs
    pop  es
    pop  ds
    popa
    add  esp, 8                     ; vector + error code
    iret
