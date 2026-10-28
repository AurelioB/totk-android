#ifndef TOTK_RUNTIME_ADRENO_GMEM_H
#define TOTK_RUNTIME_ADRENO_GMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*TotkAndroidChipProbe)(void *context, uint32_t *chip_id);

static inline bool totk_a722_sysmem_default(bool custom_driver, const char *tu_debug,
                                             TotkAndroidChipProbe probe, void *context,
                                             uint32_t *chip_id) {
    if (!custom_driver || tu_debug != NULL) return false;
    uint32_t observed_chip_id;
    if (!probe(context, &observed_chip_id) || observed_chip_id != 0x43020100u) return false;
    *chip_id = observed_chip_id;
    return true;
}

#endif
