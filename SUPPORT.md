# Getting help

- **How it works / install problems**: read the [README](README.md) and [docs/how-it-works.md](docs/how-it-works.md) first.
- **It does not connect**: run `sudo ./rndis-tether -v -r -a` (or look at `/var/log/rndis-tether.log` if installed as a service) and open a [bug report](../../issues/new/choose) with that output, your phone model and its USB id.
- **My phone works / does not work**: please file a *Phone report* — it grows the compatibility table for everyone.
- **iPhone**: not supported, not needed — macOS handles iPhone tethering natively.
- **Security problems**: do not use public issues; see [SECURITY.md](SECURITY.md).
