; =============================================================================
;  AxisBoot - Stage 1 (Master Boot Record)
;  Loaded by the BIOS at 0000:7C00. Loads stage 2 (LBA 1..16) to 0000:7E00.
;  Uses INT 13h LBA extensions when available, else classic CHS - so it boots
;  from hard disks, USB, and CD/DVD (El Torito), and on old BIOSes.
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

    ; INT 13h extensions present?
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .chs
    cmp bx, 0xAA55
    jne .chs
    mov byte [use_lba], 1
.chs:
    ; read drive geometry for the CHS fallback (AH=08)
    mov ah, 0x08
    mov dl, [boot_drive]
    push es
    int 0x13
    pop es
    jc .geo_default
    mov al, cl
    and al, 0x3F
    cmp al, 0
    je .geo_default
    mov [spt], al
    mov al, dh
    inc al
    mov [heads], al
    jmp .load
.geo_default:
    mov byte [spt], 18
    mov byte [heads], 2
.load:
    ; read STAGE2_SECTORS sectors from LBA 1 to 0000:7E00
    mov cx, STAGE2_SECTORS
    mov eax, 1
    mov bx, 0x7E00
.next:
    push cx
    call read_one
    jc .disk_err
    pop cx
    add bx, 512
    inc eax
    loop .next

    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

.disk_err:
    mov si, msg_diskerr
    call print
halt:
    cli
    hlt
    jmp halt

; read one sector: EAX = LBA, ES:BX = destination. CF set on error.
read_one:
    cmp byte [use_lba], 1
    jne .chs_read
    mov [dap_lba], eax
    mov [dap_off], bx
    mov [dap_seg], es
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    ret
.chs_read:
    ; CHS from LBA: sector = LBA % spt + 1; t = LBA / spt;
    ;               head = t % heads; cyl = t / heads
    push bx
    movzx ebx, byte [spt]
    xor edx, edx
    mov ecx, eax          ; ecx = LBA
    mov eax, ecx
    div ebx               ; eax = LBA/spt, edx = LBA%spt
    mov cl, dl
    inc cl                ; CL = sector (1-based)
    movzx ebx, byte [heads]
    xor edx, edx
    div ebx               ; eax = cyl, edx = head
    mov ch, al            ; CH = cylinder low 8 bits
    mov dh, dl            ; DH = head
    pop bx
    mov dl, [boot_drive]
    mov ax, 0x0201        ; AH=02 read, AL=1 sector
    int 0x13
    ret

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
    dw 1
dap_off: dw 0x7E00
dap_seg: dw 0x0000
dap_lba: dd 1
         dd 0

boot_drive:  db 0
use_lba:     db 0
spt:         db 18
heads:       db 2
msg_loading: db "AxisBoot", 0
msg_diskerr: db " - disk error", 0

; Disk identification magic (checked before the kernel ever writes a disk).
times 0x1B0 - ($ - $$) db 0
    db "AXISDISK"
times 0x1BE - ($ - $$) db 0
    db 0x80, 0, 2, 0, 0x7F, 0xFE, 0xFF, 0xFF
    dd 1, 8191
times 510 - ($ - $$) db 0
    dw 0xAA55
