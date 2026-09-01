"""Just enough of the TNFS protocol to drive a real daemon over UDP or TCP.

Written from tnfs-protocol.md (updated July 15 2020). Shared by the three
test scripts; it deliberately speaks the wire format directly rather than
reusing anything from the daemon, so a mistake in the daemon's encoding
shows up as a test failure instead of cancelling out.
"""

import os
import signal
import socket
import struct
import subprocess
import sys
import time

HOST = "127.0.0.1"

# Commands
MOUNT, UMOUNT = 0x00, 0x01
OPENDIR, READDIR, CLOSEDIR, MKDIR, RMDIR = 0x10, 0x11, 0x12, 0x13, 0x14
TELLDIR, SEEKDIR, OPENDIRX, READDIRX = 0x15, 0x16, 0x17, 0x18
OPENFILE_OLD, READ, WRITE, CLOSE = 0x20, 0x21, 0x22, 0x23
STAT, LSEEK, UNLINK, CHMOD, RENAME, OPEN = 0x24, 0x25, 0x26, 0x27, 0x28, 0x29
SIZE, FREE, SIZEBYTES, FREEBYTES = 0x30, 0x31, 0x32, 0x33

# Status codes
OK, EPERM, ENOENT, EIO = 0x00, 0x01, 0x02, 0x03
EBADF, EAGAIN, EACCES, EEXIST = 0x06, 0x07, 0x09, 0x0B
EISDIR, EINVAL, EMFILE, EFBIG = 0x0D, 0x0E, 0x10, 0x11
ENOSPC, EROFS, ENAMETOOLONG, ENOSYS = 0x12, 0x14, 0x15, 0x16
EOF, BADHANDLE = 0x21, 0xFF

# OPEN flags
O_RDONLY, O_WRONLY, O_RDWR = 0x0001, 0x0002, 0x0003
O_APPEND, O_CREAT, O_TRUNC, O_EXCL = 0x0008, 0x0100, 0x0200, 0x0400

STATUS_NAMES = {
    OK: "OK", EPERM: "EPERM", ENOENT: "ENOENT", EIO: "EIO", EBADF: "EBADF",
    EAGAIN: "EAGAIN", EACCES: "EACCES", EEXIST: "EEXIST", EISDIR: "EISDIR",
    EINVAL: "EINVAL", EMFILE: "EMFILE", EFBIG: "EFBIG", ENOSPC: "ENOSPC",
    EROFS: "EROFS", ENAMETOOLONG: "ENAMETOOLONG", ENOSYS: "ENOSYS",
    EOF: "EOF", BADHANDLE: "BADHANDLE",
}


def sname(status):
    return STATUS_NAMES.get(status, "0x%02X" % status)


class ProtocolError(Exception):
    pass


