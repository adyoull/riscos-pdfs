/* module.c: the PDFS module for RISC OS: platform functions for the core,
   *commands, and (for testing) a TCP transport to the MTP simulator.
   The USB transport for real devices comes next (see docs/DESIGN.md).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "cmodule.h"
#include "../core/pdfs.h"
#include "../core/plat.h"

#define OS_Word                 0x07
#define OS_FSControl            0x29
#define OS_ReadMonotonicTime    0x42
#define XReport_Text0           (0x20000 | 0x54C80)
#define OS_SWINumberFromString  0x39
#define Socket_Creat            0x41200
#define Socket_Connect          0x41204
#define Socket_Recv             0x41205
#define Socket_Send             0x41208
#define Socket_Close            0x41210
#define Socket_Select           0x41211

extern char module_start[], fs_info_block[];
static void *private_word;

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
  for (int i = 0; fallback[i].ext[0]; i++)
    if (fallback[i].type == type)
      {
        memcpy (buf, fallback[i].ext, strlen (fallback[i].ext) + 1);
        return 1;
      }
  return 0;
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

ERROR_BLOCK (err_connect, 0x1FAC0, "Can't connect to the simulated device");
ERROR_BLOCK (err_syntax, 0x1FAC1, "Give an address as a.b.c.d:port");
ERROR_BLOCK (err_nomem, 0x1FAB8, "Not enough memory");

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
  return add_fs ();
}

oserror *
mod_final (int fatal)
{
  (void) fatal;
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
  if (r->r[1] == 0x40)            /* Service_FSRedeclare */
    add_fs ();
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
        oserror *e = mount_tcp (tail);
        if (!e)
          for (int i = 0; pdfs_disc_name (i); i++)
            {
              write0 ("PDFS::");
              write0 (pdfs_disc_name (i));
              newline ();
            }
        return e;
      }
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
