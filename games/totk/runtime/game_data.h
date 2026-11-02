/* What is derived from the player's own game files (module tables, RomFS / add-on content / shared-font sizes and hashes)
   is read at start from the game data folder, not compiled into this program: the program holds nothing of the game, so
   one build serves every player and can be shipped on its own (the public Android APK, upstream update_runtime.py).
   The folder: TOTK_GAME_DATA; by default game-data/<version> under the current folder on Android (the app's game folder,
   where its setup or build_android.py writes it) and build/game-data/<version> elsewhere (scripts/prepare_runtime.py,
   prepare_romfs_backend.py and prepare_shared_fonts.py write it there). */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* <folder>/<name>; stops the start when it does not fit. */
void runtime_game_data_path(const char *name, char *out, size_t cap);
/* The value of KEY in <folder>/<file> ("KEY VALUE" lines); stops the start when the file or the key is missing. */
const char *runtime_game_text(const char *file, const char *key);
uint64_t runtime_game_number(const char *file, const char *key);
/* A SHA-256 value (64 lowercase hex digits). */
const char *runtime_game_sha256(const char *file, const char *key);
