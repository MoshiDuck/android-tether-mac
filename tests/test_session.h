/*
 * Test d'intégration d'une session complète, sans téléphone ni root (inclus par test_main.c).
 *
 * Un faux téléphone répond aux commandes RNDIS, au DHCP (avec un MTU de 1400), à la sollicitation de routeur IPv6
 * (préfixe et MTU de 1350) et à un ping ; l'interface utun est une paire de sockets. Le vrai code du worker
 * (session, threads, DHCP, IPv6, requêtes au superviseur) tourne dans un processus forké, et le vrai superviseur
 * (run_worker_once, handle_request) dans le processus de test, avec ifconfig/route/scutil enregistrés au lieu d'être
 * exécutés. On vérifie donc, de bout en bout, ce qui serait configuré, ce qui circule et ce qui est nettoyé.
 */
static const uint8_t PH_MAC_S[6] = {0x7e, 0x02, 0x31, 0xd7, 0xbd, 0xf2};
static const uint8_t PH_IP4[4] = {10, 217, 123, 221};
static const uint8_t LEASE_IP4[4] = {10, 217, 123, 154};

static struct {
    pthread_mutex_t lk;
    uint8_t resp[512];
    int resp_len;
    uint8_t q[16][2300];
    int qlen[16], qn;
    int64_t t0;
    int unplug_after_s;                                              /* plafond : le téléphone est débranché au plus tard à ce moment */
    int64_t echo_at;                                                 /* instant du ping répondu : débranché 1 s plus tard */
    int echoes;
} ph = {.lk = PTHREAD_MUTEX_INITIALIZER, .unplug_after_s = 20};

static int ph_unplugged(void) {
    int64_t now = now_s();
    return now - ph.t0 >= ph.unplug_after_s || (ph.echo_at && now - ph.echo_at >= 1);
}

/* met une trame Ethernet dans la file de réception (enveloppe RNDIS PACKET_MSG) */
static void ph_deliver(const uint8_t *frame, int len) {
    pthread_mutex_lock(&ph.lk);
    if (ph.qn < 16) {
        uint8_t *b = ph.q[ph.qn];
        memset(b, 0, RNDIS_HDR);
        put32(b, RNDIS_PACKET_MSG); put32(b + 4, RNDIS_HDR + len);
        put32(b + 8, RNDIS_HDR - 8); put32(b + 12, len);
        memcpy(b + RNDIS_HDR, frame, len);
        ph.qlen[ph.qn++] = RNDIS_HDR + len;
    }
    pthread_mutex_unlock(&ph.lk);
}

int fake_usb_control(libusb_device_handle *hd, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, unsigned char *buf, uint16_t len, unsigned to) {
    (void)hd; (void)req; (void)val; (void)idx; (void)to;
    if (ph_unplugged()) return LIBUSB_ERROR_NO_DEVICE;
    pthread_mutex_lock(&ph.lk);
    int r = 0;
    if (type == 0x21) {                                              /* SEND_ENCAPSULATED : une commande RNDIS */
        uint32_t mt = get32(buf), id = get32(buf + 8);
        uint8_t *o = ph.resp;
        memset(o, 0, 64);
        ph.resp_len = 0;
        if (mt == RNDIS_INIT_MSG) {
            put32(o, RNDIS_INIT_MSG | RNDIS_CMPLT); put32(o + 4, 52); put32(o + 8, id); put32(o + 36, 0x4000);
            ph.resp_len = 52;
        } else if (mt == RNDIS_QUERY_MSG) {
            put32(o, RNDIS_QUERY_MSG | RNDIS_CMPLT); put32(o + 4, 30); put32(o + 8, id);
            put32(o + 16, 6); put32(o + 20, 16);                     /* 6 octets, 16 octets après le champ de décalage */
            memcpy(o + 24, (uint8_t[]){0xf2, 0x08, 0x36, 0x81, 0x42, 0xa7}, 6);
            ph.resp_len = 30;
        } else if (mt == RNDIS_SET_MSG || mt == RNDIS_KEEPALIVE_MSG) {
            put32(o, mt | RNDIS_CMPLT); put32(o + 4, 16); put32(o + 8, id);
            ph.resp_len = 16;
        }
        r = len;
    } else if (type == 0xA1) {                                       /* GET_ENCAPSULATED : la réponse en attente */
        if (ph.resp_len) {
            r = ph.resp_len < len ? ph.resp_len : len;
            memcpy(buf, ph.resp, r);
            ph.resp_len = 0;
        } else {
            r = LIBUSB_ERROR_PIPE;
        }
    }
    pthread_mutex_unlock(&ph.lk);
    return r;
}

