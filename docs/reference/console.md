<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
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
   |   g_console_state ?                            (ownership axis, checked FIRST)
   +- USER_OWNED  -> DROP  (a userspace driver owns the UART; RTT still carries it)
   +- RECLAIMED   -> arch_console_write_sync        (panic OR a driver death took it back; polled)
   \- KERNEL_OWNED:
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

`console_emit` is the **single choke point** that decides where output goes. It first
consults the **ownership axis** `g_console_state` (who owns the UART TX register),
because in the middle value the kernel must touch the device on *no* path at all:

```
USER_OWNED    ->  DROP: the kernel touches nothing (RTT still carries the bytes)
RECLAIMED     ->  arch_console_write_sync (panic or driver death reclaimed the UART -> polled only)
KERNEL_OWNED  ->  the buffered-vs-sync sub-decision below
```

and only when `KERNEL_OWNED` does it make the buffered-vs-synchronous choice:

```
armed && !arch_in_isr() && !g_console_panicking  ->  arch_console_write      (buffered)
otherwise                                         ->  arch_console_write_sync (polled)
```

`g_console_state` starts `KERNEL_OWNED` (every board that never hands over stays here,
so the sub-decision is the whole story for them); `kos_console_publish` flips it to
`USER_OWNED`, and a panic flips it to `RECLAIMED` **from any prior state, `KERNEL_OWNED`
included** -- the axis records a publish, never whether the device is garbled, and a
thread granted the console window can wreck the channel without ever publishing. See
[architecture.md](architecture.md), "Console device handover".

The `KERNEL_OWNED` sub-decision is the load-bearing invariant of the whole design:
**the buffered producer is
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

- **Producer** (`console_tx_write`, thread context): under `IrqLock`, compute free
  space, copy the burst into `[head, ...)`, publish the new `head` and enable the
  TX-empty IRQ ("prime the pump"). Returns immediately -- the caller's cost is a
  copy, not the transmission.
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
for the copy plus a pointer store and one bit -- microseconds bounded by the ring
size -- not the ~22 ms a 256-byte transmission used to hold. The copy is inside the
lock, not outside it: that also serialises concurrent *thread* producers, which the
SPSC argument alone would not cover.

**Overflow policy: stall-with-sync-drain, not drop.** If a burst does not fit, the
producer disables the TX IRQ, drains the ring and writes the burst by polling -- in
order, with other IRQs still serviced. RTT drops on a full ring (its host may be
detached, so a blocking writer would hang forever); the UART trades latency for data
whenever the wire is alive, because **losing kernel debug output is worse than a
bounded stall.** Both poll loops are nonetheless bounded by `DRAIN_POLL_CAP`: a
channel that never frees a slot makes `drain_sync` reset the ring and the burst loop
return mid-buffer, dropping silently and without a counter. That is the one case this
path does lose bytes, and it is the case where the wire is already dead. Ring
sizing is per-chip (>= `kprintf`'s 256 B buffer; e.g. 512 B on most parts, 2048 on the
ESP32-C6) and keeps this path rare.

## The synchronous path and the panic-safe seam

`arch_console_write_sync` is the bounded polled writer -- the one that is safe with
the scheduler and IRQs down. The seam is arranged so only the buffered backends
carry a distinct implementation:

- A **fallback TU** (`arch/common/arch_console_write_sync_default.cc`) forwards
  `arch_console_write_sync` -> `arch_console_write`. Every polled-only chip reuses its
  normal writer for free.
- Nearly every chip **defines its own** bounded polled writer (the fleet wraps its
  TX-ready poll in a spin-then-drop guard, so a wedged UART drops bytes instead of hanging
  the panic path); the sim defines a bounded one-byte-at-a-time `write(1, ...)` to host
  stdout. The fallback covers any chip that supplies no distinct sync writer.

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

## Capability handover

