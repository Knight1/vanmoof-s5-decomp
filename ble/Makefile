# ble — VanMoof S5/A5 Bluetooth LE SoC (Nordic nRF52-class, Zephyr + SoftDevice
# Controller).
#
# Reconstruction scaffold. Only the VanMoof application layer is translated to C
# (50 functions; see docs/progress.md). The Zephyr kernel + BT host, the Nordic
# SoftDevice Controller, the nRF SDK/CC310 and libgcc are vendor and are NOT
# reconstructed — they are satisfied at link time by the real upstream.
#
# CONFIRMED: ARM Cortex-M4F (VFPv4 single-precision, hard-float ABI). The OEM
# image is linked at device base 0x23000 (its MCUboot app slot); see
# docs/hardware.md. We do not yet link a full image (startup + vendor are
# off-tree), so the default target is the per-translation-unit *compile gate*:
# every src/*.c must compile clean with -Wall -Wextra -Wpedantic.

PROJECT      := ble
MCU          ?= cortex-m4
FPU          ?= fpv4-sp-d16
FLOAT_ABI    ?= hard
TOOLCHAIN    := arm-none-eabi-

CC           := $(TOOLCHAIN)gcc
OBJCOPY      := $(TOOLCHAIN)objcopy
OBJDUMP      := $(TOOLCHAIN)objdump
SIZE         := $(TOOLCHAIN)size

BUILD_DIR    := build
SRC_DIR      := src
INC_DIR      := include

OEM_IMAGE    ?= ble.20240129.145222.1.5.0.main.v1.5.0-main.bin

C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
DEPS         := $(OBJS:.o=.d)

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

.PHONY: all clean size disasm help
.DEFAULT_GOAL := all

# Default: the compile gate — build every TU to an object, clean.
all: $(OBJS)
	@if [ -z "$(strip $(OBJS))" ]; then \
		echo "ble: no sources in $(SRC_DIR)/ yet."; \
	else \
		echo "ble: compiled $(words $(OBJS)) translation unit(s) clean."; \
	fi

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

size: $(OBJS)
	@$(SIZE) $(OBJS)

disasm: $(OBJS)
	@for o in $(OBJS); do $(OBJDUMP) -d -S $$o; done > $(BUILD_DIR)/$(PROJECT).lst
	@echo "wrote $(BUILD_DIR)/$(PROJECT).lst"

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets: all (compile gate, default), size, disasm, clean"
	@echo "Variables: MCU=$(MCU)"

-include $(DEPS)
