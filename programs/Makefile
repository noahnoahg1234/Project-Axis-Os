# Build the sample AxisOS programs
CC := gcc
CFLAGS := -m32 -fpie -pie -nostdlib -ffreestanding -Os

all: hello greet bsdhello

hello: hello.c
	$(CC) $(CFLAGS) -o $@ $<
greet: greet.c
	$(CC) $(CFLAGS) -o $@ $<
bsdhello: bsdhello.S
	$(CC) -m32 -fpie -pie -nostdlib -ffreestanding -o $@ $<
	printf '\011' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

clean:
	rm -f hello greet bsdhello
.PHONY: all clean
