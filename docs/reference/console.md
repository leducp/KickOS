<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The kernel console

> Reference (code-synced): how the console works *as built* and the invariants a
> change must not break. The how-&-why narrative belongs in `../book/`; this page is
> the exact contract.

## What it is, and what it is not

The kernel console is a **write-only debug facility**: the banner, `kprintf` /
`kputs`, panic and fault reports, and (via the raw `kos_kconsole_write` syscall) userspace's
fallback when no console is published.
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
   \- chip  (CONSOLE=chip|both): RAW bytes, the CRLF expansion happening at the device end
        v
console_emit                   -- THE ownership guard
   |   g_console_state ?                            (ownership axis, checked FIRST)
   +- USER_OWNED  -> DROP  (a userspace driver owns the UART; RTT still carries it; a
   |                         fault record goes to the driver, see "A fault record while a
   |                         driver owns the console")
   +- RECLAIMED   -> console_write_line_sync        (panic OR a driver death took it back)
   \- KERNEL_OWNED:
        |   panicking ?
        +- NO  -> arch_console_write        (the chip seam)
        \- YES -> console_write_line_sync   (polled under IrqLock, always safe)
        v
arch_console_write (per chip)
   +- console_tx_insert_line(buf, n, CRLF) != 0 -> the line is in the ring, return
   +- unarmed ring (pre-init)                   -> console_write_line_sync, no drain to race
   \- refused                                   -> the line does not go out; answer 0,
                                                   unless a fault record's: it makes room
                                                   (console_tx_insert_record_line)
        ... then the bytes leave ...
[drain ISR]        console_tx_isr:        on a backend with a TX interrupt
[producer drain]   drain_in_producer:     on a backend with none (irq_line < 0), by each
                                          producer until its own line has left
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

and only when `KERNEL_OWNED` does it hand the line to the chip seam:

```
!g_console_panicking  ->  arch_console_write       (the chip seam: insert, else locked write)
otherwise             ->  console_write_line_sync  (polled under IrqLock)
```

`arch_in_isr()` is NOT a condition here. It stood for "this context may not wait", which was
true of the burst producer the ring once had and is not a rule about buffering:
`console_tx_insert_line` never waits under its lock.

`g_console_state` starts `KERNEL_OWNED` (every board that never hands over stays here,
so the sub-decision is the whole story for them); `kos_console_publish` moves it to
`HANDING_OFF` from any state, drains the in-flight chip writers, and only then flips `USER_OWNED`; and a
panic flips it to `RECLAIMED` **from any prior state, `KERNEL_OWNED` included** -- the axis records a publish, never whether the device is garbled, and a
thread granted the console window can wreck the channel without ever publishing. See
[architecture.md](architecture.md), "Console device handover".

**Every line the ring accepted before a publish goes out before the driver has the UART.** The
move to `HANDING_OFF` gives up the ring under the same `IrqLock`, and giving it up flushes it
(`console_tx_deinit`): its bytes are in the device's transmitter before `USER_OWNED` is stored. A
line the ring refused was never accepted, so it is not among them: a kernel line is lost, and a
user writer waits for room and offers it again (below).

`HANDING_OFF` exists because the two halves of a publish cannot be one instant. It refuses
NEW chip writers while leaving the UART the kernel's, so a writer already counted in the
in-flight bracket can finish on a device it still owns. Flipping `USER_OWNED` in the same
masked region as the ring teardown instead makes an in-flight chunked writer resume, find
the device no longer the kernel's, and drop the rest of its message with nothing said.

The load-bearing invariant of the whole design is **line atomicity**: a reader parses lines,
so bytes from two producers inside one line destroy it while whole lines in any order stay
legible. `console_tx_insert_line` copies a whole line under one `IrqLock` or refuses it, and a
refused line DOES NOT GO OUT, so nothing writes at the device beside a running drain. A fault
record's refused line writes queued bytes itself, under the mask that holds every drain off (see
below). A panic still takes the polled path: the system stops afterwards, so a line left queued
is a line nobody reads.

**Across cores, the speaker is the device owner, not the CPU.** SMP cores in one kernel
insert normal lines into one ring; one drain sends its bytes. Own-image AMP kernels have
separate rings, so they cannot use them to arbitrate one UART. RP2350 and QEMU ARM64 instead
claim that UART in partition-shared state and keep the claim through the newline, including
user writes that reach the chip in several chunks. The wait for a peer's claim keeps
interrupts open; only the transmission is masked. The claim and wait are bounded: after a
stalled holder's deadline another node may speak. A write stops at the first input byte its
claim no longer covers and reports how many completed, so a user write's remainder is offered
again under a fresh claim. A newline spends CR and LF on the UART; if the claim dies after CR,
the thread remembers it and its next offer sends only LF, without repeating CR. A user write
with no complete input byte parks for a poll interval and offers the line again, and a task that
set O_NONBLOCK is answered `-KOS_ETIMEDOUT` instead. Panic, fault and ISR output, already masked
and with no caller to offer the rest again, waits the bounded claim masked and drops the rest
of a line it loses, uncounted. The holder rechecks its claim before each byte's store and
stops a margin short of the deadline: only a stall longer than that margin between check and
store writes into an expired grant.

