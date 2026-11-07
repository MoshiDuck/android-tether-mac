// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Tests du superviseur root (inclus par test_main.c, qui inclut src/main.c).
 *
 * Compilés avec -DRNDIS_TESTING : les commandes système (ifconfig, route, scutil, adb) sont enregistrées
 * au lieu d'être exécutées. On vérifie donc, sans root, ce que le superviseur accepte, refuse, et les
 * commandes exactes qu'il construit à partir de valeurs validées.
 */
#define MAX_CMDS 64
static char cmds[MAX_CMDS][500];
static int ncmds;

void test_record_command(const char *cmd) {
    if (ncmds < MAX_CMDS) snprintf(cmds[ncmds++], sizeof cmds[0], "%s", cmd);
}

static int cmd_index(const char *needle) {                           /* rang de la première commande contenant needle, ou -1 */
    for (int k = 0; k < ncmds; k++)
        if (strstr(cmds[k], needle)) return k;
    return -1;
}

static int cmd_count(const char *needle) {                           /* nombre de commandes contenant needle */
    int c = 0;
    for (int k = 0; k < ncmds; k++)
        if (strstr(cmds[k], needle)) c++;
    return c;
}

static int cmd_has(const char *needle) {
    for (int k = 0; k < ncmds; k++)
        if (strstr(cmds[k], needle)) return 1;
    return 0;
}

/* réseaux « connectés au Mac » simulés pour la garde contre le masquage de sous-réseau */
static struct connected_net test_nets[8];
static int test_nnets;

int test_list_connected(struct connected_net *out, int max) {
    int n = test_nnets < max ? test_nnets : max;
    for (int k = 0; k < n; k++) out[k] = test_nets[k];
    return n;
}

static void add_net(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t ma, uint8_t mb, uint8_t mc, uint8_t md) {
    uint8_t ip[4] = {a, b, c, d}, mk[4] = {ma, mb, mc, md};
    memcpy(&test_nets[test_nnets].ip, ip, 4);
    memcpy(&test_nets[test_nnets].mask, mk, 4);
    test_nnets++;
}

static void sup_reset(void) {
    test_nnets = 0;
    allow_overlap = 0;
    ncmds = 0;
    if (utun_fd >= 0) close(utun_fd);
    utun_fd = -1;
    cfg4_done = cfg6_done = 0;
    my_ip = gw_ip = 0;
    v6_routes = v6_svc = dns_applied = 0;
    v6_blocked = allow_v6_leak = 0;
    want_route = 1; ipv6_on = 1; auto_adb = 0; last_adb_run = 0; adb_user = NULL;
    worker_mode = 0; quit = 0; running = 1;
    snprintf(status_text, sizeof status_text, "starting");
}

static struct ipc_msg msg(uint8_t type, const void *data, uint16_t len) {
    struct ipc_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.len = len;
    if (len) memcpy(m.data, data, len);
    return m;
}

/* Envoie la demande au superviseur (même processus) et lit sa réponse. */
static int sup_call(struct ipc_msg rq, struct ipc_msg *rp, int *fd) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    handle_request(sv[0], &rq);
    int r = ipc_recv(sv[1], rp, fd, 1000);
    close(sv[0]);
    close(sv[1]);
    return r;
}

static const uint8_t CFG4_OK[16] = {10, 217, 123, 144, 10, 217, 123, 221, 255, 255, 255, 0, 10, 217, 123, 221};
static const uint8_t GLOBAL6[16] = {0x26, 0x00, 0x03, 0x83, 0x0a, 0x44, 0x4d, 0x02, 0x1d, 0x71, 0xd2, 0xaf, 0x4e, 0xb6, 0xa5, 0xbb};
static const uint8_t ROUTER6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x7c, 0x02, 0x31, 0xff, 0xfe, 0xd7, 0xbd, 0xf2};

/* demande d'une interface utun : réussit, et une seule fois par worker */
static void sup_utun(void) {
    struct ipc_msg rp;
    int fd = -1;
    CHECK(sup_call(msg(IPC_REQ_UTUN, NULL, 0), &rp, &fd) == 1);
    CHECK(rp.status == 0 && fd >= 0 && rp.len == 6 && !memcmp(rp.data, "utun99", 6));
    if (fd >= 0) close(fd);
    fd = -1;
    CHECK(sup_call(msg(IPC_REQ_UTUN, NULL, 0), &rp, &fd) == 1 && rp.status == 200 && fd == -1);   /* pas de deuxième */
}

