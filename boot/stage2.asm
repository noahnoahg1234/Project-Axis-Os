; =============================================================================
;  AxisBoot - Stage 2 (Boot manager)
;
;  * Reads the shared AxisOS settings sector (LBA 17)
;  * Shows a boot menu with three systems: Axis/Linux, Axis/Windows and
;    Axis/FreeBSD, plus an "Advanced mode" toggle and a timeout countdown
;  * Loads the kernel (LBA 64..) to 0x10000, detects memory, enables A20,
;    switches to 32-bit protected mode and jumps to the kernel.
;
;  Hand-off (physical memory):
;    0x0500  boot info  : 'AXBI', os, advanced, drive, cfg_valid, mem_kb
;    0x0600  settings   : raw copy of the settings sector (512 bytes)
; =============================================================================
bits 16
org 0x7E00

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 128
%endif
%define KERNEL_LBA     64
%define CONFIG_LBA     17
%define KERNEL_SEG     0x1000          ; 0x1000:0000 = 0x10000
%define BOOTINFO       0x0500
%define CONFIG_BUF     0x0600

%define OS_COUNT       4

stage2:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [drive], dl
    cld

    ; ---------------------------------------------------------- settings ---
    mov si, dap_cfg
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc .cfg_default
    cmp dword [CONFIG_BUF], 'AXCF'
    jne .cfg_default
    mov byte [cfg_valid], 1
    mov al, [CONFIG_BUF + 5]           ; default os
    cmp al, OS_COUNT
    jb .os_ok
    xor al, al
.os_ok:
    mov [selected], al
    mov al, [CONFIG_BUF + 6]           ; timeout (seconds, 0 = wait)
    cmp al, 99
    jbe .to_ok
    mov al, 5
.to_ok:
    mov [timeout], al
    mov al, [CONFIG_BUF + 9]           ; advanced mode by default
    and al, 1
    mov [advanced], al
    jmp .cfg_done
.cfg_default:
    mov dword [CONFIG_BUF], 0          ; mark as invalid for the kernel
.cfg_done:

    ; -------------------------------------------------------------- menu ---
    mov ax, 0x0003                     ; 80x25 text mode
    int 0x10
    mov ax, 0x1003                     ; bright backgrounds instead of blink
    xor bx, bx
    int 0x10
    mov ah, 0x01                       ; hide cursor
    mov cx, 0x2000
    int 0x10

    mov ax, 0xB800
    mov es, ax

    xor ah, ah                         ; remember start tick
    int 0x1A
    mov [start_lo], dx
    mov [start_hi], cx
    mov byte [counting], 1
    cmp byte [timeout], 0
    jne .redraw
    mov byte [counting], 0

.redraw:
    call draw_menu

.wait_key:
    call draw_countdown
    mov ah, 0x01
    int 0x16
    jnz .have_key
    cmp byte [counting], 0
    je .wait_key
    call seconds_left
    test ax, ax
    jz boot_selected
    jmp .wait_key

.have_key:
    xor ah, ah
    int 0x16
    mov byte [counting], 0             ; any key stops the countdown
    cmp ah, 0x48                       ; up
    je .up
    cmp ah, 0x50                       ; down
    je .down
    cmp al, 0x0D                       ; enter
    je boot_selected
    or al, 0x20                        ; lower case
    cmp al, 'a'
    je .toggle_adv
    cmp al, 'r'
    je .reboot
    cmp al, '1'
    jb .redraw
    cmp al, '0' + OS_COUNT
    ja .redraw
    sub al, '1'
    mov [selected], al
    jmp boot_selected
.up:
    mov al, [selected]
    test al, al
    jnz .up_ok
    mov al, OS_COUNT
.up_ok:
    dec al
    mov [selected], al
    jmp .redraw
.down:
    mov al, [selected]
    inc al
    cmp al, OS_COUNT
    jb .down_ok
    xor al, al
.down_ok:
    mov [selected], al
    jmp .redraw
.toggle_adv:
    xor byte [advanced], 1
    jmp .redraw
.reboot:
    int 0x19

; returns ax = seconds remaining
seconds_left:
    xor ah, ah
    int 0x1A
    sub dx, [start_lo]                 ; elapsed ticks (low word is plenty)
    mov ax, dx
    xor dx, dx
    mov cx, 18
    div cx                             ; ax = elapsed seconds
    movzx cx, byte [timeout]
    cmp ax, cx
    jae .zero
    sub cx, ax
    mov ax, cx
    ret
.zero:
    xor ax, ax
    ret

; ------------------------------------------------------------- drawing ---
draw_menu:
    mov byte [shown_secs], 0xFF
    ; background
    xor di, di
    mov ax, 0x0720
    mov cx, 80*25
    rep stosw
    ; title bar
    mov dh, 0
    mov bl, 0x1F
    call fill_row
    mov dh, 0
    mov dl, 2
    mov si, s_title
    call print_at
    mov dh, 0
    mov dl, 62
    mov si, s_version
    call print_at

    ; logo
    mov dh, 1
    mov si, s_logo1
