// SPDX-License-Identifier: GPL-3.0-or-later
/* Tests de la politique de validation du superviseur : valeurs correctes acceptées, hostiles refusées. */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "../src/policy.h"
#include "check.h"

static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint8_t x[4] = {a, b, c, d};
    uint32_t v;
    memcpy(&v, x, 4);
    return v;
}

/* masque de longueur 0 à 32, jamais d'élément indéfini (décalage de 32) */
static uint32_t sh_rand_mask(void);
static uint32_t rng = 0x2545f491u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

#define OK POLICY_OK

static void test_v4_accepts(void) {
    uint32_t m24 = ip4(255, 255, 255, 0);
    CHECK(policy_check_v4(ip4(10, 217, 123, 144), ip4(10, 217, 123, 221), m24, ip4(10, 217, 123, 221)) == OK);
    CHECK(policy_check_v4(ip4(192, 168, 42, 129), ip4(192, 168, 42, 129 - 1), m24, 0) == OK);          /* sans DNS */
    CHECK(policy_check_v4(ip4(10, 217, 123, 144), ip4(10, 217, 123, 221), m24, ip4(8, 8, 8, 8)) == OK); /* DNS hors sous-réseau */
    CHECK(policy_check_v4(ip4(10, 1, 2, 3), ip4(10, 1, 2, 1), ip4(255, 0, 0, 0), 0) == OK);            /* /8 */
    CHECK(policy_check_v4(ip4(10, 1, 2, 5), ip4(10, 1, 2, 6), ip4(255, 255, 255, 252), 0) == OK);      /* /30 */
}

static void test_v4_rejects(void) {
    uint32_t m24 = ip4(255, 255, 255, 0);
    uint32_t ip = ip4(10, 217, 123, 144), gw = ip4(10, 217, 123, 221);

    /* masque */
    CHECK(policy_check_v4(ip, gw, 0, 0) == POLICY_BAD_MASK);
    CHECK(policy_check_v4(ip, gw, ip4(255, 255, 255, 255), 0) == POLICY_BAD_MASK);                      /* /32 */
    CHECK(policy_check_v4(ip, gw, ip4(255, 255, 255, 254), 0) == POLICY_BAD_MASK);                      /* /31 */
    CHECK(policy_check_v4(ip, gw, ip4(254, 0, 0, 0), 0) == POLICY_BAD_MASK);                            /* /7 */
    CHECK(policy_check_v4(ip, gw, ip4(255, 0, 255, 0), 0) == POLICY_BAD_MASK);                          /* non contigu */
    CHECK(policy_check_v4(ip, gw, ip4(0, 255, 255, 255), 0) == POLICY_BAD_MASK);

    /* adresse */
    uint32_t bad_ips[] = {0, 0xffffffffu, ip4(127, 0, 0, 1), ip4(224, 0, 0, 1), ip4(239, 255, 255, 250),
                          ip4(240, 0, 0, 1), ip4(169, 254, 3, 4)};
    for (unsigned k = 0; k < sizeof bad_ips / sizeof *bad_ips; k++)
        CHECK(policy_check_v4(bad_ips[k], gw, m24, 0) == POLICY_BAD_IP);
    CHECK(policy_check_v4(ip4(10, 217, 123, 0), gw, m24, 0) == POLICY_BAD_IP);                          /* adresse de réseau */
    CHECK(policy_check_v4(ip4(10, 217, 123, 255), gw, m24, 0) == POLICY_BAD_IP);                        /* broadcast */

    /* passerelle */
    CHECK(policy_check_v4(ip, ip, m24, 0) == POLICY_BAD_GATEWAY);                                       /* = notre adresse */
    CHECK(policy_check_v4(ip, ip4(10, 217, 124, 221), m24, 0) == POLICY_BAD_GATEWAY);                   /* autre sous-réseau */
    CHECK(policy_check_v4(ip, ip4(10, 217, 123, 0), m24, 0) == POLICY_BAD_GATEWAY);
    CHECK(policy_check_v4(ip, ip4(10, 217, 123, 255), m24, 0) == POLICY_BAD_GATEWAY);
    CHECK(policy_check_v4(ip, 0, m24, 0) == POLICY_BAD_GATEWAY);
    CHECK(policy_check_v4(ip4(127, 0, 0, 5), ip4(127, 0, 0, 1), ip4(255, 0, 0, 0), 0) == POLICY_BAD_IP);

    /* DNS */
    CHECK(policy_check_v4(ip, gw, m24, ip4(127, 0, 0, 1)) == POLICY_BAD_DNS);
    CHECK(policy_check_v4(ip, gw, m24, ip4(224, 0, 0, 251)) == POLICY_BAD_DNS);
    CHECK(policy_check_v4(ip, gw, m24, 0xffffffffu) == POLICY_BAD_DNS);
    CHECK(policy_check_v4(ip, gw, m24, ip4(169, 254, 1, 1)) == POLICY_BAD_DNS);
}

