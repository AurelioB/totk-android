#include "vk_canvas.h"
#include <string.h>
int vk_canvas_on;
static vk_canvas_frame current = {.frame=1};
/* Canvas allocations rotate/rebuild; retain a small recent identity set rather
   than guessing all equal-sized targets. Eviction loses sharpness/eligibility,
   never nominates an unrelated image. No per-draw allocation or hashing. */
static struct { vk_canvas_image canvas, window; uint64_t seen, ui_frame; } known[16];
#ifdef TOTK_VK_TESTS
static char test_ui_vertex[80],test_terminal_fragment[80];
int vk_canvas_test_shaders(const char *ui_vertex,const char *terminal_fragment) {
    if (!ui_vertex && !terminal_fragment) {
        test_ui_vertex[0]=test_terminal_fragment[0]=0; return 1;
    }
    if (!ui_vertex || !terminal_fragment || !*ui_vertex || !*terminal_fragment ||
        strlen(ui_vertex)>=sizeof test_ui_vertex || strlen(terminal_fragment)>=sizeof test_terminal_fragment)
        return 0;
    strcpy(test_ui_vertex,ui_vertex); strcpy(test_terminal_fragment,terminal_fragment);
    return 1;
}
#endif
static int same(const vk_canvas_image *a,const vk_canvas_image *b) {
    return a->image && a->image==b->image && a->incarnation==b->incarnation && a->storage==b->storage && a->size==b->size &&
        a->width==b->width && a->height==b->height && a->format==b->format && a->level==b->level && a->layer==b->layer;
}
static unsigned slot(const vk_canvas_image *image) {
    unsigned oldest=0;
    for(unsigned i=0;i<16;i++) {
        if(same(image,&known[i].canvas))return i;
        if(known[i].seen<known[oldest].seen)oldest=i;
    }
    return oldest;
}
void vk_canvas_init(int native_ui,int framegen,int ui_aspect) { vk_canvas_on=native_ui||framegen||ui_aspect; }
int vk_canvas_ui_shader(const char *key,int after) {
    static const char *const keys[]={"7f2113e2","e9fd7b43","a662e313","16684efc","3e11cee5",
        "48c18ae6","35cbc686","4872bec9","9f47ed90","1a40e5f5","2117f58b","8d4e36d0","1469983f"};
    if(!key)return 0;
#ifdef TOTK_VK_TESTS
    if (*test_ui_vertex && !strcmp(key,test_ui_vertex)) return 1;
#endif
    unsigned count=after?sizeof keys/sizeof *keys:9;
    for(unsigned i=0;i<count;i++)if(!strncmp(key,keys[i],8))return 1;
    return 0;
}
int vk_canvas_transfer_shader(const char *key) {
#ifdef TOTK_VK_TESTS
    if (key && *test_terminal_fragment && !strcmp(key,test_terminal_fragment)) return 1;
#endif
    return key&&!strncmp(key,"dd3d1e0ebf73242845970b5655fac199a30f5acf747975c5e771caa49af8b12b",64);
}
void vk_canvas_transfer(const vk_canvas_image *source,const vk_canvas_image *window,int committed) {
    if(!vk_canvas_on||!source||!window||!source->image||!window->image||source->image==window->image)return;
    unsigned i=slot(source);
    if(!same(source,&known[i].canvas))known[i].ui_frame=0;
    known[i].canvas=*source; known[i].window=*window; known[i].seen=current.frame;
    if(!same(source,&current.canvas)) {
        uint64_t frame=current.frame; memset(&current,0,sizeof current); current.frame=frame;
        current.canvas=*source;
    }
    if(!committed)current.transfer_committed=current.presented=0;
    current.window=*window;
    if(committed)current.transfer_committed=1;
}
int vk_canvas_first_ui(const vk_canvas_image *target,const char *key) {
    if(!vk_canvas_on||!target||!vk_canvas_ui_shader(key,0))return 0;
    unsigned i=slot(target);
    if(!same(target,&known[i].canvas))return 0;
    if(known[i].ui_frame==current.frame)return same(target,&current.canvas)&&!current.ui_committed;
    known[i].ui_frame=current.frame;
    if(!same(target,&current.canvas)) {
        uint64_t frame=current.frame; memset(&current,0,sizeof current); current.frame=frame;
    }
    current.canvas=*target; current.window=known[i].window; current.first_ui=1;
    return 1;
}
int vk_canvas_in_ui(const vk_canvas_image *target) {
    if(!vk_canvas_on||!target)return 0;
    unsigned i=slot(target); return same(target,&known[i].canvas)&&known[i].ui_frame==current.frame;
}
void vk_canvas_ui_commit(const vk_canvas_image *target) {
    if(vk_canvas_on&&target&&current.first_ui&&same(target,&current.canvas))current.ui_committed=1;
}
int vk_canvas_present(const vk_canvas_image *window) {
    if(!vk_canvas_on||!window||!current.transfer_committed||!same(window,&current.window))return 0;
    current.presented=1; return 1;
}
const vk_canvas_frame *vk_canvas_current(void) { return &current; }
void vk_canvas_frame_end(void) {
    if(!vk_canvas_on)return;
    uint64_t frame=current.frame+1; memset(&current,0,sizeof current); current.frame=frame;
}
