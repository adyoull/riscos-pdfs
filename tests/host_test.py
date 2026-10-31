#!/usr/bin/env python3
"""Host tests for the PDFS core: run out/pdfs-host against sim/mtpsim.py with
different device behaviours and check the results in the simulator's folders.

    python3 tests/host_test.py [path/to/pdfs-host]

Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
"""
import struct
import os, random, re, shutil, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "out", "pdfs-host")
SIM = os.path.join(ROOT, "sim", "mtpsim.py")

passed = failed = 0
port = 7100


def ok(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what)


class Sim:
    def __init__(self, *flags):
        global port
        port += 1
        self.port = port
        self.dir = tempfile.mkdtemp(prefix="pdfs-")
        self.int = os.path.join(self.dir, "int")
        self.sd = os.path.join(self.dir, "sd")
        os.makedirs(os.path.join(self.int, "Download"))
        os.makedirs(self.sd)
        self.log = open(os.path.join(self.dir, "sim.log"), "w")
        self.p = subprocess.Popen([sys.executable, SIM, "--port", str(port), *flags,
                                   "Internal=" + self.int, "SD card=" + self.sd],
                                  stderr=self.log)
        for _ in range(50):
            time.sleep(0.05)
            if "listening" in open(self.log.name).read():
                break

    def run(self, *cmds, env=None, expect=0):
        args = [HOST, "127.0.0.1:%d" % self.port]
        for i, c in enumerate(cmds):
            if i:
                args.append("--")
            args += list(c)
        # RISC OS paths are in Latin-1: pass the bytes as they are
        args = [a.encode("latin-1") if isinstance(a, str) else a for a in args]
        e = dict(os.environ)
        if env:
            e.update(env)
        e["ASAN_OPTIONS"] = "detect_leaks=0"
        r = subprocess.run(args, capture_output=True, env=e, timeout=120)
        r.stdout = r.stdout.decode("latin-1")
        r.stderr = r.stderr.decode("latin-1")
        if expect is not None and r.returncode != expect:
            print("command:", b" ".join(args).decode("latin-1"))
            print(r.stdout, r.stderr)
        return r

    def stop(self):
        self.p.send_signal(signal.SIGTERM)
        self.p.wait()
        shutil.rmtree(self.dir)


def local(sim, *parts):
    return os.path.join(sim.int, *parts)


def tmpfile(data):
    fd, path = tempfile.mkstemp()
    os.write(fd, data)
    os.close(fd)
    return path


D = "SimPhone-Internal.$"


