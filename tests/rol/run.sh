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
DESKTOP=0 USBEMU=0 SIMFLAGS=
for a in "$@"; do
  case "$a" in
    --desktop) DESKTOP=1 ;;
    --usbemu)  USBEMU=1
               MOD=$ROOT/out/PDFS-usbemu,ffa
               [ -f "$MOD" ] || { echo "make out/PDFS-usbemu,ffa first" >&2; exit 1; } ;;
    --old-usbdriver) OLDUSB=1 ;;
    --usb-new) USBNEW=1 ;;    # *PDFSUSB short on, pipeline on
  esac
done
# USB through the emulator: zero-length packets are real (the 'short'
# flag); with --old-usbdriver they can't be, so PDFS must avoid needing
# them and the simulator fails any container that would
OLDUSB=${OLDUSB:-0}
[ $USBEMU = 1 ] && [ $OLDUSB = 1 ] && SIMFLAGS="--zlp-strict 512"
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
w('w/ToPhone/Film,ffd', 20 * 1048576 + 5)   # Filer_Action streams it to the phone
w('w/z500,ffd', 500)
w('w/big6,ffd', 6 * 1048576 + 3)  # over 4MB: written through a scrap file       # with its 12-byte header, one 512-byte packet: needs care on USB
open(os.path.join(t, 'w/local,fff'), 'w').write('a local text file\n')
open(os.path.join(t, 'w/ToPhone/ReadMe,fff'), 'w').write('Written on RISC OS\nand copied by the Filer\n')
PY
for b in basic1 free desktop eject; do python3 "$ROOT/tools/bbcbasic.py" tok "$HERE/$b.bas" "$T/w/$b,ffb"; done
mv "$T/w/desktop,ffb" "$T/w/desk,ffb"
# PDFS is loaded as a double-click does: *RMRun (its start code runs too)
cp "$MOD" "$T/w/PDFS,ffa"
{ echo "RMRun TESTDIR.PDFS"; cat "$HERE/cli-test.obey"; } > "$T/w/t,feb"
# the work directory as RISC OS sees it (IXFS: '/' and '.' swap)
sed -i "s|TESTDIR|IXFS:\$$(printf '%s' "$T/w" | tr './' '/.')|g" "$T/w/t,feb"
if [ $USBEMU = 1 ]; then
  # mount through the USB transport (a fake USB phone bridged to the simulator);
  # in the desktop the filer mounts it by itself
  sed -i 's/^PDFSMount 127.0.0.1:7015$/PDFSUSB\nPDFSMount/' "$T/w/t,feb"
  [ $OLDUSB = 1 ] && sed -i '1i Set PDFSEmu$OldDriver 1' "$T/w/t,feb"
  [ "${USBNEW:-0}" = 1 ] && sed -i 's/^PDFSUSB$/PDFSUSB short on\nPDFSUSB pipeline on\nPDFSUSB/' "$T/w/t,feb"
  sed -i '1i Set PDFSEmu$MissingZLP 0' "$T/w/t,feb"
  echo "Show PDFSEmu\$MissingZLP" >> "$T/w/t,feb"
  python3 - "$T/w/desk,ffb" <<'PY'
import sys
p = sys.argv[1]; b = open(p, "rb").read()
open(p, "wb").write(b.replace(b"PDFSMount 127.0.0.1:7015", b"PDFSDevices".ljust(24)))
PY
fi

python3 "$ROOT/sim/mtpsim.py" --port 7015 -v $SIMFLAGS "Internal=$T/phone/int" "SD card=$T/phone/sd" 2> "$OUT/sim.log" &
SIM=$!
trap 'kill $SIM 2>/dev/null || true' EXIT
sleep 1
kill -0 $SIM 2>/dev/null || { echo "the simulator did not start (port 7015 in use?): see $OUT/sim.log" >&2; exit 1; }

