#pragma once
/* A fatal error the player should be told about (Vulkan device lost). In the Android app the reason is handed to the
   launcher's menu (TotkActivity.showGameError -> SetupActivity, process ":setup", which outlives the game process), so
   the abort that follows does not drop the player on the home screen without a word (runtime/frontend_android.c).
   Elsewhere a no-op: the reason is already in the log. Called once; later calls do nothing. */
#define RUNTIME_GAME_ERROR_GPU_LOST "The graphics driver stopped responding (Vulkan device lost), so the game had to close."
#if defined(__ANDROID__) && defined(TOTK_ANDROID_APP)
void runtime_android_game_error(const char *reason);
#else
static inline void runtime_android_game_error(const char *reason) { (void)reason; }
#endif
