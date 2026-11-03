# ctucanfd: design

`dev-can-ctucanfd` is a native QNX 8.0 CAN driver, built on the CAN DDK (`libcan.a`), for the CTU CAN FD IP core on PCI. QEMU emulates that board as `ctucan_pci`, so CAN and CAN FD software for QNX can run in QEMU with no hardware and talk to host tools over SocketCAN. The only other QEMU CAN path for QNX that we found, dev-can-linux, runs Linux SJA1000 drivers (kvaser_pci), which cannot carry CAN FD.

Every number in this file comes from a command run on the build box (Ubuntu 22.04, QEMU 6.2.0) against a QNX 8.0 x86_64 target built with mkqnximage. Source reading of QEMU's model is marked as such.

## 1. The hardware as QEMU 6.2 models it

QEMU side (`qemu-system-x86_64 -device ctucan_pci,help`): two link properties, `canbus0` and `canbus1`, one per core. The board refuses to realise unless both are set. The bus object id must not be the bare string `canbus0`: QEMU then fails with `Path 'canbus0' does not uniquely identify an object`, so the scripts use `canbus0-bus`.

`info pci` in the QEMU monitor:

```
Class 3081: PCI device 1760:ff00   (class 0x0c09, CAN bus controller)
IRQ 10, pin A
BAR0: 32 bit memory at 0xfeba0000 [0xfeba7fff]
BAR1: 32 bit memory at 0xfeb90000 [0xfeb9ffff]
```

QNX side, `pci-tool -vvv -d 0:5:0`: `vid/did: 1760/ff00`, `class/subclass/reg: 0c/09/00 CANbus Serial Bus Controller`, `cmd/status registers: 103/0`, two 32-bit MEM address spaces, both `ENABLED`.

`probe/ctucan_probe.c` attaches through the PCI server, maps both BARs and reads:

```
irq: 10 (of 1)
BAR0+0x0000 bridge id: 0xc0000002 (cores=2)
core0 DEVICE_ID=0xcafd VERSION=2.2 (raw 0x0202cafd) OK
core0 MODE=0x00000010 STATUS=0x00000084 RX_MEM_INFO=0x08000800 TX_STATUS=0x00008888 YOLO=0xdeadbeef
core1 DEVICE_ID=0xcafd VERSION=2.2 (raw 0x0202cafd) OK
```

So BAR0 holds the bridge id (core count in bits 3:0) and BAR1 holds the cores, one every 0x4000 bytes. The core identification registers are in BAR1, not BAR0. After reset each core reports FDE set (MODE 0x10), idle with TX buffers free (STATUS 0x84), an 8 KiB RX FIFO that is empty (0x800 words, 0x800 free) and four empty TXT buffers (0x8888).

What the QEMU 6.2 model leaves out or does differently (from reading `hw/net/can/ctucan_core.c` and `ctucan_pci.c`, confirmed where a register read could show it):

| Behaviour | Consequence for the driver |
|---|---|
| No bit timing: BTR and BTR_FD read back `0x00000000` after the driver wrote them | Timing is computed and written for silicon; QEMU ignores it |
| No error counters, fault confinement, bus errors or bus-off | Error paths cannot be exercised in QEMU (section 8) |
| A TXT buffer goes ready to TX OK inside the TX_COMMAND write | TX completion interrupt is pending before the write returns |
| A ready buffer with priority 0 is never sent | Driver sets TX_PRIORITY to 0x4321 |
| Writes narrower than 32 bits are not shifted into place | All register access is 32-bit, 16-bit fields go through their word |
| Both cores drive one `qemu_irq`; each core raises or lowers it alone | Use one core per board for interrupt-driven nodes; a second node gets a second board |
| No bus timing: frames from the host land in the FIFO as fast as the host sends | Unpaced bursts overflow the FIFO after 102 frames of 64 bytes |

## 2. Driver architecture on the CAN DDK

### 2.1 Model

