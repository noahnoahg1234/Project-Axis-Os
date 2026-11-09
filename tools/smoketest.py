#!/usr/bin/env python3
"""AxisOS smoke test: boots the image in QEMU and checks the main features.

    make && python3 tools/smoketest.py [--shots DIR]

Needs qemu-system-i386. Each check prints PASS/FAIL; the exit code is the
number of failures.
"""
import os
import shutil
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