The kernel-side handover mechanism is **landed**. `console_tx_deinit` (flush -> disable
TX IRQ -> `irq_detach`/NVIC-mask -> disarm the ring, all under one `IrqLock`) relinquishes
the buffered path, and the privileged syscall `kos_console_publish` (29) calls it, takes a
kernel ref on the stdout endpoint, and flips `g_console_state` to `USER_OWNED` last. A
stale in-flight chip writer that raced the flip is drained via the `g_chip_writers` count
before publish returns (it lowers its own priority and yields so a lower-priority writer
can finish -- the scheduler is strict-priority). The panic path funnels through
`kpanic_enter`, which flips the UART to `RECLAIMED` and polled-prints.

**A DRIVER DEATH is the second route to `RECLAIMED`.** `cap_teardown` only NOTES it, when the
published endpoint's `recv_holders` reaches 0, and `console_on_driver_death` then asks the DEVICE:
it defers while any live domain still holds `arch_console_reclaim_window()`. A driver is a THREAD
GROUP, so the endpoint's last RECEIVER is the service thread rather than the thread holding the
registers -- reclaiming on the note alone reprogrammed the UART under a live IRQ thread and silenced
its own source. The note stays SET across a refusal and every `exit_current` and voluntary close
re-runs the check, so the LAST holder's own exit reclaims.

**The reclaim is unconditional-once, not handover-conditional.** `kpanic_enter` reclaims
whenever the state is not already `RECLAIMED`, and it stores `RECLAIMED` *before* calling
the body. Two properties follow. It runs exactly once, so a body that truncates the byte
in the shift register cannot cut the banner it just printed; and a synchronous fault
*inside* the body re-enters `kpanic_enter` and stops rather than recursing with the old
state -- a pre-existing recursion hazard that the widening closed. The safety of reclaiming
a device no driver ever touched rests entirely on every chip body being **idempotent
absolute stores**, which `arch.h` requires; every implementation was audited against
it (`xmc4800` `usic_uart.cc`, `mk64f` `chip_mk64f.cc`, `esp32c6` `chip_esp32c6.cc`,
`esp32` `chip_esp32.cc`, and the no-op fallback `arch/common/arch_console_reclaim_default.cc`).

Known artifact (XMC4800 ASC): if the crashed driver cleared `SCTR.PDL`, the TX pin is
held low across the fault, and reclaim's return to idle-high frames exactly one spurious
leading byte (~`0xC0`) before the panic banner. It is a physical UART line-recovery
transient (not lost/garbled output); the banner and fault dump that follow are byte-clean.

Still **not built**: a real `arch_console_reclaim` body on the chips that have none. Four
exist (`xmc4800` `usic_uart.cc`, `mk64f` `chip_mk64f.cc`, `esp32c6` `chip_esp32c6.cc`,
`esp32` `chip_esp32.cc`); every other chip keeps the no-op
fallback, so on those boards the reclaim is wiring with nothing behind it. The real bodies
are silicon-only -- no emulated board carries one -- and the `xmc4800` one is witnessed by
`conreclaim` (`c5d9b0d`). See
[architecture.md](architecture.md), "Object model, capabilities & IPC" ->
"Console device handover".

## The publisher's obligations

`kos_console_publish` hands the device away but cannot police what the publishing task does
next. Three properties of the published console are therefore contracts on the PUBLISHER, not
things the kernel enforces; the two console drivers (`system/driver/xmc4800/xmcuart`,
`system/driver/mk64f/k64uart`) implement all three in their `*_console_start` helper, and
`user/apps/common/initdemo` mirrors them.

**Publish and driver-spawn are ONE atomic act.** Nothing may be spawned that depends on the
console between the two. `kos_console_publish` returning has already flipped the state to
`USER_OWNED` and pointed `g_stdout_target` at the endpoint, so a driver spawn that FAILS after
it leaves the target naming an endpoint with no receiver. Every task spawned afterwards then
parks on its first `printf` probe forever: the `kos_send` blocks with no receiver to rendezvous
with, and no EPIPE fires while the publisher's own reference still holds, so the `_write`
fallback never gets the `-1` it needs to route around the dead console. The kernel cannot
cheaply tell "published" from "published and served", so the publisher must not spawn
console-dependent tasks when the driver spawn did not succeed.

