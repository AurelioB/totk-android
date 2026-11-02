/* prepare_shader_seed.py inventory() and located() for Tears of the Kingdom: every shader of the RomFS (BNSH files on
   their own or inside BFSHA / packages / effect and layout resources, agl .sharcb packages) and of the game modules,
   keyed as the runtime keys them (SHA-256 of the control section's control_key_len bytes and the code). Files are
   independent, so several threads scan them; the result (the key set, which keys are startup shaders) does not depend
   on the order. Shader bytes go to a spill file, so memory holds only the files in flight, bounded by a byte budget.
   For the pinned 1.4.3 RomFS the stage hands over the shipped list of the files that hold shaders (shader_files.c,
   made by a full scan with seed_cli --full-scan --files-out), so only those are read; any other RomFS is scanned whole. */
#define _GNU_SOURCE 1
#include "seed_int.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
/* checked(data, offset, size) without overflow */
static int in(uint64_t n, uint64_t off, uint64_t size) { return off <= n && size <= n - off; }

/* ---- the shader table ---- */
static size_t slot_of(const SeedTable *t, const uint8_t key[32]) {
    uint64_t h; memcpy(&h, key, 8);
    size_t i = (size_t)h & (t->cap - 1);
    while (t->v[i].used && memcmp(t->v[i].key, key, 32)) i = (i + 1) & (t->cap - 1);
    return i;
}
int seed_table_init(SeedTable *t, const char *spill_path, SeedErr *err) {
    memset(t, 0, sizeof *t);
    pthread_mutex_init(&t->mu, NULL);
    t->err = err; t->cap = 1 << 16;
    if (!(t->v = calloc(t->cap, sizeof *t->v))) { seed_err(err, "internal", "Out of memory"); return -1; }
    t->spill_fd = open(spill_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (t->spill_fd < 0) { seed_err(err, "io", "Cannot create %s: %s", spill_path, strerror(errno)); return -1; }
    return 0;
}
void seed_table_free(SeedTable *t) {
    if (t->spill_fd > 0) close(t->spill_fd);
    free(t->v); pthread_mutex_destroy(&t->mu);
    memset(t, 0, sizeof *t);
}
SeedShader *seed_table_get(SeedTable *t, const uint8_t key[32]) {
    SeedShader *s = &t->v[slot_of(t, key)];
    return s->used ? s : NULL;
}
int seed_table_add(SeedTable *t, const uint8_t key[32], int stage, const uint8_t *control, size_t control_size,
                   const uint8_t *code, size_t code_size, int startup) {
    pthread_mutex_lock(&t->mu);
    if ((t->n + 1) * 2 > t->cap) {
        SeedShader *old = t->v; size_t cap = t->cap;
        if (!(t->v = calloc(cap * 2, sizeof *t->v))) {
            t->v = old; pthread_mutex_unlock(&t->mu); seed_err(t->err, "internal", "Out of memory"); return -1;
        }
        t->cap = cap * 2;
        for (size_t i = 0; i < cap; i++) if (old[i].used) t->v[slot_of(t, old[i].key)] = old[i];
        free(old);
    }
    SeedShader *s = &t->v[slot_of(t, key)];
    if (s->used) {   /* shaders.setdefault: the first copy stays; the bytes are the same (they hash to the key) */
        if (startup) s->startup = 1;
        pthread_mutex_unlock(&t->mu);
        return 0;
    }
    memcpy(s->key, key, 32); s->used = 1; s->stage = (uint8_t)stage; s->startup = (uint8_t)!!startup;
    s->control_size = (uint32_t)control_size; s->code_size = (uint32_t)code_size;
    s->spill = t->spill_end; t->spill_end += control_size + code_size; t->n++;
    uint64_t at = s->spill;
    pthread_mutex_unlock(&t->mu);
    /* Nothing reads the spill before the inventory has finished. */
    if (pwrite(t->spill_fd, control, control_size, (off_t)at) != (ssize_t)control_size ||
        pwrite(t->spill_fd, code, code_size, (off_t)(at + control_size)) != (ssize_t)code_size) {
        seed_err(t->err, errno == ENOSPC ? "no_space" : "io", "Cannot write the shader work file: %s", strerror(errno ? errno : EIO));
        return -1;
    }
    return 0;
}
int seed_table_bytes(SeedTable *t, const SeedShader *s, uint8_t **control, uint8_t **code) {
    *control = malloc(s->control_size ? s->control_size : 1); *code = malloc(s->code_size ? s->code_size : 1);
    if (!*control || !*code) { free(*control); free(*code); errno = ENOMEM; return -1; }
    if (st_pread(t->spill_fd, *control, s->control_size, s->spill) ||
        st_pread(t->spill_fd, *code, s->code_size, s->spill + s->control_size)) { free(*control); free(*code); return -1; }
    return 0;
}

uint32_t seed_control_key_len(const uint8_t *control, size_t n) {
    uint32_t size = n >= 0x24 ? le32(control + 0x20) : 0;
    return size >= 0x7cc && size < 0x880 ? size : 0x880;
}
static void key_of(const uint8_t *control, size_t control_size, const uint8_t *code, size_t code_size, uint8_t key[32]) {
    Sha256 s; sha256_init(&s); sha256_update(&s, control, control_size); sha256_update(&s, code, code_size); sha256_final(&s, key);
}

/* ---- scanning ---- */
typedef struct {
    SeedInventory *inv;
    pthread_mutex_t mu; pthread_cond_t cv;
    uint64_t in_use;                  /* bytes of file data held (budget) */
    _Atomic uint64_t next;            /* next RomFS entry, then the modules */
    char **modules; size_t nmodules;  /* sorted *.patched.bin */
    int running;
} Scan;
typedef struct { Scan *s; SeedDecoder *dec; const char *top; uint64_t added; } Worker;   /* added: shaders of the file in hand */

/* Wait for room unless nothing else is held (one file larger than the budget still runs, alone). force: nested data
   whose parent already holds budget, which must not wait (it could wait for itself). */
static int budget_take(Scan *s, uint64_t n, int force) {
    pthread_mutex_lock(&s->mu);
    while (!force && s->in_use && s->in_use + n > s->inv->budget && !atomic_load(s->inv->stop)) pthread_cond_wait(&s->cv, &s->mu);
    s->in_use += n;
    pthread_mutex_unlock(&s->mu);
    return atomic_load(s->inv->stop) ? -1 : 0;
}
static void budget_give(Scan *s, uint64_t n) {
    pthread_mutex_lock(&s->mu); s->in_use -= n; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
}
static int fail(Worker *w, const char *what) {
    seed_err(w->s->inv->err, "internal", "Shader inventory: %s in %s", what, w->top);
    atomic_store(w->s->inv->stop, 1);
    return -1;
}

/* bnsh_shaders(): absolute offsets are relative to each embedded BNSH. */
static int bnsh(Worker *w, const uint8_t *d, uint64_t n, int startup) {
    if (n < 4 || memcmp(d, "BNSH", 4) || !in(n, 12, 2) || d[12] != 0xff || d[13] != 0xfe || !in(n, 96, 4) || memcmp(d + 96, "grsc", 4))
        return fail(w, "unsupported BNSH header");
    uint64_t size = le32(d + 28);
    if (size > n) return fail(w, "shader container range outside file");
    n = size;
    if (n < 136) return fail(w, "truncated BNSH header");
    uint64_t count = le32(d + 124), variations = le64(d + 128);
    if (!in(n, variations, count * 64)) return fail(w, "shader container range outside file");
    static const struct { int stage; unsigned rel; } stages[3] = { {0, 8}, {1, 40}, {5, 48} };
    for (uint64_t i = 0; i < count; i++) {
        uint64_t program = le64(d + variations + i * 64 + 16);
        if (!in(n, program, 56)) return fail(w, "shader container range outside file");
        for (int k = 0; k < 3; k++) {
            uint64_t offset = le64(d + program + stages[k].rel);
            if (!offset) continue;
            if (offset > n || n - offset < 32) return fail(w, "shader container range outside file");
            const uint8_t *e = d + offset + 8;
            uint64_t control_off = le64(e), code_off = le64(e + 8), code_size = le32(e + 16), control_size = le32(e + 20);
            if (!in(n, control_off, control_size) || !in(n, code_off, code_size)) return fail(w, "shader container range outside file");
            /* The SDK utility BNSH uses an older, shorter control layout: not the game's NVN ABI. */
            if (control_size < 0x7cc) continue;
            const uint8_t *control = d + control_off;
            uint32_t key_len = seed_control_key_len(control, control_size);
            if (control_size < key_len) continue;
            uint32_t c1 = le32(control + 0x700), length = le32(control + 0x704), actual = le32(control + 0x714);
            if (actual != (uint32_t)stages[k].stage || !(c1 <= length && length <= code_size) || !length)
                return fail(w, "invalid NVN shader bounds or stage");
            uint8_t key[32]; key_of(control, key_len, d + code_off, length, key);
            if (seed_table_add(w->s->inv->table, key, stages[k].stage, control, key_len, d + code_off, length, startup)) { atomic_store(w->s->inv->stop, 1); return -1; }
            w->added++;
        }
    }
    return 0;
}

/* sharcb_shaders(): agl shader packages (BAHS). Records between the header's start and the code pool are <u64 code offset
   in the pool, u32 control size, i32 offset of the control from the record>; a record may reuse another's control. */
static int sharcb(Worker *w, const uint8_t *d, uint64_t n) {
    if (n < 32 || memcmp(d, "BAHS", 4)) return fail(w, "expected an agl shader package");
    uint64_t start = le32(d + 20), pool_size = le32(d + 24), pool = le32(d + 28);
    if (!(32 <= start && start <= pool) || pool + pool_size > n) return fail(w, "agl shader package layout outside the file");
    static const uint8_t sizes[4][4] = { {0x80, 8, 0, 0}, {0x8c, 8, 0, 0}, {0xd1, 7, 0, 0}, {0x48, 8, 0, 0} };
    for (int p = 0; p < 4; p++) {
        uint64_t at = start;
        for (;;) {
            const uint8_t *hit = at + 4 <= pool ? memmem(d + at, pool - at, sizes[p], 4) : NULL;
            if (!hit) break;
            uint64_t found = (uint64_t)(hit - d), record = found - 8;
            at = found + 4;
            if (found < 8 || record < start || record % 4) continue;
            if (record + 16 > n) return fail(w, "agl shader record outside the file");
            uint64_t code_off = le64(d + record);
            int64_t control_at = (int64_t)record + (int32_t)le32(d + record + 12);
            uint32_t key_len = 0;
            if (control_at >= (int64_t)start) {
                uint64_t avail = (uint64_t)control_at < n ? n - (uint64_t)control_at : 0;
                key_len = seed_control_key_len(d + control_at, avail < 0x880 ? avail : 0x880);
            }
            if (!key_len || !(control_at >= (int64_t)start && control_at <= (int64_t)pool - (int64_t)key_len) || code_off >= pool_size) continue;
            /* The control is a separate object, never the record's own bytes: zero relative offsets inside arbitrary
               package data would otherwise manufacture shaders from the code pool with random stage/length. */
            if (control_at < (int64_t)record + 16 && (int64_t)record < control_at + (int64_t)key_len) continue;
            const uint8_t *control = d + control_at;
            uint32_t c1 = le32(control + 0x700), length = le32(control + 0x704), st = le32(control + 0x714);
            if ((st != 0 && st != 1 && st != 5) || !(0 < length && length <= pool_size - code_off) || c1 > length) continue;
            uint8_t key[32]; key_of(control, key_len, d + pool + code_off, length, key);
            if (seed_table_add(w->s->inv->table, key, (int)st, control, key_len, d + pool + code_off, length, 1)) { atomic_store(w->s->inv->stop, 1); return -1; }
            w->added++;
        }
    }
    return 0;
}

static const uint8_t BNSH_MARK[8] = {'B', 'N', 'S', 'H', 0, 0, 0, 0};

/* visit(): agl packages are startup shaders; SARC members are entered (decoded) up to depth 4; everything else holds
   complete, independently addressed BNSH files. */
static int visit(Worker *w, const uint8_t *d, uint64_t n, int depth, int startup) {
    if (atomic_load(w->s->inv->stop)) return -1;
    if (n >= 4 && !memcmp(d, "BAHS", 4) && sharcb(w, d, n)) return -1;
    if (n >= 4 && !memcmp(d, "SARC", 4)) {
        if (depth > 4) return 0;
        SarcMember *m; size_t count;
        if (seed_sarc(d, n, &m, &count, w->s->inv->err)) { atomic_store(w->s->inv->stop, 1); return -1; }
        int rc = 0;
        for (size_t i = 0; i < count && !rc; i++) {
            uint64_t size = 0;
            if (seed_is_zstd(m[i].data, m[i].size) && seed_zstd_size(m[i].data, m[i].size, &size, w->s->inv->err)) { rc = -1; break; }
            budget_take(w->s, size, 1);
            uint8_t *dec; size_t dn;
            if (seed_decode(w->dec, m[i].data, m[i].size, &dec, &dn, w->s->inv->err)) rc = -1;
            else rc = dec ? visit(w, dec, dn, depth + 1, startup) : visit(w, m[i].data, m[i].size, depth + 1, startup);
            if (rc == -1 && !atomic_load(w->s->inv->stop)) atomic_store(w->s->inv->stop, 1);
            free(dec); budget_give(w->s, size);
        }
        if (rc) atomic_store(w->s->inv->stop, 1);
        free(m);
        return rc;
    }
    for (uint64_t pos = 0;;) {
        const uint8_t *hit = pos < n ? memmem(d + pos, n - pos, BNSH_MARK, 8) : NULL;
        if (!hit) return 0;
        pos = (uint64_t)(hit - d);
        if (!in(n, pos + 28, 4)) return fail(w, "shader container range outside file");
        uint64_t size = le32(d + pos + 28);
        if (!in(n, pos, size)) return fail(w, "shader container range outside file");
        if (bnsh(w, d + pos, size, startup)) return -1;
        pos += size > 8 ? size : 8;
    }
}

static int startup_package(const char *name) {
    size_t n = strlen(name);
    return !strcmp(name, "Lib/NintendoSDK/TextureShader.bnsh") || !strcmp(name, "Pack/Bootup.Nin_NX_NVN.pack.zs") ||
           (!strncmp(name, "Shader/", 7) && n >= 3 && !strcmp(name + n - 3, ".zs"));
}
static int container(const uint8_t *p, uint64_t n) {
    static const char *const magic[] = { "\x28\xb5\x2f\xfd", "Yaz0", "SARC", "FSHA", "BNSH", "BAHS" };
    for (int i = 0; i < 6 && n >= 4; i++) if (!memcmp(p, magic[i], 4)) return 1;
    return 0;
}

/* One RomFS file: only containers can hold shaders (compressed files, packages and shader archives), or any file with an
   embedded BNSH. Yaz0 (Breath of the Wild's compression) is not decoded for this game: such a file is scanned as is,
   as decoder() does for 'totk'. Other files are scanned in chunks and read whole only when they hold the BNSH mark;
   a file of the shipped list (inv->files) is known to hold shaders and is read whole at once. */
static int scan_file(Worker *w, uint64_t index) {
    Scan *s = w->s; SeedInventory *inv = s->inv;
    uint64_t off, size; seed_romfs_entry(inv->romfs, index, &off, &size);
    w->top = seed_romfs_name(inv->romfs, index);
    w->added = 0;
    uint8_t head[4] = {0};
    int whole = inv->files != NULL;
    if (!whole && size && romfs_image_pread(&inv->romfs->img, head, size < 4 ? size : 4, off)) goto io;
    if (!whole) whole = container(head, size);
    if (!whole) {
        enum { CHUNK = 4 << 20 };
        uint8_t *buf = malloc(CHUNK + 8);
        if (!buf) { seed_err(inv->err, "internal", "Out of memory"); return -1; }
        size_t keep = 0;
        for (uint64_t at = 0; at < size && !whole; ) {
            size_t want = size - at < CHUNK ? (size_t)(size - at) : CHUNK;
            if (romfs_image_pread(&inv->romfs->img, buf + keep, want, off + at)) { free(buf); goto io; }
            romfs_image_drop(&inv->romfs->img, off + at, want);
            whole = memmem(buf, keep + want, BNSH_MARK, 8) != NULL;
            size_t total = keep + want;
            keep = total < 7 ? total : 7;
            memmove(buf, buf + total - keep, keep);
            at += want;
        }
        free(buf);
        if (!whole) return 0;
    }
    if (budget_take(s, size, 0)) { budget_give(s, size); return -1; }
    uint8_t *raw = NULL, *dec = NULL; size_t dn = 0; uint64_t dsize = 0; int rc = -1;
    if (seed_romfs_read(inv->romfs, index, &raw, inv->err)) goto out;
    if (seed_is_zstd(raw, size)) {
        if (seed_zstd_size(raw, size, &dsize, inv->err)) goto out;
        budget_take(s, dsize, 1);
        if (seed_decode(w->dec, raw, size, &dec, &dn, inv->err)) goto out;
        free(raw); raw = NULL; budget_give(s, size); size = 0;   /* the raw bytes are no longer needed */
    }
    rc = dec ? visit(w, dec, dn, 0, startup_package(w->top)) : visit(w, raw, size, 0, startup_package(w->top));
    if (!rc && w->added && inv->bearing) inv->bearing[index] = 1;
out:
    free(raw); free(dec); budget_give(s, size + dsize);
    if (rc) atomic_store(inv->stop, 1);
    return rc;
io:
    seed_err(inv->err, "io", "Cannot read %s from the RomFS image: %s", w->top, strerror(errno));
    atomic_store(inv->stop, 1);
    return -1;
}
/* The game modules (rewritten/<version>/<name>.patched.bin) embed a few BNSH files; all are startup shaders. */
static int scan_module(Worker *w, const char *path) {
    char *data; size_t n;
    w->top = path;
    if (st_read_file(path, &data, &n, 1u << 30)) {
        seed_err(w->s->inv->err, "missing_input", "Cannot read %s: %s", path, strerror(errno)); atomic_store(w->s->inv->stop, 1); return -1;
    }
    budget_take(w->s, n, 1);
    int rc = visit(w, (const uint8_t *)data, n, 0, 1);
    free(data); budget_give(w->s, n);
    return rc;
}

static void *scan_worker(void *arg) {
    Worker *w = arg; Scan *s = w->s; SeedInventory *inv = s->inv;
    setpriority(PRIO_PROCESS, 0, 10);   /* this thread only (Linux): keep the setup UI responsive */
    for (;;) {
        if (atomic_load(inv->stop)) break;
        uint64_t i = atomic_fetch_add(&s->next, 1), nfiles = inv->files ? inv->nfiles : inv->romfs->count;
        if (i < nfiles) {
            if (inv->files) i = inv->files[i];
            uint64_t off, size; seed_romfs_entry(inv->romfs, i, &off, &size);
            if (scan_file(w, i)) break;
            romfs_image_drop(&inv->romfs->img, off, size);   /* read once: do not keep it cached */
            atomic_fetch_add(&inv->done_bytes, size);
        } else if (i - nfiles < s->nmodules) {
            if (scan_module(w, s->modules[i - nfiles])) break;
        } else break;
    }
    pthread_mutex_lock(&s->mu); s->running--; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
    return NULL;
}

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int list_modules(Scan *s, const char *dir, SeedErr *err) {
    if (!dir) return 0;
    DIR *d = opendir(dir);
    if (!d) { seed_err(err, "missing_input", "Cannot open %s: %s", dir, strerror(errno)); return -1; }
    size_t cap = 0;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n <= 12 || strcmp(e->d_name + n - 12, ".patched.bin")) continue;
        if (s->nmodules == cap) { char **p = realloc(s->modules, (cap = cap ? cap * 2 : 4) * sizeof *p); if (!p) break; s->modules = p; }
        char path[SETUP_PATH];
        if (st_path(path, sizeof path, "%s/%s", dir, e->d_name) || !(s->modules[s->nmodules] = strdup(path))) break;
        s->nmodules++;
    }
    closedir(d);
    qsort(s->modules, s->nmodules, sizeof *s->modules, by_str);
    return 0;
}

