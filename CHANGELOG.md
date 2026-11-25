# Changelog

This file starts with the changes after commit 30fff05; earlier history is in `git log`.

## Unreleased

### Fixed

- Start and stop wrote MODE and SETTINGS as a whole word built from zero. On a core that has them, this cleared MODE.RXBAM (RX buffer automatic mode) and SETTINGS.TBFBO (TXT buffers go to "TX failed" at bus-off), whose reset value is 1. Now the word is read first and only the driver's own bits change, and start sets RXBAM and TBFBO to 1 in every write, also on a core that held them at 0 (`src/ctucanfd_hw.c`; datasheet 3.1.3, 3.1.4). QEMU 6.2 ignores both bits, so no earlier test showed it. `tests/host/test_mode_rmw.c` checks it. Not run on a core that has the bits.
- After ENA the core is bus-off while it integrates, and with TBFBO = 1 a Set ready command in that time moves the TXT buffer to "Aborted" (datasheet 2.9.7, note). The driver now holds TX after start-up and after every restart when FAULT_STATE reads bus-off, and releases it at FCSI with error active or by one read of FAULT_STATE where TX would be admitted. It is kept apart from the bus-off hold and is not counted as a recovery. `GET_XSTATS` state bit 12 shows it. A reading of none, as QEMU 6.2 gives, or of error passive sets no hold. Checked only on the host fake core (`tests/host/test_integ_hold.c`): QEMU 6.2 has no integration phase. The fake core now reads FAULT_STATE as 0 after a reset, as QEMU does. `tx_kick()` also reads FAULT_STATE once before a Set ready command and sends nothing in that kick when it reads bus-off, so a TXBHCI pass or a write that comes before the FCSI no longer sets a buffer Ready at bus-off (`docs/DESIGN.md`, section 8). Only the register accesses between the read and the command remain. Also the comment at `case TXT_ERR:` now names all three causes of "TX failed".
- Driver: a core that fails the identity check is no longer written to at teardown.
- Driver: with `-C` (classic only) no data phase timing is needed. `BTR_FD` gets a fixed tuple that fits its fields when none exists.
- Driver: numeric options take the whole string. `-R junk` and `-r 4294967297` are refused instead of becoming 0 and 1.
- Tools: `canfd-test timing` reports a deferred restart that failed or stays pending. `monitor` with no sample, an id above 29 bits, a short write, a failing `info` query, a `seqsend` length with no DLC and a `tester` run with no frame are refused or reported.
- Tools: `ecu-sim` refuses a zero or non-numeric `-p` and an `-i` above 0x7ff. The supervisor probe fails when `ConnectServerInfo` returns another connection id.
- Suite: a skipped case shows in the summary line and fails the run. `--allow-skip CASE` names a case that may skip, and `scripts/test-suite.sh` uses it for `two_nodes` when no peer is given. A case named twice runs and counts once. The summary line of a full run is unchanged.


### Changed

- libcan's thread no longer sleeps. The TX wait before a `SET_TIMING` restart and after bus-off is a state machine continued by interrupt pulses, devctls, writes and a one-shot CLOCK_MONOTONIC timer. The timer's pulse goes to a helper process the driver forks at start (`src/ctucanfd_timer.c`), which sends the new `CTUCAN_DEVCTL_TIMER` to `/dev/canU/rx0`; a helper thread cannot, since a process cannot open the paths its own libcan serves (`docs/DESIGN.md`, section 5).
- `SET_TIMING` with frames in the TXT buffers replies EOK at once and restarts when the wait ends. `GET_XSTATS` state bit 11 shows a pending restart, and bit 10 shows a TX wait. `canfd-test timing` waits for the deferred restart and prints its result; `canfd-test xstats` prints `tx_wait` and `restart_pending`.
- Bus-off keeps its policy. TX is held, frames in TXT buffers are aborted and given up after 20 ms, and the hold ends only when FAULT_STATE reads error active. With `-a`, recovery is requested when the abort phase ends.
- A controller restart (`SET_TIMING`) reads the RX FIFO out and queues its frames for the readers before the reset. Before, the reset discarded them and counted them in `rx_flushed`.
- SIGTERM shutdown runs the same TX wait to its end. It does so after `can_resmgr_start()` returns, so this is the only place the driver sleeps, for at most 40 ms.

### Added

- `tests/host/test_txw_orders.c`: every sequence of 4 TX wait events from 15 start states (311,040 sequences), with invariants checked after each event (`docs/TESTS.md`).
- `tests/host/test_no_sleep.c`: counts `nanosleep()` calls inside libcan callbacks during a `SET_TIMING` restart and bus-off with stuck frames. It builds against older drivers too (`make -C tests/host no_sleep_base BASE=...`); main 30fff05 fails it.
- `tests/host/test_rx_restart.c`: frames in the RX FIFO at a `SET_TIMING` restart, plain or deferred behind stuck frames, must reach the readers. It builds against older drivers too (`make -C tests/host rx_restart_base BASE=...`); main 30fff05 and 0110e39 fail it.
- `scripts/tests/signature.py` `rx_loss`: a `SET_TIMING` restart with 4 stuck frames under 1000 frames/s of sequenced host frames, against a plain restart on the same load. Main loses 21 or 22 frames to it in 5 of 5 rounds (`docs/TESTS.md`).
- `scripts/tests/signature.py` and `tools/ctucan-latprobe.c`: `freeze_free` (the 20 ms abort bound with no traffic, and no request waits over 5 ms meanwhile) and `kill_storm` (60 SIGKILLs under 1 ms telemetry and host load). Main fails `freeze_free` because it waits inside the request; this tree passes both (`docs/TESTS.md`).
- Slow TX diagnostics: TXT buffers that take more than 3 ms to retire are counted and logged. The log is rate-limited to one line per 100 ms.
- ecu-sim prints up to 32 overrun and EAGAIN lines per run.
- `restart_under_load` adds gap timing against the kills, slow-TX lines and ecu-sim lines to its failure message. Its checks are unchanged.
- Host tests (14 programs in `tests/host`, run with `make -C tests/host run`): `test_canfd_tool` and `test_ecu_sim` are new, and `test_supervisor` and `test_driver` have new cases. The TX wrap test now picks the next TX buffer by the programmed priority. New host cases:
  - `test_tx_wait`: the TX wait state machine, on a virtual clock with a fake timer.
  - `body_wrap`: a TX wait deadline and 32-bit timestamps across the 2^32 us wrap.
  - `body_slow_tx`: the slow-TX log line, its rate limit and its count.

### Documented

- Known limit: a restart inside an interrupt pulse can leave RX readers blocked when more reader returns are due than the report budget (`docs/DESIGN.md`, section 11).
- The 32-bit frame timestamp wraps about every 71.6 minutes. Interval math inside the driver is 64-bit.
