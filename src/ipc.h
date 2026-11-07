// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Canal entre le worker (sans privilèges) et le superviseur (root) : une socketpair AF_UNIX en mode
 * flux, avec des messages de taille FIXE (pas de longueur à interpréter côté root) et passage de
 * descripteur de fichier (SCM_RIGHTS) pour l'interface utun.
 */
#ifndef IPC_H
#define IPC_H

#include <stdint.h>

#define IPC_DATA_MAX 96

enum ipc_type {
    IPC_REQ_UTUN = 1,       /* worker -> superviseur : crée une interface utun ; réponse : nom + descripteur */
    IPC_REQ_CONFIG4,        /* ip, passerelle, masque, dns (4 x 4 octets, ordre réseau) */
    IPC_REQ_CONFIG6,        /* adresse (16) + routeur link-local (16) */
    IPC_REQ_STATUS,         /* texte d'état (voir policy_check_status) */
    IPC_REQ_ADB,            /* active le Modem USB via adb (exécuté par le superviseur) */
    IPC_REQ_DECONFIG6,      /* le téléphone n'a plus d'IPv6 : retire adresse et routes, remet le blocage (sans données) */
    IPC_REPLY = 100         /* superviseur -> worker ; status 0 = accepté */
};

struct ipc_msg {
    uint8_t type;
    uint8_t status;
    uint16_t len;           /* octets utiles dans data, <= IPC_DATA_MAX */
    uint8_t data[IPC_DATA_MAX];
};

/* Envoie un message ; fd_to_pass >= 0 joint ce descripteur. 0 si ok, -1 sinon. */
int ipc_send(int sock, const struct ipc_msg *m, int fd_to_pass);

/* Reçoit un message complet. Retourne 1 (message reçu ; *fd_out = descripteur reçu ou -1),
 * 0 (délai écoulé, timeout_ms < 0 = attente illimitée), -1 (pair fermé, message invalide ou erreur). */
int ipc_recv(int sock, struct ipc_msg *m, int *fd_out, int timeout_ms);

#endif
