// SPDX-License-Identifier: GPL-3.0-or-later
/* Tests de la lecture de `adb devices` : chaque situation doit donner le bon état et le bon message. */
#include <stdlib.h>
#include <string.h>

#include "../src/adb.h"
#include "check.h"

static uint32_t rng = 0xabcdef1u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static enum adb_status status_of(const char *out, const char *wanted, const char **serial) {
    static struct adb_devices d;                                            /* *serial pointe dedans : doit survivre à l'appel */
    adb_parse_devices(out, &d);
    return adb_choose(&d, wanted, serial);
}

static void test_serial_valid(void) {
    CHECK(adb_serial_valid("R5CX1234ABC") && adb_serial_valid("emulator-5554") && adb_serial_valid("192.168.1.5:5555"));
    CHECK(adb_serial_valid("a") && adb_serial_valid("A_b.c:d-e"));
    CHECK(!adb_serial_valid(NULL) && !adb_serial_valid(""));
    CHECK(!adb_serial_valid("a b") && !adb_serial_valid("a;b") && !adb_serial_valid("$(id)") && !adb_serial_valid("a'b"));
    CHECK(!adb_serial_valid("a\nb") && !adb_serial_valid("a`b`") && !adb_serial_valid("é") && !adb_serial_valid("a|b"));
    char s[100];
    memset(s, 'A', sizeof s); s[ADB_SERIAL_MAX] = '\0';
    CHECK(adb_serial_valid(s));                                                 /* 63 : accepté */
    s[ADB_SERIAL_MAX] = 'A'; s[ADB_SERIAL_MAX + 1] = '\0';
    CHECK(!adb_serial_valid(s));                                                /* 64 : refusé */
}

static void test_states(void) {
    const char *s;
    CHECK(status_of("List of devices attached\nR5CX1234ABC\tdevice\n\n", NULL, &s) == ADB_OK && !strcmp(s, "R5CX1234ABC"));
    CHECK(status_of("* daemon not running; starting now at tcp:5037\n* daemon started successfully\nList of devices attached\nR5CX1234ABC\tdevice\n", NULL, &s) == ADB_OK);
    CHECK(status_of("List of devices attached\nR5CX1234ABC\tdevice\r\n\r\n", NULL, &s) == ADB_OK && !strcmp(s, "R5CX1234ABC"));   /* fins de ligne Windows */
    CHECK(status_of("List of devices attached\nR5CX1234ABC       device\n", NULL, &s) == ADB_OK);               /* espaces au lieu d'une tabulation */
    CHECK(status_of("List of devices attached\nR5CX1234ABC\tdevice product:x model:SM_S928B transport_id:1\n", NULL, &s) == ADB_OK);   /* adb devices -l */

    CHECK(status_of("List of devices attached\n\n", NULL, &s) == ADB_NO_DEVICE && !*s);
    CHECK(status_of("", NULL, &s) == ADB_NO_DEVICE);
    CHECK(status_of(NULL, NULL, &s) == ADB_NO_DEVICE);
    CHECK(status_of("ADB_MISSING\nzsh: command not found: adb\n", NULL, &s) == ADB_NOT_INSTALLED);
    CHECK(status_of("ADB_MISSING\nList of devices attached\nR5CX1234ABC\tdevice\n", NULL, &s) == ADB_NOT_INSTALLED);   /* absent prime */

    CHECK(status_of("List of devices attached\nR5CX1234ABC\tunauthorized\n", NULL, &s) == ADB_UNAUTHORIZED);
    CHECK(status_of("List of devices attached\nR5CX1234ABC\toffline\n", NULL, &s) == ADB_OFFLINE);
    CHECK(status_of("List of devices attached\nR5CX1234ABC\trecovery\n", NULL, &s) == ADB_NOT_READY);
    CHECK(status_of("List of devices attached\nR5CX1234ABC\tno permissions (user in plugdev group)\n", NULL, &s) == ADB_NOT_READY);
    CHECK(status_of("List of devices attached\nR5CX1234ABC\tdevicex\n", NULL, &s) == ADB_NOT_READY);              /* « devicex » n'est pas « device » */

    /* plusieurs appareils */
    CHECK(status_of("List of devices attached\nAAA\tdevice\nBBB\tdevice\n", NULL, &s) == ADB_MULTIPLE && !*s);
    CHECK(status_of("List of devices attached\nAAA\tdevice\nBBB\tunauthorized\n", NULL, &s) == ADB_OK && !strcmp(s, "AAA"));   /* un seul prêt : c'est lui */
    CHECK(status_of("List of devices attached\nAAA\tunauthorized\nBBB\toffline\n", NULL, &s) == ADB_UNAUTHORIZED);        /* le plus actionnable d'abord */
    CHECK(status_of("List of devices attached\nAAA\toffline\nBBB\trecovery\n", NULL, &s) == ADB_OFFLINE);
    CHECK(status_of("List of devices attached\nAAA\trecovery\n", NULL, &s) == ADB_NOT_READY);
}

