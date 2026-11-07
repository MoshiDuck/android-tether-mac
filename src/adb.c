// SPDX-License-Identifier: GPL-3.0-or-later
#include "adb.h"

#include <string.h>

int adb_serial_valid(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > ADB_SERIAL_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static enum adb_dev_state state_of(const char *s) {
    if (starts(s, "device") && (s[6] == '\0' || s[6] == ' ' || s[6] == '\t')) return ADB_DEV_READY;
    if (starts(s, "unauthorized")) return ADB_DEV_UNAUTHORIZED;
    if (starts(s, "offline")) return ADB_DEV_OFFLINE;
    return ADB_DEV_OTHER;                                          /* recovery, sideload, no permissions, authorizing... */
}

void adb_parse_devices(const char *out, struct adb_devices *d) {
    memset(d, 0, sizeof *d);
    if (!out) return;
    while (*out) {
        const char *eol = out;
        while (*eol && *eol != '\n') eol++;
        char line[160];
        size_t n = (size_t)(eol - out);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, out, n);
        line[n] = '\0';
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        out = *eol ? eol + 1 : eol;

        if (!strcmp(line, "ADB_MISSING")) { d->missing = 1; continue; }
        if (starts(line, "List of devices") || starts(line, "* ") || line[0] == '\0') continue;   /* en-tête, démon, vide */
        char *tab = strpbrk(line, "\t ");
        if (!tab) continue;                                        /* pas de la forme « série  état » */
        *tab = '\0';
        char *st = tab + 1;
        while (*st == '\t' || *st == ' ') st++;
        if (!adb_serial_valid(line) || *st == '\0') { d->dropped++; continue; }
        if (d->n >= ADB_MAX_DEVICES) { d->dropped++; continue; }
        memcpy(d->dev[d->n].serial, line, strlen(line) + 1);
        d->dev[d->n].state = state_of(st);
        d->n++;
    }
}

enum adb_status adb_choose(const struct adb_devices *d, const char *wanted, const char **serial) {
    static const char none[] = "";
    int ready = 0, unauth = 0, offline = 0, other = 0, first_ready = -1;
    *serial = none;
    if (d->missing) return ADB_NOT_INSTALLED;
    if (wanted && *wanted) {                                       /* l'utilisateur a désigné son téléphone */
        for (int k = 0; k < d->n; k++) {
            if (strcmp(d->dev[k].serial, wanted)) continue;
            switch (d->dev[k].state) {
            case ADB_DEV_READY: *serial = d->dev[k].serial; return ADB_OK;
            case ADB_DEV_UNAUTHORIZED: return ADB_UNAUTHORIZED;
            case ADB_DEV_OFFLINE: return ADB_OFFLINE;
            default: return ADB_NOT_READY;
            }
        }
        return ADB_WANTED_ABSENT;
    }
    for (int k = 0; k < d->n; k++) {
        switch (d->dev[k].state) {
        case ADB_DEV_READY: if (first_ready < 0) first_ready = k; ready++; break;
        case ADB_DEV_UNAUTHORIZED: unauth++; break;
        case ADB_DEV_OFFLINE: offline++; break;
        default: other++;
        }
    }
    if (ready > 1) return ADB_MULTIPLE;
    if (ready == 1) { *serial = d->dev[first_ready].serial; return ADB_OK; }
    if (unauth) return ADB_UNAUTHORIZED;
    if (offline) return ADB_OFFLINE;
    if (other) return ADB_NOT_READY;
    return ADB_NO_DEVICE;
}

const char *adb_status_message(enum adb_status s) {
    switch (s) {
    case ADB_OK: return NULL;
    case ADB_NOT_INSTALLED:
        return "adb est introuvable : installe-le (brew install --cask android-platform-tools), ou active le partage de connexion USB à la main";
    case ADB_NO_DEVICE:
        return "adb ne voit aucun téléphone : active le débogage USB (Options pour les développeurs), vérifie le câble et déverrouille l'écran";
    case ADB_UNAUTHORIZED:
        return "le téléphone n'a pas autorisé ce Mac : déverrouille-le et accepte « Autoriser le débogage USB » (coche « Toujours autoriser »)";
    case ADB_OFFLINE:
        return "adb voit le téléphone hors ligne : débranche et rebranche le câble, ou désactive puis réactive le débogage USB";
    case ADB_NOT_READY:
        return "adb voit le téléphone dans un état inattendu (récupération, transfert…) : redémarre-le normalement";
    case ADB_MULTIPLE:
        return "plusieurs appareils adb sont prêts : désigne le bon avec -S numéro_de_série (adb devices les liste)";
    case ADB_WANTED_ABSENT:
        return "l'appareil adb demandé avec -S n'est pas branché ou pas visible : vérifie son numéro de série avec adb devices";
    }
    return NULL;
}
