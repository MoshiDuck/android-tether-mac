# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- **IPv6 lost during a session.** If the phone stopped providing IPv6 after the session had started (for example because its internet moved to Wi-Fi), the Mac kept the old address and routes: IPv6 traffic then waited for a timeout instead of failing fast, and the leak guard did not come back. The worker now follows the router lifetime and the withdrawal of the prefix announced by the phone; when the IPv6 is gone, the supervisor removes the address, the service and the routes and puts the leak guard back (new request `DECONFIG6`, validated like the others). Router solicitations continue every 30 s while there is no IPv6, so it is picked up again when it returns.
- A router advertisement with a router lifetime of 0 (not a default router) is no longer used to configure IPv6.

### Added
- **`rndis-tether --diagnose`**: a report for bug reports, with no `sudo` and no changes to the system. It shows the service (supervisor and worker), the USB devices with a network, ADB or MTP interface, the ADB state, the IPv4 and IPv6 routes of the tether, the DNS resolvers, the end of the log and a plain-language diagnosis (cable, tethering not enabled, service stopped, disabled from the menu, IPv6 blocked by the leak guard…). MAC addresses are cut after the vendor part, public IPv4 and global IPv6 addresses are masked and ADB serial numbers are not shown. The bug report form now asks for it.

### Security
- **IPv6 leak guard.** With `-r`, when the tether provides no IPv6 (for example because the phone's own internet comes from Wi-Fi), the supervisor installs two `reject` routes (`::/1` and `8000::/1`) so that the Mac's IPv6 traffic cannot bypass the phone through another network; applications fall back to IPv4, which goes through the phone, and local IPv6 networks stay reachable. When the tether does get IPv6, the `reject` routes are replaced by the real routes. They are removed at the end of the session and when the service starts. `-L` restores the previous behaviour.

## [0.3.0] - 2026-11-06

Hardening release: the subnet-overlap guard and the request rate limit tighten the supervisor, and the lease is now returned to the phone. Upgrade with `brew upgrade android-tether-mac && tethermac-install`.

### Security
- **Subnet-overlap guard.** The supervisor refuses a phone subnet that overlaps an ordinary network (/16 to /30) already connected to the Mac, so a malicious phone cannot shadow a local network. Host addresses (a VPN /32) and very wide networks (a corporate /8) do not block tethering. `-O` accepts an overlap knowingly.
- **Request rate limit.** A worker sending more than 30 requests in 10 seconds is killed and replaced.

### Added
- `rndis-tether --version` / `-V`, a man page (`man rndis-tether`), and a `VERSION` file that the release script and workflow check against the tag.
- The DHCP lease is returned to the server (`DHCPRELEASE`, RFC 2131 §3.1.7) when a session ends normally.

### Changed
- Coverage build artifacts that were committed by mistake in 0.2.0 are no longer tracked.

### Known limitation
- With `-r`, when the phone provides no IPv6 (for example because its own internet comes from Wi-Fi) but another network of the Mac does, IPv6 traffic keeps using that network instead of the phone. IPv4 is unaffected. See the roadmap.

## [0.2.0] - 2026-11-06

**Security-model release**: the root service is split so that code reading data from the phone or the network no longer runs as root. Upgrade with `brew upgrade android-tether-mac && tethermac-install`.

### Security
- **Privilege separation.** The root service is now a small *supervisor* plus an unprivileged *worker* (`nobody`, or `-W user`). The worker does all the USB, RNDIS, DHCP, NDP and packet parsing; it receives the `utun` interface by descriptor passing and can only ask the supervisor for five actions over a fixed-size channel. The supervisor validates every value (`policy.c`) before it runs `ifconfig`, `route`, `scutil` or `adb`, remembers what it applied so cleanup never depends on the worker, and starts a fresh worker for each session. A bug in the parsing code is no longer a root exploit.

### Changed
- **DHCP client follows RFC 2131.** Renewal is a *unicast* `REQUEST` with `ciaddr` and without options 50/54 at T1, then a *broadcast* `REQUEST` at T2; the server's T1/T2 (options 58/59) are honoured; retransmissions use the RFC back-off; a lease that expires, a `NAK`, or a different address restarts the session. Previously renewal was a broadcast in the "selecting" form, was never retried and ignored expiry.
- Code split into modules under `src/` (`dhcp`, `policy`, `ipc`, `bytes`); `main.c` moved to `src/main.c`.
- New option `-W user` (unprivileged worker user, default `nobody`). Log lines are tagged `[root]` / `[worker]`.

### Added
- Tests for the DHCP state machine with a simulated clock, the validation policy (accept/reject tables and a property test), the IPC channel (descriptor passing, closed peer, malformed and random input) and the supervisor itself (system commands are recorded instead of run). 1.3 million checks under ASan/UBSan.
- `make strict` (extra warnings as errors), `make analyze` (clang static analyzer), `make coverage` with per-module thresholds enforced in CI; CI on macOS 14 and 15; `actionlint`.
- Release workflow: on a `vX.Y.Z` tag it checks the changelog, builds and tests, creates the GitHub Release from the changelog, optionally updates the Homebrew formula, and verifies `brew install` on a clean runner. A CI job installs the tap formula with `--HEAD` on every push to `main`.
- `ROADMAP.md` and `RELEASING.md`.

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

[Unreleased]: https://github.com/MoshiDuck/android-tether-mac/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/MoshiDuck/android-tether-mac/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MoshiDuck/android-tether-mac/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/MoshiDuck/android-tether-mac/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/MoshiDuck/android-tether-mac/releases/tag/v0.1.0
