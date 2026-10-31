/* mtp.c: the MTP/PTP session layer. See mtp.h.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "mtp.h"
#include "plat.h"

#define STAGE_SIZE 16384

/* Requests that only read: asking again is harmless */
static int
asks_only (uint16_t op)
{
  switch (op)
    {
    case OP_GetDeviceInfo: case OP_GetStorageIDs: case OP_GetStorageInfo:
    case OP_GetNumObjects: case OP_GetObjectHandles: case OP_GetObjectInfo:
    case OP_GetObject: case OP_GetPartialObject: case OP_GetPartialObject64:
    case OP_GetObjectPropsSupported: case OP_GetObjectPropValue:
    case OP_GetObjectPropList:
      return 1;
    }
  return 0;
}

/* --- the trace (*PDFSLog trace): every request, every container header,
   and the pipes' state when something doesn't arrive ------------------- */

extern int pdfs_trace;

/* Transaction IDs: from 1, never &FFFFFFFF (the MTP standard: they wrap
   to 1); OpenSession's is 0 (transaction). */
uint32_t mtp_tid_after_open;    /* host tests: start near the wrap */

/* An Amazon Fire lost requests sent the moment its last answer arrived
   (0.16 tests 5 to 9: the request is taken at USB level, and then nothing
   more is answered). So a request waits gap_cs after the last answer. */
static void
gap_wait (mtp_dev *d)
{
  if (d->gap_cs && d->answered)
    while (pd_time_cs () - d->last_done < d->gap_cs)
      ;
}

static uint32_t
next_tid (mtp_dev *d)
{
  if (++d->tid == 0xFFFFFFFFu || d->tid == 0)
    d->tid = 1;
  return d->tid;
}

static char *
tcat (char *o, const char *s)
{
  while (*s)
    *o++ = *s++;
  *o = 0;
  return o;
}

static char *
tnum (char *o, uint32_t v)
{
  char b[12];
  int n = 0;
  do
    b[n++] = (char) ('0' + v % 10);
  while (v /= 10);
  while (n)
    *o++ = b[--n];
  *o = 0;
  return o;
}

static char *
thex (char *o, uint32_t v, int digits)
{
  for (int i = digits - 1; i >= 0; i--)
    *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
  *o = 0;
  return o;
}

static void
trace_state (mtp_dev *d, const char *why)
{
  char b[200], *q = tcat (b, "mtp: ");
  q = tcat (q, why);
  q = tcat (q, " (");
  q = tnum (q, d->stage_len - d->stage_pos);
  q = tcat (q, " bytes unread here)");
  pd_log (b);
  if (d->tr->state)
    {
      d->tr->state (d->tr, b, sizeof b);
      pd_log (b);
    }
}

int
mtp_init (mtp_dev *d, pd_transport *tr)
{
  memset (d, 0, sizeof *d);
  d->tr = tr;
  d->timeout_cs = 1000;
  d->stage_size = STAGE_SIZE;
  d->stage = pd_alloc (STAGE_SIZE);
  return d->stage ? 0 : MTP_ENOMEM;
}

void
mtp_finish (mtp_dev *d)
{
  pd_free (d->stage);
  d->stage = 0;
  pd_free (d->sbuf);
  d->sbuf = 0;
  d->streaming = 0;
  d->awaiting = 0;
  if (d->tr && d->tr->close)
    d->tr->close (d->tr);
  d->tr = 0;
}

int
mtp_supports (mtp_dev *d, uint16_t op)
{
  for (int i = 0; i < d->nops; i++)
    if (d->ops[i] == op)
      return 1;
  return 0;
}

/* --- strings ---------------------------------------------------------------- */

static int
put_utf8 (char *out, int pos, int outsz, uint32_t c)
{
  char b[4];
  int n;
  if (c < 0x80) { b[0] = (char) c; n = 1; }
  else if (c < 0x800) { b[0] = (char) (0xC0 | c >> 6); b[1] = (char) (0x80 | (c & 63)); n = 2; }
  else if (c < 0x10000) { b[0] = (char) (0xE0 | c >> 12); b[1] = (char) (0x80 | ((c >> 6) & 63));
                          b[2] = (char) (0x80 | (c & 63)); n = 3; }
  else { b[0] = (char) (0xF0 | c >> 18); b[1] = (char) (0x80 | ((c >> 12) & 63));
         b[2] = (char) (0x80 | ((c >> 6) & 63)); b[3] = (char) (0x80 | (c & 63)); n = 4; }
  if (pos + n >= outsz)
    return pos;
  for (int i = 0; i < n; i++)
    out[pos + i] = b[i];
  return pos + n;
}

