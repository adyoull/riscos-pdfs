/* usb.c: PDFS's USB transport, through the RISC OS 5 USB stack (USBDriver)
   and DeviceFS, as RISC OS Open documents it: USBDriver's Doc/USB (the USB
   API, version 0.95) and h/USBDevFS, checked against its c/usbmodule.

   Finding devices: Service_USB 1 (Service_USB_Connected) gives a list of
   USBServiceAnswer blocks, each a link word and a USBServiceCall: length,
   offset to the descriptors, DeviceFS name ("USB5"), bus, address, speed,
   then the device descriptor and the configuration's descriptors. The
   caller frees each block (OS_Module 7). An MTP interface is class 6 (still
   image), subclass 1, protocol 1 with a bulk IN, a bulk OUT and an
   interrupt IN endpoint. Others (vendor-specific ones on older Androids)
   can be named with *PDFSMount USB<n>.

   Talking to one: each endpoint is a DeviceFS stream, opened with special
   fields separated by semicolons:
     devices#interface<i>;alternate<a>;endpoint<e>;bulk;nopad;size<n>:USB<n>
   (nopad: received data isn't padded to the length asked for).
   - Input: DeviceFS_CallDevice reason 3 (WakeUpRX), R2 = USBDriver's stream
     handle (Return Handles 2, &80000007, R5), R3 = the length wanted: that
     starts a bulk IN transfer into the stream's buffer, which is read only
     as far as it has data (Check Buffer Space, &80000002), so nothing
     blocks inside DeviceFS. (USBDriver returns R3 = the buffer's size and
     R4 = its free space: c/usbmodule get_buffer_space.)
   - Output: USBDriver sends what's in the stream's buffer as one transfer
     when woken (reason 2, WakeUpTX; c/usbmodule start_write). So each piece
     is written into an empty buffer, a whole number of packets except a
     container's last; Transfer Info (&80000006) says when it's done.
   - Zero-length packets: MTP needs one after a container that's a whole
     number of packets. The stream's 'short' flag (USBDriver 0.49+, Get/Set
     Options &80000008, bit 1; USBD_FORCE_SHORT_XFER) sends one after such a
     transfer: it's set for a container's last piece only. With an older
     USBDriver the transport says no_zlp and the MTP layer avoids those
     transfers instead (core/mtp.h).
   - Interrupt IN (events): USBDriver keeps reading it by itself.
   - Unplugging: DeviceFS issues Service_DeviceDead with the device's name.

   Not used: USB control requests (&80000000). USBDriver does them
   synchronously (c/usbmodule: usbd_do_request; Doc/USB: R6 options other
   than 0 aren't available on RISC OS 5). The MTP class requests (Cancel,
   Get Device Status) are only for recovery, which here drains and reopens
   the pipes instead; *PDFSUSB control on allows Cancel for testing.

   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "cmodule.h"
#include "../core/pdfs.h"
#include "../core/plat.h"
#include "usb.h"

#ifdef USB_EMU                   /* the test build: tests/rol/usbemu.c */
oserror *usbemu_swix (unsigned swi, regs *r);
#define swix_regs usbemu_swix
#endif

#define OS_GBPB                 0x0C
#define OS_Find                 0x0D
#define OS_ServiceCall          0x30
#define OS_ReadMonotonicTime    0x42
#define DeviceFS_CallDevice     0x42744

#define DFS_USB_REQUEST         0x80000000u
#define DFS_BUFFER_SPACE        0x80000002u
#define DFS_CLEAR_STALL         0x80000005u
#define DFS_TRANSFER_INFO       0x80000006u
#define DFS_GET_HANDLES2        0x80000007u
#define DFS_GETSET_OPTIONS      0x80000008u
#define OPT_SHORT               2u          /* Get/Set Options: the 'short' flag */
#define USBDriver_Version       0x54A45
#define DEVCALL_WAKEUP_TX       2u
#define DEVCALL_WAKEUP_RX       3u

#define BULK_BUFFER             65536u
#define EVENT_BUFFER            1024u
#define MAX_OPEN                4

int usb_control_requests;       /* *PDFSUSB control on */
volatile int usb_attached;      /* Service_USB Attach seen: the filer mounts */

typedef struct usbt
{
  pd_transport t;
  usb_iface c;
  int h_in, h_out, h_int;       /* FileSwitch handles of the streams */
  unsigned s_in, s_out;         /* USBDriver's stream handles (GetHandles2 R5) */
  unsigned devhandle;           /* DeviceFS device handle (R6), for DeviceDead */
  int short_on;                 /* the output stream's 'short' flag as last set */
  char name[48];
} usbt;

