// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Politique de validation du superviseur (processus root).
 *
 * Le worker, sans privilèges, lit des données hostiles (USB, DHCP, réseau). S'il était compromis, il
 * pourrait demander n'importe quelle configuration au superviseur. Ces fonctions décident de ce que le
 * superviseur accepte d'appliquer : uniquement des valeurs plausibles, jamais du texte libre.
 * Aucun effet de bord : testé sans root ni USB.
 */
#ifndef POLICY_H
#define POLICY_H

#include <stddef.h>
#include <stdint.h>

enum policy_err {
    POLICY_OK = 0,
    POLICY_BAD_IP,
    POLICY_BAD_MASK,
    POLICY_BAD_GATEWAY,
    POLICY_BAD_DNS,
    POLICY_BAD_V6_ADDR,
    POLICY_BAD_V6_ROUTER,
    POLICY_BAD_STATUS,
    POLICY_OVERLAP,
    POLICY_BAD_MTU,
    POLICY_BAD_V6_DNS
};

#define POLICY_STATUS_MAX 90

/* IPv4 en ordre réseau. dns peut valoir 0 (aucun). */
int policy_check_v4(uint32_t ip, uint32_t gw, uint32_t mask, uint32_t dns);

/* Adresse IPv6 unicast globale (2000::/3) ou ULA (fc00::/7), routeur link-local (fe80::/10). */
int policy_check_v6(const uint8_t addr[16], const uint8_t router[16]);

/* 1 si les deux réseaux IPv4 (adresse + masque, ordre réseau) se recouvrent, 0 sinon. */
int policy_subnets_overlap(uint32_t ip_a, uint32_t mask_a, uint32_t ip_b, uint32_t mask_b);

/* Garde contre le masquage d'un réseau local par un téléphone malveillant : refuse (POLICY_OVERLAP) un
 * sous-réseau qui recouvre un réseau déjà connecté au Mac. Ne considère que les réseaux « ordinaires »,
 * de préfixe /16 à /30 : une adresse hôte (/32, VPN) ou un très large réseau (/8, entreprise) ne bloquent pas. */
int policy_check_overlap(uint32_t ip, uint32_t mask, uint32_t other_ip, uint32_t other_mask);

/* MTU annoncé par le téléphone (DHCP option 26 ou RA IPv6) : de 576 (IPv4) ou 1280 (IPv6) à 1500. */
#define POLICY_MTU_MAX 1500
#define POLICY_MTU_MIN4 576
#define POLICY_MTU_MIN6 1280
int policy_check_mtu(unsigned mtu, int ipv6);

/* Serveur DNS IPv6 annoncé par le téléphone (RDNSS) : adresse globale (2000::/3) ou ULA (fc00::/7) seulement, ni
 * link-local, ni loopback, ni multicast, ni non spécifiée, et pas notre propre adresse. */
int policy_check_dns6(const uint8_t dns[16], const uint8_t our_addr[16]);

/* Valeur reçue du téléphone -> MTU à utiliser : 0 si absente ou inférieure à `min` (on garde alors la valeur par
 * défaut), plafonnée à POLICY_MTU_MAX (les tampons du driver ne portent pas plus). */
unsigned policy_mtu_from_peer(unsigned raw, unsigned min);

/* Texte d'état affiché par l'icône : "waiting", "connecting" ou "connected <ipv4> utun<N>". */
int policy_check_status(const char *text, size_t len);

/* Limitation de débit des demandes du worker : au plus `max` demandes par fenêtre de `window` secondes. */
struct rate_limit {
    int64_t window_start;
    int count;
};

/* 1 si la demande est acceptée, 0 si la limite est dépassée. `now` est une horloge monotone en secondes. */
int rate_allow(struct rate_limit *r, int64_t now, int max, int window);

const char *policy_strerror(int err);

#endif
