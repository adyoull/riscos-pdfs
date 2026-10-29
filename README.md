# PDFS: Portable Device Filing System

A RISC OS filing system module for phones, tablets and cameras that use
MTP (Media Transfer Protocol). A connected device's storages show up as
discs, such as `PDFS::Pixel-Internal.$`, and work with the Filer, `*Copy`,
BASIC's OPENIN/OPENOUT and every program that uses FileSwitch.

Status: **0.01, test build.** The FileSwitch side and the MTP session layer
are done and tested against a simulated device over TCP. The USB transport
isn't written yet, so it can't talk to a real phone.

Licence: Apache 2.0 (see LICENSE). It contains no GPL code.

## Layout

| Path | What |
|---|---|
| `core/mtp.c` | MTP/PTP session layer: transactions, recovery (cancel, reset, reopen), UTF-16 strings |
| `core/fs.c` | the filing system: devices and storages as discs, directory cache, name and type translation, FileSwitch entry points (Open, GetBytes, PutBytes, Args, Close, File, Func) |
| `core/transport.h`, `core/plat.h` | what the core needs from the platform: a transport (USB later, TCP now) and memory, time, logging, MimeMap |
| `riscos/` | the RISC OS module: header, veneers, *commands, TCP transport over the Internet module, MimeMap |
| `host/host.c` | `pdfs-host`: the same core on Linux, driven from the command line, for tests |
| `sim/mtpsim.py` | a simulated MTP device over TCP; host folders are its storages |
| `tests/host_test.py` | 163 host tests against four kinds of device and a stalled one |
| `tests/rol/` | tests in RISC OS for Linux (riscos-linux-testbed): CLI and desktop |

## Building

Needs riscos-crossdev's toolchain (`RISCOS_TOOLCHAIN`, default
`../riscos-crossdev/toolchain/env`) and gcc for the host build.

    make            # out/PDFS,ffa and out/pdfs-host
    make check      # host tests

## Trying it

    python3 sim/mtpsim.py --port 7015 "Internal=/some/folder" "SD card=/another"

then in RISC OS:

    *RMLoad PDFS
    *PDFSMount 127.0.0.1:7015
    *PDFSDevices
    *Filer_OpenDir PDFS::SimPhone-Internal.$

`*PDFSDismount [disc]` disconnects, `*PDFSLog` shows the log, `*PDFSLog trace`
logs every FileSwitch call (also through Report_Text0).

## Tests in riscos-linux-testbed

    TB=../riscos-linux-testbed RCD_DIR=../riscos-crossdev tests/rol/run.sh
    TB=... RCD_DIR=... ROL_HARDDISC=.../HardDisc4 tests/rol/run.sh --desktop

The CLI test covers *Cat/*Ex/*Info, *CDir, *Copy both ways and between
storages, *Rename, BASIC OPENOUT/OPENUP/append, *Wipe, and free space.
The desktop test opens Filer windows on the phone, copies a folder to it
and a photo back with Filer_Action, and saves screenshots.

## Names and types

- `.` and `/` swap, a space becomes a hard space (&A0), characters RISC OS
  reserves become `_`, and so does anything outside Latin-1.
- Filetypes come from the extension (MimeMap, with a built-in table when
  MimeMap isn't there). Other files are Data (&FFD).

## Not done yet

- USB transport (the Pi first, through the USB stack's DeviceFS).
- Saving a typed file without an extension (`ReadMe`, Text) gives the phone
  a name without one, so after a remount it shows as Data. Whether to add the
  extension (`ReadMe.txt`) and how RISC OS should see that name is still open.
- PDFSFiler (icon bar icons), mounting when a device is plugged in, and
  SWIs (the module has no SWI chunk yet). Filing system number &FA and error
  base &1FA00 are provisional until allocated by ROOL.
