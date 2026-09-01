#include "net.h"

#include "dropbox.h"
#include "handlers.h"
#include "log.h"
#include "server.h"
#include "session.h"
#include "tnfs.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_TCP_CLIENTS 32

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    if (stop_requested)
        _exit(1);           /* a second signal means "now" */
    stop_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);       /* no SA_RESTART: poll must wake */
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

int net_listen(int port)
{
    struct sockaddr_in6 addr;
    int on = 1, off = 0;

    srv.udp_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    srv.tcp_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (srv.udp_fd < 0 || srv.tcp_fd < 0) {
        log_err("socket: %s", strerror(errno));
        return -1;
    }

    /* One dual-stack socket per transport; IPv4 peers arrive as v4-mapped. */
    setsockopt(srv.udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(srv.tcp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    setsockopt(srv.tcp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    memset(&addr, 0, sizeof addr);
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons((uint16_t)port);

    if (bind(srv.udp_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_err("bind udp port %d: %s", port, strerror(errno));
        return -1;
    }
    if (bind(srv.tcp_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_err("bind tcp port %d: %s", port, strerror(errno));
        return -1;
    }
    if (listen(srv.tcp_fd, 8) != 0) {
        log_err("listen: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void serve_udp(void)
{
    uint8_t in[TNFS_RECV_BUF], out[TNFS_MAX_MSG];
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof peer;
    ssize_t n;
    size_t reply;

    n = recvfrom(srv.udp_fd, in, sizeof in, 0,
                 (struct sockaddr *)&peer, &peerlen);
    if (n <= 0)
        return;

    reply = tnfs_handle(in, (size_t)n, (struct sockaddr *)&peer, peerlen,
                        -1, out, sizeof out);
    if (reply > 0)
        sendto(srv.udp_fd, out, reply, 0, (struct sockaddr *)&peer, peerlen);
}

static void serve_tcp(int fd, int *alive)
{
    uint8_t in[TNFS_RECV_BUF], out[TNFS_MAX_MSG];
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof peer;
    ssize_t n;
    size_t reply;

    /* TNFS has no length prefix, so a stream carries one datagram per read.
     * That is what deployed clients send; a request split across segments is
     * not reassembled. */
    n = recv(fd, in, sizeof in, 0);
    if (n <= 0) {
        *alive = 0;
        return;
    }
    if (getpeername(fd, (struct sockaddr *)&peer, &peerlen) != 0) {
        *alive = 0;
        return;
    }

    reply = tnfs_handle(in, (size_t)n, (struct sockaddr *)&peer, peerlen,
                        fd, out, sizeof out);
    if (reply > 0 && send(fd, out, reply, 0) < 0)
        *alive = 0;
}

int net_run(void)
{
    int clients[MAX_TCP_CLIENTS];
    int nclients = 0;

    install_signal_handlers();
    log_info("listening port=%d root=%s incoming=%s",
             srv.port, srv.root_path, srv.inc_fd >= 0 ? "yes" : "no");

    while (!stop_requested) {
        struct pollfd pfd[2 + MAX_TCP_CLIENTS];
        int nfds = 0, rc;
        time_t now;

        pfd[nfds].fd = srv.udp_fd; pfd[nfds].events = POLLIN; nfds++;
        pfd[nfds].fd = srv.tcp_fd; pfd[nfds].events = POLLIN; nfds++;
        for (int i = 0; i < nclients; i++) {
            pfd[nfds].fd = clients[i];
            pfd[nfds].events = POLLIN;
            nfds++;
        }

        rc = poll(pfd, (nfds_t)nfds, 1000);
        now = time(NULL);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_err("poll: %s", strerror(errno));
            break;
        }

        if (rc == 0) {
            session_expire(now);
            dropbox_tick(now);
            continue;
        }

        if (pfd[0].revents & POLLIN)
            serve_udp();

        if (pfd[1].revents & POLLIN) {
            int c = accept(srv.tcp_fd, NULL, NULL);
            if (c >= 0) {
                if (nclients < MAX_TCP_CLIENTS)
                    clients[nclients++] = c;
                else
                    close(c);
            }
        }

        for (int i = 0; i < nclients; i++) {
            int alive = 1;
            if (pfd[2 + i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (pfd[2 + i].revents & POLLIN)
                    serve_tcp(clients[i], &alive);
                else
                    alive = 0;
            }
            if (!alive) {
                session_close_tcp(clients[i]);
                close(clients[i]);
                clients[i] = clients[--nclients];
                i--;
            }
        }

        session_expire(now);
        dropbox_tick(now);
    }

    log_info("shutting down");
    /* Closing every session unlinks the temp file of any upload still in
     * flight, so a shutdown leaves nothing half-written in the drop box. */
    session_close_all();
    for (int i = 0; i < nclients; i++)
        close(clients[i]);
    close(srv.udp_fd);
    close(srv.tcp_fd);
    return 0;
}
