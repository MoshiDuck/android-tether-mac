# Comment fonctionne TetherMac

*English: [how-it-works.md](how-it-works.md)*

TetherMac donne accès à Internet à un Mac via le partage de connexion USB d'un téléphone Android, **sans aucune extension noyau**. Cette page explique le problème, la méthode et chaque pièce, pour pouvoir auditer, déboguer ou étendre le projet.

Sources : [`src/`](../src) (le driver en C : `main.c`, `dhcp.c`, `policy.c`, `ipc.c`) et [`menubar/RNDISTether.swift`](../menubar/RNDISTether.swift) (l'icône de barre de menus).

## 1. Le problème

Quand tu actives le partage de connexion USB, un téléphone Android se présente comme une carte réseau USB utilisant le protocole **RNDIS** (Remote NDIS, un protocole Microsoft qui transporte des trames Ethernet sur USB).

- macOS n'a **aucun pilote RNDIS intégré**. (Il gère CDC-ECM/NCM, que certains téléphones utilisent à la place, ainsi que le partage de connexion de l'iPhone.)
- La solution classique, [HoRNDIS](https://github.com/jwise/HoRNDIS), est un **kext** : du code chargé dans le noyau. Apple les supprime progressivement. Sur Apple Silicon ils sont bloqués par défaut ; en charger un demande de baisser le niveau de sécurité au démarrage et de l'approuver à la main, et chaque version de macOS rend cela plus difficile.
- Le remplaçant officiel, **DriverKit**, demande pour les pilotes réseau une autorisation spéciale d'Apple qu'un projet open source amateur ne peut pas raisonnablement obtenir.

## 2. La méthode

Au lieu d'un pilote *dans* le noyau, TetherMac est un programme ordinaire qui fait le travail *en dehors*, en deux moitiés :

```
┌────────────┐   USB (libusb)    ┌─────────────────────┐   utun    ┌──────────────┐
│  Android   │ ◄───────────────► │  rndis-tether       │ ◄───────► │ pile réseau  │
│  (RNDIS)   │ trames Ethernet   │  (espace utilisat.) │ paquets IP│    macOS     │
└────────────┘ dans paquets RNDIS│  ARP · DHCP · liens │           └──────────────┘
                                 └─────────────────────┘
```

1. **Côté USB** : [libusb](https://libusb.info) ouvre le téléphone par l'API USB normale de macOS en espace utilisateur (IOKit). Ça marche parce que macOS ne réclame pas l'interface RNDIS du téléphone : personne d'autre ne l'utilise.
2. **Côté réseau** : une interface **`utun`**, du même type que celles des VPN, est fournie par le noyau et alimentée depuis l'espace utilisateur.

Aucun code n'est chargé dans le noyau et aucune autorisation Apple n'est nécessaire. Seuls les droits root sont requis, pour créer l'interface `utun` et modifier les routes.

## 3. Côté USB : parler RNDIS

### Trouver le téléphone

Le driver parcourt les appareils USB à la recherche d'une interface de contrôle RNDIS :

| Classe / sous-classe / protocole | Vu sur |
|----------------------------------|--------|
| `0xE0 / 0x01 / 0x03` (contrôleur sans fil, RNDIS) | La plupart des Android (Samsung Galaxy S24 Ultra : `04e8:6864`) |
| `0xEF / 0x04 / 0x01` (divers, RNDIS) | Certains appareils |
| `0x02 / 0x02 / 0xFF` (CDC ACM, protocole propriétaire) | Anciens appareils |

Il cherche ensuite l'**interface de données** correspondante (classe `0x0A`) avec ses deux points d'accès « bulk » (entrée/sortie), plus le point d'interruption de l'interface de contrôle. S'il ne trouve que du CDC-ECM/NCM, il indique que macOS le gère nativement.

Un téléphone en mode normal (MTP) a un autre identifiant USB (`04e8:6860` sur le S24 Ultra) et aucune interface RNDIS : c'est pourquoi le partage de connexion USB doit d'abord être activé.

### La négociation RNDIS

Les messages de contrôle passent par le point de contrôle (`SEND_ENCAPSULATED_COMMAND` / `GET_ENCAPSULATED_RESPONSE`). Les entiers sont en little-endian.

| Étape | Message | Rôle |
|-------|---------|------|
| 1 | `INITIALIZE` (0x02) | Négocier la version et la taille maximale de transfert |
| 2 | `QUERY` (0x04) pour `OID_802_3_PERMANENT_ADDRESS` | Le téléphone indique l'adresse MAC à utiliser côté Mac |
| 3 | `SET` (0x05) pour `OID_GEN_CURRENT_PACKET_FILTER` = `0x0D` | Recevoir les trames dirigées, broadcast et multicast |
| … | `KEEPALIVE` (0x08) | Le téléphone envoie des pings ; on répond sur le canal de contrôle, sinon il nous lâche |
| fin | `HALT` (0x03) | Arrêt propre |

### Faire passer les paquets

Chaque trame Ethernet est enveloppée dans un message RNDIS `PACKET` : un en-tête de 44 octets (type `0x01`, longueur totale, décalage et longueur des données, …) suivi de la trame. Deux détails importants :

- Une lecture « bulk » peut contenir **plusieurs** paquets RNDIS à la suite ; le driver les analyse en boucle.
- Quand la longueur d'une écriture « bulk » est un multiple exact de la taille maximale de paquet du point d'accès, un **paquet de longueur nulle** doit suivre, sinon le téléphone attend la suite.

## 4. Côté réseau : d'Ethernet à IP et retour

Une interface `utun` ne transporte que des **paquets IP**, chacun précédé de 4 octets indiquant la famille d'adresses (IPv4 ou IPv6). Le téléphone parle **Ethernet**. Le driver traduit :

- **Téléphone → Mac** : retirer l'en-tête Ethernet de 14 octets (et le remplissage éventuel, grâce au champ longueur d'IP), ajouter l'en-tête de famille de 4 octets, écrire dans `utun`.
- **Mac → téléphone** : lire un paquet IPv4 ou IPv6 depuis `utun`, retirer l'en-tête de famille, ajouter un en-tête Ethernet (destination : la MAC du téléphone, source : la nôtre ; pour l'IPv6 multicast, `33:33:` suivi des quatre derniers octets de l'adresse), envelopper en RNDIS, envoyer par USB.

Comme il n'y a pas de vraie carte réseau, le driver joue aussi les rôles de sa pile réseau :

- **Client DHCP** (RFC 2131, `src/dhcp.c`) : envoie `DISCOVER`, prend l'`OFFER`, envoie `REQUEST`, reçoit l'`ACK`, en trames Ethernet/IP/UDP brutes avec le drapeau broadcast, puisqu'on n'a pas encore d'adresse. Il lit adresse, masque, routeur, DNS, durée du bail et les T1/T2 du serveur. Le bail suit ensuite la norme : à **T1** (option 58, sinon 50 % du bail) une requête `REQUEST` en **unicast** au serveur avec notre adresse dans `ciaddr` et sans les options 50/54 ; sans réponse, elle est retransmise selon le délai de la norme ; à **T2** (option 59, sinon 87,5 %) elle devient une requête en **broadcast** ; si le bail expire, ou si le serveur répond `NAK` ou avec une autre adresse, la session est relancée de zéro. Toute la machine à états est pilotée par une horloge injectée et testée sans attendre.
- **ARP** : répond à « qui a notre IP ? » venant du téléphone, et apprend la MAC du téléphone à partir de l'ARP et du trafic IP.

### IPv6

Même principe pour IPv6, avec ICMPv6 qui joue le rôle d'ARP et de DHCP :

- **Sollicitation / annonce de routeur** : peu après la connexion, le driver envoie une sollicitation de routeur (`ff02::2`). Le téléphone répond par une annonce portant un préfixe `/64`. Le driver fabrique une adresse avec ce préfixe et un identifiant d'interface aléatoire (SLAAC), et la configure avec `ifconfig inet6`. Seule la première annonce utilisable est retenue par session.
- **Découverte de voisins** : le téléphone demande « qui a cette adresse ? » par des sollicitations de voisin. Il est le seul autre hôte du lien : le driver répond donc à toute demande valide par une annonce de voisin portant notre adresse MAC, faute de quoi le téléphone ne pourrait pas nous renvoyer de paquets. Les demandes invalides (mauvaise limite de sauts, source non spécifiée, cible multicast…) sont ignorées.
- **Checksums** : les checksums ICMPv6 couvrent un pseudo-en-tête IPv6 ; `icmp6_csum` les calcule et les tests la recoupent avec une implémentation indépendante.
- Tout ce qui est lu sur le réseau est vérifié en bornes et fuzzé, comme les autres analyseurs.
- `-4` désactive tout ceci.

## 5. Intégration à macOS

Après le DHCP, le driver lance `ifconfig` pour fixer l'adresse de l'interface `utun` (point à point vers la passerelle du téléphone, MTU 1500).

### Routes (`-r`)

Au lieu de remplacer la route par défaut, il ajoute deux routes **plus précises**, `0.0.0.0/1` et `128.0.0.0/1`, via le téléphone. Ensemble elles couvrent tout IPv4 et priment sur la route par défaut, mais celle-ci reste intacte. Retirer ces deux routes remet tout comme avant. Quand une adresse IPv6 a été configurée, la même méthode s'applique avec `::/1` et `8000::/1`, pour que le trafic IPv6 ne contourne pas le tethering par le Wi-Fi.

### DNS

Le DNS a été la partie délicate. Une première version déclarait un résolveur *supplémentaire* dans SystemConfiguration : ça marchait tant que le Wi-Fi était allumé, mais Wi-Fi coupé, macOS n'avait plus de résolveur *par défaut* et les noms ne se résolvaient plus.

La solution est de décrire le tethering comme un **service réseau**, comme le font les clients VPN, en écrivant avec `scutil` les entrées `State:/Network/Service/rndis-tether/IPv4` (adresse, routeur, interface) et `.../DNS` (le serveur DNS du téléphone). macOS l'élit alors service principal quand plus rien d'autre n'a de route, et utilise son DNS. Les deux entrées sont retirées à la sortie.

Dès qu'une adresse IPv6 existe, une troisième entrée, `.../IPv6` (adresse, longueur de préfixe, adresse link-local du routeur, interface), est ajoutée. Ce n'est pas cosmétique : sans elle, macOS n'envoie que des requêtes `A` au résolveur du tethering, et les noms qui n'existent qu'en IPv6 ne se résolvent pas (`scutil --dns` n'affiche que « Request A records »). Avec elle, le résolveur affiche aussi « Request AAAA records ».

