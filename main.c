// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * rndis-tether — USB tethering Android -> macOS, 100 % espace utilisateur.
 *
 * Principe :
 *   - libusb parle RNDIS directement au téléphone (aucun kext / DriverKit)
 *   - une interface utun (L3) expose le réseau au système
 *   - ARP et DHCP (client) sont gérés ici car RNDIS transporte des trames Ethernet
 *
 * Lancer avec sudo (création utun, ifconfig, route).
 */
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <libusb.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <time.h>
#include <unistd.h>

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

#define CDC_SEND_ENCAPSULATED   0x00
#define CDC_GET_ENCAPSULATED    0x01

static libusb_context *ctx;
static libusb_device_handle *h;
static int ctrl_if = -1, data_if = -1;
static uint8_t ep_in, ep_out, ep_int;
static int mps_out = 512;

static volatile sig_atomic_t running = 1, quit;
static int verbose, want_route, wait_mode, dns_applied, auto_adb;
static uint32_t srv_ip, renew_period; /* DHCP: serveur et période de renouvellement (s) */
static int utun_fd = -1;
static char utun_name[IFNAMSIZ];
static uint8_t my_mac[6], gw_mac[6];
static uint32_t my_ip, gw_ip; /* network order */
static uint32_t req_counter = 1;
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void putbe16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void putbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void on_signal(int s) { (void)s; running = 0; quit = 1; }

/* ---------- Contrôle par l'icône de barre de menus ----------
 * Le service tourne en root ; l'app (utilisateur) crée/supprime FLAG_DISABLED pour
 * l'éteindre/l'allumer, et lit STATUS_FILE (réécrit toutes les ~5 s) pour l'état. */
#define FLAG_DISABLED "/Users/Shared/rndis-tether.disabled"
#define STATUS_FILE   "/Users/Shared/rndis-tether.status"
static const char *adb_user;
static char status_text[160] = "starting";

static int is_disabled(void) { return access(FLAG_DISABLED, F_OK) == 0; }

static void status_write(void) {
    FILE *f = fopen(STATUS_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f, "%s\n", status_text);
    fclose(f);
    chmod(STATUS_FILE ".tmp", 0644);
    rename(STATUS_FILE ".tmp", STATUS_FILE);
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_text, sizeof status_text, fmt, ap);
    va_end(ap);
    status_write();
}

/* Change l'état s'il diffère, sinon rafraîchit le fichier au plus toutes les 5 s. */
static void status_tick(const char *text) {
    static time_t last;
    time_t now = time(NULL);
    if (text && strcmp(text, status_text)) { set_status("%s", text); last = now; }
    else if (now - last >= 5) { status_write(); last = now; }
}

/* ---------- RNDIS control channel ---------- */
static int ctrl_send(const uint8_t *msg, size_t len) {
    return libusb_control_transfer(h, 0x21, CDC_SEND_ENCAPSULATED, 0, ctrl_if, (uint8_t *)msg, len, 1000);
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
    uint32_t blen = get32(resp + 16), boff = get32(resp + 20);
    if (blen < 6 || 8 + boff + 6 > (uint32_t)r) { LOG("RNDIS: réponse MAC invalide"); return -1; }
    memcpy(my_mac, resp + 8 + boff, 6);

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
        if (mlen < RNDIS_HDR || off + (int)mlen > n) break;
        if (type == RNDIS_PACKET_MSG) {
            uint32_t doff = get32(buf + off + 8), dlen = get32(buf + off + 12);
            uint32_t start = off + 8 + doff;
            if (start + dlen <= (uint32_t)off + mlen) cb(buf + start, dlen, arg);
        }
        off += mlen;
    }
}

/* ---------- DHCP client ---------- */
struct lease { uint32_t ip, mask, gw, dns, srv, secs; uint8_t type; uint8_t srvmac[6]; };
static const uint8_t cookie[4] = {0x63, 0x82, 0x53, 0x63};

static uint16_t ip_csum(const uint8_t *p, int len) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < len; i += 2) s += p[i] << 8 | p[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
}

