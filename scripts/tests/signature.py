#!/usr/bin/env python3
# Copyright 2026 The ctucanfd authors
# SPDX-License-Identifier: Apache-2.0
"""Signature tests: properties that need the driver's design, not only correct code.

  freeze_free  Bus-off and a SET_TIMING restart with frames that do not finish, while a second
               client asks the driver for its statistics back to back. Pass needs both: the
               stuck frames are given up at the 20 ms bound even with no traffic at all, and no
               request waits more than 5 ms meanwhile. A driver that waits inside the request
               keeps the bound but freezes every client for 20 to 40 ms; one that never waits
               but has no wake-up of its own misses the bound when nothing else happens.
  rx_loss      A SET_TIMING restart with 4 frames that do not finish, while the host sends 64-byte
               frames with sequence numbers at a rate the bus could carry (--rate, 1000 frames/s by
               default) and a reader on the target checks them. A plain restart with nothing in
               flight, on the same load, runs first as the control. Pass: no RX FIFO overflow, no
               frame out of order or bad, and no more frames lost than the control lost. The line
               also says whether no frame was lost at all. A driver that waits inside the request
               leaves the RX FIFO alone meanwhile, and the restart's reset then empties it.
  kill_storm   60 SIGKILLs of the driver under a 1 ms telemetry stream and 500 frames/s of host
               load, with ctucan-supervisor restarting it: half by name (driver and helper), half
               the driver alone. Every kill is one short gap, the stream keeps its order and CRC,
               and the driver leaves no processes behind.

Runs against any tree that builds ctucan-latprobe, main included: the tests use only ctutest.py's
helpers, the standard CAN devctls and the fault-injection scenarios main has.

  scripts/tests/signature.py --target a --if vcan0 [--rate N] [freeze_free] [rx_loss] [kill_storm]
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctutest as c  # noqa: E402

LAT = "/tmp/ctucan-latprobe"
FREEZE_US = 5000        # the longest a request may wait while a fault is handled


def put_latprobe(t):
    t.put(os.path.join(c.ROOT, "build", "x86_64", "ctucan-latprobe"), LAT)


def latprobe(t, log):
    m = re.search(r"latprobe: (\d+) devctls, longest (\d+) us at (\d+) ms, over 1 ms (\d+), over 5 ms (\d+), "
                  r"over 10 ms (\d+)", t.out(f"cat {log}"))
    c.check(m is not None, f"no latprobe output: {t.out(f'cat {log}').strip()}")
    n, worst, at, o1, o5, o10 = map(int, m.groups())
    return dict(n=n, worst=worst, at=at, o1=o1, o5=o5, o10=o10)


def t_freeze_free(t, bus):
    """Bus-off and restart with stuck frames: 20 ms bound kept with no traffic, and no request waits."""
    # 1. idle baseline: what a request costs here with nothing going on
    t.driver(variant=c.FI, force=True)
    put_latprobe(t)
    t.bg(f"{LAT} {c.RX0} 1000", "/tmp/lat0.log")
    time.sleep(1.4)
    base = latprobe(t, "/tmp/lat0.log")

    # 2. the bound with nothing else running (as fi_busoff_abort_notick): 4 frames held in flight,
    # bus-off with aborts that take 100 ms, one xstats read 300 ms later
    t.driver(variant=c.FI, force=True)
    c.fi(t, 4, 5000, 4)
    t.ssh(f"{c.TEST} seqsend {c.TX1} 2F1 16 4 0 >/dev/null")
    x0 = t.xstats()
    c.fi(t, 3, 2, 1)
    time.sleep(0.3)
    xa = t.xstats()
    bound_ok = x0["inflight"] == 4 and xa["tx_abort_forced"] == 4 and xa["inflight"] == 0

    # 3. the same bus-off while a client asks for statistics back to back
    t.driver(variant=c.FI, force=True)
    put_latprobe(t)
    c.fi(t, 4, 5000, 4)
    t.ssh(f"{c.TEST} seqsend {c.TX1} 2F2 16 4 0 >/dev/null")
    t.bg(f"{LAT} {c.RX0} 1200", "/tmp/lat1.log")
    time.sleep(0.4)
    c.fi(t, 3, 2, 1)
    time.sleep(1.2)
    busoff = latprobe(t, "/tmp/lat1.log")
    xb = t.xstats()

    # 4. SET_TIMING with 4 frames that do not finish, the same client running
    t.driver(variant=c.FI, force=True)
    put_latprobe(t)
    c.fi(t, 4, 5000, 4)
    t.ssh(f"{c.TEST} seqsend {c.TX1} 2F3 16 4 0 >/dev/null")
    x2 = t.xstats()
    t.bg(f"{LAT} {c.RX0} 1200", "/tmp/lat2.log")
    time.sleep(0.4)
    out = t.out(f"{c.TEST} timing {c.RX0} 4 43 6 6")
    time.sleep(1.2)
    restart = latprobe(t, "/tmp/lat2.log")
    x3 = t.xstats()

    def show(p):
        return f"longest {p['worst']} us at {p['at']} ms, {p['o5']} over 5 ms of {p['n']}"
    detail = (f"baseline {show(base)}; bus-off: {show(busoff)}; restart: {show(restart)}; "
              f"bound with no traffic: tx_abort_forced {xa['tx_abort_forced']} inflight {xa['inflight']}")
    c.check(bound_ok, f"20 ms bound missed with no traffic: {detail}")
    c.check(xb["inflight"] == 0 and xb["busoff_hold"] == 1, f"bus-off with the client running: {xb}")
    c.check("No error" in out and x2["inflight"] == 4 and x3["tx_aborted"] - x2["tx_aborted"] == 4
            and x3["restarts"] == 1, f"restart: {out.strip()} {x3}")
    c.check(busoff["worst"] < FREEZE_US and restart["worst"] < FREEZE_US,
            f"a request waited over {FREEZE_US} us while the fault was handled: {detail}")
    return (f"4 stuck frames given up at the 20 ms bound with no traffic (tx_abort_forced 4); "
            f"with a client asking back to back, bus-off {show(busoff)}, restart {show(restart)}; "
            f"idle {show(base)}")


RATE = 1000            # rx_loss host load, frames/s. A 64-byte CAN FD frame takes about 360 us at the
                       # driver's default 500 kbit/s and 2 Mbit/s, so a real bus carries about 2700/s.
                       # On the QEMU target every driver takes in about 2000/s at most, and the reader
                       # (one devctl per frame) falls behind at 2000/s, so the test stays well below.


def flood(bus, cid, n, rate):
    """n frames with canfd-test's sequenced payload, paced to `rate` frames/s on the host clock."""
    t0 = time.monotonic()
    for q in range(n):
        ahead = t0 + q / rate - time.monotonic()
        if ahead > 0.0005:
            time.sleep(ahead)
        bus.send(cid, c.pattern(q, 64))


