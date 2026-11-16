/* Breath of the Wild game-code patches. None yet on Android: the game runs as shipped. The functions the shared
   runtime calls answer "not available" until BotW's own sites are ported (MacNativeGames game_patches_botw.c). */
#include <stdio.h>
#include "game_patches.h"
#include "game_version.h"

void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
void runtime_game_patches(Module *m) { (void)m; fprintf(stderr, "[botw] %s: no game-code patches\n", TOTK_GAME_VERSION); }
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
