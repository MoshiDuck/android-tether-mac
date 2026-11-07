# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1] - 2026-11-06

**Security release — users of 0.1.0 should upgrade** (`brew upgrade android-tether-mac && tethermac-install`).

### Added
- IPv6 support: router solicitation/advertisement with SLAAC address, neighbor advertisement replies, IPv6 packet relay in both directions, `::/1` + `8000::/1` routes with `-r`, and an IPv6 entry in the SystemConfiguration service so macOS asks for AAAA records. Validated on a Galaxy S24 Ultra; `-4` turns it off.
- The DHCP lease renewal is now logged ("demande de renouvellement" / "bail renouvelé") so it can be observed.
- Unit and fuzz tests (`make test`) run under AddressSanitizer and UBSan, without a phone or root.
- Continuous integration (build with `-Werror`, tests, menu bar app build, shell script lint).
- Security policy, code of conduct, support guide, issue and pull request templates, Dependabot.
- Documentation of the internals in `docs/how-it-works.md` (English and French).

### Security
- Fixed an out-of-bounds read in the RNDIS receive path: a malformed `data_length` or `data_offset` sent by a USB device could overflow a 32-bit bounds check.
- Fixed a signed/unsigned comparison on the RNDIS message length that let a very large length move the parser outside its buffer.
- The MAC address query response is now validated in 64-bit arithmetic.

## [0.1.0] - 2026-11-06

First public release.

### Added
- Userspace RNDIS driver (libusb + `utun`): handshake, keepalive, DHCP client with lease renewal, ARP.
- `-r` routing through the phone with DNS applied via SystemConfiguration; `-w` wait and reconnect; `-a` enable tethering through `adb`.
- Menu bar app to enable/disable and show status; LaunchDaemon installer (`install.sh`, `uninstall.sh`).
- Homebrew tap (`brew install MoshiDuck/tap/android-tether-mac`).
- Tested on a Samsung Galaxy S24 Ultra (SM-S928B, Android 16, One UI 8).

[Unreleased]: https://github.com/MoshiDuck/android-tether-mac/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/MoshiDuck/android-tether-mac/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/MoshiDuck/android-tether-mac/releases/tag/v0.1.0
