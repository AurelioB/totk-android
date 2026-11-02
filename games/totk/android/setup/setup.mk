# The on-device setup library and its tools. Included by android/Makefile; also usable alone:
#   make -f android/setup/setup.mk OUT=<dir> NDK=<ndk> HACTOOL_SRC=<dir> [CAPSTONE_PREFIX_ANDROID=.. CAPSTONE_PREFIX_HOST=..] setup-android setup-host
# Targets
#   $(OUT)/libtotk_setup.so      Android arm64 JNI library (com.aure.totk.setup.NativeSetup)
#   $(OUT)/libhactool.so         Android arm64 PIE executable, packed as a library so it lands in nativeLibraryDir
#   $(OUT)/totk-setup            host CLI (same sources minus JNI) for the importer tests; SANITIZE=1: ASan + UBSan
#   $(OUT)/host/libhactool.so    host hactool for those tests (the CLI looks for it in <dir of totk-setup>/host)
# Inputs: OUT, NDK + API (Android), HACTOOL_SRC (android/setup/hactool/deps.py prepare_hactool), the code stage's
# CAPSTONE_PREFIX_ANDROID / CAPSTONE_PREFIX_HOST or CAPSTONE_SRC (android/setup/rewrite/deps.py), HOST_CC, PYTHON.
# Each stage sub-directory provides sources.mk: rewrite/ (REWRITE_SRCS, REWRITE_INCLUDES, REWRITE_CFLAGS, REWRITE_LIBS),
# seed/ (SEED_*), companion/ (COMPANION_*), the same four variables; includes are bare directories.
# Everything defined here is SETUP_* so the including Makefile's own CC / CFLAGS / OBJS stay untouched.
# Sources are reached only through pattern rules (%.o: /%), so make may count them as intermediate files and delete
# them after the build (it did once, under `make -o`). GNU make >= 4.4: no file is ever intermediate.
.NOTINTERMEDIATE:
SETUP_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SETUP_TOTK := $(abspath $(SETUP_DIR)/../..)
ifeq ($(OUT),)
  $(error setup.mk: set OUT)
endif
PYTHON ?= python3
API ?= 33
HOST_CC ?= cc
SANITIZE ?= 0

-include $(SETUP_DIR)/rewrite/sources.mk
-include $(SETUP_DIR)/seed/sources.mk
-include $(SETUP_DIR)/companion/sources.mk

SETUP_PINS := $(OUT)/totk_pins.h
ifndef TOTK_PINS_H
$(SETUP_PINS): $(SETUP_TOTK)/scripts/make_pins_header.py $(SETUP_TOTK)/assets/expected-v1.4.3.json
	@mkdir -p $(dir $@)
	$(PYTHON) $(SETUP_TOTK)/scripts/make_pins_header.py $(SETUP_TOTK)/assets/expected-v1.4.3.json $@
endif

SETUP_CORE := $(addprefix $(SETUP_DIR)/,aes.c container.c containers.c fonts.c hactool_run.c json.c keys.c move.c romfs_image.c \
  romfs_index.c setup.c sha256.c stages.c util.c)
SETUP_STAGE_SRCS = $(REWRITE_SRCS) $(SEED_SRCS) $(COMPANION_SRCS)
# Capstone from source only for a target without a prebuilt prefix (decided when this file is read).
setup_capstone = $(if $(1),,$(if $(CAPSTONE_SRC),$(CAPSTONE_SRCS)))
SETUP_STAGE_INCLUDES = $(REWRITE_INCLUDES) $(SEED_INCLUDES) $(COMPANION_INCLUDES) $(if $(CAPSTONE_PREFIX),,$(if $(CAPSTONE_SRC),$(CAPSTONE_INCLUDES)))
SETUP_STAGE_LIBS = $(REWRITE_LIBS) $(SEED_LIBS) $(COMPANION_LIBS)
SETUP_HEADERS := $(wildcard $(SETUP_DIR)/*.h $(SETUP_DIR)/*/*.h) $(SETUP_TOTK)/runtime/romfs_proof.h
SETUP_CFLAGS = $(PREFIX_MAP) -std=gnu11 -O2 -g -fPIC -fvisibility=hidden -Wall -Wextra -I$(SETUP_DIR) -I$(OUT) $(addprefix -I,$(SETUP_STAGE_INCLUDES)) \
  $(REWRITE_CFLAGS) $(SEED_CFLAGS) $(COMPANION_CFLAGS)
SETUP_ANDROID_CC = $(NDK)/toolchains/llvm/prebuilt/$(shell uname -s | tr A-Z a-z)-x86_64/bin/clang --target=aarch64-linux-android$(API)
# 16 KB pages (Android 15+ devices); static libraries (capstone) stay internal so only the JNI entry points are exported.
SETUP_ANDROID_LDFLAGS := -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--exclude-libs,ALL
ifeq ($(SANITIZE),1)
  SETUP_HOST_SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
endif
# Objects mirror the absolute source path below one folder per target.
setup_objs = $(patsubst /%,$(OUT)/setup-obj/$(1)/%.o,$(abspath $(2)))

