/* filer.c: PDFS's desktop filer, a Wimp task inside the module, as
   ADFSFiler and SCSIFiler are: an icon on the left of the icon bar for each
   mounted storage of each portable device.

   - SELECT or ADJUST on an icon opens its root directory.
   - MENU: Open '$', Free, Eject (this storage), Eject all (when the device
     has more than one): safe to remove or unplug.
   - Files dragged to an icon are copied to that storage's root (Filer_Action);
     a save box dragged to it saves there.
   - Icons follow mounting and dismounting (checked once a second).

   The task is started by Service_StartWimp (*Desktop_PDFSFiler) or, when the
   module is loaded in the desktop, from a callback. It runs in USR mode on a
   stack inside the module (pd_task_stack: pd_start in header.s sets sp).
   Double-clicking the module (*RMRun) enters it too: in the desktop that
   starts the filer if it isn't running; otherwise it just returns.

   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#include "cmodule.h"
#include "pdfs.h"
#include "filer.h"
#include "usb.h"

#define OS_CLI                  0x05
#define OS_Exit                 0x11
#define Wimp_Initialise         0x400C0
#define Wimp_CreateIcon         0x400C2
#define Wimp_DeleteIcon         0x400C4
#define Wimp_GetPointerInfo     0x400CF
#define Wimp_CreateMenu         0x400D4
#define Wimp_CloseDown          0x400DD
#define Wimp_StartTask          0x400DE
#define Wimp_ReportError        0x400DF
#define Wimp_PollIdle           0x400E1
#define Wimp_SendMessage        0x400E7
#define Free_Register           0x444C0
#define Free_DeRegister         0x444C1

#define MAX_ICONS               16
#define STACK_SIZE              16384
#define ICON_PRIORITY           0x30000000      /* right of the discs; each next icon one lower */

int filer_task;                 /* 0: not running, -1: starting, else the task handle */
uint32_t pd_task_stack[STACK_SIZE / 4];   /* the task's stack (pd_start) */

/* Fixed slots: each icon's indirected text points into its slot, so slots
   never move while their icon exists */
typedef struct
{
  int used, handle;
  char disc[40];
  char text[16];
} baricon;

static baricon icons[MAX_ICONS];
static char menu_disc[40];      /* the disc whose menu is open */
static int poll_errors;
static unsigned block[64];      /* Wimp_Poll block */
static unsigned menu[7 + 4 * 6];

static const char validation[] = "Spdfs_dev";

/* --- small helpers ------------------------------------------------------------------- */