The CAN DDK (QNX docs: `com.qnx.doc.ddk/topic/can/`) defines a driver as a standalone executable that statically links `libcan.a`. libcan creates one resource manager path per mailbox, `/dev/canU/rxN` and `/dev/canU/txN`, on a single-threaded dispatch loop. The driver supplies callbacks in `can_drvr_funcs_t`:

- `transmit(CANDEV *)`: libcan has queued a frame on a TX mailbox.
- `devctl(CANDEV *, io_devctl_t *)`: called first for every devctl. Return a byte count for data, `EOK`, or -1 with errno; errno `ENOTSUP` hands the command back to libcan's default handling (that is how `CAN_DEVCTL_WRITE_CANMSG_EXT` and `CAN_DEVCTL_READ_CANMSG_EXT` reach the queues).
- `event_handler(void *hdl, int code)`: called for the interrupt pulse. It returns the `CANDEV` that has news (libcan then replies to blocked readers or refills a TX mailbox) and is called again until it returns NULL.

Queues are libcan's: each `CANDEV` has a `free_queue` and a `msg_queue` of `canmsg_t`, moved with `canmsg_dequeue_element()` and `canmsg_queue_element()`. It links for x86_64 with `-lcan -lpci -lsecpol -lslog2`.

Device numbering follows QNX's convention: RX mailboxes first, TX numbered after them. With the defaults that is `/dev/can1/rx0` and `/dev/can1/tx1` for core 0.

### 2.2 Source layout

| File | Role |
|---|---|
| `src/ctucanfd_regs.h` | Register map and bit fields, written from the datasheet; our own definitions |
| `src/ctucanfd_hw.[ch]` | One core: identify, reset, start/stop, write a TXT buffer, pop an RX frame, DLC tables, bit timing. No libcan or OS state |
| `src/dev-can-ctucanfd.c` | Options, PCI attach, libcan glue, interrupt service, devctls, shutdown |
| `tools/canfd-test.c` | Client: send/recv with the extended-message devctls (full 64-byte payload), info, pulse |
| `probe/ctucan_probe.c` | Identification probe |
| `scripts/qemu-can.sh`, `host-vcan.sh`, `gate-test.sh` | QEMU launch with CAN, host vcan0, end-to-end test |

### 2.3 Startup

1. `ThreadCtl(_NTO_TCTL_IO, 0)`. Without it the probe got SIGSEGV during PCI access.
2. `pci_device_find(idx, 0x1760, 0xff00)`, `pci_device_attach(..., pci_attachFlags_EXCLUSIVE_OWNER)`, `pci_device_read_ba()`, `pci_device_read_irq()` (gives 10), `mmap_device_memory(PROT_NOCACHE)` of the 16 KiB core window in BAR1.
3. Check DEVICE_ID 0xCAFD, then soft reset (MODE.RST) so a restarted driver never inherits TX buffers or interrupt state from a dead one.
4. Bit timing from the core clock (default 100 MHz), 500 kbit/s nominal at 87.5 % sample point and 2 Mbit/s data at 75 %: the driver prints `brp 4 tq 50` and `brp 1 tq 50`.
5. `can_resmgr_init()`, `can_resmgr_init_device()` per mailbox with `cflags |= CANDEV_CFLAG_CANFD`, `can_resmgr_create_device()`, `can_resmgr_attach_intr()`, then enable the core (SETTINGS.ENA, MODE.FDE) and its interrupts, then `can_resmgr_start()`.

## 3. Interrupts on QNX 8

The QNX 8.0 migration guide (`com.qnx.doc.qnxsdp.migration/topic/kernel.html`): "Interrupt Service Routines (ISRs) are no longer supported. Instead, QNX has a new mechanism for attaching threads to interrupts: Interrupt Service Threads (ISTs). If you are using events, there is currently no need for you to update your code". `InterruptAttach()` and `InterruptAttachArray()` are listed as removed; `InterruptAttachThread()` is new.

### 3.1 Current path: the libcan thread is the IST

`can_resmgr_attach_intr()` ties the interrupt to a pulse; libcan's pulse handler calls `event_handler()` (CAN DDK docs), which does all device work in thread context:

