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
# --charging: a phone (through the USB emulator) plugged in set to charging,
# switched to file transfer as on a Pi (it takes nothing in, drops off the
# bus, Service_DeviceDead, back as a new USB device); checks that none of
# USBDriver's hazards seen on the Pi happen, that the phone is mounted after
# the switch, and that PDFS doesn't hold the desktop. --charging-only: never
# switched. Add --irisram to run !IrisRAM (copying Iris to the RAM disc) at
# the same time, as at a Pi's boot (IRIS_HARDDISC, see below).
# CHARGING_SWITCH=cs: when the phone is switched, after PDFS's first command.
# --allow=empty|denied|busy: a tablet that asks its owner to allow access to
# its data, for each new session (Android); the owner taps Allow ALLOW_AFTER
# seconds (6) after it asks. Checks it asks once and its storages appear.
#
# Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
: "${TB:?set TB to the riscos-linux-testbed directory}"
MOD=$ROOT/out/PDFS,ffa
[ -f "$MOD" ] || { echo "make first" >&2; exit 1; }
DESKTOP=0 USBEMU=0 SIMFLAGS= CHARGING= IRISRAM=0
for a in "$@"; do
  case "$a" in
    --desktop) DESKTOP=1 ;;
    --charging|--charging-only)
               # the USB emulator's phone, plugged in set to charging, then
               # switched to file transfer (or never: --charging-only)
               CHARGING=${a#--}; DESKTOP=1; USBEMU=1
               MOD=$ROOT/out/PDFS-usbemu,ffa ;;
    --irisram) IRISRAM=1 ;;
    --fire)    FIRE=1 ;;             # with --usbemu: the phone is a Fire tablet (MTP as FF/FF/00)
    --allow=*) # a tablet asking to allow access to its data (the simulator's
               # --needs-allow: empty, denied or busy until the owner taps
               # Allow, ALLOW_AFTER seconds after it asks)
               # ALLOW_RECONNECT=1: it drops off USB and comes back after
               # Allow (CHARGING_SWITCH cs after the first command), as a
               # Fire tablet does; ALLOW_SLOW=1: it answers GetStorageInfo
               # 2.5 seconds late, as a Fire did just after coming back
               CHARGING=allow; DESKTOP=1; USBEMU=1
               [ "${ALLOW_RECONNECT:-0}" = 1 ] && CHARGING=allow-reconnect
               MOD=$ROOT/out/PDFS-usbemu,ffa
               SIMFLAGS="$SIMFLAGS --needs-allow ${a#--allow=} --allow-after ${ALLOW_AFTER:-6}"
               [ "${ALLOW_SLOW:-0}" = 1 ] && SIMFLAGS="$SIMFLAGS --slow 0x1005=2500" ;;
    --usbemu)  USBEMU=1
               MOD=$ROOT/out/PDFS-usbemu,ffa
               [ -f "$MOD" ] || { echo "make out/PDFS-usbemu,ffa first" >&2; exit 1; } ;;
    --old-usbdriver) OLDUSB=1 ;;
  esac
