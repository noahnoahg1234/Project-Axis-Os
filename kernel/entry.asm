; AxisOS kernel entry point and interrupt stubs (32-bit protected mode)
bits 32
section .text.entry
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cli
    mov esp, 0x90000
    cld
    ; clear .bss
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .text
extern isr_dispatch

%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1
    jmp isr_common
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
ISR_ERR   21
%assign i 22
%rep 10
ISR_NOERR i
%assign i i+1
%endrep
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

isr_common:
    pusha
    xor eax, eax
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    push esp
    call isr_dispatch
    add esp, 4
    pop eax
    mov ds, ax
    mov es, ax
    popa
    add esp, 8
    iret

section .data
global isr_table
isr_table:
%assign i 0
%rep 48
    dd isr %+ i
%assign i i+1
%endrep
