/* fs.c: PDFS's filing system: devices and their storages ("discs"), the
   directory cache that maps RISC OS paths to MTP object handles, and the
   FileSwitch entry points.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "pdfs.h"
#include "mtp.h"
#include "plat.h"

unsigned pdfs_dir_ttl_cs = 500;
unsigned pdfs_timeout_cs = 1000;

#define MAX_DEVICES   4
#define MAX_STORAGES  4
#define BUFSIZE       4096          /* FileSwitch buffer size for our files */
#define READAHEAD     65536
#define MAX_WRITE     (256u << 20)  /* largest file we'll buffer for writing */
#define HEADROOM      12            /* bytes before out-data for the container header */

/* --- types -------------------------------------------------------------------- */

typedef struct node node;
struct node
{
  node *parent, *child, *next;
  struct storage *st;
  uint32_t handle;            /* 0 = storage root, or a file not yet sent to the device */
  uint8_t isdir, listed, gone, pending;
  int refs;                   /* open files using it */
  unsigned listed_at;
  unsigned seen;              /* mark of the last listing that included it */
  uint64_t size;
  uint32_t mtime;             /* unix seconds */
  int filetype;               /* -1: from the name */
  char *mtp_name;             /* UTF-8 */
  char *ro_name;              /* the machine's alphabet, RISC OS form */
};

typedef struct storage
{
  struct device *dev;
  uint32_t id;
  char desc[48];
  char disc[40];
  uint64_t free, capacity;
  int readonly;
  node root;
} storage;

typedef struct device
{
  int used;
  mtp_dev mtp;
  storage st[MAX_STORAGES];
  int nst;
} device;

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
  uint8_t *rbuf;              /* read-ahead */
  uint64_t roff;
  uint32_t rlen;
};

static device devices[MAX_DEVICES];
static ofile *files;
static node *pending_create;   /* created by FSEntry_File 7, not yet sent */

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
E (err_toobig, 0xB9, "File too big for PDFS to write");
E (err_badname, 0xCC, "Bad name");
E (err_unsupported, 0xA5, "The device can't do that");
E (err_proto, 0xCA, "The device sent something PDFS doesn't understand");
E (err_toomany, 0xCB, "Too many portable devices");
E (err_badhandle, 0xDE, "Channel");

static pd_error err_device;    /* a response code from the device, with its number */

static const char hexd[] = "0123456789ABCDEF";
static char *cat_num (char *o, uint64_t v);

