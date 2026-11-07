// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * rndis-tether — USB tethering Android -> macOS, 100 % espace utilisateur.
 *
 * Principe :
 *   - libusb parle RNDIS directement au téléphone (aucun kext / DriverKit)
 *   - une interface utun (L3) expose le réseau au système
 *   - ARP, DHCP, NDP sont gérés ici car RNDIS transporte des trames Ethernet
 *
 * Deux processus (séparation des privilèges) :
 *   - le SUPERVISEUR (root) ne fait presque rien : il crée l'interface utun, applique la configuration
 *     réseau (ifconfig, route, scutil) après validation par policy.c, et nettoie à la fin ;
 *   - le WORKER, lancé par fork() puis réduit à l'utilisateur « nobody », fait tout ce qui lit des
 *     données hostiles (USB, RNDIS, DHCP, NDP, paquets). Il reçoit l'interface utun par passage de
 *     descripteur et ne peut demander au superviseur que des actions précises (ipc.h).
 *
 * Lancer avec sudo.
 */
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <libusb.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "adb.h"
#include "bytes.h"
#include "dhcp.h"
#include "diagnose.h"
#include "ipc.h"
#include "logsan.h"
#include "policy.h"

#ifndef RNDIS_VERSION
#define RNDIS_VERSION "unknown"
#endif

/* ---------- RNDIS ---------- */
#define RNDIS_PACKET_MSG        0x00000001u
#define RNDIS_INIT_MSG          0x00000002u
#define RNDIS_HALT_MSG          0x00000003u
#define RNDIS_QUERY_MSG         0x00000004u
#define RNDIS_SET_MSG           0x00000005u
#define RNDIS_KEEPALIVE_MSG     0x00000008u
#define RNDIS_CMPLT             0x80000000u
#define OID_802_3_PERM_ADDR     0x01010101u
#define OID_GEN_PKT_FILTER      0x0001010Eu
#define PKT_FILTER_DEFAULT      0x0000000Du /* directed | all-multicast | broadcast */
#define RNDIS_HDR               44
#define RX_BUF                  32768
#define MTU                     1500
static int cur_mtu = MTU;               /* worker : MTU de l'interface (celui du téléphone s'il en annonce un plus petit) */
static int iface_mtu = MTU;             /* superviseur : MTU appliqué à l'interface */
static unsigned v6_mtu;                 /* worker : MTU annoncé dans la RA (0 si absent) */

#define CDC_SEND_ENCAPSULATED   0x00
#define CDC_GET_ENCAPSULATED    0x01

static libusb_context *ctx;
static libusb_device_handle *h;
static int ctrl_if = -1, data_if = -1;
static uint8_t ep_in, ep_out, ep_int;
static int mps_out = 512;

static volatile sig_atomic_t running = 1, quit;
static int verbose, want_route, wait_mode, dns_applied, auto_adb;
static uint32_t srv_ip;               /* DHCP : serveur */
static struct dhcp_client dhcp_c;     /* cycle de vie du bail (voir dhcp.c) ; protégé par dhcp_lock */
static int dhcp_active;               /* un bail est géré (session établie) */
static pthread_mutex_t dhcp_lock = PTHREAD_MUTEX_INITIALIZER;

/* IPv6 : adresse SLAAC construite à partir de la première annonce de routeur (RA) reçue */
static int ipv6_on = 1;               /* -4 le désactive */
static uint8_t v6_addr[16], v6_router[16];
static int v6_svc;                    /* service IPv6 déclaré dans SystemConfiguration (pour que macOS demande les AAAA) */
static int v6_claimed;               /* une RA exploitable a déjà été prise en compte */
static int v6_pending;                /* adresse prête, à configurer par le thread de contrôle */
static int v6_withdraw;               /* le téléphone n'offre plus d'IPv6 : à retirer par le thread de contrôle */
static int64_t v6_expire;             /* fin de validité du routeur (now_s) ; 0 = pas de suivi */
static int v6_routes;                 /* routes ::/1 et 8000::/1 installées */
static int cfg4_done, cfg6_done;        /* superviseur : configuration déjà appliquée pour le worker courant */
static int v6_blocked;                /* routes « reject » installées : l'IPv6 ne peut pas fuir par un autre réseau */
static int allow_v6_leak;             /* -L : ne pas bloquer l'IPv6 hors tethering */
static int utun_fd = -1;
static char utun_name[IFNAMSIZ];
static uint8_t my_mac[6], gw_mac[6];
static uint32_t my_ip, gw_ip; /* network order */
static uint32_t req_counter = 1;

/* Séparation des privilèges */
#define EXIT_NO_DEVICE 3                /* worker : aucun appareil RNDIS (sans -w) */
#define EXIT_FATAL 70                   /* worker : impossible d'abandonner les privilèges */
static int worker_mode;                 /* 1 dans le processus sans privilèges */
static int ipc_sock = -1;               /* worker : canal vers le superviseur */
static pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *worker_user = "nobody";
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

#ifdef RNDIS_TESTING
/* Compilé avec -DRNDIS_TESTING (tests uniquement) : les commandes système sont enregistrées, pas exécutées. */
void test_record_command(const char *cmd);
static int (*test_worker_entry)(int sock);   /* si défini, le worker forké exécute cette fonction au lieu de worker_main */
#endif

PRINTF_LIKE(1, 2) static void logf_(const char *fmt, ...) {
    va_list ap;
    fputs(worker_mode ? "[worker] " : "[root] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

static void on_signal(int s) { (void)s; running = 0; quit = 1; }

/* Horloge monotone en secondes : continue d'avancer pendant la veille du Mac (comme le bail DHCP). */
#ifdef RNDIS_TESTING
static volatile int64_t test_clock_offset;      /* les tests simulent une veille en avançant l'horloge */
#endif
static int64_t now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
#ifdef RNDIS_TESTING
    return t.tv_sec + test_clock_offset;
#else
    return t.tv_sec;
#endif
}

/* ---------- Worker : demandes au superviseur ----------
 * Appel synchrone (une demande, une réponse). Retourne le code de la réponse (0 = accepté),
 * ou -1 si le canal est rompu : le worker s'arrête alors (le superviseur a disparu). */
static int ipc_call(int type, const void *data, size_t len, struct ipc_msg *reply, int *fd_out, int timeout_ms) {
    struct ipc_msg rq, rp;
    int fd = -1, r;
    if (len > IPC_DATA_MAX) return -1;
    memset(&rq, 0, sizeof rq);
    rq.type = (uint8_t)type;
    rq.len = (uint16_t)len;
    if (len) memcpy(rq.data, data, len);
    pthread_mutex_lock(&ipc_lock);
    r = ipc_send(ipc_sock, &rq, -1) < 0 ? -1 : ipc_recv(ipc_sock, &rp, &fd, timeout_ms);
    pthread_mutex_unlock(&ipc_lock);
    if (r != 1 || rp.type != IPC_REPLY) {
        if (fd >= 0) close(fd);
        LOG("Canal avec le superviseur perdu : arrêt");
        running = 0;
        quit = 1;
        return -1;
    }
    if (reply) *reply = rp;
    if (fd_out) *fd_out = fd; else if (fd >= 0) close(fd);
    return rp.status;
}

/* Demande l'interface utun : retourne son descripteur (nom dans utun_name) ou -1. */
static int request_utun(void) {
    struct ipc_msg rp;
    int fd = -1;
    if (ipc_call(IPC_REQ_UTUN, NULL, 0, &rp, &fd, 15000) != 0 || fd < 0 || rp.len < 5 || rp.len >= IFNAMSIZ ||
        memcmp(rp.data, "utun", 4) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    memcpy(utun_name, rp.data, rp.len);
    utun_name[rp.len] = '\0';
    return fd;
}

/* MTU annoncé par le téléphone (DHCP option 26) : utilisé s'il est plausible, sinon on garde 1500. */
static unsigned lease_mtu(const struct lease *L) {
    unsigned m = policy_mtu_from_peer(L->mtu, POLICY_MTU_MIN4);
    if (L->mtu && !m) LOG("DHCP: MTU %u annoncé par le téléphone ignoré (trop petit)", (unsigned)L->mtu);
    return m && m < MTU ? m : 0;                                      /* 0 : rien à changer */
}

static int request_config4(const struct lease *L) {
    uint8_t d[18];
    unsigned m = lease_mtu(L);
    memcpy(d, &L->ip, 4); memcpy(d + 4, &L->gw, 4); memcpy(d + 8, &L->mask, 4); memcpy(d + 12, &L->dns, 4);
    putbe16(d + 16, (uint16_t)m);
    if (ipc_call(IPC_REQ_CONFIG4, d, m ? 18 : 16, NULL, NULL, 30000) != 0) return -1;
    if (m) { __atomic_store_n(&cur_mtu, (int)m, __ATOMIC_RELAXED); LOG("MTU de l'interface : %u (annoncé par le téléphone)", m); }
    return 0;
}

static void request_config6(void) {
    uint8_t d[34];
    memcpy(d, v6_addr, 16); memcpy(d + 16, v6_router, 16);
    putbe16(d + 32, (uint16_t)v6_mtu);
    int r = ipc_call(IPC_REQ_CONFIG6, d, v6_mtu ? 34 : 32, NULL, NULL, 30000);
    if (r > 0) { LOG("IPv6: configuration refusée par le superviseur"); return; }
    if (r == 0 && v6_mtu && (int)v6_mtu < __atomic_load_n(&cur_mtu, __ATOMIC_RELAXED)) {
        __atomic_store_n(&cur_mtu, (int)v6_mtu, __ATOMIC_RELAXED);
        LOG("MTU de l'interface : %u (annoncé dans l'annonce de routeur IPv6)", v6_mtu);
    }
}

static void request_deconfig6(void) {
    if (ipc_call(IPC_REQ_DECONFIG6, NULL, 0, NULL, NULL, 30000) > 0) LOG("IPv6: retrait refusé par le superviseur");
}

static void request_adb(void) { ipc_call(IPC_REQ_ADB, NULL, 0, NULL, NULL, 90000); }

/* ---------- Contrôle par l'icône de barre de menus ----------
 * Le service tourne en root ; l'app (utilisateur) crée/supprime FLAG_DISABLED pour
 * l'éteindre/l'allumer, et lit STATUS_FILE (réécrit toutes les ~5 s) pour l'état. */
#define FLAG_DISABLED "/Users/Shared/rndis-tether.disabled"
#define STATUS_FILE   "/Users/Shared/rndis-tether.status"
static const char *adb_user;
static const char *adb_serial;       /* -S : numéro de série du téléphone à basculer (plusieurs appareils adb) */
static char status_text[160] = "starting";

#ifdef RNDIS_TESTING
static int is_disabled(void) { return 0; }      /* les tests ne dépendent pas du vrai fichier de l'icône de menu */
#else
static int is_disabled(void) { return access(FLAG_DISABLED, F_OK) == 0; }
#endif

static void status_write(void) {
#ifdef RNDIS_TESTING
    return;                                 /* les tests ne touchent pas au vrai fichier d'état */
#endif
    FILE *f = fopen(STATUS_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f, "%s\n", status_text);
    fclose(f);
    chmod(STATUS_FILE ".tmp", 0644);
    rename(STATUS_FILE ".tmp", STATUS_FILE);
}

PRINTF_LIKE(1, 2) static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_text, sizeof status_text, fmt, ap);
    va_end(ap);
    status_write();
}

