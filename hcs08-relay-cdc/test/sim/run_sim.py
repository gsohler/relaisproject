#!/usr/bin/env python3
"""run_sim.py - runs the bootloader's disk code (built with SDCC, same flags
as the bootloader) in the uCsim HCS08 simulator (package sdcc-ucsim) and
compares the results with the gcc build.

    python3 test/sim/run_sim.py build/relay_fw-<version>.s19

Checks:
  1. disk_read() of the volume sectors gives the same bytes as the gcc build
  2. writing the firmware file programs the same bytes as in the file and
     ends with status OK (CRC check passed, info block written)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FLAGS = ["-ms08", "--std-sdcc11", "--opt-code-size", "--stack-auto", "-DXTAL_MHZ=12",
         "-I" + os.path.join(ROOT, "src"), "-I" + os.path.join(ROOT, "boot")]
MEM = ["--code-loc", "0xE800", "--data-loc", "0x00B0", "--xram-loc", "0x0100",
       "--xram-size", "0x0B00", "--stack-loc", "0xE7FF"]
APP_START, APP_INFO, BOOT_START = 0x1960, 0xDBB0, 0xDC00
UPD_OK = 2


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit("run_sim: %s failed:\n%s%s" % (cmd[0], r.stdout, r.stderr))
    return r.stdout


def ihx(path, chunks):
    """chunks: list of (addr, bytes)"""
    with open(path, "w") as f:
        for addr, data in chunks:
            for i in range(0, len(data), 16):
                d = data[i:i + 16]
                a = addr + i
                rec = bytes([len(d), a >> 8, a & 0xFF, 0]) + d
                f.write(":%s%02X\n" % (rec.hex().upper(), (-sum(rec)) & 0xFF))
        f.write(":00000001FF\n")


def simulate(work, files, dumps):
    """runs until sim_done(), returns {addr: bytes} for the dump ranges"""
    m = re.search(r"([0-9A-F]{8})\s+_sim_done\b", open(os.path.join(work, "sim.map")).read())
    stop = int(m.group(1), 16)
    cmds = "reset\nbreak 0x%04x\nrun\n" % stop
    for a, n in dumps:
        cmds += "dump rom 0x%04x 0x%04x 16\n" % (a, a + n - 1)
    cmds += "quit\n"
    out = subprocess.run(["shc08", "-t", "HCS08"] + files, input=cmds, capture_output=True,
                         text=True, timeout=600).stdout
    if "Stop at 0x%06x" % stop not in out and "0x%04x" % stop not in out.lower():
        sys.exit("run_sim: simulation did not reach sim_done():\n" + out[-2000:])
    mem = {}
    for line in out.splitlines():
        m = re.match(r"^0x([0-9a-f]{4})\s+((?:[0-9a-f]{2} ?)+)", line.strip())
        if m:
            base = int(m.group(1), 16)
            for i, b in enumerate(m.group(2).split()):
                mem[base + i] = int(b, 16)
    return mem


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if not shutil.which("shc08"):
        sys.exit("run_sim: shc08 not found (sudo apt install sdcc-ucsim)")
    fw_path = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="relay_sim_")
    objs = []
    for src in ["test/sim/sim_main.c", "boot/disk.c", "boot/util.c", "test/sim/flash_sim.c"]:
        obj = os.path.join(work, os.path.basename(src).replace(".c", ".rel"))
        run(["sdcc"] + FLAGS + MEM + ["-c", os.path.join(ROOT, src), "-o", obj])
        objs.append(obj)
    run(["sdcc", "-ms08", "--out-fmt-ihx"] + MEM + objs + ["-o", os.path.join(work, "sim.ihx")])
    run(["gcc", "-DHOST_TEST", "-I" + os.path.join(ROOT, "src"), "-I" + os.path.join(ROOT, "boot"),
         "-o", os.path.join(work, "host_image"), os.path.join(HERE, "host_image.c"),
         os.path.join(ROOT, "boot/disk.c"), os.path.join(ROOT, "boot/util.c"),
         os.path.join(ROOT, "src/flash.c")], cwd=work)
    image = subprocess.run([os.path.join(work, "host_image")], capture_output=True).stdout

    fails = 0
    # 1. read the volume sectors
    for lba in (0, 1, 2, 7, 13, 14, 15, 16, 2047):
        p = os.path.join(work, "params.ihx")
        ihx(p, [(0x0C00, bytes([lba >> 8, lba & 0xFF, 0, 0, 0]))])
        mem = simulate(work, [os.path.join(work, "sim.ihx"), p], [(0x0C10, 512)])
        got = bytes(mem.get(0x0C10 + i, -1) & 0xFF for i in range(512))
        want = image[lba * 512:(lba + 1) * 512]
        if got != want:
            fails += 1
            diff = [i for i in range(512) if got[i] != want[i]]
            print("FAIL sector %d: %d bytes differ, first at %d: sim %02X gcc %02X"
                  % (lba, len(diff), diff[0], got[diff[0]], want[diff[0]]))
    print("read: %d sectors compared" % 9)

    # 2. write the firmware file
    text = open(fw_path, "rb").read()
    if len(text) > 0x4000 - 64:
        sys.exit("run_sim: firmware file too large for the harness")
    p = os.path.join(work, "params.ihx")
    ihx(p, [(0x0C00, bytes([0, 0, 0xEE, 0, 1])), (0x8000, text + bytes(64))])
    expect = {}
    for line in text.decode().splitlines():
        if line.startswith("S1"):
            raw = bytes.fromhex(line[2:])
            for i, b in enumerate(raw[3:-1]):
                expect[(raw[1] << 8 | raw[2]) + i] = b
    code_end = max(a for a in expect if a < APP_INFO) + 1
    mem = simulate(work, [os.path.join(work, "sim.ihx"), p],
                   [(0x0C02, 1), (APP_START, code_end - APP_START), (APP_INFO, BOOT_START - APP_INFO)])
    if mem.get(0x0C02) != UPD_OK:
        fails += 1
        print("FAIL write: status %s, want %d (OK)" % (mem.get(0x0C02), UPD_OK))
    bad = [a for a in list(range(APP_START, code_end)) + list(range(APP_INFO, BOOT_START))
           if mem.get(a) != expect.get(a, 0xFF)]
    if bad:
        fails += 1
        print("FAIL write: %d flash bytes differ, first 0x%04X: sim %s want %02X"
              % (len(bad), bad[0], mem.get(bad[0]), expect.get(bad[0], 0xFF)))
    print("write: %d bytes compared, status %s" % (len(expect), mem.get(0x0C02)))
    shutil.rmtree(work)
    print("simulator: %s" % ("all tests passed" if not fails else "%d FAILURES" % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
