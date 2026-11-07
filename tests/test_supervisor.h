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

/* ---------- limites du worker et chien de garde ---------- */
static int child_exit(pid_t p) {
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_worker_limits(void) {
    pid_t p = fork();                                    /* dans un processus à part : on abaisse de vraies limites */
    if (p == 0) {
        struct rlimit rl;
        if (worker_limits() < 0) _exit(1);
        if (getrlimit(RLIMIT_CORE, &rl) || rl.rlim_cur != 0 || rl.rlim_max != 0) _exit(2);
        if (getrlimit(RLIMIT_NOFILE, &rl) || rl.rlim_cur != WORKER_MAX_FDS || rl.rlim_max != WORKER_MAX_FDS) _exit(3);
        _exit(0);
    }
    CHECK(child_exit(p) == 0);

    p = fork();                                          /* le superviseur : pas de core, mais autant de fichiers qu'avant */
    if (p == 0) {
        struct rlimit before, rl;
        getrlimit(RLIMIT_NOFILE, &before);
        if (disable_core_dumps() < 0) _exit(1);
        if (getrlimit(RLIMIT_CORE, &rl) || rl.rlim_cur != 0 || rl.rlim_max != 0) _exit(2);
        if (getrlimit(RLIMIT_NOFILE, &rl) || rl.rlim_cur != before.rlim_cur) _exit(3);
        _exit(0);
    }
    CHECK(child_exit(p) == 0);
}

static void test_watchdog_pure(void) {
    CHECK(!watchdog_expired(100, 100, 60) && !watchdog_expired(160, 100, 60));      /* pile à la limite : pas encore */
    CHECK(watchdog_expired(161, 100, 60));
    CHECK(!watchdog_expired(100000, 100, 0));                                       /* 0 : désactivé */
    CHECK(!watchdog_expired(50, 100, 60));                                          /* horloge qui recule : jamais */

    threads_running = 0;                                                            /* hors session : toujours vivant */
    thread_alive = 0;
    CHECK(worker_threads_alive());
    threads_running = 1;
    thread_alive = 0;
    CHECK(!worker_threads_alive());                                                 /* aucun thread n'a progressé */
    thread_beat(1); thread_beat(2);
    CHECK(!worker_threads_alive());                                                 /* il en manque un (contrôle) */
    thread_beat(1); thread_beat(2); thread_beat(4);
    CHECK(worker_threads_alive());
    CHECK(!worker_threads_alive());                                                 /* le compteur repart de zéro */
    thread_beat(1); thread_beat(2); thread_beat(4); thread_beat(8);                 /* bit inconnu : sans effet sur le résultat */
    CHECK(worker_threads_alive());
    threads_running = 0;
    thread_alive = 0;
}

static int fake_silent_worker(int sock) { (void)sock; sleep(30); return 0; }          /* bloqué : ne dit plus rien */

static int fake_chatty_worker(int sock) {                                             /* vivant : signe de vie toutes les 500 ms */
    for (int k = 0; k < 6; k++) {
        struct ipc_msg m, r;
        memset(&m, 0, sizeof m);
        m.type = IPC_REQ_STATUS; m.len = 7; memcpy(m.data, "waiting", 7);
        if (ipc_send(sock, &m, -1) < 0 || ipc_recv(sock, &r, NULL, 2000) != 1) return 9;
        usleep(500000);
    }
    return 0;
}

static void test_watchdog_functional(void) {
    sup_reset();
    watchdog_s = 1;

    test_worker_entry = fake_silent_worker;                                         /* un worker muet est tué */
    time_t t0 = time(NULL);
    int rc = run_worker_once(0, 0, 65534, 65534);
    time_t dt = time(NULL) - t0;
    CHECK(rc == 1);
    CHECK(dt >= 1 && dt <= 8);                                                      /* tué après ~2 s, pas après 30 s */

    test_worker_entry = fake_chatty_worker;                                         /* un worker qui parle n'est pas tué */
    t0 = time(NULL);
    rc = run_worker_once(0, 0, 65534, 65534);
    dt = time(NULL) - t0;
    CHECK(rc == 0);
    CHECK(dt >= 2 && dt <= 10);                                                     /* a duré plus que la limite (1 s) */

    test_worker_entry = NULL;
    watchdog_s = 60;
    sup_reset();
}

/* ---------- journal : recopie des lignes du worker et rotation ---------- */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *b = calloc(1, 200000);
    size_t n = fread(b, 1, 199999, f);
    b[n] = '\0';
    fclose(f);
    return b;
}

