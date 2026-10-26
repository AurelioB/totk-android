/* Opt-in primary-command-buffer timestamps. Slot lifetime/publication is supplied by nvn_vk.c's
   existing fence queue; only the completion thread owns timing aggregates. No additional waits,
   barriers or submissions. CPU audit state belongs exclusively to the recording lock. */
#include "vk_int.h"

#define GP_RECORDS 2048
#define GP_GROUPS 16384
#define GP_WINDOW 300

typedef struct { uint32_t format, w, h, load, store, bytes; } gp_attachment;
typedef struct {
    gp_attachment a[10];
    uint64_t shader;
    uint32_t w, h, samples, colors, bucket, compute;
} gp_key;
typedef struct { gp_key key; uint64_t frame; uint32_t draws, instance, reason, detail, stats_q1; } gp_record;
struct vk_gpuprof_slot {
    VkQueryPool pool, stats;
    uint32_t n, dropped, ns;
    uint64_t sealed;
    gp_record rec[GP_RECORDS];
    uint64_t ticks[GP_RECORDS * 2];
    uint64_t fragments[GP_RECORDS];
};
typedef struct { gp_key key; uint64_t hash, count, draws, fragments, stats_count; double ms; } gp_group;
typedef struct {
    gp_key key;
    uint64_t lo[10], hi[10];
    uint32_t stored, read, load_dead;
} gp_audit;

int vk_gpuprof_on;
static int stats_on;
static vk_gpuprof_slot *current;
static gp_record *open_record;
static gp_audit open_audit;
static gp_audit *audit;
static size_t audit_n, audit_cap;
static uint64_t frame = 1;
static uint32_t frame_instance;
static uint64_t audit_load_passes, audit_store_passes, audit_load_ops, audit_store_ops;
static uint64_t audit_overflow;
/* Timing aggregates below are accessed ONLY by the completion thread. */
static gp_group groups[GP_GROUPS];
static uint64_t window, total_records, dropped_records, failed_slots, loads, clears, stores, discards;
static uint64_t stats_failures;
static uint64_t load_bytes, store_bytes, unknown_bytes;
static double total_ms, render_ms, compute_ms;
static const char *const end_names[] = {
    "other", "nvn-barrier", "texture-barrier", "attachment-change", "clear",
    "copy-blit", "query-report", "compute-dispatch", "cpu-wait", "submission-end",
    "uniform-flush", "present", "diagnostic", "deferred-barrier-hazard"
};

