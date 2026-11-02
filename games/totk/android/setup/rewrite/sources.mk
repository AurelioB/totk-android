# The code stage (C port of the rewriter and of the runtime-tables writer), included by android/setup/setup.mk.
# Capstone 5.0.9 (arm64 only) comes from one of:
#   CAPSTONE_PREFIX  an install prefix from deps.py prepare_capstone() (include/capstone/capstone.h, lib/libcapstone.a);
#                    setup.mk sets it per target, so the variables below that read it are recursive (=)
#   CAPSTONE_SRC     the pinned source tree (deps.py fetch_capstone()); setup.mk then compiles CAPSTONE_SRCS with
#                    CAPSTONE_CFLAGS / CAPSTONE_INCLUDES into its own per-target object folders
# The stage also needs totk_pins.h (setup.mk generates it into $(OUT) and adds -I$(OUT)).
REWRITE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REWRITE_SRCS := $(addprefix $(REWRITE_DIR)/,rewrite.c nso.c tables.c rw_util.c code.c stage_code.c)
# Host-only driver for the oracle tests (tests/setup/test_rewrite_parity.py); not part of libtotk_setup.so.
REWRITE_CLI_SRCS := $(REWRITE_DIR)/rewrite_cli.c

CAPSTONE_SRCS = $(addprefix $(CAPSTONE_SRC)/,cs.c Mapping.c MCInst.c MCInstrDesc.c MCRegisterInfo.c SStream.c utils.c \
  arch/AArch64/AArch64BaseInfo.c arch/AArch64/AArch64Disassembler.c arch/AArch64/AArch64InstPrinter.c \
  arch/AArch64/AArch64Mapping.c arch/AArch64/AArch64Module.c)
CAPSTONE_CFLAGS = -DCAPSTONE_HAS_ARM64 -DCAPSTONE_USE_SYS_DYN_MEM -w
CAPSTONE_INCLUDES = $(CAPSTONE_SRC)/include $(CAPSTONE_SRC)

REWRITE_INCLUDES = $(REWRITE_DIR) $(if $(CAPSTONE_PREFIX),$(CAPSTONE_PREFIX)/include,$(CAPSTONE_SRC)/include)
REWRITE_CFLAGS = -std=gnu11
REWRITE_LIBS = $(if $(CAPSTONE_PREFIX),$(CAPSTONE_PREFIX)/lib/libcapstone.a)
