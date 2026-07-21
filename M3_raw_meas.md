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

================================================================================

# M3 fleet-validation pass (2026-07-20, same bench window)

Extension of the XMC+K64F pass above to the rest of the connected fleet, same bench
window (2026-07-20), same branch `m3-integration`. Two threads run per board:
  1. **M3 capability/IPC core selftest under enforcement** -- arch-neutral, expected to
     pass fleet-wide (the same 42-test set as above; a no-MPU board runs 40, see WROOM).
  2. **Clock-hardening silicon validation** via a NEW harness `user/apps/clocksoak`
     (gated `-DKICKOS_CLOCKSOAK_TEST=ON`, per-board `-DKICKOS_CLOCKSOAK_WRAP_MS`,
     arch-neutral: `clock_now` + `sleep_ns` only). Phase A idles past >1 counter wrap
     period (one sleep of 1.5x the wrap -- idle-wrap observer); phase B soaks across N
     wraps, asserting monotonicity + per-chunk rate + whole-soak rate.

Same capture convention as above: verbatim KickOS output, only the trailing serial CR
dropped and the J-Link/ST-Link/ESP-ROM boot preamble (and any ANSI escapes) trimmed.

--------------------------------------------------------------------------------

## STM32F411-Disco (armv7m Cortex-M4F / PMSA, 84 MHz)

### selftest -- UNDER ENFORCEMENT
Build: preset `f411disco-st` + `-DKICKOS_HAVE_MPU=1`, target `selftest`.
Flash: ST-Link `st-flash --connect-under-reset --reset write <bin> 0x08000000` -> OK.
Console: FTDI on PA2, `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_BH001J9H-if00-port0` @115200.
Result: **42/42 pass, 0 fail** (source log `.session/logs/f411-sel-BH001J9H.log`).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   f411disco
   arch    armv7m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 20:53:30
   app     Jul 20 2026 20:53:30

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

### clocksoak -- clock-hardening silicon validation
Build: preset `f411disco` + `-DKICKOS_CLOCKSOAK_TEST=ON -DKICKOS_CLOCKSOAK_WRAP_MS=51000`,
app `clocksoak` (banner `mpu off`). Counter: TIM2, real ~51 s wrap period.
Result: **PASS** (source log `.session/logs/f411-clocksoak.log`).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   f411disco
   arch    armv7m
   mpu     off
   sched   tickless
   build   Jul 20 2026 20:52:46
   app     Jul 20 2026 20:52:46

[clocksoak] START clock-hardening soak harness
[clocksoak] boot: cpu_clock_hz = 84000000  wrap_period = 51000 ms  wraps = 3  t0 = 25397464 ns
[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)
[clocksoak] phase A: requested 76500000000 ns, measured 76500065929 ns  (mono=1 rate=1)
[clocksoak] phase B: soak across N wraps
[clocksoak] wrap 1/3: measured 51000061119 ns  cum=127500963834 ns  (seam_mono=1 rate=1)
[clocksoak] wrap 2/3: measured 51000061119 ns  cum=178501387477 ns  (seam_mono=1 rate=1)
[clocksoak] wrap 3/3: measured 51000061119 ns  cum=229501811168 ns  (seam_mono=1 rate=1)
[clocksoak] soak total: requested 153000000000 ns, measured 153001745239 ns, ratio x100 = 100 (expect ~100)
[clocksoak] VERDICT PASS: monotonic across wraps, rate-correct, idle kept counting
[clocksoak] clock soak test done
```

--------------------------------------------------------------------------------

## ESP32-WROOM (Xtensa LX6 / NO MPU, 240 MHz)

### selftest -- 40/40 (no-MPU board)
Build: `-DKICKOS_HAVE_MPU=0`, target `selftest` (banner `mpu off`).
Flash: esptool over CH340.
Result: **40/40 pass, 0 fail** (source log `.session/logs/wroom-selftest.log`).
NOTE: 40 vs the 42 on MPU boards -- the two absent tests are the `HAVE_MPU`-gated ones
(`endpoint_bound` + `mpu_privileged_guard`), correct for a no-MPU board; all M3-core tests
pass. Renumbering below (cap_index0 lands at 28, confused_deputy at 40) follows from the
two dropped tests, not a reordering.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   esp32-wroom
   arch    lx6
   mpu     off
   sched   tickless
   build   Jul 20 2026 20:55:59
   app     Jul 20 2026 20:55:59

1..40
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
ok 28 - cap_index0
ok 29 - console_publish_priv
ok 30 - irq_thread_ctx
ok 31 - irq_as_event
ok 32 - irq_mask_drop
ok 33 - irq_autorearm
ok 34 - irq_phantom_wake
ok 35 - irq_ownership
ok 36 - irq_spurious
ok 37 - caller_stack
ok 38 - domain_share
ok 39 - mmio_grant
# [confdep] unpriv rodata literal reaches the console
ok 40 - confused_deputy
# all tests passed
```