static uint64_t hash_bytes(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static uint64_t key_hash(const gp_key *key) { return hash_bytes(UINT64_C(14695981039346656037), key, sizeof *key); }
static uint32_t bucket(uint32_t draws) { uint32_t b = 0; while (draws) { b++; draws >>= 1; } return b; }
/* Core VkFormat families are contiguous. Use the VIEW format, not guest storage: a BC
   texture can have an uncompressed attachment view and depth formats can be substituted. */
static uint32_t texel_bytes(VkFormat f) {
    if (f >= VK_FORMAT_R4G4B4A4_UNORM_PACK16 && f <= VK_FORMAT_A1R5G5B5_UNORM_PACK16) return 2;
    if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB) return 1;
    if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) return 2;
    if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) return 3;
    if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) return 4;
    if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) return 2;
    if (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) return 4;
    if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) return 6;
    if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT) return 4;
    if (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT) return 8;
    if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT) return 12;
    if (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) return 16;
    if (f == VK_FORMAT_D16_UNORM) return 2;
    if (f == VK_FORMAT_S8_UINT) return 1;
    if (f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_X8_D24_UNORM_PACK32 ||
        f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) return 4;
    return 0;
}
static int rank_cmp(const void *a, const void *b) {
    const gp_group *x = *(gp_group *const *)a, *y = *(gp_group *const *)b;
    return x->ms < y->ms ? 1 : x->ms > y->ms ? -1 : x->hash < y->hash ? -1 : x->hash > y->hash;
}
static void report(void) {
    gp_group *rank[GP_GROUPS]; size_t n = 0;
    for (size_t i = 0; i < GP_GROUPS; i++) if (groups[i].count) rank[n++] = &groups[i];
    qsort(rank, n, sizeof *rank, rank_cmp);
    LOGV("[gpu-profile] frames %llu-%llu: measured %.3f ms/frame (render %.3f compute %.3f), %.2f passes+dispatches/frame; LOAD %.2f CLEAR %.2f STORE %.2f DONT_CARE-store %.2f ops/frame; logical attachment estimate load %.3f store %.3f MiB/frame (not actual traffic); unknown-byte attachments %llu; dropped %llu failed-slots %llu",
         (unsigned long long)(window * GP_WINDOW + 1), (unsigned long long)((window + 1) * GP_WINDOW),
         total_ms / GP_WINDOW, render_ms / GP_WINDOW, compute_ms / GP_WINDOW, total_records / (double)GP_WINDOW,
         loads / (double)GP_WINDOW, clears / (double)GP_WINDOW, stores / (double)GP_WINDOW, discards / (double)GP_WINDOW,
         load_bytes / (1048576.0 * GP_WINDOW), store_bytes / (1048576.0 * GP_WINDOW),
         (unsigned long long)unknown_bytes, (unsigned long long)dropped_records, (unsigned long long)failed_slots);
    for (size_t i = 0; i < n && i < 15; i++) {
        const gp_group *g = rank[i]; const gp_key *k = &g->key;
        char attachments[800]; size_t used = 0; attachments[0] = 0;
        for (int j = 0; j < 10; j++) if (k->a[j].format) {
            const gp_attachment *a = &k->a[j];
            int nn = snprintf(attachments + used, sizeof attachments - used, " %c%d=%u:%ux%u:%u/%u", j < 8 ? 'c' : 'd', j, a->format, a->w, a->h, a->load, a->store);
            if (nn < 0 || (size_t)nn >= sizeof attachments - used) break;
            used += (size_t)nn;
        }
        LOGV("[gpu-profile] #%zu sig=%016llx %s %ux%u samples=%u colors=%u draws-bucket=%u shader=%016llx: %.3f ms/frame %.2f%% %.2f instances/frame %.2f draws/frame; attachments(fmt:WxH:load/store)%s",
             i + 1, (unsigned long long)g->hash, k->compute ? "compute" : "render", k->w, k->h, k->samples, k->colors, k->bucket,
             (unsigned long long)k->shader, g->ms / GP_WINDOW, total_ms ? 100 * g->ms / total_ms : 0,
             g->count / (double)GP_WINDOW, g->draws / (double)GP_WINDOW, attachments);
        LOGV("[gpu-profile scale] sig=%016llx: %.2f draws/instance, %.1f ns/draw (whole pass); fragment-stats %s valid=%llu/%llu invocations=%.0f/frame %.0f/instance %.3f ns/invocation (whole pass)",
             (unsigned long long)g->hash, g->count ? g->draws / (double)g->count : 0, g->draws ? g->ms * 1e6 / g->draws : 0,
             stats_on ? "on" : "off", (unsigned long long)g->stats_count, (unsigned long long)g->count,
             g->fragments / (double)GP_WINDOW, g->stats_count ? g->fragments / (double)g->stats_count : 0, g->fragments ? g->ms * 1e6 / g->fragments : 0);
    }
    memset(groups, 0, sizeof groups);
    total_records = dropped_records = failed_slots = loads = clears = stores = discards = 0;
    if (stats_on) LOGV("[gpu-profile scale] fragment-stat failed slots=%llu; counts are implementation-defined invocations, not covered pixels", (unsigned long long)stats_failures);
    stats_failures = 0;
    load_bytes = store_bytes = unknown_bytes = 0; total_ms = render_ms = compute_ms = 0;
}
static void advance(uint64_t target) { while (window < target) { report(); window++; } }

