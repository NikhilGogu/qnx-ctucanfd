# Host unit tests for the parts of the driver that need no QNX: the hardware
# layer (src/ctucanfd_hw.c) on a fake core, and pure helpers. Any C11 compiler:
#   make -C tests/host run
# SAN= turns the sanitizers off where the toolchain has none.
SRC     := ../../src
BUILD   := build
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow \
           -DEOK=0 -DCTUCAN_HW_EXTERN_IO -Ishim -I$(SRC)
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

HW      := $(SRC)/ctucanfd_hw.c fake_core.c
HDRS    := $(SRC)/ctucanfd_hw.h $(SRC)/ctucanfd_regs.h $(SRC)/ctucanfd_time.h fake_core.h check.h shim/sys/can_dcmd.h
TESTS   := test_dlc_timing test_frames test_core test_time

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD):
	mkdir -p $@

$(BUILD)/%: %.c $(HW) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW)

run: all
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
