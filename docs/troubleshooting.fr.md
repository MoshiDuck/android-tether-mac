# Dépannage

*English version: [troubleshooting.md](troubleshooting.md)*

Commence par le rapport. Il ne demande pas `sudo`, ne modifie rien, et sa première section donne un diagnostic en langage clair :

```bash
rndis-tether --diagnose
```

Les adresses MAC sont coupées après la partie fabricant, les IPv4 publiques et IPv6 globales sont masquées et les numéros de série `adb` ne sont pas affichés — relis-le quand même avant de le coller dans un ticket.

## Ce que dit le diagnostic, et quoi faire

| Le rapport dit | Ce que ça veut dire | Quoi faire |
|---|---|---|
| Le service ne tourne pas | Aucun processus `rndis-tether` | `sudo ./install.sh` (ou `tethermac-install` avec Homebrew) ; à la main : `sudo rndis-tether -r -w -a` |
| Désactivé depuis l'icône de la barre de menus | Le service est au repos volontairement | Rallume-le depuis l'icône |
| Aucun téléphone Android sur l'USB | Rien avec une interface réseau, `adb` ou MTP | Un câble qui transmet des données, déverrouille le téléphone, essaie un autre port |
| Transfert de fichiers seulement (MTP) | Le téléphone est branché mais le partage est coupé | Réglages → Point d'accès → Partage de connexion USB, ou lance le service avec `-a` (débogage USB requis) |
| CDC-ECM/NCM | macOS gère déjà ce téléphone | Tu n'as pas besoin de ce driver |
| Seule l'interface `adb` | Le débogage USB est actif, pas le partage | Active le partage de connexion USB sur le téléphone |
| Connecté, mais routes absentes | Le service tourne sans `-r` | Ajoute `-r` (le service installé l'a) |
| L'IPv6 est bloqué | Le tethering n'a pas d'IPv6 (fréquent quand l'internet du téléphone vient du Wi-Fi) | Normal : les applications passent en IPv4 via le téléphone. `-L` laisse l'IPv6 sortir par les autres réseaux |

## Ce que dit le journal

`/var/log/rndis-tether.log` (le service) ou le terminal (lancement à la main). Les lignes sont étiquetées `[root]` (superviseur) ou `[worker]`.

| Ligne du journal | Sens |
|---|---|
| `adb : adb est introuvable…` | Installe-le : `brew install --cask android-platform-tools` |
| `adb : le téléphone n'a pas autorisé ce Mac…` | Déverrouille le téléphone et accepte « Autoriser le débogage USB » (coche « Toujours autoriser ») |
| `adb : plusieurs appareils adb sont prêts…` | Plusieurs téléphones : désigne le tien avec `-S série` (`adb devices` les liste) |
| `Configuration IPv4 refusée : le sous-réseau … recouvre un réseau déjà utilisé` | Le sous-réseau du téléphone recouvre un réseau du Mac (souvent `192.168.42.0/24` ou `192.168.0.0/24`). Change le sous-réseau du téléphone, ou `-O` pour l'accepter en connaissance de cause |
| `Le worker ne donne plus signe de vie…` | Le worker s'est bloqué ; il a été tué et remplacé. Envoie le journal si ça se répète |
| `Réveil du Mac détecté…` | Le Mac dormait ; la session redémarre toute seule |
| `IPv6: le téléphone n'en fournit plus…` | Le téléphone ne fournit plus d'IPv6 ; la garde contre la fuite est revenue jusqu'à son retour |
| `MTU de l'interface : …` | Le téléphone a annoncé un MTU plus petit, désormais appliqué |
| `lignes du worker supprimées` | Le worker a trop écrit ; le superviseur a gardé 100 lignes par 10 s |

## Symptômes

**L'icône indique « waiting ».** Le téléphone n'est pas encore en mode partage USB. Regarde `--diagnose` ; avec `-a` le service bascule le téléphone lui-même, mais il faut le débogage USB et un Mac autorisé (voir les lignes `adb` ci-dessus).

**Connecté, mais les pages ne chargent pas.** Vérifie le DNS : `scutil --dns` doit lister d'abord le résolveur du téléphone. Wi-Fi allumé, macOS peut garder le DNS du Wi-Fi pour certains noms ; ce n'est pas grave tant que le trafic passe par le téléphone (`route -n get 1.1.1.1` montre l'interface, `utunN` quand c'est le cas).

**Seule l'IPv4 marche.** Vérifie que le téléphone a de l'IPv6 : ses données mobiles en ont, son accès par Wi-Fi n'en donne en général pas. `--diagnose` dit quand l'IPv6 est bloqué volontairement.

**Ça s'est arrêté après la veille du Mac.** Le service relance la session en quelques secondes. Sinon, envoie `--diagnose` et la fin du journal.

**Je veux tout retirer.** `sudo ./uninstall.sh` (ou `tethermac-uninstall`) : il retire le service, l'icône, le journal et sa rotation.

## Signaler un problème

Ouvre un ticket avec le formulaire *Bug report* et colle la sortie de `rndis-tether --diagnose`. Un *Phone report* (modèle, version d'Android, identifiant USB) est la contribution la plus utile pour les téléphones des autres. Problème de sécurité : voir [SECURITY.md](../SECURITY.md), pas un ticket public.
