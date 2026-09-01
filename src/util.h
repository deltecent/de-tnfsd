/* util.h - little-endian codecs, name validation, wildcard matching. */
#ifndef DE_UTIL_H
#define DE_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Longest single path component we will accept from a client. */
#define TNFS_MAX_NAME 100
/* Longest whole client path we will accept. */
#define TNFS_MAX_PATH 256

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v & 0xffffffffu));
    put_u32(p + 4, (uint32_t)((v >> 32) & 0xffffffffu));
}

static inline uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Bounded NUL-terminated string read out of a datagram.
 *
 * Reads the string starting at *off, copies at most dstsz-1 bytes into dst,
 * and advances *off past the terminator. Returns 0 on success, -1 if the
 * datagram ends before a terminator is found or the string does not fit.
 */
int msg_get_string(const uint8_t *msg, size_t len, size_t *off,
                   char *dst, size_t dstsz);

/* True if the component is a syntactically acceptable path component:
 * non-empty, not "." or "..", no '/', no control characters, within
 * TNFS_MAX_NAME. */
int name_is_valid_component(const char *name);

/* Stricter still: the leaf name of a drop-box upload. Adds "no leading dot"
 * and a conservative charset (alnum and " ._-+()!#$%&,;=@[]^{}~'"). */
int name_is_valid_upload(const char *name);

/* Case-insensitive '*'/'?' wildcard match. Empty pattern matches everything. */
int wildcard_match(const char *pattern, const char *name, int case_sensitive);

/* Map a host errno onto the nearest TNFS status code. */
int tnfs_errno(int e);

/* Parse a byte count with an optional K/M/G suffix (powers of 1024).
 * Returns 0 on success, -1 on a malformed or out-of-range value. */
int parse_size(const char *s, uint64_t *out);

#endif /* DE_UTIL_H */
