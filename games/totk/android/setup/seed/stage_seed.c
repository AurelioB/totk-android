/* setup_stage_seed: the Vulkan shader seed made on the phone from the player's own RomFS and rewritten modules, the same
   seed android/build_android.py --shaders makes on a PC (scripts/prepare_shader_seed.py --keep-spirv, place_shaders,
   scripts/pack_shader_seed.py):
     1. every shader of the game (inventory.c) and the ones the shared list locates elsewhere (located),
     2. the plan: each startup shader's default bind state, then every bind state the list names (shader_list.c),
     3. translation by the bundled Vulkan service (translate.c),
     4. seed.vkpack + seed-id.txt in out_seed_dir (pack.c); nothing else stays there.
   The shared list is the APK asset assets/shader_list.tsv the request hands over (setup_shader_list); without it the
   seed holds the default bind states only. Work files (the spill, the ~71k loose translator outputs, the resume journal)
   live in app-private internal storage, <noBackupDir>/setup-work/shaders-<inputs identity> (never the game's volume, which
   may be an SD card behind FUSE), and only seed.vkpack and seed-id.txt are written into out_seed_dir. Work files are
   removed on success and on cancel; after a crash or failure the next run with the same inputs resumes them (translate.c).
   A failed or cancelled run keeps nothing in out_seed_dir: the orchestrator discards the staging folder. */
#define _GNU_SOURCE 1
#include "seed_int.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define WORKER_MEMORY (384ull << 20)   /* per translator service: the largest shaders need far more than the median */
#define SCAN_BUDGET_MAX (1ull << 30)   /* decoded RomFS bytes in flight while scanning */
/* Free space. Scratch: the spill (0.23 GB) and the loose seed (1.47 GB in ~71k files, more in whole blocks) at the end of
   the translation, the peak. The game's volume: seed.vkpack (1.48 GB). On one volume the pack replaces the loose files
   as it is written (pack.c), so the scratch peak plus a margin is enough. */
#define SCRATCH_NEEDED (2ull << 30)
#define PACK_NEEDED (1600ull << 20)
#define SAME_VOLUME_MARGIN (256ull << 20)
/* The watchdog of one translation request. The host p95 is ~20-40 ms and the slowest shaders a few hundred ms; a phone
   is several times slower, so a minute without a reply means the service hangs. */
#define SEED_REQUEST_TIMEOUT_MS 60000

/* MemAvailable (/proc files report size 0, so read them as a stream); 2 GB when unknown. */
static uint64_t mem_available(void) {
    FILE *f = fopen("/proc/meminfo", "re");
    char line[256]; unsigned long long kb = 0;
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    if (f) fclose(f);
    return kb ? (uint64_t)kb << 10 : 2ull << 30;
}
static int cpus(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n < 1 ? 1 : n > 64 ? 64 : (int)n; }

static int report(SetupCtx *c, SeedErr *err, int rc) {
    if (!rc) return 0;
    if (rc == SETUP_CANCELLED || (!seed_err_set(err) && setup_cancelled(c))) return SETUP_CANCELLED;
    if (rc) {
        if (seed_err_set(err)) setup_fail(c, err->code, "%s", err->msg);
        else setup_fail(c, "internal", "The shader seed failed");
        return -1;
    }
    return 0;
}

/* The inventory and plan as identifiers only (the host oracle test compares them with prepare_shader_seed.py):
   S <key> <stage> <startup> <located> <control bytes> <code bytes>, then J <variant> <default|listed> per job. */
static int dump_plan(const char *path, const SeedTable *t, const SeedPlan *p) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    char key[65];
    for (size_t i = 0; i < t->cap; i++) if (t->v[i].used) {
        sha256_hex(t->v[i].key, key);
        fprintf(f, "S\t%s\t%d\t%d\t%d\t%u\t%u\n", key, t->v[i].stage, t->v[i].startup, t->v[i].located, t->v[i].control_size, t->v[i].code_size);
    }
    for (size_t i = 0; i < p->njobs; i++) fprintf(f, "J\t%s\t%s\n", p->jobs[i].variant, p->jobs[i].listed ? "listed" : "default");
    return fclose(f);
}