int
mtp_get_string (const uint8_t *p, const uint8_t *end, char *out, int outsz)
{
  int pos = 0;
  if (p >= end)
    return -1;
  int n = p[0];
  if (p + 1 + 2 * n > end)
    return -1;
  for (int i = 0; i < n; i++)
    {
      uint32_t c = get16 (p + 1 + 2 * i);
      if (c == 0)
        break;
      if (c >= 0xD800 && c < 0xDC00 && i + 1 < n)
        {
          uint32_t c2 = get16 (p + 3 + 2 * i);
          if (c2 >= 0xDC00 && c2 < 0xE000)
            {
              c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
              i++;
            }
        }
      pos = put_utf8 (out, pos, outsz, c);
    }
  if (outsz > 0)
    out[pos] = 0;
  return 1 + 2 * n;
}

int
mtp_put_string (uint8_t *p, const char *s)
{
  const uint8_t *u = (const uint8_t *) s;
  int n = 0;                            /* UTF-16 units written */
  if (!*u)
    {
      p[0] = 0;
      return 1;
    }
  while (*u && n < 253)
    {
      uint32_t c = *u++;
      if (c >= 0xC0)
        {
          int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
          c &= 0x3F >> extra;
          while (extra-- && (*u & 0xC0) == 0x80)
            c = (c << 6) | (*u++ & 63);
        }
      if (c >= 0x10000)
        {
          c -= 0x10000;
          put16 (p + 1 + 2 * n++, 0xD800 + (c >> 10));
          put16 (p + 1 + 2 * n++, 0xDC00 + (c & 0x3FF));
        }
      else
        put16 (p + 1 + 2 * n++, c);
    }
  put16 (p + 1 + 2 * n++, 0);
  p[0] = (uint8_t) n;
  return 1 + 2 * n;
}

/* --- reading ---------------------------------------------------------------- */

/* Get n bytes of input into dst (NULL: discard them). */
static int
rd (mtp_dev *d, uint8_t *dst, uint32_t n)
{
  unsigned last = pd_time_cs ();
  while (n)
    {
      if (d->stage_pos < d->stage_len)
        {
          uint32_t k = d->stage_len - d->stage_pos;
          if (k > n)
            k = n;
          if (dst)
            {
              memcpy (dst, d->stage + d->stage_pos, k);
              dst += k;
            }
          d->stage_pos += k;
          n -= k;
          continue;
        }
      unsigned waited = pd_time_cs () - last;
      if (waited >= d->timeout_cs)
        return PDT_TIMEOUT;
      int got;
      if (dst && n >= 4096)
        {
          got = d->tr->read (d->tr, dst, n, d->timeout_cs - waited);
          if (got > 0)
            {
              dst += got;
              n -= (uint32_t) got;
            }
        }
      else
        {
          d->stage_pos = d->stage_len = 0;
          got = d->tr->read (d->tr, d->stage, d->stage_size, d->timeout_cs - waited);
          if (got > 0)
            d->stage_len = (uint32_t) got;
        }
      if (got == PDT_LOST && !d->repeatable && !d->rstreaming)
        {
          /* the input pipe failed and was cleared: a request that changes
             something isn't asked again, so wait on for its answer */
          d->stage_pos = d->stage_len = 0;
          continue;
        }
      if (got < 0)
        return got;
      if (got > 0)
        last = pd_time_cs ();
    }
  return 0;
}

/* --- transactions ------------------------------------------------------------ */

/* Read the reply to transaction tid: data (into in) and the response.
   Returns the response code or a negative error. */
