/* usbemu.c: a stand-in for USBDriver and DeviceFS, for testing PDFS's USB
   transport (riscos/usb.c) in RISC OS for Linux, which has no USB.

   Built into a test copy of the module (make out/PDFS-usbemu,ffa): usb.c's
   SWI calls come here first. It shows one MTP device, "USB9" (and a USB
   memory stick, USB8, that PDFS must ignore), in Service_USB 1; its bulk
   pipes, opened through OS_Find as DeviceFS streams, are carried to
   sim/mtpsim.py on 127.0.0.1:7015 over TCP. The streams behave as
   USBDriver's do: OS_GBPB fills or empties a buffer of the size asked for;
   WakeUpTX sends what's in the output buffer as one transfer, followed by a
   zero-length packet if the stream's 'short' flag is set (Get/Set Options)
   and the transfer is a whole number of 512-byte packets; WakeUpRX
   fetches up to the bytes asked for into the input buffer; BufferSpace
   gives the size and free space. Everything else goes to the real SWI.

   PDFSEmu$Scenario "charging" plays a phone being switched from charging
   only to file transfer, as on a Pi (0.15 tests): see "the charging
   scenario" below.

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
#define Socket_Ioctl            0x41212

#define FAKE_BASE  0x7E00     /* our stream "handles"; real ones are small numbers */
#define NSTREAM    6

typedef struct
{
  int used, output, ep;
  int close_failed;         /* PDFSEmu$CloseFails: its first close failed */
  int purged;               /* its buffer was purged (nothing left to send) */
  int gen;                  /* which enumeration of the phone it's on */
  int failed;               /* its transfer failed when the device went */
  int inflight;             /* a transfer the charging phone NAKs is on the bus */
  unsigned toggle;          /* host side: packets sent since the pipe was opened (or ClearStall) */
  unsigned flags;           /* Get/Set Options: bit 1 'short' */
  int halted;               /* this input pipe's read failed (babble, or PDFSEmu$InHaltAfter): no more until ClearStall */
  unsigned rd_len, rd_got;  /* the bulk IN read in progress: its length, and bytes so far (0: none) */
  unsigned next_len;        /* the next read's length (WakeUpRX's count while a read is going; 0: none) */
  unsigned cap, len;        /* buffer size and bytes in it */
  uint8_t *buf;
} stream;

static stream streams[NSTREAM];
static int sock = -1;

/* The bulk IN pipe as USBDriver and a host controller run it (0.16.2):
   - one read at a time; its length is WakeUpRX's count (DeviceFS's
     OS_GBPB 4 passes its byte count as WakeUpRX's count too), cut to the
     free space in whole packets; a WakeUpRX while a read is going only sets
     the next read's length, which starts when the read ends;
   - the device sends each container as 512-byte packets, the last one
     short, and a zero-length packet after a container of 512n bytes;
   - a read ends at a short packet, or when it's full;
   - a packet bigger than the room left in the read is babble: the read
     fails (TransferInfo -1), the packet is lost, and no more reads start
     until ClearStall.
   Data from the simulator waits in rxq until a read takes it. */
#define RX_MPS 512
static uint8_t rxq[131072];
static unsigned rxq_len;
static unsigned rxq_head;          /* where its first byte is (moved down at each fill) */
static uint32_t rx_cont_left;       /* bytes of the IN container in hand still to send; 0 between containers */
static int rx_zlp_due;
unsigned usbemu_babbles;

static void
rx_reset (void)
{
  rxq_len = 0;
  rxq_head = 0;
  rx_cont_left = 0;
  rx_zlp_due = 0;
}
unsigned usbemu_transfers, usbemu_short;
static uint32_t container_left;     /* bytes of the current OUT container still to come */
static uint8_t hdr_bytes[4];
static unsigned hdr_have;

static void
set_count (const char *var, unsigned x)
{
  char v[12], t[12];
  int k = 0;
  do
    t[k++] = (char) ('0' + x % 10);
  while ((x /= 10));
  for (int j = 0; j < k; j++)
    v[j] = t[k - 1 - j];
  v[k] = 0;
  regs r = {{ (unsigned) var, (unsigned) v, (unsigned) k, 0, 0 }};
  swix_regs (0x24, &r);         /* OS_SetVarVal */
}

static unsigned usbemu_missing_zlp;

/* Follow the containers in what goes out. A transfer ends with a short
   packet if it isn't a whole number of 512-byte packets, or with a
   zero-length packet if the stream's 'short' flag is set; either tells the
   device the container has ended. Count those that come too soon
   (PDFSEmu$ShortPackets), and containers that end on a packet boundary
   with nothing to say so (PDFSEmu$MissingZLP). */
