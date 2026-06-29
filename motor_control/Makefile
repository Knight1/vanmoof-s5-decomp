# vanmoof-s5-decomp — motor_control (TI TMS320F280049C, C2000/C28x DSP — NOT ARM)
#
# There is no GCC backend for the C28x and stock Ghidra has no C28x processor
# module, so — exactly like the S3 `motorware` target — this is an analysis-only
# Makefile. It drives tools/bootstream.py, which parses the C28x boot-table and
# extracts the flash regions (raw LE, 2 bytes/word) for disassembly by a C28x
# tool (IDA `tms32028`, or a Ghidra C28x SLEIGH). See README.md "Tooling".

PROJECT      := motor_control
PY           := python3
BUILD_DIR    := build
TOOLS        := tools

OEM_DIR      ?= ../../VanMooof-Firmware/SA5/v1.5.0-main_device_files
OEM_IMAGE    ?= $(OEM_DIR)/motor_control.20240129.154239.1.5.0.main.v1.5.0-main.bin

IDA          ?= /mnt/c/Program Files/IDA 7.0/idat64.exe

.PHONY: all info image ida clean help
.DEFAULT_GOAL := help

all: info image

# Boot-table report: key, entry point, flash segment map.
info: $(OEM_IMAGE)
	$(PY) $(TOOLS)/bootstream.py "$(OEM_IMAGE)"

# Extract the reconstructed flash regions (raw LE, 2 bytes/word) + manifest.
image: $(OEM_IMAGE) | $(BUILD_DIR)
	$(PY) $(TOOLS)/bootstream.py --extract=$(BUILD_DIR)/image "$(OEM_IMAGE)"
	@echo "regions + manifest in $(BUILD_DIR)/image/"

# Disassemble the C28x app with IDA 7.0's tms32028 processor (headless from WSL),
# exporting build/ida/functions.asm + funcmap.json + mmio_hits.json.
ida: image | $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/ida
	TVHEADLESS=1 "$(IDA)" -c -A -ptms32028 \
		-L"$$(wslpath -m $(BUILD_DIR)/ida/ida.log)" \
		-S"$$(wslpath -m ida/build_db.py) $$(wslpath -m $(BUILD_DIR)/image) $$(wslpath -m $(BUILD_DIR)/ida)" \
		"$$(wslpath -m $(BUILD_DIR)/image/region_08065C.bin)"
	@echo "disassembly -> $(BUILD_DIR)/ida/functions.asm"

$(BUILD_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "motor_control (TMS320F280049C / C2000 — analysis only; needs a C28x disassembler)"
	@echo "Targets:"
	@echo "  info   — boot-table report (key, entry, flash segment map)"
	@echo "  image  — extract flash regions -> $(BUILD_DIR)/image/ (for IDA/Ghidra-C28x)"
	@echo "  ida    — disassemble with IDA 7.0 tms32028 -> $(BUILD_DIR)/ida/functions.asm"
	@echo "  clean  — remove $(BUILD_DIR)/"
	@echo "Variables: OEM_IMAGE=$(OEM_IMAGE)"
