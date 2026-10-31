/* fs.c: PDFS's filing system: devices and their storages ("discs"), the
   directory cache that maps RISC OS paths to MTP object handles, and the
   FileSwitch entry points.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "pdfs.h"
#include "mtp.h"
#include "plat.h"

unsigned pdfs_dir_ttl_cs = 500;
int pdfs_hide_ext = 1;
unsigned pdfs_timeout_cs = 1000;
/* An Android device is left alone this long after its session opens
   before it's asked about its storages: an Amazon Fire just back on the
   bus after Allow answered a few requests at once and then nothing more
   (0.16 tests 5 to 7); OpenMTP, used seconds later, works. */
unsigned pdfs_settle_cs = 500;
/* The least time between an answer from an Android device and the next
   request (see gap_wait in mtp.c); -1: the default. *PDFSMount sets it
   from PDFS$Gap. */
int pdfs_gap_cs = -1;
#define ANDROID_GAP_CS 1

#define MAX_DEVICES   4
#define MAX_STORAGES  4
#define BUFSIZE       4096          /* FileSwitch buffer size for our files */
#define READAHEAD     65536
#define MAX_WRITE     0xFFFFFFF0u   /* largest file (MTP sizes are 32 bits) */
#define SPILL_AT      (4u << 20)    /* bigger files being written go to a scrap file */
#define STREAM_AT     (1u << 20)    /* new files this big or bigger are sent as they're written */
#define HEADROOM      12            /* bytes before out-data for the container header */

/* --- types -------------------------------------------------------------------- */

typedef struct node node;
struct node
{
  node *parent, *child, *next;
  struct storage *st;
  uint32_t handle;            /* 0 = storage root, or a file not yet sent to the device */
  uint8_t isdir, listed, gone, pending;
  uint8_t sending;            /* closed, being sent in the background */
  int refs;                   /* open files using it */
  unsigned listed_at;
  unsigned seen;              /* mark of the last listing that included it */
  uint64_t size;
  uint32_t decl;              /* size given when created with FSEntry_File 7 */
  uint32_t mtime;             /* unix seconds */
  int filetype;               /* -1: from the name */
  int name_type;              /* type the name's suffix gives, or -1 */
  char *mtp_name;             /* UTF-8 */
  char *ro_name;              /* the machine's alphabet, RISC OS form, as shown */
  char *ro_full;              /* the whole name in RISC OS form (with the suffix) */
  char sfx[18];               /* type suffix of mtp_name (".txt", ",ffb") or "" */
};

typedef struct storage
{
  struct device *dev;
  uint32_t id;
  char desc[48];
  char disc[40];
  uint64_t free, capacity;
  int readonly;
  int ejected;                /* *PDFSEject'ed: hidden until the device is mounted again */
  int info_pending;           /* found, but not yet asked about (GetStorageInfo): hidden */
  unsigned info_at;           /* when free space was last read */
  unsigned info_failed_at;    /* when the device last didn't answer that */
  node root;
} storage;

typedef struct device
{
  int used;
  mtp_dev mtp;
  storage st[MAX_STORAGES];
  int nst;
  int waiting;                /* no storages yet: access not allowed on the device */
  unsigned next_look;         /* when to ask for its storages again (pd_time_cs) */
  int silent;                 /* looks in a row it didn't answer */
  unsigned settle_until;      /* an Android device just opened: not asked about its storages before this */
} device;

/* A storage ejected on its own, or not yet asked about, isn't shown */
static int
st_hidden (const storage *st)
{
  return st->ejected || st->info_pending;
}

typedef struct ofile ofile;
struct ofile
{
  ofile *next;
  node *n;
  int writable, dirty, loaded;
  uint32_t fs_handle;         /* FileSwitch's handle for the stream */
  uint64_t size;
  uint8_t *wbuf;              /* whole file for writing (HEADROOM bytes before data) */
  uint32_t wcap;
  int spill;                  /* or, when it's big, a scrap file (pd_spill_open) */
  int lazy;                   /* created big: no storage yet (it may be streamed) */
  /* Or sent to the device as it's written (a big file created with its
     size, written from the start: Filer_Action, *Copy): stream 1 while
     sending, 2 once it has gone wrong (written again, differently, where
     it had gone), 3 failed; sent = bytes sent so far. What's written is
     kept as well (wbuf / spill) */
  int stream;
  uint64_t sent;
  uint32_t slen;              /* bytes in the SendObject (the last may follow by an edit) */
  uint8_t *tail;              /* while streaming: a bit for each 4K block written */
  uint8_t *rbuf;              /* read-ahead */
  uint32_t rgen;              /* from a device without partial reads: which whole-object */
  uint64_t rpos;              /* stream (mtp_rstream) is this file's, and how far it has got */
  int fill;                   /* being received whole, into its storage, in the background */
  uint64_t have;              /* bytes of it there so far */
  uint64_t roff;
  uint32_t rlen;
};

typedef struct { ofile *f; uint64_t base; } fill_src;
static int f_read (ofile *f, uint64_t off, void *dst, uint32_t n);
static int fill_from (void *ctx, uint8_t *dst, uint64_t off, uint32_t len);
static void f_free (ofile *f);

static device devices[MAX_DEVICES];
static ofile *files;
static int fill_run (unsigned until_cs);
#define FILL_SLICE 3            /* cs of receiving from a device per null event */
static node *pending_create;   /* created by FSEntry_File 7, not yet sent */
int pdfs_background;            /* the filer task sends big files in the background */
int pdfs_background_option;     /* *PDFSBackground: whether it should (off by default) */

/* --- errors -------------------------------------------------------------------- */

#define E(name, num, text) static const pd_error name = { PDFS_ERRBASE | (num), text }
E (err_notfound, 0xD6, "Not found");
E (err_nodisc, 0xD3, "Disc not found");      /* not FileCore's numbers: ours */
E (err_nodevices, 0xD4, "No portable device is mounted");
E (err_exists, 0xC4, "Already exists");
E (err_notempty, 0xB4, "Directory not empty");
E (err_isdir, 0xB5, "Is a directory");
E (err_notdir, 0xB6, "Not a directory");
E (err_io, 0xC7, "The device didn't respond (transfer failed)");
E (err_timeout, 0xC8, "The device stopped responding");
E (err_gone, 0xC9, "The device has been disconnected");
E (err_denied, 0xBD, "Access denied by the device");
E (err_readonly, 0xC1, "The device's storage is read-only");
E (err_full, 0xC6, "The device's storage is full");
E (err_nomem, 0xB8, "Not enough memory");
E (err_toobig, 0xB9, "File too big for a portable device (4GB at most)");
E (err_noscrap, 0xBA, "No room for the file: PDFS keeps big files in the scrap directory while writing them");
E (err_scrapio, 0xBB, "PDFS couldn't use its scrap file");
E (err_busy, 0xBC, "The device is busy receiving a file: try again when it has finished");
E (err_order, 0xBE, "The file couldn't be sent to the device: it wasn't written in order");
E (err_stream_size, 0xBF, "The file sent to the device isn't the size it was created with");
E (err_badname, 0xCC, "Bad name");
E (err_unsupported, 0xA5, "The device can't do that");
E (err_proto, 0xCA, "The device sent something PDFS doesn't understand");
E (err_toomany, 0xCB, "Too many portable devices");
E (err_badhandle, 0xDE, "Channel");

static pd_error err_device;    /* a response code from the device, with its number */

static const char hexd[] = "0123456789ABCDEF";
static char *cat_num (char *o, uint64_t v);
static char *cat (char *o, const char *s);
static char *cat_tenths (char *o, uint64_t v);
extern int pdfs_trace;

static const pd_error *
rc_error (int rc)
{
  if (rc == RC_OK)
    return 0;
  switch (rc)
    {
    case PDT_TIMEOUT: return &err_timeout;
    case PDT_GONE: return &err_gone;
    case PDT_IO:
    case PDT_LOST: return &err_io;
    case MTP_ENOMEM: return &err_nomem;
    case MTP_EBUSY: return &err_busy;
    case MTP_EPROTO: return &err_proto;
    case RC_InvalidObjectHandle:
    case RC_InvalidParentObject: return &err_notfound;
    case RC_AccessDenied:
    case RC_ObjectWriteProtected: return &err_denied;
    case RC_StoreReadOnly: return &err_readonly;
    case RC_StoreFull: return &err_full;
    case RC_OperationNotSupported: return &err_unsupported;
    }
  /* Anything else: "The device refused (&2002)" */
  static const char t[] = "The device refused (MTP response &";
  int i = 0;
  for (; t[i]; i++)
    err_device.errmess[i] = t[i];
  for (int s = 12; s >= 0; s -= 4)
    err_device.errmess[i++] = hexd[(rc >> s) & 15];
  err_device.errmess[i++] = ')';
  err_device.errmess[i] = 0;
  err_device.errnum = PDFS_ERRBASE | 0xCD;
  return &err_device;
}

/* --- small string helpers --------------------------------------------------------- */

static int
lc (int c)
{
  c &= 0xFF;
  if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
    return c + 32;
  return c;
}

static int
name_eq (const char *a, const char *b, int blen)
{
  int i = 0;
  for (; i < blen; i++)
    if (!a[i] || lc (a[i]) != lc (b[i]))
      return 0;
  return a[i] == 0;
}

static int
exact_eq (const char *a, const char *b, int blen)
{
  int i = 0;
  for (; i < blen; i++)
    if (a[i] != b[i])
      return 0;
  return a[i] == 0;
}

static int
memcmp_bytes (const uint8_t *a, const uint8_t *b, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return 1;
  return 0;
}

static char *
str_dup (const char *s, int n)
{
  if (n < 0)
    n = (int) strlen (s);
  char *p = pd_alloc ((size_t) n + 1);
  if (p)
    {
      memcpy (p, s, (size_t) n);
      p[n] = 0;
    }
  return p;
}

static void
str_copy (char *d, const char *s, int size)
{
  int i = 0;
  for (; s[i] && i < size - 1; i++)
    d[i] = s[i];
  d[i] = 0;
}

/* Device (UTF-8) name to RISC OS name: '.'<->'/', space -> hard space,
   characters RISC OS reserves -> '_', anything outside Latin-1 -> '_'. */
static char *
to_riscos_name (const char *u8)
{
  int n = (int) strlen (u8);
  char *o = pd_alloc ((size_t) n + 1), *p = o;
  const uint8_t *u = (const uint8_t *) u8;
  if (!o)
    return 0;
  while (*u)
    {
      uint32_t c = *u++;
      if (c >= 0xC0)
        {
          int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
          c &= 0x3F >> extra;
          while (extra-- && (*u & 0xC0) == 0x80)
            c = (c << 6) | (*u++ & 63);
        }
      else if (c >= 0x80)
        c = '_';
      if (c == '.')
        c = '/';
      else if (c == '/')
        c = '.';      /* can't happen on a device, but keep it reversible */
      else if (c == ' ')
        c = 0xA0;
      else if (c < 32 || c == 127 || c > 255 || c == '$' || c == '&' || c == '%' || c == '@'
               || c == '^' || c == ':' || c == '#' || c == '*' || c == '"' || c == '|')
        c = '_';
      *p++ = (char) c;
    }
  *p = 0;
  return o;
}

/* RISC OS leaf name to UTF-8 for the device. */
static char *
to_device_name (const char *ro, int n)
{
  char *o = pd_alloc ((size_t) n * 2 + 1), *p = o;
  if (!o)
    return 0;
  for (int i = 0; i < n; i++)
    {
      uint32_t c = (uint8_t) ro[i];
      if (c == '/')
        c = '.';
      else if (c == 0xA0)
        c = ' ';
      if (c < 0x80)
        *p++ = (char) c;
      else
        {
          *p++ = (char) (0xC0 | c >> 6);
          *p++ = (char) (0x80 | (c & 63));
        }
    }
  *p = 0;
  return o;
}

/* --- dates --------------------------------------------------------------------- */