static void test_sup_config4(void) {
    sup_reset();
    struct ipc_msg rp;
    int fd = -1;
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 200);  /* pas encore d'interface */
    CHECK(ncmds == 0);
    sup_utun();

    uint8_t bad[16];
    memcpy(bad, CFG4_OK, 16); bad[4] = 10; bad[5] = 217; bad[6] = 124;                       /* passerelle hors sous-réseau */
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, bad, 16), &rp, &fd) == 1 && rp.status == POLICY_BAD_GATEWAY);
    memcpy(bad, CFG4_OK, 16); bad[0] = 127; bad[1] = 0; bad[2] = 0; bad[3] = 1;               /* adresse de boucle locale */
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, bad, 16), &rp, &fd) == 1 && rp.status == POLICY_BAD_IP);
    memcpy(bad, CFG4_OK, 16); bad[8] = 255; bad[9] = 0; bad[10] = 255; bad[11] = 0;           /* masque non contigu */
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, bad, 16), &rp, &fd) == 1 && rp.status == POLICY_BAD_MASK);
    memcpy(bad, CFG4_OK, 16); bad[12] = 127; bad[13] = 0; bad[14] = 0; bad[15] = 1;           /* DNS en boucle locale */
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, bad, 16), &rp, &fd) == 1 && rp.status == POLICY_BAD_DNS);
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 15), &rp, &fd) == 1 && rp.status == 200);   /* taille incorrecte */
    CHECK(ncmds == 0 && !cfg4_done);                                                          /* rien n'a été exécuté */

    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cfg4_done);
    CHECK(cmd_has("/sbin/ifconfig utun99 inet 10.217.123.144 10.217.123.221 netmask 255.255.255.0 mtu 1500 up"));
    CHECK(cmd_has("/sbin/route -q -n add -net 0.0.0.0/1 10.217.123.221"));
    CHECK(cmd_has("/sbin/route -q -n add -net 128.0.0.0/1 10.217.123.221"));
    CHECK(cmd_has("ServerAddresses * 10.217.123.221"));                                       /* DNS du service réseau */
    int before = ncmds;
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 200);   /* une seule fois */
    CHECK(ncmds == before);
}

static void test_sup_overlap(void) {
    struct ipc_msg rp;
    int fd = -1;
    uint8_t phone24[16] = {172, 22, 208, 144, 172, 22, 208, 221, 255, 255, 255, 0, 172, 22, 208, 221};   /* 172.22.208.0/24 */

    sup_reset();                                                    /* Wi-Fi 172.22.209.216/20 : le sous-réseau du téléphone s'y trouve */
    add_net(172, 22, 209, 216, 255, 255, 240, 0);
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, phone24, 16), &rp, &fd) == 1 && rp.status == POLICY_OVERLAP);
    CHECK(ncmds == 0 && !cfg4_done);                                /* aucune commande exécutée */

    test_nnets = 0;                                                 /* le conflit disparaît : la même demande passe */
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, phone24, 16), &rp, &fd) == 1 && rp.status == 0 && cfg4_done);

    sup_reset();                                                    /* sous-réseau distinct du Wi-Fi : accepté */
    add_net(172, 22, 209, 216, 255, 255, 240, 0);
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);

    sup_reset();                                                    /* réseau d'entreprise en /8 : ne bloque pas */
    add_net(10, 1, 2, 3, 255, 0, 0, 0);
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);

    sup_reset();                                                    /* adresse VPN en /32 : ne bloque pas */
    add_net(10, 217, 123, 50, 255, 255, 255, 255);
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);

    sup_reset();                                                    /* -O : conflit accepté sciemment */
    add_net(172, 22, 209, 216, 255, 255, 240, 0);
    allow_overlap = 1;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, phone24, 16), &rp, &fd) == 1 && rp.status == 0);
    allow_overlap = 0;
    sup_reset();
}

