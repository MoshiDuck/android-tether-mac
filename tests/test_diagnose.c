// SPDX-License-Identifier: GPL-3.0-or-later
/* Tests de la logique de `--diagnose` : le caviardage ne doit rien laisser passer, le verdict doit être juste. */
#include <stdlib.h>
#include <string.h>

#include "../src/diagnose.h"
#include "check.h"

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static int redacts_to(const char *in, const char *want) {
    char out[256];
    diag_redact(in, out, sizeof out);
    if (strcmp(out, want)) fprintf(stderr, "  caviardage de \"%s\" : \"%s\" au lieu de \"%s\"\n", in, out, want);
    return !strcmp(out, want);
}

static void test_redact_mac(void) {
    CHECK(redacts_to("MAC hôte: f2:08:36:81:42:a7", "MAC hôte: f2:08:36:xx:xx:xx"));
    CHECK(redacts_to("F2:08:36:81:42:A7", "F2:08:36:xx:xx:xx"));
    CHECK(redacts_to("link f2:08:36:81:42:a7, up", "link f2:08:36:xx:xx:xx, up"));
    CHECK(redacts_to("f2:08:36:81:42", "f2:08:36:81:42"));                 /* cinq groupes : pas une MAC */
}

static void test_redact_ipv4(void) {
    CHECK(redacts_to("IP publique 72.219.13.220.", "IP publique 72.219.x.x."));
    CHECK(redacts_to("gateway 10.217.123.221, DNS 10.217.123.221", "gateway 10.217.123.221, DNS 10.217.123.221"));
    CHECK(redacts_to("192.168.0.1 172.16.5.4 172.31.0.1 169.254.1.2 127.0.0.1", "192.168.0.1 172.16.5.4 172.31.0.1 169.254.1.2 127.0.0.1"));
    CHECK(redacts_to("172.32.0.1", "172.32.x.x"));                          /* hors de 172.16/12 : public */
    CHECK(redacts_to("100.64.0.1 100.128.0.1", "100.64.0.1 100.128.x.x"));  /* CGNAT conservé, le reste masqué */
    CHECK(redacts_to("224.0.0.251 255.255.255.255 0.0.0.0", "224.0.0.251 255.255.255.255 0.0.0.0"));
    CHECK(redacts_to("8.8.8.8", "8.8.x.x"));
    CHECK(redacts_to("add net 128.0.0.0: gateway", "add net 128.0.0.0: gateway"));   /* bloc réseau, pas un hôte */
    CHECK(redacts_to("8.8.0.1 8.8.1.0", "8.8.x.x 8.8.x.x"));
    CHECK(redacts_to("192.0.2.1 192.169.0.1 172.15.0.1 169.253.1.1", "192.0.x.x 192.169.x.x 172.15.x.x 169.253.x.x"));   /* juste hors des plages privées */
    CHECK(redacts_to("256.1.1.1", "256.1.1.1"));                            /* pas une adresse */
    CHECK(redacts_to("1.2.3", "1.2.3"));
    CHECK(redacts_to("version 0.4.0-dev", "version 0.4.0-dev"));
    CHECK(redacts_to("utun15 mtu 1500", "utun15 mtu 1500"));
}

static void test_redact_ipv6(void) {
    CHECK(redacts_to("addr 2600:383:a42:eff8:412c:a939:6764:bc93/64", "addr 2600:383:xxxx/64"));
    CHECK(redacts_to("2606:4700:4700::1111", "2606:4700:xxxx"));
    CHECK(redacts_to("fe80::2e58:4fff:febc:ad83%en0", "fe80::2e58:4fff:febc:ad83%en0"));
    CHECK(redacts_to("fd39:fbfa:6382::", "fd39:fbfa:6382::"));
    CHECK(redacts_to("::1 et ::", "::1 et ::"));
    CHECK(redacts_to("::/1 8000::/1", "::/1 8000::/1"));
    CHECK(redacts_to("2600::1", "2600::xxxx"));
}

