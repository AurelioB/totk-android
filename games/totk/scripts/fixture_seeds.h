/* Seed words for the host fixtures of the v143 patches (scripts/v143_*_fixture.c). The repository holds no game code,
   so a fixture lays out, at each guarded site, either
   - the player's own word, read by the verify script from their main NSO and handed over in the file named by
     $FIXTURE_SEEDS (records of a little-endian u64 offset and u32 word; fixture_seeds.py), so the production guards
     check their real digests and the emulated veneers replay the real instructions; or
   - built with -DCODE_GUARD_STANDIN (dataless unit tests), code_guard_standin(offset), which the guards then expect. */
#pragma once
#include "code_guard.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t fixture_seed(uint64_t off) {
#ifdef CODE_GUARD_STANDIN
    return code_guard_standin(off);
#else
    static struct { uint64_t off; uint32_t word; } *seeds; static size_t count;
    if (!seeds) {
        const char *path = getenv("FIXTURE_SEEDS");
        FILE *f = path ? fopen(path, "rb") : NULL;
        if (!f) { fprintf(stderr, "fixture: FIXTURE_SEEDS must name the seed file from the player's main\n"); exit(2); }
        unsigned char record[12];
        while (fread(record, 1, sizeof record, f) == sizeof record) {
            void *grown = realloc(seeds, (count + 1) * sizeof *seeds);
            if (!grown) { perror("realloc"); exit(2); }
            seeds = grown; memcpy(&seeds[count].off, record, 8); memcpy(&seeds[count].word, record + 8, 4); count++;
        }
        fclose(f);
    }
    for (size_t i = 0; i < count; i++) if (seeds[i].off == off) return seeds[i].word;
    fprintf(stderr, "fixture: no seed word for main+%#llx\n", (unsigned long long)off); exit(2);
#endif
}
static void fixture_seed_at(unsigned char *base, uint64_t off) { uint32_t w = fixture_seed(off); memcpy(base + off, &w, 4); }
