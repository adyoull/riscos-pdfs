/* cmodule.c: small helpers for the test bed's C modules.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0
   (the same code is in riscos-linux-testbed under the MIT licence). */
#include "cmodule.h"

void *
rma_claim (size_t size)
{
  regs r = {{ 6, 0, 0, size }};
  if (swix_regs (OS_Module, &r))
    return NULL;
  return (void *) r.r[2];
}

void
rma_free (void *p)
{
  regs r = {{ 7, 0, (unsigned) p }};
  if (p)
    swix_regs (OS_Module, &r);
}

void
write0 (const char *s)
{
  regs r = {{ (unsigned) s }};
  swix_regs (OS_Write0, &r);
}

void
newline (void)
{
  regs r = {{ 0 }};
  swix_regs (OS_NewLine, &r);
}

void
write_hex8 (unsigned v)
{
  char buf[12];
  regs r = {{ v, (unsigned) buf, sizeof buf }};
  if (!swix_regs (OS_ConvertHex8, &r))
    write0 (buf);
}

void
write_dec (unsigned v)
{
  char buf[12];
  regs r = {{ v, (unsigned) buf, sizeof buf }};
  if (!swix_regs (OS_ConvertCardinal4, &r))
    write0 (buf);
}

void *
memset (void *d, int c, size_t n)
{
  unsigned char *p = d;
  while (n--)
    *p++ = (unsigned char) c;
  return d;
}

void *
memcpy (void *d, const void *s, size_t n)
{
  unsigned char *p = d;
  const unsigned char *q = s;
  if ((((unsigned) p | (unsigned) q | n) & 3) == 0)
    {
      unsigned *pw = (unsigned *) p;
      const unsigned *qw = (const unsigned *) q;
      for (n >>= 2; n; n--)
	*pw++ = *qw++;
      return d;
    }
  while (n--)
    *p++ = *q++;
  return d;
}

void *
memmove (void *d, const void *s, size_t n)
{
  unsigned char *p = d;
  const unsigned char *q = s;
  if (p <= q || p >= q + n)
    return memcpy (d, s, n);
  while (n--)
    p[n] = q[n];
  return d;
}

size_t
strlen (const char *s)
{
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}
