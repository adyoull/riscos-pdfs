/* usbemu.c: a stand-in for USBDriver and DeviceFS, for testing PDFS's USB
   transport (riscos/usb.c) in RISC OS for Linux, which has no USB.

   Built into a test copy of the module (make out/PDFS-usbemu,ffa): usb.c's
   SWI calls come here first. It shows one MTP device, "USB9" (and a USB
   memory stick, USB8, that PDFS must ignore), in Service_USB 1; its bulk
   pipes, opened through OS_Find as DeviceFS streams, are carried to
   sim/mtpsim.py on 127.0.0.1:7015 over TCP. The streams behave as
   USBDriver's do: OS_GBPB fills or empties a buffer of the size asked for;
   WakeUpTX sends what's in the output buffer as one transfer; WakeUpRX
   fetches up to the bytes asked for into the input buffer; BufferSpace
   gives the size and free space. Everything else goes to the real SWI.

   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "cmodule.h"
#include "../../core/plat.h"

#define OS_GBPB                 0x0C
#define OS_Find                 0x0D
#define OS_ServiceCall          0x30
#define OS_Module               0x1E
#define DeviceFS_CallDevice     0x42744
#define Socket_Creat            0x41200
#define Socket_Connect          0x41204
#define Socket_Recv             0x41205
#define Socket_Send             0x41208
#define Socket_Close            0x41210
#define Socket_Select           0x41211

#define FAKE_BASE  0x7E00     /* our stream "handles"; real ones are small numbers */
#define NSTREAM    6

typedef struct
{
  int used, output, ep;
  unsigned cap, len;        /* buffer size and bytes in it */
  uint8_t *buf;
} stream;

static stream streams[NSTREAM];
static int sock = -1;
unsigned usbemu_transfers, usbemu_short;
static uint32_t container_left;     /* bytes of the current OUT container still to come */
static uint8_t hdr_bytes[4];
static unsigned hdr_have;

/* Follow the containers in what goes out; a transfer that isn't a whole
   number of 512-byte packets ends with a short packet: the device would
   take it as the end of the container. Count those that come too soon. */
static void
check_transfer (const uint8_t *p, unsigned n)
{
  unsigned i = 0;
  while (i < n)
    {
      if (container_left == 0)
        {
          hdr_bytes[hdr_have++] = p[i++];
          if (hdr_have == 4)
            {
              container_left = (uint32_t) hdr_bytes[0] | (uint32_t) hdr_bytes[1] << 8
                               | (uint32_t) hdr_bytes[2] << 16 | (uint32_t) hdr_bytes[3] << 24;
              container_left = container_left > 4 ? container_left - 4 : 0;
              hdr_have = 0;
            }
          continue;
        }
      unsigned k = n - i < container_left ? n - i : container_left;
      i += k;
      container_left -= k;
    }
  if ((n & 511) && (container_left || hdr_have))
    {
      usbemu_short++;
      char v[12];
      int k = 0;
      unsigned x = usbemu_short;
      char t[12];
      do
        t[k++] = (char) ('0' + x % 10);
      while ((x /= 10));
      for (int j = 0; j < k; j++)
        v[j] = t[k - 1 - j];
      v[k] = 0;
      regs r = {{ (unsigned) "PDFSEmu$ShortPackets", (unsigned) v, (unsigned) k, 0, 0 }};
      swix_regs (0x24, &r);     /* OS_SetVarVal */
    }
}

static void
log_line (const char *s)
{
  pd_log (s);
}

static int
connect_sim (void)
{
  if (sock >= 0)
    return 0;
  regs r = {{ 2, 1, 0 }};
  if (swix_regs (Socket_Creat, &r))
    return -1;
  sock = (int) r.r[0];
  for (int style = 0; style < 2; style++)
    {
      uint8_t sa[16];
      memset (sa, 0, sizeof sa);
      if (style == 0)
        sa[0] = 16, sa[1] = 2;
      else
        sa[0] = 2;
      sa[2] = 7015 >> 8;
      sa[3] = 7015 & 255;
      sa[4] = 127;
      sa[7] = 1;
      regs k = {{ (unsigned) sock, (unsigned) sa, 16 }};
      if (!swix_regs (Socket_Connect, &k))
        return 0;
    }
  regs cl = {{ (unsigned) sock }};
  swix_regs (Socket_Close, &cl);
  sock = -1;
  return -1;
}

