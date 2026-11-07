# Roadmap

*Français : [ROADMAP.fr.md](ROADMAP.fr.md)*

Where TetherMac is going, in the order that matters. It is written from what has actually been measured, not wished for: one phone tested, `main.c` at 44 % test coverage, a root service that parses hostile data. Priorities follow one rule — **safety and correctness first, then compatibility, then comfort, then reach.**

Legend: ✅ done · 🔶 done in the code, waiting for validation on real hardware · ⬜ planned

## Where we are

| Area | State |
|------|-------|
| Core | ✅ userspace RNDIS (libusb + `utun`), DHCP, ARP, IPv4 and IPv6, DNS, routes, reconnection |
| Service | ✅ LaunchDaemon + menu bar app, `adb` activation, Homebrew tap, `--HEAD` channel |
| Quality | ✅ strict warnings, static analysis, ASan/UBSan tests (1.3 M checks), coverage thresholds, CI on 2 macOS versions |
| Security | ✅ 3 memory-safety bugs found by fuzzing and fixed; ✅ privilege separation (validated live); ✅ RFC 2131 DHCP (validated live) |
| Phones | one: Samsung Galaxy S24 Ultra (SM-S928B, Android 16) |

---

## 0.2.0 — Safe by construction  *(released 2026-11-06)*

Goal: a bug in the code that reads data from the phone or the network can no longer give root.

- ✅ **Privilege separation** — a tiny root supervisor and an unprivileged worker (`nobody`); fixed-size IPC; every request validated by `policy.c`; one fresh worker per session. *Validated live on the S24 Ultra (2026-11-06):* `nobody` can open the USB device; the two-process service carried IPv4 and IPv6 traffic, survived the menu bar toggle and a real USB drop, and cleaned up routes and DNS each time. The `-W` option remains for systems where that is not the case.
- ✅ **DHCP per RFC 2131** — unicast renewal at T1, broadcast rebind at T2, retransmission back-off, expiry and NAK handling. *Validated live on the S24 Ultra (2026-11-06):* the unicast renewal at T1 was accepted (`bail renouvelé`). The rebind at T2 and expiry paths are covered by simulated-clock tests only.

Released as 0.2.0. Both items were validated on the Galaxy S24 Ultra; the remaining hardening work moved to 0.3.0 and 0.4.0.

## 0.3.0 — Hardening, first part  *(released 2026-11-06)*

Goal: close the cheap, high-value gaps left after privilege separation.

- ✅ **`DHCPRELEASE` on shutdown** so the phone's address pool is not left with stale leases.
- ✅ **Subnet-overlap guard** — refuse a phone subnet that overlaps a network the Mac already uses (a malicious phone could otherwise shadow a LAN). Needs a clear error message, since a legitimate clash would block tethering.
- ✅ **IPC rate limiting** — a worker that sends more than 30 requests in 10 seconds is killed and replaced (a normal worker sends fewer than ten per session).
- ✅ **`--version` and a man page.**

Released as 0.3.0. Validated on the Galaxy S24 Ultra: `DHCPRELEASE` at the end of a session (log line), `--version` and the man page, and no false refusal by the overlap guard, including with the Mac's real Wi-Fi connected. The rate limit is covered by tests only.

## 0.4.0 — Confinement and observability

