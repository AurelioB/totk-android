/* Host-only production guard/publication fixture; synthetic module, no game. The site words come from the player's
   main (verify_v143_drs_framegen.py) or, built with -DCODE_GUARD_STANDIN, are stand-ins (fixture_seeds.h). */
#include <assert.h>
#include <setjmp.h>
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
static jmp_buf failure;
_Noreturn void runtime_fail(const char *message){(void)message;longjmp(failure,1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t value){(void)value;}
void dm_gfx_set_flag(const char *name,int value){(void)name;(void)value;}
void runtime_survey_init(Module *m){(void)m;}
static const uint64_t seed[]={0x584e84,0x584e88,0x584e8c,0x584e90,0x584790,0x584b68,0xa1bb0,0xa1bb4,
 0x2d02094,0x5848a8,0x5848ac,0x5848b0};
static Module module;
static unsigned char *before;
static void reset(void){
 module.text_end=0x690100;module.file_size=0x2d03000;module.size=0x2d08000;
 memset(module.base,0,module.size);
 for(size_t i=0;i<sizeof seed/sizeof *seed;i++)fixture_seed_at(module.base,seed[i]);
 unsetenv("TOTK_DRS_FRAMEGEN");unsetenv("TOTK_FRAMEGEN_DIAG");unsetenv("TOTK_DRS60");
 unsetenv("TOTK_ASPECT");g_aspect_slot=NULL;
 setenv("TOTK_FPS60","0",1);setenv("TOTK_FRAMEGEN","locked",1);
 drs_framegen_cap=NULL;drs_framegen_sample=NULL;
}
static void reject(void){
 memcpy(before,module.base,module.size);
 if(!setjmp(failure)){patch_drs_framegen(&module);assert(!"DRS framegen accepted invalid input");}
 assert(!memcmp(before,module.base,module.size));
}
static void unchanged(void){
 memcpy(before,module.base,module.size);patch_drs_framegen(&module);
 assert(!memcmp(before,module.base,module.size));assert(!drs_framegen_cap&&!drs_framegen_sample);
}
static void dump(const char *path){
 FILE *f=fopen(path,"wb");assert(f&&fwrite(module.base,1,module.size,f)==module.size&&!fclose(f));
}
int main(int argc,char **argv){
 module=(Module){.name="main"};
 module.base=aligned_alloc(0x4000,0x2d08000);before=malloc(0x2d08000);assert(module.base&&before);
 reset();unchanged();module.base[seed[0]]^=1;unchanged();
 setenv("TOTK_DRS_FRAMEGEN","0",1);unchanged();
 const char *modes[]={"0","1","interp","hybrid",""};
 for(size_t i=0;i<sizeof modes/sizeof *modes;i++){
  reset();setenv("TOTK_DRS_FRAMEGEN","1",1);setenv("TOTK_FRAMEGEN",modes[i],1);unchanged();
 }
 reset();setenv("TOTK_DRS_FRAMEGEN","1",1);setenv("TOTK_FPS60","1",1);unchanged();
 unsetenv("TOTK_FPS60");unchanged();
 const char *bad[]={"","2","true","-1","1 "," 1"};
 for(size_t i=0;i<sizeof bad/sizeof *bad;i++){reset();setenv("TOTK_DRS_FRAMEGEN",bad[i],1);reject();}
 for(size_t i=0;i<sizeof seed/sizeof *seed;i++){
  reset();setenv("TOTK_DRS_FRAMEGEN","1",1);module.base[seed[i]]^=1;reject();
 }
 for(unsigned i=0;i<64;i++){
  reset();setenv("TOTK_DRS_FRAMEGEN","1",1);module.base[0x690260+i]=1;reject();
 }
 for(unsigned i=48;i<72;i++){
  if(i>=56&&i<64)continue; /* existing aspect slot and its padding are not ours */
  reset();setenv("TOTK_DRS_FRAMEGEN","1",1);module.base[0x2d04000+i]=1;reject();
 }
 reset();setenv("TOTK_DRS_FRAMEGEN","1",1);module.file_size=0x2d02094;reject();
 reset();setenv("TOTK_DRS_FRAMEGEN","1",1);module.text_end=0x584e90;reject();
 reset();setenv("TOTK_DRS_FRAMEGEN","1",1);module.size=0x2d04040;reject();
 reset();setenv("TOTK_DRS_FRAMEGEN","1",1);patch_drs_framegen(&module);
 assert(atomic_load(drs_framegen_cap)==0x7f7fffff&&!drs_framegen_sample);
 runtime_drs_framegen_budget(25.5);float stress=153;uint32_t bits;memcpy(&bits,&stress,4);
 assert(atomic_load(drs_framegen_cap)==bits);
 const double invalid[]={0,23.99,30.01,NAN,INFINITY,FLT_MAX};
 for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
  runtime_drs_framegen_budget(invalid[i]);assert(atomic_load(drs_framegen_cap)==0x7f7fffff);
 }
 reject();
 /* Existing aspect ownership must survive both cap publication and the
    diagnostic snapshot; a nonzero aspect must not be mistaken for padding. */
 reset();setenv("TOTK_DRS_FRAMEGEN","1",1);setenv("TOTK_FRAMEGEN_DIAG","1",1);
 float wide=7.0f/3.0f;memcpy(module.base+0x2d04000+ASPECT_DATA_SLOT,&wide,4);
 patch_drs_framegen(&module);runtime_drs_framegen_budget(25.5);
 atomic_store(drs_framegen_sample,UINT64_C(0xffffffff3f800000));
 assert(!memcmp(module.base+0x2d04000+ASPECT_DATA_SLOT,&wide,4));
 if(argc==3){
  reset();setenv("TOTK_DRS_FRAMEGEN","1",1);patch_drs_framegen(&module);dump(argv[1]);
  reset();setenv("TOTK_FRAMEGEN_DIAG","1",1);patch_drs_framegen(&module);
  assert(!drs_framegen_cap&&drs_framegen_sample);
  uint32_t first=fixture_seed(seed[0]);assert(!memcmp(module.base+0x584e84,&first,4));
  const uint64_t aspect_site=0x1000;uint32_t divide=0x1e221820; /* fdiv s0,s1,s2 */
  memcpy(module.base+aspect_site,&divide,4);
  aspect_install(&module,"fixture",&aspect_site,1,0x690130,0x2d04000);
  dump(argv[2]);
 }
 free(before);free(module.base);
 puts("DRS framegen guards PASS: disabled original bytes, incompatible modes, 12 corruptions, 64 code/16 data padding bytes, malformed values, bounds, native fallback publication, independent DIAG, disjoint live aspect ownership");
 return 0;
}