/* --- the descriptors: an Android phone (MTP) and a memory stick ---------------------- */

static const uint8_t phone[] = {
  /* device: USB 2.0, VID 18D1 (Google), PID 4EE1 */
  18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xD1, 0x18, 0xE1, 0x4E, 0x00, 0x04, 1, 2, 3, 1,
  /* configuration */
  9, 2, 39, 0, 1, 1, 0, 0x80, 250,
  /* interface 0: still image class, PTP/MTP */
  9, 4, 0, 0, 3, 6, 1, 1, 4,
  7, 5, 0x81, 2, 0x00, 0x02, 0,       /* bulk IN 1, 512 */
  7, 5, 0x01, 2, 0x00, 0x02, 0,       /* bulk OUT 1, 512 */
  7, 5, 0x82, 3, 28, 0, 6,            /* interrupt IN 2 */
};

static const uint8_t stick[] = {
  18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x81, 0x07, 0x51, 0x55, 0x00, 0x01, 1, 2, 3, 1,
  9, 2, 32, 0, 1, 1, 0, 0x80, 100,
  9, 4, 0, 0, 2, 8, 6, 0x50, 0,        /* mass storage, SCSI, bulk-only */
  7, 5, 0x81, 2, 0x00, 0x02, 0,
  7, 5, 0x02, 2, 0x00, 0x02, 0,
};

/* One Service_USB record: link word, then the USBServiceCall block */
static uint8_t *
make_record (uint8_t *next, const char *name, int addr, const uint8_t *desc, unsigned len)
{
  regs r = {{ 6, 0, 0, 4 + 32 + len }};
  if (swix_regs (OS_Module, &r))
    return next;
  uint8_t *b = (uint8_t *) r.r[2];
  memset (b, 0, 4 + 32 + len);
  memcpy (b, &next, 4);
  uint8_t *rec = b + 4;
  rec[0] = (uint8_t) (32 + len);
  rec[1] = (uint8_t) ((32 + len) >> 8);
  rec[2] = 32;
  for (int i = 0; name[i]; i++)
    rec[4 + i] = (uint8_t) name[i];
  rec[24] = 1;                  /* bus */
  rec[25] = (uint8_t) addr;
  rec[28] = 3;                  /* high speed */
  memcpy (rec + 32, desc, len);
  return b;
}

/* --- streams --------------------------------------------------------------------------- */

static int
num_after (const char *p, const char *key)
{
  for (; *p && *p != ':'; p++)
    {
      int i = 0;
      while (key[i] && p[i] == key[i])
        i++;
      if (!key[i])
        {
          int v = 0;
          for (p += i; *p >= '0' && *p <= '9'; p++)
            v = v * 10 + (*p - '0');
          return v;
        }
    }
  return -1;
}

static stream *
fake (unsigned h)
{
  if (h < FAKE_BASE || h >= FAKE_BASE + NSTREAM || !streams[h - FAKE_BASE].used)
    return 0;
  return &streams[h - FAKE_BASE];
}

static stream *
fake_stream (unsigned s)       /* "USBDriver's stream handle" = handle + 0x100 */
{
  return fake (s - 0x100);
}

static int
open_fake (const char *path, int output)
{
  const char *colon = path;
  while (*colon && *colon != ':')
    colon++;
  if (colon[1] != 'U' || colon[2] != 'S' || colon[3] != 'B' || colon[4] != '9')
    return -1;
  for (int i = 0; i < NSTREAM; i++)
    if (!streams[i].used)
      {
        stream *s = &streams[i];
        s->cap = (unsigned) num_after (path, "size");
        s->ep = num_after (path, "endpoint");
        if (s->cap == 0 || s->cap > 1 << 20)
          return -1;
        s->buf = pd_alloc (s->cap);
        if (!s->buf)
          return -1;
        s->used = 1;
        s->output = output;
        s->len = 0;
        if (s->ep == 1 && !output && connect_sim ())
          {
            pd_free (s->buf);
            s->used = 0;
            return -1;
          }
        return FAKE_BASE + i;
      }
  return -1;
}

