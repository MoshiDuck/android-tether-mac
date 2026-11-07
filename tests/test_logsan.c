// SPDX-License-Identifier: GPL-3.0-or-later
/* Tests de l'assainissement des lignes de journal du worker. */
#include <stdlib.h>
#include <string.h>

#include "../src/logsan.h"
#include "check.h"

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static int sanitizes_to(const char *in, const char *want) {
    char out[LOG_LINE_MAX];
    size_t n = log_sanitize(in, strlen(in), out, sizeof out);
    if (strcmp(out, want) || n != strlen(want)) fprintf(stderr, "  assainir \"%s\" : \"%s\" au lieu de \"%s\"\n", in, out, want);
    return !strcmp(out, want) && n == strlen(want);
}

static void test_basic(void) {
    CHECK(sanitizes_to("DHCP en cours sur utun15…", "DHCP en cours sur utun15…"));          /* UTF-8 conservé */
    CHECK(sanitizes_to("ligne\n", "ligne"));
    CHECK(sanitizes_to("ligne\r\n\n", "ligne"));
    CHECK(sanitizes_to("", ""));
    CHECK(sanitizes_to("\n", ""));
    CHECK(sanitizes_to("a\tb", "a?b"));
    CHECK(sanitizes_to("\x1b[31mrouge\x1b[0m", "?[31mrouge?[0m"));                           /* séquences d'échappement neutralisées */
    CHECK(sanitizes_to("x\x7f" "y", "x?y"));
    CHECK(sanitizes_to("a\nb", "a?b"));                                                      /* un saut de ligne au milieu : pas de fausse ligne */
}

static void test_prefix(void) {
    CHECK(sanitizes_to("[worker] bonjour", "bonjour"));
    CHECK(sanitizes_to("[root] fausse ligne du superviseur", "[root] fausse ligne du superviseur"));   /* gardée, mais le superviseur remet « [worker] » devant */
    CHECK(sanitizes_to("[worker] [worker] deux fois", "[worker] deux fois"));                /* un seul préfixe retiré */
    CHECK(sanitizes_to("[worker]", "[worker]"));                                             /* pas d'espace : ce n'est pas le préfixe */
    CHECK(sanitizes_to("[worker] ", ""));
}

static void test_truncation(void) {
    char in[2000], out[LOG_LINE_MAX];
    memset(in, 'A', sizeof in - 1);
    in[sizeof in - 1] = '\0';
    size_t n = log_sanitize(in, strlen(in), out, sizeof out);
    CHECK(n == strlen(out) && n < LOG_LINE_MAX);
    CHECK(n >= LOG_LINE_MAX - 8 && !strcmp(out + n - 5, "[...]"));
    memset(in, 'B', LOG_LINE_MAX - 1); in[LOG_LINE_MAX - 1] = '\0';                       /* juste ce qui rentre : pas de marque */
    n = log_sanitize(in, strlen(in), out, sizeof out);
    CHECK(n == LOG_LINE_MAX - 1 && out[0] == 'B' && !strstr(out, "[...]"));
    memset(in, 'C', LOG_LINE_MAX); in[LOG_LINE_MAX] = '\0';                               /* un octet de trop : marque */
    n = log_sanitize(in, strlen(in), out, sizeof out);
    CHECK(n < LOG_LINE_MAX && !strcmp(out + n - 5, "[...]"));
    memset(in, '\x1b', 1000); in[1000] = '\0';                                            /* les contrôles sont aussi neutralisés dans la partie gardée */
    n = log_sanitize(in, strlen(in), out, sizeof out);
    CHECK(!strchr(out, '\x1b') && out[0] == '?');
}

static void test_small_buffers(void) {
    char out[64];
    CHECK(log_sanitize("abc", 3, out, 0) == 0);
    for (size_t cap = 1; cap < 40; cap++) {
        memset(out, 'Z', sizeof out);
        size_t n = log_sanitize("[worker] une ligne assez longue pour être tronquée dans un petit tampon", 70, out, cap);
        CHECK(n < cap && out[n] == '\0' && strlen(out) == n);
    }
}

/* Propriété : quelle que soit l'entrée, la sortie est terminée, courte, sans caractère de contrôle. */
static void test_property(void) {
    for (int k = 0; k < 20000; k++) {
        char in[700], out[LOG_LINE_MAX];
        size_t len = rnd() % sizeof in;
        for (size_t i = 0; i < len; i++) in[i] = (char)(rnd() % 256);
        size_t n = log_sanitize(in, len, out, sizeof out);
        CHECK(n < LOG_LINE_MAX && out[n] == '\0' && strlen(out) == n);
        int clean = 1;
        for (size_t i = 0; i < n; i++) if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] == 0x7f) clean = 0;
        CHECK(clean);
    }
}

void run_logsan_tests(void) {
    test_basic();
    test_prefix();
    test_truncation();
    test_small_buffers();
    test_property();
}
