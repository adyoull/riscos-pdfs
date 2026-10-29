#!/bin/bash
# Test the PDFS module in RISC OS for Linux (riscos-linux-testbed), with
# sim/mtpsim.py as the device on 127.0.0.1:7015.
#
#   TB=path/to/riscos-linux-testbed RCD_DIR=path/to/riscos-crossdev tests/rol/run.sh
#   ... ROL_HARDDISC=path/to/HardDisc4 tests/rol/run.sh --desktop
#
# The CLI test runs cli-test.obey (*Cat, *Copy both ways, *Rename, *Wipe,
# BASIC OPENOUT/OPENUP, free space) and checks the device's folders. The
# desktop test runs desktop.bas as a Wimp task: Filer windows on the phone,
# a folder copied to it and a photo copied back with Filer_Action, and
# screenshots (out/rol/desktop*.png).
#
# Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
: "${TB:?set TB to the riscos-linux-testbed directory}"
MOD=$ROOT/out/PDFS,ffa
[ -f "$MOD" ] || { echo "make first" >&2; exit 1; }
DESKTOP=0
[ "${1:-}" = --desktop ] && DESKTOP=1
OUT=$ROOT/out/rol
mkdir -p "$OUT"
T=$(mktemp -d /tmp/pdfs-rol.XXXXXX)
fail=0
check() { if eval "$1"; then echo "ok: $2"; else echo "FAIL: $2"; fail=1; fi; }

# the phone
mkdir -p "$T/phone/int/Download" "$T/phone/int/DCIM/Camera" "$T/phone/int/Documents" "$T/phone/sd/Movies" "$T/w/ToPhone"
printf 'JPG' > "$T/phone/int/Download/pic.jpg"
printf 'notes\n' > "$T/phone/int/Download/notes.txt"
python3 - "$T" <<'PY'
import os, random, sys
t = sys.argv[1]; random.seed(2)
def w(p, n): open(os.path.join(t, p), 'wb').write(random.randbytes(n))
for i in range(6): w('phone/int/DCIM/Camera/IMG_20261029_09%02d.jpg' % (10 + i), 200000 + i * 1000)
w('phone/int/DCIM/Camera/VID_20261028_1802.mp4', 3000000)
open(os.path.join(t, 'phone/int/Documents/Shopping list.txt'), 'w').write('milk\nbread\n')
w('phone/int/Documents/Boarding pass.pdf', 80000)
w('phone/sd/Movies/Holiday.mkv', 250000)
w('w/rand,ffd', 100000); w('w/ToPhone/Archive,ffd', 100000)
open(os.path.join(t, 'w/local,fff'), 'w').write('a local text file\n')
open(os.path.join(t, 'w/ToPhone/ReadMe,fff'), 'w').write('Written on RISC OS\nand copied by the Filer\n')
PY
for b in basic1 free desktop; do python3 "$ROOT/tools/bbcbasic.py" tok "$HERE/$b.bas" "$T/w/$b,ffb"; done
mv "$T/w/desktop,ffb" "$T/w/desk,ffb"
cp "$HERE/cli-test.obey" "$T/w/t,feb"

python3 "$ROOT/sim/mtpsim.py" --port 7015 -v "Internal=$T/phone/int" "SD card=$T/phone/sd" 2> "$OUT/sim.log" &
SIM=$!
trap 'kill $SIM 2>/dev/null || true' EXIT
sleep 1
kill -0 $SIM 2>/dev/null || { echo "the simulator did not start (port 7015 in use?): see $OUT/sim.log" >&2; exit 1; }

if [ $DESKTOP = 0 ]; then
  "$TB/bin/rol-run" -t 200 -m "$MOD" -C "$T/w" -c 'Obey t' > "$OUT/cli.txt" 2>&1 || true
  cat "$OUT/cli.txt"
  check 'grep -q "pic/jpg .*&C85 .* 3  bytes" "$OUT/cli.txt"' "*Info: type from the extension"
  check 'cmp -s "$T/w/back" "$T/w/rand,ffd"' "*Copy to the phone and back: same contents"
  check 'grep -q "^length 39" "$OUT/cli.txt"' "OPENOUT, OPENUP and append"
  check '[ -f "$T/phone/sd/OnCard.txt" ]' "*Copy between storages; a Text file gets .txt"
  check '[ ! -e "$T/phone/int/Books" ]' "*Wipe R deletes a tree"
  check 'grep -q "0 errors, 0 recoveries" "$OUT/cli.txt"' "no transport errors"
else
  : "${ROL_HARDDISC:?set ROL_HARDDISC to a HardDisc4 (desktop mode)}"
  ROL_HARDDISC=$ROL_HARDDISC "$TB/bin/rol-run" ${ROL_VERBOSE:+-v} -t 90 -w 45 -s "$OUT/desktop.png" -m "$MOD" -C "$T/w" -c 'Run desk' || true
  cat "$T/w/results"
  check 'cmp -s "$T/w/ToPhone/Archive,ffd" "$T/phone/int/Documents/ToPhone/Archive"' "Filer_Action copies a folder to the phone"
  check 'cmp -s "$T/w/IMG_20261029_0910.jpg" "$T/phone/int/DCIM/Camera/IMG_20261029_0910.jpg"' "Filer_Action copies a photo back"
  check 'grep -q "filetype C85" "$T/w/results"' "the photo is a JPEG on RISC OS"
  check '[ -f "$T/phone/int/Documents/ToPhone/ReadMe.txt" ]' "a Text file copied by the Filer is ReadMe.txt on the phone"
  check 'grep -q "icon .*: Internal" "$T/w/results" && grep -q "icon .*: SD card" "$T/w/results"' "icon bar icons for both storages"
  check 'cmp -s "$T/w/local,fff" "$T/phone/sd/local.txt"' "a file dropped on the SD card's icon is copied there"
  check 'grep -q "ShowFree ok" "$T/w/results"' "*ShowFree (the icon menu's Free) works"
  echo "screenshots: $OUT/desktop*.png"
fi
rm -rf "$T"
exit $fail
