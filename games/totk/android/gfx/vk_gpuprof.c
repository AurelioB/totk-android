/* Opt-in GPU timestamps. Slot lifetime/publication is supplied by nvn_vk.c's fence
   queue. Deferred pass-end tokens retain immutable query coordinates, so recorder
   assembly never touches the walking thread's current pass. */
#include "vk_int.h"
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>

#define GP_RECORDS 2048
#define GP_GROUPS 16384
#define GP_WINDOW 300
#define GP_SEGMENTS 64
#define GP_SPAN_BASE (GP_RECORDS * 2)
#define GP_PRE_QUERY (GP_SPAN_BASE + GP_SEGMENTS + 1)
#define GP_TIMESTAMPS (GP_PRE_QUERY + 1)
enum { GP_IA_VERTICES, GP_IA_PRIMITIVES, GP_VS, GP_CLIP_INVOCATIONS, GP_CLIP_PRIMITIVES, GP_FS, GP_STATS };
#define GP_STATS_MASK (VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | \
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT)

typedef struct { uint32_t format, w, h, load, store, bytes; } gp_attachment;
typedef struct {
    gp_attachment a[10];
    uint64_t shader;
    uint32_t w, h, samples, colors, compute;
} gp_key;
typedef struct {
    gp_key key; uint64_t frame, counters[GP_STATS];
    uint32_t draws, instance, reason, detail, stats_q1, stats_count, stats_invalid;
} gp_record;
typedef struct { uint64_t frame; uint32_t first, last; } gp_segment;
struct vk_gpuprof_slot {
    VkQueryPool pool, stats;
    uint32_t n, dropped, ns;
    uint64_t sealed;
    uint32_t nsegment, has_pre;
    VkCommandBuffer pre_cb;
    gp_segment segment[GP_SEGMENTS];
    uint64_t span_ticks[GP_SEGMENTS + 1], pre_tick;
    gp_record rec[GP_RECORDS];
    uint64_t ticks[GP_RECORDS * 2];
    uint64_t counters[GP_RECORDS][GP_STATS];
    uint32_t stats_owner[GP_RECORDS];
};
typedef struct {
    gp_key key; uint64_t hash, count, draws, counters[GP_STATS], stats_count, stats_draws;
    uint32_t ordinal, reference_w, reference_h, scene_relative;
    double ms, stats_ms;
} gp_group;
typedef struct {
    gp_key key;
    uint64_t lo[10], hi[10];
    uint32_t stored, read, load_dead;
} gp_audit;

