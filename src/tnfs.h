/* tnfs.h - TNFS wire protocol constants.
 *
 * Derived solely from the protocol specification (tnfs-protocol.md,
 * updated July 15 2020). Nothing here depends on any particular server
 * implementation.
 */
#ifndef DE_TNFS_H
#define DE_TNFS_H

#include <stdint.h>

/* ---- Datagram geometry -------------------------------------------------
 *
 * The spec caps a reply at "the maximum TNFS datagram size" without naming
 * it; 532 bytes is the size every deployed client expects, so a header plus
 * a 512-byte payload is the largest reply we ever build.
 */
#define TNFS_HEADER_SIZE   4        /* session(2) seq(1) cmd(1)            */
#define TNFS_MAX_PAYLOAD   512      /* largest READ/WRITE data block       */
#define TNFS_MAX_MSG       532      /* largest datagram we send            */
#define TNFS_RECV_BUF      2048     /* generous receive buffer             */

/* Version this daemon speaks: 1.2 (MSB major, LSB minor). Anything above
 * 0x0100 means LSEEK replies carry the resulting file position. */
#define TNFS_VERSION_MAJOR 1
#define TNFS_VERSION_MINOR 2

/* Minimum client retry interval, milliseconds, reported by MOUNT. */
#define TNFS_MIN_RETRY_MS  1000

/* ---- Commands ---------------------------------------------------------- */
#define TNFS_MOUNT         0x00
#define TNFS_UMOUNT        0x01

#define TNFS_OPENDIR       0x10
#define TNFS_READDIR       0x11
#define TNFS_CLOSEDIR      0x12
#define TNFS_MKDIR         0x13
#define TNFS_RMDIR         0x14
#define TNFS_TELLDIR       0x15
#define TNFS_SEEKDIR       0x16
#define TNFS_OPENDIRX      0x17
#define TNFS_READDIRX      0x18

#define TNFS_OPENFILE_OLD  0x20
#define TNFS_READ          0x21
#define TNFS_WRITE         0x22
#define TNFS_CLOSE         0x23
#define TNFS_STAT          0x24
#define TNFS_LSEEK         0x25
#define TNFS_UNLINK        0x26
#define TNFS_CHMOD         0x27
#define TNFS_RENAME        0x28
#define TNFS_OPEN          0x29

#define TNFS_SIZE          0x30
#define TNFS_FREE          0x31
#define TNFS_SIZEBYTES     0x32
#define TNFS_FREEBYTES     0x33

/* ---- Return codes ------------------------------------------------------ */
#define TNFS_OK            0x00
#define TNFS_EPERM         0x01
#define TNFS_ENOENT        0x02
#define TNFS_EIO           0x03
#define TNFS_ENXIO         0x04
#define TNFS_E2BIG         0x05
#define TNFS_EBADF         0x06
#define TNFS_EAGAIN        0x07
#define TNFS_ENOMEM        0x08
#define TNFS_EACCES        0x09
#define TNFS_EBUSY         0x0A
#define TNFS_EEXIST        0x0B
#define TNFS_ENOTDIR       0x0C
#define TNFS_EISDIR        0x0D
#define TNFS_EINVAL        0x0E
#define TNFS_ENFILE        0x0F
#define TNFS_EMFILE        0x10
#define TNFS_EFBIG         0x11
#define TNFS_ENOSPC        0x12
#define TNFS_ESPIPE        0x13
#define TNFS_EROFS         0x14
#define TNFS_ENAMETOOLONG  0x15
#define TNFS_ENOSYS        0x16
#define TNFS_ENOTEMPTY     0x17
#define TNFS_ELOOP         0x18
#define TNFS_ENODATA       0x19
#define TNFS_ENOSTR        0x1A
#define TNFS_EPROTO        0x1B
#define TNFS_EBADFD        0x1C
#define TNFS_EUSERS        0x1D
#define TNFS_ENOBUFS       0x1E
#define TNFS_EALREADY      0x1F
#define TNFS_ESTALE        0x20
#define TNFS_EOF           0x21
#define TNFS_EBADHANDLE    0xFF

/* ---- OPEN flags (wire values, not the host's O_* values) --------------- */
#define TNFS_O_RDONLY      0x0001
#define TNFS_O_WRONLY      0x0002
#define TNFS_O_RDWR        0x0003
#define TNFS_O_ACCMODE     0x0003
#define TNFS_O_APPEND      0x0008
#define TNFS_O_CREAT       0x0100
#define TNFS_O_TRUNC       0x0200
#define TNFS_O_EXCL        0x0400

/* ---- LSEEK whence ------------------------------------------------------ */
#define TNFS_SEEK_SET      0x00
#define TNFS_SEEK_CUR      0x01
#define TNFS_SEEK_END      0x02

/* ---- OPENDIRX options -------------------------------------------------- */
#define TNFS_DIROPT_NO_FOLDERSFIRST 0x01
#define TNFS_DIROPT_NO_SKIPHIDDEN   0x02
#define TNFS_DIROPT_NO_SKIPSPECIAL  0x04
#define TNFS_DIROPT_DIR_PATTERN     0x08
/* Recursive traverse is accepted and ignored (DESIGN.md 4). */
#define TNFS_DIROPT_TRAVERSE        0x10

#define TNFS_DIRSORT_NONE           0x01
#define TNFS_DIRSORT_CASE           0x02
#define TNFS_DIRSORT_DESCENDING     0x04
#define TNFS_DIRSORT_MODIFIED       0x08
#define TNFS_DIRSORT_SIZE           0x10

/* ---- READDIRX entry/status flags --------------------------------------- */
#define TNFS_DIRENTRY_DIR           0x01
#define TNFS_DIRENTRY_HIDDEN        0x02
#define TNFS_DIRENTRY_SPECIAL       0x04

#define TNFS_DIRSTATUS_EOF          0x01

#endif /* DE_TNFS_H */
