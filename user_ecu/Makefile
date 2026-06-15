# user_ecu — VanMoof S5/A5 main vehicle controller (ARM Cortex-M).
#
# Analysis scaffold: no sources exist yet, so `make` currently has nothing to
# compile. The structure mirrors the S3 wares so that, once the startup path
# and exact MCU are confirmed, dropping .c/.S files into src/ "just works".
#
# Core CONFIRMED: ARM Cortex-M4F with an active VFPv4 single-precision FPU
# (hard-float ABI) — the control path is Q16.16 fixed-point but `FUN_00000370`
# uses genuine VFP ops (vfma.f32 etc.). MCU family is NXP S32K-like; exact SKU
# is still open (see docs/hardware.md).

PROJECT      := user_ecu
MCU          ?= cortex-m4
FPU          ?= fpv4-sp-d16
FLOAT_ABI    ?= hard
TOOLCHAIN    := arm-none-eabi-

CC           := $(TOOLCHAIN)gcc
AS           := $(TOOLCHAIN)gcc
LD           := $(TOOLCHAIN)gcc
OBJCOPY      := $(TOOLCHAIN)objcopy
OBJDUMP      := $(TOOLCHAIN)objdump
SIZE         := $(TOOLCHAIN)size

BUILD_DIR    := build
SRC_DIR      := src
INC_DIR      := include
LD_SCRIPT    := linker_user_ecu.ld

# OEM image to compare against (override on the command line if elsewhere).
OEM_IMAGE    ?= user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin

# --- sources -----------------------------------------------------------------
C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
S_SRCS       := $(wildcard $(SRC_DIR)/*.S)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
                $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS))
DEPS         := $(OBJS:.o=.d)

# --- flags -------------------------------------------------------------------
# Cortex-M4F hard-float (VFPv4 single-precision), confirmed from the OEM image.
ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfpu=$(FPU) -mfloat-abi=$(FLOAT_ABI)

CFLAGS       := $(ARCH_FLAGS) \
                -std=c11 \
                -Os -g3 \
                -ffunction-sections -fdata-sections \
                -fno-common -fno-builtin \
                -Wall -Wextra -Wpedantic -Wshadow \
                -Wno-unused-parameter \
                -I$(INC_DIR) \
                -MMD -MP

ASFLAGS      := $(ARCH_FLAGS) -g3 -I$(INC_DIR) -MMD -MP -Wa,--no-warn

LDFLAGS      := $(ARCH_FLAGS) \
                -T$(LD_SCRIPT) \
                -Wl,--gc-sections \
                -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
                -nostartfiles -nostdlib

LDLIBS       := -lgcc

# --- targets -----------------------------------------------------------------
.PHONY: all clean compare size disasm help
.DEFAULT_GOAL := all

all:
	@if [ -z "$(strip $(OBJS))" ]; then \
		echo "user_ecu: no sources in $(SRC_DIR)/ yet — this is the analysis scaffold."; \
		echo "See docs/progress.md for the decomp worklist."; \
	else \
		$(MAKE) $(BUILD_DIR)/$(PROJECT).bin size; \
	fi

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PROJECT).elf: $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

size: $(BUILD_DIR)/$(PROJECT).elf
	@$(SIZE) $<

disasm: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJDUMP) -d -S $< > $(BUILD_DIR)/$(PROJECT).lst
	@echo "wrote $(BUILD_DIR)/$(PROJECT).lst"

compare: $(BUILD_DIR)/$(PROJECT).bin
	@if [ ! -f "$(OEM_IMAGE)" ]; then \
		echo "OEM_IMAGE not found at $(OEM_IMAGE)"; exit 1; \
	fi
	@echo "OEM:  $$(stat -c%s $(OEM_IMAGE)) bytes  $(OEM_IMAGE)"
	@echo "OURS: $$(stat -c%s $(BUILD_DIR)/$(PROJECT).bin) bytes  $(BUILD_DIR)/$(PROJECT).bin"
	@diff_bytes=$$(cmp -l $(OEM_IMAGE) $(BUILD_DIR)/$(PROJECT).bin 2>/dev/null | wc -l); \
	echo "differing bytes: $$diff_bytes"
	@cmp $(OEM_IMAGE) $(BUILD_DIR)/$(PROJECT).bin && echo "BYTE-EQUIVALENT" || true

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets: all (default), size, disasm, compare, clean"
	@echo "Variables: MCU=$(MCU)  OEM_IMAGE=$(OEM_IMAGE)"

-include $(DEPS)
