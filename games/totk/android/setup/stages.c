/* Decrypted route inputs: the 1.4.3 ExeFS (main / sdk / subsdk0 files, recognised by their pinned SHA-256 rather than
   their names, or one raw ExeFS PFS0 image) and the raw 1.4.3 RomFS (one image or ordered split parts), copied into
   staging with a streaming SHA-256 and checked against the pins. */
#include "setup_internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *const setup_exefs_names[EXEFS_COUNT] = { "main", "sdk", "subsdk0", "main.npdm", "rtld" };
#define EXEFS_MAX_FILE (512ull << 20)

/* Copies [off, off+len) of fd to path (created, fsync'd), hashing it into sha when given. */
static int copy_range(SetupCtx *c, int fd, uint64_t off, uint64_t len, const char *path, Sha256 *sha,
                      const char *stage, uint64_t base_done, uint64_t total, const char *detail) {
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) return st_io_fail(c, "create", path);
    size_t cap = 8u << 20; uint8_t *buf = malloc(cap); int rc = 0;
    if (!buf) { close(out); setup_fail(c, "internal", "Out of memory"); return -1; }
    uint64_t last = 0; StCache cache = {0, 0, 0};
    for (uint64_t done = 0; done < len;) {
        if (setup_cancelled(c)) { rc = SETUP_CANCELLED; break; }
        size_t n = len - done < cap ? (size_t)(len - done) : cap;
        if (st_pread(fd, buf, n, off + done)) { rc = st_io_fail(c, "read", detail); break; }
        st_cache_drop(fd, off + done, n);
        if (sha) sha256_update(sha, buf, n);
        if (st_write(out, buf, n)) { rc = st_io_fail(c, "write", path); break; }
        done += n;
        st_cache_written(out, &cache, done);
        uint64_t now = st_now_ms();
        if (stage && (now - last >= 100 || done == len)) { setup_progress(c, stage, base_done + done, total, detail); last = now; }
    }
    free(buf);
    if (!rc && fsync(out)) rc = st_io_fail(c, "sync", path);
    if (!rc) st_cache_drop(out, 0, 0);   /* the whole file is on disk now; nothing it caches is needed again soon */
    if (close(out) && !rc) rc = st_io_fail(c, "write", path);
    if (rc) unlink(path);
    return rc;
}

static int pin_index(const SetupCtx *c, const char *hex) {
    for (int i = 0; i < EXEFS_COUNT; i++) if (!strcmp(c->pins.exefs[i], hex)) return i;
    return -1;
}
static int name_index(const char *name) {
    for (int i = 0; i < EXEFS_COUNT; i++) if (!strcmp(setup_exefs_names[i], name)) return i;
    return -1;
}

/* The verdict on an ExeFS whose file i hashed to hex[i] (empty: not present); the order of the checks decides which
   problem is reported when there are several. */
static int exefs_verdict(SetupCtx *c, char hex[EXEFS_COUNT][65], int from_update) {
    for (int i = 0; i < EXEFS_COUNT; i++) {
        if (!hex[i][0]) {
            if (i <= EXEFS_SUBSDK0) { setup_fail(c, "missing_input", "The ExeFS has no %s", setup_exefs_names[i]); return -1; }
            continue;
        }
        if (strcmp(hex[i], c->pins.exefs[i])) {
            setup_fail(c, "unsupported_version", from_update ? "The update is not version 1.4.3 (its %s differs); only 1.4.3 is supported"
                                                             : "%s is not the 1.4.3 version; only 1.4.3 is supported", setup_exefs_names[i]);
            return -1;
        }
    }
    return 0;
}
/* The ExeFS hactool extracted into dir: hashed here, since the bytes were never in our hands. */
int setup_exefs_check(SetupCtx *c, const char *dir, int from_update) {
    char hex[EXEFS_COUNT][65];
    for (int i = 0; i < EXEFS_COUNT; i++) {
        char path[SETUP_PATH];
        hex[i][0] = 0;
        if (st_path(path, sizeof path, "%s/%s", dir, setup_exefs_names[i])) return st_io_fail(c, "open", path);
        if (!st_exists(path)) continue;
        int rc = st_sha256_file(c, path, hex[i], NULL);
        if (rc) return rc;
    }
    return exefs_verdict(c, hex, from_update);
}