/* Chien de garde. Le worker lit des données hostiles : un blocage (volontaire ou non) ne doit pas laisser le Mac
 * sans réseau. Il envoie son état au moins toutes les WORKER_HEARTBEAT_S secondes, mais seulement si ses trois
 * threads (réception, émission, contrôle) ont tourné depuis le dernier signe de vie ; le superviseur tue un worker
 * silencieux depuis plus de watchdog_s secondes, nettoie la configuration réseau et en relance un neuf. */
#define WORKER_HEARTBEAT_S 10
#define WORKER_THREADS 7                    /* bits : 1 réception, 2 émission, 4 contrôle */
static int watchdog_s = 60;
static unsigned thread_alive;               /* bits posés par chaque thread à chaque tour de boucle */
static int threads_running;                 /* une session est active : les trois threads doivent progresser */

static void thread_beat(unsigned bit) { __atomic_or_fetch(&thread_alive, bit, __ATOMIC_RELAXED); }

/* Vrai si tous les threads ont progressé depuis le dernier appel (puis remet le compteur à zéro). */
static int worker_threads_alive(void) {
    if (!__atomic_load_n(&threads_running, __ATOMIC_ACQUIRE)) return 1;
    return (__atomic_exchange_n(&thread_alive, 0, __ATOMIC_RELAXED) & WORKER_THREADS) == WORKER_THREADS;
}

/* Veille du Mac : la boucle du superviseur tourne toutes les secondes ; un écart bien plus grand entre deux tours
 * signifie que le Mac dormait. Le lien USB, le bail DHCP et l'IPv6 sont alors à refaire : plutôt que d'attendre
 * des délais d'expiration un par un, on termine la session proprement (bail rendu si possible, routes retirées)
 * et le superviseur en relance une neuve, qui retrouve le téléphone et refait DHCP. */
#define WAKE_GAP_S 15
static int wake_gap_s = WAKE_GAP_S;
static int wake_detected(int64_t now, int64_t prev_tick, int gap_s) {
    return gap_s > 0 && now - prev_tick >= gap_s;
}

/* Vrai si le worker est silencieux depuis trop longtemps. */
static int watchdog_expired(int64_t now, int64_t last_seen, int limit_s) {
    return limit_s > 0 && now - last_seen > limit_s;
}

/* Change l'état s'il diffère, sinon rafraîchit le fichier au plus toutes les 5 s. */
static void status_tick(const char *text) {
    static time_t last;
    if (worker_mode) {                  /* le worker signale les changements ; le superviseur écrit le fichier */
        static int64_t last_sent;
        int64_t now = now_s();
        if (text && strcmp(text, status_text)) {
            snprintf(status_text, sizeof status_text, "%s", text);
            ipc_call(IPC_REQ_STATUS, text, strlen(text), NULL, NULL, 5000);
            last_sent = now;
        } else if (now - last_sent >= WORKER_HEARTBEAT_S && policy_check_status(status_text, strlen(status_text)) == POLICY_OK &&
                   worker_threads_alive()) {
            ipc_call(IPC_REQ_STATUS, status_text, strlen(status_text), NULL, NULL, 5000);   /* signe de vie pour le chien de garde */
            last_sent = now;
        }
        return;
    }
    time_t now = time(NULL);
    if (text && strcmp(text, status_text)) { set_status("%s", text); last = now; }
    else if (now - last >= 5) { status_write(); last = now; }
}

/* ---------- RNDIS control channel ---------- */
static int ctrl_send(const uint8_t *msg, size_t len) {
    uint8_t buf[256];                                 /* libusb veut un tampon non const, même en émission */
    if (len > sizeof buf) return LIBUSB_ERROR_INVALID_PARAM;
    memcpy(buf, msg, len);
    return libusb_control_transfer(h, 0x21, CDC_SEND_ENCAPSULATED, 0, ctrl_if, buf, len, 1000);
}

static void answer_keepalive(const uint8_t *req) {
    uint8_t r[16];
    put32(r, RNDIS_KEEPALIVE_MSG | RNDIS_CMPLT);
    put32(r + 4, 16);
    memcpy(r + 8, req + 8, 4);
    put32(r + 12, 0);
    ctrl_send(r, sizeof r);
}

/* Envoie une commande et attend la réponse correspondante. Retourne la taille ou <0. */
static int rndis_cmd(uint8_t *msg, size_t len, uint32_t expect, uint8_t *resp, size_t cap) {
    uint32_t rid = get32(msg + 8);
    int r = ctrl_send(msg, len);
    if (r < 0) return r;
    for (int i = 0; i < 40 && running; i++) {
        r = libusb_control_transfer(h, 0xA1, CDC_GET_ENCAPSULATED, 0, ctrl_if, resp, cap, 1000);
        if (r >= 12) {
            uint32_t type = get32(resp);
            if (type == RNDIS_KEEPALIVE_MSG) { answer_keepalive(resp); continue; }
            if (type == expect && get32(resp + 8) == rid) return r;
        } else if (r < 0 && r != LIBUSB_ERROR_PIPE && r != LIBUSB_ERROR_TIMEOUT) {
            return r;
        }
        usleep(50000);
    }
    return -1;
}

/* Extrait l'adresse MAC d'une réponse QUERY_CMPLT de r octets. Les champs viennent
 * du téléphone : tout est vérifié en 64 bits pour qu'aucune valeur ne déborde. */
static int rndis_parse_mac(const uint8_t *resp, int r, uint8_t mac[6]) {
    if (r < 28) return -1;
    uint64_t blen = get32(resp + 16), boff = get32(resp + 20);
    if (blen < 6 || 8 + boff + 6 > (uint64_t)r) return -1;
    memcpy(mac, resp + 8 + boff, 6);
    return 0;
}

static int rndis_init(void) {
    uint8_t m[24], resp[4096];
    put32(m, RNDIS_INIT_MSG); put32(m + 4, 24); put32(m + 8, req_counter++);
    put32(m + 12, 1); put32(m + 16, 0); put32(m + 20, 0x4000);
    int r = rndis_cmd(m, sizeof m, RNDIS_INIT_MSG | RNDIS_CMPLT, resp, sizeof resp);
    if (r < 52) { LOG("RNDIS: pas de réponse à INITIALIZE (%d)", r); return -1; }
    if (get32(resp + 12) != 0) { LOG("RNDIS: INITIALIZE refusé, status=0x%08x", get32(resp + 12)); return -1; }
    VLOG("RNDIS init OK (max transfer %u)", get32(resp + 36));

    /* adresse MAC attribuée à l'hôte */
    uint8_t q[28];
    memset(q, 0, sizeof q);
    put32(q, RNDIS_QUERY_MSG); put32(q + 4, 28); put32(q + 8, req_counter++);
    put32(q + 12, OID_802_3_PERM_ADDR); put32(q + 16, 0); put32(q + 20, 20);
    r = rndis_cmd(q, sizeof q, RNDIS_QUERY_MSG | RNDIS_CMPLT, resp, sizeof resp);
    if (r < 28 || get32(resp + 12) != 0) { LOG("RNDIS: lecture MAC impossible"); return -1; }
    if (rndis_parse_mac(resp, r, my_mac) < 0) { LOG("RNDIS: réponse MAC invalide"); return -1; }

    uint8_t s[32];
    memset(s, 0, sizeof s);
    put32(s, RNDIS_SET_MSG); put32(s + 4, 32); put32(s + 8, req_counter++);
    put32(s + 12, OID_GEN_PKT_FILTER); put32(s + 16, 4); put32(s + 20, 20);
    put32(s + 28, PKT_FILTER_DEFAULT);
    r = rndis_cmd(s, sizeof s, RNDIS_SET_MSG | RNDIS_CMPLT, resp, sizeof resp);
    if (r < 16 || get32(resp + 12) != 0) { LOG("RNDIS: SET packet filter refusé"); return -1; }
    return 0;
}

static void rndis_halt(void) {
    uint8_t m[12];
    put32(m, RNDIS_HALT_MSG); put32(m + 4, 12); put32(m + 8, req_counter++);
    ctrl_send(m, sizeof m);
}

/* ---------- Data path ---------- */
static int send_frame(const uint8_t *frame, int len) {
    uint8_t buf[RNDIS_HDR + 2048];
    if (!h) return LIBUSB_ERROR_NO_DEVICE;
    if (len > 2048) return -1;
    memset(buf, 0, RNDIS_HDR);
    put32(buf, RNDIS_PACKET_MSG); put32(buf + 4, RNDIS_HDR + len);
    put32(buf + 8, RNDIS_HDR - 8); put32(buf + 12, len);
    memcpy(buf + RNDIS_HDR, frame, len);
    int total = RNDIS_HDR + len, got, r;
    pthread_mutex_lock(&tx_lock);
    r = libusb_bulk_transfer(h, ep_out, buf, total, &got, 2000);
    if (r == 0 && total % mps_out == 0) libusb_bulk_transfer(h, ep_out, buf, 0, &got, 2000);
    pthread_mutex_unlock(&tx_lock);
    return r;
}

typedef void (*frame_cb)(const uint8_t *f, int len, void *arg);

static void rx_parse(const uint8_t *buf, int n, frame_cb cb, void *arg) {
    int off = 0;
    while (off + RNDIS_HDR <= n) {
        uint32_t type = get32(buf + off), mlen = get32(buf + off + 4);
        if (mlen < RNDIS_HDR || mlen > (uint32_t)(n - off)) break; /* n - off >= RNDIS_HDR ici */
        if (type == RNDIS_PACKET_MSG) {
            /* champs fournis par l'appareil : calcul en 64 bits pour éviter tout débordement */
            uint64_t doff = get32(buf + off + 8), dlen = get32(buf + off + 12);
            uint64_t start = (uint64_t)off + 8 + doff;
            if (start + dlen <= (uint64_t)off + mlen) cb(buf + start, (int)dlen, arg);
        }
        off += mlen;
    }
}

/* ---------- DHCP client ---------- */
/* Construction, analyse et cycle de vie du bail : voir dhcp.c (testés sans USB ni horloge). */
struct dhcp_wait { uint32_t xid; uint8_t want; struct lease L; int done, nak; };

static void dhcp_cb(const uint8_t *f, int len, void *arg) {
    struct dhcp_wait *w = arg;
    struct lease L;
    if (w->done || dhcp_parse(f, len, my_mac, w->xid, &L) < 0) return;
    if (L.type == 6) { w->nak = 1; return; }
    if (L.type == w->want) { w->L = L; w->done = 1; }
}

static int dhcp_wait_for(struct dhcp_wait *w, int timeout_ms) {
    uint8_t *buf = malloc(RX_BUF);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (running && !w->done && !w->nak) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= timeout_ms) break;
        int got = 0;
        int r = libusb_bulk_transfer(h, ep_in, buf, RX_BUF, &got, 500);
        if (r == 0 && got > 0) rx_parse(buf, got, dhcp_cb, w);
        else if (r != LIBUSB_ERROR_TIMEOUT && r != 0) { free(buf); return -1; }
    }
    free(buf);
    return w->done ? 0 : -1;
}

