#!/usr/bin/env python3
"""incoming/ takes uploads and gives nothing back.

This is the spec for DESIGN.md 5: each channel by which the drop box could
leak, and the check that it is closed. It runs with the drop box mode 0777 on
purpose - the daemon is the only enforcement point, so any refusal that has
quietly come to depend on the kernel shows up here as a failure rather than
as a deployment that happens to be tight enough.

Usage: test_dropbox.py [path/to/de-tnfsd]
"""

import os
import shutil
import stat as statmod
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tnfslib as t

PLANTED = "planted.dat"
PLANTED_BODY = b"a file already in the drop box\n"
OUTSIDE_BODY = b"a file the drop box must not be able to redirect a write into\n"


def build_tree(base, dropbox_mode=0o777):
    root = os.path.join(base, "root")
    pub = os.path.join(root, "pub")
    inc = os.path.join(root, "incoming")
    os.makedirs(pub)
    os.makedirs(inc)

    with open(os.path.join(pub, "readme.txt"), "w") as f:
        f.write("public\n")
    with open(os.path.join(inc, PLANTED), "wb") as f:
        f.write(PLANTED_BODY)
    with open(os.path.join(base, "outside.txt"), "wb") as f:
        f.write(OUTSIDE_BODY)

    # A symlink under pub aimed at the drop box, and one inside the drop box
    # aimed out of the tree. Neither may be usable.
    os.symlink(inc, os.path.join(pub, "peek"))
    os.symlink(os.path.join(base, "outside.txt"), os.path.join(inc, "redirect"))

    # World-writable, deliberately: nothing below may depend on mode bits.
    os.chmod(inc, dropbox_mode)
    return root, pub, inc


def upload(c, name, data, r=None, label=None):
    """OPEN/WRITE/CLOSE an upload; returns (open_status, write_status, close_status)."""
    status, handle = c.open("/incoming/" + name, t.O_WRONLY | t.O_CREAT)
    if status != t.OK:
        return status, None, None
    wstatus = t.OK
    for off in range(0, len(data), 512):
        wstatus, _ = c.write(handle, data[off:off + 512])
        if wstatus != t.OK:
            break
    cstatus = c.close_file(handle)
    return status, wstatus, cstatus


