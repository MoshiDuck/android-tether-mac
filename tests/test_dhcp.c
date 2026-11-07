// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Tests du module DHCP : trames de renouvellement conformes à la RFC 2131 et cycle de vie du bail
 * (T1, T2, retransmissions, expiration, NAK) avec une horloge simulée : aucune attente réelle.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/bytes.h"
#include "../src/dhcp.h"
#include "check.h"

static const uint8_t MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t SRV_MAC[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint8_t x[4] = {a, b, c, d};
    uint32_t v;
    memcpy(&v, x, 4);
    return v;
}

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

/* cherche l'option `code` dans la zone d'options de la trame ; NULL si absente */
static const uint8_t *opt(const uint8_t *f, int len, uint8_t code) {
    const uint8_t *o = f + 14 + 20 + 8 + 240, *end = f + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) return NULL;
        if (o[0] == code) return o;
        o += 2 + o[1];
    }
    return NULL;
}

/* ---------- trames de renouvellement ---------- */
static void test_renew_frames(void) {
    uint32_t me = ip4(10, 217, 123, 144), srv = ip4(10, 217, 123, 221);
    uint8_t f[700];

    int n = dhcp_build_renew(f, MAC, 0xCAFE0001u, me, SRV_MAC, srv, 1);        /* RENEWING : unicast */
    CHECK(!memcmp(f, SRV_MAC, 6) && !memcmp(f + 6, MAC, 6));
    CHECK(f[12] == 0x08 && f[13] == 0x00);
    const uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    CHECK(ip[0] == 0x45 && ip[9] == 17 && ip_csum(ip, 20) == 0);
    CHECK(!memcmp(ip + 12, &me, 4) && !memcmp(ip + 16, &srv, 4));              /* de nous, vers le serveur */
    CHECK((udp[0] << 8 | udp[1]) == 68 && (udp[2] << 8 | udp[3]) == 67);
    CHECK(b[0] == 1 && be32(b + 4) == 0xCAFE0001u);
    CHECK((b[10] << 8 | b[11]) == 0);                                          /* pas de drapeau broadcast */
    CHECK(!memcmp(b + 12, &me, 4));                                            /* ciaddr renseigné */
    CHECK(!memcmp(b + 28, MAC, 6) && !memcmp(b + 236, dhcp_cookie, 4));
    const uint8_t *t = opt(f, n, 53);
    CHECK(t && t[2] == 3);                                                     /* DHCPREQUEST */
    CHECK(opt(f, n, 50) == NULL && opt(f, n, 54) == NULL);                     /* RFC 2131 §4.3.2 : interdits ici */
    const uint8_t *pr = opt(f, n, 55);
    CHECK(pr && memchr(pr + 2, 58, pr[1]) && memchr(pr + 2, 59, pr[1]));       /* on demande T1 et T2 */

    n = dhcp_build_renew(f, MAC, 0xCAFE0002u, me, SRV_MAC, srv, 0);            /* REBINDING : broadcast */
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    ip = f + 14;
    CHECK(!memcmp(f, bcast, 6) && !memcmp(f + 6, MAC, 6));
    CHECK(!memcmp(ip + 12, &me, 4) && !memcmp(ip + 16, bcast, 4));             /* de nous, vers 255.255.255.255 */
    CHECK(ip_csum(ip, 20) == 0 && !memcmp(f + 14 + 20 + 8 + 12, &me, 4));
    CHECK(opt(f, n, 50) == NULL && opt(f, n, 54) == NULL);
}

