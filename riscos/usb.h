/* usb.h: PDFS's USB transport (usb.c).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef PDFS_USB_H
#define PDFS_USB_H

#include <stdint.h>
#include "../core/pdfs.h"

/* An interface of a connected USB device that has bulk IN and OUT endpoints */
typedef struct
{
  char dev[20];                 /* DeviceFS name: "USB5" */
  uint16_t vid, pid;
  uint8_t speed;                /* 1 low, 2 full, 3 high, 4 super */
  uint8_t iface, alt, cls, sub, proto;
  uint8_t ep_in, ep_out, ep_int;
  uint16_t mps, mps_out;        /* bulk IN and OUT packet sizes */
  uint8_t usable;               /* has bulk IN and OUT */
  uint8_t mtp;                  /* usable and class 6/1/1 (still image, PTP/MTP) */
} usb_iface;

extern int usb_control_requests;
extern volatile int usb_attached;

/* Connected devices' interfaces: MTP ones, or all of them. */
int usb_scan (usb_iface *out, int max, int all);
int usb_is_open (const char *dev);
const pd_error *usb_mount (const usb_iface *c);
void usb_device_dead (const char *name);
void usb_dying (void);

#endif