def suite(flags):
    print("== device:", " ".join(flags) or "(Android-like, everything supported)")
    s = Sim(*flags)
    try:
        # sizes around FileSwitch's 4K buffer and the 64K read-ahead
        for size in (0, 1, 4095, 4096, 4097, 65536, 65537, 300001):
            data = random.randbytes(size)
            src = tmpfile(data)
            r = s.run(["save", src, D + ".Download.f%d/bin" % size, "FFD"])
            ok(r.returncode == 0, "save %d" % size)
            p = local(s, "Download", "f%d.bin" % size)
            ok(os.path.exists(p) and open(p, "rb").read() == data, "saved content %d" % size)
            out = src + ".out"
            r = s.run(["get", D + ".Download.f%d/bin" % size, out])
            ok(r.returncode == 0 and open(out, "rb").read() == data, "read back %d" % size)
            os.unlink(src)
            os.unlink(out)

        # overwrite an existing file with *Save (shorter), then OPENOUT (longer)
        p = local(s, "Download", "over.txt")
        open(p, "wb").write(b"x" * 10000)
        src = tmpfile(b"short")
        r = s.run(["save", src, D + ".Download.over/txt", "FFF"])
        ok(r.returncode == 0 and open(p, "rb").read() == b"short", "overwrite with save")
        long = random.randbytes(20000)
        src2 = tmpfile(long)
        r = s.run(["openout", src2, D + ".Download.over/txt"])
        ok(r.returncode == 0 and open(p, "rb").read() == long, "overwrite with openout")
        # open for update and append at the end
        tail = b"--appended--"
        src3 = tmpfile(tail)
        r = s.run(["append", src3, D + ".Download.over/txt"])
        ok(r.returncode == 0 and open(p, "rb").read() == long + tail, "append (update)")

        # listing, types, info
        r = s.run(["ls", D + ".Download"])
        ok(re.search(r"F over +\d+  FFF", r.stdout) and "over/txt" not in r.stdout,
           "ls shows the name without its known extension, and the type")
        r = s.run(["info", D + ".Download.over/txt"])
        ok("type 1" in r.stdout and "length %d" % (len(long) + len(tail)) in r.stdout, "info")
        r = s.run(["info", D + ".Download.nothing"])
        ok("type 0" in r.stdout, "info on a missing file says not found")

        # directories
        r = s.run(["mkdir", D + ".Books"], ["mkdir", D + ".Books.SF"],
                  ["save", src, D + ".Books.SF.Astounding/epub", "FFD"])
        ok(r.returncode == 0 and os.path.isfile(local(s, "Books", "SF", "Astounding.epub")),
           "mkdir and save in a new directory")
        r = s.run(["rm", D + ".Books"], expect=1)
        ok("not empty" in r.stdout, "deleting a non-empty directory is refused")

        # rename in place, then move to another directory, then delete
        if "--no-rename" in flags:
            r = s.run(["mv", D + ".Books.SF.Astounding/epub", D + ".Books.SF.X/epub"], expect=1)
            ok("bad rename" in r.stdout, "no renaming: bad rename (FileSwitch copies instead)")
            shutil.move(local(s, "Books", "SF", "Astounding.epub"), local(s, "Download", "A.epub"))
        else:
            r = s.run(["mv", D + ".Books.SF.Astounding/epub", D + ".Books.SF.Astounding_1950/epub"])
            ok(r.returncode == 0 and os.path.isfile(local(s, "Books", "SF", "Astounding_1950.epub")),
               "rename")
            r = s.run(["mv", D + ".Books.SF.Astounding_1950/epub", D + ".Download.A/epub"])
            ok(r.returncode == 0 and os.path.isfile(local(s, "Download", "A.epub")), "move")
        r = s.run(["rm", D + ".Download.A/epub"], ["rm", D + ".Books.SF"], ["rm", D + ".Books"])
        ok(r.returncode == 0 and not os.path.exists(local(s, "Books"))
           and not os.path.exists(local(s, "Download", "A.epub")), "delete file and directories")

        # names: spaces, accents, characters RISC OS reserves
        os.makedirs(local(s, "Astounding SF"))
        open(local(s, "Astounding SF", "Café #1.txt"), "w").write("é")
        r = s.run(["ls", D + ".Astounding\xa0SF"])
        ok(r.returncode == 0 and "Caf\xe9\xa0_1 " in r.stdout,
           "names with spaces, accents and # are listed")
        src4 = tmpfile(b"new")
        r = s.run(["save", src4, D + ".Astounding\xa0SF.R\xe9sum\xe9/txt", "FFF"])
        ok(r.returncode == 0 and os.path.isfile(local(s, "Astounding SF", "Résumé.txt")),
           "saving a Latin-1 name makes a UTF-8 name on the device")

        # changes made on the device appear (directory cache expires)
        open(local(s, "Download", "added-on-phone.txt"), "w").write("hi")
        r = s.run(["ls", D + ".Download"], ["sleep", "5.2"], ["ls", D + ".Download"])
        ok(r.stdout.count("added-on-phone ") >= 1, "a file added on the device appears")

        # filetypes as extensions on the device
        rename_ok = "--no-rename" not in flags
        r = s.run(["save", src4, D + ".Letter", "FFF"], ["save", src4, D + ".Prog", "FFB"],
                  ["save", src4, D + ".Photo/jpg", "C85"], ["save", src4, D + ".Plain", "FFD"])
        ok(r.returncode == 0 and os.path.isfile(local(s, "Letter.txt")), "Text saved as Letter.txt")
        ok(os.path.isfile(local(s, "Prog,ffb")), "a type with no extension saved as ,xxx")
        ok(os.path.isfile(local(s, "Photo.jpg")) and os.path.isfile(local(s, "Plain")),
           "names that already have the extension, and Data, are kept")
        r = s.run(["ls", D], ["info", D + ".Prog"], ["info", D + ".Letter/txt"], ["info", D + ".Photo/jpg"])
        ok(re.search(r"F Letter +\d+  FFF", r.stdout) and re.search(r"F Prog +\d+  FFB", r.stdout)
           and re.search(r"F Photo +\d+  C85", r.stdout), "listed without suffixes, with their types")
        ok(r.stdout.count("type 1 load FFFFFB") == 1 and r.stdout.count("type 1 load FFFFFF") == 1
           and r.stdout.count("type 1 load FFFC85") == 1, "found by either name")
        r = s.run(["mv", D + ".Letter", D + ".Memo"], expect=0 if rename_ok else 1)
        ok(os.path.isfile(local(s, "Memo.txt")) == rename_ok, "a rename keeps the extension")
        r = s.run(["settype", D + ".Prog", "FFF"], ["ls", D])
        if rename_ok:
            ok(os.path.isfile(local(s, "Prog.txt")) and re.search(r"F Prog +\d+  FFF", r.stdout),
               "setting the type changes the extension")
        else:
            ok(re.search(r"F Prog +\d+  FFF", r.stdout), "setting the type without renaming: remembered")
        os.makedirs(local(s, "Clash"))
        for n in ("a.txt", "a.jpg", "b.txt", "b", "c.txt"):
            open(local(s, "Clash", n), "w").write(n)
        os.makedirs(local(s, "com.example.app"))
        r = s.run(["ls", D + ".Clash"], ["ls", D])
        ok(all(x in r.stdout for x in ("a/txt ", "a/jpg ", "b/txt ", "F b ", "F c "))
           and "com/example/app" in r.stdout, "names that would clash keep their extensions")
        r = s.run(["ls", D], env={"PDFS_SHOW_EXT": "1"})
        ok("Photo/jpg" in r.stdout and ("Prog/txt" in r.stdout if rename_ok else "Prog,ffb" in r.stdout),
           "*PDFSExtensions show lists whole names")

        # the second storage
        r = s.run(["save", src4, "SimPhone-SD_card.$.onsd/txt", "FFF"])
        ok(r.returncode == 0 and os.path.isfile(os.path.join(s.sd, "onsd.txt")), "second storage")
        r = s.run(["mv", "SimPhone-SD_card.$.onsd/txt", D + ".onsd/txt"], expect=1)
        ok("bad rename" in r.stdout, "rename across storages is refused (FileSwitch copies)")
    finally:
        s.stop()


