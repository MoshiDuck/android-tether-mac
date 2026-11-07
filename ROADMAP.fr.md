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
| Sécurité | ✅ 3 bogues mémoire trouvés par fuzzing et corrigés ; ✅ séparation des privilèges (validée en réel) ; ✅ DHCP conforme RFC 2131 (validé en réel) |
| Téléphones | un seul : Samsung Galaxy S24 Ultra (SM-S928B, Android 16) |

---

## 0.2.0 — Sûr par construction  *(publiée le 2026-11-06)*

Objectif : un bogue dans le code qui lit des données du téléphone ou du réseau ne peut plus donner root.

- ✅ **Séparation des privilèges** — un petit superviseur root et un worker sans privilèges (`nobody`) ; IPC à messages de taille fixe ; chaque demande validée par `policy.c` ; un worker neuf par session. *Validée en réel sur le S24 Ultra (2026-11-06) :* `nobody` peut ouvrir l'appareil USB ; le service à deux processus a fait passer IPv4 et IPv6, résisté à l'interrupteur de l'icône et à une vraie coupure USB, et nettoyé routes et DNS à chaque fois. L'option `-W` reste disponible pour les systèmes où ce ne serait pas le cas.
- ✅ **DHCP selon la RFC 2131** — renouvellement en unicast à T1, rebind en broadcast à T2, retransmissions, gestion de l'expiration et du NAK. *Validé en réel sur le S24 Ultra (2026-11-06) :* le renouvellement unicast à T1 a été accepté (`bail renouvelé`). Le rebind à T2 et l'expiration ne sont couverts que par des tests à horloge simulée.

Publiée en 0.2.0. Les deux points ont été validés sur le Galaxy S24 Ultra ; le reste du durcissement passe en 0.3.0 et 0.4.0.

## 0.3.0 — Durcissement, première partie  *(publiée le 2026-11-06)*

Objectif : fermer les failles faciles à fermer et à forte valeur, laissées après la séparation des privilèges.

- ✅ **`DHCPRELEASE` à l'arrêt**, pour ne pas laisser de baux orphelins dans le pool d'adresses du téléphone.
- ✅ **Garde contre le chevauchement de sous-réseau** — refuser un sous-réseau du téléphone qui recouvre un réseau déjà utilisé par le Mac (un téléphone malveillant pourrait sinon masquer un réseau local). Message d'erreur clair, car un conflit légitime bloquerait le tethering.
- ✅ **Limitation de débit de l'IPC** — un worker qui envoie plus de 30 demandes en 10 secondes est tué et remplacé (un worker normal en fait moins de dix par session).
- ✅ **`--version` et une page de manuel.**

Publiée en 0.3.0. Validée sur le Galaxy S24 Ultra : `DHCPRELEASE` à la fin d'une session (ligne du journal), `--version` et la page de manuel, et aucun faux refus de la garde de sous-réseau, y compris avec le vrai Wi-Fi du Mac connecté. La limite de débit n'est couverte que par les tests.

## 0.4.0 — Confinement et observabilité

