# The companion stage (setup_stage_companion: a C port of scripts/make_companion_assets.py + companion_decor.py +
# bflyt_render.py), included by android/setup/setup.mk after seed/sources.mk: the RomFS / Zstandard / SARC reader is the
# seed stage's (seed/romfs_read.c, compiled once in SEED_SRCS), so ZSTD_PREFIX_* must be set as for the seed stage.
# libwebp comes from deps.py prepare_libwebp() as an install prefix per target:
#   WEBP_PREFIX_ANDROID  (libtotk_setup.so)   WEBP_PREFIX_HOST  (totk-setup)
# selected per target like ZSTD_PREFIX, so the variables that read WEBP_PREFIX are recursive (=).
COMPANION_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
COMPANION_SRCS := $(addprefix $(COMPANION_DIR)/,stage_companion.c cg_image.c cg_texture.c cg_formats.c cg_layout.c cg_webp.c)

$(OUT)/libtotk_setup.so: WEBP_PREFIX = $(WEBP_PREFIX_ANDROID)
$(OUT)/totk-setup: WEBP_PREFIX = $(WEBP_PREFIX_HOST)
companion_webp = $(if $(WEBP_PREFIX),$(WEBP_PREFIX),$(error setup.mk: set WEBP_PREFIX_ANDROID / WEBP_PREFIX_HOST (android/setup/companion/deps.py prepare_libwebp)))

COMPANION_INCLUDES = $(COMPANION_DIR) $(SEED_DIR) $(companion_webp)/include
COMPANION_CFLAGS =
COMPANION_LIBS = $(addprefix $(companion_webp)/lib/,libwebpdemux.a libwebpmux.a libwebp.a libsharpyuv.a)