def recovery():
    print("== recovery: the device ignores one GetObjectPropList (falls back)")
    s = Sim("--stall-op", "0x9805")
    try:
        open(local(s, "Download", "a.txt"), "w").write("a")
        r = s.run(["ls", D + ".Download"], ["ls", D + ".Download"], ["discs"],
                  env={"PDFS_TIMEOUT_CS": "150"}, expect=None)
        ok("lists folders: an object at a time" in r.stdout, "a proplist stall falls back to listing an object at a time")
        ok("F a " in r.stdout, "the listing still works")
        r = s.run(["discs"])
        ok(r.returncode == 0, "device still usable")
    finally:
        s.stop()
    print("== recovery: the device ignores one GetObjectHandles")
    s = Sim("--stall-op", "0x1007")
    try:
        open(local(s, "Download", "a.txt"), "w").write("a")
        r = s.run(["ls", D + ".Download"], ["ls", D + ".Download"],
                  env={"PDFS_TIMEOUT_CS": "150"}, expect=None)
        ok("stopped responding" in r.stdout, "a stall is reported as an error")
        ok("F a " in r.stdout, "after recovery the next listing works")
        r = s.run(["discs"])
        ok(r.returncode == 0, "device still usable")
    finally:
        s.stop()


def trace_and_free():
    print("== *PDFSLog trace: every request, and what arrived; the Free window after no answer")
    s = Sim("--android", "--stall-op", "0x1008")
    try:
        open(local(s, "Download", "a.txt"), "w").write("a")
        r = s.run(["ls", D + ".Download"],
                  env={"PDFS_TRACE": "", "PDFS_VERBOSE": "1", "PDFS_TIMEOUT_CS": "150"}, expect=None)
        out = r.stdout + r.stderr
        ok("mtp> &1007" in out and "mtp< data &1007" in out and "mtp< response &2001" in out,
           "the trace shows the requests and the containers that came back")
        ok("mtp: &1008 failed, error 1" in out and "nothing more arrived" in out,
           "and where an answer didn't come")
    finally:
        s.stop()
    s = Sim("--stall-op", "0x1005", "--stall-skip", "2")
    try:
        disc = D.split(".")[0]
        t = time.time()
        r = s.run(["freewin", disc, "4"], env={"PDFS_TIMEOUT_CS": "150"}, expect=None)
        lines = [l for l in r.stdout.splitlines() if l.startswith("freewin")]
        ok(len(lines) == 4 and all(" size 0 " not in l for l in lines),
           "the Free window shows the last known figures when the device doesn't answer")
        ok(time.time() - t < 8, "and doesn't wait for the device each time (%.1f s)" % (time.time() - t))
    finally:
        s.stop()


