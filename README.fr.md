# TetherMac

**Partage de connexion USB pour téléphones Android sur macOS, sans extension noyau, compatible Apple Silicon.**

macOS n'a pas de pilote RNDIS intégré, et les kexts comme HoRNDIS ne se chargent plus sur macOS récent. TetherMac (`rndis-tether`) est un petit pilote en espace utilisateur : il parle RNDIS au téléphone via libusb, gère ARP et DHCP, et expose la liaison par une interface `utun`. Une icône dans la barre de menus permet de l'activer ou de le désactiver.

> **Android uniquement.** Les iPhone fonctionnent déjà nativement sur macOS (active le point d'accès personnel et branche le câble) : ce projet n'est pas nécessaire.

*English: [README.md](README.md)*

## Installation

### Homebrew (recommandé)

```sh
brew install MoshiDuck/tap/android-tether-mac
tethermac-install        # demande ton mot de passe ; installe le service + l'icône
```

Mise à jour : `brew upgrade android-tether-mac && tethermac-install`.

### Depuis les sources

```sh
brew install libusb
git clone https://github.com/MoshiDuck/android-tether-mac.git && cd android-tether-mac
sudo ./install.sh
```

Le script compile tout, installe un service (LaunchDaemon, démarre au boot) et l'icône de menu (`/Applications/RNDISTether.app`, lancée à l'ouverture de session). Désinstallation : `sudo ./uninstall.sh`.

> **Attention :** tant que le téléphone est connecté, le service fait passer **tout** le trafic par le téléphone (`-r`), même si le Wi-Fi est allumé. Ça consomme tes données mobiles. Utilise l'icône pour le désactiver.

## Utilisation manuelle

```sh
make
sudo ./rndis-tether -v -r -w -a
```

- `-r` route tout via le téléphone et applique son DNS (retirés à la sortie)
- `-w` attend le téléphone et se reconnecte après un débranchement
- `-a` active le Modem USB du téléphone via `adb` (débogage USB requis)
- `-4` IPv4 uniquement (désactive le relais IPv6)
- `-W user` utilisateur sans privilèges du processus worker (défaut `nobody`)
- `-L` avec `-r`, laisse l'IPv6 sortir par les autres réseaux du Mac quand le tethering n'en a pas (sinon elle est bloquée)
- `-O` accepte un sous-réseau du téléphone qui recouvre un réseau déjà utilisé (refusé par défaut)
- `-V`, `--version` affiche la version
- `-U user` utilisateur pour `adb` (défaut `$SUDO_USER`)
- `-d vid:pid` cible un appareil précis ; `-v` logs détaillés

Journal du service : `/var/log/rndis-tether.log`.

## Comment ça marche

Explication détaillée (USB, RNDIS, utun, DHCP, DNS, service, séparation des privilèges) : [docs/how-it-works.fr.md](docs/how-it-works.fr.md)

**Séparation des privilèges** : seul un petit superviseur tourne en root ; tout ce qui analyse des données du téléphone ou du réseau tourne dans un worker sans privilèges (`nobody`), et le superviseur valide chaque demande reçue. Un bogue dans l'analyse des paquets n'est donc plus une faille root.

Où va le projet : [ROADMAP.fr.md](ROADMAP.fr.md).

## Limites

- IPv6 demande que le téléphone annonce un préfixe `/64` (c'était le cas sur le téléphone testé, en données mobiles) ; sinon le tethering reste en IPv4. Android ne délègue pas de préfixe quand l'accès Internet du téléphone vient d'un Wi-Fi : si un autre réseau du Mac a de l'IPv6, le trafic IPv6 continue alors d'utiliser ce réseau au lieu du téléphone (l'IPv4 passe toujours par le téléphone). `-4` désactive IPv6. IPv6 n'est pas plus rapide qu'IPv4 ici : le réseau mobile domine
- Testé uniquement sur un **Samsung Galaxy S24 Ultra** (`04e8:6864`). Les autres téléphones ne sont pas testés : les retours sont les bienvenus (issue avec modèle et sortie de `sudo ./rndis-tether -v -r -a`)
- DHCP : le renouvellement unicast à T1 a été observé sur le téléphone testé (forme RFC 2131, acceptée par son serveur) ; le rebind à T2 et l'expiration sont couverts par des tests à horloge simulée mais pas encore vus sur un vrai bail
- Si le superviseur est tué avec `kill -9`, les routes `0.0.0.0/1` et `128.0.0.0/1` peuvent rester : `sudo route -n delete -net 0.0.0.0/1` et `128.0.0.0/1`. Les routes IPv6 « reject » sont retirées au redémarrage du service, ou avec `sudo route -n delete -inet6 -net ::/1 ::1` et `sudo route -n delete -inet6 -net 8000::/1 ::1`
- Si le téléphone propose CDC-NCM/ECM, macOS le gère déjà nativement

## Contribuer et licence

Les pull requests sont les bienvenues (voir [CONTRIBUTING.md](CONTRIBUTING.md)). Licence GPL-3.0-or-later : tu peux utiliser et modifier le code ; toute version modifiée distribuée doit publier son code source sous la même licence.