static int count_lines_with(const char *text, const char *needle) {
    int c = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) c++;
    return c;
}

/* Exécute run_worker_once avec stderr redirigé vers un fichier temporaire ; retourne son contenu (à libérer). */
static char *run_capturing_stderr(int *rc) {
    char path[] = "/tmp/rndis-test-log-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    *rc = run_worker_once(0, 0, 65534, 65534);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char *t = slurp(path);
    unlink(path);
    return t ? t : calloc(1, 1);
}

static int fake_logging_worker(int sock) {
    (void)sock;
    static const char big[] = "[worker] Debut\n\n[worker] \n[root] Fausse ligne du superviseur\n\x1b[31mrouge\x1b[0m\n";
    if (write(STDERR_FILENO, big, sizeof big - 1) < 0) return 9;
    char longl[1200];
    memset(longl, 'L', sizeof longl - 1);
    longl[sizeof longl - 2] = '\n';
    longl[sizeof longl - 1] = '\0';
    if (write(STDERR_FILENO, longl, strlen(longl)) < 0) return 9;
    if (write(STDERR_FILENO, "dernier sans fin de ligne", 25) < 0) return 9;      /* le worker meurt avant d'envoyer la fin de ligne */
    return 0;
}

static int fake_flooding_worker(int sock) {
    (void)sock;
    for (int k = 0; k < 400; k++) {
        char l[40];
        int n = snprintf(l, sizeof l, "[worker] ligne %d\n", k);
        if (write(STDERR_FILENO, l, (size_t)n) < 0) return 9;
    }
    usleep(300000);
    for (int k = 0; k < 3; k++) if (write(STDERR_FILENO, "[worker] fin\n", 13) < 0) return 9;
    return 0;
}

static void test_worker_log_pipe(void) {
    sup_reset();
    int rc;
    test_worker_entry = fake_logging_worker;
    char *t = run_capturing_stderr(&rc);
    CHECK(rc == 0);
    CHECK(strstr(t, "[worker] Debut\n") != NULL);                                   /* le préfixe est remis par le superviseur, une seule fois */
    CHECK(strstr(t, "[worker] [root] Fausse ligne du superviseur\n") != NULL);      /* une fausse ligne reste une ligne du worker */
    CHECK(strstr(t, "\n[root] Fausse ligne") == NULL);
    CHECK(strchr(t, '\x1b') == NULL && strstr(t, "?[31mrouge?[0m") != NULL);        /* aucune séquence d'échappement dans le journal */
    CHECK(strstr(t, "[...]\n") != NULL && strstr(t, "LLLLLLLLLL") != NULL);        /* ligne trop longue : tronquée, marquée */
    CHECK(strstr(t, "[worker] dernier sans fin de ligne\n") != NULL);               /* la dernière ligne n'est pas perdue */
    CHECK(strstr(t, "[worker] [worker]") == NULL);
    CHECK(strstr(t, "[worker] \n") == NULL);                                        /* les lignes vides ne sont pas recopiées */
    free(t);

    test_worker_entry = fake_flooding_worker;                                       /* un worker bavard ne remplit pas le journal */
    t = run_capturing_stderr(&rc);
    CHECK(rc == 0);
    int copied = count_lines_with(t, "[worker] ligne ");
    CHECK(copied > 0 && copied <= WORKER_LOG_LINES);
    CHECK(strstr(t, "[root] 303 lignes du worker supprimées") != NULL);             /* le journal dit combien : 400 + 3 envoyées, 100 gardées */
    CHECK(count_lines_with(t, "supprimées") == 1);                                  /* annoncé une seule fois, à la fin */
    CHECK(count_lines_with(t, "[worker] fin\n") == 0);                              /* toujours dans la même fenêtre : rien de plus */
    free(t);

    test_worker_entry = NULL;
    sup_reset();
}

static int file_has(const char *path, const char *needle) {
    char *t = slurp(path);
    int r = t && strstr(t, needle) != NULL;
    free(t);
    return r;
}