def open_like_openmtp():
    print("== opening: the session first (transaction 0), then the device info; Android left to settle")
    s = Sim("--android")
    try:
        t = time.time()
        r = s.run(["discs"], env={"PDFS_TRACE": "", "PDFS_VERBOSE": "1", "PDFS_SETTLE_CS": "150"})
        cmds = [l.split("mtp> ")[1] for l in r.stderr.splitlines() if "mtp> " in l]
        ok(cmds[:2] and cmds[0].startswith("&1002 t0 (") and cmds[1].startswith("&1001 t1"),
           "OpenSession (t0) comes first, then GetDeviceInfo (t1): %s" % cmds[:2])
        sid = int(cmds[0].split("(")[1].rstrip(")"), 16)
        ok(sid & 1 and sid != 0xFFFFFFFF, "the session id is odd (%08X)" % sid)
        ok("leaving it 1.5 s to settle" in r.stderr and time.time() - t >= 1.5
           and "SimPhone-Internal" in r.stdout, "an Android device is left 1.5 s, then mounted")
    finally:
        s.stop()


def spec_details():
    print("== events read one at a time (several waiting, of different lengths)")
    s = Sim()
    try:
        def ev(code, *params):
            b = struct.pack("<IHHI", 12 + 4 * len(params), 4, code, 0) + b"".join(
                struct.pack("<I", p) for p in params)
            return b.hex()
        # ObjectAdded (16), StorageInfoChanged (16), DevicePropChanged (12),
        # ObjectInfoChanged (16), a 3-parameter one (24), StoreAdded (16)
        fake = ev(0x4002, 5) + ev(0x400C, 0x10001) + ev(0x4006) + ev(0x4007, 9) \
            + ev(0x4008, 1, 2, 3) + ev(0x4004, 0x20001)
        r = s.run(["discs"], ["tick"], env={"PDFS_FAKE_EVENTS": fake, "PDFS_VERBOSE": "1"})
        got = [l.split("&")[1] for l in r.stderr.splitlines() if l.startswith("pdfs: event")]
        ok([int(g, 16) for g in got] == [0x4002, 0x400C, 0x4006, 0x4007, 0x4008, 0x4004],
           "six events, each read whole and in order: %s" % got)
        r = s.run(["discs"], ["tick"], ["tick"], env={"PDFS_FAKE_EVENTS": "0000" + ev(0x4002, 5),
                                                     "PDFS_VERBOSE": "1"})
        ok("out of step" in r.stderr, "an event channel out of step is dropped, not read wrongly")
    finally:
        s.stop()
    print("== transaction IDs wrap past &FFFFFFFF to 1")
    s = Sim()
    try:
        r = s.run(["ls", D], ["ls", D + ".Download"],
                  env={"PDFS_TRACE": "", "PDFS_VERBOSE": "1", "PDFS_TID_START": "FFFFFFFD"})
        tids = [l.split(" t")[1].split()[0] for l in r.stderr.splitlines() if "mtp> " in l]
        i = tids.index("4294967294") if "4294967294" in tids else -1
        ok(i >= 0 and tids[i + 1] == "1" and "4294967295" not in tids,
           "&FFFFFFFE, then 1 (never &FFFFFFFF): %s" % tids[max(i, 0):i + 3])
        ok(r.returncode == 0, "and the device carries on")
    finally:
        s.stop()
    print("== dates with a time zone (and tenths) are read as UTC")
    infos = []
    for zone in (None, "+0130", "-0800", "Z"):
        s = Sim(*(["--date-zone", zone] if zone else []))
        try:
            p = local(s, "Download", "z.txt")
            open(p, "w").write("z")
            os.utime(p, (1700000000, 1700000000))
            r = s.run(["info", D + ".Download.z/txt"])
            infos.append(r.stdout.strip())
        finally:
            s.stop()
    ok(len(set(infos)) == 1 and "load" in infos[0], "the same time whatever the zone: %s" % infos)


