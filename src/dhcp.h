// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Client DHCP (RFC 2131) : construction/analyse des trames et machine à états du bail.
 *
 * Ce module ne dépend ni de l'USB ni de l'horloge système : le temps est passé en paramètre,
 * ce qui permet de tester tout le cycle de vie du bail (T1, T2, relances, expiration) sans attendre.
 * Les adresses IPv4 sont en ordre réseau, les durées en secondes.
 */
#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

#define DHCP_INFINITE 0xffffffffu

struct lease {
    uint32_t ip, mask, gw, dns, srv;
    uint32_t secs;          /* option 51 : durée du bail */
    uint32_t t1, t2;        /* options 58/59 : 0 si absentes */
    uint16_t mtu;           /* option 26 : MTU de l'interface, 0 si absente */
    uint8_t type;           /* option 53 : type de message */
    uint8_t srvmac[6];      /* adresse Ethernet de l'émetteur de la réponse */
};

extern const uint8_t dhcp_cookie[4];

uint16_t ip_csum(const uint8_t *p, int len);

/* DISCOVER / REQUEST « sélection » : broadcast depuis 0.0.0.0. Retourne la taille de la trame. */
int dhcp_build(uint8_t *f, const uint8_t mac[6], uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t srv_ip);

/* REQUEST de renouvellement : ciaddr = notre adresse, sans option 50 ni 54 (RFC 2131 §4.3.2).
 * unicast=1 : RENEWING, envoyé directement au serveur ; unicast=0 : REBINDING, en broadcast. */
int dhcp_build_renew(uint8_t *f, const uint8_t mac[6], uint32_t xid, uint32_t ip,
                     const uint8_t srv_mac[6], uint32_t srv_ip, int unicast);

/* DHCPRELEASE (RFC 2131 §3.1.7) : rend le bail au serveur, en unicast, avec ciaddr et l'identifiant du serveur. */
int dhcp_build_release(uint8_t *f, const uint8_t mac[6], uint32_t xid, uint32_t ip,
                       const uint8_t srv_mac[6], uint32_t srv_ip);

/* Analyse une réponse serveur (OFFER/ACK/NAK) adressée à mac avec ce xid. 0 si valide, -1 sinon. */
int dhcp_parse(const uint8_t *f, int len, const uint8_t mac[6], uint32_t xid, struct lease *L);

/* ---------- cycle de vie du bail ---------- */
enum dhcp_state { DHCP_BOUND, DHCP_RENEWING, DHCP_REBINDING, DHCP_EXPIRED };
enum dhcp_action {
    DHCP_NOTHING,
    DHCP_SEND_RENEW,        /* envoyer la requête de renouvellement en unicast */
    DHCP_SEND_REBIND,       /* envoyer la requête en broadcast */
    DHCP_RESTART            /* bail perdu : relancer une session complète */
};
enum dhcp_input { DHCP_IGNORED = 0, DHCP_ACKED = 1, DHCP_NAKED = -1, DHCP_ADDR_CHANGED = -2 };

struct dhcp_client {
    enum dhcp_state state;
    uint32_t ip, srv;
    uint8_t srvmac[6];
    uint32_t lease, t1, t2;     /* secondes ; lease == DHCP_INFINITE : aucun renouvellement */
    int64_t bound_at;           /* instant du dernier ACK */
    int64_t next_tx;            /* prochaine retransmission */
    uint32_t xid;               /* transaction de renouvellement en cours (0 = aucune) */
};

/* Bail obtenu (ACK initial ou renouvelé) à l'instant now. */
void dhcp_client_bound(struct dhcp_client *c, const struct lease *L, int64_t now);

/* À appeler régulièrement. new_xid sert si une nouvelle transaction démarre. */
enum dhcp_action dhcp_client_tick(struct dhcp_client *c, int64_t now, uint32_t new_xid);

/* Réponse serveur (déjà validée par dhcp_parse avec c->xid). */
enum dhcp_input dhcp_client_input(struct dhcp_client *c, const struct lease *reply, int64_t now);

#endif
