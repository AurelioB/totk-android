/* Shared walking-owner canvas identity / UI boundary facts. No camera inference.
   Image identity is the original 1x image, never a native UI or framegen sibling. */
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t image, incarnation, storage, size, serial;
    uint32_t width, height, format, level, layer;
} vk_canvas_image;
typedef struct {
    uint64_t frame;
    vk_canvas_image canvas, window;
    uint32_t first_ui, ui_committed, transfer_committed, presented;
} vk_canvas_frame;
extern int vk_canvas_on; /* immutable after init; caller's disabled path is one branch */
/* ui_aspect: TOTK_ASPECT asks for a ratio other than 16:9, whose HUD squeeze needs the canvas (vk_ui_aspect.h). */
void vk_canvas_init(int native_ui, int framegen, int ui_aspect);
int vk_canvas_ui_shader(const char *vs_key, int after_boundary);
int vk_canvas_transfer_shader(const char *fs_key);
#ifdef TOTK_VK_TESTS
/* Explicit owned-fixture identities only; NULL,NULL removes the nomination.
 * Never compiled into application shader classification. */
int vk_canvas_test_shaders(const char *ui_vertex,const char *terminal_fragment);
#endif
/* Discovery before the transfer is required to bind a native source. Only
   committed=1 (after an emitted draw) establishes the transfer-to-present chain. */
void vk_canvas_transfer(const vk_canvas_image *source, const vk_canvas_image *window, int committed);
/* Returns 1 BEFORE the first emitted UI draw. Preparation is idempotent until
   ui_commit, so native UI and framegen can both observe the same boundary.
   Call ui_commit only after a real draw was emitted. Extents are guest/active. */
int vk_canvas_first_ui(const vk_canvas_image *target, const char *vs_key);
int vk_canvas_in_ui(const vk_canvas_image *target);
void vk_canvas_ui_commit(const vk_canvas_image *target);
int vk_canvas_present(const vk_canvas_image *window);
const vk_canvas_frame *vk_canvas_current(void); /* walking owner; valid until next mutation */
void vk_canvas_frame_end(void);
