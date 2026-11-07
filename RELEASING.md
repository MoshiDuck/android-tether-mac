# Releasing

Maintainer checklist for publishing a version (for example `0.1.1`).

1. **Make sure `main` is green**: CI passes, and `make CFLAGS="-O2 -Wall -Wextra -Werror" && make test` pass locally.
2. **Update `CHANGELOG.md`**: move the entries from *Unreleased* under a new `## [X.Y.Z] - YYYY-MM-DD` heading, and update the comparison links at the bottom.
3. **Test on a real phone** when the change touches USB, DHCP, IPv6, routes or DNS: install with `sudo ./install.sh` and check `/var/log/rndis-tether.log`.
4. **Commit** the changelog on `main` with a clean working tree.
5. **Run the release script** from the repository root:

   ```sh
   scripts/release.sh X.Y.Z
   ```

   It creates the annotated tag `vX.Y.Z`, pushes `main` and the tag, computes the `sha256` of GitHub's source archive, updates the Homebrew formula in the tap checkout next to this repository (`../homebrew-tap`, or `TAP_DIR=…`), and pushes the tap.
6. **Check the install path**: `brew update && brew install MoshiDuck/tap/android-tether-mac` on a clean setup (or `brew upgrade`), then `brew test android-tether-mac`.
7. Optionally publish a GitHub Release from the tag, pasting the changelog section.

Notes:

- Never move or delete a published tag: it changes the archive checksum and breaks `brew install` for everyone who has the formula. Publish a new patch version instead.
- Versions follow [Semantic Versioning](https://semver.org/). While the major version is `0`, minor bumps may include behaviour changes.
