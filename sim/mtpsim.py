#!/usr/bin/env python3
"""mtpsim: a simulated MTP device (an Android-like phone) for testing PDFS.

It speaks MTP's bulk-pipe framing over TCP: the same containers (length,
type, code, transaction ID, payload) that travel over USB bulk endpoints,
one TCP connection standing in for the bulk IN/OUT pair. Storages are real
directories on the host, so tests can check results with ordinary file
operations.

    mtpsim.py [--port 7015] [--model SimPhone] STORAGE=DIR [STORAGE=DIR ...]

Options for testing PDFS's behaviour:
    --no-proplist     don't support GetObjectPropList (forces the fallback)
    --no-partial64    don't support Android's GetPartialObject64
    --no-edit         don't support Android's edit operations
    --no-rename       don't support SetObjectPropValue (renaming)
    --odd-proplist    as some Android SD cards: GetObjectPropList at depth 1
                      lists the folder itself among its children, and leaves
                      out every child after the first two (GetObjectHandles
                      is still right)
    --loop-handles    a folder's GetObjectHandles and property list include
                      the folder itself and the folder above it
    --zlp-strict N    fail data phases that are a multiple of N bytes (USB
                      without zero-length packets)
    --delay-ms N      sleep N ms before each response (a slow device)
    --stall-op CODE   never answer the first command with this code
                      (tests time-outs and recovery)
    --once            exit when the first connection closes

Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
"""
import argparse, os, socket, struct, sys, threading, time, datetime

# Container types
CMD, DATA, RESP, EVENT = 1, 2, 3, 4

# Operations
OP = dict(GetDeviceInfo=0x1001, OpenSession=0x1002, CloseSession=0x1003,
          GetStorageIDs=0x1004, GetStorageInfo=0x1005, GetNumObjects=0x1006,
          GetObjectHandles=0x1007, GetObjectInfo=0x1008, GetObject=0x1009,
          DeleteObject=0x100B, SendObjectInfo=0x100C, SendObject=0x100D,
          MoveObject=0x1019, GetPartialObject=0x101B,
          GetObjectPropsSupported=0x9801, GetObjectPropValue=0x9803,
          SetObjectPropValue=0x9804, GetObjectPropList=0x9805,
          GetPartialObject64=0x95C1, SendPartialObject=0x95C2,
          TruncateObject=0x95C3, BeginEditObject=0x95C4, EndEditObject=0x95C5)

# Responses
OK, GENERAL, NOT_OPEN, NOT_SUPPORTED, INVALID_STORAGE, INVALID_HANDLE = \
    0x2001, 0x2002, 0x2003, 0x2005, 0x2008, 0x2009
WRITE_PROTECTED, ACCESS_DENIED, INVALID_PARENT, INVALID_PARAM, ALREADY_OPEN = \
    0x200D, 0x200F, 0x201A, 0x201D, 0x201E
NO_VALID_INFO, PARAM_NOT_SUPPORTED = 0x2015, 0x2006
DEPTH_UNSUPPORTED = 0xA808

# Object formats and properties
FMT_UNDEFINED, FMT_ASSOCIATION = 0x3000, 0x3001
P_STORAGE, P_FORMAT, P_PROTECTION, P_SIZE, P_FILENAME, P_CREATED, P_MODIFIED, \
    P_PARENT = 0xDC01, 0xDC02, 0xDC03, 0xDC04, 0xDC07, 0xDC08, 0xDC09, 0xDC0B
T_UINT16, T_UINT32, T_UINT64, T_STR = 0x0004, 0x0006, 0x0008, 0xFFFF