- 🟡 **Privilege-drop hardening** — ✅ done: no core files for either process, the worker is limited to 128 file descriptors, and the worker already verifies that it cannot get root back (`setuid(0)` must fail). Not done on purpose: `RLIMIT_FSIZE` (the worker writes to the log through an inherited descriptor, so a zero limit would kill it) and `RLIMIT_NPROC` (counted per user, shared with every other process of `nobody`, so it cannot be set reliably); the sandbox profile below is the right tool for both.
- ✅ **Worker watchdog** — the worker sends a sign of life every 10 s, and only if its three threads (receive, send, control) have run since the last one; the supervisor kills a worker that is silent for 60 s, cleans up the network configuration and starts a new one. Tested with a silent and a chatty fake worker.
- ✅ **Log hygiene** — the worker no longer holds the log file: its output goes through a pipe, and the supervisor copies it after removing control characters, truncating long lines and limiting the rate (100 lines per 10 s, with a count of what was dropped), so a hijacked worker cannot impersonate the supervisor or fill the disk. `newsyslog` rotates the file above 1 MB (five compressed archives) and the supervisor reopens it. Tested with fake workers and forked processes; the rotation by the real `newsyslog` is not yet checked on a Mac.
- ⬜ **macOS sandbox profile for the worker** (Seatbelt, deny by default; allow only the USB IOKit classes, the IPC socket and read access to the flag file). The goal is that even a compromised worker cannot open files or sockets.
- ✅ **`rndis-tether --diagnose`** — a single command, with no `sudo`, that prints a shareable report: service state (supervisor and worker), USB interfaces, ADB state, routes, DNS, the end of the log, and a plain-language diagnosis. MAC addresses, public IPs and serial numbers are masked. Still to add: the RNDIS handshake and DHCP exchange of the last session.
- ⬜ **Sleep/wake handling** — detect a Mac wake, re-verify the lease and the USB link, recover cleanly.
- 🟡 **IPv6 completeness** — ✅ done: the router lifetime and the withdrawal of the prefix are tracked, so an IPv6 that the phone stops providing is removed and the leak guard comes back, and a router solicitation is sent every 30 s while there is no IPv6 so that it is picked up when it returns. Still to do: a changed prefix (today a new one is only taken after the old one is withdrawn), RDNSS DNS servers, the RA MTU option, optional temporary addresses.
- ⬜ **MTU and TCP MSS** — honour the path MTU, optional MSS clamping for networks that drop fragments.
- ⬜ **Robust `adb` handling** — several devices (`adb -s`), none authorised, USB debugging disabled: clear messages instead of silent failure.
- ✅ **IPv6 leak guard** *(validated live on the S24 Ultra on 2026-11-07: with the phone's Wi-Fi on the reject routes are installed and `curl -6` fails in 0.05 s while IPv4 goes through the phone)* — with `-r`, when the tether provides no IPv6 (for example because the phone's own internet comes from Wi-Fi) but another network of the Mac does, IPv6 traffic bypasses the phone. Find a safe way to block it (so apps fall back to IPv4 through the phone), and test it on a real phone.

Exit criteria: the sandbox is on by default; `--diagnose` documented in the issue templates; sleep/wake tested.

## 0.5.0 — Hardware-free integration tests

Today `main.c` is covered at 44 %; the untested part is exactly the USB and session loop that real phones exercise.

- ⬜ **USB abstraction layer** — put libusb behind a small interface (`usb_ops`) so the session code can run against a **fake device**.
- ⬜ **Fake RNDIS phone** — a test double that speaks the RNDIS handshake, answers DHCP (including NAK and renewal), sends Router Advertisements, answers NDP, misbehaves on purpose (truncated, oversized, out-of-order messages, vanishing mid-session). Runs in CI with no phone.
- ⬜ **ThreadSanitizer job** — once sessions run under test, check the three-thread design for data races.
- ⬜ **Fuzz targets with libFuzzer** (Homebrew LLVM), seed corpora, and continuous fuzzing (OSS-Fuzz once the project qualifies).
- ⬜ **Modularisation** — split `main.c` into `rndis`, `usbdev`, `ndp`, `session`, `supervisor`, `worker`; remove the `RNDIS_TESTING` hooks in favour of dependency injection.
- ⬜ **Mutation testing** on `dhcp.c` and `policy.c` to prove the tests actually bite.
- ⬜ **Menu bar app tests** (XCTest) for state parsing and the toggle.

Exit criteria: `main.c` ≥ 80 % lines; the fake phone covers happy path, every failure above, and renewal; fuzzing runs on a schedule.

## 0.6.0 — Performance

Measured so far: ≈ 42 Mbit/s, limited by the mobile network, not by the driver. Before optimising, measure the driver on its own.

- ⬜ **Benchmark harness** — loopback throughput and latency against the fake phone, and `iperf3` against a real one over Wi-Fi-less uplink, recorded per release.
- ⬜ **RNDIS aggregation** — send several packets per bulk transfer (the S24 Ultra announces 23 700 bytes per transfer); today each packet is its own transfer.
- ⬜ **Asynchronous receive** — several libusb transfers in flight instead of one blocking read.
- ⬜ **Fewer syscalls per packet** and QoS classes for the threads.

Exit criteria: documented numbers; no regression gate in CI beyond a generous threshold.

## 0.7.0 — Comfort

