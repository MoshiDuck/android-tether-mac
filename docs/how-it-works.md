# How TetherMac works

*Français : [how-it-works.fr.md](how-it-works.fr.md)*

TetherMac gives a Mac internet access through an Android phone's USB tethering, **without any kernel extension**. This page explains the problem, the approach, and each moving part, so you can audit, debug or extend it.

Source: [`main.c`](../main.c) (driver, ~700 lines of C) and [`menubar/RNDISTether.swift`](../menubar/RNDISTether.swift) (menu bar icon).

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

- **DHCP client**: sends `DISCOVER`, takes the `OFFER`, sends `REQUEST`, receives `ACK` — as raw Ethernet/IP/UDP frames with the broadcast flag, since we have no address yet. It reads address, netmask, router, DNS and lease time. The lease is renewed at half its length with a new `REQUEST`.
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

Instead of replacing the default route, it adds two **more specific** routes, `0.0.0.0/1` and `128.0.0.0/1`, via the phone. Together they cover all of IPv4 and beat the default route, but the existing default route stays untouched. Removing the two routes restores everything. When an IPv6 address has been configured, the same trick is applied with `::/1` and `8000::/1`, so IPv6 traffic does not leak around the tether through Wi-Fi.

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

## 7. Trade-offs and limitations

- **Throughput**: packets cross the kernel/userspace boundary and are copied; a native driver would be somewhat faster. On a Galaxy S24 Ultra, about 42 Mbit/s was measured, probably limited by the mobile network rather than the driver.
- **Always-on service**: if the process stops, the network disappears (it restarts automatically under launchd).
- **IPv6 depends on the phone**: it needs a Router Advertisement with a `/64` prefix (validated on a Galaxy S24 Ultra, whose mobile network provides IPv6). Without one, nothing IPv6 is configured and the tether stays IPv4-only. IPv6 DNS servers announced by the router (RDNSS) are not used; names are resolved through the phone's IPv4 DNS server, which also answers `AAAA` queries. No speed difference between IPv4 and IPv6 was measurable in tests; the mobile network dominates.
- **Tested on one phone** (Galaxy S24 Ultra, Android 16, One UI 8). Phones with different RNDIS quirks may need changes.
- **All traffic via the phone** while connected with `-r`, even if Wi-Fi is on — this uses mobile data.
- **Security**: the service runs as root. It only processes data from the USB device you plug in and from the tether, and it passes values read from the phone to external commands only after formatting them as numeric IP addresses.

## 8. Where to look in the code

| Topic | Function(s) in `main.c` |
|-------|-------------------------|
| Device discovery | `find_device`, `is_rndis_ctrl` |
| RNDIS handshake | `rndis_cmd`, `rndis_init`, `rndis_halt`, `answer_keepalive` |
| USB data path | `send_frame`, `rx_parse`, `rx_thread`, `tx_thread`, `ctrl_thread` |
| Ethernet ↔ IP | `rx_frame`, `tx_thread` |
| DHCP / ARP | `build_dhcp`, `dhcp_parse`, `dhcp_acquire`, `handle_arp` |
| IPv6 (NDP, RA, SLAAC) | `icmp6_csum`, `build_na`, `parse_ra`, `build_rs`, `handle_ipv6`, `configure_ipv6` |
| `utun`, routes, DNS | `utun_open`, `configure_iface`, `dns_apply`, `unconfigure_iface` |
| Service control | `is_disabled`, `set_status`, `status_tick`, `adb_enable`, `session`, `main` |