static uint32_t
days_from_civil (int y, int m, int d)
{
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (unsigned) ((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t) (era * 146097 + (int) doe - 719468);
}

static uint32_t
parse_mtp_date (const char *s)
{
  int v[6] = { 0 }, w[6] = { 4, 2, 2, 2, 2, 2 };
  for (int f = 0; f < 6; f++)
    {
      if (f == 3)
        {
          if (*s != 'T')
            return 0;
          s++;
        }
      for (int i = 0; i < w[f]; i++, s++)
        {
          if (*s < '0' || *s > '9')
            return 0;
          v[f] = v[f] * 10 + (*s - '0');
        }
    }
  uint32_t t = days_from_civil (v[0], v[1], v[2]) * 86400u + (uint32_t) (v[3] * 3600 + v[4] * 60 + v[5]);
  /* The MTP standard's optional parts: tenths (".s"), then the time zone:
     "Z" (UTC) or +hhmm/-hhmm. Without one the zone isn't said, and the time
     is taken as it stands (as PDFS writes its own); with one, it's UTC. */
  if (*s == '.')
    for (s++; *s >= '0' && *s <= '9'; s++)
      ;
  if ((*s == '+' || *s == '-') && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9')
    {
      int sign = *s == '-' ? -1 : 1;
      int hh = (s[1] - '0') * 10 + (s[2] - '0'), mm = 0;
      const char *q = s + 3;
      if (*q == ':')
        q++;
      if (q[0] >= '0' && q[0] <= '9' && q[1] >= '0' && q[1] <= '9')
        mm = (q[0] - '0') * 10 + (q[1] - '0');
      if (hh <= 14 && mm < 60)
        t -= (uint32_t) (sign * (hh * 3600 + mm * 60));
    }
  return t;
}

static void
format_mtp_date (uint32_t t, char *o)
{
  uint32_t days = t / 86400, secs = t % 86400;
  /* civil_from_days */
  uint32_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  uint32_t f[6] = { y, m, d, secs / 3600, secs / 60 % 60, secs % 60 };
  int w[6] = { 4, 2, 2, 2, 2, 2 }, k = 0;
  for (int i = 0; i < 6; i++)
    {
      if (i == 3)
        o[k++] = 'T';
      for (int j = w[i] - 1; j >= 0; j--)
        {
          uint32_t v = f[i];
          for (int q = 0; q < j; q++)
            v /= 10;
          o[k++] = (char) ('0' + v % 10);
        }
    }
  o[k] = 0;
}

/* RISC OS 5-byte time (centiseconds since 1900) from unix seconds */
static uint64_t
riscos_time (uint32_t t)
{
  return ((uint64_t) t + 2208988800u) * 100;
}

/* --- nodes ------------------------------------------------------------------------ */

static int
node_filetype (node *n)
{
  if (n->filetype >= 0)
    return n->filetype;
  return n->name_type >= 0 ? n->name_type : 0xFFD;
}

static void
node_loadexec (node *n, pd_reg *load, pd_reg *exec)
{
  uint64_t t = riscos_time (n->mtime);
  int type = n->isdir ? 0xFFD : node_filetype (n);
  *load = (pd_reg) (0xFFF00000u | (uint32_t) type << 8 | (uint32_t) (t >> 32));
  *exec = (pd_reg) (uint32_t) t;
}

static void
free_tree (node *n)
{
  node *c = n->child;
  while (c)
    {
      node *nx = c->next;
      c->parent = 0;
      if (c->refs)
        c->gone = 1;          /* an open file holds it: freed when closed */
      else
        {
          free_tree (c);
          pd_free (c->mtp_name);
          pd_free (c->ro_name);
          pd_free (c->ro_full);
          if (pending_create == c)
            pending_create = 0;
          pd_free (c);
        }
      c = nx;
    }
  n->child = 0;
  n->listed = 0;
}

static void
unlink_node (node *n)
{
  node *p = n->parent;
  if (p)
    {
      node **pp = &p->child;
      while (*pp && *pp != n)
        pp = &(*pp)->next;
      if (*pp)
        *pp = n->next;
    }
  n->parent = 0;
  n->next = 0;
}

static void
drop_node (node *n)
{
  unlink_node (n);
  free_tree (n);
  if (n->refs)
    {
      n->gone = 1;
      return;
    }
  if (pending_create == n)
    pending_create = 0;
  pd_free (n->mtp_name);
  pd_free (n->ro_name);
  pd_free (n->ro_full);
  pd_free (n);
}

/* --- names and filetypes ------------------------------------------------------------
   A file's type is kept in its name on the device, as other systems expect:
   a known extension ("Letter.txt": MimeMap), or RISC OS's ",xxx" for types
   with none ("Prog,ffb"). RISC OS sees the name without that suffix
   ("Letter", Text), unless two names in a directory would then be the same
   or *PDFSExtensions show is set; the name with the suffix ("Letter/txt")
   finds the file too. */

static int
hexval (int c)
{
  c = lc (c);
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* The type suffix of a device name: sets sfx (the suffix with its '.' or ',')
   and returns the type, or -1 with sfx "" if there is none. */
static int
name_suffix (const char *name, char *sfx)
{
  int n = (int) strlen (name);
  sfx[0] = 0;
  if (n > 4 && name[n - 4] == ',')
    {
      int a = hexval (name[n - 3]), b = hexval (name[n - 2]), c = hexval (name[n - 1]);
      if (a >= 0 && b >= 0 && c >= 0)
        {
          str_copy (sfx, name + n - 4, 5);
          return a << 8 | b << 4 | c;
        }
    }
  int dot = -1;
  for (int i = 0; i < n; i++)
    if (name[i] == '.')
      dot = i;
  if (dot > 0 && n - dot - 1 >= 1 && n - dot - 1 <= 8)
    {
      int t = pd_ext_to_filetype (name + dot + 1);
      if (t >= 0)
        {
          str_copy (sfx, name + dot, 10);
          return t;
        }
    }
  return -1;
}

/* Work out ro_name, ro_full, sfx and name_type from mtp_name. */
static int
derive_names (node *n)
{
  char *full = to_riscos_name (n->mtp_name), *shown;
  if (!full)
    return -1;
  n->name_type = n->isdir ? -1 : name_suffix (n->mtp_name, n->sfx);
  if (n->isdir)
    n->sfx[0] = 0;
  if (pdfs_hide_ext && n->sfx[0])
    shown = str_dup (full, (int) (strlen (full) - strlen (n->sfx)));
  else
    shown = str_dup (full, -1);
  if (!shown)
    {
      pd_free (full);
      return -1;
    }
  pd_free (n->ro_name);
  pd_free (n->ro_full);
  n->ro_name = shown;
  n->ro_full = full;
  return 0;
}

/* Two files whose names are the same without their suffixes ("a.txt" and
   "a.jpg"), or one that matches another name ("a.txt" and "a"): show those
   with their suffixes. */
static void
fix_clashes (node *dir)
{
  if (!pdfs_hide_ext)
    return;
  int count = 0;
  for (node *c = dir->child; c; c = c->next)
    count++;
  if (count < 2)
    return;
  unsigned size = 1;
  while (size < (unsigned) count * 2)
    size <<= 1;
  node **tab = pd_alloc (size * sizeof *tab);
  if (!tab)
    return;
  for (node *c = dir->child; c; c = c->next)
    {
      unsigned h = 5381;
      for (const char *p = c->ro_name; *p; p++)
        h = h * 33 + (unsigned) lc (*p);
      for (h &= size - 1; tab[h]; h = (h + 1) & (size - 1))
        {
          node *d = tab[h];
          if (name_eq (d->ro_name, c->ro_name, (int) strlen (c->ro_name)))
            {
              node *show[2] = { c, d };
              for (int i = 0; i < 2; i++)
                if (show[i]->sfx[0] && strlen (show[i]->ro_name) != strlen (show[i]->ro_full))
                  {
                    char *f = str_dup (show[i]->ro_full, -1);
                    if (f)
                      {
                        pd_free (show[i]->ro_name);
                        show[i]->ro_name = f;
                      }
                  }
              break;
            }
        }
      if (!tab[h])
        tab[h] = c;
    }
  pd_free (tab);
}

static node *
new_node (node *parent, const char *mtp_name)
{
  node *n = pd_alloc (sizeof *n);
  if (!n)
    return 0;
  n->filetype = -1;
  n->mtp_name = str_dup (mtp_name, -1);
  if (!n->mtp_name || derive_names (n))
    {
      pd_free (n->mtp_name);
      pd_free (n->ro_name);
      pd_free (n->ro_full);
      pd_free (n);
      return 0;
    }
  n->st = parent->st;
  n->parent = parent;
  n->next = parent->child;
  parent->child = n;
  return n;
}

static int
set_name (node *n, const char *mtp_name)
{
  char *a = str_dup (mtp_name, -1);
  if (!a)
    return -1;
  char *old = n->mtp_name;
  n->mtp_name = a;
  if (derive_names (n))
    {
      n->mtp_name = old;
      pd_free (a);
      return -1;
    }
  pd_free (old);
  if (n->parent)
    fix_clashes (n->parent);
  return 0;
}

/* The device name for a new file or a renamed one: the RISC OS leaf, plus
   the suffix for its type unless the leaf already gives that type.
   keep_sfx: the suffix a renamed file had, to keep if the type is the same. */
static char *
add_type_suffix (char *dn, int isdir, int type, const char *keep_sfx)
{
  if (!dn || isdir || type < 0)
    return dn;
  char sfx[18], add[18];
  int own = name_suffix (dn, sfx);
  if (own == type || (type == 0xFFD && own < 0))
    return dn;
  add[0] = 0;
  if (keep_sfx && keep_sfx[0])
    {
      /* keep ".jpeg" rather than switch to ".jpg" when the type is the same */
      char t[20], x[18];
      t[0] = 'x';
      str_copy (t + 1, keep_sfx, sizeof t - 1);
      if (name_suffix (t, x) == type)
        str_copy (add, keep_sfx, sizeof add);
    }
  if (!add[0] && type != 0xFFD)
    {
      char ext[16];
      if (pd_filetype_to_ext (type, ext) && strlen (ext) <= 8)
        {
          add[0] = '.';
          str_copy (add + 1, ext, sizeof add - 1);
        }
      else
        {
          add[0] = ',';
          add[1] = "0123456789abcdef"[(type >> 8) & 15];
          add[2] = "0123456789abcdef"[(type >> 4) & 15];
          add[3] = "0123456789abcdef"[type & 15];
          add[4] = 0;
        }
    }
  if (!add[0])
    return dn;
  int a = (int) strlen (dn), b = (int) strlen (add);
  char *o = pd_alloc ((size_t) (a + b + 1));
  if (o)
    {
      memcpy (o, dn, (size_t) a);
      memcpy (o + a, add, (size_t) b + 1);
    }
  pd_free (dn);
  return o;
}

static void
rederive (node *dir)
{
  for (node *c = dir->child; c; c = c->next)
    {
      derive_names (c);
      if (c->child)
        rederive (c);
    }
  fix_clashes (dir);
}

void
pdfs_set_hide_ext (int on)
{
  pdfs_hide_ext = on != 0;
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        rederive (&devices[d].st[s].root);
}

/* --- listing directories ------------------------------------------------------------- */

typedef struct
{
  uint32_t handle, parent, storage;
  uint16_t format;
  uint8_t have_name;
  uint64_t size;
  uint32_t mtime;
  char name[256];
} entry;

static void
apply_entry (node *dir, entry *e, uint32_t mark)
{
  node *n;
  for (n = dir->child; n; n = n->next)
    if (n->handle == e->handle)
      break;
  if (!n)
    {
      n = new_node (dir, e->name);
      if (!n)
        return;
      n->handle = e->handle;
    }
  else if (!exact_eq (n->mtp_name, e->name, (int) strlen (e->name)))
    set_name (n, e->name);
  if (n->isdir != (e->format == FMT_Association))
    {
      n->isdir = e->format == FMT_Association;
      derive_names (n);
    }
  n->size = e->size;
  n->mtime = e->mtime;
  n->seen = mark;
}

/* Parse a GetObjectPropList dataset into entries. */
static int
parse_proplist (const uint8_t *p, uint32_t len, entry **out, int *count)
{
  const uint8_t *end = p + len;
  if (len < 4)
    return -1;
  uint32_t nel = get32 (p);
  p += 4;
  entry *es = 0;
  int ne = 0, cap = 0, cur = -1;
  for (uint32_t i = 0; i < nel; i++)
    {
      if (p + 8 > end)
        goto bad;
      uint32_t h = get32 (p);
      uint16_t prop = get16 (p + 4), dt = get16 (p + 6);
      p += 8;
      if (cur < 0 || es[cur].handle != h)
        {
          for (cur = 0; cur < ne && es[cur].handle != h; cur++)
            ;
          if (cur == ne)
            {
              if (ne == cap)
                {
                  int nc = cap ? cap * 2 : 64;
                  entry *n2 = pd_realloc (es, sizeof (entry) * (size_t) nc);
                  if (!n2)
                    goto bad;
                  es = n2;
                  cap = nc;
                }
              memset (&es[ne], 0, sizeof (entry));
              es[ne].handle = h;
              es[ne].format = FMT_Undefined;
              ne++;
            }
        }
      entry *e = &es[cur];
      uint64_t v = 0;
      int sz;
      switch (dt)
        {
        case 1: case 2: sz = 1; break;
        case 3: case 4: sz = 2; break;
        case 5: case 6: sz = 4; break;
        case 7: case 8: sz = 8; break;
        case 9: case 10: sz = 16; break;
        case DT_STR: sz = 0; break;
        default:
          if (dt & 0x4000)
            {
              /* an array: count, then elements */
              int el = (dt & 0xF) <= 2 ? 1 : (dt & 0xF) <= 4 ? 2 : (dt & 0xF) <= 6 ? 4 : (dt & 0xF) <= 8 ? 8 : 16;
              if (p + 4 > end)
                goto bad;
              uint32_t k = get32 (p);
              p += 4 + (size_t) k * (size_t) el;
              continue;
            }
          goto bad;
        }
      if (dt == DT_STR)
        {
          char tmp[256];
          int k = mtp_get_string (p, end, tmp, sizeof tmp);
          if (k < 0)
            goto bad;
          p += k;
          if (prop == PROP_ObjectFileName)
            {
              str_copy (e->name, tmp, sizeof e->name);
              e->have_name = 1;
            }
          else if (prop == PROP_DateModified)
            e->mtime = parse_mtp_date (tmp);
          continue;
        }
      if (p + sz > end)
        goto bad;
      if (sz == 1) v = p[0];
      else if (sz == 2) v = get16 (p);
      else if (sz == 4) v = get32 (p);
      else if (sz >= 8) v = get64 (p);
      p += sz;
      switch (prop)
        {
        case PROP_StorageID: e->storage = (uint32_t) v; break;
        case PROP_ObjectFormat: e->format = (uint16_t) v; break;
        case PROP_ObjectSize: e->size = v; break;
        case PROP_ParentObject: e->parent = (uint32_t) v; break;
        }
    }
  *out = es;
  *count = ne;
  return 0;
bad:
  pd_free (es);
  return -1;
}

/* Parse an ObjectInfo dataset into an entry. */
static int
parse_objectinfo (const uint8_t *p, uint32_t len, entry *e)
{
  const uint8_t *end = p + len;
  char tmp[64];
  if (len < 53)
    return -1;
  e->storage = get32 (p);
  e->format = get16 (p + 4);
  e->size = get32 (p + 8);
  e->parent = get32 (p + 38);
  p += 52;
  int k = mtp_get_string (p, end, e->name, sizeof e->name);
  if (k < 0)
    return -1;
  e->have_name = 1;
  p += k;
  k = mtp_get_string (p, end, tmp, sizeof tmp);          /* created */
  if (k < 0)
    return 0;
  p += k;
  if (mtp_get_string (p, end, tmp, sizeof tmp) > 1)      /* modified */
    e->mtime = parse_mtp_date (tmp);
  return 0;
}

/* Sort handles (shell sort: no C library) and find one */
static void
sort_handles (uint32_t *h, uint32_t n)
{
  for (uint32_t gap = n / 2; gap; gap /= 2)
    for (uint32_t i = gap; i < n; i++)
      for (uint32_t j = i; j >= gap && h[j - gap] > h[j]; j -= gap)
        {
          uint32_t t = h[j];
          h[j] = h[j - gap];
          h[j - gap] = t;
        }
}

static int
has_handle (const uint32_t *h, uint32_t n, uint32_t x)
{
  uint32_t lo = 0, hi = n;
  while (lo < hi)
    {
      uint32_t mid = lo + (hi - lo) / 2;
      if (h[mid] < x)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo < n && h[lo] == x;
}

/* List a directory from the device.

   GetObjectHandles (this storage, this parent) says which objects are in
   it; GetObjectPropList at depth 1 gives their names, sizes and dates in
   one go. Not every device's property list can be trusted on its own: on
   some Android SD cards it lists the folder itself among its children (so
   a folder seemed to contain itself, and opening it went round in a loop)
   and leaves children out. So the handles decide what's there, and any
   the property list missed are asked about one at a time (GetObjectInfo).
   Big folders on slow cards can take the device a while to answer, so
   listing allows it 30 seconds. */
static const pd_error *
list_dir (node *dir)
{
  storage *st = dir->st;
  mtp_dev *m = &st->dev->mtp;
  entry *es = 0;
  int ne = 0, rc;
  int root = dir == &st->root;
  uint32_t parent = root ? 0xFFFFFFFFu : dir->handle;
  uint32_t *hs = 0, nh = 0;
  int have_handles = 0;
  const pd_error *err = 0;
  unsigned saved_timeout = m->timeout_cs;
  if (m->timeout_cs < 3000)
    m->timeout_cs = 3000;

  /* 1. what's in it */
  {
    uint32_t p[3] = { st->id, 0, parent };
    mtp_data in = { 0, 0, 0 };
    rc = mtp_transaction (m, OP_GetObjectHandles, p, 3, 0, 0, &in, 0);
    if (rc < 0)
      {
        pd_free (in.buf);
        err = rc_error (rc);
        goto out;
      }
    if (rc == RC_OK)
      {
        uint32_t n = in.len >= 4 ? get32 (in.buf) : 0;
        if (in.len < 4 + 4 * (uint64_t) n || !(hs = pd_alloc (4 * (size_t) (n ? n : 1))))
          {
            pd_free (in.buf);
            err = in.len < 4 + 4 * (uint64_t) n ? &err_proto : &err_nomem;
            goto out;
          }
        for (uint32_t i = 0; i < n; i++)
          {
            uint32_t h = get32 (in.buf + 4 + 4 * i);
            if (h != dir->handle || root)
              hs[nh++] = h;
          }
        sort_handles (hs, nh);
        have_handles = 1;
      }
    pd_free (in.buf);
  }

  /* 2. their names, sizes and dates, all at once if the device can */
  if (mtp_supports (m, OP_GetObjectPropList) && !m->no_proplist)
    {
      uint32_t p[5] = { root ? 0u : dir->handle, 0, 0xFFFFFFFFu, 0, 1 };
      mtp_data in = { 0, 0, 0 };
      /* 5 seconds at most: a device that takes longer gets folders listed
         an object at a time from now on */
      unsigned listing_cs = m->timeout_cs;
      if (have_handles && m->timeout_cs > 500)
        m->timeout_cs = 500;
      rc = mtp_transaction (m, OP_GetObjectPropList, p, 5, 0, 0, &in, 0);
      m->timeout_cs = listing_cs;
      if (rc == PDT_TIMEOUT && have_handles)
        {
          pd_log ("pdfs: the device didn't answer GetObjectPropList: folders are listed an object at a time now");
          m->no_proplist = 1;
        }
      if (rc == RC_OK)
        {
          int e = parse_proplist (in.buf, in.len, &es, &ne);
          pd_free (in.buf);
          if (e < 0)
            {
              err = &err_proto;
              goto out;
            }
          /* Keep this folder's children: by the handle list when there is
             one; otherwise by what each says of itself (not the folder,
             this storage, this parent). */
          int k = 0;
          for (int i = 0; i < ne; i++)
            {
              entry *x = &es[i];
              int keep;
              if (!x->have_name || (!root && x->handle == dir->handle))
                keep = 0;
              else if (have_handles)
                keep = has_handle (hs, nh, x->handle);
              else
                keep = (x->storage == 0 || x->storage == st->id)
                       && (root || x->parent == 0 || x->parent == dir->handle);
              if (keep)
                es[k++] = *x;
            }
          ne = k;
        }
      else
        {
          pd_free (in.buf);
          if (rc < 0 && !(rc == PDT_TIMEOUT && have_handles))
            {
              err = rc_error (rc);
              goto out;
            }
          ne = 0;                         /* refused (or too slow): object info for each */
        }
    }
  else if (!have_handles)
    {
      err = rc_error (rc);
      goto out;
    }

  /* 3. any the property list didn't give: one at a time */
  if (have_handles)
    {
      /* which handles are covered (es is in the device's order) */
      uint32_t *got = pd_alloc (4 * (size_t) (ne ? ne : 1));
      if (!got)
        {
          err = &err_nomem;
          goto out;
        }
      for (int i = 0; i < ne; i++)
        got[i] = es[i].handle;
      sort_handles (got, (uint32_t) ne);
      int nprop = ne;             /* got[] covers these; ne grows below */
      uint32_t missing = 0;
      for (uint32_t i = 0; i < nh; i++)
        missing += !has_handle (got, (uint32_t) ne, hs[i]);
      if (missing)
        {
          entry *n2 = pd_realloc (es, sizeof (entry) * (size_t) (ne + missing));
          if (!n2)
            {
              pd_free (got);
              err = &err_nomem;
              goto out;
            }
          es = n2;
          for (uint32_t i = 0; i < nh; i++)
            {
              if (has_handle (got, (uint32_t) nprop, hs[i]))
                continue;
              uint32_t h = hs[i];
              mtp_data oi = { 0, 0, 0 };
              rc = mtp_transaction (m, OP_GetObjectInfo, &h, 1, 0, 0, &oi, 0);
              memset (&es[ne], 0, sizeof (entry));
              if (rc == RC_OK && parse_objectinfo (oi.buf, oi.len, &es[ne]) == 0)
                {
                  es[ne].handle = h;
                  ne++;
                }
              pd_free (oi.buf);
              if (rc < 0)
                {
                  pd_free (got);
                  err = rc_error (rc);
                  goto out;
                }
            }
        }
      pd_free (got);
    }

  /* 4. into the directory cache. Never a folder inside itself or inside
     one of its own children (whatever the device says): an object that's
     this folder or one above it, or that says its parent is another
     folder, is left out (and logged) */
  {
    static unsigned seq;
    uint32_t mark = ++seq;
    if (pdfs_trace)
      {
        char b[120], *q = cat (b, "list ");
        q = cat_num (q, dir->handle);
        q = cat (q, ": ");
        q = cat_num (q, nh);
        q = cat (q, have_handles ? " handles, " : " (no handle list), ");
        q = cat_num (q, (uint64_t) ne);
        cat (q, " objects");
        pd_log (b);
      }
    for (int i = 0; i < ne; i++)
      {
        entry *x = &es[i];
        int bad = 0;
        for (node *a = dir; a && !bad; a = a->parent)
          bad = a != &st->root && a->handle == x->handle;
        if (!root && x->parent && x->parent != 0xFFFFFFFFu && x->parent != dir->handle)
          bad = 1;
        if (bad)
          {
            char b[340], *q = cat (b, "list: left out ");
            q = cat_num (q, x->handle);
            q = cat (q, " '");
            q = cat (q, x->name);
            q = cat (q, "', parent ");
            q = cat_num (q, x->parent);
            q = cat (q, ", in ");
            cat_num (q, dir->handle);
            pd_log (b);
            continue;
          }
        apply_entry (dir, x, mark);
      }
    pd_free (es);
    /* names shown with their suffixes because of a clash: check again */
    for (node *c = dir->child; c; c = c->next)
      if (c->sfx[0] && strlen (c->ro_name) == strlen (c->ro_full))
        derive_names (c);
    fix_clashes (dir);
    /* Children not seen have gone (except files we're still creating) */
    node *c = dir->child;
    while (c)
      {
        node *nx = c->next;
        if (c->seen != mark && !c->pending)
          drop_node (c);
        c = nx;
      }
    dir->listed = 1;
    dir->listed_at = pd_time_cs ();
    es = 0;
  }
out:
  m->timeout_cs = saved_timeout;
  pd_free (es);
  pd_free (hs);
  return err;
}

static const pd_error *
ensure_listed (node *dir)
{
  if (dir->pending)
    return 0;                   /* a new directory, still to be made on the device */
  if (dir->listed && pd_time_cs () - dir->listed_at < pdfs_dir_ttl_cs)
    return 0;
  if (dir->st->dev->mtp.streaming && dir->listed)   /* busy receiving a file: what we know */
    return 0;
  return list_dir (dir);
}

static void
invalidate_all (node *n)
{
  for (node *c = n; c; c = c->next)
    {
      if (c->listed)
        c->listed_at = pd_time_cs () - pdfs_dir_ttl_cs - 1;
      if (c->child)
        invalidate_all (c->child);
    }
}

/* --- paths ------------------------------------------------------------------------- */

static storage *
find_disc (const char *name, int len)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        if (!st_hidden (&devices[d].st[s])
            && (len < 0 ? 1 : name_eq (devices[d].st[s].disc, name, len)))
          return &devices[d].st[s];
  return 0;
}

/* Resolve a FileSwitch path (":disc.$.a.b", or "$.a.b" for the first disc).
   *found = the object or 0; *dir = the directory it is (or would be) in;
   *leaf and *leaflen = its name. Errors only for a missing disc or directory on
   the way, or a device failure. */
static const pd_error *
resolve (const char *path, node **found, node **dir, const char **leaf, int *leaflen)
{
  const char *p = path;
  storage *st;
  *found = 0;
  *dir = 0;
  *leaf = 0;
  *leaflen = 0;
  if (*p == ':')
    {
      const char *q = ++p;
      while (*q && *q != '.')
        q++;
      st = find_disc (p, (int) (q - p));
      if (!st)
        return &err_nodisc;
      p = *q ? q + 1 : q;
    }
  else
    {
      st = find_disc (0, -1);
      if (!st)
        return &err_nodevices;
    }
  if (*p == '$')
    {
      p++;
      if (*p == '.')
        p++;
    }
  node *cur = &st->root;
  if (!*p)
    {
      *found = cur;
      *dir = 0;
      return 0;
    }
  for (;;)
    {
      const char *q = p;
      while (*q && *q != '.')
        q++;
      int len = (int) (q - p);
      if (len == 0)
        return &err_badname;
      if (!cur->isdir && cur != &st->root)
        return &err_notdir;
      const pd_error *e = ensure_listed (cur);
      if (e)
        return e;
      node *c;
      for (c = cur->child; c; c = c->next)
        if (name_eq (c->ro_name, p, len))
          break;
      if (!c)                 /* "Letter/txt" for "Letter" */
        for (c = cur->child; c; c = c->next)
          if (c->sfx[0] && name_eq (c->ro_full, p, len))
            break;
      if (!*q)
        {
          *found = c;
          *dir = cur;
          *leaf = p;
          *leaflen = len;
          return 0;
        }
      if (!c)
        return &err_notfound;
      cur = c;
      p = q + 1;
    }
}

static void drain (device *dv);

/* resolve, but first wait for the object to finish being sent if it's
   being sent in the background (it can't be changed meanwhile) */
static const pd_error *
resolve_idle (const char *path, node **found, node **dir, const char **leaf, int *leaflen)
{
  const pd_error *e = resolve (path, found, dir, leaf, leaflen);
  if (!e && *found && (*found)->sending)
    {
      drain ((*found)->st->dev);
      e = resolve (path, found, dir, leaf, leaflen);
    }
  return e;
}

/* --- device operations -------------------------------------------------------------- */

static mtp_dev *
dev_of (node *n)
{
  return &n->st->dev->mtp;
}

/* Android's SendPartialObject, in two parts if one would need a zero-length
   packet the transport can't send. data has HEADROOM bytes before it. */
static int
send_partial (mtp_dev *m, uint32_t h, uint64_t off, ofile *f, uint64_t from, uint32_t len)
{
  uint32_t first = mtp_needs_zlp (m, len) && len > 1 ? len - 1 : len;
  uint32_t sp[4] = { h, (uint32_t) off, (uint32_t) (off >> 32), first };
  fill_src src = { f, from };
  int rc = mtp_transaction_fill (m, OP_SendPartialObject, sp, 4, first, fill_from, &src, 0);
  if (rc == RC_OK && first < len)
    {
      uint64_t o2 = off + first;
      uint32_t sp2[4] = { h, (uint32_t) o2, (uint32_t) (o2 >> 32), len - first };
      fill_src src2 = { f, from + first };
      rc = mtp_transaction_fill (m, OP_SendPartialObject, sp2, 4, len - first, fill_from, &src2, 0);
    }
  return rc;
}

static int
can_edit (mtp_dev *m)
{
  return mtp_supports (m, OP_BeginEditObject) && mtp_supports (m, OP_SendPartialObject)
         && mtp_supports (m, OP_TruncateObject) && mtp_supports (m, OP_EndEditObject);
}

/* Name a new object (with its type suffix) and send its ObjectInfo, for a
   directory or a file of len bytes. Sets n->handle. */
static const pd_error *
send_info (node *n, uint32_t len)
{
  mtp_dev *m = dev_of (n);
  uint8_t info[HEADROOM + 52 + 3 * 512 + 16];
  uint8_t *p = info + HEADROOM;
  node *dir = n->parent;
  if (!dir)
    return &err_notfound;
  if (!n->isdir)
    {
      /* a new file: its name gets the suffix for its type ("Letter.txt") */
      char *dn = add_type_suffix (str_dup (n->mtp_name, -1), 0, node_filetype (n), 0);
      if (!dn)
        return &err_nomem;
      if (!exact_eq (n->mtp_name, dn, (int) strlen (dn)))
        {
          int t = node_filetype (n);
          if (set_name (n, dn))
            {
              pd_free (dn);
              return &err_nomem;
            }
          n->filetype = t == n->name_type ? -1 : t;
        }
      pd_free (dn);
    }
  memset (p, 0, 52);
  put32 (p, n->st->id);
  put16 (p + 4, n->isdir ? FMT_Association : FMT_Undefined);
  put32 (p + 8, n->isdir ? 0 : len);
  put32 (p + 38, dir == &n->st->root ? 0 : dir->handle);
  put16 (p + 42, n->isdir ? 1 : 0);            /* association type: generic folder */
  int k = 52;
  k += mtp_put_string (p + k, n->mtp_name);
  char date[20];
  format_mtp_date (n->mtime, date);
  k += mtp_put_string (p + k, date);           /* created */
  k += mtp_put_string (p + k, date);           /* modified */
  p[k++] = 0;                                  /* keywords */
  uint32_t params[2] = { n->st->id, dir == &n->st->root ? 0xFFFFFFFFu : dir->handle };
  uint32_t rp[5];
  int rc = mtp_transaction (m, OP_SendObjectInfo, params, 2, p, (uint32_t) k, 0, rp);
  if (rc != RC_OK)
    return rc_error (rc);
  n->handle = rp[2];
  n->pending = 0;
  return 0;
}

/* Create an object on the device: a directory, or a file with the contents
   of f (0: empty). Sets n->handle. */
static const pd_error *
send_object (node *n, ofile *f)
{
  mtp_dev *m = dev_of (n);
  uint32_t len = f ? (uint32_t) f->size : 0;
  uint8_t info[HEADROOM + 4];
  if (!n->parent)
    return &err_notfound;
  /* A file whose data phase would need a zero-length packet that can't be
     sent: send all but its last byte, then add that with an edit */
  uint32_t sendlen = len;
  if (!n->isdir && len > 1 && mtp_needs_zlp (m, len) && can_edit (m))
    sendlen = len - 1;
  const pd_error *e = send_info (n, sendlen);
  if (e || n->isdir)
    return e;
  int rc;
  if (sendlen)
    {
      fill_src src = { f, 0 };
      rc = mtp_transaction_fill (m, OP_SendObject, 0, 0, sendlen, fill_from, &src, 0);
    }
  else
    rc = mtp_transaction (m, OP_SendObject, 0, 0, info + HEADROOM, 0, 0, 0);
  if (rc == RC_OK && sendlen < len)
    {
      uint32_t h = n->handle;
      rc = mtp_transaction (m, OP_BeginEditObject, &h, 1, 0, 0, 0, 0);
      if (rc == RC_OK)
        {
          rc = send_partial (m, h, sendlen, f, sendlen, len - sendlen);
          int rc2 = mtp_transaction (m, OP_EndEditObject, &h, 1, 0, 0, 0, 0);
          if (rc == RC_OK)
            rc = rc2;
        }
    }
  n->size = len;
  return rc_error (rc);
}

static const pd_error *
delete_object (node *n)
{
  uint32_t p[2] = { n->handle, 0 };
  int rc = mtp_transaction (dev_of (n), OP_DeleteObject, p, 2, 0, 0, 0, 0);
  if (rc == RC_InvalidObjectHandle)
    rc = RC_OK;               /* already gone */
  return rc_error (rc);
}

/* Rename an object on the device (dn: the new device name). */
static const pd_error *
rename_object (node *n, const char *dn)
{
  mtp_dev *m = dev_of (n);
  if (!mtp_supports (m, OP_SetObjectPropValue))
    return &err_unsupported;
  uint8_t buf[HEADROOM + 1 + 2 * 256];
  if (strlen (dn) > 250)
    return &err_badname;
  int k = mtp_put_string (buf + HEADROOM, dn);
  uint32_t p[2] = { n->handle, PROP_ObjectFileName };
  int rc = mtp_transaction (m, OP_SetObjectPropValue, p, 2, buf + HEADROOM, (uint32_t) k, 0, 0);
  if (rc != RC_OK)
    return rc_error (rc);
  return set_name (n, dn) ? &err_nomem : 0;
}

/* Set a file's type: on the device that means a new suffix on its name
   ("Letter.txt" to "Letter.jpg"). If the device can't rename it, the type
   is only remembered here. */
static void
set_type (node *n, int t)
{
  if (n->isdir || t == node_filetype (n))
    return;
  if (!n->pending && n->handle && !n->st->readonly)
    {
      int base = (int) (strlen (n->mtp_name) - strlen (n->sfx));
      char *dn = add_type_suffix (str_dup (n->mtp_name, base), 0, t, 0);
      if (dn && !rename_object (n, dn))
        {
          pd_free (dn);
          n->filetype = t == n->name_type ? -1 : t;
          return;
        }
      pd_free (dn);
    }
  n->filetype = t == n->name_type ? -1 : t;
}

/* Write a buffered file back to the device. */
static const pd_error *
commit (ofile *f)
{
  node *n = f->n;
  mtp_dev *m = dev_of (n);
  const pd_error *e;
  if (f->size > MAX_WRITE)
    return &err_toobig;
  uint32_t len = (uint32_t) f->size;
  n->mtime = pd_unix_time ();

  if (!n->pending && n->handle && can_edit (m))
    {
      /* Android: edit the object in place, so it keeps its handle */
      uint32_t h = n->handle;
      int rc = mtp_transaction (m, OP_BeginEditObject, &h, 1, 0, 0, 0, 0);
      if (rc == RC_OK)
        {
          if (len)
            rc = send_partial (m, h, 0, f, 0, len);
          uint32_t tp[3] = { h, len, 0 };
          if (rc == RC_OK)
            rc = mtp_transaction (m, OP_TruncateObject, tp, 3, 0, 0, 0, 0);
          int rc2 = mtp_transaction (m, OP_EndEditObject, &h, 1, 0, 0, 0, 0);
          if (rc == RC_OK)
            rc = rc2;
          if (rc == RC_OK)
            {
              n->size = len;
              f->dirty = 0;
              return 0;
            }
        }
      if (rc < 0)
        return rc_error (rc);
      /* Edit refused: fall through to replacing the object */
    }
  if (!n->pending && n->handle)
    {
      e = delete_object (n);
      if (e)
        return e;
      n->handle = 0;
      n->pending = 1;
    }
  e = send_object (n, f);
  if (!e)
    f->dirty = 0;
  return e;
}

/* A file created with FSEntry_File 7 is held back until it is opened (as
   *Save does: create, open, write, close), so its contents go over once.
   Any other call sends it as it is. */
static const pd_error *
flush_pending (node *keep)
{
  node *n = pending_create;
  if (!n || n == keep)
    return 0;
  pending_create = 0;
  if (n->gone || !n->parent)
    return 0;
  for (ofile *f = files; f; f = f->next)
    if (f->n == n)
      return 0;               /* open: sent when closed */
  ofile tmp;
  memset (&tmp, 0, sizeof tmp);
  tmp.n = n;
  tmp.size = n->size;           /* no buffer: reads as zeros */
  if (n->size > MAX_WRITE)
    return &err_toobig;
  return commit (&tmp);
}

/* --- open files ----------------------------------------------------------------------- */

static ofile *
find_file (pd_reg h)
{
  for (ofile *f = files; f; f = f->next)
    if ((pd_reg) f == h)
      return f;
  return 0;
}

static const pd_error *
grow (ofile *f, uint64_t need)
{
  if (need > MAX_WRITE)
    return &err_toobig;
  if (f->wbuf && need <= f->wcap)
    return 0;
  uint32_t cap = f->wcap ? f->wcap : 4096;
  while (cap < need)
    cap *= 2;
  uint8_t *b = pd_realloc (f->wbuf, (size_t) cap + HEADROOM);
  if (!b)
    return &err_nomem;
  memset (b + HEADROOM + f->wcap, 0, cap - f->wcap);
  if (!f->wbuf)
    memset (b, 0, HEADROOM);
  f->wbuf = b;
  f->wcap = cap;
  return 0;
}

/* Read [off, off+len) of the object into dst; *got = bytes read. */
/* --- a file's contents while it's open for writing: in memory, or in a
   scrap file once it passes SPILL_AT ---------------------------------------- */

static const pd_error *
f_spill (ofile *f, uint64_t len)
{
  int h = pd_spill_open (len);
  if (!h)
    return &err_noscrap;
  if (f->size && pd_spill_write (h, 0, f->wbuf + HEADROOM, (uint32_t) f->size) < 0)
    {
      pd_spill_close (h);
      return &err_scrapio;
    }
  pd_free (f->wbuf);
  f->wbuf = 0;
  f->wcap = 0;
  f->spill = h;
  return 0;
}

/* Make the file len bytes long (new bytes are zero) */
static const pd_error *
f_setsize (ofile *f, uint64_t len)
{
  const pd_error *e;
  if (f->lazy)
    {
      f->lazy = 0;              /* it's all zeros: as if empty, then made len long */
      f->size = 0;
    }
  if (len > MAX_WRITE)
    return &err_toobig;
  if (!f->spill && len > SPILL_AT && (e = f_spill (f, len)))
    return e;
  if (f->spill)
    {
      if (len != f->size && pd_spill_setext (f->spill, len) < 0)
        return &err_scrapio;
    }
  else
    {
      if ((e = grow (f, len)))
        return e;
      if (len > f->size)
        memset (f->wbuf + HEADROOM + f->size, 0, (size_t) (len - f->size));
    }
  f->size = len;
  return 0;
}

static const pd_error *
f_write (ofile *f, uint64_t off, const void *src, uint32_t n)
{
  const pd_error *e;
  if (f->lazy && (e = f_setsize (f, f->size)))
    return e;
  if (off + n > f->size && (e = f_setsize (f, off + n)))
    return e;
  if (f->spill)
    {
      if (pd_spill_write (f->spill, off, src, n) < 0)
        return &err_scrapio;
    }
  else
    memcpy (f->wbuf + HEADROOM + off, src, n);
  f->dirty = 1;
  return 0;
}

/* Read n bytes at off (within the file's size) */
static int
f_read (ofile *f, uint64_t off, void *dst, uint32_t n)
{
  if (f->spill)
    {
      int k = pd_spill_read (f->spill, off, dst, n);
      if (k < 0)
        return -1;
      if ((uint32_t) k < n)
        memset ((uint8_t *) dst + k, 0, n - (uint32_t) k);
      return 0;
    }
  if (f->wbuf)
    memcpy (dst, f->wbuf + HEADROOM + off, n);
  else
    memset (dst, 0, n);     /* created with a size but never written */
  return 0;
}

static void
f_free (ofile *f)
{
  pd_free (f->tail);
  f->tail = 0;
  pd_free (f->wbuf);
  f->wbuf = 0;
  f->wcap = 0;
  if (f->spill)
    pd_spill_close (f->spill);   /* deletes the scrap file */
  f->spill = 0;
}

/* For mtp_transaction_fill: the file from base on */

static int
fill_from (void *ctx, uint8_t *dst, uint64_t off, uint32_t len)
{
  fill_src *fs = ctx;
  return f_read (fs->f, fs->base + off, dst, len);
}

static const pd_error *
read_object (node *n, uint64_t off, uint8_t *dst, uint32_t len, uint32_t *got)
{
  mtp_dev *m = dev_of (n);
  uint32_t rp[5] = { 0 };
  mtp_data in = { dst, len, 0 };
  int rc;
  *got = 0;
  if (mtp_supports (m, OP_GetPartialObject64))
    {
      uint32_t p[4] = { n->handle, (uint32_t) off, (uint32_t) (off >> 32), len };
      rc = mtp_transaction (m, OP_GetPartialObject64, p, 4, 0, 0, &in, rp);
    }
  else if (mtp_supports (m, OP_GetPartialObject) && off + len <= 0xFFFFFFFFu)
    {
      uint32_t p[3] = { n->handle, (uint32_t) off, len };
      rc = mtp_transaction (m, OP_GetPartialObject, p, 3, 0, 0, &in, rp);
    }
  else
    return &err_unsupported;
  if (rc != RC_OK)
    return rc_error (rc);
  *got = in.len;
  return 0;
}

/* Load the whole object into f->wbuf (for update, or a device without
   partial reads). */
static const pd_error *
load_whole (ofile *f)
{
  node *n = f->n;
  const pd_error *e;
  uint64_t size = f->size;
  int dirty = f->dirty;
  f->loaded = 1;
  f->size = 0;
  if ((e = f_setsize (f, size)))
    return e;
  if (n->pending || !n->handle || size == 0)
    return 0;
  mtp_dev *m = dev_of (n);
  if (mtp_supports (m, OP_GetPartialObject64) || mtp_supports (m, OP_GetPartialObject))
    {
      uint8_t *tmp = pd_alloc (READAHEAD);
      if (!tmp)
        return &err_nomem;
      uint64_t off = 0;
      while (off < size)
        {
          uint32_t want = size - off > READAHEAD ? READAHEAD : (uint32_t) (size - off), got;
          e = read_object (n, off, tmp, want, &got);
          if (!e && got)
            e = f_write (f, off, tmp, got);
          if (e || !got)
            break;
          off += got;
        }
      pd_free (tmp);
      f->dirty = dirty;
      return e;
    }
  /* No partial reads: the whole object comes as one data phase, received a
     piece at a time (into memory or a scrap file, as big as it is) */
  uint32_t h = n->handle;
  uint64_t total;
  int rc = mtp_rstream_begin (m, OP_GetObject, &h, 1, &total);
  if (rc != RC_OK)
    return rc_error (rc);
  uint8_t *tmp = pd_alloc (READAHEAD);
  e = tmp ? 0 : &err_nomem;
  for (uint64_t off = 0; !e && off < size; )
    {
      uint32_t want = size - off > READAHEAD ? READAHEAD : (uint32_t) (size - off);
      int got = mtp_rstream_get (m, tmp, want);
      if (got <= 0)
        e = got < 0 ? rc_error (got) : &err_io;
      else
        {
          e = f_write (f, off, tmp, (uint32_t) got);
          off += (uint32_t) got;
        }
    }
  pd_free (tmp);
  rc = mtp_rstream_end (m);
  if (!e && rc != RC_OK)
    e = rc_error (rc);
  f->dirty = dirty;
  return e;
}

/* Read from a device that can't send part of an object: its whole object
   comes as one data phase, received only as far as the reader has asked
   (Filer_Action reads in blocks it times, so the desktop keeps going and
   its window shows the copy). Reading on from there is cheap; going back
   starts the object again. */
static const pd_error *
read_streamed (ofile *f, uint64_t off, uint8_t *dst, uint32_t len)
{
  mtp_dev *m = dev_of (f->n);
  if (!(m->rstreaming && m->rgen == f->rgen) || off < f->rpos)
    {
      uint32_t h = f->n->handle;
      uint64_t total;
      int rc = mtp_rstream_begin (m, OP_GetObject, &h, 1, &total);
      if (rc != RC_OK)
        return rc_error (rc);
      f->rgen = m->rgen;
      f->rpos = 0;
    }
  while (f->rpos < off)
    {
      uint64_t skip = off - f->rpos;
      int got = mtp_rstream_get (m, 0, skip > 0x100000 ? 0x100000 : (uint32_t) skip);
      if (got <= 0)
        return got < 0 ? rc_error (got) : &err_io;
      f->rpos += (uint32_t) got;
    }
  while (len)
    {
      int got = mtp_rstream_get (m, dst, len);
      if (got <= 0)
        return got < 0 ? rc_error (got) : &err_io;
      f->rpos += (uint32_t) got;
      dst += got;
      len -= (uint32_t) got;
    }
  if (m->rstreaming && m->rgen == f->rgen && m->rleft == 0)
    {
      int rc = mtp_rstream_end (m);
      if (rc != RC_OK)
        return rc_error (rc);
    }
  return 0;
}

static const pd_error *
fs_open (pd_regs *r)
{
  node *n, *dir;
  const char *leaf;
  int leaflen;
  int reason = (int) r->r[0];
  const pd_error *e = resolve_idle ((const char *) r->r[1], &n, &dir, &leaf, &leaflen);
  if (e)
    return e;
  if (!n && reason != 1)
    {
      r->r[1] = 0;            /* not found */
      return 0;
    }
  if (n && n->isdir && reason != 0)
    return &err_isdir;
  if (reason != 0 && (n ? n->st : dir->st)->readonly)
    return &err_readonly;
  if (!n)
    {
      /* Create and open for update */
      flush_pending (0);
      char *dn = to_device_name (leaf, leaflen);
      if (!dn)
        return &err_nomem;
      n = new_node (dir, dn);
      pd_free (dn);
      if (!n)
        return &err_nomem;
      n->pending = 1;
      n->mtime = pd_unix_time ();
    }
  else if (n != pending_create)
    flush_pending (0);
  if (n == pending_create)
    pending_create = 0;
  ofile *f = pd_alloc (sizeof *f);
  if (!f)
    return &err_nomem;
  f->n = n;
  n->refs++;
  f->writable = reason != 0;
  f->fs_handle = (uint32_t) r->r[3];
  f->size = n->isdir ? 0 : n->size;
  if (reason == 1 && !n->pending)
    {
      /* OPENOUT on an existing file: start it empty */
      f->size = 0;
      f->loaded = 1;
      f->dirty = 1;
    }
  if (n->pending)
    {
      f->loaded = 1;
      f->dirty = 1;
      uint64_t want = f->size;
      f->size = 0;
      if (want > STREAM_AT && !pdfs_background)
        {
          f->size = want;       /* storage when it's first written, unless streamed */
          f->lazy = 1;
        }
      else if (want && (e = f_setsize (f, want)))
        {
          n->refs--;
          pd_free (f);
          return e;
        }
    }
  f->next = files;
  files = f;
  r->r[0] = (pd_reg) (0x40000000u | (f->writable ? 0x80000000u : 0) | (n->isdir ? 0x20000000u : 0));
  r->r[1] = (pd_reg) f;
  r->r[2] = n->isdir ? 0 : BUFSIZE;
  r->r[3] = (pd_reg) (uint32_t) f->size;
  r->r[4] = (pd_reg) ((f->size + BUFSIZE - 1) & ~(uint64_t) (BUFSIZE - 1));
  return 0;
}

/* --- streaming a big file to the device as it's written ------------------------------
   Filer_Action and FileSwitch's *Copy create a file with its size (FSEntry_
   File 7), open it, and write it from the start. Sending it as it comes,
   not all at the close, is what makes it act as other filing systems do:
   each write takes as long as the device does, so Filer_Action's window
   shows the copy, and the desktop runs between its writes.

   FileSwitch doesn't pass the writes quite in order: a 4K buffer it's
   filling can go after data beyond it. So what's written is kept (as any
   file being written is: memory, or a scrap file), each 4K block written is
   marked, and the file goes to the device as far as it's been written
   without a gap. */

#define SBLOCK      4096

static int
block_done (ofile *f, uint64_t blk)
{
  return (f->tail[blk >> 3] >> (blk & 7)) & 1;
}

/* Send what's written from f->sent on, up to the first gap (all: up to
   the end, gaps as zeros, at the close) */
static const pd_error *
stream_pump (ofile *f, int all)
{
  mtp_dev *m = dev_of (f->n);
  uint8_t buf[2048];
  while (f->sent < f->slen && f->stream == 1)
    {
      if (!all && !block_done (f, f->sent / SBLOCK))
        break;
      /* the run of written blocks from here (all: to the end) */
      uint64_t end = f->sent;
      while (end < f->slen && (all || block_done (f, end / SBLOCK)))
        end = (end / SBLOCK + 1) * SBLOCK;
      if (end > f->slen)
        end = f->slen;
      while (f->sent < end)
        {
          uint32_t k = end - f->sent > sizeof buf ? sizeof buf : (uint32_t) (end - f->sent);
          if (f->sent >= f->size)
            memset (buf, 0, k);                   /* never written: zeros */
          else
            {
              uint32_t have = f->size - f->sent < k ? (uint32_t) (f->size - f->sent) : k;
              if (f_read (f, f->sent, buf, have) < 0)
                {
                  f->stream = 3;
                  return &err_scrapio;
                }
              if (have < k)
                memset (buf + have, 0, k - have);
            }
          int rc = mtp_stream_put (m, buf, k);
          if (rc < 0)
            {
              f->stream = 3;                      /* the transfer failed */
              return rc_error (rc);
            }
          f->sent += k;
        }
    }
  return 0;
}

static const pd_error *
stream_start (ofile *f)
{
  node *n = f->n;
  mtp_dev *m = dev_of (n);
  /* a data phase that would need a zero-length packet: all but the last
     byte, which goes by an edit at the close */
  uint32_t slen = n->decl > 1 && mtp_needs_zlp (m, n->decl) ? n->decl - 1 : n->decl;
  pd_free (f->tail);
  f->tail = pd_alloc (n->decl / SBLOCK / 8 + 2);     /* the blocks written */
  if (!f->tail)
    return &err_nomem;
  const pd_error *e = send_info (n, slen);
  if (e)
    return e;
  f->slen = slen;
  int rc = mtp_stream_begin (m, OP_SendObject, 0, 0, slen);
  if (rc < 0)
    return rc_error (rc);
  f->stream = 1;
  f->sent = 0;
  return 0;
}

static const pd_error *
stream_put (ofile *f, uint64_t off, const uint8_t *src, uint32_t len)
{
  const pd_error *e;
  if (f->stream != 1)
    {
      if (off + len > f->size)
        f->size = off + len;
      return 0;
    }
  if (off < f->sent)
    {
      /* written again where it's already gone: fine if it's the same
         (FileSwitch writing its buffer out twice) */
      uint64_t end = off + len < f->sent ? off + len : f->sent;
      uint8_t buf[512];
      for (uint64_t o = off; o < end; )
        {
          uint32_t k = end - o > sizeof buf ? sizeof buf : (uint32_t) (end - o);
          uint32_t have = o >= f->size ? 0 : (f->size - o < k ? (uint32_t) (f->size - o) : k);
          memset (buf, 0, k);
          if (have && f_read (f, o, buf, have) < 0)
            return &err_scrapio;
          if (memcmp_bytes (buf, src + (o - off), k))
            {
              f->stream = 2;
              return 0;
            }
          o += k;
        }
    }
  if ((e = f_write (f, off, src, len)))
    {
      f->stream = 3;
      return e;
    }
  /* mark the blocks it fills (to the end of the file, for the last) */
  uint64_t end = off + len > f->n->decl ? f->n->decl : off + len;
  for (uint64_t blk = off / SBLOCK; blk * SBLOCK < end; blk++)
    {
      uint64_t b0 = blk * SBLOCK, b1 = b0 + SBLOCK < f->n->decl ? b0 + SBLOCK : f->n->decl;
      if (off <= b0 && end >= b1)
        f->tail[blk >> 3] |= (uint8_t) (1 << (blk & 7));
    }
  return stream_pump (f, 0);
}

/* At the close: finish the transfer; the file must be the size it was
   created with (Android can trim it if it's shorter). */
static const pd_error *
stream_finish (ofile *f)
{
  node *n = f->n;
  mtp_dev *m = dev_of (n);
  uint32_t rp[5];
  if (f->stream == 1)
    stream_pump (f, 1);         /* the rest (any gap left: zeros) */
  int rc = m->streaming ? mtp_stream_end (m, rp) : PDT_IO;
  int state = f->stream;
  f->stream = 0;                /* (the tail is freed with the file, at the close) */
  if (rc != RC_OK || state != 1 || f->size > n->decl)
    {
      if (n->handle)
        delete_object (n);
      n->handle = 0;
      n->pending = 1;
      n->size = 0;
      if (rc < 0 && rc != MTP_EBUSY)
        return rc_error (rc);
      return state == 2 ? &err_order : rc != RC_OK ? rc_error (rc) : &err_stream_size;
    }
  if (f->slen < n->decl && f->size >= n->decl)
    {
      /* the last byte, by an edit */
      uint64_t last = n->decl - 1;
      uint8_t b[HEADROOM + 1];
      uint32_t h = n->handle, sp[4] = { h, (uint32_t) last, (uint32_t) (last >> 32), 1 };
      int ok = f_read (f, last, b + HEADROOM, 1) == 0;
      if (ok)
        {
          ok = (rc = mtp_transaction (m, OP_BeginEditObject, &h, 1, 0, 0, 0, 0)) == RC_OK;
        }
      if (ok)
        {
          rc = mtp_transaction (m, OP_SendPartialObject, sp, 4, b + HEADROOM, 1, 0, 0);
          int rc2 = mtp_transaction (m, OP_EndEditObject, &h, 1, 0, 0, 0, 0);
          ok = rc == RC_OK && rc2 == RC_OK;
        }
      if (!ok)
        {
          delete_object (n);
          n->handle = 0;
          n->pending = 1;
          return &err_stream_size;
        }
    }
  if (f->size < n->decl)
    {
      uint32_t h = n->handle, tp[3] = { h, (uint32_t) f->size, 0 };
      if (!can_edit (m)
          || (rc = mtp_transaction (m, OP_BeginEditObject, &h, 1, 0, 0, 0, 0)) != RC_OK
          || (rc = mtp_transaction (m, OP_TruncateObject, tp, 3, 0, 0, 0, 0),
              mtp_transaction (m, OP_EndEditObject, &h, 1, 0, 0, 0, 0), rc) != RC_OK)
        {
          delete_object (n);
          n->handle = 0;
          n->pending = 1;
          return &err_stream_size;
        }
    }
  n->size = f->size;
  n->mtime = pd_unix_time ();
  f->dirty = 0;
  return 0;
}

/* --- sending big files in the background -----------------------------------------------
   Filer_Action saves a file that fits in its memory with one OS_File Save,
   which FileSwitch passes on as one PutBytes and a close inside the one SWI:
   sending it there would hold up the whole desktop until the device had it
   all. So when the filer task is running (pdfs_background), a new file of
   BG_AT bytes or more is kept as it's written (in memory, or a scrap file:
   pd_spill_*), and closing it returns at once; the filer task sends it a
   piece at a time (pdfs_bg_run) on null events. Anything else that needs
   the device meanwhile waits for it (drain, from mtp's busy_wait), except
   listings, which give what's known. */

#define BG_AT     (1u << 20)
#define BG_PIECE  65536
#define BG_QUEUE  24             /* files waiting at most (each holds its contents); more: wait for the first */

typedef struct job job;
struct job
{
  job *next;
  ofile *f;                     /* the closed file: its contents (0: a directory) */
  node *n;
  device *dv;
  int closing;                  /* all sent: waiting for the device's answer */
  uint32_t owner;               /* the task that wrote it (Filer_Action, say) */
  unsigned wait_from;
  uint32_t sendlen;             /* bytes in the SendObject (the last may go by an edit) */
  uint64_t sent;
  int started;
  uint8_t *buf;
};

static job *jobs;
static int in_job;
static int bg_paused;
static uint64_t bg_batch, bg_batch_done;   /* bytes queued since the queue was empty, sent */
static int bg_files_done;
static pd_error bg_err;
static int bg_err_set;

static void
bg_failed (node *n, const pd_error *e)
{
  char *p = bg_err.errmess;
  p = cat (p, "PDFS couldn't send '");
  for (int i = 0; n->ro_name && n->ro_name[i] && p < bg_err.errmess + 120; i++)
    *p++ = n->ro_name[i];
  p = cat (p, "' to the device: ");
  for (int i = 0; e->errmess[i] && p < bg_err.errmess + sizeof bg_err.errmess - 1; i++)
    *p++ = e->errmess[i];
  *p = 0;
  bg_err.errnum = e->errnum;
  bg_err_set = 1;
  pd_log (bg_err.errmess);
}

static void
job_free (job *j)
{
  ofile *f = j->f;
  node *n = j->n;
  n->sending = 0;
  if (f)
    {
      bg_batch_done += f->size;
      bg_files_done++;
    }
  if (f)
    {
      f_free (f);
      pd_free (f->rbuf);
      pd_free (f);
    }
  pd_free (j->buf);
  pd_free (j);
  n->decl = 0;
  if (--n->refs == 0 && n->gone)
    drop_node (n);
}

/* Send some of a job, till the time 'until' (or all of it, until = 0).
   Returns 1 when it's finished (sent, or failed and reported). */
static int
job_step (job *j, unsigned until)
{
  ofile *f = j->f;
  node *n = j->n;
  mtp_dev *m = dev_of (n);
  const pd_error *e = 0;
  uint32_t rp[5];
  int rc;
  if (n->gone || !n->parent)
    return 1;
  in_job = 1;
  if (!f)
    {
      if (n->parent->pending)
        e = &err_notfound;        /* its parent couldn't be made */
      else
        e = send_object (n, 0);   /* a directory */
      in_job = 0;
      if (e)
        {
          bg_failed (n, e);
          if (n->refs == 1)
            drop_node (n);
        }
      return 1;
    }
  if (!j->started)
    {
      uint32_t len = (uint32_t) f->size;
      j->sendlen = len > 1 && mtp_needs_zlp (m, len) && can_edit (m) ? len - 1 : len;
      j->started = 1;
      n->mtime = pd_unix_time ();
      if (n->parent->pending)
        {
          e = &err_notfound;      /* its directory couldn't be made */
          goto done;
        }
      if (mtp_needs_zlp (m, j->sendlen) || !(j->buf = pd_alloc (BG_PIECE)))
        {
          e = send_object (n, f);         /* the way that needs no streaming */
          goto done;
        }
      if ((e = send_info (n, j->sendlen)))
        goto failed;
      rc = mtp_stream_begin (m, OP_SendObject, 0, 0, j->sendlen);
      if (rc < 0)
        {
          e = rc_error (rc);
          goto failed;
        }
    }
  while (j->sent < j->sendlen)
    {
      uint32_t k = j->sendlen - j->sent > BG_PIECE ? BG_PIECE : (uint32_t) (j->sendlen - j->sent);
      if (f_read (f, j->sent, j->buf, k) < 0)
        {
          e = &err_scrapio;
          goto failed;
        }
      if ((rc = mtp_stream_put (m, j->buf, k)) < 0)
        {
          e = rc_error (rc);
          goto failed;
        }
      j->sent += k;
      if (until && (int) (pd_time_cs () - until) >= 0)
        break;
    }
  if (j->sent < j->sendlen)
    {
      in_job = 0;
      return 0;
    }
  if (!j->closing)
    {
      if ((rc = mtp_stream_close (m)) < 0)
        {
          e = rc_error (rc);
          goto failed;
        }
      j->closing = 1;
      j->wait_from = pd_time_cs ();
    }
  /* the device's answer: it may take a while to finish writing the file */
  rc = mtp_stream_result (m, rp, until ? 0 : -1);
  if (rc == MTP_EAGAIN)
    {
      if (pd_time_cs () - j->wait_from < 6000)
        {
          in_job = 0;
          return 0;
        }
      rc = mtp_stream_result (m, rp, -1);
    }
  if (rc != RC_OK)
    {
      e = rc_error (rc);
      goto failed;
    }
  if (j->sendlen < f->size)
    {
      /* the last byte, by an edit (no zero-length packet needed) */
      uint32_t h = n->handle;
      rc = mtp_transaction (m, OP_BeginEditObject, &h, 1, 0, 0, 0, 0);
      if (rc == RC_OK)
        {
          rc = send_partial (m, h, j->sendlen, f, j->sendlen, (uint32_t) f->size - j->sendlen);
          int rc2 = mtp_transaction (m, OP_EndEditObject, &h, 1, 0, 0, 0, 0);
          if (rc == RC_OK)
            rc = rc2;
        }
      if (rc != RC_OK)
        {
          e = rc_error (rc);
          goto failed;
        }
    }
  n->size = f->size;
  f->dirty = 0;
  in_job = 0;
  return 1;

failed:
  if (m->streaming)
    mtp_stream_end (m, rp);       /* the rest as zeros; then it's deleted */
  if (n->parent && n->parent->pending)
    e = &err_notfound;            /* its directory couldn't be made */
  if (n->handle && !m->tr->gone)
    delete_object (n);
  n->handle = 0;
  n->pending = 1;
done:
  in_job = 0;
  if (e)
    {
      bg_failed (n, e);
      if (n->pending && n->refs == 1)
        drop_node (n);            /* not there: don't show it */
    }
  else
    n->size = f->size;
  return 1;
}

/* Finish every job for this device now */
static void
drain (device *dv)
{
  if (in_job)
    return;
  job **pp = &jobs;
  while (*pp)
    {
      job *j = *pp;
      if (j->dv != dv)
        {
          pp = &j->next;
          continue;
        }
      job_step (j, 0);
      *pp = j->next;
      job_free (j);
    }
}

static void
busy_wait (void *md)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used && &devices[d].mtp == md)
      drain (&devices[d]);
}

/* Drop the device's jobs (it has gone) */
static void
bg_abort (device *dv)
{
  job **pp = &jobs;
  while (*pp)
    {
      job *j = *pp;
      if (j->dv != dv)
        {
          pp = &j->next;
          continue;
        }
      if (j->n->ro_name)
        bg_failed (j->n, &err_gone);
      *pp = j->next;
      job_free (j);
    }
}

static int
bg_busy (device *dv)
{
  for (job *k = jobs; k; k = k->next)
    if (k->dv == dv)
      return 1;
  return 0;
}

static int
bg_queue (node *n, ofile *f)
{
  int queued = 0;
  for (job *k = jobs; k; k = k->next)
    queued += k->f != 0;
  while (f && queued >= BG_QUEUE && jobs)
    {
      /* enough waiting (each holds its file in memory): send the first */
      job *k = jobs;
      jobs = k->next;
      job_step (k, 0);
      queued -= k->f != 0;
      job_free (k);
    }
  job *j = pd_alloc (sizeof *j);
  if (!j)
    return 0;
  j->f = f;
  j->n = n;
  j->dv = n->st->dev;
  j->owner = pd_current_task ();
  n->sending = 1;
  if (!jobs)
    {
      bg_batch = bg_batch_done = 0;
      bg_files_done = 0;
    }
  if (f)
    bg_batch += f->size;
  job **pp = &jobs;
  while (*pp)
    pp = &(*pp)->next;
  *pp = j;
  return 1;
}

/* At the close: send the file in the background? (a big one; or any, while
   others are waiting to go to the same device, to keep them in order).
   Then the job has f, and the node's reference. */
static int
bg_take (ofile *f)
{
  node *n = f->n;
  if (!pdfs_background || !f->writable || !f->dirty || !n->pending || n->isdir || n->gone
      || f->size > MAX_WRITE || f->stream)
    return 0;
  if (f->size < BG_AT && !bg_busy (n->st->dev))
    return 0;
  if (!bg_queue (n, f))
    return 0;
  n->size = f->size;
  return 1;
}

/* A new directory while files are waiting to go: made in its turn */
static int
bg_mkdir (node *n)
{
  if (!pdfs_background || !bg_busy (n->st->dev) || !bg_queue (n, 0))
    return 0;
  n->refs++;
  return 1;
}

/* For the filer task: send for up to cs centiseconds. Returns 1 while there
   are files still to send. */
int
pdfs_bg_run (unsigned cs)
{
  unsigned until = (pd_time_cs () + cs) | 1;
  if (pdfs_reading ())
    {
      /* files coming from a device first: their readers wait. Short slices:
         the reader (Filer_Action) gets its turn often */
      unsigned short_until = (pd_time_cs () + (cs > FILL_SLICE ? FILL_SLICE : cs)) | 1;
      if (fill_run (short_until))
        return 1;
    }
  if (bg_paused)
    return jobs != 0;
  while (jobs)
    {
      job *j = jobs;
      if (!job_step (j, until))
        return 1;
      jobs = j->next;
      job_free (j);
      if ((int) (pd_time_cs () - until) >= 0)
        break;
    }
  return jobs != 0;
}

static char *
node_path (char *o, char *end, node *n)
{
  if (!n->parent)
    {
      o = cat (o, "PDFS::");
      o = cat (o, n->st->disc);
      return cat (o, ".$");
    }
  o = node_path (o, end, n->parent);
  if (o < end - 2)
    *o++ = '.';
  for (const char *p = n->ro_name; p && *p && o < end - 1; p++)
    *o++ = *p;
  *o = 0;
  return o;
}

void
pdfs_bg_status (pdfs_bg_state *s)
{
  memset (s, 0, sizeof *s);
  s->paused = bg_paused;
  s->batch = bg_batch;
  s->batch_done = bg_batch_done;
  s->files_done = bg_files_done;
  for (job *j = jobs; j; j = j->next)
    {
      if (!j->f)
        continue;
      s->active = 1;
      s->left += j->f->size - j->sent;
      s->batch_done += j->sent;
      if (!s->path[0] && j->n->parent)
        {
          s->owner = j->owner;
          char tmp[512];
          node_path (tmp, tmp + sizeof tmp - 1, j->n);
          int l = (int) strlen (tmp);
          const char *from = l > 255 ? tmp + l - 255 : tmp;   /* the end of a long one */
          memcpy (s->path, from, strlen (from) + 1);
        }
    }
  if (jobs)
    s->active = 1;
}

void
pdfs_bg_pause (int on)
{
  bg_paused = on;
}

/* Abort: everything waiting is dropped; the file being sent is cancelled
   on the device and deleted there */
void
pdfs_bg_abort_all (void)
{
  bg_paused = 0;
  while (jobs)
    {
      job *j = jobs;
      jobs = j->next;
      node *n = j->n;
      mtp_dev *m = dev_of (n);
      in_job = 1;
      if (j->f && j->started)
        {
          if (m->streaming)
            mtp_stream_abort (m);
          if (n->handle && !m->tr->gone)
            delete_object (n);
          n->handle = 0;
          n->pending = 1;
        }
      in_job = 0;
      if (j->f)
        j->f->size = 0;           /* not counted as sent */
      /* a directory made in its turn stays; a file never sent isn't shown */
      if (n->pending && n->refs == 1 && !n->gone && j->f)
        drop_node (n);
      else if (n->pending && !j->f && n->refs == 1 && !n->gone)
        drop_node (n);
      job_free (j);
      bg_files_done--;
    }
}

/* Send everything still waiting (the filer task is stopping) */
void
pdfs_bg_finish (void)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      drain (&devices[d]);
}