static uint16_t csum16(const uint8_t *p, int n) { return ip_csum(p, n); }

/* Trame DHCP du serveur (OFFER ou ACK) pour la demande `rq` (trame Ethernet complète). */
static void ph_dhcp_reply(const uint8_t *rq, uint8_t type) {
    uint8_t f[600];
    const uint8_t *b = rq + 14 + 20 + 8;
    memset(f, 0, sizeof f);
    memcpy(f, b + 28, 6); memcpy(f + 6, PH_MAC_S, 6); putbe16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20, *o = udp + 8;
    ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
    memcpy(ip + 12, PH_IP4, 4); memset(ip + 16, 0xff, 4);
    putbe16(udp, 67); putbe16(udp + 2, 68);
    o[0] = 2; o[1] = 1; o[2] = 6;
    memcpy(o + 4, b + 4, 4);                                         /* xid */
    memcpy(o + 16, LEASE_IP4, 4);
    memcpy(o + 28, b + 28, 6);
    memcpy(o + 236, dhcp_cookie, 4);
    uint8_t *p = o + 240;
    *p++ = 53; *p++ = 1; *p++ = type;
    *p++ = 54; *p++ = 4; memcpy(p, PH_IP4, 4); p += 4;
    *p++ = 1; *p++ = 4; *p++ = 255; *p++ = 255; *p++ = 255; *p++ = 0;
    *p++ = 3; *p++ = 4; memcpy(p, PH_IP4, 4); p += 4;
    *p++ = 6; *p++ = 4; memcpy(p, PH_IP4, 4); p += 4;
    *p++ = 26; *p++ = 2; *p++ = 1400 >> 8; *p++ = 1400 & 0xff;
    *p++ = 51; *p++ = 4; *p++ = 0; *p++ = 0; *p++ = 0x0e; *p++ = 0x10;           /* 3600 s */
    *p++ = 255;
    int bootp = (int)(p - o) < 300 ? 300 : (int)(p - o);
    int total = 14 + 20 + 8 + bootp;
    putbe16(ip + 2, 20 + 8 + bootp); putbe16(udp + 4, 8 + bootp);
    putbe16(ip + 10, csum16(ip, 20));
    ph_deliver(f, total);
}

/* Réponse à un ping : mêmes adresses échangées, type ICMP 0. */
static void ph_echo_reply(const uint8_t *rq, int len) {
    uint8_t f[300];
    if (len > (int)sizeof f) return;
    memcpy(f, rq, len);
    memcpy(f, rq + 6, 6); memcpy(f + 6, PH_MAC_S, 6);
    uint8_t *ip = f + 14;
    memcpy(ip + 12, rq + 14 + 16, 4); memcpy(ip + 16, rq + 14 + 12, 4);
    int ihl = (ip[0] & 15) * 4;
    int iplen = ip[2] << 8 | ip[3];
    uint8_t *icmp = ip + ihl;
    icmp[0] = 0; putbe16(icmp + 2, 0);
    putbe16(icmp + 2, csum16(icmp, iplen - ihl));
    putbe16(ip + 10, 0);
    putbe16(ip + 10, csum16(ip, ihl));
    ph_deliver(f, len);
    pthread_mutex_lock(&ph.lk);
    ph.echoes++;
    ph.echo_at = now_s();
    pthread_mutex_unlock(&ph.lk);
}

static void ph_handle_frame(const uint8_t *f, int len) {
    if (len < 14) return;
    uint16_t et = f[12] << 8 | f[13];
    if (et == 0x0800 && len >= 14 + 20 + 8 + 240 && f[14 + 9] == 17 && (f[14 + 20 + 2] << 8 | f[14 + 20 + 3]) == 67) {
        const uint8_t *b = f + 14 + 20 + 8;
        const uint8_t *o = b + 240, *end = f + len;
        uint8_t t = 0;
        while (o + 2 <= end && *o != 255) {
            if (*o == 0) { o++; continue; }
            if (o[0] == 53 && o[1] >= 1) t = o[2];
            o += 2 + o[1];
        }
        if (t == 1) ph_dhcp_reply(f, 2);                             /* DISCOVER -> OFFER */
        else if (t == 3) ph_dhcp_reply(f, 5);                        /* REQUEST -> ACK */
    } else if (et == 0x0800 && len >= 14 + 20 + 8 && f[14 + 9] == 1 && f[14 + 20] == 8) {
        ph_echo_reply(f, len);                                       /* ping */
    } else if (et == 0x86DD && len >= 14 + 40 + 8 && f[14 + 6] == 58 && f[14 + 40] == 133) {
        uint8_t ra[300], opts[64];                                   /* sollicitation de routeur -> annonce avec préfixe et MTU */
        put_prefix_opt(opts, 64, 0xC0, 3600);
        put_mtu_opt(opts + 32, 1350);
        int n = make_ra(ra, opts, 40);
        ph_deliver(ra, n);
    }
}

