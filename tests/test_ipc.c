// SPDX-License-Identifier: GPL-3.0-or-later
/* Tests du canal worker <-> superviseur : messages, passage de descripteur, pair fermé, données hostiles. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/ipc.h"
#include "check.h"

static uint32_t rng = 0x1b873593u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static void pair(int sv[2]) {
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK(r == 0);
}

static struct ipc_msg mk(uint8_t type, const void *data, uint16_t len) {
    struct ipc_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.len = len;
    if (len) memcpy(m.data, data, len);
    return m;
}

static void test_roundtrip(void) {
    int sv[2];
    pair(sv);
    const uint8_t payload[16] = {10, 217, 123, 144, 10, 217, 123, 221, 255, 255, 255, 0, 10, 217, 123, 221};
    struct ipc_msg out = mk(IPC_REQ_CONFIG4, payload, sizeof payload), in;
    int fd = 42;
    CHECK(ipc_send(sv[0], &out, -1) == 0);
    CHECK(ipc_recv(sv[1], &in, &fd, 1000) == 1);
    CHECK(fd == -1 && in.type == IPC_REQ_CONFIG4 && in.len == 16 && !memcmp(in.data, payload, 16));

    CHECK(ipc_recv(sv[1], &in, &fd, 50) == 0);                       /* rien à lire : délai écoulé */

    for (int k = 0; k < 40; k++) {                                    /* plusieurs messages à la suite, dans l'ordre (4 Ko < tampon du canal) */
        struct ipc_msg m = mk(IPC_REQ_STATUS, "waiting", 7);
        m.status = (uint8_t)k;
        CHECK(ipc_send(sv[0], &m, -1) == 0);
    }
    for (int k = 0; k < 40; k++) {
        CHECK(ipc_recv(sv[1], &in, &fd, 1000) == 1 && in.status == (uint8_t)k && in.len == 7);
    }
    close(sv[0]);
    close(sv[1]);
}

static void test_fd_passing(void) {
    int sv[2], pp[2];
    pair(sv);
    CHECK(pipe(pp) == 0);
    struct ipc_msg out = mk(IPC_REPLY, "utun13", 6), in;
    int fd = -1;
    CHECK(ipc_send(sv[0], &out, pp[1]) == 0);                         /* on passe l'extrémité d'écriture du tube */
    CHECK(ipc_recv(sv[1], &in, &fd, 1000) == 1);
    CHECK(fd >= 0 && fd != pp[1] && in.len == 6 && !memcmp(in.data, "utun13", 6));
    if (fd >= 0) {
        char c = 0;
        CHECK(write(fd, "Z", 1) == 1);                               /* le descripteur reçu est bien utilisable */
        CHECK(read(pp[0], &c, 1) == 1 && c == 'Z');
        close(fd);
    }
    close(pp[0]); close(pp[1]); close(sv[0]); close(sv[1]);
}

static void test_peer_closed(void) {
    int sv[2];
    pair(sv);
    struct ipc_msg in;
    int fd;
    close(sv[0]);
    CHECK(ipc_recv(sv[1], &in, &fd, 1000) == -1);                     /* le pair a disparu */
    struct ipc_msg out = mk(IPC_REQ_ADB, NULL, 0);
    signal(SIGPIPE, SIG_IGN);
    CHECK(ipc_send(sv[1], &out, -1) == -1);
    close(sv[1]);
}

/* fd_out peut être NULL : le message arrive, et un descripteur joint est fermé au lieu de fuir. */
static void test_null_fd_out(void) {
    int sv[2], pp[2];
    struct ipc_msg in, out = mk(IPC_REQ_ADB, NULL, 0);
    pair(sv);
    CHECK(ipc_send(sv[0], &out, -1) == 0);
    CHECK(ipc_recv(sv[1], &in, NULL, 1000) == 1 && in.type == IPC_REQ_ADB);
    CHECK(ipc_recv(sv[1], &in, NULL, 50) == 0);                       /* délai sans message */

    CHECK(pipe(pp) == 0);
    CHECK(ipc_send(sv[0], &out, pp[0]) == 0);                         /* avec un descripteur joint */
    close(pp[0]);                                                     /* il ne reste que la copie du récepteur */
    CHECK(ipc_recv(sv[1], &in, NULL, 1000) == 1);                     /* ...que ipc_recv doit fermer */
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(pp[1], "x", 1) == -1 && errno == EPIPE);              /* plus aucun lecteur : pas de fuite */
    close(pp[1]);

    CHECK(ipc_send(sv[0], &out, -1) == 0);
    close(sv[0]);
    CHECK(ipc_recv(sv[1], &in, NULL, 1000) == 1);
    CHECK(ipc_recv(sv[1], &in, NULL, 1000) == -1);                    /* pair fermé */
    close(sv[1]);
}