static usbt *open_list[MAX_OPEN];

/* --- small helpers ------------------------------------------------------------------- */

static unsigned
now_cs (void)
{
  regs r = {{ 0 }};
  swix_regs (OS_ReadMonotonicTime, &r);
  return r.r[0];
}

static char *
put_str (char *o, const char *s)
{
  while (*s)
    *o++ = *s++;
  *o = 0;
  return o;
}

static char *
put_dec (char *o, unsigned v)
{
  char b[12];
  int n = 0;
  do
    b[n++] = (char) ('0' + v % 10);
  while ((v /= 10) && n < 11);
  while (n)
    *o++ = b[--n];
  *o = 0;
  return o;
}

static unsigned
le16 (const uint8_t *p)
{
  return (unsigned) p[0] | (unsigned) p[1] << 8;
}

/* --- finding devices -------------------------------------------------------------------- */

/* Interfaces of one Service_USB record (after its link word): appended to
   out, up to max. Returns the new count. */
static int
decode_record (const uint8_t *rec, usb_iface *out, int n, int max, int all)
{
  unsigned total = le16 (rec), off = le16 (rec + 2);
  if (total < 32 || off < 32 || off + 18 > total)
    return n;
  const uint8_t *dd = rec + off;
  if (dd[0] < 18 || dd[1] != 1)
    return n;
  char dev[20];
  int i = 0;
  for (; i < 19 && rec[4 + i] > ' '; i++)
    dev[i] = (char) rec[4 + i];
  dev[i] = 0;
  if (!i)
    return n;
  const uint8_t *p = dd + dd[0], *end = rec + total;
  usb_iface cur;
  int have = 0;
  while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end)
    {
      if (p[1] == 4 && p[0] >= 9)          /* interface */
        {
          if (have && n < max && (all || cur.mtp))
            out[n++] = cur;
          memset (&cur, 0, sizeof cur);
          for (int k = 0; k < 20; k++)
            cur.dev[k] = dev[k];
          cur.vid = (uint16_t) le16 (dd + 8);
          cur.pid = (uint16_t) le16 (dd + 10);
          cur.speed = rec[28];
          cur.iface = p[2];
          cur.alt = p[3];
          cur.cls = p[5];
          cur.sub = p[6];
          cur.proto = p[7];
          have = 1;
        }
      else if (p[1] == 5 && p[0] >= 7 && have)   /* endpoint */
        {
          unsigned addr = p[2], type = p[3] & 3, mps = le16 (p + 4) & 0x7FF;
          if (type == 2 && (addr & 0x80) && !cur.ep_in)
            cur.ep_in = (uint8_t) (addr & 15), cur.mps = (uint16_t) mps;
          else if (type == 2 && !(addr & 0x80) && !cur.ep_out)
            cur.ep_out = (uint8_t) (addr & 15), cur.mps_out = (uint16_t) mps;
          else if (type == 3 && (addr & 0x80) && !cur.ep_int)
            cur.ep_int = (uint8_t) (addr & 15);
          cur.usable = cur.ep_in && cur.ep_out;
          cur.mtp = cur.usable && cur.cls == 6 && cur.sub == 1 && cur.proto == 1;
        }
      p += p[0];
    }
  if (have && n < max && (all || cur.mtp))
    out[n++] = cur;
  return n;
}

int
usb_scan (usb_iface *out, int max, int all)
{
  regs r = {{ 1, 0xD2, 0 }};                   /* Service_USB 1: connected devices */
  if (swix_regs (OS_ServiceCall, &r))
    return 0;
  uint8_t *rec = (uint8_t *) r.r[2];
  int n = 0;
  while (rec)
    {
      uint8_t *next;
      memcpy (&next, rec, sizeof next);
      n = decode_record (rec + 4, out, n, max, all);
      regs f = {{ 7, 0, (unsigned) rec }};
      swix_regs (0x1E, &f);                      /* OS_Module 7: free it */
      rec = next;
    }
  return n;
}

/* --- streams -------------------------------------------------------------------------- */

static int
open_stream (const usb_iface *c, int ep, const char *type, unsigned size, int output)
{
  char path[128], *o = path;
  o = put_str (o, "devices#interface");
  o = put_dec (o, c->iface);
  o = put_str (o, ";alternate");
  o = put_dec (o, c->alt);
  o = put_str (o, ";endpoint");
  o = put_dec (o, (unsigned) ep);
  o = put_str (o, ";");
  o = put_str (o, type);
  o = put_str (o, ";nopad;size");
  o = put_dec (o, size);
  o = put_str (o, ":");
  put_str (o, c->dev);
  regs r = {{ output ? 0x8Fu : 0x4Fu, (unsigned) path }};
  if (swix_regs (OS_Find, &r))
    return 0;
  return (int) r.r[0];
}

