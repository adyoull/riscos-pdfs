/* filer.h: PDFS's desktop filer task (filer.c).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef FILER_H
#define FILER_H

extern int filer_task;          /* 0: not running, -1: starting, else the task handle */
extern unsigned filer_sp;       /* the task's stack top, for pd_start (header.s) */

oserror *filer_prepare (void *pw);  /* before entering the task */
void filer_main (void);         /* the task (USR mode, from pd_start) */
void filer_quit (void);
void filer_finalise (void);     /* module finalising */

#endif
