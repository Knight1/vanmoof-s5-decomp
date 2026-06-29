# motor_sensor — VanMoof S5/A5 motor position/current/temp sensor ECU
# (NXP Cortex-M4, LPC546xx-class, FreeRTOS).
#
# Reconstruction scaffold. Only the VanMoof application layer is translated to C
# (the sensor sampling + CAN-FD reporting + the FreeRTOS tasks; see
# docs/progress.md). The FreeRTOS kernel + Cortex-M port, the NXP LPC SDK/HAL
# (M_CAN, ADC, SCT/CTimer, GPIO/IOCON, clock, IAP-flash) and libgcc are vendor
# and are NOT reconstructed — satisfied at link time by the real upstream.
#
# CONFIRMED: raw ARM Cortex-M image, base 0x0 (raw vector table, initial SP
# 0x20008000); FreeRTOS port present; see docs/hardware.md. The default target is
# the per-translation-unit *compile gate*: every src/*.c compiles clean with
# -Wall -Wextra -Wpedantic.

PROJECT      := motor_sensor
MCU          ?= cortex-m4
FPU          ?= fpv4-sp-d16
FLOAT_ABI    ?= hard
TOOLCHAIN    := arm-none-eabi-

CC           := $(TOOLCHAIN)gcc
OBJDUMP      := $(TOOLCHAIN)objdump
SIZE         := $(TOOLCHAIN)size

BUILD_DIR    := build
SRC_DIR      := src
INC_DIR      := include

OEM_IMAGE    ?= motor_sensor.20240129.145222.1.5.0.main.v1.5.0-main.bin

C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
DEPS         := $(OBJS:.o=.d)

ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfpu=$(FPU) -mfloat-abi=$(FLOAT_ABI)

# -Wno-cast-function-type: the OEM passes worker fns to the void(*)(void*)
# task-create ABI — the same cast FreeRTOS' own xTaskCreate requires.
CFLAGS       := $(ARCH_FLAGS) \
                -std=c11 \
                -Os -g3 \
                -ffunction-sections -fdata-sections \
                -fno-common -fno-builtin \
                -Wall -Wextra -Wpedantic -Wshadow \
                -Wno-unused-parameter \
                -Wno-cast-function-type \
                -I$(INC_DIR) \
                -MMD -MP

.PHONY: all clean size disasm help
.DEFAULT_GOAL := all

all: $(OBJS)
	@if [ -z "$(strip $(OBJS))" ]; then \
		echo "motor_sensor: no sources in $(SRC_DIR)/ yet."; \
	else \
		echo "motor_sensor: compiled $(words $(OBJS)) translation unit(s) clean."; \
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
