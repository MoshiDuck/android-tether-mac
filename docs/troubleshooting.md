# Troubleshooting

*Version française : [troubleshooting.fr.md](troubleshooting.fr.md)*

Start with the report. It needs no `sudo`, changes nothing, and ends its first section with a plain-language diagnosis:

```bash
rndis-tether --diagnose
```

MAC addresses are cut after the vendor part, public IPv4 and global IPv6 addresses are masked and `adb` serial numbers are not shown — read it anyway before you paste it into an issue.

## What the diagnosis says, and what to do

| The report says | What it means | What to do |
|---|---|---|
| The service does not run | No `rndis-tether` process | `sudo ./install.sh` (or `tethermac-install` with Homebrew); by hand: `sudo rndis-tether -r -w -a` |
| Disabled from the menu bar icon | The service is idle on purpose | Turn it on again from the icon |
| No Android phone on USB | Nothing with a network, `adb` or MTP interface | Use a cable that carries data, unlock the phone, try another port |
| Only file transfer (MTP) | The phone is plugged in but tethering is off | Settings → Hotspot → USB tethering, or start the service with `-a` (needs USB debugging) |
| CDC-ECM/NCM | macOS already supports this phone | You do not need this driver |
| Only the `adb` interface | USB debugging is on, tethering is not | Turn on USB tethering on the phone |
| Connected, but the routes are missing | The service runs without `-r` | Add `-r` (the installed service has it) |
| IPv6 is blocked | The tether has no IPv6 (typical when the phone's internet comes from Wi-Fi) | Normal: applications fall back to IPv4 through the phone. `-L` lets IPv6 leave through the other networks instead |

## What the log says

`/var/log/rndis-tether.log` (the service) or the terminal (manual run). Lines are tagged `[root]` (supervisor) or `[worker]`.

| Log line | Meaning |
|---|---|
| `adb : adb est introuvable…` | Install it: `brew install --cask android-platform-tools` |
| `adb : le téléphone n'a pas autorisé ce Mac…` | Unlock the phone and accept “Allow USB debugging” (tick “Always allow”) |
| `adb : plusieurs appareils adb sont prêts…` | Several phones: pick yours with `-S serial` (`adb devices` lists them) |
| `Configuration IPv4 refusée : le sous-réseau … recouvre un réseau déjà utilisé` | The phone's subnet overlaps a network of the Mac (often `192.168.42.0/24` or `192.168.0.0/24`). Change the phone's subnet, or `-O` to accept it knowingly |
| `Le worker ne donne plus signe de vie…` | The worker hung; it was killed and replaced. Send the log if it repeats |
| `Réveil du Mac détecté…` | The Mac was asleep; the session restarts on its own |
| `IPv6: le téléphone n'en fournit plus…` | The phone stopped providing IPv6; the leak guard is back until it returns |
| `MTU de l'interface : …` | The phone announced a smaller MTU, which is now applied |
| `lignes du worker supprimées` | The worker logged too much; the supervisor kept 100 lines per 10 s |

## Symptoms

**The menu bar icon says “waiting”.** The phone is not in USB-tethering mode yet. Check `--diagnose`; with `-a` the service switches the phone itself, but that needs USB debugging and an authorised Mac (see the `adb` lines above).

**Connected, but pages do not load.** Check DNS: `scutil --dns` should list the phone's resolver first. With the Wi-Fi on, macOS may keep using Wi-Fi's DNS for some names; that is fine as long as the traffic goes through the phone (`route -n get 1.1.1.1` shows the interface, `utunN` when it does).

**Only IPv4 works.** Check that the phone itself has IPv6: its mobile data does, its Wi-Fi uplink usually does not provide a prefix. `--diagnose` says when IPv6 is blocked on purpose.

**It stopped after the Mac woke up.** The service restarts the session within a few seconds. If it does not, send `--diagnose` and the end of the log.

**I want to remove everything.** `sudo ./uninstall.sh` (or `tethermac-uninstall`): it removes the service, the icon, the log and its rotation setup.

## Reporting a problem

Open an issue with the *Bug report* form and paste the output of `rndis-tether --diagnose`. A *Phone report* (model, Android version, USB id) is the most useful contribution for other people's phones. Security problems: see [SECURITY.md](../SECURITY.md), not a public issue.