### clocksoak -- clock-hardening silicon validation
Build: `-DKICKOS_CLOCKSOAK_TEST=ON -DKICKOS_CLOCKSOAK_WRAP_MS=60000`, app `clocksoak`
(banner `mpu off`). Counter: TIMG0 (64-bit -- does NOT actually wrap in this soak, see below).
Result: **PASS** (source log `.session/logs/wroom-clocksoak.log`).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   esp32-wroom
   arch    lx6
   mpu     off
   sched   tickless
   build   Jul 20 2026 20:53:00
   app     Jul 20 2026 20:53:00

[clocksoak] START clock-hardening soak harness
[clocksoak] boot: cpu_clock_hz = 240000000  wrap_period = 60000 ms  wraps = 3  t0 = 19483675 ns
[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)
[clocksoak] phase A: requested 90000000000 ns, measured 90000024700 ns  (mono=1 rate=1)
[clocksoak] phase B: soak across N wraps
[clocksoak] wrap 1/3: measured 60000024700 ns  cum=150014809300 ns  (seam_mono=1 rate=1)
[clocksoak] wrap 2/3: measured 60000024675 ns  cum=210015029525 ns  (seam_mono=1 rate=1)
[clocksoak] wrap 3/3: measured 60000024675 ns  cum=270015249750 ns  (seam_mono=1 rate=1)
[clocksoak] soak total: requested 180000000000 ns, measured 180015225050 ns, ratio x100 = 100 (expect ~100)
[clocksoak] VERDICT PASS: monotonic across wraps, rate-correct, idle kept counting
[clocksoak] clock soak test done
```

--------------------------------------------------------------------------------

## ESP32-C6 (RV32IMAC / PMP, 160 MHz)

### selftest -- UNDER ENFORCEMENT
Build: `-DKICKOS_HAVE_MPU=1` (PMP), target `selftest` (banner `mpu enforce`).
Flash: esptool over CH343P.
Result: **42/42 pass, 0 fail** (source log `.session/logs/c6-selftest.log`).
NOTE: the ESP-ROM prints a cosmetic `SHA-256 comparison failed ... Attempting to boot
anyway...` for the raw KickOS image (expected image hash is all-`ff`) -- harmless, the
image boots and runs normally. Preamble trimmed from the capture below.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   esp32c6-wroom
   arch    rv32imac
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 20:56:02
   app     Jul 20 2026 20:56:02

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

--------------------------------------------------------------------------------

## RX72M (RXv3 / RX-MPU, 240 MHz)

### selftest -- UNDER ENFORCEMENT
Build: `-DKICKOS_HAVE_MPU=1` (RX-MPU), target `selftest` (banner `mpu enforce`).
Flash: rfp-cli / E2-Lite (FINE).
Console: SCI6 on FTDI `/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_BG03AS3Q-if00-port0` @115200.
Result: **42/42 pass, 0 fail** (source log `.session/logs/rx72m-selftest.log`).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   rx72m
   arch    rxv3
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 20:56:04
   app     Jul 20 2026 20:56:04

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

--------------------------------------------------------------------------------

## Findings (fleet pass)

3. **clocksoak: the per-`sleep_ns` overhead is a CONSTANT floor -- it does NOT scale with
   duration and does NOT accumulate across wraps.** On F411 the overhead is ~61 us on every
   51 s soak chunk (all three chunks byte-identical at `51000061119 ns`) and ~66 us on the
   76.5 s phase-A sleep; on WROOM it is ~24.7 us per chunk at 240 MHz. Whole-soak ratio
   x100 = 100 on both boards. This constant, non-accumulating floor is the proof the
   wrap-fold adds exactly zero error: a rate error would scale with duration; a lost wrap
   would show as -1 whole wrap period; a phantom leap would show as seconds-to-minutes. On
   F411 the soak crossed ~4.5 real TIM2 wraps (51 s each: 1.5 in phase A + 3 in phase B),
   validating clocksoak checklist items 1 (idle-wrap observer), 2 (soak-across-wraps),
   3 (rate/monotonicity), 4 (WFI-keeps-counting), 5 (overflow handled in the chip ISR).

4. **WROOM honest-coverage caveat: TIMG0 is 64-bit and does NOT wrap in the ~270 s soak.**
   Checklist items 1/2 (an actual counter wrap) are therefore N/A on WROOM. What the WROOM
   run does prove is items 3 (rate/monotonicity) + 4 (WAITI keeps the timebase counting --
   the CCOUNT-freeze regression fix). The `wrap N/M` lines on WROOM are soak chunks by
   wall-clock, not silicon counter rollovers.

## Coverage / what is NOT covered (fleet pass)

- The connected fleet is now M3-validated (selftest under enforcement, plus clocksoak on
  the two boards run): **XMC4800-Relax, FRDM-K64F, STM32F411-Disco, ESP32-WROOM, ESP32-C6,
  RX72M -- all PASS**.
- **picopi/RP2040: PENDING (not skipped)** -- deferred to a user-assisted run (needs a
  physical reset + a tty swap). To be run separately.
- clocksoak was run on F411 (TIM2, real wrap) and WROOM (TIMG0, 64-bit, no real wrap) only;
  the selftest-only boards (XMC, K64F, C6, RX72M) did not run clocksoak this pass.
- The other clock-hardening boards F103 / F302 / SAM3X were NOT run this pass: F103 and F302
  are not on the bench, and the SAM3X unit is retired.

================================================================================

# M3 extended-fleet pass (2026-07-20 cont.)

Continuation of the fleet pass above, same bench window (2026-07-20), same branch
`m3-integration`. Adds the two RP2xxx boards on silicon and two clock soaks (C6, RX72M),
plus a host-side capture-tool artifact that came out of the C6 soak (first mis-read as a
console-TX driver bug, since corrected). Same capture convention: verbatim
KickOS output, only the trailing serial CR dropped and the ESP-ROM / J-Link boot preamble
(and any ANSI escapes) trimmed.

--------------------------------------------------------------------------------

## picopi / RP2040 (armv6m Cortex-M0+, PMSA)

### selftest -- UNDER ENFORCEMENT
Build: preset `picopi-st` + `-DKICKOS_HAVE_MPU=1 -DKICKOS_MTX_UNIT_SCALE=10`, target `selftest`.
Flash: UF2 over the BOOTSEL mass-storage volume. Console: GP0 -> FTDI on ttyUSB1, `picocom`.
Result: **42/42 pass, 0 fail** (source log `.session/logs/picopi-selftest-10x.log`).
KEY FINDING (this M3 pass): at the DEFAULT mutex-test time unit the M0+ failed
`not ok 14 - mutex_chain_boost` (`main.cc:836`, `nth('e',1) < nth('d',1)`) and needed the 10x
unit (`-DKICKOS_MTX_UNIT_SCALE=10`) to reach 42/42. This was read as slow-core timing-margin
fragility. UPDATE 2026-07-21: root-caused on silicon to TWO separate defects, both now FIXED (see
Finding #5). The x10 capture above stays for the record; the fixed default run is 42/42 (source
log `.session/logs/rp2040-default.log`, build Jul 21 2026 13:38:38, identical 42-test list).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   picopi
   arch    armv6m
   mpu     enforce
   sched   tickless
   build   Jul 20 2026 21:05:09
   app     Jul 20 2026 21:41:57

1..42
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
ok 42 - confused_deputy
# all tests passed
```
Default-unit (x1) run, for the record -- identical except:
```
not ok 14 - mutex_chain_boost # /home/leduc/projets/KickOS/user/apps/selftest/main.cc:836: nth('e', 1) < nth('d', 1)
# 1 test(s) failed
```