static int
read_response (mtp_dev *d, uint16_t op, uint32_t tid, mtp_data *in, uint32_t *rparams)
{
  int e;
  (void) op;
  if (in)
    in->len = 0;
  for (;;)
    {
      uint8_t hdr[12];
      e = rd (d, hdr, 12);
      if (e < 0)
        {
          if (pdfs_trace)
            trace_state (d, e == PDT_TIMEOUT ? "nothing more arrived" : "reading failed");
          return e;
        }
      uint32_t len = get32 (hdr);
      uint16_t type = get16 (hdr + 4), code = get16 (hdr + 6);
      uint32_t rtid = get32 (hdr + 8);
      if (pdfs_trace)
        {
          char b[120], *q = tcat (b, "mtp< ");
          q = tcat (q, type == MTP_DATA ? "data" : type == MTP_RESP ? "response" : "type ");
          if (type != MTP_DATA && type != MTP_RESP)
            q = tnum (q, type);
          q = tcat (q, " &");
          q = thex (q, code, 4);
          q = tcat (q, " t");
          q = tnum (q, rtid);
          q = tcat (q, ", ");
          q = tnum (q, len);
          q = tcat (q, " bytes");
          pd_log (b);
        }
      if (len < 12)
        return MTP_EPROTO;
      uint32_t plen = len == 0xFFFFFFFF ? 0 : len - 12;
      if (type == MTP_DATA)
        {
          int toobig = 0;
          if (!in || rtid != tid)
            {
              e = rd (d, 0, plen);
              if (e < 0)
                return e;
              continue;
            }
          if (!in->buf)
            {
              in->buf = pd_alloc (plen + 1);
              if (!in->buf)
                {
                  rd (d, 0, plen);
                  return MTP_ENOMEM;
                }
              in->cap = plen;
            }
          uint32_t k = plen <= in->cap ? plen : in->cap;
          if (k < plen)
            toobig = 1;
          e = rd (d, in->buf, k);
          if (e < 0)
            {
              if (pdfs_trace)
                trace_state (d, "the data stopped part way");
              return e;
            }
          if (plen > k)
            {
              e = rd (d, 0, plen - k);
              if (e < 0)
                return e;
            }
          in->len = k;
          if (toobig)
            pd_log ("mtp: data longer than buffer");
          continue;
        }
      if (type == MTP_RESP)
        {
          uint32_t rp[5] = { 0 };
          uint32_t np = plen / 4;
          uint8_t pb[20];
          if (np > 5)
            np = 5;
          e = rd (d, pb, np * 4);
          if (e < 0)
            return e;
          if (plen > np * 4)
            rd (d, 0, plen - np * 4);
          if (rtid != tid)
            {
              /* A late response to an earlier (abandoned) transaction */
              pd_log_hex ("mtp: stale response for tid", rtid);
              continue;
            }
          for (uint32_t i = 0; i < np; i++)
            rp[i] = get32 (pb + 4 * i);
          if (rparams)
            for (int i = 0; i < 5; i++)
              rparams[i] = rp[i];
          d->last_done = pd_time_cs ();
          d->answered = 1;
          return code;
        }
      /* Anything else here is out of step: skip it */
      e = rd (d, 0, plen);
      if (e < 0)
        return e;
    }
}

static int
transaction (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
             const void *out, uint32_t outlen, mtp_data *in, uint32_t *rparams)
{
  uint8_t cmd[32];
  int e;
  uint32_t tid = op == OP_OpenSession ? 0 : next_tid (d);
  d->transactions++;

  put32 (cmd, 12 + 4 * (uint32_t) nparams);
  gap_wait (d);
  put16 (cmd + 4, MTP_CMD);
  put16 (cmd + 6, op);
  put32 (cmd + 8, tid);
  for (int i = 0; i < nparams; i++)
    put32 (cmd + 12 + 4 * i, params[i]);
  if (pdfs_trace)
    {
      char b[140], *q = tnum (b, pd_time_cs () % 100000);
      q = tcat (q, " mtp> &");
      q = thex (q, op, 4);
      q = tcat (q, " t");
      q = tnum (q, tid);
      for (int i = 0; i < nparams; i++)
        {
          q = tcat (q, i ? " " : " (");
          q = thex (q, params[i], 8);
        }
      if (nparams)
        q = tcat (q, ")");
      if (out)
        {
          q = tcat (q, " +");
          q = tnum (q, outlen);
          q = tcat (q, " bytes out");
        }
      pd_log (b);
    }
  e = d->tr->write (d->tr, cmd, 12 + 4 * (uint32_t) nparams, d->timeout_cs);
  if (e < 0)
    {
      if (pdfs_trace)
        trace_state (d, e == PDT_TIMEOUT ? "the command wasn't taken" : "the command failed");
      return e;
    }

  if (out && d->fill)
    {
      /* Data from a callback, in pieces: each a whole number of USB packets
         (64K), the last excepted, so no transfer ends early */
      enum { PIECE = 65536 };
      uint8_t *buf = pd_alloc (PIECE);
      if (!buf)
        return MTP_ENOMEM;
      put32 (buf, 12 + outlen);
      put16 (buf + 4, MTP_DATA);
      put16 (buf + 6, op);
      put32 (buf + 8, tid);
      uint64_t off = 0;
      uint32_t head = 12;
      e = 0;
      while (e >= 0 && (off < outlen || head))
        {
          uint32_t k = PIECE - head;
          if (k > outlen - off)
            k = (uint32_t) (outlen - off);
          if (k && d->fill (d->fill_ctx, buf + head, off, k) < 0)
            {
              e = PDT_IO;
              break;
            }
          d->tr->more = off + k < outlen;
          e = d->tr->write (d->tr, buf, head + k, d->timeout_cs + (head + k) / 4096);
          off += k;
          head = 0;
        }
      d->tr->more = 0;
      pd_free (buf);
      if (e < 0)
        return e;
      /* the device may take a while to finish writing a big file */
      unsigned t = d->timeout_cs;
      if (d->timeout_cs < 6000 && outlen > (1u << 20))
        d->timeout_cs = 6000;
      e = read_response (d, op, tid, in, rparams);
      d->timeout_cs = t;
      return e;
    }
  if (out)
    {
      /* The data container travels as one transfer: header and payload
         contiguous, so out must have 12 bytes of headroom (mtp.h). */
      uint8_t *h = (uint8_t *) out - 12;
      uint8_t save[12];
      if (mtp_needs_zlp (d, outlen) && op != OP_SendObject && op != OP_SendPartialObject)
        {
          /* No zero-length packet possible: pad the dataset by a byte (the
             device reads the fields it knows and ignores the rest) */
          uint8_t *t = pd_alloc (12 + outlen + 1);
          if (!t)
            return MTP_ENOMEM;
          memcpy (t + 12, out, outlen);
          put32 (t, 12 + outlen + 1);
          put16 (t + 4, MTP_DATA);
          put16 (t + 6, op);
          put32 (t + 8, tid);
          e = d->tr->write (d->tr, t, 12 + outlen + 1, d->timeout_cs);
          pd_free (t);
          if (e < 0)
            return e;
          goto response;
        }
      memcpy (save, h, 12);
      put32 (h, 12 + outlen);
      put16 (h + 4, MTP_DATA);
      put16 (h + 6, op);
      put32 (h + 8, tid);
      e = d->tr->write (d->tr, h, 12 + outlen, d->timeout_cs + outlen / 4096);
      memcpy (h, save, 12);
      if (e < 0)
        return e;
    }

response:
  return read_response (d, op, tid, in, rparams);
}