def main(transport="udp"):
    binary = t.binary_from_argv()
    base = tempfile.mkdtemp(prefix="de-tnfsd-drop-")
    r = t.Results("drop box [%s]" % transport)
    daemon = None
    try:
        root, pub, inc = build_tree(base)
        r.check(statmod.S_IMODE(os.stat(inc).st_mode) == 0o777,
                "row 12: the drop box is world-writable for this whole run")

        daemon = t.Daemon(binary, root, transport=transport)
        c = daemon.client("/")

        # -- row 1: it cannot be listed ---------------------------------
        status, handle = c.opendir("/incoming")
        r.check_status(status, t.EACCES, "row 1: OPENDIR /incoming")
        r.check(handle is None, "row 1: OPENDIR hands back no handle")
        status, handle, count = c.opendirx("/incoming")
        r.check_status(status, t.EACCES, "row 1: OPENDIRX /incoming")
        ci = daemon.client("/incoming")
        status, _ = ci.opendir("/")
        r.check_status(status, t.EACCES, "row 1: OPENDIR from an /incoming mount")
        ci.close()

        # -- row 2: no recursive traverse reaches into it ---------------
        TRAVERSE = 0x10
        for path in ("/", "/pub"):
            status, handle, count = c.opendirx(path, diropt=TRAVERSE)
            r.check_status(status, t.OK, "row 2: OPENDIRX %s with traverse" % path)
            names = []
            while True:
                st, name = c.readdir(handle)
                if st != t.OK:
                    break
                names.append(name)
            c.request(t.CLOSEDIR, bytes([handle]))
            r.check(PLANTED not in names,
                    "row 2: traverse of %s does not reach drop-box names" % path,
                    "(got %r)" % names)
            r.check(all("/" not in n for n in names),
                    "row 2: traverse of %s stays single-level" % path)

        # -- row 3: names inside it cannot be looked up -----------------
        st_exists, _ = c.stat("/incoming/" + PLANTED)
        st_absent, _ = c.stat("/incoming/definitely-not-there.dat")
        r.check_status(st_exists, t.EACCES, "row 3: STAT an existing drop-box name")
        r.check_status(st_absent, t.EACCES, "row 3: STAT an absent drop-box name")
        r.check(st_exists == st_absent,
                "row 3: present and absent names are indistinguishable")
        status, st = c.stat("/incoming")
        r.check(status == t.OK and statmod.S_ISDIR(st["mode"]),
                "row 3: the drop box itself still stats as a directory")

        # -- row 4: it cannot be read -----------------------------------
        status, _ = c.open("/incoming/" + PLANTED, t.O_RDONLY)
        r.check_status(status, t.EACCES, "row 4: OPEN for read")
        status, _ = c.open("/incoming/" + PLANTED, t.O_RDONLY | t.O_CREAT)
        r.check_status(status, t.EACCES, "row 4: OPEN for read with O_CREAT")

        # O_RDWR is downgraded rather than refused, so the descriptor exists
        # but has no read capability.
        status, handle = c.open("/incoming/rdwr-probe.bin", t.O_RDWR | t.O_CREAT)
        r.check_status(status, t.OK, "row 4: O_RDWR is downgraded, not refused")
        if status == t.OK:
            c.write(handle, b"x" * 16)
            rstatus, blob = c.read(handle, 16)
            r.check_status(rstatus, t.EACCES,
                           "row 4: READ on a drop-box descriptor")
            r.check(blob == b"", "row 4: no bytes come back")
            c.close_file(handle)

        # -- rows 5 and 6: every refusal looks the same -----------------
        taken, _, _ = upload(c, PLANTED, b"clobber")
        r.check_status(taken, t.EACCES, "row 5: uploading a name that exists")
        r.check(taken != t.EEXIST, "row 5: EEXIST is never returned")
        with open(os.path.join(inc, PLANTED), "rb") as f:
            r.check(f.read() == PLANTED_BODY,
                    "row 5: the existing file is untouched")

        # -- row 7: rename cannot carry anything out --------------------
        r.check_status(c.rename("/incoming/" + PLANTED, "/pub/stolen.dat"),
                       t.EACCES, "row 7: RENAME drop box -> pub")
        r.check_status(c.rename("/pub/readme.txt", "/incoming/x.dat"),
                       t.EACCES, "row 7: RENAME pub -> drop box")
        r.check_status(c.rename("/incoming/" + PLANTED, "/incoming/other.dat"),
                       t.EACCES, "row 7: RENAME within the drop box")
        r.check_status(c.rename("/pub/readme.txt", "/pub/other.txt"),
                       t.EROFS, "row 7: RENAME within pub")

        # -- row 8: a symlink under pub aimed at the drop box -----------
        status, _ = c.opendir("/pub/peek")
        r.check(status != t.OK, "row 8: OPENDIR through a symlink into the drop box",
                "(got %s)" % t.sname(status))
        status, _ = c.stat("/pub/peek/" + PLANTED)
        r.check(status != t.OK, "row 8: STAT through that symlink",
                "(got %s)" % t.sname(status))
        status, _ = c.open("/pub/peek/" + PLANTED, t.O_RDONLY)
        r.check(status != t.OK, "row 8: OPEN through that symlink",
                "(got %s)" % t.sname(status))

        # -- row 9: a symlink inside the drop box as a write redirect ---
        status, _, _ = upload(c, "redirect", b"REDIRECTED")
        r.check_status(status, t.EACCES, "row 9: uploading onto a planted symlink")
        with open(os.path.join(base, "outside.txt"), "rb") as f:
            r.check(f.read() == OUTSIDE_BODY,
                    "row 9: the symlink's target is untouched")

        # -- the drop box does accept uploads ---------------------------
        body = bytes(range(256)) * 6
        ostatus, wstatus, cstatus = upload(c, "upload.bin", body)
        r.check(ostatus == t.OK and wstatus == t.OK and cstatus == t.OK,
                "upload: open/write/close all succeed",
                "(%s/%s/%s)" % (t.sname(ostatus), t.sname(wstatus or 0),
                                t.sname(cstatus or 0)))
        landed = os.path.join(inc, "upload.bin")
        r.check(os.path.isfile(landed), "upload: the file lands in incoming/")
        if os.path.isfile(landed):
            with open(landed, "rb") as f:
                r.check(f.read() == body, "upload: contents are byte for byte")
            # 0660, and independent of the umask the daemon was started
            # with: it sets its own (main.c), so the group that drains the
            # drop box can read what landed regardless of the environment.
            r.check(statmod.S_IMODE(os.stat(landed).st_mode) == 0o660,
                    "upload: the file is created 0660")
        r.check(not any(n.startswith(".tmp-") for n in os.listdir(inc)),
                "upload: no temp file is left behind")

        # A partial upload is never visible under its final name.
        status, handle = c.open("/incoming/partial.bin", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.OK, "finalization: a second upload opens")
        c.write(handle, b"half a file")
        r.check(not os.path.exists(os.path.join(inc, "partial.bin")),
                "finalization: the name does not appear until CLOSE")
        r.check(any(n.startswith(".tmp-") for n in os.listdir(inc)),
                "finalization: the bytes go to a temp name meanwhile")
        r.check_status(c.close_file(handle), t.OK, "finalization: CLOSE succeeds")
        r.check(os.path.exists(os.path.join(inc, "partial.bin")),
                "finalization: the name appears at CLOSE")

        # A dropped TCP connection is the one teardown UDP cannot express:
        # the daemon sees POLLHUP, closes the session, and the in-flight
        # upload's temp file goes with it.
        if transport == "tcp":
            cdrop = daemon.client("/")
            status, handle = cdrop.open("/incoming/dropped.bin",
                                        t.O_WRONLY | t.O_CREAT)
            r.check_status(status, t.OK, "tcp: an upload opens on its own connection")
            cdrop.write(handle, b"half a file")
            cdrop.close()                       # no CLOSE, no UMOUNT
            deadline = time.time() + 3.0
            while time.time() < deadline:
                if not any(n.startswith(".tmp-") for n in os.listdir(inc)):
                    break
                time.sleep(0.05)
            r.check(not any(n.startswith(".tmp-") for n in os.listdir(inc)),
                    "tcp: the temp file is unlinked when the connection drops")
            r.check(not os.path.exists(os.path.join(inc, "dropped.bin")),
                    "tcp: the partial upload never lands under its name")

        # -- nothing else is permitted in the zone ----------------------
        r.check_status(c.unlink("/incoming/" + PLANTED), t.EACCES, "UNLINK")
        r.check_status(c.chmod("/incoming/" + PLANTED), t.EACCES, "CHMOD")
        r.check_status(c.mkdir("/incoming/subdir"), t.EACCES, "MKDIR")
        r.check_status(c.rmdir("/incoming"), t.EACCES, "RMDIR of the drop box")
        status, _, _ = upload(c, "sub/nested.bin", b"x")
        r.check_status(status, t.EACCES, "the drop box is flat: no subdirectories")

        # Name syntax is a property of the request, so it may differ - it says
        # nothing about what the directory holds.
        for bad in (".hidden", "with/slash", "\x01control", "x" * 200):
            status, _, _ = upload(c, bad, b"x")
            r.check(status in (t.EINVAL, t.EACCES, t.ENAMETOOLONG),
                    "bad upload name %r refused" % bad[:16],
                    "(got %s)" % t.sname(status))

        # Flags that only make sense against an existing file.
        for label, flags in (("O_TRUNC", t.O_WRONLY | t.O_CREAT | t.O_TRUNC),
                             ("O_APPEND", t.O_WRONLY | t.O_CREAT | t.O_APPEND),
                             ("no O_CREAT", t.O_WRONLY)):
            status, _ = c.open("/incoming/flagprobe.bin", flags)
            r.check_status(status, t.EACCES, "upload with %s" % label)

        c.close()
        daemon.stop()

        # -- row 10: free space is capped at the quota ------------------
        daemon = t.Daemon(binary, root, ["-q", "64K"], transport=transport)
        c = daemon.client("/")
        status, freeb = c.freebytes()
        r.check(status == t.OK and freeb <= 64 * 1024,
                "row 10: FREEBYTES is capped at the remaining quota",
                "(got %d)" % freeb)
        status, sizeb = c.sizebytes()
        r.check(status == t.OK and sizeb > 0, "SIZEBYTES answers")
        c.close()
        daemon.stop()

        # -- the size limit, checked against apparent size --------------
        daemon = t.Daemon(binary, root, ["-s", "1K"], transport=transport)
        c = daemon.client("/")
        ostatus, wstatus, cstatus = upload(c, "toobig.bin", b"z" * 4096)
        r.check_status(ostatus, t.OK, "-s: an oversized upload still opens")
        r.check_status(wstatus, t.EFBIG, "-s: the write that crosses it fails")
        r.check_status(cstatus, t.EFBIG, "-s: CLOSE reports EFBIG, not success")
        r.check(not os.path.exists(os.path.join(inc, "toobig.bin")),
                "-s: nothing lands under the requested name")
        r.check(not any(n.startswith(".tmp-") for n in os.listdir(inc)),
                "-s: the temp file is unlinked on abort")

        # A seek does not buy extra room: the limit is on apparent size.
        status, handle = c.open("/incoming/sparse.bin", t.O_WRONLY | t.O_CREAT)
        r.check_status(status, t.OK, "-s: sparse-file probe opens")
        sstatus, pos = c.lseek(handle, 1 << 20, 0)
        r.check(sstatus == t.OK and pos == 1 << 20,
                "-s: SEEK on an upload descriptor is allowed")
        wstatus, _ = c.write(handle, b"!")
        r.check_status(wstatus, t.EFBIG, "-s: one byte at a huge offset is EFBIG")
        c.close_file(handle)
        r.check(not os.path.exists(os.path.join(inc, "sparse.bin")),
                "-s: the sparse upload is discarded")
        c.close()
        daemon.stop()

        # -- row 6 again: a full drop box is indistinguishable ----------
        daemon = t.Daemon(binary, root, ["-n", "1"], transport=transport)
        c = daemon.client("/")
        full, _, _ = upload(c, "when-full.bin", b"x")
        r.check_status(full, t.EACCES, "-n: an upload into a full drop box")
        r.check(full == taken,
                "row 6: 'full' and 'name taken' are the same status")
        r.check(full == st_exists,
                "row 6: ... and the same as a refused STAT")
        r.check(not os.path.exists(os.path.join(inc, "when-full.bin")),
                "-n: nothing lands when the limit is reached")
        c.close()
        daemon.stop()
        daemon = None

        # -- row 11 ------------------------------------------------------
        print("  note row 11: session-id-plus-IP binding is not exercised here;"
              " it needs a second source address, and DESIGN.md 5 already"
              " records that it is not a security boundary on UDP.")

        # The drop box is still world-writable, and nothing above leaned on it.
        r.check(statmod.S_IMODE(os.stat(inc).st_mode) == 0o777,
                "row 12: the drop box mode was never load-bearing")
    finally:
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)
    return r.finish()


if __name__ == "__main__":
    # Every check runs over both transports: the handlers are shared, but the
    # framing, session teardown and peer lookup are not.
    sys.exit(max(main("udp"), main("tcp")))