**Only the ARM64 own-image claim renews.** There a KERNEL line (`arch_console_write` and the
polled writer) renews a grant that has ended, or reached its margin, while no peer has taken
it, so a host stall under QEMU does not cut a line no other node wanted the UART for. The
renewal is a compare-exchange of the one claim word, and a peer's steal is a compare-exchange
of the same word, so exactly one of them wins. User writes never renew: they keep the short
count and the fresh claim. A renewal can begin inside the margin, where no peer may take the
grant yet, so it is bounded: once renewing writers have put `KICKOS_DIAG_LINE_MAX` bytes under
one grant, the next ended grant expires like any other write's. Bytes a user write put under
the same grant do not count. The RP2350 never renews. Its owner and deadline are two words
beside the spinlock, so a renewal could not exclude a peer's steal already under way, and on
silicon only a debugger halt outlasts the hold mid-line.

A lease expiry can split a line around a peer's; no claim promises that a dying node
completes its output. The console is still a debug facility, not a reliable inter-node
message channel.

The ring is therefore MULTI-producer, and the exclusion is the lock rather than a structural
claim about callers. What the ring buys over the locked writer is the SHAPE of the masked
window: a copy instead of a transmission. That is why a ring too small to hold a line is a
silent regression rather than a visible one, and why `console_tx.cc` asserts the size.

## The buffered path (multi-producer ring)

The ring decouples a write from the UART bit rate. The polled alternative busy-waits
on the TX-ready flag -- telemetry measured that as the single largest on-CPU cost
(~879 us per burst at the K64F FEI clock). The buffered producer instead copies and
returns; the bytes leave later, in an interrupt.

- **Producer** (`console_tx_insert_line`, any context): under `IrqLock`, count the bytes the
  line needs once `\n` is expanded, refuse if they do not fit, else copy the line into
  `[head, ...)` expanding as it goes, publish the new `head` and enable the TX-empty IRQ
  ("prime the pump"). Returns immediately -- the caller's cost is a copy, not the
  transmission.
- **Consumer** (`console_tx_isr`, ISR context, bound to the chip's TX line via
  `irq_attach`): push ring bytes while a HW TX slot is free; when the ring drains
  to empty, disable its own TX IRQ. A chip whose console has no TX interrupt reports
  `irq_line < 0` and is drained instead by `drain_in_producer`, in the producer's own
  context, by every producer until its own line has left. Each byte is taken and pushed
  under the lock into a slot found free there, so producers drain one ring in order and none
  holds a byte across a preemption; only the wait for a slot runs with the lock open.

**Why the lock is brief, and why it is correct.** The drain ISR runs at
`PRIO_DEVICE` (0x30). `IrqLock` raises `BASEPRI` to 0x20, which masks everything
numerically >= 0x20 -- including the TX ISR (`arch/arm/armv7m/regs.h`). So the
producer's "publish head + enable IRQ" is atomic with respect to the ISR's "drain
to empty + disable IRQ": either the ISR's empty-check happened before the publish
(then the producer's enable re-arms it) or after (then the ISR sees the new bytes
and keeps draining). **No lost wakeup.** For an insert the lock is held
for the copy plus a pointer store and one bit -- microseconds bounded by the ring
size, not the ~22 ms a 256-byte transmission used to hold; a fault record's line is the one
insert that transmits under it. The copy is inside the
lock, not outside it: that also serialises concurrent *thread* producers, which the
single-producer argument alone would not cover: the ring takes a line from any
context, so its producers are many and only the lock orders them.

**Overflow policy: DROP, not stall and not a fallback write.** A line the ring cannot
take does not go out, and `console_tx_insert_line` answers zero so the caller knows.
The reason is that the alternative is unavailable rather than merely slower: the drain
is already a writer at the device, so a producer writing the refused line there itself
puts a SECOND writer on the wire and the two interleave mid-line. No locking discipline
around the fallback fixes that, because the interleaving is between the fallback and a
drain that is not holding the lock. A kernel line is lost to it; a user writer waits for
room instead (`kconsole_write_user`), as below.

**A fault record's line is the exception.** A thread-fault record (`kprintf_fault`) on the kernel's
own console is not dropped by a full ring: under the mask, the oldest queued bytes go out through
`arch_console_write_sync`, in ring order, until the line fits, and the line then queues
(`console_tx_insert_record_line`). The drain ISR and every producer are held off for that span, so
the device still has one writer and the lines queued first go out first and whole. The span is per
LINE, each line its own masked span: it sends at most the line's own length of queued bytes, CRs
included, in at most two runs, the second only where the ring wraps. On a live channel a line, at
most `KDIAG_FAULT_LINE_MAX - 1` characters, masks for at most twice that many bytes of wire time, the
time scaling with the console's baud. On a wedged channel it masks for at most one stall window of
the polled writer: once a run reports a stall, the rest of the room is taken unsent. On a backend
with no TX interrupt the drain that sends the rest runs after the mask is dropped. A record line is
still refused by a line wider than the ring, and by an insert, a record line or a producer drain's
push it interrupted. Each run is taken before it is written, so a synchronous fault in the polled writer whose
handler records again or flushes for a panic sends no byte twice; that flush loses the unsent rest
of the run.

