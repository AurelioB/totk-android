/* Compare actual Vulkan queue graphs against 98336d3. Reuse the real scene/UI
 * fixture, not a replacement generator. Only completed timestamp VALUES are
 * deterministic test inputs: this is graph identity, not performance evidence. */
#define main framegen_fixture_main
#include "test_vkframegen.c"
#undef main

#if __has_include("../games/totk/android/gfx/framegen_qcom_policy.h")
#include "../games/totk/android/gfx/framegen_qcom_policy.h"
#if __has_include("../games/totk/android/gfx/vk_framegen_lsfg.h")
#include "../games/totk/android/gfx/vk_framegen_lsfg.h"
#endif
static void retired_external_prelude(int cooldown) {
    /* Real Vulkan semaphore/lease operations, not a fake GL implementation.
       Host capability refusal is genuine; exercise the state it must drain. */
    VkSemaphore semaphore;
    VkSemaphoreCreateInfo info={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(V.dev,&info,NULL,&semaphore));
    mtl_sub_begin(); vk_submit_external_enable();
    uint64_t frame=vk_fg_real_frame_id(); vk_fg_qcom_cost_begin(frame);
    uint64_t input=vk_submit_signal(semaphore);
    CHECK(input && !vk_submit_external_disable(),"external graph prefix owns a pending replay lease");
    vk_submit_wait(semaphore); uint64_t last=mtl_sub_commit();
    vk_fg_qcom_cost_end(frame,0,0); vk_fg_frame_end(last);
    CHECK(mtl_wait(last,UINT64_C(10000000000)),"external prefix actually retires");
#if __has_include("../games/totk/android/gfx/vk_framegen_lsfg.h")
    if (vk_fg_lsfg_requested()) vk_fg_lsfg_tick(0);
    else
#endif
        vk_fg_qcom_tick(0);
    CHECK(vk_submit_external_disable(),"camera suffix has no external lease/semaphore path");
    if (cooldown) {
        fg_qcom_policy policy={0};
        CHECK(!fg_qcom_failure(&policy,FG_QCOM_TIMING) && !fg_qcom_can_generate(&policy),
            "startup timing incident selects temporary camera cooldown without permanent refusal");
    } else {
#if __has_include("../games/totk/android/gfx/vk_framegen_lsfg.h")
        if (vk_fg_lsfg_requested()) vk_fg_lsfg_refuse("graph fixture: external failure after retirement");
        else
#endif
            vk_fg_qcom_refuse("graph fixture: external failure after retirement");
    }
    vkDestroySemaphore(V.dev,semaphore,NULL);
}
#endif
static PFN_vkQueueSubmit2 queue_submit;
static PFN_vkGetQueryPoolResults query_results;
static VkFence fences[8];
static unsigned nfences,submits;
static VkResult VKAPI_CALL trace_submit(VkQueue queue,uint32_t count,const VkSubmitInfo2 *info,VkFence fence) {
    unsigned slot=0; while (slot<nfences && fences[slot]!=fence) slot++;
    if (slot==nfences) { if (nfences==8) abort(); fences[nfences++]=fence; }
    for (uint32_t i=0;i<count;i++) printf("GRAPH submit %u slot %u commands %u waits %u signals %u\n",
        ++submits,slot,info[i].commandBufferInfoCount,info[i].waitSemaphoreInfoCount,info[i].signalSemaphoreInfoCount);
    return queue_submit(queue,count,info,fence);
}
static VkResult VKAPI_CALL deterministic_queries(VkDevice device,VkQueryPool pool,uint32_t first,uint32_t count,
    size_t size,void *data,VkDeviceSize stride,VkQueryResultFlags flags) {
    VkResult result=query_results(device,pool,first,count,size,data,stride,flags);
    if (result==VK_SUCCESS && (flags&VK_QUERY_RESULT_64_BIT)) {
        uint64_t step=(uint64_t)(1000000.0/V.props.limits.timestampPeriod);
        for (uint32_t i=0;i<count;i++) *(uint64_t *)((char *)data+i*stride)=(first+i+1)*step;
    }
    return result;
}
int main(int argc,char **argv) {
    setenv("TOTK_FRAMEGEN","locked",1); unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    setenv("TOTK_FPS60","0",1); setenv("TOTK_UNCAPPED","0",1); setenv("TOTK_FPS_AUTO","0",1); setenv("TOTK_REALTIME","1",1);
    mtl_init(); CHECK(vk_fg_locked() && vk_fg_enabled(),"graph probe uses actual locked adaptive mode");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    const float transparent[4]={0}; float camera[84]; fg_test_camera(camera,0,0,0,0);
    fixture_interp_ui(&f,transparent);
#if __has_include("../games/totk/android/gfx/framegen_qcom_policy.h")
    if (argc>1) retired_external_prelude(!strcmp(argv[1],"--after-cooldown"));
#else
    (void)argc; (void)argv;
#endif
    vk_gpu_idle(); queue_submit=vkQueueSubmit2; query_results=vkGetQueryPoolResults;
    vkQueueSubmit2=trace_submit; vkGetQueryPoolResults=deterministic_queries;
    vk_present_slots slots={0}; vk_present_interp interp={.retained=-1};
    const uint64_t refresh=16666667,base=1000000000;
    uint64_t ticket_origin=0;
    for (unsigned frame=0;frame<48;frame++) {
        /* Exercise the active non-FSR pool, not storage reserved for optional FSR. */
        unsigned output_slot=frame%(VK_PRESENT_INTERP_LEGACY_CAPACITY+vk_fg_lead_refreshes());
        mtl_sub_begin(); VkCommandBuffer cb=vk_cb(); vk_fg_camera(camera);
        CHECK(vk_fg_capture(cb,f.scene,0,0,f.depth,0,0,W,H),"graph source capture");
        int prepared=vk_fg_prepare_canvas(cb,W,H),replayed=fixture_render_replay(&f,cb,prepared);
        uint64_t real=mtl_sub_commit(),last=real; int generated=0; vk_post_output output;
        if (!ticket_origin) ticket_origin=real-1;
        if (replayed) {
            mtl_sub_begin(); cb=vk_cb(); generated=vk_fg_generate(cb,output_slot,W,H,0,&output);
            if (generated) vk_fg_gpu_end(cb,output_slot);
            last=mtl_sub_commit(); if (generated) vk_fg_synthetic_range(real+1,last);
        }
        vk_fg_track_result(VK_FG_ELIGIBLE); vk_fg_frame_end(last);
        CHECK(mtl_wait(last,UINT64_C(10000000000)),"graph ticket completes");
        int accepted=generated && vk_fg_complete(output_slot); vk_fg_stats();
        uint64_t callback=base+frame*2*refresh;
        vk_present_slot reserved=vk_present_slots_reserve(&slots,1,callback,refresh,2,callback+1000000);
        vk_present_interp_pair pair=vk_present_interp_push(&interp,(int)output_slot,frame+1,1,reserved,reserved.desired_ns+3000000);
        pair=vk_present_interp_buffer(pair,vk_fg_lead_refreshes(),reserved.desired_ns+3000000);
        printf("GRAPH frame %u real-ticket %llu last-ticket %llu output-slot %u generated %d accepted %d retained-real %d reason %u real-ns %llu midpoint-ns %llu\n",
            frame,(unsigned long long)(real-ticket_origin),(unsigned long long)(last-ticket_origin),output_slot,generated,accepted,pair.real,pair.reason,
            (unsigned long long)pair.slot.desired_ns,(unsigned long long)pair.midpoint_ns);
    }
    vk_gpu_idle(); vkQueueSubmit2=queue_submit; vkGetQueryPoolResults=query_results;
    printf("GRAPH end submits %u physical-slots %u failures %u\n",submits,nfences,failures);
    return failures ? 1 : 0;
}