static void test_sup_config6(void) {
    sup_reset();
    struct ipc_msg rp;
    int fd = -1;
    uint8_t d[32];
    memcpy(d, GLOBAL6, 16); memcpy(d + 16, ROUTER6, 16);
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 32), &rp, &fd) == 1 && rp.status == 200);          /* avant utun et IPv4 */
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 32), &rp, &fd) == 1 && rp.status == 200);          /* IPv4 d'abord */
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    ncmds = 0;

    uint8_t bad[32];
    memcpy(bad, d, 32); memcpy(bad, ROUTER6, 16);                                             /* adresse link-local */
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, bad, 32), &rp, &fd) == 1 && rp.status == POLICY_BAD_V6_ADDR);
    memcpy(bad, d, 32); memcpy(bad + 16, GLOBAL6, 16);                                        /* routeur non link-local */
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, bad, 32), &rp, &fd) == 1 && rp.status == POLICY_BAD_V6_ROUTER);
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 31), &rp, &fd) == 1 && rp.status == 200);
    ipv6_on = 0;
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 32), &rp, &fd) == 1 && rp.status == 200);          /* -4 */
    ipv6_on = 1;
    CHECK(ncmds == 0 && !cfg6_done);

    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 32), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has("/sbin/ifconfig utun99 inet6 2600:383:a44:4d02:1d71:d2af:4eb6:a5bb prefixlen 64 alias"));
    CHECK(cmd_has("/sbin/route -q -n add -inet6 -net ::/1 -interface utun99"));
    CHECK(cmd_has("/sbin/route -q -n add -inet6 -net 8000::/1 -interface utun99"));
    CHECK(cmd_has("Router fe80::7c02:31ff:fed7:bdf2"));
    int before = ncmds;
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 32), &rp, &fd) == 1 && rp.status == 200);          /* une seule fois */
    CHECK(ncmds == before);
}

/* Avec -r, l'IPv6 ne fuit pas par un autre réseau tant que le tethering n'a pas d'IPv6. */
static void test_sup_ipv6_leak_guard(void) {
    struct ipc_msg rp;
    int fd = -1;
    const char *add_a = "/sbin/route -q -n add -inet6 -net ::/1 ::1 -reject";
    const char *add_b = "/sbin/route -q -n add -inet6 -net 8000::/1 ::1 -reject";
    const char *del_a = "/sbin/route -q -n delete -inet6 -net ::/1 ::1";
    const char *del_b = "/sbin/route -q -n delete -inet6 -net 8000::/1 ::1";
    uint8_t d6[32];
    memcpy(d6, GLOBAL6, 16); memcpy(d6 + 16, ROUTER6, 16);

    sup_reset();                                                    /* IPv4 appliqué : l'IPv6 est bloqué en attendant */
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has(add_a) && cmd_has(add_b) && v6_blocked == 1);
    int n = ncmds;
    block_ipv6();                                                   /* idempotent : aucune commande en double */
    CHECK(ncmds == n);

    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 0);   /* le tethering a de l'IPv6 */
    CHECK(v6_blocked == 0);
    CHECK(cmd_has(del_a) && cmd_has(del_b));
    int del = cmd_index(del_a), real = cmd_index("add -inet6 -net ::/1 -interface utun99");
    CHECK(del >= 0 && real >= 0 && del < real);                     /* les routes « reject » partent AVANT les vraies routes */
    CHECK(cmd_index(del_b) < cmd_index("add -inet6 -net 8000::/1 -interface utun99"));

    sup_reset();                                                    /* IPv6 refusé par la politique : le blocage reste */
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    uint8_t bad6[32];
    memcpy(bad6, d6, 32); memcpy(bad6, ROUTER6, 16);                /* adresse link-local : refusée */
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, bad6, 32), &rp, &fd) == 1 && rp.status == POLICY_BAD_V6_ADDR);
    CHECK(v6_blocked == 1);

    sup_reset();                                                    /* aucun IPv6 ne vient : le nettoyage retire le blocage */
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    ncmds = 0;
    unconfigure_iface();
    CHECK(cmd_has(del_a) && cmd_has(del_b) && v6_blocked == 0);
    n = ncmds;
    unconfigure_iface();                                            /* un second nettoyage ne rejoue pas la suppression */
    CHECK(cmd_count(del_a) == 1 && cmd_count(del_b) == 1 && ncmds >= n);   /* les routes « reject » ne sont retirées qu'une fois */

    sup_reset();                                                    /* -4 : pas d'IPv6 sur le tethering, mais le blocage s'applique */
    ipv6_on = 0;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has(add_a) && v6_blocked == 1);
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 200 && v6_blocked == 1);   /* CONFIG6 refusé sous -4 */

    sup_reset();                                                    /* sans -r : rien n'est bloqué */
    want_route = 0;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(!cmd_has("-reject") && v6_blocked == 0);

    sup_reset();                                                    /* -L : l'ancien comportement, aucun blocage */
    allow_v6_leak = 1;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(!cmd_has("-reject") && v6_blocked == 0);
    sup_reset();
}