/* Propriété : si la politique accepte, les conditions de sécurité tiennent, quelles que soient les entrées. */
static void test_v4_property(void) {
    int accepted = 0, rejected = 0;
    for (int iter = 0; iter < 400000; iter++) {
        /* configuration plausible (masque /8../30, passerelle dans le sous-réseau), puis corrompue au hasard */
        uint32_t hm = 0xffffffffu << (2 + rnd() % 23);
        uint32_t hi = rnd(), hg = (hi & hm) | (rnd() & ~hm), hd = rnd() % 3 ? 0 : rnd();
        uint32_t ip = htonl(hi), gw = htonl(hg), mask = htonl(hm), dns = htonl(hd);
        switch (rnd() % 7) {
        case 0: ip = rnd(); break;
        case 1: gw = rnd(); break;
        case 2: mask = rnd(); break;
        case 3: dns = rnd(); break;
        case 4: gw = ip; break;
        default: break;
        }
        if (policy_check_v4(ip, gw, mask, dns) != OK) { rejected++; continue; }
        accepted++;
        uint32_t i = ntohl(ip), g = ntohl(gw), m = ntohl(mask), d = ntohl(dns);
        CHECK((i >> 24) != 127 && (i >> 24) < 224 && i != 0 && i != 0xffffffffu);
        CHECK((g & m) == (i & m) && g != i);
        CHECK(i != (i & m) && i != ((i & m) | ~m) && g != (i & m) && g != ((i & m) | ~m));
        CHECK(__builtin_popcount(m) >= 8 && __builtin_popcount(m) <= 30);
        CHECK(((m | (m - 1)) + 1) == 0 || (~m & (~m + 1)) == 0);                  /* masque contigu */
        CHECK(d == 0 || ((d >> 24) != 127 && (d >> 24) < 224));
    }
    CHECK(accepted > 100000 && rejected > 10000);                                 /* les deux chemins sont bien exercés */
}

