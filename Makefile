# Build with QNX SDP 8.0:  source ~/qnx800/qnxsdp-env.sh && make [ARCH=x86_64|aarch64le] [FI=1]
# FI=1 builds the fault-injection variant into build/<arch>-fi (docs/DESIGN.md, section 8).
ARCH    ?= x86_64
CC      := qcc -Vgcc_nto$(ARCH)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror=implicit-function-declaration
OUT     := build/$(ARCH)

DRV_SRC := src/dev-can-ctucanfd.c src/ctucanfd_hw.c
DRV_HDR := src/ctucanfd_hw.h src/ctucanfd_regs.h src/ctucanfd_devctl.h

ifeq ($(FI),1)
OUT     := $(OUT)-fi
CFLAGS  += -DCTUCAN_FI
DRV_SRC += src/ctucanfd_fi.c
endif

all: $(OUT)/dev-can-ctucanfd $(OUT)/canfd-test $(OUT)/ctucan-probe $(OUT)/ecu-sim $(OUT)/ctucan-supervisor

$(OUT):
	mkdir -p $@

# libcan.a is static; it needs libsecpol and libslog2 at link time.
$(OUT)/dev-can-ctucanfd: $(DRV_SRC) $(DRV_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(DRV_SRC) -lcan -lpci -lsecpol -lslog2

$(OUT)/canfd-test: tools/canfd-test.c src/ctucanfd_devctl.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/ecu-sim: tools/ecu-sim.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/ctucan-supervisor: tools/ctucan-supervisor.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/ctucan-probe: probe/ctucan_probe.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< -lpci

clean:
	rm -rf build

.PHONY: all clean