int seed_inventory_run(SeedInventory *inv) {
    Scan s; memset(&s, 0, sizeof s);
    s.inv = inv; pthread_mutex_init(&s.mu, NULL); pthread_cond_init(&s.cv, NULL);
    int rc = list_modules(&s, inv->modules, inv->err);
    inv->total_bytes = 0;
    for (uint64_t k = 0, n = inv->files ? inv->nfiles : inv->romfs->count; k < n; k++) {
        uint64_t o, z; seed_romfs_entry(inv->romfs, inv->files ? inv->files[k] : k, &o, &z); inv->total_bytes += z;
    }
    Worker *w = calloc((size_t)inv->threads, sizeof *w); pthread_t *t = calloc((size_t)inv->threads, sizeof *t);
    int started = 0;
    if (!rc && (!w || !t)) { seed_err(inv->err, "internal", "Out of memory"); rc = -1; }
    for (int i = 0; !rc && i < inv->threads; i++) {
        w[i].s = &s;
        if (!(w[i].dec = seed_decoder_new(inv->dicts))) { seed_err(inv->err, "internal", "Out of memory"); rc = -1; break; }
        pthread_mutex_lock(&s.mu); s.running++; pthread_mutex_unlock(&s.mu);
        if (pthread_create(&t[i], NULL, scan_worker, &w[i])) { s.running--; seed_err(inv->err, "internal", "Cannot start a thread"); rc = -1; break; }
        started++;
    }
    if (rc) atomic_store(inv->stop, 1);
    /* The stage thread reports progress and polls cancellation; setup_progress belongs to this thread. */
    pthread_mutex_lock(&s.mu);
    while (s.running) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 200000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&s.cv, &s.mu, &until);
        pthread_mutex_unlock(&s.mu);
        if (setup_cancelled(inv->c)) atomic_store(inv->stop, 1);
        /* done/total in bytes so the UI shows a real percentage (it adds the GB figures and the read rate itself). */
        char detail[128]; uint64_t done = atomic_load(&inv->done_bytes);
        snprintf(detail, sizeof detail, "finding the game's shaders: %zu so far", inv->table->n);
        setup_progress(inv->c, "shaders", done < inv->total_bytes ? done : inv->total_bytes, inv->total_bytes, detail);
        pthread_mutex_lock(&s.mu);
        pthread_cond_broadcast(&s.cv);   /* budget waiters re-check stop */
    }
    pthread_mutex_unlock(&s.mu);
    for (int i = 0; i < started; i++) pthread_join(t[i], NULL);
    for (int i = 0; w && i < inv->threads; i++) seed_decoder_free(w[i].dec);
    for (size_t i = 0; i < s.nmodules; i++) free(s.modules[i]);
    free(s.modules); free(w); free(t);
    pthread_mutex_destroy(&s.mu); pthread_cond_destroy(&s.cv);
    if (seed_err_set(inv->err)) return -1;
    if (setup_cancelled(inv->c) || atomic_load(inv->stop)) return SETUP_CANCELLED;
    return rc;
}

