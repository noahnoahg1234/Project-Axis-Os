; =============================================================================
;  AxisBoot - Stage 1 (Master Boot Record)
;  Loaded by the BIOS at 0000:7C00. Its only job is to load stage 2
;  (AxisBoot menu) from LBA 1..16 to 0000:7E00 and jump to it.
; =============================================================================
bits 16
org 0x7C00

%define STAGE2_SECTORS 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_loading
    call print

    ; Make sure the BIOS supports INT 13h extensions (LBA reads)
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext

    ; Read stage 2
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .disk_err

    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

.no_ext:
    mov si, msg_noext
    call print
    jmp halt
.disk_err:
    mov si, msg_diskerr
    call print
halt:
    cli
    hlt
    jmp halt

; si = zero terminated string
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0x7E00, 0x0000
    dq 1

boot_drive:  db 0
msg_loading: db "AxisBoot", 0
msg_noext:   db " - BIOS has no LBA support", 0
msg_diskerr: db " - disk read error", 0

; Disk identification magic. The kernel checks this before it ever writes
; to a disk, so it can never overwrite a disk that is not an AxisOS disk.
times 0x1B0 - ($ - $$) db 0
    db "AXISDISK"
times 0x1BE - ($ - $$) db 0
; One active partition entry spanning the image (helps USB-HDD emulation)
    db 0x80, 0, 2, 0, 0x7F, 0xFE, 0xFF, 0xFF
    dd 1, 8191
times 510 - ($ - $$) db 0
    dw 0xAA55