int fake_usb_bulk(libusb_device_handle *hd, unsigned char ep, unsigned char *buf, int len, int *got, unsigned to) {
    (void)hd;
    *got = 0;
    if (ph_unplugged()) return LIBUSB_ERROR_NO_DEVICE;
    if (!(ep & 0x80)) {                                              /* envoi du Mac vers le téléphone */
        if (len >= RNDIS_HDR && get32(buf) == RNDIS_PACKET_MSG)
            ph_handle_frame(buf + 8 + get32(buf + 8), (int)get32(buf + 12));
        *got = len;
        return 0;
    }
    for (unsigned waited = 0; waited < to && waited < 200; waited += 5) {      /* réception : attend une trame, au plus 200 ms */
        pthread_mutex_lock(&ph.lk);
        if (ph.qn > 0) {
            int n = ph.qlen[0] < len ? ph.qlen[0] : len;
            memcpy(buf, ph.q[0], n);
            memmove(ph.q[0], ph.q[1], (size_t)(ph.qn - 1) * sizeof ph.q[0]);
            memmove(&ph.qlen[0], &ph.qlen[1], (size_t)(ph.qn - 1) * sizeof ph.qlen[0]);
            ph.qn--;
            pthread_mutex_unlock(&ph.lk);
            *got = n;
            return 0;
        }
        pthread_mutex_unlock(&ph.lk);
        usleep(5000);
        if (ph_unplugged()) return LIBUSB_ERROR_NO_DEVICE;
    }
    return LIBUSB_ERROR_TIMEOUT;
}

int fake_usb_interrupt(libusb_device_handle *hd, unsigned char ep, unsigned char *buf, int len, int *got, unsigned to) {
    (void)hd; (void)ep; (void)buf; (void)len; (void)to;
    *got = 0;
    usleep(100000);
    return ph_unplugged() ? LIBUSB_ERROR_NO_DEVICE : LIBUSB_ERROR_TIMEOUT;
}

/* Le « worker » forké : la vraie session, sur le faux téléphone. */
static int fake_session_worker(int sock) {
    worker_mode = 1;
    ipc_sock = sock;
    test_fake_usb = 1;
    ph.t0 = now_s();
    h = (libusb_device_handle *)(uintptr_t)1;
    ctrl_if = 0; data_if = 1; ep_in = 0x81; ep_out = 0x02; ep_int = 0x83; mps_out = 512;
    return session();
}

/* Côté « système » : une fois l'IPv4 configuré, envoie un ping par l'interface utun et lit la réponse. */
static struct { uint8_t reply[200]; int len; } ping_result;

static void *ping_helper(void *a) {
    (void)a;
    for (int k = 0; k < 1500 && !(cfg4_done && test_utun_peer >= 0); k++) usleep(10000);
    usleep(1200000);                                                 /* laisse démarrer les threads et l'annonce IPv6 */
    uint8_t p[4 + 20 + 8 + 4];
    memset(p, 0, sizeof p);
    putbe32(p, AF_INET);
    uint8_t *ip = p + 4;
    ip[0] = 0x45; putbe16(ip + 2, 20 + 8 + 4); ip[8] = 64; ip[9] = 1;
    memcpy(ip + 12, LEASE_IP4, 4); memcpy(ip + 16, (uint8_t[]){8, 8, 8, 8}, 4);
    putbe16(ip + 10, csum16(ip, 20));
    uint8_t *icmp = ip + 20;
    icmp[0] = 8; putbe16(icmp + 4, 0x1234); putbe16(icmp + 6, 1);
    memcpy(icmp + 8, "ping", 4);
    putbe16(icmp + 2, csum16(icmp, 12));
    if (test_utun_peer < 0 || write(test_utun_peer, p, sizeof p) != (ssize_t)sizeof p) return NULL;
    struct pollfd pf = {.fd = test_utun_peer, .events = POLLIN};
    if (poll(&pf, 1, 5000) > 0) ping_result.len = (int)read(test_utun_peer, ping_result.reply, sizeof ping_result.reply);
    return NULL;
}