**The one kernel caller that WAITS is the bench report** (`kprintf_paced`, in this file, under
`KICKOS_BENCH`). Forty phase rows at about 45 bytes each is roughly 156 ms of wire time at 115200,
so the ring fills a dozen rows in and the rest are refused; three silicon captures of one campaign
carried 6, 9 and 7 of 40. It offers a refused line again rather than losing it, waiting between
attempts with **interrupts open and no lock held** until the drain has taken at least one byte
(`console_tx_wait_progress`), which is what lets the drain ISR run at all. It gives up once a whole
attempt passes with nothing leaving the ring, so a dead or unread console costs one bounded poll
window per line and never a hang: a board that gets itself back through
`KICKOS_SHUTDOWN_TO_BOOTLOADER` must not need a button press because nobody had a terminal open.
Every multi-line printer in `kernel/bench/bench.cc` uses it, not the forty-row table alone: the ring
holds about eleven lines at the default 256-byte bound and about five on a board that lowers it,
which is inside the length of the shorter blocks.

The kernel console is a DEBUG facility, so a line lost to pressure is lost and nothing counts it.
The USER path is different: `kos_kconsole_write` splits a write into chunks, waits where the
console cannot take a chunk yet (below), and STOPS at the first byte the console refused and
returns how much landed, in the shape of `write(2)`: a short count where a peer node's claim ended
inside the write, or `-KOS_EBUSY` when a driver serves the console and the caller's own stdout send
would be taken now, so userspace sends there instead. Carrying on past a refusal would put a hole
in the middle of a line whose tail arrived, which is worse than losing the line.

**The one copy of the userspace policy is `stdout_write` (`<kickos/sys/emit.h>`)**: the TAP harness,
`kos_print` and libc's `_write` all write through it, so `kos_print` blocks where `write(1)` would
and, for a task that set O_NONBLOCK, stops where it would wait. `stdout_write` sends on capability
0, and a receiver taking fewer bytes than a chunk, none included, is offered the rest again there:
the rendezvous consumed the receive, so the next send parks until the driver receives again and
the retry never spins. Only a send no route serves hands the remainder to the kernel console:
`-KOS_EBADF` (capability 0 empty), `-KOS_EAGAIN` (no receiver serves the console) and
`-KOS_ECONNREFUSED` (its driver gone for good). `kconsole_write_all`, the entry for a writer that
starts at the kernel console, hands it back to `stdout_write` on `-KOS_EBUSY`.

**A refusal ends the write, as `write(2)` ends.** Any other answer of the send, and any refusal of
the kernel console but `-KOS_EBUSY`, stops `stdout_write` where it stands: it reports the bytes
that went and that error, and the bytes after them are not counted taken. That covers a buffer
the kernel refuses (`-KOS_EFAULT`) and a writer cancelled while it waited (`-KOS_ECANCELED`).
libc's `_write` returns the count where any byte went, else -1 with `errno` from the kernel's
error (`EFAULT`, `ECANCELED`, `EAGAIN` for a would-wait). `kos_print` returns nothing and does
what the one writer does.

**A full ring makes the user writer wait in the kernel, as a pipe's writer waits.** On a backend
with no TX interrupt the writer drains the ring itself, synchronously, sending the queued bytes
ahead of its line until the line fits and never more than the line needs, then its own line: a
writer above the one that queued those bytes does not wait for it to run. On a backend whose TX
interrupt drains the ring the writer sleeps on the console until that drain has freed the room its
line needs (`console_room_wait`, `console_tx_room_freed`), a publish and a reclaim waking it too. A
peer node's claim on a shared UART is waited out by parking a poll interval between offers
(`console_claim_wait`), since nothing marks the end of that claim. A TX channel that never drains
blocks the writer as a pipe nobody reads does. No bound gives a line up,
and output is not lost. A writer the console cannot serve at all waits in the kernel too, in the
dark window below, and the only way out of either wait is the writer's own kill or slay; a task
that set O_NONBLOCK is answered at once instead. A writer whose own task holds the console's
registers is not made to wait for a drain that task can stop: its line is lost to the kernel
console at once. The raw `kos_kconsole_write` is the same kernel write, and the one call that
drops where no route reaches the wire: it answers how much it took, and an app that calls it marks
the line as the measurement (`tests/static/check_kconsole_emit.sh`).

**A thread of the task that serves the console has no capability 0.** A send there would wait on
the very receiver that is sending, so neither the spawn nor a publish seats one in it, and a
publish empties the one a member already holds (`cap_console_serve`). Its `kos_print`, `printf`,
`std::terminate` and exception reports take the kernel console, which drops the line at once
while that task owns the UART: the write returns, and the line is lost to that route, RTT still
carrying it where the build has RTT. A driver that needs a line on the wire writes it through its
own device path.

### Non-blocking stdout

