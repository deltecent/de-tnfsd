/* main.c - argument parsing, the startup checks that make the two-zone
 * invariant true, privilege drop, and then the event loop.
 *
 * Startup order is load-bearing (DESIGN.md 7):
 *   1. parse config, bind sockets
 *   2. open the root, pub and incoming directory fds; run the layout checks
 *   3. drop privileges and confine
 *   4. serve - from here no absolute path is ever resolved again
 */
#include "dropbox.h"
#include "log.h"
#include "net.h"
#include "server.h"
#include "tnfs.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct server srv;

#define DEFAULT_PORT        16384
#define DEFAULT_MAX_SIZE    (16ull * 1024 * 1024)
#define DEFAULT_MAX_FILES   256ull
#define DEFAULT_MAX_TOTAL   (1024ull * 1024 * 1024)

#define UNPRIV_USER "tnfs"

static int notify_fd = -1;

static void usage(FILE *f)
{
    fprintf(f,
"usage: de-tnfsd [-p <port>] [-s <max-file-size>] [-n <max-files>]\n"
"                [-q <max-total-bytes>] [--no-incoming] [-v] <root>\n"
"\n"
"  -p  port to listen on                            (default %d)\n"
"  -s  maximum size of one uploaded file            (default 16M, 0 = no limit)\n"
"  -n  maximum number of files in incoming/         (default 256, 0 = no limit)\n"
"  -q  maximum total bytes in incoming/             (default 1G,  0 = no limit)\n"
"      --no-incoming    serve pub/ only; reject all writes\n"
"  -v  verbose logging\n"
"\n"
"Size arguments take an optional K, M or G suffix (powers of 1024).\n"
"<root> must contain a pub/ directory and, unless --no-incoming, incoming/.\n",
        DEFAULT_PORT);
}

/* A tiny hand-rolled parser: getopt_long is not POSIX, and the option set is
 * small enough that this is shorter than working around that. */
static int parse_args(int argc, char **argv)
{
    const char *root = NULL;
    int i = 1;

    srv.port = DEFAULT_PORT;
    srv.max_file_size = DEFAULT_MAX_SIZE;
    srv.max_files = DEFAULT_MAX_FILES;
    srv.max_total_bytes = DEFAULT_MAX_TOTAL;

    while (i < argc) {
        const char *a = argv[i];

        if (strcmp(a, "--no-incoming") == 0) {
            srv.no_incoming = 1;
            i++;
            continue;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        }
        if (strcmp(a, "-v") == 0) {
            log_set_verbose(1);
            i++;
            continue;
        }
        if (a[0] == '-' && a[1] != '\0' && strchr("psnq", a[1]) != NULL) {
            const char *val = (a[2] != '\0') ? a + 2
                            : (i + 1 < argc) ? argv[++i] : NULL;
            uint64_t v;

            if (val == NULL) {
                fprintf(stderr, "de-tnfsd: option -%c needs a value\n", a[1]);
                return -1;
            }
            if (a[1] == 'p') {
                char *end;
                long port = strtol(val, &end, 10);
                if (*end != '\0' || port < 1 || port > 65535) {
                    fprintf(stderr, "de-tnfsd: bad port '%s'\n", val);
                    return -1;
                }
                srv.port = (int)port;
            } else {
                if (parse_size(val, &v) != 0) {
                    fprintf(stderr, "de-tnfsd: bad size '%s'\n", val);
                    return -1;
                }
                switch (a[1]) {
                case 's': srv.max_file_size = v; break;
                case 'n': srv.max_files = v; break;
                case 'q': srv.max_total_bytes = v; break;
                }
            }
            i++;
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "de-tnfsd: unknown option '%s'\n", a);
            return -1;
        }
        if (root != NULL) {
            fprintf(stderr, "de-tnfsd: only one root directory may be given\n");
            return -1;
        }
        root = a;
        i++;
    }

    if (root == NULL) {
        usage(stderr);
        return -1;
    }
    if (snprintf(srv.root_path, sizeof srv.root_path, "%s", root) >=
        (int)sizeof srv.root_path) {
        fprintf(stderr, "de-tnfsd: root path too long\n");
        return -1;
    }
    return 0;
}

/* Step 2: open the three directory fds and check the layout that the
 * capability table depends on. The daemon refuses to start rather than
 * degrade. */
