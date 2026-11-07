// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Tests unitaires et de robustesse du driver, sans téléphone ni root.
 * On inclut src/main.c directement pour atteindre ses fonctions static ; main() est renommé.
 * Les données venant du téléphone sont considérées comme hostiles : les tests de
 * « fuzz » tournent sous AddressSanitizer/UBSan (voir le Makefile).
 */
#define main rndis_tether_main
#include "../src/main.c"
#undef main
#include <fcntl.h>

#include "check.h"
int checks, failures;
#include "test_supervisor.h"
void run_dhcp_tests(void);
void run_policy_tests(void);
void run_ipc_tests(void);

static const uint8_t TEST_MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint8_t x[4] = {a, b, c, d};
    uint32_t v;
    memcpy(&v, x, 4);
    return v;
}

/* générateur pseudo-aléatoire déterministe (xorshift32) pour des tests reproductibles */
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* ---------- helpers d'octets ---------- */
static void test_byte_helpers(void) {
    uint8_t b[4];
    put32(b, 0x12345678u);
    CHECK(b[0] == 0x78 && b[3] == 0x12);          /* RNDIS = little-endian */
    CHECK(get32(b) == 0x12345678u);
    putbe32(b, 0xA1B2C3D4u);
    CHECK(b[0] == 0xA1 && b[3] == 0xD4);          /* réseau = big-endian */
    CHECK(be32(b) == 0xA1B2C3D4u);
    putbe16(b, 0xBEEF);
    CHECK(b[0] == 0xBE && b[1] == 0xEF);
}