def android_gap():
    print("== an Android device: a gap between an answer and the next request")
    for gap, want in ((None, 1), ("3", 3), ("0", 0)):
        s = Sim("--android")
        try:
            for i in range(6):
                open(local(s, "Download", "f%d.txt" % i), "w").write("x")
            env = {"PDFS_TRACE": "", "PDFS_VERBOSE": "1"}
            if gap is not None:
                env["PDFS_GAP_CS"] = gap
            r = s.run(["ls", D + ".Download"], ["discs"], env=env)
            ts = [int(l.split(" mtp>")[0]) for l in r.stderr.splitlines() if " mtp> &1008" in l]
            steps = [b - a for a, b in zip(ts, ts[1:])]
            line = ("leaves %d cs between" % want) in r.stdout
            if want:
                ok(len(ts) >= 6 and min(steps) >= want and line,
                   "gap %s: requests %d cs or more apart (%s), and *PDFSDevices says so" % (gap, want, steps))
            else:
                ok(len(ts) >= 6 and "cs between" not in r.stdout, "gap 0: none (%s)" % steps)
        finally:
            s.stop()


def zlp():
    print("== USB without zero-length packets (data phases of 512n bytes)")
    for flags in ([], ["--no-edit"]):
        s = Sim("--zlp-strict", "512", *flags)
        env = {"PDFS_NO_ZLP": "512"}
        try:
            for size in (500, 1012, 499, 1):
                data = random.randbytes(size)
                src = tmpfile(data)
                r = s.run(["save", src, D + ".Download.z%d/bin" % size, "FFD"], env=env,
                          expect=None)
                p = local(s, "Download", "z%d.bin" % size)
                if size in (500, 1012) and flags:
                    ok(r.returncode != 0, "no edit operations: a 512n data phase still fails (%d)" % size)
                else:
                    ok(r.returncode == 0 and open(p, "rb").read() == data,
                       "file of %d bytes sent without a zero-length packet %s" % (size, flags))
                os.unlink(src)
            # an ObjectInfo dataset of 500 bytes: a name of 189 characters
            name = "N" * 185 + "/bin"
            src = tmpfile(b"x")
            r = s.run(["save", src, D + ".Download." + name, "FFD"], env=env, expect=None)
            ok(r.returncode == 0 and os.path.isfile(local(s, "Download", "N" * 185 + ".bin")),
               "an ObjectInfo of 500 bytes is padded %s" % flags)
            if not flags:
                # in-place edit (Android) of a 500-byte file
                data = random.randbytes(500)
                src = tmpfile(data)
                r = s.run(["openout", src, D + ".Download.z1/bin"], env=env)
                ok(r.returncode == 0 and open(local(s, "Download", "z1.bin"), "rb").read() == data,
                   "an edit in place of 500 bytes goes in two parts")
        finally:
            s.stop()


def big():
    print("== big files (over 4MB go to a scrap file while being written)")
    for flags in ([], ["--no-partial64", "--no-edit"], ["--zlp-strict", "512"]):
        s = Sim(*flags)
        scrap = tempfile.mkdtemp(prefix="pdfs-scrap-")
        env = {"PDFS_SPILL_DIR": scrap}
        if "--zlp-strict" in flags:
            env["PDFS_NO_ZLP"] = "512"
        try:
            sizes = (5 * 1048576 - 12, 40 * 1048576 + 7) if "--zlp-strict" in flags else (5 * 1048576, 40 * 1048576 + 7)
            for size in sizes:
                data = random.randbytes(size)
                src = tmpfile(data)
                r = s.run(["save", src, D + ".Download.big%d/bin" % size, "FFD"], env=env)
                p = local(s, "Download", "big%d.bin" % size)
                ok(r.returncode == 0 and open(p, "rb").read() == data,
                   "a %d-byte file written %s" % (size, flags))
                os.unlink(src)
            # copied as Filer_Action does: streamed to the device as written
            for size in (sizes[0], sizes[1] + 4093):
                data = random.randbytes(size)
                src = tmpfile(data)
                # sent as it's written; kept too (a scrap file here), as
                # FileSwitch can write a block after ones beyond it
                fenv = dict(env)
                r = s.run(["facopy", src, D + ".Download.fa%d" % size, "C85"], env=fenv)
                p = local(s, "Download", "fa%d.jpg" % size)
                ok(r.returncode == 0 and os.path.isfile(p) and open(p, "rb").read() == data,
                   "a %d-byte file copied as Filer_Action does %s" % (size, flags))
                os.unlink(src)
            if "--zlp-strict" in flags:
                # one that would need a zero-length packet, copied as Filer_Action
                # does, with no scrap directory: streamed, its last byte by an edit
                size = 512 * 10241 - 12          # over 4MB
                data = random.randbytes(size)
                src = tmpfile(data)
                r = s.run(["facopy", src, D + ".Download.zlpfa", "FFD"], env=env)
                p = local(s, "Download", "zlpfa")
                ok(r.returncode == 0 and os.path.isfile(p) and open(p, "rb").read() == data,
                   "a %d-byte file streamed, its last byte by an edit %s" % (size, flags))
                os.unlink(src)
            # written out of order: refused, and nothing left on the device
            data = random.randbytes(6 * 1048576)
            src = tmpfile(data)
            r = s.run(["facopy", src, D + ".Download.swapped", "FFD", "1048576"],
                      env=dict(env, PDFS_FA_SWAP="1"), expect=None)
            sp = local(s, "Download", "swapped")
            ok(("wasn't written in order" in r.stdout and not os.path.exists(sp))
               or (r.returncode == 0 and os.path.exists(sp) and open(sp, "rb").read() == data),
               "a big file written out of order is sent right (or refused) %s" % flags)
            os.unlink(src)
            # append to a big file: it's loaded into a scrap file first
            tail = b"--tail--"
            src = tmpfile(tail)
            p = local(s, "Download", "big%d.bin" % sizes[1])
            before = open(p, "rb").read()
            r = s.run(["append", src, D + ".Download.big%d/bin" % sizes[1]], env=env)
            ok(r.returncode == 0 and open(p, "rb").read() == before + tail,
               "appending to a 40MB file %s" % flags)
            ok(os.listdir(scrap) == [], "scrap files deleted %s" % flags)
        finally:
            s.stop()
            shutil.rmtree(scrap, ignore_errors=True)


