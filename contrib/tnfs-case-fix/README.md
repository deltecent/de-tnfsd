# tnfs-case-fix.sh

For deployments that need uppercase file names, like CP/M.

Many 8-bit clients send every path in uppercase. CP/M's command processor
converts a whole command line to uppercase before any program sees it. On
a case-sensitive filesystem, a file added with a lowercase name (`scp`,
`rsync`, a tarball) is unreachable to that client. This script renames such
files to uppercase under `pub/` and `for-review/`, on a schedule, so they
become reachable again.

The same script also drains `incoming/` into `for-review/` as its first
step, needed before there is anything under `for-review/` to rename. See
the main `README.md`'s "Draining the drop box" section for what that does,
why it is safe, and how to set up `for-review/`. This doc only covers the
uppercase-rename half.

## What the rename step does

Renames any file or directory under `pub/` and `for-review/` that is not
already uppercase. Never touches `pub/`/`incoming/` themselves (a rename
there would break the daemon's zone-name matching), and never touches a
dot-prefixed name (an in-flight upload's temp name). Never overwrites: if
both `readme.txt` and `README.TXT` already exist in the same folder, both
are left alone and the collision is logged.

The script runs as root, on a cron schedule, and logs every action
(`MOVED`/`RENAMED`/`SKIP ...`) to `/var/log/tnfs-case-fix.log`.

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

No user or group setup is needed. This install never touches an existing
account's group membership.
