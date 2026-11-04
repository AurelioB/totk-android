/* Android suspends the guest at acquire, after giving back its virtual core.
 * No renderer, filesystem, window or UI-thread lock is held by this gate. */
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    _Atomic int paused;
} AndroidGamePause;
#define ANDROID_GAME_PAUSE_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }

static inline void android_game_pause_set(AndroidGamePause *p, int paused) {
    pthread_mutex_lock(&p->lock);
    atomic_store_explicit(&p->paused, paused, memory_order_release);
    pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->lock);
}
/* A save may need later guest frames to finish. Let those frames through until
 * Commit, including a save that starts on another thread while we are asleep.
 * This poll does no filesystem I/O and never takes the save/close mutex. */
static inline void android_game_pause_wait(AndroidGamePause *p, int (*save_pending)(void)) {
    if (!atomic_load_explicit(&p->paused, memory_order_acquire)) return;
    pthread_mutex_lock(&p->lock);
    while (atomic_load_explicit(&p->paused, memory_order_acquire) && !save_pending()) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 50000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&p->changed, &p->lock, &until);
    }
    pthread_mutex_unlock(&p->lock);
}

#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
int runtime_android_game_paused(void);
void runtime_android_game_pause_wait(void);
void runtime_audio_output_pause(int paused); /* atomic notification only; AAudio work belongs to its worker */
#else
static inline int runtime_android_game_paused(void) { return 0; }
#endif
