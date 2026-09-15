#!/usr/bin/env python3
"""-i matches an *existing* name case-insensitively, and only that.

The daemon resolves exact names by default. With -i, a lookup that misses
exactly falls back to an ASCII case-insensitive scan of the directory it is
standing in -- so a CP/M client that upper-cased "HeLlo.TxT" into "HELLO.TXT",
or a Mac/Windows user who typed "hello.txt", still reaches the real file. The
fold is deliberately narrow:

  * it never overrides an exact match (exact wins, no scan);
  * it applies to interior path components as well as the leaf;
  * it only ever *finds an existing name* -- a create keeps the requested
    spelling verbatim, so uploads to the drop box stay exact and two
    differently-cased names never collide there.

Usage: test_ignorecase.py [path/to/de-tnfsd]
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tnfslib as t


def case_sensitive_fs(base):
    """Whether `base` is on a case-sensitive filesystem. macOS and Windows
    default to case-insensitive volumes, on which the *host* folds names before
    the daemon ever sees them -- so the checks that prove the daemon's own fold
    is opt-in, or that a create stays exact, cannot be observed there and are
    skipped rather than reported as failures."""
    probe = os.path.join(base, "CaseProbe")
    with open(probe, "w") as f:
        f.write("x")
    try:
        return not os.path.exists(os.path.join(base, "caseprobe"))
    finally:
        os.remove(probe)


def build_split(base):
    """A normal two-zone root, but with mixed-case names in pub so that no
    client casing matches by accident."""
    root = os.path.join(base, "srv")
    os.makedirs(os.path.join(root, "pub", "SubDir"))
    os.makedirs(os.path.join(root, "incoming"))
    with open(os.path.join(root, "pub", "HeLlo.TxT"), "w") as f:
        f.write("mixed case leaf\n")
    with open(os.path.join(root, "pub", "SubDir", "File.DAT"), "wb") as f:
        f.write(b"\xDE\xAD" * 64)
    return root


def check_pub_readonly(r, binary, transport):
    base = tempfile.mkdtemp(prefix="de-tnfsd-ic-ro-")
    daemon = None
    try:
        root = build_split(base)
        daemon = t.Daemon(binary, root, ["-i"], transport=transport)
        c = daemon.client("/pub")

        body = b"mixed case leaf\n"
        # Exact still works, and so does every folding of it.
        for label, name in [("exact", "HeLlo.TxT"),
                            ("all lower", "hello.txt"),
                            ("all upper (CP/M)", "HELLO.TXT"),
                            ("odd mix", "hELLO.txt")]:
            status, blob = c.read_all("/" + name)
            r.check(status == t.OK and blob == body,
                    "-i pub: read %r resolves the real file" % name,
                    "(got %s)" % t.sname(status))

        # An interior component folds too.
        status, blob = c.read_all("/subdir/file.dat")
        r.check(status == t.OK and len(blob) == 128,
                "-i pub: a mixed-case subdirectory component resolves",
                "(got %s, %d bytes)" % (t.sname(status), len(blob)))
        status, blob = c.read_all("/SUBDIR/FILE.DAT")
        r.check(status == t.OK and len(blob) == 128,
                "-i pub: fully upper-cased path resolves whole")

        # STAT folds the same way OPEN does.
        status, st = c.stat("/hello.txt")
        r.check(status == t.OK and st["size"] == len(body),
                "-i pub: STAT folds an existing name")

        # A name with no case-insensitive match is still a clean miss.
        status, _ = c.open("/nope.txt", t.O_RDONLY)
        r.check_status(status, t.ENOENT, "-i pub: a genuine miss is still ENOENT")

        # The listing reports the real on-disk names, not the client's casing.
        names = sorted(c.listdir("/"))
        r.check(names == ["HeLlo.TxT", "SubDir"],
                "-i pub: listing still shows the real names", "(got %r)" % names)
        c.close()
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def check_off_by_default(r, binary, transport):
    """Without -i the fold does not happen: exact only."""
    base = tempfile.mkdtemp(prefix="de-tnfsd-ic-off-")
    daemon = None
    try:
        root = build_split(base)
        daemon = t.Daemon(binary, root, [], transport=transport)
        c = daemon.client("/pub")

        status, blob = c.read_all("/HeLlo.TxT")
        r.check(status == t.OK and blob == b"mixed case leaf\n",
                "no -i: the exact name reads")
        status, _ = c.open("/hello.txt", t.O_RDONLY)
        r.check_status(status, t.ENOENT, "no -i: a differently-cased name misses")
        status, _ = c.open("/subdir/File.DAT", t.O_RDONLY)
        r.check_status(status, t.ENOENT,
                       "no -i: a mixed-case component misses")
        c.close()
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def check_create_stays_exact(r, binary, transport):
    """The fold finds existing names only. Overwrite folds (it targets a file
    that exists); create does not (there is nothing to find), so a new file
    keeps exactly the name the client asked for -- and the drop box, which
    only ever creates, stays case-sensitive."""
    base = tempfile.mkdtemp(prefix="de-tnfsd-ic-rw-")
    daemon = None
    try:
        root = os.path.join(base, "repo")
        os.makedirs(root)
        with open(os.path.join(root, "README.md"), "w") as f:
            f.write("original\n")

        daemon = t.Daemon(binary, root, ["--serve-root-rw", "-i"],
                          transport=transport)
        c = daemon.client("/")

        # Overwrite via a folded name hits the real, existing file.
        status, h = c.open("/readme.md", t.O_WRONLY | t.O_TRUNC)
        r.check_status(status, t.OK, "-i rw: open existing file by folded name")
        if status == t.OK:
            c.write(h, b"replaced")
            c.close_file(h)
        with open(os.path.join(root, "README.md")) as f:
            r.check(f.read() == "replaced",
                    "-i rw: the fold overwrote the real file")
        # The real on-disk name is unchanged and no second file appeared;
        # os.listdir reports the true name on either kind of host FS.
        r.check(sorted(os.listdir(root)) == ["README.md"],
                "-i rw: still exactly one file, under its real name",
                "(got %r)" % sorted(os.listdir(root)))

        # A genuinely new name has nothing to fold to, so it is created verbatim.
        status, h = c.open("/NewFile.TXT", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.OK, "-i rw: create a brand-new name")
        if status == t.OK:
            c.write(h, b"x")
            c.close_file(h)
        r.check(os.path.exists(os.path.join(root, "NewFile.TXT")),
                "-i rw: a create keeps the requested casing exactly")
        c.close()
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def check_dropbox_stays_exact(r, binary, transport):
    """Even with -i, an upload is a create: it does not fold onto an existing
    name, so a differently-cased upload lands as its own file rather than
    colliding with, or overwriting, what is already there."""
    base = tempfile.mkdtemp(prefix="de-tnfsd-ic-db-")
    daemon = None
    try:
        root = os.path.join(base, "srv")
        os.makedirs(os.path.join(root, "pub"))
        inc = os.path.join(root, "incoming")
        os.makedirs(inc)
        with open(os.path.join(inc, "hello"), "w") as f:
            f.write("already here\n")

        daemon = t.Daemon(binary, root, ["-i"], transport=transport)
        c = daemon.client("/incoming")

        # A differently-cased upload is not folded onto the existing "hello":
        # it is created as its own file.
        status, h = c.open("/HELLO", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.OK, "-i dropbox: upload of a folded name is a new file")
        if status == t.OK:
            c.write(h, b"fresh")
            c.close_file(h)
        r.check(sorted(os.listdir(inc)) == ["HELLO", "hello"],
                "-i dropbox: both casings exist; the fold did not collide",
                "(got %r)" % sorted(os.listdir(inc)))
        with open(os.path.join(inc, "hello")) as f:
            r.check(f.read() == "already here\n",
                    "-i dropbox: the pre-existing file was untouched")

        # The exact-name taken check is still exact: re-uploading "hello" is
        # refused because that name is taken.
        status, _ = c.open("/hello", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.EACCES, "-i dropbox: an exact taken name is refused")
        c.close()
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def main(transport="udp"):
    binary = t.binary_from_argv()
    r = t.Results("ignore-case [%s]" % transport)

    probe_base = tempfile.mkdtemp(prefix="de-tnfsd-ic-probe-")
    try:
        sensitive = case_sensitive_fs(probe_base)
    finally:
        shutil.rmtree(probe_base, ignore_errors=True)

    check_pub_readonly(r, binary, transport)
    check_create_stays_exact(r, binary, transport)
    if sensitive:
        # These distinguish two casings of one name, which only a case-sensitive
        # host filesystem can hold; on macOS/Windows the host folds first.
        check_off_by_default(r, binary, transport)
        check_dropbox_stays_exact(r, binary, transport)
    else:
        print("  note skipping case-sensitive-host checks "
              "(host filesystem folds case)")
    return r.finish()


if __name__ == "__main__":
    sys.exit(max(main("udp"), main("tcp")))