.logo:
    inc dh
    mov dl, 22
    mov bl, 0x0F
    call print_at
.logo_skip:
    lodsb                              ; advance si past this line
    test al, al
    jnz .logo_skip
    cmp byte [si], 0xFF
    jne .logo
    mov dh, 8
    mov dl, 6
    mov bl, 0x07
    mov si, s_choose
    call print_at

    ; frame
    mov dh, 9
    call frame_top
    mov dh, 10
.frame_mid:
    call frame_side
    inc dh
    cmp dh, 18
    jb .frame_mid
    call frame_bottom

    ; entries
    xor cx, cx
.entry:
    push cx
    mov di, cx
    shl di, 1
    mov si, [entry_names + di]
    mov dh, cl
    shl dh, 1
    add dh, 11                         ; rows 11, 13, 15
    mov dl, 8
    mov bl, [entry_color + di]
    cmp cl, [selected]
    jne .not_sel
    mov bl, [entry_sel + di]
    push dx
    push si
    mov si, s_blank_bar
    call print_at
    pop si
    pop dx
.not_sel:
    call print_at
    pop cx
    inc cx
    cmp cx, OS_COUNT
    jb .entry

    ; advanced toggle
    mov dh, 19
    mov dl, 8
    mov bl, 0x07
    mov si, s_adv_off
    cmp byte [advanced], 0
    je .adv_print
    mov bl, 0x0E
    mov si, s_adv_on
.adv_print:
    call print_at

    ; help footer
    mov dh, 24
    mov bl, 0x70
    call fill_row
    mov dh, 24
    mov dl, 1
    mov si, s_help
    call print_at
    ret

; redraws the countdown line only when what it shows has changed
draw_countdown:
    pusha
    mov al, 0xFE                       ; 0xFE = "stopped"
    cmp byte [counting], 0
    je .have_state
    call seconds_left
.have_state:
    cmp al, [shown_secs]
    je .done
    mov [shown_secs], al
    mov dh, 21
    mov bl, 0x07
    call fill_row
    cmp byte [counting], 0
    je .manual
    mov dh, 21
    mov dl, 8
    mov bl, 0x07
    mov si, s_auto
    call print_at
    movzx ax, byte [shown_secs]
    mov bl, 10
    div bl                             ; al = tens, ah = ones
    add ax, 0x3030
    mov di, (21*80 + 40)*2
    mov [es:di], al
    mov byte [es:di+1], 0x0E
    mov [es:di+2], ah
    mov byte [es:di+3], 0x0E
    mov dh, 21
    mov dl, 43
    mov bl, 0x07
    mov si, s_secs
    call print_at
    jmp .done
.manual:
    mov dh, 21
    mov dl, 8
    mov bl, 0x08
    mov si, s_manual
    call print_at
.done:
    popa
    ret

frame_top:
    mov si, s_frame_top
    jmp frame_common
frame_side:
    mov si, s_frame_side
    jmp frame_common
frame_bottom:
    mov si, s_frame_bot
frame_common:
    push dx
    mov dl, 6
    mov bl, 0x08
    call print_at
    pop dx
    ret

; dh = row, bl = attribute
fill_row:
    pusha
    movzx ax, dh
    imul ax, ax, 160
    mov di, ax
    mov ah, bl
    mov al, ' '
    mov cx, 80
    rep stosw
    popa
    ret

; dh = row, dl = col, bl = attribute, si = string
print_at:
    pusha
    movzx ax, dh
    imul ax, ax, 80                    ; (mul would clobber dx = row/col)
    movzx cx, dl
    add ax, cx
    shl ax, 1
    mov di, ax
    mov ah, bl
.next:
    lodsb
    test al, al
    jz .end
    stosw
    jmp .next
.end:
    popa
    ret

; ------------------------------------------------------------- booting ---
boot_selected:
    xor di, di                         ; clear screen
    mov ax, 0x0720
    mov cx, 80*25
    rep stosw
    mov dh, 1
    mov dl, 2
    mov bl, 0x0F
    mov si, s_loading
    call print_at

    ; memory size (KB above 1 MB)
    mov ax, 0xE801
    int 0x15
    jc .e88
    test ax, ax
    jnz .e801_ok
    mov ax, cx
    mov bx, dx
.e801_ok:
    movzx eax, ax
    movzx ebx, bx
    shl ebx, 6
    add eax, ebx
    mov [mem_kb], eax
    jmp .mem_done
.e88:
    mov ah, 0x88
    int 0x15
    movzx eax, ax
    mov [mem_kb], eax
.mem_done:

    ; load kernel in chunks of 32 sectors (16 KiB, never crosses 64K)
    mov word [k_seg], KERNEL_SEG
    mov dword [k_lba], KERNEL_LBA
    mov word [k_left], KERNEL_SECTORS
.load:
    mov ax, [k_left]
    test ax, ax
    jz .loaded
    cmp ax, 32
    jbe .chunk
    mov ax, 32
