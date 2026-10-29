#!/usr/bin/env python3
# check-pic.py IMAGE0 IMAGE1 BASE1 GOT_START GOT_END
# IMAGE0 is a module linked at 0, IMAGE1 the same module linked at BASE1.
# Passes if they differ only in GOT words, each by exactly BASE1: then the
# module runs anywhere once cmodule.inc has relocated its GOT.
import struct, sys
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
base, gs, ge = (int(x, 0) for x in sys.argv[3:6])
if len(a) != len(b):
    sys.exit(f"check-pic: sizes differ ({len(a)} vs {len(b)})")
bad = 0
for off in range(0, len(a) - 3, 4):
    x, = struct.unpack_from("<I", a, off)
    y, = struct.unpack_from("<I", b, off)
    if x != y:
        if gs <= off < ge and (y - x) & 0xFFFFFFFF == base:
            continue
        print(f"check-pic: absolute address outside the GOT at +{off:#x}: {x:#x} / {y:#x}")
        bad += 1
if bad:
    sys.exit(1)
print(f"check-pic: OK ({(ge - gs) // 4} GOT entries, {len(a)} bytes)")