def restart_under_load(t, bus, rate, secs, stuck):
    """One SET_TIMING restart halfway through `secs` of sequenced host frames at `rate`, read by
    canfd-test seqrecv on the target. With `stuck`, 4 frames that never finish are in the TXT buffers
    first, so the restart waits for the TX wait. Returns the counts."""
    n = int(rate * secs)
    t.driver("-q 1024", variant=c.FI, force=True)
    if stuck:
        c.fi(t, 4, 5000, 4)
        t.ssh(f"{c.TEST} seqsend {c.TX1} 2F4 16 4 0 >/dev/null")
    x0, s0 = t.xstats(), t.stats()
    t.bg(f"{c.TEST} seqrecv {c.RX0} {n} 60000", "/tmp/rxl.log")
    time.sleep(0.3)
    bus.drain()
    th = threading.Thread(target=flood, args=(bus, 0x2A9, n, rate))
    th.start()
    time.sleep(secs / 2)
    out = t.out(f"{c.TEST} timing {c.RX0} 4 43 6 6")
    th.join()
    # The reader ends after n frames. Frames that were lost are made up by more frames after the
    # last one (sequence n, n+1, ...), one every 5 ms, so it ends and counts each loss as a gap.
    time.sleep(0.5)
    log, extra = "", 0
    while extra < 4000:
        log = t.out("cat /tmp/rxl.log")
        if "seqrecv:" in log:
            break
        for _ in range(20):
            bus.send(0x2A9, c.pattern(n + extra, 64))
            extra += 1
            time.sleep(0.005)
    x1, s1 = t.xstats(), t.stats()
    m = re.search(r"seqrecv: got (\d+) of (\d+), bad (\d+), missing (\d+), out_of_order (\d+)", log)
    c.check(m is not None, f"no seqrecv result: {log.strip()}")
    got, _, bad, missing, ooo = map(int, m.groups())
    r = {"n": n, "extra": extra, "got": got, "lost": missing, "ooo": ooo, "bad": bad,
         "ovf": s1["hw_overflow"] - s0["hw_overflow"], "swq": s1["swq_full"] - s0["swq_full"],
         "flushed": x1["rx_flushed"] - x0["rx_flushed"]}
    r["text"] = (f"{n} sent and {extra} after them: read {got}, lost {missing}, out of order {ooo}, bad {bad}; "
                 f"FIFO overflows {r['ovf']}, flushed by the reset {r['flushed']}, mailbox queue full {r['swq']}")
    ok = "No error" in out and x1["restarts"] - x0["restarts"] == 1
    if stuck:
        ok = ok and x0["inflight"] == 4 and x1["tx_aborted"] - x0["tx_aborted"] == 4
    else:
        ok = ok and x0["inflight"] == 0
    c.check(ok, f"restart ({'4 stuck frames' if stuck else 'nothing in flight'}): {out.strip()} {x1}; {r['text']}")
    return r


