/* mtp.c: the MTP/PTP session layer. See mtp.h.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "mtp.h"
#include "plat.h"

#define STAGE_SIZE 16384

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
      if (got < 0)
        return got;
      if (got > 0)
        last = pd_time_cs ();
    }
  return 0;
}

/* --- transactions ------------------------------------------------------------ */

static int
transaction (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
             const void *out, uint32_t outlen, mtp_data *in, uint32_t *rparams)
{
  uint8_t cmd[32];
  int e;
  uint32_t tid = ++d->tid;
  d->transactions++;

  put32 (cmd, 12 + 4 * (uint32_t) nparams);
  put16 (cmd + 4, MTP_CMD);
  put16 (cmd + 6, op);
  put32 (cmd + 8, tid);
  for (int i = 0; i < nparams; i++)
    put32 (cmd + 12 + 4 * i, params[i]);
  e = d->tr->write (d->tr, cmd, 12 + 4 * (uint32_t) nparams, d->timeout_cs);
  if (e < 0)
    return e;

  if (out)
    {
      /* The data container travels as one transfer: header and payload
         contiguous, so out must have 12 bytes of headroom (mtp.h). */
      uint8_t *h = (uint8_t *) out - 12;
      uint8_t save[12];
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

  if (in)
    in->len = 0;
  for (;;)
    {
      uint8_t hdr[12];
      e = rd (d, hdr, 12);
      if (e < 0)
        return e;
      uint32_t len = get32 (hdr);
      uint16_t type = get16 (hdr + 4), code = get16 (hdr + 6);
      uint32_t rtid = get32 (hdr + 8);
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
            return e;
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
          return code;
        }
      /* Anything else here is out of step: skip it */
      e = rd (d, 0, plen);
      if (e < 0)
        return e;
    }
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

int
mtp_transaction (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
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
  int rc = transaction (d, op, params, nparams, out, outlen, in, rparams);
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
      if (rc != PDT_GONE && mtp_recover (d, 1) < 0)
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
  mtp_data in = { 0, 0, 0 };
  int rc = mtp_transaction (d, OP_GetDeviceInfo, 0, 0, 0, 0, &in, 0);
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

  d->session_id = 1;
  d->tid = 0;
  uint32_t sp = d->session_id;
  rc = mtp_transaction (d, OP_OpenSession, &sp, 1, 0, 0, 0, 0);
  if (rc == RC_SessionAlreadyOpen)
    {
      /* Left open by someone else (or by us, before a restart): start afresh */
      mtp_transaction (d, OP_CloseSession, 0, 0, 0, 0, 0, 0);
      d->tid = 0;
      rc = mtp_transaction (d, OP_OpenSession, &sp, 1, 0, 0, 0, 0);
      if (rc == RC_SessionAlreadyOpen)
        rc = RC_OK;
    }
  if (rc != RC_OK)
    return rc < 0 ? rc : -rc;
  d->open = 1;
  return 0;

bad:
  pd_free (in.buf);
  return MTP_EPROTO;
}

int
mtp_poll_event (mtp_dev *d, uint16_t *code, uint32_t *param1)
{
  uint8_t ev[32];
  if (!d->tr->event)
    return 0;
  int n = d->tr->event (d->tr, ev, sizeof ev);
  if (n < 12 || get16 (ev + 4) != MTP_EVENT)
    return 0;
  *code = get16 (ev + 6);
  *param1 = n >= 16 ? get32 (ev + 12) : 0;
  return 1;
}