A client sets stdout or stderr non-blocking with `fcntl(fd, F_SETFL, O_NONBLOCK)`, and `F_GETFL`
reads the flag back (`user/src/newlib_fcntl.cc`); either answers -1 with `errno` where the kernel
refuses, `EINVAL` for a thread in no task. Blocking is the default. Fds 0 to 2 share one
description, the console, and its O_NONBLOCK is ONE flag per task, shared by all the task's
threads as a process's threads share an open file description, and no other task's: the kernel
keeps it in the task (`kos_task_nonblock`, `KOS_SYS_TASK_NONBLOCK`), so `fcntl` is a syscall and a
write is not. A thread spawned naming no task and no memory of its own joins its spawner's task,
so threads never placed in tasks share their spawner's flag. The kernel reads the flag on the
paths that would wait:

- A send of no timeout on the published console that would park, `kos_send` or `kos_send_timed`
  with `KOS_TIMEOUT_NONE`, is answered `-KOS_ETIMEDOUT` at once: a console whose task lives with no
  receiver taking a message now. A send with a finite timeout keeps it, `kos_console_flush`'s
  included, and a send on any other endpoint parks as before.
- A kernel console write that would wait or have to try again is answered `-KOS_ETIMEDOUT`: the
  dark window, a full ring, a peer node's claim on a shared UART.
- `kickos::stdout_write` stops at that answer and reports it with the bytes that went, so a write
  that would block writes nothing and returns -1 with `errno` `EAGAIN`, and the task carries on.
  A partial accept returns the short count: a receiver taking part of a message, or a kernel
  console taking part of a write.
- It never answers `EPIPE`. A console whose endpoint is gone for good falls back to the kernel
  console, which then owns the device, so nothing is ever unwritable for good.

RTT drops on a full ring for its own reason: its host may be detached, so a blocking
writer would hang forever. A channel that never frees a slot makes the producer drain
reset the ring, which is the one case bytes already queued are lost, and it is the case
where the wire is already dead. Ring sizing is DERIVED, never set. The knob is the line bound `KICKOS_DIAG_LINE_MAX`
(default 256 B, lowered per board where RAM is short), and `kickos/console_tx.h` computes
the ring as `2 * (KICKOS_DIAG_LINE_MAX - 1) + 1` rounded up to a power of two. What
`kernel/init/console_tx.cc` still asserts is the one relation no derivation covers: the
FAULT reporter's separate buffer (`KDIAG_FAULT_LINE_MAX`) must also fit, so it may not
exceed the ordinary bound. A ring one byte short would make every such line REFUSED and
quietly served by the locked synchronous writer, which still prints, so nothing a run
reports would move.

## The synchronous path and the panic-safe seam

`arch_console_write_sync` is the bounded polled writer -- the one that is safe with
the scheduler and IRQs down. The seam is arranged so only the buffered backends
carry a distinct implementation:

- Every chip **defines its own** bounded polled writer (the fleet wraps its TX-ready poll
  in a spin-then-drop guard, so a wedged UART drops bytes instead of hanging the panic
  path, and answers false once it stalled); the sim defines a bounded `write(1, ...)` to
  host stdout.
- On an own-image AMP node the writer drops the rest of a line whose claim it lost, up to
  its newline, across every call that carries the line.
- A **fallback TU** (`arch/common/arch_console_write_sync_default.cc`) forwards
  `arch_console_write_sync` -> `arch_console_write` and is extracted by nothing. It is kept
  as a trap marker, not as a service: `arch_console_write` enters the line insert, whose
  refusal path calls `console_write_line_sync` -> `arch_console_write_sync`, so a chip that
  resolved to it would recurse off its stack on the panic path.

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
implemented across the fleet (K64F, XMC4800, the STM32 F1/F3/F4 parts, RP2040, RP2350,
i.MX RT1062, SAM3X, RX72M, ESP32, ESP32-C6, and the sim); three illustrative ones:

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

A chip's TX IRQ-line number is a HW-confirm item (a wrong line never drains -- the ring
fills, every line after it is refused, and the console goes dark a few lines into the
boot, with no diagnostic naming the line). The buffered drain is **silicon-validated** on
the XMC4800, ESP32-C6, and K64F (a full selftest streamed in-order over the armed ring).
The **sim's backend** additionally exercises the full ring -- producer, publish+prime,
async drain ISR, wrap, and overflow -- in-tree under `ctest`, which is what a
sync-path-only run cannot cover.

## Boot ordering

The banner prints from `kmain` **before** `irq_init` -- the ring is not armed yet,
so it goes out on the sync path. `console_buffer_init` arms the ring immediately
after `irq_init`; steady-state logging from then on is buffered. So the ring speeds
ongoing logging, not the one-time banner. (If banner cost ever matters, bring-up
would have to be reordered so the console arms earlier.)

## The sim

The sim supplies a **fictional TX peripheral** (see Per-chip backend above), so it
arms the buffered path like a real chip: after `console_buffer_init`, steady-state
writes go through `console_tx_insert_line` into the ring and drain asynchronously in
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
- **The fault handler enters ISR context** (`isr_frame_enter` before `kickos_isr_fault`),
  which no longer changes the transport: the insert never waits, so a fault report is
  queued like any other line and `arch_shutdown`'s flush carries it.
