/* Private to vk_draw.c. Diagnostic tables exist only with TOTK_VK_XLATE_STATS=1.
 * The ordinary record_stage specialization contains none of these calls. */
enum { XLATE_TABLE = 65536, XLATE_IDS = 1 << 20 };
enum {
    XM_DISABLED, XM_COLD, XM_PROGRAM, XM_STAGE, XM_DEFERRED, XM_TEXTURE, XM_SAMPLER,
    XM_UNIFORM_BIND, XM_STORAGE, XM_GENERATED, XM_UNIFORM_VERSION, XM_DIRTY,
    XM_LAG, XM_SPAN, XM_REGISTRY, XM_SAMPLER_REGISTRY, XM_SUBMISSION, XM_CHUNK,
    XM_DIMENSIONS, XM_WRITABLE, XM_UNWATCHED, XM_OTHER, XM_N
};
static const char *const xlate_miss_names[XM_N] = {
    "disabled", "cold", "program", "stage", "deferred_handle", "texture_handle", "sampler",
    "uniform_address_size", "storage_buffer", "generated_block", "uniform_version", "dirty_generation",
    "lag_state", "mapping_span", "texture_registration_lifetime", "sampler_registration", "submission",
    "chunk_ticket", "dimensions", "prior_writable", "prior_unwatched", "other"
};
typedef struct { uint64_t word[8]; } xlate_value;
typedef struct { xlate_value key, value; int used; } xlate_entry;
typedef struct {
    uint64_t decisions, previous, address_previous, changed[5], address_changed[5], unknown;
    uint64_t version_previous, address_version_previous, dirty_previous, address_dirty_previous;
} xlate_uniform_counts;
static struct xlate_draw_stats {
    uint64_t submission, submissions, frames, frame, draws, stage[6], hits[6], direct[6], misses[6][XM_N];
    uint64_t uniform_total, uniform_known, uniform_unique, dependency_total, dependency_unique, overflow;
    uint64_t texture_uses, texture_distinct, texture_max, texture_frame_distinct, texture_peak_distinct, view_unknown;
    uint64_t *texture_types; uint64_t *texture_frames; uint64_t *texture_views;
    xlate_entry *dependencies, *addresses;
    unsigned dependency_entries, address_entries;
    struct { xlate_value value; int valid; } previous[6][16];
    struct { vk_shader *shader; VkDescriptorBufferInfo selected[16]; int invalid; } memo[128];
    xlate_uniform_counts uniform[6][16];
} *g_xlate;
static pthread_mutex_t g_xlate_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t xlate_hash(const xlate_value *key) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < 8; i++) { h = (h ^ key->word[i]) * 1099511628211ull; h ^= h >> 29; }
    return h;
}
static xlate_entry *xlate_entry_get(xlate_entry *table, const xlate_value *key, int *fresh) {
    unsigned h = (unsigned)xlate_hash(key) & (XLATE_TABLE - 1);
    for (unsigned i = 0; i < XLATE_TABLE; i++, h = (h + 1) & (XLATE_TABLE - 1)) {
        xlate_entry *entry = &table[h];
        if (!entry->used) {
            unsigned *count = table == g_xlate->dependencies ? &g_xlate->dependency_entries : &g_xlate->address_entries;
            if (*count >= XLATE_TABLE * 3 / 4) { g_xlate->overflow++; *fresh = 0; return NULL; }
            (*count)++; entry->used = 1; entry->key = *key; *fresh = 1; return entry;
        }
        if (!memcmp(&entry->key, key, sizeof *key)) { *fresh = 0; return entry; }
    }
    g_xlate->overflow++; *fresh = 0; return NULL;
}
static double xlate_fraction(uint64_t n, uint64_t d) { return d ? (double)n / d : 0; }
static void xlate_report(uint64_t frame, int final) {
    if (!g_xlate) return;
    struct xlate_draw_stats *s = g_xlate;
    double frames = s->frames ? (double)s->frames : 1;
    LOGV("[xlate-summary] frame=%llu final=%d frames=%llu submissions=%llu draws=%llu uniform_unique=%llu uniform_decisions=%llu uniform_known=%llu read_unique=%llu read_decisions=%llu overflow=%llu",
        (unsigned long long)frame, final, (unsigned long long)s->frames, (unsigned long long)s->submissions,
        (unsigned long long)s->draws, (unsigned long long)s->uniform_unique, (unsigned long long)s->uniform_total,
        (unsigned long long)s->uniform_known, (unsigned long long)s->dependency_unique,
        (unsigned long long)s->dependency_total, (unsigned long long)s->overflow);
    for (int st = 0; st < 6; st++) if (s->stage[st]) {
        uint64_t miss = 0; for (int r = 0; r < XM_N; r++) miss += s->misses[st][r];
        LOGV("[xlate-freeze] frame=%llu stage=%d requests=%llu hits=%llu misses=%llu direct=%llu requests_per_frame=%.3f misses_per_frame=%.3f",
            (unsigned long long)frame, st, (unsigned long long)s->stage[st], (unsigned long long)s->hits[st],
            (unsigned long long)miss, (unsigned long long)s->direct[st], s->stage[st] / frames, miss / frames);
        for (int r = 0; r < XM_N; r++) if (s->misses[st][r])
            LOGV("[xlate-miss] frame=%llu stage=%d first=%s count=%llu per_frame=%.3f", (unsigned long long)frame,
                st, xlate_miss_names[r], (unsigned long long)s->misses[st][r], s->misses[st][r] / frames);
    }
    for (int st = 0; st < 6; st++) for (int slot = 0; slot < 16; slot++) {
        xlate_uniform_counts *u = &s->uniform[st][slot]; if (!u->decisions) continue;
        LOGV("[xlate-uniform] frame=%llu stage=%d slot=%d decisions=%llu previous=%llu address_previous=%llu version_previous=%llu address_version_previous=%llu dirty_previous=%llu address_dirty_previous=%llu changed_address=%.6f changed_size=%.6f changed_version=%.6f changed_dirty=%.6f changed_snapshot=%.6f address_changed_size=%.6f address_changed_version=%.6f address_changed_dirty=%.6f address_changed_snapshot=%.6f unknown=%llu",
            (unsigned long long)frame, st, slot, (unsigned long long)u->decisions, (unsigned long long)u->previous,
            (unsigned long long)u->address_previous, (unsigned long long)u->version_previous,
            (unsigned long long)u->address_version_previous, (unsigned long long)u->dirty_previous,
            (unsigned long long)u->address_dirty_previous, xlate_fraction(u->changed[0], u->previous),
            xlate_fraction(u->changed[1], u->previous), xlate_fraction(u->changed[2], u->version_previous),
            xlate_fraction(u->changed[4], u->dirty_previous), xlate_fraction(u->changed[3], u->previous),
            xlate_fraction(u->address_changed[1], u->address_previous),
            xlate_fraction(u->address_changed[2], u->address_version_previous),
            xlate_fraction(u->address_changed[4], u->address_dirty_previous),
            xlate_fraction(u->address_changed[3], u->address_previous), (unsigned long long)u->unknown);
    }
    LOGV("[xlate-textures] frame=%llu uses=%llu distinct_per_frame=%.3f peak_distinct=%llu max_id=%llu view_unknown=%llu",
        (unsigned long long)frame, (unsigned long long)s->texture_uses, s->texture_distinct / frames,
        (unsigned long long)s->texture_peak_distinct, (unsigned long long)s->texture_max, (unsigned long long)s->view_unknown);
    s->frames = s->submissions = s->draws = s->uniform_total = s->uniform_known = s->uniform_unique = 0;
    s->dependency_total = s->dependency_unique = s->overflow = 0;
    s->texture_uses = s->texture_distinct = s->texture_peak_distinct = s->view_unknown = 0;
    memset(s->stage, 0, sizeof s->stage); memset(s->hits, 0, sizeof s->hits); memset(s->direct, 0, sizeof s->direct);
    memset(s->misses, 0, sizeof s->misses); memset(s->uniform, 0, sizeof s->uniform);
}
static void xlate_texture_histogram(uint64_t frame) {
    if (!g_xlate) return;
    /* Exact target sets, grouped by the complete per-ID mask, not just popcount. */
    struct { uint64_t mask, views, ids; } hist[128] = {{0}}; unsigned count = 0; uint64_t overflow = 0;
    for (unsigned id = 0; id < XLATE_IDS; id++) if (g_xlate->texture_types[id]) {
        uint64_t mask = g_xlate->texture_types[id], views = g_xlate->texture_views[id]; unsigned k = 0;
        while (k < count && (hist[k].mask != mask || hist[k].views != views)) k++;
        if (k == count) { if (count == 128) { overflow++; continue; } hist[count].mask = mask; hist[count++].views = views; }
        hist[k].ids++;
    }
    for (unsigned k = 0; k < count; k++) LOGV("[xlate-texture-types] frame=%llu target_mask=0x%llx view_mask=0x%llx ids=%llu overflow=%llu",
        (unsigned long long)frame, (unsigned long long)hist[k].mask, (unsigned long long)hist[k].views,
        (unsigned long long)hist[k].ids, (unsigned long long)overflow);
}
static void xlate_exit(void) {
    if (pthread_mutex_trylock(&g_xlate_mu)) {
        LOGV("[xlate-exit] unavailable=walking_active (periodic reports remain valid)"); return;
    }
    /* Account for a final submission without a present (host fixtures do this). */
    g_xlate->texture_distinct += g_xlate->texture_frame_distinct;
    if (g_xlate->texture_frame_distinct > g_xlate->texture_peak_distinct) g_xlate->texture_peak_distinct = g_xlate->texture_frame_distinct;
    if (g_xlate->draws || g_xlate->frames) xlate_report(g_xlate->frame, 1);
    xlate_texture_histogram(g_xlate->frame);
    pthread_mutex_unlock(&g_xlate_mu);
}
static void xlate_begin(void) {
    pthread_mutex_lock(&g_xlate_mu);
    if (!g_xlate) {
        g_xlate = calloc(1, sizeof *g_xlate);
        if (!g_xlate) DIE("xlate diagnostic allocation failed");
        g_xlate->dependencies = calloc(XLATE_TABLE, sizeof *g_xlate->dependencies);
        g_xlate->addresses = calloc(XLATE_TABLE, sizeof *g_xlate->addresses);
        g_xlate->texture_types = calloc(XLATE_IDS, sizeof *g_xlate->texture_types);
        g_xlate->texture_frames = calloc(XLATE_IDS, sizeof *g_xlate->texture_frames);
        g_xlate->texture_views = calloc(XLATE_IDS, sizeof *g_xlate->texture_views);
        if (!g_xlate->dependencies || !g_xlate->addresses || !g_xlate->texture_types || !g_xlate->texture_frames || !g_xlate->texture_views)
            DIE("xlate diagnostic table allocation failed");
        g_xlate->submission = UINT64_MAX; g_xlate->frame = 1; atexit(xlate_exit);
    }
    uint64_t sub = vk_game_submit_serial();
    if (g_xlate->submission != sub) {
        g_xlate->submission = sub; g_xlate->submissions++;
        memset(g_xlate->dependencies, 0, XLATE_TABLE * sizeof *g_xlate->dependencies);
        memset(g_xlate->addresses, 0, XLATE_TABLE * sizeof *g_xlate->addresses);
        g_xlate->dependency_entries = g_xlate->address_entries = 0;
    }
}
static void xlate_end(void) { pthread_mutex_unlock(&g_xlate_mu); }
static int xlate_dependency(const xlate_value *key) {
    int fresh;
    xlate_entry *entry = xlate_entry_get(g_xlate->dependencies, key, &fresh);
    if (key->word[0] != 1) { g_xlate->dependency_total++; g_xlate->dependency_unique += fresh; }
    return entry && fresh;
}
static void xlate_uniform_compare(xlate_uniform_counts *u, const xlate_value *old, const xlate_value *value, int address) {
    uint64_t *changed = address ? u->address_changed : u->changed;
    changed[0] += old->word[0] != value->word[0]; changed[1] += old->word[1] != value->word[1];
    if ((old->word[2] & value->word[2] & XLATE_UNIFORM_OWNED) != 0) {
        if (address) u->address_version_previous++; else u->version_previous++;
        changed[2] += old->word[3] != value->word[3] || old->word[5] != value->word[5];
    }
    if ((old->word[2] & value->word[2] & XLATE_UNIFORM_DIRTY) != 0) {
        if (address) u->address_dirty_previous++; else u->dirty_previous++;
        changed[4] += old->word[4] != value->word[4];
    }
    changed[3] += memcmp(old->word + 5, value->word + 5, 3 * sizeof(uint64_t)) != 0;
}
static void xlate_uniform(int stage, int slot, uint64_t address, uint64_t size, uint64_t extent, const VkDescriptorBufferInfo *selected) {
    uint64_t range = MINV(size ? size : 0x10000, V.props.limits.maxUniformBufferRange);
    uint64_t version[4]; vk_xlate_uniform_key(address, range, version);
    xlate_value value = {{address, size, version[0], version[1], version[2], version[3],
        (uint64_t)(uintptr_t)selected->buffer, selected->offset}};
    xlate_uniform_counts *u = &g_xlate->uniform[stage][slot]; u->decisions++; g_xlate->uniform_total++;
    /* The metadata helper marks unknown authority explicitly. It is not a content hash. */
    u->unknown += !(version[0] & XLATE_UNIFORM_OWNED);
    if (g_xlate->previous[stage][slot].valid) {
        u->previous++;
        xlate_uniform_compare(u, &g_xlate->previous[stage][slot].value, &value, 0);
    }
    xlate_value address_key = {{address}}; int fresh;
    xlate_entry *entry = xlate_entry_get(g_xlate->addresses, &address_key, &fresh);
    if (entry) {
        if (!fresh) {
            u->address_previous++;
            xlate_uniform_compare(u, &entry->value, &value, 1);
        }
        entry->value = value;
    }
    g_xlate->previous[stage][slot].valid = 1; g_xlate->previous[stage][slot].value = value;
    /* First tuple excludes selected arena addresses; second includes the selected read authority. */
    if (version[0] & XLATE_UNIFORM_OWNED) {
        g_xlate->uniform_known++;
        xlate_value uniform = {{1, address, extent, version[1], version[3]}};
        g_xlate->uniform_unique += xlate_dependency(&uniform);
    }
    xlate_value read = {{2, address, range, version[1], version[2], version[3],
        (uint64_t)(uintptr_t)selected->buffer, selected->offset}};
    xlate_dependency(&read);
}
static void xlate_observe_uniforms(vk_shader *sh, int stage, const VkDescriptorBufferInfo selected[16]) {
    uint16_t seen = 0; uint64_t extent[16] = {0};
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->kind != RB_CBUF && b->kind != RB_UBO_PTR) ||
            b->reg < 0 || b->reg >= 16) continue;
        seen |= (uint16_t)(1u << b->reg);
        const p_bind_buf *u = &S.ubo[stage][b->reg];
        uint64_t range = MINV(u->size ? u->size : 0x10000, V.props.limits.maxUniformBufferRange);
        extent[b->reg] = MAXV(extent[b->reg], b->extent ? MINV(b->extent, range) : range);
    }
    for (int slot = 0; slot < 16; slot++) if (seen >> slot & 1) {
        const p_bind_buf *u = &S.ubo[stage][slot];
        xlate_uniform(stage, slot, u->addr, u->size, extent[slot], &selected[slot]);
    }
}
static void xlate_selected(vk_shader *sh, int stage, const vk_record_descriptor *input) {
    unsigned m = ((uintptr_t)sh >> 4) % 128;
    memset(g_xlate->memo[m].selected, 0, sizeof g_xlate->memo[m].selected);
    for (int i = 0, k = 0; i < sh->nb; k += sh->b[i++].count) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) &&
            b->reg >= 0 && b->reg < 16) g_xlate->memo[m].selected[b->reg] = input[k].b;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && b->kind == RB_SBUF_UBO) {
            const vk_record_descriptor *d = &input[k];
            xlate_value dependency = {{7, (uint64_t)(uintptr_t)d->b.buffer, d->b.offset, d->b.range,
                (uint64_t)b->rw, vk_record_uniform_revision()}};
            xlate_dependency(&dependency);
        }
    }
    xlate_observe_uniforms(sh, stage, g_xlate->memo[m].selected);
}
static int xlate_first_miss(vk_shader *sh, int stage, const struct freeze_key *key, int disabled) {
    unsigned m = ((uintptr_t)sh >> 4) % 128; __typeof__(g_freeze_memo[0]) *last = &g_freeze_memo[m];
    if (disabled) return XM_DISABLED;
    if (!last->valid) return g_xlate->memo[m].invalid ? g_xlate->memo[m].invalid : XM_COLD;
    if (last->key.shader != sh) return XM_PROGRAM;
    if (last->key.stage != (uint32_t)stage) return XM_STAGE;
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i]; record_binding now; record_binding_of(b, stage, &now);
        if (!memcmp(&now, &last->bindings[i], sizeof now)) continue;
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b->kind == RB_SBUF || b->kind == RB_SBUF_UBO) return XM_STORAGE;
        if (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) return XM_UNIFORM_BIND;
        if (b->kind == RB_DRV0 || b->kind == RB_DRV2) return XM_GENERATED;
        int sampler = b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER ||
            (b->dtype == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && (now.value[0] & 0xfffff) == (last->bindings[i].value[0] & 0xfffff));
        int slot = sampler && b->sreg >= 0 ? b->sreg : b->reg;
        if (vk_xlate_deferred_slot(stage, b->kind == RB_IMG ? 1 : sampler && b->sreg != -2 ? 2 : 0, slot)) return XM_DEFERRED;
        if (sampler) return XM_SAMPLER;
        return XM_TEXTURE;
    }
    if (last->key.uniforms != key->uniforms) return XM_UNIFORM_VERSION;
    if (last->key.cpu_dirty != key->cpu_dirty || last->key.dirty_epoch != key->dirty_epoch) return XM_DIRTY;
    if (last->key.native_sample != key->native_sample || last->key.native_image != key->native_image) return XM_LAG;
    if (last->key.spans != key->spans) return XM_SPAN;
    if (last->key.resources != key->resources) return XM_REGISTRY;
    if (last->key.samplers != key->samplers) return XM_SAMPLER_REGISTRY;
    if (last->key.submission != key->submission) return XM_SUBMISSION;
    if (last->key.chunk != key->chunk) return XM_CHUNK;
    if (last->key.width != key->width || last->key.height != key->height) return XM_DIMENSIONS;
    return XM_OTHER;
}
static void xlate_stage(vk_shader *sh, int stage, const struct freeze_key *key, int hit, int disabled, const void *unused) {
    (void)unused;
    unsigned m = ((uintptr_t)sh >> 4) % 128; g_xlate->stage[stage]++;
    if (hit > 0) g_xlate->hits[stage]++;
    else if (hit < 0) g_xlate->direct[stage]++;
    else g_xlate->misses[stage][xlate_first_miss(sh, stage, key, disabled)]++;
    g_xlate->memo[m].shader = sh;
    if (hit > 0) xlate_observe_uniforms(sh, stage, g_xlate->memo[m].selected);
    for (int i = 0; i < sh->nb; i++) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (b->kind == RB_SBUF && b->reg >= 0 && b->reg < 16) {
                const p_bind_buf *u = &S.ssbo[stage][b->reg]; uint64_t v[4]; vk_xlate_uniform_key(u->addr, u->size, v);
                xlate_value dep = {{3, u->addr, u->size, (uint64_t)b->rw, v[0], v[1], v[2], vk_record_spans_generation(key->spans)}};
                xlate_dependency(&dep);
            }
            continue;
        }
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_SAMPLER) continue;
        uint64_t texture = b->reg >= 0 ? (b->kind == RB_IMG ? (b->reg < 32 ? S.img[stage][b->reg] : 0) :
            (b->reg < 128 ? S.tex[stage][b->reg] : 0)) : 0;
        if (!texture) continue;
        unsigned id = (unsigned)(texture & 0xfffff); g_xlate->texture_uses++;
        if (id > g_xlate->texture_max) g_xlate->texture_max = id;
        if (g_xlate->texture_frames[id] != g_xlate->frame) { g_xlate->texture_frames[id] = g_xlate->frame; g_xlate->texture_frame_distinct++; }
        /* Usage bits 0=storage, 1=texel, 2=sampled; requested target bits start at 3. */
        unsigned usage = b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? 0 :
            (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->dtype == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) ? 1 : 2;
        unsigned bit = b->target >= 0 && b->target < 60 ? (unsigned)b->target + 3 : 63;
        g_xlate->texture_types[id] |= (1ull << bit) | (1ull << usage);
        xlate_value dep = {{4, id, (uint64_t)b->dtype, (uint64_t)(unsigned)b->target, (uint64_t)b->rw, key->resources}};
        xlate_dependency(&dep);
    }
}
static void xlate_texture_view(uint64_t texture, vk_tex *object, VkImageView view) {
    if (!texture || !view) return;
    int type = vk_xlate_texture_view_type(object, view);
    if (type >= 0 && type < 63) g_xlate->texture_views[texture & 0xfffff] |= 1ull << type;
    else g_xlate->view_unknown++;
}
static void xlate_immediate_stage(vk_shader *sh, int stage, const dinfo *info) {
    struct freeze_key key = {0}; key.resources = vk_treg_revision();
    xlate_stage(sh, stage, &key, -1, 0, NULL);
    VkDescriptorBufferInfo selected[16] = {{0}};
    for (int i = 0, k = 0; i < sh->nb; k += sh->b[i++].count) {
        const vk_bind *b = &sh->b[i];
        if (b->dtype == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && (b->kind == RB_CBUF || b->kind == RB_UBO_PTR) &&
            b->reg >= 0 && b->reg < 16) selected[b->reg] = info[k].b;
    }
    xlate_observe_uniforms(sh, stage, selected);
}
static void xlate_uncacheable(vk_shader *sh, int stage, int cacheable, int unwatched) {
    (void)stage; unsigned m = ((uintptr_t)sh >> 4) % 128;
    g_xlate->memo[m].invalid = cacheable ? 0 : unwatched ? XM_UNWATCHED : XM_WRITABLE;
}
static void xlate_buffers(vk_shader *sh, const p_draw_idx *di) {
    uint32_t used = 0;
    for (int a = 0; a < 16; a++) if ((sh->vin_mask >> a & 1) && a < S.nattr && S.attr[a].fmt) used |= 1u << (S.attr[a].stream & 15);
    for (unsigned i = 0; i < 16; i++) if (used >> i & 1) {
        uint64_t size = S.vbuf[i].size ? S.vbuf[i].size : 16, v[4]; vk_xlate_uniform_key(S.vbuf[i].address, size, v);
        xlate_value dep = {{5, S.vbuf[i].address, size, 16, v[1], v[2], v[3], g_record_draw ? vk_record_spans_generation(g_record_draw->spans) : 0}};
        xlate_dependency(&dep);
    }
    if (di) {
        uint64_t size = (uint64_t)(uint32_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4), v[4];
        vk_xlate_uniform_key(di->ibuf, size, v);
        xlate_value dep = {{6, di->ibuf, size, 16, v[1], v[2], v[3], g_record_draw ? vk_record_spans_generation(g_record_draw->spans) : 0}};
        xlate_dependency(&dep);
    }
}
static void xlate_draw_frame(uint64_t frame) {
    if (!g_xlate) return;
    pthread_mutex_lock(&g_xlate_mu);
    g_xlate->texture_distinct += g_xlate->texture_frame_distinct;
    if (g_xlate->texture_frame_distinct > g_xlate->texture_peak_distinct) g_xlate->texture_peak_distinct = g_xlate->texture_frame_distinct;
    g_xlate->texture_frame_distinct = 0; g_xlate->frames++; g_xlate->frame = frame + 1;
    if (!(frame % 300)) xlate_report(frame, 0);
    if (!(frame % 3000)) xlate_texture_histogram(frame);
    pthread_mutex_unlock(&g_xlate_mu);
}
#ifdef TOTK_VK_TESTS
int vk_draw_test_xlate_frame(uint64_t frame) {
    vk_rec_lock();
    int ok = g_xlate && g_xlate->draws && !g_xlate->overflow &&
        g_xlate->uniform_unique <= g_xlate->uniform_known && g_xlate->uniform_known <= g_xlate->uniform_total &&
        g_xlate->dependency_unique <= g_xlate->dependency_total;
    if (g_xlate) for (int stage = 0; stage < 6; stage++) {
        uint64_t total = g_xlate->hits[stage] + g_xlate->direct[stage];
        for (int reason = 0; reason < XM_N; reason++) total += g_xlate->misses[stage][reason];
        ok &= total == g_xlate->stage[stage];
    }
    xlate_draw_frame(frame); vk_xlate_recording_stats_frame(frame);
    vk_rec_unlock(); return ok;
}
#endif
