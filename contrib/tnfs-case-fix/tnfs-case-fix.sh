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
#
# Any skipped move/rename (name collision) is recorded in $CONFLICTS, a
# small state file rebuilt each run: still-unresolved conflicts keep their
# original first-seen timestamp, resolved ones (collision cleared, or the
# source file is gone) drop out automatically, and new ones are added.

ROOT="/srv/tnfs"
INCOMING="$ROOT/incoming"
REVIEW="$ROOT/for-review"
LOG="/var/log/tnfs-case-fix.log"
CONFLICTS="$REVIEW/CONFLICTS.log"
OWNER="tnfs:tnfs"

declare -A prev_seen
declare -A cur_conflicts

if [ -f "$CONFLICTS" ]; then
    while IFS=$'\t' read -r ts path _desc; do
        case "$ts" in \#*) continue ;; esac
        [ -n "$path" ] && prev_seen["$path"]="$ts"
    done < "$CONFLICTS"
fi

record_conflict() {
    # $1 = unique key (the source path), $2 = human-readable description
    local key="$1" desc="$2" ts
    ts="${prev_seen[$key]:-$(date '+%F %T')}"
    cur_conflicts["$key"]="$ts"$'\t'"$key"$'\t'"$desc"
}

# --- Step 1: sweep completed uploads from incoming/ into for-review/ -----
if [ -d "$INCOMING" ] && [ -d "$REVIEW" ]; then
    while IFS= read -r path; do
        base=$(basename "$path")
        target="$REVIEW/$base"
        if [ -e "$target" ]; then
            echo "$(date '+%F %T') SKIP MOVE (target exists): $path -> $target" >> "$LOG"
            record_conflict "$path" "$path -> $target (target already exists in for-review/)"
        else
            mv -n "$path" "$target" \
                && chown "$OWNER" "$target" \
                && chmod 0664 "$target" \
                && echo "$(date '+%F %T') MOVED: $path -> $target" >> "$LOG"
        fi
    done < <(find "$INCOMING" -maxdepth 1 -type f ! -name '.*')
fi

# --- Step 2: case-fix filenames under pub/ and for-review/ ---------------
# -depth: process each directory's contents before the directory itself,
# so a parent dir's own rename never invalidates paths already queued for
# its children. CONFLICTS.log itself is excluded so it never gets renamed.
while IFS= read -r path; do
    dir=$(dirname "$path")
    base=$(basename "$path")
    upper=$(echo "$base" | tr '[:lower:]' '[:upper:]')
    if [ "$base" != "$upper" ]; then
        target="$dir/$upper"
        if [ -e "$target" ]; then
            echo "$(date '+%F %T') SKIP (target exists): $path -> $target" >> "$LOG"
            record_conflict "$path" "$path -> $target (uppercase target already exists)"
        else
            mv -n "$path" "$target" && echo "$(date '+%F %T') RENAMED: $path -> $target" >> "$LOG"
        fi
    fi
done < <(find "$ROOT" -depth -mindepth 2 ! -path "$INCOMING/*" ! -path "$CONFLICTS" ! -name '.*')

# --- Rewrite CONFLICTS.log to reflect only currently-active conflicts ----
if [ -d "$REVIEW" ]; then
    {
        echo "# first-seen<TAB>path<TAB>description -- rebuilt every run, safe to delete"
        for key in "${!cur_conflicts[@]}"; do
            printf '%s\n' "${cur_conflicts[$key]}"
        done | sort
    } > "$CONFLICTS"
    chown "$OWNER" "$CONFLICTS"
    chmod 0664 "$CONFLICTS"
fi
