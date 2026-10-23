/* The production metadata clock, without IPC, output devices or guest memory mappings. */
#include "../../runtime/ipc.h"
/* This fixture uses only the clock, not the constructor's IPC service registration. */
#define runtime_ipc_register(name, handler, open, close) ((void)0)
#include "../../runtime/audio_renderer.c"
#undef runtime_ipc_register

static void check(int scenario) {
    int16_t first[100] = {0}, second[480] = {0};
    AudioRenderer renderer = {0};
    ServerVoice voice = {.in = {.in_use=1, .sample_rate=48000, .pitch=1},
        .state=SV_STARTED, .valid={1,1}};
    voice.buffers[0] = (AudrenWaveBuffer){.address=(uintptr_t)first, .size=sizeof first,
        .end=100, .end_of_stream=scenario==0, .loop=scenario==2, .loop_count=-1};
    voice.buffers[1] = (AudrenWaveBuffer){.address=(uintptr_t)second, .size=sizeof second, .end=480};
    if (scenario==3) {
        voice.buffers[2] = voice.buffers[1];
        voice.buffers[1] = (AudrenWaveBuffer){.end_of_stream=1};
        voice.valid[2] = 1;
    }
    advance_voice(&renderer, &voice, 240.0/48000);
    unsigned consumed=scenario==2?0:scenario==3?2:1;
    unsigned index=scenario==2?0:scenario==3?2:1;
    uint64_t offset=scenario==2?40:((scenario==0||scenario==3)&&!AUDIO_EOS_PLAYS_ON)?0:140;
    uint64_t played=scenario==2||scenario==1?240:offset;
    if (voice.consumed!=consumed || voice.index!=index || voice.offset!=offset || voice.played!=played) {
        fprintf(stderr,"EOS clock %d got consumed/index/offset/played %u/%u/%llu/%llu; want %u/%u/%llu/%llu\n",
            scenario,voice.consumed,voice.index,(unsigned long long)voice.offset,(unsigned long long)voice.played,
            consumed,index,(unsigned long long)offset,(unsigned long long)played);
        exit(1);
    }
    if (scenario==0||scenario==3) {
        advance_voice(&renderer,&voice,240.0/48000);
        if (voice.offset!=offset+240 || voice.played!=played+240) exit(1);
    }
}

int main(void) {
    for (int scenario=0;scenario<4;scenario++) check(scenario);
    puts("4 EOS metadata paths checked (stream boundary, continuous, loop, empty EOS)");
    return 0;
}
