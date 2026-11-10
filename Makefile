# AxisOS build
#   make                build build/axisos.img (bootable raw disk image)
#   make run            boot it in QEMU (window + serial console in the terminal)
#   make run-nographic  boot it with only the serial console
#   make clean

CC      := gcc
LD      := ld
NASM    := nasm
OBJCOPY := objcopy
QEMU    := qemu-system-i386

CFLAGS  := -m32 -march=i686 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
           -fno-builtin -nostdlib -mgeneral-regs-only -fno-asynchronous-unwind-tables \
           -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 --param=min-pagesize=0
LDFLAGS := -m elf_i386 -T kernel/linker.ld -nostdlib -z noexecstack --no-warn-rwx-segments

C_SRCS  := $(wildcard kernel/*.c)
OBJS    := build/entry.o $(patsubst kernel/%.c,build/%.o,$(C_SRCS))

IMG     := build/axisos.img

all: $(IMG)

build:
	mkdir -p build

build/entry.o: kernel/entry.asm | build
	$(NASM) -f elf32 $< -o $@

build/%.o: kernel/%.c kernel/axis.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.elf: $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

build/kernel.bin: build/kernel.elf
	$(OBJCOPY) -O binary $< $@

build/stage1.bin: boot/stage1.asm | build
	$(NASM) -f bin $< -o $@

build/stage2.bin: boot/stage2.asm build/kernel.bin
	$(NASM) -f bin -DKERNEL_SECTORS=$$(( ($$(stat -c%s build/kernel.bin) + 511) / 512 )) $< -o $@

$(IMG): build/stage1.bin build/stage2.bin build/kernel.bin tools/mkimage.py
	python3 tools/mkimage.py $@ build/stage1.bin build/stage2.bin build/kernel.bin

run: $(IMG)
	$(QEMU) -drive file=$(IMG),format=raw,if=ide -m 64 -nic user,model=e1000 -serial stdio

run-nographic: $(IMG)
	$(QEMU) -drive file=$(IMG),format=raw,if=ide -m 64 -nic user,model=e1000 -display none -serial stdio

clean:
	rm -f build/*.o build/*.bin build/*.elf build/*.img

.PHONY: all run run-nographic clean
