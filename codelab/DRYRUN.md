# Dry runs of the codelab

Dates: 3 and 4 November 2026 (UTC). Each pass follows `qnx-canfd-qemu.md` from the top in a new workspace on the build box: Ubuntu 22.04.5 LTS, kernel 6.8.0-1066-aws, QEMU 6.2.0 (Debian 1:6.2+dfsg-2ubuntu6.31) with KVM, can-utils 2020.11.0-1, QNX SDP 8.0 in `~/qnx800`.

How a pass runs: `dryrun/gen_pass.py` turns the codelab into a bash script in which every `bash` block runs as written, in order, apart from the substitutions below. A block whose first line is `# terminal 2` plays the reader's second terminal: it runs in its own session and gets Ctrl+C (SIGINT) at the next such block or at the end of the step. An ERR trap logs every command that exits non-zero. `dryrun/run_pass.sh N` removes the targets `tcl2` and `tc2`, generates the script, runs it with a clean environment (`env -i`, so nothing from the SDP is set beforehand) and removes both targets again. The generated scripts and full logs stayed on the box (`~/cldry/pass<N>/`).

Deviations from the text, the same in every pass:

* `~/canfd-lab` was `~/cldry/pass<N>/canfd-lab`, and `NODE1` and `NODE2` were `~/targets/tcl2` and `~/targets/tc2`, the build box's names for targets of this agent. Both targets were created from nothing by the codelab's own commands in every pass.
* `git clone https://github.com/NikhilGogu/qnx-ctucanfd.git` was redirected (`url.<bundle>.insteadOf` in a private `GIT_CONFIG_GLOBAL`) to a git bundle of the local repository, because the commits for this codelab are not pushed yet; the public repository holds v0.1.0.
* `sudo apt install ...` ran as `apt-get -s install ...` (simulated; the box is shared). `sudo adduser` ran as `sudo -n adduser`; the user was already in both groups, so logging out and in was not needed.
* `vcan0` already existed and stayed up: the target `tc` of the main test runs was attached to it, idle, during every pass. The optional `sudo ip link delete vcan0` at the end was not run.

## Pass 1, 3 Nov 21:38 UTC: stopped in step 7

Steps 1 to 6 ran as written. In step 7 the generated script hung at `wait`: it also waited for the job playing terminal 2, which a reader's first terminal would not do. Harness error: `gen_pass.py` now starts terminal 2 with `setsid -f`, so it is not a child of the first terminal.

Text fixed: step 6 said the driver has one thread. `pidin` showed two, the resource manager thread in `RECEIVE` and a thread at priority 254 in `INTR`; the step now shows both and `pidin ... irqs` (vector `0xa`, event `PULSE`).

## Pass 2, 3 Nov 21:50 UTC: stopped in step 10

Steps 1 to 9 as written. In step 10, `scripts/qemu-can.sh` failed on node 2: `Failed to get "write" lock ... Is another process using the image`. The QEMU that `mkqnximage --run=-h` had started was still running: mkqnximage returns before QEMU writes its pid file, so the `mkqnximage --stop` right after it can miss that QEMU. On node 1 the same sequence had worked by timing.

Fixed in the code (commit 0b61e01): `qemu-can.sh` stops every QEMU whose working directory is the target directory before it starts the target with CAN. The codelab no longer calls `mkqnximage --stop` before it.

Text fixed:

