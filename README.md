# AxisOS

A small 32-bit x86 operating system written from scratch, with its own
bootloader, that combines **Linux**, **Windows** and **FreeBSD** command lines
in one system.

AxisOS is its own kernel. It does not run real Linux, Windows or FreeBSD
programs; instead it gives you each system's commands, prompt, path style and
look, and lets you use all of them together.

## Features

| Feature | Details |
|---|---|
| **AxisBoot bootloader** | Custom two-stage BIOS bootloader with a menu for Axis/Linux, Axis/Windows and Axis/FreeBSD, an Advanced-mode toggle, quick-boot keys (1-3) and a countdown. It reads its default system and timeout from the AxisOS settings. |
| **Three systems** | Each one has its own prompt (`user@axis:~$`, `C:\Users\user>`, `user@axis:~ %`), colours, path style, error messages, boot screen and crash screen. Switch at any time with `os linux` / `os windows` / `os freebsd` (or `wsl`, `cmd`, `bsd`). |
| **All commands everywhere** | `ls` and `dir`, `cat` and `type`, `rm` and `del`, `cp` and `copy`, `clear` and `cls`, `sysctl`, `chflags`, `pciconf`, `ee`, `tasklist`, `systeminfo` and more all work in every system. Run `help` to see the table. |
| **Not case-sensitive** | `LS`, `Dir`, `CAT ~/WELCOME.TXT` all work. File names keep their case but are matched without it. |
| **Autocorrect** | Mistyped commands and paths are fixed (`sl` becomes `ls`, `cd documnets` becomes `cd Documents`). You can set it to Automatic, Suggest-only or Off. |
| **Shell** | Pipes `\|`, redirection `> >> <`, `;` `&&` `\|\|`, `$VAR` / `%VAR%` variables, aliases (`alias` / `doskey`), history (Up/Down), Tab completion, line editing. |
| **File explorer** | `explorer` / `files`: two-pane file manager with preview, copy/cut/paste, rename, delete, new file/folder, go-to-path, find and "open a shell here". |
| **Text editor** | `nano` / `edit` / `ee`: full-screen editor with save, find, cut/paste line and go-to-line. |
| **Settings app** | `settings` / `control` / `bsdconfig`: default system, boot timeout, colour theme (6), autocorrect, advanced-at-boot, delete confirmations, auto-save, hidden files, 12/24-hour clock, time zone, computer name, user name, switch system now, reset files. |
| **Advanced mode** | Harder to use, more powerful: no autocorrect, no confirmations, terse `?` errors, a root prompt (`#`), a hex view in the explorer, and expert commands (`peek`, `poke`, `inb`, `outb`, `sector`, `cpuid`, `heap`, `dmesg`, `hexdump`, `format`, `panic`). Turn it on with `advanced on`, `su`, `sudo <cmd>`, or the A key in the boot menu. |
| **Unified filesystem** | One case-insensitive tree with `/home/user`, `C:\Users\user`, `/Windows`, `/usr/local` and more. It is saved to the AxisOS disk, so files and settings persist across reboots. |

## Build and run

You need `nasm`, `gcc` (with `-m32` support), GNU `ld`, `python3` and QEMU.

```bash
sudo apt install nasm gcc binutils python3 qemu-system-x86
```

```bash
make
```

```bash
make run
```

`make run` opens a QEMU window and also mirrors the console on your terminal
(serial). `make run-nographic` uses the terminal only.

The image `build/axisos.img` is a raw 4 MiB BIOS disk image. It also works in
VirtualBox or VMware (convert it with `VBoxManage convertfromraw`) as an IDE
disk. Settings and files are saved only to a disk that carries the AxisOS
signature, so AxisOS never writes to any other disk.

### Tests

```bash
python3 tools/smoketest.py
```

This boots each system in QEMU and checks about 30 behaviours: autocorrect,
case-insensitivity, pipes, the Windows and FreeBSD commands, Advanced mode,
the explorer, and settings and files persisting across a reboot.

## Try these

```
help | more              every command, with its Linux / Windows / FreeBSD name
sl                       autocorrected to ls
cd documnets             autocorrected to Documents
DIR /W                   Windows dir, any letter case
cat ~/welcome.txt | grep help > found.txt
os windows               switch to Axis/Windows (cd.., type, %USERNAME% ...)
explorer                 file explorer
settings                 change theme, default system, autocorrect ...
advanced on              expert mode (try: heap, cpuid, sector 0, panic)
```

## Layout

```
boot/stage1.asm     AxisBoot MBR: loads stage 2
boot/stage2.asm     AxisBoot menu, kernel loader, switch to protected mode
kernel/entry.asm    kernel entry, interrupt stubs
kernel/main.c       start-up and per-system boot screens
kernel/shell.c      axsh: parser, pipes, autocorrect, history, completion
kernel/commands.c   all built-in commands and the command table
kernel/fs.c         AxisFS: unified case-insensitive filesystem + persistence
kernel/explorer.c   file explorer
kernel/editor.c     text editor
kernel/apps.c       settings app, pager, dialogs
kernel/*.c          console, keyboard, ATA disk, RTC, serial, interrupts, heap
tools/mkimage.py    builds the disk image
tools/smoketest.py  automated QEMU test
```

### Disk layout

| LBA | Contents |
|---|---|
| 0 | AxisBoot stage 1 (MBR, `AXISDISK` signature) |
| 1-16 | AxisBoot stage 2 |
| 17 | Settings sector (shared by AxisBoot and the kernel) |
| 64+ | Kernel (loaded at `0x10000`) |
| 2048-2559 | Saved filesystem |