- **`arch_shutdown` flushes synchronously** (`console_tx_flush_sync`) before
  `_exit`, because IRQs/signals are masked there and the `SIGUSR1` drain can no
  longer run -- otherwise the final `[ktrace] counters` line would be stranded in
  the ring.

## Invariants a change must not break

1. **One line is one indivisible insert.** `console_tx_insert_line` copies a whole line
   under one `IrqLock` or refuses it, and the refusal path (`console_write_line_sync`) holds
   `IrqLock` across the transmission. A path that splits a line across two inserts, or drops
   either lock, puts one producer's bytes inside another's line.
2. **The TX ISR is `IrqLock`-maskable.** It must sit in the device priority band
   (`PRIO_DEVICE`), or the producer's publish+prime is no longer atomic against it.
3. **The TX IRQ is enabled whenever the ring is non-empty.** The ISR disables it
   *only* on drain-to-empty; the producer *always* re-enables after publishing.
   It is stated of ONE ring over one gate bit, so **a chip whose kernels are several
   supplies no backend at all**: two private rings cannot serialize access to one UART.
   Own-image RP2350 and QEMU ARM64 instead write through a polled path and hold a
   partition claim across chunks of a line. RP2350 uses a hardware spinlock; ARM64 uses
   one atomic owner/deadline word in the shared window.
   **Ownership is bounded by a DEADLINE taken when the claim is, never by what the
   holder writes next**: a node that stops mid-line, or dies there, would otherwise
   keep the UART indefinitely. On ARM64 a kernel line may renew a grant no peer took,
   until renewing writers have put `KICKOS_DIAG_LINE_MAX` bytes under it, so the bound is
   that line's length past one hold. A peer may take an expired claim. A write stops at the
   first byte its claim does not cover and counts only what went out; it never
   deliberately writes into a peer's claim. An expired holder can still leave a split
   line on the wire.
4. **Panic output must not depend on the buffered path or on a debug probe.** It
   flushes the ring, forces the sync path, and -- once the eventual userspace driver
   owns the UART -- must *reclaim + reinit* the peripheral and polled-print,
   because RTT needs a J-Link and the userspace driver may be the thing that
   crashed. The diag LED is the always-present 1-bit last resort.
5. **While a driver owns the UART the kernel writes nothing at it**, a fault record included: the
   record goes to the driver, held whole and handed over between two of its receives. A kernel
   write at the device beside the driver's drain interleaves with it byte by byte, tearing the
   record into the driver's lines.

## Capability handover

The kernel-side handover mechanism is **landed**. `console_tx_deinit` (flush -> disable
TX IRQ -> `irq_detach`/NVIC-mask -> disarm the ring, all under one `IrqLock`) relinquishes
the buffered path, and the syscall `kos_console_publish` (29), gated on `AUTH_CONSOLE`,
calls it, takes a kernel ref on the stdout endpoint, and flips `g_console_state` to
`USER_OWNED` last. A
stale in-flight chip writer that raced the flip is drained via the `g_chip_writers` count
before publish returns (it lowers its own priority and yields so a lower-priority writer
can finish -- the scheduler is strict-priority). The panic path funnels through
`kpanic_enter`, which flips the UART to `RECLAIMED` and polled-prints.

**Who may publish, and what HANDOUT allows.** The caller holds `AUTH_CONSOLE` (`-KOS_EPERM`
otherwise) and names an endpoint through a capability holding HANDOUT: `-KOS_EBADF` for a bad,
stale or non-endpoint capability, `-KOS_EACCES` for one without HANDOUT, a WAIT-only capability
included. It also names the TASK the driver runs in, whose end is the driver's death:
`KOS_TASK_NONE` for the caller's own, or a task the caller created (`-KOS_EBADF` for a handle naming
nothing, `-KOS_EPERM` for another creator's, `-KOS_EBUSY` for one already ended). The init publishes
for the task it has just created for the driver. `-KOS_EBUSY` also answers a task a thread outside
of which holds the console's registers (below). The publish seats WAIT on that capability when it
does not hold it already, counted in `recv_holders`, and the endpoint receives as a fresh one does
for its creator. Every publish therefore leaves the publisher holding WAIT and HANDOUT, and a WAIT
it drops at the end of the handover. An endpoint's
creator holds all four rights, so a first publish goes through the creator's capability. HANDOUT
is also what lets the init, which keeps a console driver's endpoint narrowed to SIGNAL, TRANSFER
and HANDOUT between its instances, publish it again at every start: a restart repeats the first
handover step for step. `-KOS_EOVERFLOW` answers a reference or receiver count at its ceiling. A
refusal changes nothing: no right seated, no reference taken, the console untouched.
`cap_console_publish_through` in `kernel/syscall/cap.cc` is the rule.

