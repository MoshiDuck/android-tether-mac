// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Tests unitaires et de robustesse du driver, sans téléphone ni root.
 * On inclut main.c directement pour atteindre ses fonctions static ; main() est renommé.
 * Les données venant du téléphone sont considérées comme hostiles : les tests de
 * « fuzz » tournent sous AddressSanitizer/UBSan (voir le Makefile).
 */
#define main rndis_tether_main
#include "../main.c"
#undef main
#include <fcntl.h>

static int checks, failures;
#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

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
    int n = build_dhcp(f, 1, 0xDEADBEEFu, 0, 0);
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
    CHECK(!memcmp(b + 28, TEST_MAC, 6) && !memcmp(b + 236, cookie, 4));
    const uint8_t *o = find_option(b + 240, f + n, 53);
    CHECK(o && o[2] == 1);                        /* DISCOVER */
    CHECK(find_option(b + 240, f + n, 50) == NULL);

    n = build_dhcp(f, 3, 1, ip4(10, 0, 0, 5), ip4(10, 0, 0, 1));
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
    build_dhcp(f, 1, xid, 0, 0);
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
    CHECK(dhcp_parse(f, n, 0xCAFEF00Du, &L) == 0);
    CHECK(L.type == 2);
    CHECK(L.ip == ip4(10, 217, 123, 144) && L.mask == ip4(255, 255, 255, 0));
    CHECK(L.gw == ip4(10, 217, 123, 221) && L.dns == ip4(10, 217, 123, 221));
    CHECK(L.srv == ip4(10, 217, 123, 221) && L.secs == 3599);
    CHECK(!memcmp(L.srvmac, (uint8_t[]){0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, 6));

    /* réponses à rejeter */
    CHECK(dhcp_parse(f, n, 0x1111u, &L) < 0);                   /* mauvais xid */
    uint8_t g[700];
    memcpy(g, f, n); g[14 + 20 + 8 + 28] ^= 0xff;
    CHECK(dhcp_parse(g, n, 0xCAFEF00Du, &L) < 0);               /* mauvaise MAC cliente */
    memcpy(g, f, n); g[14 + 20 + 8 + 236] ^= 0xff;
    CHECK(dhcp_parse(g, n, 0xCAFEF00Du, &L) < 0);               /* mauvais cookie */
    memcpy(g, f, n); g[14 + 20 + 3] = 99;
    CHECK(dhcp_parse(g, n, 0xCAFEF00Du, &L) < 0);               /* mauvais port UDP */
    memcpy(g, f, n); g[14 + 20 + 8] = 1;
    CHECK(dhcp_parse(g, n, 0xCAFEF00Du, &L) < 0);               /* op=requête, pas réponse */
    CHECK(dhcp_parse(f, 100, 0xCAFEF00Du, &L) < 0);             /* trame tronquée */

    /* NAK */
    n = make_reply(f, 7, 6, (const uint8_t *)"", 0);
    CHECK(dhcp_parse(f, n, 7, &L) == 0 && L.type == 6);

    /* option qui annonce une longueur dépassant la trame : ne doit rien lire au-delà */
    const uint8_t evil[] = {6, 200, 1, 2, 3};
    n = make_reply(f, 8, 2, evil, sizeof evil);
    CHECK(dhcp_parse(f, n, 8, &L) == 0 && L.dns == 0);
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
            memcpy(p + 14 + 20 + 8 + 236, cookie, 4);
            uint32_t x = rnd(); putbe32(p + 14 + 20 + 8 + 4, x);
            dhcp_parse(p, len, x, &L);
        } else {
            dhcp_parse(p, len, rnd(), &L);
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
        rx_frame(p, len, NULL);
        free(p);
    }
    close(utun_fd);
    utun_fd = -1;
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
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
