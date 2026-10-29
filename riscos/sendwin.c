/* sendwin.c: the window shown while PDFS sends files to a device in the
   background. It is Filer_Action's own window: its template (FCount) and
   its words come from Filer_Action's resources in the ROM, so it looks and
   reads as every other file copy does: "Copying files - Writing", the
   object's path, bytes to go, files copied, the progress bar, and Abort and
   Pause (Continue).

   If those resources can't be found, there's no window: the copy still
   goes on in the background.

   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "cmodule.h"
#include "pdfs.h"
#include "sendwin.h"

#define Wimp_CreateWindow       0x400C1
#define Wimp_DeleteWindow       0x400C3
#define Wimp_OpenWindow         0x400C5
#define Wimp_CloseWindow        0x400C6
#define Wimp_GetWindowState     0x400CB
#define Wimp_GetIconState       0x400CE
#define Wimp_SetIconState       0x400CD
#define Wimp_ForceRedraw        0x400D1
#define Wimp_OpenTemplate       0x400D9
#define Wimp_CloseTemplate      0x400DA
#define Wimp_LoadTemplate       0x400DB
#define Wimp_ResizeIcon         0x400FC
#define MessageTrans_OpenFile   0x41501
#define MessageTrans_Lookup     0x41502
#define MessageTrans_CloseFile  0x41504
#define TaskManager_TaskNameFromHandle 0x42680

/* FCount's icons (FilerAct h/actionwind) */
#define I_PATH          2
#define I_TOP_FIELD     3
#define I_BOTTOM_FIELD  4
#define I_TOP_LINE      5
#define I_BOTTOM_LINE   6
#define I_ABORT         7
#define I_NO_SKIP       8
#define I_YES_RETRY     9
#define I_MISC          10
#define I_SKIP          11
#define I_BAR           16
#define I_BAR_BOX       15

static int win;                 /* the window handle, or 0 */
static int unavailable;         /* the template couldn't be loaded */
static int is_open, paused_shown = -1;
static unsigned wdef[(88 + 32 * 20) / 4 + 4];
static char ind[2048];          /* the template's indirected data */
static char title[96], path[96], top[24], bottom[24], top_line[40], bottom_line[40], button[2][24];
static int bar_x0, bar_max, bar_last = -1;
static unsigned msgs[4];
static int msgs_open;

static int
str_len (const char *s)
{
  int n = 0;
  while (s[n])
    n++;
  return n;
}

static void
str_copy (char *d, const char *s, int size)
{
  int i = 0;
  for (; s[i] && i < size - 1; i++)
    d[i] = s[i];
  d[i] = 0;
}

