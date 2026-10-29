/* cmodule.h: the C side of the test bed's C modules (see cmodule.inc).
   Freestanding: no C library, no floating point, no absolute addresses
   outside the GOT (check-pic.sh).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0
   (the same code is in riscos-linux-testbed under the MIT licence). */
#ifndef CMODULE_H
#define CMODULE_H

#include <stddef.h>

typedef struct { int errnum; char errmess[252]; } oserror;
typedef struct { unsigned r[10]; } regs;

/* An error block in read-only data: ERROR_BLOCK (bad_param, 0x123, "Bad")
   at file level, then ERR (bad_param) is its oserror *. */
#define ERROR_BLOCK(name, num, msg) \
  static const struct { int errnum; char errmess[sizeof (msg)]; } \
    name##_block = { num, msg }
#define ERR(name) ((oserror *) &name##_block)

/* Module entry points the C code provides. */
oserror *mod_init (const char *tail, int instance, void *pw);
oserror *mod_final (int fatal);
oserror *mod_swi (unsigned number, regs *r);
void mod_service (regs *r);
oserror *mod_command (int index, const char *tail, int argc);

/* Calls a SWI (the X bit is added): r->r[0..9] in and out. */
oserror *swix_regs (unsigned swi, regs *r);

#define OS_WriteC		0x00
#define OS_Write0		0x02
#define OS_NewLine		0x03
#define OS_Module		0x1E
#define OS_ChangeDynamicArea	0x2A
#define OS_DynamicArea		0x66
#define OS_Hardware		0x7A
#define OS_ConvertHex8		0xD4
#define OS_ConvertCardinal4	0xD8

/* Small helpers in cmodule.c */
void *rma_claim (size_t size);
void rma_free (void *p);
void write0 (const char *s);
void newline (void);
void write_hex8 (unsigned v);
void write_dec (unsigned v);

void *memset (void *d, int c, size_t n);
void *memcpy (void *d, const void *s, size_t n);
void *memmove (void *d, const void *s, size_t n);
size_t strlen (const char *s);

#endif