static int
str_eq (const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

static char *
append (char *o, const char *s)
{
  while (*s)
    *o++ = *s++;
  *o = 0;
  return o;
}

static oserror *
oscli (const char *a, const char *b, const char *c)
{
  char cmd[256], *p = cmd;
  p = append (p, a);
  if (b)
    p = append (p, b);
  if (c)
    append (p, c);
  regs r = {{ (unsigned) cmd }};
  return swix_regs (OS_CLI, &r);
}

static void
report (oserror *e)
{
  if (!e)
    return;
  regs r = {{ (unsigned) e, 1, (unsigned) "PDFS Filer" }};
  swix_regs (Wimp_ReportError, &r);
}

/* --- icons ------------------------------------------------------------------------------ */

static void
add_icon (const char *disc, const char *desc)
{
  baricon *b = 0;
  for (int k = 0; k < MAX_ICONS && !b; k++)
    if (!icons[k].used)
      b = &icons[k];
  if (!b)
    return;
  int i = 0;
  for (; disc[i] && i < 39; i++)
    b->disc[i] = disc[i];
  b->disc[i] = 0;
  for (i = 0; desc[i] && i < 12; i++)
    b->text[i] = desc[i];
  b->text[i] = 0;
  int w = i * 16 > 68 ? i * 16 : 68;
  unsigned blk[9];
  blk[0] = (unsigned) -5;                       /* left side, by priority */
  blk[1] = 0;
  blk[2] = (unsigned) -16;
  blk[3] = (unsigned) w;
  blk[4] = 84;
  blk[5] = 0x1700310B;                          /* text + sprite, centred, indirected, click */
  blk[6] = (unsigned) b->text;
  blk[7] = (unsigned) validation;
  blk[8] = sizeof b->text;
  static unsigned order;
  regs r = {{ ICON_PRIORITY - (order++ & 0xFFFF), (unsigned) blk }};
  if (!swix_regs (Wimp_CreateIcon, &r))
    {
      b->handle = (int) r.r[0];
      b->used = 1;
    }
}

static void
remove_icon (int k)
{
  unsigned blk[2] = { (unsigned) -2, (unsigned) icons[k].handle };
  regs r = {{ 0, (unsigned) blk }};
  swix_regs (Wimp_DeleteIcon, &r);
  icons[k].used = 0;
}

/* Make the icons match the mounted storages. */
static void
refresh (void)
{
  pdfs_tick ();                 /* device events; drops unplugged devices */
  /* a USB device plugged in: mount it. A phone may take a few seconds to be
     ready (or reconnects when File transfer is chosen): try a few times */
  static int attach_tries;
  if (usb_attached)
    {
      usb_attached = 0;
      attach_tries = 5;
    }
  if (attach_tries > 0)
    {
      int before = pdfs_disc_count ();
      oscli ("PDFSMount -auto", 0, 0);
      attach_tries = pdfs_disc_count () > before ? 0 : attach_tries - 1;
    }
  for (int k = 0; k < MAX_ICONS; k++)
    {
      if (!icons[k].used)
        continue;
      int found = 0;
      const char *d;
      for (int i = 0; (d = pdfs_disc_name (i)); i++)
        if (str_eq (d, icons[k].disc))
          found = 1;
      if (!found)
        {
          /* the device has gone: close its windows */
          oscli ("Filer_CloseDir PDFS::", icons[k].disc, ".$");
          remove_icon (k);
        }
    }
  const char *d;
  for (int i = 0; (d = pdfs_disc_name (i)); i++)
    {
      int found = 0;
      for (int k = 0; k < MAX_ICONS; k++)
        if (icons[k].used && str_eq (d, icons[k].disc))
          found = 1;
      if (!found)
        add_icon (d, pdfs_disc_desc (i));
    }
}

static int
icon_index (int handle)
{
  for (int k = 0; k < MAX_ICONS; k++)
    if (icons[k].used && icons[k].handle == handle)
      return k;
  return -1;
}

static void
open_root (int k)
{
  report (oscli ("Filer_OpenDir PDFS::", icons[k].disc, ".$"));
}

/* --- ejecting -------------------------------------------------------------------------- */

/* Eject one storage, or (all) every storage of its device. Refused while
   files are open on it. Its windows close and its icons go; when the device
   has nothing left mounted, its session is closed and it can be unplugged. */
static void
eject (int k, int all)
{
  char disc[40], label[16], msg[200];
  const char *model = 0;
  int left = 0;
  for (int i = 0; i < 40; i++)
    disc[i] = icons[k].disc[i];
  for (int i = 0; i < 16; i++)
    label[i] = icons[k].text[i];
  if (pdfs_eject_info (disc, all, &left, &model) < 0)
    return;
  char name[48];
  int i = 0;
  for (; model && model[i] && i < 47; i++)
    name[i] = model[i];
  name[i] = 0;
  int gone = all || left <= 1;
  /* the windows: of this storage, or of each of the device's */
  int dev = pdfs_device_of (disc);
  for (int j = 0; j < MAX_ICONS; j++)
    if (icons[j].used && (j == k || (gone && pdfs_device_of (icons[j].disc) == dev)))
      oscli ("Filer_CloseDir PDFS::", icons[j].disc, ".$");
  oserror *e = oscli ("PDFSEject ", disc, all ? " all" : 0);
  refresh ();
  if (e)
    {
      report (e);
      return;
    }
  char *p = msg + 4;
  if (gone)
    append (append (p, name), " has been ejected: it's safe to unplug it.");
  else
    append (append (append (append (p, "'"), label), "' has been ejected from "), name);
  *(unsigned *) msg = 0;
  regs r = {{ (unsigned) msg, 1 | 1 << 8 | 1 << 9, (unsigned) "PDFS Filer" }};   /* OK, information */
  swix_regs (Wimp_ReportError, &r);
}

/* --- menu ------------------------------------------------------------------------------- */

static void
menu_item (unsigned *m, unsigned flags, const char *text)
{
  m[0] = flags;
  m[1] = (unsigned) -1;
  m[2] = 0x07000021;                             /* text, filled, black on white */
  char *t = (char *) &m[3];
  int i = 0;
  for (; text[i] && i < 12; i++)
    t[i] = text[i];
  if (i < 12)
    t[i] = 0;
}

static void
show_menu (int k, int x)
{
  char *title = (char *) menu;
  const char *t = "PDFS";
  int i = 0;
  for (; t[i]; i++)
    title[i] = t[i];
  title[i] = 0;
  menu[3] = 0x00070207;                          /* title colours, work area colours */
  menu[4] = 9 * 16 + 12;                         /* width */
  menu[5] = 44;
  menu[6] = 0;
  menu_item (&menu[7], 0, "Open '$'");
  menu_item (&menu[13], 0, "Free...");
  /* a device with more than one storage mounted: Eject this one, or all */
  int left = 1;
  pdfs_eject_info (icons[k].disc, 0, &left, 0);
  int items = left > 1 ? 4 : 3;
  menu_item (&menu[19], items == 3 ? 0x80 : 0, "Eject");
  if (items == 4)
    menu_item (&menu[25], 0x80, "Eject all");
  for (int i = 0; i < 40; i++)
    menu_disc[i] = icons[k].disc[i];
  regs r = {{ 0, (unsigned) menu, (unsigned) (x - 64), (unsigned) (96 + items * 44) }};
  swix_regs (Wimp_CreateMenu, &r);
}

static void
menu_select (void)
{
  int k = -1;
  for (int j = 0; j < MAX_ICONS; j++)
    if (icons[j].used && str_eq (icons[j].disc, menu_disc))
      k = j;
  if (k < 0)
    return;                     /* its device has gone */
  unsigned ptr[5];
  regs pr = {{ 0, (unsigned) ptr }};
  swix_regs (Wimp_GetPointerInfo, &pr);
  switch ((int) block[0])
    {
    case 0:
      open_root (k);
      break;
    case 1:
      report (oscli ("ShowFree -fs PDFS ", icons[k].disc, 0));
      break;
    case 2:
    case 3:
      eject (k, (int) block[0] == 3);
      return;                   /* the menu has gone with the icon */
    }
  if (ptr[2] & 1)               /* ADJUST: keep the menu open */
    {
      regs r = {{ 0, (unsigned) menu, 0, 0 }};
      swix_regs (Wimp_CreateMenu, &r);
    }
}

/* --- messages --------------------------------------------------------------------------- */

static void
send (unsigned *msg, unsigned action, int size, unsigned to, unsigned your_ref)
{
  msg[0] = (unsigned) (size + 3) & ~3u;
  msg[3] = your_ref;
  msg[4] = action;
  regs r = {{ 17, (unsigned) msg, to, 0 }};
  swix_regs (Wimp_SendMessage, &r);
}

/* Copy one object to a storage's root with Filer_Action (as the Filer does). */
static void
copy_to (int k, const char *path)
{
  int len = (int) strlen (path), dot = -1;
  for (int i = 0; i < len; i++)
    if (path[i] == '.')
      dot = i;
  if (dot <= 0 || len > 200)
    return;
  regs st = {{ (unsigned) "Filer_Action" }};
  if (swix_regs (Wimp_StartTask, &st) || !st.r[0])
    return;
  unsigned fa = st.r[0], m[64];
  char *s = (char *) &m[5];
  for (int i = 0; i < dot; i++)
    s[i] = path[i];
  s[dot] = 0;
  send (m, 0x403, 20 + dot + 1, fa, 0);               /* FilerSelectionDirectory */
  char *e = append (s, path + dot + 1);
  e[0] = ' ';
  e[1] = 0;
  send (m, 0x404, 20 + (int) strlen (s) + 1, fa, 0);  /* FilerAddSelection */
  m[5] = 0;                                           /* copy */
  m[6] = 1;                                           /* verbose */
  e = append (append (append ((char *) &m[7], "PDFS::"), icons[k].disc), ".$");
  send (m, 0x405, 28 + (int) (e - (char *) &m[7]) + 1, fa, 0);   /* FilerAction */
}

static void
message (int code)
{
  unsigned action = block[4];
  switch (action)
    {
    case 0:                     /* Message_Quit */
      filer_quit ();
      return;

    case 1:                     /* Message_DataSave: a save box dropped on an icon */
      {
        int k = icon_index ((int) block[6]);
        if ((int) block[5] != -2 || k < 0)
          return;
        char leaf[64];
        const char *n = (const char *) &block[11];
        int i = 0;
        for (; n[i] > ' ' && i < 63; i++)
          leaf[i] = n[i];
        leaf[i] = 0;
        char *e = append (append (append (append ((char *) &block[11], "PDFS::"), icons[k].disc), ".$."), leaf);
        send (block, 2, (int) (e - (char *) block) + 1, block[1], block[2]);   /* DataSaveAck */
        return;
      }

    case 3:                     /* Message_DataLoad: a file dropped on an icon */
      {
        int k = icon_index ((int) block[6]);
        if ((int) block[5] != -2 || k < 0)
          return;
        if (block[3] == 0)      /* not the end of a save to us: copy it */
          copy_to (k, (const char *) &block[11]);
        send (block, 4, (int) block[0], block[1], block[2]);   /* DataLoadAck */
        return;
      }

    case 0x502:                 /* Message_HelpRequest */
      {
        int k = icon_index ((int) block[9]);
        if ((int) block[8] != -2 || k < 0)
          return;
        char *e = append ((char *) &block[5],
                          "This is a portable device's storage (a phone, tablet or camera).|M"
                          "Click SELECT to open it.|MDrag files here to copy them to it.|M"
                          "Click MENU for its menu.");
        send (block, 0x503, (int) (e - (char *) block) + 1, block[1], block[2]);
        return;
      }
    }
  (void) code;
}

/* --- the Free module (*ShowFree, and the menu's Free...) ---------------------------------- */

void pd_free_entry (void);      /* header.s */
uint32_t pd_free_stack[1536];   /* its stack (6K) */
static void *private_word;
static int free_registered;

/* Free's reasons: 1 device name, 2 free space, 3 is this file on this
   device, 4 free space in 64 bits. r[3] is the device: our disc name. */
int
free_entry (unsigned *r)
{
  const char *disc = (const char *) r[3];
  switch (r[0])
    {
    case 1:
      {
        char *o = (char *) r[2];
        int i = 0;
        for (; disc[i] > ' '; i++)
          o[i] = disc[i];
        o[i] = 0;
        r[0] = (unsigned) i + 1;        /* with the terminator */
        return 0;
      }
    case 2:
    case 4:
      {
        uint64_t size = 0, free = 0;
        unsigned *o = (unsigned *) r[2];
        pdfs_free_space (disc, &size, &free);
        if (r[0] == 2)
          {
            o[0] = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned) size;
            o[1] = free > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned) free;
            o[2] = size - free > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned) (size - free);
          }
        else
          {
            uint64_t v[3] = { size, free, size - free };
            for (int i = 0; i < 3; i++)
              {
                o[2 * i] = (unsigned) v[i];
                o[2 * i + 1] = (unsigned) (v[i] >> 32);
              }
            r[0] = 0;
          }
        return 0;
      }
    case 3:
      {
        /* "PDFS::disc.$.x" or ":disc.$.x": is disc ours? */
        const char *f = (const char *) r[2];
        while (*f && *f != ':')
          f++;
        while (*f == ':')
          f++;
        int i = 0;
        for (; disc[i] > ' ' && f[i] && f[i] != '.'; i++)
          if ((disc[i] | 0x20) != (f[i] | 0x20))
            return 0;
        return disc[i] <= ' ' && (!f[i] || f[i] == '.');
      }
    }
  return 0;
}

