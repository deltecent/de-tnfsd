#!/usr/bin/env python3
"""A freshly accepted TCP connection is always served, even after churn.

Regression for issue #3. `net_run()` built its pollfd array and `nfds` from the
current client count, then accepted a new connection *before* the servicing
loop -- so the new fd landed at clients[nclients] and the loop read its revents
from pfd[2 + nclients], an index poll() never filled. That slot held a stale
value left on the stack by an earlier pass; if a previous client had hung up
there with POLLHUP, the fresh connection was torn down before its first recv().

The tell was a strace showing `accept() = 9` immediately followed by `close(9)`
-- no recv(), no getpeername() -- on a daemon that had already handled dozens of
connections, while UDP kept mounting fine. A fresh daemon never showed it,
because the stale slot was still zeroed; it took connection churn to seed a
POLLHUP into the slot a later accept would reuse.

The exact stale bits depend on the host's poll() semantics, so this drives
churn -- persistent idle slots plus a stream of abrupt FIN/RST teardowns -- and
asserts that an ordinary MOUNT interleaved throughout always gets a full,
correct reply and the daemon stays responsive. It passes trivially on the
fixed daemon and flakes hard on the buggy one.

Usage: test_tcpchurn.py [path/to/de-tnfsd]
"""

import os
import shutil
import socket
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tnfslib as t

ROUNDS = 200
SLOT_HOLDERS = 4      # idle mounted connections that keep clients[] non-empty


def build_root(base):
    root = os.path.join(base, "root")
    os.makedirs(os.path.join(root, "pub"))
    os.makedirs(os.path.join(root, "incoming"))
    with open(os.path.join(root, "pub", "hello.txt"), "w") as f:
        f.write("hi\n")
    return root


def stray_connect(port, reset):
    """Connect, get accepted into clients[], then tear down abruptly.

    A plain close sends FIN; SO_LINGER {1,0} sends RST instead. Both leave a
    revents bit in the slot the daemon was polling -- the seed the old bug then
    misread against a later accept."""
    s = socket.create_connection((t.HOST, port), timeout=3.0)
    if reset:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
    s.close()


def check_churn(r, binary, transport):
    if transport != "tcp":
        r.check(True, "tcp-churn: not applicable to udp (no accept path)")
        return

    base = tempfile.mkdtemp(prefix="de-tnfsd-churn-")
    daemon = None
    holders = []
    try:
        root = build_root(base)
        daemon = t.Daemon(binary, root, transport=transport)

        # Persistent idle mounts hold low slots in clients[], so each stray
        # connection is accepted into a higher index that a later real MOUNT
        # will reuse -- the collision the bug needed.
        for _ in range(SLOT_HOLDERS):
            holders.append(daemon.client("/"))

        good = 0
        for i in range(ROUNDS):
            # Seed a couple of slots with an abrupt teardown, alternating the
            # FIN and RST paths so both revents shapes get exercised.
            stray_connect(daemon.port, reset=(i % 2 == 0))
            stray_connect(daemon.port, reset=(i % 3 == 0))

            # A brand-new connection that must be served cleanly.
            c = t.Client(daemon.port, transport=transport)
            try:
                status, _ = c.mount_raw("/")
                if not r.check(status == t.OK,
                               "tcp-churn: fresh MOUNT served (round %d)" % i,
                               "(got %s)" % t.sname(status)):
                    break
                # And the session actually works, not just the mount header.
                names = sorted(c.listdir("/"))
                if not r.check(names == ["incoming", "pub"],
                               "tcp-churn: fresh session lists root (round %d)" % i,
                               "(got %r)" % names):
                    break
                c.request(t.UMOUNT)
                good += 1
            except (t.ProtocolError, socket.timeout, OSError) as e:
                r.check(False, "tcp-churn: fresh connection served (round %d)" % i,
                        "(%s: %s)" % (type(e).__name__, e))
                break
            finally:
                c.close()

        r.check(good == ROUNDS,
                "tcp-churn: all %d post-churn mounts succeeded" % ROUNDS,
                "(got %d)" % good)

        # The idle holders were never disturbed by any of the above.
        for h in holders:
            r.check(sorted(h.listdir("/")) == ["incoming", "pub"],
                    "tcp-churn: a persistent connection stayed usable")
    finally:
        for h in holders:
            h.close()
        if daemon is not None:
            daemon.stop()
        shutil.rmtree(base, ignore_errors=True)


def main(transport="udp"):
    binary = t.binary_from_argv()
    r = t.Results("tcp-churn [%s]" % transport)
    check_churn(r, binary, transport)
    return r.finish()


if __name__ == "__main__":
    sys.exit(max(main("udp"), main("tcp")))