static void
send_all (const uint8_t *p, unsigned n)
{
  while (n && sock >= 0)
    {
      regs r = {{ (unsigned) sock, (unsigned) p, n, 0 }};
      if (swix_regs (Socket_Send, &r) || (int) r.r[0] <= 0)
        return;
      p += r.r[0];
      n -= r.r[0];
    }
}

/* WakeUpRX: fetch what the "device" has, up to bytes */
static void
fetch (stream *s, unsigned bytes)
{
  if (sock < 0 || s->ep != 1)
    return;
  if (bytes > s->cap - s->len)
    bytes = s->cap - s->len;
  if (!bytes)
    return;
  uint32_t set[8] = { 0 };
  set[sock / 32] |= 1u << (sock % 32);
  uint32_t tv[2] = { 0, 2000 };
  regs sel = {{ (unsigned) sock + 1, (unsigned) set, 0, 0, (unsigned) tv }};
  if (swix_regs (Socket_Select, &sel) || sel.r[0] == 0)
    return;
  regs r = {{ (unsigned) sock, (unsigned) (s->buf + s->len), bytes, 0 }};
  if (!swix_regs (Socket_Recv, &r) && (int) r.r[0] > 0)
    s->len += r.r[0];
}

/* --- the SWIs ---------------------------------------------------------------------------- */

oserror *
usbemu_swix (unsigned swi, regs *r)
{
  stream *s;
  switch (swi)
    {
    case OS_ServiceCall:
      if (r->r[1] == 0xD2 && r->r[0] == 1)
        {
          oserror *e = swix_regs (swi, r);       /* anything real first */
          uint8_t *list = (uint8_t *) r->r[2];
          list = make_record (list, "USB8", 2, stick, sizeof stick);
          list = make_record (list, "USB9", 3, phone, sizeof phone);
          r->r[2] = (unsigned) list;
          return e;
        }
      break;

    case OS_Find:
      if (r->r[0] == 0 && (s = fake (r->r[1])))
        {
          pd_free (s->buf);
          s->used = 0;
          return 0;
        }
      if (r->r[0] != 0)
        {
          int h = open_fake ((const char *) r->r[1], (r->r[0] & 0xC0) == 0x80);
          if (h > 0)
            {
              r->r[0] = (unsigned) h;
              return 0;
            }
        }
      break;

    case OS_GBPB:
      if ((s = fake (r->r[1])))
        {
          uint8_t *p = (uint8_t *) r->r[2];
          unsigned n = r->r[3];
          if (r->r[0] == 2 && s->output)
            {
              unsigned room = s->cap - s->len;
              unsigned k = n < room ? n : room;
              memcpy (s->buf + s->len, p, k);
              s->len += k;
              r->r[2] += k;
              r->r[3] = n - k;
            }
          else if (r->r[0] == 4 && !s->output)
            {
              unsigned k = n < s->len ? n : s->len;
              memcpy (p, s->buf, k);
              memmove (s->buf, s->buf + k, s->len - k);
              s->len -= k;
              r->r[2] += k;
              r->r[3] = n - k;
            }
          return 0;
        }
      break;

    case DeviceFS_CallDevice:
      switch (r->r[0])
        {
        case 0x80000002u:                        /* BufferSpace */
          if ((s = fake (r->r[2])))
            {
              r->r[3] = s->cap;
              r->r[4] = s->cap - s->len;
              return 0;
            }
          break;
        case 0x80000007u:                        /* GetHandles2 */
          if ((s = fake (r->r[2])))
            {
              r->r[3] = 0;
              r->r[4] = 0;
              r->r[5] = r->r[2] + 0x100;
              r->r[6] = 0x9999;
              return 0;
            }
          break;
        case 0x80000006u:                        /* TransferInfo: done */
          r->r[3] = 1;
          return 0;
        case 0x80000005u:                        /* ClearStall */
          return 0;
        case 0x80000000u:                        /* control request */
          log_line ("usbemu: control request");
          return 0;
        case 2:                                  /* WakeUpTX: one transfer */
          if ((s = fake_stream (r->r[2])) && s->output && s->len)
            {
              usbemu_transfers++;
              check_transfer (s->buf, s->len);
              send_all (s->buf, s->len);
              s->len = 0;
            }
          return 0;
        case 3:                                  /* WakeUpRX */
          if ((s = fake_stream (r->r[2])))
            fetch (s, r->r[3]);
          return 0;
        }
      break;
    }
  return swix_regs (swi, r);
}
