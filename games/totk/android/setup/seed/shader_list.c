/* The shared shader list (assets/shader_list.tsv, scripts/shader_list.py ShaderList.load) read from the APK asset with
   pread, keeping only what the seed needs: V rows (a shader key and the bind state, "deps", it was translated for) and
   L rows (where a shader that is no BNSH file sits). Every row is validated as load() does, so a list the Python tools
   reject is rejected here too. Then prepare_shader_seed.py plan(). */
#define _GNU_SOURCE 1
#include "seed_int.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
static int is_key(const char *s, size_t n) { if (n != 64) return 0; for (size_t i = 0; i < n; i++) if (hexval((unsigned char)s[i]) < 0) return 0; return 1; }
static void key_bytes(const char *s, uint8_t out[32]) { for (int i = 0; i < 32; i++) out[i] = (uint8_t)(hexval(s[2 * i]) << 4 | hexval(s[2 * i + 1])); }
/* VARIANT ^[0-9a-f]{64}-[0-9a-f]{12}_b$ */
static int is_variant(const char *s, size_t n) {
    if (n != 79 || s[64] != '-' || s[77] != '_' || s[78] != 'b' || !is_key(s, 64)) return 0;
    for (int i = 65; i < 77; i++) if (hexval((unsigned char)s[i]) < 0) return 0;
    return 1;
}
static int is_pipeline(const char *s, size_t n) {   /* ^(?:[0-9a-f]{2})+$ */
    if (!n || n % 2) return 0;
    for (size_t i = 0; i < n; i++) if (hexval((unsigned char)s[i]) < 0) return 0;
    return 1;
}
static int is_hex10(const char *s, size_t n, uint64_t *v) {   /* ^[0-9a-f]{1,10}$ */
    if (!n || n > 10) return 0;
    *v = 0;
    for (size_t i = 0; i < n; i++) { int h = hexval((unsigned char)s[i]); if (h < 0) return 0; *v = *v << 4 | (uint64_t)h; }
    return 1;
}
static int src_char(int ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || strchr("_.()+-", ch); }
/* SOURCE ^(?:module:[a-z0-9]+|[A-Za-z0-9_.()+-]+(?:/[A-Za-z0-9_.()+-]+)*)$ and no part '.' or '..' */
static int is_source(const char *s, size_t n) {
    if (n > 7 && !memcmp(s, "module:", 7)) {
        int ok = 1;
        for (size_t i = 7; i < n && ok; i++) ok = (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9');
        if (ok) return 1;
    }
    size_t part = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            if (!part) return 0;
            const char *p = s + i - part;
            if ((part == 1 && p[0] == '.') || (part == 2 && p[0] == '.' && p[1] == '.')) return 0;
            part = 0;
        } else if (!src_char((unsigned char)s[i])) return 0;
        else part++;
    }
    return 1;
}

/* ---- deps: shader_list.py canonical(): accepted only when the text is exactly json.dumps(deps, sort_keys=True,
   separators=(',', ':')) of {kind: {slot: str | {str: str}}} with kinds from tex/img/attr/fout. A strict parser of that
   form is the same test: anything json.dumps would write differently (spacing, order, escapes, duplicates) fails. ---- */
typedef struct { const char *p, *end; } Cur;
/* A JSON string as json.dumps (ensure_ascii) writes it; out = its UTF-8 bytes, for Python's code point order. */
static int jstring(Cur *c, char *out, size_t cap, size_t *len) {
    if (c->p >= c->end || *c->p != '"') return -1;
    c->p++; *len = 0;
#define PUT(b) do { if (*len >= cap) return -1; out[(*len)++] = (char)(b); } while (0)
    while (c->p < c->end && *c->p != '"') {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch != '\\') { if (ch < 0x20 || ch > 0x7e) return -1; PUT(ch); continue; }
        if (c->p >= c->end) return -1;
        ch = (unsigned char)*c->p++;
        const char *simple = strchr("\"\\nrtbf", ch);
        if (ch && simple) { static const char v[] = "\"\\\n\r\t\b\f"; PUT(v[simple - "\"\\nrtbf"]); continue; }
        if (ch != 'u' || c->end - c->p < 4) return -1;
        uint32_t u = 0;
        for (int i = 0; i < 4; i++) { int h = hexval((unsigned char)c->p[i]); if (h < 0) return -1; u = u << 4 | (uint32_t)h; }
        c->p += 4;
        /* json.dumps escapes exactly the characters outside ' '..'~' that have no short form */
        if ((u >= 0x20 && u <= 0x7e) || u == '\n' || u == '\r' || u == '\t' || u == '\b' || u == '\f') return -1;
        if (u >= 0xd800 && u < 0xdc00 && c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
            uint32_t lo = 0; int ok = 1;
            for (int i = 0; i < 4 && ok; i++) { int h = hexval((unsigned char)c->p[2 + i]); ok = h >= 0; lo = lo << 4 | (uint32_t)(h < 0 ? 0 : h); }
            if (ok && lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); c->p += 6; }
        }
        if (u < 0x80) PUT(u);
        else if (u < 0x800) { PUT(0xc0 | u >> 6); PUT(0x80 | (u & 63)); }
        else if (u < 0x10000) { PUT(0xe0 | u >> 12); PUT(0x80 | (u >> 6 & 63)); PUT(0x80 | (u & 63)); }
        else { PUT(0xf0 | u >> 18); PUT(0x80 | (u >> 12 & 63)); PUT(0x80 | (u >> 6 & 63)); PUT(0x80 | (u & 63)); }
    }