int
mtp_recover (mtp_dev *d, int level)
{
  d->recoveries++;
  pd_log_hex ("mtp: recovering, level", (uint32_t) level);
  d->stage_pos = d->stage_len = 0;
  int e = d->tr->reset ? d->tr->reset (d->tr, d->tid, level) : 0;
  if (e < 0)
    return e;
  d->broken = 0;
  if (e == 1 || level >= 2 || !d->open)
    {
      /* The session may have gone with the connection: open it again */
      uint32_t p = d->session_id;
      d->tid = 0;
      int rc = transaction (d, OP_OpenSession, &p, 1, 0, 0, 0, 0);
      if (rc == RC_SessionAlreadyOpen)
        rc = RC_OK;
      if (rc != RC_OK)
        {
          d->broken = 1;
          return rc < 0 ? rc : PDT_IO;
        }
      d->open = 1;
    }
  return 0;
}

static int transaction_r (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
                          const void *out, uint32_t outlen, mtp_data *in, uint32_t *rparams);

int
mtp_transaction (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
                 const void *out, uint32_t outlen, mtp_data *in, uint32_t *rparams)
{
  if (d->streaming && d->busy_wait)
    d->busy_wait (d);
  if (d->streaming)
    return MTP_EBUSY;
  if (d->rstreaming && d->rbusy)
    d->rbusy (d);               /* its reader may take the rest now */
  if (d->rstreaming)
    mtp_rstream_end (d);        /* what's left of it is dropped: its reader starts again */
  unsigned t0 = pd_time_cs ();
  int rc = transaction_r (d, op, params, nparams, out, outlen, in, rparams);
  unsigned dt = pd_time_cs () - t0;
  if (pdfs_trace)
    {
      char b[120], *q = tcat (b, "mtp: &");
      q = thex (q, op, 4);
      q = tcat (q, rc >= 0 ? " answered &" : " failed, error ");
      q = rc >= 0 ? thex (q, (uint32_t) rc, 4) : tnum (q, (uint32_t) -rc);
      if (in && rc >= 0)
        {
          q = tcat (q, ", ");
          q = tnum (q, in->len);
          q = tcat (q, " bytes");
        }
      q = tcat (q, " in ");
      q = tnum (q, dt);
      q = tcat (q, " cs");
      pd_log (b);
    }
  if (dt > d->slowest_cs)
    {
      d->slowest_cs = dt;
      d->slowest_op = op;
    }
  if (rc == RC_OK && in && in->len > 1024)
    {
      d->bytes_in += in->len;
      d->in_cs += dt;
    }
  if (rc == RC_OK && outlen > 1024)
    {
      d->bytes_out += outlen;
      d->out_cs += dt;
    }
  return rc;
}

