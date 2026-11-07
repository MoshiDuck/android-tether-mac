// SPDX-License-Identifier: GPL-3.0-or-later
/* Lecture/écriture d'entiers dans des tampons d'octets, indépendante de l'endianness de la machine. */
#ifndef BYTES_H
#define BYTES_H

#include <stdint.h>

/* little-endian (RNDIS) */
static inline void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline uint32_t get32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* big-endian (ordre réseau) */
static inline uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
static inline void putbe16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void putbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

#endif
