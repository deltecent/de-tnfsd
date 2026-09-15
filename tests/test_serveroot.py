#!/usr/bin/env python3
"""--serve-root and --serve-root-rw collapse the namespace to one zone.

The default server keeps two disjoint zones (pub/ reads, incoming/ writes); the
two serve-root modes are an opt-in relaxation for the "quick server in a repo"
case. --serve-root serves <root> itself read-only with no drop box;
--serve-root-rw additionally allows creating and *overwriting* files -- the one
thing the drop box must never do -- while the directory-shape mutations
(MKDIR/RMDIR/UNLINK/RENAME/CHMOD) stay refused everywhere.

Both modes serve <root> whatever is in it: a pub/ or incoming/ that happens to
exist is just an ordinary entry, not a zone.

Usage: test_serveroot.py [path/to/de-tnfsd]
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
            with open(full, "rb") as f:
                h.update(f.read())
    return h.hexdigest()


def build_tree(base):
    """A root that is not laid out for the split server: loose files at the top,
    a subdirectory, and directories literally named pub/ and incoming/ to prove
    they are served as ordinary entries rather than treated as zones."""
    root = os.path.join(base, "repo")
    os.makedirs(os.path.join(root, "src"))
    os.makedirs(os.path.join(root, "pub"))        # ordinary here, not a zone
    os.makedirs(os.path.join(root, "incoming"))   # ordinary here, not a zone
    with open(os.path.join(root, "README.md"), "w") as f:
        f.write("top level file\n")
    with open(os.path.join(root, "src", "main.c"), "wb") as f:
        f.write(bytes(range(256)) * 8)
    with open(os.path.join(root, "pub", "note.txt"), "w") as f:
        f.write("just a file in a dir called pub\n")
    return root


def check_readonly(r, binary, transport):
    base = tempfile.mkdtemp(prefix="de-tnfsd-sr-ro-")
    daemon = None
    try:
        root = build_tree(base)
        before = tree_digest(root)

        daemon = t.Daemon(binary, root, ["--serve-root"], transport=transport)
        c = daemon.client("/")

        names = sorted(c.listdir("/"))
        r.check(names == ["README.md", "incoming", "pub", "src"],
                "serve-root: root lists the real tree, not pub/incoming",
                "(got %r)" % names)

        status, blob = c.read_all("/README.md")
        r.check(status == t.OK and blob == b"top level file\n",
                "serve-root: a top-level file reads back byte for byte")
        status, blob = c.read_all("/src/main.c")
        r.check(status == t.OK and len(blob) == 2048,
                "serve-root: a file in a subdirectory reads whole",
                "(got %s, %d bytes)" % (t.sname(status), len(blob)))
        # pub/ and incoming/ are just directories here.
        status, blob = c.read_all("/pub/note.txt")
        r.check(status == t.OK and blob == b"just a file in a dir called pub\n",
                "serve-root: a dir named pub/ is served as an ordinary dir")

        status, st = c.stat("/README.md")
        r.check(status == t.OK and st["size"] == 15, "serve-root: STAT size")
        r.check(status == t.OK and (st["mode"] & 0o222) == 0,
                "serve-root: STAT reports no write bits")

        # Every way of asking to write is refused before any syscall.
        write_flags = [
            ("O_WRONLY", t.O_WRONLY),
            ("O_RDWR", t.O_RDWR),
            ("O_WRONLY|O_CREAT", t.O_WRONLY | t.O_CREAT),
        ]
        for label, flags in write_flags:
            status, _ = c.open("/README.md", flags)
            r.check_status(status, t.EROFS, "serve-root: OPEN existing %s" % label)
            status, _ = c.open("/newfile.txt", flags)
            r.check_status(status, t.EROFS, "serve-root: OPEN new %s" % label)

        # A read descriptor cannot be turned into a write descriptor.
        status, handle = c.open("/README.md", t.O_RDONLY)
        r.check_status(status, t.OK, "serve-root: OPEN read-only succeeds")
        status, _ = c.write(handle, b"clobber")
        r.check_status(status, t.EROFS, "serve-root: WRITE on a read descriptor")
        c.close_file(handle)

        # The directory-shape mutations stay refused everywhere.
        r.check_status(c.unlink("/README.md"), t.EROFS, "serve-root: UNLINK")
        r.check_status(c.chmod("/README.md"), t.EROFS, "serve-root: CHMOD")
        r.check_status(c.mkdir("/newdir"), t.EROFS, "serve-root: MKDIR")
        r.check_status(c.rmdir("/src"), t.EROFS, "serve-root: RMDIR")
        r.check_status(c.rename("/README.md", "/other.md"), t.EROFS,
                       "serve-root: RENAME")

        # Confinement is the same dirfd walk as the split server: a ".."
        # component is rejected outright, so the served root is still a floor.
        r.check_status(c.read_all("/../etc/passwd")[0], t.EINVAL,
                       "serve-root: a .. component is refused")
        r.check_status(c.stat("/../../etc/hosts")[0], t.EINVAL,
                       "serve-root: STAT cannot climb out with ..")

        c.close()

        # There are no sub-zones: only "/" is mountable now.
        for loc in ("/pub", "/incoming"):
            cc = t.Client(daemon.port, transport=transport)
            status, _ = cc.mount_raw(loc)
            cc.close()
            r.check_status(status, t.ENOENT, "serve-root: %s is not mountable" % loc)

        after = tree_digest(root)
        r.check(before == after, "serve-root: the tree is unchanged on disk")
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def check_readwrite(r, binary, transport):
    base = tempfile.mkdtemp(prefix="de-tnfsd-sr-rw-")
    daemon = None
    try:
        root = build_tree(base)
        daemon = t.Daemon(binary, root, ["--serve-root-rw"], transport=transport)
        c = daemon.client("/")

        # Reading still works.
        status, blob = c.read_all("/README.md")
        r.check(status == t.OK and blob == b"top level file\n",
                "serve-root-rw: existing files still read")

        # Create a brand-new file, write it, read it back.
        status, handle = c.open("/created.txt", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.OK, "serve-root-rw: OPEN new file for write")
        if status == t.OK:
            status, n = c.write(handle, b"fresh contents")
            r.check(status == t.OK and n == 14, "serve-root-rw: WRITE a new file",
                    "(got %s, %d)" % (t.sname(status), n))
            c.close_file(handle)
        status, blob = c.read_all("/created.txt")
        r.check(status == t.OK and blob == b"fresh contents",
                "serve-root-rw: the new file reads back what was written")
        r.check(os.path.exists(os.path.join(root, "created.txt")),
                "serve-root-rw: the new file is on disk under the real name")

        # Overwrite an existing file -- the thing the drop box refuses.
        status, handle = c.open("/README.md", t.O_WRONLY | t.O_TRUNC)
        r.check_status(status, t.OK, "serve-root-rw: OPEN existing for overwrite")
        if status == t.OK:
            c.write(handle, b"replaced!")
            c.close_file(handle)
        status, blob = c.read_all("/README.md")
        r.check(status == t.OK and blob == b"replaced!",
                "serve-root-rw: an existing file is overwritten",
                "(got %r)" % blob)

        # O_EXCL still means exclusive: it will not clobber.
        status, _ = c.open("/created.txt", t.O_WRONLY | t.O_CREAT | t.O_EXCL)
        r.check_status(status, t.EEXIST, "serve-root-rw: O_EXCL on an existing name")

        # A directory cannot be opened as a file.
        status, _ = c.open("/src", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.EISDIR, "serve-root-rw: OPEN a directory for write")

        # The directory-shape mutations are still refused, files-only by design.
        r.check_status(c.unlink("/created.txt"), t.EROFS, "serve-root-rw: UNLINK")
        r.check_status(c.chmod("/created.txt"), t.EROFS, "serve-root-rw: CHMOD")
        r.check_status(c.mkdir("/newdir"), t.EROFS, "serve-root-rw: MKDIR")
        r.check_status(c.rmdir("/src"), t.EROFS, "serve-root-rw: RMDIR")
        r.check_status(c.rename("/created.txt", "/moved.txt"), t.EROFS,
                       "serve-root-rw: RENAME")
        c.close()

        log = daemon.stop()
        daemon = None
        r.check("open write ip=" in log, "serve-root-rw: a write open is logged")
        r.check("write ok ip=" in log, "serve-root-rw: a closed write is logged")

        # -s caps a written file the same way it caps an upload: EFBIG, and the
        # bound is the file's own apparent size, not bytes transferred.
        daemon = t.Daemon(binary, root, ["--serve-root-rw", "-s", "4"],
                          transport=transport)
        c = daemon.client("/")
        status, handle = c.open("/small.bin", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.OK, "serve-root-rw -s: OPEN new file")
        if status == t.OK:
            status, _ = c.write(handle, b"1234")
            r.check_status(status, t.OK, "serve-root-rw -s: a write within -s")
            status, _ = c.write(handle, b"5")
            r.check_status(status, t.EFBIG, "serve-root-rw -s: a write past -s")
            c.close_file(handle)
        c.close()
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def main(transport="udp"):
    binary = t.binary_from_argv()
    r = t.Results("serve-root [%s]" % transport)
    check_readonly(r, binary, transport)
    check_readwrite(r, binary, transport)
    return r.finish()


if __name__ == "__main__":
    sys.exit(max(main("udp"), main("tcp")))
