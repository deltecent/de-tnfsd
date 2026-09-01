# de-tnfsd: two zones, no read-write anywhere

Status: design sketch, not yet implemented.

This describes a replacement TNFS daemon built around one idea: the served
namespace is split into a read-only zone and a write-only zone, and **no path
is ever both**. The classic anonymous-ftpd layout:

```
<root>/
    pub/         anonymous read-only, recursive
    incoming/    anonymous drop box: create new files only.
                 Cannot be listed, read, stat'ed, overwritten, or deleted.
```

Nothing else is served. There are no read-write directories, no per-user
areas, and no authentication.

It is written in C, as the current daemon is, and runs either straight from a
shell or under systemd with no change in behavior or arguments (§7).

The daemon is the sole authority on what a client may do. Access is decided by
which zone a path is in, and by nothing else — not by the mode bits on the
directory, not by the uid the daemon runs as. A `chmod 777 incoming` changes
nothing a client can observe, because the daemon never offers a listing or a
read of that zone in the first place.

The wire protocol is unchanged, so existing FujiNet and 8-bit clients work
without modification. What changes is the server's internal shape: the current
daemon decides permissions in four unrelated places (a command-byte gate in
`datagram.c`, an open-flags check in `auth.c`, path-string checks scattered
through `tnfs_file.c`, and a silent fall-back-to-root in `directory.c`), which
is why adding a write-only directory to it touches so much. Here, every
request passes through one resolve-and-authorize step, and the two-zone policy
is three lines of a table.


## 1. Namespace

`pub` and `incoming` are fixed names directly under the server root. They are
not configurable, because configurability is what turns "these two zones can
never overlap" from an invariant into a thing you have to check.

The root directory itself is **synthetic**: a listing of `/` returns exactly
two entries, `pub` and `incoming`, regardless of what else happens to exist on
disk under root. Any path whose first component is neither of those resolves
to `ENOENT`. This means a stray file dropped in the root by an admin is not
accidentally served, and it removes the root directory as a place where the
two zones could interact.

`incoming` is flat. There are no subdirectories in it, and `MKDIR` is refused
everywhere. A flat drop box is much easier to reason about: there is exactly
one directory in the system that the daemon can write to, and it has no
children that could inherit or diverge from its policy.


## 2. The permission model

Six capabilities, assigned per zone. This is the whole policy:

| Zone        | LOOKUP | LIST      | READ | CREATE | MODIFY | REMOVE |
|-------------|--------|-----------|------|--------|--------|--------|
| `/`         | yes    | synthetic | no   | no     | no     | no     |
| `/pub`      | yes    | yes       | yes  | no     | no     | no     |
| `/incoming` | no     | no        | no   | yes    | no     | no     |

- **LOOKUP** — may learn whether a name exists (`STAT`, and any error that
  distinguishes "no such file" from "not permitted").
- **LIST** — may enumerate names.
- **READ** — may open for reading and read bytes.
- **CREATE** — may create a file that does not exist. Does not imply the
  ability to see that it worked beyond the status code.
- **MODIFY** — overwrite, truncate, append, rename, chmod.
- **REMOVE** — unlink, rmdir.

MODIFY and REMOVE are unset in every row. They are kept in the table so that
the code has a real capability to check rather than a hardcoded `return
EROFS`, and so a future variant (a moderated area, a per-device scratch zone)
has somewhere to attach without reopening the design.

The invariant worth stating out loud, because everything else exists to
protect it:

> No zone has both READ and CREATE. The set of readable paths and the set of
> writable paths are disjoint, and the daemon refuses to start if the
> filesystem layout does not guarantee that.

Startup checks that enforce it: `pub` and `incoming` must both exist, must be
real directories (not symlinks), must be on the same mount as root, and must
not be the same directory (compared by `st_dev`/`st_ino`, not by name).

### The daemon is the only enforcement point