static void test_overlap(void) {
    uint32_t m8 = ip4(255, 0, 0, 0), m15 = ip4(255, 254, 0, 0), m16 = ip4(255, 255, 0, 0);
    uint32_t m20 = ip4(255, 255, 240, 0), m24 = ip4(255, 255, 255, 0), m30 = ip4(255, 255, 255, 252);

    /* recouvrement brut */
    CHECK(policy_subnets_overlap(ip4(192, 168, 1, 5), m24, ip4(192, 168, 1, 200), m24));       /* même réseau */
    CHECK(!policy_subnets_overlap(ip4(192, 168, 1, 5), m24, ip4(192, 168, 2, 5), m24));        /* réseaux voisins */
    CHECK(policy_subnets_overlap(ip4(10, 0, 0, 5), m8, ip4(10, 217, 123, 1), m24));            /* l'un contient l'autre */
    CHECK(policy_subnets_overlap(ip4(10, 217, 123, 1), m24, ip4(10, 0, 0, 5), m8));            /* dans les deux sens */
    CHECK(!policy_subnets_overlap(ip4(10, 0, 0, 5), m8, ip4(11, 0, 0, 5), m8));

    /* garde : réseaux « ordinaires » de /16 à /30 */
    CHECK(policy_check_overlap(ip4(192, 168, 1, 7), m24, ip4(192, 168, 1, 42), m24) == POLICY_OVERLAP);
    CHECK(policy_check_overlap(ip4(172, 22, 208, 144), m24, ip4(172, 22, 209, 216), m20) == POLICY_OVERLAP); /* /24 dans un /20 */
    CHECK(policy_check_overlap(ip4(10, 217, 123, 144), m24, ip4(172, 22, 209, 216), m20) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(192, 168, 1, 7), m24, ip4(192, 168, 2, 7), m24) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(10, 1, 2, 5), m30, ip4(10, 1, 2, 6), m30) == POLICY_OVERLAP);             /* limite haute /30 */
    CHECK(policy_check_overlap(ip4(10, 1, 0, 9), m24, ip4(10, 1, 0, 1), m16) == POLICY_OVERLAP);             /* limite basse /16 */

    /* ne bloquent pas : /8 d'entreprise, /15, adresse hôte (VPN), /31, masque nul ou non contigu */
    CHECK(policy_check_overlap(ip4(10, 217, 123, 144), m24, ip4(10, 1, 2, 3), m8) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(10, 1, 2, 5), m24, ip4(10, 1, 2, 9), m15) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(10, 217, 123, 144), m24, ip4(10, 217, 123, 50), ip4(255, 255, 255, 255)) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(10, 217, 123, 144), m24, ip4(10, 217, 123, 50), ip4(255, 255, 255, 254)) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(10, 217, 123, 144), m24, ip4(10, 217, 123, 50), 0) == POLICY_OK);
    CHECK(policy_check_overlap(ip4(10, 217, 123, 144), m24, ip4(10, 217, 123, 50), ip4(255, 0, 255, 0)) == POLICY_OK);
}

static void test_overlap_property(void) {
    int refused = 0;
    for (int iter = 0; iter < 400000; iter++) {
        uint32_t ma = htonl(0xffffffffu << (2 + rnd() % 23)), mb = htonl(sh_rand_mask());
        uint32_t a = rnd(), b = (iter & 1) ? ((a & ma) | (rnd() & ~ma)) : rnd();    /* souvent dans le même réseau */
        int ab = policy_subnets_overlap(a, ma, b, mb), ba = policy_subnets_overlap(b, mb, a, ma);
        CHECK(ab == ba);                                                          /* symétrique */
        if (policy_check_overlap(a, ma, b, mb) == POLICY_OVERLAP) {
            refused++;
            int p = __builtin_popcount(ntohl(mb));
            CHECK(ab && p >= 16 && p <= 30);                                      /* jamais de refus hors de la règle */
        }
        if (ma == mb && (a & ma) == (b & mb)) CHECK(ab);                          /* même réseau : toujours recouvrement */
    }
    CHECK(refused > 1000);
}

static void test_v6(void) {
    uint8_t global[16] = {0x26, 0x00, 0x03, 0x83, 0x0a, 0x44, 0x4d, 0x02, 0xe0, 0x93, 0x84, 0x46, 0x5f, 0xb7, 0xd7, 0x5f};
    uint8_t ula[16] = {0xfd, 0x12, 0x34, 0x56};
    uint8_t ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x7c, 0x02, 0x31, 0xff, 0xfe, 0xd7, 0xbd, 0xf2};
    CHECK(policy_check_v6(global, ll) == OK);
    CHECK(policy_check_v6(ula, ll) == OK);

    uint8_t bad[16];
    memset(bad, 0, sizeof bad);
    CHECK(policy_check_v6(bad, ll) == POLICY_BAD_V6_ADDR);                         /* :: */
    bad[15] = 1;
    CHECK(policy_check_v6(bad, ll) == POLICY_BAD_V6_ADDR);                         /* ::1 */
    CHECK(policy_check_v6(ll, ll) == POLICY_BAD_V6_ADDR);                          /* adresse link-local */
    memset(bad, 0, sizeof bad); bad[0] = 0xff; bad[1] = 0x02; bad[15] = 1;
    CHECK(policy_check_v6(bad, ll) == POLICY_BAD_V6_ADDR);                         /* multicast */
    CHECK(policy_check_v6(global, global) == POLICY_BAD_V6_ROUTER);                /* routeur non link-local */
    uint8_t site[16] = {0xfe, 0xc0};
    CHECK(policy_check_v6(global, site) == POLICY_BAD_V6_ROUTER);                  /* fec0::/10 (obsolète) */
    memset(bad, 0, sizeof bad);
    CHECK(policy_check_v6(global, bad) == POLICY_BAD_V6_ROUTER);
}