/* ---------- checksum IP ---------- */
static void test_ip_csum(void) {
    /* exemple classique : en-tête IPv4, checksum attendu 0xb861 */
    uint8_t h[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0x00, 0x00,
                     0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    CHECK(ip_csum(h, 20) == 0xb861);
    h[10] = 0xb8; h[11] = 0x61;
    CHECK(ip_csum(h, 20) == 0);                   /* un en-tête valide somme à 0 */
}

/* ---------- construction DHCP ---------- */
static const uint8_t *find_option(const uint8_t *opts, const uint8_t *end, uint8_t code) {
    while (opts < end && *opts != 255) {
        if (*opts == 0) { opts++; continue; }
        if (opts + 2 > end || opts + 2 + opts[1] > end) return NULL;
        if (opts[0] == code) return opts;
        opts += 2 + opts[1];
    }
    return NULL;
}

static void test_build_dhcp(void) {
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t f[700];
    int n = dhcp_build(f, my_mac, 1, 0xDEADBEEFu, 0, 0);
    CHECK(n == 342);                              /* 14 + 20 + 8 + 300 */
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(!memcmp(f, bcast, 6) && !memcmp(f + 6, TEST_MAC, 6));
    CHECK(f[12] == 0x08 && f[13] == 0x00);
    const uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    CHECK(ip[0] == 0x45 && ip[9] == 17);
    CHECK((ip[2] << 8 | ip[3]) == 328);
    CHECK(ip_csum(ip, 20) == 0);                  /* checksum IP valide */
    CHECK((udp[0] << 8 | udp[1]) == 68 && (udp[2] << 8 | udp[3]) == 67);
    CHECK((udp[4] << 8 | udp[5]) == 308);
    CHECK(b[0] == 1 && b[1] == 1 && b[2] == 6);
    CHECK(be32(b + 4) == 0xDEADBEEFu);
    CHECK((b[10] << 8 | b[11]) == 0x8000);        /* broadcast demandé */
    CHECK(!memcmp(b + 28, TEST_MAC, 6) && !memcmp(b + 236, dhcp_cookie, 4));
    const uint8_t *o = find_option(b + 240, f + n, 53);
    CHECK(o && o[2] == 1);                        /* DISCOVER */
    CHECK(find_option(b + 240, f + n, 50) == NULL);

    n = dhcp_build(f, my_mac, 3, 1, ip4(10, 0, 0, 5), ip4(10, 0, 0, 1));
    b = f + 14 + 20 + 8;
    o = find_option(b + 240, f + n, 53);
    CHECK(o && o[2] == 3);                        /* REQUEST */
    o = find_option(b + 240, f + n, 50);
    CHECK(o && !memcmp(o + 2, (uint8_t[]){10, 0, 0, 5}, 4));
    o = find_option(b + 240, f + n, 54);
    CHECK(o && !memcmp(o + 2, (uint8_t[]){10, 0, 0, 1}, 4));
}

/* Fabrique une réponse serveur valide à partir d'une trame client. */
static int make_reply(uint8_t *f, uint32_t xid, uint8_t type, const uint8_t *opts, int optlen) {
    dhcp_build(f, my_mac, 1, xid, 0, 0);
    uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    putbe16(udp, 67); putbe16(udp + 2, 68);
    b[0] = 2;
    memcpy(b + 16, (uint8_t[]){10, 217, 123, 144}, 4);     /* yiaddr */
    memcpy(f + 6, (uint8_t[]){0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, 6);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    memcpy(o, opts, optlen); o += optlen;
    *o++ = 255;
    return o - f;
}

static void test_dhcp_parse(void) {
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t f[700];
    const uint8_t opts[] = {
        1, 4, 255, 255, 255, 0,          /* masque */
        3, 4, 10, 217, 123, 221,         /* routeur */
        6, 4, 10, 217, 123, 221,         /* DNS */
        51, 4, 0, 0, 0x0e, 0x0f,         /* bail 3599 s */
        54, 4, 10, 217, 123, 221,        /* serveur */
    };
    struct lease L;
    int n = make_reply(f, 0xCAFEF00Du, 2, opts, sizeof opts);
    CHECK(dhcp_parse(f, n, my_mac, 0xCAFEF00Du, &L) == 0);
    CHECK(L.type == 2);
    CHECK(L.ip == ip4(10, 217, 123, 144) && L.mask == ip4(255, 255, 255, 0));
    CHECK(L.gw == ip4(10, 217, 123, 221) && L.dns == ip4(10, 217, 123, 221));
    CHECK(L.srv == ip4(10, 217, 123, 221) && L.secs == 3599);
    CHECK(!memcmp(L.srvmac, (uint8_t[]){0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, 6));

    /* réponses à rejeter */
    CHECK(dhcp_parse(f, n, my_mac, 0x1111u, &L) < 0);                   /* mauvais xid */
    uint8_t g[700];
    memcpy(g, f, n); g[14 + 20 + 8 + 28] ^= 0xff;
    CHECK(dhcp_parse(g, n, my_mac, 0xCAFEF00Du, &L) < 0);               /* mauvaise MAC cliente */
    memcpy(g, f, n); g[14 + 20 + 8 + 236] ^= 0xff;
    CHECK(dhcp_parse(g, n, my_mac, 0xCAFEF00Du, &L) < 0);               /* mauvais dhcp_cookie */
    memcpy(g, f, n); g[14 + 20 + 3] = 99;
    CHECK(dhcp_parse(g, n, my_mac, 0xCAFEF00Du, &L) < 0);               /* mauvais port UDP */
    memcpy(g, f, n); g[14 + 20 + 8] = 1;
    CHECK(dhcp_parse(g, n, my_mac, 0xCAFEF00Du, &L) < 0);               /* op=requête, pas réponse */
    CHECK(dhcp_parse(f, 100, my_mac, 0xCAFEF00Du, &L) < 0);             /* trame tronquée */

    /* NAK */
    n = make_reply(f, 7, 6, (const uint8_t *)"", 0);
    CHECK(dhcp_parse(f, n, my_mac, 7, &L) == 0 && L.type == 6);

    /* option qui annonce une longueur dépassant la trame : ne doit rien lire au-delà */
    const uint8_t evil[] = {6, 200, 1, 2, 3};
    n = make_reply(f, 8, 2, evil, sizeof evil);
    CHECK(dhcp_parse(f, n, my_mac, 8, &L) == 0 && L.dns == 0);
}

/* ---------- parsing RNDIS reçu ---------- */
struct sink { int count; int lens[8]; uint8_t first[8]; };
static void collect(const uint8_t *fr, int len, void *arg) {
    struct sink *s = arg;
    if (s->count < 8) { s->lens[s->count] = len; s->first[s->count] = len ? fr[0] : 0; }
    s->count++;
    volatile uint8_t acc = 0;                     /* touche chaque octet : ASan détecte les débordements */
    for (int i = 0; i < len; i++) acc ^= fr[i];
    (void)acc;
}

static int add_packet(uint8_t *buf, int off, int datalen, uint8_t fill) {
    memset(buf + off, 0, RNDIS_HDR);
    put32(buf + off, RNDIS_PACKET_MSG);
    put32(buf + off + 4, RNDIS_HDR + datalen);
    put32(buf + off + 8, RNDIS_HDR - 8);
    put32(buf + off + 12, datalen);
    memset(buf + off + RNDIS_HDR, fill, datalen);
    return RNDIS_HDR + datalen;
}

static void test_rx_parse(void) {
    uint8_t *buf = calloc(1, 4096);
    struct sink s = {0};
    int n = add_packet(buf, 0, 60, 0xA0);
    n += add_packet(buf, n, 14, 0xB0);
    rx_parse(buf, n, collect, &s);
    CHECK(s.count == 2 && s.lens[0] == 60 && s.lens[1] == 14);
    CHECK(s.first[0] == 0xA0 && s.first[1] == 0xB0);

    /* message non-paquet (≥ 44 octets) au milieu : ignoré, la suite est lue */
    memset(&s, 0, sizeof s);
    n = add_packet(buf, 0, 20, 1);
    memset(buf + n, 0, 48);
    put32(buf + n, RNDIS_KEEPALIVE_MSG); put32(buf + n + 4, 48);
    n += 48;
    n += add_packet(buf, n, 30, 2);
    rx_parse(buf, n, collect, &s);
    CHECK(s.count == 2 && s.lens[0] == 20 && s.lens[1] == 30);

    /* longueur de message plus grande que le tampon : rien */
    memset(&s, 0, sizeof s);
    n = add_packet(buf, 0, 50, 3);
    rx_parse(buf, n - 10, collect, &s);
    CHECK(s.count == 0);

    /* longueur de message minuscule : arrêt, pas de boucle infinie */
    memset(&s, 0, sizeof s);
    n = add_packet(buf, 0, 10, 4);
    put32(buf + 4, 3);
    rx_parse(buf, n, collect, &s);
    CHECK(s.count == 0);

    /* data_length qui dépasse le message */
    memset(&s, 0, sizeof s);
    n = add_packet(buf, 0, 10, 5);
    put32(buf + 12, 5000);
    rx_parse(buf, n, collect, &s);
    CHECK(s.count == 0);

    /* débordement 32 bits : start + dlen revient « dans » le message */
    memset(&s, 0, sizeof s);
    n = add_packet(buf, 0, 10, 6);
    put32(buf + 12, 0xFFFFFF00u);
    rx_parse(buf, n, collect, &s);
    CHECK(s.count == 0);

    /* data_offset énorme */
    memset(&s, 0, sizeof s);
    n = add_packet(buf, 0, 10, 7);
    put32(buf + 8, 0xFFFFFFF0u);
    rx_parse(buf, n, collect, &s);
    CHECK(s.count == 0);
    free(buf);
}

/* ---------- MAC ---------- */
static void test_parse_mac(void) {
    uint8_t resp[64] = {0}, mac[6];
    put32(resp, RNDIS_QUERY_MSG | RNDIS_CMPLT);
    put32(resp + 16, 6);
    put32(resp + 20, 20);
    memcpy(resp + 8 + 20, TEST_MAC, 6);
    CHECK(rndis_parse_mac(resp, 40, mac) == 0 && !memcmp(mac, TEST_MAC, 6));
    CHECK(rndis_parse_mac(resp, 20, mac) < 0);                    /* réponse trop courte */
    put32(resp + 16, 3);
    CHECK(rndis_parse_mac(resp, 40, mac) < 0);                    /* longueur < 6 */
    put32(resp + 16, 6);
    put32(resp + 20, 100);
    CHECK(rndis_parse_mac(resp, 40, mac) < 0);                    /* offset hors de la réponse */
    put32(resp + 20, 0xFFFFFFF8u);
    CHECK(rndis_parse_mac(resp, 40, mac) < 0);                    /* débordement 32 bits */
}

/* ---------- fuzz : données hostiles, doit survivre sous ASan/UBSan ---------- */
static void test_fuzz(void) {
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t *buf = malloc(4096);                  /* taille exacte : ASan voit chaque octet hors limite */
    for (int iter = 0; iter < 200000; iter++) {
        int len = rnd() % 700;
        uint8_t *p = malloc(len ? len : 1);
        for (int i = 0; i < len; i++) p[i] = rnd();
        struct lease L;
        if (len >= 300 && (iter & 1)) {           /* préfixe presque valide pour aller plus loin */
            p[12] = 0x08; p[13] = 0; p[14] = 0x45; p[14 + 9] = 17;
            p[14 + 20 + 2] = 0; p[14 + 20 + 3] = 68;
            p[14 + 20 + 8] = 2;
            memcpy(p + 14 + 20 + 8 + 28, TEST_MAC, 6);
            memcpy(p + 14 + 20 + 8 + 236, dhcp_cookie, 4);
            uint32_t x = rnd(); putbe32(p + 14 + 20 + 8 + 4, x);
            dhcp_parse(p, len, my_mac, x, &L);
        } else {
            dhcp_parse(p, len, my_mac, rnd(), &L);
        }
        uint8_t mac[6];
        if (len >= 28) rndis_parse_mac(p, len, mac);
        struct sink s = {0};
        if (len >= RNDIS_HDR && (iter % 3)) {     /* en-têtes RNDIS plausibles, champs aléatoires */
            put32(p, RNDIS_PACKET_MSG);
            put32(p + 4, RNDIS_HDR + rnd() % 256);
        }
        rx_parse(p, len, collect, &s);
        free(p);
    }
    free(buf);
    CHECK(1);                                     /* arriver ici sans crash = succès */
}

/* rx_frame (Ethernet -> utun) : trames aléatoires, écriture redirigée vers /dev/null */
static void test_fuzz_rx_frame(void) {
    utun_fd = open("/dev/null", O_WRONLY);
    CHECK(utun_fd >= 0);
    my_ip = ip4(192, 0, 2, 1);                    /* évite toute réponse ARP vers un USB absent */
    gw_ip = ip4(192, 0, 2, 254);
    for (int iter = 0; iter < 200000; iter++) {
        int len = rnd() % 2200;
        uint8_t *p = malloc(len ? len : 1);
        for (int i = 0; i < len; i++) p[i] = rnd();
        if (len >= 34 && (iter & 1)) {            /* IPv4 plausible, longueur IP aléatoire */
            p[12] = 0x08; p[13] = 0x00;
            p[16] = rnd(); p[17] = rnd();
        }
        if (len >= 42 && (iter & 3) == 2) {       /* ARP : jamais pour notre IP */
            p[12] = 0x08; p[13] = 0x06;
            memcpy(p + 38, (uint8_t[]){198, 51, 100, 7}, 4);
        }
        if (len >= 14 + 44 && (iter % 5) == 1) {  /* IPv6 : ICMPv6 NS/RA plausibles, champs aléatoires */
            p[12] = 0x86; p[13] = 0xDD;
            p[14] = 0x60; p[14 + 6] = 58; p[14 + 7] = (iter & 8) ? 255 : rnd();
            p[14 + 40] = (iter & 16) ? 134 : 135; p[14 + 41] = 0;
            p[14 + 8] = (iter & 32) ? 0xfe : p[14 + 8]; p[14 + 9] = (iter & 32) ? 0x80 : p[14 + 9];
            if (iter & 64) { p[14 + 4] = 0; p[14 + 5] = (len - 14 - 40) & 0xff; }
            v6_claimed = 0;                       /* rejoue la prise en compte de RA à chaque fois */
        }
        rx_frame(p, len, NULL);
        free(p);
    }
    close(utun_fd);
    utun_fd = -1;
}

/* ---------- IPv6 ---------- */

/* checksum ICMPv6 par une construction différente (pseudo-en-tête explicite), pour recouper icmp6_csum */
static uint16_t ref_csum6(const uint8_t *src, const uint8_t *dst, const uint8_t *msg, int len) {
    uint8_t *b = calloc(1, 40 + len + 1);
    memcpy(b, src, 16); memcpy(b + 16, dst, 16);
    putbe32(b + 32, len); b[39] = 58;
    memcpy(b + 40, msg, len);
    uint32_t s = 0;
    int total = 40 + len + (len & 1);
    for (int i = 0; i < total; i += 2) s += b[i] << 8 | b[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    free(b);
    return ~s;
}

static void test_icmp6_csum(void) {
    uint8_t src[16], dst[16], msg[41];
    for (int iter = 0; iter < 2000; iter++) {
        for (int i = 0; i < 16; i++) { src[i] = rnd(); dst[i] = rnd(); }
        int len = 1 + rnd() % 40;                 /* inclut des longueurs impaires */
        for (int i = 0; i < len; i++) msg[i] = rnd();
        CHECK(icmp6_csum(src, dst, msg, len) == ref_csum6(src, dst, msg, len));
    }
}

static const uint8_t PHONE_MAC[6] = {0x7e, 0x02, 0x31, 0xd7, 0xbd, 0xf2};

/* Trame NS (sollicitation de voisin) venant du téléphone. */
static int make_ns(uint8_t *f, const uint8_t *src_ip, const uint8_t *target) {
    memset(f, 0, 14 + 40 + 32);
    memcpy(f, (uint8_t[]){0x33, 0x33, 0xff, 0, 0, 1}, 6); memcpy(f + 6, PHONE_MAC, 6); putbe16(f + 12, 0x86DD);
    uint8_t *ip = f + 14;
    ip[0] = 0x60; putbe16(ip + 4, 32); ip[6] = 58; ip[7] = 255;
    memcpy(ip + 8, src_ip, 16);
    memcpy(ip + 24, (uint8_t[]){0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, 0, 0, 1}, 16);
    uint8_t *m = ip + 40;
    m[0] = 135; memcpy(m + 8, target, 16);
    m[24] = 1; m[25] = 1; memcpy(m + 26, PHONE_MAC, 6);
    putbe16(m + 2, icmp6_csum(ip + 8, ip + 24, m, 32));
    return 14 + 40 + 32;
}

static void test_build_na(void) {
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t phone_ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x7c, 0x02, 0x31, 0xff, 0xfe, 0xd7, 0xbd, 0xf2};
    uint8_t target[16] = {0x26, 0, 0x03, 0x83, 0x0a, 0x44, 0x4d, 0x02, 0, 0, 0, 0, 0, 0, 0xab, 0xcd};
    uint8_t f[200], out[200];
    int n = make_ns(f, phone_ll, target);
    CHECK(build_na(out, f, n) == 86);
    CHECK(!memcmp(out, PHONE_MAC, 6) && !memcmp(out + 6, TEST_MAC, 6));   /* répond au téléphone, depuis notre MAC */
    CHECK(out[12] == 0x86 && out[13] == 0xDD);
    const uint8_t *ip = out + 14, *m = ip + 40;
    CHECK(ip[6] == 58 && ip[7] == 255 && (ip[4] << 8 | ip[5]) == 32);
    CHECK(!memcmp(ip + 8, target, 16) && !memcmp(ip + 24, phone_ll, 16)); /* source = la cible sollicitée */
    CHECK(m[0] == 136 && m[1] == 0 && m[4] == 0x60);                       /* NA, Solicited + Override */
    CHECK(!memcmp(m + 8, target, 16));
    CHECK(m[24] == 2 && m[25] == 1 && !memcmp(m + 26, TEST_MAC, 6));       /* option : notre MAC */
    CHECK(ref_csum6(ip + 8, ip + 24, m, 32) == 0);                         /* checksum valide */

    /* sollicitations à ne pas traiter */
    uint8_t g[200];
    memcpy(g, f, n); g[14 + 7] = 254;
    CHECK(build_na(out, g, n) < 0);                                        /* hop limit != 255 */
    memcpy(g, f, n); memset(g + 14 + 8, 0, 16);
    CHECK(build_na(out, g, n) < 0);                                        /* source :: (DAD du téléphone) */
    memcpy(g, f, n); g[14 + 40 + 1] = 1;
    CHECK(build_na(out, g, n) < 0);                                        /* code != 0 */
    memcpy(g, f, n); g[14 + 40] = 136;
    CHECK(build_na(out, g, n) < 0);                                        /* pas une NS */
    memcpy(g, f, n); g[14 + 40 + 8] = 0xff;
    CHECK(build_na(out, g, n) < 0);                                        /* cible multicast */
    memcpy(g, f, n); putbe16(g + 14 + 4, 500);
    CHECK(build_na(out, g, n) < 0);                                        /* longueur annoncée > trame */
    CHECK(build_na(out, f, 60) < 0);                                       /* trame tronquée */
}

static void test_build_rs(void) {
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t f[100];
    CHECK(build_rs(f) == 70);
    CHECK(!memcmp(f, (uint8_t[]){0x33, 0x33, 0, 0, 0, 2}, 6) && !memcmp(f + 6, TEST_MAC, 6));
    const uint8_t *ip = f + 14, *m = ip + 40;
    CHECK(ip[0] == 0x60 && ip[6] == 58 && ip[7] == 255 && (ip[4] << 8 | ip[5]) == 16);
    CHECK(ip[8] == 0xfe && ip[9] == 0x80 && ip[16] == (TEST_MAC[0] ^ 2) && ip[19] == 0xff && ip[20] == 0xfe);
    CHECK(ip[24] == 0xff && ip[25] == 0x02 && ip[39] == 2);                /* ff02::2 */
    CHECK(m[0] == 133 && m[8] == 1 && m[9] == 1 && !memcmp(m + 10, TEST_MAC, 6));
    CHECK(ref_csum6(ip + 8, ip + 24, m, 16) == 0);
}

/* Trame RA du téléphone ; opts/optlen = options ICMPv6 à la suite de l'en-tête RA (16 octets). */
static int make_ra(uint8_t *f, const uint8_t *opts, int optlen) {
    int plen = 16 + optlen;
    memset(f, 0, 14 + 40 + plen);
    memcpy(f, (uint8_t[]){0x33, 0x33, 0, 0, 0, 1}, 6); memcpy(f + 6, PHONE_MAC, 6); putbe16(f + 12, 0x86DD);
    uint8_t *ip = f + 14;
    ip[0] = 0x60; putbe16(ip + 4, plen); ip[6] = 58; ip[7] = 255;
    memcpy(ip + 8, (uint8_t[]){0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x7c, 0x02, 0x31, 0xff, 0xfe, 0xd7, 0xbd, 0xf2}, 16);
    memcpy(ip + 24, (uint8_t[]){0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 16);
    uint8_t *m = ip + 40;
    m[0] = 134; m[6] = 0x07; m[7] = 0x08;                                  /* durée de vie du routeur : 1800 s */
    memcpy(m + 16, opts, optlen);
    putbe16(m + 2, icmp6_csum(ip + 8, ip + 24, m, plen));
    return 14 + 40 + plen;
}

static void put_prefix_opt(uint8_t *o, uint8_t plen, uint8_t flags, uint32_t valid) {
    memset(o, 0, 32);
    o[0] = 3; o[1] = 4; o[2] = plen; o[3] = flags;
    putbe32(o + 4, valid); putbe32(o + 8, valid / 2);
    memcpy(o + 16, (uint8_t[]){0x26, 0, 0x03, 0x83, 0x0a, 0x44, 0x4d, 0x02}, 8);
}

static void test_parse_ra(void) {
    uint8_t f[300], opts[64];
    struct ra r;
    put_prefix_opt(opts, 64, 0xC0, 3600);                                  /* L + A */
    int n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && r.have_prefix && r.plen == 64 && r.router_lifetime == 1800);
    CHECK(!memcmp(r.prefix, (uint8_t[]){0x26, 0, 0x03, 0x83, 0x0a, 0x44, 0x4d, 0x02}, 8));
    CHECK(!memcmp(r.router, (uint8_t[]){0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x7c, 0x02, 0x31, 0xff, 0xfe, 0xd7, 0xbd, 0xf2}, 16));

    /* option SLLA (type 1) avant le préfixe : bien ignorée, préfixe trouvé quand même */
    uint8_t two[64] = {1, 1, 0x7e, 0x02, 0x31, 0xd7, 0xbd, 0xf2};
    put_prefix_opt(two + 8, 64, 0xC0, 3600);
    n = make_ra(f, two, 40);
    CHECK(parse_ra(f, n, &r) == 0 && r.have_prefix);

    put_prefix_opt(opts, 64, 0x80, 3600);                                  /* A absent : pas de SLAAC */
    n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && !r.have_prefix);
    put_prefix_opt(opts, 48, 0xC0, 3600);                                  /* pas un /64 */
    n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && !r.have_prefix);
    put_prefix_opt(opts, 64, 0xC0, 0);                                     /* durée de validité nulle */
    n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && !r.have_prefix);
    put_prefix_opt(opts, 64, 0xC0, 3600); opts[16] = 0xfe; opts[17] = 0x80; /* préfixe link-local */
    n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && !r.have_prefix);

    /* option de longueur 0 : doit s'arrêter (pas de boucle infinie) */
    memset(opts, 0, 32); opts[0] = 25;
    n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && !r.have_prefix);
    /* option qui dépasse la trame */
    put_prefix_opt(opts, 64, 0xC0, 3600); opts[1] = 20;
    n = make_ra(f, opts, 32);
    CHECK(parse_ra(f, n, &r) == 0 && !r.have_prefix);

    /* RA à rejeter */
    put_prefix_opt(opts, 64, 0xC0, 3600);
    n = make_ra(f, opts, 32);
    uint8_t g[300];
    memcpy(g, f, n); g[14 + 7] = 64;
    CHECK(parse_ra(g, n, &r) < 0);                                         /* hop limit */
    memcpy(g, f, n); g[14 + 8] = 0x26;
    CHECK(parse_ra(g, n, &r) < 0);                                         /* source non link-local */
    memcpy(g, f, n); g[14 + 40] = 135;
    CHECK(parse_ra(g, n, &r) < 0);                                         /* pas une RA */
    CHECK(parse_ra(f, 50, &r) < 0);                                        /* tronquée */
}

