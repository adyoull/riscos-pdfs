/* pdfs.h: the PDFS core's interface to its platform veneer.
   The veneer (riscos/ for the module, host/ for the Linux test harness)
   passes FileSwitch's register blocks to pdfs_fs_entry and turns the
   pd_error it returns into the platform's error.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef PDFS_H
#define PDFS_H

#include <stdint.h>
#include "transport.h"

#define PDFS_VERSION "0.02"

/* Filing system number: provisional until allocated by RISC OS Open. */
#define PDFS_FSNUM 0xFA

/* Registers in and out of a FileSwitch entry point. On RISC OS these are
   the real R0-R7; pointers fit because pd_reg is pointer-sized. */
typedef uintptr_t pd_reg;
typedef struct { pd_reg r[8]; } pd_regs;

typedef struct { int errnum; char errmess[252]; } pd_error;

/* FileSwitch entry points (FSEntry_...). */
enum { PD_OPEN, PD_GETBYTES, PD_PUTBYTES, PD_ARGS, PD_CLOSE, PD_FILE, PD_FUNC };
const pd_error *pdfs_fs_entry (int entry, pd_regs *r);

/* Devices */
const pd_error *pdfs_mount (pd_transport *tr);      /* takes ownership of tr */
const pd_error *pdfs_dismount (const char *disc);   /* disc name, or "" for all */
int pdfs_disc_count (void);
const char *pdfs_disc_name (int i);                 /* NULL past the end */
const char *pdfs_disc_desc (int i);
const pd_error *pdfs_free_space (const char *disc, uint64_t *size, uint64_t *free);                 /* the storage's description ("SD card") */
/* Write a description of the mounted devices, one line at a time. */
void pdfs_describe (void (*line) (const char *s));
/* Called now and then (from a callback or ticker on RISC OS): poll events. */
void pdfs_tick (void);
void pdfs_finish (void);

/* Settings */
extern unsigned pdfs_dir_ttl_cs;          /* how long a directory listing stays fresh */
extern unsigned pdfs_timeout_cs;          /* per-transfer time-out */
extern int pdfs_trace;                     /* log every FileSwitch call (*PDFSLog trace) */
extern int pdfs_hide_ext;                  /* show "Letter.txt" as "Letter" (*PDFSExtensions) */
void pdfs_set_hide_ext (int on);

/* Error helpers */
#define PDFS_ERRBASE (0x10000 | (PDFS_FSNUM << 8))

#endif
