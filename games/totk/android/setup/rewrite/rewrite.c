/* C port of third_party/d2r/patch_nso.py (+ call_window.py, hostreg.py) for the configuration the Android build uses:
   DM_RULESET=native, DM_PRIVATE_GATES=0, DM_HOST_TP=tpidr_el0. The upstream cut / observation / parameter tables were
   keyed by other games' build ids and are not carried (the Python rewriter keeps them empty too); every other step runs in the same
   order with the same checks, because the output must stay byte-identical to the Python rewriter (the pinned
   android_rewritten_sha256). Capstone 5.0.9 (the version requirements.txt pins for the Python binding) decodes, so the
   mnemonic / operand text / register-access answers the heuristics read are the same ones the Python script saw.
   Threads: the linear scan for rewrite points and the disassembly of the functions holding x18 sites run on up to
   RW_THREADS cores (one Capstone handle each); every decision and every write stays on the calling thread in the same
   order as before, so the output does not depend on the thread count.
   Memory: the module image, the patch area and one batch of disassembled functions at a time (main: ~150 MB peak). */
#include "rw_internal.h"
#include <capstone/capstone.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define RW_THREADS 8
#define SCAN_CHUNK (256u << 10)      /* text bytes per scan work item */
#define DECODE_BATCH (1u << 20)      /* function bytes disassembled per batch (Ins is 200 bytes per 4) */

const char *const rw_tsv_names[6] = {"fixups", "gates", "sites", "cuts", "params", "obs"};

/* The only modules this port accepts (Tears of the Kingdom 1.4.3); anything else is refused before rewriting. */
static const struct { const char *name, *build_id; } KNOWN[] = {
    {"main", "277178b7dba1b6d4949a84778d4abc58"}, {"subsdk0", "f39f4c431234c72eb520cfdd1001bbcf"}, {"sdk", "574284b40c06cf1821a44857939083be"}};
/* patch_nso.py SIGNED_TABLES: signed 32-bit offset jump tables of 1.4.3 main. The 20 bytes ending at each dispatch br
   and the n table entries are checked by SHA-256 before use (no game bytes here); the entries are then read from main. */
static const struct { uint64_t br, adr, table; int n; const char *dispatch_sha256, *table_sha256; } SIGNED[] = {
    {0x393bb0, 0x393ba0, 0x2d8626c, 6, "c4b04a76294666a64d2781ce7baeace80a5f4c7d642e8fa3ce378f654f5a2db0", "d33595fbce2691ca1f39bbc1c6844276e2b186fca36257fd37a47eb5300de0ce"},
    {0x596d30, 0x596d20, 0x2d89154, 9, "4013676edf743295ec1ebf998686b48611e56940a0c1dc3ae3b4181ca4a82301", "b631a17de2c73343aaee6d69f1b8a9d405da78dc04d47340e1bff9ca5a8bdafe"},
    {0x598ef4, 0x598ee4, 0x2d89038, 9, "9870cc3d2f0c71d6a09d96e7ac43fcf70d76695ff463760694559035fb4967f1", "35d321f36b6efb7fd9781f31fe86ec09fd86e4be05102e5316e73fb6a5fe3ab2"},
    {0x5f2a9c, 0x5f2a90, 0x2d892e0, 5, "d67af8cdfbb0358495b476292eae9c7b52067e0d38ab4439a3ba760eb21f0f4a", "c3511ac589afaca4b75f001ad082a90aef06118c7e65d7f9076ab78dc862f0f2"},
    {0x6397d8, 0x6397cc, 0x2d88e7c, 7, "f3bc8a30d4d2885e8c328e9c2d145f9a24ad89f36e72ca52bd179f1733016f46", "419fdee7c1384717b79e08e702eb2ca83db9aa68468e28468915e2c03230e98e"}};
#define SIGNED_BUILD "277178b7dba1b6d4949a84778d4abc58"
/* Key page indices (patch_nso.py KEY_INDEX); the loader writes "actual key number x 8" into the page. */
static const char *const KEY_NAMES[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
enum { K18, KTLS, KTLR, KGATE };

typedef struct { uint64_t a; uint32_t w; char mn[32]; char op[160]; } Ins;
typedef struct { Ins *v; size_t n; } Seg;
typedef struct { size_t *v; size_t n; } Group;
enum { K_SVC, K_TPIDR_R, K_TPIDR_W, K_TPIDRRO_R, K_CNTPCT, K_CNTVCT, K_X18 };
static const char *const KIND[] = {"svc", "tpidr_r", "tpidr_w", "tpidrro_r", "cntpct", "cntvct", "x18"};
typedef struct { uint64_t a; uint32_t kind, arg; } Kind;
typedef struct { uint64_t prefix, call; uint32_t word; } Call;
typedef struct { uint64_t br, base, width; } Jump;

typedef struct {
    jmp_buf jb; char *err; size_t errlen; int cancelled;
    csh hd; cs_insn *d1, *d2;
    int nth; csh th[RW_THREADS]; cs_insn *tins[RW_THREADS];   /* per-thread decoders without detail */
    RwNso n; uint8_t *img; uint64_t len, text_end, key, patch_off;
    uint32_t *patch; size_t np, capp;
    RwGate *gates; size_t ng, capg;
    Kind *kinds; size_t nk, capk;
    uint8_t *done;
    int64_t *starts; size_t ns, caps;
    Seg *win; size_t nw, capw;
    Group *groups; size_t ngr, capgr;
    Call *calls; size_t nc, capc;
    Jump *jumps; size_t nj, capj;
    Ins **pre; size_t *npre, nfn;    /* per x18 function: its instructions while its batch is analysed */
    struct Fn *fns;                  /* the x18 functions, in address order */
    Kind **scan_v; size_t *scan_n, nscan;   /* per scan chunk */
    RwTags tags; RwRelr *relr;
    RwPoll poll; void *ud;
} Rw;

#define FAIL(...) do { snprintf(R->err, R->errlen, __VA_ARGS__); longjmp(R->jb, 1); } while (0)
static void *xrealloc(Rw *R, void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) FAIL("out of memory"); return q; }
#define PUSH(arr, n, cap, val) do { if ((n) == (cap)) { (cap) = (cap) ? 2 * (cap) : 64; (arr) = xrealloc(R, (arr), (cap) * sizeof *(arr)); } (arr)[(n)++] = (val); } while (0)
static int poll_soft(Rw *R, uint64_t done, uint64_t total) { if (R->poll && R->poll(R->ud, done, total)) { R->cancelled = 1; return 1; } return 0; }
static void poll(Rw *R, uint64_t done, uint64_t total) { if (poll_soft(R, done, total)) longjmp(R->jb, 1); }

/* ---- the worker pool: items claimed in any order, results stored per item; the caller (thread 0) polls cancellation
   between its own items. Workers never longjmp: allocation failures are flagged and raised after the join. ---- */
typedef struct Par Par;
struct Par {
    Rw *R; size_t n; void (*fn)(Par *, int tid, size_t item);
    _Atomic size_t next; _Atomic uint64_t done; atomic_int stop, oom;
    uint64_t base, total;            /* poll(base + done, total) */
    void *ctx;
};
typedef struct { Par *P; int tid; } ParArg;
static void par_loop(Par *P, int tid) {
    while (!atomic_load(&P->stop)) {
        size_t i = atomic_fetch_add(&P->next, 1);
        if (i >= P->n) break;
        P->fn(P, tid, i);
        if (!tid && poll_soft(P->R, P->base + atomic_load(&P->done), P->total)) atomic_store(&P->stop, 1);
    }
}
static void *par_thread(void *a) { ParArg *p = a; par_loop(p->P, p->tid); return NULL; }
static void par_run(Rw *R, Par *P) {
    pthread_t th[RW_THREADS]; ParArg args[RW_THREADS]; int started = 0;
    for (int t = 1; t < R->nth && (size_t)t < P->n; t++) {
        args[t] = (ParArg){P, t};
        if (pthread_create(&th[t], NULL, par_thread, &args[t])) break;   /* fewer threads, same result */
        started = t;
    }
    par_loop(P, 0);
    for (int t = 1; t <= started; t++) pthread_join(th[t], NULL);
    if (R->cancelled) longjmp(R->jb, 1);
    if (atomic_load(&P->oom)) FAIL("out of memory");
}
#define HX(v) ((unsigned long long)(v))

/* ---- decoding ---- */
static int decode(csh h, cs_insn *buf, uint32_t w, uint64_t a) {
    uint8_t b[4]; wr32(b, w); const uint8_t *p = b; size_t n = 4; uint64_t at = a;
    return cs_disasm_iter(h, &p, &n, &at, buf) ? 1 : 0;
}
static void fill(Ins *x, const cs_insn *c, uint32_t w) {
    x->a = c->address; x->w = w;
    snprintf(x->mn, sizeof x->mn, "%s", c->mnemonic); snprintf(x->op, sizeof x->op, "%s", c->op_str);
}
/* patch_nso.py dis(): exactly one instruction, with detail. */
static cs_insn *dis_into(Rw *R, cs_insn *buf, uint32_t w, uint64_t a) { if (!decode(R->hd, buf, w, a)) FAIL("%#x", w); return buf; }
static Ins ins_at(Rw *R, uint32_t w, uint64_t a) { Ins x; dis_into(R, R->d1, w, a); fill(&x, R->d1, w); return x; }
/* md.disasm(img[fa:fb], fa): stops at the first invalid word. Thread-safe with the thread's own handle. */
static int disasm_function(csh h, cs_insn *c, const uint8_t *img, uint64_t fa, uint64_t fb, Ins **out, size_t *count) {
    const uint8_t *p = img + fa; size_t n = fb - fa, k = 0; uint64_t at = fa;
    Ins *v = malloc((n / 4 ? n / 4 : 1) * sizeof *v);
    if (!v) return -1;
    while (n >= 4 && cs_disasm_iter(h, &p, &n, &at, c)) fill(&v[k++], c, rd32(c->bytes));
    *out = v; *count = k;
    return 0;
}
static cs_arm64_op *opnd(Rw *R, const Ins *x, int i) {      /* operands[i], negative from the end like Python */
    cs_insn *c = dis_into(R, R->d1, x->w, x->a); cs_arm64 *d = &c->detail->arm64;
    int k = i < 0 ? d->op_count + i : i;
    if (k < 0 || k >= d->op_count) FAIL("operand %d missing at %llx %s %s", i, HX(x->a), x->mn, x->op);
    return &d->operands[k];
}
static int64_t last_imm(Rw *R, const Ins *x) { return opnd(R, x, -1)->imm; }
static int64_t imm_of(Rw *R, const Ins *x, int i) { return opnd(R, x, i)->imm; }
static int64_t floordiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b && (a < 0) != (b < 0)) ? q - 1 : q; }

