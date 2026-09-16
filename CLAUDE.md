# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`de-tnfsd` is a TNFS daemon whose served namespace is split into a read-only
zone (`<root>/pub`) and a write-only drop box (`<root>/incoming`), with no path
that is ever both. `DESIGN.md` is the specification and is normative — read it
before changing behavior; its section numbers (§1–§9) are cited throughout the
code and this file.

**This is a clean-room implementation.** The predecessor daemon lives at
`../tnfsd` (a separate git repo, not a submodule). The *only* file in it that
may be consulted is the protocol specification, `../tnfsd/tnfs-protocol.md`.
Do not read, copy, or port its C sources. `DESIGN.md` names some of them
(`datagram.c`, `auth.c`, `tnfs_file.c`, `directory.c`, `traverse.c`) purely to
describe behavior *not* to reproduce; those descriptions are sufficient and the
files themselves are off limits.

## Build and test

```
make                    # -> bin/de-tnfsd
make check              # build, then run all six suites
make debug              # rebuild with -fsanitize=address,undefined
make clean
```

Tests are protocol-level Python driven against a real daemon over both
transports — no test framework, no fixtures. Each is a standalone script
taking the daemon path as `argv[1]`, so a single suite runs on its own:

```
python3 tests/test_confinement.py bin/de-tnfsd
python3 tests/test_readonly.py    bin/de-tnfsd
python3 tests/test_dropbox.py     bin/de-tnfsd
python3 tests/test_serveroot.py   bin/de-tnfsd
python3 tests/test_ignorecase.py  bin/de-tnfsd
python3 tests/test_tcpchurn.py    bin/de-tnfsd
```

`test_ignorecase.py` probes whether the host filesystem is case-sensitive and
skips the checks that a case-insensitive host (macOS, Windows) cannot exhibit —
so its full set only runs on a case-sensitive filesystem such as Linux CI.

`tests/tnfslib.py` is the shared client: it encodes the wire format directly
from the protocol document rather than reusing anything from the daemon, so an
encoding mistake fails a test instead of cancelling out. Each suite spawns its
own daemon on an ephemeral port and prints one `ok`/`FAIL` line per check.
Each suite's `main()` takes a transport and is run twice, once over UDP and
once over TCP, so every check covers both; the handlers are shared, but the
framing, session teardown, and peer lookup are not.

Run `make debug` and the suites at least once before calling a change done;
the sanitizers catch what the protocol-level checks cannot see.

## The invariant everything else protects

> No zone has both READ and CREATE. The set of readable paths and the set of
> writable paths are disjoint.

This holds in the **default** two-zone server, and it is what the code is built
around. The two opt-in serve-root modes (`--serve-root`, `--serve-root-rw`,
DESIGN.md §2) relax it on purpose and only when asked: they collapse the
namespace to the single zone `<root>`. `--serve-root` is read-only (still
disjoint, trivially); `--serve-root-rw` is the one configuration where a zone
holds both READ and CREATE, deliberately. Guard against *accidentally*
reintroducing a read-write zone in the default server; do not "fix" the
serve-root modes back out.

Zone names are fixed and deliberately not configurable. In the default server
the root is synthetic: it lists exactly `pub` and `incoming` whatever else is
on disk, and any other first component is `ENOENT`. In a serve-root mode the
root is a real zone walked and listed like any directory, `srv.serve_mode`
carries which mode is active, and only `/` is mountable.

`DESIGN.md` §5 enumerates the twelve channels by which the drop box could leak
and what closes each; it is the spec for `tests/test_dropbox.py`, which labels
its checks by row number. Check any proposed change against that table.

## Layout

| File | Role |
|---|---|
| `src/tnfs.h` | Wire protocol constants, derived only from `tnfs-protocol.md` |
| `src/zone.[ch]` | Zones, the capability table, and `path_resolve()` — the single authorization point |
| `src/handlers.c` | One handler per command; asserts a capability, then acts via `*at()` |
| `src/session.[ch]` | Sessions, dir/file handle tables, upload finalization |
| `src/dir.[ch]` | Directory snapshots, sorting, wildcard filtering |
| `src/dropbox.[ch]` | Quota accounting, temp-file sweep, per-IP leaky buckets |
| `src/net.c` | UDP/TCP sockets and the poll loop |
| `src/main.c` | Args, startup layout checks, privilege drop, `sd_notify` |
| `src/server.h` | The only file-scope state: config, startup dirfds, sockets, drop-box counters |

