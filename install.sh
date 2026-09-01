#!/bin/sh
# install.sh - build de-tnfsd, put the pieces where the shipped unit expects
# them, and hand back a running service.
#
# It installs three things and nothing else:
#
#   1. the binary, at $PREFIX/sbin/de-tnfsd
#   2. the system user 'tnfs' and the served layout: <root>/pub (2775) and
#      <root>/incoming (2770), both owned tnfs:tnfs
#   3. the systemd unit, with its paths rewritten if --prefix or --root move
#
# Re-running it is safe: an existing user or directory is kept, ownership and
# the two zone modes are reasserted, and a unit file that differs from what
# this script would write is backed up rather than overwritten silently.
#
# The modes above are the habit README recommends, not policy. The daemon is
# the only enforcement point; nothing a client can do changes if you set them
# differently afterwards. What they decide is who on the *host* can fill pub
# and drain the drop box - see README "Permissions" and "Draining the drop
# box".
#
# usage: sudo ./install.sh [--prefix DIR] [--root DIR] [--port N]
#                          [--no-service] [--no-start] [--check] [--dry-run]

set -eu

PREFIX=/usr/local
ROOT=/srv/tnfs
PORT=
USER_NAME=tnfs
UNIT_DIR=/etc/systemd/system
WANT_SERVICE=yes
WANT_START=yes
WANT_CHECK=no
DRY=no

die() { echo "install.sh: $*" >&2; exit 1; }
say() { echo "==> $*"; }

# Every state-changing command goes through run(), which is what makes
# --dry-run tell the truth instead of approximating it.
run() {
    if [ "$DRY" = yes ]; then
        echo "     + $*"
    else
        "$@"
    fi
}

while [ $# -gt 0 ]; do
    case $1 in
    --prefix)     PREFIX=${2:?--prefix needs a directory}; shift 2 ;;
    --prefix=*)   PREFIX=${1#*=}; shift ;;
    --root)       ROOT=${2:?--root needs a directory}; shift 2 ;;
    --root=*)     ROOT=${1#*=}; shift ;;
    --port)       PORT=${2:?--port needs a number}; shift 2 ;;
    --port=*)     PORT=${1#*=}; shift ;;
    --user)       USER_NAME=${2:?--user needs a name}; shift 2 ;;
    --user=*)     USER_NAME=${1#*=}; shift ;;
    --no-service) WANT_SERVICE=no; shift ;;
    --no-start)   WANT_START=no; shift ;;
    --check)      WANT_CHECK=yes; shift ;;
    --dry-run)    DRY=yes; shift ;;
    -h|--help)    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"
                  exit 0 ;;
    *)            die "unknown option '$1' (try --help)" ;;
    esac
done

