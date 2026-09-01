#include "dir.h"

#include "server.h"
#include "tnfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

struct sort_ctx {
    int folders_first;
    int case_sensitive;
    int descending;
    int by_mtime;
    int by_size;
};

static int entry_cmp(const struct dirent_snap *a, const struct dirent_snap *b,
                     const struct sort_ctx *c)
{
    int r;

    if (c->folders_first) {
        int ad = (a->flags & TNFS_DIRENTRY_DIR) ? 1 : 0;
        int bd = (b->flags & TNFS_DIRENTRY_DIR) ? 1 : 0;
        if (ad != bd)
            return bd - ad;             /* directories first, always */
    }
    if (c->by_mtime)
        r = (a->mtime < b->mtime) ? -1 : (a->mtime > b->mtime);
    else if (c->by_size)
        r = (a->size < b->size) ? -1 : (a->size > b->size);
    else
        r = c->case_sensitive ? strcmp(a->name, b->name)
                              : strcasecmp(a->name, b->name);
    if (r == 0)
        r = strcmp(a->name, b->name);   /* stable, deterministic tiebreak */
    return c->descending ? -r : r;
}

/* Merge sort, so the comparison can carry a context without needing the
 * non-portable qsort_r and without a file-scope variable. */
static void sort_entries(struct dirent_snap *a, size_t n,
                         struct dirent_snap *tmp, const struct sort_ctx *c)
{
    size_t mid, i, j, k;

    if (n < 2)
        return;
    mid = n / 2;
    sort_entries(a, mid, tmp, c);
    sort_entries(a + mid, n - mid, tmp, c);

    i = 0; j = mid; k = 0;
    while (i < mid && j < n)
        tmp[k++] = entry_cmp(&a[i], &a[j], c) <= 0 ? a[i++] : a[j++];
    while (i < mid)
        tmp[k++] = a[i++];
    while (j < n)
        tmp[k++] = a[j++];
    memcpy(a, tmp, n * sizeof *a);
}

static void fill_stat(struct dirent_snap *e, const struct stat *st)
{
    e->size  = (st->st_size > 0xffffffffLL) ? 0xffffffffu
                                            : (uint32_t)st->st_size;
    e->mtime = (uint32_t)st->st_mtime;
    e->ctime = (uint32_t)st->st_ctime;
    e->flags = 0;
    if (S_ISDIR(st->st_mode))
        e->flags |= TNFS_DIRENTRY_DIR;
    else if (!S_ISREG(st->st_mode))
        e->flags |= TNFS_DIRENTRY_SPECIAL;
    if (e->name[0] == '.')
        e->flags |= TNFS_DIRENTRY_HIDDEN;
}

int dir_snapshot(int dirfd, const char *pattern, uint8_t diropt,
                 uint8_t dirsort, uint16_t maxres, struct dir_list *out)
{
    DIR *d;
    struct dirent *de;
    struct dirent_snap *ents = NULL, *tmp = NULL;
    size_t n = 0, cap = 0;
    int skip_hidden  = !(diropt & TNFS_DIROPT_NO_SKIPHIDDEN);
    int skip_special = !(diropt & TNFS_DIROPT_NO_SKIPSPECIAL);
    int dir_pattern  = (diropt & TNFS_DIROPT_DIR_PATTERN) != 0;
    int case_sens    = (dirsort & TNFS_DIRSORT_CASE) != 0;

    memset(out, 0, sizeof *out);

    d = fdopendir(dirfd);
    if (d == NULL) {
        int e = errno;
        close(dirfd);
        return tnfs_errno(e);
    }

    while ((de = readdir(d)) != NULL) {
        struct dirent_snap e;
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (strlen(de->d_name) > TNFS_MAX_NAME)
            continue;               /* unreachable over the wire anyway */
        if (skip_hidden && de->d_name[0] == '.')
            continue;

        memset(&e, 0, sizeof e);
        strcpy(e.name, de->d_name);
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;               /* vanished under us, or unreadable */
        fill_stat(&e, &st);

        if (skip_special && (e.flags & TNFS_DIRENTRY_SPECIAL))
            continue;
        if (pattern != NULL && *pattern != '\0') {
            int is_dir = (e.flags & TNFS_DIRENTRY_DIR) != 0;
            if ((!is_dir || dir_pattern) &&
                !wildcard_match(pattern, e.name, case_sens))
                continue;
        }

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            struct dirent_snap *p;
            if (ncap > DIR_MAX_ENTRIES)
                ncap = DIR_MAX_ENTRIES;
            if (n == ncap)
                break;              /* listing is capped, not unbounded */
            p = realloc(ents, ncap * sizeof *ents);
            if (p == NULL) {
                free(ents);
                closedir(d);
                return TNFS_ENOMEM;
            }
            ents = p;
            cap = ncap;
        }
        ents[n++] = e;
        if (maxres != 0 && n >= maxres)
            break;
    }
    closedir(d);

    if (!(dirsort & TNFS_DIRSORT_NONE) && n > 1) {
        struct sort_ctx c = {
            .folders_first  = !(diropt & TNFS_DIROPT_NO_FOLDERSFIRST),
            .case_sensitive = case_sens,
            .descending     = (dirsort & TNFS_DIRSORT_DESCENDING) != 0,
            .by_mtime       = (dirsort & TNFS_DIRSORT_MODIFIED) != 0,
            .by_size        = (dirsort & TNFS_DIRSORT_SIZE) != 0,
        };
        tmp = malloc(n * sizeof *tmp);
        if (tmp == NULL) {
            free(ents);
            return TNFS_ENOMEM;
        }
        sort_entries(ents, n, tmp, &c);
        free(tmp);
    }

    out->ents = ents;
    out->n = n;
    out->pos = 0;
    return TNFS_OK;
}

int dir_synthetic_root(struct dir_list *out)
{
    struct dirent_snap *ents;
    struct stat st;
    size_t n = 0;

    memset(out, 0, sizeof *out);
    ents = calloc(2, sizeof *ents);
    if (ents == NULL)
        return TNFS_ENOMEM;

    strcpy(ents[n].name, "pub");
    if (fstat(srv.pub_fd, &st) == 0)
        fill_stat(&ents[n], &st);
    ents[n].flags |= TNFS_DIRENTRY_DIR;
    n++;

    if (srv.inc_fd >= 0) {
        strcpy(ents[n].name, "incoming");
        if (fstat(srv.inc_fd, &st) == 0)
            fill_stat(&ents[n], &st);
        ents[n].flags |= TNFS_DIRENTRY_DIR;
        /* The drop box's size is not the client's business: it is the one
         * number a listing of the root could otherwise leak. */
        ents[n].size = 0;
        n++;
    }

    out->ents = ents;
    out->n = n;
    out->pos = 0;
    return TNFS_OK;
}

void dir_free(struct dir_list *l)
{
    free(l->ents);
    l->ents = NULL;
    l->n = l->pos = 0;
}