- ⬜ **Failover mode** — leave the default route alone and let the tether become the primary service only when Wi-Fi goes down (rank the SystemConfiguration service instead of adding split routes). Today `-r` sends *everything* through the phone, which spends mobile data.
- ⬜ **Wi-Fi and the tether at the same time.** Today it is either/or: one connection travels over one link, so two links do not add up for a single download. In order of realism: (1) the *failover* mode above keeps the tether as a backup; (2) *per-flow load balancing* with `pf` (`route-to`, sticky per connection) spreads several simultaneous connections over both links — it helps parallel downloads, not one big file, and some sites dislike a client that changes address; (3) real *bonding* of a single connection needs an aggregation server (MPTCP-style) and is out of scope for this project. *Measured on 2026-11-06 (Mac on Wi-Fi, phone on LTE, two independent lines, three short rounds of 20 MB downloads, one connection per link at the same time): about 95 Mbit/s through the tether alone, 70 through Wi-Fi alone, and 172 with both at once — close to the sum, which shows that option (2) is worth prototyping. One connection still uses a single link.*
- ⬜ **Menu bar**: phone model, IPv4/IPv6 addresses, traffic counters, notifications on connect/disconnect, "open at login", a mode switch (always / failover / off).
- ⬜ **Settings file** (`/etc/rndis-tether.conf`) instead of arguments baked into a launchd plist.
- ⬜ **Localisation** (English, French).
- ⬜ **Data-use warning** — surface the phone's "metered" state and a configurable cap.

## 0.8.0 — Compatibility

- ⬜ **Phone matrix** — Pixel, Xiaomi, OnePlus, Motorola, Sony, and other Samsung models; USB-id database with per-device quirks; the *Phone report* issue form feeds the table in the README.
- ⬜ **Android's direction.** Android now also supports CDC-NCM for USB tethering, which macOS already handles natively, and RNDIS is the older path. A phone that offers NCM does not need this project. Track which devices still ship RNDIS only, and keep the scope honest: this tool exists for those.
- ⬜ **Intel Macs and older macOS** — a universal (arm64 + x86_64) build, tested on at least one Intel machine.

## 1.0.0 — Something you can recommend

Criteria, all required:

1. At least **five different phone models** confirmed working by independent reporters.
2. **Privilege separation and sandbox** on by default, reviewed by someone other than the author.
3. **Signed and notarised** by Apple (Developer ID), distributed as a notarised installer as well as through Homebrew.
4. **Integration tests without hardware** with `main.c` ≥ 80 % line coverage, and scheduled fuzzing.
5. Three months without a known high-severity bug.
6. Documentation complete: install, troubleshooting (`--diagnose`), architecture, security model.

---

## Distribution and trust (continuous)

- ✅ GitHub Release created from the changelog on every tag; `brew install` verified on a clean runner.
- ⬜ **Signed tags** (SSH or GPG) and published SHA-256 sums.
- ⬜ **Apple Developer ID** signing and notarisation of the menu bar app, then a notarised `.pkg` with a prebuilt universal binary (libusb linked statically) so users need neither Homebrew nor developer tools.
- ⬜ **Homebrew bottles** (prebuilt binaries) from the tap; later, a Cask for the app; `homebrew-core` once the project meets its notability bar.
- ⬜ **Supply chain** — pin GitHub Actions by commit SHA, OpenSSF Scorecard badge, SBOM attached to releases, branch protection requiring CI.
- ⬜ **Security tooling on** — private vulnerability reporting, CodeQL default setup, Dependabot alerts.

## Community and project health (continuous)

- ⬜ "Good first issue" labels, GitHub Discussions, a short design doc for contributors.
- ⬜ A second maintainer: one person is a single point of failure for a root service.
- ⬜ A written announcement (architecture and the security fixes found by fuzzing), and posts where Android and Mac users look for this.

## Risks and unknowns

| Risk | What we do |
|------|-----------|
| `nobody` cannot open USB devices on some macOS versions | Works on the tested macOS 26.5 (probe: open succeeded as `nobody`). Keep `-W` for other cases; consider a dedicated service user created by `install.sh`. |
| macOS tightens `utun`, IOKit or launchd rules | Keep the privileged part tiny; test each new macOS beta; the CI runs on two versions. |
| Android drops RNDIS for NCM | The project becomes unnecessary for those phones, which is fine; track device support and say so in the README. |
| A malicious phone | It can already offer any DHCP configuration. The supervisor validates what it applies, and the subnet-overlap guard closes the LAN-shadowing case. |
| Single maintainer, single test phone | Phone reports, a second maintainer, a fake phone in CI. |

## How to influence this roadmap

Open an issue with the *Phone report* form (the most valuable contribution), a bug report with `--diagnose` output once it exists, or a pull request — see [CONTRIBUTING.md](CONTRIBUTING.md).
