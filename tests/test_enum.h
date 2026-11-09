// SPDX-License-Identifier: GPL-3.0-or-later
/* Énumération USB simulée : find_device() et diag_usb() devant des périphériques fabriqués (descripteurs, échec
 * d'ouverture, mauvais identifiants). Inclus après src/main.c, comme test_session.h. */

struct fake_dev {
    uint16_t vid, pid;
    int cfg_err;                                                     /* 1 : la lecture de la configuration échoue */
    int open_err;                                                    /* code d'erreur de libusb_open (0 : réussit) */
    int nif;
    struct libusb_interface_descriptor alt[4];                       /* une altsetting par interface */
    struct libusb_endpoint_descriptor ep[4][3];
};

static struct fake_dev fdev[6];
static int nfdev;

static void fd_reset(void) { memset(fdev, 0, sizeof fdev); nfdev = 0; }

static struct fake_dev *fd_add(uint16_t vid, uint16_t pid) {
    struct fake_dev *d = &fdev[nfdev++];
    d->vid = vid; d->pid = pid;
    return d;
}

static void fd_iface(struct fake_dev *d, uint8_t cls, uint8_t sub, uint8_t proto) {
    struct libusb_interface_descriptor *a = &d->alt[d->nif];
    a->bInterfaceNumber = (uint8_t)d->nif; a->bInterfaceClass = cls; a->bInterfaceSubClass = sub; a->bInterfaceProtocol = proto;
    a->endpoint = d->ep[d->nif];
    d->nif++;
}

static void fd_ep(struct fake_dev *d, uint8_t addr, uint8_t attrs, uint16_t mps) {
    struct libusb_interface_descriptor *a = &d->alt[d->nif - 1];
    struct libusb_endpoint_descriptor *e = &d->ep[d->nif - 1][a->bNumEndpoints++];
    e->bEndpointAddress = addr; e->bmAttributes = attrs; e->wMaxPacketSize = mps;
}

/* un téléphone RNDIS complet : contrôle (0xE0/1/3, interruption 0x83) + données (0x0A, bulk 0x81 et 0x02) */
static struct fake_dev *fd_phone(uint16_t vid, uint16_t pid, uint16_t mps) {
    struct fake_dev *d = fd_add(vid, pid);
    fd_iface(d, 0xE0, 1, 3); fd_ep(d, 0x83, 3, 8);
    fd_iface(d, 0x0A, 0, 0); fd_ep(d, 0x81, 2, 512); fd_ep(d, 0x02, 2, mps);
    return d;
}

ssize_t fake_enum_list(libusb_device ***list) {
    static libusb_device *ptrs[6];
    for (int i = 0; i < nfdev; i++) ptrs[i] = (libusb_device *)&fdev[i];
    *list = ptrs;
    return nfdev;
}

int fake_enum_dev_desc(libusb_device *dev, struct libusb_device_descriptor *dd) {
    const struct fake_dev *d = (const struct fake_dev *)dev;
    memset(dd, 0, sizeof *dd);
    dd->idVendor = d->vid; dd->idProduct = d->pid;
    return 0;
}

int fake_enum_cfg(libusb_device *dev, struct libusb_config_descriptor **cfg) {
    static struct libusb_config_descriptor c;
    static struct libusb_interface ifs[4];
    struct fake_dev *d = (struct fake_dev *)dev;
    if (d->cfg_err) return LIBUSB_ERROR_IO;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < d->nif; i++) { ifs[i].altsetting = &d->alt[i]; ifs[i].num_altsetting = 1; }
    c.bNumInterfaces = (uint8_t)d->nif;
    c.interface = ifs;
    *cfg = &c;
    return 0;
}

int fake_enum_open(libusb_device *dev, libusb_device_handle **hd) {
    const struct fake_dev *d = (const struct fake_dev *)dev;
    if (d->open_err) return d->open_err;
    *hd = (libusb_device_handle *)(uintptr_t)1;
    return 0;
}