/* ---- operand-text helpers (the Python regexes) ---- */
static int wch(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
/* Next \b[xw](\d+)\b match at or after *i: returns the number (capped), its start in *s and end in *e; -1 when none. */
static long next_reg(const char *op, size_t *i, size_t *s, size_t *e) {
    for (size_t k = *i; op[k]; k++) {
        if ((op[k] != 'x' && op[k] != 'w') || (k && wch((unsigned char)op[k - 1]))) continue;
        size_t j = k + 1; long v = 0;
        while (op[j] >= '0' && op[j] <= '9') { if (v < 100000) v = v * 10 + (op[j] - '0'); j++; }
        if (j == k + 1 || wch((unsigned char)op[j])) continue;
        *s = k; *e = j; *i = j; return v;
    }
    return -1;
}
static int is18(const char *op, size_t s, size_t e) { return e - s == 3 && op[s + 1] == '1' && op[s + 2] == '8'; }
static int has18(const char *op) { size_t i = 0, s, e; while (next_reg(op, &i, &s, &e) >= 0) if (is18(op, s, e)) return 1; return 0; }
static int num(const char *name) {                     /* fullmatch [xw](\d+); -1 = None */
    if (!name || (name[0] != 'x' && name[0] != 'w') || !name[1]) return -1;
    long v = 0; for (const char *p = name + 1; *p; p++) { if (*p < '0' || *p > '9') return -1; if (v < 100000) v = v * 10 + (*p - '0'); }
    return (int)v;
}
static int pyint(const char *s, int64_t *out) {        /* int(s, 0) for the forms capstone prints */
    int neg = 0; uint64_t v = 0; const char *p = s;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; if (!*p) return -1;
        for (; *p; p++) { int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1; if (d < 0 || v >> 59) return -1; v = v * 16 + (uint64_t)d; }
    } else {
        if (!*p || (p[0] == '0' && p[1])) return -1;
        for (; *p; p++) { if (*p < '0' || *p > '9' || v > UINT64_MAX / 16) return -1; v = v * 10 + (uint64_t)(*p - '0'); }
    }
    *out = neg ? -(int64_t)v : (int64_t)v; return 0;
}
static int64_t must_int(Rw *R, const char *s) { int64_t v; if (pyint(s, &v)) FAIL("invalid literal for int(): %s", s); return v; }

typedef struct { char buf[160]; char *t[96]; int n; } Ops;
static void ops(const Ins *x, Ops *o) {                /* op_str without brackets, split on ',', stripped */
    size_t k = 0; for (const char *p = x->op; *p && k < sizeof o->buf - 1; p++) if (*p != '[' && *p != ']') o->buf[k++] = *p;
    o->buf[k] = 0; o->n = 0; char *p = o->buf;
    for (;;) {
        char *c = strchr(p, ','); if (c) *c = 0;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p + strlen(p); while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (o->n < 96) o->t[o->n++] = p;
        if (!c) break;
        p = c + 1;
    }
}
static int in_list(const char *s, const char *const *l) { for (; *l; l++) if (!strcmp(s, *l)) return 1; return 0; }
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static int condbr(const char *m) { return !strcmp(m, "cbz") || !strcmp(m, "cbnz") || !strcmp(m, "tbz") || !strcmp(m, "tbnz") || starts(m, "b."); }

/* regs_in(): register numbers named in the operand text plus capstone's read/write report. */
static uint64_t regs_in(Rw *R, const Ins *x) {
    uint64_t m = 0; size_t i = 0, s, e; long v;
    while ((v = next_reg(x->op, &i, &s, &e)) >= 0) if (v < 64) m |= 1ull << v;
    cs_insn *c = dis_into(R, R->d1, x->w, x->a); cs_regs rd, wr; uint8_t nr, nw;
    if (cs_regs_access(R->hd, c, rd, &nr, wr, &nw) == CS_ERR_OK) {
        for (int k = 0; k < nr; k++) { int r = num(cs_reg_name(R->hd, rd[k])); if (r >= 0 && r < 64) m |= 1ull << r; }
        for (int k = 0; k < nw; k++) { int r = num(cs_reg_name(R->hd, wr[k])); if (r >= 0 && r < 64) m |= 1ull << r; }
    }
    return m;
}
static void pick(Rw *R, uint64_t avoid, int n, int *out) {
    int k = 0; for (int r = 0; r < 16 && k < n; r++) if (!(avoid >> r & 1) && r != 18) out[k++] = r;
    if (k != n) FAIL("no free scratch register");
}

