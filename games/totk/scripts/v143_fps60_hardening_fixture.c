/* Production patcher and presenter policy, private caller-owned image only.
 * The host clock is deterministic; no guest entry, GPU or sleep is executed. */
#include "runtime.h"
#include "game_patches.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
static unsigned cutscene_log_count;
static int fixture_fprintf(FILE *stream,const char *format,...){
    if(!strncmp(format,"[fps60-cutscene]",sizeof "[fps60-cutscene]"-1))cutscene_log_count++;
    va_list args;va_start(args,format);int result=vfprintf(stream,format,args);va_end(args);return result;
}
static uint64_t clock_ns=1000000000ull;
static int fixture_clock_gettime(clockid_t clock,struct timespec *t){
    assert(clock==CLOCK_MONOTONIC);t->tv_sec=clock_ns/1000000000ull;t->tv_nsec=clock_ns%1000000000ull;return 0;
}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t m){(void)m;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
#define clock_gettime fixture_clock_gettime
#define fprintf fixture_fprintf
#include "../runtime/game_patches_v143.c"
#undef clock_gettime
#undef fprintf

/* The runner extracts these three complete functions verbatim from hle_nvn.c.
 * These are external services, not copies of interval-selection policy. */
typedef struct { int interval; } win_t;
enum { MFX_OFF, MFX_HALF, MFX_30, MFX_MAX };
static int g_hz=60,g_paced_hz=60,g_display_mode,g_uncapped;
static unsigned g_fps_auto_ticks;
static int fixed_clock,detclock,mfx,uncapped_interval;
static uint64_t display_refresh=16666667ull;
static int runtime_android_fps_auto_active(void){return 0;}
static int runtime_fps_hybrid_selected(void){return 0;}
static int runtime_fps_adaptive_selected(void){return 0;}
static int acquire_clock_on(void){return fixed_clock;}
static int dm_detclock_enabled(void){return detclock;}
static int mfx_enabled(void){return mfx;}
static int uncapped_now(void){return g_uncapped;}
static int uncapped_interval_on(void){return uncapped_interval;}
static int mfx_own_clock(void){return g_paced_hz%30!=0;}
static void present_cfg(const win_t *o){(void)o;}
static uint64_t runtime_android_display_period(unsigned ticks){return display_refresh*ticks;}
static uint64_t runtime_android_acquire_period(unsigned ticks){return display_refresh*ticks;}
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#define LOG(...) ((void)0)
#include "fps60_hardening_present.inc"
#undef LOG

static void policy_checks(int on){
    win_t w={.interval=1};
    assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
    if(on){
        /* Ordinary update/type-query entries may fire every frame. Their
         * individual diagnostics are not authority for either pacing path. */
        for(unsigned i=0;i<3;i++)atomic_store(cutscene_markers+i,clock_ns);
        assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        assert(present_ticks(&w)==1);
        unsigned logs=cutscene_log_count;
        for(unsigned i=0;i<100;i++){
            clock_ns++;
            for(unsigned marker=0;marker<3;marker++)atomic_store(cutscene_markers+marker,clock_ns);
            assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        }
        assert(cutscene_log_count==logs); /* refreshed stamps alone must not log */
        clock_ns=1000000000ull;
        for(unsigned marker=0;marker<3;marker++)atomic_store(cutscene_markers+marker,clock_ns);
        atomic_store(cutscene_reason,CUTSCENE_CAMERA_ACTIVE);
        atomic_store(cutscene_markers+3,clock_ns);
        atomic_store(cutscene_stamp,clock_ns);
        assert(runtime_cutscene_present_ticks(1,display_refresh)==2);
        logs=cutscene_log_count;
        for(unsigned i=0;i<100;i++){
            clock_ns++;
            atomic_store(cutscene_markers+3,clock_ns);
            atomic_store(cutscene_stamp,clock_ns);
            assert(runtime_cutscene_present_ticks(1,display_refresh)==2);
        }
        assert(cutscene_log_count==logs);
        clock_ns=1000000000ull;atomic_store(cutscene_markers+3,clock_ns);atomic_store(cutscene_stamp,clock_ns);
    }
    for(unsigned ticks=0;ticks<4;ticks++){
        unsigned expected=on&&ticks<2?2:ticks;
        assert(runtime_cutscene_present_ticks(ticks,display_refresh)==expected);
        assert(runtime_cutscene_present_ticks(ticks,8333333ull)==(on?4:ticks));
    }
    if(on){
        clock_ns=999999999ull;assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        clock_ns=1199999999ull;assert(runtime_cutscene_present_ticks(1,display_refresh)==2);
        clock_ns=1200000000ull;assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        /* A later admitted camera update refreshes the shared timestamp. */
        atomic_store(cutscene_stamp,clock_ns);assert(runtime_cutscene_present_ticks(1,display_refresh)==2);
        clock_ns+=200000000ull;assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        clock_ns=1000000000ull;atomic_store(cutscene_stamp,clock_ns);
    }
    /* The real HLE selector, including paths which used to ignore guest=2.
     * At 60 Hz active cutscenes must pace 30 real presents, not merely write 2
     * to the guest. At 120 Hz this requires FOUR physical display ticks. */
    for(int hz=60;hz<=120;hz+=60)for(int display=0;display<2;display++)
    for(int uncapped=0;uncapped<2;uncapped++)for(int fixed=0;fixed<2;fixed++){
        g_hz=g_paced_hz=hz;display_refresh=hz==60?16666667ull:8333333ull;
        g_display_mode=display;g_uncapped=uncapped;fixed_clock=fixed;uncapped_interval=1;
        unsigned want=on?(unsigned)(hz/30):1;
        assert(present_ticks(&w)==want);
        uint64_t period=present_period(&w);
        assert(llabs((long long)period-(on?33333333ll:1000000000ll/hz))<=2);
        if(on){
            clock_ns=1200000000ull;
            assert(present_ticks(&w)==1);
            assert(llabs((long long)present_period(&w)-1000000000ll/hz)<=1);
            clock_ns=1000000000ull;
        }
    }
    /* MFX and deterministic pacing remain authoritative. A framegen env
     * disables only the cutscene floor; fade clamps remain independent. */
    fixed_clock=g_display_mode=g_uncapped=0;g_hz=g_paced_hz=60;display_refresh=16666667ull;
    mfx=MFX_HALF;assert(present_ticks(&w)==2);
    mfx=MFX_30;assert(present_period(&w)==1000000000ull/30);
    mfx=MFX_MAX;assert(present_ticks(&w)==1);
    mfx=MFX_OFF;detclock=1;assert(present_ticks(&w)==1);detclock=0;
    if(on){
        /* The completion hook, or a null instruction entry, revokes the same
         * stamp used by the guest join and by the production HLE selector. */
        atomic_store(cutscene_reason,CUTSCENE_CAMERA_DONE);
        atomic_store(cutscene_stamp,0);
        assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        assert(present_ticks(&w)==1);
        atomic_store(cutscene_reason,CUTSCENE_CAMERA_EMPTY);
        assert(runtime_cutscene_present_ticks(1,display_refresh)==1);
        for(unsigned i=0;i<4;i++)atomic_store(cutscene_markers+i,0);
        atomic_store(cutscene_reason,CUTSCENE_NONE);
        atomic_store(cutscene_guest,0);
    }
}
int main(int argc,char **argv){
    if(argc!=4&&argc!=5)return 2;
    FILE *f=fopen(argv[1],"rb");struct stat st;
    if(!f||fstat(fileno(f),&st))return 2;
    size_t data=((size_t)st.st_size+0x3fff)&~(size_t)0x3fff,size=data+0x4000;
    unsigned char *base=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(base==MAP_FAILED||fread(base,1,(size_t)st.st_size,f)!=(size_t)st.st_size)return 2;fclose(f);
    Module m={.name="main",.base=base,.file_size=(size_t)st.st_size,.size=size,.text_end=strtoull(argv[3],NULL,0)};
    if(argc==5){
        unsigned slot=(unsigned)strtoul(argv[4],NULL,0);
        assert(slot>=CUTSCENE_STAMP_SLOT&&slot<CUTSCENE_DATA_END);
        base[data+slot]=1; /* caller-selected negative control in this private mapping */
    }
    runtime_game_patches(&m);
    int on=cutscene_stamp!=NULL;policy_checks(on);
    uint32_t fade;memcpy(&fade,base+0x0a7ac4,4);
    printf("{\"fade_clamps\":%s,\"hardening\":%s,\"policy\":\"pass\",\"stamp_slot\":%u,\"window_slot\":%u}\n",
        (fade&0xfc000000)==0x14000000?"true":"false",on?"true":"false",CUTSCENE_STAMP_SLOT,CUTSCENE_WINDOW_SLOT);
    f=fopen(argv[2],"wb");if(!f||fwrite(base,1,size,f)!=size||fclose(f))return 2;
    munmap(base,size);return 0;
}