static int fd_find(uint16_t vid, uint16_t pid, char **log) {
    int rc;
    fflush(stderr);
    *log = NULL;
    FILE *old = stderr;
    char path[] = "/tmp/fd_find_XXXXXX";
    int fdlog = mkstemp(path);
    int saved = dup(STDERR_FILENO);
    dup2(fdlog, STDERR_FILENO);
    test_fake_enum = 1; h = NULL; ctrl_if = data_if = -1; ep_in = ep_out = ep_int = 0; mps_out = 0;
    rc = find_device(vid, pid);
    test_fake_enum = 0;
    fflush(stderr);
    dup2(saved, STDERR_FILENO); close(saved);
    (void)old;
    off_t n = lseek(fdlog, 0, SEEK_END);
    *log = calloc(1, (size_t)n + 1);
    lseek(fdlog, 0, SEEK_SET);
    if (n > 0 && read(fdlog, *log, (size_t)n) < 0) (*log)[0] = '\0';
    close(fdlog); unlink(path);
    return rc;
}

static void test_find_device(void) {
    char *log;
    worker_mode = 1;

    fd_reset(); fd_phone(0x04e8, 0x6864, 512);                       /* le cas normal */
    CHECK(fd_find(0, 0, &log) == 0);
    CHECK(ctrl_if == 0 && data_if == 1 && ep_in == 0x81 && ep_out == 0x02 && ep_int == 0x83 && mps_out == 512);
    CHECK(h != NULL && strstr(log, "Appareil RNDIS trouvé: 04e8:6864") != NULL);
    free(log);

    fd_reset(); fd_phone(0x04e8, 0x6864, 0);                         /* taille de paquet absente : 512 par défaut */
    CHECK(fd_find(0, 0, &log) == 0 && mps_out == 512);
    free(log);
    fd_reset(); fd_phone(0x04e8, 0x6864, 64);                        /* USB pleine vitesse */
    CHECK(fd_find(0, 0, &log) == 0 && mps_out == 64);
    free(log);

    fd_reset(); fd_phone(0x04e8, 0x6864, 512);                       /* filtre -d : bon et mauvais identifiants */
    CHECK(fd_find(0x04e8, 0x6864, &log) == 0);
    free(log);
    CHECK(fd_find(0x04e8, 0x6860, &log) == -1 && h == NULL);
    free(log);
    CHECK(fd_find(0x18d1, 0x6864, &log) == -1);
    free(log);

    fd_reset();                                                      /* un clavier d'abord, le téléphone ensuite */
    { struct fake_dev *k = fd_add(0x046d, 0xc31c); fd_iface(k, 3, 1, 1); fd_ep(k, 0x81, 3, 8); }
    fd_phone(0x04e8, 0x6864, 512);
    CHECK(fd_find(0, 0, &log) == 0 && strstr(log, "04e8:6864") != NULL);
    free(log);

    fd_reset(); fd_phone(0x04e8, 0x6864, 512); fdev[0].open_err = LIBUSB_ERROR_ACCESS;   /* sans les droits */
    CHECK(fd_find(0, 0, &log) == -1 && h == NULL);
    CHECK(strstr(log, "Ouverture USB 04e8:6864") != NULL && strstr(log, "sudo") != NULL);
    free(log);

    fd_reset(); fd_phone(0x04e8, 0x6864, 512); fdev[0].cfg_err = 1;                        /* configuration illisible : ignoré */
    CHECK(fd_find(0, 0, &log) == -1);
    free(log);

    fd_reset();                                                      /* contrôle sans interface de données */
    { struct fake_dev *d = fd_add(0x04e8, 0x6864); fd_iface(d, 0xE0, 1, 3); fd_ep(d, 0x83, 3, 8); }
    CHECK(fd_find(0, 0, &log) == -1);
    free(log);

    fd_reset();                                                      /* interface de données sans point d'entrée de sortie */
    { struct fake_dev *d = fd_add(0x04e8, 0x6864); fd_iface(d, 0xE0, 1, 3); fd_ep(d, 0x83, 3, 8);
      fd_iface(d, 0x0A, 0, 0); fd_ep(d, 0x81, 2, 512); }
    CHECK(fd_find(0, 0, &log) == -1);
    free(log);

    fd_reset();                                                      /* une interface vendeur avec deux bulk n'est pas l'interface de données */
    { struct fake_dev *d = fd_add(0x04e8, 0x6864); fd_iface(d, 0xE0, 1, 3); fd_ep(d, 0x83, 3, 8);
      fd_iface(d, 0xFF, 0, 0); fd_ep(d, 0x81, 2, 512); fd_ep(d, 0x02, 2, 512); }
    CHECK(fd_find(0, 0, &log) == -1);
    free(log);

    fd_reset();                                                      /* aucun périphérique */
    CHECK(fd_find(0, 0, &log) == -1);
    free(log);

    fd_reset();                                                      /* RNDIS sur la signature 0xEF/4/1, interruption absente */
    { struct fake_dev *d = fd_add(0x1234, 0x5678); fd_iface(d, 0xEF, 4, 1);
      fd_iface(d, 0x0A, 0, 0); fd_ep(d, 0x82, 2, 512); fd_ep(d, 0x01, 2, 512); }
    CHECK(fd_find(0, 0, &log) == 0 && ep_int == 0 && ep_in == 0x82 && ep_out == 0x01 && data_if == 1);
    free(log);

    fd_reset();                                                      /* CDC-ECM : macOS le gère, message une seule fois */
    { struct fake_dev *d = fd_add(0x04e8, 0x6863); fd_iface(d, 2, 6, 0); }
    CHECK(fd_find(0, 0, &log) == -1 && strstr(log, "CDC-ECM") != NULL);
    free(log);
    CHECK(fd_find(0, 0, &log) == -1 && strstr(log, "CDC-ECM") == NULL);
    free(log);

    h = NULL; worker_mode = 0;
}