/* ---------- DHCPRELEASE ---------- */
static void test_release_frame(void) {
    uint32_t me = ip4(10, 217, 123, 144), srv = ip4(10, 217, 123, 221);
    uint8_t f[700];
    int n = dhcp_build_release(f, MAC, 0xBEEF0001u, me, SRV_MAC, srv);
    CHECK(!memcmp(f, SRV_MAC, 6) && !memcmp(f + 6, MAC, 6));                   /* unicast vers le serveur */
    const uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    CHECK(ip[0] == 0x45 && ip[9] == 17 && ip_csum(ip, 20) == 0);
    CHECK(!memcmp(ip + 12, &me, 4) && !memcmp(ip + 16, &srv, 4));
    CHECK((udp[0] << 8 | udp[1]) == 68 && (udp[2] << 8 | udp[3]) == 67);
    CHECK(b[0] == 1 && be32(b + 4) == 0xBEEF0001u);
    CHECK(!memcmp(b + 12, &me, 4));                                            /* ciaddr = l'adresse rendue */
    CHECK(!memcmp(b + 28, MAC, 6) && !memcmp(b + 236, dhcp_cookie, 4));
    const uint8_t *t = opt(f, n, 53);
    CHECK(t && t[2] == 7);                                                     /* DHCPRELEASE */
    const uint8_t *s = opt(f, n, 54);
    CHECK(s && s[1] == 4 && !memcmp(s + 2, &srv, 4));                          /* identifiant du serveur */
    CHECK(opt(f, n, 50) == NULL && opt(f, n, 55) == NULL);                     /* ni adresse demandée ni liste de paramètres */
    CHECK(n >= 14 + 20 + 8 + 300);
}

/* ---------- options T1 / T2 ---------- */
static int make_reply(uint8_t *f, uint8_t type, uint32_t xid, const uint8_t *opts, int optlen) {
    dhcp_build(f, MAC, 1, xid, 0, 0);
    uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    putbe16(udp, 67); putbe16(udp + 2, 68);
    b[0] = 2;
    memcpy(b + 16, (uint8_t[]){10, 217, 123, 144}, 4);
    memcpy(f + 6, SRV_MAC, 6);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    memcpy(o, opts, optlen); o += optlen;
    *o++ = 255;
    return o - f;
}

static void test_parse_t1_t2(void) {
    uint8_t f[700];
    const uint8_t opts[] = {51, 4, 0, 0, 0x0e, 0x10,      /* 3600 s */
                            58, 4, 0, 0, 0x03, 0xe8,      /* T1 = 1000 s */
                            59, 4, 0, 0, 0x07, 0xd0};     /* T2 = 2000 s */
    struct lease L;
    int n = make_reply(f, 5, 77, opts, sizeof opts);
    CHECK(dhcp_parse(f, n, MAC, 77, &L) == 0);
    CHECK(L.type == 5 && L.secs == 3600 && L.t1 == 1000 && L.t2 == 2000);
    n = make_reply(f, 5, 78, opts, 6);                    /* sans T1/T2 */
    CHECK(dhcp_parse(f, n, MAC, 78, &L) == 0 && L.t1 == 0 && L.t2 == 0);
}

/* ---------- cycle de vie du bail ---------- */
static struct lease mklease(uint32_t secs, uint32_t t1, uint32_t t2) {
    struct lease L;
    memset(&L, 0, sizeof L);
    L.ip = ip4(10, 217, 123, 144); L.srv = ip4(10, 217, 123, 221);
    memcpy(L.srvmac, SRV_MAC, 6);
    L.secs = secs; L.t1 = t1; L.t2 = t2; L.type = 5;
    return L;
}

static void test_timers(void) {
    struct dhcp_client c;
    memset(&c, 0, sizeof c);
    struct lease L = mklease(3600, 0, 0);
    dhcp_client_bound(&c, &L, 100);
    CHECK(c.state == DHCP_BOUND && c.t1 == 1800 && c.t2 == 3150 && c.lease == 3600 && c.bound_at == 100);

    L = mklease(3600, 1000, 2000);                         /* T1/T2 du serveur honorés */
    dhcp_client_bound(&c, &L, 0);
    CHECK(c.t1 == 1000 && c.t2 == 2000);

    L = mklease(3600, 2000, 1000);                         /* T1 >= T2 : valeurs par défaut */
    dhcp_client_bound(&c, &L, 0);
    CHECK(c.t1 == 1800 && c.t2 == 3150);
    L = mklease(3600, 0, 3000);                            /* T1 absent */
    dhcp_client_bound(&c, &L, 0);
    CHECK(c.t1 == 1800 && c.t2 == 3150);
    L = mklease(3600, 1000, 3600);                         /* T2 = bail */
    dhcp_client_bound(&c, &L, 0);
    CHECK(c.t1 == 1800 && c.t2 == 3150);

    L = mklease(10, 0, 0);                                 /* bail minuscule : plancher à 60 s */
    dhcp_client_bound(&c, &L, 0);
    CHECK(c.lease == 60 && c.t1 == 30 && c.t2 == 52);

    L = mklease(DHCP_INFINITE, 0, 0);                      /* bail infini : jamais de renouvellement */
    dhcp_client_bound(&c, &L, 0);
    CHECK(dhcp_client_tick(&c, 10000000, 1) == DHCP_NOTHING && c.state == DHCP_BOUND);
}

