# Security policy

## Why this matters

TetherMac's background service parses data sent by a USB device (the phone) and by the network behind it. Memory-safety bugs in that parsing are security bugs. The test suite (`make test`) fuzzes these parsers under AddressSanitizer/UBSan.

**Trust model.** The service is split in two processes:

- the **supervisor** runs as root, is small, and never parses data from the phone or the network. It creates the `utun` interface, applies network configuration only after validating it (`src/policy.c`), and cleans up;
- the **worker** does all the parsing (USB, RNDIS, DHCP, NDP, packets) as an unprivileged user (`nobody`) and can only ask the supervisor for six precise actions over a fixed-size channel (`src/ipc.h`).

The worker is treated as *untrusted*. The worker is not sandboxed yet (see the [roadmap](ROADMAP.md)).

## Supported versions

Only the latest release receives security fixes.

| Version | Supported |
|---------|-----------|
| latest release | ✅ |
| older releases | ❌ — please upgrade |

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Use GitHub's private reporting: on the repository page go to **Security → Report a vulnerability**. If that option is unavailable, contact the maintainer through their GitHub profile (<https://github.com/MoshiDuck>) and ask for a private channel.

Please include:

- what the problem is and which component is affected (a function in `src/`, the install scripts, the menu bar app);
- how to reproduce it (a crafted USB descriptor, a malformed RNDIS/DHCP packet, a script, …);
- the version (`brew info android-tether-mac` or the git commit).

You can expect an acknowledgement within a few days. Fixes are released as soon as they are ready and credited in [CHANGELOG.md](CHANGELOG.md), unless you prefer to stay anonymous.

## Scope notes

- The service trusts the *local administrator*: anyone who can already run commands as root can do anything, so that is out of scope.
- The service does not trust the phone or the network: out-of-bounds reads, crashes caused by malformed input, and command injection through values read from the device are all in scope.
- **Privilege boundary issues are in scope and valuable**: a way for the worker to make the supervisor run something it should not, to apply a configuration that `policy.c` should have refused, to regain root, or to leak the supervisor's file descriptors.