/* Rapport --diagnose : seuls les appareils avec une interface réseau, ADB ou MTP sont listés. */
static void test_diag_usb(void) {
    struct diag_facts f;
    char buf[1024];
    FILE *o;

    fd_reset();
    { struct fake_dev *d = fd_phone(0x04e8, 0x6864, 512); fd_iface(d, 0xFF, 0x42, 1); }          /* RNDIS + ADB */
    { struct fake_dev *k = fd_add(0x046d, 0xc31c); fd_iface(k, 3, 1, 1); }                       /* clavier : absent du rapport */
    { struct fake_dev *m = fd_add(0x04e8, 0x6860); fd_iface(m, 6, 1, 1); }                       /* MTP */
    { struct fake_dev *b = fd_add(0x1111, 0x2222); fd_iface(b, 2, 6, 0); b->cfg_err = 1; }       /* configuration illisible : ignoré */
    memset(&f, 0, sizeof f);
    o = tmpfile();
    test_fake_enum = 1;
    diag_usb(o, &f);
    test_fake_enum = 0;
    rewind(o);
    size_t n = fread(buf, 1, sizeof buf - 1, o);
    buf[n] = '\0';
    fclose(o);
    CHECK(strstr(buf, "04e8:6864") != NULL && strstr(buf, "04e8:6860") != NULL);
    CHECK(strstr(buf, "046d") == NULL && strstr(buf, "1111") == NULL);
    CHECK(f.usb_rndis && f.usb_adb && f.usb_mtp && !f.usb_native);

    fd_reset();                                                      /* rien d'utile : message dédié */
    { struct fake_dev *k = fd_add(0x046d, 0xc31c); fd_iface(k, 3, 1, 1); }
    memset(&f, 0, sizeof f);
    o = tmpfile();
    test_fake_enum = 1;
    diag_usb(o, &f);
    test_fake_enum = 0;
    rewind(o);
    n = fread(buf, 1, sizeof buf - 1, o);
    buf[n] = '\0';
    fclose(o);
    CHECK(strstr(buf, "aucun appareil") != NULL && !f.usb_rndis && !f.usb_adb && !f.usb_mtp);

    fd_reset();                                                      /* CDC-ECM : signalé comme géré par macOS */
    { struct fake_dev *e = fd_add(0x04e8, 0x6863); fd_iface(e, 2, 6, 0); }
    memset(&f, 0, sizeof f);
    o = tmpfile();
    test_fake_enum = 1;
    diag_usb(o, &f);
    test_fake_enum = 0;
    fclose(o);
    CHECK(f.usb_native && !f.usb_rndis);
}

static void run_enum_tests(void) {
    test_find_device();
    test_diag_usb();
}
