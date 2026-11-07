# TetherMac

[![CI](https://github.com/MoshiDuck/android-tether-mac/actions/workflows/ci.yml/badge.svg)](https://github.com/MoshiDuck/android-tether-mac/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Latest tag](https://img.shields.io/github/v/tag/MoshiDuck/android-tether-mac)](https://github.com/MoshiDuck/android-tether-mac/tags)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Apple%20Silicon%20%2B%20Intel-lightgrey)

**USB tethering for Android phones on macOS — no kernel extension, works on Apple Silicon.**

macOS has no built-in RNDIS driver, and kext-based drivers such as HoRNDIS no longer load on modern macOS. TetherMac (`rndis-tether`) is a small userspace driver: it speaks RNDIS to the phone over libusb, handles ARP and DHCP itself, and exposes the link as a `utun` interface. A menu bar icon lets you switch it on and off.

> **Android only.** iPhones already work natively on macOS (enable Personal Hotspot and plug in the cable) — you don't need this.

*Français : [README.fr.md](README.fr.md)*

## Features

- Pure userspace (libusb + `utun`), no kext, no DriverKit entitlement
- **Privilege separation**: only a tiny supervisor runs as root; everything that parses data from the phone or the network runs in an unprivileged worker (`nobody`), and the supervisor validates every request it receives
- Runs as a background service from boot; connects as soon as a phone is plugged in
- Menu bar icon: status, enable/disable
- Optionally switches the phone into USB-tethering mode through `adb`
- Applies the phone's DNS (so you can turn Wi-Fi off), reconnects after unplugging
- RFC 2131 DHCP client: unicast renewal at T1, broadcast rebind at T2, retransmissions, expiry and NAK handling
- IPv4 **and IPv6** (when the phone's network provides it): IPv6 traffic goes through the phone too, instead of leaking out through Wi-Fi

## Requirements

- macOS 13+ (developed on macOS 26, Apple Silicon)
- [Homebrew](https://brew.sh) `libusb`, Xcode command line tools (`clang`, `swiftc`)
- An Android phone with USB tethering (RNDIS). Some phones use CDC-NCM/ECM instead; macOS already handles those natively
- Optional: `adb` and USB debugging, for automatic switching to tethering mode

## Install

### Homebrew (recommended)

```sh
brew install MoshiDuck/tap/android-tether-mac
tethermac-install        # asks for your password once; installs the service + menu bar icon
```

Update later with `brew upgrade android-tether-mac && tethermac-install`.

**Latest development version** (every change on `main`, tested by CI on each push):

```sh
brew install --HEAD MoshiDuck/tap/android-tether-mac
brew upgrade --fetch-HEAD android-tether-mac && tethermac-install   # to update it later
```

### From source

```sh
brew install libusb
git clone https://github.com/MoshiDuck/android-tether-mac.git && cd android-tether-mac
sudo ./install.sh
```

This builds everything, installs a LaunchDaemon (`/usr/local/bin/rndis-tether`) and the menu bar app (`/Applications/RNDISTether.app`). Uninstall with `sudo ./uninstall.sh`.

> **Heads-up:** the service routes *all* traffic through the phone while it is connected (`-r`), even if Wi-Fi is on. That uses your mobile data. Use the menu bar toggle to switch it off.

## Manual use

```sh
make
sudo ./rndis-tether -v -r -w -a
```

| Flag | Meaning |
|------|---------|
| `-r` | Route all traffic through the phone (split routes `0/1` + `128/1`) and apply its DNS; both removed on exit |
| `-w` | Wait for the phone and reconnect after unplugging |
| `-a` | Enable tethering on the phone through `adb` (needs USB debugging) |
| `-4` | IPv4 only (turn the IPv6 relay off) |
| `-W user` | Unprivileged user for the worker process (default `nobody`) |
| `-L` | With `-r`, let IPv6 leave through the Mac's other networks when the tether has none (otherwise it is blocked) |
| `-O` | Accept a phone subnet that overlaps a network already in use (refused by default) |
| `-V`, `--version` | Print the version |
| `--diagnose` | Print a report for bug reports (no `sudo`; MAC addresses, public IPs and serial numbers are masked) |
| `-U user` | User to run `adb` as (default `$SUDO_USER`; set by the LaunchDaemon) |
| `-S serial` | Serial number of the phone for `adb` when several devices are connected (`adb devices` lists them) |
| `-d vid:pid` | Target a specific device (hex) |
| `-v` | Verbose logs |

Log file when installed as a service: `/var/log/rndis-tether.log`.

## How it works

*Full write-up: [docs/how-it-works.md](docs/how-it-works.md)*

1. Finds an RNDIS interface (class `E0/01/03`, `EF/04/01` or `02/02/FF`) with its bulk data interface.
2. RNDIS handshake (`INITIALIZE`, MAC query, packet filter), keepalive replies on the interrupt endpoint.
3. DHCP client over raw Ethernet frames, then ARP responder.
4. IPv4 and IPv6 packets are copied between the phone and a `utun` interface; Ethernet headers are added/stripped in userspace. IPv6 uses a router solicitation, SLAAC and neighbor advertisements.
5. The menu bar app talks to the root service through two files in `/Users/Shared` (`rndis-tether.disabled`, `rndis-tether.status`).
6. The service is split in two processes — a root supervisor and an unprivileged worker that does all the parsing — see *Privilege separation* in [docs/how-it-works.md](docs/how-it-works.md).

## Compatibility

| Phone | Android | USB id (tethering) | Status |
|-------|---------|--------------------|--------|
| Samsung Galaxy S24 Ultra (SM-S928B) | Android 16, One UI 8 | `04e8:6864` | Tested: DHCP, DNS, HTTPS, reconnect |
| Anything else | — | — | **Untested** — reports welcome |

This has only been tested on a **Samsung Galaxy S24 Ultra**. Other phones should work if they expose RNDIS, but may need adjustments — please [open an issue](https://github.com/MoshiDuck/android-tether-mac/issues) with your model and `sudo ./rndis-tether -v -r -a` output so the table can grow.

## Limitations

- IPv6 needs the phone to advertise a `/64` prefix (it did on the tested phone, on mobile data); otherwise the tether stays IPv4-only. Android does not delegate a prefix when the phone's own internet comes from Wi-Fi. In that case, with `-r`, the supervisor blocks IPv6 towards the Internet (two `reject` routes) so that nothing bypasses the phone: applications fall back to IPv4, which goes through the phone, and local IPv6 networks stay reachable. `-L` keeps the previous behaviour (IPv6 leaves through the other network). IPv6-only DNS servers announced by the router are not used. Use `-4` to turn IPv6 off. IPv6 is not faster than IPv4 here — the mobile network dominates
- DHCP: the unicast renewal at T1 has been observed on the tested phone (RFC 2131 form, accepted by its server); the rebind at T2 and the expiry path are covered by simulated-clock tests but not yet seen on a real lease
- If the supervisor is killed with `kill -9`, the routes `0.0.0.0/1` and `128.0.0.0/1` may remain: `sudo route -n delete -net 0.0.0.0/1` and `128.0.0.0/1`. The IPv6 `reject` routes are removed when the service starts again, or with `sudo route -n delete -inet6 -net ::/1 ::1` and `sudo route -n delete -inet6 -net 8000::/1 ::1`

## Development

```sh
make strict        # all warnings are errors (-Wshadow, -Wformat=2, -Wcast-qual, …)
make               # build the driver
make test          # unit, state-machine, IPC, supervisor and fuzz tests under ASan/UBSan — no phone or root needed
make analyze       # clang static analyzer
make coverage      # line coverage report (CI enforces thresholds per module)
menubar/build.sh   # build the menu bar app
```

The parsers handle data from the phone and the network, so they are fuzzed in the test suite and run in an unprivileged process (see [SECURITY.md](SECURITY.md)). CI runs all of the above on every push, on two macOS versions. Where the project is heading: [ROADMAP.md](ROADMAP.md).

## Contributing

Pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md), the [Code of Conduct](CODE_OF_CONDUCT.md) and the [roadmap](ROADMAP.md). Phone reports and bug reports with `-v` logs and your phone's `vid:pid` are especially useful. Changes are listed in [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). You are free to use, study and modify this software; if you distribute a modified version, you must release its source under the same license.