/* How far the sends to a storage have got (percent), or -1 if there are none */
int
pdfs_bg_progress (const char *disc)
{
  storage *st = find_disc (disc, (int) strlen (disc));
  uint64_t total = 0, done = 0;
  int any = 0;
  if (!st)
    return -1;
  for (job *j = jobs; j; j = j->next)
    if (j->n->st == st)
      {
        any = 1;
        if (j->f)
          {
            total += j->f->size;
            done += j->sent;
          }
      }
  if (!any)
    return -1;
  int pc = total ? (int) (done * 100 / total) : 0;
  return pc > 99 ? 99 : pc;
}

/* A background send that failed (once; 0 if none) */
const pd_error *
pdfs_bg_error (void)
{
  if (!bg_err_set)
    return 0;
  bg_err_set = 0;
  return &bg_err;
}

/* --- receiving a whole file in the background -------------------------------------

   From a device that can't send part of a file, a file is one long data
   phase. A read from FileSwitch can't let the desktop in, so instead of
   receiving it in reads (Filer_Action's), it's received into the file's
   storage (memory or a scrap file) a slice at a time from the filer's null
   events (pdfs_bg_run), and a read waits only for what hasn't come yet.
   Another request to the device while it's coming takes the rest first
   (fill_busy), rather than drop it. A file closed before it has all come:
   the rest is read and dropped in the background too (the device sends it
   anyway). */