Filesystem permissions do not implement any part of this policy, and the
design does not assume anything about them. A client is in `pub` or it is in
`incoming`; the capability table above is consulted, and the answer does not
depend on mode bits, ownership, ACLs, or which uid the daemon happens to be
running as.

The concrete claim: set `incoming` to `0777` and a client still cannot list
it, stat a name in it, read a byte out of it, overwrite anything in it, or
delete anything from it — because those operations are refused before any
syscall is attempted. `OPENDIR` on that zone returns `EACCES` from the
capability check; there is no code path in which the daemon calls `opendir()`
on the drop box and lets the kernel decide. The same is true in the other
direction: making `pub` world-writable on disk does not make it writable over
TNFS, because the zone has no CREATE capability to grant.

This is a deliberate departure from how the classic ftpd drop box worked,
where a `0733` directory was doing real enforcement work. Relying on mode bits
means the policy is only as good as the deployment, it breaks silently when
someone fixes "wrong-looking" permissions or restores from a backup that
flattens them, and it cannot express the distinctions this design needs
anyway — Unix has no mode that means "create but never overwrite". Putting
every decision in one capability table means the policy is testable over the
wire (§5) and is the same on every host.

An operator is still free to set restrictive modes, and it costs nothing to do
so. The design simply does not count it as a control: nothing in §5 is closed
by a mode bit, and the test suite runs with the drop box world-writable
precisely to prove the daemon is not leaning on the filesystem.


## 3. Architecture: resolve, authorize, act

Every request handler that names a path calls one function:

```
        client path ("foo/bar.dsk")
                 |
                 v
    +------------------------------+
    |  resolve(session, path)      |   component split, no "..", no ".",
    |                              |   no empty or separator-bearing
    |                              |   components, length caps
    +------------------------------+
                 |
                 v
    +------------------------------+
    |  walk from root dirfd with   |   openat(dirfd, comp,
    |  openat(..., O_NOFOLLOW)     |   O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)
    +------------------------------+
                 |
                 v
       resolved { zone, dirfd, leaf, caps }
                 |
                 v
    +------------------------------+
    |  handler asserts the cap it  |   caps & CAP_READ, caps & CAP_CREATE...
    |  needs, then acts via *at()  |
    +------------------------------+
```

Two properties fall out of this that the current daemon has to work for:

**Confinement is structural.** The walk starts at a directory fd opened once
at startup and descends one component at a time with `O_NOFOLLOW`. There is no
string concatenation, no `normalize_path`, no `strstr(name, "..")`, and no
`realpath` prefix comparison. A `..` component is rejected before the walk; a
symlink component fails the `openat` outright. There is no window between
checking a path and using it, so the symlink-swap race that string-based
confinement is prone to does not exist.

**The zone is known before the walk finishes.** The first component selects
the zone and therefore the capability set. A request for `incoming/anything`
is authorized (or refused) without the daemon needing to know whether the file
exists — which is exactly the property that keeps the drop box opaque.

Handlers act through `openat`, `fstatat`, `unlinkat` and friends relative to
the resolved directory fd. No handler ever builds an absolute path string.

### Per-fd capabilities

`OPEN` records the resolved capability set in the file-descriptor slot
alongside the fd itself. `READ`, `WRITE` and `SEEK` check the *fd's* caps and
never look at a path again. This is the piece the current design is missing:
its read-only gate fires in `tnfs_decode()` before the session or path is
known, so it can only make decisions on the command byte, which is why `-r`
and a writable directory cannot coexist there.

Concretely, a descriptor opened in `incoming` carries `CAP_CREATE` only, so a
`READBLOCK` against it is refused by the daemon *and* would fail in the kernel
anyway (the underlying fd is `O_WRONLY`). Two independent gates, same as the
existing read-only mode's belt-and-braces arrangement.

### No global scratch state

