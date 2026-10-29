/* transport.h: the interface between the MTP session layer and the thing that
   carries its containers: USB bulk endpoints on a real machine, a TCP
   connection to the simulator in testing. Only this layer differs between
   32-bit RISC OS, 64-bit RISC OS and the Linux test harness.
   Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0. */
#ifndef PDFS_TRANSPORT_H
#define PDFS_TRANSPORT_H

#include <stdint.h>

#define PDT_TIMEOUT   (-1)      /* nothing arrived in time */
#define PDT_IO        (-2)      /* the transfer failed */
#define PDT_GONE      (-3)      /* the device has been unplugged / disconnected */

typedef struct pd_transport pd_transport;
struct pd_transport
{
  /* Send len bytes (one USB bulk OUT transfer). 0 or a PDT_ error. */
  int (*write) (pd_transport *t, const void *buf, uint32_t len, unsigned timeout_cs);
  /* Receive up to max bytes (one bulk IN transfer, or what a stream has).
     Returns the number of bytes (0 = an empty transfer, e.g. a USB ZLP),
     or a PDT_ error. */
  int (*read) (pd_transport *t, void *buf, uint32_t max, unsigned timeout_cs);
  /* Poll the event (interrupt) channel: up to max bytes, 0 if nothing. */
  int (*event) (pd_transport *t, void *buf, uint32_t max);
  /* Recover after an error: cancel the transaction in progress (MTP class
     request Cancel), wait for the device to be ready, drop stale input.
     level 2 = also reset the pipes / reconnect. Returns 0, 1 if the session
     was lost (e.g. a reconnection: the caller opens it again), or a PDT_ error. */
  int (*reset) (pd_transport *t, uint32_t transaction_id, int level);
  void (*close) (pd_transport *t);
  const char *name;           /* for messages: "USB", "TCP 127.0.0.1:7015" */
  /* USB bulk OUT: the packet size, and whether a transfer that's a whole
     number of packets can't be ended with a zero-length packet (RISC OS's
     DeviceFS streams). Then PDFS never sends one: it pads datasets by a
     byte and sends file data in two parts. 0, 0 for a stream (TCP). */
  uint32_t max_packet;
  int no_zlp;
  /* Set by the MTP layer around write: 1 while more of the same container
     follows this write (a container sent in pieces), 0 for its last. */
  int more;
  /* Set by the transport when the device has been unplugged. */
  volatile int gone;
};

#endif