def t_rx_loss(t, bus, rate=None, secs=2.0):
    """SET_TIMING restart with stuck frames under host RX load loses no more than a plain restart."""
    rate = rate or RATE
    ctl = restart_under_load(t, bus, rate, secs, stuck=False)
    t.stop_all()
    run = restart_under_load(t, bus, rate, secs, stuck=True)
    strict = run["got"] == run["n"] and run["lost"] == 0 and run["ooo"] == 0 and run["bad"] == 0 and run["ovf"] == 0
    detail = (f"{rate} frames/s; plain restart (control): {ctl['text']}; restart with 4 stuck frames: "
              f"{run['text']}; no frame lost at all: {'yes' if strict else 'no'}")
    c.check(run["ovf"] == 0 and run["ooo"] == 0 and run["bad"] == 0, f"FIFO overflow or bad frames: {detail}")
    c.check(run["lost"] <= ctl["lost"], f"lost {run['lost']} with stuck frames, {ctl['lost']} in the plain "
            f"restart: {detail}")
    return detail


def procs(t):
    """dev-can-ctucanfd processes as (pid, ppid), and how many are DEAD."""
    out = t.out("pidin -p dev-can-ctucanfd family 2>/dev/null")
    rows = [ln.split() for ln in out.splitlines()[1:] if "dev-can-ctucanfd" in ln]
    pp = [(int(r[0]), int(r[4])) for r in rows if len(r) >= 5]
    dead = t.out("pidin -p dev-can-ctucanfd 2>/dev/null | grep -c DEAD").strip()
    return pp, int(dead or 0)


def driver_pid(pp):
    """The driver, not its helper: the process whose parent is not another driver process."""
    pids = {p for p, _ in pp}
    own = [p for p, pp_ in pp if pp_ not in pids]
    return own[0] if len(own) == 1 else None