static int fills;               /* files being received in the background */
static uint8_t *fill_tmp;

static int
can_read_parts (mtp_dev *m)
{
  return mtp_supports (m, OP_GetPartialObject64) || mtp_supports (m, OP_GetPartialObject);
}

static const pd_error *
fill_start (ofile *f)
{
  uint64_t size = f->size;
  const pd_error *e;
  if (!fill_tmp && !(fill_tmp = pd_alloc (READAHEAD)))
    return &err_nomem;
  f->size = 0;
  if ((e = f_setsize (f, size)))
    return e;
  f->dirty = 0;
  f->fill = 1;
  f->have = 0;
  f->rgen = 0;
  fills++;
  return 0;
}

static void
fill_done (ofile *f)
{
  f->fill = 0;
  f->loaded = 1;                /* all of it is in its storage now */
  f->dirty = 0;
  fills--;
}

/* Receive until there's up to bytes of it (or until the time until_cs, if
   not 0) */
static const pd_error *
fill_more (ofile *f, uint64_t upto, unsigned until_cs)
{
  mtp_dev *m = dev_of (f->n);
  if (upto > f->size)
    upto = f->size;
  if (!(m->rstreaming && m->rgen == f->rgen))
    {
      /* (again: the device was wanted for something it couldn't wait for) */
      if (f->have)
        pd_log_hex ("pdfs: a file coming whole from the device starts again; bytes to skip", (uint32_t) f->have);
      uint32_t h = f->n->handle;
      uint64_t total;
      int rc = mtp_rstream_begin (m, OP_GetObject, &h, 1, &total);
      if (rc != RC_OK)
        return rc_error (rc);
      f->rgen = m->rgen;
      for (uint64_t skip = f->have; skip; )
        {
          int got = mtp_rstream_get (m, 0, skip > READAHEAD ? READAHEAD : (uint32_t) skip);
          if (got <= 0)
            return got < 0 ? rc_error (got) : &err_io;
          skip -= (uint32_t) got;
        }
    }
  while (f->have < upto)
    {
      uint32_t want = f->size - f->have > READAHEAD ? READAHEAD : (uint32_t) (f->size - f->have);
      int got = mtp_rstream_get (m, fill_tmp, want);
      if (got <= 0)
        return got < 0 ? rc_error (got) : &err_io;
      const pd_error *e = f_write (f, f->have, fill_tmp, (uint32_t) got);
      f->dirty = 0;
      if (e)
        return e;
      f->have += (uint32_t) got;
      if (until_cs && (int) (pd_time_cs () - until_cs) >= 0)
        break;
    }
  if (f->have >= f->size)
    {
      int rc = m->rstreaming && m->rgen == f->rgen ? mtp_rstream_end (m) : RC_OK;
      fill_done (f);
      if (rc != RC_OK)
        return rc_error (rc);
    }
  return 0;
}