static int st(const char *s) { return policy_check_status(s, strlen(s)); }

static void test_status(void) {
    CHECK(st("waiting") == OK);
    CHECK(st("connecting") == OK);
    CHECK(st("connected 10.217.123.85 utun13") == OK);
    CHECK(st("connected 192.168.42.129 utun0") == OK);

    CHECK(st("") == POLICY_BAD_STATUS);
    CHECK(st("waiting ") == POLICY_BAD_STATUS);
    CHECK(st("Waiting") == POLICY_BAD_STATUS);
    CHECK(st("disabled") == POLICY_BAD_STATUS);                                    /* seul le superviseur l'écrit */
    CHECK(st("connected") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.217.123.85") == POLICY_BAD_STATUS);
    CHECK(st("connected 999.1.1.1 utun13") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.0.0.1 eth0") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.0.0.1 utun") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.0.0.1 utun123456") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.0.0.1 utun1x") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.0.0.1 utun1 extra") == POLICY_BAD_STATUS);
    CHECK(st("waiting\nconnected 1.2.3.4 utun1") == POLICY_BAD_STATUS);            /* saut de ligne */
    CHECK(st("waiting;rm -rf /") == POLICY_BAD_STATUS);
    CHECK(st("connected $(id) utun1") == POLICY_BAD_STATUS);
    CHECK(st("connected 10.0.0.1 utun1\"") == POLICY_BAD_STATUS);
    CHECK(policy_check_status("wait\0ing", 8) == POLICY_BAD_STATUS);                /* NUL incorporé */

    char longtxt[200];
    memset(longtxt, 'a', sizeof longtxt);
    CHECK(policy_check_status(longtxt, 150) == POLICY_BAD_STATUS);
    CHECK(policy_check_status(longtxt, POLICY_STATUS_MAX + 1) == POLICY_BAD_STATUS);
}

static void test_status_fuzz(void) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789 .:_-\n;$\"'\\\0";
    for (int iter = 0; iter < 200000; iter++) {
        char buf[100];
        size_t len = rnd() % 95;
        for (size_t k = 0; k < len; k++) buf[k] = alphabet[rnd() % (sizeof alphabet)];
        if (iter % 3 == 0 && len > 12) memcpy(buf, "connected ", 10);
        int r = policy_check_status(buf, len);
        if (r == OK) {                                                              /* tout ce qui passe est strictement borné */
            CHECK(len > 0 && len <= POLICY_STATUS_MAX);
            for (size_t k = 0; k < len; k++) CHECK(buf[k] != '\n' && buf[k] != '\0' && buf[k] != ';' && buf[k] != '$');
        }
    }
}

static uint32_t sh_rand_mask(void) {
    uint32_t sh = rnd() % 33;
    return sh >= 32 ? 0 : 0xffffffffu << sh;
}

void run_policy_tests(void) {
    test_overlap();
    test_overlap_property();
    test_v4_accepts();
    test_v4_rejects();
    test_v4_property();
    test_v6();
    test_status();
    test_status_fuzz();
    CHECK(strlen(policy_strerror(POLICY_BAD_IP)) > 0 && strlen(policy_strerror(999)) > 0);
}