- 🟡 **Durcissement de l'abandon de privilèges** — ✅ fait : pas de fichier core pour les deux processus, le worker est limité à 128 descripteurs de fichiers, et il vérifie déjà qu'il ne peut pas retrouver root (`setuid(0)` doit échouer). Volontairement non fait : `RLIMIT_FSIZE` (le worker écrit dans le journal par un descripteur hérité, une limite nulle le tuerait) et `RLIMIT_NPROC` (compté par utilisateur, partagé avec tous les autres processus de `nobody`, donc impossible à régler de façon fiable) ; le profil de bac à sable ci-dessous est le bon outil pour les deux.
- ✅ **Chien de garde du worker** — le worker envoie un signe de vie toutes les 10 s, et seulement si ses trois threads (réception, émission, contrôle) ont tourné depuis le précédent ; le superviseur tue un worker silencieux depuis 60 s, nettoie la configuration réseau et en relance un neuf. Testé avec un faux worker muet et un faux worker bavard.
- ✅ **Hygiène des journaux** — le worker ne tient plus le fichier journal : ses sorties passent par un tube, et le superviseur les recopie après avoir retiré les caractères de contrôle, tronqué les lignes trop longues et limité le débit (100 lignes par 10 s, avec le nombre de lignes supprimées), donc un worker détourné ne peut ni se faire passer pour le superviseur ni remplir le disque. `newsyslog` fait tourner le fichier au-delà de 1 Mo (cinq archives compressées) et le superviseur le rouvre. Testé avec de faux workers et des processus forkés ; la rotation par le vrai `newsyslog` n'est pas encore vérifiée sur un Mac.
- ⬜ **Profil de bac à sable macOS pour le worker** (Seatbelt, tout refusé par défaut ; seulement les classes IOKit USB, le socket IPC et la lecture du fichier d'état). Même un worker compromis ne pourrait plus ouvrir de fichiers ni de sockets. *Premier essai (2026-11-07) :* `sandbox_init` avec un profil « tout refusé » bloque bien la lecture de fichiers hors des chemins autorisés, mais avec seulement les deux classes USB autorisées, libusb ne trouve alors plus aucun appareil, et `socket()` reste permis. Il faut donc itérer sur un Mac avec root et le téléphone branché (ouvrir l'appareil est impossible en utilisateur normal) ; reporté à une version suivante plutôt que livré sans test.
- ✅ **`rndis-tether --diagnose`** — une commande, sans `sudo`, qui produit un rapport partageable : état du service (superviseur et worker), interfaces USB, état d'ADB, routes, DNS, fin du journal et diagnostic en langage clair. Adresses MAC, IP publiques et numéros de série masqués. Reste à ajouter : la négociation RNDIS et l'échange DHCP de la dernière session.
- 🟡 **Gestion veille/réveil** — ✅ fait : le superviseur remarque un réveil (écart de 15 s ou plus entre deux de ses tours d'une seconde), termine la session proprement et en lance une neuve qui retrouve le téléphone et refait DHCP ; l'horloge du bail compte déjà la veille (`CLOCK_MONOTONIC` continue sous macOS). Testé avec un saut d'horloge simulé. Pas encore vérifié sur une vraie veille du Mac avec le S24 Ultra.
- 🟡 **IPv6 complet** — ✅ fait : la durée de vie du routeur et le retrait du préfixe sont suivis, donc un IPv6 que le téléphone ne fournit plus est retiré et la garde revient, et une sollicitation de routeur part toutes les 30 s tant qu'il n'y a pas d'IPv6, pour le reprendre s'il revient. Un changement de préfixe est suivi aussi (quand le routeur n'annonce plus le nôtre mais un autre, tout est retiré et l'annonce suivante installe le nouveau). Reste : serveurs DNS RDNSS, adresses temporaires optionnelles.
- ✅ **MTU** — le MTU annoncé par le téléphone est appliqué : option 26 du DHCP (désormais demandée) et option MTU de l'annonce de routeur IPv6, le plus petit des deux l'emportant, validé par le superviseur (576 à 1500 en IPv4, 1280 à 1500 en IPv6 ; jamais au-delà de 1500, la taille des tampons du driver). L'écrêtage du MSS TCP n'est volontairement pas fait : le Mac est une machine terminale, pas un routeur, son noyau déduit déjà le MSS annoncé du MTU de l'interface. Testé avec des valeurs forgées des deux côtés ; pas encore vu avec un téléphone qui annonce un MTU plus petit (le S24 Ultra n'en annonce aucun).
- ✅ **`adb` robuste** — avant de basculer le téléphone, le superviseur regarde ce qu'`adb devices` voit et écrit dans le journal, une fois, ce qu'il faut faire : adb introuvable, aucun téléphone, téléphone non autorisé, hors ligne, plusieurs appareils prêts. `-S numéro_de_série` désigne le bon téléphone (`adb -s`). La lecture de la sortie d'adb est un module pur (`src/adb.c`), testé à 100 % avec des sorties forgées. Pas encore vu avec un vrai téléphone non autorisé ou plusieurs appareils.
- ✅ **Garde contre la fuite IPv6** *(validée en réel sur le S24 Ultra le 2026-11-07 : avec le Wi-Fi du téléphone allumé, les routes « reject » sont posées et `curl -6` échoue en 0,05 s pendant que l'IPv4 passe par le téléphone)* — avec `-r`, quand le tethering ne fournit pas d'IPv6 (par exemple parce que l'accès Internet du téléphone vient d'un Wi-Fi) mais qu'un autre réseau du Mac en a, le trafic IPv6 contourne le téléphone. Trouver une façon sûre de le bloquer (pour que les applications se rabattent sur l'IPv4 via le téléphone), et la tester sur un vrai téléphone.