static void test_renew_cycle(void) {
    struct dhcp_client c;
    memset(&c, 0, sizeof c);
    struct lease L = mklease(3600, 0, 0);
    dhcp_client_bound(&c, &L, 1000);

    CHECK(dhcp_client_tick(&c, 1000 + 1799, 11) == DHCP_NOTHING);              /* avant T1 */
    CHECK(dhcp_client_tick(&c, 1000 + 1800, 11) == DHCP_SEND_RENEW);           /* T1 */
    CHECK(c.state == DHCP_RENEWING && c.xid == 11);
    CHECK(dhcp_client_tick(&c, 1000 + 1800, 12) == DHCP_NOTHING);              /* pas de double envoi */

    struct lease ack = mklease(3600, 0, 0);
    CHECK(dhcp_client_input(&c, &ack, 1000 + 1801) == DHCP_ACKED);
    CHECK(c.state == DHCP_BOUND && c.bound_at == 2801 && c.xid == 0);          /* bail relancé à partir de l'ACK */
    CHECK(dhcp_client_tick(&c, 2801 + 1799, 13) == DHCP_NOTHING);
    CHECK(dhcp_client_tick(&c, 2801 + 1800, 13) == DHCP_SEND_RENEW);           /* prochain T1 */
}

static void test_retransmissions(void) {
    struct dhcp_client c;
    memset(&c, 0, sizeof c);
    struct lease L = mklease(3600, 0, 0);                  /* T1 = 1800, T2 = 3150 */
    dhcp_client_bound(&c, &L, 0);

    CHECK(dhcp_client_tick(&c, 1800, 21) == DHCP_SEND_RENEW);
    /* RENEWING : on réessaie à la moitié du temps restant jusqu'à T2 : (3150-1800)/2 = 675 s */
    CHECK(dhcp_client_tick(&c, 1800 + 674, 22) == DHCP_NOTHING);
    CHECK(dhcp_client_tick(&c, 1800 + 675, 22) == DHCP_SEND_RENEW);
    CHECK(c.xid == 21);                                    /* même transaction pour les retransmissions */
    CHECK(dhcp_client_tick(&c, 2475 + 337, 23) == DHCP_SEND_RENEW);            /* moitié de (3150-2475) */

    CHECK(dhcp_client_tick(&c, 3150, 31) == DHCP_SEND_REBIND);                 /* T2 : broadcast */
    CHECK(c.state == DHCP_REBINDING && c.xid == 31);
    CHECK(dhcp_client_tick(&c, 3150, 32) == DHCP_NOTHING);
    CHECK(dhcp_client_tick(&c, 3150 + 224, 32) == DHCP_NOTHING);               /* moitié de (3600-3150) = 225 */
    CHECK(dhcp_client_tick(&c, 3150 + 225, 32) == DHCP_SEND_REBIND);

    struct lease ack = mklease(3600, 0, 0);
    CHECK(dhcp_client_input(&c, &ack, 3400) == DHCP_ACKED);                    /* l'ACK arrive en REBINDING */
    CHECK(c.state == DHCP_BOUND && c.bound_at == 3400);
}

