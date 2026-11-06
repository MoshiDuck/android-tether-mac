# TetherMac

**Partage de connexion USB pour téléphones Android sur macOS, sans extension noyau, compatible Apple Silicon.**

macOS n'a pas de pilote RNDIS intégré, et les kexts comme HoRNDIS ne se chargent plus sur macOS récent. TetherMac (`rndis-tether`) est un petit pilote en espace utilisateur : il parle RNDIS au téléphone via libusb, gère ARP et DHCP, et expose la liaison par une interface `utun`. Une icône dans la barre de menus permet de l'activer ou de le désactiver.

> **Android uniquement.** Les iPhone fonctionnent déjà nativement sur macOS (active le point d'accès personnel et branche le câble) : ce projet n'est pas nécessaire.

*English: [README.md](README.md)*

## Installation

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
- `-U user` utilisateur pour `adb` (défaut `$SUDO_USER`)
- `-d vid:pid` cible un appareil précis ; `-v` logs détaillés

Journal du service : `/var/log/rndis-tether.log`.

## Limites

- IPv4 uniquement
- Testé sur un Samsung (`04e8:6864`)
- Si le service est tué avec `kill -9`, les routes `0.0.0.0/1` et `128.0.0.0/1` peuvent rester : `sudo route -n delete -net 0.0.0.0/1` et `128.0.0.0/1`
- Si le téléphone propose CDC-NCM/ECM, macOS le gère déjà nativement

## Contribuer et licence

Les pull requests sont les bienvenues (voir [CONTRIBUTING.md](CONTRIBUTING.md)). Licence GPL-3.0-or-later : tu peux utiliser et modifier le code ; toute version modifiée distribuée doit publier son code source sous la même licence.