static void test_redact_misc(void) {
    CHECK(redacts_to("", ""));
    CHECK(redacts_to("rien à caviarder : ok", "rien à caviarder : ok"));
    CHECK(redacts_to("deadbeef cafe", "deadbeef cafe"));
    char tiny[8];
    CHECK(diag_redact("72.219.13.220 et du texte", tiny, sizeof tiny) < sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny);
    char one[1];
    CHECK(diag_redact("abc", one, 1) == 0 && one[0] == '\0');
    CHECK(diag_redact("abc", one, 0) == 0);
}

/* Propriété : un texte assemblé à partir de morceaux sensibles ne doit plus jamais les contenir, quelle que
 * soit la ponctuation autour, et caviarder une deuxième fois ne change plus rien. */
static void test_redact_property(void) {
    static const char *secrets[] = {"f2:08:36:81:42:a7", "72.219.13.220", "2600:383:a42:eff8:412c:a939:6764:bc93",
                                    "8.8.4.4", "2a01:e0a:1b2:3c40::1"};
    static const char *tails[] = {"81:42:a7", ".13.220", "a42:eff8", ".4.4", "3c40::1"};   /* la partie masquée de chacun, dans le même ordre */
    static const char *glue[] = {" ", ", ", ".", ": ", " (", ") ", "=", "/64 ", "\t"};
    for (int n = 0; n < 4000; n++) {
        char in[400] = "", out[600], again[600];
        int used[5] = {0};
        for (int k = 0; k < 6; k++) {
            int s = (int)(rnd() % 5);
            used[s] = 1;
            strcat(in, secrets[s]);
            strcat(in, glue[rnd() % 9]);
            strcat(in, "mot");
            strcat(in, glue[rnd() % 9]);
        }
        diag_redact(in, out, sizeof out);
        for (int s = 0; s < 5; s++) if (used[s]) CHECK(strstr(out, tails[s]) == NULL);
        diag_redact(out, again, sizeof again);
        CHECK(strcmp(out, again) == 0);
    }
}

/* Aucune entrée, même hostile, ne doit déborder ni laisser une sortie non terminée. */
static void test_redact_fuzz(void) {
    for (int n = 0; n < 20000; n++) {
        char in[200];
        size_t len = rnd() % (sizeof in - 1);
        static const char alpha[] = "0123456789abcdefABCDEF:.:.:.%/ zZ_-";
        for (size_t i = 0; i < len; i++) in[i] = alpha[rnd() % (sizeof alpha - 1)];
        in[len] = '\0';
        size_t osz = 1 + rnd() % 300;
        char *out = malloc(osz);
        size_t w = diag_redact(in, out, osz);
        CHECK(w < osz && out[w] == '\0' && strlen(out) == w);
        free(out);
    }
}

static void test_iface_kind(void) {
    CHECK(diag_iface_kind(0xE0, 1, 3) == DIAG_IF_RNDIS);
    CHECK(diag_iface_kind(0xEF, 4, 1) == DIAG_IF_RNDIS);
    CHECK(diag_iface_kind(2, 2, 0xFF) == DIAG_IF_RNDIS);
    CHECK(diag_iface_kind(2, 6, 0) == DIAG_IF_NATIVE_NET);
    CHECK(diag_iface_kind(2, 13, 0) == DIAG_IF_NATIVE_NET);
    CHECK(diag_iface_kind(0xFF, 0x42, 1) == DIAG_IF_ADB);
    CHECK(diag_iface_kind(6, 1, 1) == DIAG_IF_MTP);
    CHECK(diag_iface_kind(3, 1, 1) == DIAG_IF_OTHER);                       /* clavier */
    CHECK(diag_iface_kind(8, 6, 0x50) == DIAG_IF_OTHER);                    /* disque */
    for (int k = 0; k <= DIAG_IF_MTP; k++) CHECK(strlen(diag_iface_name((enum diag_iface)k)) > 3);
}