The current implementation keeps request state in file-scope buffers
(`fnbuf`, `dirbuf`, `iobuf`, and the `_path` / `_pattern` / `_list_files`
globals in `traverse.c`). That is safe only because the server is
single-threaded, and it is a standing hazard for any future change. In the new
daemon, per-request state lives on the stack or in the session; the only
file-scope state is the config and the listening sockets.

### Language and platform

C, as now — C11, no external dependencies beyond libc, building with the same
kind of plain makefile the current tree uses. The reasons are the ones that
applied to the original: the daemon has to be trivially buildable on whatever
a hobbyist has, it does nothing that would benefit from a runtime, and the
existing code, tests, and packaging are already C-shaped. The safety argument
for another language is real but is answered here by structure rather than by
the language — `openat` walks instead of path arithmetic, per-request state
instead of global buffers, one authorization point instead of four — which is
most of what the memory-safety fixes on the current branch were doing by hand.

**POSIX only. Windows is not supported and will not be.** The resolver is
built directly on `openat` with `O_NOFOLLOW`, `fstatat`, `renameat` and
`unlinkat` — POSIX.1-2008, present on Linux, the BSDs, and macOS. Windows has
no equivalent, and the only way to support it would be to reimplement
confinement on canonicalized path strings, which is the weaker technique this
whole design exists to get away from.

So there is no portability layer and no backend seam: the `*at()` calls are
used inline, and the `WIN32` ifdefs that thread through the current tree are
gone. Anyone who wants a Windows build can write one against a different
design; this one is not going to be compromised to leave room for it.


## 4. Behavior, command by command

`pub` behaves like a normal read-only TNFS export. The interesting column is
`incoming`.

### Session

| Command | Behavior |
|---|---|
| `MOUNT` (0x00) | Mount location may be `/`, empty, `/pub`, or `/incoming`; anything else → `ENOENT`. The mount point is resolved to a zone once and stored in the session, so a client that mounts `/pub` directly gets the same capabilities as one that mounts `/` and prefixes its paths. User id and password fields are accepted and ignored — access is anonymous. |
| `UMOUNT` (0x01) | Closes the session, closes its fds and dir handles. |

### Directory

| Command | `/` | `/pub` | `/incoming` |
|---|---|---|---|
| `OPENDIR` (0x10) | synthetic handle listing `pub`, `incoming` | real handle | **`EACCES`** |
| `OPENDIRX` (0x17) | as above, options ignored | real handle | **`EACCES`** |
| `READDIR` (0x11) | from handle | from handle | unreachable |
| `READDIRX` (0x18) | from handle | from handle | unreachable |
| `TELLDIR` / `SEEKDIR` / `CLOSEDIR` | handle ops, no path | same | unreachable |
| `MKDIR` (0x13) | `EROFS` | `EROFS` | `EACCES` |
| `RMDIR` (0x14) | `EROFS` | `EROFS` | `EACCES` |

`OPENDIR` on `incoming` returns `EACCES` rather than `ENOENT`. The directory's
*existence* is not secret — clients have to know where to upload — only its
contents are. Returning `ENOENT` would also make a well-behaved client believe
its upload target is missing.

The current server's fall-back behavior here must not be carried over: today,
`tnfs_opendir()` responds to a path that fails validation by silently
substituting the server root and returning a valid handle. Every refusal in
the new daemon is an error status; there is no path by which a denied request
quietly succeeds against a different directory.

`OPENDIRX`'s recursive traverse option (`TNFS_DIROPT_TRAVERSE`) is **not
implemented**. A handle lists one directory; a client that wants a subtree
opens each directory in turn. The flag is accepted and ignored rather than
rejected, so a client that sets it gets a normal single-level listing instead
of an error.

Dropping it removes the single largest piece of machinery in the current
server — the `nftw` walk, its file-scope state, the glob matcher, and the sort
— along with an unbounded in-memory result list that one client could inflate
by traversing a large `pub`. It is also the feature that, in the current code,
walks the real filesystem from the root and would happily descend into a drop
box and return its filenames.