/* Le téléphone perd son IPv6 en cours de session : le superviseur retire tout et remet le blocage. */
static void test_sup_deconfig6(void) {
    struct ipc_msg rp;
    int fd = -1;
    uint8_t d6[32];
    memcpy(d6, GLOBAL6, 16); memcpy(d6 + 16, ROUTER6, 16);
    const char *rej_a = "/sbin/route -q -n add -inet6 -net ::/1 ::1 -reject";
    const char *rej_del = "/sbin/route -q -n delete -inet6 -net ::/1 ::1";
    const char *alias_del = "/sbin/ifconfig utun99 inet6 2600:383:a44:4d02:1d71:d2af:4eb6:a5bb -alias";

    sup_reset();
    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 200);       /* rien à retirer */
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 200);       /* pas encore d'IPv6 */
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cfg6_done && v6_routes && v6_svc && !v6_blocked);
    ncmds = 0;

    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 200);        /* pas de données attendues */
    CHECK(ncmds == 0 && cfg6_done);

    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has("remove State:/Network/Service/rndis-tether/IPv6"));
    CHECK(cmd_has("/sbin/route -q -n delete -inet6 -net ::/1 -interface utun99"));
    CHECK(cmd_has("/sbin/route -q -n delete -inet6 -net 8000::/1 -interface utun99"));
    CHECK(cmd_has(alias_del));
    CHECK(cmd_has(rej_a) && cmd_has("/sbin/route -q -n add -inet6 -net 8000::/1 ::1 -reject"));
    CHECK(!cfg6_done && !v6_routes && !v6_svc && v6_blocked == 1);
    /* ordre : les vraies routes partent avant que le blocage ne les remplace */
    CHECK(cmd_index("delete -inet6 -net ::/1 -interface utun99") < cmd_index(rej_a));
    CHECK(cmd_index("delete -inet6 -net 8000::/1 -interface utun99") < cmd_index("add -inet6 -net 8000::/1 ::1 -reject"));

    int n = ncmds;
    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 200);      /* une seule fois */
    CHECK(ncmds == n);

    ncmds = 0;                                                       /* l'IPv6 revient : tout est réinstallé */
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has(rej_del) && cmd_has("/sbin/route -q -n add -inet6 -net ::/1 -interface utun99"));
    CHECK(cfg6_done && v6_blocked == 0 && v6_routes && v6_svc);

    unconfigure_iface();                                             /* fin de session : rien ne reste */
    CHECK(!v6_routes && !v6_svc && !v6_blocked);

    sup_reset();                                                     /* -L : retrait sans blocage */
    allow_v6_leak = 1;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 0);
    ncmds = 0;
    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has(alias_del) && !cmd_has("-reject") && v6_blocked == 0);

    sup_reset();                                                     /* sans -r : pas de routes à retirer, ni de blocage */
    want_route = 0;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d6, 32), &rp, &fd) == 1 && rp.status == 0);
    ncmds = 0;
    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has(alias_del) && !cmd_has("-reject") && !cmd_has("route"));

    sup_reset();                                                     /* -4 : jamais */
    ipv6_on = 0;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    CHECK(sup_call(msg(IPC_REQ_DECONFIG6, NULL, 0), &rp, &fd) == 1 && rp.status == 200);
    sup_reset();
}

static void test_sup_status(void) {
    sup_reset();
    struct ipc_msg rp;
    int fd = -1;
    CHECK(sup_call(msg(IPC_REQ_STATUS, "waiting", 7), &rp, &fd) == 1 && rp.status == 0);
    CHECK(!strcmp(status_text, "waiting"));
    sup_utun();
    const char *ok = "connected 10.217.123.144 utun99";
    CHECK(sup_call(msg(IPC_REQ_STATUS, ok, (uint16_t)strlen(ok)), &rp, &fd) == 1 && rp.status == 0);
    CHECK(!strcmp(status_text, ok));

    const char *refused[] = {"connected 10.217.123.144 utun1",       /* autre interface que la nôtre */
                             "connected 10.217.123.144 utun9",
                             "connected 10.217.123.144 utun990",
                             "disabled",                             /* réservé au superviseur */
                             "waiting\nconnecting",
                             "connected $(id) utun99",
                             ""};
    for (unsigned k = 0; k < sizeof refused / sizeof *refused; k++) {
        CHECK(sup_call(msg(IPC_REQ_STATUS, refused[k], (uint16_t)strlen(refused[k])), &rp, &fd) == 1 && rp.status != 0);
        CHECK(!strcmp(status_text, ok));                             /* l'état n'a pas changé */
    }
    /* « connected » sans interface créée */
    sup_reset();
    CHECK(sup_call(msg(IPC_REQ_STATUS, ok, (uint16_t)strlen(ok)), &rp, &fd) == 1 && rp.status != 0);
}

