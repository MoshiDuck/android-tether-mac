// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

int ipc_send(int sock, const struct ipc_msg *m, int fd_to_pass) {
    struct ipc_msg copy = *m;                       /* iovec veut un tampon non const */
    struct iovec iov = {.iov_base = &copy, .iov_len = sizeof copy};
    struct msghdr mh;
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } ctl;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd_to_pass >= 0) {
        memset(&ctl, 0, sizeof ctl);
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof ctl.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd_to_pass, sizeof(int));
    }
    ssize_t n;
    do n = sendmsg(sock, &mh, 0); while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof copy ? 0 : -1;
}

int ipc_recv(int sock, struct ipc_msg *m, int *fd_out, int timeout_ms) {
    *fd_out = -1;
    uint8_t *dst = (uint8_t *)m;
    size_t got = 0;
    while (got < sizeof *m) {
        if (got == 0 || timeout_ms >= 0) {            /* attend l'arrivée (puis le reste du message) */
            struct pollfd p = {.fd = sock, .events = POLLIN};
            int r = poll(&p, 1, timeout_ms);
            if (r < 0) { if (errno == EINTR) continue; goto fail; }
            if (r == 0) { if (got == 0) return 0; goto fail; }   /* message coupé en plein milieu : invalide */
        }
        struct iovec iov = {.iov_base = dst + got, .iov_len = sizeof *m - got};
        struct msghdr mh;
        union { char buf[CMSG_SPACE(sizeof(int) * 4)]; struct cmsghdr align; } ctl;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = ctl.buf;
        mh.msg_controllen = sizeof ctl.buf;
        ssize_t n = recvmsg(sock, &mh, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;                         /* 0 = pair fermé */
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int nfds = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int k = 0; k < nfds; k++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + k * sizeof(int), sizeof fd);
                if (*fd_out < 0) *fd_out = fd; else close(fd);   /* un seul descripteur accepté */
            }
        }
        got += (size_t)n;
    }
    if (m->len > IPC_DATA_MAX) goto fail;
    return 1;
fail:
    if (*fd_out >= 0) { close(*fd_out); *fd_out = -1; }
    return -1;
}