case $PREFIX in /*) ;; *) die "--prefix must be an absolute path" ;; esac
case $ROOT   in /*) ;; *) die "--root must be an absolute path" ;; esac
case $PORT   in ''|*[!0-9]*) [ -z "$PORT" ] || die "--port must be a number" ;; esac

cd "$(dirname "$0")"
[ -f src/main.c ] && [ -f Makefile ] || die "run this from the de-tnfsd source tree"

# The daemon's unprivileged user is compiled in (UNPRIV_USER in src/main.c),
# so --user has to agree with the binary or the privilege drop fails at
# startup rather than here.
COMPILED_USER=$(sed -n 's/^#define UNPRIV_USER "\(.*\)"/\1/p' src/main.c)
[ -n "$COMPILED_USER" ] || die "cannot find UNPRIV_USER in src/main.c"
[ "$USER_NAME" = "$COMPILED_USER" ] ||
    die "--user $USER_NAME disagrees with the compiled-in user '$COMPILED_USER'"

[ "$(uname -s)" = Linux ] || die "this script is Linux-only (system user and
systemd unit); elsewhere: 'make install', create the '$USER_NAME' user and the
$ROOT/pub and $ROOT/incoming directories by hand, and start the binary from
whatever supervisor you use"

[ "$DRY" = yes ] || [ "$(id -u)" = 0 ] ||
    die "must run as root (or use --dry-run to see what it would do)"

# 1. Build.
say "building"
run make
[ "$WANT_CHECK" = no ] || { say "running the test suites"; run make check; }

# 2. The user. --system gives it no password, no ageing and a uid below the
#    login range; -U puts it in a group of its own, which is the group the
#    layout below is shared with.
if id -u "$USER_NAME" >/dev/null 2>&1; then
    say "user '$USER_NAME' already exists"
else
    say "creating system user '$USER_NAME'"
    run useradd --system -U --no-create-home \
        --home-dir "$ROOT" --shell /usr/sbin/nologin \
        --comment "de-tnfsd" "$USER_NAME"
fi

# 3. The layout. mkdir first, then chown/chmod unconditionally on the two
#    zones - an existing tree gets the ownership fixed, which is the usual
#    reason an upgrade would fail its startup probe.
say "layout in $ROOT"
run mkdir -p "$ROOT/pub" "$ROOT/incoming"
run chown "$USER_NAME:$USER_NAME" "$ROOT" "$ROOT/pub" "$ROOT/incoming"
run chmod 0755 "$ROOT"
run chmod 2775 "$ROOT/pub"       # setgid: subdirectories stay group-readable
run chmod 2770 "$ROOT/incoming"  # setgid: uploads inherit group tnfs

# 4. The binary.
say "installing $PREFIX/sbin/de-tnfsd"
run make install PREFIX="$PREFIX"

if [ "$WANT_SERVICE" = no ]; then
    say "done (no service installed)"
    echo "    run it with: $PREFIX/sbin/de-tnfsd${PORT:+ -p $PORT} $ROOT"
    exit 0
fi

command -v systemctl >/dev/null 2>&1 || die "no systemctl; re-run with --no-service"

# 5. The unit, with the three paths that depend on --prefix/--root rewritten.
#    The comment about User= is in the shipped file and survives the rewrite.
UNIT_SRC=de-tnfsd.service
UNIT_DST=$UNIT_DIR/de-tnfsd.service
TMP_UNIT=$(mktemp)
trap 'rm -f "$TMP_UNIT"' EXIT

sed -e "s|^ExecStart=.*|ExecStart=$PREFIX/sbin/de-tnfsd${PORT:+ -p $PORT} $ROOT|" \
    -e "s|^ReadOnlyPaths=.*|ReadOnlyPaths=$ROOT/pub|" \
    -e "s|^ReadWritePaths=.*|ReadWritePaths=$ROOT/incoming|" \
    "$UNIT_SRC" > "$TMP_UNIT"

if [ -f "$UNIT_DST" ] && ! cmp -s "$TMP_UNIT" "$UNIT_DST"; then
    BACKUP=$UNIT_DST.$(date +%Y%m%d%H%M%S).bak
    say "existing unit differs; keeping a copy at $BACKUP"
    run cp -p "$UNIT_DST" "$BACKUP"
fi

say "installing $UNIT_DST"
if [ "$DRY" = yes ]; then
    echo "     + install -m 644 <rewritten unit> $UNIT_DST"
    sed 's/^/       | /' "$TMP_UNIT"
else
    install -m 644 "$TMP_UNIT" "$UNIT_DST"
fi

run systemctl daemon-reload

if [ "$WANT_START" = no ]; then
    say "done (unit installed, not started)"
    echo "    systemctl enable --now de-tnfsd"
    exit 0
fi

say "enabling and starting de-tnfsd"
run systemctl enable de-tnfsd
run systemctl restart de-tnfsd

if [ "$DRY" = no ]; then
    # The daemon is Type=notify, so by the time systemctl returns it has
    # bound its sockets, passed its own startup probes and dropped
    # privileges. Show the lines that prove it.
    systemctl --no-pager --lines=10 status de-tnfsd || true
fi

say "done"
