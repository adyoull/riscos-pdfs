# Changes

## 0.15 beta (30 October 2026)

Fixes for phones that are switched between charging only and file transfer,
and for folders that seemed to contain themselves. Still a beta: use it at
your own risk, with devices whose data you have backed up (see the README
and LICENSE).

- A phone plugged in set to *charging only* no longer holds up the desktop
  or the boot (it stopped a Pi in !IrisRAM's banner). Such a phone shows
  its MTP interface but takes nothing in: PDFS now gives up on it after
  3 seconds, logs that it isn't taking anything in, and leaves its USB
  streams open until it goes, because closing them would wait for ever.
- Switching a phone from charging to file transfer no longer crashes
  USBDriver ("abort on data transfer" in USBDriver). The phone drops off the
  bus as it switches; PDFS no longer makes a synchronous ClearStall request
  when a transfer fails, and no longer makes the close of a stream whose
  transfer failed succeed (which had USBDriver free a transfer of a device
  that had gone). When the phone comes back in file transfer mode, it's
  mounted.
- Folders listed by their object handles, with the property list only for
  names, sizes and dates: some Android SD cards listed a folder inside
  itself ("DCIM" in DCIM, "Files by Google" in itself) and left files out
  (one photo in Camera). An object that is the folder, a folder above it,
  or says its parent is another folder is left out, and logged. Listing a
  big folder allows the device 30 seconds.
- The filer mounts devices already plugged in once the desktop has started,
  not before its first poll.
- *PDFSLog logs Service_DeviceDead and failed stream closes.
- Tests: a simulated phone switched from charging to file transfer, in
  riscos-linux-testbed (`tests/rol/run.sh --charging`, `--charging-only`,
  with `--irisram` while !IrisRAM copies Iris to the RAM disc), with the
  USBDriver hazards seen on a Pi counted; simulated devices that list a
  folder inside itself.

Known: if a phone is switched while a command to it is on its way, one USB
stream can't be closed, and *ShutDown may then report "Device not found".

## 0.14 beta (29 October 2026)

The first public release: a beta. It works with an Android phone on a
Raspberry Pi running RISC OS 5; other devices and machines still need trying.
Use it at your own risk, with devices whose data you have backed up: it
comes without warranty or liability (see the README and LICENSE).

- MTP devices (phones, tablets, media players, cameras) on USB appear as
  discs, `PDFS::<Model>-<Storage>.$`, with a drive icon on the icon bar for
  each storage; they're mounted when plugged in (and set to File transfer).
- Works with the Filer, *Copy, BASIC and every program that uses FileSwitch:
  directories, reading, writing, renaming, moving, deleting, free space.
- Filetypes as name extensions on the device (`Letter.txt` is a Text file
  `Letter`), from MimeMap; `,xxx` for types with none. *PDFSExtensions.
- Files are written to the device as other filing systems write: while the
  program writes them, so Filer_Action's window shows the whole copy and
  its Abort works. Big files are kept in a dynamic area, or a scrap file,
  while they're written. *PDFSBackground on sends big new files afterwards
  instead, with a copy window of PDFS's own.
- The icon's menu: Open '$', Free, Eject, Eject all (safe to unplug).
  Unplugging without ejecting removes the icons.
- USB: DeviceFS streams on the device's bulk endpoints; no zero-length
  packets are needed (data phases that would need one go in two parts).
- *PDFSMount, *PDFSDismount, *PDFSDevices, *PDFSUSB, *PDFSEject, *PDFSLog.

Since the last test build (0.13): *PDFSBackground's help, the release zip
(`make dist`), GitHub builds, screenshots, docs/REFERENCES.md.