/* The RomFS entries of the shipped list (shader_files.c) when it describes this RomFS (same index bytes), else none:
   *files = NULL and the inventory scans every file. Largest first, so no big package starts last and runs alone (the
   result does not depend on the order). */
typedef struct { uint64_t size, index; } Sized;
static int by_size_desc(const void *a, const void *b) {
    const Sized *x = a, *y = b;
    return x->size != y->size ? (x->size < y->size) - (x->size > y->size) : (x->index > y->index) - (x->index < y->index);
}
static int shader_files(const SeedRomfs *r, const char index_sha[65], uint64_t **files, size_t *n, SeedErr *err) {
    *files = NULL; *n = 0;
    if (strcmp(index_sha, SEED_SHADER_FILES_INDEX_SHA256)) return 0;
    size_t count = SEED_SHADER_FILES_COUNT ? SEED_SHADER_FILES_COUNT : 1;
    Sized *v = malloc(count * sizeof *v);
    if (!v || !(*files = malloc(count * sizeof **files))) { free(v); seed_err(err, "internal", "Out of memory"); return -1; }
    for (size_t i = 0; i < SEED_SHADER_FILES_COUNT; i++) {
        int64_t at = seed_romfs_find(r, SEED_SHADER_FILES[i]);
        if (at < 0) {   /* cannot happen with the same index; refuse rather than make a smaller seed */
            seed_err(err, "internal", "The RomFS lacks %s of the shader file list", SEED_SHADER_FILES[i]);
            free(v); free(*files); *files = NULL; return -1;
        }
        uint64_t off; v[i].index = (uint64_t)at; seed_romfs_entry(r, v[i].index, &off, &v[i].size);
    }
    qsort(v, SEED_SHADER_FILES_COUNT, sizeof *v, by_size_desc);
    for (size_t i = 0; i < SEED_SHADER_FILES_COUNT; i++) (*files)[(*n)++] = v[i].index;
    free(v);
    return 0;
}

/* shader_files.c for this RomFS: the files a full scan found shaders in (bearing), in index (path) order. */
static int write_shader_files(const char *path, const SeedRomfs *r, const char index_sha[65], const uint8_t *bearing) {
    Buf b = {0};
    buf_printf(&b, "/* Generated by seed_cli --full-scan --files-out (android/setup/seed/seed_cli.c) from the RomFS whose\n"
               "   runtime-index.bin has the SHA-256 below; do not edit. Identifiers only: the RomFS paths of the files that hold\n"
               "   shaders (seed_int.h). */\n#include \"seed_int.h\"\n\n"
               "const char SEED_SHADER_FILES_INDEX_SHA256[65] = \"%s\";\nconst char *const SEED_SHADER_FILES[] = {\n", index_sha);
    size_t n = 0;
    for (uint64_t i = 0; i < r->count; i++) if (bearing[i]) {
        buf_puts(&b, "    \"");
        for (const unsigned char *p = (const unsigned char *)seed_romfs_name(r, i); *p; p++) {
            if (*p == '"' || *p == '\\') buf_printf(&b, "\\%c", *p);
            else if (*p < 0x20 || *p > 0x7e) buf_printf(&b, "\\%03o", *p);
            else buf_printf(&b, "%c", *p);
        }
        buf_puts(&b, "\",\n"); n++;
    }
    buf_printf(&b, "};\nconst size_t SEED_SHADER_FILES_COUNT = %zu;\n", n);
    int rc = b.oom ? -1 : st_write_file(path, b.p, b.n, 0644);
    buf_free(&b);
    return rc;
}

/* ---- scratch ---- */
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
/* Everything the loose files depend on, as a SHA-256: the chain, the RomFS index, the shared list, the translator binary,
   the game modules (name, size, modification time) and the sample. A resumed scratch must come from the same inputs. */
