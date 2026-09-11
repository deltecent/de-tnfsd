#!/bin/bash
# For deployments that need uppercase file names, like CP/M.
#
# 1. Moves completed uploads out of incoming/ into for-review/, since
#    incoming/ has no LIST or READ capability by design -- nothing can read
#    its contents back out over TNFS (see DESIGN.md). for-review/ is a
#    plain directory that is not a TNFS zone, so it stays invisible to
#    every TNFS client: de-tnfsd only ever resolves a root-level path
#    against the two fixed zone names "pub"/"incoming" (zone_from_name()
#    in src/zone.c) and returns a hardcoded pub/incoming listing for the
#    root itself (dir_synthetic_root() in src/dir.c) -- any other
#    directory under the TNFS root, present or not, is never looked up
#    and never listed.
#
#    A non-dot regular file in incoming/ is safe to move: de-tnfsd finalizes
#    an upload with linkat() from its temp name to the final name before
#    removing the temp name (session_close_file() in src/session.c), so the
#    final name only ever appears once the file is complete.
#
# 2. Renames files/directories under pub/ and for-review/ to uppercase, so
#    8-bit clients (which always request uppercase paths) can reach content
#    added with lowercase names by other means (scp, rsync, tarballs, etc).
#
# Never touches $ROOT/pub or $ROOT/incoming themselves (-mindepth 2) --
# de-tnfsd matches those two zone names case-sensitively against fixed
# lowercase strings; renaming them breaks zone resolution outright.
# Never touches dotfiles (! -name '.*') -- de-tnfsd's in-flight uploads use
# a dot-prefixed temp name inside incoming/ before their final rename.

ROOT="/srv/tnfs"
INCOMING="$ROOT/incoming"
REVIEW="$ROOT/for-review"
LOG="/var/log/tnfs-case-fix.log"
OWNER="tnfs:tnfs"

# --- Step 1: sweep completed uploads from incoming/ into for-review/ -----
if [ -d "$INCOMING" ] && [ -d "$REVIEW" ]; then
    find "$INCOMING" -maxdepth 1 -type f ! -name '.*' | while IFS= read -r path; do
        base=$(basename "$path")
        target="$REVIEW/$base"
        if [ -e "$target" ]; then
            echo "$(date '+%F %T') SKIP MOVE (target exists): $path -> $target" >> "$LOG"
        else
            mv -n "$path" "$target" \
                && chown "$OWNER" "$target" \
                && chmod 0664 "$target" \
                && echo "$(date '+%F %T') MOVED: $path -> $target" >> "$LOG"
        fi
    done
fi

# --- Step 2: case-fix filenames under pub/ and for-review/ ---------------
# -depth: process each directory's contents before the directory itself,
# so a parent dir's own rename never invalidates paths already queued for
# its children.
find "$ROOT" -depth -mindepth 2 ! -path "$INCOMING/*" ! -name '.*' | while IFS= read -r path; do
    dir=$(dirname "$path")
    base=$(basename "$path")
    upper=$(echo "$base" | tr '[:lower:]' '[:upper:]')
    if [ "$base" != "$upper" ]; then
        target="$dir/$upper"
        if [ -e "$target" ]; then
            echo "$(date '+%F %T') SKIP (target exists): $path -> $target" >> "$LOG"
        else
            mv -n "$path" "$target" && echo "$(date '+%F %T') RENAMED: $path -> $target" >> "$LOG"
        fi
    fi
done
