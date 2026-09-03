#include "handlers.h"

#include "dir.h"
#include "dropbox.h"
#include "log.h"
#include "server.h"
#include "session.h"
#include "tnfs.h"
#include "util.h"
#include "zone.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct req {
    struct session *s;
    uint16_t sid;
    uint8_t  seq;
    uint8_t  cmd;
    const uint8_t *data;        /* payload, after the 4-byte header */
    size_t   len;               /* payload length                   */
    uint8_t *out;
    size_t   outsz;
};

/* ---- reply construction ------------------------------------------------ */

static void put_header(struct req *q, uint16_t sid)
{
    put_u16(q->out, sid);
    q->out[2] = q->seq;
    q->out[3] = q->cmd;
}

/* Header plus a status byte: the shape of every refusal, and of every reply
 * that carries nothing else. */
static size_t reply_status(struct req *q, uint8_t status)
{
    put_header(q, q->sid);
    q->out[4] = status;
    return 5;
}

/* Header, status TNFS_OK, and n bytes of payload the caller has already
 * written at out+5. */
static size_t reply_ok(struct req *q, size_t n)
{
    put_header(q, q->sid);
    q->out[4] = TNFS_OK;
    return 5 + n;
}

static uint8_t *body(struct req *q)
{
    return q->out + 5;
}

/* Ask the client to come back later (used for rate limiting). */
static size_t reply_backoff(struct req *q, uint16_t ms)
{
    put_header(q, q->sid);
    q->out[4] = TNFS_EAGAIN;
    put_u16(q->out + 5, ms);
    return 7;
}

/* ---- helpers ----------------------------------------------------------- */

static int want_string(struct req *q, size_t *off, char *dst, size_t dstsz)
{
    return msg_get_string(q->data, q->len, off, dst, dstsz);
}

/* Refusal for every mutating command, decided by zone alone: the drop box
 * answers EACCES so its refusals are all alike, everything else EROFS. */
static uint8_t mutation_refusal(enum zone z)
{
    return (z == ZONE_INCOMING) ? TNFS_EACCES : TNFS_EROFS;
}

static int statvfs_zone(struct statvfs *vfs)
{
    int fd = (srv.inc_fd >= 0) ? srv.inc_fd : srv.pub_fd;
    return fstatvfs(fd, vfs);
}

/* ---- session ----------------------------------------------------------- */

static size_t h_mount(struct req *q, const struct sockaddr *peer,
                      socklen_t peerlen, int tcp_fd)
{
    char location[TNFS_MAX_PATH + 1], userid[64], password[64];
    size_t off = 0;
    struct session *s;
    int zone;
    const char *p;
    size_t n;

    /* Version(2), mount location, user id, password. The credentials are
     * accepted and ignored: access here is anonymous. */
    if (q->len < 2)
        return reply_status(q, TNFS_EINVAL);
    off = 2;
    if (want_string(q, &off, location, sizeof location) != 0)
        return reply_status(q, TNFS_EINVAL);
    if (want_string(q, &off, userid, sizeof userid) != 0)
        userid[0] = '\0';
    if (want_string(q, &off, password, sizeof password) != 0)
        password[0] = '\0';
    (void)userid;
    (void)password;

    /* Only "/", "/pub" and "/incoming" are mountable; a mount is resolved to
     * a zone once, so mounting /pub is the same as mounting / and prefixing. */
    p = location;
    while (*p == '/')
        p++;
    n = strlen(p);
    while (n > 0 && p[n - 1] == '/')
        n--;
    if (n == 0)
        zone = ZONE_ROOT;
    else if (n == 3 && strncmp(p, "pub", 3) == 0)
        zone = ZONE_PUB;
    else if (n == 8 && strncmp(p, "incoming", 8) == 0)
        zone = ZONE_INCOMING;
    else
        zone = -1;

    if (zone == ZONE_INCOMING && srv.inc_fd < 0)
        zone = -1;

    if (zone < 0) {
        /* A failed MOUNT still reports the server's protocol version. */
        put_u16(q->out, 0);
        q->out[2] = q->seq;
        q->out[3] = q->cmd;
        q->out[4] = TNFS_ENOENT;
        q->out[5] = TNFS_VERSION_MINOR;
        q->out[6] = TNFS_VERSION_MAJOR;
        return 7;
    }

    s = session_new(peer, peerlen, tcp_fd, zone);
    if (s == NULL) {
        put_u16(q->out, 0);
        q->out[2] = q->seq;
        q->out[3] = q->cmd;
        q->out[4] = TNFS_EUSERS;
        q->out[5] = TNFS_VERSION_MINOR;
        q->out[6] = TNFS_VERSION_MAJOR;
        return 7;
    }

    log_info("mount sid=0x%04x ip=%s zone=%s transport=%s",
             s->sid, s->ip, zone_name((enum zone)zone),
             tcp_fd >= 0 ? "tcp" : "udp");

    put_u16(q->out, s->sid);
    q->out[2] = q->seq;
    q->out[3] = q->cmd;
    q->out[4] = TNFS_OK;
    q->out[5] = TNFS_VERSION_MINOR;
    q->out[6] = TNFS_VERSION_MAJOR;
    put_u16(q->out + 7, TNFS_MIN_RETRY_MS);
    return 9;
}