static int build_dhcp(uint8_t *f, uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t srv_ip) {
    memset(f, 0, 600);
    memset(f, 0xff, 6); memcpy(f + 6, my_mac, 6); putbe16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;
    putbe32(b + 4, xid);
    putbe16(b + 10, 0x8000); /* réponse en broadcast */
    memcpy(b + 28, my_mac, 6);
    memcpy(b + 236, cookie, 4);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4; }
    if (srv_ip) { *o++ = 54; *o++ = 4; memcpy(o, &srv_ip, 4); o += 4; }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;
    int blen = o - b; if (blen < 300) blen = 300;
    int udplen = 8 + blen, iplen = 20 + udplen;
    ip[0] = 0x45; putbe16(ip + 2, iplen); ip[8] = 64; ip[9] = 17;
    memset(ip + 16, 0xff, 4);
    putbe16(ip + 10, ip_csum(ip, 20));
    putbe16(udp, 68); putbe16(udp + 2, 67); putbe16(udp + 4, udplen);
    return 14 + iplen;
}

static int dhcp_parse(const uint8_t *f, int len, uint32_t xid, struct lease *L) {
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00) return -1;
    const uint8_t *ip = f + 14;
    int ihl = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || len < 14 + ihl + 8 + 240 || ip[9] != 17) return -1;
    const uint8_t *udp = ip + ihl, *b = udp + 8;
    if ((udp[2] << 8 | udp[3]) != 68 || b[0] != 2 || be32(b + 4) != xid) return -1;
    if (memcmp(b + 28, my_mac, 6) || memcmp(b + 236, cookie, 4)) return -1;
    memset(L, 0, sizeof *L);
    memcpy(&L->ip, b + 16, 4);
    memcpy(L->srvmac, f + 6, 6);
    const uint8_t *o = b + 240, *end = f + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        uint8_t code = o[0], l = o[1];
        if (code == 53 && l >= 1) L->type = o[2];
        else if (code == 1 && l >= 4) memcpy(&L->mask, o + 2, 4);
        else if (code == 3 && l >= 4) memcpy(&L->gw, o + 2, 4);
        else if (code == 6 && l >= 4) memcpy(&L->dns, o + 2, 4);
        else if (code == 51 && l >= 4) L->secs = be32(o + 2);
        else if (code == 54 && l >= 4) memcpy(&L->srv, o + 2, 4);
        o += 2 + l;
    }
    return 0;
}

struct dhcp_wait { uint32_t xid; uint8_t want; struct lease L; int done, nak; };

