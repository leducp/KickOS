<!-- SPDX-License-Identifier: CECILL-C -->
# The kernel console

> Reference (code-synced): how the console works *as built* and the invariants a
> change must not break. The how-&-why narrative belongs in `../book/`; this page is
> the exact contract.

## What it is, and what it is not

The kernel console is a **write-only debug facility**: the banner, `kprintf` /
`kputs`, panic and fault reports, and (via a syscall) unprivileged `kos::print`.
It is the standard microkernel *exception* -- a minimal, kernel-owned output path
that must work during bring-up, in a fault, and before any driver exists. It is
**not** a general device driver: the eventual userspace UART/console driver is a
separate thing that will take the peripheral as a capability (see
[architecture.md](architecture.md), "Object model, capabilities & IPC" ->
"Console device handover"). Keep that
distinction -- much of the design below exists to protect the *debug* console's
"always works, even while dying" guarantee.

## The pipeline

```
kprintf / kputs / kernel code          (kprintf formats into a 256B stack buffer)
        |
        v
kconsole_write                 -- frontend, fans out to compile-time backends
   +- RTT   (CONSOLE=rtt|both):  memcpy -> SEGGER control block, under IrqLock
   \- chip  (CONSOLE=chip|both): CRLF-expand '\n'->'\r\n' (MCU only), then per chunk:
        v
console_emit                   -- THE routing guard
   |   armed && !arch_in_isr() && !panicking ?
   +- YES -> arch_console_write        (buffered)
   \- NO  -> arch_console_write_sync   (polled, always safe)
        v
[buffered]  console_tx_write:   memcpy burst into the SPSC ring (lock-free)
                                -> IrqLock { publish head; enable TX IRQ } -> return
        ... asynchronously ...
[drain ISR] console_tx_isr:     push ring bytes while a HW TX slot is free;
                                disable own TX IRQ when the ring drains empty
```

Source: `kernel/init/console.cc` (frontend + routing + panic), `kernel/init/console_tx.cc`
(the ring + drain + arming), `lib/include/kickos/console_tx.h` (the seam), and the
per-chip backends under `arch/*/chip/*`.

## The frontend: `kconsole_write`

`kconsole_write` fans out to every **compile-time-enabled** backend (`KICKOS_CONSOLE`
= `chip` | `rtt` | `both`, lowered to `KICKOS_CONSOLE_CHIP` / `_RTT` by CMake):

- **RTT** (`lib/rtt.cc`, channel 0) is a `memcpy` into a RAM control block that a
  debug probe drains; it is written under a short `IrqLock` because the same ring
  is touched from thread/ISR/fault contexts. It is arch-neutral (nothing Cortex-M
  in it). Channel 1 of the same block carries binary telemetry -- see
  [telemetry.md](telemetry.md).
- **chip** is the UART path. On MCU builds `'\n'` is expanded to `'\r\n'` here
  (`KICKOS_CONSOLE_CRLF`, off on the sim so the TAP stream stays raw); each
  resulting chunk goes through `console_emit`.

RTT and the UART are **separate transports** and can both be enabled -- RTT is not
a UART tee. Note that RTT needs a debug probe attached, so it is a
dev/bring-up transport, never the sole panic path in the field (see Invariants).

## The routing guard: `console_emit`

`console_emit` is the **single choke point** that decides buffered vs synchronous:

```
armed && !arch_in_isr() && !g_console_panicking  ->  arch_console_write      (buffered)
otherwise                                         ->  arch_console_write_sync (polled)
```

This is the load-bearing invariant of the whole design: **the buffered producer is
only ever entered in ordinary thread context.** Any ISR/fault caller, a panic in
progress, and all pre-arm boot output take the polled path. That is what lets the
ring be a true single-producer / single-consumer structure without a general lock
(see below). All four of `kconsole_write`'s chip-side calls go through
`console_emit` -- there is no other caller of `arch_console_write` in the tree, so
the guard has complete coverage.

## The buffered path (SPSC ring)

The ring decouples a write from the UART bit rate. The polled alternative busy-waits
on the TX-ready flag -- telemetry measured that as the single largest on-CPU cost
(~879 us per burst at the K64F FEI clock). The buffered producer instead copies and
returns; the bytes leave later, in an interrupt.

