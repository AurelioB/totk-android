/* MIT. GL_QCOM_frame_extrapolation bridge, authored from the Khronos API spec.
 * No third-party sample implementation is included. All shared pixels are
 * private AHBs: a failed GL handoff cannot corrupt the ordinary generator.
 */
#include "vk_framegen_qcom.h"
#include <math.h>
#include "framegen_qcom_policy.h"
#include "vk_overlay.h"
#include "framegen_dispatch_trace.h"

static _Atomic int requested=-1, refused;
static _Atomic uint64_t refusals;
static _Atomic(const char *) refusal_reason;
int vk_fg_qcom_requested(void) {
    int on=atomic_load_explicit(&requested,memory_order_relaxed);
    if (on>=0) return on!=0;
    const char *engine=getenv("TOTK_FRAMEGEN_ENGINE"), *mode=getenv("TOTK_FRAMEGEN");
    on=fg_qcom_requested(engine,mode) ? (fg_qcom_adaptive(mode) ? 2 : 1) : 0;
    int expected=-1;
    if (atomic_compare_exchange_strong(&requested,&expected,on)) {
        if (engine && *engine && strcmp(engine,"camera") && strcmp(engine,"lsfg") && !on)
            LOGV("framegen qcom refused: TOTK_FRAMEGEN_ENGINE=qcom requires exact TOTK_FRAMEGEN=locked or adaptive (using existing generator)");
    } else on=expected;
    return on!=0;
}
static int adaptive_requested(void) {
    return vk_fg_qcom_requested() && atomic_load_explicit(&requested,memory_order_relaxed)==2;
}
void vk_fg_qcom_refuse(const char *reason) {
    atomic_fetch_add_explicit(&refusals,1,memory_order_relaxed);
    if (fg_qcom_refuse(&refused)) {
        atomic_store_explicit(&refusal_reason,reason,memory_order_release);
        LOGV("framegen qcom refused: %s (%s)",reason,adaptive_requested() ?
            "adaptive generation suspended; real-only, no camera fallback" :
            "permanent fallback to locked camera interpolation");
    }
}

#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
#include "vk_framegen.h"
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <time.h>
#include <errno.h>

enum { SOURCES=4 };
/* 100ms is a failure deadline, not a wait. Neither poll nor query harvest blocks.
 * A GPU/driver-wide hang is not recoverable by swapping generators. */
#define QCOM_TIMEOUT_NS UINT64_C(100000000)
#define EGL_FUNCS(X) \
    X(eglGetDisplay) X(eglInitialize) X(eglBindAPI) X(eglChooseConfig) X(eglCreateContext) X(eglGetError) \
    X(eglCreatePbufferSurface) X(eglMakeCurrent) X(eglQueryString) X(eglGetProcAddress) X(eglQueryContext) X(eglDestroyContext)
#define GL_FUNCS(X) \
    X(glGetString) X(glGetError) X(glGenTextures) X(glDeleteTextures) X(glBindTexture) \
    X(glTexParameteri) X(glFlush) X(glGetIntegerv) \
    X(glGenFramebuffers) X(glDeleteFramebuffers) X(glBindFramebuffer) \
    X(glFramebufferTexture2D) X(glCheckFramebufferStatus) X(glClearColor) X(glClear)