#undef PUT
    if (c->p >= c->end) return -1;
    c->p++;
    return 0;
}
static int lit(Cur *c, char ch) { if (c->p < c->end && *c->p == ch) { c->p++; return 0; } return -1; }
/* sorted, unique keys: each key greater than the previous (memcmp of UTF-8 = code point order) */
static int after(const char *prev, size_t pn, int first, const char *k, size_t kn) {
    if (first) return 0;
    int c = memcmp(prev, k, pn < kn ? pn : kn);
    return c < 0 || (!c && pn < kn) ? 0 : -1;
}
/* depth 0: kinds (their object texts recorded in l), 1: slots, 2: an image slot's {type, fmt} */
static int jobject(Cur *c, int depth, const char *base, SeedListed *l) {
    char prev[512], key[512]; size_t pn = 0, kn;
    if (lit(c, '{')) return -1;
    if (!lit(c, '}')) return 0;
    for (int first = 1;; first = 0) {
        if (jstring(c, key, sizeof key, &kn) || after(prev, pn, first, key, kn) || lit(c, ':')) return -1;
        if (depth == 0) {
            static const char *const kinds[4] = { "attr", "fout", "img", "tex" };
            int k = 0;
            while (k < 4 && !(kn == strlen(kinds[k]) && !memcmp(key, kinds[k], kn))) k++;
            const char *begin = c->p;
            if (k == 4 || jobject(c, 1, base, l)) return -1;
            l->kind_off[k] = (uint32_t)(begin - base); l->kind_len[k] = (uint32_t)(c->p - begin);
        } else if (depth == 1 && c->p < c->end && *c->p == '{') {
            if (jobject(c, 2, base, l)) return -1;
        } else {
            char v[512]; size_t vn;
            if (jstring(c, v, sizeof v, &vn)) return -1;
        }
        memcpy(prev, key, kn); pn = kn;
        if (!lit(c, '}')) return 0;
        if (lit(c, ',')) return -1;
    }
}
/* kind_off / kind_len: the text of each kind's object (attr, fout, img, tex), for the answers JSON. */
static int deps_canonical(const char *s, size_t n, SeedListed *l) {
    Cur c = { s, s + n };
    return jobject(&c, 0, s, l) || c.p != c.end ? -1 : 0;
}
/* The answers of a listed row as json.dumps({**ANSWERS, **deps}, sort_keys=True, separators=(',', ':')). */
void seed_answers_json(const SeedListed *l, Buf *b) {
    static const char *const kinds[4] = { "attr", "fout", "img", "tex" };
    buf_puts(b, "{");
    for (int k = 0; k < 4; k++) {
        buf_printf(b, "%s\"%s\":", k ? "," : "", kinds[k]);
        if (l && l->kind_len[k]) buf_add(b, l->deps + l->kind_off[k], l->kind_len[k]); else buf_puts(b, "{}");
    }
    buf_puts(b, "}");
}

