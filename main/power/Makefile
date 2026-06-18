# Makefile — power service CAN/transport-core reconstruction (per-TU compile)
#
# The reconstructed C is behaviour-oriented, not a linkable rebuild (the rest of
# the service — the vm framework, libstdc++, mosquitto — is vendor/off-image).
# This just compiles each translation unit clean, like the other targets.
#
# Native AArch64 build (if the cross toolchain is present):
#   make CC=aarch64-linux-gnu-gcc
# Otherwise host gcc is used purely for a warning/syntax check (the C is portable).

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Iinclude
BUILD   := build
SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean
all: $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
