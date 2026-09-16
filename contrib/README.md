# contrib/

Optional, community-contributed helpers that sit *alongside* de-tnfsd rather
than being part of the daemon. Nothing here is built by `make`, exercised by
`make check`, or required to run the server — the daemon behaves identically
whether or not any of this is installed.

## Support status

These scripts are provided as-is and are not covered by the daemon's test
suites or its stability guarantees. Several of them depend on de-tnfsd
*internal* behavior that the daemon is free to change (temp-file naming,
upload finalization, the fixed `pub`/`incoming` zone names). When that
behavior changes, a contrib script can break silently. Read a script before
installing it, and re-check it after upgrading the daemon.

## What belongs here

Operator-side tooling that composes with the served namespace from outside
the daemon: drain/review workflows, on-disk curation, monitoring. Anything
that would change the daemon's enforcement, add a zone, or relax the
read/write disjointness invariant does **not** belong here — that is a
daemon change and a `DESIGN.md` discussion.

## Layout

Each tool lives in its own subdirectory with its own README:

| Directory | Purpose |
|---|---|
| `tnfs-case-fix/` | Drain finished uploads out of `incoming/` into a review dir, and uppercase-normalize names on disk for uppercase-only clients (CP/M). |

## Installing

Nothing here installs itself. Each subdirectory's README lists the files,
where they go, and the privileges the cron/systemd unit runs with. Review
those privileges before enabling anything — most of this runs as `root`.