### File

| Command | `/pub` | `/incoming` |
|---|---|---|
| `OPEN` (0x29), `OPENFILE_OLD` (0x20) | `O_RDONLY` only; any of `O_WRONLY`/`O_RDWR`/`O_APPEND`/`O_CREAT`/`O_TRUNC`/`O_EXCL` → `EROFS` | see below |
| `READ` (0x21) | yes | fd has no `CAP_READ` → `EACCES` |
| `WRITE` (0x22) | fd has no write cap → `EROFS` | yes |
| `SEEK` (0x25) | yes | yes, within the file being written |
| `CLOSE` (0x23) | yes | yes — see "finalization" below |
| `STAT` (0x24) | full stat | **`EACCES`** for any name inside; the directory itself stats as a directory |
| `UNLINK` (0x26) | `EROFS` | `EACCES` |
| `CHMOD` (0x27) | `EROFS` | `EACCES` |
| `RENAME` (0x28) | `EROFS` | `EACCES` |

`RENAME` is refused unconditionally, in both directions, even between two
paths in the same zone. A rename that could move a file from `incoming` to
`pub` would be a read primitive for the drop box; rather than reason about
which combinations are safe, the daemon does not implement the operation.

**`OPEN` in `incoming`** is the one place the daemon writes anything:

- Access mode must be `O_WRONLY`. `O_RDWR` is silently downgraded to
  `O_WRONLY` rather than refused, because several clients request `O_RDWR`
  by default and the downgrade costs nothing. `O_RDONLY` → `EACCES`.
- `O_CREAT` is required. `O_EXCL` is forced on regardless of what the client
  asked for; this is what makes overwrite impossible.
- `O_TRUNC` and `O_APPEND` are refused — both only make sense against a file
  that already exists.
- `O_NOFOLLOW` is forced on, so a symlink planted in the drop box by some
  other means cannot be used to redirect a write.
- The client's mode argument is ignored. Files are created `0600` as a
  sane default, not as a control — no part of the policy depends on it.
- The leaf name is validated: no leading dot, no path separators, no control
  characters, length capped, and a conservative charset. A rejected name gets
  `EINVAL`.
- If the name already exists, the upload is **rejected** — no auto-suffixing,
  no server-assigned name. The reply is `EACCES` rather than `EEXIST`, which
  keeps every refusal in the zone indistinguishable and denies an attacker an
  existence oracle. That indistinguishability is cheap rather than critical:
  the property the design actually rests on is that an uploaded file can never
  be read back, and knowing a name is taken is not much of a prize. It costs
  nothing to withhold, so it is withheld.

`SEEK` on a drop-box descriptor is allowed. The client can only rewrite bytes
of the file it created moments ago in the same session, which tells it
nothing it did not already know. The one wrinkle is that seek-then-write
creates sparse files, so the size cap in §6 is enforced against the file's
apparent size, not the bytes actually transferred.

**Finalization.** A file is created under a unique temporary name
(`.tmp-<random>`), and `CLOSE` renames it to the client's requested name with
`renameat`. Two things fall out: a partially uploaded file never appears
complete to whatever drains the directory, and a session that dies mid-upload
leaves a dotfile that a janitor can sweep on age. If the target name was taken
in the meantime, the rename fails and the temp file is unlinked — the client
sees `EACCES`, consistent with every other refusal in that zone.

### Device

| Command | Behavior |
|---|---|
| `SIZE` (0x30), `SIZEBYTES` (0x32) | size of the filesystem holding `incoming` |
| `FREE` (0x31), `FREEBYTES` (0x33) | free space, **capped at the remaining drop-box quota** |

Clients legitimately need free space to decide whether an upload will fit.
Reporting the quota remainder rather than the raw filesystem figure both
answers that question more accurately and avoids handing out a running total
of everything the volume holds.