done
# USB through the emulator: zero-length packets are real (the 'short'
# flag); with --old-usbdriver they can't be, so PDFS must avoid needing
# them and the simulator fails any container that would
OLDUSB=${OLDUSB:-0}
[ $USBEMU = 1 ] && [ $OLDUSB = 1 ] && SIMFLAGS="--zlp-strict 512"
# PDFS_SIM_RATE=KB/s: a device that takes files in that slowly
SIMFLAGS="$SIMFLAGS ${PDFS_SIM_RATE:+--rate $PDFS_SIM_RATE} ${PDFS_SIM_FLAGS:-}"
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
w('phone/int/Download/bigvideo.mp4', 24 * 1048576 + 7)   # copied back by Filer_Action
w('w/rand,ffd', 100000); w('w/ToPhone/Archive,ffd', 100000)
w('w/ToPhone/Film,ffd', 20 * 1048576 + 5)   # Filer_Action streams it to the phone
w('w/z500,ffd', 500)
w('w/big6,ffd', 6 * 1048576 + 3)  # over 4MB: written through a scrap file       # with its 12-byte header, one 512-byte packet: needs care on USB
open(os.path.join(t, 'w/local,fff'), 'w').write('a local text file\n')
open(os.path.join(t, 'w/ToPhone/ReadMe,fff'), 'w').write('Written on RISC OS\nand copied by the Filer\n')
PY
for b in basic1 free desktop eject charging; do python3 "$ROOT/tools/bbcbasic.py" tok "$HERE/$b.bas" "$T/w/$b,ffb"; done
mv "$T/w/desktop,ffb" "$T/w/desk,ffb"
# PDFS is loaded as a double-click does: *RMRun (its start code runs too)
cp "$MOD" "$T/w/PDFS,ffa"
{ echo "RMRun TESTDIR.PDFS"; [ -n "${PDFS_TRACE:-}" ] && echo "PDFSLog trace"; cat "$HERE/cli-test.obey"; } > "$T/w/t,feb"
# the work directory as RISC OS sees it (IXFS: '/' and '.' swap)
sed -i "s|TESTDIR|IXFS:\$$(printf '%s' "$T/w" | tr './' '/.')|g" "$T/w/t,feb"
if [ $USBEMU = 1 ]; then
  # mount through the USB transport (a fake USB phone bridged to the simulator);
  # in the desktop the filer mounts it by itself
  sed -i 's/^PDFSMount 127.0.0.1:7015$/PDFSUSB\nPDFSMount/' "$T/w/t,feb"
  [ $OLDUSB = 1 ] && sed -i '1i Set PDFSEmu$OldDriver 1' "$T/w/t,feb"
  [ ${FIRE:-0} = 1 ] && sed -i '1i Set PDFSEmu$Phone fire' "$T/w/t,feb"
  sed -i '1i Set PDFSEmu$MissingZLP 0' "$T/w/t,feb"
  sed -i '1i Set PDFSEmu$Babbles 0' "$T/w/t,feb"
  echo "Show PDFSEmu\$MissingZLP" >> "$T/w/t,feb"
  echo "Show PDFSEmu\$Babbles" >> "$T/w/t,feb"
  python3 - "$T/w/desk,ffb" <<'PY'
import sys
p = sys.argv[1]; b = open(p, "rb").read()
open(p, "wb").write(b.replace(b"PDFSMount 127.0.0.1:7015", b"PDFSDevices".ljust(24)))
PY
fi

[ -n "${PDFS_GAP:-}" ] && sed -i "1i Set PDFS\$Gap $PDFS_GAP" "$T/w/t,feb"
[ -n "${PDFS_EVENTS:-}" ] && sed -i "1i Set PDFS\$Events $PDFS_EVENTS" "$T/w/t,feb"
[ -n "${EMU_IN_HALT:-}" ] && sed -i "1i Set PDFSEmu\$InHaltAfter $EMU_IN_HALT" "$T/w/t,feb"
[ -n "${EMU_IN_DROP:-}" ] && sed -i "1i Set PDFSEmu\$InHaltDrop $EMU_IN_DROP" "$T/w/t,feb"
if [ -n "${PDFS_TRACE:-}" ]; then
  # list a folder once, carrying on if it fails, and keep the log (the trace)
  sed -i '0,/^PDFSMount/{/^PDFSMount/a X Ex PDFS::SimPhone-Internal.$.Download\nPDFSLog { > TESTDIR.trlog }
}' "$T/w/t,feb"
  sed -i "s|TESTDIR|IXFS:\$$(printf '%s' "$T/w" | tr './' '/.')|g" "$T/w/t,feb"
fi

