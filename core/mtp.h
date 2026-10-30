/* mtp.h: the MTP/PTP session layer: containers, transactions, recovery.
   Written from the USB-IF MTP 1.1 specification and ISO 15740 (PTP).
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef PDFS_MTP_H
#define PDFS_MTP_H

#include <stdint.h>
#include "transport.h"

/* Container types */
#define MTP_CMD    1
#define MTP_DATA   2
#define MTP_RESP   3
#define MTP_EVENT  4

/* Operations */
#define OP_GetDeviceInfo          0x1001
#define OP_OpenSession            0x1002
#define OP_CloseSession           0x1003
#define OP_GetStorageIDs          0x1004
#define OP_GetStorageInfo         0x1005
#define OP_GetNumObjects          0x1006
#define OP_GetObjectHandles       0x1007
#define OP_GetObjectInfo          0x1008
#define OP_GetObject              0x1009
#define OP_DeleteObject           0x100B
#define OP_SendObjectInfo         0x100C
#define OP_SendObject             0x100D
#define OP_MoveObject             0x1019
#define OP_GetPartialObject       0x101B
#define OP_GetObjectPropsSupported 0x9801
#define OP_GetObjectPropValue     0x9803
#define OP_SetObjectPropValue     0x9804
#define OP_GetObjectPropList      0x9805
/* Android extensions */
#define OP_GetPartialObject64     0x95C1
#define OP_SendPartialObject      0x95C2
#define OP_TruncateObject         0x95C3
#define OP_BeginEditObject        0x95C4
#define OP_EndEditObject          0x95C5

/* Responses */
#define RC_OK                     0x2001
#define RC_GeneralError           0x2002
#define RC_SessionNotOpen         0x2003
#define RC_OperationNotSupported  0x2005
#define RC_InvalidStorageID       0x2008
#define RC_InvalidObjectHandle    0x2009
#define RC_StoreFull              0x200C
#define RC_ObjectWriteProtected   0x200D
#define RC_StoreReadOnly          0x200E
#define RC_AccessDenied           0x200F
#define RC_DeviceBusy             0x2019
#define RC_InvalidParentObject    0x201A
#define RC_InvalidParameter       0x201D
#define RC_SessionAlreadyOpen     0x201E
#define RC_TransactionCancelled   0x201F
#define RC_SpecByDepthUnsupported 0xA808

/* Events */
#define EV_ObjectAdded            0x4002
#define EV_ObjectRemoved          0x4003
#define EV_StoreAdded             0x4004
#define EV_StoreRemoved           0x4005
#define EV_ObjectInfoChanged      0x4007
#define EV_StorageInfoChanged     0x400C

/* Object formats and properties */
#define FMT_Undefined             0x3000
#define FMT_Association           0x3001
#define PROP_StorageID            0xDC01
#define PROP_ObjectFormat         0xDC02
#define PROP_ObjectSize           0xDC04
#define PROP_ObjectFileName       0xDC07
#define PROP_DateModified         0xDC09
#define PROP_ParentObject         0xDC0B
#define DT_UINT16                 0x0004
#define DT_UINT32                 0x0006
#define DT_UINT64                 0x0008
#define DT_UINT128                0x000A
#define DT_STR                    0xFFFF

/* Negative results from mtp_transaction: transport errors (transport.h)
   and these. */
#define MTP_EBUSY    (-12)        /* a streamed transaction is open */
#define MTP_EAGAIN   (-13)        /* no response yet (mtp_stream_result) */
#define MTP_EPROTO   (-10)        /* the device sent something malformed */
#define MTP_ENOMEM   (-11)
#define MTP_ETOOBIG  (-14)        /* more data than the caller's buffer (was -12, the same as MTP_EBUSY) */

typedef int (*mtp_fill_fn) (void *ctx, uint8_t *dst, uint64_t off, uint32_t len);