Critères de sortie : `--diagnose` cité dans les modèles de tickets (fait) ; le nouveau comportement du service installé (worker sous ses limites, journal par le tube, chien de garde, MTU, messages d'`adb`) et une vraie veille/réveil vérifiés sur un Mac avec un téléphone ; le profil de bac à sable passe à la version suivante s'il n'est pas validé d'ici là.

## 0.5.0 — Tests d'intégration sans matériel

Aujourd'hui `main.c` n'est couvert qu'à 44 % ; la partie non testée est justement la boucle USB et de session que les vrais téléphones sollicitent.

- ⬜ **Couche d'abstraction USB** — placer libusb derrière une petite interface (`usb_ops`) pour que le code de session tourne face à un **faux appareil**.
- ⬜ **Faux téléphone RNDIS** — un double de test qui parle la négociation RNDIS, répond au DHCP (NAK et renouvellement compris), envoie des annonces de routeur, répond au NDP, et se comporte mal exprès (messages tronqués, trop grands, désordonnés, disparition en pleine session). Tourne en CI sans téléphone.
- ⬜ **Tâche ThreadSanitizer** — une fois les sessions sous test, vérifier l'absence de courses de données dans la conception à trois threads.
- ⬜ **Cibles de fuzzing libFuzzer** (LLVM de Homebrew), corpus de départ, fuzzing continu (OSS-Fuzz quand le projet y sera éligible).
- ⬜ **Modularisation** — scinder `main.c` en `rndis`, `usbdev`, `ndp`, `session`, `supervisor`, `worker` ; remplacer les crochets `RNDIS_TESTING` par de l'injection de dépendances.
- ⬜ **Tests de mutation** sur `dhcp.c` et `policy.c` pour prouver que les tests mordent vraiment.
- ⬜ **Tests de l'app de menu** (XCTest) : lecture de l'état et interrupteur.

Critères de sortie : `main.c` ≥ 80 % des lignes ; le faux téléphone couvre le cas nominal, chaque défaillance ci-dessus et le renouvellement ; le fuzzing tourne à intervalles réguliers.

## 0.6.0 — Performance

Mesuré jusqu'ici : ≈ 42 Mbit/s, limités par le réseau mobile et non par le driver. Avant d'optimiser, mesurer le driver seul.

- ⬜ **Banc de mesure** — débit et latence en boucle locale face au faux téléphone, et `iperf3` face à un vrai téléphone sans Wi-Fi, consignés à chaque version.
- ⬜ **Agrégation RNDIS** — plusieurs paquets par transfert bulk (le S24 Ultra annonce 23 700 octets par transfert) ; aujourd'hui chaque paquet est un transfert.
- ⬜ **Réception asynchrone** — plusieurs transferts libusb en vol au lieu d'une lecture bloquante.
- ⬜ **Moins d'appels système par paquet** et classes de QoS pour les threads.

Critères de sortie : chiffres documentés ; pas d'autre garde-fou de régression en CI qu'un seuil large.

## 0.7.0 — Confort

- ⬜ **Mode secours (failover)** — laisser la route par défaut tranquille et ne faire du tethering le service principal que si le Wi-Fi tombe (classer le service SystemConfiguration au lieu d'ajouter des routes scindées). Aujourd'hui `-r` fait passer *tout* par le téléphone, ce qui consomme des données mobiles.
- ⬜ **Wi-Fi et tethering en même temps.** Aujourd'hui c'est l'un ou l'autre : une connexion passe par un seul lien, donc deux liens ne s'additionnent pas pour un seul téléchargement. Du plus réaliste au plus ambitieux : (1) le mode *secours* ci-dessus garde le tethering en réserve ; (2) un *équilibrage par flux* avec `pf` (`route-to`, collant par connexion) répartit plusieurs connexions simultanées sur les deux liens — utile pour des téléchargements en parallèle, pas pour un gros fichier unique, et certains sites n'aiment pas qu'un client change d'adresse ; (3) un vrai *regroupement* d'une seule connexion exige un serveur d'agrégation (de type MPTCP) et sort du cadre de ce projet. *Mesuré le 2026-11-06 (Mac sur Wi-Fi, téléphone en LTE, deux lignes indépendantes, trois courtes manches de téléchargements de 20 Mo, une connexion par lien en même temps) : environ 95 Mbit/s par le tethering seul, 70 par le Wi-Fi seul, et 172 avec les deux à la fois — proche de la somme, ce qui montre que l'option (2) mérite un prototype. Une seule connexion n'utilise toujours qu'un lien.* adresses IPv4/IPv6, compteurs de trafic, notifications de connexion/déconnexion, « ouvrir à la connexion », choix du mode (toujours / secours / arrêt).
- ⬜ **Fichier de réglages** (`/etc/rndis-tether.conf`) au lieu d'arguments figés dans un plist launchd.
- ⬜ **Localisation** (anglais, français).
- ⬜ **Alerte de consommation** — afficher l'état « données limitées » du téléphone et un plafond configurable.

## 0.8.0 — Compatibilité

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
| `nobody` ne peut pas ouvrir d'appareil USB sur certaines versions de macOS | Fonctionne sur le macOS 26.5 testé (sonde : ouverture réussie sous `nobody`). L'option `-W` reste pour les autres cas ; envisager un utilisateur de service dédié créé par `install.sh`. |
| macOS durcit `utun`, IOKit ou launchd | Garder la partie privilégiée minuscule ; tester chaque bêta de macOS ; la CI tourne sur deux versions. |
| Android abandonne RNDIS pour NCM | Le projet devient inutile pour ces téléphones, ce qui est très bien ; suivre le support par appareil et le dire dans le README. |
| Un téléphone malveillant | Il peut déjà proposer n'importe quelle configuration DHCP. Le superviseur valide ce qu'il applique, et la garde contre le chevauchement de sous-réseau ferme le cas du réseau local masqué. |
| Un seul mainteneur, un seul téléphone de test | Rapports de téléphones, un second mainteneur, un faux téléphone en CI. |

## Comment influencer cette feuille de route

Ouvrir un ticket avec le formulaire *Phone report* (la contribution la plus précieuse), un rapport de bogue avec la sortie de `--diagnose` quand il existera, ou une pull request : voir [CONTRIBUTING.md](CONTRIBUTING.md).
