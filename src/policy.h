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
    POLICY_BAD_STATUS
};

#define POLICY_STATUS_MAX 90

/* IPv4 en ordre réseau. dns peut valoir 0 (aucun). */
int policy_check_v4(uint32_t ip, uint32_t gw, uint32_t mask, uint32_t dns);

/* Adresse IPv6 unicast globale (2000::/3) ou ULA (fc00::/7), routeur link-local (fe80::/10). */
int policy_check_v6(const uint8_t addr[16], const uint8_t router[16]);

/* Texte d'état affiché par l'icône : "waiting", "connecting" ou "connected <ipv4> utun<N>". */
int policy_check_status(const char *text, size_t len);

const char *policy_strerror(int err);

#endif
