/* session.h - per-client state. Everything a request touches lives here or
 * on the stack; there are no shared scratch buffers.
 */
#ifndef DE_SESSION_H
#define DE_SESSION_H

#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

#include "dir.h"
#include "tnfs.h"
#include "zone.h"

#define MAX_SESSIONS       64
#define MAX_DIR_HANDLES     8
#define MAX_FILE_HANDLES    8

/* A session with no open upload may idle this long; one holding an upload
 * slot is cut off sooner, so a stalled transfer releases the slot. */
#define SESSION_IDLE_SEC   600
#define UPLOAD_IDLE_SEC    120

struct file_slot {
    int      in_use;
    int      fd;
    unsigned caps;              /* the fd's own capabilities, not a path's */
    enum zone zone;

    /* Drop-box upload bookkeeping. */
    int      is_upload;
    char     tmpname[32];
    char     target[TNFS_MAX_NAME + 1];
    uint64_t apparent;          /* highest offset written + length         */
    int      dead;              /* aborted; CLOSE reports close_status     */
    uint8_t  close_status;

    /* Download bookkeeping. The path is the one the client asked for, not a
     * reconstruction: pub is a tree, so the leaf alone would not say which
     * file was served. */
    char     path[TNFS_MAX_PATH + 1];
    uint64_t size;              /* file size at OPEN                       */
    uint64_t nread;             /* bytes handed to the client              */
    int      read_failed;       /* a READ errored; logged once             */
    int      hit_eof;           /* a READ ran off the end: client saw it all */
    time_t   opened;
    time_t   last_active;
};

struct dir_slot {
    int in_use;
    struct dir_list list;
};

struct session {
    int       in_use;
    uint16_t  sid;
    int       mount_zone;
    struct sockaddr_storage peer;
    socklen_t peerlen;
    char      ip[64];
    int       tcp_fd;           /* -1 for a UDP session */
    time_t    last_active;

    struct dir_slot  dirs[MAX_DIR_HANDLES];
    struct file_slot files[MAX_FILE_HANDLES];

    /* Last reply, resent verbatim if the client retries the same sequence
     * number - the datagram may simply have been lost on the way back. */
    int      have_last;
    uint8_t  last_seq;
    uint8_t  last_cmd;
    uint8_t  last_reply[TNFS_MAX_MSG];
    size_t   last_reply_len;
};

void sockaddr_ip(const struct sockaddr *sa, char *buf, size_t bufsz);
int  sockaddr_same_ip(const struct sockaddr *a, const struct sockaddr *b);

struct session *session_new(const struct sockaddr *peer, socklen_t peerlen,
                            int tcp_fd, int mount_zone);
struct session *session_find(uint16_t sid, const struct sockaddr *peer);
void session_close(struct session *s);
void session_close_tcp(int fd);
void session_close_all(void);
void session_expire(time_t now);

int  session_open_uploads(const struct session *s);

/* Handle tables. Slot 0 is a valid handle; -1 means none free. */
int  session_alloc_dir(struct session *s);
int  session_alloc_file(struct session *s);

/* Close an open file slot. commit is honoured only for uploads: nonzero
 * finalizes by rename, zero unlinks the temp file. Returns a TNFS status. */
int  session_close_file(struct session *s, int handle, int commit);

#endif /* DE_SESSION_H */