def t_kill_storm(t, bus, kills=60, every=0.5):
    """SIGKILL x60 under 1 ms telemetry and host load: one short gap per kill, stream intact, nothing leaked."""
    c.deploy_tools(t)
    t.bg("/tmp/ctucan-supervisor -- /tmp/dev-can-ctucanfd", "/tmp/sup.log")
    c.wait_path(t, c.TX1)
    time.sleep(0.5)
    pp0, _ = procs(t)
    t.bg("/tmp/ecu-sim -p 1000 -i 2F8", "/tmp/ecu.log")
    cd = c.Candump(c.bus_if(bus), "2F8:7FF")
    load = subprocess.Popen(["cangen", c.bus_if(bus), "-I", "2A6", "-L", "64", "-f", "-b", "-g", "2", "-D", "i"])
    time.sleep(1.5)
    alone = 0
    for k in range(kills):
        if k % 2 == 0:
            t.ssh("slay -s KILL dev-can-ctucanfd")
        else:
            pid = driver_pid(procs(t)[0])
            c.check(pid is not None, f"kill {k + 1}: no driver to kill: {procs(t)}")
            t.ssh(f"kill -9 {pid}")
            alone += 1
        time.sleep(every)
    time.sleep(1.0)
    load.terminate()
    load.wait()
    frames = cd.stop()
    pp1, dead = procs(t)
    t.ssh("slay -s TERM ecu-sim; sleep 0.3")
    sup = t.out("cat /tmp/sup.log")
    t.stop_all()
    seqs = [c.seq_of(f["data"]) for f in frames]
    bad_crc = sum(1 for f in frames if c.crc8(f["data"][:63]) != f["data"][63])
    holes = [b["t"] - a["t"] for a, b in zip(frames, frames[1:]) if b["t"] - a["t"] > 0.005]
    ready = [float(x) for x in re.findall(r"ready \([^)]*\) ([\d.]+) ms after spawn", sup)]
    c.check(bad_crc == 0 and all(b > a for a, b in zip(seqs, seqs[1:])),
            f"{bad_crc} bad CRC; sequence backwards or repeated")
    c.check(sup.count("died") == kills and len(ready) == kills + 1,
            f"supervisor saw {sup.count('died')} deaths and {len(ready)} starts for {kills} kills")
    c.check(len(holes) == kills, f"{len(holes)} gaps over 5 ms for {kills} kills: "
            f"{', '.join(f'{h * 1000:.1f}' for h in holes[:12])}")
    c.check(len(pp1) == len(pp0) and dead == 0,
            f"processes before {len(pp0)} {pp0}, after {len(pp1)} {pp1}, DEAD {dead}")
    hs = sorted(h * 1000 for h in holes)
    return (f"{kills} kills ({kills - alone} by name, {alone} driver alone) under 1 ms telemetry and host load: "
            f"{len(frames)} frames, order and CRC intact; one gap per kill, median {hs[len(hs) // 2]:.1f} ms, "
            f"max {hs[-1]:.1f} ms; driver ready median {sorted(ready[1:])[len(ready[1:]) // 2]:.1f} ms after "
            f"respawn; {len(pp1)} driver process(es) after, as before, 0 DEAD")


TESTS = {"freeze_free": t_freeze_free, "rx_loss": t_rx_loss, "kill_storm": t_kill_storm}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default="tc")
    ap.add_argument("--if", dest="ifname", default="vcan0")
    ap.add_argument("--rate", type=int, default=RATE, help=f"rx_loss host load, frames/s ({RATE})")
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    TESTS["rx_loss"] = lambda t, bus: t_rx_loss(t, bus, a.rate)
    t, bus = c.Target(a.target), c.Bus(a.ifname)
    failed = 0
    for n in dict.fromkeys(a.tests or list(TESTS)):     # a case named twice runs once
        t0 = time.monotonic()
        try:
            print(f"PASS {n}: {TESTS[n](t, bus)} ({time.monotonic() - t0:.1f} s)", flush=True)
        except c.Fail as e:
            failed += 1
            print(f"FAIL {n}: {e} ({time.monotonic() - t0:.1f} s)", flush=True)
        finally:
            t.stop_all()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
