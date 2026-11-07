# Contributing

Please follow the [Code of Conduct](CODE_OF_CONDUCT.md). For security problems see [SECURITY.md](SECURITY.md) instead of opening an issue.

Thanks for helping! The most valuable contributions:

- **Phone reports**: open an issue with your phone model, its USB id (`ioreg -p IOUSB -l -w0 | grep -E 'idVendor|idProduct'`), and the output of `sudo ./rndis-tether -v -r -a`.
- **Fixes and features**: IPv6 RDNSS support, a "failover only" mode, Homebrew packaging, other RNDIS quirks.

## Pull requests

1. Fork, create a branch, keep changes focused.
2. `make CFLAGS="-O2 -Wall -Wextra -Werror"` and `make test` must pass; `menubar/build.sh` must build the menu bar app. Any code that parses data from the phone or the network needs a test in `tests/test_main.c` — the fuzz tests run under AddressSanitizer. Add a line to `CHANGELOG.md` under *Unreleased*.
3. Describe what you tested (phone model, macOS version).
4. By submitting a PR you agree your contribution is licensed under GPL-3.0-or-later, like the rest of the project. Add a `Signed-off-by:` line (`git commit -s`) to certify you have the right to submit it ([DCO](https://developercertificate.org)).