static int
str_eq (const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

/* A word from Filer_Action's Messages (token), or the English one */
static void
word (char *out, int size, const char *token, const char *english)
{
  if (!msgs_open)
    {
      regs r = {{ (unsigned) msgs, (unsigned) "Resources:$.Resources.FilerAct.Messages", 0 }};
      msgs_open = swix_regs (MessageTrans_OpenFile, &r) ? -1 : 1;
    }
  if (msgs_open > 0)
    {
      regs r = {{ (unsigned) msgs, (unsigned) token, (unsigned) out, (unsigned) size, 0, 0, 0, 0 }};
      if (!swix_regs (MessageTrans_Lookup, &r))
        {
          out[r.r[3] < (unsigned) size ? r.r[3] : (unsigned) size - 1] = 0;
          for (int i = 0; out[i]; i++)
            if (out[i] < ' ')
              out[i] = 0;
          return;
        }
    }
  str_copy (out, english, size);
}

/* "12,345,678" */
static void
number (char *o, uint64_t v)
{
  char t[32];
  int k = 0, digits = 0;
  do
    {
      if (digits && digits % 3 == 0)
        t[k++] = ',';
      t[k++] = (char) ('0' + (int) (v % 10));
      v /= 10;
      digits++;
    }
  while (v);
  while (k)
    *o++ = t[--k];
  *o = 0;
}

static unsigned *
icon (int i)
{
  return &wdef[(88 + 32 * i) / 4];
}

/* Point an icon's indirected text at our buffer */
static void
own_text (int i, char *buf, int size)
{
  unsigned *ic = icon (i);
  if (!(ic[4] & 0x100))
    return;
  ic[5] = (unsigned) buf;
  ic[7] = (unsigned) size;
}

static void
redraw_icon (int i)
{
  unsigned b[4] = { (unsigned) win, (unsigned) i, 0, 0 };
  regs r = {{ 0, (unsigned) b }};
  swix_regs (Wimp_SetIconState, &r);
}

static void
set_text (int i, char *buf, int size, const char *s)
{
  if (str_eq (buf, s))
    return;
  str_copy (buf, s, size);
  redraw_icon (i);
}

static void
delete_icon (int i)
{
  unsigned *ic = icon (i);
  ic[4] |= 0x00800000;          /* deleted */
}

static int
load (void)
{
  regs o = {{ 0, (unsigned) "Resources:$.Resources.FilerAct.Templates" }};
  if (swix_regs (Wimp_OpenTemplate, &o))
    return -1;
  char name[12] = "FCount";
  regs l = {{ 0, (unsigned) wdef, (unsigned) ind, (unsigned) (ind + sizeof ind), (unsigned) -1,
              (unsigned) name, 0 }};
  oserror *e = swix_regs (Wimp_LoadTemplate, &l);
  regs c = {{ 0 }};
  swix_regs (Wimp_CloseTemplate, &c);
  if (e || l.r[6] == 0)
    return -1;
  unsigned nicons = wdef[21];
  if (nicons <= I_BAR || nicons > 20)
    return -1;
  /* our own buffers for what changes */
  if (wdef[14] & 0x100)                         /* title */
    {
      wdef[18] = (unsigned) title;
      wdef[20] = sizeof title;
    }
  own_text (I_PATH, path, sizeof path);
  own_text (I_TOP_FIELD, top, sizeof top);
  own_text (I_BOTTOM_FIELD, bottom, sizeof bottom);
  own_text (I_TOP_LINE, top_line, sizeof top_line);
  own_text (I_BOTTOM_LINE, bottom_line, sizeof bottom_line);
  own_text (I_ABORT, button[0], sizeof button[0]);
  own_text (I_MISC, button[1], sizeof button[1]);
  title[0] = path[0] = top[0] = bottom[0] = 0;
  word (top_line, sizeof top_line, "30", "bytes to go");
  word (bottom_line, sizeof bottom_line, "31", "files copied");
  word (button[0], sizeof button[0], "5", "Abort");
  word (button[1], sizeof button[1], "6", "Pause");
  delete_icon (I_NO_SKIP);
  delete_icon (I_YES_RETRY);
  delete_icon (I_SKIP);
  /* the bar: an empty one in its box */
  unsigned *box = icon (I_BAR_BOX), *bar = icon (I_BAR);
  bar_x0 = (int) box[0];
  bar_max = (int) box[2] - (int) box[0];
  bar[0] = box[0];
  bar[2] = box[0] + 2;
  bar_last = 2;
  regs w = {{ 0, (unsigned) wdef }};
  if (swix_regs (Wimp_CreateWindow, &w))
    return -1;
  win = (int) w.r[0];
  return 0;
}

static void
set_title (const char *t)
{
  if (str_eq (title, t))
    return;
  str_copy (title, t, sizeof title);
  regs r = {{ (unsigned) win, 0x4B534154, 3 }};     /* the title bar */
  swix_regs (Wimp_ForceRedraw, &r);
}

static void
set_bar (uint64_t done, uint64_t total)
{
  int size = total ? (int) ((done * (uint64_t) bar_max) / total) : 0;
  if (size < 2)
    size = 2;
  if (size > bar_max)
    size = bar_max;
  if (size == bar_last)
    return;
  unsigned *bar = icon (I_BAR);
  regs r = {{ (unsigned) win, I_BAR, (unsigned) bar_x0, bar[1], (unsigned) (bar_x0 + size), bar[3] }};
  swix_regs (Wimp_ResizeIcon, &r);
  /* redraw the whole box: a shorter bar leaves the old one otherwise */
  unsigned *box = icon (I_BAR_BOX);
  regs f = {{ (unsigned) win, box[0], box[1], box[2], box[3] }};
  swix_regs (Wimp_ForceRedraw, &f);
  bar_last = size;
}

static void
open_window (void)
{
  unsigned s[9];
  s[0] = (unsigned) win;
  regs g = {{ 0, (unsigned) s }};
  if (swix_regs (Wimp_GetWindowState, &g))
    return;
  s[7] = (unsigned) -1;                         /* on top */
  regs o = {{ 0, (unsigned) s }};
  swix_regs (Wimp_OpenWindow, &o);
  is_open = 1;
}

static void
close_window (void)
{
  unsigned b[1] = { (unsigned) win };
  regs c = {{ 0, (unsigned) b }};
  swix_regs (Wimp_CloseWindow, &c);
  is_open = 0;
}

/* Is this task a Filer_Action still running? Then its own window is (or
   may be) showing this copy: ours waits till it has gone, and then opens
   where Filer_Action's was (they're the same template), so the copy reads
   as one window going on from reading to writing. */
static int
filer_action_running (uint32_t task)
{
  if (!task)
    return 0;
  regs r = {{ task }};
  if (swix_regs (TaskManager_TaskNameFromHandle, &r))
    return 0;                                     /* gone */
  char name[48];
  word (name, sizeof name, "89", "Filer Action Window");
  const char *a = (const char *) r.r[0], *b = name;
  while (*a >= ' ' && *a == *b)
    a++, b++;
  return *a < ' ' && *b < ' ';
}

void
sendwin_update (void)
{
  pdfs_bg_state st;
  pdfs_bg_status (&st);
  if (!st.active)
    {
      if (win && is_open)
        close_window ();
      return;
    }
  if (!win)
    {
      if (unavailable || load () < 0)
        {
          unavailable = 1;
          return;
        }
    }
  /* "Copying files - Writing", or "Copying files - Paused" */
  char t[96], w[48];
  word (t, 48, "29", "Copying files");
  if (st.paused)
    word (w, sizeof w, "80", "Paused");
  else
    word (w, sizeof w, "82", "Writing");
  int k = str_len (t);
  str_copy (t + k, " - ", sizeof t - k);
  str_copy (t + k + 3, w, sizeof t - k - 3);
  set_title (t);
  /* the object's path: its end, if it's long */
  int pl = str_len (st.path);
  set_text (I_PATH, path, sizeof path, st.path + (pl > (int) sizeof path - 1 ? pl - (int) sizeof path + 1 : 0));
  char n[32];
  number (n, st.left);
  set_text (I_TOP_FIELD, top, sizeof top, n);
  number (n, (uint64_t) st.files_done);
  set_text (I_BOTTOM_FIELD, bottom, sizeof bottom, n);
  if (st.paused != paused_shown)
    {
      char b[24];
      if (st.paused)
        word (b, sizeof b, "26", "Continue");
      else
        word (b, sizeof b, "6", "Pause");
      set_text (I_MISC, button[1], sizeof button[1], b);
      paused_shown = st.paused;
    }
  set_bar (st.batch_done, st.batch);
  if (!is_open && !filer_action_running (st.owner))
    open_window ();
}

/* A Wimp event for the window? 1 if it was */
int
sendwin_event (int reason, unsigned *block)
{
  if (!win)
    return 0;
  switch (reason)
    {
    case 2:                                         /* Open_Window_Request */
      if ((int) block[0] != win)
        return 0;
      {
        regs o = {{ 0, (unsigned) block }};
        swix_regs (Wimp_OpenWindow, &o);
      }
      return 1;
    case 3:                                         /* Close_Window_Request */
      if ((int) block[0] != win)
        return 0;
      close_window ();
      return 1;
    case 6:                                         /* Mouse_Click */
      if ((int) block[3] != win)
        return 0;
      if ((int) block[4] == I_ABORT)
        {
          pdfs_bg_abort_all ();
          sendwin_update ();
        }
      else if ((int) block[4] == I_MISC)
        {
          pdfs_bg_state st;
          pdfs_bg_status (&st);
          pdfs_bg_pause (!st.paused);
          sendwin_update ();
        }
      return 1;
    }
  return 0;
}

int
sendwin_handle (void)
{
  return win;
}

/* The task is stopping */
void
sendwin_finish (void)
{
  if (win)
    {
      unsigned b[1] = { (unsigned) win };
      regs d = {{ 0, (unsigned) b }};
      swix_regs (Wimp_DeleteWindow, &d);
    }
  win = 0;
  is_open = 0;
  unavailable = 0;
  paused_shown = -1;
  if (msgs_open > 0)
    {
      regs c = {{ (unsigned) msgs }};
      swix_regs (MessageTrans_CloseFile, &c);
    }
  msgs_open = 0;
}
