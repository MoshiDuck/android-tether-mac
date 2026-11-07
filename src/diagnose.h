// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Logique pure de `rndis-tether --diagnose` : caviardage des données personnelles, classement des
 * interfaces USB, lecture de l'état du service et verdict en langage clair.
 *
 * Le rapport est fait pour être collé dans un ticket GitHub : il ne doit contenir ni adresse MAC complète,
 * ni adresse IPv4 publique, ni adresse IPv6 globale. Aucune entrée/sortie ici : testé sans root ni USB.
 */
#ifndef DIAGNOSE_H
#define DIAGNOSE_H

#include <stddef.h>
#include <stdint.h>

/* Caviarde `in` dans `out` (toujours terminé par un zéro, tronqué à `outsz`) et retourne la longueur écrite.
 *  - MAC aa:bb:cc:dd:ee:ff    -> aa:bb:cc:xx:xx:xx   (le fabricant reste lisible)
 *  - IPv4 publique a.b.c.d    -> a.b.x.x             (les adresses privées, locales et de bouclage restent)
 *  - IPv6 globale 2600:383:…  -> 2600:383:xxxx       (les adresses fe80::, fc00::/7 et ::1 restent) */
size_t diag_redact(const char *in, char *out, size_t outsz);

enum diag_iface { DIAG_IF_OTHER = 0, DIAG_IF_RNDIS, DIAG_IF_NATIVE_NET, DIAG_IF_ADB, DIAG_IF_MTP };

/* Classe une interface USB d'après sa classe/sous-classe/protocole. */
enum diag_iface diag_iface_kind(uint8_t cls, uint8_t sub, uint8_t proto);
const char *diag_iface_name(enum diag_iface k);

enum diag_state { DIAG_ST_UNKNOWN = 0, DIAG_ST_STARTING, DIAG_ST_WAITING, DIAG_ST_CONNECTING,
                  DIAG_ST_CONNECTED, DIAG_ST_STOPPED, DIAG_ST_DISABLED };

/* Interprète le fichier d'état du service ("connected 10.0.0.2 utun4", "waiting", …). NULL ou vide : inconnu. */
enum diag_state diag_state_parse(const char *status);

struct diag_facts {
    int status_known;            /* le fichier d'état existe */
    enum diag_state state;
    int supervisor_running;      /* un processus rndis-tether tourne en root */
    int worker_running;          /* et son worker sans privilèges */
    int disabled_flag;           /* l'icône de menu a désactivé le service */
    int usb_rndis;               /* un téléphone en mode Modem USB (RNDIS) est branché */
    int usb_native;              /* un téléphone en CDC-ECM/NCM : macOS le gère sans ce driver */
    int usb_adb;                 /* une interface ADB est visible : débogage USB actif */
    int usb_mtp;                 /* un téléphone est branché mais en transfert de fichiers seulement */
    int tether_route;            /* les routes du tethering sont installées */
    int v6_reject;               /* les routes de blocage IPv6 sont installées */
};

/* Écrit dans `out` un verdict de plusieurs lignes ("- …\n"), ou "- Rien d'anormal détecté.\n". */
size_t diag_verdict(const struct diag_facts *f, char *out, size_t outsz);

#endif
