/* Host-only verification of production DRS60 guards; no game execution. The site words come from the player's main
   (verify_v143_drs60.py) or, built with -DCODE_GUARD_STANDIN, are stand-ins (fixture_seeds.h). */
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
static const uint64_t seed[]={0x584e84,0x584e88,0x584e8c,0x584e90,0x584790,0x584b68,0xa1bb0,0xa1bb4,0x2d02094};
static Module module;
static unsigned char *before;
static void reset(void){
 memset(module.base,0,module.size);
 for(size_t i=0;i<sizeof seed/sizeof *seed;i++)fixture_seed_at(module.base,seed[i]);
 unsetenv("TOTK_DRS60");unsetenv("TOTK_FPS60");
}
static void reject(void){
 memcpy(before,module.base,module.size);
 if(!setjmp(failure)){patch_drs60(&module);assert(!"DRS60 accepted invalid input");}
 assert(!memcmp(before,module.base,module.size));
}
int main(int argc,char **argv){
 module=(Module){.name="main",.text_end=0x690100,.file_size=0x2d03000,.size=0x2d04000};
 module.base=malloc(module.size);before=malloc(module.size);assert(module.base&&before);
 reset();memcpy(before,module.base,module.size);patch_drs60(&module);assert(!memcmp(before,module.base,module.size));
 setenv("TOTK_DRS60","0",1);patch_drs60(&module);assert(!memcmp(before,module.base,module.size));
 setenv("TOTK_DRS60","1",1);setenv("TOTK_FPS60","0",1);
 module.base[seed[0]]^=1;memcpy(before,module.base,module.size);
 patch_drs60(&module);assert(!memcmp(before,module.base,module.size));
 for(size_t i=0;i<sizeof seed/sizeof *seed;i++){
  reset();setenv("TOTK_DRS60","1",1);setenv("TOTK_FPS60","1",1);module.base[seed[i]]^=1;reject();
 }
 for(unsigned i=0;i<20;i++){
  reset();setenv("TOTK_DRS60","1",1);module.base[0x690240+i]=1;reject();
 }
 const char *bad[]={"2","true","-1","1 "," 1"};
 for(size_t i=0;i<sizeof bad/sizeof *bad;i++){reset();setenv("TOTK_DRS60",bad[i],1);reject();}
 reset();setenv("TOTK_DRS60","1",1);module.file_size=0x2d02094;reject();module.file_size=0x2d03000;
 reset();setenv("TOTK_DRS60","1",1);module.text_end=0x584e90;reject();module.text_end=0x690100;
 reset();setenv("TOTK_DRS60","1",1);setenv("TOTK_FPS60","1",1);patch_drs60(&module);
 uint32_t jump;memcpy(&jump,module.base+0x584e84,4);assert(jump==branch(0x584e84,0x690240));
 for(size_t i=1;i<sizeof seed/sizeof *seed;i++){uint32_t w=fixture_seed(seed[i]);assert(!memcmp(module.base+seed[i],&w,4));}
 reject();
 if(argc==2){FILE *f=fopen(argv[1],"wb");assert(f&&fwrite(module.base,1,module.size,f)==module.size&&!fclose(f));}
 free(before);free(module.base);puts("DRS60 guards PASS: default off, FPS60=0 unchanged, 9 word/data corruptions, 20 padding bytes, invalid values and bounds");
 return 0;
}
