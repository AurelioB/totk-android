/* Production patch admission/layout tests; no guest bytes in this file.
   Real-word mode is seeded from the player's authenticated NSO by the verifier. */
#define TOTK_THERMAL_PATCH_TEST 1
#include <assert.h>
#include "../runtime/platform.h"
/* Exercise the Android policy without importing Android system headers. */
#ifndef MENU_CAPTURE_NATIVE_HOST
#undef TOTK_ANDROID
#define TOTK_ANDROID 1
#endif
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"unexpected failure: %s\n",message);abort();}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t value){(void)value;}
void dm_gfx_set_flag(const char *name,int value){(void)name;(void)value;}
void runtime_survey_init(Module *m){(void)m;}
enum { ALLOCATION=0x4524000, FILE_SIZE=0x451cdd8, TEXT_END=0x2bb9c00, SITE=0x105bbcc };
static const uint64_t hardening_sites[]={
    0x0a7ac4,0x95dd24,0x6155e4,0x615660,0x45c700,0x2b23c7c,
    0x42c3bc,0xa5a188,0x2db238,0xa5a670,
    0xa5a18c,0xa5a660,0xa5a664,0xa5a668,0xa5a66c,0xa5a674,
};
static Module module;
static unsigned char *before;
static uint64_t stub(void){return ((module.text_end+15)&~15ULL)+MENU_CAPTURE_STUB;}
static uint64_t tail(void){return ((module.text_end+15)&~15ULL)+MENU_CAPTURE_TAIL;}
static void reset(void){
    module.text_end=TEXT_END;module.file_size=FILE_SIZE;module.size=ALLOCATION;
    memset(module.base,0,ALLOCATION);
    fixture_seed_at(module.base,SITE);fixture_seed_at(module.base,SITE+4);
    for(unsigned i=0;i<sizeof hardening_sites/sizeof *hardening_sites;i++)
        fixture_seed_at(module.base,hardening_sites[i]);
    unsetenv("TOTK_MENU_CAPTURE_FIX");unsetenv("TOTK_THERMAL_STEP");
    unsetenv("TOTK_FPS60");unsetenv("TOTK_FRAMEGEN");
    unsetenv("TOTK_FPS60_HARDENING");unsetenv("TOTK_FPS60_FADE_CLAMP");
}
static void unchanged(void){
    memcpy(before,module.base,ALLOCATION);int outcome=patch_menu_capture(&module);
    assert(runtime_menu_capture_state()==(outcome<0?-1:outcome));
    assert(!memcmp(before,module.base,ALLOCATION));
}
static uint32_t word(uint64_t off){uint32_t w;memcpy(&w,module.base+off,4);return w;}
static uint64_t branch_target(uint64_t off){
    uint32_t w=word(off);assert((w&0xfc000000u)==0x14000000u);
    int32_t delta=(int32_t)(w<<6)>>4;return off+delta;
}
static void applied(void){
    memcpy(before,module.base,ALLOCATION);assert(patch_menu_capture(&module)==1);
    assert(runtime_menu_capture_state()==1);
    assert(branch_target(SITE)==stub());
    assert(branch_target(stub()+8)==tail());
    assert(branch_target(tail()+4)==SITE+4);
    assert(word(stub())==fixture_seed(SITE));
    assert(word(SITE+4)==fixture_seed(SITE+4));
    /* Exactly one patched site and this veneer; no neighbouring owner writes. */
    memcpy(before+SITE,module.base+SITE,4);
    memcpy(before+stub(),module.base+stub(),MENU_CAPTURE_HEAD_WORDS*4);
    memcpy(before+tail(),module.base+tail(),MENU_CAPTURE_TAIL_WORDS*4);
    assert(!memcmp(before,module.base,ALLOCATION));
}
int main(int argc,char **argv){
    module=(Module){.name="main",.base=aligned_alloc(0x4000,ALLOCATION)};
    before=malloc(ALLOCATION);assert(module.base&&before);
    if(argc==2&&(!strcmp(argv[1],"unsupported")||!strcmp(argv[1],"host-default"))){
        const char *values[]={NULL,"0","1"};
        unsigned count=!strcmp(argv[1],"host-default")?1:sizeof values/sizeof *values;
        for(unsigned i=0;i<count;i++){
            reset();
            if(values[i])setenv("TOTK_MENU_CAPTURE_FIX",values[i],1);
            memcpy(before,module.base,ALLOCATION);
            assert(patch_menu_capture(&module)==0);
            assert(runtime_menu_capture_state()==0);
            assert(!memcmp(before,module.base,ALLOCATION));
        }
        free(before);free(module.base);
        puts("unsupported menu capture path PASS: complete image unchanged");
        return 0;
    }
    reset();applied();unchanged();
    reset();module.base[SITE]^=1;unchanged();
    const char *disabled[]={"0","","2","true","-1","1 "," 1"};
    for(unsigned i=0;i<sizeof disabled/sizeof *disabled;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX",disabled[i],1);unchanged();
    }
    for(unsigned i=0;i<2;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.base[SITE+4*i]^=1;unchanged();
    }
    for(unsigned i=0;i<MENU_CAPTURE_HEAD_WORDS*4;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.base[stub()+i]=1;unchanged();
    }
    for(unsigned i=0;i<MENU_CAPTURE_TAIL_WORDS*4;i++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.base[tail()+i]=1;unchanged();
    }
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=SITE+4;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.size=SITE+4;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=UINT64_MAX;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=TEXT_END+0x2b0;unchanged();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.size=tail()+MENU_CAPTURE_TAIL_WORDS*4-1;unchanged();
    /* Range rejection happens before accessing absent sites/padding. */
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=0x9100000;module.size=module.file_size=0xa000000;unchanged();
    /* The site-to-head branch fits, but the tail-to-site return does not. */
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);
    module.text_end=(SITE+(1ull<<27)-MENU_CAPTURE_STUB-1)&~15ull;
    module.size=module.file_size=0xa000000;unchanged();
    /* Actual 1.4.3 geometry: 1024 bytes before RO_OFF=0x2bba000.
       Also test the exact site-end and last fitting page alignment. */
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=SITE+8;applied();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.text_end=TEXT_END+0x2a0;applied();
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);module.size=module.file_size=tail()+MENU_CAPTURE_TAIL_WORDS*4;applied();
    for(unsigned lag=0;lag<2;lag++)for(unsigned primary=0;primary<2;primary++)for(unsigned alias=0;alias<2;alias++){
        reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);
        setenv("TOTK_FRAME_LAG",lag?"1":"0",1);
        setenv("TOTK_VK_ONE_PRIMARY",primary?"1":"0",1);
        setenv("TOTK_WB_ALIAS",alias?"1":"0",1);
        applied();unchanged(); /* repeated install fails closed, no partial writes */
    }
    /* Install the real three production groups in all six orders, with both
       menu/thermal switches on and off. No owner may depend on another's order. */
    static const unsigned orders[][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    int (*const patches[])(Module *)={patch_menu_capture,patch_thermal_step,patch_fps60_hardening};
    unsigned char *coexist=malloc(ALLOCATION);assert(coexist);
    for(unsigned menu=0;menu<2;menu++)for(unsigned thermal=0;thermal<2;thermal++)
        for(unsigned order=0;order<6;order++){
            reset();setenv("TOTK_MENU_CAPTURE_FIX",menu?"1":"0",1);
            setenv("TOTK_THERMAL_STEP",thermal?"1":"0",1);
            const unsigned indices[]={1,6,4};
            for(unsigned i=0;i<3;i++)memcpy(module.base+fps60[indices[i]].off,&fps60[indices[i]].replacement,4);
            for(unsigned i=0;i<3;i++)assert(patches[orders[order][i]](&module)>=0);
            if(menu){
                assert(branch_target(SITE)==stub());
                assert(branch_target(stub()+8)==tail());
                assert(branch_target(tail()+4)==SITE+4);
            }else{
                assert(word(SITE)==fixture_seed(SITE));
                for(unsigned i=0;i<MENU_CAPTURE_HEAD_WORDS*4;i++)assert(!module.base[stub()+i]);
                for(unsigned i=0;i<MENU_CAPTURE_TAIL_WORDS*4;i++)assert(!module.base[tail()+i]);
            }
            for(unsigned i=0;i<3;i++)
                if(thermal)assert(branch_target(fps60[indices[i]].off)==TEXT_END+916+i*36);
                else assert(word(fps60[indices[i]].off)==fps60[indices[i]].replacement);
            for(unsigned i=0;i<10;i++){
                uint64_t target=branch_target(hardening_sites[i]);
                assert(target>=TEXT_END+416&&target<TEXT_END+916);
            }
            if(!order)memcpy(coexist,module.base,ALLOCATION);
            else assert(!memcmp(coexist,module.base,ALLOCATION));
        }
    free(coexist);
    /* Occupy every other tail byte: both split gaps are genuinely disjoint
       even with full-size framegen and thermal reservations. */
    reset();setenv("TOTK_MENU_CAPTURE_FIX","1",1);
    memset(module.base+TEXT_END,0xa5,1024);
    memset(module.base+stub(),0,MENU_CAPTURE_HEAD_WORDS*4);
    memset(module.base+tail(),0,MENU_CAPTURE_TAIL_WORDS*4);applied();
    if(argc==2||argc==3){
        reset();if(argc==3)setenv("TOTK_MENU_CAPTURE_FIX",argv[2],1);patch_menu_capture(&module);
        FILE *f=fopen(argv[1],"wb");assert(f);
        assert(fwrite(module.base,1,ALLOCATION,f)==ALLOCATION);assert(!fclose(f));
    }
    free(before);free(module.base);
    puts("menu capture guards PASS: default on, exact 1 on, explicit 0/malformed off without writes, both hashes, 20 occupied bytes, real tail geometry, bounds/range, exact boundaries, three branches, eight policy combinations, all six menu/thermal/hardening orders (24 combinations)");
    return 0;
}
