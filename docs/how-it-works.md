# How TetherMac works

*Français : [how-it-works.fr.md](how-it-works.fr.md)*

TetherMac gives a Mac internet access through an Android phone's USB tethering, **without any kernel extension**. This page explains the problem, the approach, and each moving part, so you can audit, debug or extend it.

Source: [`src/`](../src) (the driver in C: `main.c`, `dhcp.c`, `policy.c`, `ipc.c`) and [`menubar/RNDISTether.swift`](../menubar/RNDISTether.swift) (menu bar icon).

## 1. The problem

When you switch on USB tethering, an Android phone presents itself as a USB network adapter using a protocol called **RNDIS** (Remote NDIS, a Microsoft protocol that carries Ethernet frames over USB).

- macOS has **no built-in RNDIS driver**. (It does handle CDC-ECM/NCM, which some phones use instead, and iPhone tethering.)
- The classic fix, [HoRNDIS](https://github.com/jwise/HoRNDIS), is a **kext**: code loaded into the kernel. Apple has been phasing kexts out. On Apple Silicon they are blocked by default; loading one means lowering the boot security level and approving it manually, and new releases of macOS keep making that harder.
- The official replacement, **DriverKit**, needs a special entitlement granted by Apple for networking drivers, which an open-source hobby project cannot realistically obtain.

## 2. The approach

Instead of a driver *inside* the kernel, TetherMac is an ordinary program that does the work *outside* it, in two halves:

```
┌────────────┐   USB (libusb)    ┌─────────────────────┐   utun    ┌──────────────┐
│  Android   │ ◄───────────────► │  rndis-tether       │ ◄───────► │ macOS network│
│  (RNDIS)   │  Ethernet frames  │  (userspace, root)  │ IP packets│    stack     │
└────────────┘  in RNDIS packets │  ARP · DHCP · glue  │           └──────────────┘
                                 └─────────────────────┘
```

1. **USB side**: [libusb](https://libusb.info) opens the phone through macOS's normal userspace USB API (IOKit). This works because macOS does not claim the phone's RNDIS interface, so nobody else is using it.
2. **Network side**: a **`utun`** interface — the same kind of virtual interface VPNs use — is provided by the kernel and fed from userspace.

No code is loaded into the kernel, and no Apple entitlement is needed. The only requirement is root, to create the `utun` interface and change routes.

## 3. USB side: speaking RNDIS

### Finding the phone

The driver scans USB devices for an RNDIS control interface:

| Class / subclass / protocol | Seen on |
|-----------------------------|---------|
| `0xE0 / 0x01 / 0x03` (wireless controller, RNDIS) | Most Android phones (Samsung Galaxy S24 Ultra: `04e8:6864`) |
| `0xEF / 0x04 / 0x01` (miscellaneous, RNDIS) | Some devices |
| `0x02 / 0x02 / 0xFF` (CDC ACM, vendor protocol) | Older devices |

It then looks for the matching **data interface** (class `0x0A`) and its two bulk endpoints (in/out), plus the control interface's interrupt endpoint. If it only finds CDC-ECM/NCM, it tells you macOS handles that natively.

A phone in normal (MTP) mode shows a different USB id (`04e8:6860` on the S24 Ultra) and has no RNDIS interface — which is why USB tethering must be switched on first.

### The RNDIS handshake

Control messages go over the control endpoint (`SEND_ENCAPSULATED_COMMAND` / `GET_ENCAPSULATED_RESPONSE`). All integers are little-endian.

| Step | Message | Purpose |
|------|---------|---------|
| 1 | `INITIALIZE` (0x02) | Negotiate version and maximum transfer size |
| 2 | `QUERY` (0x04) for `OID_802_3_PERMANENT_ADDRESS` | The phone tells us the MAC address to use as the Mac's side |
| 3 | `SET` (0x05) for `OID_GEN_CURRENT_PACKET_FILTER` = `0x0D` | Receive directed, broadcast and multicast frames |
| … | `KEEPALIVE` (0x08) | The phone pings now and then; we answer on the control channel, or it drops us |
| end | `HALT` (0x03) | Polite shutdown |

### Moving packets

Every Ethernet frame is wrapped in an RNDIS `PACKET` message: a 44-byte header (type `0x01`, total length, data offset, data length, …) followed by the frame. Details that matter:

- One bulk read can contain **several** RNDIS packets back to back; the driver parses them in a loop.
- When a bulk write's length is an exact multiple of the endpoint's max packet size, a **zero-length packet** must follow, or the phone waits for more data.

## 4. Network side: from Ethernet to IP and back

A `utun` interface carries only **IP packets**, each prefixed by 4 bytes saying the address family (IPv4 or IPv6). The phone speaks **Ethernet**. The driver translates:

- **Phone → Mac**: strip the 14-byte Ethernet header (and any Ethernet padding, using the IP length field), prepend the 4-byte family header, write to `utun`.
- **Mac → phone**: read an IPv4 or IPv6 packet from `utun`, drop the family header, add an Ethernet header (destination: the phone's MAC, source: ours; for IPv6 multicast, `33:33:` plus the last four bytes of the address), wrap in RNDIS, send over USB.

Because there is no real network card, the driver also plays the roles a card's network stack would:

- **DHCP client** (RFC 2131, `src/dhcp.c`): sends `DISCOVER`, takes the `OFFER`, sends `REQUEST`, receives `ACK` — as raw Ethernet/IP/UDP frames with the broadcast flag, since we have no address yet. It reads address, netmask, router, DNS, lease time and the server's T1/T2. The lease then follows the RFC: at **T1** (option 58, else 50 % of the lease) a **unicast** `REQUEST` to the server with our address in `ciaddr` and without options 50/54; unanswered, it is retransmitted with the RFC back-off; at **T2** (option 59, else 87.5 %) it becomes a **broadcast** `REQUEST`; if the lease expires, or the server answers `NAK` or with a different address, the session is restarted from scratch. The whole state machine is driven by an injected clock and tested without waiting.
- **ARP**: answers "who has our IP?" from the phone, and learns the phone's MAC from ARP and IP traffic.

### IPv6

The same idea applies to IPv6, with ICMPv6 doing the work of ARP and DHCP:

- **Router solicitation / advertisement**: shortly after connecting, the driver sends a Router Solicitation (`ff02::2`). The phone answers with a Router Advertisement carrying a `/64` prefix. The driver builds an address from that prefix plus a random interface identifier (SLAAC), and configures it with `ifconfig inet6`. Only the first usable advertisement is used per session.
- **Neighbor discovery**: the phone asks "who has this address?" with Neighbor Solicitations. The phone is the only other host on the link, so the driver answers any such request with a Neighbor Advertisement carrying our MAC address — otherwise the phone could not send packets back. Requests that are invalid (wrong hop limit, unspecified source, multicast target…) are ignored.
- **Checksums**: ICMPv6 checksums cover an IPv6 pseudo-header; they are computed by `icmp6_csum`, which the tests cross-check against an independent implementation.
- Everything read from the network is bounds-checked and fuzzed, like the rest of the parsers.
- `-4` turns all of this off.

## 5. Integrating with macOS

After DHCP, the driver runs `ifconfig` to set the address on the `utun` interface (point-to-point to the phone's gateway, MTU 1500).

### Routes (`-r`)

Instead of replacing the default route, it adds two **more specific** routes, `0.0.0.0/1` and `128.0.0.0/1`, via the phone. Together they cover all of IPv4 and beat the default route, but the existing default route stays untouched. Removing the two routes restores everything. When an IPv6 address has been configured, the same trick is applied with `::/1` and `8000::/1`, so IPv6 traffic does not leak around the tether through Wi-Fi. If the tether has no IPv6 (Android does not delegate a prefix when the phone's own internet comes from Wi-Fi), the supervisor installs the same two routes as `reject` routes instead: IPv6 destinations outside the local networks fail immediately, so applications fall back to IPv4, which goes through the phone. Local IPv6 networks keep working because their routes are more specific. The `reject` routes are replaced by the real ones if the tether later gets IPv6, removed at the end of the session, and also cleaned up when the service starts. `-L` disables this.

### DNS

DNS was the subtle part. A first version registered a *supplemental* resolver in SystemConfiguration, which worked while Wi-Fi was on — but with Wi-Fi off, macOS had no *default* resolver, and names did not resolve.

The fix is to describe the tether as a **network service**, like VPN clients do, by writing `State:/Network/Service/rndis-tether/IPv4` (address, router, interface) and `.../DNS` (the phone's DNS server) with `scutil`. macOS then elects it as the primary service when nothing else has a route, and uses its DNS. Both entries are removed on exit.

Once an IPv6 address exists, a third entry, `.../IPv6` (address, prefix length, the router's link-local address, interface), is added. It is not cosmetic: without it, macOS only sends `A` queries through the tether's resolver, so IPv6-only host names do not resolve (`scutil --dns` shows "Request A records" only). With it, the resolver also shows "Request AAAA records".

## 6. Running as a service

`install.sh` sets up two launchd jobs:

- **LaunchDaemon** (root, starts at boot): runs `rndis-tether -r -w -a -U <your user>`.
  - `-w`: wait for a phone and reconnect after unplugging.
  - `-a`: if no RNDIS device is found, switch the phone to tethering by running `adb shell svc usb setFunctions rndis` **as your user** (so it uses your ADB keys and PATH, not root's). `adb` usually reports an error because the USB bus re-enumerates during the switch; that is ignored.
- **LaunchAgent** (your session): the menu bar app.

**Dead sessions.** If the phone vanishes or re-enumerates, USB calls return an I/O or "no device" error — or just time out. The driver treats a device error, or five consecutive send timeouts, as a dead session: it removes routes and DNS, releases the interface, and goes back to waiting.

**Menu bar ↔ service.** The app is not root, so it does not talk to the service directly. They share two files in `/Users/Shared`:

| File | Written by | Meaning |
|------|-----------|---------|
| `rndis-tether.disabled` | menu bar app | If it exists, the service stays idle (and tears down any active session) |
| `rndis-tether.status` | service, every ~5 s | `waiting`, `connecting`, `connected <ip> <iface>`, `disabled`, … |

If the status file is older than 20 seconds, the icon shows "service stopped".

### Privilege separation

The service is two processes, so that the code reading hostile data never runs as root:

| | Supervisor | Worker |
|---|---|---|
| Runs as | root | `nobody` (or the user given with `-W`) |
| Does | creates the `utun` interface; applies `ifconfig`/`route`/`scutil` after validation; removes them at the end; writes the status file; runs `adb` | USB, RNDIS, DHCP, ARP, NDP, packet relay |
| Reads hostile data | no | yes |

How it works:

1. The supervisor (`supervisor_main`) resolves the worker user, then `fork()`s. The child (`worker_main`) closes every inherited file descriptor except the channel, calls `setgroups`/`setgid`/`setuid`, and **checks that it cannot become root again** (`setuid(0)` must fail) — otherwise it refuses to run.
2. The two processes talk over a `socketpair` with **fixed-size messages** (`ipc.h`) and file-descriptor passing: the supervisor creates the `utun` interface and hands the descriptor to the worker, which can then read and write packets without any privilege.
3. The worker can only make six requests: create the interface, apply an IPv4 configuration, apply an IPv6 configuration, withdraw the IPv6 configuration (when the phone stops providing it), report a status, and enable tethering through `adb`. **The supervisor validates every value** (`policy.c`): a gateway must be in the assigned subnet, addresses must be unicast and routable, the subnet must not overlap an ordinary network already connected to the Mac (so a malicious phone cannot shadow your LAN; `-O` overrides), the status text is restricted to a few exact forms, and so on. A worker that sends more than 30 requests in 10 seconds is killed and replaced, and so is a worker that stays silent for 60 seconds (it sends a sign of life every 10 seconds, and only while its three threads are running). Neither process can write core files, the worker is limited to 128 file descriptors, and it does not hold the log file: its output goes through a pipe and the supervisor copies it after cleaning it and limiting its rate. Commands are built from validated numbers, never from text sent by the worker.
4. The supervisor remembers what it applied (routes, DNS, service entries), so cleanup never depends on the worker. A worker crash, an unplug or a malformed message ends that worker; the supervisor cleans up and starts a fresh one. **There is one worker per session**, so no state survives from one session to the next.

What this does and does not buy:

- A bug in the USB, RNDIS, DHCP or packet code (the code that parses data from the phone and the network) is no longer a root exploit: the attacker gets the `nobody` user, not root.
- A compromised worker can still send traffic through the tunnel, and ask for a *valid* configuration (the same thing a malicious phone with a DHCP server could do anyway). It cannot run commands, write files as root or change arbitrary system settings.
- The worker is not sandboxed yet (it could still open files readable by `nobody`, or make network connections of its own). A macOS sandbox profile is on the [roadmap](../ROADMAP.md).

## 7. Trade-offs and limitations

- **Throughput**: packets cross the kernel/userspace boundary and are copied; a native driver would be somewhat faster. On a Galaxy S24 Ultra, about 42 Mbit/s was measured, probably limited by the mobile network rather than the driver.
- **Always-on service**: if the process stops, the network disappears (it restarts automatically under launchd).
- **IPv6 depends on the phone**: it needs a Router Advertisement with a `/64` prefix (validated on a Galaxy S24 Ultra, whose mobile network provides IPv6). Without one, nothing IPv6 is configured and the tether stays IPv4-only. IPv6 DNS servers announced by the router (RDNSS) are not used; names are resolved through the phone's IPv4 DNS server, which also answers `AAAA` queries. No speed difference between IPv4 and IPv6 was measurable in tests; the mobile network dominates.
- **Tested on one phone** (Galaxy S24 Ultra, Android 16, One UI 8). Phones with different RNDIS quirks may need changes.
- **All traffic via the phone** while connected with `-r`, even if Wi-Fi is on — this uses mobile data.
- **Security**: only a small supervisor runs as root; everything that parses data from the phone or the network runs in an unprivileged worker (see *Privilege separation*). The supervisor passes values to external commands only after validating them and formatting them as numeric addresses.

## 8. Where to look in the code

| Topic | Function(s) (`src/main.c` unless noted) |
|-------|-------------------------|
| Device discovery | `find_device`, `is_rndis_ctrl` |
| RNDIS handshake | `rndis_cmd`, `rndis_init`, `rndis_halt`, `answer_keepalive` |
| USB data path | `send_frame`, `rx_parse`, `rx_thread`, `tx_thread`, `ctrl_thread` |
| Ethernet ↔ IP | `rx_frame`, `tx_thread` |
| DHCP / ARP | `dhcp_build`, `dhcp_build_renew`, `dhcp_parse` (`src/dhcp.c`) ; `dhcp_acquire`, `handle_arp` |
| DHCP lease lifecycle (T1, T2, expiry) | `dhcp_client_bound`, `dhcp_client_tick`, `dhcp_client_input` (`src/dhcp.c`) |
| IPv6 (NDP, RA, SLAAC) | `icmp6_csum`, `build_na`, `parse_ra`, `build_rs`, `handle_ipv6`, `configure_ipv6` |
| `utun`, routes, DNS | `utun_open`, `configure_iface`, `dns_apply`, `unconfigure_iface` |
| Service control | `is_disabled`, `set_status`, `status_tick`, `adb_enable`, `session`, `main` |
| Privilege separation (root side) | `supervisor_main`, `run_worker_once`, `handle_request` |
| Request validation / IPC | `policy_check_v4`, `policy_check_v6`, `policy_check_status` (`src/policy.c`) ; `ipc_send`, `ipc_recv` (`src/ipc.c`) ; worker side: `worker_main`, `drop_privileges`, `ipc_call` |