1. Read INT_STAT masked by the enabled set. Zero means the shared line was someone else's.
2. Write the same bits back to INT_STAT first, so events raised while we work stay pending.
3. RBNEI or DOI: pop frames from RX_DATA until RX_STATUS.RXFRC is zero (budget 512), deliver them, clear an overrun with COMMAND.CDO, clear RBNEI again once the FIFO is empty.
4. TXBHCI: read TX_STATUS, retire the buffer (TX OK, error or abort), mark TX mailboxes for libcan, start the next queued frame.
5. EWLI, FCSI, BEI: error accounting (section 8).
6. Repeat while INT_STAT shows enabled events (up to 16 passes), then return each mailbox with news, one per call.
7. `InterruptUnmask()`, then read INT_STAT once more (section 3.2).

No code runs in kernel context, and all callbacks share one thread, so the driver state has no locks.

### 3.2 Lost interrupt: found and fixed

The first version unmasked and returned. A 1 ms paced burst stalled it: the driver had 327 of 900 frames while the controller held a full FIFO and a pending, enabled interrupt that never fired (`canctl -d` register dump: `INT_STAT 0x00000409`, `INT_ENA 0x00000c5c`, `RX_STATUS 0x00000660` = 102 frames, `RX_FR_CTR 0x000001ad`). A frame that arrives between the last INT_STAT read and the unmask raises the line while it is masked; on this route no second interrupt comes for a line that is already high. The handler now re-reads INT_STAT after `InterruptUnmask()`; if anything is pending it masks again and services it. Anything later raises a fresh interrupt. Result: `400/400` frames in a 400-frame burst at the same 1 ms pacing, with the re-check catching 6 late events (`late events 6` in the debug dump).

### 3.3 Explicit IST (next step)

QNX recommends explicit threads "to increase efficiency and control". The plan is an optional IST created with `InterruptAttachThread()` at a priority above client threads. It drains the RX FIFO into a single-producer, single-consumer ring and acknowledges the core, so RX latency no longer depends on how busy the resmgr thread is with client devctls. It then wakes the libcan thread with a pulse. Half of this is checked: a pulse sent with `MsgSendPulse()` on a file descriptor opened to our own `/dev/can1/rx0`, with the interrupt pulse code, reached `event_handler()` (`canfd-test pulse /dev/can1/rx0 0` gave `events 1`), while a pulse with an unregistered code (1) was dropped by libcan without calling the driver. Still to check: that `InterruptDetach()` on the iid from `can_resmgr_attach_intr()` leaves libcan's pulse handler attached, so the IST alone owns the line. The same self-pulse gives a periodic watchdog that re-runs the service loop. We will only switch if measurements (RX latency under client load, overrun at line rate on hardware) show a gain.

## 4. RX path

The RX FIFO holds frames as a frame format word (DLC, RTR, IDE, FDF, BRS, ESI, RWCNT), an identifier word, two timestamp words and up to 16 data words. The driver always pops RWCNT words so the FIFO stays aligned.

The identifier word uses the same layout as a libcan MID (11-bit id in bits 28:18, 29-bit id as is), so IDs pass through unchanged and `ext.is_extended_mid` comes from IDE.

Each RX mailbox has a MID and a filter; a frame goes to every mailbox where `((mid ^ mbox.mid) & mbox.mfilter) == 0`. The default filter 0 accepts everything. When a mailbox queue is full the oldest frame is dropped (`sw_receive_q_full`). A FIFO overrun is counted in `hw_receive_overflows` and cleared.

QEMU leaves the timestamp words at zero, so `ext.timestamp` is CLOCK_MONOTONIC in microseconds, adjustable with `CAN_DEVCTL_SET_TIMESTAMP`. On silicon the core timestamp can replace it.

Planned: map `CAN_DEVCTL_SET_MFILTER` onto the core's hardware filters (FILTER_A/B/C and the range filter) when one RX mailbox is configured.

## 5. TX path