/* mtp_transaction without the measuring */
static int
transaction_r (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
               const void *out, uint32_t outlen, mtp_data *in, uint32_t *rparams)
{
  if (d->broken)
    {
      int e = mtp_recover (d, 1);
      if (e < 0)
        e = mtp_recover (d, 2);
      if (e < 0)
        return e;
    }
  int owned = in && !in->buf;
  d->repeatable = asks_only (op);
  int rc = transaction (d, op, params, nparams, out, outlen, in, rparams);
  /* The answer was lost on the way (the transport has cleared the input
     pipe): a request that only reads is asked again, twice at most */
  for (int again = 0; rc == PDT_LOST && again < 2 && asks_only (op); again++)
    {
      d->lost++;
      pd_log_hex ("mtp: the answer was lost on the way: asking again, op", op);
      d->stage_pos = d->stage_len = 0;
      if (owned && in->buf)
        {
          pd_free (in->buf);
          in->buf = 0;
        }
      if (in)
        in->len = 0;
      rc = transaction (d, op, params, nparams, out, outlen, in, rparams);
    }
  d->repeatable = 0;
  if (rc == PDT_LOST)
    rc = PDT_IO;                /* lost a third time */
  if (rc < 0 && rc != MTP_ENOMEM)
    {
      d->errors++;
      d->broken = 1;
      pd_log_hex ("mtp: transfer failed for op", op);
      pd_log_hex ("mtp: error", (uint32_t) -rc);
      if (owned && in->buf)
        {
          pd_free (in->buf);
          in->buf = 0;
        }
      /* Recover now, so the next call finds the device usable; the caller
         still sees this call's error. */
      if (rc != PDT_GONE && !d->opening && mtp_recover (d, 1) < 0)
        mtp_recover (d, 2);
    }
  else if (rc == RC_SessionNotOpen && op != OP_OpenSession)
    {
      /* The device lost our session (it was reset): open it again, retry once */
      d->open = 0;
      if (mtp_recover (d, 1) == 0)
        rc = transaction (d, op, params, nparams, out, outlen, in, rparams);
    }
  return rc;
}

/* --- device info and session ------------------------------------------------- */

static int
skip_array16 (const uint8_t **p, const uint8_t *end, uint16_t *keep, int *nkeep, int max)
{
  if (*p + 4 > end)
    return -1;
  uint32_t n = get32 (*p);
  *p += 4;
  if (*p + 2 * n > end)
    return -1;
  for (uint32_t i = 0; i < n; i++)
    if (keep && *nkeep < max)
      keep[(*nkeep)++] = get16 (*p + 2 * i);
  *p += 2 * n;
  return 0;
}

int
mtp_open (mtp_dev *d)
{
  /* A device that doesn't answer its first command isn't recovered and
     tried again here: give up quickly, and the filer tries again later.
     A phone set to charging only can enumerate and never answer, or drop
     off the bus as it switches to file transfer; waiting for it (10
     seconds, then recovering and waiting again) held up the desktop,
     and reopening streams on a phone that was going crashed USBDriver. */
  /* As OpenMTP (go-mtpfs) and libmtp do: the session first, numbered at
     random (odd: never 0 or &FFFFFFFF), with transaction 0 (the PTP
     standard's number for OpenSession); then the device's description,
     inside the session. 0.16 test 7 and before asked for the description
     first, outside a session; an Amazon Fire stopped answering a few
     requests later (OpenMTP, which opens the session first, works). */
  mtp_data in = { 0, 0, 0 };
  unsigned saved_timeout = d->timeout_cs;
  if (d->timeout_cs > 300)
    d->timeout_cs = 300;        /* (pdfs_mount sets less) */
  uint32_t sid = (pd_time_cs () * 2654435761u) | 1;
  if (sid == 0xFFFFFFFFu)
    sid = 0x12345679u;
  d->session_id = sid;
  d->tid = 0;
  d->opening = 1;
  int rc = mtp_transaction (d, OP_OpenSession, &sid, 1, 0, 0, 0, 0);
  d->opening = 0;
  if (rc == RC_SessionAlreadyOpen)
    {
      /* Left open by someone else (or by us, before a restart): start afresh */
      mtp_transaction (d, OP_CloseSession, 0, 0, 0, 0, 0, 0);
      d->tid = 0;
      rc = mtp_transaction (d, OP_OpenSession, &sid, 1, 0, 0, 0, 0);
      if (rc == RC_SessionAlreadyOpen)
        rc = RC_OK;
    }
  if (rc != RC_OK)
    {
      d->timeout_cs = saved_timeout;
      return rc < 0 ? rc : -rc;
    }
  d->open = 1;
  rc = mtp_transaction (d, OP_GetDeviceInfo, 0, 0, 0, 0, &in, 0);
  d->timeout_cs = saved_timeout;
  if (rc != RC_OK)
    {
      pd_free (in.buf);
      return rc < 0 ? rc : -rc;
    }
  const uint8_t *p = in.buf, *end = in.buf + in.len;
  char tmp[128];
  int k;
  if (p + 8 > end)
    goto bad;
  p += 8;                                       /* standard version, vendor ext ID, version */
  if ((k = mtp_get_string (p, end, tmp, sizeof tmp)) < 0)
    goto bad;
  p += k;
  /* vendor extensions, e.g. "microsoft.com: 1.0; android.com: 1.0;" */
  d->android = 0;
  for (int i = 0; tmp[i]; i++)
    {
      const char *a = "android.com";
      int j = 0;
      while (a[j] && tmp[i + j] == a[j])
        j++;
      if (!a[j])
        d->android = 1;
    }
  /* Android's GetObjectPropList is unreliable: some list a folder among its
     own children and leave children out, and some (an Amazon Fire) don't
     answer it for a big folder; libmtp doesn't use it on Android either.
     Folders are listed with GetObjectHandles and GetObjectInfo instead. */
  d->no_proplist = d->android;
  p += 2;                                       /* functional mode */
  d->nops = 0;
  if (skip_array16 (&p, end, d->ops, &d->nops, (int) (sizeof d->ops / sizeof d->ops[0])) < 0
      || skip_array16 (&p, end, 0, 0, 0) < 0      /* events */
      || skip_array16 (&p, end, 0, 0, 0) < 0      /* device properties */
      || skip_array16 (&p, end, 0, 0, 0) < 0      /* capture formats */
      || skip_array16 (&p, end, 0, 0, 0) < 0)     /* playback formats */
    goto bad;
  if ((k = mtp_get_string (p, end, d->manufacturer, sizeof d->manufacturer)) < 0)
    goto bad;
  p += k;
  if ((k = mtp_get_string (p, end, d->model, sizeof d->model)) < 0)
    goto bad;
  p += k;
  if ((k = mtp_get_string (p, end, d->version, sizeof d->version)) < 0)
    goto bad;
  p += k;
  mtp_get_string (p, end, d->serial, sizeof d->serial);
  pd_free (in.buf);
  if (mtp_tid_after_open)
    d->tid = mtp_tid_after_open;

  return 0;

bad:
  pd_free (in.buf);
  return MTP_EPROTO;
}