class Client:
    def __init__(self, port, host=HOST, timeout=3.0, transport="udp"):
        self.host, self.port, self.transport = host, port, transport
        if transport == "tcp":
            self.sock = socket.create_connection((host, port), timeout)
        elif transport == "udp":
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        else:
            raise ValueError("unknown transport %r" % transport)
        self.sock.settimeout(timeout)
        self.sid = 0
        self.seq = 0

    def close(self):
        self.sock.close()

    def _send(self, msg):
        """One request out. On TCP the daemon reads one datagram per recv,
        so a request is still exactly one write."""
        if self.transport == "tcp":
            self.sock.send(msg)
        else:
            self.sock.sendto(msg, (self.host, self.port))

    def request(self, cmd, payload=b"", sid=None):
        """Send one command, return (status, data-after-status)."""
        sid = self.sid if sid is None else sid
        self.seq = (self.seq + 1) & 0xFF
        msg = struct.pack("<HBB", sid, self.seq, cmd) + payload
        self._send(msg)
        while True:
            reply = self.sock.recv(2048)
            if len(reply) < 5:
                raise ProtocolError("short reply: %r" % reply)
            rsid, rseq, rcmd = struct.unpack("<HBB", reply[:4])
            if rseq != self.seq or rcmd != cmd:
                continue            # a stale reply to an earlier request
            return reply[4], reply[5:]

    def mount_raw(self, location="/"):
        """MOUNT that captures the session id from the reply header."""
        self.seq = (self.seq + 1) & 0xFF
        payload = struct.pack("<H", 0x0102) + location.encode() + b"\0\0\0"
        msg = struct.pack("<HBB", 0, self.seq, MOUNT) + payload
        self._send(msg)
        reply = self.sock.recv(2048)
        rsid, rseq, rcmd = struct.unpack("<HBB", reply[:4])
        status = reply[4]
        if status == OK:
            self.sid = rsid
        return status, reply[5:]

    # -- convenience wrappers ------------------------------------------
    def opendir(self, path):
        status, data = self.request(OPENDIR, path.encode() + b"\0")
        return status, (data[0] if status == OK and data else None)

    def opendirx(self, path, diropt=0, dirsort=0, maxres=0, pattern=""):
        payload = (bytes([diropt, dirsort]) + struct.pack("<H", maxres) +
                   pattern.encode() + b"\0" + path.encode() + b"\0")
        status, data = self.request(OPENDIRX, payload)
        if status != OK:
            return status, None, 0
        count = struct.unpack("<H", data[1:3])[0] if len(data) >= 3 else 0
        return status, data[0], count

    def readdir(self, handle):
        status, data = self.request(READDIR, bytes([handle]))
        if status != OK:
            return status, None
        return status, data.split(b"\0")[0].decode()

    def listdir(self, path):
        """Every name in a directory, via OPENDIR/READDIR/CLOSEDIR."""
        status, handle = self.opendir(path)
        if status != OK:
            raise ProtocolError("OPENDIR %s: %s" % (path, sname(status)))
        names = []
        while True:
            status, name = self.readdir(handle)
            if status == EOF:
                break
            if status != OK:
                raise ProtocolError("READDIR: %s" % sname(status))
            names.append(name)
        self.request(CLOSEDIR, bytes([handle]))
        return names

    def open(self, path, flags, mode=0o644):
        payload = struct.pack("<HH", flags, mode) + path.encode() + b"\0"
        status, data = self.request(OPEN, payload)
        return status, (data[0] if status == OK and data else None)

    def read(self, handle, size):
        status, data = self.request(READ, bytes([handle]) + struct.pack("<H", size))
        if status != OK:
            return status, b""
        n = struct.unpack("<H", data[:2])[0]
        return status, data[2:2 + n]

    def read_all(self, path):
        status, handle = self.open(path, O_RDONLY)
        if status != OK:
            return status, b""
        blob = b""
        while True:
            status, chunk = self.read(handle, 512)
            if status == EOF:
                break
            if status != OK:
                return status, blob
            blob += chunk
        self.request(CLOSE, bytes([handle]))
        return OK, blob

    def write(self, handle, data):
        payload = bytes([handle]) + struct.pack("<H", len(data)) + data
        status, resp = self.request(WRITE, payload)
        if status != OK:
            return status, 0
        return status, struct.unpack("<H", resp[:2])[0]

    def close_file(self, handle):
        status, _ = self.request(CLOSE, bytes([handle]))
        return status

    def stat(self, path):
        status, data = self.request(STAT, path.encode() + b"\0")
        if status != OK:
            return status, None
        mode, uid, gid, size, atime, mtime, ctime = struct.unpack("<HHHIIII", data[:22])
        return status, {"mode": mode, "size": size, "mtime": mtime}

    def lseek(self, handle, offset, whence=0):
        payload = bytes([handle, whence]) + struct.pack("<i", offset)
        status, data = self.request(LSEEK, payload)
        if status != OK:
            return status, None
        return status, struct.unpack("<I", data[:4])[0]

    def unlink(self, path):
        return self.request(UNLINK, path.encode() + b"\0")[0]

    def mkdir(self, path):
        return self.request(MKDIR, path.encode() + b"\0")[0]

    def rmdir(self, path):
        return self.request(RMDIR, path.encode() + b"\0")[0]

    def chmod(self, path, mode=0o777):
        return self.request(CHMOD, struct.pack("<H", mode) + path.encode() + b"\0")[0]

    def rename(self, src, dst):
        payload = src.encode() + b"\0" + dst.encode() + b"\0"
        return self.request(RENAME, payload)[0]

    def freebytes(self):
        status, data = self.request(FREEBYTES)
        if status != OK:
            return status, 0
        return status, struct.unpack("<Q", data[:8])[0]

    def sizebytes(self):
        status, data = self.request(SIZEBYTES)
        if status != OK:
            return status, 0
        return status, struct.unpack("<Q", data[:8])[0]


class Daemon:
    """A de-tnfsd process on an ephemeral port."""

    def __init__(self, binary, root, extra_args=(), transport="udp"):
        self.port = _free_port()
        self.transport = transport
        cmd = [binary, "-p", str(self.port)] + list(extra_args) + [root]
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self._wait_ready()

    def _wait_ready(self):
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                out = self.proc.stdout.read().decode(errors="replace")
                raise RuntimeError("daemon exited during startup:\n%s" % out)
            try:
                c = Client(self.port, timeout=0.25, transport=self.transport)
            except OSError:              # TCP listener not up yet
                time.sleep(0.1)
                continue
            try:
                if c.mount_raw("/")[0] == OK:
                    c.request(UMOUNT)
                    return
            except (socket.timeout, OSError):
                pass
            finally:
                c.close()
            time.sleep(0.1)
        raise RuntimeError("daemon did not become ready")

    def client(self, location="/"):
        c = Client(self.port, transport=self.transport)
        status, _ = c.mount_raw(location)
        if status != OK:
            raise ProtocolError("MOUNT %s: %s" % (location, sname(status)))
        return c

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        return self.proc.stdout.read().decode(errors="replace")


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((HOST, 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Results:
    def __init__(self, title):
        self.title = title
        self.failures = []
        self.checks = 0
        print("== %s" % title)

    def check(self, ok, label, detail=""):
        self.checks += 1
        if ok:
            print("  ok   %s" % label)
        else:
            print("  FAIL %s %s" % (label, detail))
            self.failures.append("%s %s" % (label, detail))
        return ok

    def check_status(self, got, want, label):
        return self.check(got == want, label,
                          "(got %s, want %s)" % (sname(got), sname(want)))

    def check_status_in(self, got, wanted, label):
        return self.check(got in wanted, label,
                          "(got %s, want one of %s)" %
                          (sname(got), " ".join(sname(w) for w in wanted)))

    def finish(self):
        if self.failures:
            print("\n%s: %d of %d checks FAILED" %
                  (self.title, len(self.failures), self.checks))
            return 1
        print("\n%s: all %d checks passed" % (self.title, self.checks))
        return 0


def binary_from_argv(default="bin/de-tnfsd"):
    path = sys.argv[1] if len(sys.argv) > 1 else default
    if not os.path.isfile(path) or not os.access(path, os.X_OK):
        sys.exit("usage: %s [path/to/de-tnfsd]" % sys.argv[0])
    return os.path.abspath(path)
