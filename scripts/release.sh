#!/bin/sh
# Publie une version : tag Git + mise à jour de la formule Homebrew du tap.
#
# Usage :  scripts/release.sh 0.2.0
#
# Prérequis : arbre Git propre sur main, dépôt du tap cloné à côté
# (../homebrew-tap, ou TAP_DIR=/chemin/vers/homebrew-tap).
set -e

VER="$1"
case "$VER" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "Usage: $0 MAJEUR.MINEUR.PATCH  (ex. 0.2.0)" >&2; exit 1 ;;
esac

cd "$(dirname "$0")/.."
TAP_DIR="${TAP_DIR:-$(cd .. && pwd)/homebrew-tap}"
FORMULA="$TAP_DIR/Formula/android-tether-mac.rb"
REPO=MoshiDuck/android-tether-mac

[ -f "$FORMULA" ] || { echo "Formule introuvable : $FORMULA" >&2; exit 1; }
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || { echo "Passe sur la branche main." >&2; exit 1; }
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "Des modifications ne sont pas commitées." >&2
    exit 1
fi
git rev-parse "v$VER" >/dev/null 2>&1 && { echo "Le tag v$VER existe déjà." >&2; exit 1; }
[ "$(cat VERSION)" = "$VER" ] || { echo "Le fichier VERSION contient \"$(cat VERSION)\" : mets-le à \"$VER\", commite, puis relance." >&2; exit 1; }

echo "==> Tag v$VER"
git tag -a "v$VER" -m "v$VER"
git push origin main "v$VER"

URL="https://github.com/$REPO/archive/refs/tags/v$VER.tar.gz"
echo "==> Calcul du sha256 de $URL"
SHA=$(curl -fsSL --retry 5 --retry-delay 2 "$URL" | shasum -a 256 | cut -d' ' -f1)
[ -n "$SHA" ] || { echo "sha256 vide, abandon." >&2; exit 1; }

echo "==> Mise à jour de la formule ($SHA)"
sed -i '' -e "s|^  url .*|  url \"$URL\"|" -e "s|^  sha256 .*|  sha256 \"$SHA\"|" "$FORMULA"
git -C "$TAP_DIR" add -A
git -C "$TAP_DIR" commit -q -m "android-tether-mac $VER"
git -C "$TAP_DIR" push

echo "OK : v$VER publiée. Les utilisateurs mettent à jour avec :"
echo "  brew update && brew upgrade android-tether-mac && tethermac-install"
