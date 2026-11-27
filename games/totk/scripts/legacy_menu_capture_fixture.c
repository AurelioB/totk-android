/* The legacy startup path has no menu installer. Its existing aspect patch may
   still install: compare complete startup images across unset/0/1 menu intent. */
#include "../runtime/game_patches.c"
#include <assert.h>
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"unexpected failure: %s\n",message);abort();}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t value){(void)value;}
void dm_gfx_set_flag(const char *name,int value){(void)name;(void)value;}
void runtime_survey_init(Module *m){(void)m;}
int main(void){
    enum { SIZE=0x4524000 };
    unsigned char *image=aligned_alloc(0x4000,SIZE),*before=malloc(SIZE);
    assert(image&&before);
    Module module={.name="main",.base=image,.text_end=0x2bb9c00,.file_size=0x451cdd8,.size=SIZE};
    setenv("TOTK_REALTIME","0",1);setenv("TOTK_FPS60","0",1);
    setenv("TOTK_HIGHFPS_FIX","0",1);setenv("TOTK_OUTPUT_RES","0",1);
    unsetenv("TOTK_RENDER_RES");unsetenv("TOTK_PCHTXT");unsetenv("TOTK_TRAP_AT");unsetenv("TOTK_ASPECT");
    const char *values[]={NULL,"0","1"};
    for(unsigned i=0;i<sizeof values/sizeof *values;i++){
        if(values[i])setenv("TOTK_MENU_CAPTURE_FIX",values[i],1);else unsetenv("TOTK_MENU_CAPTURE_FIX");
        memset(image,0xa5,SIZE);
#if defined(TOTK_GAME_V121)
        /* Synthetic FDIVs, not game words, for the unconditional legacy aspect owner. */
        const uint64_t sites[]={0xcb4b10,0x12521cc,0x19b36f0,0x1ecdf68};
        const uint32_t fdiv=0x1e201800;
        for(unsigned site=0;site<sizeof sites/sizeof *sites;site++)memcpy(image+sites[site],&fdiv,4);
        memset(image+module.text_end,0,4*ASPECT_STUB);
#endif
        runtime_game_patches(&module);
        if(!i)memcpy(before,image,SIZE);else assert(!memcmp(before,image,SIZE));
    }
    free(before);free(image);puts("legacy menu image unchanged: complete native startup images match across unset/0/1");
    return 0;
}