int
mtp_poll_event (mtp_dev *d, uint16_t *code, uint32_t *param1)
{
  /* One event container at a time, by the length in its header: events
     are 12 to 24 bytes, and several can be waiting (0.16 test 8 and
     before read up to 32 bytes at once, which could take one event and
     part of the next, and read every later one out of step). The
     transport gives nothing until a whole header is there. */
  uint8_t ev[64];
  if (!d->tr->event)
    return 0;
  int n = d->tr->event (d->tr, ev, 12);
  if (n < 12)
    return 0;
  uint32_t len = get32 (ev);
  if (len < 12 || len > sizeof ev || get16 (ev + 4) != MTP_EVENT)
    {
      /* out of step: drop what's waiting (fewer than 12 bytes at a time,
         so a part header goes too) and start afresh with the next one */
      while (d->tr->event (d->tr, ev, 11) > 0)
        ;
      pd_log ("mtp: the event channel was out of step: what was waiting was dropped");
      return 0;
    }
  if (len > 12)
    {
      int k = d->tr->event (d->tr, ev + 12, len - 12);
      if (k < 0)
        k = 0;
      for (uint32_t i = 12 + (uint32_t) k; i < len; i++)
        ev[i] = 0;
    }
  *code = get16 (ev + 6);
  *param1 = len >= 16 ? get32 (ev + 12) : 0;
  return 1;
}

int
mtp_transaction_fill (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
                      uint32_t outlen, mtp_fill_fn fill, void *ctx, uint32_t *rparams)
{
  static uint8_t mark[16];      /* "there is data": the callback gives it */
  if (d->streaming && d->busy_wait)
    d->busy_wait (d);           /* first: it may use fill itself */
  d->fill = fill;
  d->fill_ctx = ctx;
  int rc = mtp_transaction (d, op, params, nparams, mark + 12, outlen, 0, rparams);
  d->fill = 0;
  d->fill_ctx = 0;
  return rc;
}

/* --- streamed data phases ------------------------------------------------------- */

enum { SPIECE = 65536 };        /* a whole number of USB packets */

static int
stream_flush (mtp_dev *d, int last)
{
  if (!d->slen)
    return 0;
  d->tr->more = !last;
  unsigned t0 = pd_time_cs ();
  int e = d->tr->write (d->tr, d->sbuf, d->slen, d->timeout_cs + d->slen / 4096);
  unsigned dt = pd_time_cs () - t0;
  d->tr->more = 0;
  if (e >= 0)
    {
      d->bytes_out += d->slen;
      d->out_cs += dt;
    }
  if (dt > d->slowest_cs)
    {
      d->slowest_cs = dt;
      d->slowest_op = d->sop;
    }
  d->slen = 0;
  return e;
}