.chunk:
    mov [k_chunk], ax
    mov [dap_k + 2], ax
    mov word [dap_k + 4], 0
    mov bx, [k_seg]
    mov [dap_k + 6], bx
    mov eax, [k_lba]
    mov [dap_k + 8], eax
    mov dword [dap_k + 12], 0
    xor ax, ax
    mov ds, ax
    mov si, dap_k
    mov ah, 0x42
    mov dl, [drive]
    int 0x13
    jc disk_error
    movzx eax, word [k_chunk]
    add [k_lba], eax
    sub [k_left], ax
    shl ax, 5                          ; sectors * 512 / 16
    add [k_seg], ax
    mov ax, 0xB800                     ; progress dot
    mov es, ax
    mov di, [dot_pos]
    mov word [es:di], 0x0AFE
    add word [dot_pos], 2
    jmp .load
.loaded:

    ; boot info for the kernel
    mov dword [BOOTINFO], 'AXBI'
    mov al, [selected]
    mov [BOOTINFO + 4], al
    mov al, [advanced]
    mov [BOOTINFO + 5], al
    mov al, [drive]
    mov [BOOTINFO + 6], al
    mov al, [cfg_valid]
    mov [BOOTINFO + 7], al
    mov eax, [mem_kb]
    mov [BOOTINFO + 8], eax

    ; A20: BIOS first, then the "fast A20" port
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al

    mov ax, 0x0003                     ; fresh text mode for the kernel
    int 0x10
    mov ax, 0x1003
    xor bx, bx
    int 0x10

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

disk_error:
    mov ax, 0xB800
    mov es, ax
    mov dh, 3
    mov dl, 2
    mov bl, 0x4F
    mov si, s_diskerr
    call print_at
.hang:
    hlt
    jmp .hang

bits 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, 0x10000
    jmp eax

; ---------------------------------------------------------------- data ---
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF              ; 0x08 code
    dq 0x00CF92000000FFFF              ; 0x10 data
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

align 4
dap_cfg:
    db 0x10, 0
    dw 1
    dw CONFIG_BUF, 0
    dq CONFIG_LBA
dap_k:
    db 0x10, 0
    times 14 db 0


drive:     db 0
selected:  db 0
advanced:  db 0
timeout:   db 5
cfg_valid: db 0
counting:  db 1
shown_secs: db 0xFF
start_lo:  dw 0
start_hi:  dw 0
mem_kb:    dd 0
k_seg:     dw 0
k_left:    dw 0
k_chunk:   dw 0
k_lba:     dd 0
dot_pos:   dw (3*80 + 2)*2

entry_names: dw s_linux, s_windows, s_freebsd, s_dos
entry_color: dw 0x0A, 0x0B, 0x0C, 0x0E
entry_sel:   dw 0x2F, 0x1F, 0x4F, 0x60

s_title:     db "AxisBoot  -  Unified Boot Manager", 0
s_version:   db "AxisOS 1.0  (x86)", 0
s_logo1:     db "     _          _      ___  ____  ", 0
             db "    / \   __  _(_)___ / _ \/ ___| ", 0
             db "   / _ \  \ \/ / / __| | | \___ \ ", 0
             db "  / ___ \  >  <| \__ \ |_| |___) |", 0
             db " /_/   \_\/_/\_\_|___/\___/|____/ ", 0
             db 0xFF
s_choose:    db "Choose the system to start:", 0
s_frame_top: db 0xC9, "==================================================================", 0xBB, 0
s_frame_side:db 0xBA, "                                                                  ", 0xBA, 0
s_frame_bot: db 0xC8, "==================================================================", 0xBC, 0
s_blank_bar: db "                                                                ", 0
s_linux:     db " 1  Axis/Linux     bash-style shell, ls/cat/grep, ~ paths    ", 0
s_windows:   db " 2  Axis/Windows   cmd-style shell, dir/type/copy, C:\ paths ", 0
s_freebsd:   db " 3  Axis/FreeBSD   BSD sh/csh shell, sysctl, ee, chflags     ", 0
s_dos:       db " 4  Axis/DOS       MS-DOS style, DIR/TYPE/COPY, C:\ prompt    ", 0
s_adv_off:   db "[ ] Advanced mode  (A to toggle - expert tools, no hand-holding)", 0
s_adv_on:    db "[X] Advanced mode  (A to toggle - expert tools, no hand-holding)", 0
s_auto:      db "Starting the selected system in", 0
s_secs:      db "seconds...", 0
s_manual:    db "Countdown stopped. Press Enter to boot.", 0
s_help:      db "Up/Down: select   Enter: boot   1-4: quick boot   A: advanced   R: reboot", 0
s_loading:   db "AxisBoot: loading AxisOS kernel", 0
s_diskerr:   db " Disk read error while loading the kernel. System halted. ", 0

stage2_end:
%if stage2_end - $$ > 16*512
%error "stage2 is larger than 16 sectors"
%endif