static void test_state_parse(void) {
    CHECK(diag_state_parse(NULL) == DIAG_ST_UNKNOWN);
    CHECK(diag_state_parse("") == DIAG_ST_UNKNOWN);
    CHECK(diag_state_parse("starting") == DIAG_ST_STARTING);
    CHECK(diag_state_parse("waiting") == DIAG_ST_WAITING);
    CHECK(diag_state_parse("connecting") == DIAG_ST_CONNECTING);
    CHECK(diag_state_parse("connected 10.217.123.154 utun15") == DIAG_ST_CONNECTED);
    CHECK(diag_state_parse("  stopped") == DIAG_ST_STOPPED);
    CHECK(diag_state_parse("disabled\n") == DIAG_ST_DISABLED);
    CHECK(diag_state_parse("connectedX") == DIAG_ST_UNKNOWN);
    CHECK(diag_state_parse("wait") == DIAG_ST_UNKNOWN);
}

static int says(const struct diag_facts *f, const char *needle) {
    char v[1500];
    diag_verdict(f, v, sizeof v);
    return strstr(v, needle) != NULL;
}

static struct diag_facts healthy(void) {
    struct diag_facts f;
    memset(&f, 0, sizeof f);
    f.status_known = 1; f.state = DIAG_ST_CONNECTED; f.supervisor_running = 1; f.worker_running = 1;
    f.usb_rndis = 1; f.usb_adb = 1; f.tether_route = 1;
    return f;
}

static void test_verdict(void) {
    struct diag_facts f = healthy();
    CHECK(says(&f, "Rien d'anormal"));
    char v[1500];
    CHECK(diag_verdict(&f, v, sizeof v) == strlen(v));

    f = healthy(); f.supervisor_running = 0; f.worker_running = 0; f.state = DIAG_ST_UNKNOWN; f.status_known = 0;
    CHECK(says(&f, "ne tourne pas") && !says(&f, "Rien d'anormal"));

    f = healthy(); f.worker_running = 0;
    CHECK(says(&f, "worker"));

    f = healthy(); f.disabled_flag = 1;
    CHECK(says(&f, "désactivé"));
    f = healthy(); f.state = DIAG_ST_DISABLED; f.worker_running = 0;
    CHECK(says(&f, "désactivé") && !says(&f, "worker"));

    f = healthy(); f.usb_rndis = 0; f.usb_native = 1;
    CHECK(says(&f, "macOS le gère"));

    f = healthy(); f.usb_rndis = 0; f.usb_adb = 0; f.usb_mtp = 1;
    CHECK(says(&f, "transfert de fichiers"));

    f = healthy(); f.usb_rndis = 0; f.usb_adb = 0;
    CHECK(says(&f, "Aucun téléphone") && says(&f, "câble"));

    f = healthy(); f.usb_rndis = 0;
    CHECK(says(&f, "Seule l'interface ADB"));

    f = healthy(); f.state = DIAG_ST_WAITING; f.tether_route = 0;
    CHECK(says(&f, "attend encore"));

    f = healthy(); f.state = DIAG_ST_CONNECTING;
    CHECK(says(&f, "Connexion en cours"));

    f = healthy(); f.tether_route = 0;
    CHECK(says(&f, "routes du tethering sont absentes"));

    f = healthy(); f.v6_reject = 1;
    CHECK(says(&f, "IPv6 du Mac est bloqué"));

    f = healthy(); f.status_known = 0;
    CHECK(says(&f, "fichier d'état est introuvable"));

    /* sortie tronquée : jamais de débordement, toujours terminée */
    f = healthy(); f.supervisor_running = 0; f.usb_rndis = 0; f.usb_adb = 0;
    for (size_t sz = 1; sz < 60; sz++) {
        char t[60];
        size_t w = diag_verdict(&f, t, sz);
        CHECK(w < sz && t[w] == '\0');
    }
    CHECK(diag_verdict(&f, v, 0) == 0);
}

void run_diagnose_tests(void) {
    test_redact_mac();
    test_redact_ipv4();
    test_redact_ipv6();
    test_redact_misc();
    test_redact_property();
    test_redact_fuzz();
    test_iface_kind();
    test_state_parse();
    test_verdict();
}
