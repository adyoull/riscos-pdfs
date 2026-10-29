/* sendwin.h: the window shown while files are sent in the background (sendwin.c).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef SENDWIN_H
#define SENDWIN_H

void sendwin_update (void);     /* open, update or close it to match the sends */
int sendwin_event (int reason, unsigned *block);   /* 1: the event was for it */
void sendwin_finish (void);     /* the task is stopping */

#endif
