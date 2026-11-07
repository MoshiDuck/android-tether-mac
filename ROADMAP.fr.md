# Feuille de route

*English: [ROADMAP.md](ROADMAP.md)*

Où va TetherMac, dans l'ordre qui compte. Elle part de ce qui a été mesuré, pas de ce qu'on espère : un seul téléphone testé, `main.c` couvert à 44 % par les tests, un service root qui lit des données hostiles. Une seule règle fixe les priorités : **sûreté et exactitude d'abord, puis compatibilité, puis confort, puis diffusion.**

Légende : ✅ fait · 🔶 fait dans le code, en attente de validation sur matériel réel · ⬜ prévu

## Où nous en sommes

| Domaine | État |
|---------|------|
| Cœur | ✅ RNDIS en espace utilisateur (libusb + `utun`), DHCP, ARP, IPv4 et IPv6, DNS, routes, reconnexion |
| Service | ✅ LaunchDaemon + icône de menu, activation par `adb`, tap Homebrew, canal `--HEAD` |
| Qualité | ✅ avertissements stricts, analyse statique, tests ASan/UBSan (1,3 M de vérifications), seuils de couverture, CI sur 2 versions de macOS |
| Sécurité | ✅ 3 bogues mémoire trouvés par fuzzing et corrigés ; 🔶 séparation des privilèges ; 🔶 DHCP conforme RFC 2131 |
| Téléphones | un seul : Samsung Galaxy S24 Ultra (SM-S928B, Android 16) |

---

## 0.2.0 — Sûr par construction  *(prochaine version)*

Objectif : un bogue dans le code qui lit des données du téléphone ou du réseau ne peut plus donner root.

- 🔶 **Séparation des privilèges** — un petit superviseur root et un worker sans privilèges (`nobody`) ; IPC à messages de taille fixe ; chaque demande validée par `policy.c` ; un worker neuf par session. *À valider en réel :* que macOS laisse `nobody` ouvrir l'appareil USB (sinon, repli sur un utilisateur dédié ou l'utilisateur de la session via `-W`).
- 🔶 **DHCP selon la RFC 2131** — renouvellement en unicast à T1, rebind en broadcast à T2, retransmissions, gestion de l'expiration et du NAK. *À valider en réel :* un renouvellement unicast auprès du serveur DHCP du téléphone.
- ⬜ **`DHCPRELEASE` à l'arrêt**, pour ne pas laisser de baux orphelins dans le pool d'adresses du téléphone.
- ⬜ **Garde contre le chevauchement de sous-réseau** — refuser un sous-réseau du téléphone qui recouvre un réseau déjà utilisé par le Mac (un téléphone malveillant pourrait sinon masquer un réseau local). Message d'erreur clair, car un conflit légitime bloquerait le tethering.
- ⬜ **Durcissement de l'abandon de privilèges** — limites de ressources (`RLIMIT_NPROC`, `RLIMIT_FSIZE`) et auto-test au démarrage prouvant que le worker a bien perdu root.
- ⬜ **Limitation de débit de l'IPC et chien de garde** — un worker qui inonde le superviseur, ou qui ne répond plus, est tué et remplacé.
- ⬜ **Hygiène des journaux** — droits et rotation de `/var/log/rndis-tether.log` (`newsyslog`) ; pas d'adresses d'autres appareils dans les logs.
- ⬜ **`--version` et une page de manuel.**

Critères de sortie : tout ce qui précède au vert en CI ; séparation des privilèges et DHCP validés sur le S24 Ultra ; le `CHANGELOG` signale le changement de modèle de sécurité.

## 0.3.0 — Confinement et observabilité

