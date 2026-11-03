# Changes

## 0.16.2 beta (3 November 2026)

For devices whose answers went missing (an Amazon Fire, and probably the
tablets that "do nothing" on some machines).

- PDFS took what had come in from a device with OS_GBPB. On a USB device,
  DeviceFS passes each OS_GBPB's byte count to USBDriver as the length of
  the next USB read, so after a short answer (say 343 bytes) the next read
  was too short for a whole 512-byte packet. The controller then reports
  "babble", the packet is lost and the input pipe stops: the lost answers
  0.16 cleared up and asked again for. PDFS now takes the bytes straight
  from DeviceFS's buffer (as Dave Higton's !MTP does), which starts no USB
  read, and asks for every read itself: always 16 KB, a whole number of
  packets, with the next one asked for as soon as the last is taken (so
  big copies are as fast as before).
- Unplugging a device while PDFS is in the middle of a USB call: PDFS
  stops using the device's streams straight after the call (they may have
  been closed during it).

## 0.16.1 beta (1 November 2026)

For PDFS loaded at the F12 command line, and devices that ask for access.

- PDFS's icon bar task couldn't start when PDFS was loaded with *RMLoad at
  the F12 command line, and nothing said so: a device plugged in then did
  nothing. Now `*PDFSUSB`, `*PDFSDevices` or `*PDFSMount` typed in a task
  window starts it, and typed at F12 they say how to start it. *PDFSLog
  notes it too.
- A device mounted while the icon bar task wasn't running stayed "mounted"
  with no storages: those commands now finish mounting it.
- When a device is connected but still doesn't let PDFS see its storages
  after 20 seconds (it waits for "Allow access"), a box says what to do on
  it: unlock it, choose File transfer, tap Allow. Once per connection;
  `PDFS$AllowHint` sets the seconds (0: never).
- An Android device is asked about its storages 1 second after it
  connects (was 5): the wait wasn't what made the Amazon Fire work.
- Android's "Internal shared storage" is called "Internal" (its icon said
  "Internal sha").
- *PDFSLog says which device was opened, and each storage made ready.
  *PDFSDevices names a USB device "USB9", not "USB USB9".

## 0.16 beta (31 October 2026)

Android devices that ask for access, Amazon Fire tablets and Motorola
phones in file transfer mode, and answers lost on the way in. Still a beta:
use it at your own risk, with devices whose data you have backed up (see
the README and LICENSE).

- Answers lost on the way in are asked for again. Now and then RISC OS's
  USB driver fails an input transfer (an Amazon Fire on a Pi), and starts
  no more until the pipe is cleared: the device seemed to stop answering,
  and the Filer gave up after 30 seconds. PDFS now notices at once, logs
  why the transfer failed, clears the pipe, drops what's left of the
  answer, and asks again if the request only reads something (a folder, a
  file's details, part of a file); otherwise it waits on for the answer.
  *PDFSDevices counts the answers asked for again.
- "Allow access to device data?": until it's answered, Android shows no
  storages. PDFS now waits in the same session (closing it and trying again
  opened a new session each time, which asked again, so the owner's Allow
  never reached a session still there), and mounts the storages once
  they're there, including when the device reconnects after Allow (an
  Amazon Fire does) or answers slowly at first.
- Android's vendor-specific MTP interface (class FF/FF/00 with an
  interrupt endpoint: Amazon Fire tablets, Motorola phones in File
  transfer mode) is recognised as MTP and mounted by itself. ADB's
  interface isn't.
- Android devices: folders are listed an object at a time
  (GetObjectHandles, then GetObjectInfo for each), not with
  GetObjectPropList, which some (an Amazon Fire) never answer for a big
  folder; libmtp doesn't use it on Android either. On other devices, a
  property list that takes over 5 seconds switches the device to the same.
- The session is opened as the MTP specification says: OpenSession first,
  as transaction 0, with a random session ID, then GetDeviceInfo in the
  session. Transaction IDs wrap past &FFFFFFFF to 1.
- Events are read one container at a time, by the length in its header
  (several waiting could be misread). Dates with a time zone (`Z`,
  `+hhmm`) are read as UTC.
- An Android device is left 5 seconds after its session opens before it's
  asked about its storages, from the filer without holding the desktop;
  then 1/100 second between an answer and its next request (`PDFS$Gap`).
- From a device that can only send whole files (no GetPartialObject), a
  file being read is received in the background, between the desktop's
  polls, and reads wait only for what hasn't arrived yet.
- *PDFSDevices: how files are read and folders listed, the data received
  and sent and how fast, and the longest wait for one request.
- `*PDFSLog trace` also logs every MTP request (with the time), each
  container that comes back, and the USB pipes' state when an answer
  doesn't come.
- The Free window doesn't wait for a device that has stopped answering:
  it shows the last figures known.
- System variables, read when a device is mounted: `PDFS$Gap` (cs between
  an answer and the next request to an Android device; 0 for none) and
  `PDFS$Events 0` (leave the event pipe closed).
- Tests: Android's Allow prompt (with the device reconnecting, or slow);
  an Amazon Fire's descriptors; an input transfer failing in the USB
  emulation, with the answer held or lost; Android devices; events of
  mixed lengths; transaction ID wrap; time zones; 499 host tests.

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
