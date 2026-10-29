# Testing PDFS with a real phone

PDFS 0.02 has been tested against a simulated phone only: over TCP, and
through an emulation of RISC OS's USB stack. These steps try it with a real
device on a Raspberry Pi (or any machine with RISC OS 5's USB stack).

## 1. Load it

Copy `PDFS,ffa` to the Pi and double-click it, or:

    *RMLoad PDFS

If it's loaded after the desktop has started and no icon task appears in the
Task Manager, start the filer with `*WimpTask Desktop_PDFSFiler`.

## 2. Connect the phone

- Android: plug it in, pull down the notification "Charging this device via
  USB" and choose **File transfer**. The phone reconnects as an MTP device.
- iPhone: unlock it and choose Trust (PTP only: photos, read-only).
- Cameras: switch them to PTP/MTP or "PC connection".

Within a second or two, drive icons should appear on the icon bar
("Internal", "SD card").

## 3. If no icon appears

In a task window (Ctrl-F12):

    *PDFSUSB

It lists every USB interface with bulk endpoints. The phone should show
class `06/01/01` and `- MTP`. Then:

    *PDFSMount

- An interface with class `FF/..` (older Androids): `*PDFSMount USB<n>`.
- "Can't open the device's USB endpoints": something else claimed it.
- Nothing listed for the phone: it isn't in File transfer mode, or the USB
  stack doesn't see it (`*USBDevices`).

## 4. Things to try

- Open Internal, DCIM, Camera: photos show as JPEG.
- Copy a photo to the RISC OS disc; open it.
- Drag a Text file from RISC OS to the phone's icon: it arrives as `name.txt`
  in the phone's top folder.
- Copy a file of exactly 500 bytes (500 + 12 = one 512-byte packet: the
  case that needs the zero-length-packet workaround).
- Copy a large file (100 MB+) both ways: note the time.
- Make a directory, rename a file, delete it.
- Menu > Free on the icon.
- With an SD card in the phone: Menu > Eject on the SD card icon (only that
  icon goes), then Menu > Eject all on the other (a box says it can be
  unplugged). Try Eject while a file on the phone is open in an
  application: it should be refused.
- *PDFSMount brings back storages ejected one at a time.
- Unplug the phone without ejecting: the icons and windows should go.

## 5. What to send back if something goes wrong

    *PDFSLog trace
    (do the thing that fails)
    *Spool RAM:$.pdfslog
    *PDFSLog
    *PDFSUSB
    *PDFSDevices
    *Spool

and send `RAM:$.pdfslog`. With Reporter running, `*PDFSLog trace` also sends
each FileSwitch call to it.

If the machine freezes during a transfer, note which step; `*PDFSUSB control
off` (the default) keeps PDFS away from the USB control requests that are
known to freeze some USB drivers.
