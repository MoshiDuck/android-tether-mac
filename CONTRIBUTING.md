# Contributing

Thanks for helping! The most valuable contributions:

- **Phone reports**: open an issue with your phone model, its USB id (`ioreg -p IOUSB -l -w0 | grep -E 'idVendor|idProduct'`), and the output of `sudo ./rndis-tether -v -r -a`.
- **Fixes and features**: IPv6, a "failover only" mode, Homebrew packaging, other RNDIS quirks.

## Pull requests

1. Fork, create a branch, keep changes focused.
2. `make` must build without warnings; `menubar/build.sh` for the menu bar app.
3. Describe what you tested (phone model, macOS version).
4. By submitting a PR you agree your contribution is licensed under GPL-3.0-or-later, like the rest of the project. Add a `Signed-off-by:` line (`git commit -s`) to certify you have the right to submit it ([DCO](https://developercertificate.org)).
