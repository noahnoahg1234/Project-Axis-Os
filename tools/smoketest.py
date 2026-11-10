#!/usr/bin/env python3
"""AxisOS smoke test: boots the image in QEMU and checks the main features.

    make && python3 tools/smoketest.py [--shots DIR]

Needs qemu-system-i386. Each check prints PASS/FAIL; the exit code is the
number of failures.
"""
import functools
import http.server
import os
import shutil
import threading
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import AxisVM  # noqa: E402

IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "axisos.img")
failures = 0
shots = None
if "--shots" in sys.argv:
    shots = sys.argv[sys.argv.index("--shots") + 1]
    os.makedirs(shots, exist_ok=True)


def check(name, cond, detail=""):
    global failures
    print("%s  %s" % ("PASS" if cond else "FAIL", name))
    if not cond:
        failures += 1
        if detail:
            print("      got: " + detail.strip().replace("\n", "\n           ")[:600])


def shot(vm, name):
    if shots:
        vm.screenshot(os.path.join(shots, name + ".png"))


def boot(key, prompt, image=IMG, snapshot=True):
    vm = AxisVM(image, snapshot=snapshot)
    time.sleep(1.0)
    shot(vm, "bootmenu")
    vm.key(key)
    ok = vm.wait_for(prompt if prompt == ">" else prompt + " ", 20)
    time.sleep(0.3)
    return vm, ok


# ---------------------------------------------------------------- Linux ----
vm, ok = boot("1", "$")
check("boots into Axis/Linux", ok)
out = vm.run("ls", "$")
check("ls lists the home folder", "welcome.txt" in out, out)
out = vm.run("sl", "$")
check("autocorrect fixes 'sl' -> 'ls'", "autocorrect: sl -> ls" in out and "welcome.txt" in out, out)
out = vm.run("LS -L", "$")
check("commands are case-insensitive", "welcome.txt" in out and "rw" in out, out)
out = vm.run("cd documnets && pwd", "$")
check("autocorrect fixes paths", "/home/user/Documents" in out, out)
out = vm.run("cat ~/WELCOME.TXT | grep explorer", "$")
check("pipes + case-insensitive file names", "explorer" in out, out)
out = vm.run("echo one > /tmp/x.txt && echo two >> /tmp/x.txt && type /tmp/x.txt", "$")
check("redirection and Windows 'type' in Linux mode", "one" in out and "two" in out, out)
out = vm.run("dir /tmp", "$")
check("Windows 'dir' works in Linux mode", "x.txt" in out and "File(s)" in out, out)
out = vm.run("calc 2+3*(4-1)", "$")
check("calculator", "11" in out, out)
out = vm.run("frobnicate", "$")
check("unknown command message", "command not found" in out, out)
out = vm.run("peek 0x500", "$")
check("advanced commands are locked in normal mode", "Advanced mode command" in out, out)
vm.type("explorer\r")
time.sleep(0.8)
shot(vm, "explorer")
vm.key("q")
time.sleep(0.3)
out = vm.run("echo still here", "$")
check("explorer opens and quits back to the shell", "still here" in out, out)
vm.close()

# -------------------------------------------------------------- Windows ----
vm, ok = boot("2", ">")
check("boots into Axis/Windows", ok)
out = vm.run("dir", ">")
check("dir shows C:\\Users\\user", "Directory of C:\\Users\\user" in out, out)
out = vm.run("cd..", ">")
out = vm.run("cd", ">")
check("cd.. and cd (print cwd)", out.strip().endswith("C:\\Users"), out)
out = vm.run("type c:\\WINDOWS\\win.ini", ">")
check("drive letters and backslashes", "[fonts]" in out, out)
out = vm.run("ls /", ">")
check("Linux 'ls' works in Windows mode", "Windows" in out and "home" in out, out)
out = vm.run("echo %USERNAME%@%COMPUTERNAME%", ">")
check("%VAR% expansion", "user@axis" in out, out)
out = vm.run("foo", ">")
check("Windows-style unknown command message", "is not recognized" in out, out)
shot(vm, "windows")
vm.close()

# -------------------------------------------------------------- FreeBSD ----
vm, ok = boot("3", "%")
check("boots into Axis/FreeBSD", ok)
out = vm.run("sysctl kern.ostype", "%")
check("sysctl", "AxisOS" in out, out)
out = vm.run("uname -a", "%")
check("uname -a reports the FreeBSD personality", "FreeBSD" in out, out)
out = vm.run("advanced on", "#")
check("advanced mode switches the prompt", "Advanced mode" in out, out)
out = vm.run("sl", "#")
check("advanced mode has no autocorrect", "?" in out and "autocorrect" not in out, out)
out = vm.run("cpuid", "#")
check("advanced tools unlocked", "vendor" in out, out)
out = vm.run("exit", "%")
check("exit leaves advanced mode", "Left advanced mode" in out, out)
out = vm.run("os linux", "$")
check("switch personality at runtime", "Axis/Linux" in out, out)
vm.close()

