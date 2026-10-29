# PDFS: Portable Device Filing System

A RISC OS filing system module for phones, tablets, media players and cameras that use
MTP (Media Transfer Protocol). A connected device's storages show up as
discs, such as `PDFS::Pixel-Internal.$`, and work with the Filer, `*Copy`,
BASIC's OPENIN/OPENOUT and every program that uses FileSwitch.

Status: **0.11.** USB (the RISC OS 5 USB stack), the icon bar filer and
filetype extensions are done. It's tested against a simulated device, over
TCP and through an emulation of USBDriver and DeviceFS; it still has to be
tried with more real devices (see docs/TESTING-ON-A-PI.md).

Licence: Apache 2.0 (see LICENSE). It contains no GPL code.

## Layout

| Path | What |
|---|---|
| `core/mtp.c` | MTP/PTP session layer: transactions, recovery (cancel, reset, reopen), UTF-16 strings |
| `core/fs.c` | the filing system: devices and storages as discs, directory cache, name and type translation, FileSwitch entry points (Open, GetBytes, PutBytes, Args, Close, File, Func) |
| `core/transport.h`, `core/plat.h` | what the core needs from the platform: a transport (USB later, TCP now) and memory, time, logging, MimeMap |
| `riscos/module.c`, `header.s` | the RISC OS module: header, veneers, *commands, TCP transport over the Internet module, MimeMap |
| `riscos/usb.c` | the USB transport: finds MTP interfaces (Service_USB), DeviceFS streams for the endpoints |
| `riscos/filer.c` | the icon bar filer: a drive icon for each storage, its menu, drag to copy, Free |
| `riscos/sendwin.c` | Filer_Action's window (its own template and messages), shown while files are sent in the background |
| `tools/mksprites.py` | draws the drive icon and makes the ResourceFS block |
| `host/host.c` | `pdfs-host`: the same core on Linux, driven from the command line, for tests |
| `sim/mtpsim.py` | a simulated MTP device over TCP; host folders are its storages |
| `tests/host_test.py` | 163 host tests against four kinds of device and a stalled one |
| `tests/rol/` | tests in RISC OS for Linux (riscos-linux-testbed): CLI and desktop, over TCP or emulated USB (`usbemu.c`) |

## Building

Needs riscos-crossdev's toolchain (`RISCOS_TOOLCHAIN`, default
`../riscos-crossdev/toolchain/env`) and gcc for the host build.

    make            # out/PDFS,ffa and out/pdfs-host
    make check      # host tests

## Using it

    *RMLoad PDFS

In the desktop, plug the device in and, on an Android device, set it to *File transfer* (Android) or
unlock it: a drive icon appears on the icon bar for each of its storages
("Internal", "SD card"). Click it to open it; drag files to it to copy them.
Its menu has Open '$', Free, **Eject** (that storage) and, when the device
has more than one storage, **Eject all**: each writes what's waiting, closes
the windows and removes the icons; when nothing of the device is left, its
session is closed and it says the device can be unplugged. Eject is refused
while files on it are open. Unplugging without ejecting removes the icons
too.

Outside the desktop:

| Command | What |
|---|---|
| `*PDFSMount` | mount every MTP device connected by USB |
| `*PDFSMount USB<n> [<interface>]` | mount that USB device (for ones that don't say they're MTP) |
| `*PDFSMount <a.b.c.d>:<port>` | connect to the simulator (testing) |
| `*PDFSDevices` | mounted devices and storages, with free space |
| `*PDFSUSB [control on\|off]` | connected USB devices' interfaces, and which are MTP |
| `*PDFSEject <disc> [all]` | eject a storage, or (all) the whole device, safely |
| `*PDFSDismount [<disc>]` | disconnect at once |
| `*PDFSExtensions [show\|hide]` | how names with a type extension are shown |
| `*PDFSLog [on\|off\|trace]` | the log; trace logs every FileSwitch call (also to Reporter) |

Paths are `PDFS::<Model>-<Storage>.$.…`, e.g. `PDFS::Pixel_7-Internal.$.DCIM`.

To try it without a device: `python3 sim/mtpsim.py --port 7015 "Internal=/some/folder"`
and `*PDFSMount 127.0.0.1:7015`.

## Tests in riscos-linux-testbed

    TB=../riscos-linux-testbed RCD_DIR=../riscos-crossdev tests/rol/run.sh
    TB=... RCD_DIR=... ROL_HARDDISC=.../HardDisc4 tests/rol/run.sh --desktop
    make out/PDFS-usbemu,ffa && TB=... tests/rol/run.sh --usbemu [--old-usbdriver] [--desktop]

