// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy.h"

#include <arpa/inet.h>
#include <string.h>

/* unicast routable : ni 0.0.0.0, ni broadcast, ni boucle locale, ni multicast/classe E, ni lien-local */
static int usable_unicast(uint32_t host) {
    if (host == 0 || host == 0xffffffffu) return 0;
    if ((host >> 24) == 127) return 0;
    if ((host >> 24) >= 224) return 0;
    if ((host >> 16) == 0xA9FE) return 0;           /* 169.254.0.0/16 */
    return 1;
}

int policy_check_v4(uint32_t ip, uint32_t gw, uint32_t mask, uint32_t dns) {
    uint32_t i = ntohl(ip), g = ntohl(gw), m = ntohl(mask), d = ntohl(dns);

    if (m == 0 || (~m & (~m + 1)) != 0) return POLICY_BAD_MASK;     /* le masque doit être contigu */
    int prefix = __builtin_popcount(m);
    if (prefix < 8 || prefix > 30) return POLICY_BAD_MASK;

    if (!usable_unicast(i)) return POLICY_BAD_IP;
    if (!usable_unicast(g)) return POLICY_BAD_GATEWAY;
    if (g == i || (g & m) != (i & m)) return POLICY_BAD_GATEWAY;    /* passerelle dans notre sous-réseau */

    uint32_t net = i & m, bcast = net | ~m;
    if (i == net || i == bcast) return POLICY_BAD_IP;
    if (g == net || g == bcast) return POLICY_BAD_GATEWAY;

    if (dns != 0 && !usable_unicast(d)) return POLICY_BAD_DNS;
    return POLICY_OK;
}

int policy_subnets_overlap(uint32_t ip_a, uint32_t mask_a, uint32_t ip_b, uint32_t mask_b) {
    uint32_t a = ntohl(ip_a), ma = ntohl(mask_a), b = ntohl(ip_b), mb = ntohl(mask_b);
    uint32_t wide = ma < mb ? ma : mb;              /* masque contigu : le plus petit est le réseau le plus large */
    return (a & wide) == (b & wide);
}

int policy_check_overlap(uint32_t ip, uint32_t mask, uint32_t other_ip, uint32_t other_mask) {
    uint32_t om = ntohl(other_mask);
    if (om == 0 || (~om & (~om + 1)) != 0) return POLICY_OK;      /* masque inexploitable : on ne juge pas */
    int prefix = __builtin_popcount(om);
    if (prefix < 16 || prefix > 30) return POLICY_OK;
    return policy_subnets_overlap(ip, mask, other_ip, other_mask) ? POLICY_OVERLAP : POLICY_OK;
}

int rate_allow(struct rate_limit *r, int64_t now, int max, int window) {
    if (r->count == 0 || now < r->window_start || now - r->window_start >= window) {
        r->window_start = now;                      /* nouvelle fenêtre (ou horloge qui a reculé) */
        r->count = 0;
    }
    if (r->count >= max) return 0;
    r->count++;
    return 1;
}

int policy_check_mtu(unsigned mtu, int ipv6) {
    unsigned min = ipv6 ? POLICY_MTU_MIN6 : POLICY_MTU_MIN4;
    return mtu >= min && mtu <= POLICY_MTU_MAX ? POLICY_OK : POLICY_BAD_MTU;
}

unsigned policy_mtu_from_peer(unsigned raw, unsigned min) {
    if (raw < min) return 0;
    return raw > POLICY_MTU_MAX ? POLICY_MTU_MAX : raw;
}

int policy_check_v6(const uint8_t addr[16], const uint8_t router[16]) {
    int global = (addr[0] & 0xe0) == 0x20;          /* 2000::/3 */
    int ula = (addr[0] & 0xfe) == 0xfc;             /* fc00::/7 */
    if (!global && !ula) return POLICY_BAD_V6_ADDR;
    if (router[0] != 0xfe || (router[1] & 0xc0) != 0x80) return POLICY_BAD_V6_ROUTER;
    return POLICY_OK;
}

static int is_status_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '.' || c == ':' || c == '_' || c == '-';
}

int policy_check_status(const char *text, size_t len) {
    if (len == 0 || len > POLICY_STATUS_MAX) return POLICY_BAD_STATUS;
    for (size_t k = 0; k < len; k++)
        if (!is_status_char((unsigned char)text[k])) return POLICY_BAD_STATUS;   /* inclut tout NUL ou saut de ligne */

    if ((len == 7 && !memcmp(text, "waiting", 7)) || (len == 10 && !memcmp(text, "connecting", 10)))
        return POLICY_OK;

    if (len > 10 && !memcmp(text, "connected ", 10)) {
        const char *ip = text + 10, *sp = memchr(ip, ' ', len - 10);
        if (!sp || sp == ip || sp - ip > 15) return POLICY_BAD_STATUS;
        char ipbuf[16];
        memcpy(ipbuf, ip, (size_t)(sp - ip));
        ipbuf[sp - ip] = '\0';
        uint8_t raw[4];
        if (inet_pton(AF_INET, ipbuf, raw) != 1) return POLICY_BAD_STATUS;
        const char *ifc = sp + 1;
        size_t ilen = len - (size_t)(ifc - text);
        if (ilen < 5 || ilen > 8 || memcmp(ifc, "utun", 4)) return POLICY_BAD_STATUS;
        for (size_t k = 4; k < ilen; k++)
            if (ifc[k] < '0' || ifc[k] > '9') return POLICY_BAD_STATUS;
        return POLICY_OK;
    }
    return POLICY_BAD_STATUS;
}

const char *policy_strerror(int err) {
    switch (err) {
    case POLICY_OK: return "ok";
    case POLICY_BAD_IP: return "adresse IPv4 refusée";
    case POLICY_BAD_MASK: return "masque refusé";
    case POLICY_BAD_GATEWAY: return "passerelle refusée";
    case POLICY_BAD_DNS: return "serveur DNS refusé";
    case POLICY_BAD_V6_ADDR: return "adresse IPv6 refusée";
    case POLICY_BAD_V6_ROUTER: return "routeur IPv6 refusé";
    case POLICY_BAD_STATUS: return "état refusé";
    case POLICY_OVERLAP: return "sous-réseau déjà utilisé par ce Mac";
    case POLICY_BAD_MTU: return "MTU refusé";
    default: return "erreur inconnue";
    }
}
