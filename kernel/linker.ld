ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    /*
     * Put .bss above 1 MB. Physical 0xA0000-0xFFFFF (640 KB-1 MB) is the VGA /
     * BIOS-ROM hole, not usable RAM, and the kernel's BSS no longer fits under
     * 640 KB. Linking it at 0x100000 keeps every global in real memory. It is
     * NOLOAD (zero-filled at boot by entry.asm) so it adds nothing to the image.
     * Stack (0x1F0000) and heap (0x200000) sit safely above it.
     */
    . = 0x100000;
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    __bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