void vk_gpuprof_slot_begin(vk_gpuprof_slot **slot, VkCommandBuffer cb) {
    static int initialized;
    if (!initialized) {
        initialized = 1;
        const char *e = getenv("TOTK_VK_GPU_PROFILE");
        if (e && (!strcmp(e, "1") || !strcmp(e, "2"))) {
            vk_gpuprof_on = V.timestamp_bits && V.props.limits.timestampComputeAndGraphics && V.props.limits.timestampPeriod > 0 ? atoi(e) : 0;
            LOGV("[gpu-profile] %s (TOTK_VK_GPU_PROFILE=%s); 300-frame windows, primary passes only; recorder workers must be disabled",
                 vk_gpuprof_on ? "enabled" : "unavailable: queue/device lacks graphics+compute timestamps", e);
            const char *se = getenv("TOTK_VK_GPU_PROFILE_STATS");
            int requested = se && !strcmp(se, "1");
            stats_on = vk_gpuprof_on && requested && V.has_pipeline_statistics;
            if (requested) LOGV("[gpu-profile] fragment-invocation statistics %s (pipelineStatisticsQuery=%d; TOTK_VK_GPU_PROFILE_STATS=1)", stats_on ? "on" : "unavailable", V.has_pipeline_statistics);
        }
    }
    if (!vk_gpuprof_on) return;
    if (!*slot) {
        *slot = calloc(1, sizeof **slot); if (!*slot) DIE("GPU profiler slot allocation");
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_RECORDS * 2};
        VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &(*slot)->pool));
        if (stats_on) {
            qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = GP_RECORDS;
            qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
            VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &(*slot)->stats));
        }
    }
    current = *slot; current->n = current->dropped = 0; current->sealed = 0;
    current->ns = 0;
    vkCmdResetQueryPool(cb, current->pool, 0, GP_RECORDS * 2);
    if (stats_on) vkCmdResetQueryPool(cb, current->stats, 0, GP_RECORDS);
}
void vk_gpuprof_slot_seal(vk_gpuprof_slot *slot) {
    if (!slot) return;
    slot->sealed = frame - 1; current = NULL;
}
void vk_gpuprof_slot_complete(vk_gpuprof_slot *slot) {
    if (!slot) return;
    VkResult result = slot->n ? vkGetQueryPoolResults(V.dev, slot->pool, 0, slot->n * 2,
        slot->n * 2 * sizeof(uint64_t), slot->ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) : VK_SUCCESS;
    VkResult sr = stats_on && slot->ns ? vkGetQueryPoolResults(V.dev, slot->stats, 0, slot->ns,
        slot->ns * sizeof(uint64_t), slot->fragments, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) : VK_SUCCESS;
    stats_failures += sr != VK_SUCCESS;
    for (uint32_t i = 0; i < slot->n; i++) {
        gp_record *r = &slot->rec[i]; advance((r->frame - 1) / GP_WINDOW);
        if (result) continue;
        /* Unsigned subtraction also handles wrap on devices with fewer than 64 timestamp bits. */
        uint64_t delta = slot->ticks[2 * i + 1] - slot->ticks[2 * i];
        if (V.timestamp_bits < 64) delta &= (UINT64_C(1) << V.timestamp_bits) - 1;
        double ms = (double)delta * V.props.limits.timestampPeriod / 1e6;
        r->key.bucket = bucket(r->draws);
        total_records++; total_ms += ms; if (r->key.compute) compute_ms += ms; else render_ms += ms;
        for (int j = 0; j < 10; j++) {
            const gp_attachment *a = &r->key.a[j]; if (!a->format) continue;
            uint64_t bytes = (uint64_t)r->key.w * r->key.h * a->bytes * r->key.samples;
            unknown_bytes += !a->bytes;
            if (a->load == VK_ATTACHMENT_LOAD_OP_LOAD) { loads++; load_bytes += bytes; }
            if (a->load == VK_ATTACHMENT_LOAD_OP_CLEAR) clears++;
            if (a->store == VK_ATTACHMENT_STORE_OP_STORE) { stores++; store_bytes += bytes; } else discards++;
        }
        uint64_t hash = key_hash(&r->key); size_t at = hash % GP_GROUPS, tries = 0;
        if (vk_gpuprof_on == 2) {
            const char *reason = r->reason < sizeof end_names / sizeof *end_names ? end_names[r->reason] : "unknown";
            LOGV("[gpu-split] frame=%llu instance=%u sig=%016llx kind=%s end=%s detail=%#x draws=%u ms=%.4f; emitted-tail srcStage=%#llx srcAccess=%#llx dstStage=%#llx dstAccess=%#llx",
                 (unsigned long long)r->frame, r->instance, (unsigned long long)hash, r->key.compute ? "compute" : "render", reason, r->detail, r->draws, ms,
                 (unsigned long long)VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, (unsigned long long)VK_ACCESS_2_MEMORY_WRITE_BIT,
                 (unsigned long long)VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, (unsigned long long)(VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
        }
        while (groups[at].count && (groups[at].hash != hash || memcmp(&groups[at].key, &r->key, sizeof r->key)) && ++tries < GP_GROUPS) at = (at + 1) % GP_GROUPS;
        if (tries == GP_GROUPS) { dropped_records++; continue; }
        gp_group *g = &groups[at]; if (!g->count) { g->key = r->key; g->hash = hash; }
        g->count++; g->draws += r->draws; g->ms += ms;
        if (r->stats_q1 && sr == VK_SUCCESS) {
            g->fragments += slot->fragments[r->stats_q1 - 1]; g->stats_count++;
        }
    }
    dropped_records += slot->dropped; failed_slots += result != VK_SUCCESS;
    if (slot->sealed) advance(slot->sealed / GP_WINDOW);
}
static void begin_record(VkCommandBuffer cb, int compute) {
    open_record = NULL; memset(&open_audit, 0, sizeof open_audit);
    if (!current || current->n == GP_RECORDS) { if (current) current->dropped++; audit_overflow++; return; }
    open_record = &current->rec[current->n]; memset(open_record, 0, sizeof *open_record);
    open_record->frame = frame; open_record->key.compute = (uint32_t)compute;
    open_record->instance = ++frame_instance;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, current->n * 2);
    if (!compute && stats_on) {
        open_record->stats_q1 = ++current->ns;
        vkCmdBeginQuery(cb, current->stats, open_record->stats_q1 - 1, 0);
    }
}
void vk_gpuprof_read(uint64_t address, uint64_t size) {
    if (!address || !size) return;
    uint64_t end = size > UINT64_MAX - address ? UINT64_MAX : address + size;
    for (size_t i = 0; i < audit_n; i++) {
        uint32_t live = audit[i].stored & ~audit[i].read;
        while (live) {
            int j = __builtin_ctz(live); live &= live - 1;
            if (audit[i].lo[j] < end && address < audit[i].hi[j]) audit[i].read |= 1u << j;
        }
    }
}
void vk_gpuprof_begin(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat formats[10],
                     const tex_t *const textures[10], const uint32_t widths[10], const uint32_t heights[10], uint32_t samples) {
    begin_record(cb, 0); if (!open_record) return;
    gp_key *k = &open_record->key; k->w = ri->renderArea.extent.width; k->h = ri->renderArea.extent.height;
    k->samples = samples; k->colors = ri->colorAttachmentCount;
    for (int j = 0; j < 10; j++) {
        const VkRenderingAttachmentInfo *a = j < 8 ? (j < (int)ri->colorAttachmentCount ? &ri->pColorAttachments[j] : NULL) : j == 8 ? ri->pDepthAttachment : ri->pStencilAttachment;
        if (!a || !a->imageView || !textures[j]) continue;
        const tex_t *t = textures[j]; uint32_t bytes = texel_bytes(formats[j]);
        /* Depth and stencil are distinct load/store operations: do not count a packed DS image twice. */
        if (j == 9) bytes = 1;
        else if (j == 8 && formats[j] == VK_FORMAT_D32_SFLOAT_S8_UINT) bytes = 4;
        else if (j == 8 && formats[j] == VK_FORMAT_D24_UNORM_S8_UINT) bytes = 3;
        k->a[j] = (gp_attachment){formats[j], widths[j], heights[j], a->loadOp, a->storeOp, bytes};
        open_audit.lo[j] = (uint64_t)(uintptr_t)t->mem;
        open_audit.hi[j] = open_audit.lo[j] + t->L.size;
        if (a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) vk_gpuprof_read(open_audit.lo[j], t->L.size);
        if (a->storeOp == VK_ATTACHMENT_STORE_OP_STORE) open_audit.stored |= 1u << j;
    }
}
void vk_gpuprof_dispatch_begin(VkCommandBuffer cb) { begin_record(cb, 1); }
void vk_gpuprof_draw(vk_shader *const shaders[6], int compute) {
    if (!open_record) return;
    if (!open_record->draws) {
        uint64_t h = UINT64_C(14695981039346656037);
        for (int i = 0; i < 6; i++) if (shaders[i]) { h = hash_bytes(h, &i, sizeof i); h = hash_bytes(h, shaders[i]->key, strlen(shaders[i]->key) + 1); }
        open_record->key.shader = h;
    }
    open_record->draws++;
    /* Conservatively regard every bound texture/storage image in the used stages as read. This
       deliberately over-retains stores, including storage writes, whole mip chains and aliases. */
    for (int st = 0; st < 6; st++) if (shaders[st] && (compute ? st == 5 : st < 2)) {
        for (int j = 0; j < 160; j++) {
            uint64_t handle = j < 128 ? S.tex[st][j] : S.img[st][j - 128]; if (!handle) continue;
            vk_treg t; if (vk_treg_get((int)(handle & 0xfffff), &t)) vk_gpuprof_read((uint64_t)(uintptr_t)t.t.mem, t.t.L.size);
            else for (size_t a = 0; a < audit_n; a++) audit[a].read = UINT32_MAX;
        }
    }
}
void vk_gpuprof_clear(uint32_t mask) {
    if (!open_record || open_record->draws) return;
    for (int j = 0; j < 10; j++) if ((mask >> j & 1) && open_record->key.a[j].format && open_record->key.a[j].load == VK_ATTACHMENT_LOAD_OP_LOAD) open_audit.load_dead |= 1u << j;
}
void vk_gpuprof_end(VkCommandBuffer cb, vk_pass_end_kind reason, uint32_t detail) {
    if (!open_record) return;
    open_record->reason = (uint32_t)reason; open_record->detail = detail;
    if (open_record->stats_q1) vkCmdEndQuery(cb, current->stats, open_record->stats_q1 - 1);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, current->n * 2 + 1);
    if (!open_record->key.compute) {
        if (audit_n == audit_cap) { audit_cap = audit_cap ? 2 * audit_cap : 256; audit = realloc(audit, audit_cap * sizeof *audit); if (!audit) DIE("GPU profiler audit allocation"); }
        open_audit.key = open_record->key; open_audit.key.bucket = bucket(open_record->draws);
        audit[audit_n++] = open_audit;
    }
    current->n++; open_record = NULL;
}
void vk_gpuprof_present(void) {
    for (size_t i = 0; i < audit_n; i++) {
        uint32_t unused = audit[i].stored & ~audit[i].read;
        audit_load_passes += audit[i].load_dead != 0; audit_store_passes += unused != 0;
        audit_load_ops += (uint32_t)__builtin_popcount(audit[i].load_dead); audit_store_ops += (uint32_t)__builtin_popcount(unused);
        /* Bound candidate logging: retain counts for all passes, show at most 15 signatures/window. */
        static uint64_t seen[15]; static unsigned ns;
        if ((frame - 1) % GP_WINDOW == 0 && i == 0) ns = 0;
        if (audit[i].load_dead || unused) {
            uint64_t h = key_hash(&audit[i].key); unsigned k = 0; while (k < ns && seen[k] != h) k++;
            if (k == ns && ns < 15) { seen[ns++] = h; LOGV("[gpu-profile audit] frame %llu sig=%016llx LOAD-before-full-clear=%#x STORE-no-later-observed-read=%#x (candidate only; cross-frame contents may be live)", (unsigned long long)frame, (unsigned long long)h, audit[i].load_dead, unused); }
        }
    }
    audit_n = 0;
    if (frame % GP_WINDOW == 0) {
        LOGV("[gpu-profile audit] frames %llu-%llu: LOAD-before-full-clear %.2f passes %.2f ops/frame; STORE-no-later-observed-read %.2f passes %.2f ops/frame; candidates only, never changes load/store ops; dropped %llu",
             (unsigned long long)(frame - GP_WINDOW + 1), (unsigned long long)frame,
             audit_load_passes / (double)GP_WINDOW, audit_load_ops / (double)GP_WINDOW,
             audit_store_passes / (double)GP_WINDOW, audit_store_ops / (double)GP_WINDOW, (unsigned long long)audit_overflow);
        audit_load_passes = audit_store_passes = audit_load_ops = audit_store_ops = audit_overflow = 0;
    }
    frame++;
    frame_instance = 0;
}
