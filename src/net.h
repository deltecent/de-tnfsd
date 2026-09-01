/* net.h - sockets and the event loop. */
#ifndef DE_NET_H
#define DE_NET_H

/* Bind the UDP and TCP sockets. Returns 0, or -1 with a message logged. */
int net_listen(int port);

/* Serve until SIGINT or SIGTERM. Returns 0 on a clean shutdown. */
int net_run(void);

#endif /* DE_NET_H */