int vk_gpuprof_on;
static int stats_on, secondary_pass;
static vk_gpuprof_slot *current;
static gp_record *open_record;
static gp_audit open_audit;
static VkCommandBuffer open_cb;
typedef struct {
    gp_record *parent; gp_audit audit; VkCommandBuffer parent_cb, cb;
    int secondary, skipped;
} gp_scope;
static gp_scope scopes[16];
static unsigned scope_n, scope_overflow;
static gp_audit *audit;
static size_t audit_n, audit_cap;
static uint64_t frame = 1;
static uint32_t frame_instance;
static uint64_t audit_load_passes, audit_store_passes, audit_load_ops, audit_store_ops;
static uint64_t audit_overflow;
/* Timing aggregates below are accessed ONLY by the completion thread. */
static gp_group groups[GP_GROUPS];
static struct {
    uint64_t frame, first, last, dropped;
    double measured;
    uint32_t scene_w, scene_h;
    int has_ticks, failed;
} completed;
static uint64_t last_completed, window_frames;
static double window_span, window_gap;
static int partial_report;
typedef struct {
    uint64_t frame, dropped; uint32_t n, capacity;
    double span_ms, measured_ms, gap_ms;
    gp_group *pass;
} gp_long_frame;
static gp_long_frame *long_ring;
static gp_long_frame frame_pending; /* one reusable completion-owner frame outside long-frame mode */
static uint32_t game_reference_w, game_reference_h; /* immutable fallback when no scene MRT was observed */
static uint32_t long_frames;
static double long_ms = 50;
static uint64_t window, total_records, dropped_records, failed_slots, loads, clears, stores, discards;
static uint64_t stats_failures;
static uint64_t load_bytes, store_bytes, unknown_bytes;
static double total_ms, render_ms, compute_ms;
static const char *const end_names[] = {
    "other", "nvn-barrier", "texture-barrier", "attachment-change", "clear",
    "copy-blit", "query-report", "compute-dispatch", "cpu-wait", "submission-end",
    "uniform-flush", "present", "diagnostic", "deferred-barrier-hazard", "early-submit-split"
};
static FILE *output_file;
static char output_path[PATH_MAX];
static uint64_t output_bytes, output_limit = UINT64_C(16) << 20, output_failures;
static uint64_t report_end;
static uint64_t env_u64(const char *name, uint64_t fallback, uint64_t minimum, uint64_t maximum) {
    const char *e = getenv(name); char *end = NULL;
    if (!e || !*e || *e == '-') return fallback;
    unsigned long long value = strtoull(e, &end, 10);
    return end && !*end && value >= minimum && value <= maximum ? value : fallback;
}
static void output_init(void) {
    char cwd[PATH_MAX]; struct timespec now;
    if (!getcwd(cwd, sizeof cwd) || clock_gettime(CLOCK_REALTIME, &now)) {
        LOGV("[gpu-profile] cannot resolve output game directory"); output_failures++; return;
    }
    int n = snprintf(output_path, sizeof output_path, "%s/gpuprof-%llu-%ld-%09ld.jsonl",
        cwd, (unsigned long long)now.tv_sec, (long)getpid(), now.tv_nsec);
    if (n < 0 || (size_t)n >= sizeof output_path - 2) { output_failures++; return; }
    output_limit = env_u64("TOTK_VK_GPU_PROFILE_MAX_BYTES", output_limit, 4096, UINT64_C(1) << 30);
    output_file = fopen(output_path, "wb");
    if (!output_file) output_failures++;
    LOGV("[gpu-profile] JSONL %s path=%s max-bytes=%llu rotations=3 (TOTK_VK_GPU_PROFILE_MAX_BYTES)",
        output_file ? "enabled" : "unavailable", output_path, (unsigned long long)output_limit);
}
static void output_row(const char *text) {
    if (!output_file) return;
    size_t n = strlen(text);
    if (n > output_limit) { output_failures++; return; }
    if (output_bytes + n > output_limit) {
        fclose(output_file); output_file = NULL;
        char from[PATH_MAX], to[PATH_MAX];
        for (int i = 3; i > 0; i--) {
            snprintf(to, sizeof to, "%s.%d", output_path, i);
            if (i == 1) snprintf(from, sizeof from, "%s", output_path);
            else snprintf(from, sizeof from, "%s.%d", output_path, i - 1);
            if (rename(from, to) && access(from, F_OK) == 0) {
                LOGV("[gpu-profile] JSONL rotation failed path=%s", from); output_failures++; return;
            }
        }
        output_file = fopen(output_path, "wb"); output_bytes = 0;
        if (!output_file) { output_failures++; return; }
    }
    if (fwrite(text, 1, n, output_file) != n) {
        LOGV("[gpu-profile] JSONL write failed path=%s", output_path);
        fclose(output_file); output_file = NULL; output_failures++; return;
    }
    output_bytes += n;
}
typedef struct { char data[8192]; size_t used; int failed; } gp_json;
static void json_add(gp_json *row, const char *format, ...) {
    if (row->failed) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(row->data + row->used, sizeof row->data - row->used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof row->data - row->used) { row->failed = 1; return; }
    row->used += (size_t)n;
}
static const char *kind_name(const gp_key *key) {
    static const char *const kinds[] = {"render", "compute", "upload", "transfer", "post", "upscale", "present", "framegen"};
    return key->compute < sizeof kinds / sizeof *kinds ? kinds[key->compute] : "other";
}
static uint32_t gcd_u32(uint32_t a, uint32_t b);
static void output_pass(const gp_group *g, const char *type, uint64_t pass_frame) {
    const gp_key *k = &g->key;
    gp_json row = {0};
    uint64_t start = pass_frame ? (pass_frame - 1) / GP_WINDOW * GP_WINDOW + 1 : window * GP_WINDOW + 1;
    uint64_t end = pass_frame ? start + GP_WINDOW - 1 : report_end;
    json_add(&row, "{\"schema\":3,\"type\":\"%s\",\"window_start\":%llu,\"window_end\":%llu,"
        "\"key\":\"%016llx\",\"kind\":\"%s\",\"ordinal\":%u,\"samples\":%u,\"shader\":\"%016llx\","
        "\"gpu_ms\":%.9f,\"count\":%llu,\"draws\":%llu,\"stats_valid\":%llu",
        type, (unsigned long long)start, (unsigned long long)end,
        (unsigned long long)g->hash, kind_name(k), g->ordinal, k->samples, (unsigned long long)k->shader,
        g->ms, (unsigned long long)g->count, (unsigned long long)g->draws, (unsigned long long)g->stats_count);
    json_add(&row, ",\"render_width\":%u,\"render_height\":%u,\"reference_width\":%u,\"reference_height\":%u,"
        "\"scene_relative\":%s", k->w, k->h, g->reference_w, g->reference_h, g->scene_relative ? "true" : "false");
    if (pass_frame) json_add(&row, ",\"frame\":%llu", (unsigned long long)pass_frame);
    json_add(&row, ",\"attachments\":[");
    int comma = 0;
    for (unsigned j = 0; j < 10; j++) if (k->a[j].format) {
        const gp_attachment *a = &k->a[j];
        uint32_t gw = gcd_u32(a->w, g->reference_w), gh = gcd_u32(a->h, g->reference_h);
        json_add(&row, "%s{\"slot\":%u,\"format\":%u,\"width\":%u,\"height\":%u,\"width_num\":%u,\"width_den\":%u,"
            "\"height_num\":%u,\"height_den\":%u,\"load\":%u,\"store\":%u}",
            comma++ ? "," : "", j, a->format, a->w, a->h, a->w / gw, g->reference_w / gw,
            a->h / gh, g->reference_h / gh, a->load, a->store);
    }
    json_add(&row, "]");
    if (g->stats_count) {
        static const char *const names[GP_STATS] = {"ia_vertices", "ia_primitives", "vs_invocations",
            "clipping_invocations", "clipping_primitives", "fs_invocations"};
        json_add(&row, ",\"stats_gpu_ms\":%.9f,\"stats_draws\":%llu,\"stats\":{",
            g->stats_ms, (unsigned long long)g->stats_draws);
        for (unsigned st = 0; st < GP_STATS; st++)
            json_add(&row, "%s\"%s\":%llu", st ? "," : "", names[st], (unsigned long long)g->counters[st]);
        json_add(&row, "},\"ratios\":{\"verts_per_draw\":%.9f,\"prims_per_draw\":%.9f,"
            "\"fragments_per_prim\":%.9f,\"ns_per_vertex\":%.9f,\"ns_per_primitive\":%.9f}",
            g->stats_draws ? g->counters[0] / (double)g->stats_draws : 0,
            g->stats_draws ? g->counters[1] / (double)g->stats_draws : 0,
            g->counters[1] ? g->counters[5] / (double)g->counters[1] : 0,
            g->counters[0] ? g->stats_ms * 1e6 / g->counters[0] : 0,
            g->counters[1] ? g->stats_ms * 1e6 / g->counters[1] : 0);
    }
    json_add(&row, "}\n");
    if (row.failed) output_failures++; else output_row(row.data);
}

