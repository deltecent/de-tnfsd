/* zone.h - the two-zone namespace, the capability table, and the single
 * resolve-and-authorize entry point every path-bearing handler goes through.
 *
 * The invariant this file exists to protect: no zone has both CAP_READ and
 * CAP_CREATE, so the readable and writable path sets are disjoint.
 */
#ifndef DE_ZONE_H
#define DE_ZONE_H

#include "util.h"

enum zone {
    ZONE_ROOT = 0,      /* synthetic: lists exactly "pub" and "incoming" */
    ZONE_PUB,
    ZONE_INCOMING
};

#define CAP_LOOKUP  0x01u   /* may learn whether a name exists            */
#define CAP_LIST    0x02u   /* may enumerate names                        */
#define CAP_READ    0x04u   /* may open for reading and read bytes        */
#define CAP_CREATE  0x08u   /* may create a file that does not exist      */
#define CAP_MODIFY  0x10u   /* overwrite, truncate, append, rename, chmod */
#define CAP_REMOVE  0x20u   /* unlink, rmdir                              */

/* The whole policy. MODIFY and REMOVE are unset everywhere; they exist so
 * handlers check a real capability rather than hardcoding a refusal. */
unsigned zone_caps(enum zone z);

/* "pub" / "incoming" -> zone, or -1. */
int zone_from_name(const char *name);
const char *zone_name(enum zone z);

/* A path resolved to a directory fd plus an optional leaf name. */
struct resolved {
    enum zone zone;
    unsigned caps;
    int  dirfd;                     /* parent directory                    */
    int  owned;                     /* dirfd must be closed by the caller  */
    int  has_leaf;                  /* 0 => the path names the zone root   */
    char leaf[TNFS_MAX_NAME + 1];
};

/* Split, validate, and walk. mount_zone is the zone the session mounted;
 * when it is ZONE_ROOT the first path component selects the zone.
 *
 * Returns 0 on success, or a TNFS status code. The walk uses openat() with
 * O_NOFOLLOW one component at a time from a startup dirfd: no path strings
 * are built, so there is no check-then-use window to race.
 */
int path_resolve(int mount_zone, const char *path, struct resolved *out);

void resolved_release(struct resolved *r);

#endif /* DE_ZONE_H */
