/* pdfs.h: the PDFS core's interface to its platform veneer.
   The veneer (riscos/ for the module, host/ for the Linux test harness)
   passes FileSwitch's register blocks to pdfs_fs_entry and turns the
   pd_error it returns into the platform's error.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef PDFS_H
#define PDFS_H

#include <stdint.h>
#include "transport.h"

#define PDFS_VERSION "0.16.3"

/* Filing system number: provisional until allocated by RISC OS Open. */
#define PDFS_FSNUM 0xC9                /* 201: allocated by ROOL (Nov 2026) */

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
extern int pdfs_mount_defer;    /* set while the filer mounts: storages are asked about from its poll */
const char *pdfs_disc_name (int i);                 /* NULL past the end */
const char *pdfs_disc_desc (int i);
int pdfs_eject_info (const char *disc, int all, int *storages, const char **model);
const pd_error *pdfs_eject (const char *disc, int all, int *gone);
void pdfs_uneject (void);
int pdfs_device_of (const char *disc);
const pd_error *pdfs_free_space (const char *disc, uint64_t *size, uint64_t *free);                 /* the storage's description ("SD card") */
/* Write a description of the mounted devices, one line at a time. */
void pdfs_describe (void (*line) (const char *s));
/* Called now and then (from a callback or ticker on RISC OS): poll events. */
void pdfs_tick (void);
void pdfs_finish (void);
/* Big new files sent in the background, by the filer task (fs.c) */
extern int pdfs_background;                /* sending in the background now */
extern int pdfs_background_option;         /* *PDFSBackground on: the filer task does it */
int pdfs_bg_run (unsigned cs);             /* send (and receive, pdfs_reading) for cs centiseconds; 1: more to do */
int pdfs_reading (void);                   /* a file is coming from a device in the background */
void pdfs_bg_finish (void);                /* send the rest now */
int pdfs_bg_progress (const char *disc);   /* percent, or -1: nothing being sent */
const pd_error *pdfs_bg_error (void);      /* a send that failed, once */
typedef struct
{
  int active;                   /* files waiting or being sent */
  int paused;
  char path[256];               /* the one being sent: "PDFS::disc.$.dir.name" */
  uint64_t left;                /* bytes still to send, of everything waiting */
  uint64_t batch, batch_done;   /* bytes since the queue was last empty, and sent */
  int files_done;               /* files sent since then */
  uint32_t owner;               /* the task that wrote the one being sent */
} pdfs_bg_state;
void pdfs_bg_status (pdfs_bg_state *s);
void pdfs_bg_pause (int on);
void pdfs_bg_abort_all (void);             /* stop: nothing half-sent is left */

/* Settings */
extern unsigned pdfs_dir_ttl_cs;          /* how long a directory listing stays fresh */
extern unsigned pdfs_timeout_cs;          /* per-transfer time-out */
const char *pdfs_waiting_untold (void);  /* a device waiting for Allow, to tell its owner about (once) */
void pdfs_settle_now (void);             /* finish mounts that wait for the filer's poll (from *commands) */
extern unsigned pdfs_settle_cs;
extern unsigned pdfs_hint_cs;            /* PDFS$AllowHint: cs before the "tap Allow" box (0 never) */
extern int pdfs_gap_cs;                  /* PDFS$Gap: cs between requests to Android devices (-1 default) */           /* an Android device is left this long after opening */
extern int pdfs_trace;                     /* log every FileSwitch call (*PDFSLog trace) */
extern int pdfs_hide_ext;                  /* show "Letter.txt" as "Letter" (*PDFSExtensions) */
void pdfs_set_hide_ext (int on);

/* Error helpers */
#define PDFS_ERRBASE (0x10000 | (PDFS_FSNUM << 8))

#endif
