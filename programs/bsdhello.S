.code32
.globl _start
_start:
    push $msglen
    push $msg
    push $1
    mov $4, %eax        # SYS_write
    call stub           # [esp]=ret,[esp+4]=1,[esp+8]=msg,[esp+12]=len
    add $12, %esp
    push $0
    mov $1, %eax        # SYS_exit
    call stub
stub:
    int $0x80
    ret
.section .rodata
msg: .ascii "Hello from a FreeBSD-style program on AxisOS!\n"
msglen = . - msg
