/* Host fixture exercises the production optional patch functions, including
 * verify-all-before-any-write failures. No guest execution. The site words come
 * from the player's main (verify_v143_render.py) or, built with
 * -DCODE_GUARD_STANDIN, are stand-ins (fixture_seeds.h). */
#include <setjmp.h>
#include <assert.h>
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
static jmp_buf failure;
_Noreturn void runtime_fail(const char *s){(void)s;longjmp(failure,1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t m){(void)m;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
static const uint64_t seeds[]={
 0x6835a4,0x6835a8,0x683604,0x683608,0x68360c,0x683610,0x683614,0x683618,
 0x68361c,0x683620,0x683624,0x683628,0x68362c,0x683630,0x683634,0x683638,
 0x628c20,0x628c28,0x628c30,0x628c34,0x628c38,0x628c3c,
 0x586944,
};
static Module m;
static unsigned char *before;
static void reset(void){
 memset(m.base,0,m.size);
 for(size_t i=0;i<sizeof seeds/sizeof *seeds;i++)fixture_seed_at(m.base,seeds[i]);
 unsetenv("TOTK_SHADOW_RES");unsetenv("TOTK_FAR_CLAMP");unsetenv("TOTK_FXAA");
}
static void reject(void (*patch)(Module *)){
 memcpy(before,m.base,m.size);
 if(!setjmp(failure)){patch(&m);assert(!"patch unexpectedly accepted mismatch");}
 assert(!memcmp(before,m.base,m.size));
}
static void corrupt_each(void (*patch)(Module *),const char *name,const char *value,size_t lo,size_t hi){
 for(size_t i=lo;i<hi;i++){
  reset();setenv(name,value,1);m.base[seeds[i]]^=1;reject(patch);
 }
}
static uint32_t word(uint64_t off){uint32_t v;memcpy(&v,m.base+off,4);return v;}
int main(int argc,char **argv){
 m=(Module){.name="main",.size=0x6a0000,.file_size=0x698000,.text_end=0x690100};
 m.base=calloc(1,m.size);before=malloc(m.size);assert(m.base&&before);
 reset();memcpy(before,m.base,m.size);
 patch_shadow_res(&m);patch_far_clamp(&m);patch_fxaa(&m);
 assert(!memcmp(before,m.base,m.size));
 setenv("TOTK_SHADOW_RES","0",1);setenv("TOTK_FAR_CLAMP","0",1);setenv("TOTK_FXAA","1",1);
 patch_shadow_res(&m);patch_far_clamp(&m);patch_fxaa(&m);assert(!memcmp(before,m.base,m.size));
 corrupt_each(patch_shadow_res,"TOTK_SHADOW_RES","288",0,16);
 corrupt_each(patch_far_clamp,"TOTK_FAR_CLAMP","12000",16,22);
 corrupt_each(patch_fxaa,"TOTK_FXAA","0",22,23);
 const char *bad_far[]={"-1","nan","inf","12000m","1e100","1e-100","oops"};
 for(size_t i=0;i<sizeof bad_far/sizeof *bad_far;i++){reset();setenv("TOTK_FAR_CLAMP",bad_far[i],1);reject(patch_far_clamp);}
 reset();setenv("TOTK_SHADOW_RES","320",1);reject(patch_shadow_res);
 reset();setenv("TOTK_FXAA","false",1);reject(patch_fxaa);
 reset();setenv("TOTK_FAR_CLAMP","12000",1);m.base[0x690220]=1;reject(patch_far_clamp);
 reset();setenv("TOTK_FAR_CLAMP","12000",1);m.text_end=0x628c38;reject(patch_far_clamp);m.text_end=0x690100;
 reset();setenv("TOTK_SHADOW_RES","288",1);m.text_end=0x683614;reject(patch_shadow_res);m.text_end=0x690100;
 reset();setenv("TOTK_FXAA","0",1);m.text_end=0x586944;reject(patch_fxaa);m.text_end=0x690100;
 reset();setenv("TOTK_SHADOW_RES","256",1);patch_shadow_res(&m);
 assert(word(0x6835a4)==0x5280200b&&word(0x6835a8)==0x14000017);
 assert(word(0x628c30)==fixture_seed(0x628c30)&&word(0x586944)==fixture_seed(0x586944));
 reset();setenv("TOTK_SHADOW_RES","288",1);setenv("TOTK_FAR_CLAMP","12000",1);setenv("TOTK_FXAA","0",1);
 patch_shadow_res(&m);patch_far_clamp(&m);patch_fxaa(&m);
 assert(word(0x6835a4)==0x5280240b&&word(0x6835a8)==0x14000017&&word(0x586944)==0x3906301f);
 assert(word(0x628c30)==branch(0x628c30,0x690220));
 /* Retry must refuse patched code, never silently stack veneers. */
 reject(patch_shadow_res);reject(patch_far_clamp);reject(patch_fxaa);
 if(argc==2){FILE *f=fopen(argv[1],"wb");assert(f);assert(fwrite(m.base,1,m.size,f)==m.size);assert(!fclose(f));}
 free(before);free(m.base);puts("render patch guards: PASS (23 site corruptions, defaults, malformed switches, bounds, padding, independent selection)");return 0;
}
