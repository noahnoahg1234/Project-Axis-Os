/* AxisOS - interrupts, PIC, PIT timer and the per-system crash screens */
#include "axis.h"

typedef struct __attribute__((packed)) {
    uint16_t off_lo, sel;
    uint8_t zero, flags;
    uint16_t off_hi;
} idt_entry_t;
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

extern uint32_t isr_table[48];
static idt_entry_t idt[256];
static void (*irq_handlers[16])(regs_t *);
volatile uint32_t ticks;

static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range exceeded",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 floating point", "Alignment check", "Machine check",
    "SIMD floating point", "Virtualization", "Control protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Security", "Reserved"};
static const char *win_stop[32] = {
    "KMODE_EXCEPTION_NOT_HANDLED (DIVIDE_BY_ZERO)", "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", "NMI_HARDWARE_FAILURE",
    "KMODE_EXCEPTION_NOT_HANDLED (BREAKPOINT)", "INTEGER_OVERFLOW", "KMODE_EXCEPTION_NOT_HANDLED",
    "ILLEGAL_INSTRUCTION", "NO_EXCEPTION_HANDLING_SUPPORT", "UNEXPECTED_KERNEL_MODE_TRAP (DOUBLE_FAULT)",
    "UNEXPECTED_KERNEL_MODE_TRAP", "INVALID_TSS", "SEGMENT_NOT_PRESENT", "KERNEL_STACK_INPAGE_ERROR",
    "SYSTEM_SERVICE_EXCEPTION", "PAGE_FAULT_IN_NONPAGED_AREA"};

static void idt_set(int n, uint32_t handler) {
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].off_hi = handler >> 16;
    idt[n].sel = 0x08;
    idt[n].zero = 0;
    idt[n].flags = 0x8E;
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xF8); /* enable IRQ0 (timer), IRQ1 (keyboard), IRQ2 (cascade) */
    outb(0xA1, 0xFF);
}

static void timer_irq(regs_t *r) {
    (void)r;
    ticks++;
}

void idt_init(void) {
    for (int i = 0; i < 48; i++) idt_set(i, isr_table[i]);
    idt_ptr_t p = {sizeof idt - 1, (uint32_t)idt};
    __asm__ volatile("lidt %0" ::"m"(p));
    pic_remap();
    uint16_t div = 1193182 / TICK_HZ;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_set_handler(0, timer_irq);
}

void irq_set_handler(int irq, void (*h)(regs_t *)) { irq_handlers[irq] = h; }

void sleep_ms(uint32_t ms) {
    uint32_t end = ticks + (ms * TICK_HZ + 999) / 1000;
    while ((int32_t)(end - ticks) > 0) cpu_hlt();
}

static void wait_key_and_reboot(void) {
    /* interrupts are off: poll the keyboard controller and serial port */
    while (inb(0x64) & 1) inb(0x60);
    for (;;) {
        if ((inb(0x64) & 1) && !(inb(0x60) & 0x80)) break;
        if (serial_getc() >= 0) break;
    }
    sys_reboot();
}

static void panic_screen(regs_t *r) {
    irq_off();
    con_serial_mirror(true);
    uint32_t cr2;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    const char *name = exc_names[r->int_no & 31];
    uint32_t up = ticks / TICK_HZ;
    while (con_capturing()) {
        char *b = con_capture_end(NULL);
        kfree(b);
    }
    if (cur_os == OS_WINDOWS) {
        con_set_attr(ATTR(C_WHITE, C_BLUE));
        con_clear();
        kprintf("\n\n    :(\n\n");
        kprintf("    Your PC ran into a problem and needs to restart. We're just\n");
        kprintf("    collecting some error info, and then you can restart.\n\n");
        kprintf("    0%% complete\n\n\n");
        const char *stop = (r->int_no < 15 && win_stop[r->int_no]) ? win_stop[r->int_no] : "UNEXPECTED_KERNEL_MODE_TRAP";
        kprintf("    Stop code: %s\n", stop);
        kprintf("    What failed: axiskrnl.sys  (EIP %08x, error %08x)\n\n", r->eip, r->err);
        kprintf("    Press any key to restart.\n");
    } else if (cur_os == OS_FREEBSD) {
        con_set_attr(ATTR(C_LGRAY, C_BLACK));
        con_clear();
        kprintf("\n\nFatal trap %u: %s while in kernel mode\n", r->int_no, name);
        kprintf("cpuid = 0; apic id = 00\n");
        if (r->int_no == 14) kprintf("fault virtual address   = 0x%x\n", cr2);
        kprintf("instruction pointer     = 0x%x:0x%x\n", r->cs, r->eip);
        kprintf("stack pointer           = 0x10:0x%x\n", r->esp_dummy);
        kprintf("code segment            = base 0x0, limit 0xfffff, type 0x1b\n");
        kprintf("processor eflags        = 0x%x\n", r->eflags);
        kprintf("current process         = 1 (axsh)\n");
        kprintf("trap number             = %u\n", r->int_no);
        kprintf("panic: %s\n", name);
        kprintf("cpuid = 0\nUptime: %us\n", up);
        kprintf("Automatic reboot disabled - press a key on the console to reboot\n");
    } else {
        con_set_attr(ATTR(C_LGRAY, C_BLACK));
        con_clear();
        kprintf("[%5u.%06u] %s: 0000 [#1] AxisOS i686\n", up, (ticks % TICK_HZ) * 10000, name);
        kprintf("[%5u.%06u] CPU: 0 PID: 1 Comm: axsh Not tainted %s-axis\n", up, 0, AXIS_VERSION);
        kprintf("[%5u.%06u] EIP: 0x%08x  EFLAGS: %08x  CS: %04x  ERR: %08x\n", up, 0, r->eip, r->eflags, r->cs, r->err);
        kprintf("[%5u.%06u] EAX: %08x EBX: %08x ECX: %08x EDX: %08x\n", up, 0, r->eax, r->ebx, r->ecx, r->edx);
        kprintf("[%5u.%06u] ESI: %08x EDI: %08x EBP: %08x CR2: %08x\n", up, 0, r->esi, r->edi, r->ebp, cr2);
        kprintf("[%5u.%06u] ---[ end trace 0000000000000000 ]---\n", up, 0);
        con_set_attr(ATTR(C_WHITE, C_BLACK));
        kprintf("[%5u.%06u] Kernel panic - not syncing: Fatal exception (%s)\n", up, 0, name);
        kprintf("\nPress any key to reboot.\n");
    }
    wait_key_and_reboot();
}

void isr_dispatch(regs_t *r) {
    if (r->int_no < 32) {
        panic_screen(r);
        return;
    }
    int irq = (int)r->int_no - 32;
    if (irq_handlers[irq]) irq_handlers[irq](r);
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
