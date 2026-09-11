#!/bin/bash
# For deployments that need uppercase file names, like CP/M. See the repo's
# main README.md ("Draining the drop box") for why step 1 is safe, and this
# directory's own README.md for step 2. Two hazards both steps must avoid:
#
# - Never rename $ROOT/pub or $ROOT/incoming themselves (-mindepth 2) --
#   de-tnfsd matches those two zone names case-sensitively against fixed
#   lowercase strings; renaming them breaks zone resolution outright.
# - Never touch a dotfile (! -name '.*') -- de-tnfsd's in-flight uploads use
#   a dot-prefixed temp name inside incoming/ before their final rename.

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