## 5. What could leak the drop box

The design goal is a directory that anonymous clients can write into and learn
nothing about. Each row is a channel, and what closes it. This list is the
spec for `tests/test_dropbox.py`.

| # | Channel | Closed by |
|---|---|---|
| 1 | `OPENDIR` / `OPENDIRX` on `incoming` | `EACCES`; zone has no LIST |
| 2 | Recursive traverse from `/` or `/pub` reaching into `incoming` | no recursive traverse exists; a handle lists one directory (§4) |
| 3 | `STAT` on a name inside `incoming` | `EACCES`; zone has no LOOKUP |
| 4 | `OPEN` for read | zone has no READ; fd would be `O_WRONLY` regardless |
| 5 | `O_EXCL` returning `EEXIST` on a name that exists | all refusals in the zone return `EACCES` |
| 6 | Distinguishable errors — `ENOSPC` vs `EEXIST` vs `EINVAL` | quota and collision refusals both return `EACCES`; only name-syntax rejection differs, and that is a property of the request, not the directory |
| 7 | `RENAME` out of `incoming` into `pub` | `RENAME` unimplemented in every direction |
| 8 | Symlink under `pub` pointing at `incoming` | `O_NOFOLLOW` on every walk component |
| 9 | Symlink planted inside `incoming` used as a write redirect | `O_NOFOLLOW` on create; the daemon never opens an existing name in this zone |
| 10 | `FREE` deltas as a coarse occupancy oracle | acknowledged and accepted; capped reporting bounds the signal to the quota, not the volume |
| 11 | Session hijack — reusing another client's session id | session is bound to source IP, as today; note this is weak against a spoofing attacker on UDP and is not a security boundary |
| 12 | Permissive modes on disk — operator sets `incoming` to `0777` | nothing changes: every refusal above is a capability check, taken before any syscall (§2) |

Row 12 is the one to keep honest as the code grows. The test suite runs with
the drop box world-writable, so any refusal that has quietly come to depend on
the kernel rather than on the capability table shows up as a failure rather
than as a deployment that happens to be tight enough.


## 6. Standing up to abuse

A publicly writable endpoint is a standing invitation. None of this is
optional:

- **Drop-box limits** — maximum file size, maximum file count, and total
  bytes. Specified below, since they are the ones an operator actually tunes.
- **Concurrency caps**: open-for-write descriptors per session and across the
  whole server.
- **Rate limiting per source IP**: uploads per minute and bytes per minute,
  with a small leaky bucket. Mostly this exists so a single client cannot fill
  the quota before an operator notices.
- **Idle timeouts** on sessions with an open write fd, so a stalled upload
  releases its slot and its temp file.
- **Every upload logged**: source IP, requested name, final size, duration,
  outcome. This is the only visibility an operator has into the directory,
  since the daemon deliberately cannot list it.

### Drop-box limits

Three independent limits, each with a flag (§8) and each disabled by setting
it to `0`:

| Limit | Flag | Default | Applies to |
|---|---|---|---|
| Maximum file size | `-s` | 16M | one uploaded file |
| Maximum file count | `-n` | 256 | files in `incoming` |
| Maximum total bytes | `-q` | 1G | sum of file sizes in `incoming` |

`-n` is the one that answers "how many files may sit in the drop box", `-s`
bounds any single upload, and `-q` exists because `-n` alone does not bound
disk use — 256 files at 16M each is 4G.

**File count and total bytes are checked at `OPEN`.** If either limit is
already met, the open is refused with `EACCES` — the same status as every
other refusal in the zone (§5, row 6), so a client cannot distinguish "the
drop box is full" from "that name is taken". In-flight uploads count toward
both limits while they are open, using the requested-so-far size for bytes, so
a burst of simultaneous opens cannot collectively overshoot.

