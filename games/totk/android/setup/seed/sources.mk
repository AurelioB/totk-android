# The shader seed stage (setup_stage_seed: a C port of scripts/prepare_shader_seed.py for Vulkan + scripts/pack_shader_seed.py),
# included by android/setup/setup.mk. Zstandard comes from deps.py prepare_zstd() as an install prefix per target:
#   ZSTD_PREFIX_ANDROID  (libtotk_setup.so)   ZSTD_PREFIX_HOST  (totk-setup)
# Each target selects its own prefix below (as setup.mk does for CAPSTONE_PREFIX), so the variables that read ZSTD_PREFIX
# are recursive (=); objects inherit the value from the library they are built for.
SEED_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SEED_SRCS := $(addprefix $(SEED_DIR)/,stage_seed.c romfs_read.c inventory.c shader_list.c shader_files.c translate.c pack.c)
# Host/shell driver for the oracle test (tests/setup/test_seed_parity.py) and device measurements; not part of
# libtotk_setup.so. It links SEED_SRCS with the core's util.c, json.c and sha256.c only.
SEED_CLI_SRCS := $(SEED_DIR)/seed_cli.c

$(OUT)/libtotk_setup.so: ZSTD_PREFIX = $(ZSTD_PREFIX_ANDROID)
$(OUT)/totk-setup: ZSTD_PREFIX = $(ZSTD_PREFIX_HOST)
seed_zstd = $(if $(ZSTD_PREFIX),$(ZSTD_PREFIX),$(error setup.mk: set ZSTD_PREFIX_ANDROID / ZSTD_PREFIX_HOST (android/setup/seed/deps.py prepare_zstd)))

SEED_INCLUDES = $(SEED_DIR) $(seed_zstd)/include
SEED_CFLAGS =
SEED_LIBS = $(seed_zstd)/lib/libzstd.a