static void
close_stream (int *h)
{
  if (*h)
    {
      regs r = {{ 0, (unsigned) *h }};
      swix_regs (OS_Find, &r);
      *h = 0;
    }
}

/* R3 = buffer size, R4 = free bytes */
static int
buffer_space (usbt *u, int h, unsigned *used, unsigned *free)
{
  regs r = {{ DFS_BUFFER_SPACE, (unsigned) u->c.dev, (unsigned) h, 0, 0 }};
  if (swix_regs (DeviceFS_CallDevice, &r) || r.r[4] > r.r[3])
    return -1;
  *used = r.r[3] - r.r[4];
  *free = r.r[4];
  return 0;
}

static void
wake (usbt *u, unsigned call, unsigned stream, unsigned bytes)
{
  regs r = {{ call, (unsigned) u->c.dev, stream, bytes }};
  swix_regs (DeviceFS_CallDevice, &r);
}

static int
handles (usbt *u, int h, unsigned *stream)
{
  regs r = {{ DFS_GET_HANDLES2, (unsigned) u->c.dev, (unsigned) h, 0, 0, 0, 0 }};
  if (swix_regs (DeviceFS_CallDevice, &r) || !r.r[5] || !r.r[6])
    return -1;
  *stream = r.r[5];
  u->devhandle = r.r[6];
  return 0;
}

static int
gbpb (int h, void *buf, unsigned len, int write)
{
  regs r = {{ write ? 2u : 4u, (unsigned) h, (unsigned) buf, len }};
  if (swix_regs (OS_GBPB, &r) || r.r[3] > len)
    return -1;
  return (int) (len - r.r[3]);
}

static void
close_streams (usbt *u)
{
  close_stream (&u->h_in);
  close_stream (&u->h_out);
  close_stream (&u->h_int);
  u->s_in = u->s_out = 0;
}

static int
open_streams (usbt *u)
{
  u->h_in = open_stream (&u->c, u->c.ep_in, "bulk", BULK_BUFFER, 0);
  u->h_out = open_stream (&u->c, u->c.ep_out, "bulk", BULK_BUFFER, 1);
  if (u->c.ep_int)
    u->h_int = open_stream (&u->c, u->c.ep_int, "interrupt", EVENT_BUFFER, 0);
  if (!u->h_in || !u->h_out || handles (u, u->h_in, &u->s_in) || handles (u, u->h_out, &u->s_out))
    {
      close_streams (u);
      return -1;
    }
  u->short_on = 0;              /* new streams: 'short' off */
  return 0;
}

/* --- the transport ------------------------------------------------------------------- */

/* Wait until the output buffer is empty and its transfer has finished.
   0, or a PDT_ error. */
static int
out_idle (usbt *u, unsigned timeout_cs)
{
  unsigned start = now_cs (), empty_since = 0, used, free;
  for (;;)
    {
      if (u->t.gone || buffer_space (u, u->h_out, &used, &free))
        return u->t.gone = 1, PDT_GONE;
      regs ti = {{ DFS_TRANSFER_INFO, (unsigned) u->c.dev, u->s_out, 0, 0 }};
      int status = swix_regs (DeviceFS_CallDevice, &ti) ? 1 : (int) ti.r[3];
      if (status < 0)
        return PDT_IO;          /* a stalled pipe: usb_reset clears it */
      if (used == 0 && status != 0)
        return 0;
      /* the buffer has emptied: don't wait long for a status that may
         never say "complete" */
      if (used == 0)
        {
          if (!empty_since)
            empty_since = now_cs () | 1;
          else if (now_cs () - empty_since > 50)
            return 0;
        }
      else
        wake (u, DEVCALL_WAKEUP_TX, u->s_out, 0);
      if (now_cs () - start > timeout_cs)
        return PDT_TIMEOUT;
    }
}

/* Set or clear the output stream's 'short' flag (Get/Set Options: R3 = EOR
   mask, R4 = AND mask). Only while no transfer is active: callers wait. */
static int
set_short (usbt *u, int on)
{
  if (u->short_on == on)
    return 0;
  regs r = {{ DFS_GETSET_OPTIONS, (unsigned) u->c.dev, u->s_out,
              on ? OPT_SHORT : 0, ~OPT_SHORT }};
  if (swix_regs (DeviceFS_CallDevice, &r))
    return -1;
  u->short_on = on;
  return 0;
}