## 6. Fonctionnement en service

`install.sh` met en place deux tâches launchd :

- **LaunchDaemon** (root, démarre au boot) : lance `rndis-tether -r -w -a -U <ton utilisateur>`.
  - `-w` : attendre un téléphone et se reconnecter après un débranchement.
  - `-a` : si aucun appareil RNDIS n'est trouvé, basculer le téléphone en partage de connexion avec `adb shell svc usb setFunctions rndis`, exécuté **en tant que ton utilisateur** (donc avec tes clés ADB et ton PATH, pas ceux de root). `adb` signale souvent une erreur parce que le bus USB se ré-énumère pendant la bascule ; c'est ignoré.
- **LaunchAgent** (ta session) : l'app de barre de menus.

**Sessions mortes.** Si le téléphone disparaît ou se ré-énumère, les appels USB renvoient une erreur d'E/S ou « pas d'appareil », ou simplement un timeout. Le driver considère une erreur d'appareil, ou cinq timeouts d'envoi de suite, comme une session morte : il retire routes et DNS, libère l'interface et retourne en attente.

**Barre de menus ↔ service.** L'app n'est pas root, elle ne parle donc pas directement au service. Ils partagent deux fichiers dans `/Users/Shared` :

| Fichier | Écrit par | Signification |
|---------|-----------|---------------|
| `rndis-tether.disabled` | l'app de menu | S'il existe, le service reste inactif (et démonte toute session active) |
| `rndis-tether.status` | le service, toutes les ~5 s | `waiting`, `connecting`, `connected <ip> <iface>`, `disabled`, … |

