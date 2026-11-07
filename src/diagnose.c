// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnose.h"

#include <stdio.h>
#include <string.h>

/* ---------- Caviardage ---------- */

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_run_char(char c) { return is_hex(c) || c == ':' || c == '.'; }

/* Écrit s dans out à la position *o sans dépasser outsz - 1. */
static void put(char *out, size_t outsz, size_t *o, const char *s, size_t n) {
    for (size_t i = 0; i < n && *o + 1 < outsz; i++) out[(*o)++] = s[i];
}

/* a.b.c.d avec quatre nombres décimaux de 0 à 255 : remplit v[4] et retourne 1. */
static int parse_ipv4(const char *s, size_t n, int v[4]) {
    size_t i = 0;
    for (int k = 0; k < 4; k++) {
        if (i >= n || s[i] < '0' || s[i] > '9') return 0;
        int x = 0, digits = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            x = x * 10 + (s[i] - '0');
            if (++digits > 3 || x > 255) return 0;
            i++;
        }
        v[k] = x;
        if (k < 3) { if (i >= n || s[i] != '.') return 0; i++; }
    }
    return i == n;
}

static int ipv4_is_public(const int v[4]) {
    if (v[0] == 0 || v[0] == 10 || v[0] == 127 || v[0] >= 224) return 0;
    if (v[0] == 172 && v[1] >= 16 && v[1] <= 31) return 0;
    if (v[0] == 192 && v[1] == 168) return 0;
    if (v[0] == 169 && v[1] == 254) return 0;
    if (v[0] == 100 && v[1] >= 64 && v[1] <= 127) return 0;     /* CGNAT : pas identifiant */
    if (v[2] == 0 && v[3] == 0) return 0;                       /* a.b.0.0 : un bloc réseau (128.0.0.0), pas un hôte */
    return 1;
}

/* aa:bb:cc:dd:ee:ff : six groupes de deux chiffres hexadécimaux. */
static int is_mac(const char *s, size_t n) {
    if (n != 17) return 0;
    for (size_t i = 0; i < n; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return 0; }
        else if (!is_hex(s[i])) return 0;
    }
    return 1;
}

/* IPv6 textuelle : groupes hexadécimaux de 1 à 4 chiffres séparés par ':', avec au plus un "::". */
static int is_ipv6(const char *s, size_t n) {
    int groups = 0, colons = 0, dbl = 0;
    size_t i = 0;
    if (n < 2) return 0;
    while (i < n) {
        if (s[i] == ':') {
            colons++;
            if (i + 1 < n && s[i + 1] == ':') { if (++dbl > 1) return 0; i++; colons++; }
            i++;
            continue;
        }
        size_t st = i;
        while (i < n && is_hex(s[i])) i++;
        if (i == st || i - st > 4) return 0;
        if (i < n && s[i] != ':') return 0;
        groups++;
    }
    return colons >= 2 && groups <= 8 && (dbl || groups == 8);
}

