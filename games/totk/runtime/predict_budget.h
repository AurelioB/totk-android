/* TOTK_PREDICT_MISS_PCT conversion only: no environment, policy or logging.
 * Accept signed decimal fractions with an optional signed decimal exponent,
 * but no whitespace. Validate the exact finite 0..100 percent range before
 * rounding half-up to integer parts per million, without floating point or
 * locale dependence. Invalid input leaves the caller-owned default unchanged. */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline int predict_budget_parse_pct(const char *text, unsigned *ppm) {
    if (!text || !ppm) return 0;
    const char *p = text, *point = NULL, *first = NULL;
    int negative = *p == '-';
    if (*p == '+' || *p == '-') p++;
    unsigned digits = 0, prefix = 0, kept = 0, first_digit = 0, tail_nonzero = 0;
    while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (*p == '.') {
            if (point) return 0;
            point = p++;
            continue;
        }
        unsigned digit = (unsigned)(*p - '0');
        digits = 1;
        if (!first && digit) { first = p; first_digit = digit; }
        else if (first && digit) tail_nonzero = 1;
        if (first && kept < 8) { prefix = prefix * 10 + digit; kept++; }
        p++;
    }
    if (!point) point = p;
    uintmax_t exponent = 0;
    int exponent_negative = 0, exponent_overflow = 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        exponent_negative = *p == '-';
        if (*p == '+' || *p == '-') p++;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p++ - '0');
            if (!exponent_overflow) {
                if (exponent > (UINTMAX_MAX - digit) / 10) exponent_overflow = 1;
                else exponent = exponent * 10 + digit;
            }
        }
    }
    if (!digits || *p) return 0;
    if (!first) { *ppm = 0; return 1; }   /* Both signs of exact zero are valid. */
    if (negative) return 0;
    if (exponent_overflow) {
        if (!exponent_negative) return 0;
        *ppm = 0;
        return 1;
    }

    /* First significant digit's power of ten, combined as signed magnitudes
     * so arbitrarily large explicit exponents never overflow signed math. */
    ptrdiff_t offset = first < point ? point - first - 1 : point - first;
    int power_negative = offset < 0;
    uintmax_t power = power_negative ? (uintmax_t)-offset : (uintmax_t)offset;
    if (power_negative == exponent_negative) {
        power = exponent > UINTMAX_MAX - power ? UINTMAX_MAX : power + exponent;
    } else if (power >= exponent) power -= exponent;
    else { power = exponent - power; power_negative = exponent_negative; }
    if (!power_negative && (power > 2 ||
        (power == 2 && (first_digit != 1 || tail_nonzero)))) return 0;
    if (power_negative && power > 5) { *ppm = 0; return 1; }

    /* At most seven ppm integer digits plus the first rounding digit matter.
     * Later nonzero digits were still inspected for the exact 100% bound. */
    while (kept < 8) { prefix *= 10; kept++; }
    unsigned integer_digits = power_negative ? 5 - (unsigned)power : 5 + (unsigned)power;
    unsigned divisor = 1;
    for (unsigned i = integer_digits; i < 8; i++) divisor *= 10;
    *ppm = prefix / divisor + (prefix % divisor >= divisor / 2);
    return 1;
}