**File size is checked on every write**, against the file's *apparent* size —
the highest offset written plus the length, not the number of bytes
transferred — so a client cannot seek to 4G and write one byte. A write that
would cross `-s`, or that would push the directory past `-q`, fails with
`EFBIG` and aborts the upload: the temp file is unlinked immediately, the
descriptor is marked dead, and `CLOSE` reports `EFBIG` rather than renaming
anything into place. `EFBIG` is deliberately distinguishable here, unlike the
refusals at `OPEN` — it describes the client's own file, not the contents of
the directory, so it leaks nothing and telling the client its upload was too
big is far more useful than a generic error.

`FREE` and `FREEBYTES` report whichever is smaller, actual filesystem free
space or the remaining `-q` allowance (§4), so a client that checks before
uploading gets an answer consistent with what the limits will actually permit.

**Keeping count.** The daemon may read `incoming` even though clients may not
— the capability table governs client requests, not the daemon's own
housekeeping, and this is a place where that distinction earns its keep. At
startup it scans the directory once to establish the file count and byte
total, then maintains both incrementally as uploads finalize.

The wrinkle is that whatever drains the drop box removes files
behind the daemon's back, so the in-memory counters drift upward from reality
and the drop box would eventually refuse uploads into a directory that is
actually empty. Two cheap corrections: rescan on a timer (60s is fine), and
always rescan before refusing an upload on a count or quota limit, so a
refusal is never based on a stale count. Both are bounded by `-n` — a
directory that can hold at most a few hundred files is trivial to rescan.

Temp files from aborted uploads (`.tmp-*`) count toward the limits while they
exist and are swept by age at each rescan, so a client that opens uploads and
disappears cannot wedge the drop box permanently.

**Draining the drop box is out of scope for the daemon and should run as a
different uid.** The daemon's job ends at "the file is on disk". Anything that
inspects, scans, moves, or publishes uploads is a separate process with its
own privileges — that separation is what keeps the daemon's own capability set
as small as it is.


## 7. Process model and deployment

Startup order matters, because it is what lets the daemon give up everything
afterwards:

1. Parse config, bind sockets.
2. `open()` the root, `pub`, and `incoming` directory fds; run the §2 layout
   checks against them.
3. Drop privileges and confine (the existing `chroot` path; on Linux, Landlock
   or a seccomp filter is a cheap addition here).
4. Serve. From this point the daemon never resolves an absolute path again —
   every operation is relative to a dirfd opened in step 2.

The daemon needs exactly two things from the filesystem: it must be able to
read `pub`, and it must be able to create files in `incoming`. Startup checks
both by attempting them, and refuses to start if either fails. Beyond that it
has no opinion about modes — see §2: the permission policy is the daemon's,
not the filesystem's, and a world-writable drop box changes nothing a client
can do.

An operator who wants tight modes anyway can have them; `0755` on `pub` and
`0733` on `incoming` (the classic ftpd arrangement) work fine and are a
reasonable habit. They are just not load-bearing here, and the daemon will not
warn about, complain about, or "fix" whatever it finds.

### Running from a shell

The default mode is a foreground process:

```
$ de-tnfsd /srv/tnfs
```

It does not fork, does not detach, does not write a pidfile, and does not
background itself. It logs to stderr, one line per event. `SIGINT` and
`SIGTERM` both start a clean shutdown: stop accepting, close open descriptors,
unlink any temp files from uploads that were still in flight, close sockets,
exit 0. A second signal exits immediately. Exit status is 0 for a clean
shutdown, non-zero with a specific message on stderr for a startup failure
(bad arguments, missing `pub` or `incoming`, port in use, cannot drop
privileges).

This is the same binary and the same arguments used under systemd. There is no
`-D`/`--daemon` flag and no separate service mode, because a foreground
process that logs to stderr is both what a supervisor wants and what someone
debugging on the console wants.

### Running under systemd