**A DRIVER DEATH is the second route to `RECLAIMED`, and a driver is dead only when its task
dies.** The task the console was published for ending, or its slot going back before it ever ran,
NOTES the death (`task_end`, `cap_console_task_ended`); a kill or a slay of that task ends it at
once, before its members have run to their deaths (`task_stop`), so a slay that times out still
leaves the console ended, and `console_on_driver_death` then asks the
DEVICE: it defers while any live thread still holds `arch_console_reclaim_window()`. The task's end
slays every member, so a holder of that task is gone in finite time: the note stays SET across a
refusal and every `exit_current` re-runs the check, so the LAST holder's own exit reclaims. A
death noted while a publish is still handing the device over is acted on when the hand-off
completes (`console_owner_set_user`), so no path erases it; only the next publish retires it.

**The console's registers stay inside the task the console is published for.** A publish is
refused `-KOS_EBUSY` while a thread outside the task it names holds a device window over
`arch_console_reclaim_window()`, and from the publish to the reclaim a spawn giving such a window
to a thread of any other task is refused `-KOS_EBUSY`; a window is taken only at a spawn, so no
other path moves one. Every holder the reclaim waits for is therefore a member of the task whose
end it follows, which that end slays, so the reclaim always comes.

**A publish onto another endpoint moves stdout with it.** Every thread's capability 0 naming the
console it replaces is seated on the new endpoint, and a sender parked on the replaced one is
answered `-KOS_EAGAIN`: `kickos::stdout_write` offers the line to the kernel console, which hands
it to the new endpoint with `-KOS_EBUSY` once that one is served.

**From the task's end on, the console endpoint takes no send** (`EP_CONSOLE_ENDED`,
`endpoint_takes_sends` in `kernel/include/kickos/endpoint.h`), though a receiver of it may still
wait: its receivers are that task's members, slain but not yet gone, and a line handed to one dies
with it. Every sender parked there is answered at the end, as on an endpoint with no receiver, and
a new one at once, so its line goes to the kernel console's dark window instead. A held fault
record is handed to no receiver either. The next publish lifts it.

**The published endpoint losing its last receiver is no death.** A driver whose service thread exits
while its task lives is a live endpoint with no receiver: a send parks there, receiver or not, as a
write to a pipe does, and a receiver coming back takes it. Only the task's end answers the senders
parked on an endpoint nobody receives on, as any such endpoint answers them.

**The reclaim runs once.** A driver-death reclaim stores `RECLAIMED` before its body, and nothing
after it reprograms the device again: a fault record still open across it is written by it, never
by the record's own end. **The panic reclaim is unconditional-once, not handover-conditional.** `kpanic_enter` reclaims
whenever the state is not already `RECLAIMED`, and it stores `RECLAIMED` *before* calling
the body. Two properties follow. It runs exactly once, so a body that truncates the byte
in the shift register cannot cut the banner it just printed; and a synchronous fault
*inside* the body re-enters `kpanic_enter` and stops rather than recursing with the old
state -- a pre-existing recursion hazard that the widening closed. The safety of reclaiming
a device no driver ever touched rests entirely on every chip body being **idempotent
absolute stores**, which `arch.h` requires; every implementation in the tree was audited
against it, the no-op fallback `arch/common/arch_console_reclaim_default.cc` included.
`esp32c6` is the one body that is not pure straight-line: its `_SYNC` stores land only on
the closing `REG_UPDATE` write, so it first waits out the previous synchronisation. That
wait is bounded and read-only, so re-entry from a nested fault still repeats harmlessly.

Known artifact (XMC4800 ASC): if the crashed driver cleared `SCTR.PDL`, the TX pin is
held low across the fault, and reclaim's return to idle-high frames exactly one spurious
leading byte (~`0xC0`) before the panic banner. It is a physical UART line-recovery
transient (not lost/garbled output); the banner and fault dump that follow are byte-clean.

**On q35 the kernel writes COM1 under a holder.** `arch_shutdown` prints the exit line there, and
a panic its report, whatever task holds the 16550's ports, and the no-op reclaim below restores
nothing first: a holder that reprogrammed the device garbles both (`boards.md`, the
`qemu-x86_64` caveats).

Still **not built**: a real `arch_console_reclaim` body on the chips that have none, which
today are `an505`, `imx8mp`, `mps2`, `nrf51`, `q35`, `sam3x8e`, `stm32f103`, `stm32f302` and the
three `virt_*` machines. Those keep the no-op fallback, so on their boards the reclaim is wiring with nothing behind it. The chips
that do have one are what `grep -rn "^void arch_console_reclaim(void)" arch/` reports, minus
the declaration in `arch/include/kickos/arch/arch.h`. The real bodies
are silicon-only -- no emulated board carries one -- and the `xmc4800` one is witnessed by
`conreclaim` (`c5d9b0d`). See
[architecture.md](architecture.md), "Object model, capabilities & IPC" ->
"Console device handover".

## A fault record while a driver owns the console

A thread-fault record (`kickos_thread_fault_exit`, and the selftest trap witness) is the one
kernel print that still reaches the wire while a userspace driver owns the UART, and it reaches
it THROUGH THAT DRIVER: the kernel writes none of it at a device it handed away. Each
`kprintf_fault` line the reporter prints goes to the kernel console as before; from `HANDING_OFF`
on, the chip path refuses it and the same masked read holds it in the record of the thread
printing it instead, opening that record on its first such line. `krecord_end` ends the record.