static int scratch_identity(SetupCtx *c, const char *translator, const char *rewritten_dir, const char index_sha[65],
                            const SeedList *list, const SeedOptions *opt, char id[65]) {
    char xsha[65];
    if (st_sha256_file(c, translator, xsha, NULL)) return -1;
    Buf b = {0};
    buf_printf(&b, "chain %s\nindex %s\nlist %s\ntranslator %s\nsample %zu\n", SEED_CHAIN, index_sha,
               list->sha256[0] ? list->sha256 : "none", xsha, opt->sample_every);
    DIR *d = rewritten_dir ? opendir(rewritten_dir) : NULL;
    char **names = NULL; size_t n = 0, cap = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) { char **p = realloc(names, (cap = cap ? cap * 2 : 8) * sizeof *p); if (!p) break; names = p; }
        if (!(names[n] = strdup(e->d_name))) break;
        n++;
    }
    if (d) closedir(d);
    if (n) qsort(names, n, sizeof *names, by_name);
    for (size_t i = 0; i < n; i++) {
        struct stat st; char p[SETUP_PATH];
        if (!st_path(p, sizeof p, "%s/%s", rewritten_dir, names[i]) && !stat(p, &st))
            buf_printf(&b, "module %s %lld %lld.%09ld\n", names[i], (long long)st.st_size, (long long)st.st_mtim.tv_sec, (long)st.st_mtim.tv_nsec);
        free(names[i]);
    }
    free(names);
    int rc = b.oom ? -1 : 0;
    if (!rc) sha256_buf_hex(b.p, b.n, id);
    else setup_fail(c, "internal", "Out of memory");
    buf_free(&b);
    return rc;
}
/* <root>/shaders-<id>: kept from an interrupted run with the same inputs (*resuming), else made anew. Every other
   shaders-* folder there belongs to a finished or abandoned run of this app (setup.lock admits one run at a time). */
static int scratch_open(const char *root, const char id[65], char *work, size_t cap, int *resuming) {
    char stamp[SETUP_PATH], loose[SETUP_PATH], mine[32];
    *resuming = 0;
    snprintf(mine, sizeof mine, "shaders-%.16s", id);
    if (st_mkdirs(root) || st_path(work, cap, "%s/%s", root, mine) || st_path(stamp, sizeof stamp, "%s/identity", work) ||
        st_path(loose, sizeof loose, "%s/loose", work)) return -1;
    DIR *d = opendir(root);
    for (struct dirent *e; d && (e = readdir(d));) {
        char p[SETUP_PATH];
        if (!strncmp(e->d_name, "shaders-", 8) && strcmp(e->d_name, mine) && !st_path(p, sizeof p, "%s/%s", root, e->d_name)) st_rmtree(p);
    }
    if (d) closedir(d);
    char *text = NULL; size_t n = 0; struct stat st;
    if (!st_read_file(stamp, &text, &n, 256) && n == 64 && !memcmp(text, id, 64) && !stat(loose, &st) && S_ISDIR(st.st_mode)) *resuming = 1;
    free(text);
    if (*resuming) return 0;
    if (st_rmtree(work) || st_mkdirs(loose)) return -1;
    return st_write_file(stamp, id, 64, 0600);
}
/* Bytes the entries of dir occupy on disk: what a resumed scratch already holds of its peak. */
static uint64_t dir_bytes(const char *dir) {
    DIR *d = opendir(dir); uint64_t n = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        struct stat st;
        if (e->d_name[0] != '.' && !fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) n += (uint64_t)st.st_blocks * 512;
    }
    if (d) closedir(d);
    return n;
}
/* Free space for the rest of the run: the scratch's peak less what a resumed scratch already holds, and the pack. */
static int space_check(SetupCtx *c, const char *work, const char *loose, const char *out_seed_dir, int resuming) {
    uint64_t held = resuming ? dir_bytes(loose) : 0, scratch = held < SCRATCH_NEEDED ? SCRATCH_NEEDED - held : 0;
    struct stat a, b;
    int same = !stat(work, &a) && !stat(out_seed_dir, &b) && a.st_dev == b.st_dev;
    if (same && st_free_bytes(work) < scratch + SAME_VOLUME_MARGIN) {
        setup_fail(c, "no_space", "The shader seed needs %llu MB of free space", (unsigned long long)((scratch + SAME_VOLUME_MARGIN) >> 20)); return -1;
    }
    if (!same && st_free_bytes(work) < scratch) {
        setup_fail(c, "no_space", "The shader seed needs %llu MB of free space in the phone's internal storage for its work files",
                   (unsigned long long)(scratch >> 20)); return -1;
    }
    if (!same && st_free_bytes(out_seed_dir) < PACK_NEEDED) {
        setup_fail(c, "no_space", "The shader seed needs %llu MB of free space where the game is installed", (unsigned long long)(PACK_NEEDED >> 20)); return -1;
    }
    return 0;
}

