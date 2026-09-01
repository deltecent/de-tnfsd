# de-tnfsd

A TNFS daemon whose served namespace is split into a read-only zone and a
write-only zone, with no path that is ever both.

```
<root>/
    pub/         anonymous read-only, recursive
    incoming/    anonymous drop box: create new files only.
                 Cannot be listed, read, stat'ed, overwritten, or deleted.
```

Nothing else is served. There are no read-write directories, no per-user
areas, and no authentication. The wire protocol is unchanged, so existing
FujiNet and 8-bit clients work without modification.

`DESIGN.md` is the specification; this README is how to build and run it.

## Building

POSIX only — Linux, the BSDs, macOS. C11, libc, no external dependencies.
There is no Windows build and there will not be one; see DESIGN.md §3.

```
make                    # -> bin/de-tnfsd
make check              # build and run the three test suites
make debug              # rebuild with ASan and UBSan
make install            # PREFIX=/usr/local
```

## Running

```
de-tnfsd [-p <port>] [-s <max-file-size>] [-n <max-files>]
         [-q <max-total-bytes>] [--no-incoming] [-v] <root>

  -p  port to listen on                            (default 16384)
  -s  maximum size of one uploaded file            (default 16M, 0 = no limit)
  -n  maximum number of files in incoming/         (default 256, 0 = no limit)
  -q  maximum total bytes in incoming/             (default 1G,  0 = no limit)
      --no-incoming    serve pub/ only; reject all writes
  -v  verbose logging
```

Size arguments take a `K`, `M` or `G` suffix (powers of 1024); a bare number
is bytes. An unparseable value is a startup error, not a silently clamped
default.

It runs in the foreground, logs one line per event to stderr, and does not
fork, detach, or write a pidfile. `SIGINT` and `SIGTERM` start a clean
shutdown — including unlinking the temp file of any upload still in flight;
a second signal exits immediately. The same binary and arguments are used
under systemd (`de-tnfsd.service` ships a unit); when `$NOTIFY_SOCKET` is
unset every part of the readiness protocol is a no-op, which is what makes
the shell case work unchanged.

Started as root, the daemon chroots to `<root>` and drops to `nobody` after
opening its directory fds — which is why the unit must not set `User=`.

## Permissions

The daemon is the only enforcement point. Set `incoming` to `0777` and a
client still cannot list it, stat a name in it, read a byte out of it,
overwrite anything in it, or delete anything from it: those operations are
refused by the capability table before any syscall is attempted. Making `pub`
world-writable does not make it writable over TNFS either.

Tight modes (`0755` on `pub`, `0733` on `incoming`) are a reasonable habit and
cost nothing, but nothing here depends on them, and the drop-box test suite
runs with the directory world-writable to keep that honest.

The daemon needs exactly two things from the filesystem: it must be able to
read `pub`, and create files in `incoming`. It checks both by attempting them
at startup and refuses to start if either fails.

## Draining the drop box

Out of scope for the daemon, and it should run as a different uid. The
daemon's job ends at "the file is on disk"; anything that inspects, scans,
moves, or publishes uploads is a separate process with its own privileges.

Since the daemon deliberately cannot list the drop box for anyone, its upload
log is the operator's visibility into it: source IP, requested name, final
size, duration, and outcome, one line per upload.

## Tests

Each test is a standalone script that spawns a real daemon and speaks TNFS to
it, running every check twice — once over UDP and once over TCP:

```
python3 tests/test_confinement.py bin/de-tnfsd   # nothing outside the root is reachable
python3 tests/test_readonly.py    bin/de-tnfsd   # pub refuses every mutation
python3 tests/test_dropbox.py     bin/de-tnfsd   # DESIGN.md 5, row by row
```
