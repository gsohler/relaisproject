#!/usr/bin/env python3
"""mkfw.py - builds the firmware file for the USB bootloader.

    mkfw.py build/relay_cdc.s19 --version 1.2 -o build/relay_fw-1.2.s19
    mkfw.py --combine build/bootloader.s19 build/relay_fw-1.2.s19 -o build/relay_combined-1.2.s19

Firmware file (copy it onto the RELAYBOOT drive):
  - vector table 0xFFC0-0xFFFF of the SDCC output moved to 0xDBC0-0xDBFF
    (hardware vector redirection, the bootloader owns the real vectors),
  - NV registers 0xFFB0-0xFFBF dropped (they belong to the bootloader),
  - everything else must lie in 0x1960-0xDBAF,
  - info block at 0xDBB0 added as the last record: magic "RLFW", CRC16 over
    the application area, version string.

Combined file (for programming new boards with USBDM): bootloader +
firmware in one S19.

Layout numbers must match src/layout.h.
"""
import argparse
import sys

APP_START = 0x1960
APP_INFO = 0xDBB0
APP_VECTORS = 0xDBC0
BOOT_START = 0xDC00
VECTOR_OFFSET = 0x2400


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def read_s19(path):
    mem = {}
    for no, line in enumerate(open(path), 1):
        line = line.strip()
        if not line:
            continue
        if line[0] != "S" or len(line) < 4:
            sys.exit("%s:%d: not an S-record" % (path, no))
        raw = bytes.fromhex(line[2:])
        if raw[0] != len(raw) - 1 or (sum(raw) & 0xFF) != 0xFF:
            sys.exit("%s:%d: bad length or checksum" % (path, no))
        if line[1] == "1":
            addr = (raw[1] << 8) | raw[2]
            for i, b in enumerate(raw[3:-1]):
                a = addr + i
                if a in mem and mem[a] != b:
                    sys.exit("%s:%d: address 0x%04X written twice" % (path, no, a))
                mem[a] = b
        elif line[1] in "2378":
            sys.exit("%s:%d: only 16-bit S1 records are supported" % (path, no))
    return mem


def record(rtype, addr, data):
    raw = bytes([len(data) + 3, addr >> 8, addr & 0xFF]) + bytes(data)
    return "S%s%s%02X\n" % (rtype, raw.hex().upper(), (~sum(raw)) & 0xFF)


def runs(mem, addrs, maxlen=32):
    """Yields (start, bytes) of consecutive addresses, at most maxlen long."""
    start, buf = None, []
    for a in addrs:
        if buf and (a != start + len(buf) or len(buf) == maxlen):
            yield start, buf
            start, buf = None, []
        if not buf:
            start = a
        buf.append(mem[a])
    if buf:
        yield start, buf


def write_s19(path, header, mem, last=None):
    """last: addresses written at the end of the file (info block)"""
    last = set(last or ())
    with open(path, "w") as f:
        f.write(record("0", 0, header.encode()))
        for a, d in runs(mem, sorted(a for a in mem if a not in last)):
            f.write(record("1", a, d))
        for a, d in runs(mem, sorted(last)):
            f.write(record("1", a, d))
        f.write(record("9", 0, b""))


def make_firmware(src, version, out):
    raw = read_s19(src)
    fw = {}
    for a, b in raw.items():
        if 0xFFB0 <= a <= 0xFFBF:
            continue                                    # NVPROT/NVOPT: bootloader
        if a >= 0xFFC0:
            fw[a - VECTOR_OFFSET] = b                   # vectors -> 0xDBC0..
        elif APP_START <= a < APP_INFO:
            fw[a] = b
        else:
            sys.exit("mkfw: %s: address 0x%04X outside the application area "
                     "0x%04X-0x%04X" % (src, a, APP_START, APP_INFO - 1))
    if APP_VECTORS + 0x3E not in fw or APP_VECTORS + 0x3F not in fw:
        sys.exit("mkfw: %s: no reset vector" % src)
    entry = (fw[APP_VECTORS + 0x3E] << 8) | fw[APP_VECTORS + 0x3F]
    if not APP_START <= entry < APP_INFO:
        sys.exit("mkfw: reset vector 0x%04X outside the application" % entry)

    image = bytes(fw.get(a, 0xFF) for a in range(APP_START, APP_INFO)) + \
        bytes(fw.get(a, 0xFF) for a in range(APP_VECTORS, BOOT_START))
    crc = crc16(image)
    ver = version.encode("ascii")
    if len(ver) > 8:
        sys.exit("mkfw: version string longer than 8 characters")
    info = b"RLFW" + bytes([crc >> 8, crc & 0xFF]) + ver.ljust(8, b"\0") + b"\xff\xff"
    for i, b in enumerate(info):
        fw[APP_INFO + i] = b

    write_s19(out, "relay_fw " + version, fw, last=range(APP_INFO, APP_INFO + 16))
    code_end = max(a for a in fw if a < APP_INFO)
    print("mkfw: %s: firmware %s, code 0x%04X-0x%04X (%d bytes free), CRC %04X"
          % (out, version, APP_START, code_end, APP_INFO - 1 - code_end, crc))


def combine(boot, fw, out):
    a, b = read_s19(boot), read_s19(fw)
    if any(x < BOOT_START for x in a):
        sys.exit("mkfw: %s: bootloader below 0x%04X" % (boot, BOOT_START))
    if any(x >= BOOT_START for x in b):
        sys.exit("mkfw: %s: firmware reaches into the bootloader" % fw)
    mem = dict(a)
    mem.update(b)
    write_s19(out, "relay_combined", mem)
    print("mkfw: %s: bootloader + firmware" % out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--version")
    ap.add_argument("--combine", action="store_true")
    a = ap.parse_args()
    if a.combine:
        if len(a.inputs) != 2:
            sys.exit("mkfw: --combine needs bootloader.s19 and firmware.s19")
        combine(a.inputs[0], a.inputs[1], a.output)
    else:
        if len(a.inputs) != 1 or not a.version:
            sys.exit("mkfw: need one input file and --version")
        make_firmware(a.inputs[0], a.version, a.output)


if __name__ == "__main__":
    main()