int setup_exefs_from_files(SetupCtx *c, const SetupRequest *r, const char *out) {
    if (!r->nexefs) { setup_fail(c, "missing_input", "The 1.4.3 ExeFS (main, sdk, subsdk0) was not selected"); return -1; }
    if (st_rmtree(out) || st_mkdirs(out)) return st_io_fail(c, "create", out);
    uint8_t magic[4] = {0};
    const SetupInput *first = &r->exefs[0];
    if (r->nexefs == 1 && first->size >= 16 && !st_pread(first->fd, magic, 4, first->offset) && !memcmp(magic, "PFS0", 4)) {
        PEntry *e; int n;
        if (first->offset) { setup_fail(c, "internal", "ExeFS image with an offset"); return -1; }
        if (part_parse(c, first->fd, 0, first->size, "PFS0", 24, &e, &n)) return -1;
        int rc = 0, seen = 0; char hex[EXEFS_COUNT][65] = {{0}};
        for (int i = 0; i < n && !rc; i++) {
            int k = name_index(e[i].name);
            if (k < 0) continue;
            char path[SETUP_PATH]; uint8_t d[32]; Sha256 sha; sha256_init(&sha);
            if (e[i].size > EXEFS_MAX_FILE || st_path(path, sizeof path, "%s/%s", out, e[i].name)) { setup_fail(c, "missing_input", "ExeFS entry %s is too large", e[i].name); rc = -1; break; }
            rc = copy_range(c, first->fd, e[i].off, e[i].size, path, &sha, "exefs", 0, 0, e[i].name);
            sha256_final(&sha, d); sha256_hex(d, hex[k]);   /* the copy's own hash: the file is not read again */
            seen |= 1 << k;
        }
        free(e);
        if (rc) return rc;
        if (!(seen & 1 << EXEFS_NPDM)) { setup_fail(c, "missing_input", "The selected PFS0 is not an ExeFS (no main.npdm)"); return -1; }
        return exefs_verdict(c, hex, 0);
    }
    int seen = 0; char found[EXEFS_COUNT][65] = {{0}};
    for (int i = 0; i < r->nexefs; i++) {
        const SetupInput *in = &r->exefs[i];
        char tmp[SETUP_PATH], path[SETUP_PATH], hex[65]; uint8_t d[32]; Sha256 sha; sha256_init(&sha);
        if (in->size > EXEFS_MAX_FILE) { setup_fail(c, "missing_input", "%s is too large to be an ExeFS file", in->name); return -1; }
        if (st_path(tmp, sizeof tmp, "%s/.part%d", out, i)) return st_io_fail(c, "create", tmp);
        int rc = copy_range(c, in->fd, in->offset, in->size, tmp, &sha, "exefs", 0, 0, in->name);
        if (rc) return rc;
        sha256_final(&sha, d); sha256_hex(d, hex);
        int k = pin_index(c, hex);
        if (k < 0) {
            unlink(tmp);
            if (name_index(in->name) >= 0) setup_fail(c, "unsupported_version", "%s is not the 1.4.3 version; only 1.4.3 is supported", in->name);
            else setup_fail(c, "missing_input", "%s is not a 1.4.3 ExeFS file (expected main, sdk and subsdk0)", in->name);
            return -1;
        }
        if (seen & 1 << k) { unlink(tmp); setup_fail(c, "missing_input", "%s was selected twice", setup_exefs_names[k]); return -1; }
        seen |= 1 << k; memcpy(found[k], hex, 65);
        if (st_path(path, sizeof path, "%s/%s", out, setup_exefs_names[k]) || rename(tmp, path)) return st_io_fail(c, "rename", tmp);
    }
    return exefs_verdict(c, found, 0);   /* every copied file matched a pin while it was copied */
}

/* The picked parts (any split the user's dump came in) are re-cut into the layout part_bytes asks for in out_dir. */
int setup_romfs_copy(SetupCtx *c, const SetupInput *parts, int nparts, const char *out_dir, uint64_t part_bytes, char sha_hex[65]) {
    if (!nparts) { setup_fail(c, "missing_input", "The 1.4.3 RomFS image was not selected"); return -1; }
    uint64_t total = 0;
    for (int i = 0; i < nparts; i++) {
        if (parts[i].size > UINT64_MAX - total) { setup_fail(c, "missing_input", "RomFS parts are too large"); return -1; }
        total += parts[i].size;
    }
    if (total != c->pins.romfs_size) {
        setup_fail(c, "hash_mismatch", "The RomFS%s %llu bytes; the 1.4.3 RomFS has %llu (wrong version, update not applied, missing parts, or not a raw RomFS image)",
                   nparts > 1 ? " parts add up to" : " image has", (unsigned long long)total, (unsigned long long)c->pins.romfs_size);
        return -1;
    }
    uint8_t h[8];
    if (parts[0].size < 8 || st_pread(parts[0].fd, h, 8, parts[0].offset)) return st_io_fail(c, "read", parts[0].name);
    if (h[0] != 0x50 || h[1] || h[2] || h[3] || h[4] || h[5] || h[6] || h[7]) {
        setup_fail(c, "missing_input", "%s is not a raw RomFS image (pick the decrypted RomFS, or the first part first)", parts[0].name);
        return -1;
    }
    /* Reads go straight into the writer's ring; its threads hash and write while the next buffer is read. */
    RomfsWriter w;
    if (romfs_writer_open(c, &w, out_dir, total, part_bytes)) return -1;
    int rc = 0; uint64_t done = 0, last = 0;
    for (int p = 0; p < nparts && !rc; p++) {
        char detail[300];
        snprintf(detail, sizeof detail, nparts > 1 ? "part %d of %d" : "copying", p + 1, nparts);
        for (uint64_t pos = 0; pos < parts[p].size;) {
            if (setup_cancelled(c)) { rc = SETUP_CANCELLED; break; }
            uint8_t *buf; size_t room;
            if ((rc = romfs_writer_reserve(c, &w, &buf, &room))) break;
            size_t n = parts[p].size - pos < room ? (size_t)(parts[p].size - pos) : room;
            if (st_pread(parts[p].fd, buf, n, parts[p].offset + pos)) { rc = st_io_fail(c, "read", parts[p].name); break; }
            st_cache_drop(parts[p].fd, parts[p].offset + pos, n);
            if ((rc = romfs_writer_commit(c, &w, n))) break;
            pos += n; done += n;
            uint64_t now = st_now_ms();
            if (now - last >= 100 || done == total) { setup_progress(c, "romfs", done, total, detail); last = now; }
        }
    }
    if (!rc) rc = romfs_writer_finish(c, &w, sha_hex);
    if (!rc && strcmp(sha_hex, c->pins.romfs_sha256)) {
        setup_fail(c, "hash_mismatch", "The RomFS does not match 1.4.3 (SHA-256 differs): wrong version, update not applied, parts out of order, or a damaged dump");
        rc = -1;
    }
    if (rc) romfs_writer_abort(&w);
    return rc;
}