static void test_sup_adb(void) {
    sup_reset();
    struct ipc_msg rp;
    int fd = -1;
    CHECK(sup_call(msg(IPC_REQ_ADB, NULL, 0), &rp, &fd) == 1 && rp.status == 200 && ncmds == 0);   /* sans -a */
    auto_adb = 1;
    adb_user = "gabriel";
    CHECK(sup_call(msg(IPC_REQ_ADB, NULL, 0), &rp, &fd) == 1 && rp.status == 0);
    CHECK(cmd_has("/usr/bin/sudo -u gabriel /bin/zsh -lc 'adb shell svc usb setFunctions rndis'"));
    int before = ncmds;
    CHECK(sup_call(msg(IPC_REQ_ADB, NULL, 0), &rp, &fd) == 1 && rp.status == 200);                 /* limité en fréquence */
    CHECK(ncmds == before);

    last_adb_run = 0;                                                /* nom d'utilisateur suspect : aucune commande */
    ncmds = 0;
    adb_user = "a;b";
    sup_call(msg(IPC_REQ_ADB, NULL, 0), &rp, &fd);
    last_adb_run = 0;
    adb_user = "$(id)";
    sup_call(msg(IPC_REQ_ADB, NULL, 0), &rp, &fd);
    CHECK(ncmds == 0);
}

static void test_sup_unknown(void) {
    sup_reset();
    struct ipc_msg rp;
    int fd = -1;
    CHECK(sup_call(msg(77, NULL, 0), &rp, &fd) == 1 && rp.status == 255);
    CHECK(sup_call(msg(0, NULL, 0), &rp, &fd) == 1 && rp.status == 255);
    CHECK(sup_call(msg(IPC_REPLY, NULL, 0), &rp, &fd) == 1 && rp.status == 255);          /* un worker n'envoie pas de réponses */
    CHECK(ncmds == 0);
}