- **Producer** (`console_tx_write`, thread context): compute free space, `memcpy`
  the burst into `[head, ...)` **lock-free**, then under a brief `IrqLock`: publish
  the new `head` and enable the TX-empty IRQ ("prime the pump"). Returns
  immediately -- the caller's cost is a `memcpy`, not the transmission.
- **Consumer** (`console_tx_isr`, ISR context, bound to the chip's TX line via
  `irq_attach`): push ring bytes while a HW TX slot is free; when the ring drains
  to empty, disable its own TX IRQ.

**Why the lock is brief, and why it is correct.** The drain ISR runs at
`PRIO_DEVICE` (0x30). `IrqLock` raises `BASEPRI` to 0x20, which masks everything
numerically >= 0x20 -- including the TX ISR (`arch/arm/armv7m/regs.h`). So the
producer's "publish head + enable IRQ" is atomic with respect to the ISR's "drain
to empty + disable IRQ": either the ISR's empty-check happened before the publish
(then the producer's enable re-arms it) or after (then the ISR sees the new bytes
and keeps draining). **No lost wakeup, and no dropped output.** The lock is held
for a pointer store plus one bit -- nanoseconds -- not the ~22 ms a 256-byte
transmission used to hold. The `memcpy` itself is outside the lock: the ISR only
ever reads up to the *published* head, so writing the not-yet-published region
concurrently is the standard SPSC guarantee.

**Overflow policy: block-with-sync-drain, not drop.** If a burst does not fit, the
producer disables the TX IRQ, drains the ring and writes the burst by polling -- in
order, with other IRQs still serviced. RTT drops on a full ring (its host may be
detached, so a blocking writer would hang forever); the UART always makes bounded
progress, so **losing kernel debug output is worse than a bounded stall.** Ring
sizing is per-chip (>= `kprintf`'s 256 B buffer; e.g. 512 B on most parts, 2048 on the
ESP32-C6) and keeps this path rare.

## The synchronous path and the panic-safe seam

`arch_console_write_sync` is the bounded polled writer -- the one that is safe with
the scheduler and IRQs down. The seam is arranged so only the buffered backends
carry a distinct implementation:

- A **weak default** in `console.cc` forwards `arch_console_write_sync` ->
  `arch_console_write`. Every polled-only chip reuses its normal writer for free.
- Nearly every chip **overrides** it with a bounded polled writer (the fleet wraps its
  TX-ready poll in a spin-then-drop guard, so a wedged UART drops bytes instead of hanging
  the panic path); the sim overrides with a bounded one-byte-at-a-time `write(1, ...)` to
  host stdout. The weak default covers any chip that supplies no distinct sync writer.

**Panic and fault.** `kpanic` sets `g_console_panicking` (forcing every subsequent
write to the sync path), flushes the ring in order (`console_tx_flush_sync` --
disables the TX IRQ first so the ISR cannot race the drain), then prints via the
now-forced sync path, then halts. `kickos_isr_fault` flushes first too; it runs in
ISR context, so `console_emit` already routes its report to sync.

## Per-chip backend

A chip with a buffered console supplies a 4-function struct plus one hook:

```c
struct console_tx_backend { slot_free; push; irq_enable; irq_disable; };
console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line);
```

`console_buffer_init` (called from `kmain` after `irq_init`) asks the chip for its
backend; if present it binds `console_tx_isr` to `irq_line`, unmasks the line
(priority lands in the `IrqLock`-maskable band), and arms the ring. The generic
ring never names a register; the chip supplies only these pokes. The backend is
implemented across the fleet (K64F, XMC4800, the STM32 F1/F3/F4 parts, RP2040, SAM3X,
RX72M, ESP32, ESP32-C6, and the sim); three illustrative ones:

- **K64F (Kinetis UART0)** -- `slot_free` = `S1.TDRE`, `push` = `D`, the IRQ gate =
  `C2.TIE`, line = **IRQ 31** (UART0 RX/TX combined). `arch/arm/chip/mk64f/chip_mk64f.cc`.
- **XMC4800 (USIC0 CH0, ASC)** -- `slot_free` = `TCSR.TDV` clear, `push` = `TBUF0`,
  the IRQ gate = `CCR.TBIEN` routed by `INPR.TBINP` to service-request output SR0,
  line = **NVIC 84**. The gate lives in the shared USIC layer
  (`usic.cc`: `tx_irq_route`/`enable`/`disable`); the backend is in `usic_uart.cc`.
