# Releasing

How a change reaches Homebrew users.

| Channel | Who gets it | When |
|---------|-------------|------|
| `brew install --HEAD` | people who want the latest code | every push to `main` (CI checks that the HEAD install works) |
| `brew install` / `brew upgrade` | everyone | when a version tag `vX.Y.Z` is released |

A *stable* release on every commit would flood users with versions and defeat semantic versioning, so stable releases are cut on demand with one command.

## Publishing a version (for example `0.1.2`)

1. **Make sure `main` is green**: CI passes (`make strict`, `make test`, `make analyze`, coverage thresholds).
2. **Set `VERSION`** to `X.Y.Z` (during development it holds `X.Y.Z-dev`; the release script and the release workflow refuse a tag that does not match it).
3. **Update `CHANGELOG.md`**: move the entries from *Unreleased* under a new `## [X.Y.Z] - YYYY-MM-DD` heading and update the comparison links at the bottom. The release workflow refuses a tag without that section.
4. **Test on a real phone** when the change touches USB, DHCP, IPv6, routes, DNS or privilege handling: `sudo ./install.sh` and check `/var/log/rndis-tether.log`.
5. **Commit** `VERSION` and the changelog on `main` with a clean working tree.
6. **Run** `scripts/release.sh X.Y.Z` from the repository root. It creates the annotated tag, pushes `main` and the tag, updates the Homebrew formula in the tap (`../homebrew-tap`, or `TAP_DIR=…`) and pushes it.
7. **The `Release` workflow then runs on the tag** and:
   - rebuilds and retests;
   - creates the GitHub Release with the changelog section as notes;
   - updates the formula if the tap token is configured (it is idempotent: it does nothing when the script already did it);
   - **verifies `brew install` from the tap** on a clean runner and checks the installed version.

## One-time setup: let CI update the tap

Without this, `scripts/release.sh` updates the tap itself and CI only verifies it.

1. Create a **fine-grained personal access token** limited to the `homebrew-tap` repository with *Contents: Read and write*.
2. In this repository: *Settings → Secrets and variables → Actions → New repository secret*, name `HOMEBREW_TAP_TOKEN`.

## Rules

- Never move or delete a published tag: it changes the archive checksum and breaks `brew install` for everyone who has the formula. Publish a new patch version instead.
- Versions follow [Semantic Versioning](https://semver.org/). While the major version is `0`, minor bumps may include behaviour changes.
- Security fixes are announced in the changelog under *Security*, and the release says that users should upgrade.