static void dhcp_cb(const uint8_t *f, int len, void *arg) {
    struct dhcp_wait *w = arg;
    struct lease L;
    if (w->done || dhcp_parse(f, len, w->xid, &L) < 0) return;
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
        send_frame(f, build_dhcp(f, 1, w.xid, 0, 0));
        if (dhcp_wait_for(&w, 3000) < 0) continue;
        struct lease offer = w.L;
        w.done = 0; w.want = 5;
        VLOG("DHCP OFFER %s, REQUEST", inet_ntoa((struct in_addr){offer.ip}));
        send_frame(f, build_dhcp(f, 3, w.xid, offer.ip, offer.srv));
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

static void sh(const char *fmt, ...) {
    char cmd[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    VLOG("$ %s", cmd);
    if (system(cmd) != 0) VLOG("(commande en échec)");
}

static void ipstr(uint32_t ip, char *out) { inet_ntop(AF_INET, &ip, out, INET_ADDRSTRLEN); }

/* DNS via SystemConfiguration : résolveur « supplémentaire » valable pour tous les domaines */
static void scutil_run(const char *script) {
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
    sh("/sbin/ifconfig %s inet %s %s netmask %s mtu %d up", utun_name, ip, gw, mask, MTU);
    if (want_route) {
        sh("/sbin/route -q -n add -net 0.0.0.0/1 %s", gw);
        sh("/sbin/route -q -n add -net 128.0.0.0/1 %s", gw);
        if (L->dns) dns_apply(L);
    }
}

static void unconfigure_iface(void) {
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

static void rx_frame(const uint8_t *f, int len, void *arg) {
    (void)arg;
    if (len < 14) return;
    uint16_t et = f[12] << 8 | f[13];
    if (et == 0x0806) { handle_arp(f, len); return; }
    if (et != 0x0800 || len < 34) return; /* IPv6 non géré */
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
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = read(utun_fd, in, sizeof in);
        if (n <= 4) continue;
        uint32_t af = be32(in);
        int len = n - 4;
        if (af != AF_INET || len > MTU) continue; /* IPv6 non géré */
        memcpy(frame, gw_mac, 6); memcpy(frame + 6, my_mac, 6); putbe16(frame + 12, 0x0800);
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
    time_t next_renew = time(NULL) + renew_period;
    while (running) {
        if (renew_period && time(NULL) >= next_renew) {
            VLOG("DHCP renouvellement");
            send_frame(f, build_dhcp(f, 3, rand(), my_ip, srv_ip));
            next_renew += renew_period;
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

/* Bascule le téléphone en RNDIS via adb, exécuté en tant qu'utilisateur appelant sudo
 * (ses clés ADB et son PATH, pas ceux de root). */
static void adb_enable(void) {
    const char *u = adb_user ? adb_user : getenv("SUDO_USER");
    if (!u || !*u) return;
    for (const char *c = u; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') return;
    sh("/usr/bin/sudo -u %s /bin/zsh -lc 'adb shell svc usb setFunctions rndis' >/dev/null 2>&1 || true", u); /* adb échoue souvent car l'USB se ré-énumère */
}

static void usage(const char *p) {
    fprintf(stderr, "Usage: sudo %s [-r] [-v] [-d vid:pid]\n"
                    "  -r  route tout le trafic via le téléphone (routes 0/1 et 128/1, retirées à la sortie)\n"
                    "      (applique aussi le DNS du téléphone, retiré à la sortie)\n"
                    "  -w  attendre le téléphone et se reconnecter après un débranchement\n"
                    "  -a  activer le Modem USB du téléphone via adb (débogage USB requis)\n"
                    "  -U  utilisateur pour adb (défaut: SUDO_USER ; requis en LaunchDaemon)\n"
                    "  -v  logs détaillés\n"
                    "  -d  cibler un appareil précis (hex), ex: 18d1:4ee3\n", p);
}

int main(int argc, char **argv) {
    unsigned vid = 0, pid = 0;
    int opt;
    while ((opt = getopt(argc, argv, "rwavd:U:h")) != -1) {
        switch (opt) {
        case 'U': adb_user = optarg; break;
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

    if (libusb_init(&ctx)) { LOG("libusb_init a échoué"); return 1; }
    int rc = 1, announced = 0;
    time_t last_adb = 0;
    do {
        if (is_disabled()) { status_tick("disabled"); sleep(1); continue; }
        int ok = find_device(vid, pid) == 0;
        if (!ok && auto_adb && time(NULL) - last_adb >= 10) {
            last_adb = time(NULL);
            LOG("Activation du Modem USB via adb…");
            adb_enable();
            sleep(3);
            ok = find_device(vid, pid) == 0;
        }
        if (!ok) {
            if (!wait_mode) {
                LOG("Aucun appareil RNDIS. Branche le téléphone puis active Réglages > Point d'accès > Partage de connexion USB.");
                break;
            }
            if (!announced) { LOG("En attente d'un téléphone en mode Modem USB…"); announced = 1; }
            status_tick("waiting");
            sleep(2);
            continue;
        }
        announced = 0;
        rc = session();
        if (wait_mode && !quit) { LOG("Session terminée, nouvelle tentative…"); sleep(2); }
    } while (wait_mode && !quit);
    set_status("stopped");
    libusb_exit(ctx);
    return rc;
}

static int session(void) {
    int rc = 1;
    running = 1; utun_fd = -1; my_ip = gw_ip = srv_ip = renew_period = 0;
    status_tick("connecting");
    if (libusb_claim_interface(h, ctrl_if) || libusb_claim_interface(h, data_if)) {
        LOG("Impossible de réserver les interfaces USB"); goto out;
    }
    if (rndis_init() < 0) goto out;
    LOG("MAC hôte: %02x:%02x:%02x:%02x:%02x:%02x", my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);

    if ((utun_fd = utun_open()) < 0) goto out;
    struct lease L;
    LOG("DHCP en cours sur %s…", utun_name);
    if (dhcp_acquire(&L) < 0) { LOG("Pas de réponse DHCP du téléphone"); goto out; }
    my_ip = L.ip; gw_ip = L.gw; srv_ip = L.srv; memcpy(gw_mac, L.srvmac, 6);
    renew_period = L.secs >= 120 ? L.secs / 2 : 60;
    char a[16], g[16], d[16];
    ipstr(L.ip, a); ipstr(L.gw, g); ipstr(L.dns, d);
    LOG("%s: IP %s, passerelle %s, DNS %s, bail %us", utun_name, a, g, L.dns ? d : "-", L.secs);
    configure_iface(&L);

    pthread_t t_rx, t_tx, t_ct;
    pthread_create(&t_rx, NULL, rx_thread, NULL);
    pthread_create(&t_tx, NULL, tx_thread, NULL);
    pthread_create(&t_ct, NULL, ctrl_thread, NULL);
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
    unconfigure_iface();
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