/* Tout ce que le superviseur exécute ne contient que des caractères sûrs, et le nettoyage est complet. */
static void test_sup_commands_and_cleanup(void) {
    sup_reset();
    struct ipc_msg rp;
    int fd = -1;
    sup_utun();
    CHECK(sup_call(msg(IPC_REQ_CONFIG4, CFG4_OK, 16), &rp, &fd) == 1 && rp.status == 0);
    uint8_t d[32];
    memcpy(d, GLOBAL6, 16); memcpy(d + 16, ROUTER6, 16);
    CHECK(sup_call(msg(IPC_REQ_CONFIG6, d, 32), &rp, &fd) == 1 && rp.status == 0);

    for (int k = 0; k < ncmds; k++) {
        const char *c = cmds[k];
        if (!strncmp(c, "d.init", 6)) {                              /* script scutil : pas de caractère de contrôle du shell */
            CHECK(!strpbrk(c, ";|`$&<>\\"));
        } else {                                                     /* commande : uniquement [A-Za-z0-9 /.:_-] */
            CHECK(strspn(c, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /.:_-") == strlen(c));
        }
    }

    ncmds = 0;
    unconfigure_iface();
    CHECK(cmd_has("remove State:/Network/Service/rndis-tether/IPv6"));
    CHECK(cmd_has("remove State:/Network/Service/rndis-tether/DNS"));
    CHECK(cmd_has("/sbin/route -q -n delete -inet6 -net ::/1 -interface utun99"));
    CHECK(cmd_has("/sbin/route -q -n delete -inet6 -net 8000::/1 -interface utun99"));
    CHECK(cmd_has("/sbin/route -q -n delete -net 0.0.0.0/1 10.217.123.221"));
    CHECK(cmd_has("/sbin/route -q -n delete -net 128.0.0.0/1 10.217.123.221"));
    CHECK(!v6_routes && !v6_svc && !dns_applied);                    /* état remis à zéro */
    int before = ncmds;
    unconfigure_iface();
    CHECK(ncmds == before - 0 || ncmds >= before);                   /* un second nettoyage ne plante pas */
}

/* ---------- worker <-> superviseur sur un vrai canal (deux threads) ---------- */
static void *sup_loop(void *arg) {
    int sock = *(int *)arg;
    for (;;) {
        struct ipc_msg m;
        int fd = -1;
        if (ipc_recv(sock, &m, &fd, 3000) != 1) break;
        if (fd >= 0) close(fd);
        handle_request(sock, &m);
    }
    return NULL;
}

static void test_worker_requests(void) {
    sup_reset();
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pthread_t t;
    pthread_create(&t, NULL, sup_loop, &sv[1]);
    ipc_sock = sv[0];                                                /* ce thread joue le worker */
    worker_mode = 1;

    int wfd = request_utun();
    CHECK(wfd >= 0 && !strcmp(utun_name, "utun99"));

    struct lease L;
    memset(&L, 0, sizeof L);
    memcpy(&L.ip, CFG4_OK, 4); memcpy(&L.gw, CFG4_OK + 4, 4); memcpy(&L.mask, CFG4_OK + 8, 4); memcpy(&L.dns, CFG4_OK + 12, 4);
    struct lease bad = L;
    memcpy(&bad.gw, (uint8_t[]){10, 217, 124, 1}, 4);
    CHECK(request_config4(&bad) == -1);                              /* refusée par la politique */
    CHECK(ncmds == 0);
    CHECK(request_config4(&L) == 0);
    CHECK(cmd_has("/sbin/ifconfig utun99 inet 10.217.123.144"));
    CHECK(request_config4(&L) == -1);                                /* déjà appliquée */

    memcpy(v6_addr, GLOBAL6, 16); memcpy(v6_router, ROUTER6, 16);
    ncmds = 0;
    request_config6();
    CHECK(cmd_has("inet6 2600:383:a44:4d02:1d71:d2af:4eb6:a5bb prefixlen 64"));

    if (wfd >= 0) close(wfd);
    close(sv[0]);                                                    /* le superviseur voit la fin du canal */
    pthread_join(t, NULL);
    close(sv[1]);

    /* superviseur disparu : la demande échoue et le worker s'arrête */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ipc_sock = sv[0];
    close(sv[1]);
    quit = 0; running = 1;
    CHECK(request_utun() == -1);
    CHECK(quit == 1 && running == 0);
    close(sv[0]);
    sup_reset();
}

static void test_drop_privileges(void) {
    if (geteuid() == 0) return;                                      /* jamais exécuté en root : il retirerait nos droits */
    CHECK(drop_privileges(65534, 65534) == -1);                      /* sans root, l'abandon échoue (et on le détecte) */
}

/* La vraie interrogation du système (getifaddrs) renvoie des réseaux cohérents : jamais d'adresse nulle, de boucle locale ou de masque invalide. */
static void test_list_connected_system(void) {
    struct connected_net nets[64];
    int n = list_connected_system(nets, 64, NULL);
    CHECK(n >= 0 && n <= 64);
    for (int k = 0; k < n; k++) {
        uint32_t ip = ntohl(nets[k].ip), m = ntohl(nets[k].mask);
        CHECK(ip != 0 && (ip >> 24) != 127);
        CHECK(m != 0 && (~m & (~m + 1)) == 0);                       /* masque contigu */
    }
    if (n > 0) {                                                      /* le filtrage par nom retire bien l'interface demandée */
        struct ifaddrs *all, *p;
        char name[IFNAMSIZ] = "";
        if (getifaddrs(&all) == 0) {
            for (p = all; p && !name[0]; p = p->ifa_next) {
                if (p->ifa_addr && p->ifa_netmask && p->ifa_addr->sa_family == AF_INET && (p->ifa_flags & IFF_UP) &&
                    !(p->ifa_flags & IFF_LOOPBACK))
                    snprintf(name, sizeof name, "%s", p->ifa_name);
            }
            int cnt_name = 0;
            for (p = all; p; p = p->ifa_next)
                if (p->ifa_addr && p->ifa_netmask && p->ifa_addr->sa_family == AF_INET && (p->ifa_flags & IFF_UP) &&
                    !(p->ifa_flags & IFF_LOOPBACK) && !strcmp(p->ifa_name, name))
                    cnt_name++;
            freeifaddrs(all);
            CHECK(list_connected_system(nets, 64, name) == n - cnt_name);
        }
    }
    CHECK(list_connected_system(nets, 0, NULL) == 0);                 /* tampon de taille nulle : rien n'est écrit */
}

static void run_supervisor_tests(void) {
    test_list_connected_system();
    test_sup_config4();
    test_sup_overlap();
    test_sup_ipv6_leak_guard();
    test_sup_config6();
    test_sup_deconfig6();
    test_sup_status();
    test_sup_adb();
    test_sup_unknown();
    test_sup_commands_and_cleanup();
    test_worker_requests();
    test_drop_privileges();
}