static void
stream_failed (mtp_dev *d)
{
  d->awaiting = 0;
  pd_free (d->sbuf);
  d->sbuf = 0;
  d->streaming = 0;
  d->errors++;
  d->broken = 1;
}

int
mtp_stream_begin (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams, uint32_t total)
{
  if (d->streaming && d->busy_wait)
    d->busy_wait (d);
  if (d->streaming)
    return MTP_EBUSY;
  if (d->rstreaming && d->rbusy)
    d->rbusy (d);
  if (d->rstreaming)
    mtp_rstream_end (d);
  if (d->broken && mtp_recover (d, 1) < 0 && mtp_recover (d, 2) < 0)
    return PDT_IO;
  d->sbuf = pd_alloc (SPIECE);
  if (!d->sbuf)
    return MTP_ENOMEM;
  uint8_t cmd[32];
  uint32_t tid = next_tid (d);
  d->transactions++;
  put32 (cmd, 12 + 4 * (uint32_t) nparams);
  gap_wait (d);
  put16 (cmd + 4, MTP_CMD);
  put16 (cmd + 6, op);
  put32 (cmd + 8, tid);
  for (int i = 0; i < nparams; i++)
    put32 (cmd + 12 + 4 * i, params[i]);
  int e = d->tr->write (d->tr, cmd, 12 + 4 * (uint32_t) nparams, d->timeout_cs);
  if (e < 0)
    {
      stream_failed (d);
      return e;
    }
  /* the data container's header goes with the first piece of data */
  put32 (d->sbuf, 12 + total);
  put16 (d->sbuf + 4, MTP_DATA);
  put16 (d->sbuf + 6, op);
  put32 (d->sbuf + 8, tid);
  d->slen = 12;
  d->sleft = total;
  d->stid = tid;
  d->sop = op;
  d->streaming = 1;
  return 0;
}

int
mtp_stream_put (mtp_dev *d, const void *data, uint32_t len)
{
  const uint8_t *p = data;
  if (!d->streaming)
    return PDT_IO;
  if (len > d->sleft)
    len = (uint32_t) d->sleft;
  while (len)
    {
      uint32_t k = SPIECE - d->slen;
      if (k > len)
        k = len;
      if (p)
        memcpy (d->sbuf + d->slen, p, k);
      else
        memset (d->sbuf + d->slen, 0, k);
      d->slen += k;
      d->sleft -= k;
      len -= k;
      if (p)
        p += k;
      if (d->slen == SPIECE || d->sleft == 0)
        {
          int e = stream_flush (d, d->sleft == 0);
          if (e < 0)
            {
              stream_failed (d);
              return e;
            }
        }
    }
  return 0;
}

/* The data phase's end: send what's owed (zeros) and the rest. The response
   is read by mtp_stream_result; till then other transactions wait. */
int
mtp_stream_close (mtp_dev *d)
{
  if (!d->streaming)
    return PDT_IO;
  if (d->awaiting)
    return 0;
  int e = 0;
  while (d->sleft && e >= 0)    /* owed: zeros (the caller deals with the result) */
    {
      uint32_t k = d->sleft > SPIECE ? SPIECE : (uint32_t) d->sleft;
      e = mtp_stream_put (d, 0, k);
    }
  if (e < 0)
    return e;
  e = stream_flush (d, 1);
  pd_free (d->sbuf);
  d->sbuf = 0;
  if (e < 0)
    {
      d->streaming = 0;
      d->errors++;
      d->broken = 1;
      return e;
    }
  d->awaiting = 1;
  return 0;
}

/* Its response: MTP_EAGAIN if it hasn't come within wait_cs (the device may
   take a while to finish writing a big file); wait_cs = -1: up to a minute. */
int
mtp_stream_result (mtp_dev *d, uint32_t *rparams, int wait_cs)
{
  if (!d->streaming || !d->awaiting)
    return PDT_IO;
  if (wait_cs >= 0 && d->stage_pos >= d->stage_len)
    {
      d->stage_pos = d->stage_len = 0;
      int got = d->tr->read (d->tr, d->stage, d->stage_size, (unsigned) wait_cs);
      if (got == PDT_TIMEOUT || got == 0)
        return MTP_EAGAIN;
      if (got < 0)
        {
          d->streaming = d->awaiting = 0;
          d->errors++;
          d->broken = 1;
          return got;
        }
      d->stage_len = (uint32_t) got;
    }
  d->streaming = d->awaiting = 0;
  unsigned t = d->timeout_cs;
  if (d->timeout_cs < 6000)
    d->timeout_cs = 6000;
  int e = read_response (d, d->sop, d->stid, 0, rparams);
  d->timeout_cs = t;
  if (e < 0)
    {
      d->errors++;
      d->broken = 1;
    }
  return e;
}

