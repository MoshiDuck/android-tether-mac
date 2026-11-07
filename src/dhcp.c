// SPDX-License-Identifier: GPL-3.0-or-later
#include "dhcp.h"

#include <string.h>

#include "bytes.h"

const uint8_t dhcp_cookie[4] = {0x63, 0x82, 0x53, 0x63};

#define MIN_LEASE 60            /* garde-fou : évite une boucle de renouvellement si le serveur annonce 0-59 s */
#define MIN_RETRY 60            /* RFC 2131 §4.4.5 : attente minimale entre deux retransmissions */

uint16_t ip_csum(const uint8_t *p, int len) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < len; i += 2) s += p[i] << 8 | p[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
}

/* Ethernet + IPv4 + UDP 68->67 autour d'une charge BOOTP de blen octets déjà écrite en f+42. */
static int finish_frame(uint8_t *f, int blen, uint32_t src_ip, uint32_t dst_ip) {
    uint8_t *ip = f + 14, *udp = ip + 20;
    int udplen = 8 + blen, iplen = 20 + udplen;
    ip[0] = 0x45; putbe16(ip + 2, iplen); ip[8] = 64; ip[9] = 17;
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    putbe16(ip + 10, ip_csum(ip, 20));
    putbe16(udp, 68); putbe16(udp + 2, 67); putbe16(udp + 4, udplen);
    return 14 + iplen;
}

/* Liste des paramètres demandés : masque, routeur, DNS, MTU, durée du bail, T1, T2. */
static uint8_t *put_param_request(uint8_t *o) {
    *o++ = 55; *o++ = 7; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 26; *o++ = 51; *o++ = 58; *o++ = 59;
    return o;
}

int dhcp_build(uint8_t *f, const uint8_t mac[6], uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t srv_ip) {
    memset(f, 0, 600);
    memset(f, 0xff, 6); memcpy(f + 6, mac, 6); putbe16(f + 12, 0x0800);
    uint8_t *b = f + 14 + 20 + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;
    putbe32(b + 4, xid);
    putbe16(b + 10, 0x8000);                      /* réponse en broadcast : on n'a pas encore d'adresse */
    memcpy(b + 28, mac, 6);
    memcpy(b + 236, dhcp_cookie, 4);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4; }
    if (srv_ip) { *o++ = 54; *o++ = 4; memcpy(o, &srv_ip, 4); o += 4; }
    o = put_param_request(o);
    *o++ = 255;
    int blen = o - b;
    if (blen < 300) blen = 300;
    return finish_frame(f, blen, 0, 0xffffffffu);
}

int dhcp_build_renew(uint8_t *f, const uint8_t mac[6], uint32_t xid, uint32_t ip,
                     const uint8_t srv_mac[6], uint32_t srv_ip, int unicast) {
    memset(f, 0, 600);
    if (unicast) memcpy(f, srv_mac, 6); else memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6); putbe16(f + 12, 0x0800);
    uint8_t *b = f + 14 + 20 + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;
    putbe32(b + 4, xid);
    memcpy(b + 12, &ip, 4);                       /* ciaddr : notre adresse actuelle */
    memcpy(b + 28, mac, 6);
    memcpy(b + 236, dhcp_cookie, 4);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = 3;                /* DHCPREQUEST, sans option 50 ni 54 */
    o = put_param_request(o);
    *o++ = 255;
    int blen = o - b;
    if (blen < 300) blen = 300;
    return finish_frame(f, blen, ip, unicast ? srv_ip : 0xffffffffu);
}

int dhcp_build_release(uint8_t *f, const uint8_t mac[6], uint32_t xid, uint32_t ip,
                       const uint8_t srv_mac[6], uint32_t srv_ip) {
    memset(f, 0, 600);
    memcpy(f, srv_mac, 6); memcpy(f + 6, mac, 6); putbe16(f + 12, 0x0800);
    uint8_t *b = f + 14 + 20 + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;
    putbe32(b + 4, xid);
    memcpy(b + 12, &ip, 4);                       /* ciaddr : l'adresse que l'on rend */
    memcpy(b + 28, mac, 6);
    memcpy(b + 236, dhcp_cookie, 4);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = 7;                /* DHCPRELEASE */
    *o++ = 54; *o++ = 4; memcpy(o, &srv_ip, 4); o += 4;
    *o++ = 255;
    int blen = o - b;
    if (blen < 300) blen = 300;
    return finish_frame(f, blen, ip, srv_ip);
}