static int dhcp_acquire(struct lease *out) {
    uint8_t f[700];
    srand(time(NULL) ^ getpid());
    for (int attempt = 1; attempt <= 6 && running; attempt++) {
        struct dhcp_wait w = {.xid = rand(), .want = 2};
        VLOG("DHCP DISCOVER (essai %d)", attempt);
        send_frame(f, dhcp_build(f, my_mac, 1, w.xid, 0, 0));
        if (dhcp_wait_for(&w, 3000) < 0) continue;
        struct lease offer = w.L;
        w.done = 0; w.want = 5;
        VLOG("DHCP OFFER %s, REQUEST", inet_ntoa((struct in_addr){offer.ip}));
        send_frame(f, dhcp_build(f, my_mac, 3, w.xid, offer.ip, offer.srv));
        if (dhcp_wait_for(&w, 3000) < 0) continue;
        *out = w.L;
        if (!out->gw) out->gw = out->srv;
        if (!out->mask) out->mask = htonl(0xffffff00);
        return 0;
    }
    return -1;
}

/* ---------- utun ---------- */
static int utun_open(void) {
#ifdef RNDIS_TESTING
    snprintf(utun_name, sizeof utun_name, "utun99");
    return open("/dev/null", O_RDWR);
#endif
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) { perror("socket(utun)"); return -1; }
    struct ctl_info ci;
    memset(&ci, 0, sizeof ci);
    strlcpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof ci.ctl_name);
    if (ioctl(fd, CTLIOCGINFO, &ci) < 0) { perror("CTLIOCGINFO"); close(fd); return -1; }
    struct sockaddr_ctl sc;
    memset(&sc, 0, sizeof sc);
    sc.sc_len = sizeof sc; sc.sc_family = AF_SYSTEM; sc.ss_sysaddr = AF_SYS_CONTROL;
    sc.sc_id = ci.ctl_id; sc.sc_unit = 0;
    if (connect(fd, (struct sockaddr *)&sc, sizeof sc) < 0) {
        perror("connect(utun) — lance avec sudo"); close(fd); return -1;
    }
    socklen_t l = sizeof utun_name;
    if (getsockopt(fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, utun_name, &l) < 0) {
        perror("UTUN_OPT_IFNAME"); close(fd); return -1;
    }
    return fd;
}

