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
- Runs as a background service from boot; connects as soon as a phone is plugged in
- Menu bar icon: status, enable/disable
- Optionally switches the phone into USB-tethering mode through `adb`
- Applies the phone's DNS (so you can turn Wi-Fi off), renews the DHCP lease, reconnects after unplugging

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
| `-U user` | User to run `adb` as (default `$SUDO_USER`; set by the LaunchDaemon) |
| `-d vid:pid` | Target a specific device (hex) |
| `-v` | Verbose logs |

Log file when installed as a service: `/var/log/rndis-tether.log`.

## How it works

*Full write-up: [docs/how-it-works.md](docs/how-it-works.md)*

1. Finds an RNDIS interface (class `E0/01/03`, `EF/04/01` or `02/02/FF`) with its bulk data interface.
2. RNDIS handshake (`INITIALIZE`, MAC query, packet filter), keepalive replies on the interrupt endpoint.
3. DHCP client over raw Ethernet frames, then ARP responder.
4. IPv4 packets are copied between the phone and a `utun` interface; Ethernet headers are added/stripped in userspace.
5. The menu bar app talks to the root service through two files in `/Users/Shared` (`rndis-tether.disabled`, `rndis-tether.status`).

## Compatibility

| Phone | Android | USB id (tethering) | Status |
|-------|---------|--------------------|--------|
| Samsung Galaxy S24 Ultra (SM-S928B) | Android 16, One UI 8 | `04e8:6864` | Tested: DHCP, DNS, HTTPS, reconnect |
| Anything else | — | — | **Untested** — reports welcome |

This has only been tested on a **Samsung Galaxy S24 Ultra**. Other phones should work if they expose RNDIS, but may need adjustments — please [open an issue](https://github.com/MoshiDuck/android-tether-mac/issues) with your model and `sudo ./rndis-tether -v -r -a` output so the table can grow.

## Limitations

- IPv4 only (IPv6 is ignored; apps fall back to IPv4)
- DHCP lease renewal (every half lease, ~30 min) has not been observed over a full lease yet
- If the service is killed with `kill -9`, the routes `0.0.0.0/1` and `128.0.0.0/1` may remain: `sudo route -n delete -net 0.0.0.0/1` and `128.0.0.0/1`

## Development

```sh
make CFLAGS="-O2 -Wall -Wextra -Werror"   # build the driver, warnings are errors
make test                                  # unit + fuzz tests under ASan/UBSan, no phone or root needed
menubar/build.sh                           # build the menu bar app
```

The service runs as root and parses data coming from a USB device, so the parsers are fuzzed in the test suite (see [SECURITY.md](SECURITY.md)). CI runs all of the above on every push.

## Contributing

Pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md). Phone reports and bug reports with `-v` logs and your phone's `vid:pid` are especially useful. Changes are listed in [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). You are free to use, study and modify this software; if you distribute a modified version, you must release its source under the same license.
