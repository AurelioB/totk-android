/* Standalone production-parser regressions. The frontend owns environment,
 * warning and default handling; conversion never changes an invalid output. */
#include "../../runtime/predict_budget.h"
#include <assert.h>
#include <stddef.h>

int main(void) {
    static const struct { const char *text; unsigned ppm; } valid[] = {
        {"0", 0}, {"1", 10000}, {"1.5", 15000}, {"2", 20000}, {"100", 1000000},
        {".5", 5000}, {"2.", 20000}, {"+2", 20000}, {"0002.0000", 20000},
        {"100.000000", 1000000}, {"0.0000499999", 0}, {"0.00005", 1},
        {"0.0001499999", 1}, {"0.00015", 2}, {"1.23454", 12345},
        {"1.23455", 12346}, {"99.99995", 1000000}, {"1.234549999999999999", 12345},
        {"-0", 0}, {"-0.0", 0}, {"+0e-1", 0}, {"2e0", 20000}, {"1.5E+0", 15000},
        {"1e2", 1000000}, {"0.01e4", 1000000}, {"100000000000000000000e-18", 1000000},
        {"1e-4", 1}, {"5e-5", 1}, {"4.999999e-5", 0}, {"15e-5", 2}, {"0001500e-7", 2},
        {"0e999999999999999999999999999999", 0}, {"-0e-999999999999999999999999999999", 0},
        {"1e-999999999999999999999999999999", 0}
    };
    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        unsigned ppm = 20000;
        assert(predict_budget_parse_pct(valid[i].text, &ppm));
        assert(ppm == valid[i].ppm);
    }
    static const char *const invalid[] = {
        NULL, "", ".", "+", "+.", "-.", "-1", "-0.00001", "100.00001", "101",
        "999999999999999999999999999999", " 2", "2 ", "\t2", "2\n",
        "nan", "NaN", "inf", "Infinity", "-inf", "0x1p1", "0X2", "2junk",
        "2%", "2.0.0", "2e999", "1,5", "++2", "1e", "1e+", ".e1", "e1", "1e0.0",
        "1e 0", "1e0 ", "-1e-999999999999999999999999999999", "1e999999999999999999999999999999",
        "100.00000000000000000000001e0", "1.00000000000000000000001e2"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        unsigned ppm = 20000;   /* Unset/invalid retain the frontend's 2% default. */
        assert(!predict_budget_parse_pct(invalid[i], &ppm));
        assert(ppm == 20000);
    }
    assert(!predict_budget_parse_pct("2", NULL));
    return 0;
}