python3 "$ROOT/sim/mtpsim.py" --port 7015 -v $SIMFLAGS "Internal=$T/phone/int" "SD card=$T/phone/sd" 2> "$OUT/sim.log" &
SIM=$!
trap 'kill $SIM 2>/dev/null || true' EXIT
sleep 1
kill -0 $SIM 2>/dev/null || { echo "the simulator did not start (port 7015 in use?): see $OUT/sim.log" >&2; exit 1; }

if [ $DESKTOP = 0 ]; then
  "$TB/bin/rol-run" ${ROL_VERBOSE:+-v} -t 200 -C "$T/w" -c 'Obey t' > "$OUT/cli.txt" 2>&1 || true
  cat "$OUT/cli.txt"
  [ -n "${PDFS_TRACE:-}" ] && { echo "--- trace"; tr -d '\r' < "$T/w/trlog" 2>/dev/null; }
  check 'grep -q "pic/jpg .*&C85 .* 3  bytes" "$OUT/cli.txt"' "*Info: type from the extension"
  check 'cmp -s "$T/w/back" "$T/w/rand,ffd"' "*Copy to the phone and back: same contents"
  check 'grep -q "^length 39" "$OUT/cli.txt"' "OPENOUT, OPENUP and append"
  check 'grep -q "^grown ok" "$OUT/cli.txt"' "a 12MB file written with no size given (memory, then a scrap file)"
  check '[ -f "$T/phone/sd/OnCard.txt" ]' "*Copy between storages; a Text file gets .txt"
  check '[ ! -e "$T/phone/int/Books" ]' "*Wipe R deletes a tree"
  check 'grep -q "0 errors, 0 recoveries" "$OUT/cli.txt"' "no transport errors"
  check 'cmp -s "$T/w/z500,ffd" "$T/phone/int/Download/z500"' "a 500-byte file (one 512-byte data phase)"
  check 'cmp -s "$T/w/big6,ffd" "$T/phone/int/Download/big6"' "a 6MB file (through memory or a scrap file)"
  check '[ ! -d "$T/w/scrap/PDFS" ] || [ -z "$(ls -A "$T/w/scrap/PDFS")" ]' "no scrap file left (big files are kept in memory when there's room)"
  check 'grep -q "^SimPhone-SD_card has been ejected$" "$OUT/cli.txt"' "*PDFSEject one storage"
  check 'grep -q "eject refused: .*Files on it are open" "$OUT/cli.txt"' "eject refused while a file is open"
  check 'grep -q "^SimPhone has been ejected: it can be unplugged" "$OUT/cli.txt" && grep -q "CloseSession\|op 1003" "$OUT/sim.log"' "*PDFSEject all closes the session"
  check '! grep -q "ZLP needed" "$OUT/sim.log"' "no data phase that would need a zero-length packet"
  if [ $USBEMU = 1 ]; then
    if [ ${FIRE:-0} = 1 ]; then
      check 'grep -q "USB9 .*1949:0581 .*interface 0/0 .*FF/FF/00" "$OUT/cli.txt" && grep -A1 "interface 0/0 .*FF/FF/00" "$OUT/cli.txt" | grep -q "  - MTP" && grep -A1 "interface 1/0 .*FF/42/01" "$OUT/cli.txt" | grep -vq "  - MTP"' "*PDFSUSB lists the Fire's vendor-specific MTP interface as MTP, and not its ADB one"
    else
    check 'grep -q "USB9 .*18D1:4EE1.*06/01/01" "$OUT/cli.txt" && grep -q "MTP" "$OUT/cli.txt"' "*PDFSUSB lists the phone as MTP"
    fi
    check 'grep -q "PDFSEmu\$ShortPackets : 0" "$OUT/cli.txt"' "no transfer ends with a short packet inside a container"
    check 'grep -q "PDFSEmu\$MissingZLP : 0" "$OUT/cli.txt"' "every container ends with a short or zero-length packet"
    check 'grep -q "PDFSEmu\$Babbles : 0" "$OUT/cli.txt"' "no USB read is shorter than the packet that comes (babble: a lost answer)"
    check 'grep -q "USB8 .*08/06/50" "$OUT/cli.txt" && grep -q "PDFS test SimPhone (USB9)" "$OUT/cli.txt"' "the memory stick is listed but not mounted; the phone is, over USB"
  fi
