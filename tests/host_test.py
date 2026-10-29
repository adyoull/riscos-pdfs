#!/usr/bin/env python3
"""Host tests for the PDFS core: run out/pdfs-host against sim/mtpsim.py with
different device behaviours and check the results in the simulator's folders.

    python3 tests/host_test.py [path/to/pdfs-host]

Copyright 2026 Andrew Youll. Licensed under the Apache License, Version 2.0.
"""
import os, random, shutil, signal, subprocess, sys, tempfile, time

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
        ok("over/txt" in r.stdout and "FFF" in r.stdout, "ls shows translated name + type")
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
        ok(r.returncode == 0 and "Caf\xe9\xa0_1/txt" in r.stdout,
           "names with spaces, accents and # are listed")
        src4 = tmpfile(b"new")
        r = s.run(["save", src4, D + ".Astounding\xa0SF.R\xe9sum\xe9/txt", "FFF"])
        ok(r.returncode == 0 and os.path.isfile(local(s, "Astounding SF", "Résumé.txt")),
           "saving a Latin-1 name makes a UTF-8 name on the device")

        # changes made on the device appear (directory cache expires)
        open(local(s, "Download", "added-on-phone.txt"), "w").write("hi")
        r = s.run(["ls", D + ".Download"], ["sleep", "5.2"], ["ls", D + ".Download"])
        ok(r.stdout.count("added-on-phone/txt") >= 1, "a file added on the device appears")

        # the second storage
        r = s.run(["save", src4, "SimPhone-SD_card.$.onsd/txt", "FFF"])
        ok(r.returncode == 0 and os.path.isfile(os.path.join(s.sd, "onsd.txt")), "second storage")
        r = s.run(["mv", "SimPhone-SD_card.$.onsd/txt", D + ".onsd/txt"], expect=1)
        ok("bad rename" in r.stdout, "rename across storages is refused (FileSwitch copies)")
    finally:
        s.stop()


def recovery():
    print("== recovery: the device ignores one GetObjectPropList")
    s = Sim("--stall-op", "0x9805")
    try:
        open(local(s, "Download", "a.txt"), "w").write("a")
        r = s.run(["ls", D + ".Download"], ["ls", D + ".Download"],
                  env={"PDFS_TIMEOUT_CS": "150"}, expect=None)
        ok("stopped responding" in r.stdout, "a stall is reported as an error")
        ok("a/txt" in r.stdout, "after recovery the next listing works")
        r = s.run(["discs"])
        ok(r.returncode == 0, "device still usable")
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
    recovery()
    print("%d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