static int open_zones(void)
{
    struct stat root_st, pub_st, inc_st;

    srv.root_fd = srv.pub_fd = srv.inc_fd = -1;

    srv.root_fd = open(srv.root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (srv.root_fd < 0) {
        log_err("cannot open root %s: %s", srv.root_path, strerror(errno));
        return -1;
    }
    if (fstat(srv.root_fd, &root_st) != 0) {
        log_err("cannot stat root: %s", strerror(errno));
        return -1;
    }

    /* O_NOFOLLOW with O_DIRECTORY is what enforces "must not be a symlink". */
    srv.pub_fd = openat(srv.root_fd, "pub",
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (srv.pub_fd < 0) {
        log_err("%s/pub: %s (must exist and be a real directory)",
                srv.root_path, strerror(errno));
        return -1;
    }
    if (fstat(srv.pub_fd, &pub_st) != 0)
        return -1;
    if (pub_st.st_dev != root_st.st_dev) {
        log_err("%s/pub is on a different mount than the root", srv.root_path);
        return -1;
    }

    if (srv.no_incoming) {
        log_info("--no-incoming: serving pub/ only, all writes refused");
        return 0;
    }

    srv.inc_fd = openat(srv.root_fd, "incoming",
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (srv.inc_fd < 0) {
        log_err("%s/incoming: %s (must exist and be a real directory; "
                "use --no-incoming to serve pub/ only)",
                srv.root_path, strerror(errno));
        return -1;
    }
    if (fstat(srv.inc_fd, &inc_st) != 0)
        return -1;
    if (inc_st.st_dev != root_st.st_dev) {
        log_err("%s/incoming is on a different mount than the root",
                srv.root_path);
        return -1;
    }
    /* Compared by identity, not by name: two names for one directory would
     * make the readable and writable sets overlap, which is the one thing
     * the whole design exists to prevent. */
    if (inc_st.st_dev == pub_st.st_dev && inc_st.st_ino == pub_st.st_ino) {
        log_err("%s/pub and %s/incoming are the same directory",
                srv.root_path, srv.root_path);
        return -1;
    }
    return 0;
}

/* The daemon needs exactly two things from the filesystem: it must be able to
 * read pub, and create files in incoming. Both are checked by attempting
 * them - after the privilege drop, so the answer is about the uid that will
 * actually serve requests. */
static int probe_zones(void)
{
    int fd;
    char tmp[32];

    fd = openat(srv.pub_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        log_err("cannot read %s/pub: %s", srv.root_path, strerror(errno));
        return -1;
    }
    close(fd);

    if (srv.inc_fd < 0)
        return 0;

    if (dropbox_make_temp(tmp, sizeof tmp) != 0)
        return -1;
    fd = openat(srv.inc_fd, tmp,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0660);
    if (fd < 0) {
        log_err("cannot create files in %s/incoming: %s",
                srv.root_path, strerror(errno));
        return -1;
    }
    close(fd);
    unlinkat(srv.inc_fd, tmp, 0);
    return 0;
}

/* Step 3. Confinement of the served namespace is structural - it comes from
 * the dirfd walk, not from here - so this is defence in depth rather than the
 * mechanism anything relies on. */
static int drop_privileges(void)
{
    struct passwd *pw;

    if (geteuid() != 0) {
        log_info("not running as root; no privilege drop");
        return 0;
    }

    pw = getpwnam(UNPRIV_USER);        /* before chroot: needs /etc/passwd */
    if (pw == NULL) {
        log_err("cannot drop privileges: no such user '%s'", UNPRIV_USER);
        return -1;
    }

    if (chroot(srv.root_path) != 0 || chdir("/") != 0) {
        log_err("chroot %s: %s", srv.root_path, strerror(errno));
        return -1;
    }
    if (setgroups(0, NULL) != 0 ||
        setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) {
        log_err("cannot drop privileges: %s", strerror(errno));
        return -1;
    }
    if (setuid(0) == 0) {
        log_err("privilege drop did not stick");
        return -1;
    }
    log_info("dropped privileges to %s (uid=%d gid=%d), chrooted to %s",
             UNPRIV_USER, (int)pw->pw_uid, (int)pw->pw_gid, srv.root_path);
    return 0;
}

/* sd_notify without libsystemd: the protocol is one datagram to the socket
 * named in $NOTIFY_SOCKET. The socket is connected before the chroot; when
 * the variable is unset, everything here is a no-op, which is what makes the
 * shell case work unchanged. */
static void notify_connect(void)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un addr;

    if (path == NULL || *path == '\0')
        return;

    notify_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (notify_fd < 0)
        return;
    fcntl(notify_fd, F_SETFD, FD_CLOEXEC);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path[0] == '@') {               /* abstract namespace */
        addr.sun_path[0] = '\0';
        snprintf(addr.sun_path + 1, sizeof addr.sun_path - 1, "%s", path + 1);
    } else {
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    }
    if (connect(notify_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(notify_fd);
        notify_fd = -1;
    }
}

static void notify_send(const char *state)
{
    if (notify_fd >= 0)
        (void)send(notify_fd, state, strlen(state), 0);
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) != 0)
        return 2;

    /* Uploads are created 0660 so that the group draining the drop box can
     * read them (DESIGN.md 6). The inherited umask would otherwise decide
     * that silently - systemd's default 0022 strips the group-write bit - so
     * it is set here rather than left to the environment. 0002 keeps the
     * world bits off whatever mode a create asks for. */
    umask(0002);

    dropbox_init();
    notify_connect();

    if (net_listen(srv.port) != 0)
        return 1;
    if (open_zones() != 0)
        return 1;
    if (drop_privileges() != 0)
        return 1;
    if (probe_zones() != 0)
        return 1;

    dropbox_scan();
    if (srv.inc_fd >= 0)
        log_info("dropbox files=%llu bytes=%llu limits: size=%llu files=%llu total=%llu",
                 (unsigned long long)srv.db.count,
                 (unsigned long long)srv.db.bytes,
                 (unsigned long long)srv.max_file_size,
                 (unsigned long long)srv.max_files,
                 (unsigned long long)srv.max_total_bytes);

    notify_send("READY=1");
    return net_run();
}
