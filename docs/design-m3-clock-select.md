<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M3 clock retune design

> **Status: LANDED.** The code-synced sequence is in
> [`reference/invariants.md`](reference/invariants.md) under
> `clock-retune-coherence-tail`, `clock-anchor-sole-writer-at-rate-edge` and
> `timer-arm-dedup-needs-disarm`.

`kos_cpu_clock_hz()` reads the core rate. `KOS_SYS_CPU_CLOCK_SET` accepts a
`kos_pstate_t`, requires `KOS_AUTH_PSTATE`, and returns the **landed** rate in Hz. A
backend that cannot change its clock returns 0. A staged transition that lands on a
fallback point returns that point's nonzero rate, so the caller can run the same
coherence tail whenever the rate actually changed. There is no separate P-state read
call: the landed rate is the observable state.

## Retune sequence

A retune is a single-core operation from privileged syscall context. Refuse it before
masking interrupts unless the console is `KERNEL_OWNED`; the kernel cannot recompute a
userspace driver's baud. Under one `IrqLock`, disarm the timer, drain the console to
**shift idle**, call `arch_cpu_clock_set`, recompute baud after an actual rate change,
and re-arm the timer. Nanosecond deadlines and round-robin slices retain their values.

The backend raises flash wait states and voltage before increasing frequency, and
lowers them after decreasing it. Where the silicon requires it, the backend walks its
divider or PLL state machine rather than jumping directly. It re-anchors the monotonic
clock at the rate edge: capture elapsed nanoseconds under the old rate, change the
clock, then accrue later ticks at the new rate. `arch_clock_now` only reads that
anchor; it never changes the rate lazily. Otherwise a read just after the hardware
change would reprice all elapsed time and shift deadlines by seconds.

Disarming before the change is required even if the nearest deadline is unchanged:
`ktime_rearm` deduplicates that deadline, so a bare re-arm could leave hardware
counting at the old rate. The retune sequence must return without a half-updated
anchor or timer state.

## Backend scope

XMC4800 supports 144, 96 and 48 MHz via its divider staircase. K64F supports 120 MHz
PEE and about 20.97 MHz FEI; MID rounds up to MAX. Other chips return the unsupported
fallback until they implement the full sequence. A P-state whose peripheral clock
cannot produce the console baud within tolerance is refused without moving the clock.

## Ruling 5: multicore boundary

The `IrqLock` argument quiesces one timer on one kernel core. A shared-kernel retune
requires a cross-core barrier and per-core timer re-arm before it can use this seam.
Explicit STOP/STANDBY states, counters stopped in deep sleep, DVFS and peripheral clock
gating need separate designs.