Si le fichier d'état a plus de 20 secondes, l'icône affiche « service arrêté ».

### Séparation des privilèges

Le service est composé de deux processus, pour que le code qui lit des données hostiles ne tourne jamais en root :

| | Superviseur | Worker |
|---|---|---|
| Tourne en | root | `nobody` (ou l'utilisateur donné avec `-W`) |
| Fait | crée l'interface `utun` ; applique `ifconfig`/`route`/`scutil` après validation ; les retire à la fin ; écrit le fichier d'état ; lance `adb` | USB, RNDIS, DHCP, ARP, NDP, relais des paquets |
| Lit des données hostiles | non | oui |

Fonctionnement :

1. Le superviseur (`supervisor_main`) résout l'utilisateur du worker, puis fait un `fork()`. L'enfant (`worker_main`) ferme tous les descripteurs hérités sauf le canal, appelle `setgroups`/`setgid`/`setuid`, et **vérifie qu'il ne peut plus redevenir root** (`setuid(0)` doit échouer) ; sinon il refuse de fonctionner.
2. Les deux processus dialoguent par une `socketpair` avec des **messages de taille fixe** (`ipc.h`) et passage de descripteur : le superviseur crée l'interface `utun` et en transmet le descripteur au worker, qui lit et écrit les paquets sans aucun privilège.
3. Le worker ne peut faire que cinq demandes : créer l'interface, appliquer une configuration IPv4, une configuration IPv6, signaler un état, et activer le tethering via `adb`. **Le superviseur valide chaque valeur** (`policy.c`) : la passerelle doit être dans le sous-réseau attribué, les adresses unicast et routables, le texte d'état limité à quelques formes exactes, etc. Les commandes sont construites à partir de nombres validés, jamais de texte envoyé par le worker.
4. Le superviseur retient ce qu'il a appliqué (routes, DNS, entrées du service), donc le nettoyage ne dépend jamais du worker. Un plantage, un débranchement ou un message mal formé termine ce worker ; le superviseur nettoie et en démarre un neuf. **Il y a un worker par session** : aucun état ne survit d'une session à l'autre.

Ce que cela apporte, et ses limites :

- Un bogue dans le code USB, RNDIS, DHCP ou paquets (celui qui analyse les données du téléphone et du réseau) n'est plus une faille root : l'attaquant obtient l'utilisateur `nobody`, pas root.
- Un worker compromis peut encore envoyer du trafic dans le tunnel et demander une configuration *valide* (ce qu'un téléphone malveillant avec un serveur DHCP peut déjà faire). Il ne peut pas exécuter de commandes, écrire des fichiers en root ni changer des réglages système arbitraires.
- Le worker n'est pas encore confiné par un bac à sable (il pourrait ouvrir des fichiers lisibles par `nobody` ou faire des connexions réseau). Un profil de sandbox macOS figure dans la [feuille de route](../ROADMAP.fr.md).

## 7. Compromis et limites

- **Débit** : les paquets traversent la frontière noyau/espace utilisateur et sont copiés ; un pilote natif serait un peu plus rapide. Sur un Galaxy S24 Ultra, environ 42 Mbit/s ont été mesurés, probablement limités par le réseau mobile plutôt que par le driver.
- **Service permanent** : si le processus s'arrête, le réseau disparaît (il redémarre automatiquement sous launchd).
- **IPv6 dépend du téléphone** : il faut une annonce de routeur avec un préfixe `/64` (validé sur un Galaxy S24 Ultra, dont le réseau mobile fournit l'IPv6). Sans elle, rien n'est configuré en IPv6 et le tethering reste en IPv4 seul. Les serveurs DNS IPv6 annoncés par le routeur (RDNSS) ne sont pas utilisés : les noms sont résolus par le DNS IPv4 du téléphone, qui répond aussi aux requêtes `AAAA`. Aucune différence de vitesse entre IPv4 et IPv6 n'a été mesurable ; le réseau mobile domine.
- **Testé sur un seul téléphone** (Galaxy S24 Ultra, Android 16, One UI 8). Des téléphones aux particularités RNDIS différentes peuvent demander des adaptations.
- **Tout le trafic passe par le téléphone** pendant la connexion avec `-r`, même si le Wi-Fi est allumé : ça consomme des données mobiles.
- **Sécurité** : seul un petit superviseur tourne en root ; tout ce qui analyse des données du téléphone ou du réseau tourne dans un worker sans privilèges (voir *Séparation des privilèges*). Le superviseur ne transmet des valeurs à des commandes externes qu'après les avoir validées et mises sous forme d'adresses numériques.

## 8. Où regarder dans le code

| Sujet | Fonction(s) (`src/main.c` sauf mention) |
|-------|-------------------------|
| Détection de l'appareil | `find_device`, `is_rndis_ctrl` |
| Négociation RNDIS | `rndis_cmd`, `rndis_init`, `rndis_halt`, `answer_keepalive` |
| Flux de données USB | `send_frame`, `rx_parse`, `rx_thread`, `tx_thread`, `ctrl_thread` |
| Ethernet ↔ IP | `rx_frame`, `tx_thread` |
| DHCP / ARP | `dhcp_build`, `dhcp_build_renew`, `dhcp_parse` (`src/dhcp.c`) ; `dhcp_acquire`, `handle_arp` |
| Cycle de vie du bail (T1, T2, expiration) | `dhcp_client_bound`, `dhcp_client_tick`, `dhcp_client_input` (`src/dhcp.c`) |
| IPv6 (NDP, RA, SLAAC) | `icmp6_csum`, `build_na`, `parse_ra`, `build_rs`, `handle_ipv6`, `configure_ipv6` |
| `utun`, routes, DNS | `utun_open`, `configure_iface`, `dns_apply`, `unconfigure_iface` |
| Contrôle du service | `is_disabled`, `set_status`, `status_tick`, `adb_enable`, `session`, `main` |
| Séparation des privilèges (côté root) | `supervisor_main`, `run_worker_once`, `handle_request` |
| Validation des demandes / IPC | `policy_check_v4`, `policy_check_v6`, `policy_check_status` (`src/policy.c`) ; `ipc_send`, `ipc_recv` (`src/ipc.c`) ; côté worker : `worker_main`, `drop_privileges`, `ipc_call` |