- **sim (fictional TX peripheral)** -- `slot_free`/`push`/`irq_enable`/`irq_disable`
  over a host-emulated TX-empty line delivered by `SIGUSR1` (a shared vector with
  the IRQ-inject signal); `push` writes one byte to host stdout, `irq_enable`
  `raise()`s the line, and the drain ISR (`console_tx_service` -> `kickos_isr_irq`)
  runs the real `console_tx_isr`. A **synthetic per-delivery slot budget** forces
  the ring to drain over several deliveries so it genuinely fills, primes, wraps,
  and empties -- host stdout never blocks, so without it the ring would drain in one
  shot. `arch/sim/sim.cc`.

A chip's TX IRQ-line number is a HW-confirm item (a wrong line silently never drains --
the ring fills and everything falls back to the bounded sync path, so it *looks* like it
works). The buffered drain is **silicon-validated** on the XMC4800, ESP32-C6, and K64F (a
full selftest streamed in-order over the armed ring). The **sim's backend** additionally
exercises the full SPSC ring -- producer, publish+prime, async drain ISR, wrap, and
overflow -- in-tree under `ctest`, which is what a sync-path-only run cannot cover.

## Boot ordering

The banner prints from `kmain` **before** `irq_init` -- the ring is not armed yet,
so it goes out on the sync path. `console_buffer_init` arms the ring immediately
after `irq_init`; steady-state logging from then on is buffered. So the ring speeds
ongoing logging, not the one-time banner. (If banner cost ever matters, bring-up
would have to be reordered so the console arms earlier.)

## The sim

The sim supplies a **fictional TX peripheral** (see Per-chip backend above), so it
arms the buffered path like a real chip: after `console_buffer_init`, steady-state
writes go through `console_tx_write` into the SPSC ring and drain asynchronously in
a `SIGUSR1`-delivered TX-empty ISR. This is deliberate -- the MCU backends never run
in-tree, so the sim is the **only** in-tree exerciser of the ring/drain/wrap/overflow
paths; the selftest and stress runs push enough output to fill and wrap the ring
repeatedly under `ctest`.

Details specific to the sim:

- **Output is still raw** -- CRLF expansion stays off (`KICKOS_CONSOLE_CRLF`), so the
  TAP stream is byte-clean for the test harness.
- **The emulated TX line shares `SIGUSR1`** with the IRQ-inject path (a shared
  interrupt vector): `on_sigusr1` consumes the assertion, runs the drain, and
  re-asserts if the synthetic slot budget left bytes queued.
- **The fault handler enters ISR context** (`isr_frame_enter` before
  `kickos_isr_fault`) so the routing guard sees `arch_in_isr()` and takes the sync
  writer -- parity with the ARM fault path, where the report must not be enqueued
  into a ring that the handler `_exit()`s without draining.
- **`arch_shutdown` flushes synchronously** (`console_tx_flush_sync`) before
  `_exit`, because IRQs/signals are masked there and the `SIGUSR1` drain can no
  longer run -- otherwise the final `[ktrace] counters` line would be stranded in
  the ring.

## Invariants a change must not break

1. **The buffered producer runs in thread context only.** The SPSC argument
   depends on it. Keep the `arch_in_isr()` guard at the lowest choke point
   (`console_emit`); any future path that logs from a raw ISR must not reach
   `console_tx_write`.
2. **The TX ISR is `IrqLock`-maskable.** It must sit in the device priority band
   (`PRIO_DEVICE`), or the producer's publish+prime is no longer atomic against it.
3. **The TX IRQ is enabled whenever the ring is non-empty.** The ISR disables it
   *only* on drain-to-empty; the producer *always* re-enables after publishing.
4. **Panic output must not depend on the buffered path or on a debug probe.** It
   flushes the ring, forces the sync path, and -- once the eventual userspace driver
   owns the UART -- must *reclaim + reinit* the peripheral and polled-print,
   because RTT needs a J-Link and the userspace driver may be the thing that
   crashed. The diag LED is the always-present 1-bit last resort.

## Future (capability handover)

When a userspace UART driver takes the device as a capability, the kernel must
relinquish it: a `console_tx_deinit` (disable TX IRQ -> `irq_detach` -> flush ->
disarm), written against that real caller -- not now. The panic path then moves to
a kernel-retained transport per invariant 4. See [architecture.md](architecture.md),
"Object model, capabilities & IPC" -> "Console device handover".
