/* filer.h: PDFS's desktop filer task (filer.c).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef FILER_H
#define FILER_H

extern int filer_task;          /* 0: not running, -1: starting, else the task handle */
extern int filer_quitted;       /* the task was quit (Task Manager, or the desktop closing):
                                   not started again until the next desktop or *Desktop_PDFSFiler */

oserror *filer_prepare (void *pw);  /* remembers the private word */
void filer_main (void);         /* the task */
void filer_entry (void);        /* the module's start code (USR mode, from pd_start) */
void filer_quit (void);
void filer_finalise (void);     /* module finalising */
void filer_started (void);      /* module.c: the task is running (the post-filter goes) */

#endif
