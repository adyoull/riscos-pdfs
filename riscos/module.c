/* module.c: the PDFS module for RISC OS: platform functions for the core,
   *commands, and (for testing) a TCP transport to the MTP simulator.
   The USB transport for real devices comes next (see docs/DESIGN.md).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "cmodule.h"
#include "../core/pdfs.h"
#include "../core/plat.h"
#include "filer.h"
#include "usb.h"

#define OS_Word                 0x07
#define OS_FSControl            0x29
#define OS_ReadMonotonicTime    0x42
#define XReport_Text0           (0x20000 | 0x54C80)
#define OS_SWINumberFromString  0x39
#define OS_AddCallBack          0x54
#define OS_RemoveCallBack       0x5F
#define Wimp_StartTask          0x400DE
#define Wimp_ReadSysInfo        0x400F2
#define ResourceFS_RegisterFiles   0x41B40
#define ResourceFS_DeregisterFiles 0x41B41
#define Socket_Creat            0x41200
#define Socket_Connect          0x41204
#define Socket_Recv             0x41205
#define Socket_Send             0x41208
#define Socket_Close            0x41210
#define Socket_Select           0x41211

extern char module_start[], fs_info_block[];
static void *private_word;
static int callback_set;
void pd_callback (void);        /* header.s */


/* in header.s */
extern const char resource_files[];
void call_resourcefs (void *code, void *ws, const void *files);


/* --- memory ---------------------------------------------------------------------- */

/* Blocks carry their size in front, for pd_realloc */
void *
pd_alloc (size_t n)
{
  uint32_t *p = rma_claim (n + 8);
  if (!p)
    return 0;
  p[0] = (uint32_t) n;
  memset (p + 2, 0, n);
  return p + 2;
}

void
pd_free (void *p)
{
  if (p)
    rma_free ((uint32_t *) p - 2);
}

void *
pd_realloc (void *p, size_t n)
{
  if (!p)
    return pd_alloc (n);
  size_t old = ((uint32_t *) p)[-2];
  void *q = pd_alloc (n);
  if (!q)
    return 0;
  memcpy (q, p, old < n ? old : n);
  pd_free (p);
  return q;
}

/* --- time -------------------------------------------------------------------------- */

unsigned
pd_time_cs (void)
{
  regs r = {{ 0 }};
  swix_regs (OS_ReadMonotonicTime, &r);
  return r.r[0];
}

uint32_t
pd_unix_time (void)
{
  uint8_t b[8] = { 3 };
  regs r = {{ 14, (unsigned) b }};
  if (swix_regs (OS_Word, &r))
    return 0;
  uint64_t cs = b[0] | (uint64_t) b[1] << 8 | (uint64_t) b[2] << 16 | (uint64_t) b[3] << 24
                | (uint64_t) b[4] << 32;
  return (uint32_t) (cs / 100 - 2208988800u);
}

/* --- log ------------------------------------------------------------------------------- */

#define LOGSIZE 8192
static char logbuf[LOGSIZE];
static unsigned logpos;           /* total bytes ever written */
static int logging = 1;

static void
log_chars (const char *s)
{
  while (*s)
    logbuf[logpos++ % LOGSIZE] = *s++;
}

void
pd_log (const char *s)
{
  if (!logging)
    return;
  log_chars (s);
  log_chars ("\n");
  if (pdfs_trace)                   /* also to Reporter (the test bed: RISCOS_REPORT) */
    {
      regs r = { { 0 } };
      r.r[0] = (unsigned) s;
      swix_regs (XReport_Text0, &r);
    }
}

