# PDFS: Portable Device Filing System

A RISC OS filing system module for phones, tablets and cameras that use
MTP (Media Transfer Protocol). A connected device's storages show up as
discs, such as `PDFS::Pixel-Internal.$`, and work with the Filer, `*Copy`,
BASIC's OPENIN/OPENOUT and every program that uses FileSwitch.

Status: **0.06.** USB (the RISC OS 5 USB stack), the icon bar filer and
filetype extensions are done. It's tested against a simulated device, over
TCP and through an emulation of USBDriver and DeviceFS; it still has to be
tried with a real phone (see docs/TESTING-ON-A-PI.md).

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

In the desktop, plug the phone in and set it to *File transfer* (Android) or
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
| `*PDFSUSB [control\|short\|pipeline on\|off]` | connected USB devices' interfaces, which are MTP, and how PDFS uses USB (switches for testing) |
| `*PDFSEject <disc> [all]` | eject a storage, or (all) the whole device, safely |
| `*PDFSDismount [<disc>]` | disconnect at once |
| `*PDFSExtensions [show\|hide]` | how names with a type extension are shown |
| `*PDFSLog [on\|off\|trace]` | the log; trace logs every FileSwitch call (also to Reporter) |

Paths are `PDFS::<Model>-<Storage>.$.…`, e.g. `PDFS::Pixel_7-Internal.$.DCIM`.

To try it without a phone: `python3 sim/mtpsim.py --port 7015 "Internal=/some/folder"`
and `*PDFSMount 127.0.0.1:7015`.

## Tests in riscos-linux-testbed

    TB=../riscos-linux-testbed RCD_DIR=../riscos-crossdev tests/rol/run.sh
    TB=... RCD_DIR=... ROL_HARDDISC=.../HardDisc4 tests/rol/run.sh --desktop
    make out/PDFS-usbemu,ffa && TB=... tests/rol/run.sh --usbemu [--old-usbdriver] [--desktop]

`--usbemu` uses a test build whose USB transport talks to an emulation of
USBDriver and DeviceFS (tests/rol/usbemu.c): a fake phone, USB9, bridged to
the simulator, which then fails any data phase that would need a USB
zero-length packet (`--zlp-strict 512`).

The CLI test covers *Cat/*Ex/*Info, *CDir, *Copy both ways and between
storages, *Rename, BASIC OPENOUT/OPENUP/append, *Wipe, and free space.
The desktop test opens Filer windows on the phone, copies a folder to it
and a photo back with Filer_Action, and saves screenshots.

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

- Copied by the Filer (Filer_Action) or *Copy, a file of 4 MB or more is
  sent to the device as it's written: these create the file with its size
  and write it in order, so PDFS starts the transfer at the first write and
  passes each block on. The desktop keeps going between blocks, and no
  scrap file is needed. While that's going on, other requests that need the
  device get "The device is busy receiving a file"; directory listings come
  from what PDFS already knows.
- Otherwise a file being written is kept in memory up to 4 MB, and beyond
  that in a scrap file in `<Wimp$ScrapDir>.PDFS`, and sent when it's closed.
  Scrap files are deleted when the file is closed or its device goes; any
  left by a crash are deleted the next time one is needed.

## USB notes

Written to RISC OS Open's USB API: USBDriver's `Doc/USB` (0.95) and
`h/USBDevFS`, checked against its `c/usbmodule` (from ROOL's source archive,
src-iyo-dev.5.31).

- Devices come from Service_USB 1 (USBServiceAnswer/USBServiceCall blocks).
  Endpoints are DeviceFS streams with special fields
  `interface;alternate;endpoint;bulk;nopad;size`.
- Reads: WakeUpRX with the length wanted, then only what Check Buffer Space
  says is there, so nothing blocks inside DeviceFS. (USBDriver returns R3 =
  the buffer's size, R4 = free, as `get_buffer_space` does.)
- Writes: USBDriver sends what's in the buffer as one transfer, so each piece
  goes into an empty buffer, in whole packets but for a container's last.
- MTP needs a zero-length packet after a container that's a whole number of
  packets. By default PDFS avoids such containers (as 0.03 did, which works
  on a Pi): datasets get a padding byte and such a file goes as all but its
  last byte followed by an edit that adds it. `*PDFSUSB short on` uses the
  stream's `short` flag instead (USBDriver 0.49+, Get/Set Options, set for a
  container's last piece only). Both are written to ROOL's documentation,
  but only the default has been tried on real hardware.
- Writes go into an empty buffer, one piece at a time (as 0.03).
  `*PDFSUSB pipeline on` adds to the buffer while a transfer is going
  (USBDriver's write_cb starts the next transfer itself): faster, untried on
  real hardware.
- Control requests aren't used: USBDriver does them synchronously.
  `*PDFSUSB control on` allows MTP's Cancel request for testing.

## Not done yet

- Testing with real phones and cameras (docs/TESTING-ON-A-PI.md).
- SWIs (no SWI chunk yet). Filing system number &FA and error base &1FA00 are
  provisional until allocated by ROOL.