static void test_wanted(void) {
    const char *s;
    const char *two = "List of devices attached\nAAA\tdevice\nBBB\tdevice\n";
    CHECK(status_of(two, "BBB", &s) == ADB_OK && !strcmp(s, "BBB"));            /* -S désigne le bon */
    CHECK(status_of(two, "AAA", &s) == ADB_OK && !strcmp(s, "AAA"));
    CHECK(status_of(two, "CCC", &s) == ADB_WANTED_ABSENT && !*s);
    CHECK(status_of(two, "", &s) == ADB_MULTIPLE);                              /* vide : comme absent */
    CHECK(status_of("List of devices attached\nAAA\tunauthorized\nBBB\tdevice\n", "AAA", &s) == ADB_UNAUTHORIZED);
    CHECK(status_of("List of devices attached\nAAA\toffline\nBBB\tdevice\n", "AAA", &s) == ADB_OFFLINE);
    CHECK(status_of("List of devices attached\nAAA\trecovery\n", "AAA", &s) == ADB_NOT_READY);
    CHECK(status_of("List of devices attached\n", "AAA", &s) == ADB_WANTED_ABSENT);
    CHECK(status_of("ADB_MISSING\n", "AAA", &s) == ADB_NOT_INSTALLED);
    CHECK(status_of(two, "AA", &s) == ADB_WANTED_ABSENT);                       /* pas de correspondance partielle */
}

static void test_hostile(void) {
    struct adb_devices d;
    const char *s;
    adb_parse_devices("List of devices attached\nA;rm -rf /\tdevice\nB$(id)\tdevice\nGOOD\tdevice\n", &d);   /* séries dangereux : écartés */
    CHECK(d.n == 1 && d.dropped == 2 && !strcmp(d.dev[0].serial, "GOOD"));
    CHECK(adb_choose(&d, NULL, &s) == ADB_OK && !strcmp(s, "GOOD"));

    char big[4000] = "List of devices attached\n";                              /* beaucoup d'appareils : plafonné */
    for (int k = 0; k < 20; k++) { char l[40]; snprintf(l, sizeof l, "DEV%d\tdevice\n", k); strcat(big, l); }
    adb_parse_devices(big, &d);
    CHECK(d.n == ADB_MAX_DEVICES && d.dropped == 20 - ADB_MAX_DEVICES);
    CHECK(adb_choose(&d, NULL, &s) == ADB_MULTIPLE);

    char longline[1000];                                                        /* ligne très longue : tronquée, sans débordement */
    memset(longline, 'X', sizeof longline - 1); longline[sizeof longline - 1] = '\0';
    char out[1200];
    snprintf(out, sizeof out, "%s\tdevice\nOK\tdevice\n", longline);
    adb_parse_devices(out, &d);
    CHECK(d.n == 1 && !strcmp(d.dev[0].serial, "OK"));

    adb_parse_devices("nodelimiter\n\t\n \tdevice\nA\t\n", &d);                  /* lignes mal formées */
    CHECK(d.n == 0);

    for (int k = 0; k < 20000; k++) {                                           /* aucune entrée ne fait déborder ni planter */
        char in[300];
        size_t len = rnd() % sizeof in;
        static const char alpha[] = "ABab09:._- \t\n\r*;$device";
        for (size_t i = 0; i < len; i++) in[i] = alpha[rnd() % (sizeof alpha - 1)];
        in[len] = '\0';
        adb_parse_devices(in, &d);
        CHECK(d.n >= 0 && d.n <= ADB_MAX_DEVICES);
        for (int j = 0; j < d.n; j++) CHECK(adb_serial_valid(d.dev[j].serial));
        enum adb_status st = adb_choose(&d, NULL, &s);
        CHECK(st == ADB_OK ? (s != NULL) : !*s);
    }
}

static void test_messages(void) {
    CHECK(adb_status_message(ADB_OK) == NULL);
    for (int k = ADB_NOT_INSTALLED; k <= ADB_WANTED_ABSENT; k++) {
        const char *m = adb_status_message((enum adb_status)k);
        CHECK(m && strlen(m) > 20);
        for (int j = ADB_NOT_INSTALLED; j < k; j++) CHECK(strcmp(m, adb_status_message((enum adb_status)j)) != 0);   /* tous différents */
    }
    CHECK(strstr(adb_status_message(ADB_UNAUTHORIZED), "Autoriser") != NULL);
    CHECK(strstr(adb_status_message(ADB_MULTIPLE), "-S") != NULL);
    CHECK(strstr(adb_status_message(ADB_NOT_INSTALLED), "android-platform-tools") != NULL);
    CHECK(adb_status_message((enum adb_status)99) == NULL);
}

void run_adb_tests(void) {
    test_serial_valid();
    test_states();
    test_wanted();
    test_hostile();
    test_messages();
}
