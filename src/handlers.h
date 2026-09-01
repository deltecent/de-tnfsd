/* handlers.h - decode one request, produce one reply. */
#ifndef DE_HANDLERS_H
#define DE_HANDLERS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Returns the number of bytes to send back, or 0 for "say nothing".
 * tcp_fd is -1 for a request that arrived over UDP. */
size_t tnfs_handle(const uint8_t *msg, size_t len,
                   const struct sockaddr *peer, socklen_t peerlen,
                   int tcp_fd, uint8_t *out, size_t outsz);

#endif /* DE_HANDLERS_H */