static size_t h_umount(struct req *q)
{
    log_info("umount sid=0x%04x ip=%s", q->s->sid, q->s->ip);
    session_close(q->s);
    q->s = NULL;
    return reply_status(q, TNFS_OK);
}

/* ---- directories ------------------------------------------------------- */

/* Open a snapshot of the directory the path names, honouring the zone's LIST
 * capability. Shared by OPENDIR and OPENDIRX. */
static uint8_t opendir_common(struct req *q, const char *path,
                              const char *pattern, uint8_t diropt,
                              uint8_t dirsort, uint16_t maxres,
                              int *handle_out, size_t *count_out)
{
    struct resolved r;
    struct dir_list list;
    int rc, handle, fd;

    rc = path_resolve(q->s->mount_zone, path, &r);
    if (rc != TNFS_OK)
        return (uint8_t)rc;

    /* The drop box has no LIST capability, so this is refused here, before
     * any syscall: there is no code path in which the daemon calls opendir()
     * on it and lets the kernel decide. EACCES rather than ENOENT, because
     * the directory's existence is not the secret - its contents are. */
    if (!(r.caps & CAP_LIST)) {
        resolved_release(&r);
        return TNFS_EACCES;
    }

    if (r.zone == ZONE_ROOT) {
        resolved_release(&r);
        rc = dir_synthetic_root(&list);
        if (rc != TNFS_OK)
            return (uint8_t)rc;
    } else {
        if (r.has_leaf)
            fd = openat(r.dirfd, r.leaf,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        else
            fd = openat(r.dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            int e = errno;
            resolved_release(&r);
            return (uint8_t)tnfs_errno(e);
        }
        resolved_release(&r);
        rc = dir_snapshot(fd, pattern, diropt, dirsort, maxres, &list);
        if (rc != TNFS_OK)
            return (uint8_t)rc;
    }

    handle = session_alloc_dir(q->s);
    if (handle < 0) {
        dir_free(&list);
        return TNFS_EMFILE;
    }
    q->s->dirs[handle].list = list;
    *handle_out = handle;
    *count_out = list.n;
    return TNFS_OK;
}

static size_t h_opendir(struct req *q)
{
    char path[TNFS_MAX_PATH + 1];
    size_t off = 0, count = 0;
    int handle = -1;
    uint8_t st;

    if (want_string(q, &off, path, sizeof path) != 0)
        return reply_status(q, TNFS_EINVAL);

    /* Plain OPENDIR lists everything the directory holds, including dotfiles;
     * only OPENDIRX applies the hidden/special defaults. */
    st = opendir_common(q, path, NULL,
                        TNFS_DIROPT_NO_SKIPHIDDEN, 0, 0, &handle, &count);
    if (st != TNFS_OK)
        return reply_status(q, st);

    body(q)[0] = (uint8_t)handle;
    return reply_ok(q, 1);
}

static size_t h_opendirx(struct req *q)
{
    char pattern[TNFS_MAX_NAME + 1], path[TNFS_MAX_PATH + 1];
    size_t off, count = 0;
    int handle = -1;
    uint8_t diropt, dirsort, st;
    uint16_t maxres;

    if (q->len < 4)
        return reply_status(q, TNFS_EINVAL);
    diropt  = q->data[0];
    dirsort = q->data[1];
    maxres  = get_u16(q->data + 2);
    off = 4;
    if (want_string(q, &off, pattern, sizeof pattern) != 0)
        return reply_status(q, TNFS_EINVAL);
    if (want_string(q, &off, path, sizeof path) != 0)
        return reply_status(q, TNFS_EINVAL);

    /* TNFS_DIROPT_TRAVERSE is accepted and ignored: a handle lists exactly
     * one directory. A client that asked for a subtree gets a normal
     * single-level listing rather than an error. */
    diropt &= (uint8_t)~TNFS_DIROPT_TRAVERSE;

    st = opendir_common(q, path, pattern, diropt, dirsort, maxres,
                        &handle, &count);
    if (st != TNFS_OK)
        return reply_status(q, st);

    body(q)[0] = (uint8_t)handle;
    /* Bytes 6-7: number of matching entries, 16-bit little endian. */
    put_u16(body(q) + 1, count > 0xffff ? 0xffff : (uint16_t)count);
    return reply_ok(q, 3);
}

static struct dir_slot *dir_handle(struct req *q, uint8_t h)
{
    if (h >= MAX_DIR_HANDLES || !q->s->dirs[h].in_use)
        return NULL;
    return &q->s->dirs[h];
}

static size_t h_readdir(struct req *q)
{
    struct dir_slot *d;
    struct dirent_snap *e;
    size_t namelen;

    if (q->len < 1)
        return reply_status(q, TNFS_EINVAL);
    d = dir_handle(q, q->data[0]);
    if (d == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    if (d->list.pos >= d->list.n)
        return reply_status(q, TNFS_EOF);

    e = &d->list.ents[d->list.pos++];
    namelen = strlen(e->name) + 1;
    memcpy(body(q), e->name, namelen);
    return reply_ok(q, namelen);
}

static size_t h_readdirx(struct req *q)
{
    struct dir_slot *d;
    uint8_t want, *p, *count_p, *status_p;
    size_t written = 0, emitted = 0, first_pos;

    if (q->len < 2)
        return reply_status(q, TNFS_EINVAL);
    d = dir_handle(q, q->data[0]);
    if (d == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    if (d->list.pos >= d->list.n)
        return reply_status(q, TNFS_EOF);

    want = q->data[1];
    first_pos = d->list.pos;

    p = body(q);
    count_p  = p++;
    status_p = p++;
    put_u16(p, first_pos > 0xffff ? 0xffff : (uint16_t)first_pos);
    p += 2;
    written = 4;

    while (d->list.pos < d->list.n && (want == 0 || emitted < want)) {
        struct dirent_snap *e = &d->list.ents[d->list.pos];
        size_t need = 1 + 4 + 4 + 4 + strlen(e->name) + 1;

        if (written + need > TNFS_MAX_PAYLOAD)
            break;
        if (emitted == 0xff)
            break;

        *p++ = e->flags;
        put_u32(p, e->size);  p += 4;
        put_u32(p, e->mtime); p += 4;
        put_u32(p, e->ctime); p += 4;
        memcpy(p, e->name, strlen(e->name) + 1);
        p += strlen(e->name) + 1;

        written += need;
        emitted++;
        d->list.pos++;
    }

    if (emitted == 0)               /* one entry was too big to fit at all */
        return reply_status(q, TNFS_EOF);

    *count_p  = (uint8_t)emitted;
    /* Tell the client the directory ended here, sparing it a round trip. */
    *status_p = (d->list.pos >= d->list.n) ? TNFS_DIRSTATUS_EOF : 0;
    return reply_ok(q, written);
}

static size_t h_telldir(struct req *q)
{
    struct dir_slot *d;

    if (q->len < 1)
        return reply_status(q, TNFS_EINVAL);
    d = dir_handle(q, q->data[0]);
    if (d == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    put_u32(body(q), (uint32_t)d->list.pos);
    return reply_ok(q, 4);
}

static size_t h_seekdir(struct req *q)
{
    struct dir_slot *d;
    uint32_t pos;

    if (q->len < 5)
        return reply_status(q, TNFS_EINVAL);
    d = dir_handle(q, q->data[0]);
    if (d == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    pos = get_u32(q->data + 1);
    if (pos > d->list.n)
        return reply_status(q, TNFS_EINVAL);
    d->list.pos = pos;
    return reply_status(q, TNFS_OK);
}

static size_t h_closedir(struct req *q)
{
    struct dir_slot *d;

    if (q->len < 1)
        return reply_status(q, TNFS_EINVAL);
    d = dir_handle(q, q->data[0]);
    if (d == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    dir_free(&d->list);
    memset(d, 0, sizeof *d);
    return reply_status(q, TNFS_OK);
}

/* MKDIR and RMDIR are refused everywhere: the drop box is flat and pub is
 * read-only, so there is no zone with a directory-creating capability. */
static size_t h_mk_rmdir(struct req *q)
{
    char path[TNFS_MAX_PATH + 1];
    struct resolved r;
    size_t off = 0;
    int rc;

    if (want_string(q, &off, path, sizeof path) != 0)
        return reply_status(q, TNFS_EINVAL);
    rc = path_resolve(q->s->mount_zone, path, &r);
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);
    resolved_release(&r);
    return reply_status(q, mutation_refusal(r.zone));
}

/* ---- files ------------------------------------------------------------- */

static struct file_slot *file_handle(struct req *q, uint8_t h)
{
    if (h >= MAX_FILE_HANDLES || !q->s->files[h].in_use)
        return NULL;
    return &q->s->files[h];
}

static void stat_reply(struct req *q, uint16_t mode, uint32_t size,
                       uint32_t atime, uint32_t mtime, uint32_t ctime,
                       size_t *len)
{
    uint8_t *p = body(q);

    put_u16(p, mode);      p += 2;
    put_u16(p, 0);         p += 2;   /* uid: anonymous access, so no owner */
    put_u16(p, 0);         p += 2;   /* gid */
    put_u32(p, size);      p += 4;
    put_u32(p, atime);     p += 4;
    put_u32(p, mtime);     p += 4;
    put_u32(p, ctime);     p += 4;
    *p++ = '\0';                     /* uidstring */
    *p++ = '\0';                     /* gidstring */
    *len = (size_t)(p - body(q));
}

static size_t h_stat(struct req *q)
{
    char path[TNFS_MAX_PATH + 1];
    struct resolved r;
    struct stat st;
    size_t off = 0, len;
    int rc;

    if (want_string(q, &off, path, sizeof path) != 0)
        return reply_status(q, TNFS_EINVAL);
    rc = path_resolve(q->s->mount_zone, path, &r);
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);

    /* A zone root always stats as a directory, whatever its mode on disk;
     * the drop box has to be stat-able for a client to know where to upload. */
    if (!r.has_leaf) {
        int fd = (r.zone == ZONE_ROOT) ? srv.root_fd
               : (r.zone == ZONE_PUB)  ? srv.pub_fd : srv.inc_fd;
        uint16_t mode = (uint16_t)(S_IFDIR |
                        (r.zone == ZONE_INCOMING ? 0311 : 0555));
        resolved_release(&r);
        if (fstat(fd, &st) != 0)
            return reply_status(q, (uint8_t)tnfs_errno(errno));
        stat_reply(q, mode, 0, (uint32_t)st.st_atime,
                   (uint32_t)st.st_mtime, (uint32_t)st.st_ctime, &len);
        return reply_ok(q, len);
    }

    /* A name inside the drop box: the zone has no LOOKUP, so the daemon does
     * not go and find out whether it exists. */
    if (!(r.caps & CAP_LOOKUP)) {
        resolved_release(&r);
        return reply_status(q, TNFS_EACCES);
    }

    if (fstatat(r.dirfd, r.leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        int e = errno;
        resolved_release(&r);
        return reply_status(q, (uint8_t)tnfs_errno(e));
    }
    resolved_release(&r);

    /* A symlink is never served, so it does not exist as far as a client is
     * concerned. */
    if (S_ISLNK(st.st_mode))
        return reply_status(q, TNFS_ENOENT);

    stat_reply(q, (uint16_t)(st.st_mode & ~0222u),   /* pub is read-only */
               st.st_size > 0xffffffffLL ? 0xffffffffu : (uint32_t)st.st_size,
               (uint32_t)st.st_atime, (uint32_t)st.st_mtime,
               (uint32_t)st.st_ctime, &len);
    return reply_ok(q, len);
}

static size_t open_in_pub(struct req *q, struct resolved *r, uint16_t flags)
{
    struct stat st;
    int fd, handle;

    /* Anything that could write is refused before the open, not by the
     * kernel: pub has no CREATE and no MODIFY capability. */
    if ((flags & TNFS_O_ACCMODE) != TNFS_O_RDONLY ||
        (flags & (TNFS_O_APPEND | TNFS_O_CREAT | TNFS_O_TRUNC | TNFS_O_EXCL)))
        return reply_status(q, TNFS_EROFS);
    if (!(r->caps & CAP_READ))
        return reply_status(q, TNFS_EACCES);

    fd = openat(r->dirfd, r->leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return reply_status(q, (uint8_t)tnfs_errno(errno));
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return reply_status(q, S_ISDIR(st.st_mode) ? TNFS_EISDIR : TNFS_ENOENT);
    }

    handle = session_alloc_file(q->s);
    if (handle < 0) {
        close(fd);
        return reply_status(q, TNFS_EMFILE);
    }
    q->s->files[handle].fd = fd;
    q->s->files[handle].caps = CAP_READ;
    q->s->files[handle].zone = ZONE_PUB;
    q->s->files[handle].opened = time(NULL);
    q->s->files[handle].last_active = q->s->files[handle].opened;

    body(q)[0] = (uint8_t)handle;
    return reply_ok(q, 1);
}

static size_t open_in_dropbox(struct req *q, struct resolved *r, uint16_t flags)
{
    struct file_slot *f;
    struct stat st;
    char tmp[sizeof f->tmpname];
    int fd, handle, rc;

    /* Read is not a thing that happens here. O_RDWR is quietly downgraded to
     * write-only, because several clients ask for it by default. */
    if ((flags & TNFS_O_ACCMODE) == TNFS_O_RDONLY ||
        (flags & TNFS_O_ACCMODE) == 0)
        return reply_status(q, TNFS_EACCES);
    if (!(flags & TNFS_O_CREAT))
        return reply_status(q, TNFS_EACCES);
    /* O_APPEND only makes sense against a file that already exists, which a
     * drop-box upload never is. O_TRUNC is accepted rather than refused: real
     * FujiNet clients (fujinet-pc and the ESP32 firmware's NetworkProtocolTNFS)
     * unconditionally OR O_TRUNC into every write-mode open regardless of
     * whether the target exists, so refusing it makes every upload from an
     * actual FujiNet client fail. It changes nothing here either way -- O_EXCL
     * is forced on below regardless of what the client asked for, so the real
     * open always creates a fresh file and O_TRUNC is a no-op on it. */
    if (flags & TNFS_O_APPEND)
        return reply_status(q, TNFS_EACCES);

    /* Name syntax is a property of the request, not of the directory, so it
     * is the one refusal here that is allowed to look different. */
    if (!name_is_valid_upload(r->leaf))
        return reply_status(q, TNFS_EINVAL);

    if (!rate_allow_upload(q->s->ip))
        return reply_backoff(q, 2000);
    if (session_open_uploads(q->s) >= MAX_UPLOADS_PER_SESSION)
        return reply_status(q, TNFS_EACCES);

    rc = dropbox_check_open();
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);

    /* The daemon may look; the client may not. A taken name is refused with
     * the zone's uniform EACCES rather than EEXIST. */
    if (fstatat(srv.inc_fd, r->leaf, &st, AT_SYMLINK_NOFOLLOW) == 0)
        return reply_status(q, TNFS_EACCES);

    if (dropbox_make_temp(tmp, sizeof tmp) != 0)
        return reply_status(q, TNFS_EIO);

    /* O_EXCL is forced on regardless of what the client asked for; with
     * O_NOFOLLOW, a symlink planted here cannot redirect the write. */
    fd = openat(srv.inc_fd, tmp,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0660);
    if (fd < 0)
        return reply_status(q, (uint8_t)tnfs_errno(errno));

    handle = session_alloc_file(q->s);
    if (handle < 0) {
        close(fd);
        unlinkat(srv.inc_fd, tmp, 0);
        return reply_status(q, TNFS_EMFILE);
    }

    f = &q->s->files[handle];
    f->fd = fd;
    f->caps = CAP_CREATE;
    f->zone = ZONE_INCOMING;
    f->is_upload = 1;
    f->opened = time(NULL);
    f->last_active = f->opened;
    snprintf(f->tmpname, sizeof f->tmpname, "%s", tmp);
    snprintf(f->target, sizeof f->target, "%s", r->leaf);
    dropbox_open_begin();

    log_info("upload start ip=%s name=%s", q->s->ip, f->target);

    body(q)[0] = (uint8_t)handle;
    return reply_ok(q, 1);
}

static size_t h_open(struct req *q)
{
    char path[TNFS_MAX_PATH + 1];
    struct resolved r;
    size_t off, len;
    uint16_t flags;
    int rc;

    /* OPENFILE_OLD (0x20) predates the wider flag field. The specification
     * does not give its layout, so it is parsed like OPEN; in this daemon the
     * only consequence of a client meaning something narrower is which
     * refusal it gets, never whether a write is allowed. */
    if (q->len < 4)
        return reply_status(q, TNFS_EINVAL);
    flags = get_u16(q->data);
    /* q->data + 2 is the client's mode argument, which is ignored: files are
     * created 0660 as a default, not as a control. */
    off = 4;
    if (want_string(q, &off, path, sizeof path) != 0)
        return reply_status(q, TNFS_EINVAL);

    rc = path_resolve(q->s->mount_zone, path, &r);
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);
    if (!r.has_leaf) {
        resolved_release(&r);
        return reply_status(q, TNFS_EISDIR);
    }

    if (r.zone == ZONE_INCOMING)
        len = open_in_dropbox(q, &r, flags);
    else
        len = open_in_pub(q, &r, flags);
    resolved_release(&r);
    return len;
}

static size_t h_read(struct req *q)
{
    struct file_slot *f;
    uint16_t want;
    ssize_t n;

    if (q->len < 3)
        return reply_status(q, TNFS_EINVAL);
    f = file_handle(q, q->data[0]);
    if (f == NULL)
        return reply_status(q, TNFS_EBADHANDLE);

    /* The check is on the descriptor's own capabilities; no path is consulted
     * again after OPEN. A drop-box descriptor has CAP_CREATE only - and is
     * O_WRONLY underneath, so the kernel would refuse this too. */
    if (!(f->caps & CAP_READ))
        return reply_status(q, TNFS_EACCES);

    want = get_u16(q->data + 1);
    if (want > TNFS_MAX_PAYLOAD)
        want = TNFS_MAX_PAYLOAD;

    n = read(f->fd, body(q) + 2, want);
    if (n < 0)
        return reply_status(q, (uint8_t)tnfs_errno(errno));
    if (n == 0)
        return reply_status(q, TNFS_EOF);

    f->last_active = time(NULL);
    put_u16(body(q), (uint16_t)n);
    return reply_ok(q, 2 + (size_t)n);
}

/* Abandon an upload: the bytes go now, and CLOSE reports why. */
static void abort_upload(struct session *s, struct file_slot *f, uint8_t status)
{
    if (f->fd >= 0) {
        close(f->fd);
        f->fd = -1;
    }
    unlinkat(srv.inc_fd, f->tmpname, 0);
    f->dead = 1;
    f->close_status = status;
    (void)s;
}

static size_t h_write(struct req *q)
{
    struct file_slot *f;
    uint16_t want;
    const uint8_t *data;
    uint64_t new_apparent, delta;
    off_t pos;
    size_t done = 0;
    int rc;

    if (q->len < 3)
        return reply_status(q, TNFS_EINVAL);
    f = file_handle(q, q->data[0]);
    if (f == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    if (!(f->caps & CAP_CREATE))
        return reply_status(q, TNFS_EROFS);
    if (f->dead)
        return reply_status(q, f->close_status);

    want = get_u16(q->data + 1);
    if ((size_t)want + 3 > q->len)
        return reply_status(q, TNFS_EINVAL);
    if (want > TNFS_MAX_PAYLOAD)
        return reply_status(q, TNFS_EINVAL);
    data = q->data + 3;

    if (!rate_allow_bytes(q->s->ip, want))
        return reply_backoff(q, 2000);

    pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return reply_status(q, (uint8_t)tnfs_errno(errno));

    /* Apparent size, not bytes transferred: a client that seeks to 4G and
     * writes one byte has made a 4G file. */
    new_apparent = (uint64_t)pos + want;
    if (new_apparent < f->apparent)
        new_apparent = f->apparent;
    delta = new_apparent - f->apparent;

    rc = dropbox_check_write(delta, new_apparent);
    if (rc != TNFS_OK) {
        abort_upload(q->s, f, (uint8_t)rc);
        return reply_status(q, (uint8_t)rc);
    }

    while (done < want) {
        ssize_t n = write(f->fd, data + done, (size_t)want - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            abort_upload(q->s, f, (uint8_t)tnfs_errno(errno));
            return reply_status(q, f->close_status);
        }
        done += (size_t)n;
    }

    f->apparent = new_apparent;
    f->last_active = time(NULL);
    dropbox_grew(delta);

    put_u16(body(q), (uint16_t)done);
    return reply_ok(q, 2);
}

static size_t h_close(struct req *q)
{
    uint8_t h;
    int rc;

    if (q->len < 1)
        return reply_status(q, TNFS_EINVAL);
    h = q->data[0];
    if (file_handle(q, h) == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    rc = session_close_file(q->s, h, 1);
    return reply_status(q, (uint8_t)rc);
}

static size_t h_lseek(struct req *q)
{
    struct file_slot *f;
    int32_t offset;
    int whence;
    off_t pos;

    if (q->len < 6)
        return reply_status(q, TNFS_EINVAL);
    f = file_handle(q, q->data[0]);
    if (f == NULL)
        return reply_status(q, TNFS_EBADHANDLE);
    if (f->dead)
        return reply_status(q, f->close_status);

    switch (q->data[1]) {
    case TNFS_SEEK_SET: whence = SEEK_SET; break;
    case TNFS_SEEK_CUR: whence = SEEK_CUR; break;
    case TNFS_SEEK_END: whence = SEEK_END; break;
    default: return reply_status(q, TNFS_EINVAL);
    }
    offset = (int32_t)get_u32(q->data + 2);

    pos = lseek(f->fd, (off_t)offset, whence);
    if (pos < 0)
        return reply_status(q, (uint8_t)tnfs_errno(errno));

    f->last_active = time(NULL);
    put_u32(body(q), (uint32_t)pos);
    return reply_ok(q, 4);
}

/* UNLINK, CHMOD and RENAME are refused in every zone. RENAME in particular is
 * unimplemented in both directions: a rename that could move a file out of
 * the drop box would be a read primitive for it, and deciding which
 * combinations are safe is not worth doing when none are needed. */
static size_t h_refuse_path(struct req *q, size_t skip)
{
    char path[TNFS_MAX_PATH + 1];
    struct resolved r;
    size_t off = skip;
    int rc;

    if (q->len < skip)
        return reply_status(q, TNFS_EINVAL);
    if (want_string(q, &off, path, sizeof path) != 0)
        return reply_status(q, TNFS_EINVAL);
    rc = path_resolve(q->s->mount_zone, path, &r);
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);
    resolved_release(&r);
    return reply_status(q, mutation_refusal(r.zone));
}

static size_t h_rename(struct req *q)
{
    char from[TNFS_MAX_PATH + 1], to[TNFS_MAX_PATH + 1];
    struct resolved a, b;
    size_t off = 0;
    int rc;

    if (want_string(q, &off, from, sizeof from) != 0 ||
        want_string(q, &off, to, sizeof to) != 0)
        return reply_status(q, TNFS_EINVAL);

    rc = path_resolve(q->s->mount_zone, from, &a);
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);
    resolved_release(&a);
    rc = path_resolve(q->s->mount_zone, to, &b);
    if (rc != TNFS_OK)
        return reply_status(q, (uint8_t)rc);
    resolved_release(&b);

    /* If either end touches the drop box, the refusal is the drop box's. */
    if (a.zone == ZONE_INCOMING || b.zone == ZONE_INCOMING)
        return reply_status(q, TNFS_EACCES);
    return reply_status(q, TNFS_EROFS);
}

/* ---- devices ----------------------------------------------------------- */

static size_t h_device(struct req *q)
{
    struct statvfs vfs;
    uint64_t total, freeb;

    if (statvfs_zone(&vfs) != 0)
        return reply_status(q, (uint8_t)tnfs_errno(errno));

    total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    /* Free space is capped at the remaining drop-box quota: it answers "will
     * my upload fit" more accurately and does not hand out a running total of
     * everything the volume holds. */
    if (srv.inc_fd >= 0)
        freeb = dropbox_free_bytes(freeb);

    switch (q->cmd) {
    case TNFS_SIZE: {
        uint64_t kb = total / 1024;
        put_u32(body(q), kb > 0xffffffffu ? 0xffffffffu : (uint32_t)kb);
        return reply_ok(q, 4);
    }
    case TNFS_FREE: {
        uint64_t kb = freeb / 1024;
        put_u32(body(q), kb > 0xffffffffu ? 0xffffffffu : (uint32_t)kb);
        return reply_ok(q, 4);
    }
    case TNFS_SIZEBYTES:
        put_u64(body(q), total);
        return reply_ok(q, 8);
    case TNFS_FREEBYTES:
        put_u64(body(q), freeb);
        return reply_ok(q, 8);
    }
    return reply_status(q, TNFS_ENOSYS);
}

/* ---- dispatch ---------------------------------------------------------- */

size_t tnfs_handle(const uint8_t *msg, size_t len,
                   const struct sockaddr *peer, socklen_t peerlen,
                   int tcp_fd, uint8_t *out, size_t outsz)
{
    struct req q;
    struct session *s;
    size_t n;

    if (len < TNFS_HEADER_SIZE)
        return 0;

    memset(&q, 0, sizeof q);
    q.sid   = get_u16(msg);
    q.seq   = msg[2];
    q.cmd   = msg[3];
    q.data  = msg + TNFS_HEADER_SIZE;
    q.len   = len - TNFS_HEADER_SIZE;
    q.out   = out;
    q.outsz = outsz;

    if (q.cmd == TNFS_MOUNT)
        return h_mount(&q, peer, peerlen, tcp_fd);

    s = session_find(q.sid, peer);
    if (s == NULL) {
        put_u16(out, q.sid);
        out[2] = q.seq;
        out[3] = q.cmd;
        out[4] = TNFS_EBADHANDLE;
        return 5;
    }
    q.s = s;
    s->last_active = time(NULL);
    s->tcp_fd = tcp_fd;
    memcpy(&s->peer, peer, peerlen);
    s->peerlen = peerlen;

    /* A repeat of the last sequence number is a retry, not a new request:
     * resend the same answer rather than acting twice. */
    if (s->have_last && s->last_seq == q.seq && s->last_cmd == q.cmd) {
        memcpy(out, s->last_reply, s->last_reply_len);
        return s->last_reply_len;
    }

    switch (q.cmd) {
    case TNFS_UMOUNT:       n = h_umount(&q); break;
    case TNFS_OPENDIR:      n = h_opendir(&q); break;
    case TNFS_OPENDIRX:     n = h_opendirx(&q); break;
    case TNFS_READDIR:      n = h_readdir(&q); break;
    case TNFS_READDIRX:     n = h_readdirx(&q); break;
    case TNFS_TELLDIR:      n = h_telldir(&q); break;
    case TNFS_SEEKDIR:      n = h_seekdir(&q); break;
    case TNFS_CLOSEDIR:     n = h_closedir(&q); break;
    case TNFS_MKDIR:        /* fall through */
    case TNFS_RMDIR:        n = h_mk_rmdir(&q); break;
    case TNFS_OPEN:         /* fall through */
    case TNFS_OPENFILE_OLD: n = h_open(&q); break;
    case TNFS_READ:         n = h_read(&q); break;
    case TNFS_WRITE:        n = h_write(&q); break;
    case TNFS_CLOSE:        n = h_close(&q); break;
    case TNFS_STAT:         n = h_stat(&q); break;
    case TNFS_LSEEK:        n = h_lseek(&q); break;
    case TNFS_UNLINK:       n = h_refuse_path(&q, 0); break;
    case TNFS_CHMOD:        n = h_refuse_path(&q, 2); break;
    case TNFS_RENAME:       n = h_rename(&q); break;
    case TNFS_SIZE:
    case TNFS_FREE:
    case TNFS_SIZEBYTES:
    case TNFS_FREEBYTES:    n = h_device(&q); break;
    default:                n = reply_status(&q, TNFS_ENOSYS); break;
    }

    /* q.s is NULL after UMOUNT tore the session down. */
    if (q.s != NULL && n > 0 && n <= sizeof s->last_reply) {
        memcpy(s->last_reply, out, n);
        s->last_reply_len = n;
        s->last_seq = q.seq;
        s->last_cmd = q.cmd;
        s->have_last = 1;
    }
    return n;
}