static int starts_with_ci(const char *s, size_t n, const char *p) {
    size_t l = strlen(p);
    if (n < l) return 0;
    for (size_t i = 0; i < l; i++) {
        char a = s[i], b = p[i];
        if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* fe80::/10, fc00::/7 et ::1 / :: ne permettent pas d'identifier quelqu'un. */
static int ipv6_is_local(const char *s, size_t n) {
    if (n <= 3 && (!strncmp(s, "::", n) || !strncmp(s, "::1", n))) return 1;
    if (n >= 2 && s[n - 1] == ':' && s[n - 2] == ':') return 1;      /* un préfixe réseau (8000::, 2600:383::) n'identifie personne */
    if (starts_with_ci(s, n, "fe8") || starts_with_ci(s, n, "fe9") || starts_with_ci(s, n, "fea") ||
        starts_with_ci(s, n, "feb")) return 1;
    if (starts_with_ci(s, n, "fc") || starts_with_ci(s, n, "fd")) return 1;
    return 0;
}

size_t diag_redact(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (outsz == 0) return 0;
    for (size_t i = 0; in[i];) {
        if (!is_run_char(in[i])) { put(out, outsz, &o, in + i, 1); i++; continue; }
        size_t j = i;
        while (in[j] && is_run_char(in[j])) j++;
        /* "clé:2600::1" ou "mot.1.2.3.4" : le séparateur de tête n'appartient pas à l'adresse */
        while (i < j && (in[i] == '.' || (in[i] == ':' && !(i + 1 < j && in[i + 1] == ':')))) { put(out, outsz, &o, in + i, 1); i++; }
        if (i == j) continue;
        size_t n = j - i;
        /* un point ou deux-points final fait partie de la phrase, pas de l'adresse */
        while (n > 0 && (in[i + n - 1] == '.' || (in[i + n - 1] == ':' && n > 2 && in[i + n - 2] != ':'))) n--;
        /* un mot qui touche la plage ("abcd", "utun15") n'est pas une adresse */
        int glued = (i > 0 && ((in[i - 1] >= 'g' && in[i - 1] <= 'z') || (in[i - 1] >= 'G' && in[i - 1] <= 'Z') || in[i - 1] == '_')) ||
                    (in[i + n] && ((in[i + n] >= 'g' && in[i + n] <= 'z') || (in[i + n] >= 'G' && in[i + n] <= 'Z') || in[i + n] == '_'));
        int v[4];
        if (!glued && is_mac(in + i, n)) {
            put(out, outsz, &o, in + i, 8);
            put(out, outsz, &o, ":xx:xx:xx", 9);
        } else if (!glued && parse_ipv4(in + i, n, v) && ipv4_is_public(v)) {
            char b[24];
            int l = snprintf(b, sizeof b, "%d.%d.x.x", v[0], v[1]);
            put(out, outsz, &o, b, (size_t)l);
        } else if (!glued && is_ipv6(in + i, n) && !ipv6_is_local(in + i, n)) {
            size_t k = 0;                                       /* on garde le préfixe jusqu'au deuxième ':' */
            int colons = 0;
            while (k < n) { if (in[i + k] == ':' && ++colons == 2) break; k++; }
            put(out, outsz, &o, in + i, k);
            put(out, outsz, &o, ":xxxx", 5);
        } else {
            put(out, outsz, &o, in + i, n);
        }
        put(out, outsz, &o, in + i + n, j - i - n);             /* ponctuation finale conservée */
        i = j;
    }
    out[o] = '\0';
    return o;
}

/* ---------- Interfaces USB ---------- */

enum diag_iface diag_iface_kind(uint8_t cls, uint8_t sub, uint8_t proto) {
    if ((cls == 0xE0 && sub == 1 && proto == 3) || (cls == 0xEF && sub == 4 && proto == 1) ||
        (cls == 2 && sub == 2 && proto == 0xFF)) return DIAG_IF_RNDIS;
    if (cls == 2 && (sub == 6 || sub == 13)) return DIAG_IF_NATIVE_NET;
    if (cls == 0xFF && sub == 0x42 && proto == 1) return DIAG_IF_ADB;
    if (cls == 6 && sub == 1 && proto == 1) return DIAG_IF_MTP;                  /* PTP / MTP */
    return DIAG_IF_OTHER;
}

const char *diag_iface_name(enum diag_iface k) {
    switch (k) {
    case DIAG_IF_RNDIS: return "RNDIS (Modem USB)";
    case DIAG_IF_NATIVE_NET: return "CDC-ECM/NCM (géré par macOS)";
    case DIAG_IF_ADB: return "ADB (débogage USB)";
    case DIAG_IF_MTP: return "MTP/PTP (transfert de fichiers)";
    default: return "autre";
    }
}

/* ---------- État du service ---------- */

static int word_is(const char *s, const char *w) {
    size_t l = strlen(w);
    return !strncmp(s, w, l) && (s[l] == '\0' || s[l] == ' ' || s[l] == '\n');
}

enum diag_state diag_state_parse(const char *s) {
    if (!s) return DIAG_ST_UNKNOWN;
    while (*s == ' ' || *s == '\t') s++;
    if (word_is(s, "starting")) return DIAG_ST_STARTING;
    if (word_is(s, "waiting")) return DIAG_ST_WAITING;
    if (word_is(s, "connecting")) return DIAG_ST_CONNECTING;
    if (word_is(s, "connected")) return DIAG_ST_CONNECTED;
    if (word_is(s, "stopped")) return DIAG_ST_STOPPED;
    if (word_is(s, "disabled")) return DIAG_ST_DISABLED;
    return DIAG_ST_UNKNOWN;
}

/* ---------- Verdict ---------- */

static void line(char *out, size_t outsz, size_t *o, const char *s) {
    put(out, outsz, o, "- ", 2);
    put(out, outsz, o, s, strlen(s));
    put(out, outsz, o, "\n", 1);
}

size_t diag_verdict(const struct diag_facts *f, char *out, size_t outsz) {
    size_t o = 0;
    if (outsz == 0) return 0;
    if (!f->supervisor_running)
        line(out, outsz, &o, "Le service ne tourne pas. Lance-le avec « sudo ./install.sh » ou « tethermac-install », "
                             "ou à la main avec « sudo rndis-tether -r -w -a ».");
    else if (!f->worker_running && f->state != DIAG_ST_DISABLED)
        line(out, outsz, &o, "Le superviseur tourne mais son worker non : il vient peut-être de redémarrer, "
                             "sinon regarde le journal ci-dessous.");
    if (f->disabled_flag || f->state == DIAG_ST_DISABLED)
        line(out, outsz, &o, "Le service est désactivé depuis l'icône de la barre de menus : réactive-le.");
    if (f->usb_native)
        line(out, outsz, &o, "Le téléphone expose CDC-ECM/NCM : macOS le gère déjà sans ce driver.");
    if (!f->usb_rndis && !f->usb_native && f->usb_mtp)
        line(out, outsz, &o, "Le téléphone est branché en transfert de fichiers seulement : active "
                             "Réglages > Point d'accès > Partage de connexion USB, ou lance le service avec -a.");
    if (!f->usb_rndis && !f->usb_native && !f->usb_mtp && !f->usb_adb)
        line(out, outsz, &o, "Aucun téléphone Android détecté sur l'USB : vérifie le câble (il doit transmettre des données) "
                             "et déverrouille le téléphone.");
    if (!f->usb_rndis && f->usb_adb && !f->usb_mtp && !f->usb_native)
        line(out, outsz, &o, "Seule l'interface ADB est visible : active le partage de connexion USB sur le téléphone.");
    if (f->usb_rndis && f->supervisor_running && f->state == DIAG_ST_WAITING)
        line(out, outsz, &o, "Un téléphone RNDIS est branché mais le service attend encore : donne-lui quelques secondes, "
                             "sinon envoie le journal.");
    if (f->state == DIAG_ST_CONNECTING)
        line(out, outsz, &o, "Connexion en cours (RNDIS, DHCP). Si cela dure plus d'une minute, regarde le journal.");
    if (f->state == DIAG_ST_CONNECTED && !f->tether_route)
        line(out, outsz, &o, "Le service se dit connecté mais les routes du tethering sont absentes : il tourne peut-être "
                             "sans l'option -r (le trafic reste sur l'autre réseau).");
    if (f->state == DIAG_ST_CONNECTED && f->tether_route && f->v6_reject)
        line(out, outsz, &o, "Le tethering n'a pas d'IPv6 : l'IPv6 du Mac est bloqué (normal si l'internet du téléphone "
                             "vient du Wi-Fi) et le trafic passe en IPv4.");
    if (!f->status_known && f->supervisor_running)
        line(out, outsz, &o, "Le fichier d'état est introuvable (/Users/Shared/rndis-tether.status).");
    if (o == 0) line(out, outsz, &o, "Rien d'anormal détecté.");
    out[o] = '\0';
    return o;
}