static void
free_register (int on)
{
  if (on == free_registered)
    return;
  regs r = {{ PDFS_FSNUM, (unsigned) pd_free_entry, (unsigned) private_word }};
  if (!swix_regs (on ? Free_Register : Free_DeRegister, &r) || !on)
    free_registered = on;
}

/* --- the task --------------------------------------------------------------------------- */

void
filer_quit (void)
{
  free_register (0);
  if (filer_task > 0)
    {
      regs r = {{ (unsigned) filer_task, 0x4B534154 }};
      swix_regs (Wimp_CloseDown, &r);
    }
  filer_task = 0;
  for (int k = 0; k < MAX_ICONS; k++)
    icons[k].used = 0;
  menu_disc[0] = 0;
  regs x = {{ 0, 0, 0 }};
  swix_regs (OS_Exit, &x);
}

void
filer_main (void)
{
  static const unsigned msgs[] = { 1, 3, 0x502, 0 };
  regs r = {{ 310, 0x4B534154, (unsigned) "PDFS Filer", (unsigned) msgs }};
  if (swix_regs (Wimp_Initialise, &r))
    {
      filer_task = 0;
      regs x = {{ 0, 0, 0 }};
      swix_regs (OS_Exit, &x);
    }
  filer_task = (int) r.r[1];
  free_register (1);
  oscli ("IconSprites Resources:$.Resources.PDFS.Sprites", 0, 0);
  for (int k = 0; k < MAX_ICONS; k++)
    icons[k].used = 0;
  poll_errors = 0;
  oscli ("PDFSMount -auto", 0, 0);  /* devices already plugged in */
  refresh ();
  unsigned next = 0;
  for (;;)
    {
      regs t = {{ 0 }};
      swix_regs (0x42, &t);                         /* OS_ReadMonotonicTime */
      if ((int) (t.r[0] - next) >= 0)
        {
          refresh ();
          next = t.r[0] + 100;
        }
      regs p = {{ (1 << 4) | (1 << 5) | (1 << 11) | (1 << 12), (unsigned) block, next }};
      if (swix_regs (Wimp_PollIdle, &p))
        {
          if (++poll_errors > 10)     /* something is badly wrong: stop */
            filer_quit ();
          continue;
        }
      poll_errors = 0;
      switch ((int) p.r[0])
        {
        case 6:                                     /* Mouse_Click */
          if ((int) block[3] == -2)
            {
              int k = icon_index ((int) block[4]);
              if (k < 0)
                break;
              if (block[2] & 2)
                show_menu (k, (int) block[0]);
              else if (block[2] & 5)
                open_root (k);
            }
          break;
        case 9:                                     /* Menu_Selection */
          menu_select ();
          break;
        case 17:
        case 18:                                    /* User_Message(_Recorded) */
          message ((int) p.r[0]);
          break;
        }
    }
}

/* Remember the private word (for Free_Register) */
oserror *
filer_prepare (void *pw)
{
  private_word = pw;
  return 0;
}

/* The module's start code (pd_start), on the task's stack: entered by
   *Desktop_PDFSFiler (filer_task -1), or by *RMRun when the module is
   double-clicked. */
void
filer_entry (void)
{
  regs w = {{ 3 }};
  if (filer_task == -1
      || (filer_task == 0 && !swix_regs (0x400F2, &w) && w.r[0]))   /* Wimp_ReadSysInfo 3 */
    {
      filer_task = -1;
      filer_main ();
    }
  regs x = {{ 0, 0, 0 }};
  swix_regs (OS_Exit, &x);
}

void
filer_finalise (void)
{
  free_register (0);
  if (filer_task > 0)
    {
      regs r = {{ (unsigned) filer_task, 0x4B534154 }};
      swix_regs (Wimp_CloseDown, &r);
    }
  filer_task = 0;
}