## Design rules that are easy to violate accidentally

- **The daemon is the sole enforcement point.** Mode bits, ownership, and the
  daemon's uid are never consulted for policy. A refusal must come from the
  capability check *before* any syscall — never from letting the kernel return
  `EACCES`. `test_dropbox.py` runs with `incoming` at `0777` to catch a refusal
  that has drifted into relying on the kernel.
- **One authorization point.** Every path-bearing handler goes through
  `path_resolve()` and then asserts the capability it needs. Do not add a
  second permission check elsewhere.
- **No path strings.** `path_resolve()` splits into components, rejects
  `..`/`.`/interior-empty, and walks from a startup dirfd with
  `openat(..., O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)`. Never build a path with
  `strcat`, never `realpath`, never `strstr(name, "..")`. The `-i`
  (`srv.ignore_case`) fallback stays inside this: on an exact `ENOENT` it
  `readdir`s the directory the walk already holds and retries the same
  `*at(dirfd, entry, O_NOFOLLOW)` against a real entry name — never a
  constructed path. It resolves *existing* names only, so a create keeps the
  requested spelling and the drop box (which resolves before the fold) stays
  case-sensitive; do not extend it to create targets.
- **Capabilities live on the fd.** `OPEN` records the capability set in the
  file slot; `READ`/`WRITE`/`LSEEK` check the fd's caps and never re-examine a
  path.
- **No global scratch state.** Per-request state is on the stack or in the
  session. `struct server srv` holds config, dirfds, sockets, and drop-box
  counters, and nothing else belongs there.
- **Refusals in `incoming` are uniformly `EACCES`** — name taken, quota met,
  count met, read attempted, stat attempted. The documented exceptions are
  `EINVAL` for a bad upload *name* (a property of the request, not the
  directory) and `EFBIG` on a write past `-s`/`-q` (which describes the
  client's own file).
- **Uploads finalize by `linkat`, not `renameat`.** `renameat` would silently
  replace a file that appeared under the target name mid-upload, and
  overwriting is the one thing this zone must never do; `linkat` fails with
  `EEXIST` instead, and the temp file is then unlinked.
- **Size limits are checked against apparent size** (highest offset written +
  length), so a seek cannot buy extra room.
- **POSIX only.** No `WIN32` ifdefs, no portability layer, no backend seam.
  This is a hard constraint (§3), not a default. The one `__linux__` ifdef is
  `confine()` in `main.c` — the Landlock ruleset of §7 step 3. It is a
  hardening step, not a backend: it compiles away to `return 0`, and the
  daemon behaves identically without it.

## Deliberately absent — do not reintroduce

Read-write directories in the default two-zone server (the opt-in
`--serve-root-rw` mode is the one sanctioned exception, and it still refuses
every directory-shape mutation); working `UNLINK`/`RENAME`/`CHMOD`/`MKDIR`/
`RMDIR` (refused in every zone and every mode, including `--serve-root-rw`,
which allows file create/overwrite but no tree restructuring); arbitrary mount
subdirectories (only `/`, `/pub`, `/incoming` by default, only `/` in a
serve-root mode); `.ignore` files; users, passwords, per-user areas; Windows
support; Atari ATR virtualization; and `OPENDIRX`'s recursive traverse — its
flag is accepted and ignored, yielding a normal single-level listing rather
than an error.

## Interpretations the spec left open

Recorded here because they are choices, not deductions, and a future change
should not silently reverse them:

- `OPENDIRX` reports its match count as 2 bytes, per the prose ("Bytes 6-7");
  the worked example in the document appears to show 4.
- `OPENFILE_OLD` (0x20) has no documented layout, so it is parsed exactly like
  `OPEN` (0x29). Misreading a client's intent here can only change *which*
  refusal it gets, never whether a write is allowed.
- TNFS has no length prefix, so the TCP transport treats one `recv` as one
  datagram; a request split across segments is not reassembled.
- Symlinks are omitted from listings entirely, since the daemon will not follow
  one and offering the name would only advertise something unreadable.