static uint64_t hash_bytes(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b) { uint32_t r = a % b; a = b; b = r; }
    return a ? a : 1;
}
static uint64_t key_hash(const gp_key *key) {
    uint64_t h = UINT64_C(14695981039346656037);
    h = hash_bytes(h, &key->shader, sizeof key->shader);
    const uint32_t fields[] = {key->w, key->h, key->samples, key->colors, key->compute};
    h = hash_bytes(h, fields, sizeof fields);
    for (unsigned j = 0; j < 10; j++) {
        const gp_attachment *a = &key->a[j];
        const uint32_t values[] = {a->format, a->w, a->h, a->load, a->store};
        h = hash_bytes(h, values, sizeof values);
    }
    return h;
}
static int key_equal(const gp_key *a, const gp_key *b) {
    if (a->w != b->w || a->h != b->h || a->shader != b->shader || a->samples != b->samples ||
        a->colors != b->colors || a->compute != b->compute) return 0;
    for (unsigned j = 0; j < 10; j++) {
        const gp_attachment *x = &a->a[j], *y = &b->a[j];
        if (x->format != y->format || x->w != y->w || x->h != y->h ||
            x->load != y->load || x->store != y->store) return 0;
    }
    return 1;
}
static gp_long_frame *pending_frame(uint64_t number) {
    return long_ring ? &long_ring[number % long_frames] : &frame_pending;
}
static void capture_record(const gp_record *r, double ms, int stats_valid) {
    gp_long_frame *f = pending_frame(r->frame);
    if (f->frame != r->frame) { f->frame = r->frame; f->n = 0; f->dropped = 0; }
    if (f->n == GP_GROUPS) { f->dropped++; completed.dropped++; dropped_records++; return; }
    if (f->n == f->capacity) {
        uint32_t capacity = f->capacity ? f->capacity * 2 : 256;
        gp_group *passes = realloc(f->pass, capacity * sizeof *passes);
        if (!passes) { f->dropped++; completed.dropped++; dropped_records++; return; }
        f->pass = passes; f->capacity = capacity;
    }
    gp_group *g = &f->pass[f->n++];
    *g = (gp_group){.key = r->key, .hash = key_hash(&r->key), .ordinal = r->instance,
        .count = 1, .draws = r->draws, .ms = ms};
    if (stats_valid) {
        memcpy(g->counters, r->counters, sizeof g->counters);
        g->stats_count = 1; g->stats_draws = r->draws; g->stats_ms = ms;
    }
}
static uint64_t long_finish(uint64_t number, double span, double measured, double gap) {
    if (!long_ring) return 0;
    gp_long_frame *f = &long_ring[number % long_frames];
    if (f->frame != number) return 0;
    f->span_ms = span; f->measured_ms = measured; f->gap_ms = gap;
    if (span > long_ms) {
        for (uint32_t i = 0; i < f->n; i++) output_pass(&f->pass[i], "long_frame", number);
        LOGV("[gpu-profile long] frame=%llu span=%.3f measured=%.3f gap=%.3f passes=%u dropped=%llu threshold=%.3f ms",
            (unsigned long long)number, span, measured, gap, f->n, (unsigned long long)f->dropped, long_ms);
    }
    return f->dropped;
}
static unsigned active_colors(const gp_key *key) {
    unsigned colors = 0;
    for (unsigned j = 0; j < 8; j++) colors += key->a[j].format != 0;
    return colors;
}
static void aggregate_sample(const gp_group *sample) {
    const gp_key *key = &sample->key;
    total_records += sample->count; total_ms += sample->ms;
    if (key->compute == 1) compute_ms += sample->ms;
    else if (!key->compute) render_ms += sample->ms;
    for (unsigned j = 0; j < 10; j++) {
        const gp_attachment *a = &key->a[j]; if (!a->format) continue;
        uint64_t bytes = (uint64_t)key->w * key->h * a->bytes * key->samples;
        unknown_bytes += !a->bytes;
        if (a->load == VK_ATTACHMENT_LOAD_OP_LOAD) { loads++; load_bytes += bytes; }
        if (a->load == VK_ATTACHMENT_LOAD_OP_CLEAR) clears++;
        if (a->store == VK_ATTACHMENT_STORE_OP_STORE) { stores++; store_bytes += bytes; } else discards++;
    }
    uint64_t reference = ((uint64_t)sample->reference_w << 32) | sample->reference_h;
    size_t at = (sample->hash ^ sample->ordinal ^ reference ^ sample->scene_relative) % GP_GROUPS, tries = 0;
    while (groups[at].count && (groups[at].hash != sample->hash || groups[at].ordinal != sample->ordinal ||
        groups[at].reference_w != sample->reference_w || groups[at].reference_h != sample->reference_h ||
        groups[at].scene_relative != sample->scene_relative || !key_equal(&groups[at].key, key)) &&
        ++tries < GP_GROUPS) at = (at + 1) % GP_GROUPS;
    if (tries == GP_GROUPS) { dropped_records++; completed.dropped++; return; }
    gp_group *g = &groups[at];
    if (!g->count) { *g = *sample; return; }
    g->count += sample->count; g->draws += sample->draws; g->ms += sample->ms;
    for (unsigned st = 0; st < GP_STATS; st++) g->counters[st] += sample->counters[st];
    g->stats_count += sample->stats_count; g->stats_draws += sample->stats_draws; g->stats_ms += sample->stats_ms;
}
/* A frame can span several retired slots, and its largest scene MRT can be
   recorded after a shadow/prepass. Resolve the common reference only once the
   complete frame is known; query records cannot borrow retired-slot storage. */