`Type=notify` if `sd_notify` is worth the ~20 lines to implement inline
(readiness after step 4, and `WATCHDOG=1` if a watchdog is configured);
`Type=simple` otherwise. Either way, no libsystemd dependency — the notify
protocol is a datagram to the socket named in `$NOTIFY_SOCKET`, and the
daemon should behave identically when that variable is unset, which is what
makes the shell case work unchanged.

```ini
[Unit]
Description=de-tnfsd TNFS Server
After=network-online.target

[Service]
Type=notify
ExecStart=/usr/local/sbin/de-tnfsd /srv/tnfs
Restart=on-failure

# The daemon drops privileges and confines itself after opening its
# directory fds. Do NOT add User= -- that would leave it without the
# privileges the confinement step needs.

ReadOnlyPaths=/srv/tnfs/pub
ReadWritePaths=/srv/tnfs/incoming
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
NoNewPrivileges=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX

[Install]
WantedBy=multi-user.target
```

`ReadWritePaths=` on the drop box pairs with `ProtectSystem=strict`; the unit
above ships correct, and the startup create-probe in step 2 fails loudly if
someone edits it into something that cannot work.

Socket activation is not planned. It saves nothing meaningful for a daemon
that binds two sockets at startup, and it complicates the step ordering above.


## 8. Configuration surface

```
de-tnfsd [-p <port>] [-s <max-file-size>] [-n <max-files>]
         [-q <max-total-bytes>] [--no-incoming] <root>

  -p  port to listen on                            (default 16384)
  -s  maximum size of one uploaded file            (default 16M, 0 = no limit)
  -n  maximum number of files in incoming/         (default 256, 0 = no limit)
  -q  maximum total bytes in incoming/             (default 1G,  0 = no limit)
      --no-incoming    serve pub/ only; reject all writes
```

Size arguments accept a `K`, `M`, or `G` suffix (powers of 1024); a bare
number is bytes. Out-of-range or unparseable values are a startup error, not a
silently clamped default. Semantics for all three are in §6.

That is the whole of it. There is no `-r`: read-only is the only mode `pub`
has. There is no `-i`: the drop box is `<root>/incoming` or it does not exist.
`--no-incoming` runs a pure read-only server, for hosts that should not accept
uploads at all — it is the only way to disable the drop box, since `-n 0`
means unlimited rather than zero files.

The daemon refuses to start rather than degrade: missing `pub`, missing
`incoming` (without `--no-incoming`), either one a symlink, the two resolving
to the same directory, or `incoming` not writable by the daemon's uid are all
fatal at startup with a specific message.


## 9. Compatibility, and what this drops

The wire protocol is untouched; version negotiation, session handling, and
every datagram layout stay as specified in `tnfs-protocol.md`. A stock FujiNet
client sees an ordinary TNFS server with two directories.

Carried over from the current implementation:

- The UDP/TCP event loop, session migration on TCP reconnect, and the
  session-id-plus-IP binding.
- `tests/test_confinement.py` and `tests/test_readonly.py` — both are
  protocol-level, so they run against the new daemon unmodified and make a
  decent conformance floor. `test_readonly.py` should pass verbatim against
  `pub`.

Deliberately dropped:

- Read-write directories of any kind.
- `UNLINK`, `RENAME`, `CHMOD`, `MKDIR`, `RMDIR` — all refused, all zones.
- Arbitrary per-session mount subdirectories; only `/`, `/pub`, `/incoming`.
- `.ignore` files. Client-visible filtering that the client can also turn off
  is not a security mechanism, and it is a second, weaker path-matching engine
  to maintain.
- Users, passwords, per-user areas.
- Windows support, and with it every `WIN32` ifdef in the tree (§3).
- Atari ATR virtualization (`-a`). `pub` serves the bytes that are on disk;
  a client that wants an ATR image reads a file that is one.
- `OPENDIRX`'s recursive traverse (§4). The flag is accepted and ignored.