#define DECLARE(name) __typeof__(&name) name;
typedef struct {
    EGL_FUNCS(DECLARE)
    GL_FUNCS(DECLARE)
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC native_buffer;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence;
    void (*extrapolate)(GLuint,GLuint,GLuint,GLfloat);
    PFNGLGENQUERIESEXTPROC gen_queries;
    PFNGLBEGINQUERYEXTPROC begin_query;
    PFNGLENDQUERYEXTPROC end_query;
    PFNGLGETQUERYOBJECTUIVEXTPROC query_available;
    PFNGLGETQUERYOBJECTUI64VEXTPROC query_result;
} qcom_api;
#undef DECLARE
typedef struct {
    AHardwareBuffer *buffer;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    EGLImageKHR egl;
    GLuint texture;
    uint32_t w,h;
    int initialized;
} qcom_image;
typedef struct {
    qcom_image scene;
    int copied;
} qcom_source;
typedef struct {
    qcom_image output;
    VkSemaphore to_gl,from_gl;
    GLuint query;
    uint64_t frame,started,input_lower_ns;
    uint64_t call_begin_ns,call_end_ns,flush_end_ns;
    int monitor,input_monitor; /* duplicated native-fence FDs; -1 when absent */
    int disjoint,bad;
    fg_qcom_job job;
    fg_dispatch_trace trace; /* copied recorder scope; AFME-only diagnostics */
} qcom_output;
static struct {
    pthread_mutex_t mu;
    pthread_mutex_t policy_mu; /* never held by driver/context warmup */
    qcom_api a;
    void *egl_lib,*gl_lib;
    EGLDisplay display;
    EGLContext context;
    EGLint priority;
    EGLSurface surface;
    int device,initialized,diag;
    char model[PROP_VALUE_MAX],product_device[PROP_VALUE_MAX];
    _Atomic int live; /* admission published once; dormant path never takes Q.mu */
    _Atomic unsigned pending_jobs,pending_costs;
    _Atomic int worker_started,warmed,cooling,suspended;
    _Atomic unsigned quarantined;
    uint32_t warm_w,warm_h;
    uint64_t warm_cpu_ns,warm_call_ns;
    fg_qcom_policy policy;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID ahb_properties;
    PFN_vkGetSemaphoreFdKHR semaphore_fd;
    PFN_vkImportSemaphoreFdKHR import_semaphore;
    VkQueryPool copy_queries;
    qcom_source source[SOURCES];
    qcom_output output[VK_PRESENT_SLOT_CAPACITY];
    uint64_t generated,timed,missing,disjoint,timeout,imports,slow;
    double sum_ms,max_ms;
    uint64_t copy_samples;
    double copy_sum_ms,copy_max_ms;
    double fence_sum_ms,fence_max_ms,charged_sum_ms,charged_max_ms;
    uint64_t import_calls,import_cpu_ns,import_cpu_max,call_cpu_ns,call_cpu_max;
} Q={.mu=PTHREAD_MUTEX_INITIALIZER,.policy_mu=PTHREAD_MUTEX_INITIALIZER};
static int extension(const char *list,const char *name) {
    if (!list) return 0;
    size_t n=strlen(name);
    for (const char *p=list; (p=strstr(p,name)); p+=n)
        if ((p==list || p[-1]==' ') && (!p[n] || p[n]==' ')) return 1;
    return 0;
}
static int current(void) {
    if (Q.a.eglMakeCurrent(Q.display,Q.surface,Q.surface,Q.context)) return 1;
    vk_fg_qcom_refuse("EGL context binding failed"); return 0;
}
static void unbind(void) { Q.a.eglMakeCurrent(Q.display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); }
void vk_fg_qcom_device(PFN_vkGetInstanceProcAddr ip,PFN_vkGetDeviceProcAddr dp,int interop) {
    if (!vk_fg_qcom_requested()) return;
    if (adaptive_requested()) {
        __system_property_get("ro.product.model",Q.model);
        __system_property_get("ro.product.device",Q.product_device);
        if (!fg_qcom_adaptive_device(Q.model,Q.product_device,V.props.vendorID,V.props.deviceName)) {
            LOGV("framegen adaptive qcom device refused: model=%s device=%s vendor=0x%x GPU=%s; qualified AYN Thor/kalama/Turnip A740 only (no GL warmup or AFME call)",
                Q.model,Q.product_device,V.props.vendorID,V.props.deviceName);
            vk_fg_qcom_refuse("adaptive AFME is qualified only on Thor/A740; other devices unsafe");
            return;
        }
    }
    const char *diag=getenv("TOTK_FRAMEGEN_DIAG"); Q.diag=diag && !strcmp(diag,"1");
    if (!interop) { vk_fg_qcom_refuse("Vulkan AHB/foreign-queue/external-semaphore-FD extensions missing"); return; }
    PFN_vkGetPhysicalDeviceExternalSemaphoreProperties props=
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)ip(V.inst,"vkGetPhysicalDeviceExternalSemaphoreProperties");
    VkPhysicalDeviceExternalSemaphoreInfo info={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties out={VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    if (props) props(V.pd,&info,&out);
    VkExternalSemaphoreFeatureFlags needed=VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT|VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    Q.ahb_properties=(PFN_vkGetAndroidHardwareBufferPropertiesANDROID)dp(V.dev,"vkGetAndroidHardwareBufferPropertiesANDROID");
    Q.semaphore_fd=(PFN_vkGetSemaphoreFdKHR)dp(V.dev,"vkGetSemaphoreFdKHR");
    Q.import_semaphore=(PFN_vkImportSemaphoreFdKHR)dp(V.dev,"vkImportSemaphoreFdKHR");
    if ((out.externalSemaphoreFeatures&needed)!=needed || !Q.ahb_properties || !Q.semaphore_fd || !Q.import_semaphore) {
        vk_fg_qcom_refuse("Vulkan SYNC_FD import/export or AHB entry points missing"); return;
    }
    Q.device=1;
}
static const char *priority_name(EGLint priority) {
    switch (priority) {
    case EGL_CONTEXT_PRIORITY_HIGH_IMG: return "HIGH";
    case EGL_CONTEXT_PRIORITY_MEDIUM_IMG: return "MEDIUM";
    case EGL_CONTEXT_PRIORITY_LOW_IMG: return "LOW";
    default: return "UNKNOWN";
    }
}
static int init(void *unused) {
    (void)unused;
    if (fg_qcom_refused(&refused)) return 0;
    if (Q.initialized) return 1;
    if (!Q.device) return 0;
    const char *diag=getenv("TOTK_FRAMEGEN_DIAG"); Q.diag=diag && !strcmp(diag,"1");
    Q.egl_lib=dlopen("libEGL.so",RTLD_NOW|RTLD_LOCAL);
    Q.gl_lib=dlopen("libGLESv2.so",RTLD_NOW|RTLD_LOCAL);
    if (!Q.egl_lib || !Q.gl_lib) { vk_fg_qcom_refuse("EGL/GLES libraries unavailable"); return 0; }
#define LOAD_EGL(name) Q.a.name=(__typeof__(Q.a.name))dlsym(Q.egl_lib,#name); if (!Q.a.name) goto missing;
    EGL_FUNCS(LOAD_EGL)
#undef LOAD_EGL
#define LOAD_GL(name) Q.a.name=(__typeof__(Q.a.name))dlsym(Q.gl_lib,#name); if (!Q.a.name) goto missing;
    GL_FUNCS(LOAD_GL)
#undef LOAD_GL
    Q.display=Q.a.eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major,minor,count;
    EGLConfig config;
    const EGLint config_attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    const EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,3,
        EGL_CONTEXT_PRIORITY_LEVEL_IMG,EGL_CONTEXT_PRIORITY_HIGH_IMG,EGL_NONE};
    const EGLint surface_attrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
    if (Q.display==EGL_NO_DISPLAY || !Q.a.eglInitialize(Q.display,&major,&minor) ||
        !Q.a.eglBindAPI(EGL_OPENGL_ES_API) || !Q.a.eglChooseConfig(Q.display,config_attrs,&config,1,&count) || !count) goto missing;
    const char *egl_ext=Q.a.eglQueryString(Q.display,EGL_EXTENSIONS);
    if (!extension(egl_ext,"EGL_ANDROID_get_native_client_buffer") || !extension(egl_ext,"EGL_ANDROID_image_native_buffer") ||
        !extension(egl_ext,"EGL_KHR_image_base") || !extension(egl_ext,"EGL_ANDROID_native_fence_sync") ||
        !extension(egl_ext,"EGL_KHR_wait_sync")) goto missing;
    if (!extension(egl_ext,"EGL_IMG_context_priority")) {
        LOGV("framegen qcom EGL priority: extension=0 requested=HIGH granted=unavailable");
        vk_fg_qcom_refuse("EGL_IMG_context_priority unavailable for high-priority experiment"); return 0;
    }
    Q.context=Q.a.eglCreateContext(Q.display,config,EGL_NO_CONTEXT,context_attrs);
    if (Q.context==EGL_NO_CONTEXT) {
        LOGV("framegen qcom EGL priority: extension=1 requested=HIGH granted=unavailable create-error=0x%x",
            Q.a.eglGetError());
        vk_fg_qcom_refuse("high-priority EGL context creation failed"); return 0;
    }
    EGLBoolean queried=Q.a.eglQueryContext(Q.display,Q.context,EGL_CONTEXT_PRIORITY_LEVEL_IMG,&Q.priority);
    EGLint priority_error=queried ? EGL_SUCCESS : Q.a.eglGetError();
    LOGV("framegen qcom EGL priority: extension=1 requested=HIGH granted=%s(0x%x) query=%u error=0x%x vendor=%s (EGL hint/grant; kernel preemption not verified)",
        priority_name(Q.priority),Q.priority,queried,priority_error,Q.a.eglQueryString(Q.display,EGL_VENDOR));
    if (!queried || Q.priority!=EGL_CONTEXT_PRIORITY_HIGH_IMG) {
        Q.a.eglDestroyContext(Q.display,Q.context); Q.context=EGL_NO_CONTEXT;
        vk_fg_qcom_refuse("HIGH EGL context priority not granted/verified"); return 0;
    }
    Q.surface=Q.a.eglCreatePbufferSurface(Q.display,config,surface_attrs);
    if (Q.context==EGL_NO_CONTEXT || Q.surface==EGL_NO_SURFACE || !current()) goto missing;
    LOGV("framegen qcom GL implementation: vendor=%s renderer=%s version=%s",
        (const char *)Q.a.glGetString(GL_VENDOR),(const char *)Q.a.glGetString(GL_RENDERER),(const char *)Q.a.glGetString(GL_VERSION));
    const char *gl_ext=(const char *)Q.a.glGetString(GL_EXTENSIONS);
    if (!extension(gl_ext,"GL_QCOM_frame_extrapolation") || !extension(gl_ext,"GL_OES_EGL_image")) {
        unbind(); vk_fg_qcom_refuse("GL_QCOM_frame_extrapolation/GL_OES_EGL_image absent in app context"); return 0;
    }
    /* Timer queries are required for this locked-mode prototype's exact total
     * GPU ceiling. No unmeasured GL work may be treated as free headroom. */
    if (!extension(gl_ext,"GL_EXT_disjoint_timer_query")) {
        unbind(); vk_fg_qcom_refuse("GL_EXT_disjoint_timer_query unavailable for total-GPU admission"); return 0;
    }
#define PROC(field,type,name) Q.a.field=(type)Q.a.eglGetProcAddress(name); if (!Q.a.field) { unbind(); goto missing; }
    PROC(native_buffer,PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC,"eglGetNativeClientBufferANDROID")
    PROC(create_image,PFNEGLCREATEIMAGEKHRPROC,"eglCreateImageKHR")
    PROC(destroy_image,PFNEGLDESTROYIMAGEKHRPROC,"eglDestroyImageKHR")
    PROC(target_image,PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,"glEGLImageTargetTexture2DOES")
    PROC(create_sync,PFNEGLCREATESYNCKHRPROC,"eglCreateSyncKHR")
    PROC(destroy_sync,PFNEGLDESTROYSYNCKHRPROC,"eglDestroySyncKHR")
    PROC(wait_sync,PFNEGLWAITSYNCKHRPROC,"eglWaitSyncKHR")
    PROC(dup_fence,PFNEGLDUPNATIVEFENCEFDANDROIDPROC,"eglDupNativeFenceFDANDROID")
    PROC(extrapolate,void (*)(GLuint,GLuint,GLuint,GLfloat),"glExtrapolateTex2DQCOM")
    PROC(gen_queries,PFNGLGENQUERIESEXTPROC,"glGenQueriesEXT")
    PROC(begin_query,PFNGLBEGINQUERYEXTPROC,"glBeginQueryEXT")
    PROC(end_query,PFNGLENDQUERYEXTPROC,"glEndQueryEXT")
    PROC(query_available,PFNGLGETQUERYOBJECTUIVEXTPROC,"glGetQueryObjectuivEXT")
    PROC(query_result,PFNGLGETQUERYOBJECTUI64VEXTPROC,"glGetQueryObjectui64vEXT")
#undef PROC
    for (unsigned i=0;i<VK_PRESENT_SLOT_CAPACITY;i++) Q.output[i].monitor=Q.output[i].input_monitor=-1;
    VkQueryPoolCreateInfo qi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=SOURCES*2};
    if (vkCreateQueryPool(V.dev,&qi,NULL,&Q.copy_queries)!=VK_SUCCESS) {
        unbind(); vk_fg_qcom_refuse("AHB-copy timestamp pool unavailable"); return 0;
    }
    Q.initialized=1;
    atomic_store_explicit(&Q.live,1,memory_order_release);
    LOGV("framegen qcom: app-context GL_QCOM_frame_extrapolation, scale=0.5; AHB RGBA16F pre-UI copy, native SYNC_FD GPU handoffs; admission=max(GL timer,native-fence span); no next-real wait");
    unbind(); return 1;
