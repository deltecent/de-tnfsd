#include "dropbox.h"

#include "log.h"
#include "server.h"
#include "tnfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TMP_PREFIX ".tmp-"

void dropbox_scan(void)
{
    DIR *d;
    struct dirent *de;
    int fd;
    uint64_t count = 0, bytes = 0;
    time_t now = time(NULL);

    if (srv.inc_fd < 0)
        return;

    fd = openat(srv.inc_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || (d = fdopendir(fd)) == NULL) {
        if (fd >= 0)
            close(fd);
        log_err("dropbox: cannot scan incoming: %s", strerror(errno));
        return;
    }

    while ((de = readdir(d)) != NULL) {
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;

        if (strncmp(de->d_name, TMP_PREFIX, sizeof TMP_PREFIX - 1) == 0 &&
            now - st.st_mtime > TMP_MAX_AGE_SEC) {
            /* An upload that was abandoned mid-flight. Sweeping it on age is
             * what keeps a client that opens and disappears from wedging the
             * drop box against its own limits. */
            if (unlinkat(fd, de->d_name, 0) == 0) {
                log_info("dropbox sweep name=%s size=%lld",
                         de->d_name, (long long)st.st_size);
                continue;
            }
        }
        count++;
        bytes += (uint64_t)st.st_size;
    }
    closedir(d);

    srv.db.count = count;
    srv.db.bytes = bytes;
    srv.db.last_scan = now;
}

void dropbox_tick(time_t now)
{
    if (srv.inc_fd < 0)
        return;
    if (now - srv.db.last_scan >= DROPBOX_RESCAN_SEC)
        dropbox_scan();
}

static int over_limits(void)
{
    if (srv.max_files != 0 &&
        srv.db.count + srv.db.inflight_count >= srv.max_files)
        return 1;
    if (srv.max_total_bytes != 0 &&
        srv.db.bytes + srv.db.inflight_bytes >= srv.max_total_bytes)
        return 1;
    return 0;
}

int dropbox_check_open(void)
{
    if (srv.db.inflight_count >= MAX_UPLOADS_TOTAL)
        return TNFS_EACCES;
    if (!over_limits())
        return TNFS_OK;
    /* Never refuse on a stale count: whatever drains the drop box removes
     * files behind our back, so rescan before saying no. */
    dropbox_scan();
    return over_limits() ? TNFS_EACCES : TNFS_OK;
}

int dropbox_check_write(uint64_t delta, uint64_t new_apparent)
{
    if (srv.max_file_size != 0 && new_apparent > srv.max_file_size)
        return TNFS_EFBIG;
    if (srv.max_total_bytes != 0 &&
        srv.db.bytes + srv.db.inflight_bytes + delta > srv.max_total_bytes)
        return TNFS_EFBIG;
    return TNFS_OK;
}

void dropbox_open_begin(void)
{
    srv.db.inflight_count++;
}

void dropbox_grew(uint64_t delta)
{
    srv.db.inflight_bytes += delta;
}

static void inflight_done(uint64_t apparent)
{
    if (srv.db.inflight_count > 0)
        srv.db.inflight_count--;
    if (srv.db.inflight_bytes >= apparent)
        srv.db.inflight_bytes -= apparent;
    else
        srv.db.inflight_bytes = 0;
}

void dropbox_close_commit(uint64_t apparent)
{
    inflight_done(apparent);
    srv.db.count++;
    srv.db.bytes += apparent;
}

void dropbox_close_abort(uint64_t apparent)
{
    inflight_done(apparent);
}

uint64_t dropbox_free_bytes(uint64_t fs_free)
{
    uint64_t used, quota_left;

    if (srv.max_total_bytes == 0)
        return fs_free;
    used = srv.db.bytes + srv.db.inflight_bytes;
    quota_left = (used >= srv.max_total_bytes) ? 0 : srv.max_total_bytes - used;
    return quota_left < fs_free ? quota_left : fs_free;
}

static int urandom_fd = -1;

void dropbox_init(void)
{
    /* Opened once at startup: after the confinement step there is no
     * filesystem path left to open it by. */
    urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (urandom_fd < 0)
        log_err("dropbox: /dev/urandom unavailable: %s", strerror(errno));
}

int dropbox_make_temp(char *buf, size_t bufsz)
{
    unsigned char rnd[8];
    int got = 0;

    if (urandom_fd >= 0)
        got = (read(urandom_fd, rnd, sizeof rnd) == (ssize_t)sizeof rnd);
    if (!got) {
        static uint64_t counter;
        uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ ++counter;
        memcpy(rnd, &seed, sizeof rnd);
    }
    if (snprintf(buf, bufsz, TMP_PREFIX "%02x%02x%02x%02x%02x%02x%02x%02x",
                 rnd[0], rnd[1], rnd[2], rnd[3],
                 rnd[4], rnd[5], rnd[6], rnd[7]) >= (int)bufsz)
        return -1;
    return 0;
}

/* ---- Per-IP leaky buckets ---------------------------------------------- */

#define RATE_SLOTS          64
#define RATE_UPLOADS_PER_MIN 30
#define RATE_BYTES_PER_MIN   (64ull * 1024 * 1024)

struct rate_slot {
    char     ip[64];
    time_t   last;
    double   uploads;       /* tokens consumed, drained over a minute */
    double   bytes;
};

static struct rate_slot rate_slots[RATE_SLOTS];

static struct rate_slot *rate_find(const char *ip)
{
    struct rate_slot *oldest = &rate_slots[0];
    time_t now = time(NULL);

    for (int i = 0; i < RATE_SLOTS; i++) {
        if (strcmp(rate_slots[i].ip, ip) == 0)
            return &rate_slots[i];
        if (rate_slots[i].last < oldest->last)
            oldest = &rate_slots[i];
    }
    memset(oldest, 0, sizeof *oldest);
    snprintf(oldest->ip, sizeof oldest->ip, "%s", ip);
    oldest->last = now;
    return oldest;
}

static void rate_drain(struct rate_slot *s, time_t now)
{
    double elapsed = (double)(now - s->last);

    if (elapsed <= 0)
        return;
    s->uploads -= elapsed * (RATE_UPLOADS_PER_MIN / 60.0);
    s->bytes   -= elapsed * ((double)RATE_BYTES_PER_MIN / 60.0);
    if (s->uploads < 0) s->uploads = 0;
    if (s->bytes   < 0) s->bytes   = 0;
    s->last = now;
}

int rate_allow_upload(const char *ip)
{
    time_t now = time(NULL);
    struct rate_slot *s = rate_find(ip);

    rate_drain(s, now);
    if (s->uploads + 1 > RATE_UPLOADS_PER_MIN)
        return 0;
    s->uploads += 1;
    return 1;
}

int rate_allow_bytes(const char *ip, uint64_t bytes)
{
    time_t now = time(NULL);
    struct rate_slot *s = rate_find(ip);

    rate_drain(s, now);
    if (s->bytes + (double)bytes > (double)RATE_BYTES_PER_MIN)
        return 0;
    s->bytes += (double)bytes;
    return 1;
}
