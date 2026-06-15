# Top-level dispatcher for VanMoof S5/A5 firmware decompilations.
#
# Each target lives in its own subdirectory with its own Makefile. This file
# is a thin forwarder so you can run `make user_ecu` (or just `make`, which
# builds the active target) from the repo root. Targets are added here as
# their subdirectories are scaffolded.

WARES        := user_ecu
ACTIVE_WARE  := user_ecu

.PHONY: all clean help $(WARES) all-wares compare size disasm

all: $(ACTIVE_WARE)

help:
	@echo "Targets:"
	@echo "  <target>      — build one target ($(WARES))"
	@echo "  all-wares     — build every target listed in \$$(WARES)"
	@echo "  clean         — clean every target"
	@echo "  compare       — diff active target against its OEM image"
	@echo "  size          — show size of active target"
	@echo "Active target: $(ACTIVE_WARE)"

$(WARES):
	$(MAKE) -C $@

all-wares: $(WARES)

clean:
	@for w in $(WARES); do $(MAKE) -C $$w clean; done

compare size disasm:
	$(MAKE) -C $(ACTIVE_WARE) $@