int dhcp_parse(const uint8_t *f, int len, const uint8_t mac[6], uint32_t xid, struct lease *L) {
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00) return -1;
    const uint8_t *ip = f + 14;
    int ihl = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || len < 14 + ihl + 8 + 240 || ip[9] != 17) return -1;
    const uint8_t *udp = ip + ihl, *b = udp + 8;
    if ((udp[2] << 8 | udp[3]) != 68 || b[0] != 2 || be32(b + 4) != xid) return -1;
    if (memcmp(b + 28, mac, 6) || memcmp(b + 236, dhcp_cookie, 4)) return -1;
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
        else if (code == 58 && l >= 4) L->t1 = be32(o + 2);
        else if (code == 59 && l >= 4) L->t2 = be32(o + 2);
        else if (code == 26 && l >= 2) L->mtu = (uint16_t)(o[2] << 8 | o[3]);
        else if (code == 54 && l >= 4) memcpy(&L->srv, o + 2, 4);
        o += 2 + l;
    }
    return 0;
}

/* ---------- cycle de vie du bail (RFC 2131 §4.4.5) ---------- */

void dhcp_client_bound(struct dhcp_client *c, const struct lease *L, int64_t now) {
    uint32_t lease = L->secs;
    if (lease != DHCP_INFINITE && lease < MIN_LEASE) lease = MIN_LEASE;
    c->ip = L->ip ? L->ip : c->ip;
    c->srv = L->srv ? L->srv : c->srv;
    if (L->srv) memcpy(c->srvmac, L->srvmac, 6);
    c->lease = lease;
    if (lease == DHCP_INFINITE) {
        c->t1 = c->t2 = DHCP_INFINITE;
    } else {
        /* T1/T2 du serveur si cohérents (0 < T1 < T2 < bail), sinon valeurs par défaut 50 % et 87,5 % */
        uint32_t t1 = L->t1, t2 = L->t2;
        if (!(t1 > 0 && t2 > t1 && t2 < lease)) {
            t1 = lease / 2;
            t2 = (uint32_t)((uint64_t)lease * 7 / 8);
        }
        c->t1 = t1;
        c->t2 = t2;
    }
    c->bound_at = now;
    c->next_tx = 0;
    c->xid = 0;
    c->state = DHCP_BOUND;
}

/* Prochaine retransmission : la moitié du temps restant jusqu'à l'échéance, au moins MIN_RETRY. */
static int64_t next_retry(int64_t now, int64_t deadline) {
    int64_t half = (deadline - now) / 2;
    return now + (half < MIN_RETRY ? MIN_RETRY : half);
}

enum dhcp_action dhcp_client_tick(struct dhcp_client *c, int64_t now, uint32_t new_xid) {
    if (c->state == DHCP_EXPIRED) return DHCP_RESTART;   /* NAK ou adresse changée : plus rien à renouveler */
    if (c->lease == DHCP_INFINITE) return DHCP_NOTHING;
    int64_t elapsed = now - c->bound_at;

    if (elapsed >= (int64_t)c->lease) {
        c->state = DHCP_EXPIRED;
        return DHCP_RESTART;
    }
    if (elapsed >= (int64_t)c->t2) {
        if (c->state != DHCP_REBINDING) {         /* T2 atteint : on passe en broadcast */
            c->state = DHCP_REBINDING;
            c->xid = new_xid;
            c->next_tx = next_retry(now, c->bound_at + c->lease);
            return DHCP_SEND_REBIND;
        }
        if (now >= c->next_tx) {
            c->next_tx = next_retry(now, c->bound_at + c->lease);
            return DHCP_SEND_REBIND;
        }
        return DHCP_NOTHING;
    }
    if (elapsed >= (int64_t)c->t1) {
        if (c->state == DHCP_BOUND) {             /* T1 atteint : on demande au serveur, en unicast */
            c->state = DHCP_RENEWING;
            c->xid = new_xid;
            c->next_tx = next_retry(now, c->bound_at + c->t2);
            return DHCP_SEND_RENEW;
        }
        if (c->state == DHCP_RENEWING && now >= c->next_tx) {
            c->next_tx = next_retry(now, c->bound_at + c->t2);
            return DHCP_SEND_RENEW;
        }
    }
    return DHCP_NOTHING;
}

enum dhcp_input dhcp_client_input(struct dhcp_client *c, const struct lease *r, int64_t now) {
    if (c->state != DHCP_RENEWING && c->state != DHCP_REBINDING) return DHCP_IGNORED;
    if (r->type == 6) {                           /* NAK : le serveur nous retire l'adresse */
        c->state = DHCP_EXPIRED;
        return DHCP_NAKED;
    }
    if (r->type != 5) return DHCP_IGNORED;
    if (r->ip && r->ip != c->ip) {                /* adresse différente : la session doit être refaite */
        c->state = DHCP_EXPIRED;
        return DHCP_ADDR_CHANGED;
    }
    struct lease nl = *r;
    if (nl.secs == 0) nl.secs = c->lease;         /* ACK sans option 51 : on garde la durée précédente */
    dhcp_client_bound(c, &nl, now);
    return DHCP_ACKED;
}