if [ $DESKTOP = 0 ]; then
  "$TB/bin/rol-run" -t 200 -C "$T/w" -c 'Obey t' > "$OUT/cli.txt" 2>&1 || true
  cat "$OUT/cli.txt"
  check 'grep -q "pic/jpg .*&C85 .* 3  bytes" "$OUT/cli.txt"' "*Info: type from the extension"
  check 'cmp -s "$T/w/back" "$T/w/rand,ffd"' "*Copy to the phone and back: same contents"
  check 'grep -q "^length 39" "$OUT/cli.txt"' "OPENOUT, OPENUP and append"
  check '[ -f "$T/phone/sd/OnCard.txt" ]' "*Copy between storages; a Text file gets .txt"
  check '[ ! -e "$T/phone/int/Books" ]' "*Wipe R deletes a tree"
  check 'grep -q "0 errors, 0 recoveries" "$OUT/cli.txt"' "no transport errors"
  check 'cmp -s "$T/w/z500,ffd" "$T/phone/int/Download/z500"' "a 500-byte file (one 512-byte data phase)"
  check 'cmp -s "$T/w/big6,ffd" "$T/phone/int/Download/big6"' "a 6MB file (through a scrap file)"
  check '[ -d "$T/w/scrap/PDFS" ] && [ -z "$(ls -A "$T/w/scrap/PDFS")" ]' "the scrap file is deleted"
  check 'grep -q "^SimPhone-SD_card has been ejected$" "$OUT/cli.txt"' "*PDFSEject one storage"
  check 'grep -q "eject refused: .*Files on it are open" "$OUT/cli.txt"' "eject refused while a file is open"
  check 'grep -q "^SimPhone has been ejected: it can be unplugged" "$OUT/cli.txt" && grep -q "CloseSession\|op 1003" "$OUT/sim.log"' "*PDFSEject all closes the session"
  check '! grep -q "ZLP needed" "$OUT/sim.log"' "no data phase that would need a zero-length packet"
  if [ $USBEMU = 1 ]; then
    check 'grep -q "USB9 .*18D1:4EE1.*06/01/01" "$OUT/cli.txt" && grep -q "MTP" "$OUT/cli.txt"' "*PDFSUSB lists the phone as MTP"
    check 'grep -q "PDFSEmu\$ShortPackets : 0" "$OUT/cli.txt"' "no transfer ends with a short packet inside a container"
    check 'grep -q "PDFSEmu\$MissingZLP : 0" "$OUT/cli.txt"' "every container ends with a short or zero-length packet"
    check 'grep -q "USB8 .*08/06/50" "$OUT/cli.txt" && grep -q "PDFS test SimPhone (USB USB9)" "$OUT/cli.txt"' "the memory stick is listed but not mounted; the phone is, over USB"
  fi
else
  : "${ROL_HARDDISC:?set ROL_HARDDISC to a HardDisc4 (desktop mode)}"
  ROL_HARDDISC=$ROL_HARDDISC "$TB/bin/rol-run" ${ROL_VERBOSE:+-v} -t 90 -w 45 -s "$OUT/desktop.png" -C "$T/w" -c 'Run desk' || true
  cat "$T/w/results"
  check 'cmp -s "$T/w/ToPhone/Archive,ffd" "$T/phone/int/Documents/ToPhone/Archive"' "Filer_Action copies a folder to the phone"
  check 'cmp -s "$T/w/IMG_20261029_0910.jpg" "$T/phone/int/DCIM/Camera/IMG_20261029_0910.jpg"' "Filer_Action copies a photo back"
  check 'grep -q "filetype C85" "$T/w/results"' "the photo is a JPEG on RISC OS"
  check '[ -f "$T/phone/int/Documents/ToPhone/ReadMe.txt" ]' "a Text file copied by the Filer is ReadMe.txt on the phone"
  check 'cmp -s "$T/w/ToPhone/Film,ffd" "$T/phone/int/Documents/ToPhone/Film"' "a 20MB file copied by Filer_Action"
  grep "longest pause" "$T/w/results" || true
  check 'grep -q "icon .*: Internal" "$T/w/results" && grep -q "icon .*: SD card" "$T/w/results"' "icon bar icons for both storages"
  check 'cmp -s "$T/w/local,fff" "$T/phone/sd/local.txt"' "a file dropped on the SD card's icon is copied there"
  check 'grep -q "ShowFree ok" "$T/w/results"' "*ShowFree (the icon menu's Free) works"
  check 'grep -q "big file on phone: type 1 length 6291459" "$T/w/results" && cmp -s "$T/w/big6,ffd" "$T/phone/int/Download/big6"' "a 6MB file copied through a scrap file"
  check 'grep -q "scrap directory type 2, files left: 0" "$T/w/results"' "no scrap files left"
  if [ $USBEMU = 1 ]; then
    check 'grep -q "icons after unplug: 0" "$T/w/results"' "unplugging the phone removes its icons"
  fi
  echo "screenshots: $OUT/desktop*.png"
fi
rm -rf "$T"
exit $fail
