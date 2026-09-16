/* server.h - the one piece of file-scope state: config, startup dirfds,
 * listening sockets, and the drop-box counters.
 *
 * Everything else (per-request scratch, per-session handles) lives on the
 * stack or in a session, so nothing here is touched mid-request except the
 * drop-box accounting, which is the daemon's own housekeeping and not
 * request state (DESIGN.md 6).
 */
#ifndef DE_SERVER_H
#define DE_SERVER_H

#include <limits.h>
#include <stdint.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* How the served namespace is laid out (DESIGN.md 4). SPLIT is the default and
 * the only mode that upholds the disjointness invariant structurally: pub/ reads,
 * incoming/ writes, nothing both. The two serve-root modes are an opt-in
 * relaxation for the "quick server in a repo" case: the root itself becomes a
 * single zone, read-only or read/write, with no drop box. */
enum serve_mode {
    SERVE_SPLIT = 0,        /* <root>/pub read-only + <root>/incoming write-only */
    SERVE_ROOT_RO,          /* <root> served read-only, no incoming             */
    SERVE_ROOT_RW           /* <root> served read/write (create + overwrite)    */
};

struct dropbox_state {
    uint64_t count;          /* files in incoming/, from the last scan     */
    uint64_t bytes;          /* sum of their sizes                         */
    uint64_t inflight_count; /* uploads currently open                      */
    uint64_t inflight_bytes; /* their apparent sizes so far                 */
    time_t   last_scan;
};

struct server {
    /* Configuration (DESIGN.md 8). */
    int      port;
    uint64_t max_file_size;
    uint64_t max_files;
    uint64_t max_total_bytes;
    int      no_incoming;
    int      ignore_case;   /* -i: fall back to ASCII case-insensitive match */
    enum serve_mode serve_mode;
    char     root_path[PATH_MAX];

    /* Directory fds opened once at startup; every operation is relative to
     * one of these and the daemon never resolves an absolute path again. */
    int root_fd;
    int pub_fd;
    int inc_fd;             /* -1 when --no-incoming */

    /* Listening sockets. */
    int udp_fd;
    int tcp_fd;

    struct dropbox_state db;
};

extern struct server srv;

#endif /* DE_SERVER_H */
