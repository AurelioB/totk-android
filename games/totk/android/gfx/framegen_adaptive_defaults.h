/* MIT. Scheduler-path default-on selectors: TOTK_FG_ADAPTIVE_SLOTS=1 implies
 * TOTK_FG_ADAPTIVE_CHUNK=1, TOTK_FG_ADAPTIVE_LATTICE=1 and (adaptive QCOM only)
 * TOTK_FG_ADAPTIVE_LEAN=1 unless overridden.
 * Callers consult these only while the slot scheduler is active.
 * Absent -> 1 (default on); exact "0" -> 0 (r7/r6b path); exact "1" -> 1;
 * any other spelling -> -1 (invalid: treated as off and logged by the caller). */
#pragma once
#include <stddef.h>
#include <stdio.h>

static inline int fga_default_on_request(const char *value) {
    if (!value) return 1;
    if (value[0]=='0' && !value[1]) return 0;
    return value[0]=='1' && !value[1] ? 1 : -1;
}
/* Default-off scheduler opt-ins (EARLY_APPLY, BUDGET, PIPE_SETTLE, DIRECT_OUT):
 * absent or exact "0" -> 0, exact "1" -> 1, any other spelling -> -1 (invalid:
 * treated as off and logged by the caller). */
static inline int fga_default_off_request(const char *value) {
    if (!value || (value[0]=='0' && !value[1])) return 0;
    return value[0]=='1' && !value[1] ? 1 : -1;
}
/* Startup audit only. The effective value is what the selector's consumer
 * resolves; source is default (absent), explicit (exact 0/1) or invalid. */
static inline int fga_selector_effective(const char *value,int default_on,const char **source) {
    int request=default_on ? fga_default_on_request(value) : fga_default_off_request(value);
    *source=request<0 ? "invalid" : value ? "explicit" : "default";
    return request==1;
}
enum { FGA_SELECTORS=7, FGA_SELECTOR_LEAN=5, FGA_SELECTOR_DIRECT_OUT=6 };
/* Selector i's environment name, or NULL past the end. */
static inline const char *fga_selector_env(unsigned i) {
    static const char *const names[FGA_SELECTORS]={"TOTK_FG_ADAPTIVE_CHUNK","TOTK_FG_ADAPTIVE_LATTICE",
        "TOTK_FG_ADAPTIVE_EARLY_APPLY","TOTK_FG_ADAPTIVE_BUDGET","TOTK_FG_ADAPTIVE_PIPE_SETTLE",
        "TOTK_FG_ADAPTIVE_LEAN","TOTK_FG_ADAPTIVE_DIRECT_OUT"};
    return i<FGA_SELECTORS ? names[i] : NULL;
}
/* values[i] is the raw environment value of fga_selector_env(i) (NULL when
 * absent). Writes one "NAME=value(source)" row per selector; returns the
 * snprintf-style length so callers can detect truncation. */
static inline int fga_selector_summary(char *out,size_t size,const char *const values[FGA_SELECTORS]) {
    static const int default_on[FGA_SELECTORS]={1,1,0,0,0,1,0}; /* CHUNK, LATTICE, LEAN */
    size_t used=0; int total=0;
    for (unsigned i=0;i<FGA_SELECTORS;i++) {
        const char *source; int value=fga_selector_effective(values[i],default_on[i],&source);
        int n=snprintf(out+used,used<size ? size-used : 0,"%s%s=%d(source=%s)",i ? " " : "",
            fga_selector_env(i)+sizeof "TOTK_FG_ADAPTIVE_"-1,value,source);
        if (n<0) return n;
        total+=n; used=(size_t)total<size ? (size_t)total : size;
    }
    return total;
}
