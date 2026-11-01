/* Dataless host test for the 1.4.3 native60 fade/cutscene patch table.
 * Build: make -C games/totk/android fps60patchtest TARGET=linux CC=clang OUT=<scratch>
 * Run: <scratch>/fps60patchtest
 *
 * A zero-filled synthetic main carries stand-in words at the guarded sites
 * (CODE_GUARD_STANDIN: the guards expect code_guard_standin(offset) instead of
 * the game's words, so no game code is needed or kept here). The real
 * patch_fps60_hardening() must install exactly the gated sites, branch each one
 * into the reserved veneer and back (replaying the site's own word), and refuse
 * every single-word mismatch or occupied padding before writing anything.
 * TOTK_FPS60_FADE_CLAMP=0 must leave the four fade words original. A second
 * case runs the real frame-step and output-size patchers together: the adaptive
 * capture veneer (+0..+20) and the output veneer (+20..+48) must coexist
 * without touching the aspect veneers. The stored digests are checked against
 * a real main by scripts/verify_code_guards.py; instruction semantics against
 * the real image by scripts/verify_v143_fps60_hardening.py. */
#include "runtime.h"
#include "game_patches.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf fail_jump;
static const char *fail_message;
_Noreturn void runtime_fail(const char *s){fail_message=s;longjmp(fail_jump,1);}
int mfxup_output_enabled(void){return 0;}
void runtime_oe_push_message(uint32_t m){(void)m;}
void dm_gfx_set_flag(const char *s,int v){(void)s;(void)v;}
void runtime_survey_init(Module *m){(void)m;}
#define CODE_GUARD_STANDIN 1
#include "game_patches_v143.c"

static unsigned failures,checks;
#define CHECK(cond,...) do{checks++;if(!(cond)){failures++;fprintf(stderr,"FAIL %s:%d: ",__FILE__,__LINE__);\
    fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)

/* Four fcvtzs fade conversions, three diagnostic entries, the scripted-camera
 * update, the interval join and the camera completion store. */
static struct { uint64_t off; uint32_t word; } expected_sites[]={
    {0x0a7ac4,0},{0x95dd24,0},{0x6155e4,0},{0x615660,0},
    {0x45c700,0},{0x2b23c7c,0},{0x42c3bc,0},{0xa5a188,0},
    {0x2db238,0},{0xa5a670,0},
};
/* Adjacent words the camera veneers rely on: never written, always checked. */
static struct { uint64_t off; uint32_t word; } expected_guards[]={
    {0xa5a18c,0},{0xa5a660,0},{0xa5a664,0},
    {0xa5a668,0},{0xa5a66c,0},{0xa5a674,0},
};
enum { NSITES=sizeof expected_sites/sizeof *expected_sites,
       NGUARDS=sizeof expected_guards/sizeof *expected_guards, NFADES=4,
       TEXT_END=0x2bb9c00, FILE_SIZE=0x2bc4000, IMAGE_SIZE=FILE_SIZE+0x4000,
       STUB=TEXT_END+416, VENEER_BYTES=500, DATA=FILE_SIZE };

static uint8_t *image,*pristine;
static Module module;
static uint32_t word_at(uint64_t off){uint32_t w;memcpy(&w,image+off,4);return w;}
static void put_word(uint8_t *to,uint64_t off,uint32_t w){memcpy(to+off,&w,4);}
static void reset_image(void){memcpy(image,pristine,IMAGE_SIZE);}
static void set_env(const char *name,const char *value){if(value)setenv(name,value,1);else unsetenv(name);}
static int run(void (*patch)(Module *)){
    fail_message=NULL;
    if(setjmp(fail_jump))return 0;
    patch(&module);
    return 1;
}
static int run_patch(void){return run(patch_fps60_hardening);}
/* B imm26 at `off` -> absolute target, or 0 when the word is not a branch. */
static uint64_t branch_target(uint64_t off,uint32_t w){
    if((w&0xfc000000u)!=0x14000000u)return 0;
    int64_t imm=(int64_t)(w&0x3ffffffu);if(imm&0x2000000)imm-=0x4000000;
    return off+(uint64_t)(imm*4);
}
/* Site -> veneer in the reserved region; the veneer replays the original word
 * and returns with a branch to site+4. */