void
pd_log_hex (const char *what, uint32_t v)
{
  char h[12] = "&";
  for (int i = 0; i < 8; i++)
    h[1 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
  h[9] = 0;
  if (!logging)
    return;
  log_chars (what);
  log_chars (" ");
  log_chars (h);
  log_chars ("\n");
}

static void
show_log (void)
{
  unsigned start = logpos > LOGSIZE ? logpos - LOGSIZE : 0;
  for (unsigned i = start; i < logpos; i++)
    {
      char c = logbuf[i % LOGSIZE];
      if (c == '\n')
        newline ();
      else
        {
          regs r = {{ (unsigned) (unsigned char) c }};
          swix_regs (OS_WriteC, &r);
        }
    }
}

/* --- filetypes ---------------------------------------------------------------------- */

static unsigned mimemap_translate;

/* The names are arrays, not pointers: a table of pointers would need
   relocating, and only the GOT is relocated when the module starts. */
static const struct { char ext[8]; int type; } fallback[] = {
  { "txt", 0xFFF }, { "pdf", 0xADF }, { "jpg", 0xC85 }, { "jpeg", 0xC85 }, { "png", 0xB60 },
  { "gif", 0x695 }, { "zip", 0xA91 }, { "htm", 0xFAF }, { "html", 0xFAF }, { "csv", 0xDFE },
  { "mp3", 0x1AD }, { "epub", 0xBE8 }, { "", 0 } };

static int
ext_eq (const char *a, const char *b)
{
  for (; *a && *b; a++, b++)
    {
      int x = *a | 0x20, y = *b | 0x20;
      if (x != y)
        return 0;
    }
  return *a == *b;
}

int
pd_ext_to_filetype (const char *ext)
{
  if (mimemap_translate)
    {
      char buf[128];
      regs r = {{ 3, (unsigned) ext, 0, (unsigned) buf }};
      /* MimeMap says Data for anything it doesn't know (and for everything
         when its mapping file isn't loaded): then try our own table */
      if (!swix_regs (mimemap_translate, &r) && (int) r.r[3] >= 0 && r.r[3] < 0xFFD)
        return (int) r.r[3];
    }
  for (int i = 0; fallback[i].ext[0]; i++)
    if (ext_eq (fallback[i].ext, ext))
      return fallback[i].type;
  return -1;
}

int
pd_filetype_to_ext (int type, char *buf)
{
  if (mimemap_translate)
    {
      char ext[64];
      regs r = {{ 0, (unsigned) type, 3, (unsigned) ext }};
      ext[0] = 0;
      if (!swix_regs (mimemap_translate, &r) && ext[0] > ' ')
        {
          int i = 0;
          for (; i < 15 && ext[i] > ' '; i++)
            buf[i] = ext[i];
          buf[i] = 0;
          /* MimeMap gives a type for this extension back? Then use it */
          if (pd_ext_to_filetype (buf) == type)
            return 1;
        }
    }
  for (int i = 0; fallback[i].ext[0]; i++)
    if (fallback[i].type == type)
      {
        memcpy (buf, fallback[i].ext, strlen (fallback[i].ext) + 1);
        return 1;
      }
  return 0;
}

/* --- scrap files (big files being written): <Wimp$ScrapDir>.PDFS ---------------------- */

#define OS_Args                 0x09
#define OS_GBPB                 0x0C
#define OS_Find                 0x0D
#define OS_File                 0x08

static struct { int handle; char name[16]; } spills[8];
static unsigned spill_seq;
static int spill_dir_ready;

static void
spill_path (char *o, const char *leaf)
{
  const char *d = "<Wimp$ScrapDir>.PDFS";
  while (*d)
    *o++ = *d++;
  if (leaf)
    {
      *o++ = '.';
      while (*leaf)
        *o++ = *leaf++;
    }
  *o = 0;
}

int
pd_spill_open (void)
{
  char path[64];
  if (!spill_dir_ready)
    {
      /* our directory in the scrap directory; files left in it by a crash
         or a reset are deleted (OS_FSControl 27: wipe, forced) */
      spill_path (path, 0);
      regs cd = {{ 8, (unsigned) path, 0, 0, 0 }};
      if (swix_regs (OS_File, &cd))
        return 0;
      spill_path (path, "*");
      regs w = {{ 27, (unsigned) path, 0, 2 }};
      swix_regs (OS_FSControl, &w);
      spill_dir_ready = 1;
    }
  for (int i = 0; i < 8; i++)
    if (!spills[i].handle)
      {
        char leaf[16] = "S", *q = leaf + 1;
        unsigned v = ++spill_seq;
        char t[12];
        int k = 0;
        do
          t[k++] = (char) ('0' + v % 10);
        while ((v /= 10));
        while (k)
          *q++ = t[--k];
        *q = 0;
        spill_path (path, leaf);
        regs c = {{ 11, (unsigned) path, 0xFFD, 0, 0, 0 }};    /* create, empty */
        if (swix_regs (OS_File, &c))
          return 0;
        regs o = {{ 0xCF, (unsigned) path }};                     /* open for update */
        if (swix_regs (OS_Find, &o) || !o.r[0])
          return 0;
        spills[i].handle = (int) o.r[0];
        for (int j = 0; j < 16; j++)
          spills[i].name[j] = leaf[j];
        return spills[i].handle;
      }
  return 0;
}

int
pd_spill_read (int h, uint64_t off, void *buf, uint32_t len)
{
  regs r = {{ 3, (unsigned) h, (unsigned) buf, len, (unsigned) off }};
  if (swix_regs (OS_GBPB, &r))
    return -1;
  return (int) (len - r.r[3]);
}

int
pd_spill_write (int h, uint64_t off, const void *buf, uint32_t len)
{
  regs r = {{ 1, (unsigned) h, (unsigned) buf, len, (unsigned) off }};
  return swix_regs (OS_GBPB, &r) || r.r[3] ? -1 : 0;
}

int
pd_spill_setext (int h, uint64_t ext)
{
  regs r = {{ 3, (unsigned) h, (unsigned) ext }};
  return swix_regs (OS_Args, &r) ? -1 : 0;
}

void
pd_spill_close (int h)
{
  regs c = {{ 0, (unsigned) h }};
  swix_regs (OS_Find, &c);
  for (int i = 0; i < 8; i++)
    if (spills[i].handle == h)
      {
        char path[64];
        spill_path (path, spills[i].name);
        regs d = {{ 6, (unsigned) path }};                        /* delete */
        swix_regs (OS_File, &d);
        spills[i].handle = 0;
      }
}

/* --- TCP transport (testing with sim/mtpsim.py) ----------------------------------------- */

typedef struct
{
  pd_transport t;
  int fd;
  uint32_t addr;                  /* network order bytes a.b.c.d in memory order */
  uint16_t port;
  char name[40];
} tcp;

ERROR_BLOCK (err_connect, 0x1FAE0, "Can't connect to the simulated device");
ERROR_BLOCK (err_syntax, 0x1FAE1, "Give an address as a.b.c.d:port");
ERROR_BLOCK (err_nomem, 0x1FAB8, "Not enough memory");
ERROR_BLOCK (err_desktop, 0x1FAE3, "Use *Desktop to start the PDFS filer");
ERROR_BLOCK (err_ext_syntax, 0x1FAE2, "Syntax: *PDFSExtensions [show|hide]");

static int
tcp_connect (tcp *c)
{
  regs r = {{ 2, 1, 0 }};                 /* AF_INET, SOCK_STREAM */
  if (swix_regs (Socket_Creat, &r))
    return -1;
  c->fd = (int) r.r[0];
  uint8_t sa[16];
  for (int style = 0; style < 2; style++)
    {
      memset (sa, 0, sizeof sa);
      if (style == 0)
        {
          sa[0] = 16;                     /* BSD 4.4: length, family */
          sa[1] = 2;
        }
      else
        sa[0] = 2;                        /* older: 16-bit family */
      sa[2] = (uint8_t) (c->port >> 8);
      sa[3] = (uint8_t) c->port;
      memcpy (sa + 4, &c->addr, 4);
      regs k = {{ (unsigned) c->fd, (unsigned) sa, 16 }};
      if (!swix_regs (Socket_Connect, &k))
        return 0;
    }
  regs cl = {{ (unsigned) c->fd }};
  swix_regs (Socket_Close, &cl);
  c->fd = -1;
  return -1;
}

static int
tcp_write (pd_transport *t, const void *buf, uint32_t len, unsigned timeout_cs)
{
  tcp *c = (tcp *) t;
  (void) timeout_cs;
  const uint8_t *p = buf;
  while (len)
    {
      regs r = {{ (unsigned) c->fd, (unsigned) p, len, 0 }};
      if (swix_regs (Socket_Send, &r))
        return PDT_IO;
      if ((int) r.r[0] <= 0)
        return PDT_GONE;
      p += r.r[0];
      len -= r.r[0];
    }
  return 0;
}

static int
tcp_read (pd_transport *t, void *buf, uint32_t max, unsigned timeout_cs)
{
  tcp *c = (tcp *) t;
  uint32_t set[8] = { 0 };
  set[c->fd / 32] |= 1u << (c->fd % 32);
  uint32_t tv[2] = { timeout_cs / 100, (timeout_cs % 100) * 10000 };
  regs s = {{ (unsigned) c->fd + 1, (unsigned) set, 0, 0, (unsigned) tv }};
  if (swix_regs (Socket_Select, &s))
    return PDT_IO;
  if (s.r[0] == 0)
    return PDT_TIMEOUT;
  regs r = {{ (unsigned) c->fd, (unsigned) buf, max, 0 }};
  if (swix_regs (Socket_Recv, &r))
    return PDT_IO;
  if (r.r[0] == 0)
    return PDT_GONE;
  return (int) r.r[0];
}

static int
tcp_reset (pd_transport *t, uint32_t tid, int level)
{
  tcp *c = (tcp *) t;
  (void) tid;
  (void) level;
  regs cl = {{ (unsigned) c->fd }};
  swix_regs (Socket_Close, &cl);
  return tcp_connect (c) < 0 ? PDT_GONE : 1;
}

static void
tcp_close (pd_transport *t)
{
  tcp *c = (tcp *) t;
  if (c->fd >= 0)
    {
      regs cl = {{ (unsigned) c->fd }};
      swix_regs (Socket_Close, &cl);
    }
  pd_free (c);
}

static oserror *
mount_tcp (const char *arg)
{
  uint32_t parts[5] = { 0 };
  int np = 0, digits = 0;
  const char *p = arg;
  for (; *p > ' '; p++)
    {
      if (*p >= '0' && *p <= '9')
        {
          parts[np] = parts[np] * 10 + (uint32_t) (*p - '0');
          digits++;
        }
      else if ((*p == '.' && np < 3) || (*p == ':' && np == 3))
        {
          if (!digits)
            return ERR (err_syntax);
          np++;
          digits = 0;
        }
      else
        return ERR (err_syntax);
    }
  if (np != 4 || !digits)
    return ERR (err_syntax);
  tcp *c = pd_alloc (sizeof *c);
  if (!c)
    return ERR (err_nomem);
  uint8_t *a = (uint8_t *) &c->addr;
  for (int i = 0; i < 4; i++)
    a[i] = (uint8_t) parts[i];
  c->port = (uint16_t) parts[4];
  c->fd = -1;
  if (tcp_connect (c) < 0)
    {
      pd_free (c);
      return ERR (err_connect);
    }
  c->t.write = tcp_write;
  c->t.read = tcp_read;
  c->t.reset = tcp_reset;
  c->t.close = tcp_close;
  const char pre[] = "TCP ";
  int k = 0;
  for (; pre[k]; k++)
    c->name[k] = pre[k];
  for (p = arg; *p > ' ' && k < (int) sizeof c->name - 1; p++)
    c->name[k++] = *p;
  c->name[k] = 0;
  c->t.name = c->name;
  return (oserror *) pdfs_mount (&c->t);
}

/* --- USB ---------------------------------------------------------------------------- */

ERROR_BLOCK (err_nomtp, 0x1FAE6, "No MTP device found (*PDFSUSB lists the USB devices; a phone must be set to File transfer)");
ERROR_BLOCK (err_nousbdev, 0x1FAE7, "That USB device has no interface with bulk endpoints (see *PDFSUSB)");

static int word_is (const char *tail, const char *w);

/* The callback set at initialisation in the desktop: start the filer */
void
filer_callback (void)
{
  callback_set = 0;
  regs w = {{ 3 }};
  if (filer_task || swix_regs (Wimp_ReadSysInfo, &w) || !w.r[0] || filer_prepare (private_word))
    return;
  filer_task = -1;
  regs st = {{ (unsigned) "Desktop_PDFSFiler" }};
  swix_regs (Wimp_StartTask, &st);
  if (filer_task == -1)         /* refused (no task running): leave it to *WimpTask */
    filer_task = 0;
}

/* "USB5" against a command's word (case-insensitive) */
static int
dev_eq (const char *a, const char *b)
{
  while (*a && (*a | 0x20) == (*b | 0x20))
    a++, b++;
  return !*a && *b <= ' ';
}

/* Mount USB devices: dev 0 = every MTP device not mounted yet; else that
   device ("USB5"), the given interface or its MTP one or its first usable */
static oserror *
mount_usb (const char *dev, int iface, int quiet)
{
  usb_iface c[24];
  int n = usb_scan (c, 24, dev != 0), mounted = 0;
  oserror *first = 0;
  for (int i = 0; i < n; i++)
    {
      if (dev)
        {
          if (!dev_eq (c[i].dev, dev) || !c[i].usable)
            continue;
          if (iface >= 0 ? c[i].iface != iface : !c[i].mtp)
            {
              /* no MTP interface named: take the first usable one after
                 checking there's no MTP one */
              int better = 0;
              for (int j = 0; j < n; j++)
                if (j != i && c[j].mtp && dev_eq (c[j].dev, dev))
                  better = 1;
              if (iface >= 0 || better)
                continue;
            }
        }
      if (usb_is_open (c[i].dev))
        continue;
      oserror *e = (oserror *) usb_mount (&c[i]);
      if (e && !first)
        first = e;
      if (!e)
        mounted++;
      if (dev)
        break;
    }
  if (mounted || quiet)
    return 0;
  if (first)
    return first;
  return dev ? ERR (err_nousbdev) : ERR (err_nomtp);
}

static void
hex (unsigned v, int digits)
{
  char b[9];
  for (int i = 0; i < digits; i++)
    b[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 15];
  b[digits] = 0;
  write0 (b);
}

static void
list_usb (void)
{
  usb_iface c[24];
  int n = usb_scan (c, 24, 1);
  static const char speeds[][6] = { "?", "low", "full", "high", "super" };
  if (!n)
    {
      write0 ("No USB interfaces with bulk endpoints");
      newline ();
    }
  for (int i = 0; i < n; i++)
    {
      write0 (c[i].dev);
      write0 ("  ");
      hex (c[i].vid, 4);
      write0 (":");
      hex (c[i].pid, 4);
      write0 ("  interface ");
      write_dec (c[i].iface);
      write0 ("/");
      write_dec (c[i].alt);
      write0 ("  class ");
      hex (c[i].cls, 2);
      write0 ("/");
      hex (c[i].sub, 2);
      write0 ("/");
      hex (c[i].proto, 2);
      write0 ("  ");
      write0 (speeds[c[i].speed < 5 ? c[i].speed : 0]);
      write0 (" speed");
      newline ();
      write0 ("      endpoints: bulk in ");
      write_dec (c[i].ep_in);
      write0 (", bulk out ");
      write_dec (c[i].ep_out);
      write0 (", interrupt in ");
      write_dec (c[i].ep_int);
      write0 (", packet ");
      write_dec (c[i].mps_out);
      /* vendor-specific interfaces may be MTP (older Androids) */
      int maybe = !c[i].mtp && c[i].usable && c[i].cls == 0xFF;
      write0 (c[i].mtp ? "  - MTP" : maybe ? "  - if it's MTP: *PDFSMount " : "");
      if (maybe)
        write0 (c[i].dev);
      write0 (usb_is_open (c[i].dev) ? "  (mounted)" : "");
      newline ();
    }
}

/* --- module entry points ------------------------------------------------------------- */

static oserror *
add_fs (void)
{
  regs r = {{ 12, (unsigned) module_start, (unsigned) (fs_info_block - module_start),
              (unsigned) private_word }};
  return swix_regs (OS_FSControl, &r);
}

oserror *
mod_init (const char *tail, int instance, void *pw)
{
  (void) tail;
  (void) instance;
  private_word = pw;
  regs s = {{ 0, (unsigned) "MimeMap_Translate" }};
  if (!swix_regs (OS_SWINumberFromString, &s))
    mimemap_translate = s.r[0];
  pd_log ("PDFS " PDFS_VERSION " started");
  oserror *e = add_fs ();
  if (e)
    return e;
  filer_prepare (pw);
  regs rf = {{ (unsigned) resource_files }};
  swix_regs (ResourceFS_RegisterFiles, &rf);
  /* Loaded in the desktop: start the filer from a callback, once the module
     is in the module chain (*Desktop_PDFSFiler enters it by name). Where the
     Wimp refuses (no task is running then), it starts with the next desktop
     (Service_StartWimp), or with *WimpTask Desktop_PDFSFiler. */
  regs w = {{ 3 }};
  if (!swix_regs (Wimp_ReadSysInfo, &w) && w.r[0])
    {
      regs cb = {{ (unsigned) pd_callback, (unsigned) pw }};
      if (!swix_regs (OS_AddCallBack, &cb))
        callback_set = 1;
    }
  return 0;
}

oserror *
mod_final (int fatal)
{
  (void) fatal;
  if (callback_set)
    {
      regs cb = {{ (unsigned) pd_callback, (unsigned) private_word }};
      swix_regs (OS_RemoveCallBack, &cb);
      callback_set = 0;
    }
  filer_finalise ();
  regs rf = {{ (unsigned) resource_files }};
  swix_regs (ResourceFS_DeregisterFiles, &rf);
  pdfs_finish ();
  regs r = {{ 16, (unsigned) "PDFS" }};
  swix_regs (OS_FSControl, &r);
  return 0;
}

oserror *
mod_swi (unsigned number, regs *r)
{
  (void) number;
  (void) r;
  return 0;
}

void
mod_service (regs *r)
{
  switch (r->r[1])
    {
    case 0x40:                      /* Service_FSRedeclare */
      add_fs ();
      break;
    case 0x49:                      /* Service_StartWimp */
      if (filer_task == 0 && !filer_prepare (private_word))
        {
          filer_task = -1;
          r->r[0] = (unsigned) "Desktop_PDFSFiler";
          r->r[1] = 0;
        }
      break;
    case 0x4A:                      /* Service_StartedWimp */
      if (filer_task == -1)
        filer_task = 0;
      break;
    case 0x27:                      /* Service_Reset */
      filer_task = 0;
      break;
    case 0x79:                      /* Service_DeviceDead: R3 = the device's name */
      if (r->r[3])
        usb_device_dead ((const char *) r->r[3]);
      break;
    case 0xD2:                      /* Service_USB */
      if (r->r[0] == 0)             /* Attach: the filer mounts it */
        usb_attached = 1;
      else if (r->r[0] == 4 || r->r[0] == 5)   /* the USB system is going */
        usb_dying ();
      break;
    case 0x60:                      /* Service_ResourceFSStarting */
      call_resourcefs ((void *) r->r[2], (void *) r->r[3], resource_files);
      break;
    }
}


static void
line (const char *s)
{
  write0 (s);
  newline ();
}

static int
word_is (const char *tail, const char *w)
{
  while (*tail == ' ')
    tail++;
  for (; *w; w++, tail++)
    if ((*tail | 0x20) != *w)
      return 0;
  return *tail <= ' ';
}

oserror *
mod_command (int index, const char *tail, int argc)
{
  switch (index)
    {
    case 0:                         /* *PDFS */
      {
        regs r = {{ 14, (unsigned) "PDFS" }};
        return swix_regs (OS_FSControl, &r);
      }
    case 1:                         /* *PDFSMount */
      {
        while (*tail == ' ')
          tail++;
        int before = pdfs_disc_count (), quiet = 0;
        oserror *e;
        if (word_is (tail, "-auto"))
          {
            quiet = 1;
            e = mount_usb (0, -1, 1);
          }
        else if (!argc)
          {
            int had = pdfs_disc_count ();
            pdfs_uneject ();            /* storages ejected on their own come back */
            e = mount_usb (0, -1, pdfs_disc_count () > had);
          }
        else if ((tail[0] | 0x20) == 'u' && (tail[1] | 0x20) == 's' && (tail[2] | 0x20) == 'b')
          {
            int iface = -1;
            const char *q = tail;
            while (*q > ' ')
              q++;
            while (*q == ' ')
              q++;
            if (*q >= '0' && *q <= '9')
              for (iface = 0; *q >= '0' && *q <= '9'; q++)
                iface = iface * 10 + (*q - '0');
            e = mount_usb (tail, iface, 0);
          }
        else
          e = mount_tcp (tail);
        if (!e && !quiet)
          for (int i = before; pdfs_disc_name (i); i++)
            {
              write0 ("PDFS::");
              write0 (pdfs_disc_name (i));
              newline ();
            }
        return quiet ? 0 : e;
      }
    case 8:                         /* *PDFSEject <disc> [all] */
      {
        char disc[40];
        int k = 0, gone = 0, all = 0;
        while (*tail == ' ')
          tail++;
        if (*tail == ':')
          tail++;
        for (; tail[k] > ' ' && tail[k] != '.' && k < 39; k++)
          disc[k] = tail[k];
        disc[k] = 0;
        const char *q = tail + k;
        while (*q > ' ')
          q++;
        all = argc > 1 && word_is (q, "all");
        const char *model = 0;
        pdfs_eject_info (disc, all, 0, &model);
        char name[48];
        int i = 0;
        for (; model && model[i] && i < 47; i++)
          name[i] = model[i];
        name[i] = 0;
        oserror *e = (oserror *) pdfs_eject (disc, all, &gone);
        if (!e)
          {
            write0 (gone ? name : disc);
            write0 (gone ? " has been ejected: it can be unplugged" : " has been ejected");
            newline ();
          }
        return e;
      }
    case 7:                         /* *PDFSUSB */
      if (argc >= 2 && word_is (tail, "control"))
        {
          const char *q = tail + 7;
          while (*q == ' ')
            q++;
          usb_control_requests = word_is (q, "on");
          return 0;
        }
      list_usb ();
      return 0;
    case 2:                         /* *PDFSDismount */
      {
        char disc[40];
        int k = 0;
        while (*tail == ' ')
          tail++;
        if (argc)
          for (; tail[k] > ' ' && k < 39; k++)
            disc[k] = tail[k];
        disc[k] = 0;
        return (oserror *) pdfs_dismount (disc);
      }
    case 3:                         /* *PDFSDevices */
      pdfs_describe (line);
      return 0;
    case 5:                         /* *PDFSExtensions */
      if (argc && word_is (tail, "show"))
        pdfs_set_hide_ext (0);
      else if (argc && word_is (tail, "hide"))
        pdfs_set_hide_ext (1);
      else if (argc)
        return ERR (err_ext_syntax);
      else
        {
          write0 (pdfs_hide_ext ? "Known extensions are hidden (Letter.txt is shown as Letter)"
                                : "Names are shown whole (Letter.txt is shown as Letter/txt)");
          newline ();
        }
      return 0;
    case 6:                         /* *Desktop_PDFSFiler */
      {
        /* from Service_StartWimp (-1), or *WimpTask Desktop_PDFSFiler
           in the desktop when it isn't running */
        regs w = {{ 3 }};
        if (filer_task > 0 || (filer_task == 0 && (swix_regs (Wimp_ReadSysInfo, &w) || !w.r[0])))
          return ERR (err_desktop);
        oserror *pe = filer_prepare (private_word);
        if (pe)
          return pe;
        filer_task = -1;
        regs r = {{ 2, (unsigned) "PDFS", (unsigned) tail }};
        return swix_regs (OS_Module, &r);
      }
    case 4:                         /* *PDFSLog */
      if (argc && word_is (tail, "on"))
        logging = 1;
      else if (argc && word_is (tail, "off"))
        logging = pdfs_trace = 0;
      else if (argc && word_is (tail, "trace"))
        logging = pdfs_trace = 1;
      else
        show_log ();
      return 0;
    }
  return 0;
}
