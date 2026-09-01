/* dir.h - a directory listing is snapshotted once at OPENDIR time.
 *
 * A snapshot makes TELLDIR/SEEKDIR a plain index and gives OPENDIRX the
 * match count it has to report. It lists exactly one directory: there is no
 * recursive traverse, so the size of a listing is bounded by one directory
 * and by DIR_MAX_ENTRIES.
 */
#ifndef DE_DIR_H
#define DE_DIR_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define DIR_MAX_ENTRIES 4096

struct dirent_snap {
    char     name[TNFS_MAX_NAME + 1];
    uint32_t size;
    uint32_t mtime;
    uint32_t ctime;
    uint8_t  flags;         /* TNFS_DIRENTRY_* */
};

struct dir_list {
    struct dirent_snap *ents;
    size_t n;
    size_t pos;
};

/* Snapshot the directory named by dirfd, which this call takes ownership of
 * and closes. Returns 0 or a TNFS status code. */
int dir_snapshot(int dirfd, const char *pattern, uint8_t diropt,
                 uint8_t dirsort, uint16_t maxres, struct dir_list *out);

/* The synthetic root listing: "pub" and, unless --no-incoming, "incoming". */
int dir_synthetic_root(struct dir_list *out);

void dir_free(struct dir_list *l);

#endif /* DE_DIR_H */