/* ---- loading ---- */
static char *keep(SeedList *l, const char *s, size_t n) {
    if (l->narena % 64 == 0) { char **p = realloc(l->arena, (l->narena + 64) * sizeof *p); if (!p) return NULL; l->arena = p; }
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, s, n); copy[n] = 0;
    return l->arena[l->narena++] = copy;
}
typedef struct { const char *s; size_t n; } Field;
static int row(SeedList *l, const char *line, size_t n) {
    Field f[8]; int nf = 0;
    for (size_t start = 0, i = 0; i <= n; i++) {
        if (i == n || line[i] == '\t') {
            if (nf == 8) return -1;
            f[nf++] = (Field){ line + start, i - start }; start = i + 1;
        }
    }
    if (f[0].n == 1 && f[0].s[0] == 'V' && nf == 3 && is_key(f[1].s, f[1].n)) {
        SeedListed v; memset(&v, 0, sizeof v);
        if (f[2].n < UINT32_MAX && !deps_canonical(f[2].s, f[2].n, &v)) {
            if (l->n == l->cap) { SeedListed *p = realloc(l->v, (l->cap = l->cap ? l->cap * 2 : 4096) * sizeof *p); if (!p) return -2; l->v = p; }
            key_bytes(f[1].s, v.key);
            if (!(v.deps = keep(l, f[2].s, f[2].n))) return -2;
            v.deps_len = (uint32_t)f[2].n;
            l->v[l->n++] = v;
            return 0;
        }
    }
    if (f[0].n == 1 && f[0].s[0] == 'L') {
        SeedLocator loc;
        if (nf != 6 || !is_key(f[1].s, f[1].n) || !is_source(f[2].s, f[2].n) || !is_hex10(f[3].s, f[3].n, &loc.control) ||
            !is_hex10(f[4].s, f[4].n, &loc.code) || !is_hex10(f[5].s, f[5].n, &loc.length)) return -1;
        key_bytes(f[1].s, loc.key);
        if (!(loc.source = keep(l, f[2].s, f[2].n))) return -2;
        if (l->nloc == l->caploc) { SeedLocator *p = realloc(l->loc, (l->caploc = l->caploc ? l->caploc * 2 : 256) * sizeof *p); if (!p) return -2; l->loc = p; }
        l->loc[l->nloc++] = loc;
        return 0;
    }
    /* warm rows: warm_row(fields[:-1]) plus exactly one flags field t / m / tm */
    const Field *flags = &f[nf - 1];
    int flags_ok = (flags->n == 1 && (flags->s[0] == 't' || flags->s[0] == 'm')) || (flags->n == 2 && !memcmp(flags->s, "tm", 2));
    if (!flags_ok || f[0].n != 1) return -1;
    if (f[0].s[0] == 'S' && nf == 4 && is_variant(f[1].s, f[1].n) && f[2].n == 1 && strchr("015", f[2].s[0])) return 0;
    if (f[0].s[0] == 'C' && nf == 3 && is_variant(f[1].s, f[1].n)) return 0;
    if (f[0].s[0] == 'R' && nf == 3 && is_pipeline(f[1].s, f[1].n)) return 0;
    return -1;
}
static int by_listed(const void *a, const void *b) {
    const SeedListed *x = a, *y = b;
    int c = memcmp(x->key, y->key, 32);
    if (c) return c;
    c = memcmp(x->deps, y->deps, x->deps_len < y->deps_len ? x->deps_len : y->deps_len);
    return c ? c : (x->deps_len > y->deps_len) - (x->deps_len < y->deps_len);
}

int seed_list_load(SetupCtx *c, SeedList *l, int fd, uint64_t offset, uint64_t length, SeedErr *err) {
    memset(l, 0, sizeof *l);
    enum { CHUNK = 1 << 20 };
    char *buf = malloc(2 * CHUNK + 2);   /* a carried partial line (rows are far shorter than a chunk) + the next chunk */
    if (!buf) { seed_err(err, "internal", "Out of memory"); return -1; }
    Sha256 sha; sha256_init(&sha);
    size_t carry = 0; uint64_t number = 0;
    for (uint64_t at = 0; at < length || carry; ) {
        size_t want = length - at < CHUNK ? (size_t)(length - at) : CHUNK;
        if (want && st_pread(fd, buf + carry, want, offset + at)) {
            free(buf); seed_err(err, "io", "Cannot read the shader list: %s", strerror(errno)); return -1;
        }
        sha256_update(&sha, buf + carry, want);
        at += want;
        size_t have = carry + want, start = 0;
        int last = at == length;
        if (last && have && buf[have - 1] != '\n') buf[have++] = '\n';   /* the last line needs no terminator */
        for (size_t i = 0; i < have; i++) {
            if (buf[i] != '\n') continue;
            number++;
            size_t n = i - start; const char *line = buf + start;
            if (n && line[n - 1] == '\r') n--;   /* splitlines(): \r\n ends a line too */
            if (n && line[0] != '#') {
                int rc = memchr(line, '\r', n) ? -1 : row(l, line, n);
                if (rc == -2) { free(buf); seed_err(err, "internal", "Out of memory"); return -1; }
                if (rc) { free(buf); seed_err(err, "internal", "shader_list.tsv:%llu: not a shader list row", (unsigned long long)number); return -1; }
            }
            start = i + 1;
        }
        carry = have - start;
        if (carry > CHUNK) { free(buf); seed_err(err, "internal", "shader_list.tsv: line %llu too long", (unsigned long long)number + 1); return -1; }
        memmove(buf, buf + start, carry);
        if (last) carry = 0;
        if (setup_cancelled(c)) { free(buf); return SETUP_CANCELLED; }
    }
    free(buf);
    uint8_t d[32]; sha256_final(&sha, d); sha256_hex(d, l->sha256);
    /* lst.variants is a set: sorted(listed) order, one row per (key, deps) */
    qsort(l->v, l->n, sizeof *l->v, by_listed);
    size_t k = 0;
    for (size_t i = 0; i < l->n; i++) if (!k || by_listed(&l->v[k - 1], &l->v[i])) l->v[k++] = l->v[i];
    l->n = k;
    return 0;
}
void seed_list_free(SeedList *l) {
    for (size_t i = 0; i < l->narena; i++) free(l->arena[i]);
    free(l->arena); free(l->v); free(l->loc);
    memset(l, 0, sizeof *l);
}