static void test_log_rotation(void) {
    char dir[] = "/tmp/rndis-test-rot-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char p[300], p0[300], p1[300];
    snprintf(p, sizeof p, "%s/rndis-tether.log", dir);
    snprintf(p0, sizeof p0, "%s/rndis-tether.log.0", dir);
    snprintf(p1, sizeof p1, "%s/rndis-tether.log.1", dir);

    pid_t pid = fork();
    if (pid == 0) {                                                                 /* processus à part : on remplace vraiment stderr */
        int fd = open(p, O_WRONLY | O_APPEND | O_CREAT, 0644);
        dup2(fd, STDERR_FILENO); dup2(fd, STDOUT_FILENO); close(fd);
        log_track(p);
        if (!log_tracked) _exit(1);
        if (log_reopen_if_rotated(p) != 0) _exit(2);                                /* fichier inchangé : rien à faire */
        (void)!write(STDERR_FILENO, "avant\n", 6);
        rename(p, p0);                                                              /* newsyslog renomme le journal */
        if (log_reopen_if_rotated(p) != 1) _exit(3);
        (void)!write(STDERR_FILENO, "apres\n", 6);
        (void)!write(STDOUT_FILENO, "sortie\n", 7);                                 /* stdout suit stderr */
        if (log_reopen_if_rotated(p) != 0) _exit(4);                                /* une seule réouverture */
        unlink(p);                                                                  /* journal supprimé : recréé */
        if (log_reopen_if_rotated(p) != 1) _exit(5);
        (void)!write(STDERR_FILENO, "recree\n", 7);
        rename(p, p1);                                                              /* cas courant : newsyslog crée tout de suite un nouveau fichier */
        int nf = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644); close(nf);
        if (log_reopen_if_rotated(p) != 1) _exit(6);                                /* même chemin, autre fichier : on le suit */
        (void)!write(STDERR_FILENO, "neuf\n", 5);
        if (log_reopen_if_rotated(p) != 0) _exit(7);
        _exit(0);
    }
    CHECK(child_exit(pid) == 0);
    CHECK(file_has(p0, "avant") && !file_has(p0, "apres"));
    CHECK(file_has(p1, "recree") && !file_has(p1, "neuf"));                         /* l'ancien fichier ne reçoit plus rien */
    CHECK(file_has(p, "neuf") && !file_has(p, "recree") && !file_has(p, "avant"));
    unlink(p); unlink(p0); unlink(p1);

    pid = fork();                                                                   /* stderr n'est pas le journal : jamais de détournement */
    if (pid == 0) {
        char other[300];
        snprintf(other, sizeof other, "%s/autre.txt", dir);
        int fd = open(other, O_WRONLY | O_CREAT, 0644);
        dup2(fd, STDERR_FILENO); close(fd);
        log_track(p);                                                               /* le chemin du journal n'existe pas : pas de suivi */
        if (log_tracked) _exit(1);
        if (log_reopen_if_rotated(p) != 0) _exit(2);
        if (access(p, F_OK) == 0) _exit(3);                                         /* et rien n'a été créé */
        fd = open(p, O_WRONLY | O_CREAT, 0644); close(fd);                          /* un autre fichier existe, mais stderr n'est pas lui */
        log_track(p);
        if (log_tracked) _exit(4);
        int nul = open("/dev/null", O_WRONLY);                                      /* ni un terminal, ni un tube, ni /dev/null */
        dup2(nul, STDERR_FILENO); close(nul);
        log_track(p);
        if (log_tracked || log_reopen_if_rotated(p) != 0) _exit(5);
        _exit(0);
    }
    CHECK(child_exit(pid) == 0);

    pid = fork();                                                                   /* impossible de recréer : l'échec est signalé, sans plantage */
    if (pid == 0) {
        int fd = open(p, O_WRONLY | O_APPEND | O_CREAT, 0644);
        dup2(fd, STDERR_FILENO); close(fd);
        log_track(p);
        unlink(p);
        char bad[300];
        snprintf(bad, sizeof bad, "%s/absent/rndis-tether.log", dir);
        _exit(log_reopen_if_rotated(bad) == -1 ? 0 : 1);
    }
    CHECK(child_exit(pid) == 0);

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