static void test_malformed(void) {
    int sv[2], fd;
    struct ipc_msg in;

    pair(sv);                                                         /* message coupé : le pair se ferme avant la fin */
    uint8_t half[10] = {1, 0, 4, 0};
    CHECK(write(sv[0], half, sizeof half) == (ssize_t)sizeof half);
    close(sv[0]);
    CHECK(ipc_recv(sv[1], &in, &fd, 500) == -1);
    close(sv[1]);

    pair(sv);                                                         /* message coupé et pair silencieux : délai */
    CHECK(write(sv[0], half, sizeof half) == (ssize_t)sizeof half);
    CHECK(ipc_recv(sv[1], &in, &fd, 100) == -1);
    close(sv[0]); close(sv[1]);

    pair(sv);                                                         /* longueur annoncée > taille des données */
    struct ipc_msg evil = mk(IPC_REQ_STATUS, NULL, 0);
    evil.len = 60000;
    CHECK(write(sv[0], &evil, sizeof evil) == (ssize_t)sizeof evil);
    CHECK(ipc_recv(sv[1], &in, &fd, 500) == -1 && fd == -1);
    close(sv[0]); close(sv[1]);
}

static void test_extra_fds(void) {
    int sv[2], pp[2], pq[2], fd = -1;
    pair(sv);
    CHECK(pipe(pp) == 0 && pipe(pq) == 0);
    struct ipc_msg m = mk(IPC_REPLY, NULL, 0), in;
    int two[2] = {pp[1], pq[1]};                                      /* deux descripteurs : un seul doit être gardé */
    struct iovec iov = {.iov_base = &m, .iov_len = sizeof m};
    union { char buf[CMSG_SPACE(sizeof two)]; struct cmsghdr align; } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    memset(&ctl, 0, sizeof ctl);
    mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = ctl.buf; mh.msg_controllen = sizeof ctl.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof two);
    memcpy(CMSG_DATA(c), two, sizeof two);
    CHECK(sendmsg(sv[0], &mh, 0) == (ssize_t)sizeof m);
    CHECK(ipc_recv(sv[1], &in, &fd, 1000) == 1 && fd >= 0);
    if (fd >= 0) close(fd);
    close(pp[0]); close(pp[1]); close(pq[0]); close(pq[1]); close(sv[0]); close(sv[1]);
}

/* Octets aléatoires envoyés par morceaux : ipc_recv doit toujours rendre la main sans planter. */
static void test_fuzz(void) {
    for (int iter = 0; iter < 300; iter++) {
        int sv[2];
        pair(sv);
        fcntl(sv[0], F_SETFL, O_NONBLOCK);
        uint8_t junk[400];
        size_t n = rnd() % sizeof junk;
        for (size_t k = 0; k < n; k++) junk[k] = (uint8_t)rnd();
        if (n >= sizeof(struct ipc_msg) && iter % 2) { junk[2] = 0; junk[3] = 0; }     /* longueur valide : message accepté */
        ssize_t w = write(sv[0], junk, n);
        (void)w;
        close(sv[0]);
        struct ipc_msg in;
        int fd = -1, r, count = 0;
        while ((r = ipc_recv(sv[1], &in, &fd, 100)) == 1) {
            CHECK(in.len <= IPC_DATA_MAX && fd == -1);
            if (++count > 10) break;
        }
        CHECK(r == -1 || r == 0 || count > 10);
        close(sv[1]);
    }
}

void run_ipc_tests(void) {
    test_roundtrip();
    test_fd_passing();
    test_peer_closed();
    test_null_fd_out();
    test_malformed();
    test_extra_fds();
    test_fuzz();
}
