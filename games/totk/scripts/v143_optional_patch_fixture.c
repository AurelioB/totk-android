/* Real 1.4.3 Android rewrite geometry; no guest bytes are stored here.
 * Seeds come from the player's authenticated NSO, or dataless stand-ins. */
#define TOTK_THERMAL_PATCH_TEST 1
#include <assert.h>
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
_Noreturn void runtime_fail(const char *why){fprintf(stderr,"unexpected fatal installer: %s\n",why);abort();}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t v){(void)v;}
void dm_gfx_set_flag(const char *n,int v){(void)n;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
enum { TEXT=0x2bb9c00, FILE_BYTES=0x451cdd8, DATA=0x4520000, SIZE=0x4524000 };
static const uint64_t seeds[]={
    0x105bbcc,0x105bbd0,
    0xa7ac4,0x95dd24,0x6155e4,0x615660,0x45c700,0x2b23c7c,0x42c3bc,0xa5a188,0x2db238,0xa5a670,
    0xa5a18c,0xa5a660,0xa5a664,0xa5a668,0xa5a66c,0xa5a674,
    0x584e84,0x584e88,0x584e8c,0x584e90,0x584790,0x584b68,0xa1bb0,0xa1bb4,0x2d02094,
    0x5848a8,0x5848ac,0x5848b0,0x2db2b8,0x2db354,0x2db3f0,
};
static int (*const installers[])(Module *)={patch_menu_capture,patch_thermal_step,patch_fps60_hardening,
                                         patch_drs60,patch_drs_framegen,patch_fps60_hysteresis};
static Module module;
static uint8_t *image,*pristine,*before;
static unsigned matrices,refusals,references;
static uint32_t word(uint64_t at){uint32_t v;memcpy(&v,image+at,4);return v;}
static void put(uint64_t at,uint32_t v){memcpy(image+at,&v,4);}
static void reset(void){
    module=(Module){.name="main",.base=image,.text_end=TEXT,.ro_off=TEXT+1024,
                    .file_size=FILE_BYTES,.size=SIZE};
    memset(image+TEXT,0,1024);memset(image+DATA,0,0x4000);
    for(unsigned i=0;i<sizeof seeds/sizeof *seeds;i++)fixture_seed_at(image,seeds[i]);
    for(unsigned i=0;i<sizeof fps60/sizeof *fps60;i++)put(fps60[i].off,fps60[i].replacement);
    cutscene_stamp=NULL;cutscene_markers=NULL;cutscene_reason=NULL;cutscene_guest=NULL;
    drs_framegen_cap=NULL;drs_framegen_sample=NULL;
}
static void config(unsigned profile,unsigned mask){
    setenv("TOTK_FPS60",profile==1?"1":"0",1);
    setenv("TOTK_FRAMEGEN",profile==2?"locked":"0",1);
    setenv("TOTK_MENU_CAPTURE_FIX",mask&1?"1":"0",1);
    setenv("TOTK_THERMAL_STEP",mask&2?"1":"0",1);
    setenv("TOTK_FPS60_HARDENING",mask&4?"1":"0",1);
    setenv("TOTK_FPS60_FADE_CLAMP",mask&4?"1":"0",1);
    setenv("TOTK_DRS60",mask&8?"1":"0",1);
    setenv("TOTK_DRS_FRAMEGEN",mask&8?"1":"0",1);
    setenv("TOTK_FRAMEGEN_DIAG",mask&8?"1":"0",1);
    setenv("TOTK_FPS60_HYSTERESIS","1",1);
}
/* All writers are confined to these recorded regions. A final whole-image
 * comparison checks the untouched canary, without copying 72 MB per permutation. */
typedef struct { uint8_t tail[1024],data[160];uint32_t sites[sizeof seeds/sizeof *seeds]; } Snapshot;
static void snapshot(Snapshot *s){
    memcpy(s->tail,image+TEXT,sizeof s->tail);memcpy(s->data,image+DATA,sizeof s->data);
    for(unsigned i=0;i<sizeof seeds/sizeof *seeds;i++)s->sites[i]=word(seeds[i]);
}
static int64_t signed_bits(uint32_t value,unsigned bits){return (int64_t)(value^(1u<<(bits-1)))-(1u<<(bits-1));}
static void check_refs(void){
    for(uint64_t at=TEXT;at<TEXT+1024;at+=4){
        uint32_t w=word(at);int64_t target=-1;
#ifdef CODE_GUARD_STANDIN
        /* Guard stand-ins are deliberately arbitrary words, not ARM code. */
        int standin=0;
        for(unsigned i=0;i<sizeof seeds/sizeof *seeds;i++)if(w==fixture_seed(seeds[i]))standin=1;
        if(standin)continue;
#endif
        if((w&0x7c000000u)==0x14000000u)target=(int64_t)at+signed_bits(w&0x3ffffff,26)*4;
        else if((w&0xff000010u)==0x54000000u||(w&0x7e000000u)==0x34000000u)
            target=(int64_t)at+signed_bits((w>>5)&0x7ffff,19)*4;
        else if((w&0x7e000000u)==0x36000000u)target=(int64_t)at+signed_bits((w>>5)&0x3fff,14)*4;
        else if((w&0x3b000000u)==0x18000000u)target=(int64_t)at+signed_bits((w>>5)&0x7ffff,19)*4;
        else if((w&0x9f000000u)==0x90000000u){
            target=(int64_t)(at&~4095ull)+signed_bits(((w>>5)&0x7ffff)*4+((w>>29)&3),21)*4096;
            assert(target==DATA); /* every generated ADRP owns the private page */
        }
        if(target!=-1){assert(target>=0&&(uint64_t)target<SIZE);references++;}
    }
    if(word(TEXT+916))for(unsigned i=0;i<3;i++){
        uint64_t at=TEXT+916+i*36;uint32_t adr=word(at+8),load=word(at+12);
        int64_t page=(int64_t)((at+8)&~4095ull)+signed_bits(((adr>>5)&0x7ffff)*4+((adr>>29)&3),21)*4096;
        assert((adr&0x9f00001f)==0x90000011&&(load&0xffc003ff)==0xb9400231);
        assert(page+((load>>10)&0xfff)*4==DATA+144);
    }
}
static void permutation(unsigned depth,unsigned used,unsigned order[5],unsigned profile,unsigned mask){
    if(depth<5){
        for(unsigned i=0;i<5;i++)if(!(used&(1u<<i))){order[depth]=i;permutation(depth+1,used|(1u<<i),order,profile,mask);}
        return;
    }
    reset();config(profile,mask);
    if(profile==1)assert(patch_fps60_hysteresis(&module)==1);
    for(unsigned i=0;i<5;i++){
        Snapshot a,b;snapshot(&a);int result=installers[order[i]](&module);assert(result>=0);snapshot(&b);
        if(!result)assert(!memcmp(&a,&b,sizeof a));
    }
    assert((word(0x105bbcc)!=fixture_seed(0x105bbcc))==!!(mask&1));
    assert(!!word(TEXT+916)==(profile==1&&!!(mask&2)));
    assert(!!word(TEXT+416)==(profile==1));
    assert(!!word(TEXT+320)==(profile==1&&!!(mask&8)));
    assert(!!word(TEXT+352)==(profile==2&&!!(mask&8)));
    check_refs();matrices++;
}
static void refused(unsigned which){
    memcpy(before,image,SIZE);assert(installers[which](&module)==-1);
    assert(!memcmp(before,image,SIZE));refusals++;
}
static void guard_cases(void){
    const uint64_t sites[]={0x105bbcc,0x2db208,0xa7ac4,0x584e84,0x584e84,0x2db2b8};
    const uint64_t pads[]={TEXT+276,TEXT+1023,TEXT+915,TEXT+339,TEXT+415};
    for(unsigned i=0;i<6;i++){
        unsigned profile=i==4?2:1;reset();config(profile,15);image[sites[i]]^=1;refused(i);
        if(i<5){reset();config(profile,15);image[pads[i]]=1;refused(i);}
        reset();config(profile,15);module.text_end=UINT64_MAX;refused(i);
        reset();config(profile,15);module.size=TEXT;refused(i);
        reset();config(profile,15);module.file_size=UINT64_MAX;refused(i);
        reset();config(profile,15);module.text_end=0x1000;refused(i);
        reset();config(profile,15);module.base=(uint8_t *)(UINTPTR_MAX-15);refused(i);
        if(i==1||i==2||i==4){
            reset();config(profile,15);module.file_size=0x200000000ull;module.size=module.file_size+0x4000;refused(i);
        }
        reset();config(profile,15);assert(installers[i](&module)==1);refused(i);
    }
}
static void real_startup(const char *path){
    FILE *f=fopen(path,"rb");assert(f);assert(fread(pristine,1,FILE_BYTES,f)==FILE_BYTES);
    assert(fgetc(f)==EOF&&!fclose(f));
    for(unsigned profile=0;profile<2;profile++){
        reset();memcpy(image,pristine,FILE_BYTES);config(profile,7);
        setenv("TOTK_DRS60","0",1);setenv("TOTK_DRS_FRAMEGEN","0",1);setenv("TOTK_FRAMEGEN_DIAG","0",1);
        runtime_game_patches(&module);
        assert(patch_thermal_step(&module)==(profile==1));
        assert(word(0x105bbcc)!=fixture_seed(0x105bbcc));check_refs();
    }
}
int main(int argc,char **argv){
    image=aligned_alloc(0x4000,SIZE);pristine=calloc(1,SIZE);before=malloc(SIZE);assert(image&&pristine&&before);
    memset(image,0,SIZE);reset();memcpy(pristine,image,SIZE);
    unsigned order[5];for(unsigned profile=0;profile<3;profile++)for(unsigned mask=0;mask<16;mask++)
        permutation(0,0,order,profile,mask);
    guard_cases();reset();assert(!memcmp(pristine,image,SIZE));
    if(argc==2)real_startup(argv[1]);
    printf("optional patch matrix PASS: %u combinations/orders, %u atomic nonfatal refusals, %u PC-relative references; file_size=%#x data=%#x\n",
           matrices,refusals,references,FILE_BYTES,DATA);
    free(before);free(pristine);free(image);return 0;
}