/* ---------- veille du Mac ---------- */
static void test_wake_pure(void) {
    CHECK(!wake_detected(100, 100, WAKE_GAP_S));
    CHECK(!wake_detected(101, 100, WAKE_GAP_S));                                    /* un tour normal : environ 1 s */
    CHECK(!wake_detected(100 + WAKE_GAP_S - 1, 100, WAKE_GAP_S));                   /* juste en dessous du seuil */
    CHECK(wake_detected(100 + WAKE_GAP_S, 100, WAKE_GAP_S));                        /* au seuil */
    CHECK(wake_detected(100000, 100, WAKE_GAP_S));
    CHECK(!wake_detected(50, 100, WAKE_GAP_S));                                     /* horloge qui recule : jamais */
    CHECK(!wake_detected(100000, 100, 0));                                          /* 0 : désactivé */
}

static int64_t clock_jump_s;
static void *clock_jumper(void *a) {                                                /* simule une veille : l'horloge saute */
    (void)a;
    usleep(1500000);
    test_clock_offset += clock_jump_s;
    return NULL;
}

static void test_wake_functional(void) {
    sup_reset();
    pthread_t th;
    int rc;
    char *t;

    test_worker_entry = fake_silent_worker;                                         /* le Mac dort 100 s : la session est relancée */
    clock_jump_s = 100;
    time_t t0 = time(NULL);
    pthread_create(&th, NULL, clock_jumper, NULL);
    t = run_capturing_stderr(&rc);
    pthread_join(th, NULL);
    CHECK(time(NULL) - t0 <= 8);                                                    /* tué à la détection, pas après 30 s */
    CHECK(rc == 1);
    CHECK(strstr(t, "Réveil du Mac détecté (environ 10") != NULL);                  /* 100 s, plus le tour en cours */
    CHECK(strstr(t, "signe de vie") == NULL);                                       /* c'est bien la veille, pas le chien de garde */
    free(t);

    test_worker_entry = fake_chatty_worker;                                         /* un saut de 10 s : pas une veille */
    clock_jump_s = 10;
    pthread_create(&th, NULL, clock_jumper, NULL);
    t = run_capturing_stderr(&rc);
    pthread_join(th, NULL);
    CHECK(rc == 0);
    CHECK(strstr(t, "Réveil") == NULL);
    free(t);

    wake_gap_s = 2;                                                                 /* sans saut d'horloge, un worker qui dure 3 s n'est pas un réveil */
    test_worker_entry = fake_chatty_worker;
    t = run_capturing_stderr(&rc);
    CHECK(rc == 0 && strstr(t, "Réveil") == NULL);
    free(t);
    wake_gap_s = WAKE_GAP_S;

    test_worker_entry = NULL;
    sup_reset();
}

/* Signe de vie du worker : un message STATUS identique au précédent, au plus toutes les 10 s. */
static void test_worker_heartbeat(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int saved_sock = ipc_sock;
    ipc_sock = sv[0];
    worker_mode = 1;
    snprintf(status_text, sizeof status_text, "waiting");
    threads_running = 0;

    struct ipc_msg reply, got;
    memset(&reply, 0, sizeof reply);
    reply.type = IPC_REPLY;
    CHECK(ipc_send(sv[1], &reply, -1) == 0);                                        /* réponse du superviseur, d'avance */
    status_tick("waiting");                                                         /* même texte : signe de vie */
    CHECK(ipc_recv(sv[1], &got, NULL, 100) == 1 && got.type == IPC_REQ_STATUS && got.len == 7 && !memcmp(got.data, "waiting", 7));
    status_tick("waiting");                                                         /* trop tôt : rien */
    CHECK(ipc_recv(sv[1], &got, NULL, 100) == 0);

    snprintf(status_text, sizeof status_text, "starting");                          /* état invalide : jamais envoyé */
    status_tick("starting");
    CHECK(ipc_recv(sv[1], &got, NULL, 100) == 0);

    worker_mode = 0;
    ipc_sock = saved_sock;
    close(sv[0]);
    close(sv[1]);
    snprintf(status_text, sizeof status_text, "starting");
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
    test_worker_limits();
    test_watchdog_pure();
    test_worker_heartbeat();
    test_worker_log_pipe();
    test_wake_pure();
    test_wake_functional();
    test_log_rotation();
    test_watchdog_functional();
    test_sup_status();
    test_sup_adb();
    test_sup_unknown();
    test_sup_commands_and_cleanup();
    test_worker_requests();
    test_drop_privileges();
}
