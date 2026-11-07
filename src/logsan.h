// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Assainissement des lignes de journal envoyées par le worker.
 *
 * Le worker ne tient plus le fichier journal : il écrit dans un tube, et le superviseur (root) recopie chaque
 * ligne après l'avoir nettoyée. Un worker détourné ne peut donc ni se faire passer pour le superviseur, ni
 * glisser des séquences d'échappement dans le journal, ni le remplir sans limite (voir rate_allow, policy.h).
 * Aucune entrée/sortie : testé sans root.
 */
#ifndef LOGSAN_H
#define LOGSAN_H

#include <stddef.h>

#define LOG_LINE_MAX 400            /* longueur maximale d'une ligne recopiée, marque de troncature comprise */

/* Nettoie `in` (n octets, sans exigence de zéro final) dans `out` (cap octets, toujours terminé par un zéro) et
 * retourne la longueur écrite :
 *  - retire les fins de ligne ;
 *  - remplace les caractères de contrôle (dont ESC) par « ? » ;
 *  - retire un préfixe « [worker] » (le superviseur remet le sien : aucune ligne ne peut passer pour « [root] ») ;
 *  - tronque à LOG_LINE_MAX - 1 octets avec la marque « [...] ». */
size_t log_sanitize(const char *in, size_t n, char *out, size_t cap);

#endif
