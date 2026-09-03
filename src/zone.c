#include "zone.h"

#include "server.h"
#include "tnfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_COMPONENTS 32

unsigned zone_caps(enum zone z)
{
    switch (z) {
    case ZONE_ROOT:     return CAP_LOOKUP | CAP_LIST;
    case ZONE_PUB:      return CAP_LOOKUP | CAP_LIST | CAP_READ;
    case ZONE_INCOMING: return CAP_CREATE;
    }
    return 0;
}

/* Case-insensitive: 8-bit CP/M clients uppercase an entire command line,
 * FujiNet's own path included, before a client program ever sees it -- so a
 * TNFS path arrives as /PUB or /INCOMING with no way for the client side to
 * send lowercase. Leaf names inside a zone are matched by the filesystem and
 * stay whatever case they are on disk; only the two fixed top-level zone
 * names are folded here. */
int zone_from_name(const char *name)
{
    if (strcasecmp(name, "pub") == 0)
        return ZONE_PUB;
    if (strcasecmp(name, "incoming") == 0)
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
        if (ncomp == 0) {
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
        out->dirfd = srv.root_fd;
        out->has_leaf = 0;
        return TNFS_OK;
    }

    dirfd = (zone == ZONE_PUB) ? srv.pub_fd : srv.inc_fd;

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

    /* pub: descend one component at a time, refusing symlinks outright. */
    for (int i = start; i < ncomp - 1; i++) {
        int nfd = openat(dirfd, comps[i],
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nfd < 0) {
            if (owned)
                close(dirfd);
            return tnfs_errno(errno);
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
    }
    return TNFS_OK;
}