static void check_installed(unsigned i){
    uint64_t site=expected_sites[i].off,to=branch_target(site,word_at(site));
    CHECK(to>=STUB&&to<STUB+VENEER_BYTES,"site %#llx branches to %#llx outside the reserved veneer",
          (unsigned long long)site,(unsigned long long)to);
    if(to<STUB||to>=STUB+VENEER_BYTES)return;
    int replay=0,ret=0;
    for(uint64_t at=to;at<STUB+VENEER_BYTES&&!ret;at+=4){
        uint32_t w=word_at(at);
        if(w==expected_sites[i].word)replay=1;
        if(branch_target(at,w)==site+4)ret=1;
    }
    CHECK(replay&&ret,"site %#llx veneer replay=%d return=%d",(unsigned long long)site,replay,ret);
    if(i<NFADES)CHECK(word_at(to)==expected_sites[i].word,"fade %#llx veneer must run the original fcvtzs first",
                      (unsigned long long)site);
}

static void gate_matrix(void){
    const char *fps_modes[]={NULL,"0","1"};
    const char *fg_modes[]={NULL,"","0","locked","interp","hybrid","adaptive","1"};
    const char *hardening_modes[]={NULL,"0","1"};
    const char *clamp_modes[]={NULL,"0","1"};
    for(unsigned f=0;f<3;f++)for(unsigned g=0;g<8;g++)for(unsigned h=0;h<3;h++)for(unsigned c=0;c<3;c++){
        const char *fps=fps_modes[f],*fg=fg_modes[g],*hard=hardening_modes[h],*clamp=clamp_modes[c];
        set_env("TOTK_FPS60",fps);set_env("TOTK_FRAMEGEN",fg);set_env("TOTK_FPS60_HARDENING",hard);
        set_env("TOTK_FPS60_FADE_CLAMP",clamp);
        int on=!(fps&&!strcmp(fps,"0"));
        int fades=on&&!(clamp&&!strcmp(clamp,"0"));
        int floor=on&&!(fg&&*fg&&strcmp(fg,"0"))&&!(hard&&!strcmp(hard,"0"));
        reset_image();
        int ok=run_patch();
        CHECK(ok,"gate fps=%s fg=%s hardening=%s clamp=%s refused: %s",fps?fps:"(unset)",fg?fg:"(unset)",
              hard?hard:"(unset)",clamp?clamp:"(unset)",fail_message?fail_message:"");
        if(!ok)continue;
        if(!on){
            CHECK(!memcmp(image,pristine,IMAGE_SIZE),"TOTK_FPS60=0 must leave the image untouched (fg=%s hardening=%s)",
                  fg?fg:"(unset)",hard?hard:"(unset)");
            continue;
        }
        for(unsigned i=0;i<NSITES;i++){
            if(i<NFADES?fades:floor)check_installed(i);
            else CHECK(word_at(expected_sites[i].off)==expected_sites[i].word,
                       "disabled site %#llx must keep its original word (fg=%s hardening=%s clamp=%s)",
                       (unsigned long long)expected_sites[i].off,fg?fg:"(unset)",hard?hard:"(unset)",
                       clamp?clamp:"(unset)");
        }
        for(unsigned i=0;i<NGUARDS;i++)
            CHECK(word_at(expected_guards[i].off)==expected_guards[i].word,"guard %#llx written",
                  (unsigned long long)expected_guards[i].off);
        /* Nothing outside the sites, veneer region and (floor only) data slots changes. */
        for(unsigned i=0;i<NSITES;i++)put_word(image,expected_sites[i].off,expected_sites[i].word);
        memset(image+STUB,0,VENEER_BYTES);
        memset(image+DATA+CUTSCENE_STAMP_SLOT,0,CUTSCENE_DATA_END-CUTSCENE_STAMP_SLOT);
        CHECK(!memcmp(image,pristine,IMAGE_SIZE),"bytes outside sites/veneer/data changed (floor=%d)",floor);
    }
}

/* Any single different word refuses before writing: fades+floor, fades only,
 * and with both groups disabled (clamp off, framegen) nothing may be skipped. */