/* The device is wanted while a file is coming from it: take the rest of the
   file first (or, if it's no one's now, let it be dropped) */
static void
fill_busy (void *d)
{
  for (ofile *f = files; f; f = f->next)
    if (f->fill && &f->n->st->dev->mtp == d && ((mtp_dev *) d)->rgen == f->rgen)
      {
        pd_log_hex ("pdfs: the device is wanted while a file comes from it: taking the rest first, bytes",
                    (uint32_t) (f->size - f->have));
        fill_more (f, f->size, 0);
        return;
      }
}

/* A slice of background receiving: 1 if there's more to do */
static int
fill_run (unsigned until_cs)
{
  int more = 0;
  for (ofile *f = files; f; f = f->next)
    if (f->fill && !dev_of (f->n)->streaming)
      {
        const pd_error *e = fill_more (f, f->size, until_cs);
        if (e)
          {
            /* failed: the reads will say so, receiving it in the foreground */
            char b[200], *q = cat (b, "pdfs: receiving a file in the background: ");
            cat (q, e->errmess);
            pd_log (b);
            continue;
          }
        more |= f->fill;
        if ((int) (pd_time_cs () - until_cs) >= 0)
          return 1;
      }
  /* streams whose file was closed before the end: the rest is dropped */
  for (int d = 0; d < MAX_DEVICES; d++)
    {
      mtp_dev *m = &devices[d].mtp;
      if (!devices[d].used || !m->rstreaming)
        continue;
      int owned = 0;
      for (ofile *f = files; f; f = f->next)
        owned |= f->fill && dev_of (f->n) == m && f->rgen == m->rgen;
      if (owned)
        continue;
      while (m->rstreaming && m->rleft && (int) (pd_time_cs () - until_cs) < 0)
        if (mtp_rstream_get (m, 0, READAHEAD) <= 0)
          break;
      if (m->rstreaming && !m->rleft)
        mtp_rstream_end (m);
      more |= m->rstreaming;
    }
  return more || fills;
}