/* Send (part of) a container. USBDriver starts a transfer with whatever
   is in the stream's buffer, and when it ends starts another with what has
   arrived meanwhile (c/usbmodule write_cb). A transfer that isn't a whole
   number of packets ends with a short packet, which ends the container for
   the device: so everything goes in whole packets but a container's last
   piece. The 'short' flag can be changed only while no transfer is active:
   that's the only time this waits for the buffer to empty, and at the end
   of a container. */
static int
usb_write (pd_transport *t, const void *buf, uint32_t len, unsigned timeout_cs)
{
  usbt *u = (usbt *) t;
  const uint8_t *p = buf;
  unsigned used, free, mps = t->max_packet;
  unsigned start = now_cs ();
  int e;
  while (len)
    {
      int last = !t->more;          /* this call ends the container */
      if (!t->no_zlp && u->short_on != last && len <= BULK_BUFFER)
        {
          /* the container's last piece: 'short' on (after a whole number
             of packets a zero-length packet follows); else off */
          if ((e = out_idle (u, timeout_cs)) || set_short (u, last))
            return e ? e : PDT_IO;
        }
      if (t->gone || buffer_space (u, u->h_out, &used, &free))
        return t->gone = 1, PDT_GONE;
      unsigned n = len <= free ? len : free & ~(mps - 1);
      if (n < len && !t->no_zlp && u->short_on)
        n = 0;                      /* not the last piece: wait to clear 'short' */
      if (!n)
        {
          if (!t->no_zlp && u->short_on && len > free)
            {
              if ((e = out_idle (u, timeout_cs)) || set_short (u, 0))
                return e ? e : PDT_IO;
            }
          else
            wake (u, DEVCALL_WAKEUP_TX, u->s_out, 0);
          if (now_cs () - start > timeout_cs)
            return PDT_TIMEOUT;
          continue;
        }
      int k = gbpb (u->h_out, (void *) p, n, 1);
      if (k != (int) n)
        return PDT_IO;
      p += n;
      len -= n;
      wake (u, DEVCALL_WAKEUP_TX, u->s_out, 0);
      start = now_cs ();
    }
  /* the end of a container: wait till it has gone */
  return t->more ? 0 : out_idle (u, timeout_cs);
}

static int
usb_read (pd_transport *t, void *buf, uint32_t max, unsigned timeout_cs)
{
  usbt *u = (usbt *) t;
  unsigned start = now_cs (), used, free;
  if (t->gone)
    return PDT_GONE;
  for (;;)
    {
      if (t->gone || buffer_space (u, u->h_in, &used, &free))
        return t->gone = 1, PDT_GONE;
      if (used)
        {
          int k = gbpb (u->h_in, buf, used < max ? used : max, 0);
          return k < 0 ? PDT_IO : k;
        }
      /* ask for a transfer: as much as the buffer has room for, in whole
         packets (it ends early at a short packet) */
      unsigned want = free > 32768 ? 32768 : free;
      unsigned mps = u->c.mps ? u->c.mps : 64;
      want &= ~(mps - 1);
      if (want)
        wake (u, DEVCALL_WAKEUP_RX, u->s_in, want);
      if (now_cs () - start > timeout_cs)
        return PDT_TIMEOUT;
    }
}

static int
usb_event (pd_transport *t, void *buf, uint32_t max)
{
  usbt *u = (usbt *) t;
  unsigned used, free;
  if (!u->h_int || t->gone || buffer_space (u, u->h_int, &used, &free) || used < 12)
    return 0;
  int k = gbpb (u->h_int, buf, used < max ? used : max, 0);
  return k < 0 ? 0 : k;
}

/* Class request (only with *PDFSUSB control on: see the top of the file) */
static void
class_request (usbt *u, unsigned request, void *data, unsigned len, int in)
{
  if (!usb_control_requests)
    return;
  regs r = {{ DFS_USB_REQUEST, (unsigned) u->c.dev, 0,
              (in ? 0xA1u : 0x21u) | request << 8, u->c.iface | len << 16,
              (unsigned) data, 0, 0 }};
  swix_regs (DeviceFS_CallDevice, &r);
}

static void
drain (usbt *u, unsigned cs)
{
  uint8_t junk[512];
  unsigned start = now_cs (), used, free;
  unsigned want = u->c.mps > 512 ? u->c.mps : 512;   /* whole packets */
  while (now_cs () - start < cs)
    {
      if (u->t.gone || buffer_space (u, u->h_in, &used, &free))
        return;
      if (used)
        gbpb (u->h_in, junk, used < sizeof junk ? used : sizeof junk, 0);
      else if (free >= want)
        wake (u, DEVCALL_WAKEUP_RX, u->s_in, want);
    }
}

