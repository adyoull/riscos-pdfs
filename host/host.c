/* host.c: the Linux test harness for the PDFS core. It provides the
   platform functions and a TCP transport to the simulator (sim/mtpsim.py),
   and drives pdfs_fs_entry the way FileSwitch does, so the core's
   filing-system logic can be tested without RISC OS.

     pdfs-host HOST:PORT COMMAND...
       discs                      list the mounted discs
       ls PATH                    FSEntry_Func 15 (read directory entries + info)
       info PATH                  FSEntry_File 5
       get PATH LOCALFILE         Open (read), GetBytes in 4K buffers, Close
       save LOCALFILE PATH [TYPE] as *Save: File 7, Open (update), PutBytes, Args 3, Close
       openout LOCALFILE PATH     as OPENOUT: Open (create), PutBytes, Args 3, Close
       append LOCALFILE PATH      Open (update), write at the end, Close
       rm PATH                    File 6
       mkdir PATH                 File 8
       mv PATH NEWPATH            Func 8
       free PATH                  Func 30
       canon DISC                 Func 23
     Several commands can be given, separated by "--".

   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../core/pdfs.h"
#include "../core/plat.h"

/* --- platform ------------------------------------------------------------------- */

static int verbose;

void *pd_alloc (size_t n) { return calloc (1, n ? n : 1); }
void pd_free (void *p) { free (p); }
void *pd_realloc (void *p, size_t n) { return realloc (p, n ? n : 1); }

unsigned
pd_time_cs (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (unsigned) (ts.tv_sec * 100 + ts.tv_nsec / 10000000);
}

uint32_t pd_unix_time (void) { return (uint32_t) time (0); }
void pd_log (const char *s) { if (verbose) fprintf (stderr, "%s\n", s); }
void pd_log_hex (const char *w, uint32_t v) { if (verbose) fprintf (stderr, "%s &%X\n", w, v); }

static const struct { const char *ext; int type; } types[] = {
  { "txt", 0xFFF }, { "pdf", 0xADF }, { "jpg", 0xC85 }, { "jpeg", 0xC85 }, { "png", 0xB60 },
  { "gif", 0x695 }, { "epub", 0xBE8 }, { "zip", 0xA91 }, { "mp3", 0x1AD }, { "html", 0xFAF },
  { "htm", 0xFAF }, { "csv", 0xDFE }, { "mp4", 0xBF5 }, { 0, 0 } };

int
pd_ext_to_filetype (const char *ext)
{
  for (int i = 0; types[i].ext; i++)
    if (!strcasecmp (types[i].ext, ext))
      return types[i].type;
  return -1;
}

int
pd_filetype_to_ext (int type, char *buf)
{
  for (int i = 0; types[i].ext; i++)
    if (types[i].type == type)
      {
        strcpy (buf, types[i].ext);
        return 1;
      }
  return 0;
}

/* --- scrap files (big files being written) ------------------------------------------ */

static char spill_names[16][300];
int spill_count;                /* open now (for tests) */

int
pd_spill_open (void)
{
  const char *dir = getenv ("PDFS_SPILL_DIR") ? getenv ("PDFS_SPILL_DIR") : "/tmp";
  for (int i = 0; i < 16; i++)
    if (!spill_names[i][0])
      {
        snprintf (spill_names[i], sizeof spill_names[i], "%s/pdfs-spill-XXXXXX", dir);
        int fd = mkstemp (spill_names[i]);
        if (fd < 0)
          {
            spill_names[i][0] = 0;
            return 0;
          }
        spill_count++;
        return fd + 1;
      }
  return 0;
}

int
pd_spill_read (int h, uint64_t off, void *buf, uint32_t len)
{
  ssize_t k = pread (h - 1, buf, len, (off_t) off);
  return k < 0 ? -1 : (int) k;
}

int
pd_spill_write (int h, uint64_t off, const void *buf, uint32_t len)
{
  return pwrite (h - 1, buf, len, (off_t) off) == (ssize_t) len ? 0 : -1;
}