static void mismatch_refusals(void){
    const char *fg_modes[]={"0","locked","locked"},*clamp_modes[]={NULL,NULL,"0"};
    for(unsigned mode=0;mode<3;mode++){
        set_env("TOTK_FPS60","1");set_env("TOTK_FRAMEGEN",fg_modes[mode]);set_env("TOTK_FPS60_HARDENING",NULL);
        set_env("TOTK_FPS60_FADE_CLAMP",clamp_modes[mode]);
        for(unsigned i=0;i<NSITES+NGUARDS;i++){
            uint64_t off=i<NSITES?expected_sites[i].off:expected_guards[i-NSITES].off;
            uint32_t word=i<NSITES?expected_sites[i].word:expected_guards[i-NSITES].word;
            reset_image();put_word(image,off,word^0x00100000u);
            uint8_t *before=malloc(IMAGE_SIZE);
            if(!before){perror("malloc");exit(2);}
            memcpy(before,image,IMAGE_SIZE);
            int ok=run_patch();
            CHECK(!ok&&fail_message&&strstr(fail_message,"mismatch"),"word %#llx mismatch accepted (fg=%s)",
                  (unsigned long long)off,fg_modes[mode]);
            CHECK(!memcmp(image,before,IMAGE_SIZE),"refused word %#llx still wrote the image",(unsigned long long)off);
            free(before);
        }
    }
}

static void padding_refusals(void){
    set_env("TOTK_FPS60","1");set_env("TOTK_FRAMEGEN",NULL);set_env("TOTK_FPS60_HARDENING",NULL);
    set_env("TOTK_FPS60_FADE_CLAMP",NULL);
    const uint64_t occupied[]={STUB,STUB+VENEER_BYTES-1,DATA+CUTSCENE_STAMP_SLOT,DATA+CUTSCENE_DATA_END-1};
    for(unsigned i=0;i<4;i++){
        reset_image();image[occupied[i]]=0xa5;
        int ok=run_patch();
        CHECK(!ok&&fail_message&&strstr(fail_message,"padding in use"),"occupied byte %#llx accepted",
              (unsigned long long)occupied[i]);
        for(unsigned s=0;s<NSITES;s++)
            CHECK(word_at(expected_sites[s].off)==expected_sites[s].word,"padding refusal wrote site %#llx",
                  (unsigned long long)expected_sites[s].off);
    }
    /* Neighbouring veneers (stub..+415) and data slots (+0..+79) belong to other
     * 1.4.3 patches: occupying them must not disturb this group. */
    reset_image();
    memset(image+TEXT_END,0x5a,416);memset(image+DATA,0x5a,CUTSCENE_STAMP_SLOT);
    memset(image+STUB+VENEER_BYTES,0x5a,0x2bba000-(STUB+VENEER_BYTES));
    CHECK(run_patch(),"neighbouring veneer/data use refused: %s",fail_message?fail_message:"");
    for(unsigned i=0;i<416;i++)if(image[TEXT_END+i]!=0x5a){CHECK(0,"neighbouring veneer byte +%u overwritten",i);break;}
    for(unsigned i=0;i<CUTSCENE_STAMP_SLOT;i++)if(image[DATA+i]!=0x5a){CHECK(0,"neighbouring data byte +%u overwritten",i);break;}
}

/* After floor installation, the presenter floor follows the admitted stamp only. */
static void presenter_floor(void){
    set_env("TOTK_FPS60","1");set_env("TOTK_FRAMEGEN",NULL);set_env("TOTK_FPS60_HARDENING",NULL);
    reset_image();
    CHECK(run_patch(),"floor install refused: %s",fail_message?fail_message:"");
    CHECK(runtime_cutscene_present_ticks(1,16666667)==1,"no admitted stamp must keep interval 1");
    atomic_store(cutscene_markers,cutscene_counter());
    CHECK(runtime_cutscene_present_ticks(1,16666667)==1,"a diagnostic marker alone must not floor");
    atomic_store(cutscene_stamp,cutscene_counter());
    CHECK(runtime_cutscene_present_ticks(1,16666667)==2,"admitted stamp floors 60 Hz to two ticks");
    CHECK(runtime_cutscene_present_ticks(1,8333333)==4,"admitted stamp floors 120 Hz to four ticks");
    CHECK(runtime_cutscene_present_ticks(3,16666667)==3,"larger requests stay authoritative");
    atomic_store(cutscene_stamp,cutscene_counter()-cutscene_window);
    CHECK(runtime_cutscene_present_ticks(1,16666667)==1,"stamp at the 200 ms window must expire");
    atomic_store(cutscene_stamp,0);
}

/* Real frame-step (with and without the adaptive capture store) and output-size
 * patchers on one image: both must install, the output veneer starts at +20,
 * the frame-step veneer resumes at 0x3e565c, and the aspect veneers (+48) stay
 * free. Before the +20 placement, adaptive + an output size failed at boot. */