int
pdfs_reading (void)
{
  if (fills)
    return 1;
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used && devices[d].mtp.rstreaming)
      return 1;
  return 0;
}

static const pd_error *
fs_getbytes (pd_regs *r)
{
  ofile *f = find_file (r->r[1]);
  uint8_t *dst = (uint8_t *) r->r[2];
  uint32_t n = (uint32_t) r->r[3];
  uint64_t off = (uint32_t) r->r[4];
  const pd_error *e;
  if (!f)
    return &err_badhandle;
  /* FileSwitch reads whole buffers, past the end too: give zeros there */
  uint32_t avail = off >= f->size ? 0 : (f->size - off < n ? (uint32_t) (f->size - off) : n);
  if (avail < n)
    memset (dst + avail, 0, n - avail);
  if (!avail)
    return 0;
  if (f->stream)
    return f_read (f, off, dst, avail) < 0 ? &err_scrapio : 0;   /* it's all kept */
  if (f->writable || f->loaded)
    {
      if (!f->loaded && (e = load_whole (f)))
        return e;
      return f_read (f, off, dst, avail) < 0 ? &err_scrapio : 0;
    }
  if (!f->fill && f->size > READAHEAD && !can_read_parts (dev_of (f->n)) && !f->n->pending)
    {
      /* A device that can't send part of a file: it comes whole, and is
         received into the file's storage, mostly in the background
         (pdfs_bg_run, from the filer's null events), so the desktop keeps
         going; a read waits only for what isn't there yet */
      if ((e = fill_start (f)))
        return e;
    }
  if (f->fill)
    {
      if (off + avail > f->have && (e = fill_more (f, off + avail, 0)))
        return e;
      return f_read (f, off, dst, avail) < 0 ? &err_scrapio : 0;
    }
  while (avail)
    {
      if (f->rbuf && off >= f->roff && off < f->roff + f->rlen)
        {
          uint32_t k = (uint32_t) (f->roff + f->rlen - off);
          if (k > avail)
            k = avail;
          memcpy (dst, f->rbuf + (off - f->roff), k);
          dst += k;
          off += k;
          avail -= k;
          continue;
        }
      if (!f->rbuf && !(f->rbuf = pd_alloc (READAHEAD)))
        return &err_nomem;
      uint32_t want = f->size - off < READAHEAD ? (uint32_t) (f->size - off) : READAHEAD, got;
      e = read_object (f->n, off, f->rbuf, want, &got);
      if (e == &err_unsupported)
        return read_streamed (f, off, dst, avail);   /* no partial reads on this device */
      if (e)
        return e;
      if (!got)
        return &err_io;
      f->roff = off;
      f->rlen = got;
    }
  return 0;
}

static const pd_error *
fs_putbytes (pd_regs *r)
{
  ofile *f = find_file (r->r[1]);
  const uint8_t *src = (const uint8_t *) r->r[2];
  uint32_t n = (uint32_t) r->r[3];
  uint64_t off = (uint32_t) r->r[4];
  const pd_error *e;
  if (!f)
    return &err_badhandle;
  node *nd = f->n;
  if (!pdfs_background && !f->stream && nd->pending && nd->decl >= STREAM_AT && off == 0
      && (f->size == 0 || f->lazy)    /* nothing written yet (Filer_Action writes the size it created) */
      && !dev_of (nd)->streaming && !nd->gone
      && (!mtp_needs_zlp (dev_of (nd), nd->decl) || can_edit (dev_of (nd))))
    {
      if ((e = stream_start (f)))
        return e;
    }
  if (f->stream)
    return stream_put (f, off, src, n);
  if (!f->loaded && (e = load_whole (f)))
    return e;
  return f_write (f, off, src, n);
}

static const pd_error *
fs_args (pd_regs *r)
{
  ofile *f = find_file (r->r[1]);
  const pd_error *e;
  if (!f)
    return &err_badhandle;
  switch (r->r[0])
    {
    case 3:                     /* set extent */
    case 7:                     /* ensure size */
      {
        uint64_t ext = (uint32_t) r->r[2];
        if (r->r[0] == 7 && ext <= f->size)
          return 0;
        if (f->stream)
          {
            f->size = ext;              /* checked at the close */
            return 0;
          }
        if (!f->loaded && (e = load_whole (f)))
          return e;
        if (ext != f->size)
          f->dirty = 1;
        return f_setsize (f, ext);
      }
    case 8:                     /* write zeroes */
      {
        uint64_t off = (uint32_t) r->r[2], cnt = (uint32_t) r->r[3];
        static const uint8_t zeros[4096];
        if (f->stream)
          {
            while (cnt)
              {
                uint32_t k = cnt > sizeof zeros ? sizeof zeros : (uint32_t) cnt;
                if ((e = stream_put (f, off, zeros, k)))
                  return e;
                off += k;
                cnt -= k;
              }
            return 0;
          }
        if (!f->loaded && (e = load_whole (f)))
          return e;
        while (cnt)
          {
            uint32_t k = cnt > sizeof zeros ? sizeof zeros : (uint32_t) cnt;
            if ((e = f_write (f, off, zeros, k)))
              return e;
            off += k;
            cnt -= k;
          }
        return 0;
      }
    case 9:                     /* read load/exec */
      node_loadexec (f->n, &r->r[2], &r->r[3]);
      return 0;
    case 6:                     /* flush */
      if (f->dirty && !f->n->gone && !f->stream)
        return commit (f);
      return 0;
    case 4:                     /* read allocated size */
      r->r[2] = (pd_reg) ((f->size + BUFSIZE - 1) & ~(uint64_t) (BUFSIZE - 1));
      return 0;
    case 2:                     /* read extent */
      r->r[2] = (pd_reg) (uint32_t) f->size;
      return 0;
    default:                    /* 10 image stamp, and others: nothing to do */
      return 0;
    }
}

static const pd_error *
fs_close (pd_regs *r)
{
  ofile *f = find_file (r->r[1]);
  const pd_error *e = 0;
  if (!f)
    return &err_badhandle;
  node *n = f->n;
  uint32_t load = (uint32_t) r->r[2];
  if (f->stream)
    e = stream_finish (f);      /* first: the device can do nothing else till then */
  if (!e && (load & 0xFFF00000u) == 0xFFF00000u && f->writable && !n->isdir && !n->gone)
    set_type (n, (int) ((load >> 8) & 0xFFF));
  ofile **pp = &files;
  while (*pp != f)
    pp = &(*pp)->next;
  if (!e && bg_take (f))
    {
      *pp = f->next;            /* the job has it now, and the node's ref */
      return 0;
    }
  if (f->fill)
    {
      f->fill = 0;              /* the rest is dropped in the background (fill_run) */
      f->dirty = 0;
      fills--;
    }
  if (!e && f->dirty && !n->gone)
    e = commit (f);
  *pp = f->next;
  f_free (f);                   /* and its scrap file */
  pd_free (f->rbuf);
  pd_free (f);
  if (--n->refs == 0 && n->gone)
    drop_node (n);
  else if (n->pending && !n->gone && !e)
    e = send_object (n, 0);   /* opened for writing and closed unwritten */
  if (!e)
    n->decl = 0;
  return e;
}

/* --- FSEntry_File ------------------------------------------------------------------------ */

static void
put_info (pd_regs *r, node *n)
{
  if (!n)
    {
      r->r[0] = 0;
      return;
    }
  r->r[0] = n->isdir || !n->parent ? 2 : 1;
  node_loadexec (n, &r->r[2], &r->r[3]);
  r->r[4] = (pd_reg) (uint32_t) (n->size > 0xFFFFFFFFu ? 0xFFFFFFFFu : n->size);
  r->r[5] = n->st->readonly ? 0x11 : 0x33;
}

static const pd_error *
fs_file (pd_regs *r)
{
  node *n, *dir;
  const char *leaf;
  int leaflen;
  int reason = (int) r->r[0];
  const pd_error *e;

  /* reading or writing an object's info (Filer_Action sets a new file's
     type before opening it) leaves a file just created waiting to be opened */
  if (reason > 5)
    flush_pending (0);
  /* reading or setting its info needs no waiting for a background send */
  e = (reason >= 6 ? resolve_idle : resolve) ((const char *) r->r[1], &n, &dir, &leaf, &leaflen);
  if (e)
    {
      if (reason == 5 && (e == &err_notfound || e == &err_notdir))
        {
          r->r[0] = 0;
          return 0;
        }
      return e;
    }
  switch (reason)
    {
    case 5:                     /* read catalogue info */
      put_info (r, n);
      return 0;

    case 1: case 2: case 3: case 4:   /* write info */
      if (!n)
        return &err_notfound;
      if (reason <= 2)
        {
          uint32_t load = (uint32_t) r->r[2];
          if ((load & 0xFFF00000u) == 0xFFF00000u && !n->isdir)
            set_type (n, (int) ((load >> 8) & 0xFFF));
        }
      return 0;                 /* dates and attributes: the device keeps its own */

    case 6:                     /* delete */
      if (!n)
        {
          r->r[0] = 0;
          return 0;
        }
      if (!n->parent)
        return &err_denied;
      if (n->isdir)
        {
          if ((e = ensure_listed (n)))
            return e;
          if (n->child)
            return &err_notempty;
        }
      put_info (r, n);
      if (n->st->readonly)
        return &err_readonly;
      if (!n->pending && (e = delete_object (n)))
        return e;
      drop_node (n);
      return 0;

    case 7:                     /* create file */
      {
        if (!dir)
          return &err_badname;
        if (dir->st->readonly)
          return &err_readonly;
        if (n && n->isdir)
          return &err_isdir;
        if (n && n->refs)
          return &err_denied;   /* open */
        if (n)
          {
            if (!n->pending && (e = delete_object (n)))
              return e;
            drop_node (n);
          }
        char *dn = to_device_name (leaf, leaflen);
        if (!dn)
          return &err_nomem;
        n = new_node (dir, dn);
        pd_free (dn);
        if (!n)
          return &err_nomem;
        n->pending = 1;
        n->size = (uint32_t) r->r[5] - (uint32_t) r->r[4];
        n->decl = (uint32_t) n->size;
        n->mtime = pd_unix_time ();
        uint32_t load = (uint32_t) r->r[2];
        if ((load & 0xFFF00000u) == 0xFFF00000u)
          {
            int t = (int) ((load >> 8) & 0xFFF);
            if (t != node_filetype (n))
              n->filetype = t;
          }
        pending_create = n;
        return 0;
      }

    case 8:                     /* create directory */
      {
        if (n)
          return n->isdir ? 0 : &err_exists;
        if (!dir)
          return 0;             /* the root */
        if (dir->st->readonly)
          return &err_readonly;
        char *dn = to_device_name (leaf, leaflen);
        if (!dn)
          return &err_nomem;
        n = new_node (dir, dn);
        pd_free (dn);
        if (!n)
          return &err_nomem;
        n->isdir = 1;
        derive_names (n);
        n->pending = 1;
        n->mtime = pd_unix_time ();
        if (bg_mkdir (n))
          {
            n->listed = 1;      /* new and empty */
            n->listed_at = pd_time_cs ();
            return 0;
          }
        e = send_object (n, 0);
        if (e)
          drop_node (n);
        else
          {
            n->listed = 1;      /* new and empty */
            n->listed_at = pd_time_cs ();
          }
        return e;
      }

    case 10:                    /* read block size */
      r->r[2] = BUFSIZE;
      return 0;

    default:
      return &err_unsupported;
    }
}

