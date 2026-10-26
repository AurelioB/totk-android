/* Indexed packed seeds, learned startup warming and asynchronous request persistence. */
#ifndef TOTK_VK_SEED_IO_H
#define TOTK_VK_SEED_IO_H
#include <stddef.h>
#include <stdint.h>
int vk_seed_io_enabled(void); /* default on; 0 retains legacy loose IO for diagnosis */
void vk_seed_io_start(const char *directory, const char *learned_index);
int vk_seed_io_active(void);
int vk_seed_io_ready(void); /* joins metadata only; warming continues asynchronously */
char *vk_seed_io_read(const char *name, size_t *size); /* owned mutable buffer; warmed ownership transfers */
char *vk_seed_file_read(const char *path, size_t *size);
int vk_seed_io_request(const char *directory, const char *variant, char *json, size_t size);
void vk_seed_io_published(const char *variant);
void vk_seed_io_stop(void); /* joins worker before freeing index/pack; safe with concurrent lookups */
typedef struct {
    uint64_t lookups, pack_hits, warmed_hits, demand_reads, demand_bytes, warmed_storage_reads;
    uint64_t blocking_ns, blocking_max_ns, worker_reads, worker_bytes, warm_joins, index_ns, warm_ns;
    uint64_t entries, pack_entries, cached_bytes, warm_keys, warm_ready, warm_failed, warm_skipped, warm_bytes;
    uint64_t learned_bytes, warm_cap, warm_bypassed;
} vk_seed_io_stats;
void vk_seed_io_stats_get(vk_seed_io_stats *out);
void vk_seed_io_report(void); /* default off: TOTK_SEED_STATS=1, delta per 300-present report */
#endif