PRINTF_LIKE(1, 2) static void sh(const char *fmt, ...) {
    char cmd[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
#ifdef RNDIS_TESTING
    test_record_command(cmd);
    return;
#endif
    VLOG("$ %s", cmd);
    if (system(cmd) != 0) VLOG("(commande en échec)");
}

static void ipstr(uint32_t ip, char *out) { inet_ntop(AF_INET, &ip, out, INET_ADDRSTRLEN); }

/* DNS via SystemConfiguration : résolveur « supplémentaire » valable pour tous les domaines */
static void scutil_run(const char *script) {
#ifdef RNDIS_TESTING
    test_record_command(script);
    return;
#endif
    FILE *p = popen("/usr/sbin/scutil >/dev/null 2>&1", "w");
    if (p) { fputs(script, p); pclose(p); }
}

/* Déclare l'interface comme service réseau (comme un client VPN) pour que macOS
 * puisse l'élire service principal quand il n'y a plus de Wi-Fi, avec son DNS. */
static void dns_apply(const struct lease *L) {
    char d[INET_ADDRSTRLEN], ip[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN], s[768];
    ipstr(L->dns, d); ipstr(L->ip, ip); ipstr(L->gw, gw);
    snprintf(s, sizeof s,
             "d.init\nd.add Addresses * %s\nd.add DestAddresses * %s\nd.add InterfaceName %s\nd.add Router %s\n"
             "set State:/Network/Service/rndis-tether/IPv4\n"
             "d.init\nd.add ServerAddresses * %s\nd.add SupplementalMatchDomains * \"\"\nd.add InterfaceName %s\n"
             "set State:/Network/Service/rndis-tether/DNS\n",
             ip, gw, utun_name, gw, d, utun_name);
    VLOG("DNS -> %s", d);
    scutil_run(s);
    dns_applied = 1;
}

static void configure_iface(const struct lease *L) {
    char ip[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
    ipstr(L->ip, ip); ipstr(L->gw, gw); ipstr(L->mask, mask);
    sh("/sbin/ifconfig %s inet %s %s netmask %s mtu %d up", utun_name, ip, gw, mask, iface_mtu);
    if (want_route) {
        sh("/sbin/route -q -n add -net 0.0.0.0/1 %s", gw);
        sh("/sbin/route -q -n add -net 128.0.0.0/1 %s", gw);
        if (L->dns) dns_apply(L);
    }
}

/* ---------- IPv6 : NDP, annonces de routeur (RA), SLAAC ----------
 * Le téléphone parle IPv6 sur Ethernet ; utun ne transporte que des paquets IP. Comme pour
 * IPv4, on joue donc le rôle de la pile réseau : répondre aux sollicitations de voisin (NS)
 * du téléphone avec notre MAC, solliciter un routeur (RS), lire l'annonce (RA) pour fabriquer
 * une adresse SLAAC, puis relayer les paquets dans les deux sens. */
static uint16_t icmp6_csum(const uint8_t *src, const uint8_t *dst, const uint8_t *msg, int len) {
    uint32_t s = 0;
    for (int i = 0; i < 16; i += 2) s += src[i] << 8 | src[i + 1];
    for (int i = 0; i < 16; i += 2) s += dst[i] << 8 | dst[i + 1];
    s += ((uint32_t)len >> 16) + ((uint32_t)len & 0xffff); /* longueur « couche haute » sur 32 bits */
    s += 58;                                               /* prochain en-tête : ICMPv6 */
    for (int i = 0; i + 1 < len; i += 2) s += msg[i] << 8 | msg[i + 1];
    if (len & 1) s += msg[len - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
}

/* Construit l'annonce de voisin (NA) répondant à la sollicitation (NS) contenue dans la trame f.
 * Le téléphone est le seul voisin du lien : toute cible qu'il sollicite, c'est nous.
 * Retourne la taille de la trame (86 octets) ou -1 si la NS est invalide ou à ignorer. */
static int build_na(uint8_t *out, const uint8_t *f, int len) {
    static const uint8_t zero[16];
    if (len < 14 + 40 + 24) return -1;
    const uint8_t *ip = f + 14, *icmp = ip + 40;
    if ((ip[0] >> 4) != 6 || ip[6] != 58 || ip[7] != 255) return -1; /* hop limit 255 : lien local */
    int plen = ip[4] << 8 | ip[5];
    if (plen < 24 || plen > len - 14 - 40) return -1;
    if (icmp[0] != 135 || icmp[1] != 0) return -1;
    if (!memcmp(ip + 8, zero, 16) || ip[8] == 0xff) return -1;       /* DAD d'un autre / source multicast */
    const uint8_t *target = icmp + 8;
    if (target[0] == 0xff) return -1;
    memcpy(out, f + 6, 6); memcpy(out + 6, my_mac, 6); putbe16(out + 12, 0x86DD);
    uint8_t *o = out + 14;
    memset(o, 0, 40 + 32);
    o[0] = 0x60; putbe16(o + 4, 32); o[6] = 58; o[7] = 255;
    memcpy(o + 8, target, 16); memcpy(o + 24, ip + 8, 16);
    uint8_t *m = o + 40;
    m[0] = 136; m[4] = 0x60;                                          /* flags : Solicited + Override */
    memcpy(m + 8, target, 16);
    m[24] = 2; m[25] = 1; memcpy(m + 26, my_mac, 6);                  /* option : adresse MAC cible */
    putbe16(m + 2, icmp6_csum(o + 8, o + 24, m, 32));
    return 14 + 40 + 32;
}

struct ra { int have_prefix; uint8_t prefix[16]; uint8_t plen; uint16_t router_lifetime; uint8_t router[16];
            int gone; uint8_t gone_prefix[8]; uint16_t mtu;
            int nprefixes; uint8_t prefixes[4][8]; };   /* tous les /64 utilisables annoncés (au plus 4) */      /* gone : un /64 auto-configurable annoncé avec une durée de validité nulle */

/* Analyse une annonce de routeur (RA). Tout vient du réseau : bornes vérifiées à chaque option.
 * Retourne 0 si c'est une RA valide (have_prefix indique si un préfixe /64 auto-configurable y figure). */
static int parse_ra(const uint8_t *f, int len, struct ra *r) {
    static const uint8_t zero8[8];
    memset(r, 0, sizeof *r);
    if (len < 14 + 40 + 16) return -1;
    const uint8_t *ip = f + 14, *icmp = ip + 40;
    if ((ip[0] >> 4) != 6 || ip[6] != 58 || ip[7] != 255) return -1;
    if (ip[8] != 0xfe || (ip[9] & 0xc0) != 0x80) return -1;          /* la source doit être link-local */
    int plen = ip[4] << 8 | ip[5];
    if (plen < 16 || plen > len - 14 - 40) return -1;
    if (icmp[0] != 134 || icmp[1] != 0) return -1;
    r->router_lifetime = icmp[6] << 8 | icmp[7];
    memcpy(r->router, ip + 8, 16);                                   /* adresse link-local du routeur */
    const uint8_t *o = icmp + 16, *end = icmp + plen;
    while (o + 2 <= end) {
        int l = o[1] * 8;
        if (l == 0 || o + l > end) break;
        if (o[0] == 5 && l >= 8 && !r->mtu) {                        /* option MTU */
            uint32_t m = be32(o + 4);
            r->mtu = m > 0xffff ? 0xffff : (uint16_t)m;
        }
        if (o[0] == 3 && l >= 32) {                                  /* Prefix Information */
            const uint8_t *p = o + 16;
            int onlink_auto = (o[3] & 0x40) != 0;                    /* drapeau A : SLAAC autorisé */
            int link_local = p[0] == 0xfe && (p[1] & 0xc0) == 0x80;
            int usable = o[2] == 64 && onlink_auto && p[0] != 0xff && !link_local && memcmp(p, zero8, 8);
            if (usable && be32(o + 4) > 0 && r->nprefixes < 4) memcpy(r->prefixes[r->nprefixes++], p, 8);
            if (usable && be32(o + 4) > 0 && !r->have_prefix) {
                memcpy(r->prefix, p, 16);
                r->plen = o[2];
                r->have_prefix = 1;
            } else if (usable && be32(o + 4) == 0 && !r->gone) {     /* préfixe retiré par le routeur */
                memcpy(r->gone_prefix, p, 8);
                r->gone = 1;
            }
        }
        o += l;
    }
    return 0;
}

/* Sollicitation de routeur (RS) vers ff02::2, avec notre adresse MAC. */
static int build_rs(uint8_t *out) {
    static const uint8_t dmac[6] = {0x33, 0x33, 0, 0, 0, 2};
    memset(out, 0, 14 + 40 + 16);
    memcpy(out, dmac, 6); memcpy(out + 6, my_mac, 6); putbe16(out + 12, 0x86DD);
    uint8_t *o = out + 14;
    o[0] = 0x60; putbe16(o + 4, 16); o[6] = 58; o[7] = 255;
    o[8] = 0xfe; o[9] = 0x80;                                         /* fe80:: + EUI-64 de notre MAC */
    o[16] = my_mac[0] ^ 2; o[17] = my_mac[1]; o[18] = my_mac[2]; o[19] = 0xff; o[20] = 0xfe;
    o[21] = my_mac[3]; o[22] = my_mac[4]; o[23] = my_mac[5];
    o[24] = 0xff; o[25] = 0x02; o[39] = 2;                            /* ff02::2 : tous les routeurs */
    uint8_t *m = o + 40;
    m[0] = 133; m[8] = 1; m[9] = 1; memcpy(m + 10, my_mac, 6);
    putbe16(m + 2, icmp6_csum(o + 8, o + 24, m, 16));
    return 14 + 40 + 16;
}

/* Avec -r, tant que le tethering n'a pas d'IPv6, on empêche l'IPv6 du Mac de fuir par un autre réseau (Wi-Fi...).
 * Deux routes « reject » plus précises que la route par défaut font échouer aussitôt les destinations IPv6 hors-lien ;
 * les applications se rabattent sur l'IPv4, qui passe par le téléphone. Les réseaux locaux (routes /64, plus précises)
 * restent joignables. Idempotent. */
static void block_ipv6(void) {
    if (v6_blocked || !want_route || allow_v6_leak) return;
    sh("/sbin/route -q -n add -inet6 -net ::/1 ::1 -reject");
    sh("/sbin/route -q -n add -inet6 -net 8000::/1 ::1 -reject");
    v6_blocked = 1;
}

static void unblock_ipv6(void) {
    if (!v6_blocked) return;
    sh("/sbin/route -q -n delete -inet6 -net ::/1 ::1");
    sh("/sbin/route -q -n delete -inet6 -net 8000::/1 ::1");
    v6_blocked = 0;
}

static void configure_ipv6(void) {
    char a[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, v6_addr, a, sizeof a);
    LOG("IPv6: adresse %s/64 sur %s", a, utun_name);
    sh("/sbin/ifconfig %s inet6 %s prefixlen 64 alias", utun_name, a);
    if (want_route) {
        sh("/sbin/route -q -n add -inet6 -net ::/1 -interface %s", utun_name);
        sh("/sbin/route -q -n add -inet6 -net 8000::/1 -interface %s", utun_name);
        v6_routes = 1;
        /* Sans partie IPv6 dans le service réseau, macOS ne demande que des enregistrements A
         * à notre résolveur : les noms IPv6 seuls ne se résolvent pas. */
        char rt[INET6_ADDRSTRLEN], s[512];
        inet_ntop(AF_INET6, v6_router, rt, sizeof rt);
        snprintf(s, sizeof s,
                 "d.init\nd.add Addresses * %s\nd.add PrefixLength # 64\nd.add Router %s\nd.add InterfaceName %s\n"
                 "set State:/Network/Service/rndis-tether/IPv6\n", a, rt, utun_name);
        scutil_run(s);
        v6_svc = 1;
    }
}

/* Retire le service IPv6 et les routes du tethering (pas l'adresse : l'interface disparaît avec la session). */
static void teardown_ipv6_routes(void) {
    if (v6_svc) {
        scutil_run("remove State:/Network/Service/rndis-tether/IPv6\n");
        v6_svc = 0;
    }
    if (v6_routes) {
        sh("/sbin/route -q -n delete -inet6 -net ::/1 -interface %s", utun_name);
        sh("/sbin/route -q -n delete -inet6 -net 8000::/1 -interface %s", utun_name);
        v6_routes = 0;
    }
}

/* Le téléphone n'a plus d'IPv6 en cours de session (son internet est passé sur le Wi-Fi, par exemple) :
 * on retire ce qu'on avait installé et on remet le blocage, sinon l'IPv6 du Mac repartirait par un autre réseau
 * ou resterait sans réponse jusqu'au délai d'expiration. Une nouvelle RA avec préfixe réinstalle tout. */
static void deconfigure_ipv6(void) {
    char a[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, v6_addr, a, sizeof a);
    teardown_ipv6_routes();
    sh("/sbin/ifconfig %s inet6 %s -alias", utun_name, a);
    cfg6_done = 0;
    block_ipv6();
    LOG("IPv6: retiré (le téléphone n'en fournit plus)");
}

static void unconfigure_iface(void) {
    unblock_ipv6();
    teardown_ipv6_routes();
    if (dns_applied) {
        scutil_run("remove State:/Network/Service/rndis-tether/DNS\nremove State:/Network/Service/rndis-tether/IPv4\n");
        dns_applied = 0;
    }
    if (!want_route || !gw_ip) return;
    char gw[INET_ADDRSTRLEN];
    ipstr(gw_ip, gw);
    sh("/sbin/route -q -n delete -net 0.0.0.0/1 %s", gw);
    sh("/sbin/route -q -n delete -net 128.0.0.0/1 %s", gw);
}

/* ---------- threads ---------- */
static void handle_arp(const uint8_t *f, int len) {
    if (len < 42) return;
    uint16_t op = f[20] << 8 | f[21];
    const uint8_t *smac = f + 22, *sip = f + 28, *tip = f + 38;
    if (!memcmp(sip, &gw_ip, 4)) memcpy(gw_mac, smac, 6);
    if (op == 1 && !memcmp(tip, &my_ip, 4)) {
        uint8_t r[60];
        memset(r, 0, sizeof r);
        memcpy(r, smac, 6); memcpy(r + 6, my_mac, 6); putbe16(r + 12, 0x0806);
        putbe16(r + 14, 1); putbe16(r + 16, 0x0800); r[18] = 6; r[19] = 4; putbe16(r + 20, 2);
        memcpy(r + 22, my_mac, 6); memcpy(r + 28, &my_ip, 4);
        memcpy(r + 32, smac, 6); memcpy(r + 38, sip, 4);
        send_frame(r, sizeof r);
    }
}

/* Une RA valide arrive. Sans adresse : on prend le premier préfixe utilisable d'un routeur par défaut
 * (durée de vie > 0, RFC 4861 : sinon ce n'est pas un routeur par défaut). Avec une adresse : seul notre routeur
 * compte ; une durée de vie nulle ou le retrait de notre préfixe signifie que le téléphone n'a plus d'IPv6,
 * et chaque RA normale repousse l'échéance. */
static void ra_apply(const struct ra *r, int64_t now) {
    if (!v6_claimed) {
        if (!r->have_prefix || r->router_lifetime == 0) return;
        v6_claimed = 1;
        v6_expire = now + r->router_lifetime;
        __atomic_store_n(&v6_withdraw, 0, __ATOMIC_RELAXED);
        memcpy(v6_router, r->router, 16);
        v6_mtu = policy_mtu_from_peer(r->mtu, POLICY_MTU_MIN6);
        if (v6_mtu >= MTU) v6_mtu = 0;                                   /* 1500 ou plus : rien à changer */
        memcpy(v6_addr, r->prefix, 8);
        do { arc4random_buf(v6_addr + 8, 8); v6_addr[8] &= ~0x02; }     /* identifiant aléatoire, non EUI-64 */
        while (!memcmp(v6_addr + 8, (uint8_t[8]){0}, 8));
        __atomic_store_n(&v6_pending, 1, __ATOMIC_RELEASE);
        return;
    }
    if (memcmp(r->router, v6_router, 16)) return;                    /* une RA d'un autre routeur ne nous concerne pas */
    int ours = 0;
    for (int k = 0; k < r->nprefixes; k++) if (!memcmp(r->prefixes[k], v6_addr, 8)) ours = 1;
    if (r->router_lifetime == 0 || (r->gone && !memcmp(r->gone_prefix, v6_addr, 8)) ||
        (r->nprefixes > 0 && !ours))                                 /* le routeur annonce d'autres préfixes, plus le nôtre : il a changé */
        __atomic_store_n(&v6_withdraw, 1, __ATOMIC_RELEASE);         /* on retire tout ; la RA suivante installe le nouveau */
    else v6_expire = now + r->router_lifetime;
}

/* Vrai quand l'IPv6 obtenu n'est plus valable : retrait annoncé, ou plus aucune RA avant l'échéance. */
static int v6_lost(int64_t now) {
    if (!v6_claimed || __atomic_load_n(&v6_pending, __ATOMIC_ACQUIRE)) return 0;
    return __atomic_load_n(&v6_withdraw, __ATOMIC_ACQUIRE) || (v6_expire && now > v6_expire);
}

/* IPv6 reçu du téléphone : répond aux NS, exploite les RA, relaie le reste vers utun. */
static void handle_ipv6(const uint8_t *f, int len) {
    if (len < 14 + 40) return;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 6) return;
    int n = 40 + (ip[4] << 8 | ip[5]);
    if (n > len - 14 || n > 2048) return;                 /* tronqué ou trop grand : on jette */
    if (ip[6] == 58 && n >= 44) {                         /* ICMPv6 */
        if (ip[40] == 135) {                              /* sollicitation de voisin */
            uint8_t na[14 + 40 + 32];
            if (build_na(na, f, 14 + n) > 0) send_frame(na, sizeof na);
            return;
        }
        if (ip[40] == 134) {                              /* annonce de routeur : on s'en sert, le noyau ne la voit pas */
            struct ra r;
            if (parse_ra(f, 14 + n, &r) == 0) ra_apply(&r, now_s());
            return;
        }
    }
    uint8_t out[4 + 2048];
    putbe32(out, AF_INET6);
    memcpy(out + 4, ip, n);
    if (write(utun_fd, out, 4 + n) < 0 && verbose) perror("write utun");
}

static void rx_frame(const uint8_t *f, int len, void *arg) {
    (void)arg;
    if (len < 14) return;
    uint16_t et = f[12] << 8 | f[13];
    if (et == 0x0806) { handle_arp(f, len); return; }
    if (et == 0x86DD) { if (ipv6_on) handle_ipv6(f, len); return; }
    if (et != 0x0800 || len < 34) return;
    /* réponse du serveur DHCP à notre demande de renouvellement : traitée ici, jamais relayée */
    if (f[23] == 17 && dhcp_active) {
        struct lease L;
        enum dhcp_input res = DHCP_IGNORED;
        int match;
        pthread_mutex_lock(&dhcp_lock);
        match = dhcp_c.xid && dhcp_parse(f, len, my_mac, dhcp_c.xid, &L) == 0;
        if (match) res = dhcp_client_input(&dhcp_c, &L, now_s());
        uint32_t lease = dhcp_c.lease, t1 = dhcp_c.t1, t2 = dhcp_c.t2;
        pthread_mutex_unlock(&dhcp_lock);
        if (match) {
            if (res == DHCP_ACKED) LOG("DHCP: bail renouvelé (%us, T1 %us, T2 %us)", lease, t1, t2);
            else if (res == DHCP_NAKED) { LOG("DHCP: renouvellement refusé (NAK), relance de la session"); running = 0; }
            else if (res == DHCP_ADDR_CHANGED) { LOG("DHCP: le serveur impose une autre adresse, relance de la session"); running = 0; }
            return;
        }
    }
    if (!memcmp(f + 26, &gw_ip, 4)) memcpy(gw_mac, f + 6, 6);
    int iplen = f[16] << 8 | f[17];
    int n = len - 14;
    if (iplen >= 20 && iplen < n) n = iplen; /* retire le padding Ethernet */
    uint8_t out[4 + 2048];
    if (n > 2048) return;
    putbe32(out, AF_INET);
    memcpy(out + 4, f + 14, n);
    if (write(utun_fd, out, 4 + n) < 0 && verbose) perror("write utun");
}

static void *rx_thread(void *a) {
    (void)a;
    uint8_t *buf = malloc(RX_BUF);
    while (running) {
        thread_beat(1);
        int got = 0;
        int r = libusb_bulk_transfer(h, ep_in, buf, RX_BUF, &got, 500);
        if (r == 0 && got > 0) rx_parse(buf, got, rx_frame, NULL);
        else if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
            LOG("USB RX: %s", libusb_error_name(r));
            if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) running = 0;
            else usleep(10000);
        }
    }
    free(buf);
    return NULL;
}