static void
check_transfer (const uint8_t *p, unsigned n, int short_flag)
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
  int ends = (n & 511) || short_flag;
  int inside = container_left || hdr_have;
  if (ends && inside)
    set_count ("PDFSEmu$ShortPackets", ++usbemu_short);
  if (!ends && !inside)
    set_count ("PDFSEmu$MissingZLP", ++usbemu_missing_zlp);
}

static void
log_line (const char *s)
{
  pd_log (s);
}

/* PDFSEmu$LogFile set: every log line (and the trace) to that file as it
   happens, for a test that hangs before *PDFSLog can be asked */
extern void (*pd_log_tap) (const char *s);
static int tap_handle = -1;

static void
tap (const char *s)
{
  if (tap_handle <= 0)
    return;
  unsigned n = 0;
  while (s[n])
    n++;
  regs w = {{ 2, (unsigned) tap_handle, (unsigned) s, n }};
  swix_regs (0x0C, &w);                   /* OS_GBPB 2 */
  regs c = {{ 2, (unsigned) tap_handle, (unsigned) "\n", 1 }};
  swix_regs (0x0C, &c);
  regs fl = {{ 255, (unsigned) tap_handle }};
  swix_regs (0x09, &fl);                  /* OS_Args 255: to the disc now */
}

static void
tap_open (void)
{
  if (tap_handle >= 0)
    return;
  tap_handle = 0;
  char name[256];
  regs v = {{ (unsigned) "PDFSEmu$LogFile", (unsigned) name, sizeof name - 1, 0, 3 }};
  if (swix_regs (0x23, &v))               /* OS_ReadVarVal */
    return;
  name[v.r[2]] = 0;
  regs f = {{ 0x83, (unsigned) name }};   /* OS_Find: open out */
  if (swix_regs (0x0D, &f) || !f.r[0])
    return;
  tap_handle = (int) f.r[0];
  pd_log_tap = tap;
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
  rx_reset ();
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

/* PDFSEmu$Phone "fire": an Amazon Fire tablet (VID 1949), whose MTP
   interface is Android's older vendor-specific one (FF/FF/00), with an ADB
   interface (FF/42/01) that mustn't be taken for MTP */
static const uint8_t fire[] = {
  18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x49, 0x19, 0x81, 0x05, 0x00, 0x02, 1, 2, 3, 1,
  9, 2, 62, 0, 2, 1, 0, 0x80, 250,
  9, 4, 0, 0, 3, 0xFF, 0xFF, 0x00, 4,  /* MTP, vendor-specific */
  7, 5, 0x81, 2, 0x00, 0x02, 0,
  7, 5, 0x01, 2, 0x00, 0x02, 0,
  7, 5, 0x82, 3, 28, 0, 6,
  9, 4, 1, 0, 2, 0xFF, 0x42, 0x01, 5,  /* ADB */
  7, 5, 0x83, 2, 0x00, 0x02, 0,
  7, 5, 0x02, 2, 0x00, 0x02, 0,
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

/* --- the charging scenario ---------------------------------------------------------------

   PDFSEmu$Scenario "charging": the phone is plugged in set to charging only.
   It enumerates with its MTP interface (as "USB9"), but takes nothing in
   (a command stays in the output buffer: the pipe is NAKed) and answers
   nothing. PDFSEmu$SwitchAfter centiseconds (default 300) after the first
   command written to it, it is switched to file transfer: it drops off the
   bus at once (its transfers fail; a command still in the output buffer is
   a failed transfer), Service_DeviceDead comes PDFSEmu$DeadAfter cs later
   (default 50), for USB9 and then two short-lived enumerations (USB10,
   USB11), and PDFSEmu$BackAfter cs after that (default 150) it is back as
   "USB12", in MTP mode (bridged to the simulator), with Service_USB 0
   (Attach). "charging-only": it is never switched. "reconnect": the phone
   answers from the start (bridged), and drops off and comes back the same
   way PDFSEmu$SwitchAfter cs after the first command, as a Fire tablet
   does after its owner taps Allow.

   USB's data toggle is modelled for the output pipes: a pipe opened again
   starts at DATA0 while the device carries on, until ClearStall; out of
   step, the device drops the next packet (a hazard).

   The events come asynchronously, as USBDriver's do: from a transient
   callback (OS_CallAfter, then OS_AddCallBack), or, while PDFS is waiting on
   the device (its BufferSpace, TransferInfo and WakeUp calls), there and
   then, as the Pi's logs show Service_DeviceDead arriving in the middle of
   PDFS's waits.

   USBDriver's hazards, as seen on the Pi, are counted in PDFSEmu$Hazards
   and logged ("usbemu: HAZARD ..."):
   - a close that succeeds for an output stream whose transfer failed, on
     a device that has gone (0.15 tests 2-4 and 7-8: data aborts in
     USBDriver, usbd_free_xfer / usbd_free_buffer);
   - any call on a stream after its device's Service_DeviceDead has been
     and gone (DeviceFS has freed its records: "Device not found" at
     *ShutDown, or an abort once the memory is reused);
   - a control request (ClearStall, class requests) on a device that has
     gone: USBDriver makes them synchronously;
   - a close of an output stream whose transfer the phone, set to
     charging, hasn't taken (it's on the bus, NAKed; emptying the buffer
     doesn't stop it): DeviceFS's close waits for it, for ever (0.15 tests
     9 and 10 stopped the Pi in !IrisRAM's banner until it was unplugged).
   A close of an output stream with a failed transfer still in its buffer
   fails ("USB transfer failed: IO Error") and the stream is kept, as
   DeviceFS and FileSwitch do; PDFSEmu$Leftover counts those (each is a
   "Device not found" at *ShutDown). */

#define MAX_GEN 4
static const struct { int n; char m[20]; } err_notfound = { 0x100C6, "Device not found" };
static int scenario = -1;        /* -1: not read yet; 0 normal; 1 charging; 2 charging only */
static int phone_gen;            /* 0: charging (USB9); 1: file transfer (USB12) */
static int phone_present = 1;    /* listed by Service_USB 1 */
static unsigned gone_at[MAX_GEN];   /* off the bus since (0: still there) */
static int dead_done[MAX_GEN];   /* its Service_DeviceDead has been */
static unsigned hazards, leftovers;
static unsigned switch_after = 300, dead_after = 50, back_after = 150;
static unsigned next_event_at;   /* monotonic time, 0: none */
static int next_event;           /* 1 switch, 2 dead, 3 back */
static int switch_armed;
static unsigned dev_toggle[MAX_GEN][16];  /* device side: OUT packets taken since its last ClearStall */
static int delivering;

static const char gen_name[MAX_GEN][6] = { "USB9", "USB12", "USB13", "USB14" };

static unsigned
emu_now (void)
{
  regs r = {{ 0 }};
  swix_regs (0x42, &r);
  return r.r[0];
}

static int
str_is (const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static unsigned
read_num (const char *var, unsigned dflt)
{
  char b[16];
  regs r = {{ (unsigned) var, (unsigned) b, sizeof b - 1, 0, 0 }};
  if (swix_regs (0x23, &r))
    return dflt;
  unsigned v = 0;
  for (unsigned i = 0; i < r.r[2] && b[i] >= '0' && b[i] <= '9'; i++)
    v = v * 10 + (unsigned) (b[i] - '0');
  return v;
}

/* PDFSEmu$InHaltAfter N: the Nth input transfer fails on the host's side
   (its TransferInfo says failed, and nothing more comes in) while the
   device holds its answer, until ClearStall */
static unsigned in_delivered, in_halt_after, in_halt_done, in_halt_drop;

static void
read_scenario (void)
{
  if (scenario >= 0)
    return;
  char b[24];
  regs r = {{ (unsigned) "PDFSEmu$Scenario", (unsigned) b, sizeof b - 1, 0, 0 }};
  scenario = 0;
  if (!swix_regs (0x23, &r))
    {
      b[r.r[2]] = 0;
      if (str_is (b, "charging"))
        scenario = 1;
      else if (str_is (b, "charging-only"))
        scenario = 2;
      else if (str_is (b, "reconnect"))
        scenario = 3;
    }
  in_halt_after = read_num ("PDFSEmu$InHaltAfter", 0);
  /* PDFSEmu$InHaltDrop 1: the answer held is lost when the pipe is cleared
     (as an Amazon Fire's was) */
  in_halt_drop = read_num ("PDFSEmu$InHaltDrop", 0);
  switch_after = read_num ("PDFSEmu$SwitchAfter", switch_after);
  dead_after = read_num ("PDFSEmu$DeadAfter", dead_after);
  back_after = read_num ("PDFSEmu$BackAfter", back_after);
}

static int
charging_gen (int gen)
{
  read_scenario ();
  return (scenario == 1 || scenario == 2) && gen == 0;
}

static void
hazard (const char *what)
{
  char m[200] = "usbemu: HAZARD ";
  unsigned k = 15;
  for (const char *q = what; *q && k < sizeof m - 1; q++)
    m[k++] = *q;
  m[k] = 0;
  pd_log (m);
  set_count ("PDFSEmu$Hazards", ++hazards);
}

static void
hazard_note (const char *what)
{
  char m[200] = "usbemu: note: ";
  unsigned k = 14;
  for (const char *q = what; *q && k < sizeof m - 1; q++)
    m[k++] = *q;
  m[k] = 0;
  pd_log (m);
}

/* the device of this name is the phone as it is now, and can be opened */
static int
phone_open_ok (const char *name)
{
  read_scenario ();
  const char *want = gen_name[phone_gen];
  int i = 0;
  while (want[i] && name[i] == want[i])
    i++;
  return !want[i] && (name[i] < '0' || name[i] > '9') && phone_present;
}

extern void usbemu_after (void);    /* the veneers, below */

static void
schedule (int event, unsigned cs)
{
  next_event = event;
  next_event_at = (emu_now () + cs) | 1;
  regs r = {{ cs ? cs : 1, (unsigned) usbemu_after, 0 }};
  swix_regs (0x3B, &r);             /* OS_CallAfter */
}

static void service_dead (const char *name)
{
  regs r = {{ 0, 0x79, 0x9999, (unsigned) name }};
  swix_regs (0x30, &r);             /* OS_ServiceCall */
}

/* Run the next event if it's due. From the callback, or from PDFS's own
   calls while it waits on the device. */
static void
run_events (void)
{
  if (delivering || !next_event_at || (int) (emu_now () - next_event_at) < 0)
    return;
  delivering = 1;
  int ev = next_event;
  next_event = 0;
  next_event_at = 0;
  switch (ev)
    {
    case 1:                         /* switched: off the bus */
      pd_log (scenario == 3 ? "usbemu: the device drops off the bus, to come back (as after Allow on a Fire)"
                            : "usbemu: the phone is switched to file transfer: it drops off the bus");
      if (sock >= 0)
        {
          regs cl = {{ (unsigned) sock }};
          swix_regs (Socket_Close, &cl);
          sock = -1;
          rx_reset ();
        }
      gone_at[phone_gen] = emu_now () | 1;
      phone_present = 0;
      for (int i = 0; i < NSTREAM; i++)
        if (streams[i].used && streams[i].gen == phone_gen && streams[i].output
            && (streams[i].len || streams[i].inflight))
          streams[i].failed = 1;
      schedule (2, dead_after);
      break;
    case 2:                         /* USBDriver detaches it */
      pd_log ("usbemu: Service_DeviceDead USB9 (and USB10, USB11)");
      service_dead (gen_name[phone_gen]);
      dead_done[phone_gen] = 1;
      service_dead ("USB10");
      service_dead ("USB11");
      schedule (3, back_after);
      break;
    case 3:                         /* back, in MTP mode */
      phone_gen = 1;
      phone_present = 1;
      pd_log ("usbemu: the phone is back as USB12 (file transfer): Service_USB 0");
      {
        regs r = {{ 0, 0xD2, 0 }};
        swix_regs (0x30, &r);
      }
      break;
    }
  delivering = 0;
}

/* The callback (from OS_CallAfter's handler) */
void
usbemu_event (void)
{
  run_events ();
}

__asm__ (
"	.text\n"
"	.global	usbemu_after\n"
"usbemu_after:\n"                   /* OS_CallAfter: SVC mode, IRQs off */
"	stmfd	sp!, {r0-r1, lr}\n"
"	adr	r0, usbemu_cb\n"
"	mov	r1, r12\n"
"	swi	0x20054\n"                  /* XOS_AddCallBack */
"	ldmfd	sp!, {r0-r1, pc}\n"
"usbemu_cb:\n"
"	stmfd	sp!, {r0-r12, lr}\n"
"	bl	usbemu_event\n"
"	ldmfd	sp!, {r0-r12, pc}\n");

/* A call PDFS makes on a stream: the device's state */
static int
stream_dead (stream *s, const char *what)
{
  if (dead_done[s->gen])
    {
      hazard (what);
      return 1;
    }
  return 0;
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
  if (!phone_open_ok (colon + 1))
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
        s->close_failed = 0;
        s->purged = 0;
        s->flags = 0;
        s->output = output;
        s->len = 0;
        s->gen = phone_gen;
        s->failed = 0;
        s->inflight = 0;
        s->toggle = 0;
        s->halted = 0;
        s->rd_len = s->rd_got = s->next_len = 0;
        if (gone_at[phone_gen])
          hazard_note ("a stream opened on a device that has gone (before its Service_DeviceDead)");
        if (s->ep == 1 && !output && !charging_gen (s->gen) && connect_sim ())
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

/* Socket_Recv without waiting: the test bed's Socket_Select can say a
   socket is readable after its data has been taken (a stale SIGIO), and a
   blocking recv then waits for ever (the simulator waiting for the next
   command): seen as hangs in 0.16.2's reads */
static int
recv_now (void *dst, unsigned n)
{
  unsigned on = 1, off = 0;
  regs a = {{ (unsigned) sock, 0x8004667Eu /* FIONBIO */, (unsigned) &on }};
  swix_regs (Socket_Ioctl, &a);
  regs r = {{ (unsigned) sock, (unsigned) dst, n, 0 }};
  int k = swix_regs (Socket_Recv, &r) ? -1 : (int) r.r[0];
  regs b = {{ (unsigned) sock, 0x8004667Eu, (unsigned) &off }};
  swix_regs (Socket_Ioctl, &b);
  return k;
}

/* What the simulator has sent: into rxq */
static void
rx_fill (void)
{
  if (rxq_head)
    {
      memmove (rxq, rxq + rxq_head, rxq_len);
      rxq_head = 0;
    }
  if (sock < 0 || rxq_len >= sizeof rxq)
    return;
  uint32_t set[8] = { 0 };
  set[sock / 32] |= 1u << (sock % 32);
  uint32_t tv[2] = { 0, 1000 };
  regs sel = {{ (unsigned) sock + 1, (unsigned) set, 0, 0, (unsigned) tv }};
  if (swix_regs (Socket_Select, &sel) || sel.r[0] == 0)
    {
      static unsigned quiet;
      if (pd_log_tap && emu_now () - quiet > 300)
        {
          quiet = emu_now ();
          char m[120], *q = m;
          const char *t = "usbemu: nothing more from the simulator; rxq ";
          while (*t)
            *q++ = *t++;
          unsigned v = rxq_len; char d[12]; int k = 0;
          do d[k++] = (char) ('0' + v % 10); while (v /= 10);
          while (k) *q++ = d[--k];
          t = ", cont_left ";
          while (*t) *q++ = *t++;
          v = rx_cont_left; k = 0;
          do d[k++] = (char) ('0' + v % 10); while (v /= 10);
          while (k) *q++ = d[--k];
          *q = 0;
          log_line (m);
        }
      return;
    }
  int k = recv_now (rxq + rxq_len, sizeof rxq - rxq_len);
  if (k > 0)
    rxq_len += (unsigned) k;
}

/* The size of the next packet the device would send, or -1 if it hasn't
   all arrived yet */
static int
rx_packet (void)
{
  if (rx_zlp_due)
    return 0;
  if (!rx_cont_left)
    {
      if (rxq_len < 4)
        return -1;
      const uint8_t *h = rxq + rxq_head;
      uint32_t len = h[0] | h[1] << 8 | h[2] << 16 | (uint32_t) h[3] << 24;
      if (len < 12 || len > 0x7FFFFFFF)
        len = rxq_len;            /* not a container start (out of step): as it comes */
      rx_cont_left = len;
    }
  unsigned pkt = rx_cont_left < RX_MPS ? rx_cont_left : RX_MPS;
  return rxq_len >= pkt ? (int) pkt : -1;
}

static void
rx_consume (unsigned pkt, uint8_t *dst)
{
  if (rx_zlp_due && !pkt)
    {
      rx_zlp_due = 0;
      return;
    }
  if (dst)
    memcpy (dst, rxq + rxq_head, pkt);
  rxq_head += pkt;
  rxq_len -= pkt;
  if (!rxq_len)
    rxq_head = 0;
  rx_cont_left -= pkt;
  if (!rx_cont_left && pkt == RX_MPS)
    rx_zlp_due = 1;
}

/* Time passes on the bulk IN pipe: the read in progress takes what has come */
static void
rx_pump (stream *s)
{
  if (sock < 0 || s->ep != 1 || s->output || s->halted || gone_at[s->gen] || charging_gen (s->gen))
    return;
  for (;;)
    {
      if (!s->rd_len)
        {
          if (!s->next_len)
            return;
          unsigned n = (s->cap - s->len) & ~(RX_MPS - 1);
          if (n > s->next_len)
            n = s->next_len;
          s->next_len = 0;
          if (!n)
            return;
          read_scenario ();
          if (in_halt_after && !in_halt_done && in_delivered + 1 >= in_halt_after)
            {
              in_halt_done = 1;
              s->halted = 1;
              log_line ("usbemu: the input pipe fails on the host's side (the device holds its answer)");
              return;
            }
          s->rd_len = n;
          s->rd_got = 0;
        }
      rx_fill ();
      int pkt = rx_packet ();
      if (pkt < 0)
        return;
      if ((unsigned) pkt > s->rd_len - s->rd_got)
        {
          /* babble: the packet doesn't fit the read: lost, and the pipe stops */
          char m[120];
          char *q = m;
          const char *t = "usbemu: babble: a read with ";
          while (*t)
            *q++ = *t++;
          unsigned room = s->rd_len - s->rd_got, v = room;
          char d[12];
          int k = 0;
          do d[k++] = (char) ('0' + v % 10); while (v /= 10);
          while (k)
            *q++ = d[--k];
          t = " bytes of room got a bigger packet: lost, the pipe stops (TransferInfo -1)";
          while (*t)
            *q++ = *t++;
          *q = 0;
          log_line (m);
          rx_consume ((unsigned) pkt, 0);
          s->halted = 1;
          s->rd_len = 0;
          set_count ("PDFSEmu$Babbles", ++usbemu_babbles);
          return;
        }
      rx_consume ((unsigned) pkt, s->buf + s->len);
      s->len += (unsigned) pkt;
      s->rd_got += (unsigned) pkt;
      if (pkt < RX_MPS || s->rd_got == s->rd_len)
        {
          s->rd_len = 0;
          in_delivered++;
        }
    }
}

/* WakeUpRX with a count (USBDriver: the next read's length) */
static void
rx_wake (stream *s, unsigned count)
{
  if (!count || s->ep != 1 || s->output || s->halted)
    return;
  s->next_len = count;
  rx_pump (s);
}

/* The buffer manager on a fake stream's buffer (buffer handle = handle +
   0x200): what's there, and taking it, without starting a USB read (as
   PDFS 0.16.2 does it; OS_GBPB 4 starts one) */
static void
put_num (char **q, const char *label, unsigned v)
{
  while (*label)
    *(*q)++ = *label++;
  char d[12];
  int k = 0;
  do d[k++] = (char) ('0' + v % 10); while (v /= 10);
  while (k)
    *(*q)++ = d[--k];
}

unsigned
usbemu_used (unsigned buffer)
{
  stream *s = fake (buffer - 0x200);
  if (!s)
    return 0;
  rx_pump (s);
  static unsigned last_state;
  if (pd_log_tap && emu_now () - last_state > 200)
    {
      /* (with PDFSEmu$LogFile: what the pipe is doing while PDFS waits) */
      char m[200], *q = m;
      last_state = emu_now ();
      put_num (&q, "usbemu: waiting: rd_len ", s->rd_len);
      put_num (&q, " rd_got ", s->rd_got);
      put_num (&q, " next_len ", s->next_len);
      put_num (&q, " halted ", (unsigned) s->halted);
      put_num (&q, " rxq ", rxq_len);
      put_num (&q, " cont_left ", rx_cont_left);
      put_num (&q, " zlp_due ", (unsigned) rx_zlp_due);
      put_num (&q, " sock ", (unsigned) sock);
      put_num (&q, " len ", s->len);
      *q = 0;
      log_line (m);
    }
  return s->len;
}

unsigned
usbemu_take (unsigned buffer, void *dst, unsigned n)
{
  stream *s = fake (buffer - 0x200);
  if (!s)
    return 0;
  unsigned k = n < s->len ? n : s->len;
  memcpy (dst, s->buf, k);
  memmove (s->buf, s->buf + k, s->len - k);
  s->len -= k;
  return k;
}

/* --- the SWIs ---------------------------------------------------------------------------- */

oserror *
usbemu_swix (unsigned swi, regs *r)
{
  stream *s;
  tap_open ();
  switch (swi)
    {
    case 0x54A45:                                /* USBDriver_Version */
      {
        /* PDFSEmu$OldDriver set: a USBDriver before 0.49 (no 'short') */
        regs v = {{ (unsigned) "PDFSEmu$OldDriver", 0, (unsigned) -1, 0, 0 }};
        swix_regs (0x23, &v);                    /* OS_ReadVarVal: R2 < 0 if set */
        r->r[0] = (int) v.r[2] < 0 ? 48 : 131;
        return 0;
      }

    case OS_ServiceCall:
      if (r->r[1] == 0xD2 && r->r[0] == 1)
        {
          oserror *e = swix_regs (swi, r);       /* anything real first */
          uint8_t *list = (uint8_t *) r->r[2];
          read_scenario ();
          list = make_record (list, "USB8", 2, stick, sizeof stick);
          if (phone_present)
            {
              char b[8];
              regs v = {{ (unsigned) "PDFSEmu$Phone", (unsigned) b, sizeof b - 1, 0, 0 }};
              int is_fire = !swix_regs (0x23, &v) && v.r[2] == 4 && b[0] == 'f';
              list = is_fire ? make_record (list, gen_name[phone_gen], 3 + phone_gen, fire, sizeof fire)
                             : make_record (list, gen_name[phone_gen], 3 + phone_gen, phone, sizeof phone);
            }
          r->r[2] = (unsigned) list;
          return e;
        }
      break;

    case OS_Find:
      if (r->r[0] == 0 && (s = fake (r->r[1])))
        {
          static const struct { int n; char m[32]; } ioerr = { 0x819C00, "USB transfer failed: IO Error" };
          if (stream_dead (s, "a stream closed after its device's Service_DeviceDead: DeviceFS's records are freed (Device not found, or an abort)"))
            return (oserror *) &err_notfound;
          if (!gone_at[s->gen] && charging_gen (s->gen) && s->output && (s->len || s->inflight))
            hazard ("a close of a stream whose transfer the phone set to charging hasn't taken (NAKed on the bus): DeviceFS's close waits for it, for ever (the desktop hangs until the phone is unplugged)");
          if (gone_at[s->gen] && s->output && s->failed)
            {
              if (s->len || s->inflight)
                {
                  /* DeviceFS's flush gets the failed transfer: the close
                     fails, and FileSwitch keeps the stream */
                  if (!s->close_failed)
                    set_count ("PDFSEmu$Leftover", ++leftovers);
                  s->close_failed = 1;
                  return (oserror *) &ioerr;
                }
              hazard ("a close that succeeded for an output stream whose transfer failed, on a phone that has gone: USBDriver frees the transfer of a freed device (usbd_free_xfer/usbd_free_buffer abort)");
            }
          if (s->close_failed && gone_at[s->gen])
            set_count ("PDFSEmu$Leftover", --leftovers);
          /* PDFSEmu$CloseFails set: an output stream's first close fails, as
             FileSwitch's does for a written-to DeviceFS stream whose device
             has just gone (the stream stays open) */
          regs v = {{ (unsigned) "PDFSEmu$CloseFails", 0, (unsigned) -1, 0, 0 }};
          swix_regs (0x23, &v);
          if ((int) v.r[2] < 0 && s->output && !s->close_failed && !s->purged)
            {
              static const struct { int n; char m[32]; } err = { 0x819C00, "USB transfer failed: IO Error" };
              s->close_failed = 1;
              return (oserror *) &err;
            }
          pd_free (s->buf);
          s->used = 0;
          int open = 0;
          for (int i = 0; i < NSTREAM; i++)
            open += streams[i].used;
          set_count ("PDFSEmu$Open", (unsigned) open);
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

    case 0x09:                                   /* OS_Args 254: a fake is on DeviceFS (53) */
      if (r->r[0] == 254 && (s = fake (r->r[1])))
        {
          r->r[0] = 0;
          r->r[2] = 53;
          return 0;
        }
      break;

    case OS_GBPB:
      if ((s = fake (r->r[1])))
        {
          if (stream_dead (s, "OS_GBPB on a stream after its device's Service_DeviceDead"))
            return (oserror *) &err_notfound;
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
              /* DeviceFS's gbpb_get calls WakeUpRX with the byte count
                 first: USBDriver takes it as the next read's length */
              rx_wake (s, n);
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
          run_events ();
          if ((s = fake (r->r[2])))
            {
              if (stream_dead (s, "BufferSpace on a stream after its device's Service_DeviceDead"))
                return (oserror *) &err_notfound;
              rx_pump (s);
              r->r[3] = s->cap;
              r->r[4] = s->cap - s->len;
              return 0;
            }
          break;
        case 0x80000007u:                        /* GetHandles2 */
          if ((s = fake (r->r[2])))
            {
              r->r[3] = r->r[2] + 0x200;         /* the "buffer handle" */
              r->r[4] = 0;
              r->r[5] = r->r[2] + 0x100;
              r->r[6] = 0x9999;
              return 0;
            }
          break;
        case 0x80000006u:                        /* TransferInfo */
          run_events ();
          r->r[3] = 1;                           /* done */
          if ((s = fake_stream (r->r[2])))
            {
              if (!s->output)
                rx_pump (s);
              if (s->failed || s->halted)
                r->r[3] = (unsigned) -1;         /* failed */
              else if (charging_gen (s->gen) && (s->len || s->inflight))
                r->r[3] = 0;                     /* NAKed: still going */
              else if (!s->output && (s->rd_len || s->next_len))
                r->r[3] = 0;                     /* a read going, or one to come */
            }
          return 0;
        case 13:                                 /* MonitorRX: why the transfer failed */
          if ((s = fake_stream (r->r[2])))
            {
              static const struct { int n; char m[40]; } err = { 0x819C0D, "USB transfer failed: IO Error" };
              if (s->halted || s->failed)
                return (oserror *) &err;
              return 0;
            }
          break;
        case 0x80000008u:                        /* Get/Set Options */
          if ((s = fake_stream (r->r[2])))
            {
              unsigned old = s->flags;
              s->flags = ((s->flags & r->r[4]) ^ r->r[3]) & 3;
              r->r[3] = old;
              r->r[4] = s->flags;
              return 0;
            }
          break;
        case 0x80000005u:                        /* ClearStall */
        case 0x80000000u:                        /* control request */
          log_line (r->r[0] == 0x80000000u ? "usbemu: control request" : "usbemu: ClearStall");
          if (r->r[0] == 0x80000005u && (s = fake (r->r[2])) && phone_present)
            {
              /* (toggles are per endpoint and direction: only the
                 output pipe's are modelled) */
              if (s->output)
                dev_toggle[s->gen][s->ep & 15] = 0;
              s->toggle = 0;
              if (!s->output)
                {
                  s->rd_len = 0;
                  s->next_len = 0;
                }
              if (s->halted && in_halt_drop)
                {
                  rx_reset ();
                  /* the device's answer is lost: what the bridge has (or
                     gets in the next 50 ms) is dropped */
                  uint8_t junk[512];
                  unsigned dropped = 0;
                  for (int tries = 0; tries < 25; tries++)
                    {
                      uint32_t set[8] = { 0 };
                      set[sock / 32] |= 1u << (sock % 32);
                      uint32_t tv[2] = { 0, 2000 };
                      regs sel = {{ (unsigned) sock + 1, (unsigned) set, 0, 0, (unsigned) tv }};
                      if (swix_regs (Socket_Select, &sel) || sel.r[0] == 0)
                        continue;
                      int k = recv_now (junk, sizeof junk);
                      if (k <= 0)
                        continue;
                      dropped += (unsigned) k;
                    }
                  log_line (dropped ? "usbemu: ClearStall on the failed input pipe: the answer it held is lost"
                                    : "usbemu: ClearStall on the failed input pipe (nothing held)");
                }
              else if (s->halted)
                log_line ("usbemu: ClearStall on the failed input pipe: it carries on");
              s->halted = 0;
            }
          read_scenario ();
          if (scenario > 0 && (const char *) r->r[1] && !phone_present)
            hazard ("a control request (synchronous in USBDriver) on a phone that has gone");
          return 0;
        case 2:                                  /* WakeUpTX: one transfer */
          run_events ();
          if ((s = fake_stream (r->r[2])) && s->output && s->len
              && (gone_at[s->gen] || charging_gen (s->gen)))
            {
              if (gone_at[s->gen])
                s->failed = 1;                   /* off the bus */
              else
                s->inflight = 1;                 /* on the bus, NAKed: purging the buffer doesn't stop it */
              if (!gone_at[s->gen] && scenario == 1 && !switch_armed)
                {
                  /* charging only: the pipe is NAKed; the owner switches
                     the phone to file transfer a little later */
                  switch_armed = 1;
                  pd_log ("usbemu: a command for the phone set to charging: it isn't taken");
                  schedule (1, switch_after);
                }
              return 0;
            }
          if ((s = fake_stream (r->r[2])) && s->output && s->len)
            {
              if (scenario == 3 && !switch_armed)
                {
                  switch_armed = 1;
                  schedule (1, switch_after);
                }
              usbemu_transfers++;
              unsigned packets = (s->len + 511) / 512 + ((s->len & 511) == 0 && (s->flags & 2) ? 1 : 0);
              unsigned *dt = &dev_toggle[s->gen][s->ep & 15];
              if ((s->toggle ^ *dt) & 1)
                {
                  /* USB's data toggle: a pipe opened again starts at DATA0,
                     but the device carries on where it was (only ClearStall
                     resets it). It takes the first packet for a repeat,
                     ACKs it and drops it. */
                  hazard ("the data toggle was out of step (the pipe was opened again without ClearStall): the device dropped a packet");
                  if (s->len > 512)
                    send_all (s->buf + 512, s->len - 512);
                  *dt += packets - 1;
                }
              else
                {
                  check_transfer (s->buf, s->len, (s->flags & 2) != 0);
                  send_all (s->buf, s->len);
                  *dt += packets;
                }
              s->toggle += packets;
              s->len = 0;
            }
          return 0;
        case 3:                                  /* WakeUpRX */
          run_events ();
          if ((s = fake_stream (r->r[2])) && !gone_at[s->gen] && !charging_gen (s->gen))
            rx_wake (s, r->r[3]);
          return 0;
        }
      break;
    }
  return swix_regs (swi, r);
}

/* CnpV purge of a stream's buffer ("buffer handle" = handle + 0x200): with
   nothing left to send, a CloseFails stream closes */
void
usbemu_purge (unsigned buffer)
{
  stream *s = fake (buffer - 0x200);
  if (s)
    {
      s->purged = 1;
      s->len = 0;
    }
}
