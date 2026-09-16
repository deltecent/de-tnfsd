# tnfs-case-fix.sh

Drains `incoming/` into `for-review/` on a schedule. Also renames files to
uppercase under `pub/`/`for-review/`, as an on-disk alternative to the
daemon's `-i` flag.

## Draining incoming/

This script's main job is to move each finished upload out of `incoming/`
into `for-review/`. See the main `README.md`'s "Draining the drop box"
section for what that does, why it is safe, and how to set up
`for-review/`. This doc only covers the rename step below.

## The rename step: an alternative to `-i`

The daemon's `-i` flag (see "Case-insensitive names" in the main README)
folds case at lookup time. It changes no files on disk. Any casing of an
existing name then resolves.

This script's rename step does a similar job a different way. It changes
the file names once, on disk, instead of folding every lookup.

Use the rename step if you want `pub` to show uppercase names to a person
who browses it directly. Use it if you are not running `-i`. Use it if you
want the uppercase names on disk regardless of the flag.

Many 8-bit clients need one of the two. CP/M's command processor converts a
whole command line to uppercase before any program sees it. A file added
with a lowercase name (`scp`, `rsync`, a tarball) is then unreachable to
that client. This holds unless the server runs `-i`, or this script has
renamed the file.

## What the rename step does

Renames any file or directory under `pub/` and `for-review/` that is not
already uppercase. Never touches `pub/`/`incoming/` themselves (a rename
there would break the daemon's zone-name matching), and never touches a
dot-prefixed name (an in-flight upload's temp name). Never overwrites: if
both `readme.txt` and `README.TXT` already exist in the same folder, both
are left alone and the collision is logged.

The script runs as root, on a cron schedule, and logs every action
(`MOVED`/`RENAMED`/`SKIP ...`) to `/var/log/tnfs-case-fix.log`.

## Conflicts

Every name collision (an `incoming/` file that can't move because
`for-review/` already has that name, or a lowercase name that can't be
uppercased because the uppercase name already exists) is also recorded in
`for-review/CONFLICTS.log`. Unlike the main log, this file is rebuilt each
run to reflect only what is *currently* unresolved: a conflict keeps its
original first-seen timestamp for as long as it persists, and drops out on
its own the next run after it's resolved (rename/remove one side by hand).
Check it any time you're in `for-review/` reviewing uploads — an empty file
(just the header comment) means nothing is currently stuck.

## Install

1. Set up `for-review/` first, per the main `README.md`.

2. Copy the script and cron file:

   ```
   cp tnfs-case-fix.sh /usr/local/sbin/tnfs-case-fix.sh
   chmod +x /usr/local/sbin/tnfs-case-fix.sh
   cp tnfs-case-fix.cron /etc/cron.d/tnfs-case-fix
   ```

3. Edit `ROOT` at the top of the script if your TNFS root is not
   `/srv/tnfs`.

If `for-review/` does not exist yet, step 1 skips the `incoming/` sweep and
logs a `WARNING` line instead of silently doing nothing — check the log if
uploads are not draining.

No user or group setup is needed. This install never touches an existing
account's group membership.
