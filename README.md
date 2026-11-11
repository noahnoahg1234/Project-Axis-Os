# AxisOS

A small 32-bit x86 operating system written from scratch, with its own
bootloader, that combines **Linux**, **Windows**, **FreeBSD**, **MS-DOS** and **Unix** command lines
in one system.

AxisOS is its own kernel. It does not run real Linux, Windows or FreeBSD
programs; instead it gives you each system's commands, prompt, path style and
look, and lets you use all of them together.

## Download

Grab the ready-to-run disk image from the **[Releases page](https://github.com/noahnoahg1234/Project-Axis-Os/releases/latest)** (the download button is on the right of the repo page, under *Releases*). Download **axisos.img**, then:

```bash
qemu-system-i386 -drive file=axisos.img,format=raw -nic user,model=e1000
```

Install QEMU first from https://www.qemu.org/download/. On Windows, use the full path to `qemu-system-i386.exe`. Or build it yourself with `make` (see below).

**Using PowerISO (or similar tools):** `axisos.img` is a raw hard-disk image. PowerISO can open it directly (**File -> Open**), **mount** it, **convert** it to `.iso`, or **burn / write it to a USB stick** to boot a real PC. In a virtual machine (VirtualBox / VMware / QEMU) attach `axisos.img` as the **hard disk** and boot from it.

## Features

| Feature | Details |
|---|---|
| **AxisBoot bootloader** | Custom two-stage BIOS bootloader with a menu for Axis/Linux, Axis/Windows, Axis/FreeBSD, Axis/DOS and Axis/Unix, an Advanced-mode toggle, quick-boot keys (1-3) and a countdown. It reads its default system and timeout from the AxisOS settings. |
| **Five systems** | Each one has its own prompt (`user@axis:~$`, `C:\Users\user>`, `user@axis:~ %`), colours, path style, error messages, boot screen and crash screen. Switch at any time with `os linux` / `os windows` / `os freebsd` / `os dos` / `os unix` (or `wsl`, `cmd`, `bsd`, `command`, `sh`). The MS-DOS personality has a `C:\>` prompt, `DIR`/`TYPE`/`COPY`/`DEL`, `VER`, `MEM`, and the classic `Bad command or file name` message. |
| **All commands everywhere** | `ls` and `dir`, `cat` and `type`, `rm` and `del`, `cp` and `copy`, `clear` and `cls`, `sysctl`, `chflags`, `pciconf`, `ee`, `tasklist`, `systeminfo` and more all work in every system. Run `help` to see the table. |
| **Not case-sensitive** | `LS`, `Dir`, `CAT ~/WELCOME.TXT` all work. File names keep their case but are matched without it. |
| **Autocorrect** | Mistyped commands and paths are fixed (`sl` becomes `ls`, `cd documnets` becomes `cd Documents`). You can set it to Automatic, Suggest-only or Off. |
| **Shell** | Pipes `\|`, redirection `> >> <`, `;` `&&` `\|\|`, `$VAR` / `%VAR%` variables, aliases (`alias` / `doskey`), history (Up/Down), Tab completion, line editing. |
| **File explorer** | `explorer` / `files`: two-pane file manager with preview, copy/cut/paste, rename, delete, new file/folder, go-to-path, find and "open a shell here". |
| **Text editor** | `nano` / `edit` / `ee`: full-screen editor with save, find, cut/paste line and go-to-line. |
| **Settings app** | `settings` / `control` / `bsdconfig`: default system, boot timeout, colour theme (6), autocorrect, advanced-at-boot, delete confirmations, auto-save, hidden files, 12/24-hour clock, time zone, computer name, user name, switch system now, reset files. |
| **Advanced mode** | Harder to use, more powerful: no autocorrect, no confirmations, terse `?` errors, a root prompt (`#`), a hex view in the explorer, and expert commands (`peek`, `poke`, `inb`, `outb`, `sector`, `cpuid`, `heap`, `dmesg`, `hexdump`, `format`, `panic`). Turn it on with `advanced on`, `su`, `sudo <cmd>`, or the A key in the boot menu. |
| **Web browser** | `browser` / `msedge` / `lynx`: text-mode browser with links, history, bookmarks, downloading files (press W), and saving pages. Plain `http://` sites load directly; `https://` sites and web searches (S key) go through [FrogFind](http://frogfind.com), which serves simple http versions of any page. |
| **Networking** | Two network-card drivers (Intel **e1000** and Realtek **RTL8139**), auto-detected, with DHCP, DNS, TCP and HTTP, link detection, DNS caching with fallback to public resolvers (1.1.1.1 / 8.8.8.8), and a reachability check. `net` shows status, `net test` diagnoses the connection, `net reconnect` re-runs DHCP; `ifconfig` shows `link` and `internet` status. Commands: `ifconfig` / `ipconfig`, `ping`, `nslookup`, `wget` / `curl`. QEMU's default network card works with no extra options. |
| **App Store** | `store` / `winget` / `apt` / `pkg`: install and remove apps. Built in: Calculator, Minesweeper, Paint, Task Monitor (Windows-style), Snake, Big Clock, Fortune (Linux-style), BlockWorld, Tetris, 2048, Tic-Tac-Toe, Pong, Hangman, Guess the Number (Games), Todo List, Unit Converter (Tools). Online repositories (press A) add more apps over http, including real **Linux, Windows-style and FreeBSD** programs (shown under the Programs category). Installed apps run by typing their name; `apps` lists them. |
| **Shello** | `shello`: a retro, minimal shell with a phosphor-green look and short commands (`LIST`, `GO`, `SHOW`, `NEW`, `DEL`, `MAKE`, `TIME`, `MEM`, `BEEP`, `TINT`, `RUN`). `BYE` returns to the main shell. |
| **BlockWorld** | A built-in Minecraft-style block sandbox (`blockworld`, also `mine` / `craft`, or install it from the App Store). Explore biomes (plains, forest, desert, snow, jungle, mountains, oceans), swim in flowing water, mine and place blocks, an inventory and hotbar GUI, crafting, mobs (pigs, cows, sheep, zombies, skeletons, spiders, creepers), health, day/night, **Survival and Creative** modes, a **cheat console** (`/creative`, `/give`, `/tp`, `/time`, `/spawn`, `/fly`, ...), and **saved worlds** in `/Worlds`. Keyboard or mouse. |
| **7-Zip (AxisZip)** | `7z` / `zip` / `unzip`: create and open real `.zip` files, with a built-in DEFLATE decoder so it extracts compressed zips (not just stored ones). `7z a out.zip files`, `7z x file.zip`, `7z l file.zip`. |
| **Media Player** | `player` (also `media` / `music`): plays music through the **PC speaker** - built-in public-domain tunes plus your own `.sng` files in `/Music` - with a playlist and an animated bar visualizer. |
| **Compatibility layer** | Runs small 32-bit **Linux and FreeBSD** command-line programs (ELF, built with `gcc -pie`). Download one and run it by name, e.g. `wget http://.../hello` then `./hello`. See `programs/`. Windows `.exe` is not supported. |
| **High Power mode** | `highpower on` gives a running program a much larger memory arena so heavier programs fit. It raises the memory limit only - it cannot make the CPU itself faster. |
| **Name converter** | Each program is told which system it is on: a Linux program sees "Linux", a FreeBSD program sees "FreeBSD" (via `uname` / `sysctl`). `osname auto\|linux\|windows\|freebsd` overrides what every program is told. |
| **Retro desktop** | `desktop` (or `desktop` inside Shello): a text-mode, mouse-driven desktop in the style of the DOS Shell / Windows 3.1 - clickable app icons, a Start menu and a clock. Works with the keyboard too. |
| **Network settings** | `netconf` (or the Network desktop icon): choose automatic (DHCP) or a static IP, mask, gateway and DNS, and reconnect. |
| **Archiver (AxisRAR)** | `axar` (also `winrar`): pack files and folders into one compressed `.axar` archive and extract them. `axar a backup.axar folder`, `axar x backup.axar`, `axar l backup.axar`. |
| **AI assistant** | `assistant` (also `ai`): a small offline helper built into AxisOS - no internet or API key. Ask how to do things in plain words ("how do I copy a file"), and it can run actions for you ("open the browser"). |
| **Scripts** | `.sh` / `.bat` files run with `sh file` or by name. Besides normal commands they support `:label`, `goto`, `if A == B then ...`, `input VAR prompt`, `let VAR = expr`, `random VAR max` and `pause`. Store apps are written this way. |
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

To give AxisOS a network card (for the browser, App Store and downloads), the
Makefile's `run` target already adds QEMU's default e1000. On your own command
line use `-nic user,model=e1000`.

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

This boots each system in QEMU and checks about 40 behaviours: autocorrect,
case-insensitivity, pipes, the Windows and FreeBSD commands, Advanced mode,
the explorer, networking, the browser, App Store installs, Shello, and
settings and files persisting across a reboot.

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
browser                  web browser (S searches the web)
store                    App Store (games, paint, calculator ...)
shello                   retro shell (BYE to leave)
```

## App repositories

AxisOS can't make secure https:// connections, so online app repositories must be served over plain http://.
The `store/` folder in this repository is a sample repository: `index.txt` lists the apps, one per line
(`command|Name|Category|Description|script.sh`), next to the scripts.

To try it, serve the folder from your PC:

```bash
cd store
python3 -m http.server 8000
```

Then in AxisOS open `store`, press **A** and enter `http://10.0.2.2:8000/index.txt`
(10.0.2.2 is your PC as seen from inside QEMU). Upload the folder to any http web host to share it.

AxisOS runs its own apps only: it cannot run Windows `.exe` files or Linux programs.

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
kernel/net.c        e1000 driver, ARP/IP/ICMP/UDP/DHCP/DNS/TCP, HTTP client
kernel/browser.c    text web browser
kernel/store.c      App Store, installed apps, script interpreter
kernel/games.c      Calculator, Minesweeper, Paint, Snake, Clock, Task Monitor
kernel/blockworld.c BlockWorld sandbox game (biomes, water, mobs)
kernel/zip.c        AxisZip (7-Zip-style) with a DEFLATE decoder
kernel/mediaplayer.c PC-speaker music player
kernel/shello.c     Shello retro shell
kernel/desktop.c    retro text-mode mouse desktop
kernel/compat.c     Linux/FreeBSD ELF loader + system-call layer
kernel/mouse.c      PS/2 mouse driver
kernel/assistant.c  offline AI assistant
kernel/archiver.c   AxisRAR (.axar) archiver
kernel/netconf.c    network settings screen
kernel/editor.c     text editor
kernel/apps.c       settings app, pager, dialogs
kernel/*.c          console, keyboard, ATA disk, RTC, serial, interrupts, heap
tools/mkimage.py    builds the disk image
tools/smoketest.py  automated QEMU test
store/              sample online app repository (apps + Linux/FreeBSD programs)
programs/           sample Linux/FreeBSD programs and how to build them
```

### Disk layout

| LBA | Contents |
|---|---|
| 0 | AxisBoot stage 1 (MBR, `AXISDISK` signature) |
| 1-16 | AxisBoot stage 2 |
| 17 | Settings sector (shared by AxisBoot and the kernel) |
| 64+ | Kernel (loaded at `0x10000`) |
| 2048-2559 | Saved filesystem |