### Silicon fault dump -- the enforcement-soundness race (pre-fix)
A 50 ms x300 instrumented chain-repro build (`.session/logs/rp2040-stguard.log`) faulted 2/2 runs
on silicon, exposing a REAL enforcement race (see Finding #5), NOT a timing artifact. Trimmed to
the fault frame -- note SP-OUTSIDE-OWN-STACK while the live HW MPU regions match the (wrong) `cur`
thread, i.e. physical-thread != logical-current:
```
=== HARD FAULT ===
  PC=0x10002096 LR=0x1000b4cf xPSR=0x61000000 (PSP)
  SP(pre-fault PSP)=0x20011fd0  frame@0x20011fb0
  cur tid=35 'chA' priv=0 stack=[0x20014000,0x20016000) nreg=3
  SP-OUTSIDE-OWN-STACK  tcb-saved ctx.sp=0x20015e80 switch_count=2
  tcb reg[2] base=0x20014000 end=0x20016000 attr=0x3
  MPU_CTRL=0x5 hw_regions=8
  hw reg[2] RBAR=0x20014002 RASR=0x13030019
```
MPU enforcement turned a would-be-silent cross-stack corruption into a catchable fault. Post-fix
the same repro runs 42/42 clean.

--------------------------------------------------------------------------------

## RP2350 / Waveshare Pi-Zero (Cortex-M33, armv7m layer, mpu off) -- FIRST SILICON

### selftest
First time this port ran on silicon. Build: preset `pizero2350-st`, target `selftest`
(banner `mpu off`). Flash: J-Link Pro (device `RP2350_M33_0`, SN `000177003338`).
Console: repointed from the build-only GP0/UART0 guess to UART1/GP4 -- the Pi-Zero header's
UART footprint (pin 8 TXD) routes to GP4/GP5 = UART1; UART0 cannot mux there. `picocom`.
Result: **40/40 pass, 0 fail** (source log `.session/logs/pizero2350-selftest.log`).
40 not 42 = the two `HAVE_MPU`-gated tests absent on a no-MPU build (as on WROOM). A manual POR
reset boots it cleanly; the J-Link `g` / SYSRESETREQ path did not reliably (re)start the console.
Note: the M33 rides the armv7m arch layer for the core (v7-M is a subset the M33 implements);
v8-M PMSA enforcement needs a new backend (see `docs/design-rp2350-mpu-armv8m.md`), hence `mpu off`.
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   pizero2350
   arch    armv7m
   mpu     off
   sched   tickless
   build   Jul 20 2026 21:07:15
   app     Jul 20 2026 22:02:05

1..40
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
ok 28 - cap_index0
ok 29 - console_publish_priv
ok 30 - irq_thread_ctx
ok 31 - irq_as_event
ok 32 - irq_mask_drop
ok 33 - irq_autorearm
ok 34 - irq_phantom_wake
ok 35 - irq_ownership
ok 36 - irq_spurious
ok 37 - caller_stack
ok 38 - domain_share
ok 39 - mmio_grant
ok 40 - confused_deputy
# all tests passed
```

--------------------------------------------------------------------------------

## ESP32-C6 clocksoak -> a host-side capture artifact (NOT a clock bug, NOT a driver bug)

The C6 selftest already passed 42/42 under PMP enforcement (see the fleet pass above). This
extended-pass clocksoak first LOOKED like a console-output stall; on investigation (corrected
2026-07-21) it is neither the clock, nor the timer, nor the C6 UART -- it is a HOST-SIDE capture
artifact in the pyserial `.session/cap_esp.py` tool. The narrative below is the evidence; the
earlier "M4 console-TX-driver bug" classification is REFUTED.

### The apparent stall (a truncated cap_esp.py capture)
Two soak configs were captured with the pyserial `.session/cap_esp.py` (90 s wrap and 5 s wrap).
Both completed phase A CORRECTLY and then went silent partway through phase B, always at
~11-12.5 s cumulative uptime.

90 s wrap config (`.session/logs/c6-clocksoak.log`) -- boot + phase-A header, then silence
(the `phase A:` header is the last line emitted; the phase-A RESULT line never printed):
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   esp32c6-wroom
   arch    rv32imac
   mpu     off
   sched   tickless
   build   Jul 20 2026 21:10:34
   app     Jul 20 2026 21:10:35

[clocksoak] START clock-hardening soak harness
[clocksoak] boot: cpu_clock_hz = 160000000  wrap_period = 60000 ms  wraps = 3  t0 = 6203206 ns
[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)
```

5 s wrap config (`.session/logs/c6-clocksoak-short.log`) -- gets one chunk further into phase B,
then goes silent after `wrap 1/3` (cum=12.5 s), mid-soak:
```
[clocksoak] START clock-hardening soak harness
[clocksoak] boot: cpu_clock_hz = 160000000  wrap_period = 5000 ms  wraps = 3  t0 = 6203187 ns
[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)
[clocksoak] phase A: requested 7500000000 ns, measured 7500022343 ns  (mono=1 rate=1)
[clocksoak] phase B: soak across N wraps
[clocksoak] wrap 1/3: measured 5000022344 ns  cum=12500508713 ns  (seam_mono=1 rate=1)
```

### It is NOT the clock and NOT the timer
A UART-INDEPENDENT LED beacon (WS2812 on GPIO8: green blink while idle) kept BLINKING past the
console-silence point and only went solid when the whole ~22.5 s soak completed. So the CPU ran
the entire soak, the CLINT `mtime` timer fired throughout (no blue / overdue LED), and every
completed chunk was rate-correct and monotonic (~22 us floor, matching the fleet). The `c6timer`
trace build (`.session/logs/c6-led.log`) corroborates: `ARM` / `DISARM` / `idle mt=...` entries
keep advancing (`idle mt=0x...263f53ac`, then `0x...4c64f3f2`) across and past the console-silence
point -- the timebase never froze:
```
[clocksoak] START clock-hardening soak harness
[clocksoak] boot: cpu_clock_hz = 160000000  wrap_period = 5000 ms  wraps = 3  t0 = 7242606 ns
[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)
[c6timer] idle mt=0x00000000263f53ac cmp=0x000000004798b1a4 ctl=0x00000003
[c6timer] DISARM now=0x000000004798c0b9
[c6timer] DISARM now=0x000000004798d909
[[c6timer] ARM  cmp=0x0000000077486020 now=0x00000000479971dc ctl=0x00000003
[c6timer] ARM  cmp=0x0000000077486020 now=0x00000000479c82ae ctl=0x00000003
clocksoak] phase A: requested 7500000000 ns, measured 7500091794 ns  (mono=1 rate=1)
[clocksoak] phase B: soak across N wraps
[c6timer] idle mt=0x000000004c64f3f2 cmp=0x0000000077486020 ctl=0x00000003
```
The C6 kernel / scheduler / clock are SOUND. (The RX72M soak below and the F411/WROOM soaks in
the fleet pass are the positive clock-hardening evidence; this run adds C6 to that set on the
clock axis.)

### The bytes DID leave the chip (LED-gated TX-wedge probe)
A LED-gated run drove the WS2812 beacon MAGENTA on any UART0 TX-FIFO-drain stall (the TX-wedge
probe) and left it GREEN otherwise. Across the full soak the beacon NEVER went magenta -- so the
on-chip UART0 TX FIFO drained fine the whole time and the wedge probe never tripped. The bytes
left the chip; they were dropped on the HOST side of the link. This retires the earlier
"UART0 buffered-ring console stops draining" hypothesis: the ring / ISR / TX FIFO were never the
problem.

### The clean run + root cause (host-side capture artifact)
Re-capturing the SAME clocksoak with PICOCOM on `/dev/ttyACM0` (DTR-safe) ran it end to end --
3 wraps, whole-soak ratio x100 = 100, monotonic:
```
[clocksoak] VERDICT PASS: monotonic across wraps, rate-correct, idle kept counting
[clocksoak] clock soak test done
```
So the C6 clocksoak PASSES. ROOT CAUSE is a host capture-tool artifact: the pyserial
`.session/cap_esp.py` dropped the output on long / idle-gapped runs (a quadratic
decode + regex-over-a-growing-buffer stall overflowed the serial RX), where picocom / cat do not.
The `c6-clocksoak-short.log` capture above was the truncated cap_esp run; the picocom run is the
clean one. NOT a KickOS bug, NOT a C6 driver bug -- the C6 chip / kernel / scheduler / clock /
UART are ALL SOUND (selftest 42/42 under PMP stands; the clocksoak is clock-clean).

Bench lesson: capture long ESP runs with picocom or cat (DTR-safe), not the pyserial cap_esp.py.

--------------------------------------------------------------------------------

## RX72M clocksoak (CMTW)

Build: preset `rx72m` + `-DKICKOS_CLOCKSOAK_TEST=ON -DKICKOS_CLOCKSOAK_WRAP_MS=60000`, app
`clocksoak` (banner `mpu off`). Counter: CMTW at 240 MHz. Flash: rfp-cli / E2-Lite (FINE).
Result: **PASS** (three 60 s chunks, ~19 us floor, ratio x100 = 100; source log
`.session/logs/rx72m-clocksoak.log`).
```
  ==============================================
   KickOS 0.0.1  -  microkernel RTOS
  ==============================================
   board   rx72m
   arch    rxv3
   mpu     off
   sched   tickless
   build   Jul 20 2026 21:10:37
   app     Jul 20 2026 21:10:37

[clocksoak] START clock-hardening soak harness
[clocksoak] boot: cpu_clock_hz = 240000000  wrap_period = 60000 ms  wraps = 3  t0 = 24318133 ns
[clocksoak] phase A: single sleep past 1 wrap (idle-wrap observer)
[clocksoak] phase A: requested 90000000000 ns, measured 90000019067 ns  (mono=1 rate=1)
[clocksoak] phase B: soak across N wraps
[clocksoak] wrap 1/3: measured 60000019067 ns  cum=150000268667 ns  (seam_mono=1 rate=1)
[clocksoak] wrap 2/3: measured 60000018933 ns  cum=210000387600 ns  (seam_mono=1 rate=1)
[clocksoak] wrap 3/3: measured 60000018800 ns  cum=270000507467 ns  (seam_mono=1 rate=1)
[clocksoak] soak total: requested 180000000000 ns, measured 180000488400 ns, ratio x100 = 100 (expect ~100)
[clocksoak] VERDICT PASS: monotonic across wraps, rate-correct, idle kept counting
[clocksoak] clock soak test done
```
RX (CMTW) and C6 (CLINT `mtime`) were already sound, unchanged by the clock-hardening work --
this soak confirms the RX clock sound end-to-end; the C6 soak (section above) confirmed its
CLOCK sound too (the apparent stall was a host capture-tool artifact).

--------------------------------------------------------------------------------

## Findings (extended pass)

5. **RP2040 `mutex_chain_boost` -- root-caused on silicon to TWO defects, both FIXED (2026-07-21;
   the earlier "just slow-core timing-margin, not a kernel bug" read was incomplete).**
   (a) **A REAL enforcement-soundness race (kernel, armv6m).** `switch_to()` applies the MPU
   EAGERLY (`arch_mpu_apply(next)`) but the armv6m context swap is a DEFERRED PendSV -- so in the
   window before the physical swap the OUTGOING thread runs with `next`'s region set and faults on
   its OWN stack. The instrumented silicon dump (above) caught it: SP-OUTSIDE-OWN-STACK with the
   live HW MPU matching the (wrong) `cur` thread, and PC in one thread's code while `cur` was
   another -> physical-thread != logical-current. MPU enforcement turned a would-be-silent stack
   corruption into a catchable fault. Fix: armv6m `arch_mpu_apply` now only STASHES the region
   set; `kickos_armv6m_mpu_commit` programs the HW in the PendSV epilogue, ATOMICALLY with the
   physical swap. v7-M unchanged. Silicon-validated: a 50 ms x300 chain-repro that faulted 2/2
   pre-fix now runs 42/42 clean. Latent fleet-wide (v7-M / RX / RISC-V share the same
   eager-apply + deferred-switch shape) -- TODO-tracked, fable-gated. Committed on `m3-integration`.
   (b) **The default-unit soft-fail (separate).** The M0+ was also too slow to form the 4-thread /
   2-mutex chain at the old hardcoded 1 ms `mtx` unit (needed `x10`). Fixed by a self-calibrating
   `mtx_time_unit()` that measures reschedule cost (1 ms floor / 30 ms cap): RP2040 DEFAULT
   selftest now 42/42 (source `.session/logs/rp2040-default.log`). qemu-armv7m + qemu-riscv CI
   gates still pass.

6. **RP2350 first silicon: console footprint is UART1/GP4, not UART0/GP0.** The build-only guess
   was wrong; the Pi-Zero header's TXD (pin 8) muxes only to UART1. Corrected and 40/40 on silicon.
   Reset path: manual POR boots cleanly; J-Link `g` / SYSRESETREQ did not reliably restart the
   console. The M33 runs on the armv7m arch layer (v7-M subset); v8-M PMSA enforcement is future
   work (`docs/design-rp2350-mpu-armv8m.md`), so this board is `mpu off` for now.

7. **ESP32-C6 clocksoak "stall" was a HOST capture artifact -- kernel/clock/UART all sound
   (corrected 2026-07-21; the earlier M4 console-TX-driver classification is REFUTED).** The
   pyserial `.session/cap_esp.py` captures went silent at ~11-12.5 s cumulative, but the C6 ran
   the full soak: a UART-independent WS2812 beacon kept blinking through the whole ~22.5 s and
   went solid only at completion, and the `c6timer` trace kept advancing past the silence point.
   An LED-gated run showed the beacon NEVER went magenta -> the on-chip UART0 TX FIFO drained
   fine and the TX-wedge probe never tripped, so bytes LEFT the chip. Re-capturing the same soak
   with picocom on `/dev/ttyACM0` reached `VERDICT PASS` (3 wraps, ratio x100 = 100, monotonic).
   Root cause: cap_esp.py drops output on long / idle-gapped runs (quadratic decode + regex over a
   growing buffer overflows the RX) where picocom / cat do not. NOT a KickOS bug, NOT a C6 driver
   bug; selftest 42/42 under PMP stands. Bench lesson: capture long ESP runs with picocom or cat,
   not cap_esp.py.

8. **microbit CI gate was red on pool exhaustion, not a real failure -- FIXED (test-only).** The
   mutex / endpoint tests need 3-4 threads and several cap-slots, but microbit's pool is tiny
   (`MAX_THREADS=2`, main cap table 3 free). The tests hard-failed on the shortage and LEAKED,
   cascading later tests red. Fixed by giving them the same pool-shortage SKIP guard (clean up +
   skip) the sibling multi-thread tests already use: microbit now green, while full-pool boards
   still RUN the tests. Committed.

## Coverage (extended pass)

- The connected fleet validated on silicon is now **8 boards**: XMC4800-Relax, FRDM-K64F,
  STM32F411-Disco, ESP32-WROOM, ESP32-C6, RX72M, RP2040 (picopi), RP2350 (pizero2350).
- Caveats:
  - **RP2040**: 42/42 at the DEFAULT unit after the two 2026-07-21 fixes (armv6m MPU-commit race +
    self-calibrating `mtx_time_unit()`); the `x10` capture is the pre-fix M3-pass workaround --
    finding #5.
  - **ESP32-C6**: fully clean -- selftest 42/42 under PMP and the clocksoak PASSES; the apparent
    console stall was a host cap_esp.py capture artifact, not a chip / driver bug (finding #7).
  - **Teensy 4.1 (imxrt1062)**: remains BUILD-ONLY; silicon pass pending a `teensy_loader_cli`
    install, scheduled next session.
  - **F103 / F302**: not on the bench this pass. **SAM3X**: unit retired.
- clocksoak this extended pass: C6 (CLINT `mtime`, PASS -- clean once captured with picocom) and
  RX72M (CMTW, PASS). Combined with the fleet pass, clocksoak now covers F411, WROOM, C6, RX72M.