`--usbemu` uses a test build whose USB transport talks to an emulation of
USBDriver and DeviceFS (tests/rol/usbemu.c): a fake MTP device, USB9, bridged to
the simulator, which then fails any data phase that would need a USB
zero-length packet (`--zlp-strict 512`).

The CLI test covers *Cat/*Ex/*Info, *CDir, *Copy both ways and between
storages, *Rename, BASIC OPENOUT/OPENUP/append, *Wipe, and free space.
The desktop test opens Filer windows on the simulated device, copies a folder to it
and a photo back with Filer_Action, saves 12 MB with one OS_File call, and
saves screenshots; it measures the longest pause in the desktop during each
(`PDFS_SIM_RATE=3000` makes the simulated device take 3 MB/s).

## Names and types

- A file's type is kept in its name on the device, as phones and computers
  expect: saving a Text file `Letter` makes `Letter.txt`; a type with no
  known extension gets RISC OS's `,xxx` (`Prog,ffb`). Extensions come from
  MimeMap (with a built-in table when MimeMap isn't there).
- RISC OS sees `Letter.txt` as `Letter`, type Text; `Letter/txt` finds it
  too. Names that would then clash (`a.txt` and `a.jpg`) are shown whole.
  `*PDFSExtensions show` shows every name whole.
- Setting a file's type renames it (`Letter.txt` to `Letter.jpg`); renaming
  keeps the extension.
- Otherwise: `.` and `/` swap, a space becomes a hard space (&A0), characters
  RISC OS reserves become `_`, and so does anything outside Latin-1.

## Big files

Files up to 4 GB (MTP's limit).

- **In the desktop, big new files are sent in the background.** When the
  Filer copies a file that fits in its memory, it saves it with one
  OS_File call, and FileSwitch hands the whole file over inside that one
  call: if PDFS sent it there, the desktop would stop (the pointer moving,
  nothing else) until the device had it all. So, while the PDFS filer task
  is running, a new file of 1 MB or more is only kept as it's written, and
  closing it returns at once. The filer task then sends it a piece at a
  time between other tasks' turns, and shows Filer_Action's own window
  while it does ("Copying files - Writing", the file's path, bytes to go,
  files copied, the progress bar, Abort and Pause), loaded from Filer_Action's
  resources so it looks and reads as every other copy does. Abort cancels
  the transfer and removes the part-sent file from the device. Files, and new folders, that come while one is waiting
  go after it, in order.
- Meanwhile directory listings come from what PDFS already knows, and the
  Free window shows the last figures. Anything else that needs the device
  (reading back a file being sent, deleting, renaming, ejecting) waits for
  the sending to finish. If a send fails (the device unplugged, say), the
  filer reports it and the file isn't left on the device half-written.
- At most four big files wait at once (each holds its contents); a fifth
  waits for the first to go. Quitting the desktop sends what's left first.
- A file being written is kept in memory up to 4 MB; beyond that in a
  dynamic area, "PDFS transfer" (Task Manager shows it), while there's the
  memory (48 MB is always left free), and otherwise in a scrap file in
  `<Wimp$ScrapDir>.PDFS`. Dynamic areas and scrap files go when the file has
  been sent or its device goes; scrap files left by a crash are deleted the
  next time one is needed.
- Outside the desktop (no filer task), a file of 4 MB or more copied by
  *Copy is sent to the device as it's written, as 0.05 to 0.07 did, and
  other files when they're closed.

## USB notes

The USB transport (riscos/usb.c) is the one from 0.03, which works on a
Raspberry Pi with an Android phone. Its use of DeviceFS follows RISC OS
Technologies' Bluetooth-RISCOS32 (BSD-2-Clause) findings:

- Devices come from Service_USB 1; endpoints are DeviceFS streams
  `devices#interface;alternate;endpoint;bulk;nopad;size`.
- Reads: WakeUpRX, then only what BufferSpace says is there, so nothing
  blocks inside DeviceFS.
- Writes: each piece into an empty buffer, then WakeUpTX; done when the
  buffer is empty and the transfer has finished.
- No zero-length packets: datasets get a padding byte, and a file whose
  data would be a whole number of packets goes as all but its last byte
  followed by an edit that adds it.
- Control requests aren't used (some USB drivers do them synchronously);
  `*PDFSUSB control on` allows MTP's Cancel request for testing.

(0.04-0.06 tried USBDriver's 'short' flag and pipelined writes, written to
ROOL's documentation; on the Pi they didn't work, so they were taken out.)

## Not done yet

- Testing with more real devices: phones, tablets, media players, cameras (docs/TESTING-ON-A-PI.md).
- SWIs (no SWI chunk yet). Filing system number &FA and error base &1FA00 are
  provisional until allocated by ROOL.