static struct { uint64_t off; uint32_t word; } layout_guards[]={
    {0x3e5658,0},{0x2daa54,0},{0x2daa58,0},{0x2daa6c,0},
    {0x2daa70,0},{0x2daa74,0},{0x2daa78,0},   /* frame step */
    {0xbdb578,0},{0xbdb584,0},{0x3dcb28,0},{0x3dcb2c,0},
    {0x3dcb30,0},                             /* output size */
};
static void adaptive_output_layout(void){
    const uint64_t base=(TEXT_END+15)&~15ULL;
    set_env("TOTK_REALTIME",NULL);set_env("TOTK_CLOTH_REALTIME","0");set_env("TOTK_NATIVE_UI",NULL);
    set_env("TOTK_EXPORT_IOS",NULL);set_env("TOTK_OUTPUT_RES","2560x1440");
    const char *modes[]={"adaptive","0"};
    for(unsigned mode=0;mode<2;mode++){
        set_env("TOTK_FRAMEGEN",modes[mode]);
        int capture=mode==0;
        reset_image();
        CHECK(run(patch_realtime),"frame-step refused (fg=%s): %s",modes[mode],fail_message?fail_message:"");
        CHECK(run(patch_output_res),"output size refused after frame step (fg=%s): %s",modes[mode],
              fail_message?fail_message:"");
        CHECK(branch_target(0x3e5658,word_at(0x3e5658))==base,"frame-step site must enter the +0 veneer");
        uint64_t resume=base+(capture?16:12);
        CHECK(branch_target(resume,word_at(resume))==0x3e565c,"frame-step veneer must resume at 0x3e565c (fg=%s)",modes[mode]);
        if(capture)CHECK(word_at(base+12)==0xbd000d08,"adaptive veneer must keep its capture store");
        else CHECK(word_at(base+16)==0,"legacy frame-step veneer must stay 16 bytes");
        CHECK(branch_target(0x3dcb28,word_at(0x3dcb28))==base+20,"output site must enter the +20 veneer (fg=%s)",modes[mode]);
        CHECK((word_at(base+20)&0x9f000000u)==0x90000000u,"output veneer must start with its adrp");
        CHECK(branch_target(base+44,word_at(base+44))==0x3dcb30,"output veneer must resume at 0x3dcb30");
        for(unsigned i=48;i<176;i++)if(image[base+i]){CHECK(0,"aspect veneer byte +%u written (fg=%s)",i,modes[mode]);break;}
    }
    /* The output patcher still refuses an occupied +20..+48 slot. */
    set_env("TOTK_FRAMEGEN","adaptive");
    reset_image();image[base+47]=0xa5;
    CHECK(!run(patch_output_res)&&fail_message&&strstr(fail_message,"padding not empty"),"occupied output veneer accepted");
    set_env("TOTK_FRAMEGEN",NULL);set_env("TOTK_OUTPUT_RES",NULL);set_env("TOTK_CLOTH_REALTIME",NULL);
}

int main(void){
    image=calloc(1,IMAGE_SIZE);pristine=calloc(1,IMAGE_SIZE);
    if(!image||!pristine){perror("calloc");return 2;}
    for(unsigned i=0;i<NSITES;i++)
        put_word(pristine,expected_sites[i].off,expected_sites[i].word=code_guard_standin(expected_sites[i].off));
    for(unsigned i=0;i<NGUARDS;i++)
        put_word(pristine,expected_guards[i].off,expected_guards[i].word=code_guard_standin(expected_guards[i].off));
    for(unsigned i=0;i<sizeof layout_guards/sizeof *layout_guards;i++)
        put_word(pristine,layout_guards[i].off,layout_guards[i].word=code_guard_standin(layout_guards[i].off));
    module=(Module){.name="main",.file_size=FILE_SIZE,.size=IMAGE_SIZE,.text_end=TEXT_END,.base=image};
    gate_matrix();
    mismatch_refusals();
    padding_refusals();
    presenter_floor();
    adaptive_output_layout();
    if(failures){fprintf(stderr,"fps60patchtest: %u/%u checks FAILED\n",failures,checks);return 1;}
    printf("PASS: %u checks; 10 sites + 6 guards, 216-case FPS60/framegen/hardening/fade-clamp gate, 48 single-word refusals, padding/neighbour layout, presenter floor, adaptive+output veneer coexistence\n",checks);
    return 0;
}