SETUP_ANDROID_SRCS = $(SETUP_CORE) $(SETUP_DIR)/setup_jni.c $(SETUP_STAGE_SRCS) $(call setup_capstone,$(CAPSTONE_PREFIX_ANDROID))
SETUP_HOST_SRCS = $(SETUP_CORE) $(SETUP_DIR)/setup_cli.c $(SETUP_STAGE_SRCS) $(call setup_capstone,$(CAPSTONE_PREFIX_HOST))
$(OUT)/libtotk_setup.so: CAPSTONE_PREFIX = $(CAPSTONE_PREFIX_ANDROID)
$(OUT)/totk-setup: CAPSTONE_PREFIX = $(CAPSTONE_PREFIX_HOST)

$(OUT)/libtotk_setup.so: $(call setup_objs,android,$(SETUP_ANDROID_SRCS))
	@test -n "$(NDK)" || { echo "setup.mk: set NDK"; exit 1; }
	$(SETUP_ANDROID_CC) -shared -o $@ $^ $(SETUP_STAGE_LIBS) $(SETUP_ANDROID_LDFLAGS) -Wl,--no-undefined -llog -lm
$(OUT)/totk-setup: $(call setup_objs,host$(if $(filter 1,$(SANITIZE)),-san),$(SETUP_HOST_SRCS))
	$(HOST_CC) $(SETUP_HOST_SAN) -o $@ $^ $(SETUP_STAGE_LIBS) -lm

# -Werror for the importer's own files only; capstone gets its own defines.
setup_extra = $(if $(filter $(SETUP_CORE) $(SETUP_DIR)/setup_jni.c $(SETUP_DIR)/setup_cli.c,$(1)),-Werror) $(if $(CAPSTONE_SRC),$(if $(filter $(CAPSTONE_SRC)/%,$(1)),$(CAPSTONE_CFLAGS)))
$(OUT)/setup-obj/android/%.o: /% $(SETUP_PINS) $(SETUP_HEADERS)
	@mkdir -p $(dir $@)
	$(SETUP_ANDROID_CC) $(SETUP_CFLAGS) $(call setup_extra,$<) -c $< -o $@
$(OUT)/setup-obj/host/%.o: /% $(SETUP_PINS) $(SETUP_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SETUP_CFLAGS) $(call setup_extra,$<) -c $< -o $@
$(OUT)/setup-obj/host-san/%.o: /% $(SETUP_PINS) $(SETUP_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SETUP_CFLAGS) $(SETUP_HOST_SAN) $(call setup_extra,$<) -c $< -o $@

# hactool (pinned source + build_app.py patches + the descriptor fallback, android/setup/hactool/deps.py).
SETUP_HACTOOL_C := save sha aes extkeys rsa npdm bktr kip packages pki pfs0 hfs0 nca0_romfs romfs utils nax0 nso lz4 nca xci \
  main filepath ConvertUTF cJSON
SETUP_MBEDCRYPTO_C := aes aesni arc4 asn1parse asn1write base64 bignum blowfish camellia ccm cipher cipher_wrap cmac ctr_drbg \
  des dhm ecdh ecdsa ecjpake ecp ecp_curves entropy entropy_poll error gcm gf128mul havege hmac_drbg md md2 md4 md5 md_wrap \
  memory_buffer_alloc oid padlock pem pk pk_wrap pkcs12 pkcs5 pkparse pkwrite platform ripemd160 rsa sha1 sha256 sha512 \
  threading timing version version_features xtea
SETUP_HACTOOL_SRCS = $(addprefix $(HACTOOL_SRC)/,$(addsuffix .c,$(SETUP_HACTOOL_C))) \
  $(addprefix $(HACTOOL_SRC)/mbedtls/library/,$(addsuffix .c,$(SETUP_MBEDCRYPTO_C)))
# The flags of hactool's own Makefile; its sources are third-party, so their warnings are not ours to fix.
SETUP_HACTOOL_CFLAGS = $(PREFIX_MAP) -std=gnu11 -O2 -fPIE -w -I$(HACTOOL_SRC)/mbedtls/include -D_BSD_SOURCE -D_POSIX_SOURCE \
  -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
$(OUT)/libhactool.so: $(call setup_objs,hactool-android,$(SETUP_HACTOOL_SRCS))
	$(SETUP_ANDROID_CC) -fPIE -pie -s -o $@ $^ $(SETUP_ANDROID_LDFLAGS)
$(OUT)/host/libhactool.so: $(call setup_objs,hactool-host,$(SETUP_HACTOOL_SRCS))
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^
$(OUT)/setup-obj/hactool-android/%.o: /%
	@test -n "$(HACTOOL_SRC)" || { echo "setup.mk: set HACTOOL_SRC (android/setup/hactool/deps.py)"; exit 1; }
	@mkdir -p $(dir $@)
	$(SETUP_ANDROID_CC) $(SETUP_HACTOOL_CFLAGS) -c $< -o $@
$(OUT)/setup-obj/hactool-host/%.o: /%
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SETUP_HACTOOL_CFLAGS) -c $< -o $@

.PHONY: setup-android setup-host
setup-android: $(OUT)/libtotk_setup.so $(OUT)/libhactool.so
setup-host: $(OUT)/totk-setup $(OUT)/host/libhactool.so