static void test_full_session(void) {
    sup_reset();
    memset(&ping_result, 0, sizeof ping_result);
    ph.qn = 0; ph.resp_len = 0; ph.echoes = 0; ph.echo_at = 0;
    test_worker_entry = fake_session_worker;
    pthread_t th;
    pthread_create(&th, NULL, ping_helper, NULL);
    int rc;
    char *log = run_capturing_stderr(&rc);
    pthread_join(th, NULL);
    test_worker_entry = NULL;

    CHECK(rc == 0);
    CHECK(strstr(log, "MAC hôte: f2:08:36:81:42:a7") != NULL);                             /* RNDIS initialisé, adresse lue */
    CHECK(strstr(log, "utun99: IP 10.217.123.154, passerelle 10.217.123.221, DNS 10.217.123.221, bail 3600s") != NULL);
    CHECK(strstr(log, "MTU de l'interface : 1400") != NULL);                               /* option 26 du DHCP */
    CHECK(strstr(log, "Tethering actif") != NULL);

    CHECK(cmd_has("/sbin/ifconfig utun99 inet 10.217.123.154 10.217.123.221 netmask 255.255.255.0 mtu 1400 up"));
    CHECK(cmd_has("/sbin/route -q -n add -net 0.0.0.0/1 10.217.123.221"));
    CHECK(cmd_has("/sbin/route -q -n add -net 128.0.0.0/1 10.217.123.221"));
    CHECK(cmd_has("ServerAddresses * 10.217.123.221"));                                    /* DNS du téléphone */
    CHECK(cmd_has("-inet6 -net ::/1 ::1 -reject"));                                        /* le blocage IPv6 a précédé l'IPv6 */
    CHECK(strstr(log, "IPv6: adresse 2600:383:a44:4d02:") != NULL);
    CHECK(cmd_has("prefixlen 64 alias"));
    CHECK(cmd_has("add -inet6 -net ::/1 -interface utun99"));
    CHECK(cmd_index("delete -inet6 -net ::/1 ::1") < cmd_index("add -inet6 -net ::/1 -interface utun99"));   /* blocage retiré avant les vraies routes */
    CHECK(cmd_has("/sbin/ifconfig utun99 mtu 1350"));                                      /* le plus petit des deux MTU */
    CHECK(strstr(log, "MTU de l'interface : 1350") != NULL);

    CHECK(ping_result.len == 4 + 20 + 8 + 4);                                              /* le ping a fait l'aller-retour complet */
    if (ping_result.len == 4 + 20 + 8 + 4) {
        const uint8_t *ip = ping_result.reply + 4, *icmp = ip + 20;
        CHECK(be32(ping_result.reply) == AF_INET);
        CHECK(!memcmp(ip + 12, (uint8_t[]){8, 8, 8, 8}, 4) && !memcmp(ip + 16, LEASE_IP4, 4));
        CHECK(icmp[0] == 0 && !memcmp(icmp + 8, "ping", 4) && csum16(icmp, 12) == 0 && csum16(ip, 20) == 0);
    }

    CHECK(cmd_has("/sbin/route -q -n delete -net 0.0.0.0/1 10.217.123.221"));              /* tout est nettoyé à la fin */
    CHECK(cmd_has("/sbin/route -q -n delete -inet6 -net ::/1 -interface utun99"));
    CHECK(cmd_has("remove State:/Network/Service/rndis-tether/IPv6"));
    CHECK(cmd_has("remove State:/Network/Service/rndis-tether/DNS"));
    CHECK(!v6_routes && !v6_svc && !dns_applied && !v6_blocked);
    CHECK(strstr(log, "[root] Le worker ne donne plus") == NULL);                          /* pas de chien de garde déclenché */
    free(log);
    test_fake_usb = 0;
    if (test_utun_peer >= 0) { close(test_utun_peer); test_utun_peer = -1; }
    sup_reset();
}

static void run_session_tests(void) {
    test_full_session();
}