def background():
    print("== big new files sent in the background (the filer task running)")
    for flags in ([], ["--no-partial64", "--no-edit"], ["--zlp-strict", "512"]):
        s = Sim(*flags)
        scrap = tempfile.mkdtemp(prefix="pdfs-scrap-")
        env = {"PDFS_SPILL_DIR": scrap, "PDFS_BG": "1"}
        if "--zlp-strict" in flags:
            env["PDFS_NO_ZLP"] = "512"
        try:
            # one OS_File Save's worth (open, one write, close): the close
            # returns before anything is sent; then it goes a bit at a time
            for size in (3 * 1048576 + 5, 512 * 12000 - 12, 9 * 1048576 + 1):
                data = random.randbytes(size)
                src = tmpfile(data)
                name = "bg%d" % size
                r = s.run(["save", src, D + ".Download." + name + "/bin", "FFD"],
                          ["bgprogress", "SimPhone-Internal"],
                          ["bgstep"], ["bgdone"], ["bgprogress", "SimPhone-Internal"], env=env)
                p = local(s, "Download", name + ".bin")
                ok(r.returncode == 0 and "bg: 0%\n" in r.stdout and "bg: -1%" in r.stdout
                   and open(p, "rb").read() == data,
                   "a %d-byte file sent in the background %s" % (size, flags))
                os.unlink(src)
            # sent while other things are asked for: a listing gives what's
            # known; reading it back waits for it to finish
            data = random.randbytes(6 * 1048576 + 3)
            src, back = tmpfile(data), tmpfile(b"")
            r = s.run(["facopy", src, D + ".Download.fa", "C85"], ["bgstep"],
                      ["ls", D + ".Download"], ["get", D + ".Download.fa", back], env=env)
            ok(r.returncode == 0 and open(back, "rb").read() == data
               and open(local(s, "Download", "fa.jpg"), "rb").read() == data,
               "read back while being sent: waits for it %s" % flags)
            # another file on the device while one is being sent
            r = s.run(["save", src, D + ".Download.one", "FFD"], ["bgstep"],
                      ["mkdir", D + ".Download.newdir"], ["save", src, D + ".Download.newdir.two", "FFD"],
                      ["rm", D + ".Download.one"], env=env)
            ok(r.returncode == 0 and not os.path.exists(local(s, "Download", "one"))
               and open(local(s, "Download", "newdir", "two"), "rb").read() == data,
               "other operations while sending %s" % flags)
            # folders made, and small files saved, while a big one is waiting:
            # they go in their turn, in order
            small = tmpfile(b"small file")
            r = s.run(["save", src, D + ".Download.b1", "FFD"], ["mkdir", D + ".Download.nd"],
                      ["mkdir", D + ".Download.nd.sub"], ["save", small, D + ".Download.nd.sub.s", "FFD"],
                      ["ls", D + ".Download.nd"], ["bgprogress", "SimPhone-Internal"], ["bgdone"],
                      ["ls", D + ".Download.nd.sub"], env=env)
            ok(r.returncode == 0 and "bg: 0%" in r.stdout
               and open(local(s, "Download", "b1"), "rb").read() == data
               and open(local(s, "Download", "nd", "sub", "s"), "rb").read() == b"small file",
               "folders and small files queued behind a big one %s" % flags)
            os.unlink(small)
            # the window's figures, and Abort: the file being sent is cancelled
            # and deleted on the device, the ones waiting are dropped; the
            # device works on afterwards
            r = s.run(["save", src, D + ".Download.ab1", "FFD"], ["save", src, D + ".Download.ab2", "FFD"],
                      ["bgstatus"], ["bgstep"], ["bgstatus"], ["bgabort"], ["bgstatus"],
                      ["save", small2 := tmpfile(b"after"), D + ".Download.after", "FFD"],
                      ["ls", D + ".Download"], env=env)
            ok(r.returncode == 0 and "bg: active 1 left %d batch %d done 0 files 0 path PDFS::SimPhone-Internal.$.Download.ab1"
               % (2 * len(data), 2 * len(data)) in r.stdout and "bg: active 0" in r.stdout
               and not os.path.exists(local(s, "Download", "ab1")) and not os.path.exists(local(s, "Download", "ab2"))
               and open(local(s, "Download", "after"), "rb").read() == b"after",
               "Abort cancels the sends, and the device works on %s" % flags)
            os.unlink(small2)
            # many queued at once
            r = s.run(*[["save", src, D + ".Download.q%d" % k, "FFD"] for k in range(6)], env=env)
            ok(r.returncode == 0 and all(open(local(s, "Download", "q%d" % k), "rb").read() == data
                                         for k in range(6)), "six files queued %s" % flags)
            os.unlink(src)
            os.unlink(back)
            ok(os.listdir(scrap) == [], "scrap files deleted %s" % flags)
        finally:
            s.stop()
            shutil.rmtree(scrap, ignore_errors=True)


