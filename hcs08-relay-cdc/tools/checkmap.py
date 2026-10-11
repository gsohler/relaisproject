#!/usr/bin/env python3
"""checkmap.py - sanity checks on an SDCC/sdld .map file for the MC9S08JM60.

    checkmap.py build/app/relay_cdc.map --code-end 0xDBB0
    checkmap.py build/boot/bootloader.map --code-end 0xFFA0

Checks:
  - direct-page variables (DSEG, OSEG) end at or below 0x0100,
  - RAM areas do not overlap and end below 0x0C00 (stack 0x0C00-0x10AB),
  - code + constants stay below --code-end.
SDCC does not warn when the direct page overflows; the generated code would
then silently access wrong addresses.
"""
import argparse
import re
import sys

STACK_TOP = 0x10A9
RAM_LIMIT = 0x0C00      # startup code zeroes 0x00B0-0x0BFF, stack (1.2 KiB) above


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--code-end", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--no-lib", action="store_true",
                    help="fail if library code is linked (needed with --stack-auto: "
                         "SDCC has no stack-auto library for s08)")
    a = ap.parse_args()

    areas = {}
    for line in open(a.map):
        m = re.match(r"^(\w+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8}) =", line)
        if m:
            areas[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))

    errors = []
    ram = []
    for name in ("DSEG", "OSEG", "XSEG", "XISEG"):
        if name in areas and areas[name][1]:
            start, size = areas[name]
            ram.append((start, start + size, name))
            if name in ("DSEG", "OSEG") and start + size > 0x100:
                errors.append("%s ends at 0x%04X, beyond the direct page (0x0100)"
                              % (name, start + size))
    ram.sort()
    for (s1, e1, n1), (s2, e2, n2) in zip(ram, ram[1:]):
        if s2 < e1:
            errors.append("%s (0x%04X-0x%04X) overlaps %s (0x%04X)" % (n1, s1, e1, n2, s2))
    ram_end = max((e for _, e, _ in ram), default=0)
    if ram_end > RAM_LIMIT:
        errors.append("RAM used up to 0x%04X, limit 0x%04X (stack area above)"
                      % (ram_end, RAM_LIMIT))

    code_end = 0
    for name in ("CSEG", "CONST", "XINIT", "GSINIT", "GSINIT0", "GSFINAL", "HOME", "CABS"):
        if name in areas and areas[name][1]:
            start, size = areas[name]
            if start < 0xFF00:
                code_end = max(code_end, start + size)
    if code_end > a.code_end:
        errors.append("code ends at 0x%04X, limit is 0x%04X" % (code_end, a.code_end))

    if a.no_lib:
        text = open(a.map).read()
        m = re.search(r"Libraries Linked.*?\n(.*?)(?:\n\s*\n|\Z)", text, re.S)
        mods = re.findall(r"\[\s*(\S+)\s*\]", m.group(1)) if m else []
        extra = [x for x in mods if x != "_ret.rel"]
        if extra:
            errors.append("library code linked: %s - use boot/util.h instead"
                          % ", ".join(extra))

    for e in errors:
        print("checkmap: %s: ERROR %s" % (a.map, e), file=sys.stderr)
    if errors:
        sys.exit(1)
    print("checkmap: %s: RAM 0x00B0-0x%04X, code up to 0x%04X (limit 0x%04X) - ok"
          % (a.map, ram_end, code_end, a.code_end))


if __name__ == "__main__":
    main()
