# Host unit tests: the hardware layer (src/ctucanfd_hw.c) on a fake core, pure
# helpers such as src/ctucanfd_time.h, and the driver (src/dev-can-ctucanfd.c)
# on that core with a fake libcan and the stand-in headers in shim/. Any C11
# compiler:
#   make -C tests/host run
# SAN= turns the sanitizers off where the toolchain has none.
SRC     := ../../src
BUILD   := build
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow \
           -DEOK=0 -DCTUCAN_HW_EXTERN_IO -Ishim -I$(SRC)
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

HW      := $(SRC)/ctucanfd_hw.c fake_core.c
HDRS    := $(SRC)/ctucanfd_hw.h $(SRC)/ctucanfd_regs.h $(SRC)/ctucanfd_time.h fake_core.h check.h $(wildcard shim/*.h shim/*/*.h)
DRV     := $(SRC)/dev-can-ctucanfd.c $(SRC)/ctucanfd_devctl.h fake_libcan.c fake_libcan.h
TESTS   := test_dlc_timing test_frames test_core test_time test_driver test_supervisor

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD):
	mkdir -p $@

$(BUILD)/%: %.c $(HW) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW)

# includes the driver source; see the comment at the top of test_driver.c
$(BUILD)/test_driver: test_driver.c $(HW) $(HDRS) $(DRV) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW) fake_libcan.c

# includes tools/ctucan-supervisor.c with its spawn and sleep replaced
$(BUILD)/test_supervisor: test_supervisor.c ../../tools/ctucan-supervisor.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

run: all
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
