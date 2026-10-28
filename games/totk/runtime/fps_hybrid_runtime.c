#include "fps_hybrid_runtime.h"
#ifdef GPU_VK
#include "../android/gfx/fps_hybrid.h"
#include "../android/gfx/fps_auto_scene.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t once=PTHREAD_ONCE_INIT;
static int selected;
static _Atomic int interpolating;
static fps_hybrid_policy policy; /* Only the guest acquire/present owner mutates. */
static int eligible, clock_valid, controller_valid;
static fps_auto_scene scene;
static uint64_t frames;
static int nonzero(const char *name) {
    const char *s=getenv(name); return s && *s && strcmp(s,"0");
}
static int differs(const char *name,const char *value) {
    const char *s=getenv(name); return s && strcmp(s,value);
}
static double option(const char *name,double fallback,double low,double high) {
    const char *s=getenv(name); if (!s) return fallback;
    char *end; double n=strtod(s,&end);
    if (end!=s && !*end && isfinite(n) && n>=low && n<=high) return n;
    fprintf(stderr,"[hybrid] invalid %s=%s; using %.3f (range %.3f..%.3f)\n",name,s,fallback,low,high);
    return fallback;
}
static void configure(void) {
    const char *mode=getenv("TOTK_FRAMEGEN");
    if (!mode || strcmp(mode,"hybrid")) return;
    const char *reason=NULL;
#if !(defined(__ANDROID__) && defined(TOTK_ANDROID_APP)) && !defined(TOTK_VK_TESTS)
    reason="requires the native Android Vulkan app";
#elif !defined(TOTK_GAME_V143) && !defined(TOTK_VK_TESTS)
    reason="requires game version 1.4.3 controller estimates";
#endif
    const char *fps=getenv("TOTK_FPS60");
    if (!fps || strcmp(fps,"1")) reason="requires TOTK_FPS60=1";
    else if (nonzero("TOTK_FPS_AUTO")) reason="requires TOTK_FPS_AUTO=0";
    else if (nonzero("TOTK_UNCAPPED") || nonzero("DM_DETCLOCK") || differs("TOTK_REALTIME","1"))
        reason="requires capped measured realtime";
    else if (differs("TOTK_PRESENT_SLOTS","1") || differs("DM_PRESENT_HZ","60") || differs("DM_PRESENT_INTERVAL","game"))
        reason="requires fixed slots and native 60Hz game pacing";
    else if (nonzero("TOTK_ASYNC_REPLAY")) reason="requires TOTK_ASYNC_REPLAY=0 (acquire-owned mode)";
    else if (nonzero("TOTK_ACQUIRE_SLACK") || nonzero("TOTK_ACQUIRE_CLOCK") ||
             differs("TOTK_PACE","0") || nonzero("TOTK_MFX"))
        reason="requires ACQUIRE_SLACK=0 ACQUIRE_CLOCK=0 PACE=0 MFX=0; reserve+interpolation is a later stage";
    if (reason) { fprintf(stderr,"[hybrid] refused: %s; existing native policy retained\n",reason); return; }
    fps_hybrid_config c=fps_hybrid_defaults();
    c.drop_fps=option("TOTK_HYBRID_DROP_FPS",c.drop_fps,30,59);
    c.drop_ns=(uint64_t)(option("TOTK_HYBRID_DROP_SECONDS",c.drop_ns/1e9,1,60)*1e9);
    c.recover_ns=(uint64_t)(option("TOTK_HYBRID_RECOVER_SECONDS",c.recover_ns/1e9,1,120)*1e9);
    c.dwell_ns=(uint64_t)(option("TOTK_HYBRID_DWELL_SECONDS",c.dwell_ns/1e9,1,300)*1e9);
    c.recover_score=option("TOTK_HYBRID_RECOVER_SCORE",c.recover_score,0.1,1.0);
    policy.config=c;
    selected=1;
    fprintf(stderr,"[hybrid] enabled: native60 -> real30+interp after two %.3fs windows <%.3ffps; return %.3fs >=95%% controller scores <%.3f; dwell %.3fs; acquire-owned, no guest controller writes; interpolation latency +33.333ms; reserve disabled\n",
        c.drop_ns/1e9,c.drop_fps,c.recover_ns/1e9,c.recover_score,c.dwell_ns/1e9);
}
int runtime_fps_hybrid_selected(void) { pthread_once(&once,configure); return selected; }
int runtime_fps_hybrid_interpolating(void) {
    return runtime_fps_hybrid_selected() && atomic_load_explicit(&interpolating,memory_order_acquire);
}
unsigned runtime_fps_hybrid_acquire(uint64_t now,uint64_t refresh,unsigned guest,
        int valid,double cpu,double gpu) {
    if (!runtime_fps_hybrid_selected()) return guest;
    if (!policy.last_ns) fps_hybrid_init(&policy,policy.config,now);
    clock_valid=refresh>=16000000 && refresh<=17300000 && guest>=1 && guest<=2;
    controller_valid=valid;
    int work_valid=valid && isfinite(cpu) && isfinite(gpu) && cpu>=0 && gpu>=0;
    if (!clock_valid) {
        fps_hybrid_time(&policy,now); fps_hybrid_clear_evidence(&policy);
        /* Unsupported display/guest intervals cannot admit interpolation. */
        if (policy.interp) {
            policy.interp=policy.desired=0; policy.changed_ns=now; policy.switches++;
            fprintf(stderr,"[hybrid] mode -> native60: unsupported guest interval %u / refresh %.3fms; fresh evidence required\n",guest,refresh/1e6);
        }
    } else {
        valid=controller_valid && eligible;
        if (!valid) fps_hybrid_clear_evidence(&policy);
        fps_hybrid_work(&policy,now,valid && work_valid,cpu,gpu);
        if (fps_hybrid_apply(&policy,now))
            fprintf(stderr,"[hybrid] mode -> %s: %s; raw-rate=%.3ffps switches=%llu; next acquire/slot boundary, temporal warmup/drain\n",
                policy.interp?"real30+interp":"native60",policy.interp?"sustained low real rate":"sustained controller headroom",
                policy.last_fps,(unsigned long long)policy.switches);
    }
    atomic_store_explicit(&interpolating,policy.interp,memory_order_release);
    return policy.interp && guest<2 ? 2 : guest;
}
void runtime_fps_hybrid_present(uint64_t now,uint64_t draws,int loading) {
    if (!runtime_fps_hybrid_selected()) return;
    if (!policy.last_ns) fps_hybrid_init(&policy,policy.config,now);
    unsigned scene_state=fps_auto_scene_observe(&scene,now,draws,loading);
    eligible=scene_state==FPS_SCENE_READY && clock_valid && controller_valid;
    fps_hybrid_present(&policy,now,eligible);
    if (++frames%300==0)
        fprintf(stderr,"[hybrid] last 300 real: mode=%s lifetime-native=%.3fs lifetime-interp=%.3fs switches=%llu raw-window=%.3ffps slow-windows=%u eligible=%d scene=%u draws=%llu clock=%d controller=%d; time includes pacing, not a scanout claim\n",
            policy.interp?"real30+interp":"native60",policy.time_ns[0]/1e9,policy.time_ns[1]/1e9,
            (unsigned long long)policy.switches,policy.last_fps,policy.slow_windows,eligible,
            scene_state,(unsigned long long)draws,clock_valid,controller_valid);
}
#endif