static int
usb_reset (pd_transport *t, uint32_t tid, int level)
{
  usbt *u = (usbt *) t;
  if (t->gone)
    return PDT_GONE;
  /* MTP Cancel (class request &64): code &4001, then the transaction id */
  uint8_t cancel[6] = { 0x01, 0x40, (uint8_t) tid, (uint8_t) (tid >> 8),
                        (uint8_t) (tid >> 16), (uint8_t) (tid >> 24) };
  class_request (u, 0x64, cancel, 6, 0);
  /* What a failed write left in the output buffer mustn't go out before the
     next command: clear any stall and reopen the output stream (level 1),
     or all the pipes (level 2: the session starts again) */
  regs cs = {{ DFS_CLEAR_STALL, (unsigned) u->c.dev, (unsigned) u->h_out }};
  swix_regs (DeviceFS_CallDevice, &cs);
  if (level >= 2)
    {
      regs ci = {{ DFS_CLEAR_STALL, (unsigned) u->c.dev, (unsigned) u->h_in }};
      swix_regs (DeviceFS_CallDevice, &ci);
      close_streams (u);
      if (open_streams (u))
        return t->gone = 1, PDT_GONE;
      drain (u, 20);
      return 1;
    }
  close_stream (&u->h_out);
  u->h_out = open_stream (&u->c, u->c.ep_out, "bulk", BULK_BUFFER, 1);
  if (!u->h_out || handles (u, u->h_out, &u->s_out))
    return t->gone = 1, PDT_GONE;
  u->short_on = 0;
  drain (u, 30);
  return 0;
}

static void
usb_close (pd_transport *t)
{
  usbt *u = (usbt *) t;
  close_streams (u);
  for (int i = 0; i < MAX_OPEN; i++)
    if (open_list[i] == u)
      open_list[i] = 0;
  pd_free (u);
}

/* --- mounting -------------------------------------------------------------------------- */

int
usb_is_open (const char *dev)
{
  for (int i = 0; i < MAX_OPEN; i++)
    if (open_list[i])
      {
        const char *a = open_list[i]->c.dev, *b = dev;
        while (*a && (*a | 0x20) == (*b | 0x20))
          a++, b++;
        if (!*a && *b <= ' ')
          return 1;
      }
  return 0;
}

const pd_error *
usb_mount (const usb_iface *c)
{
  static const pd_error err_open = { 0x1FAE4, "Can't open the device's USB endpoints (is something else using it?)" };
  static const pd_error err_full = { 0x1FAE5, "Too many USB devices mounted" };
  static const pd_error err_mem = { 0x1FAB8, "Not enough memory" };
  int slot = -1;
  for (int i = 0; i < MAX_OPEN; i++)
    if (!open_list[i])
      slot = i;
  if (slot < 0)
    return &err_full;
  usbt *u = pd_alloc (sizeof *u);
  if (!u)
    return &err_mem;
  u->c = *c;
  if (open_streams (u))
    {
      pd_free (u);
      return &err_open;
    }
  u->t.write = usb_write;
  u->t.read = usb_read;
  u->t.event = usb_event;
  u->t.reset = usb_reset;
  u->t.close = usb_close;
  u->t.max_packet = c->mps_out ? c->mps_out : 512;
  /* zero-length packets need the 'short' flag: USBDriver 0.49 and later */
  regs v = {{ 0 }};
  u->short_on = -1;
  u->t.no_zlp = swix_regs (USBDriver_Version, &v) || v.r[0] < 49 || set_short (u, 0);
  char *o = put_str (u->name, "USB ");
  put_str (o, c->dev);
  u->t.name = u->name;
  open_list[slot] = u;
  /* a stale response left from an earlier program: drop it */
  drain (u, 10);
  return pdfs_mount (&u->t);    /* closes the transport if it fails */
}

/* Service_DeviceDead for a DeviceFS device ("USB5") */
void
usb_device_dead (const char *name)
{
  for (int i = 0; i < MAX_OPEN; i++)
    if (open_list[i])
      {
        const char *a = open_list[i]->c.dev, *b = name;
        while (*a && (*a | 0x20) == (*b | 0x20))
          a++, b++;
        if (!*a && *b <= ' ')
          open_list[i]->t.gone = 1;
      }
}

/* The USB system is going (Service_USB 4) */
void
usb_dying (void)
{
  for (int i = 0; i < MAX_OPEN; i++)
    if (open_list[i])
      open_list[i]->t.gone = 1;
}