def odd():
    print("== a folder's property list that lists the folder itself, and not all its children")
    s = Sim("--odd-proplist")
    try:
        cam = local(s, "DCIM", "Camera")
        os.makedirs(cam)
        names = ["IMG_%04d.jpg" % k for k in range(12)]
        for n in names:
            open(os.path.join(cam, n), "wb").write(n.encode())
        r = s.run(["ls", D + ".DCIM"], ["ls", D + ".DCIM.Camera"],
                  ["get", D + ".DCIM.Camera.IMG_0011/jpg", tmp := tmpfile(b"")])
        dcim = r.stdout.split("Camera", 1)[0]
        ok(r.returncode == 0 and "DCIM" not in re.sub(r"SimPhone-Internal.\$\.DCIM", "", dcim)
           and all(n[:-4] in r.stdout for n in names) and open(tmp, "rb").read() == b"IMG_0011.jpg",
           "DCIM lists only Camera, and Camera lists all 12 photos")
        os.unlink(tmp)
    finally:
        s.stop()


def loops():
    print("== a device that lists a folder, or the folder above it, among its children")
    s = Sim("--loop-handles")
    try:
        d = local(s, "Files by Google", "Inner")
        os.makedirs(d)
        open(os.path.join(d, "x.txt"), "w").write("x")
        nb = "Files\xa0by\xa0Google"
        r = s.run(["ls", D + "." + nb], ["ls", D + "." + nb + ".Inner"])
        parts = r.stdout.split("entries")
        ok(r.returncode == 0 and len(parts) >= 3
           and "Files" not in parts[0] and "Inner" in parts[0]
           and "Inner" not in parts[1] and "Files" not in parts[1] and " x " in parts[1],
           "a folder never shows itself or the folder above it")
    finally:
        s.stop()


def stats():
    print("== *PDFSDevices: how files are read, and the transfer figures")
    for flags, want in (([], "reads files: in parts"), (["--no-partial"], "reads files: whole")):
        s = Sim(*flags)
        try:
            data = bytes(random.randrange(256) for _ in range(3 * 65536 + 5))
            open(local(s, "Download", "v.txt"), "wb").write(data)
            out = os.path.join(s.dir, "v.out")
            r = s.run(["get", D + ".Download.v", out], ["discs"])
            ok(r.returncode == 0 and open(out, "rb").read() == data, "%s: read back" % (flags or "parts"))
            ok(want in r.stdout and "received 0.1 MB" in r.stdout and "longest wait" in r.stdout,
               "%s: *PDFSDevices says how it reads, and what it received" % (flags or "parts"))
        finally:
            s.stop()


