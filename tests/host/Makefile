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
DRV     := $(SRC)/dev-can-ctucanfd.c $(SRC)/ctucanfd_devctl.h $(SRC)/ctucanfd_timer.h fake_libcan.c fake_libcan.h
TESTS   := test_dlc_timing test_frames test_core test_mode_rmw test_time test_driver test_fi test_txw_orders test_no_sleep test_rx_restart test_integ_hold test_supervisor

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD):
	mkdir -p $@

$(BUILD)/%: %.c $(HW) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW)

# includes the driver source; see the comment at the top of test_driver.c
$(BUILD)/test_driver: test_driver.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW) fake_libcan.c

# every order of TX wait events a few deep; includes the driver like test_driver
$(BUILD)/test_txw_orders: test_txw_orders.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW) fake_libcan.c

# no sleep inside libcan's callbacks; includes the driver like test_driver
$(BUILD)/test_no_sleep: test_no_sleep.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW) fake_libcan.c

$(BUILD)/test_rx_restart: test_rx_restart.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW) fake_libcan.c

$(BUILD)/test_integ_hold: test_integ_hold.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $< $(HW) fake_libcan.c

# The same tests against another tree's driver, for comparison:
#   make no_sleep_base BASE=/path/to/other/src
#   make rx_restart_base BASE=/path/to/other/src
# Only dev-can-ctucanfd.c and its own headers come from BASE.
no_sleep_base: test_no_sleep.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	test -f $(BASE)/dev-can-ctucanfd.c
	$(CC) -I$(BASE) $(CFLAGS) $(SAN) -o $(BUILD)/test_no_sleep_base $< $(HW) fake_libcan.c
	./$(BUILD)/test_no_sleep_base

# test_integ_hold.c against a driver without the integration hold: it must fail
integ_hold_base: test_integ_hold.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	test -f $(BASE)/dev-can-ctucanfd.c
	$(CC) -I$(BASE) -DBASE_DRIVER $(CFLAGS) $(SAN) -o $(BUILD)/test_integ_hold_base $< $(HW) fake_libcan.c
	./$(BUILD)/test_integ_hold_base

rx_restart_base: test_rx_restart.c $(HW) $(HDRS) $(DRV) driver_harness.h | $(BUILD)
	test -f $(BASE)/dev-can-ctucanfd.c
	$(CC) -I$(BASE) $(CFLAGS) $(SAN) -o $(BUILD)/test_rx_restart_base $< $(HW) fake_libcan.c
	./$(BUILD)/test_rx_restart_base

# the fault-injection build: the injector sits between the driver and the fake core
FI      := $(SRC)/ctucanfd_fi.c fi_glue.c fake_core_fi.c fake_core.c
$(BUILD)/test_fi: test_fi.c $(HW) $(HDRS) $(DRV) $(FI) driver_harness.h | $(BUILD)
	$(CC) $(CFLAGS) -DCTUCAN_FI $(SAN) -o $@ $< $(SRC)/ctucanfd_hw.c fi_glue.c fake_core_fi.c fake_libcan.c

# includes tools/ctucan-supervisor.c with its spawn and sleep replaced
$(BUILD)/test_supervisor: test_supervisor.c ../../tools/ctucan-supervisor.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

run: all
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done

clean:
	rm -rf $(BUILD)

.PHONY: all run clean no_sleep_base rx_restart_base integ_hold_base
