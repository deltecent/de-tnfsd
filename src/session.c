#include "session.h"

#include "dropbox.h"
#include "log.h"
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct session sessions[MAX_SESSIONS];
static uint16_t next_sid = 1;

void sockaddr_ip(const struct sockaddr *sa, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &in->sin_addr, buf, (socklen_t)bufsz);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &in6->sin6_addr, buf, (socklen_t)bufsz);
    }
}

int sockaddr_same_ip(const struct sockaddr *a, const struct sockaddr *b)
{
    if (a->sa_family != b->sa_family)
        return 0;
    if (a->sa_family == AF_INET)
        return ((const struct sockaddr_in *)a)->sin_addr.s_addr ==
               ((const struct sockaddr_in *)b)->sin_addr.s_addr;
    if (a->sa_family == AF_INET6)
        return memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr,
                      &((const struct sockaddr_in6 *)b)->sin6_addr,
                      sizeof(struct in6_addr)) == 0;
    return 0;
}

static uint16_t alloc_sid(void)
{
    for (int tries = 0; tries < 65535; tries++) {
        uint16_t candidate = next_sid++;
        int taken = 0;

        if (next_sid == 0)
            next_sid = 1;
        if (candidate == 0)
            continue;
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (sessions[i].in_use && sessions[i].sid == candidate)
                taken = 1;
        if (!taken)
            return candidate;
    }
    return 0;
}

struct session *session_new(const struct sockaddr *peer, socklen_t peerlen,
                            int tcp_fd, int mount_zone)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        struct session *s = &sessions[i];

        if (s->in_use)
            continue;
        memset(s, 0, sizeof *s);
        s->sid = alloc_sid();
        if (s->sid == 0)
            return NULL;
        s->in_use = 1;
        s->mount_zone = mount_zone;
        memcpy(&s->peer, peer, peerlen);
        s->peerlen = peerlen;
        s->tcp_fd = tcp_fd;
        s->last_active = time(NULL);
        sockaddr_ip(peer, s->ip, sizeof s->ip);
        for (int f = 0; f < MAX_FILE_HANDLES; f++)
            s->files[f].fd = -1;
        return s;
    }
    return NULL;
}

struct session *session_find(uint16_t sid, const struct sockaddr *peer)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        struct session *s = &sessions[i];

        if (!s->in_use || s->sid != sid)
            continue;
        /* The session is bound to the source IP, as the protocol's own
         * session ids are guessable. On UDP this is weak against a spoofing
         * attacker and is not treated as a security boundary. */
        if (!sockaddr_same_ip((const struct sockaddr *)&s->peer, peer))
            return NULL;
        return s;
    }
    return NULL;
}

int session_open_uploads(const struct session *s)
{
    int n = 0;

    for (int i = 0; i < MAX_FILE_HANDLES; i++)
        if (s->files[i].in_use && s->files[i].is_upload)
            n++;
    return n;
}

int session_alloc_dir(struct session *s)
{
    for (int i = 0; i < MAX_DIR_HANDLES; i++)
        if (!s->dirs[i].in_use) {
            memset(&s->dirs[i], 0, sizeof s->dirs[i]);
            s->dirs[i].in_use = 1;
            return i;
        }
    return -1;
}

int session_alloc_file(struct session *s)
{
    for (int i = 0; i < MAX_FILE_HANDLES; i++)
        if (!s->files[i].in_use) {
            memset(&s->files[i], 0, sizeof s->files[i]);
            s->files[i].in_use = 1;
            s->files[i].fd = -1;
            return i;
        }
    return -1;
}

int session_close_file(struct session *s, int handle, int commit)
{
    struct file_slot *f;
    int status = TNFS_OK;

    if (handle < 0 || handle >= MAX_FILE_HANDLES || !s->files[handle].in_use)
        return TNFS_EBADF;
    f = &s->files[handle];

    if (f->fd >= 0)
        close(f->fd);

    if (f->is_upload) {
        if (f->dead) {
            unlinkat(srv.inc_fd, f->tmpname, 0);
            dropbox_close_abort(f->apparent);
            status = f->close_status ? f->close_status : TNFS_EIO;
            log_info("upload aborted ip=%s name=%s size=%llu status=0x%02x",
                     s->ip, f->target, (unsigned long long)f->apparent, status);
        } else if (!commit) {
            unlinkat(srv.inc_fd, f->tmpname, 0);
            dropbox_close_abort(f->apparent);
            log_info("upload cancelled ip=%s name=%s size=%llu",
                     s->ip, f->target, (unsigned long long)f->apparent);
        } else if (linkat(srv.inc_fd, f->tmpname, srv.inc_fd, f->target, 0) != 0) {
            /* linkat rather than renameat: rename would silently replace a
             * file that appeared under the target name while the upload was
             * in flight, and overwriting is the one thing this zone must
             * never do. A collision is the same EACCES as every other
             * refusal here. */
            int e = errno;
            unlinkat(srv.inc_fd, f->tmpname, 0);
            dropbox_close_abort(f->apparent);
            status = TNFS_EACCES;
            log_info("upload rename failed ip=%s name=%s size=%llu err=%s",
                     s->ip, f->target, (unsigned long long)f->apparent,
                     strerror(e));
        } else {
            unlinkat(srv.inc_fd, f->tmpname, 0);
            dropbox_close_commit(f->apparent);
            log_info("upload ok ip=%s name=%s size=%llu duration=%llds",
                     s->ip, f->target, (unsigned long long)f->apparent,
                     (long long)(time(NULL) - f->opened));
        }
    }

    memset(f, 0, sizeof *f);
    f->fd = -1;
    return status;
}

void session_close(struct session *s)
{
    if (!s->in_use)
        return;
    for (int i = 0; i < MAX_DIR_HANDLES; i++)
        if (s->dirs[i].in_use)
            dir_free(&s->dirs[i].list);
    for (int i = 0; i < MAX_FILE_HANDLES; i++)
        if (s->files[i].in_use)
            session_close_file(s, i, 0);   /* an unclosed upload is discarded */
    memset(s, 0, sizeof *s);
}

void session_close_tcp(int fd)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].in_use && sessions[i].tcp_fd == fd)
            session_close(&sessions[i]);
}

void session_close_all(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].in_use)
            session_close(&sessions[i]);
}

void session_expire(time_t now)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        struct session *s = &sessions[i];
        int uploads;

        if (!s->in_use)
            continue;
        uploads = session_open_uploads(s);
        if (now - s->last_active >
            (uploads > 0 ? UPLOAD_IDLE_SEC : SESSION_IDLE_SEC)) {
            log_info("session expired sid=0x%04x ip=%s uploads=%d",
                     s->sid, s->ip, uploads);
            session_close(s);
        }
    }
}