libcan queues the frame and calls `transmit()`. The driver takes the next frame round robin across TX mailboxes, writes TXT buffer 1 (frame format, identifier, timestamp 0 = send now, data words) and issues TX_COMMAND.TXCR. One buffer in flight keeps the queue order. On TXBHCI it retires the buffer with TXCE, counts the result and starts the next frame. A plain `write()` has `mid == CAN_MSG_MID_UNKNOWN`, so the mailbox MID applies (default: standard id 0x100 + n).

Planned: use all four TXT buffers with rotating priorities so frames go back to back on silicon while keeping FIFO order.

## 6. CAN FD

The framework carries CAN FD: `CAN_MSG.dat[CAN_MSG_DATA_MAX]` with `CAN_MSG_DATA_MAX_CANFD 64`, and `CANDEV.cflags` bit 31 (`CANDEV_CFLAG_CANFD`). `CAN_MSG` has no per-frame FDF, BRS or ESI flags, so the frame format is a driver policy:

- CAN FD is on by default (`-C` for classic only). Frames longer than 8 bytes go out as CAN FD with BRS (`-S` turns BRS off; `-A` sends every frame as CAN FD).
- Lengths are rounded up to the next CAN FD size (12, 16, 20, 24, 32, 48, 64) and zero padded: a 10-byte frame from QNX reached the host as `124  [12]  00 01 02 03 04 05 06 07 08 09 00 00`, a 33-byte frame as `125  [48]` with 15 zero bytes.
- The driver rejects `CAN_DEVCTL_WRITE_CANMSG_EXT` above 64 bytes (8 in classic mode), because libcan itself does not check the length.
- RX accepts classic and CAN FD frames alike. Because the API cannot report FDF/BRS/ESI to clients, an opt-in use of `CAN_MSG.rsv[0]` would need QNX's agreement first; it is not implemented.

QEMU's host bridge enables CAN FD only when the host interface MTU is 72 (`CANFD_MTU`), so `host-vcan.sh` sets `mtu 72`.

## 7. Devctl and canctl

Handled by the driver: `GET/SET_MID`, `GET/SET_MFILTER`, `GET/SET_TIMESTAMP`, `GET_INFO`, `GET_STATS`, `ERROR` (FAULT_STATE word, REC/TEC, ERR_NORM/ERR_FD, STATUS), `SET_TIMING` (nominal, restarts the controller), `DEBUG_INFO` (register dump to slog2). The stock `canctl` works against it: `canctl -u1,tx1 -i` printed queue sizes, `Bitrate: 500000 Baud`, prescaler 4, TS1 43, TS2 6. canctl's `-w` takes a raw MID, so a standard id must be shifted left by 18 (`-w 0x123,0,hello` went out as id 0x000).

## 8. Errors and bus-off

Enabled interrupts: EWLI, FCSI, BEI, DOI (plus RBNEI and TXBHCI).

- BEI: ERR_CAPT error type goes to the matching stats counter (bit, CRC, form, ACK, stuff).
- FCSI and EWLI: FAULT_STATE (ERA, ERP, BOF) is compared with the last state; error passive and bus-off increment their counters, bus-off logs TEC and REC to slog2.
- Bus-off recovery: with `-a` the driver issues COMMAND.ERCRST, so the core rejoins after 128 times 11 recessive bits. Without it the controller stays off until `CAN_DEVCTL_SET_TIMING` or a driver restart, so a system integrator can choose.
- A TXT buffer in the error state (retransmit limit) is retired and counted as a frame error.

None of this can be triggered in QEMU 6.2. Test plan: a fault-injection build of the hardware layer that replays FCSI/BEI sequences into the service loop, and a check on real CTU CAN FD silicon if we can borrow a board.

## 9. Restart and recovery

SIGTERM makes `can_resmgr_start()` return; the driver disables the core and its interrupts, detaches, destroys the devices and calls `can_resmgr_fini()`. After `slay -s KILL dev-can-ctucanfd` the kernel removed `/dev/can1`; a fresh start reset the core and the next 64-byte frame reached vcan0. From the shell, `time` gave `0.01s real` for the driver start until the shell got control back, and `ls /dev/can1` right after showed `rx0` and `tx1`.

