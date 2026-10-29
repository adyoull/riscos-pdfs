/* plat.h: what the portable PDFS core needs from the platform it runs on.
   Each platform (riscos/, host/) provides these.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef PDFS_PLAT_H
#define PDFS_PLAT_H

#include <stddef.h>
#include <stdint.h>

void *pd_alloc (size_t n);              /* zeroed; NULL if out of memory */
void pd_free (void *p);
void *pd_realloc (void *p, size_t n);   /* contents kept; NULL on failure (p kept) */

unsigned pd_time_cs (void);
uint32_t pd_current_task (void);        /* the Wimp task running now (0: none) */             /* monotonic centiseconds */
uint32_t pd_unix_time (void);           /* seconds since 1970, UTC */

/* Log a line (no newline in s). Platforms may ignore it unless logging is on. */
void pd_log (const char *s);
void pd_log_hex (const char *what, uint32_t v);

/* RISC OS filetype for a file name extension ("txt", "pdf"), or -1 if not known.
   On RISC OS this asks MimeMap; elsewhere it uses a small built-in table. */
int pd_ext_to_filetype (const char *ext);
/* Extension for a filetype, written to buf (up to 15 chars), or 0 if none. */
int pd_filetype_to_ext (int type, char *buf);

/* Scrap files, for files being written that are too big to keep in memory.
   pd_spill_open: a new empty one, for about hint bytes (a handle, or 0 if
   there's nowhere to put it); RISC OS keeps it in a dynamic area when
   there's the memory, else in a scrap file. read returns the bytes read (fewer at the end), or -1; write
   returns 0 or -1; setext sets its length; close deletes it. */
int pd_spill_open (uint64_t hint);
int pd_spill_read (int h, uint64_t off, void *buf, uint32_t len);
int pd_spill_write (int h, uint64_t off, const void *buf, uint32_t len);
int pd_spill_setext (int h, uint64_t ext);
void pd_spill_close (int h);

/* Freestanding helpers (the RISC OS module has no C library). */
void *memset (void *d, int c, size_t n);
void *memcpy (void *d, const void *s, size_t n);
void *memmove (void *d, const void *s, size_t n);
size_t strlen (const char *s);

#endif