static void test_expiry_and_errors(void) {
    struct dhcp_client c;
    memset(&c, 0, sizeof c);
    struct lease L = mklease(3600, 0, 0);

    dhcp_client_bound(&c, &L, 0);                          /* aucune réponse jusqu'à la fin du bail */
    dhcp_client_tick(&c, 1800, 1);
    dhcp_client_tick(&c, 3150, 2);
    CHECK(dhcp_client_tick(&c, 3599, 3) != DHCP_RESTART);
    CHECK(dhcp_client_tick(&c, 3600, 3) == DHCP_RESTART && c.state == DHCP_EXPIRED);
    CHECK(dhcp_client_tick(&c, 4000, 4) == DHCP_RESTART);                      /* reste en échec */

    dhcp_client_bound(&c, &L, 0);                          /* réveil du Mac : bail déjà expiré */
    CHECK(dhcp_client_tick(&c, 5000, 5) == DHCP_RESTART);

    dhcp_client_bound(&c, &L, 10000);                      /* horloge qui recule : rien ne se passe */
    CHECK(dhcp_client_tick(&c, 9000, 6) == DHCP_NOTHING);

    dhcp_client_bound(&c, &L, 0);                          /* NAK pendant le renouvellement */
    dhcp_client_tick(&c, 1800, 7);
    struct lease nak = mklease(0, 0, 0);
    nak.type = 6;
    CHECK(dhcp_client_input(&c, &nak, 1801) == DHCP_NAKED && c.state == DHCP_EXPIRED);
    CHECK(dhcp_client_tick(&c, 1802, 8) == DHCP_RESTART);                      /* ne repasse pas en REBINDING */

    dhcp_client_bound(&c, &L, 0);                          /* le serveur nous attribue une autre adresse */
    dhcp_client_tick(&c, 1800, 9);
    struct lease other = mklease(3600, 0, 0);
    other.ip = ip4(10, 217, 123, 77);
    CHECK(dhcp_client_input(&c, &other, 1801) == DHCP_ADDR_CHANGED && c.state == DHCP_EXPIRED);

    dhcp_client_bound(&c, &L, 0);                          /* ACK hors renouvellement : ignoré */
    struct lease ack = mklease(3600, 0, 0);
    CHECK(dhcp_client_input(&c, &ack, 10) == DHCP_IGNORED && c.state == DHCP_BOUND);

    dhcp_client_tick(&c, 1800, 10);                        /* ACK sans option 51 : durée précédente conservée */
    struct lease bare = mklease(0, 0, 0);
    CHECK(dhcp_client_input(&c, &bare, 1801) == DHCP_ACKED && c.lease == 3600);
}

/* Séquences aléatoires : les invariants doivent toujours tenir, quoi que dise le serveur. */
static void test_state_machine_fuzz(void) {
    for (int iter = 0; iter < 20000; iter++) {
        struct dhcp_client c;
        memset(&c, 0, sizeof c);
        uint32_t secs = rnd() % 5 == 0 ? rnd() : rnd() % 100000;
        struct lease L = mklease(secs, rnd() % 4 ? rnd() % 50000 : 0, rnd() % 4 ? rnd() % 100000 : 0);
        int64_t now = rnd() % 1000;
        dhcp_client_bound(&c, &L, now);
        CHECK(c.lease == DHCP_INFINITE || c.lease >= 60);
        CHECK(c.lease == DHCP_INFINITE || (c.t1 > 0 && c.t1 < c.t2 && c.t2 < c.lease));
        int saw_rebind = 0, restarted = 0;
        for (int step = 0; step < 40; step++) {
            now += rnd() % 4000;
            enum dhcp_action a = dhcp_client_tick(&c, now, rnd() | 1);
            if (a == DHCP_SEND_REBIND) saw_rebind = 1;
            if (a == DHCP_SEND_RENEW) CHECK(!saw_rebind || c.state == DHCP_RENEWING); /* jamais de retour en arrière sans ACK */
            if (a == DHCP_RESTART) restarted = 1;
            if (restarted) CHECK(c.state == DHCP_EXPIRED);
            if ((a == DHCP_SEND_RENEW || a == DHCP_SEND_REBIND) && c.lease != DHCP_INFINITE)
                CHECK(c.next_tx > now);                                              /* pas d'envoi en rafale */
            if (rnd() % 3 == 0) {
                struct lease r = mklease(rnd() % 7000, 0, 0);
                r.type = rnd() % 3 == 0 ? 6 : 5;
                if (rnd() % 5 == 0) r.ip = rnd();
                enum dhcp_input in = dhcp_client_input(&c, &r, now);
                if (in == DHCP_ACKED) { CHECK(c.state == DHCP_BOUND); saw_rebind = restarted = 0; }
                else if (in == DHCP_NAKED || in == DHCP_ADDR_CHANGED) CHECK(c.state == DHCP_EXPIRED);
            }
        }
    }
}

void run_dhcp_tests(void) {
    test_renew_frames();
    test_release_frame();
    test_parse_t1_t2();
    test_timers();
    test_renew_cycle();
    test_retransmissions();
    test_expiry_and_errors();
    test_state_machine_fuzz();
}
