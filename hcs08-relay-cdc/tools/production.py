#!/usr/bin/env python3
"""production.py - programs new relay boards with a random serial number.

For every board:
  1. a random 64-bit serial number is generated on the PC (os.urandom),
     checked against the production log (no duplicates),
  2. it is patched into the image (bootloader + firmware) at 0xFFA0
     (8 bytes, big endian, + CRC16), inside the write-protected bootloader
     area, so firmware updates never touch it,
  3. the image is programmed with USBDM (mass erase first),
  4. optionally the board is checked over USB: it must enumerate with this
     serial number and answer "SERIAL <number>",
  5. only then the serial number is appended to the log (CSV).

    python3 tools/production.py                  # one board after another
    python3 tools/production.py --count 1        # just one board
    python3 tools/production.py --dry-run        # only write the image file
    python3 tools/production.py --serial 3F9A07C2D41B88E5
                                                 # reprogram a board, keep its number

Needs: make combined (build/relay_combined-<version>.s19), USBDM, pyserial
for the USB check (pip install pyserial).
"""
import argparse
import csv
import datetime
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfw import crc16, read_s19, write_s19  # noqa: E402

SERIAL_ADDR = 0xFFA0
SERIAL_LEN = 8
USB_VID, USB_PID = 0x1209, 0x0001


def load_log(path):
    if not os.path.exists(path):
        return set()
    with open(path, newline="") as f:
        return {row["serial"].split()[0].upper() for row in csv.DictReader(f) if row["serial"]}


def append_log(path, serial, image, verified):
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["serial", "date", "image", "usb_check"])
        w.writerow([serial, datetime.datetime.now().isoformat(timespec="seconds"),
                    os.path.basename(image), "ok" if verified else "skipped"])


def new_serial(used):
    while True:
        raw = os.urandom(SERIAL_LEN)
        s = raw.hex().upper()
        # all 0xFF would look like an empty slot
        if raw != b"\xff" * SERIAL_LEN and s not in used:
            return raw, s


def patch_image(image, raw, out):
    mem = read_s19(image)
    for a in range(SERIAL_ADDR, SERIAL_ADDR + SERIAL_LEN + 2):
        if a in mem:
            sys.exit("production: %s already contains data at 0x%04X" % (image, a))
    crc = crc16(raw)
    for i, b in enumerate(raw + bytes([crc >> 8, crc & 0xFF])):
        mem[SERIAL_ADDR + i] = b
    write_s19(out, "relay_board " + raw.hex().upper(), mem)


def program(flashprog, path):
    cmd = flashprog.split() + ["-target=HCS08", "-device=MC9S08JM60", "-useReset",
                               "-masserase", "-unsecure", "-program",
                               "-execute", path]
    print("  " + " ".join(cmd))
    return subprocess.run(cmd).returncode == 0


def usb_check(serial, timeout=15):
    """Waits for the board with this serial number and asks for SERIAL."""
    try:
        import serial as pyserial
        from serial.tools import list_ports
    except ImportError:
        print("  USB check skipped: pyserial missing (pip install pyserial)")
        return None
    end = time.time() + timeout
    port = None
    while time.time() < end and not port:
        for p in list_ports.comports():
            if p.vid == USB_VID and p.pid == USB_PID and (p.serial_number or "").upper() == serial:
                port = p.device
        if not port:
            time.sleep(0.5)
    if not port:
        print("  USB check FAILED: no board with serial %s appeared" % serial)
        return False
    time.sleep(0.5)
    with pyserial.Serial(port, 115200, timeout=3) as s:
        s.reset_input_buffer()
        s.write(b"SERIAL\n")
        reply = s.readline().decode("ascii", "replace").strip()
    if reply != "SERIAL " + serial:
        print("  USB check FAILED: %s answered %r" % (port, reply))
        return False
    print("  USB check ok: %s -> %s" % (port, reply))
    return True


def default_image():
    files = sorted(glob.glob("build/relay_combined-*.s19"), key=os.path.getmtime)
    return files[-1] if files else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", help="bootloader + firmware S19 (default: newest "
                                    "build/relay_combined-*.s19)")
    ap.add_argument("--log", default="production_log.csv", help="CSV log of serial numbers")
    ap.add_argument("--count", type=int, default=0, help="number of boards (0 = until Ctrl+C)")
    ap.add_argument("--flashprog", default=os.environ.get("FLASHPROG", "UsbdmFlashProgrammer"))
    ap.add_argument("--outdir", default="build/production", help="per-board image files")
    ap.add_argument("--no-usb-check", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="only write the image, no programming")
    ap.add_argument("--serial", help="reprogram one board with this existing serial number "
                                     "(must be in the log), e.g. for a bootloader update")
    a = ap.parse_args()

    image = a.image or default_image()
    if not image:
        sys.exit("production: no image - run 'make combined' first or give --image")
    os.makedirs(a.outdir, exist_ok=True)
    used = load_log(a.log)
    print("image %s, log %s (%d serial numbers so far)" % (image, a.log, len(used)))

    done = 0
    try:
        while not a.count or done < a.count:
            if not a.dry_run:
                input("\nConnect board %d (USB power + USBDM), then press Enter "
                      "(Ctrl+C to stop) " % (done + 1))
            if a.serial:
                serial = a.serial.upper()
                if serial not in used:
                    sys.exit("production: %s is not in %s - refusing to invent a number"
                             % (serial, a.log))
                raw = bytes.fromhex(serial)
                a.count = 1
            else:
                raw, serial = new_serial(used)
            out = os.path.join(a.outdir, "relay_%s.s19" % serial)
            patch_image(image, raw, out)
            print("  serial %s -> %s" % (serial, out))
            if a.dry_run:
                done += 1
                continue
            if not program(a.flashprog, out):
                print("  PROGRAMMING FAILED - serial %s not used, board not logged" % serial)
                continue
            verified = None if a.no_usb_check else usb_check(serial)
            if verified is False:
                print("  board NOT logged - check it and run again")
                continue
            append_log(a.log, serial + (" (reprogrammed)" if a.serial else ""), image, verified)
            used.add(serial)
            os.remove(out)
            done += 1
            print("  board %d done, serial %s" % (done, serial))
    except KeyboardInterrupt:
        print()
    print("%d board(s) done" % done)


if __name__ == "__main__":
    main()
