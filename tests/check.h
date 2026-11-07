// SPDX-License-Identifier: GPL-3.0-or-later
/* Mini-framework de test partagé : un compteur global, un message par échec. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

extern int checks, failures;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#endif
