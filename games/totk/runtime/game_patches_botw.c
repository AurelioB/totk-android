/* Breath of the Wild game-code patches. Only the lock-ordering fix so far on Android. The other functions the shared
   runtime calls answer "not available" until BotW's own sites are ported (MacNativeGames game_patches_botw.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_patches.h"
#include "game_version.h"

void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE=0: as shipped), from MacNativeGames game_patches_botw.c: sead's spin
   locks in this game take the lock with a plain ldxr / stxr and only put a dmb ish before the release, so nothing orders
   the loads inside the critical section after the lock is taken. The Switch's Cortex-A57 rarely satisfies such a load
   early; wide out-of-order cores do (Apple's, and the Thor's Cortex-X3 / A715): two threads read the same "next job"
   index of a sead::JobQueue and run the same job twice. On the Thor that was a model job reading a vertex-buffer list
   entry another thread had consumed (nvnBufferMap of a garbage pointer from main+0x14ef6f4, about 20 s into the boot).
   Every plain ldxr / ldxp that opens a real exclusive window (a store-exclusive on the same base register within 16
   instructions) gets the acquire bit (ldaxr / ldaxp): one bit, the same registers and size, only stronger ordering. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n) {
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { w[i] |= 0x8000u; done++; break; }
    }
    return done;
}
void runtime_game_patches(Module *m) {
    fprintf(stderr, "[botw] %s game-code patches\n", TOTK_GAME_VERSION);
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (a && !strcmp(a, "0")) { fprintf(stderr, "[botw] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n"); return; }
    unsigned t = excl_acquire(m->base, m->text_end), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off);
    fprintf(stderr, "[botw] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
}
/* Output size, aspect and render-size controls: BotW's sites are not ported yet, so each answers "not available". */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
/* The job-queue poll hook (MacNativeGames game_patches_botw.c botw_jq_hook, a SIGTRAP site): not installed here, so
   no trap is ours. */
int botw_jq_hook(void *context) { (void)context; return 0; }
