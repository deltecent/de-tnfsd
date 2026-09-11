# tnfs-case-fix.sh

For deployments that need uppercase file names, like CP/M.

Many 8-bit clients send every path in uppercase. CP/M's command processor
converts a whole command line to uppercase before any program sees it. On
a case-sensitive filesystem, a file added with a lowercase name (`scp`,
`rsync`, a tarball) is unreachable to that client. This script fixes that,
and also solves a second, related problem: how to review an upload before
it goes public.

## What it does

1. **Moves finished uploads out of `incoming/` into `for-review/`.**
   `incoming/` is a write-only drop box: no TNFS client can list, read, or
   delete what is in it. That is the whole point of the zone (see
   `DESIGN.md`), but it also means a human admin cannot look at an upload
   over TNFS to decide whether it belongs in `pub/`. This script moves each
   finished upload out to a plain directory, `for-review/`, that sits next
   to `pub/` and `incoming/` but is not a TNFS zone at all.

2. **Renames files under `pub/` and `for-review/` to uppercase**, so a
   client that only ever sends uppercase paths can reach them.

The script runs as root, on a cron schedule, and logs every action to
`/var/log/tnfs-case-fix.log`.

## Why `for-review/` is safe

`for-review/` is invisible to every TNFS client, by construction, not by a
permission trick:

- `zone_from_name()` (`src/zone.c`) only ever recognizes two names at the
  TNFS root: `pub` and `incoming`. A path resolution for any other name,
  `for-review` included, fails with `ENOENT` before the daemon opens
  anything on disk.
- `dir_synthetic_root()` (`src/dir.c`) answers a directory listing of the
  TNFS root with a fixed, hardcoded list containing only `pub` and
  `incoming`. It never reads the real root directory, so a listing can
  never reveal that `for-review/` exists.

So `for-review/` needs no extra access control of its own. It is a normal
directory that only a human with shell or SFTP access to the host can see.

## Why the move is safe

The script only moves a file out of `incoming/` once its name proves the
upload is finished. `session_close_file()` (`src/session.c`) finalizes an
upload with `linkat()` from a hidden temp name (`.tmp-<random>`) to the
final name, then removes the temp name. The final name never appears until
the file is complete. The script skips every dot-prefixed name, so it never
touches a temp file mid-upload.

## Install

1. Create the `for-review/` directory next to `pub/` and `incoming/`, owned
   the same way as `pub/`:

   ```
   mkdir /srv/tnfs/for-review
   chown tnfs:tnfs /srv/tnfs/for-review
   chmod 2775 /srv/tnfs/for-review
   ```

2. Copy the script and cron file:

   ```
   cp tnfs-case-fix.sh /usr/local/sbin/tnfs-case-fix.sh
   chmod +x /usr/local/sbin/tnfs-case-fix.sh
   cp tnfs-case-fix.cron /etc/cron.d/tnfs-case-fix
   ```

3. Edit `ROOT` at the top of the script if your TNFS root is not
   `/srv/tnfs`.

No user or group setup is needed. `2775` already gives every local user
`r-x` on `for-review/`, so any admin can read it without `sudo`. This
install never touches an existing account's group membership.

## Using it

Review `for-review/` whenever you like; no `sudo` is needed just to look.
`sudo mv` a file you approve into `pub/`; `sudo rm` anything you do not
want. Only `tnfs` itself has write access to `for-review/`, so moving or
deleting needs `sudo` -- a small, deliberate piece of friction on the one
action that changes what is public. The script does not touch `pub/` or
`for-review/` contents beyond the uppercase rename, and it never deletes
anything.