static void test_handle_ipv6(void) {
    utun_fd = open("/dev/null", O_WRONLY);
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t f[300], opts[64];
    put_prefix_opt(opts, 64, 0xC0, 3600);
    int n = make_ra(f, opts, 32);

    v6_claimed = v6_pending = 0;
    handle_ipv6(f, n);
    CHECK(v6_claimed == 1 && v6_pending == 1);                             /* adresse prête à configurer */
    CHECK(v6_router[0] == 0xfe && v6_router[1] == 0x80 && v6_router[15] == 0xf2);
    CHECK(!memcmp(v6_addr, (uint8_t[]){0x26, 0, 0x03, 0x83, 0x0a, 0x44, 0x4d, 0x02}, 8));
    CHECK(!(v6_addr[8] & 0x02));                                           /* identifiant « local », non EUI-64 */
    uint8_t first[16];
    memcpy(first, v6_addr, 16);
    v6_pending = 0;
    handle_ipv6(f, n);                                                     /* une seconde RA ne change rien */
    CHECK(v6_pending == 0 && !memcmp(first, v6_addr, 16));

    /* NS : traitée sans planter même sans périphérique USB (send_frame renvoie une erreur) */
    uint8_t ns[200], ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}, tgt[16] = {0x26, 0, 1};
    int m = make_ns(ns, ll, tgt);
    handle_ipv6(ns, m);

    /* paquet IPv6 ordinaire (UDP) : relayé vers utun, trames trop courtes ou tronquées jetées */
    uint8_t pkt[14 + 40 + 8] = {0};
    pkt[12] = 0x86; pkt[13] = 0xDD; pkt[14] = 0x60; pkt[14 + 5] = 8; pkt[14 + 6] = 17; pkt[14 + 7] = 64;
    handle_ipv6(pkt, sizeof pkt);
    handle_ipv6(pkt, 30);
    putbe16(pkt + 14 + 4, 60000);
    handle_ipv6(pkt, sizeof pkt);
    close(utun_fd);
    utun_fd = -1;
    CHECK(1);
}