/* ---- located() ---- */
typedef struct { const char *source; uint8_t *data; uint64_t n; int present; uint8_t *own[8]; } Source;
static void source_free(Source *s) { for (int i = 0; i < 8; i++) free(s->own[i]); memset(s, 0, sizeof *s); }
/* source_reader(): a RomFS path, each '/' after a package entering it (.zs decompressed), or module:<name>. */
static int source_read(SeedInventory *inv, SeedDecoder *dec, const char *source, Source *out) {
    memset(out, 0, sizeof *out); out->source = source;
    if (!strncmp(source, "module:", 7)) {
        char path[SETUP_PATH]; char *data; size_t n;
        if (!inv->modules || st_path(path, sizeof path, "%s/%s.patched.bin", inv->modules, source + 7) || st_file_size(path) < 0) return 0;
        if (st_read_file(path, &data, &n, 1u << 30)) { seed_err(inv->err, "io", "Cannot read %s: %s", path, strerror(errno)); return -1; }
        out->data = out->own[0] = (uint8_t *)data; out->n = n; out->present = 1;
        return 0;
    }
    char *copy = strdup(source);
    if (!copy) { seed_err(inv->err, "internal", "Out of memory"); return -1; }
    for (char *cut = copy + strlen(copy); cut; ) {
        *cut = 0;   /* the longest prefix that is a RomFS file */
        int64_t i = seed_romfs_find(inv->romfs, copy);
        if (i >= 0) {
            uint8_t *raw, *d; size_t dn; int k = 0;
            if (seed_romfs_read(inv->romfs, (uint64_t)i, &raw, inv->err)) { free(copy); return -1; }
            uint64_t o, size; seed_romfs_entry(inv->romfs, (uint64_t)i, &o, &size);
            out->own[k++] = raw;
            if (seed_decode(dec, raw, size, &d, &dn, inv->err)) { free(copy); return -1; }
            if (d) { out->own[k++] = d; out->data = d; out->n = dn; } else { out->data = raw; out->n = size; }
            out->present = 1;
            for (const char *member = source + strlen(copy); *member == '/' && out->present; ) {
                member++;
                const char *end = strchr(member, '/'); size_t len = end ? (size_t)(end - member) : strlen(member);
                SarcMember *m = NULL; size_t count = 0, j;
                if (out->n >= 4 && !memcmp(out->data, "SARC", 4) && seed_sarc(out->data, out->n, &m, &count, inv->err)) { free(copy); return -1; }
                for (j = 0; j < count && !(strlen(m[j].name) == len && !memcmp(m[j].name, member, len)); j++) {}
                if (j == count) { out->present = 0; free(m); break; }
                if (k == 8) { free(m); seed_err(inv->err, "internal", "Shader source nested too deeply: %s", source); free(copy); return -1; }
                if (seed_decode(dec, m[j].data, m[j].size, &d, &dn, inv->err)) { free(m); free(copy); return -1; }
                if (d) { out->own[k++] = d; out->data = d; out->n = dn; } else { out->data = (uint8_t *)m[j].data; out->n = m[j].size; }
                free(m);
                member += len;
            }
            break;
        }
        cut = strrchr(copy, '/');
    }
    free(copy);
    if (!out->present) out->data = NULL;
    return 0;
}
static int by_source(const void *a, const void *b) { return strcmp(((const SeedLocator *)a)->source, ((const SeedLocator *)b)->source); }
int seed_located(SeedInventory *inv, SeedLocator *rows, size_t n, size_t *found) {
    *found = 0;
    SeedDecoder *dec = seed_decoder_new(inv->dicts);
    if (!dec) { seed_err(inv->err, "internal", "Out of memory"); return -1; }
    qsort(rows, n, sizeof *rows, by_source);   /* one read per source; the result is a set */
    Source src; memset(&src, 0, sizeof src);
    int rc = 0;
    for (size_t i = 0; i < n && !rc; i++) {
        SeedLocator *l = &rows[i];
        if (seed_table_get(inv->table, l->key)) continue;   /* only shaders the inventory did not find */
        if (setup_cancelled(inv->c)) { rc = SETUP_CANCELLED; break; }
        if (!src.source || strcmp(src.source, l->source)) { source_free(&src); if (source_read(inv, dec, l->source, &src)) { rc = -1; break; } }
        if (!src.present || !(0 < l->length && l->length <= (16u << 20))) continue;
        const uint8_t *control = l->control < src.n ? src.data + l->control : NULL;
        uint64_t clen = control ? (src.n - l->control < 0x880 ? src.n - l->control : 0x880) : 0;
        const uint8_t *code = l->code < src.n ? src.data + l->code : NULL;
        uint64_t len = code ? (src.n - l->code < l->length ? src.n - l->code : l->length) : 0;
        uint32_t key_len = clen >= 0x7cc ? seed_control_key_len(control, clen) : 0;
        if (!key_len || clen < key_len || len != l->length) continue;
        uint8_t key[32]; key_of(control, key_len, code, len, key);
        if (memcmp(key, l->key, 32)) continue;
        uint32_t st = le32(control + 0x714), c1 = le32(control + 0x700), size = le32(control + 0x704);
        if ((st == 0 || st == 1 || st == 5) && size == l->length && c1 <= l->length) {
            if (seed_table_add(inv->table, key, (int)st, control, key_len, code, len, 0)) { rc = -1; break; }
            seed_table_get(inv->table, key)->located = 1;
            (*found)++;
        }
    }
    source_free(&src); seed_decoder_free(dec);
    return rc;
}
