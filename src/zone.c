#include "zone.h"

#include "server.h"
#include "tnfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_COMPONENTS 32

unsigned zone_caps(enum zone z)
{
    switch (z) {
    case ZONE_ROOT:
        /* In split mode the root is synthetic: it only names the two zones,
         * so it looks up and lists but never reads. In a serve-root mode the
         * root *is* the served zone and carries that mode's capabilities. This
         * is the one place the disjointness invariant is deliberately relaxed:
         * SERVE_ROOT_RW grants READ and CREATE on the same zone (DESIGN.md 4). */
        switch (srv.serve_mode) {
        case SERVE_ROOT_RO: return CAP_LOOKUP | CAP_LIST | CAP_READ;
        case SERVE_ROOT_RW: return CAP_LOOKUP | CAP_LIST | CAP_READ |
                                   CAP_CREATE | CAP_MODIFY;
        case SERVE_SPLIT:   break;
        }
        return CAP_LOOKUP | CAP_LIST;
    case ZONE_PUB:      return CAP_LOOKUP | CAP_LIST | CAP_READ;
    case ZONE_INCOMING: return CAP_CREATE;
    }
    return 0;
}

int zone_from_name(const char *name)
{
    if (strcmp(name, "pub") == 0)
        return ZONE_PUB;
    if (strcmp(name, "incoming") == 0)
        return ZONE_INCOMING;
    return -1;
}

const char *zone_name(enum zone z)
{
    switch (z) {
    case ZONE_ROOT:     return "/";
    case ZONE_PUB:      return "/pub";
    case ZONE_INCOMING: return "/incoming";
    }
    return "?";
}

void resolved_release(struct resolved *r)
{
    if (r->owned && r->dirfd >= 0)
        close(r->dirfd);
    r->dirfd = -1;
    r->owned = 0;
}

/* Split a client path into validated components. A trailing separator is
 * tolerated; an interior empty component, a "." or a ".." is not. */
static int split_path(const char *path,
                      char comps[MAX_COMPONENTS][TNFS_MAX_NAME + 1],
                      int *ncomp)
{
    const char *p = path;
    int n = 0;

    if (strlen(path) > TNFS_MAX_PATH)
        return TNFS_ENAMETOOLONG;

    while (*p == '/')
        p++;
    while (*p) {
        const char *start = p;
        size_t len;

        while (*p && *p != '/')
            p++;
        len = (size_t)(p - start);
        if (len == 0)
            return TNFS_EINVAL;         /* interior "//" */
        if (len > TNFS_MAX_NAME)
            return TNFS_ENAMETOOLONG;
        if (n >= MAX_COMPONENTS)
            return TNFS_ENAMETOOLONG;
        memcpy(comps[n], start, len);
        comps[n][len] = '\0';
        if (!name_is_valid_component(comps[n]))
            return TNFS_EINVAL;         /* rejects "." and ".." */
        n++;
        if (*p == '/') {
            p++;
            while (*p == '/')           /* tolerate only a trailing run */
                p++;
            if (*p == '\0')
                break;
        }
    }
    *ncomp = n;
    return TNFS_OK;
}

/* Opt-in (-i) ASCII case-insensitive fallback. Only ever called after an exact
 * match has already missed, so it never overrides an exact hit and costs
 * nothing on the common path. It reads the directory the resolver is standing
 * in via a fresh fd (fdopendir consumes it) and copies out the first entry that
 * equals `want` under strcasecmp. Folding is ASCII-only and under the C locale
 * by design: that is exactly what CP/M (uppercase) and DOS-style clients need,
 * and it avoids inventing Unicode casing rules the filesystem never agreed to.
 * On a case-preserving-but-insensitive host FS (macOS, Windows) an ambiguous
 * match cannot arise; on a case-sensitive one two entries can differ only in
 * case, and then the first found wins. Returns 0 and fills `real` on a match. */
static int find_ci(int dirfd, const char *want, char *real, size_t realsz)
{
    int fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d;
    struct dirent *de;
    int found = -1;

    if (fd < 0)
        return -1;
    d = fdopendir(fd);
    if (d == NULL) {
        close(fd);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (strcasecmp(de->d_name, want) == 0) {
            if (strlen(de->d_name) < realsz) {
                strcpy(real, de->d_name);
                found = 0;
            }
            break;
        }
    }
    closedir(d);
    return found;
}

