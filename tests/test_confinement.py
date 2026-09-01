#!/usr/bin/env python3
"""A client must never reach anything outside the served namespace.

Confinement here is structural rather than textual: the daemon splits a
client path into components, rejects "." and "..", and walks from a
directory fd opened at startup with openat(..., O_NOFOLLOW). No path string
is ever built, so there is nothing to normalise and no check-then-use window.

This test plants secrets outside the root, beside the root, and behind
symlinks, then confirms none of them is reachable and that the synthetic
root serves exactly two names.

Usage: test_confinement.py [path/to/de-tnfsd]
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tnfslib as t


def build_tree(base):
    root = os.path.join(base, "root")
    pub = os.path.join(root, "pub")
    inc = os.path.join(root, "incoming")
    os.makedirs(os.path.join(pub, "sub"))
    os.makedirs(inc)

    with open(os.path.join(base, "secret.txt"), "w") as f:
        f.write("SECRET OUTSIDE THE ROOT")
    with open(os.path.join(root, "stray.txt"), "w") as f:
        f.write("STRAY FILE IN THE ROOT")
    with open(os.path.join(pub, "public.txt"), "w") as f:
        f.write("public")
    with open(os.path.join(pub, "sub", "deep.txt"), "w") as f:
        f.write("deep")
    os.mkdir(os.path.join(root, "stray-dir"))

    # Symlinks: out of the served tree entirely, and across into the drop box.
    os.symlink(os.path.join(base, "secret.txt"), os.path.join(pub, "escape.txt"))
    os.symlink(base, os.path.join(pub, "escape-dir"))
    os.symlink(inc, os.path.join(pub, "peek"))
    return root


def main(transport="udp"):
    binary = t.binary_from_argv()
    base = tempfile.mkdtemp(prefix="de-tnfsd-confine-")
    r = t.Results("confinement [%s]" % transport)
    daemon = None
    try:
        root = build_tree(base)
        daemon = t.Daemon(binary, root, transport=transport)
        c = daemon.client("/")

        # The root is synthetic: exactly two names, whatever is on disk.
        names = sorted(c.listdir("/"))
        r.check(names == ["incoming", "pub"], "root lists exactly pub+incoming",
                "(got %r)" % names)

        for stray in ("/stray.txt", "stray.txt", "/stray-dir"):
            status, _ = c.stat(stray)
            r.check_status(status, t.ENOENT, "stray %s is not served" % stray)

        # Traversal, in every spelling a client can send.
        escapes = [
            "..", "/..", "../", "/../secret.txt", "../secret.txt",
            "pub/../../secret.txt", "/pub/../..", "pub/./../../secret.txt",
            "pub/sub/../../../secret.txt", "/pub/sub/../../../../secret.txt",
            "//../secret.txt", "/pub//../../secret.txt",
        ]
        for path in escapes:
            status, _ = c.stat(path)
            r.check_status_in(status, (t.EINVAL, t.ENOENT),
                              "STAT %-32r refused" % path)
            status, _ = c.open(path, t.O_RDONLY)
            r.check_status_in(status, (t.EINVAL, t.ENOENT, t.EISDIR),
                              "OPEN %-32r refused" % path)

        # A directory listing must not fall back to the root when the path is
        # bad: every refusal is an error status, never a handle onto something
        # else.
        for path in ("..", "/../", "/pub/../..", "/nosuch", "/pub/nosuch"):
            status, handle = c.opendir(path)
            r.check(status != t.OK, "OPENDIR %-16r gets no handle" % path,
                    "(got handle %s)" % handle)

        # Symlinks are not followed, in any position.
        status, _ = c.stat("/pub/escape.txt")
        r.check_status(status, t.ENOENT, "symlink out of the tree does not stat")
        status, _ = c.open("/pub/escape.txt", t.O_RDONLY)
        r.check(status != t.OK, "symlink out of the tree does not open",
                "(got %s)" % t.sname(status))
        status, _ = c.open("/pub/escape-dir/secret.txt", t.O_RDONLY)
        r.check(status != t.OK, "symlinked directory component is not followed",
                "(got %s)" % t.sname(status))
        status, handle = c.opendir("/pub/escape-dir")
        r.check(status != t.OK, "symlinked directory does not open as a listing",
                "(got %s)" % t.sname(status))

        # And the ordinary case still works, or the test above proves nothing.
        status, blob = c.read_all("/pub/public.txt")
        r.check(status == t.OK and blob == b"public", "pub/public.txt reads back")
        status, blob = c.read_all("/pub/sub/deep.txt")
        r.check(status == t.OK and blob == b"deep", "pub/sub/deep.txt reads back")

        # A session that mounted /pub cannot climb out of it either.
        cp = daemon.client("/pub")
        # Symlinks are omitted from listings: the daemon will not follow one,
        # so offering the name would only advertise something unreadable.
        listing = sorted(cp.listdir("/"))
        r.check(listing == ["public.txt", "sub"],
                "mounting /pub lists pub's own contents, symlinks omitted",
                "(got %r)" % listing)
        status, _ = cp.stat("../incoming")
        r.check_status_in(status, (t.EINVAL, t.ENOENT),
                          "/pub mount cannot name ../incoming")
        status, blob = cp.read_all("public.txt")
        r.check(status == t.OK and blob == b"public",
                "/pub mount reads its own files")
        cp.close()
        c.close()
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)
    return r.finish()


if __name__ == "__main__":
    # Every check runs over both transports: the handlers are shared, but the
    # framing, session teardown and peer lookup are not.
    sys.exit(max(main("udp"), main("tcp")))