/* Le client DHCP traite l'ACK d'un renouvellement reçu par rx_frame (sans relayer la trame). */
static void test_renew_ack(void) {
    memcpy(my_mac, TEST_MAC, 6);
    uint8_t f[700];
    struct lease first = {.ip = ip4(10, 217, 123, 144), .srv = ip4(10, 217, 123, 221), .secs = 3600};
    memset(&dhcp_c, 0, sizeof dhcp_c);
    dhcp_client_bound(&dhcp_c, &first, 1000);
    dhcp_active = 1;
    CHECK(dhcp_client_tick(&dhcp_c, 1000 + 1800, 0x1234ABCDu) == DHCP_SEND_RENEW);
    int n = make_reply(f, 0x1234ABCDu, 5, (const uint8_t[]){51, 4, 0, 0, 0x0e, 0x10}, 6);
    rx_frame(f, n, NULL);
    CHECK(dhcp_c.state == DHCP_BOUND && dhcp_c.xid == 0);                  /* ACK reconnu */

    dhcp_client_tick(&dhcp_c, now_s() + 1800, 0x1111u);                    /* nouveau T1 : retour en renouvellement */
    CHECK(dhcp_c.xid == 0x1111u);
    rx_frame(f, n, NULL);                                                  /* ACK d'un autre xid : ignoré */
    CHECK(dhcp_c.xid == 0x1111u);
    dhcp_active = 0;
}

int main(void) {
    test_byte_helpers();
    test_ip_csum();
    test_build_dhcp();
    test_dhcp_parse();
    test_rx_parse();
    test_parse_mac();
    test_fuzz();
    test_fuzz_rx_frame();
    test_icmp6_csum();
    test_build_na();
    test_build_rs();
    test_parse_ra();
    test_handle_ipv6();
    test_renew_ack();
    run_dhcp_tests();
    run_policy_tests();
    run_ipc_tests();
    run_supervisor_tests();
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
