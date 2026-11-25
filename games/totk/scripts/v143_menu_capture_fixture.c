/* Production patch admission/layout tests; no guest bytes in this file.
   Real-word mode is seeded from the player's authenticated NSO by the verifier. */
#define TOTK_THERMAL_PATCH_TEST 1
#include <assert.h>
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"unexpected failure: %s\n",message);abort();}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t value){(void)value;}
void dm_gfx_set_flag(const char *name,int value){(void)name;(void)value;}
void runtime_survey_init(Module *m){(void)m;}
enum { ALLOCATION=0x2bc0000, TEXT_END=0x2bb9c00, SITE=0x105bbcc };
static Module module;
static unsigned char *before;
static uint64_t stub(void){return ((module.text_end+15)&~15ULL)+MENU_CAPTURE_STUB;}
static void reset(void){
    module.text_end=TEXT_END;module.file_size=0x2bbb000;module.size=ALLOCATION;
    memset(module.base,0,ALLOCATION);
    fixture_seed_at(module.base,SITE);fixture_seed_at(module.base,SITE+4);
    unsetenv("TOTK_MENU_CAPTURE_FIX");unsetenv("TOTK_THERMAL_STEP");
}
static void unchanged(void){
    memcpy(before,module.base,ALLOCATION);patch_menu_capture(&module);
    assert(!memcmp(before,module.base,ALLOCATION));
}
static uint32_t word(uint64_t off){uint32_t w;memcpy(&w,module.base+off,4);return w;}
static uint64_t branch_target(uint64_t off){
    uint32_t w=word(off);assert((w&0xfc000000u)==0x14000000u);
    int32_t delta=(int32_t)(w<<6)>>4;return off+delta;
}
static void applied(void){
    memcpy(before,module.base,ALLOCATION);patch_menu_capture(&module);
    assert(branch_target(SITE)==stub());
    assert(branch_target(stub()+32)==SITE+4);
    assert(word(stub())==fixture_seed(SITE));
    assert(word(SITE+4)==fixture_seed(SITE+4));
    /* Exactly one patched site and this veneer; no neighbouring owner writes. */
    memcpy(before+SITE,module.base+SITE,4);
    memcpy(before+stub(),module.base+stub(),MENU_CAPTURE_WORDS*4);
    assert(!memcmp(before,module.base,ALLOCATION));
}
int main(int argc,char **argv){
    module=(Module){.name="main",.base=aligned_alloc(0x4000,ALLOCATION)};
    before=malloc(ALLOCATION);assert(module.base&&before);
    reset();unchanged();module.base[SITE]^=1;unchanged();
    const char *disabled[]={"0","","2","true","-1","1 "," 1"};
    for(unsigned i=0;i<sizeof disabled/sizeof *disabled;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX",disabled[i],1);unchanged();
    }
    for(unsigned i=0;i<2;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.base[SITE+4*i]^=1;unchanged();
    }
    for(unsigned i=0;i<MENU_CAPTURE_WORDS*4;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.base[stub()+i]=1;unchanged();
    }
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=SITE+4;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.size=SITE+4;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=UINT64_MAX;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=TEXT_END+0x20;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.size=stub()+MENU_CAPTURE_WORDS*4-1;unchanged();
    /* Range rejection happens before accessing absent sites/padding. */
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=0x9100000;module.size=0xa000000;unchanged();
    /* Actual 1.4.3 geometry: 1024 bytes before RO_OFF=0x2bba000.
       Also test the exact site-end and last fitting page alignment. */
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=SITE+8;applied();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=TEXT_END+0x10;applied();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.size=stub()+MENU_CAPTURE_WORDS*4;applied();
    for(unsigned lag=0;lag<2;lag++)for(unsigned primary=0;primary<2;primary++)for(unsigned alias=0;alias<2;alias++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);
        setenv("TOTK_FRAME_LAG",lag?"1":"0",1);
        setenv("TOTK_VK_ONE_PRIMARY",primary?"1":"0",1);
        setenv("TOTK_WB_ALIAS",alias?"1":"0",1);
        applied();unchanged(); /* repeated install fails closed, no partial writes */
    }
    /* Hardening stays disjoint; thermal-step wins in both installation orders. */
    for(unsigned order=0;order<2;order++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);setenv("TOTK_THERMAL_STEP","1",1);
        const unsigned indices[]={1,6,4};
        for(unsigned i=0;i<3;i++)memcpy(module.base+fps60[indices[i]].off,&fps60[indices[i]].replacement,4);
        /* Reserve the complete existing hardening owner, even where it is zero in practice. */
        memset(module.base+((TEXT_END+15)&~15ULL)+416,0xa5,500);
        if(order){unchanged();patch_thermal_step(&module);}
        else {patch_thermal_step(&module);unchanged();}
        assert(word(((TEXT_END+15)&~15ULL)+916)==fps60[1].replacement);
        assert(word(SITE)==fixture_seed(SITE));
    }
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);
    memset(module.base+((TEXT_END+15)&~15ULL)+416,0xa5,500);applied();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);applied();
    if(argc==2){
        FILE *f=fopen(argv[1],"wb");assert(f);
        assert(fwrite(module.base,1,ALLOCATION,f)==ALLOCATION);assert(!fclose(f));
    }
    free(before);free(module.base);
    puts("menu capture guards PASS: opt-in, both hashes, 36 occupied bytes, real 1.4.3 geometry, bounds/range, exact boundaries, branches, eight policy combinations, hardening coexistence/thermal preference");
    return 0;
}
