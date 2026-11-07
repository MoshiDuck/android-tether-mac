# Contributing

Please follow the [Code of Conduct](CODE_OF_CONDUCT.md). For security problems see [SECURITY.md](SECURITY.md) instead of opening an issue.

Thanks for helping! The most valuable contributions:

- **Phone reports**: open an issue with your phone model, its USB id (`ioreg -p IOUSB -l -w0 | grep -E 'idVendor|idProduct'`), and the output of `sudo ./rndis-tether -v -r -a`.
- **Fixes and features**: IPv6 RDNSS support, a "failover only" mode, Homebrew packaging, other RNDIS quirks.

## Pull requests

1. Fork, create a branch, keep changes focused.
2. `make strict`, `make test` and `make analyze` must pass; `menubar/build.sh` must build the menu bar app. Any code that parses data from the phone or the network needs a test (`tests/`) — the fuzz tests run under AddressSanitizer. Code that runs in the root supervisor must go through `src/policy.c` and be tested there. New logic belongs in a module under `src/` with its own tests, not in `src/main.c`. Add a line to `CHANGELOG.md` under *Unreleased*.
3. Describe what you tested (phone model, macOS version).
4. By submitting a PR you agree your contribution is licensed under GPL-3.0-or-later, like the rest of the project. Add a `Signed-off-by:` line (`git commit -s`) to certify you have the right to submit it ([DCO](https://developercertificate.org)).
