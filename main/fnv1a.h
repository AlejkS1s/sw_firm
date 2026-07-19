#pragma once
#include <stddef.h>
#include <stdint.h>

/* FNV-1a, 32-bit.
 *
 *   h_0 = 2166136261                      (offset basis)
 *   h_i = (h_{i-1} XOR b_i) * 16777619    (mod 2^32, FNV prime)
 *
 * Single-pass, allocation-free, branch-light — safe to run on every
 * request on an 80MHz Xtensa core. This is a change-detection
 * fingerprint, not a security primitive: it is never used to authenticate
 * or verify anything, only to answer "did the state I'm hashing change
 * since last time".
 */
static inline uint32_t fnv1a_hash(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
