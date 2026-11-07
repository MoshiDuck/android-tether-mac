#!/bin/sh
# Vérifie les seuils de couverture (lignes) d'un rapport `llvm-cov report`.
#
# Usage : scripts/check-coverage.sh rapport.txt fichier=seuil [fichier=seuil ...]
# Exemple : scripts/check-coverage.sh coverage.txt dhcp.c=95 TOTAL=35
#
# Les seuils ne doivent que monter : quand la couverture progresse, relevez-les dans la CI.
set -eu

report="${1:?rapport llvm-cov manquant}"
shift
status=0

for rule in "$@"; do
    name="${rule%%=*}"
    min="${rule#*=}"
    # 10e colonne du rapport : couverture des lignes (ex. 99.20%)
    # le nom peut être préfixé d'un dossier selon la version de llvm-cov (dhcp.c ou src/dhcp.c)
    cover=$(awk -v n="$name" '$1 == n || (length($1) > length(n) && substr($1, length($1) - length(n)) == "/" n) { gsub("%", "", $10); print $10 }' "$report" | head -n 1)
    if [ -z "$cover" ]; then
        echo "couverture: $name introuvable dans $report ; rapport reçu :" >&2
        cat "$report" >&2
        status=1
        continue
    fi
    if awk -v c="$cover" -v m="$min" 'BEGIN { exit !(c + 0 >= m + 0) }'; then
        echo "couverture: $name ${cover}% >= ${min}% : OK"
    else
        echo "couverture: $name ${cover}% < ${min}% : INSUFFISANT" >&2
        status=1
    fi
done

exit "$status"