/* --- FSEntry_Func ------------------------------------------------------------------------ */

static const pd_error *
read_dir_entries (pd_regs *r, int info)
{
  node *n, *dir;
  const char *leaf;
  int leaflen;
  const pd_error *e = resolve ((const char *) r->r[1], &n, &dir, &leaf, &leaflen);
  if (e)
    return e;
  if (!n)
    return &err_notfound;
  if (n->parent && !n->isdir)
    return &err_notdir;
  if ((e = ensure_listed (n)))
    return e;
  uint8_t *out = (uint8_t *) r->r[2];
  uint32_t want = (uint32_t) r->r[3], start = (uint32_t) r->r[4], space = (uint32_t) r->r[5];
  uint32_t idx = 0, count = 0;
  node *c = n->child;
  for (; c && idx < start; c = c->next)
    idx++;
  for (; c && count < want; c = c->next, idx++)
    {
      uint32_t nl = (uint32_t) strlen (c->ro_name) + 1;
      uint32_t need = info ? ((20 + nl + 3) & ~3u) : nl;
      if (need > space)
        break;
      if (info)
        {
          pd_regs t;
          put_info (&t, c);
          put32 (out, (uint32_t) t.r[2]);
          put32 (out + 4, (uint32_t) t.r[3]);
          put32 (out + 8, (uint32_t) t.r[4]);
          put32 (out + 12, (uint32_t) t.r[5]);
          put32 (out + 16, (uint32_t) t.r[0]);
          memcpy (out + 20, c->ro_name, nl);
          for (uint32_t k = 20 + nl; k < need; k++)
            out[k] = 0;
        }
      else
        memcpy (out, c->ro_name, nl);
      out += need;
      space -= need;
      count++;
    }
  r->r[3] = count;
  /* FileSwitch (Util_ReadDirEntry, used by *Wipe and *Copy) ignores any
     entries returned with R4 = -1: say "end" only on a call that reads none */
  r->r[4] = (c || count) ? (pd_reg) idx : (pd_reg) -1;
  return 0;
}

static const pd_error *
fs_rename (pd_regs *r)
{
  node *a, *adir, *b, *bdir;
  const char *aleaf, *bleaf;
  int alen, blen;
  const pd_error *e;
  if ((e = resolve_idle ((const char *) r->r[1], &a, &adir, &aleaf, &alen)))
    return e;
  if ((e = resolve_idle ((const char *) r->r[2], &b, &bdir, &bleaf, &blen)))
    return e;
  r->r[1] = 1;                  /* "bad rename" unless we manage it */
  if (!a || !a->parent || !bdir || a->st != bdir->st)
    return 0;                   /* across devices or storages: FileSwitch copies */
  if (b && b != a)
    return &err_exists;
  for (node *p = bdir; p; p = p->parent)
    if (p == a)
      return 0;                 /* into itself */
  if (a->pending)
    return 0;
  if (a->st->readonly)
    return &err_readonly;
  mtp_dev *m = dev_of (a);
  if (bdir != a->parent)
    {
      if (!mtp_supports (m, OP_MoveObject))
        return 0;
      uint32_t p[3] = { a->handle, a->st->id, bdir == &bdir->st->root ? 0 : bdir->handle };
      int rc = mtp_transaction (m, OP_MoveObject, p, 3, 0, 0, 0, 0);
      if (rc != RC_OK)
        return rc_error (rc);
      unlink_node (a);
      a->parent = bdir;
      a->next = bdir->child;
      bdir->child = a;
    }
  if (!exact_eq (a->ro_name, bleaf, blen))
    {
      if (!mtp_supports (m, OP_SetObjectPropValue))
        return 0;
      /* "Letter" (Letter.txt) renamed "Memo" is Memo.txt on the device */
      char *dn = add_type_suffix (to_device_name (bleaf, blen), a->isdir,
                                  a->sfx[0] ? node_filetype (a) : -1, a->sfx);
      if (!dn)
        return &err_nomem;
      if (!exact_eq (a->mtp_name, dn, (int) strlen (dn)))
        {
          int t = node_filetype (a);
          e = rename_object (a, dn);
          if (!e)
            a->filetype = t == a->name_type ? -1 : t;
        }
      pd_free (dn);
      if (e)
        return e;
    }
  r->r[1] = 0;
  return 0;
}

/* Canonicalise the disc name: two calls, the first for lengths. */
static const pd_error *
fs_canonicalise (pd_regs *r)
{
  const char *disc = (const char *) r->r[2];
  storage *st;
  if (disc && *disc)
    {
      st = find_disc (disc, (int) strlen (disc));
      if (!st)
        return &err_nodisc;
    }
  else
    {
      st = find_disc (0, -1);
      if (!st)
        return &err_nodevices;
    }
  uint32_t dlen = (uint32_t) strlen (st->disc);
  if (!r->r[3] && !r->r[4])
    {
      /* lengths only */
      r->r[1] = 0;
      r->r[3] = 0;
      r->r[4] = dlen;
      return 0;
    }
  char *buf = (char *) r->r[4];
  uint32_t size = (uint32_t) r->r[6];
  uint32_t k = dlen < size ? dlen : (size ? size - 1 : 0);
  if (buf && size)
    {
      memcpy (buf, st->disc, k);
      buf[k] = 0;
    }
  r->r[1] = 0;
  r->r[2] = (pd_reg) buf;
  r->r[3] = 0;
  r->r[4] = dlen + 1 > size ? dlen + 1 - size : 0;
  return 0;
}

static const pd_error *
storage_info (storage *st)
{
  mtp_data in = { 0, 0, 0 };
  uint32_t id = st->id;
  int rc = mtp_transaction (&st->dev->mtp, OP_GetStorageInfo, &id, 1, 0, 0, &in, 0);
  if (rc == RC_OK && in.len >= 26)
    {
      uint16_t access = get16 (in.buf + 4);
      st->readonly = access == 1 || access == 2;
      st->capacity = get64 (in.buf + 6);
      st->free = get64 (in.buf + 14);
      if (in.len > 26)
        mtp_get_string (in.buf + 26, in.buf + in.len, st->desc, sizeof st->desc);
    }
  pd_free (in.buf);
  return rc_error (rc);
}

static const pd_error *
fs_freespace (pd_regs *r, int wide)
{
  node *n, *dir;
  const char *leaf;
  int leaflen;
  const pd_error *e = resolve ((const char *) r->r[1], &n, &dir, &leaf, &leaflen);
  if (e)
    return e;
  storage *st = n ? n->st : dir->st;
  if ((e = storage_info (st)))
    return e;
  if (wide)
    {
      r->r[0] = (pd_reg) (uint32_t) st->free;
      r->r[1] = (pd_reg) (uint32_t) (st->free >> 32);
      r->r[2] = (pd_reg) (st->free > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t) st->free);
      r->r[3] = (pd_reg) (uint32_t) st->capacity;
      r->r[4] = (pd_reg) (uint32_t) (st->capacity >> 32);
    }
  else
    {
      r->r[0] = (pd_reg) (st->free > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) st->free);
      r->r[1] = (pd_reg) (st->free > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t) st->free);
      r->r[2] = (pd_reg) (st->capacity > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) st->capacity);
    }
  return 0;
}

static const pd_error *
fs_func (pd_regs *r)
{
  int reason = (int) r->r[0];
  if (reason != 23 && reason != 14 && reason != 15 && reason != 16)
    flush_pending (0);
  switch (reason)
    {
    case 8:  return fs_rename (r);
    case 14: return read_dir_entries (r, 0);
    case 15: return read_dir_entries (r, 1);
    case 23: return fs_canonicalise (r);
    case 24: r->r[4] = (pd_reg) -1; return 0;      /* FileSwitch resolves wildcards */
    case 27: r->r[2] = 0; return 0;                /* boot option: none */
    case 28: return 0;                             /* write boot option: ignored */
    case 30: return fs_freespace (r, 0);
    case 35: return fs_freespace (r, 1);
    case 10: case 16: case 17: case 32: return 0;  /* boot, shutdown, banner, stamp */
    default: return &err_unsupported;
    }
}

/* --- entry ------------------------------------------------------------------------------ */

static void release (device *dv);
static int add_storages (device *dv, unsigned look_cs);
static int finish_storages (device *dv);

int pdfs_mount_defer;           /* the filer is mounting: storages asked about from its poll */
#define LOOK_CS 150             /* longest wait for the storage list while waiting (from the poll) */

static void
poll_events (void)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    {
      device *dv = &devices[d];
      uint16_t code;
      uint32_t p1;
      if (!dv->used)
        continue;
      if (dv->mtp.tr && dv->mtp.tr->gone)
        {
          pd_log ("pdfs: device unplugged");
          release (dv);
          continue;
        }
      if (dv->settle_until)
        {
          if ((int) (pd_time_cs () - dv->settle_until) < 0)
            continue;
          dv->settle_until = 0;
          int defer = pdfs_mount_defer;
          pdfs_mount_defer = 1;     /* asked about one per poll, as when the filer mounts */
          int rc = add_storages (dv, 0);
          pdfs_mount_defer = defer;
          if (rc == RC_AccessDenied || rc == RC_DeviceBusy || (rc == RC_OK && dv->nst == 0))
            {
              dv->waiting = 1;
              dv->next_look = pd_time_cs () + 100;
              pd_log ("pdfs: the device has no storages yet: waiting for access to its data to be allowed on it");
            }
          else if (rc != RC_OK)
            pd_log ("pdfs: the device didn't say what storages it has");
          continue;
        }
      int look = 0;
      int pending = 0;
      for (int s = 0; s < dv->nst; s++)
        pending |= dv->st[s].info_pending == 1;
      if (pending && !dv->mtp.streaming)
        {
          finish_storages (dv);   /* one per poll */
          continue;
        }
      while (mtp_poll_event (&dv->mtp, &code, &p1))
        {
          pd_log_hex ("pdfs: event", code);
          if (code == 0x4004)         /* StoreAdded */
            look = 1;
          for (int s = 0; s < dv->nst; s++)
            invalidate_all (&dv->st[s].root);
        }
      if (dv->waiting && (int) (pd_time_cs () - dv->next_look) >= 0)
        look = 1;
      if (!look || dv->mtp.streaming)
        continue;
      int rc = add_storages (dv, dv->waiting ? LOOK_CS : 0);
      if (rc < 0 && !dv->waiting)
        continue;               /* a transfer error: the next call finds it */
      if (rc < 0)
        {
          /* no answer: give it a few more looks (a second each), then let it go */
          if (++dv->silent < 5)
            {
              dv->next_look = pd_time_cs () + 100;
              continue;
            }
          pd_log ("pdfs: the device stopped answering while waiting for access to be allowed");
          release (dv);
          continue;
        }
      dv->silent = 0;
      if (dv->waiting && dv->nst)
        {
          dv->waiting = 0;
          pd_log ("pdfs: access allowed: the device's storages are there now");
        }
      dv->next_look = pd_time_cs () + 100;
    }
}

int pdfs_trace = 0;

/* One log line per call that names an object: "File 6 :disc.$.a -> error" */
static void
trace (int entry, pd_regs *r, const pd_regs *out, const pd_error *e)
{
  static const char names[][6] = { "Open", "Get", "Put", "Args", "Close", "File", "Func" };
  char b[200], *p = b;
  if (!pdfs_trace || entry == PD_GETBYTES || entry == PD_PUTBYTES)
    return;
  for (const char *q = names[entry]; *q; q++)
    *p++ = *q;
  *p++ = ' ';
  unsigned v = (unsigned) r->r[0];
  if (entry == PD_ARGS || entry == PD_CLOSE)
    v = entry == PD_ARGS ? v : 0;
  p = cat_num (p, v);
  if (entry == PD_OPEN || entry == PD_FILE || (entry == PD_FUNC && v != 23))
    {
      const char *path = (const char *) r->r[1];
      *p++ = ' ';
      for (int i = 0; path && path[i] >= ' ' && p < b + 150; i++)
        *p++ = path[i];
    }
  if (!e && entry == PD_FUNC && (v == 14 || v == 15))
    {
      *p++ = ' '; *p++ = 'n'; *p++ = '=';
      p = cat_num (p, (unsigned) out->r[3]);
      *p++ = ' '; *p++ = 'o'; *p++ = '=';
      p = cat_num (p, (unsigned) out->r[4]);
    }
  if (e)
    {
      const char *m = " -> ";
      while (*m)
        *p++ = *m++;
      for (int i = 0; e->errmess[i] && p < b + sizeof b - 1; i++)
        *p++ = e->errmess[i];
    }
  *p = 0;
  pd_log (b);
}

const pd_error *
pdfs_fs_entry (int entry, pd_regs *r)
{
  const pd_error *e;
  pd_regs in = *r;
  poll_events ();
  switch (entry)
    {
    case PD_OPEN: e = fs_open (r); break;
    case PD_GETBYTES: e = fs_getbytes (r); break;
    case PD_PUTBYTES: e = fs_putbytes (r); break;
    case PD_ARGS: e = fs_args (r); break;
    case PD_CLOSE: e = fs_close (r); break;
    case PD_FILE: e = fs_file (r); break;
    case PD_FUNC: e = fs_func (r); break;
    default: e = &err_unsupported;
    }
  trace (entry, &in, r, e);
  return e;
}

void
pdfs_tick (void)
{
  poll_events ();
}

/* --- devices ------------------------------------------------------------------------------ */

static void
sanitise (char *o, const char *s, int size)
{
  int k = 0;
  for (; *s && k < size - 1; s++)
    {
      char c = *s;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
        o[k++] = c;
      else if (k && o[k - 1] != '_')
        o[k++] = '_';
    }
  while (k && o[k - 1] == '_')
    k--;
  o[k] = 0;
}

static void
make_disc_name (device *dv, storage *st)
{
  char model[24], desc[24], name[40];
  sanitise (model, dv->mtp.model[0] ? dv->mtp.model : "Device", sizeof model);
  if (!model[0])
    str_copy (model, "Device", sizeof model);
  sanitise (desc, st->desc, sizeof desc);
  if (dv->nst > 1 && desc[0])
    {
      int k = 0;
      for (int i = 0; model[i]; i++)
        name[k++] = model[i];
      name[k++] = '-';
      for (int i = 0; desc[i] && k < (int) sizeof name - 1; i++)
        name[k++] = desc[i];
      name[k] = 0;
    }
  else
    str_copy (name, model, sizeof name);
  /* Make it unique */
  str_copy (st->disc, name, sizeof st->disc);
  for (int n = 2; ; n++)
    {
      int clash = 0;
      for (int d = 0; d < MAX_DEVICES; d++)
        if (devices[d].used)
          for (int s = 0; s < devices[d].nst; s++)
            if (&devices[d].st[s] != st && name_eq (devices[d].st[s].disc, st->disc, (int) strlen (st->disc)))
              clash = 1;
      if (!clash)
        break;
      int k = (int) strlen (name);
      if (k > (int) sizeof st->disc - 4)
        k = (int) sizeof st->disc - 4;
      memcpy (st->disc, name, (size_t) k);
      st->disc[k++] = '-';
      if (n >= 10)
        st->disc[k++] = (char) ('0' + n / 10);
      st->disc[k++] = (char) ('0' + n % 10);
      st->disc[k] = 0;
    }
}