elif [ -n "$CHARGING" ]; then
  # A phone plugged in set to charging and switched to file transfer, as on
  # a Pi at boot; with --irisram, while !IrisRAM copies Iris to the RAM
  # disc. IRIS_HARDDISC: a HardDisc4 with !IrisRAM in Documents.Iris.IrisRAMv2
  # and an !Iris small enough for the test bed's RAM disc (128MB at most).
  HD=$ROL_HARDDISC
  if [ $IRISRAM = 1 ]; then HD=${IRIS_HARDDISC:-/tmp/claude-0/piimg/hd-irisram/HardDisc4}; fi
  : "${HD:?set ROL_HARDDISC to a HardDisc4 (desktop mode)}"
  # CHARGING_WATCH: how long to watch (cs); CHARGING_SWITCH: when the phone
  # is switched, in cs after PDFS's first command to it (default 300)
  printf '%s\n%s\n%s\n%s\n' "$CHARGING" "$IRISRAM" "${CHARGING_WATCH:-6000}" "${CHARGING_SWITCH:-}" > "$T/w/opts,fff"
  # PDFS_ALLOWHINT=N: PDFS$AllowHint (seconds before the "tap Allow" box)
  { [ -n "${PDFS_ALLOWHINT:-}" ] && echo "Set PDFS\$AllowHint $PDFS_ALLOWHINT"
    echo "Run charging"; } > "$T/w/cgo,feb"
  ROL_HARDDISC=$HD "$TB/bin/rol-run" ${ROL_VERBOSE:+-v} -t 300 -w 240 -s "$OUT/charging.png" -C "$T/w" -c 'Obey cgo' || true
  cat "$T/w/results" || true
  echo "--- *PDFSLog"; cat "$T/w/pdfslog" 2>/dev/null | tr -d '\r' || true
  check 'grep -q "^hazards: 0" "$T/w/results"' "none of USBDriver's hazards (seen on a Pi) happen"
  if [ "${CHARGING#allow}" != "$CHARGING" ]; then
    grep -h "PROMPT\|ALLOWED" "$OUT/sim.log" || true
    check '[ "$(grep -c "PROMPT [0-9]*: " "$OUT/sim.log")" = 1 ] && grep -q ALLOWED "$OUT/sim.log" && ! grep -q "gone before" "$OUT/sim.log"' "the tablet asks once, and the owner's Allow reaches PDFS's session"
    check 'grep -q "^icons: 2" "$T/w/results" && grep -q "Download" "$T/w/cat"' "once allowed, its storages are on the icon bar and can be read"
    # (with ALLOW_SLOW, one answer takes 2.5 s: never more than one at a time)
    HOLD=200; [ "${ALLOW_SLOW:-0}" = 1 ] && HOLD=300
    check 'awk -v m=$HOLD "/longest time the desktop was held/ {exit !(\$NF+0 < m)}" "$T/w/results"' "the desktop is never held for more than $((HOLD/100)) seconds (one answer from the device)"
  fi
  if [ "$CHARGING" = charging ]; then
    check 'grep -q "^icons: 2" "$T/w/results" && grep -q "Download" "$T/w/cat"' "after the switch the phone is mounted and can be read"
  fi
  # (with !IrisRAM, its copy holds the desktop itself)
  [ $IRISRAM = 0 ] && check 'awk "/longest time the desktop was held/ {exit !(\$NF+0 < 500)}" "$T/w/results"' "PDFS never holds the desktop for 5 seconds or more"
  [ $IRISRAM = 1 ] && check 'grep -q "^iris in RAM disc: yes" "$T/w/results"' "!IrisRAM copied Iris to the RAM disc"
  grep "^leftover streams" "$T/w/results" || true