- ⬜ **Profil de bac à sable macOS pour le worker** (Seatbelt, tout refusé par défaut ; seulement les classes IOKit USB, le socket IPC et la lecture du fichier d'état). Même un worker compromis ne pourrait plus ouvrir de fichiers ni de sockets.
- ⬜ **`rndis-tether --diagnose`** — une commande qui produit un rapport partageable (descripteurs USB, résultat de la négociation RNDIS, échange DHCP, RA reçue, routes, état du DNS), sans secrets. C'est ce qui transforme un « ça ne marche pas » en problème réparable.
- ⬜ **Gestion veille/réveil** — détecter le réveil du Mac, revérifier le bail et la liaison USB, reprendre proprement.
- ⬜ **IPv6 complet** — suivre durées de vie et changements de préfixe des RA (aujourd'hui seule la première est utilisée), serveurs DNS RDNSS, option MTU de la RA, adresses temporaires optionnelles.
- ⬜ **MTU et MSS TCP** — respecter la MTU du chemin, écrêtage du MSS optionnel pour les réseaux qui jettent les fragments.
- ⬜ **`adb` robuste** — plusieurs appareils (`adb -s`), aucun autorisé, débogage USB désactivé : des messages clairs plutôt qu'un échec silencieux.

Critères de sortie : le bac à sable actif par défaut ; `--diagnose` cité dans les modèles de tickets ; veille/réveil testé.

## 0.4.0 — Tests d'intégration sans matériel

Aujourd'hui `main.c` n'est couvert qu'à 44 % ; la partie non testée est justement la boucle USB et de session que les vrais téléphones sollicitent.

- ⬜ **Couche d'abstraction USB** — placer libusb derrière une petite interface (`usb_ops`) pour que le code de session tourne face à un **faux appareil**.
- ⬜ **Faux téléphone RNDIS** — un double de test qui parle la négociation RNDIS, répond au DHCP (NAK et renouvellement compris), envoie des annonces de routeur, répond au NDP, et se comporte mal exprès (messages tronqués, trop grands, désordonnés, disparition en pleine session). Tourne en CI sans téléphone.
- ⬜ **Tâche ThreadSanitizer** — une fois les sessions sous test, vérifier l'absence de courses de données dans la conception à trois threads.
- ⬜ **Cibles de fuzzing libFuzzer** (LLVM de Homebrew), corpus de départ, fuzzing continu (OSS-Fuzz quand le projet y sera éligible).
- ⬜ **Modularisation** — scinder `main.c` en `rndis`, `usbdev`, `ndp`, `session`, `supervisor`, `worker` ; remplacer les crochets `RNDIS_TESTING` par de l'injection de dépendances.
- ⬜ **Tests de mutation** sur `dhcp.c` et `policy.c` pour prouver que les tests mordent vraiment.
- ⬜ **Tests de l'app de menu** (XCTest) : lecture de l'état et interrupteur.

Critères de sortie : `main.c` ≥ 80 % des lignes ; le faux téléphone couvre le cas nominal, chaque défaillance ci-dessus et le renouvellement ; le fuzzing tourne à intervalles réguliers.

## 0.5.0 — Performance

Mesuré jusqu'ici : ≈ 42 Mbit/s, limités par le réseau mobile et non par le driver. Avant d'optimiser, mesurer le driver seul.

- ⬜ **Banc de mesure** — débit et latence en boucle locale face au faux téléphone, et `iperf3` face à un vrai téléphone sans Wi-Fi, consignés à chaque version.
- ⬜ **Agrégation RNDIS** — plusieurs paquets par transfert bulk (le S24 Ultra annonce 23 700 octets par transfert) ; aujourd'hui chaque paquet est un transfert.
- ⬜ **Réception asynchrone** — plusieurs transferts libusb en vol au lieu d'une lecture bloquante.
- ⬜ **Moins d'appels système par paquet** et classes de QoS pour les threads.

Critères de sortie : chiffres documentés ; pas d'autre garde-fou de régression en CI qu'un seuil large.

## 0.6.0 — Confort

- ⬜ **Mode secours (failover)** — laisser la route par défaut tranquille et ne faire du tethering le service principal que si le Wi-Fi tombe (classer le service SystemConfiguration au lieu d'ajouter des routes scindées). Aujourd'hui `-r` fait passer *tout* par le téléphone, ce qui consomme des données mobiles.
- ⬜ **Barre de menus** : modèle du téléphone, adresses IPv4/IPv6, compteurs de trafic, notifications de connexion/déconnexion, « ouvrir à la connexion », choix du mode (toujours / secours / arrêt).
- ⬜ **Fichier de réglages** (`/etc/rndis-tether.conf`) au lieu d'arguments figés dans un plist launchd.
- ⬜ **Localisation** (anglais, français).
- ⬜ **Alerte de consommation** — afficher l'état « données limitées » du téléphone et un plafond configurable.

## 0.7.0 — Compatibilité

- ⬜ **Matrice de téléphones** — Pixel, Xiaomi, OnePlus, Motorola, Sony et autres Samsung ; base d'identifiants USB avec les particularités de chaque appareil ; le formulaire *Phone report* alimente le tableau du README.
- ⬜ **La direction d'Android.** Android sait maintenant aussi faire du CDC-NCM pour le partage USB, que macOS gère déjà nativement, et RNDIS est la voie plus ancienne. Un téléphone qui propose NCM n'a pas besoin de ce projet. Suivre quels appareils n'ont encore que RNDIS, et garder un périmètre honnête : cet outil existe pour eux.
- ⬜ **Mac Intel et macOS plus anciens** — une compilation universelle (arm64 + x86_64), testée sur au moins une machine Intel.

## 1.0.0 — Quelque chose qu'on peut recommander

Critères, tous obligatoires :

1. Au moins **cinq modèles de téléphones différents** confirmés par des rapporteurs indépendants.
2. **Séparation des privilèges et bac à sable** actifs par défaut, relus par quelqu'un d'autre que l'auteur.
3. **Signé et notarisé** par Apple (Developer ID), distribué en installateur notarisé en plus de Homebrew.
4. **Tests d'intégration sans matériel** avec `main.c` ≥ 80 % des lignes, et fuzzing planifié.
5. Trois mois sans bogue grave connu.
6. Documentation complète : installation, dépannage (`--diagnose`), architecture, modèle de sécurité.

---

## Distribution et confiance (en continu)

- ✅ GitHub Release créée depuis le changelog à chaque tag ; `brew install` vérifié sur une machine propre.
- ⬜ **Tags signés** (SSH ou GPG) et sommes SHA-256 publiées.
- ⬜ **Signature Apple Developer ID** et notarisation de l'app de menu, puis un `.pkg` notarisé avec un binaire universel précompilé (libusb liée statiquement) : ni Homebrew ni outils de développement nécessaires.
- ⬜ **Bouteilles Homebrew** (binaires précompilés) depuis le tap ; plus tard un Cask pour l'app ; `homebrew-core` quand le projet atteindra son seuil de notoriété.
- ⬜ **Chaîne d'approvisionnement** — épingler les GitHub Actions par SHA, badge OpenSSF Scorecard, SBOM joint aux releases, protection de branche exigeant la CI.
- ⬜ **Outils de sécurité activés** — signalement privé de vulnérabilités, CodeQL, alertes Dependabot.

## Communauté et santé du projet (en continu)

- ⬜ Étiquettes « good first issue », GitHub Discussions, un court document de conception pour les contributeurs.
- ⬜ Un second mainteneur : une seule personne est un point de défaillance unique pour un service root.
- ⬜ Une annonce écrite (l'architecture et les failles trouvées par fuzzing), et des messages là où les utilisateurs d'Android et de Mac cherchent ce genre d'outil.

## Risques et inconnues

| Risque | Ce que nous faisons |
|--------|---------------------|
| `nobody` ne peut pas ouvrir d'appareil USB sur le macOS actuel | Valider d'abord (commande de test dans les notes de la 0.2.0) ; sinon un utilisateur de service dédié créé par `install.sh`, ou l'utilisateur de la session. |
| macOS durcit `utun`, IOKit ou launchd | Garder la partie privilégiée minuscule ; tester chaque bêta de macOS ; la CI tourne sur deux versions. |
| Android abandonne RNDIS pour NCM | Le projet devient inutile pour ces téléphones, ce qui est très bien ; suivre le support par appareil et le dire dans le README. |
| Un téléphone malveillant | Il peut déjà proposer n'importe quelle configuration DHCP. Le superviseur valide ce qu'il applique, et la garde contre le chevauchement de sous-réseau ferme le cas du réseau local masqué. |
| Un seul mainteneur, un seul téléphone de test | Rapports de téléphones, un second mainteneur, un faux téléphone en CI. |

## Comment influencer cette feuille de route

Ouvrir un ticket avec le formulaire *Phone report* (la contribution la plus précieuse), un rapport de bogue avec la sortie de `--diagnose` quand il existera, ou une pull request : voir [CONTRIBUTING.md](CONTRIBUTING.md).