def background_read():
    print("== a device that can't send part of a file: received in the background")
    s = Sim("--no-partial", "--send-rate", "20000")
    try:
        data = bytes(random.randrange(256) for _ in range(3 * 1048576 + 333))
        open(local(s, "Download", "w.txt"), "wb").write(data)
        open(local(s, "Download", "small.txt"), "wb").write(b"small")
        out = os.path.join(s.dir, "w.out")
        out2 = os.path.join(s.dir, "w2.out")
        r = s.run(["getbg", D + ".Download.w", out],
                  ["getbg", D + ".Download.w", out2, D + ".Download"],
                  ["get", D + ".Download.small", os.path.join(s.dir, "s.out")], ["discs"])
        ok(r.returncode == 0 and open(out, "rb").read() == data, "read whole, with the background between reads")
        m = re.findall(r"background steps with more to do: (\d+)", r.stdout)
        ok(m and int(m[0]) > 0, "the background received some of it (%s)" % m)
        ok(open(out2, "rb").read() == data, "another request half way: the rest is taken first, nothing lost")
        ok(open(os.path.join(s.dir, "s.out"), "rb").read() == b"small", "and the next file after it")
        ok("received " in r.stdout and "reads files: whole" in r.stdout, "*PDFSDevices")
    finally:
        s.stop()


def slow_proplist():
    print("== GetObjectPropList answered late (8 s): folders listed an object at a time instead")
    s = Sim("--proplist-hang", "8")
    try:
        open(local(s, "Download", "a.txt"), "w").write("a")
        t = time.time()
        r = s.run(["ls", D + ".Download"], ["ls", D + ".Download"], ["discs"], env={"PDFS_TRACE": ""})
        ok(r.returncode == 0 and r.stdout.count(" a ") >= 1, "the folder is listed")
        ok("lists folders: an object at a time" in r.stdout, "and from then on an object at a time")
        ok(time.time() - t < 20, "without waiting for the late answer each time (%.1f s)" % (time.time() - t))
    finally:
        s.stop()
    print("== an Android device: listed an object at a time")
    s = Sim("--android", "--proplist-hang", "30")
    try:
        open(local(s, "Download", "a.txt"), "w").write("a")
        t = time.time()
        r = s.run(["ls", D + ".Download"], ["discs"])
        ok(r.returncode == 0 and " a " in r.stdout and "an object at a time (Android)" in r.stdout
           and time.time() - t < 5, "no GetObjectPropList asked (%.1f s)" % (time.time() - t))
    finally:
        s.stop()


def allow():
    for mode in ("empty", "denied", "busy"):
        print("== a device that asks to allow access to its data (%s until allowed)" % mode)
        s = Sim("--needs-allow", mode, "--allow-after", "2.5")
        try:
            open(local(s, "Download", "a.txt"), "w").write("a")
            r = s.run(["discs"], ["waitdiscs", "10"], ["discs"], ["ls", D + ".Download"])
            log = open(s.log.name).read()
            ok(r.returncode == 0, "%s: mounted while waiting" % mode)
            ok("waiting: allow access" in r.stdout.split("discs:")[0], "%s: said to be waiting" % mode)
            ok("discs: 2" in r.stdout and " a " in r.stdout.split("discs:")[1],
               "%s: the storages appear once allowed, and can be read" % mode)
            ok(log.count("PROMPT") == 1 and "ALLOWED" in log and "gone before" not in log,
               "%s: asked once, and the Allow reached the session" % mode)
        finally:
            s.stop()


def main():
    random.seed(1)
    if not os.path.exists(HOST):
        sys.exit("build out/pdfs-host first")
    suite([])
    suite(["--no-proplist"])
    suite(["--no-partial64", "--no-edit"])
    suite(["--no-proplist", "--no-partial64", "--no-edit", "--delay-ms", "2"])
    suite(["--no-rename"])
    suite(["--odd-proplist"])
    suite(["--no-partial", "--no-edit"])   # whole objects only (GetObject), read a piece at a time
    suite(["--android"])                   # Android: no GetObjectPropList, an object at a time
    odd()
    loops()
    allow()
    slow_proplist()
    trace_and_free()
    open_like_openmtp()
    spec_details()
    android_gap()
    stats()
    background_read()
    recovery()
    zlp()
    big()
    background()
    print("%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
