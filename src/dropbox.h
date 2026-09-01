/* dropbox.h - drop-box accounting and the abuse controls around it.
 *
 * The daemon may read incoming/ even though clients may not: the capability
 * table governs client requests, not the daemon's own housekeeping.
 */
#ifndef DE_DROPBOX_H
#define DE_DROPBOX_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Uploads open at once, per session and across the server. */
#define MAX_UPLOADS_PER_SESSION 2
#define MAX_UPLOADS_TOTAL       16

/* A .tmp-* left by a session that died is swept once it is this old. */
#define TMP_MAX_AGE_SEC 300

/* Rescan interval; also rescanned before any count/quota refusal. */
#define DROPBOX_RESCAN_SEC 60

/* Opens the randomness source once, before any confinement step. */
void dropbox_init(void);

void dropbox_scan(void);
void dropbox_tick(time_t now);

/* May a new upload start? TNFS_OK, or TNFS_EACCES - the same status as every
 * other refusal in the zone, so "full" and "name taken" are indistinguishable. */
int dropbox_check_open(void);

/* May this upload grow by delta, to new_apparent bytes? TNFS_OK or TNFS_EFBIG.
 * EFBIG is deliberately distinguishable: it describes the client's own file,
 * not the contents of the directory. */
int dropbox_check_write(uint64_t delta, uint64_t new_apparent);

void dropbox_open_begin(void);
void dropbox_grew(uint64_t delta);
void dropbox_close_commit(uint64_t apparent);
void dropbox_close_abort(uint64_t apparent);

/* Free space to report: the smaller of the filesystem's and the quota's. */
uint64_t dropbox_free_bytes(uint64_t fs_free);

/* ".tmp-<16 hex>" */
int dropbox_make_temp(char *buf, size_t bufsz);

/* Leaky buckets per source IP. Return 1 to allow, 0 to refuse. */
int rate_allow_upload(const char *ip);
int rate_allow_bytes(const char *ip, uint64_t bytes);

#endif /* DE_DROPBOX_H */
