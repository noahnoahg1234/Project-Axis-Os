# Writing programs for AxisOS

AxisOS can run small **32-bit Linux and FreeBSD** command-line programs. They
must be **position-independent** (`-pie`) so the loader can place them anywhere
in memory. Windows `.exe` files are not supported.

## Build

```bash
# a Linux program
gcc -m32 -fpie -pie -nostdlib -ffreestanding -Os -o hello hello.c

# a FreeBSD-style program (mark its ELF OS/ABI as FreeBSD)
gcc -m32 -fpie -pie -nostdlib -ffreestanding -o bsdhello bsdhello.S
printf '\x09' | dd of=bsdhello bs=1 seek=7 count=1 conv=notrunc   # EI_OSABI = FreeBSD
```

Copy the file into AxisOS (for example with `wget http://.../hello`) and run it:

```
./hello
```

## System calls

The compatibility layer emulates a useful subset of the Linux i386 `int 0x80`
calls (exit, read, write, open, close, lseek, brk, uname, gettimeofday,
nanosleep, writev, getrandom, ...) and a few FreeBSD ones (exit, read, write,
open, close, obreak, getpid, __sysctl, gettimeofday).

`uname` (Linux) and `__sysctl kern.ostype` (FreeBSD) report the program's own
system - a Linux program is told "Linux", a FreeBSD program "FreeBSD". You can
override this for all programs with the `osname` command (the name converter),
and `highpower on` gives a program a much larger memory arena.

These are freestanding examples (no libc). A full static libc is usually too
large and uses calls this simple layer does not implement.
