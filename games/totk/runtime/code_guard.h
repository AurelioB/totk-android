/* Patch-site guards that carry no game code.
   A game patch must only touch the build of main it was written for, so it first checks the instructions at its sites.
   The repository and the APK hold none of those instructions: each guarded group stores the SHA-256 of the game's own
   words at its sites (4 bytes each, little-endian, in the listed order), and the check hashes the module's bytes. A patch
   that needs one of the guarded words (an instruction its veneer replays, a register field) takes it from the module
   after the check passed.

   Host tests and fixtures (no game files) define CODE_GUARD_STANDIN before including this header: every guard then
   expects code_guard_standin(offset) at each site instead of the game's word, so the tests lay out stand-in words and
   still exercise every refusal path. scripts/verify_code_guards.py checks the stored digests against a real main. */
#pragma once
#include "host_os.h"
#include "runtime.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* An arbitrary, fixed word per offset (splitmix-style mix): never a game word on purpose. */
static inline uint32_t code_guard_standin(uint64_t off) {
    uint64_t z = off + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull; z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return (uint32_t)(z ^ (z >> 31));
}

/* 1 when the n words have the SHA-256 sha256 (lowercase hex). */
static inline int code_guard_words(const uint32_t *words, size_t n, const char *sha256) {
    char hex[65]; host_sha256_hex(words, n * 4, hex);
    return !strcmp(hex, sha256);
}

/* Reads the word at each site of m into words (sites: n offsets, stride bytes apart, e.g. &table[0].off with
   sizeof *table) and returns 1 when they are the guarded group (sha256). Callers check the offsets lie in the module. */
static inline int code_guard(const Module *m, const uint64_t *sites, size_t n, size_t stride, uint32_t *words, const char *sha256) {
    const uint8_t *at = (const uint8_t *)sites;
    for (size_t i = 0; i < n; i++) { uint64_t off; memcpy(&off, at + i * stride, 8); memcpy(words + i, m->base + off, 4); }
#ifdef CODE_GUARD_STANDIN
    (void)sha256;
    for (size_t i = 0; i < n; i++) { uint64_t off; memcpy(&off, at + i * stride, 8); if (words[i] != code_guard_standin(off)) return 0; }
    return 1;
#else
    return code_guard_words(words, n, sha256);
#endif
}