# ------------------------------------- network, browser, store, shello ----
# serve the sample app repository (store/) on the host; QEMU's user network
# reaches the host as 10.0.2.2
store_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "store")
class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


handler = functools.partial(QuietHandler, directory=store_dir)
httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()

vm, ok = boot("1", "$")
out = vm.run("ifconfig", "$")
check("network card configured by DHCP", "10.0.2.15" in out, out)
out = vm.run("ping -c 1 10.0.2.2", "$")
check("ping", "1 received" in out, out)
out = vm.run("curl http://10.0.2.2:%d/index.txt | grep dice" % port, "$", 20)
check("HTTP download (curl)", "Dice Roller" in out, out)
out = vm.run("wget http://10.0.2.2:%d/quiz.sh && grep SCORE quiz.sh | head -n 1" % port, "$", 20)
check("wget saves files", "SCORE" in out, out)
vm.type("browser http://10.0.2.2:%d/\r" % port)
time.sleep(2.0)
shot(vm, "browser")
vm.key("q")
time.sleep(0.4)
out = vm.run("echo after browser", "$")
check("browser opens and closes", "after browser" in out, out)
vm.type("store\r")
time.sleep(1.0)
shot(vm, "store")
vm.key("down", "down", "down", "down", "down", "down", "down", "ret")   # Guess the Number
time.sleep(0.6)
vm.key("a")
time.sleep(0.3)
vm.type("10.0.2.2:%d/index.txt\r" % port)
time.sleep(3)
vm.key("ret")                                                          # install Dice Roller
time.sleep(2)
vm.key("q")
time.sleep(0.4)
out = vm.run("apps", "$")
check("store installs built-in and online apps", "guess" in out and "dice" in out, out)
vm.type("dice\r")
time.sleep(0.6)
out = vm.run("2", "$", 10)
check("downloaded script app runs", "Total:" in out, out)
# --- compatibility layer: real ELF programs ---
vm.run("wget http://10.0.2.2:%d/hello.elf -o hello && wget http://10.0.2.2:%d/bsdhello.elf -o bsd" % (port, port), "$", 20)
out = vm.run("./hello", "$", 10)
check("runs a real Linux ELF program", "real Linux ELF" in out, out)
out = vm.run("./bsd", "$", 10)
check("runs a FreeBSD-style ELF program", "FreeBSD-style" in out, out)
out = vm.run("highpower on", "$")
check("High Power mode toggles", "High Power mode is ON" in out, out)
out = vm.run("osname windows", "$")
check("name converter (osname) sets reported OS", "running on: Windows" in out, out)
vm.run("osname auto", "$")
# --- archiver ---
vm.run("mkdir arc && echo aaaaaaaaaa > arc/a.txt && echo bb > arc/b.txt", "$")
out = vm.run("axar a test.axar arc", "$")
check("archiver creates an archive", "Created test.axar" in out, out)
vm.run("rm -r arc", "$")
out = vm.run("winrar x test.axar && cat arc/a.txt", "$", 10)
check("archiver extracts (winrar alias)", "aaaaaaaaaa" in out, out)
# --- AI assistant ---
vm.type("assistant\r")
ok = vm.wait_for("Offline help", 5)
check("AI assistant opens", ok)
mark = len(vm.log)
vm.type("how do I install an app?\r")
ok = vm.wait_for("App Store", 5, mark)
check("AI assistant answers questions", ok)
vm.type("bye\r")
time.sleep(0.4)
# --- network settings & desktop ---
vm.type("netconf\r")
time.sleep(0.8)
shot(vm, "netconf")
vm.key("esc")
time.sleep(0.3)
out = vm.run("echo after netconf", "$")
check("network settings open and close", "after netconf" in out, out)
vm.type("desktop\r")
time.sleep(1.0)
shot(vm, "desktop")
vm.key("esc")
time.sleep(0.4)
out = vm.run("echo after desktop", "$")
check("desktop opens and closes", "after desktop" in out, out)
vm.close()
httpd.shutdown()

# ---------------------------------------------------------- persistence ----
tmp = tempfile.mkdtemp(prefix="axis-persist-")
img = os.path.join(tmp, "disk.img")
shutil.copy(IMG, img)
vm, ok = boot("ret", "$", image=img, snapshot=False)
vm.run("mkdir keep && echo saved-across-reboot > keep/f.txt", "$")
vm.type("settings\r")
time.sleep(0.8)
vm.key("right", "right")                    # default system -> FreeBSD
vm.key("down", "left", "left", "left")      # timeout -> 2 s
time.sleep(0.2)
shot(vm, "settings")
vm.key("s")
time.sleep(0.5)
vm.key("ret")
time.sleep(0.4)
mark = len(vm.log)
vm.type("reboot\r")
ok = vm.wait_for("FreeBSD personality", 25, mark)
check("saved default system is booted after the countdown", ok)
time.sleep(0.8)
out = vm.run("cat ~/keep/f.txt", "%")
check("files persist across reboot", "saved-across-reboot" in out, out)
vm.close()
shutil.rmtree(tmp, ignore_errors=True)

print("\n%d failure(s)" % failures)
sys.exit(failures)