static void finalize_passes(void) {
    gp_long_frame *f = pending_frame(completed.frame);
    if (f->frame != completed.frame) return;
    uint32_t w = completed.scene_w ? completed.scene_w : game_reference_w;
    uint32_t h = completed.scene_h ? completed.scene_h : game_reference_h;
    for (uint32_t i = 0; i < f->n; i++) {
        gp_group *sample = &f->pass[i]; const gp_key *key = &sample->key;
        sample->reference_w = w; sample->reference_h = h;
        sample->scene_relative = !key->compute && key->a[8].format &&
            (active_colors(key) >= 2 || (completed.scene_w && key->w == completed.scene_w && key->h == completed.scene_h));
        aggregate_sample(sample);
    }
}
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
    if (!partial_report) report_end = (window + 1) * GP_WINDOW;
    for (size_t i = 0; i < n; i++) output_pass(rank[i], "pass", 0);
    gp_json summary = {0};
    json_add(&summary, "{\"schema\":3,\"type\":\"window\",\"window_start\":%llu,\"window_end\":%llu,"
        "\"frames\":%llu,\"partial\":%s,\"measured_ms\":%.9f,\"span_ms\":%.9f,\"gap_ms\":%.9f,"
        "\"dropped\":%llu,\"failed_slots\":%llu,\"stats_failed_slots\":%llu,\"output_failures\":%llu}\n",
        (unsigned long long)(window * GP_WINDOW + 1), (unsigned long long)report_end,
        (unsigned long long)window_frames, partial_report ? "true" : "false", total_ms, window_span, window_gap,
        (unsigned long long)dropped_records, (unsigned long long)failed_slots,
        (unsigned long long)stats_failures, (unsigned long long)output_failures);
    output_row(summary.data);
    if (output_file) fflush(output_file);
    LOGV("[gpu-profile] frames %llu-%llu: measured %.3f ms/frame (render %.3f compute %.3f), %.2f passes+dispatches/frame; LOAD %.2f CLEAR %.2f STORE %.2f DONT_CARE-store %.2f ops/frame; logical attachment estimate load %.3f store %.3f MiB/frame (not actual traffic); unknown-byte attachments %llu; dropped %llu failed-slots %llu",
         (unsigned long long)(window * GP_WINDOW + 1), (unsigned long long)((window + 1) * GP_WINDOW),
         total_ms / GP_WINDOW, render_ms / GP_WINDOW, compute_ms / GP_WINDOW, total_records / (double)GP_WINDOW,
         loads / (double)GP_WINDOW, clears / (double)GP_WINDOW, stores / (double)GP_WINDOW, discards / (double)GP_WINDOW,
         load_bytes / (1048576.0 * GP_WINDOW), store_bytes / (1048576.0 * GP_WINDOW),
         (unsigned long long)unknown_bytes, (unsigned long long)dropped_records, (unsigned long long)failed_slots);
    for (size_t i = 0; i < n && i < 5; i++) {
        const gp_group *g = rank[i]; const gp_key *k = &g->key;
        char attachments[800]; size_t used = 0; attachments[0] = 0;
        for (int j = 0; j < 10; j++) if (k->a[j].format) {
            const gp_attachment *a = &k->a[j];
            int nn = snprintf(attachments + used, sizeof attachments - used, " %c%d=%u:%ux%u:%u/%u", j < 8 ? 'c' : 'd', j, a->format, a->w, a->h, a->load, a->store);
            if (nn < 0 || (size_t)nn >= sizeof attachments - used) break;
            used += (size_t)nn;
        }
        LOGV("[gpu-profile] #%zu sig=%016llx %s ordinal=%u area=%ux%u reference=%ux%u samples=%u colors=%u shader=%016llx: %.3f ms/frame %.2f%% %.2f instances/frame %.2f draws/frame; attachments(fmt:absolute-widthxheight:load/store)%s",
             i + 1, (unsigned long long)g->hash, kind_name(k), g->ordinal, k->w, k->h, g->reference_w, g->reference_h, k->samples, k->colors,
             (unsigned long long)k->shader, g->ms / GP_WINDOW, total_ms ? 100 * g->ms / total_ms : 0,
             g->count / (double)GP_WINDOW, g->draws / (double)GP_WINDOW, attachments);
        LOGV("[gpu-profile scale] sig=%016llx: %.2f draws/instance, %.1f ns/draw (whole pass); geometry-stats %s valid=%llu/%llu IA-vertices=%.0f IA-primitives=%.0f VS=%.0f clipping-invocations=%.0f clipping-primitives=%.0f fragments=%.0f/frame; %.2f verts/draw %.2f prims/draw %.3f fragments/prim %.3f ns/vertex %.3f ns/primitive",
             (unsigned long long)g->hash, g->count ? g->draws / (double)g->count : 0, g->draws ? g->ms * 1e6 / g->draws : 0,
             stats_on ? "on" : "off", (unsigned long long)g->stats_count, (unsigned long long)g->count,
             g->counters[0] / (double)GP_WINDOW, g->counters[1] / (double)GP_WINDOW, g->counters[2] / (double)GP_WINDOW,
             g->counters[3] / (double)GP_WINDOW, g->counters[4] / (double)GP_WINDOW, g->counters[5] / (double)GP_WINDOW,
             g->stats_draws ? g->counters[0] / (double)g->stats_draws : 0,
             g->stats_draws ? g->counters[1] / (double)g->stats_draws : 0,
             g->counters[1] ? g->counters[5] / (double)g->counters[1] : 0,
             g->counters[0] ? g->stats_ms * 1e6 / g->counters[0] : 0,
             g->counters[1] ? g->stats_ms * 1e6 / g->counters[1] : 0);
    }
    memset(groups, 0, sizeof groups);
    total_records = dropped_records = failed_slots = loads = clears = stores = discards = 0;
    if (stats_on) LOGV("[gpu-profile scale] pipeline-stat failed slots=%llu; counts are implementation-defined invocations, not covered pixels", (unsigned long long)stats_failures);
    stats_failures = 0;
    load_bytes = store_bytes = unknown_bytes = 0; total_ms = render_ms = compute_ms = 0;
    window_frames = 0; window_span = window_gap = 0;
}
static void advance(uint64_t target) { while (window < target) { report(); window++; } }
static uint64_t tick_delta(uint64_t end, uint64_t start) {
    uint64_t delta = end - start;
    return V.timestamp_bits < 64 ? delta & ((UINT64_C(1) << V.timestamp_bits) - 1) : delta;
}
static double tick_ms(uint64_t end, uint64_t start) {
    return (double)tick_delta(end, start) * V.props.limits.timestampPeriod / 1e6;
}
static void finish_frame(void) {
    if (!completed.frame) return;
    finalize_passes();
    double span = completed.has_ticks ? tick_ms(completed.last, completed.first) : 0;
    double gap = span - completed.measured;
    double epsilon = V.props.limits.timestampPeriod * (GP_RECORDS * 2.0 + 2) / 1e6;
    int failed = completed.failed || completed.dropped || !completed.has_ticks || gap < -epsilon;
    uint64_t long_dropped = long_finish(completed.frame, span, completed.measured, gap);
    gp_json row = {0};
    json_add(&row, "{\"schema\":3,\"type\":\"frame\",\"frame\":%llu,\"span_ms\":%.9f,"
        "\"measured_ms\":%.9f,\"gap_ms\":%.9f,\"dropped\":%llu,\"reconciliation_failed\":%s,\"long_dropped\":%llu,"
        "\"scene_width\":%u,\"scene_height\":%u}\n",
        (unsigned long long)completed.frame, span, completed.measured, gap,
        (unsigned long long)completed.dropped, failed ? "true" : "false", (unsigned long long)long_dropped,
        completed.scene_w, completed.scene_h);
    output_row(row.data);
    if (output_file) fflush(output_file);
    window_span += span; window_gap += gap; window_frames++;
    last_completed = completed.frame;
    memset(&completed, 0, sizeof completed);
}

