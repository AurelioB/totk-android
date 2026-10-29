#include "pso_trace_key.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t hash(const TotkPsoTraceKey *key) {
    return totk_pso_trace_key_hash(key);
}

static void fill_key(TotkPsoTraceKey *key, const char *vs, const char *fs) {
    key->vs_id = vs;
    key->fs_id = fs;
    key->fs_early_tests = 0x10203040u;
    for (unsigned i = 0; i < 8; i++) {
        key->color[i] = 0x11000000u + i;
        for (unsigned j = 0; j < 7; j++)
            key->blend[i][j] = 0x22000000u + i * 16 + j;
    }
    key->depth = 0x31;
    key->stencil = 0x32;
    key->color_count = 0x33;
    key->samples = 0x34;
    key->blend_enabled = 0x41;
    key->channel_mask = 0x42;
    key->a2c = 0x43;
    key->polygon_mode = 0x44;
    key->topology_class = 0x45;
    key->depth_clamp = 0x46;
    key->logic_op = 0x47;
    for (unsigned i = 0; i < 16; i++) {
        for (unsigned j = 0; j < 3; j++)
            key->attributes[i][j] = 0x51000000u + i * 4 + j;
        key->streams[i][0] = 0x61000000u + i;
        key->streams[i][1] = 0x62000000u + i;
    }
}

static void assert_changed(const TotkPsoTraceKey *original, TotkPsoTraceKey changed) {
    assert(hash(original) != hash(&changed));
}

static void test_canonical_vectors(void) {
    TotkPsoTraceKey key = {0};
    assert(hash(&key) == UINT64_C(0x2bf22475aa7d77d5));

    key.vs_id = "vertex";
    key.fs_id = "fragment";
    key.fs_early_tests = 1;
    assert(hash(&key) == UINT64_C(0x55395c283961d12c));
    assert(hash(&key) == hash(&key));
}

static void test_storage_independence(void) {
    char vs_a[] = "vertex-shader";
    char fs_a[] = "fragment-shader";
    char vs_b[] = "vertex-shader";
    char fs_b[] = "fragment-shader";
    TotkPsoTraceKey a, b;
    memset(&a, 0xa5, sizeof a);
    memset(&b, 0x5a, sizeof b);
    fill_key(&a, vs_a, fs_a);
    fill_key(&b, vs_b, fs_b);
    assert(a.vs_id != b.vs_id && a.fs_id != b.fs_id);
    assert(memcmp(&a, &b, sizeof a) != 0);
    assert(hash(&a) == hash(&b));
}

static void test_shader_identity(void) {
    TotkPsoTraceKey key = {0};
    TotkPsoTraceKey changed;
    uint64_t null_hash = hash(&key);
    changed = key;
    changed.vs_id = "";
    assert(hash(&changed) != null_hash);
    changed = key;
    changed.fs_id = "";
    assert(hash(&changed) != null_hash);

    key.vs_id = "same";
    key.fs_id = "other";
    uint64_t baseline = hash(&key);
    changed = key;
    changed.vs_id = "different";
    assert(hash(&changed) != baseline);
    changed = key;
    changed.fs_id = "different";
    assert(hash(&changed) != baseline);
    changed = key;
    changed.fs_early_tests++;
    assert_changed(&key, changed);
}

static void test_every_numeric_field(void) {
    TotkPsoTraceKey key;
    fill_key(&key, "vs", "fs");
    TotkPsoTraceKey changed;

#define CHECK_MUTATION(expr) do { changed = key; (expr); assert_changed(&key, changed); } while (0)
    CHECK_MUTATION(changed.fs_early_tests++);
    for (unsigned i = 0; i < 8; i++) {
        CHECK_MUTATION(changed.color[i]++);
        for (unsigned j = 0; j < 7; j++)
            CHECK_MUTATION(changed.blend[i][j]++);
    }
    CHECK_MUTATION(changed.depth++);
    CHECK_MUTATION(changed.stencil++);
    CHECK_MUTATION(changed.color_count++);
    CHECK_MUTATION(changed.samples++);
    CHECK_MUTATION(changed.blend_enabled++);
    CHECK_MUTATION(changed.channel_mask++);
    CHECK_MUTATION(changed.a2c++);
    CHECK_MUTATION(changed.polygon_mode++);
    CHECK_MUTATION(changed.topology_class++);
    CHECK_MUTATION(changed.depth_clamp++);
    CHECK_MUTATION(changed.logic_op++);
    for (unsigned i = 0; i < 16; i++) {
        for (unsigned j = 0; j < 3; j++)
            CHECK_MUTATION(changed.attributes[i][j]++);
        for (unsigned j = 0; j < 2; j++)
            CHECK_MUTATION(changed.streams[i][j]++);
    }
#undef CHECK_MUTATION
}

int main(void) {
    test_canonical_vectors();
    test_storage_independence();
    test_shader_identity();
    test_every_numeric_field();
    puts("pso trace key tests passed");
    return 0;
}