/* Ask the device for its storages and add the ones not known yet: at the
   mount, and later when it had none (access not allowed yet) or says one
   was added (StoreAdded). RC_OK, an MTP response code, or a transport error. */
static int
add_storages (device *dv, unsigned look_cs)
{
  /* From the filer (pdfs_mount_defer, or a look from its poll): the new
     storages are asked about one per poll (finish_storages), so a device
     that's slow to answer holds the desktop for one answer at a time.
     A *command asks about them all at once. */
  mtp_data in = { 0, 0, 0 };
  /* look_cs: how long the storage list may take (a look from the filer's
     poll while waiting must be brief); 0 = the usual time. Storages found
     are then asked about (GetStorageInfo) in the usual time. */
  unsigned saved = dv->mtp.timeout_cs;
  if (look_cs && look_cs < saved)
    dv->mtp.timeout_cs = look_cs;
  int rc = mtp_transaction (&dv->mtp, OP_GetStorageIDs, 0, 0, 0, 0, &in, 0);
  dv->mtp.timeout_cs = saved;
  if (rc != RC_OK || in.len < 4)
    {
      pd_free (in.buf);
      return rc == RC_OK ? MTP_EPROTO : rc;
    }
  int had = dv->nst;
  uint32_t n = get32 (in.buf);
  for (uint32_t i = 0; i < n && dv->nst < MAX_STORAGES && 8 + 4 * i <= in.len; i++)
    {
      uint32_t id = get32 (in.buf + 4 + 4 * i);
      if ((id & 0xFFFF) == 0)
        continue;             /* a logical storage that isn't there (empty card slot) */
      int known = 0;
      for (int s = 0; s < dv->nst; s++)
        known |= dv->st[s].id == id;
      if (known)
        continue;
      storage *st = &dv->st[dv->nst];
      memset (st, 0, sizeof *st);
      st->dev = dv;
      st->id = id;
      st->root.st = st;
      st->root.isdir = 1;
      st->root.filetype = -1;
      st->root.mtp_name = str_dup ("$", 1);
      st->root.ro_name = str_dup ("$", 1);
      st->info_pending = 1;
      dv->nst++;
    }
  pd_free (in.buf);
  (void) had;
  if (!pdfs_mount_defer && !look_cs)
    while (finish_storages (dv))
      ;
  return RC_OK;
}

/* Ask about one storage found but not yet asked about (GetStorageInfo).
   When none are left, the new ones get their disc names: all of them the
   first time, after that only the new ones, so discs in use keep theirs.
   Returns 1 if there are more to ask about. */
static int
finish_storages (device *dv)
{
  for (int s = 0; s < dv->nst; s++)
    if (dv->st[s].info_pending == 1)
      {
        storage_info (&dv->st[s]);
        dv->st[s].info_pending = 2;   /* asked; named below */
        break;
      }
  for (int s = 0; s < dv->nst; s++)
    if (dv->st[s].info_pending == 1)
      return 1;
  for (int s = 0; s < dv->nst; s++)
    if (dv->st[s].info_pending == 2)
      {
        dv->st[s].info_pending = 0;
        make_disc_name (dv, &dv->st[s]);
      }
  return 0;
}

const pd_error *
pdfs_mount (pd_transport *tr)
{
  device *dv = 0;
  for (int d = 0; d < MAX_DEVICES; d++)
    if (!devices[d].used)
      {
        dv = &devices[d];
        break;
      }
  if (!dv)
    {
      tr->close (tr);
      return &err_toomany;
    }
  memset (dv, 0, sizeof *dv);
  if (mtp_init (&dv->mtp, tr) < 0)
    {
      tr->close (tr);
      return &err_nomem;
    }
  /* (mtp_open gives a device 3 seconds to answer its first request: one
     set to charging only never does. After that, the usual time: a Fire
     tablet just back on the bus after Allow took over 1.5 seconds to
     answer GetStorageInfo.) */
  dv->mtp.timeout_cs = pdfs_timeout_cs;
  dv->mtp.busy_wait = busy_wait;
  dv->mtp.rbusy = fill_busy;
  int rc = mtp_open (&dv->mtp);
  if (rc < 0)
    {
      const pd_error *e = rc <= -0x1000 ? rc_error (-rc) : rc_error (rc);
      mtp_finish (&dv->mtp);
      return e;
    }
  dv->used = 1;
  if (dv->mtp.android)
    dv->mtp.gap_cs = pdfs_gap_cs >= 0 ? (unsigned) pdfs_gap_cs : ANDROID_GAP_CS;
  if (dv->mtp.android && pdfs_settle_cs)
    {
      char b[100], *q = cat (b, "pdfs: an Android device: leaving it ");
      q = cat_tenths (q, pdfs_settle_cs / 10);
      cat (q, " s to settle before asking about its storages");
      pd_log (b);
      if (pdfs_mount_defer)
        {
          /* the filer's poll looks for them when the time's up (poll_events) */
          dv->settle_until = (pd_time_cs () + pdfs_settle_cs) | 1;
          return 0;
        }
      unsigned t0 = pd_time_cs ();
      while (pd_time_cs () - t0 < pdfs_settle_cs)
        ;
    }
  rc = add_storages (dv, 0);
  if (rc == RC_AccessDenied || rc == RC_DeviceBusy || (rc == RC_OK && dv->nst == 0))
    {
      /* Android asks the owner to allow access to the device's data when a
         session is opened, and until they do, it has no storages (or
         refuses). The session stays open, and the storages are looked for
         again (poll_events). Closing it and trying again opened a new
         session each time, and each one asked again: the owner's Allow
         never reached a session that was still there. */
      dv->waiting = 1;
      dv->next_look = pd_time_cs () + 100;
      pd_log ("pdfs: the device has no storages yet: waiting for access to its data to be allowed on it");
      return 0;
    }
  if (rc != RC_OK)
    {
      dv->used = 0;
      mtp_finish (&dv->mtp);
      return rc_error (rc);
    }
  return 0;
}

static void
release (device *dv)
{
  bg_abort (dv);
  /* Close files on it (FileSwitch's handles go stale: reads will error) */
  ofile **pp = &files;
  while (*pp)
    {
      ofile *f = *pp;
      if (f->n->st->dev == dv)
        {
          *pp = f->next;
          f->n->refs--;
          if (f->fill)
            fills--;
          f_free (f);           /* and its scrap file */
          pd_free (f->rbuf);
          pd_free (f);
        }
      else
        pp = &f->next;
    }
  for (int s = 0; s < dv->nst; s++)
    {
      if (pending_create && pending_create->st == &dv->st[s])
        pending_create = 0;
      free_tree (&dv->st[s].root);
      pd_free (dv->st[s].root.mtp_name);
      pd_free (dv->st[s].root.ro_name);
    }
  if (dv->mtp.open)
    mtp_transaction (&dv->mtp, OP_CloseSession, 0, 0, 0, 0, 0, 0);
  mtp_finish (&dv->mtp);
  dv->used = 0;
}

const pd_error *
pdfs_dismount (const char *disc)
{
  int any = 0;
  for (int d = 0; d < MAX_DEVICES; d++)
    {
      device *dv = &devices[d];
      if (!dv->used)
        continue;
      int match = !*disc;
      for (int s = 0; s < dv->nst; s++)
        if (name_eq (dv->st[s].disc, disc, (int) strlen (disc)))
          match = 1;
      if (match)
        {
          flush_pending (0);
          drain (dv);
          release (dv);
          any = 1;
        }
    }
  return any || !*disc ? 0 : &err_nodisc;
}

int
pdfs_disc_count (void)
{
  int n = 0;
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        n += !st_hidden (&devices[d].st[s]);
  return n;
}

const char *
pdfs_disc_name (int i)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        if (!st_hidden (&devices[d].st[s]) && i-- == 0)
          return devices[d].st[s].disc;
  return 0;
}

const pd_error *
pdfs_free_space (const char *disc, uint64_t *size, uint64_t *free)
{
  storage *st = find_disc (disc, (int) strlen (disc));
  if (!st)
    return &err_nodisc;
  /* the Free window asks every few seconds: don't ask the device as often */
  int busy = st->dev->mtp.streaming;
  for (job *j = jobs; j; j = j->next)
    busy |= j->dv == st->dev;
  /* A device that didn't answer isn't asked again for 30 seconds (each
     try waits for it): the window shows what was last known */
  int failed_lately = st->info_failed_at && pd_time_cs () - st->info_failed_at < 3000;
  if (!failed_lately && (!st->info_at || (!busy && pd_time_cs () - st->info_at > 300)))
    {
      const pd_error *e = storage_info (st);
      if (e)
        {
          st->info_failed_at = pd_time_cs () | 1;
          if (!st->capacity)
            return e;
        }
      else
        {
          st->info_failed_at = 0;
          st->info_at = pd_time_cs () | 1;
        }
    }
  *size = st->capacity;
  *free = st->free;
  return 0;
}

/* Ejecting. pdfs_eject_info: for disc, the number of files open on it (or
   on any of its device's storages, all), how many storages its device has
   mounted, and the device's name. -1 if there's no such disc. */
int
pdfs_eject_info (const char *disc, int all, int *storages, const char **model)
{
  storage *st = find_disc (disc, (int) strlen (disc));
  if (!st)
    return -1;
  device *dv = st->dev;
  int n = 0;
  for (ofile *f = files; f; f = f->next)
    if (all ? f->n->st->dev == dv : f->n->st == st)
      n++;
  if (storages)
    {
      *storages = 0;
      for (int s = 0; s < dv->nst; s++)
        *storages += !st_hidden (&dv->st[s]);
    }
  if (model)
    *model = dv->mtp.model[0] ? dv->mtp.model : "The device";
  return n;
}

/* Which device a disc is on (an index), or -1 */
int
pdfs_device_of (const char *disc)
{
  storage *st = find_disc (disc, (int) strlen (disc));
  return st ? (int) (st->dev - devices) : -1;
}

/* Bring back the storages ejected one by one (*PDFSMount) */
void
pdfs_uneject (void)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        devices[d].st[s].ejected = 0;
}

/* Eject a storage (all: every storage of its device). Refused while files
   are open on it. When the device has none left, its session is closed:
   then *gone is set and it can be unplugged. */
const pd_error *
pdfs_eject (const char *disc, int all, int *gone)
{
  static const pd_error err_open = { PDFS_ERRBASE | 0xCE, "Files on it are open: close them, then eject it" };
  int left;
  *gone = 0;
  int busy = pdfs_eject_info (disc, all, &left, 0);
  if (busy < 0)
    return &err_nodisc;
  if (busy > 0)
    return &err_open;
  storage *st = find_disc (disc, (int) strlen (disc));
  device *dv = st->dev;
  drain (dv);                   /* files still being sent go first */
  const pd_error *e = flush_pending (0);
  if (e)
    return e;
  if (all || left <= 1)
    {
      release (dv);
      *gone = 1;
      return 0;
    }
  free_tree (&st->root);
  st->ejected = 1;
  return 0;
}

const char *
pdfs_disc_desc (int i)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        if (!st_hidden (&devices[d].st[s]) && i-- == 0)
          return devices[d].st[s].desc;
  return 0;
}

static char *
cat (char *o, const char *s)
{
  while (*s)
    *o++ = *s++;
  *o = 0;
  return o;
}

/* v tenths as "12.3" */
static char *
cat_tenths (char *o, uint64_t v)
{
  o = cat_num (o, v / 10);
  *o++ = '.';
  *o++ = (char) ('0' + v % 10);
  *o = 0;
  return o;
}

/* "24.0 MB in 12.1 s (2.0 MB/s)" */
static char *
cat_rate (char *o, uint64_t bytes, uint32_t cs)
{
  o = cat_tenths (o, bytes * 10 >> 20);
  o = cat (o, " MB in ");
  o = cat_tenths (o, cs / 10);
  o = cat (o, " s");
  if (cs)
    {
      o = cat (o, " (");
      o = cat_tenths (o, (bytes * 1000 / cs) >> 20);   /* tenths of MB/s */
      o = cat (o, " MB/s)");
    }
  return o;
}

static char *
cat_num (char *o, uint64_t v)
{
  char t[24];
  int k = 0;
  do
    {
      t[k++] = (char) ('0' + v % 10);
      v /= 10;
    }
  while (v);
  while (k)
    *o++ = t[--k];
  *o = 0;
  return o;
}

void
pdfs_describe (void (*line) (const char *s))
{
  char b[256], *p;
  int any = 0;
  for (int d = 0; d < MAX_DEVICES; d++)
    {
      device *dv = &devices[d];
      if (!dv->used)
        continue;
      any = 1;
      p = cat (b, dv->mtp.manufacturer);
      p = cat (p, " ");
      p = cat (p, dv->mtp.model);
      p = cat (p, " (");
      p = cat (p, dv->mtp.tr->name ? dv->mtp.tr->name : "?");
      p = cat (p, "): ");
      p = cat_num (p, dv->mtp.transactions);
      p = cat (p, " transactions, ");
      p = cat_num (p, dv->mtp.errors);
      p = cat (p, " errors, ");
      p = cat_num (p, dv->mtp.recoveries);
      cat (p, " recoveries");
      line (b);
      if (dv->waiting)
        line ("  waiting: allow access to its data on the device");
      mtp_dev *m = &dv->mtp;
      line (mtp_supports (m, OP_GetPartialObject64) || mtp_supports (m, OP_GetPartialObject)
            ? "  reads files: in parts"
            : "  reads files: whole (the device can't send part of one): received in the background");
      line (mtp_supports (m, OP_GetObjectPropList) && !m->no_proplist
            ? "  lists folders: all at once (property lists)"
            : m->android ? "  lists folders: an object at a time (Android)"
                         : "  lists folders: an object at a time");
      if (m->lost)
        {
          p = cat (b, "  answers lost on the way (and asked for again): ");
          cat_num (p, m->lost);
          line (b);
        }
      if (m->gap_cs)
        {
          p = cat (b, "  leaves ");
          p = cat_num (p, m->gap_cs);
          cat (p, " cs between an answer and the next request");
          line (b);
        }
      p = cat (b, "  received ");
      p = cat_rate (p, m->bytes_in, m->in_cs);
      p = cat (p, ", sent ");
      p = cat_rate (p, m->bytes_out, m->out_cs);
      p = cat (p, "; longest wait for one request ");
      p = cat_tenths (p, m->slowest_cs / 10);
      p = cat (p, " s");
      if (m->slowest_cs)
        {
          char h[8];
          p = cat (p, " (op &");
          for (int i = 0; i < 4; i++)
            h[i] = hexd[(m->slowest_op >> (12 - 4 * i)) & 15];
          h[4] = 0;
          p = cat (p, h);
          cat (p, ")");
        }
      line (b);
      for (int s = 0; s < dv->nst; s++)
        {
          storage *st = &dv->st[s];
          if (st_hidden (st))
            continue;
          p = cat (b, "  PDFS::");
          p = cat (p, st->disc);
          p = cat (p, "  \"");
          p = cat (p, st->desc);
          p = cat (p, "\"  ");
          p = cat_num (p, st->free >> 20);
          p = cat (p, " MB free of ");
          p = cat_num (p, st->capacity >> 20);
          p = cat (p, " MB");
          if (st->readonly)
            cat (p, ", read-only");
          line (b);
        }
    }
  if (!any)
    line ("No portable devices are mounted.");
}

void
pdfs_finish (void)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      release (&devices[d]);
}