/* Give up a streamed transaction part way: the device is told to cancel
   it (or the pipes are reset) before the next transaction. */
void
mtp_stream_abort (mtp_dev *d)
{
  if (!d->streaming)
    return;
  stream_failed (d);
  if (mtp_recover (d, 1) < 0)
    mtp_recover (d, 2);
}

int
mtp_stream_end (mtp_dev *d, uint32_t *rparams)
{
  int e = mtp_stream_close (d);
  if (e < 0)
    return e;
  return mtp_stream_result (d, rparams, -1);
}

/* --- data phases received a piece at a time ---------------------------------------- */

int
mtp_rstream_begin (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams, uint64_t *total)
{
  if (d->streaming && d->busy_wait)
    d->busy_wait (d);
  if (d->streaming)
    return MTP_EBUSY;
  if (d->rstreaming && d->rbusy)
    d->rbusy (d);
  if (d->rstreaming)
    mtp_rstream_end (d);
  if (d->broken && mtp_recover (d, 1) < 0 && mtp_recover (d, 2) < 0)
    return PDT_IO;
  uint8_t cmd[32];
  uint32_t tid = next_tid (d);
  d->transactions++;
  put32 (cmd, 12 + 4 * (uint32_t) nparams);
  gap_wait (d);
  put16 (cmd + 4, MTP_CMD);
  put16 (cmd + 6, op);
  put32 (cmd + 8, tid);
  for (int i = 0; i < nparams; i++)
    put32 (cmd + 12 + 4 * i, params[i]);
  int e = d->tr->write (d->tr, cmd, 12 + 4 * (uint32_t) nparams, d->timeout_cs);
  for (;;)
    {
      uint8_t hdr[12];
      if (e >= 0)
        e = rd (d, hdr, 12);
      if (e < 0)
        {
          d->errors++;
          d->broken = 1;
          return e;
        }
      uint32_t len = get32 (hdr);
      uint16_t type = get16 (hdr + 4), code = get16 (hdr + 6);
      uint32_t rtid = get32 (hdr + 8);
      if (len < 12)
        return MTP_EPROTO;
      uint32_t plen = len == 0xFFFFFFFF ? 0 : len - 12;
      if (rtid != tid)
        {
          e = rd (d, 0, plen);  /* late, from an abandoned transaction */
          continue;
        }
      if (type == MTP_RESP)
        {
          rd (d, 0, plen);
          return code;          /* no data: refused */
        }
      if (type != MTP_DATA)
        {
          e = rd (d, 0, plen);
          continue;
        }
      d->rstreaming = 1;
      d->rleft = len == 0xFFFFFFFF ? ~(uint64_t) 0 : plen;
      d->rtid = tid;
      d->rop = op;
      d->rgen++;
      *total = d->rleft;
      return RC_OK;
    }
}

int
mtp_rstream_get (mtp_dev *d, void *dst, uint32_t len)
{
  if (!d->rstreaming)
    return PDT_IO;
  if (len > d->rleft)
    len = (uint32_t) d->rleft;
  unsigned t0 = pd_time_cs ();
  int e = rd (d, dst, len);
  unsigned dt = pd_time_cs () - t0;
  if (e < 0)
    {
      d->rstreaming = 0;
      d->errors++;
      d->broken = 1;
      return e;
    }
  d->rleft -= len;
  d->bytes_in += len;
  d->in_cs += dt;
  if (dt > d->slowest_cs)
    {
      d->slowest_cs = dt;
      d->slowest_op = d->rop;
    }
  return (int) len;
}

int
mtp_rstream_end (mtp_dev *d)
{
  if (!d->rstreaming)
    return PDT_IO;
  d->rstreaming = 0;
  int e = 0;
  /* the rest of it (a device sends the whole object; there's no stopping it) */
  while (d->rleft && d->rleft != ~(uint64_t) 0 && e >= 0)
    {
      uint32_t k = d->rleft > 0x100000 ? 0x100000 : (uint32_t) d->rleft;
      e = rd (d, 0, k);
      d->rleft -= k;
    }
  if (e < 0)
    {
      d->errors++;
      d->broken = 1;
      return e;
    }
  e = read_response (d, d->rop, d->rtid, 0, 0);
  if (e < 0)
    {
      d->errors++;
      d->broken = 1;
    }
  return e;
}
