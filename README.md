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

(For the quick-and-dirty case there are two opt-in flags that instead serve a
single directory directly, read-only or read/write — see "Serving a directory
directly" below. The two-zone server above is the default and is unchanged.)

`DESIGN.md` is the specification; this README is how to build and run it.

## Building

POSIX only — Linux, the BSDs, macOS. C11, libc, no external dependencies.
There is no Windows build and there will not be one; see DESIGN.md §3.

```
make                    # -> bin/de-tnfsd
make check              # build and run the four test suites
make debug              # rebuild with ASan and UBSan
make install            # PREFIX=/usr/local
```

## Installing

`install.sh` does the whole first-time setup on Linux: build, create the
`tnfs` system user, make `<root>/pub` and `<root>/incoming` with the modes
below, install the binary, and install and start the unit.

```
sudo ./install.sh                                  # /usr/local, /srv/tnfs
sudo ./install.sh --prefix /opt/tnfs --root /data/tnfs --port 16385
./install.sh --dry-run                             # print, change nothing
```

`--no-start` installs the unit without starting it, `--no-service` skips
systemd entirely, `--check` runs the test suites before installing. Re-running
is safe: an existing user or directory is kept, and a unit file that differs
from what the script would write is backed up first. `--dry-run` needs no
privileges and prints every command it would run, including the rewritten
unit.

Everything it does is in `make install` plus a `useradd` and four `chmod`s;
elsewhere than Linux, do those by hand.

## Running

```
de-tnfsd [-p <port>] [-s <max-file-size>] [-n <max-files>]
         [-q <max-total-bytes>] [--no-incoming]
         [--serve-root | --serve-root-rw] [-v] <root>

  -p  port to listen on                            (default 16384)
  -s  maximum size of one uploaded/written file    (default 16M, 0 = no limit)
  -n  maximum number of files in incoming/         (default 256, 0 = no limit)
  -q  maximum total bytes in incoming/             (default 1G,  0 = no limit)
      --no-incoming     serve pub/ only; reject all writes
      --serve-root      serve <root> itself read-only; no drop box
      --serve-root-rw   serve <root> itself read/write (create + overwrite)
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

Started as root, the daemon chroots to `<root>` and drops to `tnfs` after
opening its directory fds — which is why the unit must not set `User=`.
On Linux it then applies a Landlock ruleset — read under `pub`, create under
`incoming`, nothing else — which needs no privilege and so applies whatever
uid it was started as. Both are defence in depth: confinement of the served
namespace comes from the dirfd walk, and the daemon runs unchanged on a
kernel without Landlock.

## Serving a directory directly

The two-zone layout is the point of this daemon, but it is awkward for the
throwaway case: pointing a server at a directory that already exists — a source
repo, a folder of disk images — just to pull files off it. There is no `pub/`
there, and making one defeats the purpose. Two flags collapse the namespace to
a single zone rooted at `<root>` itself:

```
de-tnfsd --serve-root     <dir>    # read-only:  grab files, no writes
de-tnfsd --serve-root-rw  <dir>    # read/write: also create and overwrite files
```

In both modes the whole of `<dir>` is served as one zone: it is listed and
walked like any directory, no `pub/` or `incoming/` is needed (any that happen
to exist are just ordinary entries), the drop box is off, and only `/` is
mountable. `--serve-root` is read-only. `--serve-root-rw` additionally lets a
client create a new file and **overwrite** an existing one — deliberately, and
only when you ask for it; `-s` still caps a written file's size. Directory-shape
changes (`MKDIR`, `RMDIR`, `UNLINK`, `RENAME`, `CHMOD`) are refused in every
mode, so a client can rewrite a file's contents but never restructure the tree.

These are a convenience for a quick local server. The default two-zone layout —
with its guarantee that no path is ever both readable and writable — is
unaffected and remains the mode you deploy. The daemon prints which mode it is
in on its first log line.

## Permissions

The daemon is the only enforcement point. Set `incoming` to `0777` and a
client still cannot list it, stat a name in it, read a byte out of it,
overwrite anything in it, or delete anything from it: those operations are
refused by the capability table before any syscall is attempted. Making `pub`
world-writable does not make it writable over TNFS either.

`install.sh` creates `pub` `2775` and `incoming` `2770`, both owned
`tnfs:tnfs`. Those modes are chosen for the humans on the host rather than for
the policy: setgid, so that uploads inherit group `tnfs` for whoever drains
them and so `pub` stays group-readable as you extend it. Both are explained
under "Filling pub" and "Draining the drop box" below.

Stricter modes work just as well — including the classic anonymous-ftpd `0755`
on `pub` and `0733` on `incoming`, which costs only the group drain, since
group members can then no longer read what they are draining. Nothing here
depends on any of it, and the drop-box test suite runs with the directory
world-writable to keep that honest.

The daemon needs exactly two things from the filesystem: it must be able to
read `pub`, and create files in `incoming`. It checks both by attempting them
at startup and refuses to start if either fails.

## Filling pub

The startup check covers `pub` itself, not what you later put in it, and this
is the one place where the filesystem can make the daemon look broken. The
daemon serves `pub` as its own uid, so a file it cannot open is *listed* and
then fails on read with `EACCES` — the client sees a name it cannot fetch,
which reads as a server bug rather than a permissions problem. `READ` is the
only capability involved; the daemon never needs write, so who owns the file
does not matter, only whether `tnfs` can read it.

The arrangement that makes this automatic is a setgid `pub` owned by the
daemon's group:

```
chown -R tnfs:tnfs /srv/tnfs/pub
find /srv/tnfs/pub -type d -exec chmod 2775 {} +
find /srv/tnfs/pub -type f -exec chmod 0664 {} +
usermod -aG tnfs <operator>
```

Setgid matters more here than on the flat drop box, because `pub` is
recursive: it makes each subdirectory you create inherit group `tnfs` instead
of your own primary group, so the whole tree stays readable as you extend it.
Content can then be owned by whoever maintains it — a `patrick:tnfs 2775`
subdirectory under a `tnfs:tnfs 2775` `pub` serves fine.

What is left is your umask, which decides the mode of every file you add.
`002` or `022` both work; `077` produces `0600` files that the daemon cannot
read, and the symptom is the listed-but-unfetchable file above. Most
distributions that give each user a private group already default to `002`
(via `pam_umask` and `USERGROUPS_ENAB`), so usually there is nothing to do —
but it is worth checking with `umask` before blaming the daemon.
`chmod -R g+rX /srv/tnfs/pub` repairs a tree that was populated under a
restrictive one.

## Draining the drop box

Out of scope for the daemon, and it should run as a different uid. The
daemon's job ends at "the file is on disk"; anything that inspects, scans,
moves, or publishes uploads is a separate process with its own privileges.

Uploads are created `0660`, owned by the daemon's user and group (`tnfs`), so
the intended arrangement is a `2770` drop box and a human or a drain job in
group `tnfs`:

```
chown tnfs:tnfs /srv/tnfs/incoming
chmod 2770      /srv/tnfs/incoming   # setgid: uploads inherit group tnfs
usermod -aG tnfs <operator>
```

That group can read, edit, and move what landed without `sudo`. None of it is
load-bearing for the policy — it only decides who on the host can drain the
directory, never what a TNFS client can do, which stays exactly "create a file
that does not exist yet".

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
python3 tests/test_serveroot.py   bin/de-tnfsd   # --serve-root / --serve-root-rw
```