int setup_stage_seed(SetupCtx *c, const char *romfs_dir, const char *rewritten_dir, const char *out_seed_dir) {
    return seed_run(c, romfs_dir, rewritten_dir, out_seed_dir, NULL);
}

int seed_run(SetupCtx *c, const char *romfs_dir, const char *rewritten_dir, const char *out_seed_dir, const SeedOptions *opt) {
    static const SeedOptions none;
    if (!opt) opt = &none;
    SeedErr err = { .mu = PTHREAD_MUTEX_INITIALIZER };
    SeedRomfs romfs = {0}; SeedDicts *dicts = NULL; SeedList list; SeedTable table; SeedPlan plan;
    memset(&list, 0, sizeof list); memset(&table, 0, sizeof table); memset(&plan, 0, sizeof plan);
    table.spill_fd = -1;
    atomic_int stop = 0;
    uint64_t *scan_files = NULL; size_t nscan = 0; uint8_t *bearing = NULL;
    char work[SETUP_PATH] = "", loose[SETUP_PATH], spill[SETUP_PATH], journal[SETUP_PATH], translator[SETUP_PATH], pack[SETUP_PATH], idpath[SETUP_PATH];
    int rc = -1, resuming = 0, keep_work = 0;
    uint64_t t0 = st_now_ms();
    if (st_path(translator, sizeof translator, "%s/libtotk_xlate.so", setup_lib_dir(c)) ||
        st_path(pack, sizeof pack, "%s/seed.vkpack", out_seed_dir) || st_path(idpath, sizeof idpath, "%s/seed-id.txt", out_seed_dir)) {
        setup_fail(c, "io", "Shader seed path too long"); return -1;
    }
    /* Checked first: scanning the RomFS is pointless without the translator. */
    if (access(translator, X_OK)) { setup_fail(c, "not_available", "The shader translator is not part of this build (%s)", translator); return -1; }
    if (st_mkdirs(out_seed_dir)) return st_io_fail(c, "create", out_seed_dir);
    setup_progress(c, "shaders", 0, 0, "reading the shader list");
    int lfd; uint64_t loff, llen;
    if (!setup_shader_list(c, &lfd, &loff, &llen) && (rc = seed_list_load(c, &list, lfd, loff, llen, &err))) goto done;
    rc = -1;
    if (seed_romfs_open(&romfs, romfs_dir, &err) || !(dicts = seed_dicts_load(&romfs, &err))) goto done;
    char index_sha[65]; sha256_buf_hex(romfs.index, romfs.index_size, index_sha);
    /* The scratch: app-private internal storage when the context has it (always in the app), else <out>/.work. */
    if (c->nobackup[0]) {
        char root[SETUP_PATH], id[65];
        if (scratch_identity(c, translator, rewritten_dir, index_sha, &list, opt, id)) goto done;
        if (st_path(root, sizeof root, "%s/setup-work", c->nobackup) || scratch_open(root, id, work, sizeof work, &resuming)) {
            work[0] = 0; rc = st_io_fail(c, "create", root); goto done;
        }
    } else if (st_path(work, sizeof work, "%s/.work", out_seed_dir) || st_rmtree(work)) { work[0] = 0; rc = st_io_fail(c, "create", out_seed_dir); goto done; }
    if (st_path(loose, sizeof loose, "%s/loose", work) || st_path(spill, sizeof spill, "%s/shaders.spill", work) ||
        st_path(journal, sizeof journal, "%s/translated.journal", work) || st_mkdirs(loose)) { rc = st_io_fail(c, "create", work); goto done; }
    keep_work = 1;   /* from here on a failure leaves the scratch for the next run to resume */
    if (space_check(c, work, loose, out_seed_dir, resuming) || seed_table_init(&table, spill, &err)) goto done;
    /* Only the files the shipped list names hold shaders (1,596 files, 146 MB of the 17 GB 1.4.3 image): read just those. */
    if (opt->files_out && !opt->full_scan) { seed_err(&err, "internal", "--files-out needs --full-scan"); goto done; }
    if (opt->files_out && !(bearing = calloc(romfs.count ? romfs.count : 1, 1))) { seed_err(&err, "internal", "Out of memory"); goto done; }
    if (!opt->full_scan && shader_files(&romfs, index_sha, &scan_files, &nscan, &err)) goto done;

    uint64_t avail = mem_available();
    SeedInventory inv = { .c = c, .err = &err, .table = &table, .romfs = &romfs, .dicts = dicts, .modules = rewritten_dir,
                          .threads = cpus(), .budget = avail / 3 < SCAN_BUDGET_MAX ? avail / 3 : SCAN_BUDGET_MAX,
                          .files = scan_files, .nfiles = nscan, .bearing = bearing, .stop = &stop };
    if ((rc = seed_inventory_run(&inv))) goto done;
    if (opt->files_out && write_shader_files(opt->files_out, &romfs, index_sha, bearing)) { rc = st_io_fail(c, "write", opt->files_out); goto done; }
    uint64_t t_inventory = st_now_ms();
    size_t startup = 0;
    for (size_t i = 0; i < table.cap; i++) startup += table.v[i].used && table.v[i].startup;
    size_t discovered = table.n, located = 0;
    if (!startup) { seed_err(&err, "internal", "no startup shaders discovered"); rc = -1; goto done; }
    setup_progress(c, "shaders", 0, 0, "reading the shaders the shared list locates");
    if ((rc = seed_located(&inv, list.loc, list.nloc, &located))) goto done;
    if ((rc = seed_plan(&table, &list, &plan, &err))) goto done;
    if (opt->plan_out && dump_plan(opt->plan_out, &table, &plan)) { rc = st_io_fail(c, "write", opt->plan_out); goto done; }
    if (opt->plan_only) goto done;
    if (opt->sample_every > 1) {   /* a deterministic sample of whole shader groups */
        size_t k = 0;
        for (size_t g = 0; g < plan.ngroups; g += opt->sample_every) plan.groups[k++] = plan.groups[g];
        plan.ngroups = k; plan.njobs = 0;
        for (size_t g = 0; g < k; g++) plan.njobs += plan.groups[g].n;
    }

    /* Each service holds its own working memory. Five, not eight: on the Thor eight services held the prime core at ~95 °C
       and throttled it, while five took 57.6 s instead of 53.7 s for the whole translation. */
    int workers = cpus() < 5 ? cpus() : 5;
    if ((uint64_t)workers * WORKER_MEMORY > avail) workers = (int)(avail / WORKER_MEMORY) ? (int)(avail / WORKER_MEMORY) : 1;
    if (opt->workers > 0) workers = opt->workers;
    if ((size_t)workers > plan.ngroups) workers = plan.ngroups ? (int)plan.ngroups : 1;
    SeedTranslate tr = { .c = c, .err = &err, .table = &table, .plan = &plan, .translator = translator, .out_dir = loose,
                         .workers = workers, .timeout_ms = opt->request_timeout_ms ? opt->request_timeout_ms : SEED_REQUEST_TIMEOUT_MS,
                         .journal = c->nobackup[0] ? journal : NULL, .resuming = resuming,
                         .stop = &stop, .log_mu = PTHREAD_MUTEX_INITIALIZER };
    rc = seed_translate_run(&tr);
    uint64_t t_translate = st_now_ms();
    uint64_t scratch_bytes = dir_bytes(loose) + table.spill_end;   /* the scratch's peak (the spill goes before the pack) */
    size_t ready = 0;
    for (size_t i = 0; i < table.cap; i++) ready += table.v[i].used && table.v[i].startup && table.v[i].produced;
    /* The run's report next to the staging logs (the orchestrator keeps *.log there after a failure). */
    {
        Buf b = {0}; char logpath[SETUP_PATH];
        buf_printf(&b, "chain %s\nshader_list_sha256 %s\ndiscovered %zu\nlocated %zu\nstartup %zu\njobs %zu\nlisted_jobs %zu\n"
                   "translated %llu\nfailed %llu\nduplicates %llu\nstartup_ready %zu\nworkers %d\ninventory_files %s\ninventory_bytes %llu\n"
                   "scratch %s\nscratch_bytes %llu\nresumed_shaders %zu\ninventory_ms %llu\ntranslate_ms %llu\n",
                   SEED_CHAIN, list.sha256[0] ? list.sha256 : "none", discovered, located, startup, plan.njobs, plan.listed,
                   (unsigned long long)tr.ok, (unsigned long long)tr.failed, (unsigned long long)tr.skipped, ready, workers,
                   scan_files ? "listed" : "all", (unsigned long long)inv.total_bytes, work, (unsigned long long)scratch_bytes, tr.resumed,
                   (unsigned long long)(t_inventory - t0), (unsigned long long)(t_translate - t_inventory));
        if (tr.log.p) { buf_puts(&b, "# failed translations: key, variant, why, stage, error\n"); buf_puts(&b, tr.log.p); }
        if (!b.oom && !st_path(logpath, sizeof logpath, "%s/shaders.log", setup_staging_dir(c))) st_write_file(logpath, b.p, b.n, 0644);
        buf_free(&b); buf_free(&tr.log);
    }
    if (rc) goto done;
    if (ready < SEED_MIN_SHADERS && opt->sample_every <= 1 && !opt->any_count) {
        seed_err(&err, "internal", "only %zu shaders prepared; at least %d are required for first-launch warm-up", ready, SEED_MIN_SHADERS);
        rc = -1; goto done;
    }
    /* The spill is no longer needed: give its space back before the pack is written. The pack consumes the loose files,
       so from here on the scratch cannot be resumed: without the journal a next run translates everything again. */
    seed_table_free(&table); table.spill_fd = -1;
    if ((unlink(journal) && errno != ENOENT) || st_fsync_dir(work)) { rc = st_io_fail(c, "remove", journal); goto done; }
    char seed_id[65]; uint64_t files = 0;
    if ((rc = seed_pack(c, loose, pack, seed_id, &files, &err))) goto done;
    char line[66]; snprintf(line, sizeof line, "%s\n", seed_id);
    if (st_write_file(idpath, line, 65, 0644)) { rc = st_io_fail(c, "write", idpath); goto done; }   /* published last */
    setup_progress(c, "shaders", files, files, "shader seed ready");
    rc = 0;
done:
    rc = report(c, &err, rc);
    seed_plan_free(&plan);
    if (table.v) seed_table_free(&table);
    seed_list_free(&list); seed_dicts_free(dicts); seed_romfs_close(&romfs);
    free(scan_files); free(bearing);
    /* Success, cancel or a scratch that cannot be resumed (<out>/.work) go; a failure keeps it for the next run. */
    if (work[0] && (!rc || rc == SETUP_CANCELLED || !keep_work || !c->nobackup[0])) st_rmtree(work);
    if (rc) { unlink(pack); unlink(idpath); }
    return rc;
}
