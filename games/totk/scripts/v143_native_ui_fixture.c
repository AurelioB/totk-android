/* Host fixture: production output/scene policy and verified canvas veneers. The site words come from the player's
   main (verify_v143_native_ui.py) or, built with -DCODE_GUARD_STANDIN, are stand-ins (fixture_seeds.h). */
#include <assert.h>
#include <setjmp.h>
#include "../runtime/game_patches_v143.c"
#include "fixture_seeds.h"
static jmp_buf failure;
static int catching;
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"%s\n",message);if(catching)longjmp(failure,1);exit(1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t value){(void)value;}
void dm_gfx_set_flag(const char *name,int value){(void)name;(void)value;}
void runtime_survey_init(Module *m){(void)m;}
static const uint64_t seeds[]={0xbdb578,0xbdb584,0x3dcb28,0x3dcb2c,0x3dcb30,0x585e48,0x585e50,0x585e54,0x585e60};
int main(int argc,char **argv){
 unsigned w=0,h=0,sw=0,sh=0;int output=runtime_output_res(&w,&h),scene=runtime_render_res(&sw,&sh);
 uint64_t extra=runtime_output_res_extra_bytes();
 if(argc>1&&!strcmp(argv[1],"query")){
  printf("{\"output_active\":%d,\"output\":[%u,%u],\"scene_active\":%d,\"scene\":[%u,%u],\"extra_bytes\":%llu}\n",output,w,h,scene,sw,sh,(unsigned long long)extra);return 0;
 }
 Module m={.name="main",.text_end=0xbe0100,.file_size=0xbf0000,.size=0xbf4000};
 m.base=aligned_alloc(0x4000,m.size);assert(m.base);memset(m.base,0,m.size);
 for(size_t i=0;i<sizeof seeds/sizeof *seeds;i++)fixture_seed_at(m.base,seeds[i]);
 if(argc>2&&!strcmp(argv[1],"corrupt")){
  unsigned n=(unsigned)strtoul(argv[2],NULL,0);assert(n<sizeof seeds/sizeof *seeds);
  m.base[seeds[n]]^=1;uint8_t *before=malloc(m.size);assert(before);memcpy(before,m.base,m.size);
  catching=1;if(!setjmp(failure)){if(n<5)patch_output_res(&m);else patch_render_res(&m);assert(!"corrupted original accepted");}
  assert(!memcmp(before,m.base,m.size));free(before);free(m.base);puts("{\"refused_before_write\":true}");return 0;
 }
 patch_output_res(&m);patch_render_res(&m);
 if(argc>2&&!strcmp(argv[1],"dump")){FILE *f=fopen(argv[2],"wb");assert(f&&fwrite(m.base,1,m.size,f)==m.size&&!fclose(f));}
 float value[2];memcpy(value,m.base+0xbf0010,8);
 printf("{\"output_active\":%d,\"output\":[%u,%u],\"scene\":[%.9g,%.9g],\"extra_bytes\":%llu,\"canvas\":%llu}\n",output,w,h,value[0],value[1],(unsigned long long)extra,(unsigned long long)runtime_output_size_current());
 free(m.base);return 0;
}
