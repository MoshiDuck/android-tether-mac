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

static int cmd_has(const char *needle) {
    for (int k = 0; k < ncmds; k++)
        if (strstr(cmds[k], needle)) return 1;
    return 0;
}

static void sup_reset(void) {
    ncmds = 0;
    if (utun_fd >= 0) close(utun_fd);
    utun_fd = -1;
    cfg4_done = cfg6_done = 0;
    my_ip = gw_ip = 0;
    v6_routes = v6_svc = dns_applied = 0;
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

static void run_supervisor_tests(void) {
    test_sup_config4();
    test_sup_config6();
    test_sup_status();
    test_sup_adb();
    test_sup_unknown();
    test_sup_commands_and_cleanup();
    test_worker_requests();
    test_drop_privileges();
}