else
  : "${ROL_HARDDISC:?set ROL_HARDDISC to a HardDisc4 (desktop mode)}"
  # PDFS_RMLOAD=1: PDFS loaded with *RMLoad (as at the F12 command line),
  # the desktop refusing the filer's first PDFS_RMLOAD_REFUSE starts
  { [ -n "${PDFS_RMLOAD:-}" ] && echo "Set PDFSTest\$Load rmload"
    [ -n "${PDFS_RMLOAD_REFUSE:-}" ] && echo "Set PDFSEmu\$RefuseStart $PDFS_RMLOAD_REFUSE"
    echo "Run desk"; } > "$T/w/dgo,feb"
  ROL_HARDDISC=$ROL_HARDDISC "$TB/bin/rol-run" ${ROL_VERBOSE:+-v} -t 180 -w 120 -s "$OUT/desktop.png" -C "$T/w" -c 'Obey dgo' || true
  cat "$T/w/results"
  check 'cmp -s "$T/w/ToPhone/Archive,ffd" "$T/phone/int/Documents/ToPhone/Archive"' "Filer_Action copies a folder to the phone"
  check 'cmp -s "$T/w/IMG_20261029_0910.jpg" "$T/phone/int/DCIM/Camera/IMG_20261029_0910.jpg"' "Filer_Action copies a photo back"
  check 'grep -q "filetype C85" "$T/w/results"' "the photo is a JPEG on RISC OS"
  grep -h "big file copied back\|came from the phone" "$T/w/results" || true
  check 'cmp -s "$T/phone/int/Download/bigvideo.mp4" "$(ls "$T"/w/bigvideo.mp4* 2>/dev/null | head -1)"' "a 24MB file copied from the phone by Filer_Action"
  check 'awk "/came from the phone/ {exit !(\$NF+0 < 100)}" "$T/w/results"' "the desktop keeps going while a big file comes from the phone"
  check '[ -f "$T/phone/int/Documents/ToPhone/ReadMe.txt" ]' "a Text file copied by the Filer is ReadMe.txt on the phone"
  check 'cmp -s "$T/w/ToPhone/Film,ffd" "$T/phone/int/Documents/ToPhone/Film"' "a 20MB file copied by Filer_Action"
  grep "longest pause" "$T/w/results" || true
  check 'grep -q "icon .*: Internal" "$T/w/results" && grep -q "icon .*: SD card" "$T/w/results"' "icon bar icons for both storages"
  check 'cmp -s "$T/w/local,fff" "$T/phone/sd/local.txt"' "a file dropped on the SD card's icon is copied there"
  check 'grep -q "ShowFree ok" "$T/w/results"' "*ShowFree (the icon menu's Free) works"
  check 'grep -q "big file on phone: type 1 length 6291459" "$T/w/results" && cmp -s "$T/w/big6,ffd" "$T/phone/int/Download/big6"' "a 6MB file copied through a scrap file"
  check 'grep -q "save12 on phone: type 1 length 12582912" "$T/w/results" && python3 -c "
import struct,sys
d=open(sys.argv[1],\"rb\").read()
sys.exit(not (len(d)==12582912 and all(struct.unpack_from(\"<I\",d,i)[0]==i for i in range(0,len(d)-4,4096))))" "$T/phone/int/Download/save12"' "a 12MB OS_File save"
  grep -h "OS_File save of 12MB\|while it was sent" "$T/w/results" || true
  check 'grep -q "scrap directory type [02], files left: 0" "$T/w/results"' "no scrap files left"
  if [ $USBEMU = 1 ]; then
    check 'grep -q "icons after unplug: 0" "$T/w/results"' "unplugging the phone removes its icons"
  fi
  echo "screenshots: $OUT/desktop*.png"
fi
rm -rf "$T"
exit $fail