1. The draft showed the frames as `RX`; candump `-x` marks every frame `TX`, because on `vcan0` every frame comes from a local program (QEMU's bridge or `cansend`). Outputs and explanation changed.
2. The draft gave the kill test's gap as "less than 10 ms"; this pass gave a largest gap of 20.3 ms: with a 10 ms period, the frame due while no driver runs is skipped. The text was changed to 10 or 20 ms, depending on where in the period the kill lands.
3. Outputs replaced with the real ones of this pass (probe, pidin, candump, statistics, supervisor and ecu-sim logs).

`slay` exits with status 1 after it has killed one process (its documentation: the exit status is the number of processes acted upon). The ERR trap logs those two lines in every pass; a reader sees nothing.

## Pass 3, 3 Nov 22:02:52 to 22:03:56 UTC (64 s)

Every block ran as written and every Check held, for the first time including step 10 (`tester: PASS`) and the clean-up check. Text fixed after it: a claim that `Failed to find address` appears while the target boots (it did not, in this pass), a sentence on why the telemetry range starts before the tester, and the outputs of step 10.

Rendering, checked after pass 3 and again on the final text: `~/.local/bin/claat export` (QNX's claat), the export opened in headless Chromium (Playwright from a venv outside the synced tree) with QNX's `codelab-elements` scripts from `qnx.github.io/codelabs/js/`, at 1280 and 390 pixels wide, every step screenshotted. Found and fixed:

1. The syntax highlighter took the `/*` in `build/x86_64/*` for the start of a C comment and greyed out the following lines. The copy is now `put $NODE1 $(find build/x86_64 -type f)`.
2. Long commands ran past the code box: wrapped (`apt install`, `mkqnximage`, the `ssh` options in `env.sh`, the clean-up check). Long outputs are trimmed with `...`.
3. The tester printed lines of up to 165 characters. Its report is now in lines of at most 85 characters (commit "canfd-test tester: short report lines").
4. The steps added up to 48 minutes; now 44.

On the final text, at 1280 pixels only the driver's start line and the supervisor's log lines, which are kept whole, scroll inside their boxes; at 390 pixels long code lines scroll inside their boxes and the page itself does not. No request failed.

## Pass 4, 4 Nov 00:23:34 to 00:24:31 UTC (57 s)

Every block as written, every Check held. Text fixed: the tester output in step 10 (the new line format, from this pass), and the delay between an echo request and its answer in candump, given as 1.5 to 2 ms, was up to 2.95 ms in this pass; the text now says about 1.7 ms, 1.4 to 3 ms in our runs.

## Pass 5, 4 Nov 00:25:25 to 00:26:26 UTC (61 s)

Every block as written, every Check held. One observation changed the text: `pidin -p ecu-sim` caught thread 1 in `REPLY` (handing a frame to the driver), where the text showed `NANOSLEEP`. Step 8 now says either can show.

## Pass 6, 4 Nov 00:27:15 to 00:28:13 UTC (58 s)

Every block as written, every Check held. One sentence was wrong for this pass: step 7 said the statistics show four interrupts, and they showed `irq 3 (rx 1 tx 2)`: the two frames from the host, 0.56 ms apart, came in with one interrupt. The text now gives both readings.

## Pass 7, 4 Nov 00:29:46 to 00:30:46 UTC (60 s)

Every block as written, every Check held. Afterwards a list of the QEMU processes on the box showed that a QEMU started by `qemu-can.sh` is daemonised and works in `/`, so the clean-up check, which looked for QEMU by working directory, could not have found a QEMU of this codelab: it held in passes 3 to 7 without testing anything. No QEMU of the passes was left on the box. Fixed: the check now looks for the target directory in QEMU's command line, where `qemu-can.sh` puts the monitor socket's full path; and `qemu-can.sh` now also stops the QEMU in the target's pid file (commit "qemu-can.sh also stops the QEMU in the target's pid file"; a second run on tc2 then printed `stopping QEMU pid 1064146` and started a new one).

## Pass 8, 4 Nov 00:38:45 to 00:39:49 UTC (64 s)

Every block as written, every Check held but the last: the new check printed both QEMUs, 30 ms after `mkqnximage --stop` had returned. The stop does not wait for QEMU to exit. The clean-up now waits 3 seconds before it removes the workspace, and says why.

## Pass 9, 4 Nov 00:40:52 to 00:41:57 UTC (65 s): the final text

| Step | Time | Result |
|---|---|---|
| 2 Before you begin | 0.5 s | all packages already the newest version (simulated); user in `kvm` and `libvirt`; `virsh` lists `default` as `active`; `check-net` printed nothing; `ctucan_pci` lists `canbus0` and `canbus1` |
| 3 Get the code | 0.8 s | clone and `make`; the six programs in `build/x86_64` |
| 4 Virtual CAN bus | 0.03 s | `vcan0` with `mtu 72`, `UP` |
| 5 Start a target | 19.5 s | image built; `qemu-can.sh` stopped mkqnximage's QEMU and started the target with the board; `uname` QNX 8.0.0; probe: `DEVICE_ID=0xcafd VERSION=2.2 OK` and `FILTER_STATUS=0x0000 filters present: none` on both cores |
| 6 Start the driver | 0.6 s | both start lines, and the command returned; `rx0`, `tx1`; two threads (`10r RECEIVE`, `254i INTR`); `irqs` shows vector `0xa` and `PULSE` |
| 7 First frames | 2.1 s | terminal 2: `123 [64]` with `B`, `321 [8]`, then the host's `456 [16]` with `B` and `1ABCDEF0 [4]`; the reader on QNX printed both host frames byte for byte; `stats: tx 2 rx 2 irq 4 (rx 2 tx 2)` |
| 8 ECU | 0.9 s | ecu-sim threads at `30f`, both caught in `REPLY`; five telemetry frames 9.9 to 10.1 ms apart; `7E8 03 41 5B CC` 1.041 ms after `7DF` |
| 9 Kill and restart | 5.9 s | supervised driver ready 5.6 ms after spawn; after `slay -s KILL`: 414 frames, largest gap 19.8 ms; `died 3.298 s after spawn`, next driver ready 6.6 ms after spawn |
| 10 Second node | 31.3 s | node 2 built, started, driver up; tester `PASS`: 1095 telemetry frames, seq 2644..3738, missing 0, out of order 0, bad CRC 0; period at node 2's driver mean 10.000 ms (9.643 to 10.370); echo 1000 sent, 1000 answered, round trip min 2.855 ms, p50 3.265, p99 3.705, max 3.892; diagnostics 10 of 10, 2.876 to 3.609 ms. Terminal 2 got 2000 frames, all `TX B` and `[64]`; each answer followed its request by 1.454 to 2.320 ms (p50 1.671) |
| 11 Clean up | 3.0 s | both targets stopped, workspace removed; the check printed nothing |

Verdict: every command ran as written, every Check held, and this pass needed no change to the text. All times are QEMU timing on the build box.

The durations in the text (44 minutes in all) are our estimate for a reader who types the commands and reads the explanations; the machine time of a pass is about one minute.
