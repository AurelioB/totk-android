/* The request / install.json reader and the JSON output buffer. Inputs are small (requests, install.json), so a bounded
   recursive DOM is enough; output is pure ASCII so it passes through JNI's modified UTF-8 unchanged. */
#include "setup_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end; char *err; size_t errcap; int depth; } P;
static int perr(P *p, const char *m) { if (p->err && !*p->err) snprintf(p->err, p->errcap, "JSON: %s", m); return -1; }
static void ws(P *p) { while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++; }
static int hex4(P *p, unsigned *v) {
    if (p->end - p->p < 4) return perr(p, "bad escape");
    *v = 0;
    for (int i = 0; i < 4; i++) { char c = *p->p++; *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0'); else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10); else return perr(p, "bad escape"); }
    return 0;
}
static void put_utf8(Buf *b, unsigned c) {
    char t[4]; size_t n;
    if (c < 0x80) { t[0] = (char)c; n = 1; }
    else if (c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | (c >> 6 & 63)); t[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { t[0] = (char)(0xf0 | c >> 18); t[1] = (char)(0x80 | (c >> 12 & 63)); t[2] = (char)(0x80 | (c >> 6 & 63)); t[3] = (char)(0x80 | (c & 63)); n = 4; }
    buf_add(b, t, n);
}
static int str(P *p, char **out, size_t *len) {
    Buf b = {0};
    p->p++;
    while (1) {
        if (p->p >= p->end) { buf_free(&b); return perr(p, "unterminated string"); }
        unsigned char c = (unsigned char)*p->p++;
        if (c == '"') break;
        if (c < 0x20) { buf_free(&b); return perr(p, "control character in string"); }
        if (c != '\\') { buf_add(&b, (const char *)&c, 1); continue; }
        if (p->p >= p->end) { buf_free(&b); return perr(p, "bad escape"); }
        char e = *p->p++; unsigned u;
        switch (e) {
        case '"': case '\\': case '/': buf_add(&b, &e, 1); break;
        case 'b': buf_add(&b, "\b", 1); break; case 'f': buf_add(&b, "\f", 1); break;
        case 'n': buf_add(&b, "\n", 1); break; case 'r': buf_add(&b, "\r", 1); break; case 't': buf_add(&b, "\t", 1); break;
        case 'u':
            if (hex4(p, &u)) { buf_free(&b); return -1; }
            if (u >= 0xd800 && u < 0xdc00) {
                unsigned lo;
                if (p->end - p->p < 6 || p->p[0] != '\\' || p->p[1] != 'u') { buf_free(&b); return perr(p, "lone surrogate"); }
                p->p += 2;
                if (hex4(p, &lo) || lo < 0xdc00 || lo >= 0xe000) { buf_free(&b); return perr(p, "lone surrogate"); }
                u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
            } else if (u >= 0xdc00 && u < 0xe000) { buf_free(&b); return perr(p, "lone surrogate"); }
            if (!u) { buf_free(&b); return perr(p, "NUL in string"); }
            put_utf8(&b, u); break;
        default: buf_free(&b); return perr(p, "bad escape");
        }
    }
    buf_add(&b, "", 1);
    if (b.oom) { buf_free(&b); return perr(p, "out of memory"); }
    *out = b.p; *len = b.n - 1;
    return 0;
}
static int value(P *p, Json *v);
static int grow(Json **items, char ***keys, size_t n, size_t *cap) {
    if (n < *cap) return 0;
    size_t c = *cap ? *cap * 2 : 8;
    Json *ni = realloc(*items, c * sizeof **items); if (!ni) return -1; *items = ni;
    if (keys) { char **nk = realloc(*keys, c * sizeof **keys); if (!nk) return -1; *keys = nk; }
    *cap = c; return 0;
}
static int value(P *p, Json *v) {
    memset(v, 0, sizeof *v);
    ws(p);
    if (p->p >= p->end) return perr(p, "unexpected end");
    char c = *p->p;
    if (c == '{' || c == '[') {
        if (++p->depth > 32) return perr(p, "nested too deep");
        int obj = c == '{'; size_t cap = 0;
        v->type = obj ? JSON_OBJ : JSON_ARR; p->p++; ws(p);
        if (p->p < p->end && *p->p == (obj ? '}' : ']')) { p->p++; p->depth--; return 0; }
        while (1) {
            if (v->n >= 100000 || grow(&v->items, obj ? &v->keys : NULL, v->n, &cap)) return perr(p, "too large");
            if (obj) {
                ws(p); size_t kl;
                if (p->p >= p->end || *p->p != '"') return perr(p, "expected key");
                if (str(p, &v->keys[v->n], &kl)) return -1;
                ws(p);
                if (p->p >= p->end || *p->p != ':') { free(v->keys[v->n]); return perr(p, "expected ':'"); }
                p->p++;
            }
            if (value(p, &v->items[v->n])) { if (obj) free(v->keys[v->n]); json_free(&v->items[v->n]); return -1; }
            v->n++; ws(p);
            if (p->p < p->end && *p->p == ',') { p->p++; continue; }
            if (p->p < p->end && *p->p == (obj ? '}' : ']')) { p->p++; p->depth--; return 0; }
            return perr(p, "expected ',' or end");
        }
    }
    if (c == '"') { v->type = JSON_STR; return str(p, &v->s, &v->len); }
    if (p->end - p->p >= 4 && !memcmp(p->p, "true", 4)) { v->type = JSON_BOOL; v->b = 1; p->p += 4; return 0; }
    if (p->end - p->p >= 5 && !memcmp(p->p, "false", 5)) { v->type = JSON_BOOL; p->p += 5; return 0; }
    if (p->end - p->p >= 4 && !memcmp(p->p, "null", 4)) { v->type = JSON_NULL; p->p += 4; return 0; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64]; size_t n = 0;
        while (p->p + n < p->end && n < sizeof tmp - 1 && strchr("+-0123456789.eE", p->p[n])) n++;
        memcpy(tmp, p->p, n); tmp[n] = 0; p->p += n;
        char *e; v->type = JSON_NUM; v->num = strtod(tmp, &e);
        if (e != tmp + n || n == 0) return perr(p, "bad number");
        if (!strpbrk(tmp, ".eE")) { long long x = strtoll(tmp, &e, 10); if (*e == 0 && (tmp[0] == '-' ? x <= 0 : x >= 0)) { v->is_int = 1; v->i = x; } }
        return 0;
    }
    return perr(p, "unexpected character");
}
void json_free(Json *v) {
    if (!v) return;
    free(v->s);
    for (size_t i = 0; i < v->n; i++) { json_free(&v->items[i]); if (v->keys) free(v->keys[i]); }
    free(v->items); free(v->keys);
    memset(v, 0, sizeof *v);
}
Json *json_parse(const char *text, size_t len, char *err, size_t errcap) {
    if (err && errcap) *err = 0;
    P p = { text, text + len, err, errcap, 0 };
    Json *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    if (len > (16u << 20)) { perr(&p, "too large"); free(v); return NULL; }
    if (value(&p, v)) { json_free(v); free(v); return NULL; }
    ws(&p);
    if (p.p != p.end) { perr(&p, "trailing data"); json_free(v); free(v); return NULL; }
    return v;
}
const Json *json_get(const Json *o, const char *key) {
    if (!o || o->type != JSON_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++) if (!strcmp(o->keys[i], key)) return &o->items[i];
    return NULL;
}
const char *json_str(const Json *o, const char *key) { const Json *v = json_get(o, key); return v && v->type == JSON_STR ? v->s : NULL; }
int json_int(const Json *o, const char *key, int64_t *out) { const Json *v = json_get(o, key); if (!v || v->type != JSON_NUM || !v->is_int) return -1; *out = v->i; return 0; }
int json_bool(const Json *o, const char *key) { const Json *v = json_get(o, key); return v && v->type == JSON_BOOL && v->b; }

void buf_add(Buf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256; while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c); if (!q) { b->oom = 1; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_add(b, tmp, (size_t)n); return; }
    char *big = malloc((size_t)n + 1); if (!big) { b->oom = 1; return; }
    va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
    buf_add(b, big, (size_t)n); free(big);
}
/* Decodes one UTF-8 sequence strictly (no overlongs, surrogates or > U+10FFFF); returns its length, 0 if invalid. */
static size_t utf8_one(const unsigned char *s, unsigned *cp) {
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n = c >= 0xf0 && c <= 0xf4 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
    if (!n || (n == 3 && c >= 0xf0)) return 0;
    unsigned v = c & (n == 2 ? 0x1f : n == 3 ? 0x0f : 0x07);
    for (int i = 1; i < n; i++) { if ((s[i] & 0xc0) != 0x80) return 0; v = v << 6 | (s[i] & 0x3f); }
    if ((n == 3 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (n == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;
    *cp = v; return (size_t)n;
}
void buf_jstr(Buf *b, const char *s) {
    buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;) {
        unsigned c; size_t n = utf8_one(p, &c);
        if (!n) { c = 0xfffd; n = 1; }
        p += n;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; buf_add(b, e, 2); }
        else if (c >= 0x20 && c < 0x7f) { char ch = (char)c; buf_add(b, &ch, 1); }
        else if (c < 0x10000) buf_printf(b, "\\u%04x", c);
        else { c -= 0x10000; buf_printf(b, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff)); }
    }
    buf_add(b, "\"", 1);
}
void buf_free(Buf *b) { free(b->p); memset(b, 0, sizeof *b); }