/* ---- encoders ---- */
static uint32_t B(Rw *R, uint64_t frm, uint64_t to) {
    int64_t off = (int64_t)(to - frm);
    if (off < -(1 << 27) || off >= (1 << 27) || off % 4) FAIL("branch out of range %llx -> %llx", HX(frm), HX(to));
    return 0x14000000 | ((uint32_t)(off >> 2) & 0x3FFFFFF);
}
static uint32_t pair(uint32_t op, int t, int t2, int n, int64_t off) { return op | ((uint32_t)floordiv(off, 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t; }
#define STP(t, t2, n, o) pair(0xA9000000, t, t2, n, o)
#define LDP(t, t2, n, o) pair(0xA9400000, t, t2, n, o)
#define STP_PRE(t, t2, n, o) pair(0xA9800000, t, t2, n, o)
#define LDP_POST(t, t2, n, o) pair(0xA8C00000, t, t2, n, o)
static uint32_t STUR(int t, int n, int64_t off) { return 0xF8000000 | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t LDUR(int t, int n, int64_t off) { return 0xF8400000 | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t LDRX(int t, int n, uint32_t imm12) { return 0xF9400000 | imm12 << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t STRX(int t, int n) { return 0xF9000000 | (uint32_t)n << 5 | (uint32_t)t; }
static uint32_t ADRP(Rw *R, int d, uint64_t pc, uint64_t target) {
    int64_t imm = ((int64_t)(target & ~0xFFFull) - (int64_t)(pc & ~0xFFFull)) / 4096;
    if (imm < -(1 << 20) || imm >= (1 << 20)) FAIL("adrp out of range at %llx", HX(pc));
    return 0x90000000 | ((uint32_t)imm & 3) << 29 | ((uint32_t)(imm >> 2) & 0x7FFFF) << 5 | (uint32_t)d;
}
static uint32_t ADDI(int d, int n, uint32_t imm12, int sh) { return 0x91000000 | (uint32_t)sh << 22 | imm12 << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t SUBI(int d, int n, uint32_t imm12, int sh) { return 0xD1000000 | (uint32_t)sh << 22 | imm12 << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t ADDR(int d, int n, int m) { return 0x8B000000 | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
#define BR(n) (0xD61F0000 | (uint32_t)(n) << 5)
#define BLR(n) (0xD63F0000 | (uint32_t)(n) << 5)
#define NOP 0xD503201Fu
#define BLR_X18 0xD63F0240u
#define BLR_X30 0xD63F03C0u

static uint64_t here(Rw *R) { return R->patch_off + 4 * R->np; }
static void emit(Rw *R, uint32_t w) { PUSH(R->patch, R->np, R->capp, w); }
/* hostreg.py with DM_HOST_TP=tpidr_el0: mrs xN, tpidr_el0 ; ldr xN, [xN, #16] (bionic TLS_SLOT_APP holds the slot block). */
#define HOST_TP_LEN 2
static void emit_host_tp(Rw *R, int reg) { if (reg < 0 || reg >= 31) FAIL("host thread pointer into x%d", reg); emit(R, 0xD53BD040u | (uint32_t)reg); emit(R, LDRX(reg, reg, 2)); }
static void emit_slot_addr(Rw *R, int base, int key_reg, int key) {
    emit(R, ADRP(R, key_reg, here(R), R->key)); emit(R, LDRX(key_reg, key_reg, (uint32_t)key)); emit(R, ADDR(base, base, key_reg));
}
static void put_branch(Rw *R, uint64_t at, uint64_t to) { wr32(R->img + at, B(R, at, to)); }

/* subst(): x18/w18 -> xS/wS by changing only the register fields, checked against the disassembly text. */
static uint32_t subst(Rw *R, const Ins *x, int S) {
    char want[200]; size_t i = 0, s, e, o = 0, last = 0;
    while (next_reg(x->op, &i, &s, &e) >= 0) if (is18(x->op, s, e)) {
        o += (size_t)snprintf(want + o, sizeof want - o, "%.*s%c%d", (int)(s - last), x->op + last, x->op[s], S); last = e; }
    snprintf(want + o, sizeof want - o, "%s", x->op + last);
    int pos[4], np = 0; static const int F[4] = {0, 5, 10, 16};
    for (int k = 0; k < 4; k++) if ((x->w >> F[k] & 31) == 18) pos[np++] = F[k];
    for (int k = np; k >= 1; k--) {                    /* itertools.combinations(pos, k), lexicographic */
        int idx[4]; for (int t = 0; t < k; t++) idx[t] = t;
        for (;;) {
            uint32_t nw = x->w; for (int t = 0; t < k; t++) nw = (nw & ~(31u << pos[idx[t]])) | (uint32_t)S << pos[idx[t]];
            if (decode(R->hd, R->d2, nw, x->a) && !strcmp(R->d2->mnemonic, x->mn) && !strcmp(R->d2->op_str, want)) return nw;
            int t = k - 1; while (t >= 0 && idx[t] == np - k + t) t--;
            if (t < 0) break;
            idx[t]++; for (int u = t + 1; u < k; u++) idx[u] = idx[u - 1] + 1;
        }
    }
    FAIL("subst fail %llx %s %s", HX(x->a), x->mn, x->op);
}

/* sp_delta(): writeback amount on sp ([sp, #i]! or [sp], #i), 0 when sp is not written back. */
static int hexd(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static int sp_imm(const char *p, int pre, int64_t *v) {   /* p after "#": -?(?:0x)?[0-9a-f]+ then "]!" or end */
    const char *q = p; if (*q == '-') q++;
    if (q[0] == '0' && q[1] == 'x' && hexd(q[2])) q += 2;
    if (!hexd(*q)) return 0;
    while (hexd(*q)) q++;
    if (pre ? strncmp(q, "]!", 2) : *q != 0) return 0;
    char t[64]; size_t n = (size_t)(q - p); if (n >= sizeof t) return 0;
    memcpy(t, p, n); t[n] = 0; return pyint(t, v) ? -1 : 1;
}
static int64_t sp_delta(Rw *R, const Ins *x) {
    int64_t v; const char *p;
    for (p = x->op; (p = strstr(p, "[sp, #")); p++) { int r = sp_imm(p + 6, 1, &v); if (r < 0) FAIL("sp offset %s", x->op); if (r) return v; }
    for (p = x->op; (p = strstr(p, "[sp], #")); p++) { int r = sp_imm(p + 7, 0, &v); if (r < 0) FAIL("sp offset %s", x->op); if (r) return v; }
    if (starts(x->op, "sp,") || starts(x->op, "wsp,")) FAIL("writes sp: %llx %s", HX(x->a), x->op);
    return 0;
}

/* ---- rewrite blocks ---- */
static uint64_t site_plain(Rw *R, const Ins *x) {
    int ST[2]; pick(R, regs_in(R, x), 2, ST); int S = ST[0], T = ST[1];
    int64_t dlt = sp_delta(R, x), save = dlt == 0 ? -16 : -112;     /* sp-writing forms store below sp: save deeper */
    if (dlt != 0 && dlt < -96) FAIL("sp delta %lld at %llx", (long long)dlt, HX(x->a));
    uint64_t start = here(R);
    emit(R, STP(S, T, 31, save)); emit_host_tp(R, T); emit_slot_addr(R, T, S, K18); emit(R, LDRX(S, T, 0));
    emit(R, subst(R, x, S));
    emit(R, STRX(S, T));                                             /* always write back: capstone misses some writes */
    emit(R, LDP(S, T, 31, save - dlt)); emit(R, B(R, here(R), x->a + 4));
    return start;
}
static uint64_t site_cond(Rw *R, const Ins *x) {
    int ST[2]; pick(R, regs_in(R, x), 2, ST); int S = ST[0], T = ST[1];
    uint64_t tgt = (uint64_t)last_imm(R, x), start = here(R);
    emit(R, STP(S, T, 31, -16)); emit_host_tp(R, T); emit_slot_addr(R, T, S, K18); emit(R, LDRX(S, T, 0));
    uint64_t taken = here(R) + 12; uint32_t off = (uint32_t)((taken - here(R)) >> 2), w = x->w;
    uint32_t nw = (!strcmp(x->mn, "cbz") || !strcmp(x->mn, "cbnz")) ? (w & 0xFF000000) | (off & 0x7FFFF) << 5 | (uint32_t)S
                                                                     : (w & 0xFFF80000) | (off & 0x3FFF) << 5 | (uint32_t)S;
    uint64_t pc = here(R); emit(R, nw);
    cs_insn *d = dis_into(R, R->d2, nw, pc);
    if (strcmp(d->mnemonic, x->mn) || !d->detail->arm64.op_count || (uint64_t)d->detail->arm64.operands[d->detail->arm64.op_count - 1].imm != taken) FAIL("cond rewrite %llx", HX(x->a));
    emit(R, LDP(S, T, 31, -16)); emit(R, B(R, here(R), x->a + 4));
    if (here(R) != taken) FAIL("cond layout %llx", HX(x->a));
    emit(R, LDP(S, T, 31, -16)); emit(R, B(R, here(R), tgt));
    return start;
}
static uint64_t site_pcrel(Rw *R, const Ins *x) {
    uint64_t tgt = (uint64_t)imm_of(R, x, 1); int S = 0, T = 1, ST[2]; pick(R, 0, 2, ST); S = ST[0]; T = ST[1];
    uint64_t start = here(R);
    emit(R, STP(S, T, 31, -16)); emit_host_tp(R, T); emit_slot_addr(R, T, S, K18);
    uint64_t pc = here(R); emit(R, ADRP(R, S, pc, tgt));
    if (!strcmp(x->mn, "adr")) emit(R, ADDI(S, S, (uint32_t)(tgt & 0xFFF), 0));
    emit(R, STRX(S, T)); emit(R, LDP(S, T, 31, -16)); emit(R, B(R, here(R), x->a + 4));
    return start;
}
static uint64_t site_br(Rw *R, const Ins *x, uint64_t base, uint64_t count) {
    uint64_t start = here(R);
    emit(R, STUR(16, 31, -16)); emit(R, STUR(17, 31, -24));          /* x16/x17 may be live in the function: red zone */
    emit_host_tp(R, 16); emit_slot_addr(R, 16, 17, K18); emit(R, LDRX(16, 16, 0)); emit(R, LDUR(17, 31, -24));
    uint64_t padbase = start + 4 * 24; int64_t delta = (int64_t)(padbase - base); int neg = delta < 0;
    uint64_t dl = (uint64_t)(neg ? -delta : delta), hi = dl >> 12, lo = dl & 0xFFF; uint32_t opsv[64]; int nops = 0;
    while (hi) { uint64_t h = hi < 0xFFF ? hi : 0xFFF; if (nops < 64) opsv[nops] = neg ? SUBI(16, 16, (uint32_t)h, 1) : ADDI(16, 16, (uint32_t)h, 1); nops++; hi -= h; }
    if (lo) { if (nops < 64) opsv[nops] = neg ? SUBI(16, 16, (uint32_t)lo, 0) : ADDI(16, 16, (uint32_t)lo, 0); nops++; }
    if (nops > 12) FAIL("jump table too far at %llx", HX(x->a));
    for (int i = 0; i < nops; i++) emit(R, opsv[i]);
    emit(R, BR(16));
    if (here(R) > padbase) FAIL("jump-table prologue overruns its landing pads at %llx", HX(x->a));
    while (here(R) < padbase) emit(R, NOP);
    uint64_t tails = padbase + 4 * count;
    for (uint64_t i = 0; i < count; i++) emit(R, B(R, here(R), tails + 8 * i));
    for (uint64_t i = 0; i < count; i++) { emit(R, 0xF85F0000u | 31 << 5 | 16); emit(R, B(R, here(R), base + 4 * i)); }
    return start;
}
static uint64_t site_sysreg_read(Rw *R, const Ins *x, int key) {
    int N = (int)(x->w & 31), tmp = N != 16 ? 16 : 17; uint64_t start = here(R);
    emit(R, STUR(tmp, 31, -16)); emit_host_tp(R, N); emit_slot_addr(R, N, tmp, key); emit(R, LDRX(N, N, 0)); emit(R, LDUR(tmp, 31, -16));
    emit(R, B(R, here(R), x->a + 4));
    return start;
}
static uint64_t site_tpidr_write(Rw *R, const Ins *x) {
    int N = (int)(x->w & 31), T; pick(R, N < 64 ? 1ull << N : 0, 1, &T);
    uint64_t start = here(R);
    emit(R, 0xF81F0000u | 31 << 5 | (uint32_t)T);
    int tmp = 16 != N && 16 != T ? 16 : 17 != N && 17 != T ? 17 : -1; if (tmp < 0) FAIL("no temporary at %llx", HX(x->a));
    emit(R, STUR(tmp, 31, -24)); emit_host_tp(R, T); emit_slot_addr(R, T, tmp, KTLS); emit(R, STRX(N, T)); emit(R, LDUR(tmp, 31, -24));
    emit(R, 0xF85F0000u | 31 << 5 | (uint32_t)T);
    emit(R, B(R, here(R), x->a + 4));
    return start;
}
static uint64_t site_gate(Rw *R, const Ins *x, uint32_t kind, uint32_t arg) {
    uint64_t start = here(R);
    emit(R, STP_PRE(29, 30, 31, -16));
    emit_host_tp(R, 30); emit_slot_addr(R, 30, 29, KGATE); emit(R, LDRX(30, 30, 0)); emit(R, LDRX(29, 31, 0));   /* x29 restored: host walks it */
    emit(R, BLR(30));
    uint64_t ret = here(R);
    emit(R, LDP_POST(29, 30, 31, 16)); emit(R, B(R, here(R), x->a + 4));
    RwGate g = {ret, x->a, kind, arg}; PUSH(R->gates, R->ng, R->capg, g);
    return start;
}

/* ---- exclusive windows ---- */
static const char *const EXCL_LD[] = {"ldxr", "ldaxr", "ldxp", "ldaxp", "ldxrb", "ldaxrb", "ldxrh", "ldaxrh", NULL};
static const char *const EXCL_ST[] = {"stxr", "stlxr", "stxp", "stlxp", "stxrb", "stlxrb", "stxrh", "stlxrh", NULL};
static int is_ld(const char *m) { return in_list(m, EXCL_LD); }
static int is_st(const char *m) { return in_list(m, EXCL_ST); }

static int64_t lookup_last(const uint64_t *ka, const uint64_t *va, size_t n, uint64_t k, uint64_t *v) {
    for (size_t i = n; i-- > 0;) if (ka[i] == k) { *v = va[i]; return 1; }
    return 0;
}
static int is_branchy(const char *m) { return !strcmp(m, "b") || condbr(m); }

/* site_window_group(): an exclusive window plus its out-of-line LDXR retries moved together. */
static void site_window_group(Rw *R, const Group *g, uint64_t *heads, uint64_t *entries) {
    uint64_t used = 0; size_t total = 0;
    for (size_t s = 0; s < g->n; s++) { Seg *sg = &R->win[g->v[s]]; total += sg->n; for (size_t i = 0; i < sg->n; i++) used |= regs_in(R, &sg->v[i]); }
    int ST[2]; pick(R, used, 2, ST); int S = ST[0], T = ST[1];
    size_t *eb_off = xrealloc(R, NULL, g->n * sizeof *eb_off); uint64_t *eb_tgt = xrealloc(R, NULL, g->n * sizeof *eb_tgt);
    for (size_t s = 0; s < g->n; s++) {
        Seg *sg = &R->win[g->v[s]]; heads[s] = sg->v[0].a; entries[s] = here(R);
        emit(R, STP(S, T, 31, -16)); emit_host_tp(R, T); emit_slot_addr(R, T, S, K18); emit(R, LDRX(S, T, 0));
        eb_off[s] = R->np; eb_tgt[s] = sg->v[0].a; emit(R, NOP);
    }
    uint64_t *ca = xrealloc(R, NULL, total * sizeof *ca), *cc = xrealloc(R, NULL, total * sizeof *cc), cursor = here(R); size_t nc = 0;
    for (size_t s = 0; s < g->n; s++) { Seg *sg = &R->win[g->v[s]]; for (size_t i = 0; i < sg->n; i++) { ca[nc] = sg->v[i].a; cc[nc++] = cursor; cursor += 4; } cursor += 4; }
    uint64_t *ex = NULL, dummy; size_t nex = 0, capex = 0;
    for (size_t s = 0; s < g->n; s++) {
        Seg *sg = &R->win[g->v[s]]; uint64_t fall = sg->v[sg->n - 1].a + 4; PUSH(ex, nex, capex, fall);
        for (size_t i = 0; i < sg->n; i++) if (is_branchy(sg->v[i].mn)) {
            uint64_t t = (uint64_t)last_imm(R, &sg->v[i]); int seen = 0;
            for (size_t k = 0; k < nex; k++) if (ex[k] == t) seen = 1;
            if (!lookup_last(ca, cc, nc, t, &dummy) && !seen) PUSH(ex, nex, capex, t);
        }
    }
    size_t nu = 0; for (size_t k = 0; k < nex; k++) { int dup = 0; for (size_t j = 0; j < nu; j++) if (ex[j] == ex[k]) dup = 1; if (!dup) ex[nu++] = ex[k]; }
    uint64_t *exa = xrealloc(R, NULL, (nu ? nu : 1) * sizeof *exa);
    for (size_t k = 0; k < nu; k++) exa[k] = cursor + 16 * k;
    for (size_t s = 0; s < g->n; s++) { uint64_t to; if (!lookup_last(ca, cc, nc, eb_tgt[s], &to)) FAIL("window entry"); R->patch[eb_off[s]] = B(R, R->patch_off + 4 * eb_off[s], to); }
    for (size_t s = 0; s < g->n; s++) {
        Seg *sg = &R->win[g->v[s]];
        for (size_t i = 0; i < sg->n; i++) {
            Ins *x = &sg->v[i]; uint64_t at;
            if (!lookup_last(ca, cc, nc, x->a, &at) || here(R) != at) FAIL("window layout %llx", HX(x->a));
            uint32_t w = has18(x->op) ? subst(R, x, S) : x->w;
            if (is_branchy(x->mn)) {
                uint64_t t = (uint64_t)last_imm(R, x), dst; int found = lookup_last(ca, cc, nc, t, &dst);
                if (!found) for (size_t k = 0; k < nu; k++) if (ex[k] == t) { dst = exa[k]; found = 1; break; }
                if (!found) FAIL("window branch target %llx", HX(x->a));
                uint32_t dl = (uint32_t)((int64_t)(dst - here(R)) >> 2);
                if (!strcmp(x->mn, "b")) w = B(R, here(R), dst);
                else if (!strcmp(x->mn, "tbz") || !strcmp(x->mn, "tbnz")) w = (w & 0xFFF8001F) | (dl & 0x3FFF) << 5;
                else w = (w & 0xFF00001F) | (dl & 0x7FFFF) << 5;
                cs_insn *d = dis_into(R, R->d2, w, here(R));
                if (!d->detail->arm64.op_count || (uint64_t)d->detail->arm64.operands[d->detail->arm64.op_count - 1].imm != dst) FAIL("window branch %llx", HX(x->a));
            } else {
                static const char *const bad[] = {"adr", "adrp", "bl", "br", "blr", "ret", NULL};
                if (in_list(x->mn, bad) || (!strcmp(x->mn, "ldr") && !strchr(x->op, '['))) FAIL("window instruction %llx", HX(x->a));
            }
            emit(R, w);
        }
        uint64_t fall = sg->v[sg->n - 1].a + 4, to; size_t k;
        for (k = 0; k < nu && ex[k] != fall; k++) {}
        if (k == nu) FAIL("window exit");
        if (!lookup_last(ca, cc, nc, fall, &to)) to = exa[k];
        emit(R, B(R, here(R), to));
    }
    for (size_t k = 0; k < nu; k++) {
        if (here(R) != exa[k]) FAIL("window exits");
        emit(R, STRX(S, T)); emit(R, LDP(S, T, 31, -16)); emit(R, B(R, here(R), ex[k])); emit(R, NOP);
    }
    free(eb_off); free(eb_tgt); free(ca); free(cc); free(ex); free(exa);
}

static uint64_t site_window(Rw *R, const Seg *sg) {
    uint64_t used = 0; for (size_t i = 0; i < sg->n; i++) used |= regs_in(R, &sg->v[i]);
    int ST[2]; pick(R, used, 2, ST); int S = ST[0], T = ST[1];
    uint64_t lo = sg->v[0].a, hi = sg->v[sg->n - 1].a + 4, start = here(R);
    emit(R, STP(S, T, 31, -16)); emit_host_tp(R, T); emit_slot_addr(R, T, S, K18); emit(R, LDRX(S, T, 0));
    uint64_t copy0 = here(R), exits = copy0 + 4 * sg->n;
    /* The copied body falls through to the first exit stub, so that stub must be hi (TOTK main+0x72afdc, +0x7aa630). */
    uint64_t *ext = NULL; size_t next = 0, capx = 0; PUSH(ext, next, capx, hi);
    uint32_t *words = xrealloc(R, NULL, sg->n * sizeof *words);
    for (size_t k = 0; k < sg->n; k++) {
        const Ins *x = &sg->v[k]; uint32_t w = x->w;
        if (has18(x->op)) w = subst(R, x, S);
        if (condbr(x->mn)) {
            uint64_t t = (uint64_t)last_imm(R, x), nt;
            if (lo <= t && t < hi) nt = copy0 + (t - lo);
            else { PUSH(ext, next, capx, t); nt = exits + 16 * (next - 1); }
            uint64_t pc = copy0 + 4 * k; uint32_t off = (uint32_t)((int64_t)(nt - pc) >> 2);
            if (!strcmp(x->mn, "tbz") || !strcmp(x->mn, "tbnz")) w = (w & 0xFFF8001F) | (off & 0x3FFF) << 5;
            else w = (w & 0xFF00001F) | (off & 0x7FFFF) << 5;
            cs_insn *d = dis_into(R, R->d2, w, pc);
            if (!d->detail->arm64.op_count || (uint64_t)d->detail->arm64.operands[d->detail->arm64.op_count - 1].imm != nt || strcmp(d->mnemonic, x->mn)) FAIL("window branch %llx", HX(x->a));
        } else {
            static const char *const bad[] = {"adr", "adrp", "b", "bl", "br", "blr", "ret", NULL};
            if (in_list(x->mn, bad) || (!strcmp(x->mn, "ldr") && !strchr(x->op, '['))) FAIL("%#llx", HX(x->a));
        }
        words[k] = w;
    }
    for (size_t k = 0; k < sg->n; k++) emit(R, words[k]);
    for (size_t k = 0; k < next; k++) { emit(R, STRX(S, T)); emit(R, LDP(S, T, 31, -16)); emit(R, B(R, here(R), ext[k])); emit(R, NOP); }
    free(ext); free(words);
    return start;
}

/* call_window.py prepare_call(): replay the movable stack access, x30 = virtual x18, return to BLR x30 in place. */
static void prepare_call(Rw *R, uint32_t prefix_word, uint64_t prefix, uint64_t call, uint64_t st) {
    if (call != prefix + 4) FAIL("Invalid call window or scratch register");
    static const char *const mov[] = {"str", "stp", "stur", "ldr", "ldp", "ldur", NULL};
    if (!decode(R->hd, R->d2, prefix_word, prefix) || !in_list(R->d2->mnemonic, mov) || R->d2->detail->arm64.writeback)
        FAIL("Call prefix must be a non-writeback stack access: %#llx", HX(prefix));
    cs_arm64 *d = &R->d2->detail->arm64;
    if (!d->op_count) FAIL("Call prefix operands: %#llx", HX(prefix));
    arm64_op_mem m = d->operands[d->op_count - 1].mem; const char *base = cs_reg_name(R->hd, m.base);
    if (!base || (strcmp(base, "sp") && strcmp(base, "fp")) || m.index || (!strcmp(base, "sp") && m.disp < 0))
        FAIL("Call prefix must access the SP/FP stack frame: %#llx", HX(prefix));
    if (has18(R->d2->op_str)) FAIL("Call prefix also uses virtual x18: %#llx", HX(prefix));
    uint64_t adrp_pc = st + 4 * (2 + HOST_TP_LEN);
    int64_t pages = ((int64_t)(R->key & ~4095ull) - (int64_t)(adrp_pc & ~4095ull)) / 4096;
    if (pages < -(1 << 20) || pages >= (1 << 20)) FAIL("Key page out of ADRP range");
    emit(R, prefix_word); emit(R, STUR(0, 31, -16)); emit_host_tp(R, 30);
    emit(R, 0x90000000u | ((uint32_t)pages & 3) << 29 | ((uint32_t)(pages >> 2) & 0x7FFFF) << 5);
    emit(R, 0xF9400000u | (uint32_t)((R->key & 4095) / 8 + K18) << 10);
    emit(R, 0x8B000000u | 30 << 5 | 30); emit(R, 0xF9400000u | 30 << 5 | 30); emit(R, LDUR(0, 31, -16));
    emit(R, B(R, here(R), call));
}

/* ---- lookups ---- */
static Kind *kind_of(Rw *R, uint64_t a) {
    size_t lo = 0, hi = R->nk; while (lo < hi) { size_t m = (lo + hi) / 2; if (R->kinds[m].a < a) lo = m + 1; else hi = m; }
    return lo < R->nk && R->kinds[lo].a == a ? &R->kinds[lo] : NULL;
}
static int is_done(Rw *R, uint64_t a) { return a < R->text_end && a % 4 == 0 && (R->done[a / 32] >> (a / 4 % 8) & 1); }
static void set_done(Rw *R, uint64_t a) { if (a >= R->text_end || a % 4) FAIL("site outside text %llx", HX(a)); R->done[a / 32] |= (uint8_t)(1u << (a / 4 % 8)); }
static void func_of(Rw *R, uint64_t a, uint64_t *fa, uint64_t *fb) {
    size_t lo = 0, hi = R->ns; while (lo < hi) { size_t m = (lo + hi) / 2; if ((int64_t)a < R->starts[m]) hi = m; else lo = m + 1; }
    if (lo == 0) { *fa = a; *fb = a + 4; return; }
    *fa = (uint64_t)R->starts[lo - 1]; *fb = lo < R->ns ? (uint64_t)R->starts[lo] : R->text_end;
}
static int cmp_i64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static int cmp_call(const void *a, const void *b) { uint64_t x = ((const Call *)a)->prefix, y = ((const Call *)b)->prefix; return x < y ? -1 : x > y; }

/* ldrsw_table(): a table of signed 32-bit offsets from its own base (older clang); see patch_nso.py. */
static const char *const FLAG_SETTERS[] = {"cmp", "cmn", "tst", "adds", "subs", "ands", "bics", "adcs", "sbcs", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "fccmpe", "negs", "ngcs", NULL};
static int writes(const Ins *p, int r) {
    static const char *const no[] = {"str", "stur", "stp", "strb", "strh", "cmp", "cmn", "tst", "b", "bl", "br", "blr", "ret", "cbz", "cbnz", "tbz", "tbnz", NULL};
    if (r < 0 || in_list(p->mn, no) || starts(p->mn, "b.")) return 0;
    Ops o; ops(p, &o); char x[16], w[16]; snprintf(x, sizeof x, "x%d", r); snprintf(w, sizeof w, "w%d", r);
    return !strcmp(o.t[0], x) || !strcmp(o.t[0], w);
}
static int ldrsw_table(Rw *R, const Ins *insl, size_t j, uint64_t fa, uint64_t fb, uint64_t *base, uint64_t *width) {
    const Ins *ctx = insl + (j > 48 ? j - 48 : 0); long n = (long)(j - (j > 48 ? j - 48 : 0)), k = n - 1, k2;
    Ops ok, o; char xT[16];
    if (k < 0 || strcmp(ctx[k].mn, "add")) return 0;
    ops(&ctx[k], &ok); if (ok.n != 3 || strcmp(ok.t[0], "x18")) return 0;
    const char *a = ok.t[1], *b = ok.t[2];
    for (k2 = k - 1; k2 >= 0; k2--) {
        const Ins *p = &ctx[k2];
        if (!strcmp(p->mn, "ldrsw") && strstr(p->op, "lsl #2")) {
            ops(p, &o);
            if (o.n == 4 && strcmp(o.t[0], o.t[1]) && ((!strcmp(o.t[0], a) && !strcmp(o.t[1], b)) || (!strcmp(o.t[0], b) && !strcmp(o.t[1], a)))) break;
        }
        if (writes(p, num(a)) || writes(p, num(b))) return 0;
    }
    if (k2 < 0) return 0;
    int E = num(o.t[0]), T = num(o.t[1]), I = num(o.t[2]);
    if (E < 0 || T < 0 || I < 0) return 0;
    for (long q = k2 + 1; q < k; q++) if (writes(&ctx[q], T) || writes(&ctx[q], I)) return 0;
    long k3 = k2 - 1; while (k3 >= 0 && !writes(&ctx[k3], T)) k3--;
    snprintf(xT, sizeof xT, "x%d", T);
    if (k3 < 1 || strcmp(ctx[k3].mn, "add")) return 0;
    Ops o3; ops(&ctx[k3], &o3);
    if (o3.n < 2 || strcmp(o3.t[0], xT) || strcmp(o3.t[1], xT)) return 0;
    if (o3.n < 3) FAIL("list index out of range at %llx", HX(ctx[k3].a));
    if (o3.t[2][0] != '#') return 0;
    int64_t lo = must_int(R, o3.t[2] + 1);
    long k4 = k3 - 1; while (k4 >= 0 && !writes(&ctx[k4], T)) k4--;
    if (k4 < 0 || strcmp(ctx[k4].mn, "adrp")) return 0;
    Ops o4; ops(&ctx[k4], &o4); if (strcmp(o4.t[0], xT)) return 0;
    int64_t table = imm_of(R, &ctx[k4], 1) + lo;
    int src = I; long k5 = k2 - 1;
    while (k5 >= 0 && strcmp(ctx[k5].mn, "b.hi") && strcmp(ctx[k5].mn, "b.hs")) {
        const Ins *p = &ctx[k5]; Ops op5; ops(p, &op5);
        if (in_list(p->mn, FLAG_SETTERS) || !strcmp(p->mn, "b") || !strcmp(p->mn, "br") || !strcmp(p->mn, "ret") || starts(p->mn, "b.")) return 0;
        if (!strcmp(p->mn, "mov") && op5.n == 2 && num(op5.t[0]) == src && num(op5.t[1]) >= 0) src = num(op5.t[1]);
        else if (writes(p, src)) return 0;
        k5--;
    }
    if (k5 < 1) return 0;
    long k6 = k5 - 1;
    while (k6 >= 0 && !in_list(ctx[k6].mn, FLAG_SETTERS)) { if (writes(&ctx[k6], src) || ctx[k6].mn[0] == 'b') return 0; k6--; }
    if (k6 < 0 || strcmp(ctx[k6].mn, "cmp")) return 0;
    Ops o6; ops(&ctx[k6], &o6);
    if (o6.n != 2 || num(o6.t[0]) != src || o6.t[1][0] != '#') return 0;
    int64_t cnt = must_int(R, o6.t[1] + 1) + (!strcmp(ctx[k5].mn, "b.hi") ? 1 : 0);
    if (cnt <= 0 || cnt > 4096) return 0;
    if (table < 0 || (uint64_t)table + 4 * (uint64_t)cnt > R->len) FAIL("ldrsw table outside image at %llx", HX(insl[j].a));
    int64_t mn = INT64_MAX, mx = INT64_MIN;
    for (int64_t i = 0; i < cnt; i++) {
        int64_t t = table + (int32_t)rd32(R->img + table + 4 * i);
        if (t < (int64_t)fa || t >= (int64_t)fb || t % 4) FAIL("ldrsw table %llx: target outside its function", HX(insl[j].a));
        mn = t < mn ? t : mn; mx = t > mx ? t : mx;
    }
    *base = (uint64_t)mn; *width = (uint64_t)(mx - mn) / 4 + 1; return 1;
}

/* ---- per-function analysis (windows, jump tables, calls) ---- */
typedef struct { size_t lo, hi; } Rng;                  /* inclusive instruction indices in L */
/* L / N: md.disasm(img[fa:fb], fa), disassembled ahead by the worker pool (disasm_function). */
static void analyse_function(Rw *R, uint64_t fa, uint64_t fb, const char *build, Ins *L, size_t N) {
    if (fb > R->len || fa > fb) FAIL("function outside image %llx", HX(fa));
    Rng *ws = NULL; size_t nws = 0, capws = 0;
    for (size_t i = 0; i < N; i++) {                    /* find_windows */
        if (!is_ld(L[i].mn)) continue;
        size_t j = i + 1;
        while (j < N && !is_st(L[j].mn) && !is_ld(L[j].mn)) j++;
        if (j >= N || j - i >= 16 || !is_st(L[j].mn)) continue;
        if (j + 1 < N && (!strcmp(L[j + 1].mn, "cbz") || !strcmp(L[j + 1].mn, "cbnz"))) j++;
        int any = 0; for (size_t k = i; k <= j; k++) if (has18(L[k].op)) { any = 1; break; }
        if (any) { Rng r = {i, j}; PUSH(ws, nws, capws, r); }
    }
    /* linked_windows: an out-of-line LDXR retry moves with the STXR it re-enters (no stores between LDXR and STXR). */
    size_t n0 = nws; uint64_t (*edges)[2] = NULL; size_t ned = 0, caped = 0;
    size_t *gown = NULL, ngown = 0, capgown = 0; Group *lg = NULL; size_t nlg = 0, caplg = 0;
    for (size_t j = 0; j < N; j++) {
        if (!condbr(L[j].mn)) continue;
        uint64_t t = (uint64_t)last_imm(R, &L[j]); long own = -1;
        for (size_t s = 0; s < n0; s++) if (L[ws[s].lo].a < t && t <= L[ws[s].hi].a) { own = (long)s; break; }
        if (own < 0 || (L[ws[own].lo].a <= L[j].a && L[j].a <= L[ws[own].hi].a)) continue;
        long k = (long)j, floor_ = j > 15 ? (long)j - 15 : 0;
        while (k >= floor_ && !is_ld(L[k].mn)) k--;
        if (k < floor_) FAIL("exclusive alternate entry lacks LDXR: %llx", HX(L[j].a));
        for (size_t q = (size_t)k; q < j; q++) {
            const char *m = L[q].mn;
            if (is_st(m) || starts(m, "st") || starts(m, "bl") || !strcmp(m, "b") || !strcmp(m, "br") || !strcmp(m, "ret") || starts(m, "b."))
                FAIL("unsafe exclusive retry: %llx", HX(L[j].a));
        }
        for (size_t q = (size_t)k; q <= j; q++) for (size_t s = 0; s < n0; s++)
            if (L[ws[s].lo].a <= L[q].a && L[q].a <= L[ws[s].hi].a) FAIL("overlapping exclusive retry: %llx", HX(L[j].a));
        size_t gi; for (gi = 0; gi < ngown && gown[gi] != (size_t)own; gi++) {}
        if (gi == ngown) { PUSH(gown, ngown, capgown, (size_t)own); Group g = {NULL, 0}; size_t cap = 0; PUSH(g.v, g.n, cap, (size_t)own); PUSH(lg, nlg, caplg, g); }
        int have = 0; for (size_t s = 0; s < lg[gi].n; s++) if (L[ws[lg[gi].v[s]].lo].a == L[k].a) have = 1;
        if (!have) { Rng r = {(size_t)k, j}; PUSH(ws, nws, capws, r); size_t cap = lg[gi].n; /* exact size: grow by one */
            lg[gi].v = xrealloc(R, lg[gi].v, (cap + 1) * sizeof *lg[gi].v); lg[gi].v[lg[gi].n++] = nws - 1; }
        uint64_t e[2] = {L[j].a, t}; if (ned == caped) { caped = caped ? 2 * caped : 16; edges = xrealloc(R, edges, caped * sizeof *edges); } memcpy(edges[ned++], e, sizeof e);
    }
    size_t wbase = R->nw;
    for (size_t s = 0; s < nws; s++) {                  /* windows.append + done */
        Seg sg = {xrealloc(R, NULL, (ws[s].hi - ws[s].lo + 1) * sizeof(Ins)), ws[s].hi - ws[s].lo + 1};
        memcpy(sg.v, L + ws[s].lo, sg.n * sizeof(Ins));
        PUSH(R->win, R->nw, R->capw, sg);
        for (size_t k = ws[s].lo; k <= ws[s].hi; k++) set_done(R, L[k].a);
    }
    for (size_t g = 0; g < nlg; g++) { for (size_t s = 0; s < lg[g].n; s++) lg[g].v[s] += wbase; PUSH(R->groups, R->ngr, R->capgr, lg[g]); }
    free(lg); free(gown);
    for (size_t j = 0; j < N; j++) {
        if (!strcmp(L[j].mn, "br") && !strcmp(L[j].op, "x18")) {
            uint64_t base = 0, width = 0; int hb = 0; size_t c0 = j > 32 ? j - 32 : 0;   /* arithmetic may follow the table setup */
            for (size_t q = j; q-- > c0;) if (!strcmp(L[q].mn, "adr") && starts(L[q].op, "x18,")) { base = (uint64_t)imm_of(R, &L[q], 1); hb = 1; break; }
            for (size_t q = j; q-- > c0;) if (!strcmp(L[q].mn, "add") && starts(L[q].op, "x18, x18,") && strstr(L[q].op, "lsl #2")) {
                int idx = num(cs_reg_name(R->hd, opnd(R, &L[q], 2)->reg));
                for (size_t u = j; u-- > c0;) {
                    if ((!strcmp(L[u].mn, "ldrb") || !strcmp(L[u].mn, "ldrsb")) && num(cs_reg_name(R->hd, opnd(R, &L[u], 0)->reg)) == idx) width = 256;
                    if ((!strcmp(L[u].mn, "ldrh") || !strcmp(L[u].mn, "ldrsh")) && num(cs_reg_name(R->hd, opnd(R, &L[u], 0)->reg)) == idx) width = 65536;
                    if (width) break;
                }
                break;
            }
            if (!width && !strcmp(build, SIGNED_BUILD)) for (size_t t = 0; t < sizeof SIGNED / sizeof *SIGNED; t++) if (SIGNED[t].br == L[j].a) {
                char hex[65]; RwSha256 s; if (L[j].a < 16) FAIL("signed-table dispatch %llx", HX(L[j].a));
                rw_sha256_init(&s); rw_sha256_update(&s, R->img + L[j].a - 16, 20); rw_sha256_hex(&s, hex);
                if (strcmp(hex, SIGNED[t].dispatch_sha256)) FAIL("signed-table dispatch %llx", HX(L[j].a));
                if (SIGNED[t].table + 4 * (uint64_t)SIGNED[t].n > R->len) FAIL("signed-table entries %llx", HX(L[j].a));
                rw_sha256_init(&s); rw_sha256_update(&s, R->img + SIGNED[t].table, 4 * (size_t)SIGNED[t].n); rw_sha256_hex(&s, hex);
                if (strcmp(hex, SIGNED[t].table_sha256)) FAIL("signed-table entries %llx", HX(L[j].a));
                int64_t mn = INT64_MAX, mx = INT64_MIN;
                for (int e = 0; e < SIGNED[t].n; e++) {
                    int64_t tg = (int64_t)SIGNED[t].adr + (int32_t)rd32(R->img + SIGNED[t].table + 4 * e);
                    if (tg < 0 || tg >= (int64_t)R->text_end || tg % 4) FAIL("signed-table target %llx", HX(L[j].a));
                    mn = tg < mn ? tg : mn; mx = tg > mx ? tg : mx;
                }
                base = (uint64_t)mn; width = (uint64_t)(mx - mn) / 4 + 1; hb = 1;
            }
            if (!width && ldrsw_table(R, L, j, fa, fb, &base, &width)) hb = 1;
            if (!hb || !base || !width) FAIL("jump table %llx", HX(L[j].a));
            Jump jp = {L[j].a, base, width}; PUSH(R->jumps, R->nj, R->capj, jp);
        }
        if (!strcmp(L[j].mn, "blr") && !strcmp(L[j].op, "x18")) {
            if (j == 0) FAIL("call without prefix %llx", HX(L[j].a));
            if (L[j - 1].a + 4 != L[j].a) FAIL("call prefix %llx", HX(L[j].a));
            size_t q; for (q = 0; q < R->nc && R->calls[q].prefix != L[j - 1].a; q++) {}
            Call c = {L[j - 1].a, L[j].a, L[j - 1].w};
            if (q < R->nc) R->calls[q] = c; else PUSH(R->calls, R->nc, R->capc, c);
        }
    }
    for (size_t j = 0; j < N; j++) {                    /* no branch may land inside a window except its head */
        const char *m = L[j].mn;
        if (!(!strcmp(m, "b") || !strcmp(m, "bl") || condbr(m))) continue;
        uint64_t t = (uint64_t)last_imm(R, &L[j]); int in = 0, head = 0, edge = 0;
        for (size_t s = 0; s < nws; s++) { if (L[ws[s].lo].a == t) head = 1; for (size_t k = ws[s].lo; k <= ws[s].hi; k++) if (L[k].a == t) in = 1; }
        for (size_t e = 0; e < ned; e++) if (edges[e][0] == L[j].a && edges[e][1] == t) edge = 1;
        if (in && !head && !edge) FAIL("branch into window %llx", HX(L[j].a));
    }
    free(ws); free(edges);
}

static void read_tags(Rw *R, uint64_t m0, RwTags *t) {
    if (m0 + 8 > R->len) FAIL("MOD0 outside image");
    uint64_t dyn = m0 + (uint64_t)(int64_t)(int32_t)rd32(R->img + m0 + 4); size_t cap = 0;
    for (;;) {
        if (dyn + 16 > R->len) FAIL("unterminated dynamic table");
        RwTag e = {(int64_t)rd64(R->img + dyn), rd64(R->img + dyn + 8)}; dyn += 16;
        if (!e.tag) break;
        PUSH(t->v, t->n, cap, e);
    }
}
static uint64_t need_tag(Rw *R, const RwTags *t, int64_t tag) { uint64_t v; if (!rw_tag(t, tag, &v)) FAIL("dynamic tag %lld missing", (long long)tag); return v; }

static void check_calls(Rw *R) {
    if (!R->nc) return;
    for (size_t i = 0; i < R->nc; i++) for (size_t s = 0; s < R->ns; s++)
        if ((int64_t)R->calls[i].call == R->starts[s]) FAIL("function entry into call-window second word");
    for (uint64_t a = 0; a < R->text_end; a += 4) {
        uint32_t w = rd32(R->img + a); int64_t t; int has = 1;
        if ((w & 0x7C000000) == 0x14000000) t = (int64_t)a + 4 * (int64_t)(((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000);
        else if ((w & 0xFF000010) == 0x54000000 || (w & 0x7E000000) == 0x34000000) t = (int64_t)a + 4 * ((int64_t)((w >> 5 & 0x7FFFF) ^ 0x40000) - 0x40000);
        else if ((w & 0x7E000000) == 0x36000000) t = (int64_t)a + 4 * ((int64_t)((w >> 5 & 0x3FFF) ^ 0x2000) - 0x2000);
        else has = 0;
        if (has) for (size_t i = 0; i < R->nc; i++) if ((int64_t)R->calls[i].call == t) FAIL("branch into call without preparation %llx", HX(a));
    }
    for (size_t i = 0; i < R->nc; i++)
        if (kind_of(R, R->calls[i].prefix) || is_done(R, R->calls[i].prefix) || is_done(R, R->calls[i].call)) FAIL("overlapping call window %llx", HX(R->calls[i].call));
    uint64_t m0 = rd32(R->img + 4); read_tags(R, m0, &R->tags);
    uint64_t hash = need_tag(R, &R->tags, 4), symtab = need_tag(R, &R->tags, 6); size_t nrl = 0;
    if (rw_decode_relr(R->img, R->len, &R->tags, R->n.data_off, R->key, &R->relr, &nrl, R->err, R->errlen)) longjmp(R->jb, 1);
    for (size_t i = 0; i < nrl; i++) for (size_t c = 0; c < R->nc; c++)
        if (R->relr[i].addend == R->calls[c].call) FAIL("RELR pointer enters unprepared call: %llx", HX(R->relr[i].where));
    if (hash + 8 > R->len) FAIL("DT_HASH outside image");
    uint32_t count = rd32(R->img + hash + 4);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t o = symtab + 24ull * i; if (o + 24 > R->len) FAIL("dynamic symbols outside image");
        uint16_t section = (uint16_t)(rd32(R->img + o + 4) >> 16); uint64_t value = rd64(R->img + o + 8);
        for (size_t c = 0; c < R->nc; c++) if (section && value == R->calls[c].call) FAIL("dynamic symbol enters unprepared call");
    }
    static const int64_t tabs[2][2] = {{7, 8}, {23, 2}};
    for (int k = 0; k < 2; k++) {
        uint64_t table = 0, size = 0; int ht = rw_tag(&R->tags, tabs[k][0], &table); rw_tag(&R->tags, tabs[k][1], &size);
        if (!ht || !table) continue;
        for (uint64_t o = table; o < table + size; o += 24) {
            if (o + 24 > R->len) FAIL("relocations outside image");
            uint64_t info = rd64(R->img + o + 8), addend = rd64(R->img + o + 16);
            if ((info & 0xFFFFFFFF) == 1027) for (size_t c = 0; c < R->nc; c++) if (addend == R->calls[c].call) FAIL("relative pointer enters unprepared call");
        }
    }
}

/* linear scan for every rewrite point (not relying on register read/write reports): one SCAN_CHUNK per item */
static int classify(csh h, cs_insn *c, uint32_t w, uint64_t a, Kind *kd) {
    *kd = (Kind){a, 0, 0};
    if ((w & 0xFFE0001F) == 0xD4000001) { kd->kind = K_SVC; kd->arg = w >> 5 & 0xFFFF; }
    else if ((w & 0xFFFFFFE0) == 0xD53BD040) kd->kind = K_TPIDR_R;
    else if ((w & 0xFFFFFFE0) == 0xD51BD040) kd->kind = K_TPIDR_W;
    else if ((w & 0xFFFFFFE0) == 0xD53BD060) kd->kind = K_TPIDRRO_R;
    else if ((w & 0xFFFFFFE0) == 0xD53BE020) { kd->kind = K_CNTPCT; kd->arg = w & 31; }
    else if ((w & 0xFFFFFFE0) == 0xD53BE040) { kd->kind = K_CNTVCT; kd->arg = w & 31; }
    else if (decode(h, c, w, a) && has18(c->op_str)) kd->kind = K_X18;
    else return 0;
    return 1;
}
static void scan_item(Par *P, int tid, size_t item) {
    Rw *R = P->R;
    uint64_t a0 = (uint64_t)item * SCAN_CHUNK, a1 = a0 + SCAN_CHUNK < R->text_end ? a0 + SCAN_CHUNK : R->text_end;
    Kind *v = NULL; size_t n = 0, cap = 0;
    for (uint64_t a = a0; a < a1; a += 4) {
        Kind kd;
        if (!classify(R->th[tid], R->tins[tid], rd32(R->img + a), a, &kd)) continue;
        if (n == cap) {
            Kind *g = realloc(v, (cap = cap ? 2 * cap : 256) * sizeof *v);
            if (!g) { free(v); atomic_store(&P->oom, 1); atomic_store(&P->stop, 1); return; }
            v = g;
        }
        v[n++] = kd;
    }
    R->scan_v[item] = v; R->scan_n[item] = n;
    atomic_fetch_add(&P->done, (a1 - a0) / 4);
}
typedef struct Fn { uint64_t a, b; } Fn;
typedef struct { const Fn *fns; size_t first; } DecodeBatch;   /* items are functions first, first + 1, ... */
static void decode_item(Par *P, int tid, size_t item) {
    Rw *R = P->R; const DecodeBatch *d = P->ctx; size_t i = d->first + item; const Fn *f = &d->fns[i];
    if (f->b > R->len || f->a > f->b) return;   /* analyse_function reports it, in order */
    if (disasm_function(R->th[tid], R->tins[tid], R->img, f->a, f->b, &R->pre[i], &R->npre[i])) { atomic_store(&P->oom, 1); atomic_store(&P->stop, 1); return; }
    atomic_fetch_add(&P->done, (f->b - f->a) / 4);
}
static void run(Rw *R, const char *name) {
    RwNso *n = &R->n; R->img = n->img; R->len = n->len; R->text_end = n->text_end; R->key = n->key_page_off; R->patch_off = n->patch_off;
    size_t k; for (k = 0; k < 3 && strcmp(KNOWN[k].name, name); k++) {}
    if (k == 3) FAIL("unknown module name %s", name);
    if (strcmp(KNOWN[k].build_id, n->build_id)) FAIL("%s is not the Tears of the Kingdom 1.4.3 module (build id %s)", name, n->build_id);
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &R->hd) != CS_ERR_OK) FAIL("capstone unavailable");
    cs_option(R->hd, CS_OPT_DETAIL, CS_OPT_ON);
    if (!(R->d1 = cs_malloc(R->hd)) || !(R->d2 = cs_malloc(R->hd))) FAIL("out of memory");
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int want = cpus < 1 ? 1 : cpus > RW_THREADS ? RW_THREADS : (int)cpus;
    for (; R->nth < want; R->nth++) {   /* opened here, on one thread: cs_open is not documented as thread-safe */
        if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &R->th[R->nth]) != CS_ERR_OK) { if (!R->nth) FAIL("capstone unavailable"); break; }
        if (!(R->tins[R->nth] = cs_malloc(R->th[R->nth]))) { cs_close(&R->th[R->nth]); if (!R->nth) FAIL("out of memory"); break; }
    }
    if (R->text_end % 4 || R->text_end > R->len) FAIL("bad text segment");
    R->done = xrealloc(R, NULL, R->text_end / 32 + 1); memset(R->done, 0, R->text_end / 32 + 1);
    /* function boundaries from eh_frame_hdr */
    uint64_t m0 = rd32(R->img + 4);
    if (m0 + 20 > R->len || memcmp(R->img + m0, "MOD0", 4)) FAIL("no MOD0 header");
    uint64_t ehs = m0 + (uint64_t)(int64_t)(int32_t)rd32(R->img + m0 + 16);
    if (ehs + 4 > R->len) FAIL("eh_frame_hdr outside image");
    uint8_t penc = R->img[ehs + 1], cenc = R->img[ehs + 2], tenc = R->img[ehs + 3]; uint64_t o = ehs + 4;
#define RD(enc, out) do { int f_ = (enc) & 0xF, a_ = (enc) & 0x70; int64_t v_; \
        if (f_ == 3 || f_ == 0xB) { if (o + 4 > R->len) FAIL("eh_frame_hdr"); v_ = f_ == 3 ? (int64_t)rd32(R->img + o) : (int64_t)(int32_t)rd32(R->img + o); if (a_ == 0x10) v_ += (int64_t)o; if (a_ == 0x30) v_ += (int64_t)ehs; o += 4; } \
        else if (f_ == 4) { if (o + 8 > R->len) FAIL("eh_frame_hdr"); v_ = (int64_t)rd64(R->img + o); if (a_ == 0x10) v_ += (int64_t)o; if (a_ == 0x30) v_ += (int64_t)ehs; o += 8; } \
        else { FAIL("eh_frame_hdr encoding %#x", (unsigned)(enc)); } (out) = v_; } while (0)
    int64_t tmp, nfde; RD(penc, tmp); RD(cenc, nfde); (void)tmp;
    if ((uint64_t)nfde > R->len && nfde > 0) FAIL("eh_frame_hdr count");
    for (int64_t i = 0; i < nfde; i++) { int64_t a, b; RD(tenc, a); RD(tenc, b); (void)b; if (a < (int64_t)R->text_end) PUSH(R->starts, R->ns, R->caps, a); }
#undef RD
    qsort(R->starts, R->ns, sizeof *R->starts, cmp_i64);
    { size_t u = 0; for (size_t i = 0; i < R->ns; i++) if (!u || R->starts[u - 1] != R->starts[i]) R->starts[u++] = R->starts[i]; R->ns = u; }
    /* linear scan for every rewrite point (not relying on register read/write reports), in parallel chunks joined in
       address order */
    uint64_t words = R->text_end / 4, total = 2 * words;
    poll(R, 0, total);
    R->nscan = (size_t)((R->text_end + SCAN_CHUNK - 1) / SCAN_CHUNK);
    R->scan_v = xrealloc(R, NULL, (R->nscan ? R->nscan : 1) * sizeof *R->scan_v); memset(R->scan_v, 0, (R->nscan ? R->nscan : 1) * sizeof *R->scan_v);
    R->scan_n = xrealloc(R, NULL, (R->nscan ? R->nscan : 1) * sizeof *R->scan_n); memset(R->scan_n, 0, (R->nscan ? R->nscan : 1) * sizeof *R->scan_n);
    { Par P = {.R = R, .n = R->nscan, .fn = scan_item, .total = total}; par_run(R, &P); }
    for (size_t c = 0; c < R->nscan; c++) R->capk += R->scan_n[c];
    R->kinds = xrealloc(R, NULL, (R->capk ? R->capk : 1) * sizeof *R->kinds);
    for (size_t c = 0; c < R->nscan; c++) {
        if (R->scan_n[c]) memcpy(R->kinds + R->nk, R->scan_v[c], R->scan_n[c] * sizeof *R->kinds);
        R->nk += R->scan_n[c]; free(R->scan_v[c]); R->scan_v[c] = NULL;
    }
    /* functions holding x18 sites, in address order */
    size_t capfn = 0;
    uint64_t pfa = UINT64_MAX, pfb = UINT64_MAX;
    for (size_t i = 0; i < R->nk; i++) {
        if (R->kinds[i].kind != K_X18) continue;
        uint64_t fa, fb; func_of(R, R->kinds[i].a, &fa, &fb);
        if (fa == pfa && fb == pfb) continue;
        pfa = fa; pfb = fb;
        Fn f = {fa, fb}; PUSH(R->fns, R->nfn, capfn, f);
    }
    Fn *fns = R->fns; size_t nf1 = R->nfn ? R->nfn : 1;
    R->pre = xrealloc(R, NULL, nf1 * sizeof *R->pre); memset(R->pre, 0, nf1 * sizeof *R->pre);
    R->npre = xrealloc(R, NULL, nf1 * sizeof *R->npre); memset(R->npre, 0, nf1 * sizeof *R->npre);
    /* batches of about DECODE_BATCH bytes: disassembled in parallel, then analysed one function at a time in order */
    for (size_t b0 = 0, b1; b0 < R->nfn; b0 = b1) {
        uint64_t bytes = 0;
        for (b1 = b0; b1 < R->nfn; b1++) {
            uint64_t span = fns[b1].b <= R->len && fns[b1].a <= fns[b1].b ? fns[b1].b - fns[b1].a : 0;
            if (b1 > b0 && bytes + span > DECODE_BATCH) break;
            bytes += span;
        }
        DecodeBatch d = {fns, b0};
        Par P = {.R = R, .n = b1 - b0, .fn = decode_item, .ctx = &d, .base = words + fns[b0].a / 4, .total = total};
        par_run(R, &P);
        for (size_t i = b0; i < b1; i++) {
            poll(R, words + fns[i].a / 4, total);
            analyse_function(R, fns[i].a, fns[i].b, n->build_id, R->pre[i], R->npre[i]);
            free(R->pre[i]); R->pre[i] = NULL;
        }
    }
    check_calls(R);
    /* emission, in patch_nso.py's order: window groups, single windows, call windows, then every other site */
    uint64_t *gh = NULL; size_t ngh = 0, capgh = 0;
    for (size_t g = 0; g < R->ngr; g++) {
        uint64_t *heads = xrealloc(R, NULL, R->groups[g].n * sizeof *heads), *ent = xrealloc(R, NULL, R->groups[g].n * sizeof *ent);
        site_window_group(R, &R->groups[g], heads, ent);
        for (size_t s = 0; s < R->groups[g].n; s++) { PUSH(gh, ngh, capgh, heads[s]); put_branch(R, heads[s], ent[s]); }
        free(heads); free(ent);
    }
    for (size_t s = 0; s < R->nw; s++) {
        int grouped = 0; for (size_t q = 0; q < ngh; q++) if (gh[q] == R->win[s].v[0].a) { grouped = 1; break; }
        if (grouped) continue;
        uint64_t st = site_window(R, &R->win[s]); put_branch(R, R->win[s].v[0].a, st);
    }
    free(gh);
    if (R->nc) qsort(R->calls, R->nc, sizeof *R->calls, cmp_call);
    for (size_t c = 0; c < R->nc; c++) {
        uint64_t st = here(R); prepare_call(R, R->calls[c].word, R->calls[c].prefix, R->calls[c].call, st);
        put_branch(R, R->calls[c].prefix, st);
        if (rd32(R->img + R->calls[c].call) != BLR_X18) FAIL("call window %llx", HX(R->calls[c].call));
        wr32(R->img + R->calls[c].call, BLR_X30);
        set_done(R, R->calls[c].prefix); set_done(R, R->calls[c].call);
    }
    for (size_t i = 0; i < R->nk; i++) {
        Kind *kd = &R->kinds[i]; if (is_done(R, kd->a)) continue;
        Ins x = ins_at(R, rd32(R->img + kd->a), kd->a); uint64_t st;
        switch (kd->kind) {
        case K_X18:
            if (!strcmp(x.mn, "br")) {
                size_t q; for (q = 0; q < R->nj && R->jumps[q].br != kd->a; q++) {}
                if (q == R->nj) FAIL("jump table missing %llx", HX(kd->a));
                st = site_br(R, &x, R->jumps[q].base, R->jumps[q].width);
            } else if (!strcmp(x.mn, "cbz") || !strcmp(x.mn, "cbnz") || !strcmp(x.mn, "tbz") || !strcmp(x.mn, "tbnz")) st = site_cond(R, &x);
            else if (!strcmp(x.mn, "adr") || !strcmp(x.mn, "adrp")) st = site_pcrel(R, &x);
            else if (!strcmp(x.mn, "blr") || !strcmp(x.mn, "ret") || !strcmp(x.mn, "b") || !strcmp(x.mn, "bl") || (!strcmp(x.mn, "ldr") && !strchr(x.op, '[')))
                FAIL("%llx %s %s", HX(kd->a), x.mn, x.op);
            else st = site_plain(R, &x);
            break;
        case K_TPIDR_R: st = site_sysreg_read(R, &x, KTLS); break;
        case K_TPIDRRO_R: st = site_sysreg_read(R, &x, KTLR); break;
        case K_TPIDR_W: st = site_tpidr_write(R, &x); break;
        case K_SVC: st = site_gate(R, &x, RW_GATE_SVC, kd->arg); break;
        case K_CNTPCT: st = site_gate(R, &x, RW_GATE_CNTPCT, kd->arg); break;
        default: st = site_gate(R, &x, RW_GATE_CNTVCT, kd->arg); break;
        }
        put_branch(R, kd->a, st);
    }
}

static void tsvs(Rw *R, RwModule *m) {
    static const char *const gk[] = {"svc", "cntpct", "cntvct"};
    RwBuf *t = m->tsv; int e = 0;
    e |= rw_buf_printf(&t[0], "# PATCH_OFF\t0x%llx\n# KEY_PAGE_OFF\t0x%llx\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x%llx\n# RO_OFF\t0x%llx\n# DATA_OFF\t0x%llx\n",
                       HX(m->l.patch_off), HX(m->l.key_page_off), HX(m->l.text_end), HX(m->l.ro_off), HX(m->l.data_off));
    for (int k = 0; k < 8; k++) e |= rw_buf_printf(&t[0], "# KEY_%s_IDX\t%d\n", KEY_NAMES[k], k);
    for (size_t i = 0; i < R->ng; i++) e |= rw_buf_printf(&t[1], "0x%llx\t%s\t%u\t0x%llx\n", HX(R->gates[i].ret), gk[R->gates[i].kind], R->gates[i].arg, HX(R->gates[i].original));
    for (size_t i = 0; i < R->nk; i++) if (!is_done(R, R->kinds[i].a)) e |= rw_buf_printf(&t[2], "0x%llx\t%s\n", HX(R->kinds[i].a), KIND[R->kinds[i].kind]);
    for (size_t s = 0; s < R->nw; s++) e |= rw_buf_printf(&t[2], "0x%llx\tW\t0x%llx\n", HX(R->win[s].v[0].a), HX(R->win[s].v[R->win[s].n - 1].a + 4));
    for (size_t c = 0; c < R->nc; c++) e |= rw_buf_printf(&t[2], "0x%llx\tC\t0x%llx\n", HX(R->calls[c].prefix), HX(R->calls[c].call + 4));
    for (size_t j = 0; j < R->nj; j++) e |= rw_buf_printf(&t[2], "0x%llx\tJ\t0x%llx\t%llu\n", HX(R->jumps[j].br), HX(R->jumps[j].base), HX(R->jumps[j].width));
    for (int k = 3; k < 6; k++) e |= rw_buf_printf(&t[k], "# BUILD_ID\t%s\n", m->l.build_id);
    for (int k = 0; k < 6; k++) if (!t[k].p) e |= rw_buf_printf(&t[k], "%s", "");
    if (e) FAIL("out of memory");
}

int rw_rewrite_nso(const char *nso_path, const char *name, RwModule *out, uint8_t nso_header[0x100], RwPoll pollfn, void *ud, char *err, size_t errlen) {
    Rw *R = calloc(1, sizeof *R); int rc = RW_FAIL;
    memset(out, 0, sizeof *out);
    if (!R) { snprintf(err, errlen, "out of memory"); return RW_FAIL; }
    R->err = err; R->errlen = errlen; R->poll = pollfn; R->ud = ud;
    if (rw_nso_load(nso_path, &R->n, err, errlen)) { free(R); return RW_FAIL; }
    if (nso_header) memcpy(nso_header, R->n.header, 0x100);
    if (!setjmp(R->jb)) {
        run(R, name);
        RwLayout *l = &out->l; snprintf(l->name, sizeof l->name, "%s", name); memcpy(l->build_id, R->n.build_id, 33);
        l->text_end = R->text_end; l->ro_off = R->n.ro_off; l->data_off = R->n.data_off; l->key_page_off = R->key; l->patch_off = R->patch_off;
        tsvs(R, out);
        /* <name>.patched.bin = image (with key page) + zero padding to PATCH_OFF + patch area */
        size_t size = R->patch_off + 4 * R->np;
        uint8_t *img = realloc(R->n.img, size);
        if (!img) FAIL("out of memory");
        R->n.img = R->img = img;
        memset(img + R->len, 0, R->patch_off - R->len);
        for (size_t i = 0; i < R->np; i++) wr32(img + R->patch_off + 4 * i, R->patch[i]);
        out->image = img; out->image_size = size; R->n.img = NULL;
        out->gates = R->gates; out->ngate = R->ng; R->gates = NULL;
        rc = RW_OK;
    } else rc = R->cancelled ? RW_CANCELLED : RW_FAIL;
    if (rc != RW_OK) { for (int k = 0; k < 6; k++) free(out->tsv[k].p); memset(out, 0, sizeof *out); if (rc == RW_CANCELLED) snprintf(err, errlen, "cancelled"); }
    cs_insn *bufs[2] = {R->d1, R->d2};
    for (int k = 0; k < 2; k++) if (bufs[k]) cs_free(bufs[k], 1);
    if (R->hd) cs_close(&R->hd);
    for (int t = 0; t < R->nth; t++) { cs_free(R->tins[t], 1); cs_close(&R->th[t]); }
    for (size_t s = 0; s < R->nw; s++) free(R->win[s].v);
    for (size_t g = 0; g < R->ngr; g++) free(R->groups[g].v);
    for (size_t c = 0; R->scan_v && c < R->nscan; c++) free(R->scan_v[c]);
    for (size_t i = 0; R->pre && i < R->nfn; i++) free(R->pre[i]);
    free(R->n.img); free(R->patch); free(R->gates); free(R->kinds); free(R->done); free(R->starts); free(R->win); free(R->groups);
    free(R->calls); free(R->jumps); free(R->tags.v); free(R->relr); free(R->scan_v); free(R->scan_n); free(R->pre); free(R->npre);
    free(R->fns); free(R);
    return rc;
}

void rw_free_module(RwModule *m) { free(m->image); free(m->gates); for (int k = 0; k < 6; k++) free(m->tsv[k].p); memset(m, 0, sizeof *m); }