static void *tx_thread(void *a) {
    (void)a;
    uint8_t in[4 + 2048], frame[14 + 2048];
    int fails = 0;
    struct pollfd p = {.fd = utun_fd, .events = POLLIN};
    while (running) {
        thread_beat(2);
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = read(utun_fd, in, sizeof in);
        if (n <= 4) continue;
        uint32_t af = be32(in);
        int len = n - 4;
        if (len > __atomic_load_n(&cur_mtu, __ATOMIC_RELAXED)) continue;
        memcpy(frame, gw_mac, 6); memcpy(frame + 6, my_mac, 6);
        if (af == AF_INET) {
            putbe16(frame + 12, 0x0800);
        } else if (af == AF_INET6 && ipv6_on && len >= 40) {
            putbe16(frame + 12, 0x86DD);
            if (in[4 + 24] == 0xff) {                          /* destination multicast : MAC 33:33:<4 derniers octets> */
                frame[0] = frame[1] = 0x33;
                memcpy(frame + 2, in + 4 + 24 + 12, 4);
            }
        } else {
            continue;
        }
        memcpy(frame + 14, in + 4, len);
        int r = send_frame(frame, 14 + len);
        if (r == 0) { fails = 0; continue; }
        VLOG("USB TX: %s", libusb_error_name(r));
        /* Un téléphone ré-énuméré peut ne répondre que par des timeouts : session morte */
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO || ++fails >= 5) {
            LOG("Liaison USB perdue (%s)", libusb_error_name(r));
            running = 0;
        }
    }
    return NULL;
}

/* Interruption + keepalive RNDIS */
static void *ctrl_thread(void *a) {
    (void)a;
    uint8_t n[16], resp[4096], f[700];
    time_t next_rs = time(NULL) + 1;
    int rs_sent = 0;
    while (running) {
        thread_beat(4);
        if (dhcp_active) {                                    /* T1 / T2 / expiration du bail (RFC 2131) */
            pthread_mutex_lock(&dhcp_lock);
            enum dhcp_action act = dhcp_client_tick(&dhcp_c, now_s(), arc4random() | 1);
            struct dhcp_client snap = dhcp_c;
            pthread_mutex_unlock(&dhcp_lock);
            if (act == DHCP_SEND_RENEW || act == DHCP_SEND_REBIND) {
                LOG("DHCP: %s", act == DHCP_SEND_RENEW ? "demande de renouvellement (unicast)" : "rebind (broadcast)");
                send_frame(f, dhcp_build_renew(f, my_mac, snap.xid, snap.ip, snap.srvmac, snap.srv, act == DHCP_SEND_RENEW));
            } else if (act == DHCP_RESTART) {
                LOG("DHCP: bail expiré sans réponse du serveur, relance de la session");
                running = 0;
            }
        }
        if (ipv6_on) {
            if (__atomic_load_n(&v6_pending, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&v6_pending, 0, __ATOMIC_RELAXED);
                request_config6();
            } else if (v6_lost(now_s())) {
                LOG("IPv6: le téléphone n'en fournit plus, retrait de la configuration");
                request_deconfig6();
                v6_claimed = 0; v6_expire = 0;
                __atomic_store_n(&v6_withdraw, 0, __ATOMIC_RELAXED);
                rs_sent = 0; next_rs = time(NULL) + 1;        /* on sollicite de nouveau : l'IPv6 peut revenir */
            } else if (!v6_claimed && time(NULL) >= next_rs) {
                uint8_t rs[14 + 40 + 16];                     /* on sollicite une RA (réponse quasi immédiate) */
                VLOG("IPv6: sollicitation de routeur (%d)", rs_sent + 1);
                send_frame(rs, build_rs(rs));
                rs_sent++;
                next_rs = time(NULL) + (rs_sent < 5 ? 3 : 30);   /* puis une fois par 30 s : le téléphone peut gagner l'IPv6 plus tard */
            }
        }
        if (!ep_int) { sleep(1); continue; }
        int got = 0;
        int r = libusb_interrupt_transfer(h, ep_int, n, sizeof n, &got, 500);
        if (r == LIBUSB_ERROR_NO_DEVICE) { running = 0; break; }
        if (r != 0 || got <= 0) continue;
        r = libusb_control_transfer(h, 0xA1, CDC_GET_ENCAPSULATED, 0, ctrl_if, resp, sizeof resp, 1000);
        if (r >= 12 && get32(resp) == RNDIS_KEEPALIVE_MSG) answer_keepalive(resp);
    }
    return NULL;
}

/* ---------- Device discovery ---------- */
static int is_rndis_ctrl(const struct libusb_interface_descriptor *d) {
    return (d->bInterfaceClass == 0xE0 && d->bInterfaceSubClass == 1 && d->bInterfaceProtocol == 3) ||
           (d->bInterfaceClass == 0xEF && d->bInterfaceSubClass == 4 && d->bInterfaceProtocol == 1) ||
           (d->bInterfaceClass == 2 && d->bInterfaceSubClass == 2 && d->bInterfaceProtocol == 0xFF);
}

static int find_device(uint16_t vid, uint16_t pid) {
    libusb_device **list;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    int found = 0, native = 0;
    for (ssize_t i = 0; i < cnt && !found; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) || (vid && (dd.idVendor != vid || dd.idProduct != pid))) continue;
        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(list[i], &cfg)) continue;
        int c = -1, d = -1; uint8_t ei = 0, eo = 0, eint = 0; int mps = 512;
        for (int k = 0; k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface_descriptor *a = &cfg->interface[k].altsetting[0];
            if (a->bInterfaceClass == 2 && (a->bInterfaceSubClass == 6 || a->bInterfaceSubClass == 13)) native = 1;
            if (c < 0 && is_rndis_ctrl(a)) {
                c = a->bInterfaceNumber;
                for (int e = 0; e < a->bNumEndpoints; e++)
                    if ((a->endpoint[e].bmAttributes & 3) == 3 && (a->endpoint[e].bEndpointAddress & 0x80))
                        eint = a->endpoint[e].bEndpointAddress;
            }
        }
        for (int k = 0; c >= 0 && k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface_descriptor *a = &cfg->interface[k].altsetting[0];
            if (a->bInterfaceClass != 0x0A) continue;
            for (int e = 0; e < a->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &a->endpoint[e];
                if ((ep->bmAttributes & 3) != 2) continue;
                if (ep->bEndpointAddress & 0x80) ei = ep->bEndpointAddress;
                else { eo = ep->bEndpointAddress; mps = ep->wMaxPacketSize; }
            }
            if (ei && eo) { d = a->bInterfaceNumber; break; }
        }
        libusb_free_config_descriptor(cfg);
        if (c < 0 || d < 0) continue;
        int r = libusb_open(list[i], &h);
        if (r) { LOG("Ouverture USB %04x:%04x: %s (essaie avec sudo)", dd.idVendor, dd.idProduct, libusb_error_name(r)); h = NULL; continue; }
        LOG("Appareil RNDIS trouvé: %04x:%04x", dd.idVendor, dd.idProduct);
        ctrl_if = c; data_if = d; ep_in = ei; ep_out = eo; ep_int = eint; mps_out = mps ? mps : 512;
        found = 1;
    }
    libusb_free_device_list(list, 1);
    static int warned;
    if (!found && native && !warned && (warned = 1))
        LOG("Ce téléphone expose CDC-ECM/NCM : macOS le gère nativement, ce driver n'est pas nécessaire.");
    return found ? 0 : -1;
}

static int session(void);

/* Fin d'une session normale : rend le bail au serveur (RFC 2131 §3.1.7) pour ne pas laisser une adresse
 * orpheline dans son pool. Retourne 0 s'il n'y avait rien à rendre, 1 si c'est fait, -1 si l'envoi a échoué
 * (par exemple parce que le téléphone a été débranché). */
static int release_lease(void) {
    struct dhcp_client c;
    int active;
    pthread_mutex_lock(&dhcp_lock);
    c = dhcp_c;
    active = dhcp_active;
    dhcp_active = 0;                                        /* plus aucun renouvellement après le rendu */
    pthread_mutex_unlock(&dhcp_lock);
    if (!active || c.state == DHCP_EXPIRED || !c.ip || !c.srv) return 0;
    uint8_t f[700];
    if (send_frame(f, dhcp_build_release(f, my_mac, arc4random() | 1, c.ip, c.srvmac, c.srv)) != 0) return -1;
    LOG("DHCP: bail rendu au serveur");
    return 1;
}

/* Bascule le téléphone en RNDIS via adb, exécuté en tant qu'utilisateur appelant sudo
 * (ses clés ADB et son PATH, pas ceux de root). */
