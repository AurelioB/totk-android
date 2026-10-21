/* Apply production v143 code patches to a private local image for instruction
 * emulation. No guest entry, graphics, services, or user saves are executed. */
#include "runtime.h"
#include "game_patches.h"
#include "frame_controller.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t m){(void)m;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
/* Feed the native getter the exact word emitted by Unicorn's ARM64 STR. The
 * aligned volatile store models that machine-code producer; the production
 * getter, pinning and update/reset paths are linked without adaptive runtime. */
static void print_take(const char *name,volatile uint32_t *capture){
 float taken=runtime_frame_step_seconds();
 float again=runtime_frame_step_seconds();
 printf("%s[%.9g,%.9g,%u]",name,taken,again,*capture);
}
static void consumed_cases(unsigned char *page,int argc,char **argv){
 volatile uint32_t *capture=(volatile uint32_t *)(void *)(page+12);
 printf("{\"initial\":%.9g,\"initialized_capture\":%u,\"samples\":[",runtime_frame_step_seconds(),*capture);
 for(int i=0;i<argc;i++){
  *capture=(uint32_t)strtoul(argv[i],NULL,16);
  print_take(i?",":"",capture);
 }
 printf("],\"reset\":{");
 *capture=0x3fc00000;runtime_frame_step_update(1000000000ULL);print_take("\"startup\":",capture);
 *capture=0x3fc00000;runtime_frame_step_update(2000000000ULL);print_take(",\"pause\":",capture);
 *capture=0x3fc00000;runtime_frame_step_update(2000500000ULL);print_take(",\"too_short\":",capture);
 *capture=0x3fc00000;runtime_frame_step_update(2000000000ULL);print_take(",\"backward\":",capture);
 *capture=0x3fc00000;runtime_frame_step_update(2000000000ULL);print_take(",\"duplicate\":",capture);
 *capture=0x3fc00000;runtime_frame_step_update(0);print_take(",\"zero_clock\":",capture);
 *capture=0x3fc00000;runtime_frame_step_pin(10);print_take(",\"pin\":",capture);
 *capture=0x3fc00000;print_take(",\"while_pinned\":",capture);
 *capture=0x3fc00000;runtime_frame_step_update(2025000000ULL);print_take(",\"pinned_update\":",capture);
 *capture=0x3fc00000;runtime_frame_step_pin(0);print_take(",\"unpin\":",capture);
 *capture=0x3fc00000;print_take(",\"fresh\":",capture);
 printf("}}\n");
 *capture=0;
}
int main(int argc,char **argv){
 if(argc<4)return 2;
 FILE *f=fopen(argv[1],"rb");struct stat st;
 if(!f||fstat(fileno(f),&st))return 2;
 size_t data=((size_t)st.st_size+0x3fff)&~(size_t)0x3fff,size=data+0x4000;
 unsigned char *base=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
 if(base==MAP_FAILED||fread(base,1,(size_t)st.st_size,f)!=(size_t)st.st_size)return 2;fclose(f);
 Module m={.name="main",.base=base,.file_size=(size_t)st.st_size,.size=size,.text_end=strtoull(argv[3],NULL,0)};
 runtime_game_patches(&m);
 if(argc>4&&!strcmp(argv[4],"--consume"))consumed_cases(base+data,argc-5,argv+5);
 else{
 runtime_frame_step_pin(10.0);
 float values[3];memcpy(values,base+data,sizeof values);
 printf("{\"controller_offset\":%llu,\"pin\":[%.9g,%.9g,%.9g]",
        (unsigned long long)(frame_controller_v143_address(0x1000)-0x1000),values[0],values[1],values[2]);
 runtime_frame_step_pin(0);runtime_frame_step_update(1000000000ULL);runtime_frame_step_update(1025000000ULL);
 memcpy(values,base+data,sizeof values);printf(",\"sample\":[%.9g,%.9g,%.9g]",values[0],values[1],values[2]);
 /* Repeating present intervals in ns; a long gap resets the five-sample ring.
    Each pattern measures 6000 presents, summing the production stored step. */
 printf(",\"realtime\":[");
 for(int a=4;a<argc;a++){
  uint64_t pattern[16];unsigned n=0;char *p=argv[a];
  while(*p&&n<16){pattern[n++]=strtoull(p,&p,10);if(*p==',')p++;}
  if(!n)return 2;
  uint64_t t=1000000000ULL*(uint64_t)(a+1);runtime_frame_step_update(t);t+=1000000000ULL;runtime_frame_step_update(t);
  double real=0,game=0;
  for(unsigned i=0;i<6000;i++){t+=pattern[i%n];runtime_frame_step_update(t);memcpy(values,base+data,sizeof values);real+=pattern[i%n]*1e-9;game+=values[1];}
  printf("%s[%.9f,%.9f]",a>4?",":"",real,game);
 }
 printf("]}\n");
 }
 f=fopen(argv[2],"wb");if(!f||fwrite(base,1,size,f)!=size||fclose(f))return 2;
 munmap(base,size);return 0;
}
