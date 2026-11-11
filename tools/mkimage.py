#!/usr/bin/env python3
"""Assemble the AxisOS disk image.

Layout (512-byte sectors):
  LBA 0          AxisBoot stage 1 (MBR, carries the AXISDISK signature)
  LBA 1-16       AxisBoot stage 2 (boot menu)
  LBA 17         settings sector (written by the Settings app)
  LBA 64-...     kernel (loaded to 0x10000)
  LBA 2048-2559  AxisFS (persisted filesystem)
  total          4 MiB
"""
import sys

SECTOR = 512
STAGE2_SECTORS = 16
KERNEL_LBA = 64
FS_LBA = 2048
TOTAL_SECTORS = 8192


def main():
    out, s1, s2, kern = sys.argv[1:5]
    total = int(sys.argv[5]) if len(sys.argv) > 5 else TOTAL_SECTORS
    stage1 = open(s1, "rb").read()
    stage2 = open(s2, "rb").read()
    kernel = open(kern, "rb").read()
    if len(stage1) != SECTOR or stage1[510:] != b"\x55\xaa":
        sys.exit("stage1 must be exactly one boot sector")
    if len(stage2) > STAGE2_SECTORS * SECTOR:
        sys.exit("stage2 too large")
    ksec = (len(kernel) + SECTOR - 1) // SECTOR
    if KERNEL_LBA + ksec > total:
        sys.exit("kernel too large (%d sectors)" % ksec)
    if 0x10000 + ksec * SECTOR > 0x80000:
        sys.exit("kernel does not fit below 0x80000")
    img = bytearray(total * SECTOR)
    img[0:SECTOR] = stage1
    img[SECTOR:SECTOR + len(stage2)] = stage2
    img[KERNEL_LBA * SECTOR:KERNEL_LBA * SECTOR + len(kernel)] = kernel
    with open(out, "wb") as f:
        f.write(img)
    print("%s: kernel %d bytes (%d sectors), stage2 %d bytes" % (out, len(kernel), ksec, len(stage2)))


if __name__ == "__main__":
    main()