#define ADB_USER_MAX 64
static int adb_user_valid(const char *u) {
    size_t n = u ? strlen(u) : 0;
    if (n == 0 || n > ADB_USER_MAX) return 0;
    for (const char *c = u; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') return 0;
    return 1;
}

/* Lit la sortie d'une commande fixe. Dans les tests, la sortie est celle que le test a préparée. */
#ifdef RNDIS_TESTING
const char *test_capture_text(const char *cmd);
#endif
static void sh_capture(char *out, size_t cap, const char *fmt, ...) PRINTF_LIKE(3, 4);
static void sh_capture(char *out, size_t cap, const char *fmt, ...) {
    char cmd[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    out[0] = '\0';
#ifdef RNDIS_TESTING
    test_record_command(cmd);
    snprintf(out, cap, "%s", test_capture_text(cmd));
    return;
#endif
    VLOG("$ %s", cmd);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = '\0';
    pclose(p);
}

static int adb_last_status = -1;                 /* pour ne pas répéter le même message toutes les 10 s */

/* Bascule le téléphone en Modem USB. Avant, on regarde ce qu'adb voit : un échec silencieux ne dit pas quoi faire
 * (adb absent, débogage USB coupé, téléphone non autorisé, plusieurs téléphones). Chaque problème est écrit une
 * fois dans le journal, avec la marche à suivre. */
static void adb_enable(void) {
    const char *u = adb_user ? adb_user : getenv("SUDO_USER");
    if (!adb_user_valid(u)) return;
    char out[2048];
    sh_capture(out, sizeof out, "/usr/bin/sudo -u %s /bin/zsh -lc 'command -v adb >/dev/null 2>&1 || echo ADB_MISSING; adb devices 2>&1'", u);
    struct adb_devices d;
    const char *serial;
    adb_parse_devices(out, &d);
    enum adb_status st = adb_choose(&d, adb_serial, &serial);
    if (st != ADB_OK) {
        if ((int)st != adb_last_status) LOG("adb : %s", adb_status_message(st));
        adb_last_status = (int)st;
        return;
    }
    adb_last_status = (int)st;
    if (*serial) sh("/usr/bin/sudo -u %s /bin/zsh -lc 'adb -s %s shell svc usb setFunctions rndis' >/dev/null 2>&1 || true", u, serial);
    else sh("/usr/bin/sudo -u %s /bin/zsh -lc 'adb shell svc usb setFunctions rndis' >/dev/null 2>&1 || true", u);   /* adb échoue souvent car l'USB se ré-énumère */
}

static void usage(const char *p) {
    fprintf(stderr, "Usage: sudo %s [-r] [-w] [-a] [-4] [-U user] [-S serial] [-W user] [-v] [-d vid:pid]\n"
                    "       %s --version\n"
                    "       %s --diagnose   rapport à coller dans un ticket (sans sudo)\n"
                    "  -r  route tout le trafic via le téléphone (routes 0/1 et 128/1, retirées à la sortie)\n"
                    "      (applique aussi le DNS du téléphone, retiré à la sortie)\n"
                    "  -w  attendre le téléphone et se reconnecter après un débranchement\n"
                    "  -a  activer le Modem USB du téléphone via adb (débogage USB requis)\n"
                    "  -U  utilisateur pour adb (défaut: SUDO_USER ; requis en LaunchDaemon)\n"
                    "  -S  numéro de série du téléphone pour adb quand plusieurs appareils sont branchés\n"
                    "  -W  utilisateur sans privilèges du worker (défaut: nobody)\n"
                    "  -4  IPv4 uniquement (désactive le relais IPv6)\n"
                    "  -O  accepter un sous-réseau du téléphone qui recouvre un réseau déjà utilisé\n"
                    "  -L  avec -r, laisser l'IPv6 sortir par les autres réseaux quand le tethering n'en a pas\n"
                    "  -v  logs détaillés\n"
                    "  -d  cibler un appareil précis (hex), ex: 18d1:4ee3\n"
                    "  -V  afficher la version\n", p, p, p);
}

/* ---------- Superviseur (root) ---------- */
static time_t last_adb_run;
static int allow_overlap;               /* -O : accepte un sous-réseau qui recouvre un réseau existant */
#define IPC_RATE_MAX 30                 /* un worker normal fait moins de dix demandes par session */
#define IPC_RATE_WINDOW 10              /* secondes */

/* Réseaux IPv4 actuellement connectés au Mac (hors boucle locale et hors notre interface utun). */
struct connected_net { uint32_t ip, mask; };

#ifdef RNDIS_TESTING
int test_list_connected(struct connected_net *out, int max);   /* fourni par les tests */
#endif

/* Interroge le système : réseaux IPv4 des interfaces actives, hors boucle locale et hors `skip_name`. */
static int list_connected_system(struct connected_net *out, int max, const char *skip_name) {
    struct ifaddrs *all, *p;
    int n = 0;
    if (getifaddrs(&all) < 0) return 0;
    for (p = all; p && n < max; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        if (skip_name && skip_name[0] && !strcmp(p->ifa_name, skip_name)) continue;
        out[n].ip = ((struct sockaddr_in *)(void *)p->ifa_addr)->sin_addr.s_addr;
        out[n].mask = ((struct sockaddr_in *)(void *)p->ifa_netmask)->sin_addr.s_addr;
        n++;
    }
    freeifaddrs(all);
    return n;
}

static int list_connected(struct connected_net *out, int max) {
#ifdef RNDIS_TESTING
    return test_list_connected(out, max);                  /* les tests simulent la liste ; list_connected_system est testée à part */
#else
    return list_connected_system(out, max, utun_name);
#endif
}

/* Retourne un code POLICY_* non nul si le sous-réseau du téléphone masquerait un réseau déjà utilisé. */
static int check_not_shadowing(uint32_t ip, uint32_t mask) {
    struct connected_net nets[64];
    int n = list_connected(nets, 64);
    for (int k = 0; k < n; k++) {
        if (policy_check_overlap(ip, mask, nets[k].ip, nets[k].mask) == POLICY_OK) continue;
        char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
        uint32_t net = ip & mask, onet = nets[k].ip & nets[k].mask;
        inet_ntop(AF_INET, &net, a, sizeof a);
        inet_ntop(AF_INET, &onet, b, sizeof b);
        LOG("Configuration IPv4 refusée : le sous-réseau %s/%d du téléphone recouvre un réseau déjà utilisé par ce Mac (%s/%d). "
            "Changer le sous-réseau du téléphone, ou lancer avec -O pour l'accepter.",
            a, __builtin_popcount(ntohl(mask)), b, __builtin_popcount(ntohl(nets[k].mask)));
        return POLICY_OVERLAP;
    }
    return POLICY_OK;
}

/* Traite une demande du worker. Il ne peut demander que des actions précises, et chaque valeur
 * est validée par policy.c : un worker compromis ne peut pas faire exécuter autre chose. */
static void handle_request(int sock, const struct ipc_msg *rq) {
    struct ipc_msg rp;
    int pass_fd = -1;
    memset(&rp, 0, sizeof rp);
    rp.type = IPC_REPLY;
    switch (rq->type) {
    case IPC_REQ_UTUN:
        if (utun_fd >= 0) { rp.status = 200; break; }                /* une seule interface par worker */
        if ((utun_fd = utun_open()) < 0) { rp.status = 201; break; }
        rp.len = (uint16_t)strlen(utun_name);
        memcpy(rp.data, utun_name, rp.len);
        pass_fd = utun_fd;
        break;
    case IPC_REQ_CONFIG4: {
        uint32_t ip, gw, mask, dns;
        struct lease L;
        if (utun_fd < 0 || cfg4_done || (rq->len != 16 && rq->len != 18)) { rp.status = 200; break; }
        memcpy(&ip, rq->data, 4); memcpy(&gw, rq->data + 4, 4);
        memcpy(&mask, rq->data + 8, 4); memcpy(&dns, rq->data + 12, 4);
        int e = policy_check_v4(ip, gw, mask, dns);
        if (e) { LOG("Configuration IPv4 refusée : %s", policy_strerror(e)); rp.status = (uint8_t)e; break; }
        unsigned mtu4 = rq->len == 18 ? (unsigned)(rq->data[16] << 8 | rq->data[17]) : MTU;
        if ((e = policy_check_mtu(mtu4, 0)) != POLICY_OK) { LOG("Configuration IPv4 refusée : %s", policy_strerror(e)); rp.status = (uint8_t)e; break; }
        if (!allow_overlap && (e = check_not_shadowing(ip, mask)) != POLICY_OK) { rp.status = (uint8_t)e; break; }
        memset(&L, 0, sizeof L);
        L.ip = ip; L.gw = gw; L.mask = mask; L.dns = dns;
        my_ip = ip; gw_ip = gw;
        iface_mtu = (int)mtu4;
        configure_iface(&L);
        cfg4_done = 1;
        block_ipv6();                                                /* en attendant l'IPv6 du tethering, s'il en vient */
        break;
    }
    case IPC_REQ_CONFIG6: {
        if (!ipv6_on || utun_fd < 0 || !cfg4_done || cfg6_done || (rq->len != 32 && rq->len != 34)) { rp.status = 200; break; }
        int e = policy_check_v6(rq->data, rq->data + 16);
        if (e) { LOG("Configuration IPv6 refusée : %s", policy_strerror(e)); rp.status = (uint8_t)e; break; }
        unsigned mtu6 = rq->len == 34 ? (unsigned)(rq->data[32] << 8 | rq->data[33]) : 0;
        if (mtu6 && (e = policy_check_mtu(mtu6, 1)) != POLICY_OK) { LOG("Configuration IPv6 refusée : %s", policy_strerror(e)); rp.status = (uint8_t)e; break; }
        memcpy(v6_addr, rq->data, 16);
        memcpy(v6_router, rq->data + 16, 16);
        unblock_ipv6();                                              /* les vraies routes remplacent les routes « reject » */
        configure_ipv6();
        cfg6_done = 1;
        if (mtu6 && (int)mtu6 < iface_mtu) {                         /* un seul MTU pour l'interface : le plus petit des deux */
            sh("/sbin/ifconfig %s mtu %u", utun_name, mtu6);
            iface_mtu = (int)mtu6;
        }
        break;
    }
    case IPC_REQ_DECONFIG6:
        if (!ipv6_on || utun_fd < 0 || !cfg6_done || rq->len != 0) { rp.status = 200; break; }
        deconfigure_ipv6();
        break;
    case IPC_REQ_STATUS: {
        int e = policy_check_status((const char *)rq->data, rq->len);
        if (e == POLICY_OK && !strncmp((const char *)rq->data, "connected ", 10)) {
            size_t n = strlen(utun_name);                            /* l'interface annoncée doit être la nôtre */
            if (utun_fd < 0 || rq->len < n + 1 || memcmp(rq->data + rq->len - n, utun_name, n) ||
                rq->data[rq->len - n - 1] != ' ')
                e = POLICY_BAD_STATUS;
        }
        if (e) { rp.status = (uint8_t)e; break; }
        char text[POLICY_STATUS_MAX + 1];
        memcpy(text, rq->data, rq->len);
        text[rq->len] = '\0';
        status_tick(text);
        break;
    }
    case IPC_REQ_ADB:
        if (!auto_adb || time(NULL) - last_adb_run < 10) { rp.status = 200; break; }
        last_adb_run = time(NULL);
        adb_enable();
        break;
    default:
        rp.status = 255;
    }
    if (ipc_send(sock, &rp, pass_fd) < 0) LOG("Réponse au worker non envoyée");
}

/* Abandon définitif des privilèges. Échoue si le retour à root reste possible. */
static int drop_privileges(uid_t uid, gid_t gid) {
    if (setgroups(0, NULL) < 0) return -1;
    if (setgid(gid) < 0 || setuid(uid) < 0) return -1;
    if (getuid() != uid || geteuid() != uid || getgid() != gid || getegid() != gid) return -1;
    if (setuid(0) == 0 || setgid(0) == 0) return -1;                 /* ne doit plus jamais redevenir root */
    return 0;
}

/* ---------- Worker (sans privilèges) ---------- */
static int worker_run(unsigned vid, unsigned pid) {
    if (libusb_init(&ctx)) { LOG("libusb_init a échoué"); return EXIT_FATAL; }
    int rc = 0, announced = 0;
    time_t last_adb = 0;
    while (!quit && !is_disabled()) {
        int ok = find_device(vid, pid) == 0;
        if (!ok && auto_adb && time(NULL) - last_adb >= 10) {
            last_adb = time(NULL);
            LOG("Activation du Modem USB via adb…");
            request_adb();
            sleep(3);
            ok = find_device(vid, pid) == 0;
        }
        if (!ok) {
            if (!wait_mode) {
                LOG("Aucun appareil RNDIS. Branche le téléphone puis active Réglages > Point d'accès > Partage de connexion USB.");
                rc = EXIT_NO_DEVICE;
                break;
            }
            if (!announced) { LOG("En attente d'un téléphone en mode Modem USB…"); announced = 1; }
            status_tick("waiting");
            sleep(2);
            continue;
        }
        rc = session();                  /* une seule session par worker : le suivant repart d'un état neuf */
        break;
    }
    libusb_exit(ctx);
    return quit ? 0 : rc;
}

/* Pas de fichier core : il contiendrait le trafic en clair (worker) ou la configuration (superviseur). */
static int disable_core_dumps(void) {
    struct rlimit rl = {0, 0};
    return setrlimit(RLIMIT_CORE, &rl);
}

#define WORKER_MAX_FDS 128
/* Le worker n'a besoin que de quelques descripteurs : stdin/out/err, le canal, l'interface utun, libusb.
 * Plafonner ce nombre limite les dégâts d'un worker détourné (fuite, boucle d'ouverture). */
static int worker_limits(void) {
    struct rlimit rl = {WORKER_MAX_FDS, WORKER_MAX_FDS};
    if (disable_core_dumps() < 0) return -1;
    return setrlimit(RLIMIT_NOFILE, &rl);
}

static int worker_main(int sock, unsigned vid, unsigned pid, uid_t uid, gid_t gid) {
    worker_mode = 1;
    ipc_sock = sock;
    for (int fd = 3; fd < 256; fd++)     /* ne garde que stdin/stdout/stderr et le canal du superviseur */
        if (fd != sock) close(fd);
    if (worker_limits() < 0) VLOG("limites du worker non appliquées (%s)", strerror(errno));
    if (drop_privileges(uid, gid) < 0) {
        LOG("Impossible d'abandonner les privilèges (%s)", strerror(errno));
        return EXIT_FATAL;
    }
    VLOG("démarré avec uid=%u gid=%u", (unsigned)getuid(), (unsigned)getgid());
    return worker_run(vid, pid);
}

/* ---------- Journal ----------
 * Le worker n'écrit plus dans le fichier journal : ses sorties passent par un tube que le superviseur recopie
 * après nettoyage (voir logsan.h) et limitation de débit. Un worker détourné ne peut donc pas se faire passer
 * pour le superviseur, ni polluer ou remplir le journal. Le superviseur suit aussi la rotation du fichier. */
#define LOG_FILE "/var/log/rndis-tether.log"
#define WORKER_LOG_LINES 100            /* lignes recopiées au plus, par fenêtre de WORKER_LOG_WINDOW secondes */
#define WORKER_LOG_WINDOW 10

static dev_t log_dev;
static ino_t log_ino;
static int log_tracked;                 /* stderr est bien le fichier journal : on le suit */

/* Retient le fichier journal si stderr en est un (service lancé par launchd) ; sinon (terminal, -v) ne fait rien. */
static void log_track(const char *path) {
    struct stat fs, ps;
    log_tracked = 0;
    if (fstat(STDERR_FILENO, &fs) < 0 || !S_ISREG(fs.st_mode)) return;
    if (stat(path, &ps) < 0 || ps.st_dev != fs.st_dev || ps.st_ino != fs.st_ino) return;
    log_dev = fs.st_dev; log_ino = fs.st_ino; log_tracked = 1;
}

/* Après une rotation (newsyslog renomme le fichier puis le compresse), launchd continue d'écrire dans l'ancien
 * fichier : on rouvre le nouveau. Retourne 1 si c'est fait, 0 s'il n'y avait rien à faire, -1 en cas d'échec. */
static int log_reopen_if_rotated(const char *path) {
    struct stat ps, os;
    if (!log_tracked) return 0;
    if (stat(path, &ps) == 0 && ps.st_dev == log_dev && ps.st_ino == log_ino) return 0;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return -1;
    int out_same = fstat(STDOUT_FILENO, &os) == 0 && os.st_dev == log_dev && os.st_ino == log_ino;
    dup2(fd, STDERR_FILENO);
    if (out_same) dup2(fd, STDOUT_FILENO);                           /* la sortie de route(8) et ifconfig(8) va au même fichier */
    close(fd);
    if (stat(path, &ps) == 0) { log_dev = ps.st_dev; log_ino = ps.st_ino; }
    return 1;
}

struct log_pump { int fd; int stop; pthread_t th; };

static void log_emit(const char *raw, size_t n, struct rate_limit *rl, unsigned *dropped) {
    char line[LOG_LINE_MAX];
    if (!log_sanitize(raw, n, line, sizeof line)) return;
    if (!rate_allow(rl, now_s(), WORKER_LOG_LINES, WORKER_LOG_WINDOW)) { (*dropped)++; return; }
    if (*dropped) { fprintf(stderr, "[root] %u lignes du worker supprimées (trop de messages)\n", *dropped); *dropped = 0; }
    fprintf(stderr, "[worker] %s\n", line);
}

/* Recopie les lignes du worker. S'arrête à la fin du tube, ou quand on le demande et qu'il n'y a plus rien à lire
 * (un processus enfant du worker pourrait garder le tube ouvert). */
static void *log_pump_thread(void *arg) {
    struct log_pump *p = arg;
    char buf[LOG_LINE_MAX * 2];
    size_t have = 0;
    struct rate_limit rl = {0, 0};
    unsigned dropped = 0;
    for (;;) {
        struct pollfd pf = {.fd = p->fd, .events = POLLIN};
        int pr = poll(&pf, 1, 200);
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0) {
            ssize_t n = read(p->fd, buf + have, sizeof buf - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;                                       /* fin du tube : le worker a disparu */
            have += (size_t)n;
        } else if (pr == 0 && __atomic_load_n(&p->stop, __ATOMIC_ACQUIRE)) {
            break;
        }
        size_t start = 0;
        for (size_t i = 0; i < have; i++)
            if (buf[i] == '\n') { log_emit(buf + start, i - start, &rl, &dropped); start = i + 1; }
        if (have - start >= LOG_LINE_MAX) { log_emit(buf + start, have - start, &rl, &dropped); start = have; }   /* ligne sans fin */
        memmove(buf, buf + start, have - start);
        have -= start;
    }
    if (have) log_emit(buf, have, &rl, &dropped);
    if (dropped) fprintf(stderr, "[root] %u lignes du worker supprimées (trop de messages)\n", dropped);
    return NULL;
}

/* Lance un worker et le sert jusqu'à sa fin ; nettoie ensuite la configuration réseau. Retourne son code de sortie. */
static int run_worker_once(unsigned vid, unsigned pid, uid_t uid, gid_t gid) {
    int sv[2], status = 0, reaped = 0;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return EXIT_FATAL; }
    int lp[2] = {-1, -1};
    if (pipe(lp) < 0) lp[0] = lp[1] = -1;                            /* sans tube : le worker hérite de stderr, comme avant */
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) {
        perror("fork"); close(sv[0]); close(sv[1]);
        if (lp[0] >= 0) { close(lp[0]); close(lp[1]); }
        return EXIT_FATAL;
    }
    if (child == 0) {
        close(sv[0]);
        if (lp[0] >= 0) {                                            /* ses sorties vont au tube, plus au fichier journal */
            dup2(lp[1], STDERR_FILENO); dup2(lp[1], STDOUT_FILENO);
            close(lp[0]); close(lp[1]);
        }
#ifdef RNDIS_TESTING
        if (test_worker_entry) _exit(test_worker_entry(sv[1]));       /* faux worker des tests */
#endif
        _exit(worker_main(sv[1], vid, pid, uid, gid));
    }
    close(sv[1]);
    struct log_pump pump = {.fd = lp[0]};
    int pumping = lp[0] >= 0;
    if (pumping) {
        close(lp[1]);
        if (pthread_create(&pump.th, NULL, log_pump_thread, &pump) != 0) { pumping = 0; close(lp[0]); }
    }
    utun_fd = -1; cfg4_done = cfg6_done = 0; my_ip = gw_ip = 0;
    v6_routes = v6_svc = v6_blocked = 0; iface_mtu = MTU;
    struct rate_limit rl = {0, 0};
    int64_t last_seen = now_s();                                     /* dernier signe de vie du worker */
    int64_t prev_tick = last_seen;                                   /* fin du tour précédent (détection de la veille) */

    for (;;) {
        struct ipc_msg m;
        int fd = -1;
        int r = ipc_recv(sv[0], &m, &fd, 1000);
        int64_t now = now_s();
        if (wake_detected(now, prev_tick, wake_gap_s)) {
            LOG("Réveil du Mac détecté (environ %d s de veille) : la session est relancée", (int)(now - prev_tick));
            if (fd >= 0) close(fd);
            kill(child, SIGTERM);
            break;
        }
        if (r == 1) {
            if (fd >= 0) close(fd);                                  /* un worker n'a aucune raison de nous passer un descripteur */
            if (!rate_allow(&rl, now_s(), IPC_RATE_MAX, IPC_RATE_WINDOW)) {
                LOG("Le worker envoie trop de demandes (plus de %d en %d s) : arrêt du worker", IPC_RATE_MAX, IPC_RATE_WINDOW);
                kill(child, SIGKILL);
                break;
            }
            handle_request(sv[0], &m);
            last_seen = now_s();
        } else if (r < 0) {
            break;                                                   /* worker disparu ou message invalide */
        }
        if (watchdog_expired(now_s(), last_seen, watchdog_s)) {
            LOG("Le worker ne donne plus signe de vie depuis plus de %d s : arrêt du worker", watchdog_s);
            kill(child, SIGKILL);
            break;
        }
        prev_tick = now_s();
        status_tick(NULL);                                           /* rafraîchit le fichier d'état */
        log_reopen_if_rotated(LOG_FILE);
        if (quit || is_disabled()) { kill(child, SIGTERM); break; }
        if (waitpid(child, &status, WNOHANG) == child) { reaped = 1; break; }
    }

    if (!reaped) {                                                   /* s'assure que le worker est bien terminé */
        kill(child, SIGTERM);
        for (int k = 0; k < 30 && !reaped; k++) {
            if (waitpid(child, &status, WNOHANG) == child) reaped = 1; else usleep(100000);
        }
        if (!reaped) { kill(child, SIGKILL); waitpid(child, &status, 0); }
    }
    if (pumping) {                                                   /* dernières lignes du worker, puis fermeture du tube */
        __atomic_store_n(&pump.stop, 1, __ATOMIC_RELEASE);
        pthread_join(pump.th, NULL);
        close(pump.fd);
    }
    unconfigure_iface();                                             /* routes, DNS : retirés par le superviseur, jamais par le worker */
    if (utun_fd >= 0) { close(utun_fd); utun_fd = -1; }
    close(sv[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int supervisor_main(unsigned vid, unsigned pid) {
    struct passwd *pw = getpwnam(worker_user);
    if (!pw || pw->pw_uid == 0) {                                    /* jamais de worker en root */
        LOG("Utilisateur du worker introuvable ou root : %s", worker_user);
        return 1;
    }
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    log_track(LOG_FILE);
    if (disable_core_dumps() < 0) LOG("Impossible de désactiver les fichiers core (%s)", strerror(errno));
    LOG("Superviseur %s démarré ; le worker tournera sans privilèges (utilisateur %s)", RNDIS_VERSION, worker_user);
    if (want_route && !allow_v6_leak) {                              /* routes « reject » restées d'un arrêt brutal précédent */
        sh("/sbin/route -q -n delete -inet6 -net ::/1 ::1");
        sh("/sbin/route -q -n delete -inet6 -net 8000::/1 ::1");
    }
    int rc = 0;
    while (!quit) {
        if (is_disabled()) { status_tick("disabled"); log_reopen_if_rotated(LOG_FILE); sleep(1); continue; }
        time_t t0 = time(NULL);
        int code = run_worker_once(vid, pid, uid, gid);
        if (quit) break;
        if (code == EXIT_FATAL) { LOG("Le worker n'a pas pu démarrer : arrêt"); rc = 1; break; }
        if (!wait_mode) { rc = code == 0 ? 0 : 1; break; }
        status_tick("waiting");
        LOG("Session terminée, nouvelle tentative…");
        sleep(time(NULL) - t0 < 3 ? 5 : 2);                          /* évite une boucle serrée si le worker échoue aussitôt */
    }
    set_status("stopped");
    return rc;
}

/* ---------- rndis-tether --diagnose ----------
 * Rapport à coller dans un ticket. Sans privilèges, en lecture seule : il ne modifie rien. Toutes les
 * commandes sont des chaînes fixes (aucune entrée utilisateur) et chaque ligne passe par diag_redact(). */
static int diag_exec(FILE *out, const char *cmd, void (*peek)(const char *, struct diag_facts *), struct diag_facts *f) {
    FILE *p = popen(cmd, "r");
    char raw[512], red[1100];
    int n = 0;
    if (!p) { fprintf(out, "  (commande impossible)\n"); return 0; }
    while (fgets(raw, sizeof raw, p)) {
        raw[strcspn(raw, "\n")] = '\0';
        if (peek) peek(raw, f);
        diag_redact(raw, red, sizeof red);
        fprintf(out, "  %s\n", red);
        n++;
    }
    pclose(p);
    if (!n) fprintf(out, "  (aucune sortie)\n");
    return n;
}

static void diag_peek_ps(const char *line, struct diag_facts *f) {
    while (*line == ' ') line++;
    if (!strncmp(line, "root ", 5)) f->supervisor_running = 1;
    else f->worker_running = 1;                        /* le worker tourne sous un autre utilisateur (nobody) */
}
static void diag_peek_route4(const char *line, struct diag_facts *f) { if (strstr(line, "utun")) f->tether_route = 1; }
static void diag_peek_route6(const char *line, struct diag_facts *f) { if (strstr(line, "lo0")) f->v6_reject = 1; }

static void diag_usb(FILE *out, struct diag_facts *f) {
    libusb_device **list;
    ssize_t cnt;
    int shown = 0;
    if (libusb_init(&ctx)) { fprintf(out, "  (libusb indisponible)\n"); return; }
    cnt = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor dd;
        struct libusb_config_descriptor *cfg;
        int seen[5] = {0};
        if (libusb_get_device_descriptor(list[i], &dd) || libusb_get_active_config_descriptor(list[i], &cfg)) continue;
        for (int k = 0; k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface_descriptor *a = &cfg->interface[k].altsetting[0];
            seen[diag_iface_kind(a->bInterfaceClass, a->bInterfaceSubClass, a->bInterfaceProtocol)] = 1;
        }
        libusb_free_config_descriptor(cfg);
        if (!(seen[DIAG_IF_RNDIS] || seen[DIAG_IF_NATIVE_NET] || seen[DIAG_IF_ADB] || seen[DIAG_IF_MTP])) continue;
        fprintf(out, "  %04x:%04x", dd.idVendor, dd.idProduct);
        for (int k = 1; k < 5; k++) if (seen[k]) fprintf(out, "  %s", diag_iface_name((enum diag_iface)k));
        fprintf(out, "\n");
        f->usb_rndis |= seen[DIAG_IF_RNDIS]; f->usb_native |= seen[DIAG_IF_NATIVE_NET];
        f->usb_adb |= seen[DIAG_IF_ADB]; f->usb_mtp |= seen[DIAG_IF_MTP];
        shown++;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    ctx = NULL;
    if (!shown) fprintf(out, "  (aucun appareil avec une interface réseau, ADB ou MTP)\n");
}

static int diagnose_main(void) {
    struct diag_facts f;
    char *body = NULL, verdict[1500], status[160] = "";
    size_t blen = 0;
    FILE *out = open_memstream(&body, &blen);
    memset(&f, 0, sizeof f);
    if (!out) { fprintf(stderr, "mémoire insuffisante\n"); return 1; }

    fprintf(out, "\n== Système ==\n");
    fprintf(out, "  rndis-tether %s\n", RNDIS_VERSION);
    diag_exec(out, "/usr/bin/sw_vers -productVersion 2>&1; /usr/bin/uname -m 2>&1", NULL, NULL);

    fprintf(out, "\n== Service ==\n");
    diag_exec(out, "/bin/ps -axo user=,etime=,command= | /usr/bin/grep '[r]ndis-tether' | /usr/bin/grep -v -e '--diagnose' | /usr/bin/cut -c1-120",
              diag_peek_ps, &f);
    FILE *sf = fopen(STATUS_FILE, "r");
    if (sf) {
        if (fgets(status, sizeof status, sf)) status[strcspn(status, "\n")] = '\0';
        fclose(sf);
        f.status_known = 1;
        char red[200];
        diag_redact(status, red, sizeof red);
        fprintf(out, "  état : %s\n", red);
    } else fprintf(out, "  état : fichier absent\n");
    f.state = diag_state_parse(status);
    f.disabled_flag = is_disabled();
    if (f.disabled_flag) fprintf(out, "  désactivé depuis l'icône de menu\n");

    fprintf(out, "\n== USB (appareils avec interface réseau, ADB ou MTP) ==\n");
    diag_usb(out, &f);

    fprintf(out, "\n== ADB (état des appareils, sans numéro de série) ==\n");
    diag_exec(out, "/bin/zsh -lc 'command -v adb >/dev/null 2>&1 || { echo \"adb introuvable\"; exit 0; }; "
                   "adb devices 2>/dev/null | /usr/bin/awk \"NR > 1 && NF { print \\$2 }\"' 2>&1 | /usr/bin/head -n 8", NULL, NULL);

    fprintf(out, "\n== Routes IPv4 du tethering (0/1 et 128.0/1) ==\n");
    diag_exec(out, "/usr/sbin/netstat -rn -f inet 2>&1 | /usr/bin/grep -E '^(0/1|128\\.0/1)[[:space:]]'", diag_peek_route4, &f);
    fprintf(out, "\n== Routes IPv6 (::/1 et 8000::/1) ==\n");
    diag_exec(out, "/usr/sbin/netstat -rn -f inet6 2>&1 | /usr/bin/grep -E '^(::/1|8000::/1)[[:space:]]'", diag_peek_route6, &f);

    fprintf(out, "\n== DNS (premier résolveur) ==\n");
    diag_exec(out, "/usr/sbin/scutil --dns 2>&1 | /usr/bin/sed -n '1,14p'", NULL, NULL);

    fprintf(out, "\n== Journal (40 dernières lignes) ==\n");
    diag_exec(out, "/usr/bin/tail -n 40 /var/log/rndis-tether.log 2>&1", NULL, NULL);
    fclose(out);

    diag_verdict(&f, verdict, sizeof verdict);
    printf("rndis-tether --diagnose : rapport sans mot de passe, sans adresse MAC complète, sans IPv4 publique\n"
           "ni IPv6 globale, sans numéro de série. Relis-le avant de le publier.\n\n== Diagnostic ==\n%s", verdict);
    fputs(body, stdout);
    free(body);
    return 0;
}

int main(int argc, char **argv) {
    unsigned vid = 0, pid = 0;
    int opt;
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("rndis-tether %s\n", RNDIS_VERSION); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--diagnose")) return diagnose_main();
    while ((opt = getopt(argc, argv, "rwavd:U:W:S:4OLVh")) != -1) {
        switch (opt) {
        case 'L': allow_v6_leak = 1; break;
        case 'O': allow_overlap = 1; break;
        case 'V': printf("rndis-tether %s\n", RNDIS_VERSION); return 0;
        case '4': ipv6_on = 0; break;
        case 'U': adb_user = optarg; break;
        case 'S': if (!adb_serial_valid(optarg)) { fprintf(stderr, "-S : numéro de série invalide\n"); return 2; } adb_serial = optarg; break;
        case 'W': worker_user = optarg; break;
        case 'a': auto_adb = 1; break;
        case 'r': want_route = 1; break;
        case 'w': wait_mode = 1; break;
        case 'v': verbose = 1; break;
        case 'd': if (sscanf(optarg, "%x:%x", &vid, &pid) != 2) { usage(argv[0]); return 2; } break;
        default: usage(argv[0]); return 2;
        }
    }
    if (geteuid() != 0) { LOG("Il faut les droits root : sudo %s", argv[0]); return 1; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    return supervisor_main(vid, pid);
}

static int session(void) {
    int rc = 1;
    running = 1; utun_fd = -1; my_ip = gw_ip = srv_ip = 0;
    dhcp_active = 0; memset(&dhcp_c, 0, sizeof dhcp_c);
    v6_claimed = v6_pending = v6_routes = v6_svc = v6_withdraw = 0;
    v6_expire = 0; v6_mtu = 0; cur_mtu = MTU;
    status_tick("connecting");
    if (libusb_claim_interface(h, ctrl_if) || libusb_claim_interface(h, data_if)) {
        LOG("Impossible de réserver les interfaces USB"); goto out;
    }
    if (rndis_init() < 0) goto out;
    LOG("MAC hôte: %02x:%02x:%02x:%02x:%02x:%02x", my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);

    if ((utun_fd = request_utun()) < 0) goto out;
    struct lease L;
    LOG("DHCP en cours sur %s…", utun_name);
    if (dhcp_acquire(&L) < 0) { LOG("Pas de réponse DHCP du téléphone"); goto out; }
    if (!L.srv) L.srv = L.gw;                                 /* serveur sans option 54 : on suppose le routeur */
    my_ip = L.ip; gw_ip = L.gw; srv_ip = L.srv; memcpy(gw_mac, L.srvmac, 6);
    pthread_mutex_lock(&dhcp_lock);
    dhcp_client_bound(&dhcp_c, &L, now_s());
    dhcp_active = 1;
    pthread_mutex_unlock(&dhcp_lock);
    char a[16], g[16], d[16];
    ipstr(L.ip, a); ipstr(L.gw, g); ipstr(L.dns, d);
    LOG("%s: IP %s, passerelle %s, DNS %s, bail %us", utun_name, a, g, L.dns ? d : "-", L.secs);
    if (request_config4(&L) < 0) { LOG("Le superviseur a refusé la configuration réseau"); goto out; }

    pthread_t t_rx, t_tx, t_ct;
    __atomic_store_n(&thread_alive, 0, __ATOMIC_RELAXED);
    pthread_create(&t_rx, NULL, rx_thread, NULL);
    pthread_create(&t_tx, NULL, tx_thread, NULL);
    pthread_create(&t_ct, NULL, ctrl_thread, NULL);
    __atomic_store_n(&threads_running, 1, __ATOMIC_RELEASE);
    LOG("Tethering actif. Ctrl-C pour arrêter.");
    {
        char st[96];
        snprintf(st, sizeof st, "connected %s %s", a, utun_name);
        status_tick(st);
        while (running) {
            sleep(1);
            if (is_disabled()) { LOG("Désactivé depuis l'icône de menu."); running = 0; }
            status_tick(st);
        }
    }
    LOG("Arrêt…");
    pthread_join(t_rx, NULL); pthread_join(t_tx, NULL); pthread_join(t_ct, NULL);
    __atomic_store_n(&threads_running, 0, __ATOMIC_RELEASE);
    release_lease();
    rc = 0;
out:
    if (utun_fd >= 0) close(utun_fd);
    if (h) {
        if (ctrl_if >= 0) { rndis_halt(); libusb_release_interface(h, ctrl_if); }
        if (data_if >= 0) libusb_release_interface(h, data_if);
        libusb_close(h);
        h = NULL;
    }
    return rc;
}
