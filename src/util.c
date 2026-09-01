#include "util.h"

#include "tnfs.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int msg_get_string(const uint8_t *msg, size_t len, size_t *off,
                   char *dst, size_t dstsz)
{
    size_t i = *off, n = 0;

    if (dstsz == 0)
        return -1;
    while (i < len && msg[i] != 0) {
        if (n + 1 >= dstsz)
            return -1;
        dst[n++] = (char)msg[i++];
    }
    if (i >= len)          /* ran off the end without a terminator */
        return -1;
    dst[n] = '\0';
    *off = i + 1;
    return 0;
}

int name_is_valid_component(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > TNFS_MAX_NAME)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

int name_is_valid_upload(const char *name)
{
    static const char *extra = " ._-+()!#$%&,;=@[]^{}~'";

    if (!name_is_valid_component(name))
        return 0;
    if (name[0] == '.')     /* no dotfiles: they would collide with .tmp-* */
        return 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || strchr(extra, c) != NULL)
            continue;
        return 0;
    }
    return 1;
}

static int chr_eq(char a, char b, int case_sensitive)
{
    if (case_sensitive)
        return a == b;
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

int wildcard_match(const char *pattern, const char *name, int case_sensitive)
{
    /* Iterative backtracking matcher: no recursion, so a pathological
     * pattern cannot blow the stack. */
    const char *p = pattern, *s = name;
    const char *star = NULL, *star_s = NULL;

    if (pattern == NULL || *pattern == '\0')
        return 1;

    while (*s) {
        if (*p == '?' || (*p && chr_eq(*p, *s, case_sensitive))) {
            p++;
            s++;
        } else if (*p == '*') {
            star = p++;
            star_s = s;
        } else if (star) {
            p = star + 1;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

int parse_size(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long v;
    uint64_t mult = 1;

    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s)
        return -1;

    if (*end != '\0') {
        if (end[1] != '\0')
            return -1;
        switch (*end) {
        case 'k': case 'K': mult = 1024ull; break;
        case 'm': case 'M': mult = 1024ull * 1024; break;
        case 'g': case 'G': mult = 1024ull * 1024 * 1024; break;
        default: return -1;
        }
    }
    if (v > UINT64_MAX / mult)
        return -1;
    *out = (uint64_t)v * mult;
    return 0;
}

int tnfs_errno(int e)
{
    switch (e) {
    case 0:              return TNFS_OK;
    case EPERM:          return TNFS_EPERM;
    case ENOENT:         return TNFS_ENOENT;
    case EIO:            return TNFS_EIO;
    case ENXIO:          return TNFS_ENXIO;
    case E2BIG:          return TNFS_E2BIG;
    case EBADF:          return TNFS_EBADF;
    case EAGAIN:         return TNFS_EAGAIN;
    case ENOMEM:         return TNFS_ENOMEM;
    case EACCES:         return TNFS_EACCES;
    case EBUSY:          return TNFS_EBUSY;
    case EEXIST:         return TNFS_EEXIST;
    case ENOTDIR:        return TNFS_ENOTDIR;
    case EISDIR:         return TNFS_EISDIR;
    case EINVAL:         return TNFS_EINVAL;
    case ENFILE:         return TNFS_ENFILE;
    case EMFILE:         return TNFS_EMFILE;
    case EFBIG:          return TNFS_EFBIG;
    case ENOSPC:         return TNFS_ENOSPC;
    case ESPIPE:         return TNFS_ESPIPE;
    case EROFS:          return TNFS_EROFS;
    case ENAMETOOLONG:   return TNFS_ENAMETOOLONG;
    case ENOSYS:         return TNFS_ENOSYS;
    case ENOTEMPTY:      return TNFS_ENOTEMPTY;
    case ELOOP:          return TNFS_ELOOP;
    case ENOBUFS:        return TNFS_ENOBUFS;
    default:             return TNFS_EIO;
    }
}