static const pd_error *
rc_error (int rc)
{
  if (rc == RC_OK)
    return 0;
  switch (rc)
    {
    case PDT_TIMEOUT: return &err_timeout;
    case PDT_GONE: return &err_gone;
    case PDT_IO: return &err_io;
    case MTP_ENOMEM: return &err_nomem;
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
  return days_from_civil (v[0], v[1], v[2]) * 86400u + (uint32_t) (v[3] * 3600 + v[4] * 60 + v[5]);
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
  const char *dot = 0;
  for (const char *p = n->mtp_name; *p; p++)
    if (*p == '.')
      dot = p;
  if (dot && dot != n->mtp_name && dot[1] && strlen (dot + 1) <= 8)
    {
      int t = pd_ext_to_filetype (dot + 1);
      if (t >= 0)
        return t;
    }
  return 0xFFD;
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
  pd_free (n);
}

static node *
new_node (node *parent, const char *mtp_name)
{
  node *n = pd_alloc (sizeof *n);
  if (!n)
    return 0;
  n->mtp_name = str_dup (mtp_name, -1);
  n->ro_name = to_riscos_name (mtp_name);
  if (!n->mtp_name || !n->ro_name)
    {
      pd_free (n->mtp_name);
      pd_free (n->ro_name);
      pd_free (n);
      return 0;
    }
  n->filetype = -1;
  n->st = parent->st;
  n->parent = parent;
  n->next = parent->child;
  parent->child = n;
  return n;
}

static int
set_name (node *n, const char *mtp_name)
{
  char *a = str_dup (mtp_name, -1), *b = to_riscos_name (mtp_name);
  if (!a || !b)
    {
      pd_free (a);
      pd_free (b);
      return -1;
    }
  pd_free (n->mtp_name);
  pd_free (n->ro_name);
  n->mtp_name = a;
  n->ro_name = b;
  return 0;
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
  else if (!name_eq (n->mtp_name, e->name, (int) strlen (e->name)) || strlen (n->mtp_name) != strlen (e->name))
    set_name (n, e->name);
  n->isdir = e->format == FMT_Association;
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

static const pd_error *
list_dir (node *dir)
{
  storage *st = dir->st;
  mtp_dev *m = &st->dev->mtp;
  entry *es = 0;
  int ne = 0, rc;
  int root = dir == &st->root;

  if (mtp_supports (m, OP_GetObjectPropList))
    {
      uint32_t p[5] = { root ? 0u : dir->handle, 0, 0xFFFFFFFFu, 0, 1 };
      mtp_data in = { 0, 0, 0 };
      rc = mtp_transaction (m, OP_GetObjectPropList, p, 5, 0, 0, &in, 0);
      if (rc == RC_OK)
        {
          int e = parse_proplist (in.buf, in.len, &es, &ne);
          pd_free (in.buf);
          if (e < 0)
            return &err_proto;
          /* Root lists may hold every storage's objects: keep this one's.
             (Objects that don't say which storage they're on are kept.) */
          int k = 0;
          for (int i = 0; i < ne; i++)
            if (!es[i].have_name)
              continue;
            else if (!root || es[i].storage == 0 || es[i].storage == st->id)
              es[k++] = es[i];
          ne = k;
          goto apply;
        }
      pd_free (in.buf);
      if (rc < 0)
        return rc_error (rc);
      /* Refused: fall back to handles + object info, as older devices need */
    }
  {
    uint32_t p[3] = { st->id, 0, root ? 0xFFFFFFFFu : dir->handle };
    mtp_data in = { 0, 0, 0 };
    rc = mtp_transaction (m, OP_GetObjectHandles, p, 3, 0, 0, &in, 0);
    if (rc != RC_OK)
      {
        pd_free (in.buf);
        return rc_error (rc);
      }
    uint32_t n = in.len >= 4 ? get32 (in.buf) : 0;
    if (in.len < 4 + 4 * n)
      {
        pd_free (in.buf);
        return &err_proto;
      }
    es = pd_alloc (sizeof (entry) * (n ? n : 1));
    if (!es)
      {
        pd_free (in.buf);
        return &err_nomem;
      }
    for (uint32_t i = 0; i < n; i++)
      {
        uint32_t h = get32 (in.buf + 4 + 4 * i);
        mtp_data oi = { 0, 0, 0 };
        rc = mtp_transaction (m, OP_GetObjectInfo, &h, 1, 0, 0, &oi, 0);
        if (rc == RC_OK && parse_objectinfo (oi.buf, oi.len, &es[ne]) == 0)
          es[ne++].handle = h;
        pd_free (oi.buf);
        if (rc < 0)
          {
            pd_free (in.buf);
            pd_free (es);
            return rc_error (rc);
          }
      }
    pd_free (in.buf);
  }

apply:
  {
    static unsigned seq;
    uint32_t mark = ++seq;
    for (int i = 0; i < ne; i++)
      apply_entry (dir, &es[i], mark);
    pd_free (es);
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
  }
  return 0;
}

static const pd_error *
ensure_listed (node *dir)
{
  if (dir->listed && pd_time_cs () - dir->listed_at < pdfs_dir_ttl_cs)
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
        if (len < 0 ? 1 : name_eq (devices[d].st[s].disc, name, len))
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

/* --- device operations -------------------------------------------------------------- */

static mtp_dev *
dev_of (node *n)
{
  return &n->st->dev->mtp;
}

/* Create an object on the device: a directory, or a file with the given
   data (which has HEADROOM bytes before it). Sets n->handle. */
static const pd_error *
send_object (node *n, uint8_t *data, uint32_t len)
{
  mtp_dev *m = dev_of (n);
  uint8_t info[HEADROOM + 52 + 3 * 512 + 16];
  uint8_t *p = info + HEADROOM;
  node *dir = n->parent;
  if (!dir)
    return &err_notfound;
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
  if (n->isdir)
    return 0;
  rc = mtp_transaction (m, OP_SendObject, 0, 0, data ? data : info + HEADROOM, len, 0, 0);
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
  uint8_t empty[HEADROOM + 4];
  uint8_t *data = f->wbuf ? f->wbuf + HEADROOM : empty + HEADROOM;
  n->mtime = pd_unix_time ();

  if (!n->pending && n->handle && mtp_supports (m, OP_BeginEditObject)
      && mtp_supports (m, OP_SendPartialObject) && mtp_supports (m, OP_TruncateObject)
      && mtp_supports (m, OP_EndEditObject))
    {
      /* Android: edit the object in place, so it keeps its handle */
      uint32_t h = n->handle;
      int rc = mtp_transaction (m, OP_BeginEditObject, &h, 1, 0, 0, 0, 0);
      if (rc == RC_OK)
        {
          uint32_t sp[4] = { h, 0, 0, len };
          if (len)
            rc = mtp_transaction (m, OP_SendPartialObject, sp, 4, data, len, 0, 0);
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
  e = send_object (n, data, len);
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
  tmp.size = n->size;
  if (n->size)
    {
      if (n->size > MAX_WRITE)
        return &err_toobig;
      tmp.wbuf = pd_alloc ((size_t) n->size + HEADROOM);
      if (!tmp.wbuf)
        return &err_nomem;
    }
  const pd_error *e = commit (&tmp);
  pd_free (tmp.wbuf);
  return e;
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
  f->loaded = 1;
  if (n->pending || !n->handle || f->size == 0)
    return grow (f, f->size);
  e = grow (f, f->size);
  if (e)
    return e;
  mtp_dev *m = dev_of (n);
  if (mtp_supports (m, OP_GetPartialObject64) || mtp_supports (m, OP_GetPartialObject))
    {
      uint64_t off = 0;
      while (off < f->size)
        {
          uint32_t want = f->size - off > 1048576 ? 1048576 : (uint32_t) (f->size - off), got;
          e = read_object (n, off, f->wbuf + HEADROOM + off, want, &got);
          if (e)
            return e;
          if (got == 0)
            break;
          off += got;
        }
      return 0;
    }
  uint32_t h = n->handle;
  mtp_data in = { f->wbuf + HEADROOM, f->wcap, 0 };
  int rc = mtp_transaction (m, OP_GetObject, &h, 1, 0, 0, &in, 0);
  return rc_error (rc);
}

static const pd_error *
fs_open (pd_regs *r)
{
  node *n, *dir;
  const char *leaf;
  int leaflen;
  int reason = (int) r->r[0];
  const pd_error *e = resolve ((const char *) r->r[1], &n, &dir, &leaf, &leaflen);
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
      if (f->size && (e = grow (f, f->size)))
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
  if (f->writable || f->loaded)
    {
      if (!f->loaded && (e = load_whole (f)))
        return e;
      memcpy (dst, f->wbuf + HEADROOM + off, avail);
      return 0;
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
        {
          /* No partial reads on this device: fetch the whole object once */
          if ((e = load_whole (f)))
            return e;
          memcpy (dst, f->wbuf + HEADROOM + off, avail);
          return 0;
        }
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
  if (!f->loaded && (e = load_whole (f)))
    return e;
  if ((e = grow (f, off + n)))
    return e;
  memcpy (f->wbuf + HEADROOM + off, src, n);
  if (off + n > f->size)
    f->size = off + n;
  f->dirty = 1;
  return 0;
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
        if (!f->loaded && (e = load_whole (f)))
          return e;
        if ((e = grow (f, ext)))
          return e;
        if (ext > f->size)
          memset (f->wbuf + HEADROOM + f->size, 0, (size_t) (ext - f->size));
        if (ext != f->size)
          f->dirty = 1;
        f->size = ext;
        return 0;
      }
    case 8:                     /* write zeroes */
      {
        uint64_t off = (uint32_t) r->r[2], cnt = (uint32_t) r->r[3];
        if (!f->loaded && (e = load_whole (f)))
          return e;
        if ((e = grow (f, off + cnt)))
          return e;
        memset (f->wbuf + HEADROOM + off, 0, (size_t) cnt);
        if (off + cnt > f->size)
          f->size = off + cnt;
        f->dirty = 1;
        return 0;
      }
    case 9:                     /* read load/exec */
      node_loadexec (f->n, &r->r[2], &r->r[3]);
      return 0;
    case 6:                     /* flush */
      if (f->dirty && !f->n->gone)
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
  if ((load & 0xFFF00000u) == 0xFFF00000u && f->writable && !n->isdir)
    {
      int t = (int) ((load >> 8) & 0xFFF);
      if (t != node_filetype (n))
        n->filetype = t;
    }
  if (f->dirty && !n->gone)
    e = commit (f);
  ofile **pp = &files;
  while (*pp != f)
    pp = &(*pp)->next;
  *pp = f->next;
  pd_free (f->wbuf);
  pd_free (f->rbuf);
  pd_free (f);
  if (--n->refs == 0 && n->gone)
    drop_node (n);
  else if (n->pending && !n->gone && !e)
    e = send_object (n, 0, 0);   /* opened for writing and closed unwritten */
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

  if (reason != 5)
    flush_pending (0);
  e = resolve ((const char *) r->r[1], &n, &dir, &leaf, &leaflen);
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
            {
              int t = (int) ((load >> 8) & 0xFFF);
              n->filetype = t == node_filetype (n) ? n->filetype : t;
            }
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
        n->pending = 1;
        n->mtime = pd_unix_time ();
        e = send_object (n, 0, 0);
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
  if ((e = resolve ((const char *) r->r[1], &a, &adir, &aleaf, &alen)))
    return e;
  if ((e = resolve ((const char *) r->r[2], &b, &bdir, &bleaf, &blen)))
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
      char *dn = to_device_name (bleaf, blen);
      if (!dn)
        return &err_nomem;
      uint8_t buf[HEADROOM + 1 + 2 * 256];
      int k = mtp_put_string (buf + HEADROOM, dn);
      uint32_t p[2] = { a->handle, PROP_ObjectFileName };
      int rc = mtp_transaction (m, OP_SetObjectPropValue, p, 2, buf + HEADROOM, (uint32_t) k, 0, 0);
      if (rc != RC_OK)
        {
          pd_free (dn);
          return rc_error (rc);
        }
      set_name (a, dn);
      pd_free (dn);
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
      while (mtp_poll_event (&dv->mtp, &code, &p1))
        {
          pd_log_hex ("pdfs: event", code);
          for (int s = 0; s < dv->nst; s++)
            invalidate_all (&dv->st[s].root);
        }
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
  dv->mtp.timeout_cs = pdfs_timeout_cs;
  int rc = mtp_open (&dv->mtp);
  if (rc < 0)
    {
      const pd_error *e = rc <= -0x1000 ? rc_error (-rc) : rc_error (rc);
      mtp_finish (&dv->mtp);
      return e;
    }
  mtp_data in = { 0, 0, 0 };
  rc = mtp_transaction (&dv->mtp, OP_GetStorageIDs, 0, 0, 0, 0, &in, 0);
  if (rc != RC_OK || in.len < 4)
    {
      pd_free (in.buf);
      mtp_finish (&dv->mtp);
      return rc == RC_OK ? &err_proto : rc_error (rc);
    }
  uint32_t n = get32 (in.buf);
  dv->used = 1;
  for (uint32_t i = 0; i < n && dv->nst < MAX_STORAGES && 8 + 4 * i <= in.len; i++)
    {
      storage *st = &dv->st[dv->nst];
      memset (st, 0, sizeof *st);
      st->dev = dv;
      st->id = get32 (in.buf + 4 + 4 * i);
      if ((st->id & 0xFFFF) == 0)
        continue;             /* a logical storage that isn't there (empty card slot) */
      st->root.st = st;
      st->root.isdir = 1;
      st->root.filetype = -1;
      st->root.mtp_name = str_dup ("$", 1);
      st->root.ro_name = str_dup ("$", 1);
      storage_info (st);
      dv->nst++;
    }
  pd_free (in.buf);
  for (int s = 0; s < dv->nst; s++)
    make_disc_name (dv, &dv->st[s]);
  if (dv->nst == 0)
    {
      dv->used = 0;
      mtp_finish (&dv->mtp);
      return &err_nodevices;
    }
  return 0;
}

static void
release (device *dv)
{
  /* Close files on it (FileSwitch's handles go stale: reads will error) */
  ofile **pp = &files;
  while (*pp)
    {
      ofile *f = *pp;
      if (f->n->st->dev == dv)
        {
          *pp = f->next;
          f->n->refs--;
          pd_free (f->wbuf);
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
      n += devices[d].nst;
  return n;
}

const char *
pdfs_disc_name (int i)
{
  for (int d = 0; d < MAX_DEVICES; d++)
    if (devices[d].used)
      for (int s = 0; s < devices[d].nst; s++)
        if (i-- == 0)
          return devices[d].st[s].disc;
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
      for (int s = 0; s < dv->nst; s++)
        {
          storage *st = &dv->st[s];
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