- **Whole.** A record is held in the disarmed ring's storage, idle from the publish on, and a
  reader sees it only once `krecord_end` commits it, at most `KDIAG_FAULT_RECORD_MAX` bytes. A line
  the room left cannot take ends the record there, so it never carries a cut line. Records of
  several threads written at once are each held, keyed by the thread, and read in the order they
  were committed. A record begun while the kernel still owned the console, with a publish landing
  inside it, keeps its tail: the lines before the publish went to the wire, the rest are held. A
  record with no room left at all is not held, and neither is any record on a console with no
  ring, an own-image AMP node or a chip that supplies no buffered backend; such a record reaches
  RTT where the build carries RTT, and otherwise nothing. A thread slain inside its record never
  ends it, and the slay drops it; one faulting again inside its own record adds to it.
- **Between two receives, never inside one send.** `krecord_end` commits the record and hands it
  to a receiver already parked on the published endpoint (`cap_console_deliver`). A driver that
  is not parked, mid-write in its own drain, takes it at its next receive, ahead of every queued
  sender, and a record wider than its buffer arrives over consecutive receives with nothing in
  between. The record therefore lands between two messages of the driver's input, and a line one
  send carries is never split by it. A line a writer splits across sends can be; the banner opens
  with a newline of its own, so a record never shares a line with user bytes.
- **The faulting thread never parks**, and nothing waits on time.
- **Each line reaches the wire once.** The reclaim flips the state, writes every held record and
  empties the store under the lock the hold takes, so a line printed before it is held and
  written by it, and a line printed after it reaches the device directly: a record the console's
  death lands inside is written in two parts, with other lines possibly between them, and never
  twice. The write moves nothing in the store: the committed records, the first from the line
  after the one its reader was cut in, then each open record's lines, records in the order they
  opened.
- **What still loses it.** A held record a driver never received is lost when the system ends,
  since `kickos_terminate` may not write a device the driver owns, and one the driver received is
  lost with the rest of its ring. A driver death that reclaims the device writes what the driver
  never took, polled (`console_on_driver_death`), and so does a panic (`kpanic_enter`); where the
  driver had taken part of a record, that write starts at the next line of it, so the line the
  driver was cut in is lost. The panic's write takes the lock the store is changed under, so
  another core's change finishes first; a panic raised on the core changing the store, by a fault
  inside the change, writes nothing held. Every walk of the store stops at an entry that does not
  fit below its end, so a torn one is never followed off the ring, and the panic's own output is
  never cut.
- **Bounded by the ring.** Every step on the store runs under the lock, over the ring's storage,
  `KICKOS_CONSOLE_TX_SIZE` bytes, which the board sets through `KICKOS_DIAG_LINE_MAX` and no task
  can grow. A held line walks the open area at most once per open record, plus three; a commit
  makes at most L + 6 passes over it, L the lines of that record; the reclaim's write walks it
  once per open record and then transmits what is held, polled.
- **A driver whose buffer refuses the copy is answered `-KOS_EFAULT`**, and the record is taken
  all the same, or every receive into that buffer would refuse it again.

## The publisher's obligations

`kos_console_publish` hands the device away but cannot police what the publishing task does
next. Three properties of the published console are therefore contracts on the PUBLISHER, not
things the kernel enforces. No driver honours them in its own `*_console_start`: nine of the
eleven are a one-line delegation to `drv::bring_up`, `simcon_console_start`
(`system/driver/sim/simcon`) is one too but under its two window-thread knobs, and the eleventh,
`rtusb_console_start` (`system/driver/imxrt1062/rt1062usb`), wraps that same call in a
failure-path print of the IRQ thread's stage. All three obligations live in `drv::bring_up`, in `user/src/driver_service.cc`.
`user/apps/common/initdemo` open-codes the same sequence instead, being the handover demo
rather than a driver.

**Publish and driver-spawn are ONE atomic act.** Nothing may be spawned that depends on the
console between the two. `kos_console_publish` returning has already flipped the state to
`USER_OWNED` and pointed `g_stdout_target` at the endpoint, so a driver spawn that FAILS after
it leaves the target naming an endpoint with no receiver while the task it was published for
lives. Every task spawned afterwards then parks on its first `printf` forever, as on any live
console with no receiver, and the `_write` fallback never gets the `-1` it needs to route around
it. The kernel cannot tell "published" from "published and served", so the publisher must not
spawn console-dependent tasks when the driver spawn did not succeed, and a failed start ends the
driver's task before it says why (`console_start_failed`), that end being the console's death.