Planned: a supervisor (a respawn loop in the image, or HAM if it is in the target image) and clients that reopen on error.

## 10. Testing in QEMU

```
scripts/host-vcan.sh vcan0                 # vcan module, mtu 72, link up
qtarget tc create; qtarget tc stop         # any mkqnximage target
scripts/qemu-can.sh ~/targets/tc vcan0     # same QEMU args + CAN bus + ctucan_pci + bridge
source ~/qnx800/qnxsdp-env.sh && make
scripts/gate-test.sh tc vcan0
```

`qemu-can.sh` takes the exact `QEMU Command:` line mkqnximage logged, swaps `-nographic` for `-display none -daemonize` (QEMU refuses the pair), adds a monitor socket and appends `-object can-bus,id=canbus0-bus -object can-host-socketcan,id=canhost0,if=vcan0,canbus=canbus0-bus -device ctucan_pci,canbus0=canbus0-bus,canbus1=canbus0-bus`. The pidfile stays `output/qemu.pid`, so `qtarget status|ssh|stop` keep working.

Gate run (`gate-test.sh`, QNX 8.0 x86_64 under KVM):

```
== QNX -> host (vcan0)
  vcan0  TX B -  123  [64]  00 01 02 03 ... 3E 3F
  vcan0  TX - -  7DF   [8]  02 01 05 00 00 00 00 00
== host -> QNX (/dev/can1/rx0)
/dev/can1/rx0  456  [64]  C0 C1 C2 ... FE FF
/dev/can1/rx0  1ABCDEF0  [04]  DE AD BE EF
== paced burst: 400 x 64-byte CAN FD frames, 1 ms apart
stats: tx 2 rx 402 irq 403 (rx 401 tx 2) frame_err 0 hw_overflow 0 swq_full 0 bus_off 0
PASS: 64-byte CAN FD both ways, classic and 29-bit ids, 400/400 burst frames
```

`B` in candump's `-x` column is the bit rate switch flag. An unpaced 500-frame burst (`cangen -g 0`) overflowed the 102-frame FIFO as expected (`hw_overflow 2`), and the next frame after it was still received, so overrun recovery works.

Planned tests: every length 0 to 64 both ways (DLC rounding), RTR, standard and extended ids, several mailboxes with filters, queue-full behaviour, restart under load, two QNX nodes on one QEMU bus (two boards), and aarch64le builds.

## 11. Judge demo storyline (under 3 minutes)

1. Problem (20 s): CAN FD software for QNX normally needs a CAN FD board on the desk. QEMU already emulates one, but QNX had no driver for it.
2. Architecture (30 s): one diagram. QEMU ctucan_pci, PCI server, dev-can-ctucanfd on libcan, `/dev/can1/*`, the QEMU bus bridged to host vcan0.
3. Live ECU (60 s): a simulated battery management ECU on QNX (to be written: `ecu-sim`) sends a 64-byte CAN FD status frame every 10 ms and answers a diagnostic request. The host runs `candump -t d vcan0` to show the 10 ms period, and `cansend` plays the tester.
4. Fault and recovery (40 s): `slay -s KILL dev-can-ctucanfd` mid-stream. The kernel, the ECU process and the shell survive; the supervisor restarts the driver, the ECU reopens `/dev/can1/tx1` and candump shows the gap, then the period resumes. On a microkernel the CAN driver is just a process that can crash and come back.
5. Proof (30 s): `gate-test.sh` PASS, the burst statistics, `canctl -i`, the repository.

## 12. Known limits and next steps

- One TXT buffer in flight; four with rotating priorities next.
- Software acceptance filtering only; hardware filters next.
- Error and bus-off paths are written but cannot be exercised in QEMU 6.2.
- Explicit IST (section 3.3) and a watchdog pulse are designed, half validated.
- To write: `ecu-sim`, supervisor, a broader test suite, README with build and run steps, the demo video.
