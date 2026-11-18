# Changelog

This file starts with the changes after commit 30fff05; earlier history is in `git log`.

## Unreleased

### Changed

- libcan's thread no longer sleeps. The TX wait before a `SET_TIMING` restart and after bus-off is a state machine continued by interrupt pulses, devctls, writes and a one-shot CLOCK_MONOTONIC timer. The timer's pulse goes to a helper process the driver forks at start (`src/ctucanfd_timer.c`), which sends the new `CTUCAN_DEVCTL_TIMER` to `/dev/canU/rx0`; a helper thread cannot, since a process cannot open the paths its own libcan serves (`docs/DESIGN.md`, section 5).
- `SET_TIMING` with frames in the TXT buffers replies EOK at once and restarts when the wait ends. `GET_XSTATS` state bit 11 shows a pending restart, and bit 10 shows a TX wait. `canfd-test timing` waits for the deferred restart and prints its result; `canfd-test xstats` prints `tx_wait` and `restart_pending`.
- Bus-off keeps its policy. TX is held, frames in TXT buffers are aborted and given up after 20 ms, and the hold ends only when FAULT_STATE reads error active. With `-a`, recovery is requested when the abort phase ends.
- SIGTERM shutdown runs the same TX wait to its end. It does so after `can_resmgr_start()` returns, so this is the only place the driver sleeps, for at most 40 ms.

### Added

- `tests/host/test_txw_orders.c`: every sequence of 4 TX wait events from 15 start states (311,040 sequences), with invariants checked after each event (`docs/TESTS.md`).
- `scripts/tests/signature.py` and `tools/ctucan-latprobe.c`: `freeze_free` (the 20 ms abort bound with no traffic, and no request waits over 5 ms meanwhile) and `kill_storm` (60 SIGKILLs under 1 ms telemetry and host load). Main fails `freeze_free` with a 20 ms wait; this tree passes both (`docs/TESTS.md`).
- Slow TX diagnostics: TXT buffers that take more than 3 ms to retire are counted and logged. The log is rate-limited to one line per 100 ms.
- ecu-sim prints up to 32 overrun and EAGAIN lines per run.
- `restart_under_load` adds gap timing against the kills, slow-TX lines and ecu-sim lines to its failure message. Its checks are unchanged.
- Host tests:
  - `test_tx_wait`: the TX wait state machine, on a virtual clock with a fake timer.
  - `body_wrap`: a TX wait deadline and 32-bit timestamps across the 2^32 us wrap.
  - `body_slow_tx`: the slow-TX log line, its rate limit and its count.

### Documented

- The 32-bit frame timestamp wraps about every 71.6 minutes. Interval math inside the driver is 64-bit.
