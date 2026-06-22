# charger — VanMoof S5 Liteon charger sub-ECU (ARM Cortex-M, bare-metal + mini-RTOS).
#
# Reconstruction scaffold. The charger runs Liteon-supplied firmware (model
# 5EL00000000EB); two build variants ship — `normal` and `speed` (fast charge) —
# which are the SAME code differing only in a charge-control parameter + version
# (see docs/variants.md). Only the charger APPLICATION layer is translated to C
# (the charge state machine + CAN handlers); newlib/libc, the Bosch M_CAN driver,
# the mini-RTOS/event core and flash self-programming are vendor and are NOT
# reconstructed.
#
# Primary image: charger_liteon_normal.0.0.1.8.0.untagged.x.bin (raw ARM Cortex-M,
# base 0x0, initial SP 0x20008000, soft-float). Default target is the per-TU
# compile gate.

PROJECT      := charger
MCU          ?= cortex-m4
FLOAT_ABI    ?= soft
TOOLCHAIN    := arm-none-eabi-

CC           := $(TOOLCHAIN)gcc
OBJCOPY      := $(TOOLCHAIN)objcopy
OBJDUMP      := $(TOOLCHAIN)objdump
SIZE         := $(TOOLCHAIN)size

BUILD_DIR    := build
SRC_DIR      := src
INC_DIR      := include

OEM_IMAGE    ?= charger_liteon_normal.0.0.1.8.0.untagged.x.bin

C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
DEPS         := $(OBJS:.o=.d)

ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfloat-abi=$(FLOAT_ABI)

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

all: $(OBJS)
	@if [ -z "$(strip $(OBJS))" ]; then \
		echo "charger: no sources in $(SRC_DIR)/ yet."; \
	else \
		echo "charger: compiled $(words $(OBJS)) translation unit(s) clean."; \
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

-include $(DEPS)
