#!/usr/bin/env python3
"""pub/ is read-only, and nothing a client can send changes that.

Two independent gates make it so: pub's zone has neither CAP_CREATE nor
CAP_MODIFY nor CAP_REMOVE, so every mutating command is refused before any
syscall; and a descriptor opened in pub carries CAP_READ only, so a WRITE
against it is refused without ever reaching the file.

The protocol checks below would pass against a server that refused politely
and then wrote anyway, so the test also hashes the whole served tree before
and after and requires it to be byte-for-byte identical.

Usage: test_readonly.py [path/to/de-tnfsd]
"""

import hashlib
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tnfslib as t


def tree_digest(path):
    h = hashlib.sha256()
    for dirpath, dirnames, filenames in sorted(os.walk(path)):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            h.update(os.path.relpath(full, path).encode())
            h.update(b"\0")
            if os.path.islink(full):
                h.update(os.readlink(full).encode())
            else:
                with open(full, "rb") as f:
                    h.update(f.read())
                h.update(("%o" % (os.stat(full).st_mode & 0o777)).encode())
    return h.hexdigest()


def build_tree(base):
    root = os.path.join(base, "root")
    pub = os.path.join(root, "pub")
    os.makedirs(os.path.join(pub, "sub"))
    os.makedirs(os.path.join(root, "incoming"))
    with open(os.path.join(pub, "readme.txt"), "w") as f:
        f.write("hello from pub\n")
    with open(os.path.join(pub, "sub", "data.bin"), "wb") as f:
        f.write(bytes(range(256)) * 8)
    return root


def main(transport="udp"):
    binary = t.binary_from_argv()
    base = tempfile.mkdtemp(prefix="de-tnfsd-ro-")
    r = t.Results("read-only pub [%s]" % transport)
    daemon = None
    try:
        root = build_tree(base)
        pub = os.path.join(root, "pub")
        before = tree_digest(pub)

        daemon = t.Daemon(binary, root, transport=transport)
        c = daemon.client("/")

        # Reading works, or the refusals below would prove nothing.
        names = sorted(c.listdir("/pub"))
        r.check(names == ["readme.txt", "sub"], "pub lists its entries",
                "(got %r)" % names)
        status, blob = c.read_all("/pub/readme.txt")
        r.check(status == t.OK and blob == b"hello from pub\n",
                "pub file reads back byte for byte")
        status, blob = c.read_all("/pub/sub/data.bin")
        r.check(status == t.OK and len(blob) == 2048,
                "a multi-datagram read returns the whole file",
                "(got %d bytes)" % len(blob))
        status, st = c.stat("/pub/readme.txt")
        r.check(status == t.OK and st["size"] == 15, "STAT reports the size")
        r.check(status == t.OK and (st["mode"] & 0o222) == 0,
                "STAT reports no write bits for a pub file")

        # Every way of asking to open for writing.
        write_flags = [
            ("O_WRONLY", t.O_WRONLY),
            ("O_RDWR", t.O_RDWR),
            ("O_WRONLY|O_CREAT", t.O_WRONLY | t.O_CREAT),
            ("O_RDONLY|O_CREAT", t.O_RDONLY | t.O_CREAT),
            ("O_RDONLY|O_TRUNC", t.O_RDONLY | t.O_TRUNC),
            ("O_RDONLY|O_APPEND", t.O_RDONLY | t.O_APPEND),
            ("O_RDONLY|O_EXCL", t.O_RDONLY | t.O_EXCL),
        ]
        for label, flags in write_flags:
            status, _ = c.open("/pub/readme.txt", flags)
            r.check_status(status, t.EROFS, "OPEN existing %s" % label)
            status, _ = c.open("/pub/newfile.txt", flags)
            r.check_status(status, t.EROFS, "OPEN new %s" % label)

        # Handle 0 is never handed out. A client that reads a file handle of
        # 0 as "nothing open" skips CLOSE for it, and in the drop box a
        # skipped CLOSE means an upload that never finalizes (DESIGN.md 4).
        cf = daemon.client("/")
        status, handle = cf.open("/pub/readme.txt", t.O_RDONLY)
        r.check(status == t.OK and handle != 0,
                "the first file handle of a session is never 0",
                "(got %s handle %r)" % (t.sname(status), handle))
        if status == t.OK:
            cf.close_file(handle)
        cf.close()

        # A read descriptor cannot be turned into a write descriptor.
        status, handle = c.open("/pub/readme.txt", t.O_RDONLY)
        r.check_status(status, t.OK, "OPEN read-only succeeds")
        status, _ = c.write(handle, b"clobber")
        r.check_status(status, t.EROFS, "WRITE on a pub descriptor")
        c.close_file(handle)

        # Every mutating command, on files that exist and files that do not.
        for path in ("/pub/readme.txt", "/pub/nosuch.txt", "/pub/sub"):
            r.check_status(c.unlink(path), t.EROFS, "UNLINK %s" % path)
            r.check_status(c.chmod(path), t.EROFS, "CHMOD %s" % path)
        r.check_status(c.mkdir("/pub/newdir"), t.EROFS, "MKDIR in pub")
        # The root serves exactly two names, so a third one does not exist to
        # be refused on any other grounds (DESIGN.md 1).
        r.check_status(c.mkdir("/newdir"), t.ENOENT, "MKDIR of a new root name")
        r.check_status(c.mkdir("/"), t.EROFS, "MKDIR of the root itself")
        r.check_status(c.rmdir("/incoming"), t.EACCES, "RMDIR of the drop box")
        r.check_status(c.rmdir("/pub/sub"), t.EROFS, "RMDIR in pub")
        r.check_status(c.rename("/pub/readme.txt", "/pub/other.txt"), t.EROFS,
                       "RENAME within pub")
        r.check_status(c.rename("/pub/readme.txt", "/pub/sub/moved.txt"), t.EROFS,
                       "RENAME into a pub subdirectory")

        c.close()

        # A session mounted directly on /pub gets exactly the same treatment.
        cp = daemon.client("/pub")
        status, _ = cp.open("readme.txt", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.EROFS, "OPEN for write on a /pub mount")
        r.check_status(cp.unlink("readme.txt"), t.EROFS, "UNLINK on a /pub mount")
        cp.close()

        # --no-incoming is the same server with the drop box gone.
        daemon.stop()
        daemon = t.Daemon(binary, root, ["--no-incoming"], transport=transport)
        c = daemon.client("/")
        names = sorted(c.listdir("/"))
        r.check(names == ["pub"], "--no-incoming: root lists pub only",
                "(got %r)" % names)
        status, _ = c.open("/incoming/x.bin", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.ENOENT, "--no-incoming: no drop box to write to")
        status, blob = c.read_all("/pub/readme.txt")
        r.check(status == t.OK and blob == b"hello from pub\n",
                "--no-incoming: pub still reads")
        c.close()

        after = tree_digest(pub)
        r.check(before == after, "pub is byte-for-byte unchanged on disk")
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)
    return r.finish()


if __name__ == "__main__":
    # Every check runs over both transports: the handlers are shared, but the
    # framing, session teardown and peer lookup are not.
    sys.exit(max(main("udp"), main("tcp")))