int
pd_spill_setext (int h, uint64_t ext)
{
  return ftruncate (h - 1, (off_t) ext) == 0 ? 0 : -1;
}

void
pd_spill_close (int h)
{
  char path[64];
  snprintf (path, sizeof path, "/proc/self/fd/%d", h - 1);
  char target[300];
  ssize_t n = readlink (path, target, sizeof target - 1);
  close (h - 1);
  if (n > 0)
    {
      target[n] = 0;
      unlink (target);
      for (int i = 0; i < 16; i++)
        if (!strcmp (spill_names[i], target))
          spill_names[i][0] = 0;
    }
  spill_count--;
}

/* --- TCP transport ------------------------------------------------------------------- */

typedef struct
{
  pd_transport t;
  int fd;
  struct sockaddr_in addr;
  char name[64];
} tcp;

static int
tcp_connect (tcp *c)
{
  c->fd = socket (AF_INET, SOCK_STREAM, 0);
  if (c->fd < 0)
    return -1;
  int one = 1;
  setsockopt (c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (connect (c->fd, (struct sockaddr *) &c->addr, sizeof c->addr) < 0)
    {
      close (c->fd);
      c->fd = -1;
      return -1;
    }
  return 0;
}

static int
tcp_write (pd_transport *t, const void *buf, uint32_t len, unsigned timeout_cs)
{
  tcp *c = (tcp *) t;
  (void) timeout_cs;
  const char *p = buf;
  while (len)
    {
      ssize_t n = send (c->fd, p, len, MSG_NOSIGNAL);
      if (n <= 0)
        return n == 0 || errno == EPIPE || errno == ECONNRESET ? PDT_GONE : PDT_IO;
      p += n;
      len -= (uint32_t) n;
    }
  return 0;
}

static int
tcp_read (pd_transport *t, void *buf, uint32_t max, unsigned timeout_cs)
{
  tcp *c = (tcp *) t;
  struct pollfd pf = { c->fd, POLLIN, 0 };
  int r = poll (&pf, 1, (int) timeout_cs * 10);
  if (r == 0)
    return PDT_TIMEOUT;
  if (r < 0)
    return PDT_IO;
  ssize_t n = recv (c->fd, buf, max, 0);
  if (n == 0)
    return PDT_GONE;
  return n < 0 ? PDT_IO : (int) n;
}

static int
tcp_reset (pd_transport *t, uint32_t tid, int level)
{
  /* A stream can't be resynchronised: reconnect (the device drops the session) */
  tcp *c = (tcp *) t;
  (void) tid;
  (void) level;
  close (c->fd);
  if (tcp_connect (c) < 0)
    return PDT_GONE;
  return 1;
}

static void
tcp_close (pd_transport *t)
{
  tcp *c = (tcp *) t;
  if (c->fd >= 0)
    close (c->fd);
  free (c);
}

static pd_transport *
tcp_open (const char *hostport)
{
  tcp *c = calloc (1, sizeof *c);
  char host[64];
  int port = 7015;
  snprintf (host, sizeof host, "%s", hostport);
  char *colon = strchr (host, ':');
  if (colon)
    {
      *colon = 0;
      port = atoi (colon + 1);
    }
  c->addr.sin_family = AF_INET;
  c->addr.sin_port = htons ((uint16_t) port);
  inet_pton (AF_INET, host, &c->addr.sin_addr);
  if (tcp_connect (c) < 0)
    {
      free (c);
      return 0;
    }
  c->t.write = tcp_write;
  c->t.read = tcp_read;
  c->t.reset = tcp_reset;
  c->t.close = tcp_close;
  snprintf (c->name, sizeof c->name, "TCP %s", hostport);
  c->t.name = c->name;
  return &c->t;
}

/* --- a small FileSwitch ------------------------------------------------------------- */

static int failures;

static int
check (const pd_error *e, const char *what)
{
  if (e)
    {
      printf ("ERROR %s: %s (&%X)\n", what, e->errmess, e->errnum);
      failures++;
      return -1;
    }
  return 0;
}

static const pd_error *
call (int entry, pd_regs *r)
{
  return pdfs_fs_entry (entry, r);
}

/* Paths as FileSwitch passes them: ":disc.$.path" */
static void
fspath (char *out, const char *in)
{
  if (in[0] == ':')
    snprintf (out, 1024, "%s", in);
  else
    snprintf (out, 1024, ":%s", in);
}

static void
line (const char *s)
{
  printf ("%s\n", s);
}

static int
do_ls (const char *path)
{
  char p[1024];
  fspath (p, path);
  uint8_t buf[600];
  pd_reg off = 0;
  int n = 0;
  do
    {
      pd_regs r = { { 15, (pd_reg) p, (pd_reg) buf, 3, off, sizeof buf, 0, 0 } };
      if (check (call (PD_FUNC, &r), "ls"))
        return -1;
      uint8_t *q = buf;
      for (pd_reg i = 0; i < r.r[3]; i++)
        {
          uint32_t load = *(uint32_t *) q, len = *(uint32_t *) (q + 8), type = *(uint32_t *) (q + 16);
          char *name = (char *) q + 20;
          printf ("%s %-30s %10u  %03X\n", type == 2 ? "D" : "F", name, len,
                  type == 2 ? 0x1000 : (load >> 8) & 0xFFF);
          q += (20 + strlen (name) + 1 + 3) & ~3u;
          n++;
        }
      off = r.r[4];
    }
  while (off != (pd_reg) -1);
  printf ("%d entries\n", n);
  return 0;
}

static int
do_info (const char *path)
{
  char p[1024];
  fspath (p, path);
  pd_regs r = { { 5, (pd_reg) p } };
  if (check (call (PD_FILE, &r), "info"))
    return -1;
  printf ("type %d load %08X exec %08X length %u attr %02X\n", (int) r.r[0],
          (unsigned) r.r[2], (unsigned) r.r[3], (unsigned) r.r[4], (unsigned) r.r[5]);
  return 0;
}

static int
do_get (const char *path, const char *local)
{
  char p[1024];
  fspath (p, path);
  pd_regs r = { { 0, (pd_reg) p, 0, 101 } };
  if (check (call (PD_OPEN, &r), "open"))
    return -1;
  if (!r.r[1])
    {
      printf ("ERROR get: not found\n");
      failures++;
      return -1;
    }
  pd_reg h = r.r[1];
  uint32_t ext = (uint32_t) r.r[3], bs = (uint32_t) r.r[2];
  FILE *f = fopen (local, "wb");
  uint8_t *buf = malloc (bs);
  for (uint32_t off = 0; off < ext; off += bs)
    {
      pd_regs g = { { 0, h, (pd_reg) buf, bs, off } };
      if (check (call (PD_GETBYTES, &g), "getbytes"))
        break;
      fwrite (buf, 1, ext - off < bs ? ext - off : bs, f);
    }
  fclose (f);
  free (buf);
  pd_regs c = { { 0, h, 0, 0 } };
  check (call (PD_CLOSE, &c), "close");
  return 0;
}

static uint8_t *
slurp (const char *local, uint32_t *len)
{
  FILE *f = fopen (local, "rb");
  if (!f)
    {
      perror (local);
      exit (2);
    }
  fseek (f, 0, SEEK_END);
  *len = (uint32_t) ftell (f);
  fseek (f, 0, SEEK_SET);
  uint8_t *b = malloc (*len + 1);
  if (fread (b, 1, *len, f) != *len)
    exit (2);
  fclose (f);
  return b;
}

static int
write_stream (pd_reg h, uint32_t bs, const uint8_t *data, uint32_t len, uint32_t start)
{
  /* FileSwitch writes whole buffers; the last one runs past the end */
  uint8_t *buf = calloc (1, bs);
  for (uint32_t off = start - start % bs; off < start + len; off += bs)
    {
      uint32_t base = off;
      memset (buf, 0, bs);
      if (off < start)
        {
          /* as FileSwitch does: read the partly-filled block before changing it */
          pd_regs g = { { 0, h, (pd_reg) buf, bs, off } };
          if (check (call (PD_GETBYTES, &g), "getbytes (before write)"))
            break;
        }
      for (uint32_t i = 0; i < bs; i++)
        if (base + i >= start && base + i < start + len)
          buf[i] = data[base + i - start];
      pd_regs w = { { 0, h, (pd_reg) buf, bs, off } };
      if (check (call (PD_PUTBYTES, &w), "putbytes"))
        {
          free (buf);
          return -1;
        }
    }
  free (buf);
  pd_regs a = { { 3, h, start + len } };
  return check (call (PD_ARGS, &a), "setext");
}

static int
do_save (const char *local, const char *path, int type, int how)
{
  char p[1024];
  fspath (p, path);
  uint32_t len;
  uint8_t *data = slurp (local, &len);
  uint32_t load = 0xFFF00000u | (uint32_t) type << 8, start = 0;
  if (how == 0)
    {
      pd_regs c = { { 7, (pd_reg) p, load, 0, 0, len } };
      if (check (call (PD_FILE, &c), "create"))
        return -1;
    }
  pd_regs o = { { how == 1 ? 1 : 2, (pd_reg) p, 0, 102 } };
  if (check (call (PD_OPEN, &o), "open"))
    return -1;
  if (!o.r[1])
    {
      printf ("ERROR save: open found nothing\n");
      failures++;
      return -1;
    }
  if (how == 2)
    start = (uint32_t) o.r[3];
  write_stream (o.r[1], (uint32_t) o.r[2], data, len, start);
  if (how != 0)
    {
      /* as FileSwitch does: keep the type the file has (Args 9), new date */
      pd_regs le = { { 9, o.r[1] } };
      if (!check (call (PD_ARGS, &le), "readloadexec"))
        load = (uint32_t) le.r[2];
    }
  pd_regs c = { { 0, o.r[1], load, 0 } };
  check (call (PD_CLOSE, &c), "close");
  free (data);
  return 0;
}

/* Copy a file as Filer_Action does: create it with its size and a "dead"
   load address, open it for update, set its extent to 0, write it in
   blocks (chunk bytes at a time) in order, close it, then set its type. */
static int
do_facopy (const char *local, const char *path, int type, uint32_t chunk)
{
  char p[1024];
  fspath (p, path);
  uint32_t len;
  uint8_t *data = slurp (local, &len);
  pd_regs c = { { 7, (pd_reg) p, 0xDEADDEAD, 0xDEADDEAD, 0, len } };
  if (check (call (PD_FILE, &c), "create"))
    return -1;
  pd_regs o = { { 2, (pd_reg) p, 0, 103 } };
  if (check (call (PD_OPEN, &o), "open"))
    return -1;
  pd_reg h = o.r[1];
  uint32_t bs = (uint32_t) o.r[2];
  pd_regs z = { { 3, h, 0 } };
  if (check (call (PD_ARGS, &z), "setext 0"))
    return -1;
  /* FileSwitch: whole aligned blocks go straight through, a partial block
     waits in its buffer until it's full or the file is closed */
  uint8_t *fbuf = calloc (1, bs);
  uint32_t fbuf_block = 0, fbuf_used = 0;
  int failed = 0, swap = getenv ("PDFS_FA_SWAP") != 0;
  for (uint32_t done = 0; done < len && !failed; )
    {
      uint32_t n = len - done < chunk ? len - done : chunk;
      if (swap && done == chunk && len >= 3 * chunk)
        {
          /* testing: write the third chunk before the second */
          done = 2 * chunk;
          n = chunk;
        }
      else if (swap && done == 3 * chunk && len >= 3 * chunk)
        {
          done = chunk;
          n = chunk;
          swap = 0;
        }
      for (uint32_t i = 0; i < n && !failed; )
        {
          uint32_t pos = done + i, blk = pos / bs * bs, in = pos - blk;
          if (in == 0 && n - i >= bs)
            {
              uint32_t whole = (n - i) / bs * bs;
              pd_regs w = { { 0, h, (pd_reg) (data + pos), whole, pos } };
              failed = check (call (PD_PUTBYTES, &w), "putbytes");
              i += whole;
              continue;
            }
          if (fbuf_used && fbuf_block != blk)
            {
              pd_regs w = { { 0, h, (pd_reg) fbuf, bs, fbuf_block } };
              failed = check (call (PD_PUTBYTES, &w), "putbytes (buffer)");
              fbuf_used = 0;
            }
          if (!fbuf_used)
            memset (fbuf, 0, bs);
          fbuf_block = blk;
          uint32_t k = bs - in < n - i ? bs - in : n - i;
          memcpy (fbuf + in, data + pos, k);
          fbuf_used = in + k;
          i += k;
          if (fbuf_used == bs)
            {
              pd_regs w = { { 0, h, (pd_reg) fbuf, bs, fbuf_block } };
              failed = check (call (PD_PUTBYTES, &w), "putbytes (buffer full)");
              fbuf_used = 0;
            }
        }
      done += n;
      if (!swap && getenv ("PDFS_FA_SWAP") && done == 2 * chunk)
        done = 3 * chunk;
    }
  if (!failed && fbuf_used)
    {
      pd_regs w = { { 0, h, (pd_reg) fbuf, bs, fbuf_block } };
      failed = check (call (PD_PUTBYTES, &w), "putbytes (last)");
    }
  free (fbuf);
  pd_regs a = { { 3, h, len } };
  check (call (PD_ARGS, &a), "setext");
  pd_regs le = { { 9, h } };
  check (call (PD_ARGS, &le), "readloadexec");
  pd_regs cl = { { 0, h, le.r[2], le.r[3] } };
  int e = check (call (PD_CLOSE, &cl), "close");
  pd_regs wi = { { 1, (pd_reg) p, 0xFFF00000u | (uint32_t) type << 8, 0, 0, 3 } };
  if (!e)
    check (call (PD_FILE, &wi), "writeinfo");
  free (data);
  return e;
}

static int
run (int argc, char **argv)
{
  const char *cmd = argv[0];
  if (!strcmp (cmd, "discs"))
    {
      pdfs_describe (line);
      return 0;
    }
  if (!strcmp (cmd, "ls") && argc > 1)
    return do_ls (argv[1]);
  if (!strcmp (cmd, "info") && argc > 1)
    return do_info (argv[1]);
  if (!strcmp (cmd, "get") && argc > 2)
    return do_get (argv[1], argv[2]);
  if (!strcmp (cmd, "save") && argc > 2)
    return do_save (argv[1], argv[2], argc > 3 ? (int) strtol (argv[3], 0, 16) : 0xFFD, 0);
  if (!strcmp (cmd, "facopy") && argc > 2)
    return do_facopy (argv[1], argv[2], argc > 3 ? (int) strtol (argv[3], 0, 16) : 0xFFD,
                      argc > 4 ? (uint32_t) atoi (argv[4]) : 65536 + 1000);
  if (!strcmp (cmd, "openout") && argc > 2)
    return do_save (argv[1], argv[2], 0xFFD, 1);
  if (!strcmp (cmd, "append") && argc > 2)
    return do_save (argv[1], argv[2], 0xFFD, 2);
  if (!strcmp (cmd, "rm") && argc > 1)
    {
      char p[1024];
      fspath (p, argv[1]);
      pd_regs r = { { 6, (pd_reg) p } };
      if (check (call (PD_FILE, &r), "rm"))
        return -1;
      printf ("deleted type %d\n", (int) r.r[0]);
      return 0;
    }
  if (!strcmp (cmd, "mkdir") && argc > 1)
    {
      char p[1024];
      fspath (p, argv[1]);
      pd_regs r = { { 8, (pd_reg) p } };
      return check (call (PD_FILE, &r), "mkdir");
    }
  if (!strcmp (cmd, "mv") && argc > 2)
    {
      char a[1024], b[1024];
      fspath (a, argv[1]);
      fspath (b, argv[2]);
      pd_regs r = { { 8, (pd_reg) a, (pd_reg) b } };
      if (check (call (PD_FUNC, &r), "mv"))
        return -1;
      if (r.r[1])
        {
          printf ("ERROR mv: bad rename\n");
          failures++;
        }
      return 0;
    }
  if (!strcmp (cmd, "settype") && argc > 2)
    {
      char p[1024];
      fspath (p, argv[1]);
      pd_regs r = { { 2, (pd_reg) p, 0xFFF00000u | (pd_reg) strtol (argv[2], 0, 16) << 8 } };
      return check (call (PD_FILE, &r), "settype");
    }
  if (!strcmp (cmd, "free") && argc > 1)
    {
      char p[1024];
      fspath (p, argv[1]);
      pd_regs r = { { 30, (pd_reg) p } };
      if (check (call (PD_FUNC, &r), "free"))
        return -1;
      printf ("free %u size %u\n", (unsigned) r.r[0], (unsigned) r.r[2]);
      return 0;
    }
  if (!strcmp (cmd, "canon"))
    {
      const char *d = argc > 1 ? argv[1] : 0;
      pd_regs r = { { 23, 0, (pd_reg) d, 0, 0 } };
      if (check (call (PD_FUNC, &r), "canon"))
        return -1;
      char buf[64];
      uint32_t need = (uint32_t) r.r[4] + 1;
      pd_regs r2 = { { 23, 0, (pd_reg) d, 0, (pd_reg) buf, 0, need } };
      if (check (call (PD_FUNC, &r2), "canon2"))
        return -1;
      printf ("disc %s\n", (char *) r2.r[2]);
      return 0;
    }
  if (!strcmp (cmd, "sleep") && argc > 1)
    {
      usleep ((useconds_t) (atof (argv[1]) * 1e6));
      return 0;
    }
  fprintf (stderr, "unknown command %s\n", cmd);
  failures++;
  return -1;
}

int
main (int argc, char **argv)
{
  if (argc > 1 && !strcmp (argv[1], "-v"))
    {
      verbose = 1;
      argv++;
      argc--;
    }
  if (argc < 3)
    {
      fprintf (stderr, "usage: pdfs-host [-v] HOST:PORT COMMAND... [-- COMMAND...]\n");
      return 2;
    }
  if (getenv ("PDFS_TIMEOUT_CS"))
    pdfs_timeout_cs = (unsigned) atoi (getenv ("PDFS_TIMEOUT_CS"));
  if (getenv ("PDFS_SHOW_EXT"))
    pdfs_set_hide_ext (0);
  if (getenv ("PDFS_TRACE"))
    pdfs_trace = 1;
  pd_transport *t = tcp_open (argv[1]);
  if (!t)
    {
      fprintf (stderr, "can't connect to %s\n", argv[1]);
      return 2;
    }
  if (getenv ("PDFS_NO_ZLP"))
    {
      /* behave as RISC OS's USB transport does: no zero-length packets */
      t->max_packet = (uint32_t) atoi (getenv ("PDFS_NO_ZLP"));
      t->no_zlp = 1;
    }
  if (check (pdfs_mount (t), "mount"))
    return 1;
  int i = 2;
  while (i < argc)
    {
      int j = i;
      while (j < argc && strcmp (argv[j], "--"))
        j++;
      argv[j < argc ? j : argc] = 0;
      run (j - i, argv + i);
      i = j + 1;
    }
  pdfs_finish ();
  return failures ? 1 : 0;
}