int path_resolve(int mount_zone, const char *path, struct resolved *out)
{
    char comps[MAX_COMPONENTS][TNFS_MAX_NAME + 1];
    int ncomp = 0, start = 0, rc, zone, dirfd, owned = 0;

    memset(out, 0, sizeof *out);
    out->dirfd = -1;

    rc = split_path(path, comps, &ncomp);
    if (rc != TNFS_OK)
        return rc;

    if (mount_zone == ZONE_ROOT) {
        if (srv.serve_mode != SERVE_SPLIT) {
            /* Serve-root: there are no sub-zones. The whole path is relative
             * to the root, which is a real read (or read/write) zone. */
            zone = ZONE_ROOT;
        } else if (ncomp == 0) {
            zone = ZONE_ROOT;
        } else {
            zone = zone_from_name(comps[0]);
            if (zone < 0)
                return TNFS_ENOENT;     /* the root serves nothing else */
            start = 1;
        }
    } else {
        zone = mount_zone;
    }

    if (zone == ZONE_INCOMING && srv.inc_fd < 0)
        return TNFS_ENOENT;             /* --no-incoming */

    out->zone = (enum zone)zone;
    out->caps = zone_caps((enum zone)zone);

    if (zone == ZONE_ROOT) {
        if (srv.serve_mode == SERVE_SPLIT) {
            /* Synthetic: the root names the two zones and holds no files of
             * its own, so it never descends into anything. */
            out->dirfd = srv.root_fd;
            out->has_leaf = 0;
            return TNFS_OK;
        }
        /* Serve-root: descend from the root dirfd exactly like pub below. */
        dirfd = srv.root_fd;
    } else {
        dirfd = (zone == ZONE_PUB) ? srv.pub_fd : srv.inc_fd;
    }

    if (zone == ZONE_INCOMING) {
        /* The drop box is flat: one leaf, no subdirectories. More than one
         * component is refused with the zone's uniform EACCES rather than a
         * structural error, so the shape of the refusal says nothing about
         * what is or is not in there. */
        if (ncomp - start > 1)
            return TNFS_EACCES;
        out->dirfd = dirfd;
        if (ncomp - start == 1) {
            strcpy(out->leaf, comps[ncomp - 1]);
            out->has_leaf = 1;
        }
        return TNFS_OK;
    }

    /* pub or a serve-root zone: descend one component at a time, refusing
     * symlinks outright. */
    for (int i = start; i < ncomp - 1; i++) {
        int nfd = openat(dirfd, comps[i],
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0 && errno == ENOENT && srv.ignore_case) {
            char real[TNFS_MAX_NAME + 1];
            if (find_ci(dirfd, comps[i], real, sizeof real) == 0)
                nfd = openat(dirfd, real,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (nfd < 0) {
            int e = errno;
            if (owned)
                close(dirfd);
            return tnfs_errno(e);
        }
        if (owned)
            close(dirfd);
        dirfd = nfd;
        owned = 1;
    }

    out->dirfd = dirfd;
    out->owned = owned;
    if (ncomp - start >= 1) {
        strcpy(out->leaf, comps[ncomp - 1]);
        out->has_leaf = 1;
        /* Resolve the leaf to its real casing only when it names a file that
         * already exists: an exact hit is left alone, and a genuine miss keeps
         * the requested name so a create still uses what the client asked for.
         * That is what confines the fold to "find an existing name" and keeps
         * every create -- including the drop box, which returned above -- exact. */
        if (srv.ignore_case) {
            struct stat st;
            char real[TNFS_MAX_NAME + 1];
            if (fstatat(dirfd, out->leaf, &st, AT_SYMLINK_NOFOLLOW) != 0 &&
                errno == ENOENT &&
                find_ci(dirfd, out->leaf, real, sizeof real) == 0)
                strcpy(out->leaf, real);
        }
    }
    return TNFS_OK;
}