**The publisher MUST drop its own WAIT-bearing capability immediately after the spawn.** The
dead-endpoint gate is keyed on `recv_holders` reaching **0**, and nothing else fires it.
`kos_endpoint_create` sets `recv_holders = 1` for the creator, and delegating a `CAP_WAIT` copy
to the driver at spawn bumps it to **2**, so at that instant the endpoint has two receiver
holders. If the publisher keeps its copy, the driver's death drops the count to 1, not 0: no
EPIPE is delivered, parked senders are never woken, and every client hangs for the life of the
system. That is strictly WORSE than a dark console, which is why the drop is a hard rule and
not a tidiness convention. Dropping it does not tear the endpoint down, because the kernel holds
its own `g_stdout_target` reference. A re-publish after a driver death therefore uses a FRESH
endpoint, the publisher again holding no receiver.

**There is a DARK WINDOW between the publish flip and the driver actually serving capability 0.**
`console_emit`'s chip path is already dropping (RTT still carries it) while no userspace receiver
exists yet. Output is not lost in it: a client `send` parks on `send_waiters` and the rendezvous
absorbs the gap, which is why there is no "handover in progress" state to size. What the window
does mean is that the console is dark on the wire for its duration, and that a failure inside it
is the case the atomicity rule above exists to forbid.

## The two protocols a published console endpoint carries

A published console endpoint carries **two** protocols on one capability, told apart by
whether the message arrived with a reply capability:

- a **plain send** is raw console bytes (this is the route `kos_console_publish` puts libc
  stdout on), and
- a **`kos_call`** is a `struct kos_uart_req` frame from `<kickos/sys/uart.h>`.

A driver must therefore `kos_recv` with a `struct kos_recv_info` and branch on
`info.reply_cap`. **A driver that drains the endpoint without separating them writes the
request frame to the wire as though it were text AND never replies, so the caller parks
forever on a reply that cannot come.** An unimplemented op is refused, never ignored:
refusing costs the caller an error, ignoring costs it the system.

| op | answer |
| --- | --- |
| `KOS_UART_WRITE` | queues bytes; `rsp.len` is the count ACCEPTED, which may be short, and the client retries the remainder |
| `KOS_UART_READ` | up to `req.len` bytes; `KOS_UART_F_BLOCK` is refused `-KOS_ENOSYS` rather than answered with 0 |
| `KOS_UART_STATS` | the driver's `struct kos_uart_stats` counters |
| `KOS_UART_SET_MODE` | the write policy below |
| `KOS_UART_CONFIGURE` | refused `-KOS_ENOSYS` where the device belongs to another thread |

### `KOS_UART_SET_MODE` and the write policy

`req.flags` carries `kos_uart_flags`. `KOS_UART_F_NONBLOCK` is `O_NONBLOCK` **for the
unframed console arm only**: clear, a write waits for ring room and does not give up; set, it
takes what fits and returns. An unknown bit is refused `-KOS_EINVAL` whole rather than masked,
so a flag this build does not know cannot read back as accepted, and nothing is stored on a
refusal.

Two refusals are contract rather than omission:

- **A service with no unframed console arm refuses with `-KOS_ENOSYS`.** A mode it stored
  would never be read, and the caller would believe byte loss was enabled while its writes
  still blocked.
- **A transport whose ring may have no consumer at all REQUIRES the flag and refuses
  `-KOS_ENOTSUP` on a request to clear it.** A blocking write there is unbounded: the USB CDC
  console issues no IN token until a host both enumerates the device and opens the tty, so a
  paced write would hang an un-cabled board at the first `printf` that fills the ring. A
  caller asking for back-pressure it cannot have is told so instead of being given a hang.

**Where the lost count is reported, and why it is not a return value.** The unframed arm is a
plain send, and a plain send is released the moment the receiver takes the message, in the
sender's own context; the ring accept happens afterwards in the driver's, so the accepted
count does not yet exist when the sender resumes. Loss on that path is reported through
`stats.tx_dropped`, read off the hot path with `KOS_UART_STATS`. **That supports pacing, not
retry**: a writer can see its own loss but not learn which bytes went missing. A caller needing
an exact per-call count with retry uses the framed `KOS_UART_WRITE`, which already reports a
short accept in `rsp.len`.