/* ---- plan() ---- */
static void variant_name(const SeedShader *s, const SeedListed *listed, char out[78]) {
    Buf b = {0}; char h[65];
    seed_answers_json(listed, &b);
    sha256_buf_hex(b.p ? b.p : "", b.n, h);
    buf_free(&b);
    sha256_hex(s->key, out);
    out[64] = '-'; memcpy(out + 65, h, 12); out[77] = 0;
}
static SeedShader **startup_sorted(SeedTable *t, size_t *n) {
    size_t k = 0;
    for (size_t i = 0; i < t->cap; i++) k += t->v[i].used && t->v[i].startup;
    SeedShader **v = malloc((k ? k : 1) * sizeof *v);
    if (!v) return NULL;
    k = 0;
    for (size_t i = 0; i < t->cap; i++) if (t->v[i].used && t->v[i].startup) v[k++] = &t->v[i];
    *n = k;
    return v;
}
static int by_shader_key(const void *a, const void *b) { return memcmp((*(SeedShader *const *)a)->key, (*(SeedShader *const *)b)->key, 32); }

/* plan(): the default bind state of every startup shader (sorted by key), then each listed (key, deps) of a shader found
   in this game, in sorted order. Translation runs per shader (prepare's by_key: its jobs in job order, shaders in
   order of their first job), so the jobs are stored grouped that way: each startup shader's default job followed by
   its listed jobs, then the listed-only shaders. The listed rows are sorted by key, so a shader's rows are one run. */
static size_t listed_run(const SeedList *l, const uint8_t key[32], size_t *n) {
    size_t lo = 0, hi = l->n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (memcmp(l->v[mid].key, key, 32) < 0) lo = mid + 1; else hi = mid; }
    size_t end = lo;
    while (end < l->n && !memcmp(l->v[end].key, key, 32)) end++;
    *n = end - lo;
    return lo;
}
static void add_job(SeedPlan *p, SeedShader *s, const SeedListed *listed) {
    SeedJob *j = &p->jobs[p->njobs++];
    j->shader = s; j->listed = listed; variant_name(s, listed, j->variant);
    if (listed) p->listed++;
}
int seed_plan(SeedTable *t, const SeedList *l, SeedPlan *p, SeedErr *err) {
    memset(p, 0, sizeof *p);
    size_t nstart; SeedShader **start = startup_sorted(t, &nstart);
    if (!start) { seed_err(err, "internal", "Out of memory"); return -1; }
    qsort(start, nstart, sizeof *start, by_shader_key);
    p->jobs = malloc((nstart + l->n + 1) * sizeof *p->jobs);
    p->groups = malloc((nstart + l->n + 1) * sizeof *p->groups);
    if (!p->jobs || !p->groups) { free(start); seed_err(err, "internal", "Out of memory"); return -1; }
    for (size_t i = 0; i < nstart; i++) {
        SeedGroup *g = &p->groups[p->ngroups++]; g->first = p->njobs;
        add_job(p, start[i], NULL);
        size_t n, at = listed_run(l, start[i]->key, &n);
        for (size_t k = 0; k < n; k++) add_job(p, start[i], &l->v[at + k]);
        g->n = p->njobs - g->first;
    }
    free(start);
    for (size_t i = 0; i < l->n; ) {
        size_t n; listed_run(l, l->v[i].key, &n);
        SeedShader *s = seed_table_get(t, l->v[i].key);
        if (s && !s->startup) {
            SeedGroup *g = &p->groups[p->ngroups++]; g->first = p->njobs;
            for (size_t k = 0; k < n; k++) add_job(p, s, &l->v[i + k]);
            g->n = n;
        }
        i += n;
    }
    return 0;
}
void seed_plan_free(SeedPlan *p) { free(p->jobs); free(p->groups); memset(p, 0, sizeof *p); }
