# Changes

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
