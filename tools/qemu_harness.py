#!/usr/bin/env python3
"""Drive AxisOS inside QEMU from Python (used by the smoke test).

Boots the image headless, talks to the shell through the serial console,
presses keys through the QEMU monitor and takes screenshots of the VGA screen.
"""
import os
import socket
import struct
import subprocess
import tempfile
import time
import zlib


class AxisVM:
    def __init__(self, image, memory=64, snapshot=True, no_reboot=False):
        self.dir = tempfile.mkdtemp(prefix="axisvm-")
        self.ser_path = os.path.join(self.dir, "serial")
        self.mon_path = os.path.join(self.dir, "monitor")
        drive = "file=%s,format=raw,if=ide" % image
        if snapshot:
            drive += ",snapshot=on"
        self.proc = subprocess.Popen(
            ["qemu-system-i386", "-drive", drive, "-m", str(memory), "-display", "none",
             "-serial", "unix:%s,server=on,wait=off" % self.ser_path,
             "-monitor", "unix:%s,server=on,wait=off" % self.mon_path] + (["-no-reboot"] if no_reboot else []),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.ser = self._connect(self.ser_path)
        self.mon = self._connect(self.mon_path)
        self.ser.setblocking(False)
        self.mon.setblocking(False)
        self.log = b""

    @staticmethod
    def _connect(path):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("cannot connect to " + path)

    def _pump(self):
        try:
            while True:
                d = self.ser.recv(65536)
                if not d:
                    break
                self.log += d
        except BlockingIOError:
            pass
        try:
            while True:
                if not self.mon.recv(65536):
                    break
        except BlockingIOError:
            pass

    def wait_for(self, text, timeout=15.0, start=0):
        end = time.time() + timeout
        t = text.encode()
        while time.time() < end:
            self._pump()
            if t in self.log[start:]:
                return True
            time.sleep(0.05)
        return False

    def monitor(self, cmd):
        self.mon.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        self._pump()

    def key(self, *keys):
        for k in keys:
            self.monitor("sendkey " + k)

    def type(self, text):
        self.ser.sendall(text.encode())

    def run(self, cmd, prompt_end, timeout=10.0):
        """Send a command line and return the output up to the next prompt."""
        self._pump()
        start = len(self.log)
        self.type(cmd + "\r")
        end = time.time() + timeout
        while time.time() < end:
            self._pump()
            out = self.log[start:].decode("latin-1")
            idx = out.find("\n")
            if idx >= 0 and out[idx:].rstrip().endswith(prompt_end):
                body = out[idx + 1:]
                return body[: body.rfind("\n") + 1] if "\n" in body else ""
            time.sleep(0.05)
        return self.log[start:].decode("latin-1")

    def screenshot(self, png_path):
        ppm = os.path.join(self.dir, "shot.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.monitor("screendump " + ppm)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.1)
        time.sleep(0.2)
        ppm_to_png(ppm, png_path)

    def close(self):
        try:
            self.monitor("quit")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def ppm_to_png(src, dst):
    data = open(src, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]
    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)
