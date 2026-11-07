// SPDX-License-Identifier: GPL-3.0-or-later
#include "logsan.h"

#include <string.h>

size_t log_sanitize(const char *in, size_t n, char *out, size_t cap) {
    static const char prefix[] = "[worker] ";
    static const char cut[] = "[...]";
    size_t o = 0, lim;
    if (cap == 0) return 0;
    lim = cap < LOG_LINE_MAX ? cap : LOG_LINE_MAX;                  /* place pour le zéro final comprise */
    while (n > 0 && (in[n - 1] == '\n' || in[n - 1] == '\r')) n--;
    if (n >= sizeof prefix - 1 && !memcmp(in, prefix, sizeof prefix - 1)) { in += sizeof prefix - 1; n -= sizeof prefix - 1; }
    if (n > lim - 1) {                                              /* trop long : on garde le début et on le dit */
        size_t keep = lim > sizeof cut ? lim - sizeof cut : 0;
        for (size_t i = 0; i < keep && o + 1 < lim; i++) {
            unsigned char c = (unsigned char)in[i];
            out[o++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
        }
        for (size_t i = 0; i < sizeof cut - 1 && o + 1 < lim; i++) out[o++] = cut[i];
        out[o] = '\0';
        return o;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    out[o] = '\0';
    return o;
}
