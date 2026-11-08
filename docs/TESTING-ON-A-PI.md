# Testing PDFS with a real device

PDFS has mostly been tested against a simulated phone: over TCP, and
through an emulation of RISC OS's USB stack. These steps try it with a real
device on a Raspberry Pi (or any machine with RISC OS 5's USB stack).


> **Use at your own risk.** PDFS is provided as it is, without
> warranty of any kind, and without liability for any loss or damage,
> including lost data, as the Apache License 2.0 says (sections 7 and 8).
> Only use it with devices whose data you have backed up, and keep copies
> of anything you copy to or from a device until you've checked it.

## 1. Load it

Copy `PDFS,ffa` to the Pi and double-click it, or:

    *RMLoad PDFS

Double-clicking it also starts its icon bar task, "PDFS Filer" in the Task
Manager, which notices devices being plugged in. Loaded with `*RMLoad` at
the F12 command line, the module is there but that task isn't: type
`*PDFSUSB` in a task window (or double-click PDFS) to start it. Simplest of
all: put PDFS in `!Boot.Choices.Boot.PreDesk` and restart.

## 2. Connect the phone

- Android: plug it in, pull down the notification "Charging this device via
  USB" and choose **File transfer**. The phone reconnects as an MTP device.
  If it asks "Allow access to device data?", tap **Allow**: PDFS waits for
  that (*PDFSLog says so) and mounts the storages once they're there. An
  Android device's icons appear a second or two after it connects.
  Set to *Charging only*, a phone may show an MTP interface that never
  answers: PDFS gives up on it after 3 seconds (*PDFSLog says "the device
  isn't taking anything in") and mounts it once it's switched to File
  transfer.
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

- Class `FF/FF/00` with an interrupt endpoint (Amazon Fire tablets,
  Motorola phones in File transfer mode) is MTP, and mounted by itself.
  Another `FF/..` interface (older Androids): `*PDFSMount USB<n>`.
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
- Copying a big file to the device (0.09): Filer_Action reads the file;
  then a "Copying files - Writing" window (Filer_Action's own) shows the
  transfer to the device: bytes to go and the bar, while the desktop keeps
  going (move windows, the clock ticks). Try Pause/Continue, and Abort (the
  part-sent file should not be left on the device). Try Eject during a send
  (an hourglass, then it ejects once the file is there), and unplugging
  during one (an error box saying the file couldn't be sent).
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
each FileSwitch call to it. The trace logs every request to the device with
the time it was made (in centiseconds), what came back, and, when an answer
doesn't come, the state of the USB pipes. Lines starting "usb: an input
transfer failed" give USBDriver's reason; many of them can mean a power or
cable problem (a device charging from the computer: try a powered hub).

If the machine freezes during a transfer, note which step; `*PDFSUSB control
off` (the default) keeps PDFS away from the USB control requests that are
known to freeze some USB drivers.