missing:
    vk_fg_qcom_refuse("EGL context/interop functions unavailable"); return 0;
}
int vk_fg_qcom_active(void) {
    fg_dispatch_trace *trace=vk_fg_dispatch_trace_current;
    return vk_fg_qcom_requested() && (vk_fg_locked() || adaptive_requested()) && vk_fg_enabled() &&
        FG_DISPATCH_FACT(trace,qcom_admitted,
            fg_qcom_admitted(&refused,
                FG_DISPATCH_FACT(trace,qcom_warmed,atomic_load_explicit(&Q.warmed,memory_order_acquire)),
                FG_DISPATCH_FACT(trace,qcom_suspended,atomic_load_explicit(&Q.suspended,memory_order_acquire)),
                FG_DISPATCH_FACT(trace,qcom_cooling,atomic_load_explicit(&Q.cooling,memory_order_acquire)),
                FG_DISPATCH_FACT(trace,qcom_quarantined,atomic_load_explicit(&Q.quarantined,memory_order_acquire))));
}
/* Caller already owns policy_mu: publish only values the policy has computed. */
static void overlay_policy(void) {
    uint64_t left=Q.policy.cooldown_until>Q.policy.tick ? Q.policy.cooldown_until-Q.policy.tick : 0;
    int warm=!atomic_load_explicit(&Q.warmed,memory_order_relaxed);
    int fallback=warm || left || atomic_load_explicit(&Q.suspended,memory_order_relaxed) ||
        atomic_load_explicit(&Q.quarantined,memory_order_relaxed);
    int camera=fallback && !adaptive_requested();
    vk_overlay_set_framegen(VK_OVERLAY_FG_QCOM,camera ? VK_OVERLAY_FG_INTERP : VK_OVERLAY_FG_EXTRAP,
        left ? VK_OVERLAY_FG_COOLDOWN : warm || fallback ? VK_OVERLAY_FG_WARMUP : VK_OVERLAY_FG_ACTIVE,
        left>UINT32_MAX ? UINT32_MAX : (unsigned)left,camera);
}
/* Caller owns policy_mu. These are lifetime counts, including startup timing
 * cooldowns; reprobing is an actual dispatch and recovery is its exact timing
 * completion, not permission to reuse an outstanding GPU lease. */
static void timing_log(const char *event,uint64_t frame,unsigned kind) {
    static const char *const kinds[]={"deadline","timing","slow"};
    LOGV("framegen adaptive qcom timing-%s: kind=%s frame=%llu real-tick=%llu cooldown-left=%llu backoff=%u sustained-misses=%u timing-cooldowns-total=%llu timing-reprobes-total=%llu timing-recoveries-total=%llu (real-only while suspended; strict GPU admission and lease retirement unchanged)",
        event,kind<FG_QCOM_FAILURE_KINDS ? kinds[kind] : "none",(unsigned long long)frame,
        (unsigned long long)Q.policy.tick,
        (unsigned long long)(Q.policy.cooldown_until>Q.policy.tick ? Q.policy.cooldown_until-Q.policy.tick : 0),
        Q.policy.backoff,Q.policy.misses,(unsigned long long)Q.policy.timing_cooldowns,
        (unsigned long long)Q.policy.timing_reprobes,(unsigned long long)Q.policy.timing_recoveries);
}
static void failure(unsigned kind) {
    atomic_store_explicit(&Q.suspended,1,memory_order_release);
    pthread_mutex_lock(&Q.policy_mu);
    uint64_t cooldowns=Q.policy.timing_cooldowns;
    int permanent=fg_qcom_timing_failure(&Q.policy,kind,adaptive_requested());
    if (Q.policy.timing_cooldowns!=cooldowns) timing_log("cooldown",0,kind);
    atomic_store_explicit(&Q.cooling,!fg_qcom_can_generate(&Q.policy),memory_order_release);
    overlay_policy();
    pthread_mutex_unlock(&Q.policy_mu);
    if (permanent) vk_fg_qcom_refuse("sustained post-warmup deadline/timing failures (5 in 120 real ticks)");
}
void vk_fg_qcom_late(void) {
    if (atomic_load(&Q.worker_started) && !atomic_load(&refused)) failure(FG_QCOM_DEADLINE);
}
void vk_fg_qcom_tick(int scene) {
    if (atomic_load(&refused)) {
        vk_overlay_set_framegen(VK_OVERLAY_FG_QCOM,adaptive_requested() ? VK_OVERLAY_FG_EXTRAP : VK_OVERLAY_FG_INTERP,
            VK_OVERLAY_FG_REFUSED,0,!adaptive_requested());
        (void)vk_submit_external_disable(); return;
    }
    if (!atomic_load_explicit(&Q.worker_started,memory_order_acquire)) {
        vk_overlay_set_framegen(VK_OVERLAY_FG_QCOM,adaptive_requested() ? VK_OVERLAY_FG_EXTRAP : VK_OVERLAY_FG_INTERP,
            VK_OVERLAY_FG_WARMUP,0,!adaptive_requested());
        return;
    }
    pthread_mutex_lock(&Q.policy_mu);
    fg_qcom_tick(&Q.policy,scene);
    atomic_store_explicit(&Q.cooling,!fg_qcom_can_generate(&Q.policy),memory_order_release);
    overlay_policy();
    pthread_mutex_unlock(&Q.policy_mu);
    if (!vk_fg_qcom_active()) (void)vk_submit_external_disable();
}
int vk_fg_qcom_can_generate(void) { return vk_fg_qcom_active(); }
unsigned vk_fg_qcom_pending(void) { return atomic_load_explicit(&Q.pending_costs,memory_order_acquire); }
void vk_fg_qcom_rearm(void) {
    (void)fg_qcom_rearm(&refused,&Q.suspended,atomic_load_explicit(&Q.warmed,memory_order_acquire),
        atomic_load_explicit(&Q.cooling,memory_order_acquire),
        atomic_load_explicit(&Q.quarantined,memory_order_acquire),vk_fg_qcom_pending());
}
static uint64_t monotonic_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
/* SYNC_IOC_FILE_INFO reports kernel monotonic signal times, not polling time.
 * Require every constituent fence to have signaled. Fixed storage is bounded;
 * a driver returning a larger composite is an explicit timing refusal. */
static int fence_timestamp(int fd,uint64_t *timestamp,char *label,size_t label_size) {
    struct sync_fence_info fences[16];
    struct sync_file_info info={0};
    if (ioctl(fd,SYNC_IOC_FILE_INFO,&info) || info.status!=1 || !info.num_fences || info.num_fences>16) return 0;
    info.sync_fence_info=(uint64_t)(uintptr_t)fences;
    if (ioctl(fd,SYNC_IOC_FILE_INFO,&info) || info.status!=1 || !info.num_fences || info.num_fences>16) return 0;
    uint64_t latest=0; uint32_t selected=0;
    for (uint32_t i=0;i<info.num_fences;i++) {
        if (fences[i].status!=1 || !fences[i].timestamp_ns) return 0;
        if (fences[i].timestamp_ns>latest) { latest=fences[i].timestamp_ns; selected=i; }
    }
    if (label) snprintf(label,label_size,"%.*s/%.*s constituents=%u",
        (int)sizeof fences[selected].driver_name,fences[selected].driver_name,
        (int)sizeof fences[selected].obj_name,fences[selected].obj_name,info.num_fences);
    *timestamp=latest; return 1;
}
/* This worker owns the context until warmup completes. The render/presenter
 * threads keep using real-only adaptive or the locked camera engine and never join or wait for this worker.
 * Scratch GL textures warm the API/format/size path, not the game's AHB pixels. */
