# What PDFS was written from

PDFS is licensed under the Apache License, Version 2.0 (LICENSE). All of its
code was written for it. This is what was read while writing it: published
specifications and documentation, and source code that was read for how
RISC OS and its components behave. No code from any of these was copied into
PDFS. Where a source is under a licence, it's given.

## Specifications

- **Media Transfer Protocol, revision 1.1** (USB Implementers Forum, 2011):
  containers, operations, responses, events, datasets, object properties.
  core/mtp.c, core/mtp.h, core/fs.c.
- **ISO 15740 (PTP, Picture Transfer Protocol)**, which MTP extends: the
  same transaction model, DeviceInfo, StorageInfo, ObjectInfo.
- **USB Still Image Capture Device Definition** (USB-IF): the interface
  class (6, 1, 1) and the class requests (Cancel, Get Device Status).
- **Android's MTP extensions**: the operation codes of Android's
  GetPartialObject64, SendPartialObject, TruncateObject, BeginEditObject and
  EndEditObject (0x95C1 to 0x95C5), as Android's MTP headers in AOSP
  (`frameworks/av/media/mtp`, Apache License 2.0) list them. Only the
  numbers and their parameters were used.

## RISC OS documentation

- **RISC OS Programmer's Reference Manuals** (the ROOL PRM wiki, and RISC OS
  Technologies' 2026 Reader Edition of it): FileSwitch's filing system
  interface (OS_FSControl 12, the information block and word, FSEntry_Open,
  GetBytes, PutBytes, Args, Close, File, Func), Free_Register, the Wimp
  (tasks, icons, menus, templates, messages, Wimp_ReportError), DeviceFS,
  ResourceFS, MessageTrans, MimeMap, TaskManager, OS_DynamicArea,
  OS_AddCallBack, and the module interface.
- **USBDriver's documentation** (`Doc/USB` in ROOL's USBDriver source):
  Service_USB, DeviceFS's USB special fields, DeviceFS_CallDevice reasons.

## RISC OS source code read for behaviour (ROOL, Apache License 2.0)

From RISC OS Open's sources (the 5.31 source archive):

- **FilerAct** (`c/memmanage`, `c/actionwind`, `h/actionwind`, `c/Buttons`,
  `Resources`): how Filer_Action copies (blocks timed to keep the desktop
  going; one OS_File save for a file that fits its buffer), when its window
  appears, its window's icons and its message tokens. PDFS loads
  Filer_Action's own template and messages from the ROM at run time to show
  that window; none of them are in PDFS.
- **FileSwitch** (`s/FSCtrl2`, `s/LowLevel`): what *ShutDown does, and which
  FSEntry_Func calls get special fields.
- **Free** and the **TaskManager** (`Switcher`): the Free_Register call
  convention, and the shutdown sequence.
- **DeviceFS** (`s/DeviceFS`, `s/Errors`) and **USBDriver** (`h/USBDevFS`,
  `c/usbmodule`): DeviceFS's errors, Service_USB reasons.
- **SCSISoftUSB** (`c/module`, `c/glue`): closing a device's streams on
  Service_DeviceDead.

## Other

- **RISC OS Technologies' Bluetooth-RISCOS32** (BSD-2-Clause:
  `port/c/native_usb`, `docs/USB-TRANSPORT.md`): its notes on using USB
  through DeviceFS on RISC OS 5 (opening bulk endpoint streams, WakeUpRX,
  BufferSpace, avoiding synchronous control requests). riscos/usb.c was
  written for PDFS from what those notes describe; it shares no code with
  native_usb.
