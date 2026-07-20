<!-- SPDX-License-Identifier: CECILL-C -->
# M3 raw measurement captures

Raw console captures, M3 HW validation pass on the two boards physically on the bench:
FRDM-K64F + XMC4800-Relax (both J-Link/OpenSDA, flashed one at a time). Persisted in-repo
(not the /tmp scratchpad) against power-off. Verbatim KickOS output; only the trailing CR
of each serial line is dropped and the J-Link/ESP-ROM preamble trimmed. Companion baseline
is `M1_raw_meas.md` (+ `M1_state.md`); the M1-vs-M3 delta table is at the bottom.

Bench window: 2026-07-20. Repo branch `m3-integration`, kernel/app tree at `5f3574b`
(this doc committed on top). Toolchain per `.session/env.sh` (full-C++ ARM GNU 15.3).
Correction re-run 2026-07-20 (same bench window): the selftest `t_cpu_clock_set` TEST bug (see
Findings #1) was fixed and BOTH boards re-flashed -> 42/42. The XMC/K64F selftest captures below
are the corrected 42/42 runs; the kernel/PI/clock-select code was NOT changed.

Probe / VCOM map resolved LIVE this session (SN drifts across USB swaps -- do not trust literals):
- XMC4800-Relax: J-Link `1366:1024` SN `000591165896` -> `/dev/serial/by-id/usb-SEGGER_J-Link_000591165896-if00`
- FRDM-K64F:     J-Link `1366:1015` SN `000621000000` -> `/dev/serial/by-id/usb-SEGGER_J-Link_000621000000-if00`

Bench conditions vs M1 (fairness):
- **selftest** built UNDER ENFORCEMENT (`-st` + `-DKICKOS_HAVE_MPU=1`) -- the banner reads `mpu enforce`.
  M1 selftest ran with NO MPU (`mpu off`). The M3 test set also grew (42 tests vs M1's 14):
  mutex, endpoint/IPC, cap_index0, console_publish_priv, cpu_clock, mpu_privileged_guard,
  confused_deputy are all M3-new.
- **bench** built to MATCH M1: base preset + `-DKICKOS_BENCH=1`, telemetry OFF, **no MPU**
  (banner `mpu off`) -- so the switch/IRQ cycle deltas isolate the M3 kernel-path change
  (syscall-resolve + endpoint copy-under-lock), not enforcement cost. The
  `wcase-irq` / `wcase-hold` probes are M3-new (no M1 baseline).

--------------------------------------------------------------------------------

## XMC4800-Relax (armv7m Cortex-M4F, 144 MHz)

### selftest -- UNDER ENFORCEMENT
Build: preset `xmc4800-relax-st` + `-DKICKOS_HAVE_MPU=1`, target `selftest`
(`build/xmc4800-relax-st/user/apps/selftest/selftest`).
Flash: `JLINK_SN=000591165896 FLASH_BUILD=.../build/xmc4800-relax-st tools/flash-jlink.sh xmc4800-relax selftest` -> OK (loadfile, r;g).
Result: **42/42 pass, 0 fail** (corrected; source log `.session/logs/xmc4800-selftest-42-20260720-182545.log`).
Note: the earlier 41/42 (`not ok 5 - cpu_clock_set`) was a TEST bug, now fixed -- the test called
the privileged-only clock-select syscall from the privileged root thread, which really RETUNED on
XMC silicon and returned a non-zero landed Hz (the emulators have no retune backend, so it read 0
there). The test now exercises the unprivileged-reject contract from a spawned unprivileged child;
no retune fires. Kernel/clock-select code was unchanged.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   xmc4800-relax
   arch    armv7m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 18:24:41
   app     Jul 20 2026 18:24:41

1..42
# [svc] console_write roundtrip
ok 1 - svc_roundtrip
ok 2 - fifo_order
ok 3 - preempt_on_ready
ok 4 - cpu_clock_hz
ok 5 - cpu_clock_set
ok 6 - rr_interleave
ok 7 - sleep_order
ok 8 - multi_wait
ok 9 - sem_destroy
ok 10 - sem_destroy_quiescent
ok 11 - sem_raii
ok 12 - mutex_basic
ok 13 - mutex_pi_donation
ok 14 - mutex_chain_boost
ok 15 - mutex_owner_died
ok 16 - mutex_deadlock
ok 17 - mutex_close_owned
ok 18 - mutex_multi_held
ok 19 - mutex_unlock_errors
ok 20 - mutex_owner_died_nowaiter
ok 21 - mutex_deleg_refcount
ok 22 - endpoint_rendezvous
ok 23 - endpoint_reject
ok 24 - endpoint_rights
ok 25 - endpoint_epipe
ok 26 - endpoint_dead
ok 27 - endpoint_crossdomain
ok 28 - endpoint_bound
ok 29 - cap_index0
ok 30 - console_publish_priv
ok 31 - irq_thread_ctx
ok 32 - irq_as_event
ok 33 - irq_mask_drop
ok 34 - irq_autorearm
ok 35 - irq_phantom_wake
ok 36 - irq_ownership
ok 37 - irq_spurious
ok 38 - mpu_privileged_guard
ok 39 - caller_stack
ok 40 - domain_share
ok 41 - mmio_grant
# [confdep] unpriv rodata literal reaches the console
ok 42 - confused_deputy
# all tests passed
```

### bench (no MPU, telemetry OFF -- matches M1)
Build: preset `xmc4800-relax` -B `build/xmc4800-bench` + `-DKICKOS_BENCH=1`, target `bench`.
Flash: `JLINK_SN=000591165896 FLASH_BUILD=.../build/xmc4800-bench tools/flash-jlink.sh xmc4800-relax bench` -> OK.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   xmc4800-relax
   arch    armv7m
   mpu     off
   sched   tickless
   build   Jul 20 2026 17:58:50
   app     Jul 20 2026 17:58:50

microbenchmark: context-switch throughput (all arches) + per-switch cost
+ IRQ-entry latency where a cycle counter exists. Reporter woken by the
workload, not a timer. Telemetry OFF for clean numbers.

  throughput: 35560 ctx-sw/s  (28120 ns/sw avg over 40000 switches / 1124 ms)
  wcase-hold: 46359 ns masked / 256B endpoint-copy span
  switch: 79/88/674 cyc  548/611/4680 ns  (min/avg/max, n=40001)
  irq:    139/139/144 cyc  965/965/1000 ns  (min/avg/max, n=100)
  wcase-irq[0B]: 173/173/195 cyc  1201/1201/1354 ns  (inject->entry, n=100)
  wcase-irq[64B]: 1843/1843/1865 cyc  12798/12798/12951 ns  (inject->entry, n=100)
  wcase-irq[256B]: 6835/6835/6857 cyc  47465/47465/47618 ns  (inject->entry, n=100)
  wcase-irq[1024B]: 26803/26803/26825 cyc  186131/186131/186284 ns  (inject->entry, n=100)
```

### clock-retune harness (reused clean capture)
Build: preset `xmc4800-relax` + `-DKICKOS_HAVE_MPU=1 -DKICKOS_CLOCK_RETUNE_TEST=ON`, app `clockretune`
(banner `mpu enforce`). Source log: `.session/logs/xmc-clock-retune.log` (2026-07-20).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   xmc4800-relax
   arch    armv7m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 17:38:17
   app     Jul 20 2026 17:41:40

[clockretune] START privileged retune harness
[clockretune] boot: cpu_clock_hz = 144000000  clock_now t0 = 26305631 ns
[clockretune] spin MAX: 8000000 iters in 667126167 ns
[clockretune] after set(LOW): cpu_clock_hz = 48000000
[clockretune] now around retune: pre=693733993 r0=708901117 r1=708930492 r2=708958825 r3=708999075
[clockretune] seam-crossing delta r0-pre = 15167124 ns
[clockretune] MONOTONIC OK: no backward step, no phantom jump
[clockretune] spin LOW: 8000000 iters in 2002787209 ns
[clockretune] spin ratio LOW/MAX x100 = 300 (expect ~ 300 = hz_MAX/hz_LOW)
[clockretune] sleep_ns(200000000) at LOW measured 200153791 ns
[clockretune] after set(MAX): cpu_clock_hz = 144000000
[clockretune] console still readable after both retunes (this line proves baud)
[clockretune] clock retune test done
```

### console handover (userspace console driver end-to-end -- reused clean capture)
Source log: `.session/logs/xmc-consoledemo.log` (2026-07-20). App `consoledemo`, banner `mpu enforce`.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   xmc4800-relax
   arch    armv7m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 15:25:27
   app     Jul 20 2026 15:25:27

[xmcuart] driver up (polled TX)
[worker] line 0 via the userspace console driver
[worker] line 1 via the userspace console driver
[worker] line 2 via the userspace console driver
[worker] line 3 via the userspace console driver
[worker] line 4 via the userspace console driver
[worker] done
```

--------------------------------------------------------------------------------

## FRDM-K64F (armv7m Cortex-M4F, 120 MHz)

### selftest -- UNDER ENFORCEMENT
Build: preset `frdmk64f-st` + `-DKICKOS_HAVE_MPU=1`, target `selftest`
(`build/frdmk64f-st/user/apps/selftest/selftest`).
Flash: `JLINK_SN=000621000000 FLASH_BUILD=.../build/frdmk64f-st tools/flash-jlink.sh frdmk64f selftest` -> OK (loadfile, r;g).
Result: **42/42 pass, 0 fail** (corrected; source log `.session/logs/frdmk64f-selftest-42-20260720-182603.log`).
Note: the earlier 40/42 was the SAME single test bug as XMC. The privileged root thread really
retuned the K64F down to `KOS_PSTATE_LOW`, then `cpu_clock_set` (`not ok 5`) aborted on first
TAP_CHECK BEFORE restoring MAX, so tests 6-42 ran at the too-slow ~20.97 MHz -- which is what
made `mutex_chain_boost` (`not ok 14`) lose its timing race. With the test fixed (unprivileged
child, no retune), the clock stays at boot MAX and BOTH pass. Kernel/PI/clock-select unchanged.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   frdmk64f
   arch    armv7m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 18:24:48
   app     Jul 20 2026 18:24:48

1..42
# [svc] console_write roundtrip
ok 1 - svc_roundtrip
ok 2 - fifo_order
ok 3 - preempt_on_ready
ok 4 - cpu_clock_hz
ok 5 - cpu_clock_set
ok 6 - rr_interleave
ok 7 - sleep_order
ok 8 - multi_wait
ok 9 - sem_destroy
ok 10 - sem_destroy_quiescent
ok 11 - sem_raii
ok 12 - mutex_basic
ok 13 - mutex_pi_donation
ok 14 - mutex_chain_boost
ok 15 - mutex_owner_died
ok 16 - mutex_deadlock
ok 17 - mutex_close_owned
ok 18 - mutex_multi_held
ok 19 - mutex_unlock_errors
ok 20 - mutex_owner_died_nowaiter
ok 21 - mutex_deleg_refcount
ok 22 - endpoint_rendezvous
ok 23 - endpoint_reject
ok 24 - endpoint_rights
ok 25 - endpoint_epipe
ok 26 - endpoint_dead
ok 27 - endpoint_crossdomain
ok 28 - endpoint_bound
ok 29 - cap_index0
ok 30 - console_publish_priv
ok 31 - irq_thread_ctx
ok 32 - irq_as_event
ok 33 - irq_mask_drop
ok 34 - irq_autorearm
ok 35 - irq_phantom_wake
ok 36 - irq_ownership
ok 37 - irq_spurious
ok 38 - mpu_privileged_guard
ok 39 - caller_stack
ok 40 - domain_share
ok 41 - mmio_grant
# [confdep] unpriv rodata literal reaches the console
ok 42 - confused_deputy
# all tests passed
```

### bench (no MPU, telemetry OFF -- matches M1)
Build: preset `frdmk64f` -B `build/frdmk64f-bench` + `-DKICKOS_BENCH=1`, target `bench`.
Flash: `JLINK_SN=000621000000 FLASH_BUILD=.../build/frdmk64f-bench tools/flash-jlink.sh frdmk64f bench` -> OK.
NOTE: the DWT cycle counter reads ZERO free-running on this K64F build (`switch 0/0/0 cyc`,
`irq`/`wcase-irq` all `1/1/1 cyc`). The wall-clock numbers (`throughput` ns/sw, `wcase-hold` ns,
via `clock_now`) ARE valid. Verified deterministic across a reset-and-rerun. See the comparison
note below -- the M1 K64F record (77 cyc/641 ns) came from a DWT-live run; the cyc line is
therefore NOT comparable this pass.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   frdmk64f
   arch    armv7m
   mpu     off
   sched   tickless
   build   Jul 20 2026 18:00:45
   app     Jul 20 2026 18:00:45

microbenchmark: context-switch throughput (all arches) + per-switch cost
+ IRQ-entry latency where a cycle counter exists. Reporter woken by the
workload, not a timer. Telemetry OFF for clean numbers.

  throughput: 21379 ctx-sw/s  (46772 ns/sw avg over 40000 switches / 1870 ms)
  wcase-hold: 55645 ns masked / 256B endpoint-copy span
  switch: 0/0/0 cyc  0/0/0 ns  (min/avg/max, n=40001)
  irq:    1/1/1 cyc  8/8/8 ns  (min/avg/max, n=100)
  wcase-irq[0B]: 1/1/1 cyc  8/8/8 ns  (inject->entry, n=100)
  wcase-irq[64B]: 1/1/1 cyc  8/8/8 ns  (inject->entry, n=100)
  wcase-irq[256B]: 1/1/1 cyc  8/8/8 ns  (inject->entry, n=100)
  wcase-irq[1024B]: 1/1/1 cyc  8/8/8 ns  (inject->entry, n=100)
```

### clock-retune harness (reused clean capture)
Build: preset `frdmk64f` + `-DKICKOS_HAVE_MPU=1 -DKICKOS_CLOCK_RETUNE_TEST=ON`, app `clockretune`
(banner `mpu enforce`). Source log: `.session/logs/k64-clock-retune.log` (2026-07-20).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   frdmk64f
   arch    armv7m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 17:39:11
   app     Jul 20 2026 17:41:40

[clockretune] START privileged retune harness
[clockretune] boot: cpu_clock_hz = 120000000  clock_now t0 = 25612900 ns
[clockretune] spin MAX: 8000000 iters in 800537450 ns
[clockretune] after set(LOW): cpu_clock_hz = 20971520
[clockretune] now around retune: pre=826522533 r0=832323611 r1=832414210 r2=832504523 r3=832595409
[clockretune] seam-crossing delta r0-pre = 5801078 ns
[clockretune] MONOTONIC OK: no backward step, no phantom jump
[clockretune] spin LOW: 8000000 iters in 4583379745 ns
[clockretune] spin ratio LOW/MAX x100 = 572 (expect ~ 572 = hz_MAX/hz_LOW)
[clockretune] sleep_ns(200000000) at LOW measured 200415802 ns
[clockretune] after set(MAX): cpu_clock_hz = 120000000
[clockretune] console still readable after both retunes (this line proves baud)
[clockretune] clock retune test done
```

### console handover
N/A -- no K64F console driver. K64F is the coarse-AIPS functional board; its console is the
kernel VCOM ring, not a userspace console-handover driver. No consoledemo capture exists for it.

--------------------------------------------------------------------------------

## M1-vs-M3 comparison (bench: no MPU, telemetry OFF on both)

Switch + IRQ-entry, DWT-cycle-counter line (the directly comparable metric). ns computed at
each board's clock (XMC 144 MHz, K64F 120 MHz). M1 XMC from `M1_raw_meas.md`; M1 K64F from the
`M1_state.md` / `CONTEXT.local.md` silicon record (2026-07-15, DWT-live).

| board | metric        | M1 (cyc)    | M3 (cyc)    | M1 (ns)        | M3 (ns)        | delta          | note |
|-------|---------------|-------------|-------------|----------------|----------------|----------------|------|
| XMC   | switch min    | 74          | 79          | 513            | 548            | +5 cyc / +35ns | small, expected (M3 syscall-resolve + copy-under-lock) |
| XMC   | switch avg    | 82          | 88          | 569            | 611            | +6 cyc / +42ns | same |
| XMC   | switch max    | 653         | 674         | 4534           | 4680           | +21 cyc        | tail unchanged in character |
| XMC   | irq min/avg   | 133         | 139         | 923            | 965            | +6 cyc / +42ns | small, expected |
| XMC   | irq max       | 156         | 144         | 1083           | 1000           | -12 cyc        | within noise (n=100) |
| K64F  | switch min    | 77          | N/A (DWT=0) | 641            | N/A            | n/a            | K64F DWT reads 0 free-running this pass; cyc not comparable |
| K64F  | irq           | (not rec'd) | N/A (DWT=0) | --             | N/A            | n/a            | same |

Wall-clock cross-check (not a cyc metric, throughput reporter incl. workload overhead):
- XMC throughput: M1 39299 ctx-sw/s (25445 ns/sw) -> M3 35560 ctx-sw/s (28120 ns/sw). ~9.5% lower
  throughput, consistent with the +6 cyc/switch kernel-path growth plus the larger reporter loop.
- K64F throughput: M3 21379 ctx-sw/s (46772 ns/sw). No M1 throughput on record for K64F.

M3-only probes (no M1 baseline):
- XMC `wcase-hold` 46359 ns masked / 256B endpoint-copy span; K64F 55645 ns. This is the
  interrupts-masked span of the endpoint copy-under-lock -- the honest worst-case ISR-latency
  contribution the M3 IPC path adds. `wcase-irq[NB]` inject->entry sweep valid on XMC only
  (K64F DWT dead: reads 1 cyc).

Verdict:
- **XMC4800-Relax: PASS** (selftest 42/42 after the test fix below; bench clean with a small,
  expected M3 switch/IRQ regression; clock-retune monotonic; console handover OK).
- **FRDM-K64F: PASS** (selftest 42/42 after the test fix below; bench wall-clock valid but DWT
  cyc dead -- finding #3; clock-retune monotonic).

--------------------------------------------------------------------------------

## Findings

1. **`cpu_clock_set` + K64F `mutex_chain_boost` -- FIXED (single test bug).** The original 41/42
   (XMC) and 40/42 (K64F) were ONE selftest bug, not a kernel/silicon issue. `t_cpu_clock_set`
   asserted `kos_cpu_clock_set(...) == 0` (the unprivileged-reject sentinel) but ran on the
   selftest ROOT thread, which is unconditionally PRIVILEGED (kmain). `cpu_clock_set` is
   privileged-only, so on a chip with a real retune backend (XMC/K64F) the privileged call
   actually RETUNED and returned a non-zero landed Hz -> `not ok 5`. Worse, TAP_CHECK aborts on
   first failure, so the test bailed AFTER `set(LOW)` and never restored MAX; on K64F the whole
   suite then ran at ~20.97 MHz, and that too-slow clock lost the timing race in
   `mutex_chain_boost` -> `not ok 14`. The emulators "passed" only because they have no retune
   backend (weak default returns 0). Fix (test-only, kernel untouched): `t_cpu_clock_set` now
   spawns an UNPRIVILEGED child that calls `kos_cpu_clock_set` for each P-state and reports the
   result; the parent asserts the child saw 0 (privilege gate refuses -> no retune, clock stays
   at boot MAX). Privileged real-retune coverage stays in the `clockretune` harness. Re-validated
   2026-07-20: BOTH boards 42/42 (`ok 5 - cpu_clock_set`, `ok 14 - mutex_chain_boost`); source
   logs `.session/logs/{xmc4800,frdmk64f}-selftest-42-*.log`.

2. **K64F bench DWT cycle counter reads 0 free-running** (`switch 0/0/0 cyc`, `irq`/`wcase-irq`
   `1/1/1 cyc`). Deterministic across reruns. Wall-clock (`clock_now`) numbers unaffected and
   valid. XMC DWT counts normally (79 cyc etc.). The M1 K64F record (77 cyc/641 ns) came from a
   DWT-live run, so this pass cannot produce a comparable K64F cyc delta. NOT fixed.

## Coverage / what is NOT covered

- Only the two boards physically on the bench (K64F + XMC4800). The rest of the M1 fleet
  (picopi/RP2040, RX72M, ESP32-WROOM/C6, blackpill, f411disco, f302, bluepill, RP2350) was
  NOT re-run this pass.
- K64F console handover: N/A (no K64F userspace console driver -- coarse-AIPS functional board).
- K64F bench cycle-accurate switch/IRQ: unavailable this pass (DWT reads 0); only wall-clock
  throughput + wcase-hold are valid for K64F.
- clock-retune + XMC consoledemo captures were REUSED from the 2026-07-20 clean logs
  (`.session/logs/{xmc,k64}-clock-retune.log`, `.session/logs/xmc-consoledemo.log`), not
  re-flashed this pass.
