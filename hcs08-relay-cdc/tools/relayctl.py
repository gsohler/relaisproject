#!/usr/bin/env python3
"""relayctl - command line client for the 6-channel relay board.

Examples:
    relayctl.py on 3
    relayctl.py off all
    relayctl.py set 25          # relays 1,3,6 on, the others off
    relayctl.py get
    relayctl.py bootloader      # restart as USB drive RELAYBOOT for an update
    relayctl.py --port /dev/ttyACM1 raw "TOGGLE 2"

Requires pyserial (pip install pyserial).
"""
import argparse
import sys

import serial
from serial.tools import list_ports

VID, PID = 0x1209, 0x0001


def find_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial device (default: auto-detect by USB ID)")
    ap.add_argument("command", help="on, off, toggle, set, get, id, bootloader, raw")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        sys.exit("relay board not found (VID:PID %04x:%04x)" % (VID, PID))

    cmd = a.command.upper()
    if cmd == "RAW":
        line = " ".join(a.args)
    else:
        line = " ".join([cmd] + [x.upper() for x in a.args])

    # opening the port asserts DTR, which the firmware needs to send replies.
    # Switching pulses take ~120 ms per relay, so ON ALL needs ~0.7 s.
    with serial.Serial(port, 115200, timeout=3) as s:
        s.reset_input_buffer()
        s.write((line + "\n").encode("ascii"))
        reply = s.readline().decode("ascii", "replace").strip()
    print(reply)
    sys.exit(0 if reply and not reply.startswith("ERR") else 1)


if __name__ == "__main__":
    main()