**The publisher MUST drop its own WAIT right when the handover ends.** Once the driver's task has
ended, a sender is answered only by an endpoint nobody holds WAIT on, `recv_holders` at **0**. The
publish takes a capability holding HANDOUT and leaves it holding WAIT too, so the drop is owed
after every publish. `kos_endpoint_create` sets `recv_holders = 1` for the creator, a publish
through a capability without WAIT seats the same one, and delegating a `CAP_WAIT` copy to the
driver at spawn bumps it to **2**. If the publisher keeps its WAIT, the endpoint still receives
after the driver's task ends: no answer is delivered, parked senders are never woken, and every
client hangs for the life of the system. That is strictly WORSE than a dark console, which is why
the drop is a hard rule and not a tidiness convention.
`console_handover_finish` narrows it to SIGNAL, TRANSFER and HANDOUT (`HANDOVER_KEPT`), the rights
the init's walk (`system/init/compose/walk.cc`) keeps of every endpoint it creates for a server.
Its task's end then gives the kernel its console back, and with no receiver left a send answers
`-KOS_EAGAIN` while the init's HANDOUT remains, until the endpoint next receives
(`endpoint_unserved` in `kernel/include/kickos/endpoint.h`). The endpoint outlives the instances
of its driver: a restart publishes the SAME endpoint again through that HANDOUT, which seats the
init's WAIT and hands the console over, then seats the new driver on it and narrows, as the
first start did. A send answers `-KOS_ECONNREFUSED` only once the init drops the endpoint, its
driver out of restarts. Neither drop tears the endpoint down, because the kernel holds its own
`g_stdout_target` reference.

**Two windows are dark on the wire, and output is lost in neither.** The first runs from the
publish flip to the driver serving capability 0: `console_emit`'s chip path already drops (RTT
still carries it) while no receiver exists yet, and a client `send` parks on `send_waiters`
(bounded when timed), the console being live, so the rendezvous absorbs the gap and there is no
"handover in progress" state to size. A restart's window is the same.

The second, THE DARK WINDOW, runs from the driver's task ending to the reclaim, which waits for
the thread still holding the device, or to a restart's publish. The kernel may not touch the
device in it either. A send there is answered, `-KOS_EAGAIN` while the init's HANDOUT remains, and
the writer offers the chunk to the kernel console, which WAITS, with no bound, on a console-state
wait that the reclaim and a publish both wake (`console_dark_wait`, `console_dark_wake`), then
writes it: polled after the reclaim, or handed back on `-KOS_EBUSY` after a publish, which
`kickos::stdout_write` (`<kickos/sys/emit.h>`) then sends to the endpoint. The wait ends otherwise
only on the writer's own kill or slay. A task that set O_NONBLOCK is answered `-KOS_ETIMEDOUT`
there instead. The init's report of a console driver's failed start (`kickos::driver::report`)
is such a writer: where a slain member of the ended task still holds the console's window, the
report waits for that member to exit and the reclaim to write it. The line lands exactly once: on the wire, and on RTT where the build also carries
it, a writer offering a line again putting on RTT only the bytes the chip took. So RTT shows a
line waiting in the window only once the reclaim writes it, and never one that leaves through
`-KOS_EBUSY` for the endpoint. What the window
does mean is that the console is dark on the wire for its duration, and that a failure inside it
is the case the atomicity rule above exists to forbid.

## The two protocols a published console endpoint carries

A published console endpoint carries **two** protocols on one capability, told apart by
whether the message arrived with a reply capability:

- a **plain send** is raw console bytes (this is the route `kos_console_publish` puts libc
  stdout on), and
- a **`kos_call`** is a `struct kos_uart_req` frame from `<kickos/sys/uart.h>`.

A driver must therefore receive with `kos_reply_recv` asking for a `struct kos_recv_info`,
and branch on `info.reply_cap`. **A driver that drains the endpoint without separating them
writes the request frame to the wire as though it were text AND never replies, so the caller
parks forever on a reply that cannot come.** An unimplemented op is refused, never ignored:
refusing costs the caller an error, ignoring costs it the system.

| op | answer |
| --- | --- |
| `KOS_UART_WRITE` | queues bytes; `rsp.len` is the count ACCEPTED, which may be short, and the client retries the remainder |
| `KOS_UART_READ` | up to `req.len` bytes; `KOS_UART_F_BLOCK` is refused `-KOS_ENOSYS` rather than answered with 0 |
| `KOS_UART_STATS` | the driver's `struct kos_uart_stats` counters |
| `KOS_UART_SET_MODE` | the write policy below |
| `KOS_UART_CONFIGURE` | refused `-KOS_ENOSYS` where the device belongs to another thread |

**A zero-length plain send is a flush.** The driver goes back to its receive only once its TX
ring and the device's transmit path have drained, so a sender knows its earlier bytes have left
when its next send on the endpoint is taken. `kos_console_flush(timeout_us)` (`<kickos/sys.h>`)
is those two zero-length sends on a thread's stdout, each bounded by the timeout, and the init
ends the system on exactly that ([design-m10-target.md](../design-m10-target.md), section 1.5).
Bytes stdio still buffers were never sent, so an app flushing its output calls
`fflush(stdout)` first. Drained is as
strong as the device allows: the last stop bit on every UART, and the host's acknowledgement of
the last bulk IN packet on USB CDC, where a transfer ending on a full packet is closed by a
zero-length one. The wait is bounded inside the driver, so a device that never drains costs that bound and
never the endpoint. In a two-thread driver only the IRQ thread may touch the device, so the
service thread drains the ring, then asks the IRQ thread for the device's half through the shared
block and waits for its answer under the same bound.

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