static void *warm_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&Q.mu);
    uint64_t start=monotonic_ns();
    if (!fg_qcom_initialize(&refused,adaptive_requested(),Q.model,Q.product_device,
            V.props.vendorID,V.props.deviceName,init,NULL) || !current()) {
        pthread_mutex_unlock(&Q.mu); return NULL;
    }
    GLuint texture[3]={0},fbo=0;
    AHardwareBuffer *buffer[3]={0}; EGLImageKHR image[3]={0};
    Q.a.glGenTextures(3,texture); Q.a.glGenFramebuffers(1,&fbo);
    Q.a.glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    for (unsigned i=0;i<3;i++) {
        AHardwareBuffer_Desc desc={.width=Q.warm_w,.height=Q.warm_h,.layers=1,
            .format=AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT,
            .usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER};
        if (AHardwareBuffer_allocate(&desc,&buffer[i])) {
            vk_fg_qcom_refuse("worker warmup AHB allocation failed; prior scratch resources quarantined");
            unbind(); pthread_mutex_unlock(&Q.mu); return NULL;
        }
        const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
        image[i]=Q.a.create_image(Q.display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,Q.a.native_buffer(buffer[i]),attrs);
        if (image[i]==EGL_NO_IMAGE_KHR) {
            vk_fg_qcom_refuse("worker warmup EGLImage import failed; scratch resources quarantined");
            unbind(); pthread_mutex_unlock(&Q.mu); return NULL;
        }
        Q.a.glBindTexture(GL_TEXTURE_2D,texture[i]);
        Q.a.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        Q.a.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        Q.a.target_image(GL_TEXTURE_2D,image[i]);
        Q.a.glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture[i],0);
        /* FBO renderability is not an AFME prerequisite. Initialize when
           supported; otherwise scratch contents/output are simply unobserved. */
        if (Q.a.glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE) {
            Q.a.glClearColor(i==0 ? .25f : 0,i==1 ? .5f : 0,0,1); Q.a.glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    uint64_t call=monotonic_ns();
    fg_qcom_extrapolate(Q.a.extrapolate,texture[0],texture[1],texture[2],.5f);
    Q.warm_call_ns=monotonic_ns()-call;
    GLenum error=Q.a.glGetError();
    EGLSyncKHR done=Q.a.create_sync(Q.display,EGL_SYNC_NATIVE_FENCE_ANDROID,NULL);
    Q.a.glFlush();
    int fd=done!=EGL_NO_SYNC_KHR ? Q.a.dup_fence(Q.display,done) : -1;
    if (done!=EGL_NO_SYNC_KHR) Q.a.destroy_sync(Q.display,done);
    if (done==EGL_NO_SYNC_KHR) {
        vk_fg_qcom_refuse("worker warmup native fence unavailable; scratch textures quarantined");
        unbind(); pthread_mutex_unlock(&Q.mu); return NULL;
    }
    if (fd<0 && Q.a.eglGetError()!=EGL_SUCCESS) {
        vk_fg_qcom_refuse("worker warmup fence export failed; scratch textures quarantined");
        unbind(); pthread_mutex_unlock(&Q.mu); return NULL;
    }
    /* Blocking is confined to this worker. Never retire scratch resources on a
       timeout; a real GPU hang leaves camera rendering subject to that hang. */
    if (fd>=0) {
        struct pollfd p={.fd=fd,.events=POLLIN};
        for (;;) {
            int r=poll(&p,1,1000);
            if (r>0 && (p.revents&POLLIN)) break;
            if ((r<0 && errno!=EINTR) || (r>0 && (p.revents&(POLLERR|POLLHUP|POLLNVAL)))) {
                close(fd); vk_fg_qcom_refuse("worker warmup fence failed; scratch textures quarantined");
                unbind(); pthread_mutex_unlock(&Q.mu); return NULL;
            }
        }
        close(fd);
    }
    Q.a.glBindFramebuffer(GL_FRAMEBUFFER,0); Q.a.glDeleteFramebuffers(1,&fbo);
    Q.a.glDeleteTextures(3,texture);
    for (unsigned i=0;i<3;i++) { Q.a.destroy_image(Q.display,image[i]); AHardwareBuffer_release(buffer[i]); }
    unbind();
    Q.warm_cpu_ns=monotonic_ns()-start;
    if (error!=GL_NO_ERROR) vk_fg_qcom_refuse("worker warmup GL extrapolation failed");
    else atomic_store_explicit(&Q.warmed,1,memory_order_release);
    LOGV("framegen qcom warmup: worker AHB/EGL RGBA16F %ux%u complete=%d wall=%.3f ms extrapolate-call=%.3f ms (three private scratch AHBs; no render-thread wait)",
        Q.warm_w,Q.warm_h,error==GL_NO_ERROR,Q.warm_cpu_ns/1e6,Q.warm_call_ns/1e6);
    pthread_mutex_unlock(&Q.mu); return NULL;
}
void vk_fg_qcom_prepare(uint32_t w,uint32_t h) {
    if (!vk_fg_qcom_requested() || atomic_load(&refused) || !Q.device) return;
    int expected=0;
    if (!atomic_compare_exchange_strong(&Q.worker_started,&expected,1)) return;
    Q.warm_w=w; Q.warm_h=h;
    pthread_t worker;
    if (pthread_create(&worker,NULL,warm_worker,NULL)) vk_fg_qcom_refuse("context warmup worker unavailable");
    else pthread_detach(worker);
}
/* Called only after the covering ordinary Vulkan ticket and GL native-fence
 * wait have retired. A failed bridge is quarantined, never destroyed/reused. */
static void image_free(qcom_image *im) {
    if (im->texture) Q.a.glDeleteTextures(1,&im->texture);
    if (im->egl) Q.a.destroy_image(Q.display,im->egl);
    if (im->view) vkDestroyImageView(V.dev,im->view,NULL);
    if (im->image) vkDestroyImage(V.dev,im->image,NULL);
    vk_memory_release(im->memory,VK_MEMORY_SNAPSHOT);
    if (im->buffer) AHardwareBuffer_release(im->buffer);
    memset(im,0,sizeof *im);
}
static int image_make(qcom_image *im,uint32_t w,uint32_t h,int output) {
    if (im->image && im->w==w && im->h==h) return 1;
    image_free(im);
    AHardwareBuffer_Desc desc={.width=w,.height=h,.layers=1,.format=AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT,
        .usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER};
    if (AHardwareBuffer_allocate(&desc,&im->buffer)) goto fail;
    VkAndroidHardwareBufferFormatPropertiesANDROID fp={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props={VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,.pNext=&fp};
    if (Q.ahb_properties(V.dev,im->buffer,&props)!=VK_SUCCESS || fp.format!=VK_FORMAT_R16G16B16A16_SFLOAT ||
        !(fp.formatFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        (!output && !(fp.formatFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))) goto fail;
    VkExternalMemoryImageCreateInfo external={VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&external,.imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R16G16B16A16_SFLOAT,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|
            (output ? 0 : VK_IMAGE_USAGE_TRANSFER_DST_BIT)};
    if (vkCreateImage(V.dev,&ci,NULL,&im->image)!=VK_SUCCESS) goto fail;
    int type=vk_pick_type(props.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type<0) goto fail;
    VkMemoryDedicatedAllocateInfo dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=im->image};
    VkImportAndroidHardwareBufferInfoANDROID imp={VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext=&dedicated,.buffer=im->buffer};
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&imp,
        .allocationSize=props.allocationSize,.memoryTypeIndex=(uint32_t)type};
    if (vk_memory_allocate(&ai,&im->memory,VK_MEMORY_SNAPSHOT)!=VK_SUCCESS || vkBindImageMemory(V.dev,im->image,im->memory,0)!=VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=im->image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_R16G16B16A16_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if (vkCreateImageView(V.dev,&vi,NULL,&im->view)!=VK_SUCCESS) goto fail;
    EGLClientBuffer native=Q.a.native_buffer(im->buffer);
    const EGLint attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
    im->egl=native ? Q.a.create_image(Q.display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,native,attrs) : EGL_NO_IMAGE_KHR;
    if (im->egl==EGL_NO_IMAGE_KHR) goto fail;
    Q.a.glGenTextures(1,&im->texture); Q.a.glBindTexture(GL_TEXTURE_2D,im->texture);
    Q.a.target_image(GL_TEXTURE_2D,im->egl);
    Q.a.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    Q.a.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    Q.a.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    Q.a.glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if (Q.a.glGetError()!=GL_NO_ERROR) goto fail;
    im->w=w; im->h=h; Q.imports++; return 1;
fail:
    image_free(im); vk_fg_qcom_refuse("AHB Vulkan/EGLImage/GL_TEXTURE_2D import failed"); return 0;
}
static void ownership(VkCommandBuffer cb,qcom_image *im,int acquire,int discard) {
    VkImageMemoryBarrier2 b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=acquire ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=acquire ? 0 : VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=acquire ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask=acquire ? VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT : 0,
        .oldLayout=discard ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : V.qfam,
        .dstQueueFamilyIndex=acquire ? V.qfam : VK_QUEUE_FAMILY_FOREIGN_EXT,.image=im->image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo di={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};
    vkCmdPipelineBarrier2(cb,&di);
}
int vk_fg_qcom_capture(VkCommandBuffer cb,unsigned index,VkImage scene,unsigned level,unsigned layer,uint32_t w,uint32_t h) {
    if (index>=SOURCES || !vk_fg_qcom_active()) return 0;
    vk_fg_qcom_harvest();
    if (!vk_fg_qcom_active()) return 0;
    pthread_mutex_lock(&Q.mu);
    qcom_source *s=&Q.source[index];
    for (unsigned i=0;i<VK_PRESENT_SLOT_CAPACITY;i++)
        if (fg_qcom_job_reads_source(&Q.output[i].job,index)) goto fail;
    s->copied=0;
    if (!current()) goto fail;
    uint64_t import_start=Q.diag ? monotonic_ns() : 0,imports=Q.imports;
    if (!image_make(&s->scene,w,h,0)) { unbind(); goto fail; }
    if (Q.diag && Q.imports!=imports) {
        uint64_t ns=monotonic_ns()-import_start;
        Q.import_calls++; Q.import_cpu_ns+=ns; if (ns>Q.import_cpu_max) Q.import_cpu_max=ns;
    }
    unbind();
    vkCmdResetQueryPool(cb,Q.copy_queries,index*2,2);
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,Q.copy_queries,index*2);
    ownership(cb,&s->scene,1,!s->scene.initialized);
    VkImageMemoryBarrier2 b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_BLIT_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=scene,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,1,layer,1}};
    VkDependencyInfo di={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};
    vkCmdPipelineBarrier2(cb,&di);
    VkImageBlit bl={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.srcOffsets={{0,0,0},{(int)w,(int)h,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int)w,(int)h,1}}};
    vkCmdBlitImage(cb,scene,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s->scene.image,VK_IMAGE_LAYOUT_GENERAL,1,&bl,VK_FILTER_NEAREST);
    b.srcStageMask=VK_PIPELINE_STAGE_2_BLIT_BIT; b.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
    b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cb,&di);
    ownership(cb,&s->scene,0,0); s->scene.initialized=1; s->copied=1;
    vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,Q.copy_queries,index*2+1);
    pthread_mutex_unlock(&Q.mu); return 1;
