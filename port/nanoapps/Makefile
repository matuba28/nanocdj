# nanoCDJ as a NanoApps raw-surface app.
#   ./build.sh      -> build/nanocdj.hbapp   (plain `make` works if the path has no spaces)
# Needs arm-none-eabi-gcc + pyelftools, and the NanoApps SDK: the third_party/NanoApps
# submodule (override with NANOAPPS=/path/to/NanoApps).
#
# Two halves, like NanoApps' own floating-point ports:
#   - entry.c + the NanoApps SDK: the SDK's default flags (Thumb, soft float). The
#     resident enters apps in Thumb state, and no FP instruction may run before the
#     task's FP context is attached (entry.c does that).
#   - nanoCDJ core + port: ARM state with VFP, built into libnanocdj.a. Thumb-state
#     VFP traps on this firmware, so FP code must be ARM.

NANOAPPS ?= ../../third_party/NanoApps
CORE := ../../src

APP_NAME    := nanocdj
RAW_SURFACE := 1
SRCS := entry.c audio_ring.c   # Thumb / no-FPU side (audio_ring runs the resident's unload hook)
CHECK_NO_THUMB_FP := 1

# Audio-ring overrides for test builds, e.g. ./build.sh RING_DEFS="-DRING_BLOCK_MS=200"
RING_DEFS ?=
EXTRA_CFLAGS += $(RING_DEFS)
include $(NANOAPPS)/sdk/hb_app.mk

ARM_FLAGS := -marm -mcpu=cortex-a8 -mfloat-abi=softfp -mfpu=vfpv3-d16 -mword-relocations \
             -Os -ffreestanding -fno-builtin -nostdlib -fno-common -fno-strict-aliasing \
             -fno-jump-tables -ffunction-sections -fdata-sections -Wall -Wextra \
             -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
             -I$(SDK_DIR) -I$(SDK_DIR)/freestanding -Iinclude -include compat.h -DHB_DEVICE -DHB_LV_RELOC
LIB_SRCS := hal_nanoapps.c libc.c libm.c \
            $(CORE)/app.c $(CORE)/deck.c $(CORE)/mp3stream.c $(CORE)/anlz.c $(CORE)/pdb.c \
            $(CORE)/library.c $(CORE)/gfx.c $(CORE)/util.c
LIB_OBJS := $(patsubst %.c,$(BUILD)/arm/%.o,$(notdir $(LIB_SRCS)))
LIB      := $(BUILD)/libnanocdj.a
vpath %.c . $(CORE)

$(BUILD)/arm/%.o: %.c compat.h $(wildcard $(CORE)/*.h) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(ARM_FLAGS) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	$(AR) crs $@ $^

# link the ARM library ahead of libgcc; the resident's loader handles the relocations
LDLIBS := $(LIB) $(LIBGCC)
$(BUILD)/$(APP_NAME).reloc.elf: $(LIB)
