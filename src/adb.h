// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Lecture de la sortie de `adb devices` et choix de l'appareil à basculer en Modem USB.
 *
 * Le superviseur ne faisait qu'envoyer « adb shell svc usb setFunctions rndis » sans rien dire quand cela
 * échouait. Ces fonctions pures transforment la sortie de `adb devices` en un état clair (adb absent, aucun
 * appareil, non autorisé, plusieurs appareils…) avec un message qui dit quoi faire. Aucune entrée/sortie : testé
 * sans téléphone ni adb.
 */
#ifndef ADB_H
#define ADB_H

#include <stddef.h>

#define ADB_MAX_DEVICES 8
#define ADB_SERIAL_MAX 63

enum adb_dev_state { ADB_DEV_READY = 0, ADB_DEV_UNAUTHORIZED, ADB_DEV_OFFLINE, ADB_DEV_OTHER };

struct adb_devices {
    int missing;                                    /* la commande adb n'existe pas */
    int n;                                          /* appareils lus (au plus ADB_MAX_DEVICES) */
    int dropped;                                    /* appareils au-delà de ADB_MAX_DEVICES, ou au numéro de série invalide */
    struct { char serial[ADB_SERIAL_MAX + 1]; enum adb_dev_state state; } dev[ADB_MAX_DEVICES];
};

enum adb_status {
    ADB_OK = 0,             /* un appareil prêt, à basculer */
    ADB_NOT_INSTALLED,
    ADB_NO_DEVICE,
    ADB_UNAUTHORIZED,       /* le téléphone n'a pas accepté l'ordinateur */
    ADB_OFFLINE,
    ADB_NOT_READY,          /* dans un autre état (récupération, sideload...) */
    ADB_MULTIPLE,           /* plusieurs appareils prêts et aucun numéro de série demandé */
    ADB_WANTED_ABSENT       /* le numéro de série demandé n'est pas dans la liste */
};

/* Numéro de série acceptable pour -s : 1 à ADB_SERIAL_MAX caractères parmi A-Z a-z 0-9 . _ : - */
int adb_serial_valid(const char *s);

/* Analyse la sortie de la commande de sondage (adb devices, précédée de « ADB_MISSING » si adb est introuvable). */
void adb_parse_devices(const char *out, struct adb_devices *d);

/* État global et, si ADB_OK, le numéro de série à utiliser (*serial, ou "" s'il est inutile).
 * `wanted` est le numéro demandé par l'utilisateur (NULL ou vide : aucun). *serial pointe dans `d` : il vit autant que lui. */
enum adb_status adb_choose(const struct adb_devices *d, const char *wanted, const char **serial);

/* Message en français qui dit quoi faire ; NULL pour ADB_OK. */
const char *adb_status_message(enum adb_status s);

#endif