def mtp_str(s):
    if s == "":
        return b"\x00"
    u = (s + "\x00").encode("utf-16-le")
    return bytes([len(u) // 2]) + u


def read_str(b, o):
    n = b[o]
    o += 1
    if n == 0:
        return "", o
    s = b[o:o + 2 * n].decode("utf-16-le").rstrip("\x00")
    return s, o + 2 * n


def mtp_date(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y%m%dT%H%M%S")


def parse_date(s):
    try:
        return datetime.datetime.strptime(s[:15], "%Y%m%dT%H%M%S").replace(
            tzinfo=datetime.timezone.utc).timestamp()
    except Exception:
        return None


class Device:
    def __init__(self, args):
        self.args = args
        self.model = args.model
        self.storages = []                    # (id, name, root path)
        for i, spec in enumerate(args.storage):
            name, path = spec.split("=", 1)
            os.makedirs(path, exist_ok=True)
            self.storages.append((0x10001 + i * 0x10000, name, os.path.abspath(path)))
        self.handles = {}                     # handle -> (storage id, path)
        self.paths = {}                       # path -> handle
        self.next_handle = 1
        self.pending = None                   # (storage, parent path, info) after SendObjectInfo
        self.stalled = False
        self.zlp_violations = 0
        self.ops = set(OP.values())
        if args.no_proplist:
            self.ops.discard(OP["GetObjectPropList"])
        if args.no_partial64:
            self.ops.discard(OP["GetPartialObject64"])
        if args.no_rename:
            self.ops.discard(OP["SetObjectPropValue"])
        if args.no_edit:
            for k in ("SendPartialObject", "TruncateObject", "BeginEditObject", "EndEditObject"):
                self.ops.discard(OP[k])
        self.lock = threading.Lock()

    # --- handles ---------------------------------------------------------
    def handle_for(self, sid, path):
        h = self.paths.get(path)
        if h is None:
            h = self.next_handle
            self.next_handle += 1
            self.paths[path] = h
            self.handles[h] = (sid, path)
        return h

    def storage(self, sid):
        for s in self.storages:
            if s[0] == sid:
                return s
        return None

    def lookup(self, h):
        v = self.handles.get(h)
        if v is None or not os.path.lexists(v[1]):
            return None
        return v

    def forget(self, path):
        for p in [p for p in self.paths if p == path or p.startswith(path + os.sep)]:
            h = self.paths.pop(p)
            self.handles.pop(h, None)

    def parent_handle(self, sid, path):
        root = self.storage(sid)[2]
        parent = os.path.dirname(path)
        return 0 if parent == root else self.handle_for(sid, parent)

    def dir_for_parent(self, sid, parent):
        """Directory for a ParentObject parameter: 0 or 0xFFFFFFFF = storage root."""
        st = self.storage(sid)
        if st is None:
            return None
        if parent in (0, 0xFFFFFFFF):
            return st[2]
        v = self.lookup(parent)
        if v is None or not os.path.isdir(v[1]):
            return None
        return v[1]

    def children(self, sid, d):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return []
        return [self.handle_for(sid, os.path.join(d, n)) for n in names]

    # --- datasets ----------------------------------------------------------
    def device_info(self):
        ops = sorted(self.ops)
        b = struct.pack("<HIH", 100, 6, 100) + mtp_str("microsoft.com: 1.0; android.com: 1.0;")
        b += struct.pack("<H", 0)
        b += struct.pack("<I", len(ops)) + b"".join(struct.pack("<H", o) for o in ops)
        ev = [0x4002, 0x4003, 0x4004, 0x4005, 0x4007, 0x400C]
        b += struct.pack("<I", len(ev)) + b"".join(struct.pack("<H", e) for e in ev)
        b += struct.pack("<I", 0)               # device properties
        b += struct.pack("<I", 0)               # capture formats
        b += struct.pack("<I", 2) + struct.pack("<HH", FMT_UNDEFINED, FMT_ASSOCIATION)
        b += mtp_str("PDFS test") + mtp_str(self.model) + mtp_str("1.0") + mtp_str("SIM0001")
        return b

    def storage_info(self, st):
        sv = os.statvfs(st[2])
        free = sv.f_bavail * sv.f_frsize
        cap = sv.f_blocks * sv.f_frsize
        return (struct.pack("<HHHQQI", 3, 2, 0, cap, free, 0xFFFFFFFF)
                + mtp_str(st[1]) + mtp_str(""))

    def obj_props(self, h):
        sid, path = self.handles[h]
        stt = os.stat(path)
        isdir = os.path.isdir(path)
        return dict(storage=sid, fmt=FMT_ASSOCIATION if isdir else FMT_UNDEFINED,
                    size=0 if isdir else stt.st_size, name=os.path.basename(path),
                    mtime=stt.st_mtime, parent=self.parent_handle(sid, path))

    def object_info(self, h):
        p = self.obj_props(h)
        size32 = min(p["size"], 0xFFFFFFFF)
        b = struct.pack("<IHHIHIIIIIIIHII", p["storage"], p["fmt"], 0, size32, 0, 0, 0, 0,
                        0, 0, 0, p["parent"], 1 if p["fmt"] == FMT_ASSOCIATION else 0, 0, 0)
        d = mtp_date(p["mtime"])
        return b + mtp_str(p["name"]) + mtp_str(d) + mtp_str(d) + mtp_str("")

    def prop_elements(self, h, code):
        p = self.obj_props(h)
        out = []
        def add(c, t, v):
            if code in (0xFFFFFFFF, c):
                out.append((c, t, v))
        add(P_STORAGE, T_UINT32, struct.pack("<I", p["storage"]))
        add(P_FORMAT, T_UINT16, struct.pack("<H", p["fmt"]))
        add(P_PROTECTION, T_UINT16, struct.pack("<H", 0))
        add(P_SIZE, T_UINT64, struct.pack("<Q", p["size"]))
        add(P_FILENAME, T_STR, mtp_str(p["name"]))
        add(P_MODIFIED, T_STR, mtp_str(mtp_date(p["mtime"])))
        add(P_PARENT, T_UINT32, struct.pack("<I", p["parent"]))
        return out

    # --- operations ----------------------------------------------------------
    def run(self, code, params, data_in):
        """Returns (response code, response params, data to send or None)."""
        a = self.args
        P = params + [0] * (5 - len(params))
        if code not in self.ops and code not in (0x1001, 0x1002, 0x1003):
            return NOT_SUPPORTED, [], None
        if code == OP["GetDeviceInfo"]:
            return OK, [], self.device_info()
        if code == OP["OpenSession"]:
            if self.session:
                return ALREADY_OPEN, [], None
            self.session = P[0]
            return OK, [], None
        if code == OP["CloseSession"]:
            self.session = 0
            return OK, [], None
        if not self.session:
            return NOT_OPEN, [], None
        if code == OP["GetStorageIDs"]:
            ids = [s[0] for s in self.storages]
            return OK, [], struct.pack("<I", len(ids)) + b"".join(struct.pack("<I", i) for i in ids)
        if code == OP["GetStorageInfo"]:
            st = self.storage(P[0])
            if st is None:
                return INVALID_STORAGE, [], None
            return OK, [], self.storage_info(st)
        if code in (OP["GetObjectHandles"], OP["GetNumObjects"]):
            sids = [s[0] for s in self.storages] if P[0] == 0xFFFFFFFF else [P[0]]
            hs = []
            for sid in sids:
                d = self.dir_for_parent(sid, P[2])
                if d is None:
                    return INVALID_PARENT, [], None
                hs += self.children(sid, d)
                if self.args.loop_handles and P[2] not in (0, 0xFFFFFFFF):
                    # the folder itself, and the folder above it, among its children
                    hs = [P[2]] + hs
                    up = os.path.dirname(d)
                    if up in self.paths:
                        hs.append(self.paths[up])
            if code == OP["GetNumObjects"]:
                return OK, [len(hs)], None
            return OK, [], struct.pack("<I", len(hs)) + b"".join(struct.pack("<I", h) for h in hs)
        if code == OP["GetObjectInfo"]:
            if self.lookup(P[0]) is None:
                return INVALID_HANDLE, [], None
            return OK, [], self.object_info(P[0])
        if code == OP["GetObjectPropList"]:
            h, fmt, pcode, group, depth = P
            if depth == 0:
                if self.lookup(h) is None:
                    return INVALID_HANDLE, [], None
                objs = [h]
            elif depth == 1:
                objs = []
                if h in (0, 0xFFFFFFFF):
                    for s in self.storages:
                        objs += self.children(s[0], s[2])
                else:
                    v = self.lookup(h)
                    if v is None or not os.path.isdir(v[1]):
                        return INVALID_HANDLE, [], None
                    objs = self.children(v[0], v[1])
                    if self.args.odd_proplist:
                        objs = [h] + objs[:2]
                    if self.args.loop_handles:
                        objs = [h] + objs
            else:
                return DEPTH_UNSUPPORTED, [], None
            els = []
            for o in objs:
                els += [(o,) + e for e in self.prop_elements(o, pcode)]
            b = struct.pack("<I", len(els))
            for (o, c, t, v) in els:
                b += struct.pack("<IHH", o, c, t) + v
            return OK, [], b
        if code == OP["GetObjectPropValue"]:
            if self.lookup(P[0]) is None:
                return INVALID_HANDLE, [], None
            els = self.prop_elements(P[0], P[1])
            if not els:
                return 0xA801, [], None
            return OK, [], els[0][2]
        if code in (OP["GetObject"], OP["GetPartialObject"], OP["GetPartialObject64"]):
            v = self.lookup(P[0])
            if v is None or os.path.isdir(v[1]):
                return INVALID_HANDLE, [], None
            with open(v[1], "rb") as f:
                if code == OP["GetObject"]:
                    return OK, [], f.read()
                if code == OP["GetPartialObject"]:
                    off, n = P[1], P[2]
                else:
                    off, n = P[1] | (P[2] << 32), P[3]
                f.seek(off)
                d = f.read(n)
            return OK, [len(d)], d
        if code == OP["DeleteObject"]:
            v = self.lookup(P[0])
            if v is None:
                return INVALID_HANDLE, [], None
            if os.path.isdir(v[1]):
                import shutil
                shutil.rmtree(v[1])
            else:
                os.unlink(v[1])
            self.forget(v[1])
            return OK, [], None
        if code == OP["SendObjectInfo"]:
            sid, parent = P[0], P[1]
            if sid == 0:
                sid = self.storages[0][0]
            d = self.dir_for_parent(sid, parent)
            if d is None:
                return INVALID_PARENT, [], None
            if data_in is None or len(data_in) < 52:
                return NO_VALID_INFO, [], None
            fmt = struct.unpack_from("<H", data_in, 4)[0]
            size = struct.unpack_from("<I", data_in, 8)[0]
            name, o = read_str(data_in, 52)
            created, o = read_str(data_in, o)
            modified, o = read_str(data_in, o)
            if not name or "/" in name or name in (".", ".."):
                return INVALID_PARAM, [], None
            path = os.path.join(d, name)
            if os.path.lexists(path):
                # Android refuses to create a second object with the same name
                return GENERAL, [], None
            if fmt == FMT_ASSOCIATION:
                os.mkdir(path)
                h = self.handle_for(sid, path)
                self.pending = None
                return OK, [sid, 0 if d == self.storage(sid)[2] else self.paths[d], h], None
            open(path, "wb").close()
            t = parse_date(modified)
            if t:
                os.utime(path, (t, t))
            h = self.handle_for(sid, path)
            self.pending = (h, size, t)
            return OK, [sid, 0 if d == self.storage(sid)[2] else self.paths[d], h], None
        if code == OP["SendObject"]:
            if self.pending is None:
                return NO_VALID_INFO, [], None
            h, size, t = self.pending
            self.pending = None
            sid, path = self.handles[h]
            with open(path, "wb") as f:
                f.write(data_in or b"")
            if t:
                os.utime(path, (t, t))
            return OK, [], None
        if code == OP["SetObjectPropValue"]:
            v = self.lookup(P[0])
            if v is None:
                return INVALID_HANDLE, [], None
            if P[1] != P_FILENAME:
                return ACCESS_DENIED, [], None
            name, _ = read_str(data_in, 0)
            if not name or "/" in name:
                return 0xA803, [], None
            new = os.path.join(os.path.dirname(v[1]), name)
            if os.path.lexists(new) and new != v[1]:
                return GENERAL, [], None
            os.rename(v[1], new)
            self.rehome(v[1], new, P[0])
            return OK, [], None
        if code == OP["MoveObject"]:
            v = self.lookup(P[0])
            if v is None:
                return INVALID_HANDLE, [], None
            sid = P[1] or v[0]
            d = self.dir_for_parent(sid, P[2])
            if d is None:
                return INVALID_PARENT, [], None
            new = os.path.join(d, os.path.basename(v[1]))
            if os.path.lexists(new):
                return GENERAL, [], None
            os.rename(v[1], new)
            self.rehome(v[1], new, P[0], sid)
            return OK, [], None
        if code == OP["BeginEditObject"] or code == OP["EndEditObject"]:
            if self.lookup(P[0]) is None:
                return INVALID_HANDLE, [], None
            return OK, [], None
        if code == OP["TruncateObject"]:
            v = self.lookup(P[0])
            if v is None:
                return INVALID_HANDLE, [], None
            os.truncate(v[1], P[1] | (P[2] << 32))
            return OK, [], None
        if code == OP["SendPartialObject"]:
            v = self.lookup(P[0])
            if v is None:
                return INVALID_HANDLE, [], None
            with open(v[1], "r+b") as f:
                f.seek(P[1] | (P[2] << 32))
                f.write(data_in or b"")
            return OK, [], None
        return NOT_SUPPORTED, [], None

    def rehome(self, old, new, h, sid=None):
        moved = [p for p in self.paths if p == old or p.startswith(old + os.sep)]
        for p in moved:
            hh = self.paths.pop(p)
            np = new + p[len(old):]
            self.paths[np] = hh
            self.handles[hh] = (sid or self.handles[hh][0], np)

    # --- the connection --------------------------------------------------------
    def recv_exact(self, s, n):
        b = b""
        while len(b) < n:
            c = s.recv(min(n - len(b), 65536))
            if not c:
                raise EOFError
            b += c
            if self.args.rate:              # a slow device: KB per second
                time.sleep(len(c) / (self.args.rate * 1024))
        return b

    def recv_container(self, s):
        hdr = self.recv_exact(s, 12)
        length, ctype, code, tid = struct.unpack("<IHHI", hdr)
        payload = self.recv_exact(s, length - 12) if length > 12 else b""
        return ctype, code, tid, payload

    def send_container(self, s, ctype, code, tid, payload):
        s.sendall(struct.pack("<IHHI", 12 + len(payload), ctype, code, tid) + payload)

    def serve(self, s):
        self.session = 0
        data_out_ops = (OP["SendObjectInfo"], OP["SendObject"], OP["SetObjectPropValue"],
                        OP["SendPartialObject"])
        while True:
            ctype, code, tid, payload = self.recv_container(s)
            if ctype != CMD:
                log("unexpected container type %d (code %04x)" % (ctype, code))
                continue
            params = list(struct.unpack("<%dI" % (len(payload) // 4), payload))
            data_in = None
            if code in data_out_ops:
                dtype, dcode, dtid, data_in = self.recv_container(s)
                if dtype != DATA:
                    log("expected data for %04x, got type %d" % (code, dtype))
                if self.args.zlp_strict and (12 + len(data_in)) % self.args.zlp_strict == 0:
                    # A real USB device would wait for a zero-length packet
                    # that DeviceFS can't send (Android: the next command
                    # is then taken as data and the transaction fails)
                    log("ZLP needed: %04x data phase of %d bytes" % (code, 12 + len(data_in)))
                    self.zlp_violations += 1
                    self.send_container(s, RESP, GENERAL, tid, b"")
                    continue
            if self.args.stall_op is not None and code == self.args.stall_op and not self.stalled:
                self.stalled = True
                log("stalling op %04x (tid %d): no response" % (code, tid))
                continue
            with self.lock:
                try:
                    rc, rparams, data = self.run(code, params, data_in)
                except Exception as e:
                    log("error in %04x: %r" % (code, e))
                    rc, rparams, data = GENERAL, [], None
            if self.args.delay_ms:
                time.sleep(self.args.delay_ms / 1000)
            if data is not None and rc == OK:
                self.send_container(s, DATA, code, tid, data)
            self.send_container(s, RESP, rc, tid,
                                b"".join(struct.pack("<I", p) for p in rparams))
            if self.args.verbose:
                log("op %04x %s -> %04x %s%s" % (code, [hex(p) for p in params], rc,
                    rparams, " +%d bytes" % len(data) if data is not None else ""))


def log(m):
    print("mtpsim: " + m, file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7015)
    ap.add_argument("--model", default="SimPhone")
    ap.add_argument("--no-proplist", action="store_true")
    ap.add_argument("--no-partial64", action="store_true")
    ap.add_argument("--no-edit", action="store_true")
    ap.add_argument("--no-rename", action="store_true")
    ap.add_argument("--odd-proplist", action="store_true")
    ap.add_argument("--loop-handles", action="store_true")
    ap.add_argument("--zlp-strict", type=int, default=0, metavar="N")
    ap.add_argument("--delay-ms", type=int, default=0)
    ap.add_argument("--rate", type=int, default=0, metavar="KB/S", help="receive this fast")
    ap.add_argument("--stall-op", type=lambda x: int(x, 0))
    ap.add_argument("--once", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("storage", nargs="+", help="NAME=DIR")
    args = ap.parse_args()
    dev = Device(args)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if args.rate:
        # a small window, so the sender is held up as a USB device's buffer
        # holds up DeviceFS (64KB), not by megabytes in the kernel
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    log("listening on 127.0.0.1:%d" % args.port)
    while True:
        c, addr = srv.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log("connection from %s:%d" % addr)
        try:
            dev.serve(c)
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        c.close()
        log("connection closed")
        if args.once:
            break


if __name__ == "__main__":
    main()