void vk_gpuprof_slot_begin(vk_gpuprof_slot **slot, VkCommandBuffer cb) {
    static int initialized;
    if (!initialized) {
        initialized = 1;
        const char *e = getenv("TOTK_VK_GPU_PROFILE");
        if (e && (!strcmp(e, "1") || !strcmp(e, "2") || !strcmp(e, "3"))) {
            vk_gpuprof_on = V.has_host_query_reset && V.timestamp_bits && V.props.limits.timestampComputeAndGraphics && V.props.limits.timestampPeriod > 0 ? atoi(e) : 0;
            LOGV("[gpu-profile] %s (TOTK_VK_GPU_PROFILE=%s); 300-frame windows, exact primary pass parts; recorder workers supported",
                 vk_gpuprof_on ? "enabled" : "unavailable: graphics+compute timestamps or hostQueryReset missing", e);
            const char *se = getenv("TOTK_VK_GPU_PROFILE_STATS");
            int requested = se && !strcmp(se, "1");
            stats_on = vk_gpuprof_on && requested && V.has_pipeline_statistics;
            if (requested) LOGV("[gpu-profile] geometry+fragment statistics %s (pipelineStatisticsQuery=%d; TOTK_VK_GPU_PROFILE_STATS=1)", stats_on ? "on" : "unavailable", V.has_pipeline_statistics);
            if (vk_gpuprof_on) {
                const char *mode = getenv("TOTK_OPERATION_MODE"), *resolution = getenv("TOTK_RENDER_RES");
                int handheld = mode && !strcmp(mode, "handheld");
                game_reference_w = handheld ? 1280 : 1600; game_reference_h = handheld ? 720 : 900;
                unsigned w, h; char tail;
                if (resolution && sscanf(resolution, "%ux%u%c", &w, &h, &tail) == 2 &&
                    w >= 64 && h >= 64 && w <= 4096 && h <= 4096) {
                    game_reference_w = w; game_reference_h = h;
                }
                LOGV("[gpu-profile] schema=3 absolute pass keys; frame reference=largest depth+MRT, game fallback=%ux%u",
                    game_reference_w, game_reference_h);
            }
            if (vk_gpuprof_on) output_init();
            if (vk_gpuprof_on == 3) {
                long_frames = (uint32_t)env_u64("TOTK_VK_GPU_PROFILE_LONG_FRAMES", 8, 1, 64);
                const char *threshold = getenv("TOTK_VK_GPU_PROFILE_LONG_MS"); char *end = NULL;
                if (threshold && *threshold) {
                    double parsed = strtod(threshold, &end);
                    if (end && !*end && isfinite(parsed) && parsed >= 0 && parsed <= 60000) long_ms = parsed;
                }
                long_ring = calloc(long_frames, sizeof *long_ring);
                if (!long_ring) DIE("GPU profiler long-frame ring allocation");
                LOGV("[gpu-profile] long-frame mode threshold=%.3f ms retained-frames=%u (TOTK_VK_GPU_PROFILE_LONG_MS/LONG_FRAMES)",
                    long_ms, long_frames);
            }
        } else LOGV("[gpu-profile] disabled (TOTK_VK_GPU_PROFILE=%s; default=0)", e ? e : "unset");
    }
    if (!vk_gpuprof_on) return;
    if (!*slot) {
        *slot = calloc(1, sizeof **slot); if (!*slot) DIE("GPU profiler slot allocation");
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_TIMESTAMPS};
        VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &(*slot)->pool));
        if (stats_on) {
            qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS; qi.queryCount = GP_RECORDS;
            qi.pipelineStatistics = GP_STATS_MASK;
            VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &(*slot)->stats));
        }
    }
    current = *slot; current->n = current->dropped = 0; current->sealed = 0;
    current->ns = 0; current->has_pre = 0; current->pre_cb = VK_NULL_HANDLE;
    current->nsegment = 1; current->segment[0] = (gp_segment){frame, 0, 0};
    vkResetQueryPool(V.dev, current->pool, 0, GP_TIMESTAMPS);
    if (stats_on) vkResetQueryPool(V.dev, current->stats, 0, GP_RECORDS);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, GP_SPAN_BASE);
}
void vk_gpuprof_slot_pre(vk_gpuprof_slot *slot, VkCommandBuffer cb) {
    if (!slot) return;
    slot->has_pre = 1; slot->pre_cb = cb;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot->pool, GP_PRE_QUERY);
}
void vk_gpuprof_slot_end(vk_gpuprof_slot *slot, VkCommandBuffer cb) {
    if (!slot) return;
    slot->segment[slot->nsegment - 1].last = slot->nsegment;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot->pool, GP_SPAN_BASE + slot->nsegment);
}
void vk_gpuprof_slot_cancel(vk_gpuprof_slot *slot) {
    if (current == slot) { current = NULL; open_record = NULL; scope_n = scope_overflow = 0; }
}
static void frame_marker(VkCommandBuffer cb) {
    if (!current) return;
    if (current->nsegment == GP_SEGMENTS) { current->dropped++; return; }
    uint32_t marker = current->nsegment;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, GP_SPAN_BASE + marker);
    current->segment[marker - 1].last = marker;
    current->segment[marker] = (gp_segment){frame + 1, marker, marker};
    current->nsegment++;
}
void vk_gpuprof_slot_seal(vk_gpuprof_slot *slot) {
    if (!slot) return;
    slot->sealed = frame - 1; current = NULL;
}
static void ingest_record(gp_record *r, double ms, int stats_valid) {
    completed.measured += ms;
    if (!r->key.compute && r->key.a[8].format && active_colors(&r->key) >= 2 &&
        (uint64_t)r->key.w * r->key.h > (uint64_t)completed.scene_w * completed.scene_h) {
        completed.scene_w = r->key.w; completed.scene_h = r->key.h;
    }
    capture_record(r, ms, stats_valid);
    if (vk_gpuprof_on == 2) {
        uint64_t hash = key_hash(&r->key);
        const char *reason = r->reason < sizeof end_names / sizeof *end_names ? end_names[r->reason] : "unknown";
        LOGV("[gpu-split] frame=%llu ordinal=%u sig=%016llx kind=%s end=%s detail=%#x draws=%u ms=%.4f",
            (unsigned long long)r->frame, r->instance, (unsigned long long)hash, kind_name(&r->key), reason, r->detail, r->draws, ms);
    }
}
void vk_gpuprof_slot_complete(vk_gpuprof_slot *slot) {
    if (!slot) return;
    VkResult result = slot->n ? vkGetQueryPoolResults(V.dev, slot->pool, 0, slot->n * 2,
        slot->n * 2 * sizeof(uint64_t), slot->ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) : VK_SUCCESS;
    VkResult span_result = vkGetQueryPoolResults(V.dev, slot->pool, GP_SPAN_BASE, slot->nsegment + 1,
        (slot->nsegment + 1) * sizeof(uint64_t), slot->span_ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (slot->has_pre && vkGetQueryPoolResults(V.dev, slot->pool, GP_PRE_QUERY, 1,
        sizeof slot->pre_tick, &slot->pre_tick, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT)) span_result = VK_NOT_READY;
    VkResult sr = stats_on && slot->ns ? vkGetQueryPoolResults(V.dev, slot->stats, 0, slot->ns,
        slot->ns * sizeof slot->counters[0], slot->counters, sizeof slot->counters[0], VK_QUERY_RESULT_64_BIT) : VK_SUCCESS;
    for (uint32_t q = 0; sr == VK_SUCCESS && q < slot->ns; q++)
        for (unsigned st = 0; st < GP_STATS; st++)
            slot->rec[slot->stats_owner[q]].counters[st] += slot->counters[q][st];
    for (uint32_t s = 0; s < slot->nsegment; s++) {
        const gp_segment *segment = &slot->segment[s];
        if (completed.frame && completed.frame != segment->frame) finish_frame();
        advance((segment->frame - 1) / GP_WINDOW);
        completed.frame = segment->frame;
        if (!s) {
            completed.dropped += slot->dropped; dropped_records += slot->dropped;
            failed_slots += result != VK_SUCCESS || span_result != VK_SUCCESS;
            stats_failures += sr != VK_SUCCESS;
        }
        completed.failed |= result != VK_SUCCESS || span_result != VK_SUCCESS;
        if (span_result == VK_SUCCESS) {
            uint64_t first = !s && slot->has_pre ? slot->pre_tick : slot->span_ticks[segment->first];
            if (!completed.has_ticks) completed.first = first;
            completed.last = slot->span_ticks[segment->last]; completed.has_ticks = 1;
        }
        for (uint32_t i = 0; i < slot->n; i++) {
            gp_record *r = &slot->rec[i];
            if (r->frame != segment->frame || result != VK_SUCCESS) continue;
            ingest_record(r, tick_ms(slot->ticks[2 * i + 1], slot->ticks[2 * i]),
                r->stats_count && !r->stats_invalid && sr == VK_SUCCESS);
        }
        if (segment->frame <= slot->sealed) finish_frame();
    }
    if (slot->sealed) advance(slot->sealed / GP_WINDOW);
}
static void begin_record(VkCommandBuffer cb, int compute) {
    open_record = NULL; memset(&open_audit, 0, sizeof open_audit);
    if (!current || current->n == GP_RECORDS) { if (current) current->dropped++; audit_overflow++; return; }
    uint32_t index = current->n++;
    open_record = &current->rec[index]; memset(open_record, 0, sizeof *open_record);
    open_record->frame = frame; open_record->key.compute = (uint32_t)compute;
    open_record->instance = ++frame_instance; open_cb = cb;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, current->pool, index * 2);
    if (!compute && stats_on && !secondary_pass && current->ns < GP_RECORDS) {
        open_record->stats_q1 = ++current->ns; open_record->stats_count++;
        current->stats_owner[current->ns - 1] = index;
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
void vk_gpuprof_secondary(int enabled) { secondary_pass = enabled; }
int vk_gpuprof_chunk_query(VkQueryPool *pool, uint32_t *query) {
    if (!stats_on || !current || !open_record) return 0;
    if (current->ns == GP_RECORDS) { open_record->stats_invalid = 1; return 0; }
    *pool = current->stats; *query = current->ns++;
    current->stats_owner[*query] = (uint32_t)(open_record - current->rec);
    open_record->stats_count++;
    return 1;
}
VkQueryPipelineStatisticFlags vk_gpuprof_stats_mask(void) {
    return stats_on ? GP_STATS_MASK : 0;
}
void vk_gpuprof_end_emit(VkCommandBuffer cb, const vk_gpuprof_end_token *token) {
    if (!token->pool) return;
    if (token->stats_query1) vkCmdEndQuery(cb, token->stats, token->stats_query1 - 1);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, token->pool, token->timestamp);
}
void vk_gpuprof_end_deferred(vk_pass_end_kind reason, uint32_t detail, vk_gpuprof_end_token *token) {
    memset(token, 0, sizeof *token);
    if (!open_record) return;
    open_record->reason = (uint32_t)reason; open_record->detail = detail;
    *token = (vk_gpuprof_end_token){current->pool, current->stats,
        (uint32_t)(open_record - current->rec) * 2 + 1, open_record->stats_q1};
    if (!open_record->key.compute) {
        if (audit_n == audit_cap) { audit_cap = audit_cap ? 2 * audit_cap : 256; audit = realloc(audit, audit_cap * sizeof *audit); if (!audit) DIE("GPU profiler audit allocation"); }
        open_audit.key = open_record->key;
        audit[audit_n++] = open_audit;
    }
    open_record = NULL; open_cb = VK_NULL_HANDLE;
}
void vk_gpuprof_end(VkCommandBuffer cb, vk_pass_end_kind reason, uint32_t detail) {
    vk_gpuprof_end_token token;
    vk_gpuprof_end_deferred(reason, detail, &token);
    vk_gpuprof_end_emit(cb, &token);
}
void vk_gpuprof_scope_begin(VkCommandBuffer cb, const char *label) {
    if (scope_n == sizeof scopes / sizeof *scopes) { scope_overflow++; return; }
    gp_scope *scope = &scopes[scope_n++];
    *scope = (gp_scope){.parent = open_record, .audit = open_audit, .parent_cb = open_cb,
        .cb = cb, .secondary = secondary_pass, .skipped = open_record && cb == open_cb};
    if (scope->skipped) return;
    static const char *const names[] = {"upload", "transfer", "post", "upscale", "present", "framegen"};
    unsigned kind = 2;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(label, names[i])) { kind = i + 2; break; }
    begin_record(cb, (int)kind);
}
void vk_gpuprof_scope_end(VkCommandBuffer cb) {
    if (scope_overflow) { scope_overflow--; return; }
    if (!scope_n) return;
    gp_scope *scope = &scopes[--scope_n];
    if (!scope->skipped) vk_gpuprof_end(cb, VK_PASS_END_OTHER, 0);
    open_record = scope->parent; open_audit = scope->audit;
    open_cb = scope->parent_cb; secondary_pass = scope->secondary;
}
static void present_at(VkCommandBuffer cb) {
    if (current) frame_marker(cb);
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
void vk_gpuprof_present(void) { present_at(current ? vk_cb() : VK_NULL_HANDLE); }
#ifdef TOTK_VK_TESTS
void vk_gpuprof_test_frame(VkCommandBuffer cb) {
    if (!vk_gpuprof_on) return;
    present_at(cb);
}
void vk_gpuprof_test_flush(void) {
    if (!vk_gpuprof_on || !window_frames) return;
    partial_report = 1; report_end = last_completed; report(); partial_report = 0;
}
#endif