typedef struct
{
  pd_transport *tr;
  uint32_t session_id;
  uint32_t tid;                 /* last transaction ID used */
  int open;                     /* session open */
  int broken;                   /* a transfer failed: reset before next use */
  unsigned timeout_cs;          /* per transfer: no progress for this long = stalled */
  uint16_t ops[160];            /* operations supported (from GetDeviceInfo) */
  int nops;
  char manufacturer[48], model[48], serial[40], version[24];
  /* input staging (bytes received beyond the container being read) */
  uint8_t *stage;
  uint32_t stage_len, stage_pos, stage_size;
  /* statistics */
  uint32_t transactions, errors, recoveries;
  mtp_fill_fn fill;             /* data out from a callback (mtp_transaction_fill) */
  /* A data phase being sent a piece at a time (mtp_stream_*) */
  int streaming;
  int awaiting;                 /* all sent: its response is still to be read */
  uint8_t *sbuf;
  uint32_t slen, stid;
  uint64_t sleft;               /* bytes of the container still to come */
  uint16_t sop;
  void *fill_ctx;
  /* Called when a transaction is wanted while one is being streamed: may
     finish the stream (PDFS's background sends) */
  void (*busy_wait) (void *d);
} mtp_dev;

/* Data received in a transaction. If buf is set, data goes there (up to cap
   bytes; more is MTP_ETOOBIG). If buf is NULL, it is collected in memory
   from pd_alloc (the caller frees buf). len = bytes received. */
typedef struct
{
  uint8_t *buf;
  uint32_t cap;
  uint32_t len;
} mtp_data;

int mtp_init (mtp_dev *d, pd_transport *tr);
void mtp_finish (mtp_dev *d);
/* Open the session and read the device information. */
int mtp_open (mtp_dev *d);
int mtp_supports (mtp_dev *d, uint16_t op);

/* One transaction. out/outlen: data to send (NULL for none); in: data to
   receive (NULL: none expected). Returns the response code (RC_...) or a
   negative error. rparams receives up to 5 response parameters. */
int mtp_transaction (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
                     const void *out, uint32_t outlen, mtp_data *in,
                     uint32_t *rparams);
/* The same, with the data to send (outlen bytes) fetched in pieces by
   fill(ctx, dst, offset, len), which returns 0 or -1: for files too big to
   hold in memory. Returns as mtp_transaction does. */
int mtp_transaction_fill (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams,
                          uint32_t outlen, mtp_fill_fn fill, void *ctx, uint32_t *rparams);

/* A transaction whose data is sent as it becomes available (a file being
   written): begin sends the operation and the data container's header
   (total bytes to follow); put sends data as it comes; end reads the
   response, sending zeros first for anything still owed. While one is
   open, other transactions call busy_wait (if set) and fail with
   MTP_EBUSY if it's still open. */
int mtp_stream_begin (mtp_dev *d, uint16_t op, const uint32_t *params, int nparams, uint32_t total);
int mtp_stream_put (mtp_dev *d, const void *data, uint32_t len);
int mtp_stream_end (mtp_dev *d, uint32_t *rparams);
/* end in two halves, so the response can be waited for a bit at a time */
int mtp_stream_close (mtp_dev *d);
int mtp_stream_result (mtp_dev *d, uint32_t *rparams, int wait_cs);
void mtp_stream_abort (mtp_dev *d);

/* Recover after a failed transfer (called by mtp_transaction itself). */
int mtp_recover (mtp_dev *d, int level);
/* Read one pending event, if any: returns 1 and fills code/param1. */
int mtp_poll_event (mtp_dev *d, uint16_t *code, uint32_t *param1);

/* Little-endian dataset helpers */
static inline uint16_t get16 (const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static inline uint32_t get32 (const uint8_t *p)
{ return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static inline uint64_t get64 (const uint8_t *p) { return get32 (p) | (uint64_t) get32 (p + 4) << 32; }
static inline void put16 (uint8_t *p, uint32_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static inline void put32 (uint8_t *p, uint32_t v)
{ p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24); }

/* MTP string at p (count byte + UTF-16LE): written as UTF-8 to out (size
   outsz, always terminated). Returns bytes used at p, or -1 if it runs past end. */
int mtp_get_string (const uint8_t *p, const uint8_t *end, char *out, int outsz);
/* Write UTF-8 s as an MTP string at p; returns bytes written (max 1+2*255). */
int mtp_put_string (uint8_t *p, const char *s);

/* Would a data phase of len bytes (plus the 12-byte header) need a
   zero-length packet that the transport can't send? */
static inline int
mtp_needs_zlp (const mtp_dev *d, uint32_t len)
{
  /* USB packet sizes are powers of two */
  return d->tr->no_zlp && d->tr->max_packet && ((12 + len) & (d->tr->max_packet - 1)) == 0;
}

#endif