fail:
    pthread_mutex_unlock(&Q.mu); return 0;
}
/* Q.mu/current context held. Import and semaphore allocation are preparation,
 * never extrapolation; the prepared job has no timer/fence to harvest. */
static int output_resources(qcom_output *s,uint32_t w,uint32_t h) {
    uint64_t start=Q.diag ? monotonic_ns() : 0,imports=Q.imports;
    if (!image_make(&s->output,w,h,1)) return 0;
    if (Q.diag && Q.imports!=imports) {
        uint64_t ns=monotonic_ns()-start;
        Q.import_calls++; Q.import_cpu_ns+=ns; if (ns>Q.import_cpu_max) Q.import_cpu_max=ns;
    }
    if (!s->to_gl) {
        VkExportSemaphoreCreateInfo ex={VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&ex};
        if (vkCreateSemaphore(V.dev,&si,NULL,&s->to_gl)!=VK_SUCCESS) {
            vk_fg_qcom_refuse("Vulkan export semaphore creation failed"); return 0;
        }
        si.pNext=NULL;
        if (vkCreateSemaphore(V.dev,&si,NULL,&s->from_gl)!=VK_SUCCESS) {
            vk_fg_qcom_refuse("Vulkan import semaphore creation failed"); return 0;
        }
        Q.a.gen_queries(1,&s->query);
    }
    return 1;
}
int vk_fg_qcom_output_prepare(unsigned previous,unsigned index,unsigned slot,VkImageView *view) {
    if (previous>=SOURCES || index>=SOURCES || previous==index ||
        slot>=VK_PRESENT_SLOT_CAPACITY || !view || !vk_fg_qcom_active()) return 0;
    vk_fg_qcom_harvest();
    if (!vk_fg_qcom_active()) return 0;
    pthread_mutex_lock(&Q.mu);
    qcom_source *source=&Q.source[index],*old=&Q.source[previous];
    qcom_output *s=&Q.output[slot];
    int ok=0;
    if (fg_qcom_job_available(&s->job) && source->copied && old->copied &&
        source->scene.w==old->scene.w && source->scene.h==old->scene.h && current()) {
        ok=output_resources(s,source->scene.w,source->scene.h) &&
            fg_qcom_job_prepare(&s->job,previous,index);
        unbind();
    }
    if (ok) *view=s->output.view;
    pthread_mutex_unlock(&Q.mu);
    return ok;
}
void vk_fg_qcom_output_cancel(unsigned slot) {
    if (slot>=VK_PRESENT_SLOT_CAPACITY || !atomic_load_explicit(&Q.live,memory_order_acquire)) return;
    pthread_mutex_lock(&Q.mu);
    (void)fg_qcom_job_cancel(&Q.output[slot].job);
    pthread_mutex_unlock(&Q.mu);
}
void vk_fg_qcom_acquire(VkCommandBuffer cb,unsigned slot) {
    if (slot<VK_PRESENT_SLOT_CAPACITY) ownership(cb,&Q.output[slot].output,1,0);
}
static int extrapolate_output(unsigned previous,unsigned index,unsigned output_slot,
        uint64_t frame,float phase,VkImage *image,VkImageView *view,int prepared,uint64_t deadline) {
    fg_dispatch_trace *trace=prepared ? vk_fg_dispatch_trace_current : NULL;
    if (FG_DISPATCH_GUARD(trace,"qcom-entry","previous-index-range",previous>=SOURCES) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","source-index-range",index>=SOURCES) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","source-pair-identical",previous==index) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","output-slot-range",output_slot>=VK_PRESENT_SLOT_CAPACITY) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","image-null",!image) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","view-null",!view) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","qcom-phase-invalid",!fg_qcom_phase_valid(phase)) ||
        FG_DISPATCH_GUARD(trace,"qcom-entry","qcom-inactive-before-harvest",!vk_fg_qcom_active())) return 0;
    fg_dispatch_trace_event(trace,"stage","qcom-harvest-begin","cpu-harvest");
    vk_fg_qcom_harvest();
    fg_dispatch_trace_event(trace,"stage","qcom-harvest-end","cpu-harvest");
    if (FG_DISPATCH_GUARD(trace,"qcom-entry","qcom-inactive-after-harvest",!vk_fg_qcom_active())) return 0;
    pthread_mutex_lock(&Q.mu);
    int cost_started=0,own_output=0;
    qcom_source *source=&Q.source[index], *old=&Q.source[previous];
    qcom_output *s=&Q.output[output_slot];
    if ((prepared ?
        (FG_DISPATCH_GUARD(trace,"qcom-ownership","output-not-prepared",!s->job.prepared) ||
         FG_DISPATCH_GUARD(trace,"qcom-ownership","previous-owner-mismatch",s->job.previous!=previous) ||
         FG_DISPATCH_GUARD(trace,"qcom-ownership","source-owner-mismatch",s->job.current!=index)) :
        !fg_qcom_job_available(&s->job)) ||
        FG_DISPATCH_GUARD(trace,"qcom-ownership","source-not-copied",!source->copied) ||
        FG_DISPATCH_GUARD(trace,"qcom-ownership","previous-not-copied",!old->copied) ||
        FG_DISPATCH_GUARD(trace,"qcom-ownership","source-width-mismatch",source->scene.w!=old->scene.w) ||
        FG_DISPATCH_GUARD(trace,"qcom-ownership","source-height-mismatch",source->scene.h!=old->scene.h)) goto fail;
    own_output=1;
    if (trace && trace->enabled) s->trace=*trace;
    else s->trace.enabled=0;
    fg_dispatch_trace_event(trace,"stage","qcom-context-begin","cpu-call");
    if (FG_DISPATCH_GUARD(trace,"qcom-context","context-current-refused",!current())) goto fail;
    fg_dispatch_trace_event(trace,"stage","qcom-context-end","cpu-return");
    if (FG_DISPATCH_GUARD(trace,"qcom-resources","output-resources-refused",!output_resources(s,source->scene.w,source->scene.h))) { unbind(); goto fail; }
    if (prepared && FG_DISPATCH_GUARD(trace,"qcom-resources","deadline-before-signal",host_now_ns()>=deadline)) { unbind(); goto fail; }
    vk_submit_external_enable();
    VkSemaphoreGetFdInfoKHR gi={VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore=s->to_gl,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    fg_dispatch_trace_event(trace,"stage","qcom-signal-begin","submit-call");
    if (FG_DISPATCH_GUARD(trace,"qcom-signal","external-signal-refused",!vk_submit_signal(s->to_gl))) {
        unbind(); vk_fg_qcom_refuse("bounded external replay pin unavailable"); goto fail;
    }
    fg_dispatch_trace_event(trace,"stage","qcom-signal-end","cpu-submit-return");
    int fd=-1;
    /* If export yields -1 (already signaled), this monotonic instant is a
       conservative lower bound before any GL work. Otherwise retain its exact
       kernel signal time until output completion; EGL owns the original FD. */
    s->input_lower_ns=monotonic_ns();
    if (FG_DISPATCH_GUARD(trace,"qcom-input","input-fence-export-refused",Q.semaphore_fd(V.dev,&gi,&fd)!=VK_SUCCESS)) { unbind(); vk_fg_qcom_refuse("Vulkan SYNC_FD export failed"); goto fail; }
    s->input_monitor=fd>=0 ? dup(fd) : -1;
    if (fd>=0 && FG_DISPATCH_GUARD(trace,"qcom-input","input-monitor-dup-refused",s->input_monitor<0)) { close(fd); unbind(); vk_fg_qcom_refuse("Vulkan native-fence timing duplication failed"); goto fail; }
    EGLint attrs[]={EGL_SYNC_NATIVE_FENCE_FD_ANDROID,fd,EGL_NONE};
    EGLSyncKHR wait=Q.a.create_sync(Q.display,EGL_SYNC_NATIVE_FENCE_ANDROID,attrs);
    if (FG_DISPATCH_GUARD(trace,"qcom-input","egl-input-sync-refused",wait==EGL_NO_SYNC_KHR)) { if (fd>=0) close(fd); unbind(); vk_fg_qcom_refuse("EGL native-fence import failed"); goto fail; }
    EGLBoolean waited=Q.a.wait_sync(Q.display,wait,0);
    Q.a.destroy_sync(Q.display,wait);
    if (FG_DISPATCH_GUARD(trace,"qcom-input","egl-server-wait-refused",!waited)) { unbind(); vk_fg_qcom_refuse("EGL server wait failed"); goto fail; }
    if (prepared && FG_DISPATCH_GUARD(trace,"qcom-input","deadline-after-server-wait",host_now_ns()>=deadline)) { unbind(); goto fail; }
    atomic_fetch_add_explicit(&Q.pending_costs,1,memory_order_release);
    vk_fg_qcom_cost_begin(frame); cost_started=1;
    /* A failed post-dispatch bridge permanently quarantines this output; the
       retained scenes must not be overwritten merely because it wasn't queued. */
    if (prepared) (void)fg_qcom_job_dispatch(&s->job);
    else (void)fg_qcom_job_begin(&s->job,previous,index);
    if (adaptive_requested()) {
        pthread_mutex_lock(&Q.policy_mu);
        if (fg_qcom_timing_reprobe(&Q.policy,frame,output_slot)) timing_log("reprobe",frame,FG_QCOM_FAILURE_KINDS);
        if (trace && trace->enabled)
            LOGV("[adaptive-dispatch-stage] source=%llu charge=%llu output=%u phase=%.9g deadline-ns=%llu stage=qcom-timing-policy decision-ns=%llu clock=host-monotonic real-tick=%llu cooldown-until=%llu reprobe-frame=%llu reprobe-slot=%u charge-authority=prospective-dispatch-context charge-committed=UNKNOWN (existing policy result only)",
                (unsigned long long)trace->source,(unsigned long long)frame,output_slot,phase,
                (unsigned long long)deadline,(unsigned long long)host_now_ns(),
                (unsigned long long)Q.policy.tick,(unsigned long long)Q.policy.cooldown_until,
                (unsigned long long)Q.policy.reprobe_frame,Q.policy.reprobe_slot);
        pthread_mutex_unlock(&Q.policy_mu);
    }
    Q.a.begin_query(GL_TIME_ELAPSED_EXT,s->query);
    s->call_begin_ns=Q.diag || s->trace.enabled ? monotonic_ns() : 0;
    fg_qcom_extrapolate(Q.a.extrapolate,old->scene.texture,source->scene.texture,s->output.texture,phase);
    if (Q.diag || s->trace.enabled) s->call_end_ns=monotonic_ns();
    if (Q.diag) { uint64_t ns=s->call_end_ns-s->call_begin_ns; Q.call_cpu_ns+=ns; if (ns>Q.call_cpu_max) Q.call_cpu_max=ns; }
    Q.a.end_query(GL_TIME_ELAPSED_EXT);
    GLenum error=Q.a.glGetError();
    EGLSyncKHR done=Q.a.create_sync(Q.display,EGL_SYNC_NATIVE_FENCE_ANDROID,NULL);
    Q.a.glFlush();
    fd=done!=EGL_NO_SYNC_KHR ? Q.a.dup_fence(Q.display,done) : -1;
    if (done!=EGL_NO_SYNC_KHR) Q.a.destroy_sync(Q.display,done);
    s->flush_end_ns=Q.diag || s->trace.enabled ? monotonic_ns() : 0;
    unbind();
    if (s->trace.enabled)
        LOGV("[adaptive-dispatch-stage] source=%llu charge=%llu output=%u phase=%.9g deadline-ns=%llu stage=qcom-afme-submit call-begin-ns=%llu call-end-ns=%llu flush-end-ns=%llu clock=CLOCK_MONOTONIC decision-ns=%llu decision-clock=host-monotonic charge-authority=prospective-dispatch-context charge-committed=UNKNOWN afme-gpu-done=UNKNOWN final-gpu-done=UNKNOWN",
            (unsigned long long)s->trace.source,(unsigned long long)frame,output_slot,phase,
            (unsigned long long)deadline,(unsigned long long)s->call_begin_ns,
            (unsigned long long)s->call_end_ns,(unsigned long long)s->flush_end_ns,
            (unsigned long long)host_now_ns());
    if (FG_DISPATCH_GUARD(trace,"qcom-output",done==EGL_NO_SYNC_KHR ?
            "output-sync-create-refused" : "output-fence-export-refused",fd<0) ||
        FG_DISPATCH_GUARD(trace,"qcom-output","afme-gl-error",error!=GL_NO_ERROR)) {
        if (fd>=0) close(fd);
        vk_fg_qcom_refuse("GL extrapolation/native-fence export failed; private AHBs quarantined"); goto fail;
    }
    s->monitor=dup(fd);
    VkImportSemaphoreFdInfoKHR imp={VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=s->from_gl,
        .flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=fd};
    if (FG_DISPATCH_GUARD(trace,"qcom-output","output-monitor-dup-refused",s->monitor<0) ||
        FG_DISPATCH_GUARD(trace,"qcom-output","vulkan-output-fence-import-refused",Q.import_semaphore(V.dev,&imp)!=VK_SUCCESS)) {
        close(fd); if (s->monitor>=0) close(s->monitor); s->monitor=-1;
        vk_fg_qcom_refuse("Vulkan GL SYNC_FD import failed; private AHBs quarantined"); goto fail;
    }
    s->disjoint=s->bad=0;
    s->frame=frame; s->started=host_now_ns(); Q.generated++;
    atomic_fetch_add_explicit(&Q.pending_jobs,1,memory_order_release);
    vk_submit_wait(s->from_gl);
    if (!prepared) ownership(vk_cb(),&s->output,1,0);
    s->output.initialized=1;
    *image=s->output.image; *view=s->output.view;
    fg_dispatch_trace_event(trace,"stage","qcom-handoff-complete","cpu-return-not-gpu-done");
    pthread_mutex_unlock(&Q.mu);
    return 1;
fail:
    if (cost_started) fg_qcom_job_quarantine(&s->job);
    if (prepared && cost_started) fg_dispatch_trace_event(trace,"guard","qcom-failure","post-dispatch-quarantined");
    if (cost_started) { vk_fg_qcom_cost_end(frame,0,0); atomic_fetch_sub_explicit(&Q.pending_costs,1,memory_order_release); }
    if (own_output && s->input_monitor>=0) { close(s->input_monitor); s->input_monitor=-1; }
    pthread_mutex_unlock(&Q.mu); return prepared && cost_started ? -1 : 0;
}
int vk_fg_qcom_extrapolate(unsigned previous,unsigned index,unsigned output_slot,
        uint64_t frame,float phase,VkImage *image,VkImageView *view) {
    return extrapolate_output(previous,index,output_slot,frame,phase,image,view,0,0);
}
int vk_fg_qcom_output_dispatch(unsigned previous,unsigned index,unsigned output_slot,
        uint64_t frame,float phase,uint64_t deadline) {
    VkImage image; VkImageView view;
    return extrapolate_output(previous,index,output_slot,frame,phase,&image,&view,1,deadline);
}
void vk_fg_qcom_release(VkCommandBuffer cb,unsigned output_slot) {
    if (output_slot<VK_PRESENT_SLOT_CAPACITY) ownership(cb,&Q.output[output_slot].output,0,0);
}
void vk_fg_qcom_complete(unsigned output_slot) {
    if (output_slot>=VK_PRESENT_SLOT_CAPACITY || !atomic_load_explicit(&Q.live,memory_order_acquire)) return;
    pthread_mutex_lock(&Q.mu);
    fg_qcom_job_vk_complete(&Q.output[output_slot].job);
    pthread_mutex_unlock(&Q.mu);
}
void vk_fg_qcom_harvest(void) {
    if (!atomic_load_explicit(&Q.live,memory_order_acquire) ||
        !atomic_load_explicit(&Q.pending_jobs,memory_order_acquire)) return;
    uint64_t frame[VK_PRESENT_SLOT_CAPACITY]; double ms[VK_PRESENT_SLOT_CAPACITY];
    int valid[VK_PRESENT_SLOT_CAPACITY]; unsigned count=0;
    pthread_mutex_lock(&Q.mu);
    if (!Q.initialized || !current()) { pthread_mutex_unlock(&Q.mu); return; }
    GLint disjoint=0; Q.a.glGetIntegerv(GL_GPU_DISJOINT_EXT,&disjoint);
    for (unsigned i=0;i<VK_PRESENT_SLOT_CAPACITY;i++) {
        qcom_output *s=&Q.output[i]; if (!s->job.pending || s->job.quarantined) continue;
        s->disjoint|=!!disjoint;
        struct pollfd p={.fd=s->monitor,.events=POLLIN};
        int polled=poll(&p,1,0);
        if (adaptive_requested() && ((polled<0 && errno!=EINTR) ||
            (polled>0 && (p.revents&(POLLERR|POLLHUP|POLLNVAL)))))
            vk_fg_qcom_refuse("native-fence poll failed; outstanding private AHB leases retained");
        int ready=polled>0 && (p.revents&POLLIN);
        GLuint available=0; Q.a.query_available(s->query,GL_QUERY_RESULT_AVAILABLE_EXT,&available);
        int timed_out=host_now_ns()-s->started>QCOM_TIMEOUT_NS;
        if (!ready || !available) {
            if (timed_out && !s->bad) {
                s->bad=1; Q.timeout++; Q.missing++; failure(FG_QCOM_TIMING);
                atomic_fetch_add_explicit(&Q.quarantined,1,memory_order_release);
                frame[count]=s->frame; ms[count]=0; valid[count++]=0;
                fg_dispatch_trace_event(&s->trace,"guard","qcom-harvest","afme-harvest-timeout");
            }
            /* Native-fence completion is resource-retirement authority even
               when the timer result was lost. That output's cost is already
               unavailable; never keep rebinding GL forever just for its query. */
            if (!ready || !s->bad) continue;
        }
        GLuint64 elapsed=0;
        if (ready && available && !s->disjoint) Q.a.query_result(s->query,GL_QUERY_RESULT_EXT,&elapsed);
        uint64_t input=s->input_lower_ns,output=0;
        char input_label[96],output_label[96];
        if (Q.diag) { strcpy(input_label,"already-signaled"); strcpy(output_label,"unavailable"); }
        int fence_ok=ready && fence_timestamp(s->monitor,&output,Q.diag ? output_label : NULL,sizeof output_label) &&
            (s->input_monitor<0 || fence_timestamp(s->input_monitor,&input,Q.diag ? input_label : NULL,sizeof input_label)) && output>=input;
        double fence_ms=fence_ok ? (output-input)/1e6 : NAN;
        double charged=fence_ok ? fmax(elapsed/1e6,fence_ms) : NAN;
        int measured=ready && available && !s->disjoint && fence_ok;
        GLenum error=measured ? Q.a.glGetError() : GL_NO_ERROR;
        if (error!=GL_NO_ERROR && adaptive_requested())
            vk_fg_qcom_refuse("GL timing harvest error; corruption is not a recoverable timing miss");
        int ok=measured && error==GL_NO_ERROR;
        uint64_t timestamps[2];
        if (ready && vkGetQueryPoolResults(V.dev,Q.copy_queries,s->job.current*2,2,sizeof timestamps,timestamps,
            sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
            uint64_t mask=V.timestamp_bits>=64 ? UINT64_MAX : (UINT64_C(1)<<V.timestamp_bits)-1;
            double copy=(double)((timestamps[1]-timestamps[0])&mask)*V.props.limits.timestampPeriod/1e6;
            Q.copy_samples++; Q.copy_sum_ms+=copy; if (copy>Q.copy_max_ms) Q.copy_max_ms=copy;
        }
        if (!s->bad) { frame[count]=s->frame; ms[count]=charged; valid[count++]=ok; }
        if (ok) {
            Q.timed++; Q.sum_ms+=elapsed/1e6; if (elapsed/1e6>Q.max_ms) Q.max_ms=elapsed/1e6;
            Q.fence_sum_ms+=fence_ms; if (fence_ms>Q.fence_max_ms) Q.fence_max_ms=fence_ms;
            Q.charged_sum_ms+=charged; if (charged>Q.charged_max_ms) Q.charged_max_ms=charged;
            if (charged>QCOM_TIMEOUT_NS/1e6) {
                Q.slow++; if (!s->bad) failure(FG_QCOM_SLOW);
                if (Q.diag) LOGV("framegen qcom slow-span: frame=%llu span=%.3f ms input-to-call=%.3f call-host=%.3f call-end-to-flush=%.3f flush-to-output=%.3f ms (signed overlapping host/kernel-monotonic boundaries, not isolated GPU stages)",
                    (unsigned long long)s->frame,charged,((double)s->call_begin_ns-input)/1e6,
                    (s->call_end_ns-s->call_begin_ns)/1e6,(s->flush_end_ns-s->call_end_ns)/1e6,
                    ((double)output-s->flush_end_ns)/1e6);
                if (Q.diag) LOGV("framegen qcom slow-span fences: frame=%llu EGL-priority=%s input=%s output=%s (latest timestamp constituent labels; not KGSL context IDs or scheduler state)",
                    (unsigned long long)s->frame,priority_name(Q.priority),input_label,output_label);
            } else if (!s->bad && adaptive_requested()) {
                pthread_mutex_lock(&Q.policy_mu);
                if (fg_qcom_timing_recovered(&Q.policy,s->frame,i)) timing_log("recovery",s->frame,FG_QCOM_FAILURE_KINDS);
                pthread_mutex_unlock(&Q.policy_mu);
            }
        } else if (!s->bad) { Q.missing++; if (s->disjoint) Q.disjoint++; failure(FG_QCOM_TIMING); }
        if (s->trace.enabled)
            LOGV("[adaptive-dispatch-stage] source=%llu charge=%llu output=%u phase=%.9g deadline-ns=%llu stage=qcom-afme-harvest decision-ns=%llu decision-clock=host-monotonic harvest-cpu-ns=%llu harvest-clock=CLOCK_MONOTONIC afme-input-ns=%llu afme-input-origin=%s afme-output-signal-ns=%llu afme-output-clock=kernel-CLOCK_MONOTONIC fence-pair-valid=%d timer-available=%u timer-valid=%d afme-only=1 final-gpu-done=UNKNOWN charge-authority=prospective-dispatch-context charge-committed=UNKNOWN (harvest CPU is not hardware completion)",
                (unsigned long long)s->trace.source,(unsigned long long)s->frame,i,s->trace.phase,
                (unsigned long long)s->trace.deadline,(unsigned long long)host_now_ns(),
                (unsigned long long)monotonic_ns(),(unsigned long long)input,
                s->input_monitor<0 ? "CLOCK_MONOTONIC-conservative-lower-bound" : "kernel-CLOCK_MONOTONIC",
                (unsigned long long)output,fence_ok,available,ok);
        if (s->bad) atomic_fetch_sub_explicit(&Q.quarantined,1,memory_order_release);
        close(s->monitor); s->monitor=-1;
        if (s->input_monitor>=0) close(s->input_monitor);
        s->input_monitor=-1; fg_qcom_job_gl_complete(&s->job);
        atomic_fetch_sub_explicit(&Q.pending_jobs,1,memory_order_release);
    }
    unbind(); pthread_mutex_unlock(&Q.mu);
    for (unsigned i=0;i<count;i++) {
        vk_fg_qcom_cost_end(frame[i],ms[i],valid[i]);
        atomic_fetch_sub_explicit(&Q.pending_costs,1,memory_order_release);
    }
}
void vk_fg_qcom_stats(void) {
    if (!vk_fg_qcom_requested() || !Q.diag) return;
    if (atomic_load(&Q.worker_started) && !atomic_load_explicit(&Q.warmed,memory_order_acquire) && !atomic_load(&refused)) {
        LOGV("framegen qcom last 300 real: warmup=pending camera=%d (worker owns context; no render-thread wait)",!adaptive_requested());
        return;
    }
    pthread_mutex_lock(&Q.mu);
    pthread_mutex_lock(&Q.policy_mu); fg_qcom_policy policy=Q.policy;
    memset(Q.policy.failures,0,sizeof Q.policy.failures); Q.policy.startup_ignored=Q.policy.cooldown_skips=0;
    pthread_mutex_unlock(&Q.policy_mu);
    const char *reason=atomic_load_explicit(&refusal_reason,memory_order_acquire);
    LOGV("framegen qcom last 300 real: submitted=%llu refused=%llu missing-timing=%llu disjoint=%llu timeout=%llu imports=%llu GL mean/max=%.3f/%.3f ms native-fence-span=%.3f/%.3f ms charged=%.3f/%.3f ms samples=%llu AHB-copy mean/max=%.3f/%.3f ms samples=%llu late=%llu startup-ignored=%llu cooldown-skipped=%llu cooldown-left=%llu sustained-misses=%u fallback=%d reason=%s (copy in real; max(GL,native-fence-span) in incremental; fence span conservatively includes handoff/queue gaps)",
        (unsigned long long)Q.generated,(unsigned long long)atomic_exchange(&refusals,0),
        (unsigned long long)Q.missing,(unsigned long long)Q.disjoint,(unsigned long long)Q.timeout,
        (unsigned long long)Q.imports,Q.timed ? Q.sum_ms/Q.timed : NAN,Q.timed ? Q.max_ms : NAN,
        Q.timed ? Q.fence_sum_ms/Q.timed : NAN,Q.timed ? Q.fence_max_ms : NAN,
        Q.timed ? Q.charged_sum_ms/Q.timed : NAN,Q.timed ? Q.charged_max_ms : NAN,(unsigned long long)Q.timed,
        Q.copy_samples ? Q.copy_sum_ms/Q.copy_samples : NAN,Q.copy_samples ? Q.copy_max_ms : NAN,
        (unsigned long long)Q.copy_samples,(unsigned long long)policy.failures[FG_QCOM_DEADLINE],(unsigned long long)policy.startup_ignored,
        (unsigned long long)policy.cooldown_skips,
        (unsigned long long)(policy.cooldown_until>policy.tick ? policy.cooldown_until-policy.tick : 0),
        policy.misses,atomic_load(&refused),reason ? reason : "none");
    LOGV("framegen qcom phases: import host mean/max=%.3f/%.3f ms calls=%llu extrapolate-call host mean/max=%.3f/%.3f ms calls=%llu slow-span=%llu quarantined=%u warmup-successful-real=%llu/%u (host elapsed timings include descheduling; not GPU cost)",
        Q.import_calls ? Q.import_cpu_ns/1e6/Q.import_calls : NAN,Q.import_cpu_max/1e6,(unsigned long long)Q.import_calls,
        Q.generated ? Q.call_cpu_ns/1e6/Q.generated : NAN,Q.call_cpu_max/1e6,(unsigned long long)Q.generated,
        (unsigned long long)Q.slow,atomic_load(&Q.quarantined),(unsigned long long)policy.successes,FG_QCOM_WARM_TICKS);
    if (adaptive_requested())
        LOGV("framegen adaptive qcom timing totals: timing-cooldowns-total=%llu timing-reprobes-total=%llu timing-recoveries-total=%llu reprobe-pending=%d reprobe-frame=%llu permanent-refused=%d",
            (unsigned long long)policy.timing_cooldowns,(unsigned long long)policy.timing_reprobes,
            (unsigned long long)policy.timing_recoveries,policy.reprobe_pending,
            (unsigned long long)policy.reprobe_frame,fg_qcom_refused(&refused));
    Q.import_calls=Q.import_cpu_ns=Q.import_cpu_max=Q.call_cpu_ns=Q.call_cpu_max=Q.slow=0;
    Q.generated=Q.timed=Q.missing=Q.disjoint=Q.timeout=Q.imports=0; Q.sum_ms=Q.max_ms=0;
    Q.copy_samples=0; Q.copy_sum_ms=Q.copy_max_ms=0;
    Q.fence_sum_ms=Q.fence_max_ms=Q.charged_sum_ms=Q.charged_max_ms=0;
    pthread_mutex_unlock(&Q.mu);
}
#else
void vk_fg_qcom_device(PFN_vkGetInstanceProcAddr ip,PFN_vkGetDeviceProcAddr dp,int interop) {
    (void)ip; (void)dp; (void)interop;
    if (vk_fg_qcom_requested()) vk_fg_qcom_refuse("Android EGL/AHB interop unavailable on this host");
}
int vk_fg_qcom_active(void) { return 0; }
void vk_fg_qcom_late(void) {}
void vk_fg_qcom_tick(int scene) {
    (void)scene;
    if (vk_fg_qcom_requested()) {
        vk_overlay_set_framegen(VK_OVERLAY_FG_QCOM,adaptive_requested() ? VK_OVERLAY_FG_EXTRAP : VK_OVERLAY_FG_INTERP,
            VK_OVERLAY_FG_REFUSED,0,!adaptive_requested());
        (void)vk_submit_external_disable();
    }
}
void vk_fg_qcom_rearm(void) {}
int vk_fg_qcom_can_generate(void) { return 0; }
void vk_fg_qcom_prepare(uint32_t w,uint32_t h) { (void)w; (void)h; }
unsigned vk_fg_qcom_pending(void) { return 0; }
int vk_fg_qcom_capture(VkCommandBuffer cb,unsigned source,VkImage scene,unsigned level,unsigned layer,uint32_t w,uint32_t h) {
    (void)cb; (void)source; (void)scene; (void)level; (void)layer; (void)w; (void)h; return 0;
}
int vk_fg_qcom_extrapolate(unsigned previous,unsigned current,unsigned output_slot,
        uint64_t frame,float phase,VkImage *image,VkImageView *view) {
    (void)previous; (void)current; (void)output_slot; (void)frame; (void)phase; (void)image; (void)view; return 0;
}
int vk_fg_qcom_output_prepare(unsigned previous,unsigned index,unsigned slot,VkImageView *view) {
    (void)previous; (void)index; (void)slot; (void)view; return 0;
}
void vk_fg_qcom_output_cancel(unsigned slot) { (void)slot; }
void vk_fg_qcom_acquire(VkCommandBuffer cb,unsigned slot) { (void)cb; (void)slot; }
int vk_fg_qcom_output_dispatch(unsigned previous,unsigned index,unsigned slot,uint64_t frame,float phase,uint64_t deadline) {
    (void)previous; (void)index; (void)slot; (void)frame; (void)phase; (void)deadline; return 0;
}
void vk_fg_qcom_release(VkCommandBuffer cb,unsigned output_slot) { (void)cb; (void)output_slot; }
void vk_fg_qcom_complete(unsigned output_slot) { (void)output_slot; }
void vk_fg_qcom_harvest(void) {}
void vk_fg_qcom_stats(void) {}
#endif
