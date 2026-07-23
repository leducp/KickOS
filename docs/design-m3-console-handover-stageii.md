<!-- SPDX-License-Identifier: CECILL-C -->
# Design note: M3 #4 stage (ii) -- console device handover + panic-path reclaim

**Authoritative implementation spec (fable review folded in, rulings resolved). Not a spike.** Stage (i)
(the endpoint object + `send`/`recv`/`create`) has LANDED: syscalls 26/27/28,
`kernel/include/kickos/endpoint.h`, root-only caps, `recv_holders` dead-gate + EPIPE.
Its how/why now lives in `docs/book/endpoints-synchronous-ipc-by-rendezvous.md`. This
note designs what sits ON TOP: relinquishing the UART to a userspace driver, routing
kernel output around the relinquished device, reclaiming it in a panic, and the driver
itself. It builds ONLY on the landed endpoint contract -- it does not reopen it.

Code truth consulted (do not re-survey): `kernel/init/console.cc` (`console_emit` :50-60,
`g_console_panicking` :40, `kpanic_enter` :137-142, `kickos_isr_fault` :208-225),
`kernel/init/console_tx.cc` (`g_tx` :30-45, `console_tx_flush_sync`), `lib/include/kickos/console_tx.h`
(the 4-fn backend + `arch_console_tx_backend`), `arch/include/kickos/arch/arch.h:277-278`
(`arch_console_write`/`_sync`), `kernel/irq/irq.cc:100-109` (`irq_detach`),
`arch/arm/chip/xmc4800/usic_uart.cc:148-205` (`kickos_xmc_usic_init`/`_write`),
`arch/arm/chip/mk64f/chip_mk64f.cc:298-316` (`uart0_init`, file-local),
`user/include/kickos/sys/abi.h` (syscall enum, ends at 28; `kos_thread_params` mmio grant),
`user/driver/k64dspi/` (the unprivileged-driver precedent), `user/src/newlib_stubs.cc:19-26`
(`_write`), `kernel/syscall/cap.cc` (`cap_install_defaults` installs nothing today;
`cap_install` free-slot scan :341-355 -- CHANGED by B3 to start at index 1),
`kernel/syscall/syscall.cc:304-318` (`endpoint_recv` writable-arg checks, Q8).

---

## 1. Concept: two axes, not one flag more

The console today decides buffered-vs-sync from three flags at one choke point
(`console_emit`): `console_tx_armed()`, `arch_in_isr()`, `g_console_panicking`. Handover
adds an ORTHOGONAL question those flags cannot answer: *who owns the UART TX register*.
Ownership is a three-value axis -- the kernel owns it, a userspace driver owns it, or
the panic path has forcibly reclaimed it -- and the routing decision must consult it
BEFORE the buffered-vs-sync sub-decision, because in the middle value the kernel must
touch the device on NO path at all.

Introduce one state variable next to `g_console_panicking`:

```
// kernel/init/console.cc, file-local
enum class ConsoleState : uint8_t
{
    KERNEL_OWNED,  // boot default; kernel drives the UART (buffered ring or polled)
    USER_OWNED,    // a userspace driver owns the UART; kernel chip path DROPS
    RECLAIMED      // panic forcibly took the UART back; polled-only
};
static volatile ConsoleState g_console_state = ConsoleState::KERNEL_OWNED;
```

`g_console_panicking` stays and keeps its exact current meaning, but it is now only
load-bearing in `KERNEL_OWNED` (a panic on a board that never handed over). `RECLAIMED`
subsumes it for the handed-over case.

RTT is untouched by all of this. `kconsole_write` fans out to RTT under its own IrqLock
BEFORE it ever reaches `console_emit` (console.cc :70-75), so RTT keeps working in every
state -- it is a separate transport, not a UART tee. Only the *chip* path is gated below.

## 2. Decisions

### D1 -- new `console_emit` logic (the routing choke point)

```
static void console_emit(char const* buf, size_t n)
{
    switch (g_console_state)
    {
    case ConsoleState::KERNEL_OWNED:
        // unchanged from today
        if (console_tx_armed() != 0 and arch_in_isr() == 0 and not g_console_panicking)
        {
            arch_console_write(buf, n);       // buffered ring
        }
        else
        {
            arch_console_write_sync(buf, n);  // polled
        }
        return;
    case ConsoleState::USER_OWNED:
        return;                               // DROP: the driver owns the UART
    case ConsoleState::RECLAIMED:
        arch_console_write_sync(buf, n);      // panic reclaimed it -> polled only
        return;
    }
}
```

**Why a distinct `USER_OWNED` that DROPS, and not just `armed = false`.** The tempting
minimal handover is "clear `g_tx.armed` and let the existing guard fall through." It is
WRONG: with `armed == false` the `KERNEL_OWNED` branch takes the `else` and calls
`arch_console_write_sync`, which POLL-WRITES the TX data register -- the exact register
the userspace driver now owns. That is the two-drivers-one-device hazard the architecture
rejects, reintroduced silently on the steady-state kprintf path. The kernel must not merely
stop *buffering*; it must stop *touching the device*. Only a separate state that returns
without any `arch_console_write*` call achieves that. `armed == false` remains NECESSARY
(so a stray `KERNEL_OWNED` producer cannot enqueue), but it is not SUFFICIENT.

**Stale-routing race + the in-flight chip-writer count (REQUIRED, B1).** `console_emit`
reads `g_console_state` and then, on the `KERNEL_OWNED` branch, pokes the device. A writer
that sampled `KERNEL_OWNED` (or is mid-polled-write inside `arch_console_write_sync`) BEFORE
`kos_console_publish` flips the state can still poke the UART AFTER the flip -- two drivers,
one device, the exact hazard `USER_OWNED` exists to prevent. The read-then-flip window is
not closable by the state check alone. Two mechanisms close it, both load-bearing:

- **Re-check inside `console_tx_write`'s not-armed branch.** The buffered producer's
  fall-through (`console_tx.cc:109-113`, `armed == false` -> `arch_console_write_sync`) must
  re-read `g_console_state` and DROP the chip write unless it is `KERNEL_OWNED`. Without this
  a producer that raced past the arm check pokes the driver's UART on the disarm path.

- **An in-flight chip-writer count.** A file-local `static volatile int g_chip_writers`.
  Every path that is about to poke the device -- both `console_emit` device branches and
  `console_tx`'s drain -- increments it before the poke and decrements it after (increment
  under the same read that decided to write, so a writer that decided on stale
  `KERNEL_OWNED` is counted). `kos_console_publish` (D3), AFTER `console_tx_deinit` returns
  and the state is flipped, spins with the IrqLock RELEASED until `g_chip_writers == 0`
  before it returns. Root cannot spawn the driver until publish returns, so the preempted
  stale writer has drained off the device before any userspace driver touches it. The spin
  is bounded (a poke is a handful of polled bytes at worst) and single-core-safe: masked
  producers cannot start a new poke, and the in-flight one only decrements.

### D2 -- `console_tx_deinit`: the relinquish sequence

Does not exist today (console.md's "Future" names it). Add to `console_tx.h`/`.cc`:

```
void console_tx_deinit(void);   // console_tx.h
```

```
// console_tx.cc -- one IrqLock; flush_sync's own IrqLock nests harmlessly.
void console_tx_deinit(void)
{
    if (not g_tx.armed) return;       // idempotent + guards a null backend on polled-only
                                      // chips (mps2/virt/nrf51 never arm) -- supplies D3's
                                      // idempotence directly (S2)
    IrqLock lock;
    console_tx_flush_sync();          // a. drain queued bytes on a still-kernel-owned UART
    // a'. XMC only (optional): tx_wait_idle() -- flush_sync waits for a free TX SLOT, not a
    //     drained SHIFTER; deinit-then-handover while the last byte is still shifting emits
    //     one garbled char. A bounded wait on the shifter-idle flag before disabling closes
    //     the seam on the XMC backend; harmless elsewhere.
    g_tx.backend->irq_disable();      // b. stop the TX-empty IRQ AT THE PERIPHERAL
    irq_detach(g_tx.irq_line);        // c. null the handler + NVIC-mask the line
    g_tx.armed = false;               // d. disarm the ring (producer stops buffering)
}
```

The caller (`kos_console_publish`, D3) flips `g_console_state = USER_OWNED` **LAST**, after
this returns. Store the TX line: `console_tx_init` already receives `irq_line` from
`arch_console_tx_backend`; stash it in `g_tx.irq_line` (new field) instead of dropping it.

**Monotonic-safety against a fault mid-deinit.** The four steps run under one IrqLock, so
they are atomic against every buffered producer and the drain ISR (all IrqLock). But a
*synchronous* fault (kernel bug / MPU) is not maskable and can land between any two steps.
It is safe at every interior point because the driver DOES NOT EXIST YET (publish precedes
spawn -- D6): at every point the UART is still kernel-inited and reachable, and
`g_console_state` is still `KERNEL_OWNED`, so `kickos_isr_fault`'s existing `flush_sync`
+ polled-write path works verbatim. Even after step d (`armed == false`) but before the
state flip, `KERNEL_OWNED` routes to the polled writer, which pokes a UART that is still
the kernel's -- fine. Flipping the state LAST is the whole trick: a fault before the flip
panics on a kernel-owned UART; a fault after it takes the reclaim branch (D6), which is
harmless-idempotent even though no driver ever touched the device.

### D3 -- `kos_console_publish(cap)`: new privileged syscall 29

```
KOS_SYS_console_publish = 29,   // (endpoint_cap) -> 0, or -1 (bad cap / not privileged)
```

```
kos_console_publish(cap):
    if not current->privileged:                       return -1   // privileged-only
    e = cap_resolve(current, cap, CAP_ENDPOINT, 0)                // any rights; identity only
    if e == nullptr:                                  return -1
    handle = e->obj                                               // GLOBAL gen-encoded endpoint handle
    {
        IrqLock lock
        if g_console_state == ConsoleState::KERNEL_OWNED:
            console_tx_deinit()                                   // D2 (idempotent)
        obj_ref_inc(CAP_ENDPOINT, handle, 0)                      // kernel stdout-target ref (S3)
        if g_stdout_target >= 0:
            endpoint_ref_drop(g_stdout_target)                    // drop the old target (re-publish)
        g_stdout_target = handle
        g_console_state = ConsoleState::USER_OWNED               // LAST
    }
    // B1: drain any stale chip writer that raced past the pre-flip state read, WITH THE
    // IRQLOCK RELEASED, before we return. Root spawns the driver only after we return,
    // so this closes the preempted-writer window against the userspace driver.
    while (g_chip_writers != 0) { arch_relax() }
    return 0
```

`g_stdout_target` (file-local `int`, init `-1`) holds the GLOBAL, gen-encoded endpoint
handle (NOT the pool slot index) -- it is what `cap_install_defaults` seats into every
child (D4); the pool slot index is derived from it only where `endpoint_refs[]` must be
indexed. The kernel ref on it goes through `obj_ref_inc` / `endpoint_ref_drop` (cap.cc),
NEVER raw `endpoint_refs[]` arithmetic (S3): raw bumps bypass the free-at-zero teardown and
the receiver-waiter guard that those helpers enforce. Kernel holds this one refcount so the
object survives even if root closes its own cap before spawning the driver; the ref moves on
re-publish and is released by an eventual unpublish (out of scope -- boards do not
un-hand-over today).

**Re-callable (re-publish after a driver respawn).** On the second call `g_console_state`
is already `USER_OWNED`, so `console_tx_deinit` is skipped (steps b/c/d already done, the
ring already disarmed). It just restashes the new endpoint and re-points `g_stdout_target`.
This is the mechanism D8 relies on.

Privileged-only because it disables a live IRQ line and mutates global console routing --
a capability a random unprivileged task must not hold. Only root (or a delegated privileged
supervisor) publishes.

### D4 -- `cap_install_defaults`: index 0 becomes the stdout cap

Today installs nothing; index 0 is now a REAL reservation (see the index-0 note below), so
it is the kernel-seated stdout slot exclusively.

```
cap_install_defaults(child):
    if g_stdout_target < 0:                    return   // pre-publish: nothing (unchanged)
    cap_install_at(child, /*index=*/0,
                   { obj = g_stdout_target, type = CAP_ENDPOINT, rights = CAP_SIGNAL })
    obj_ref_inc(CAP_ENDPOINT, g_stdout_target, CAP_SIGNAL)
```

Send-only: `CAP_SIGNAL` alone -- no `CAP_WAIT` (a client cannot recv-steal console traffic)
and no `CAP_TRANSFER` (cannot delegate it onward). Because the seated right lacks
`CAP_WAIT`, `obj_ref_inc`'s endpoint arm bumps `endpoint_refs` but NOT `recv_holders` --
which is correct: a client is not a receiver, and must not keep the dead-endpoint gate
open. The ref is dropped by the child's own `cap_teardown` at exit, exactly like any other
seated/delegated cap. Before publish, defaults still install nothing -- unchanged behavior,
so the selftest/bring-up world (which never publishes) is untouched.

Apps spawned BEFORE publish get an empty index 0 and fall back to `kconsole_write` (D5)
whose chip path is dark once `USER_OWNED`. Root must publish before spawning any app that
should print through the driver; this becomes a stated root-task rule (D6). REJECT the
alternative of late-seating index 0 into already-live tables: mutating another task's table
from a syscall breaks the "your table changes at spawn or by your own hand" model. (With the
index-0 reservation now real, a collision with the task's own create is no longer the
concern -- own-creates never land at 0 -- but the table-mutation objection stands alone.)

**Index-0 reservation is now REAL (B3, CHANGES landed stage-(i) cap.cc).** For index 0 to
be a dependable stdout slot, a task's own `sem`/`endpoint`/`mutex` create must never occupy
it. Change `cap_install`'s free-slot scan (cap.cc:341-355) to start at index 1, not 0:

```
// cap_install: index 0 is the kernel stdout slot; own-creates start their scan at 1.
for (int i = 1; i < KICKOS_MAX_HANDLES; i++) { ... }
```

Index 0 is then filled ONLY by `cap_install_at(child, 0, ...)` from `cap_install_defaults`.
Consequences to carry:
- This CHANGES landed stage-(i) `cap.cc` behavior -- it needs re-validation on the ii-a
  silicon run (the cap selftests and any test that assumed a first create at index 0).
- It costs one handle slot per table (own-caps now live in `[1 .. MAX-1]`).
- `KICKOS_MAX_HANDLES` default is 8; the tiny-board floor is 6 (`bluepill-c8`, `stm32f103`,
  `stm32f302`, `nrf51` -- all polled-only, none publish a handover). If the stress soak needs
  8 concurrent OWN caps it now needs `MAX_HANDLES >= 9` (8 own + slot 0); on the default-8
  boards that requires a bump, and the tiny boards cannot host that soak at all. Confirm the
  soak's real concurrent-cap count against `MAX_HANDLES - 1` before the ii-a run.

### D5 -- `_write` migration (libc-side, kernel stays simple)

`user/src/newlib_stubs.cc`. No persistent state -- each `_write` re-classifies per
invocation against the CALLING thread's own cap index 0:

```
_write(fd, buf, len):
    size_t sent = 0
    while sent < len:
        size_t chunk = min(len - sent, KOS_EP_MSG_MAX)
        int r = kos_send(0, buf + sent, chunk)           // index 0 == KOS_CAP_STDOUT
        if r < 0:                                        // pre-handover, or driver died (EPIPE)
            return kos_kconsole_write(buf + sent, len - sent)  // resend ONLY the REMAINDER (S5)
        sent += r
    return len
```

`KOS_SYS_kconsole_write` keeps its exact semantics; its chip path simply DROPS in
`USER_OWNED` per D1 while RTT still carries it. Two rules are load-bearing:
- **Per-invocation re-probe.** There is NO process-wide sticky state: every `_write`
  attempts `kos_send(0)` and, only on failure, falls back for THAT write. cap 0 is
  per-thread and fixed at spawn, so the classification is stable within a thread without
  caching it -- and, crucially, a pre-publish thread's failure (its index 0 empty) never
  poisons a post-publish thread whose index 0 IS seated. The cost is one wasted syscall per
  write for a permanently-dark client, not one total; that is the deliberate trade for
  correct per-thread routing. Re-published targets are still picked up only by freshly
  spawned children (their index 0 names the dead endpoint -- D8 limitation, called out there).
- **Remainder-only fallback.** The fallback resends `buf + sent .. len`, never the whole
  `buf` -- otherwise the chunks already delivered to the driver are duplicated on RTT when a
  mid-stream `-1` hits.

### D6 -- `arch_console_reclaim()`: new arch seam + `kpanic_enter` growth

```
// arch/include/kickos/arch/arch.h, next to arch_console_write_sync
void arch_console_reclaim(void);
```

```
// kernel/init/console.cc -- weak no-op default (boards that never hand over)
extern "C" __attribute__((weak)) void arch_console_reclaim(void) {}
```

```
kpanic_enter():                           // idempotent (nested faults re-enter it)
    arch_irq_save()                       // today; never restored
    if g_console_state == ConsoleState::USER_OWNED:
        arch_console_reclaim()            // force the UART back to polled-ready (idempotent)
        g_console_state = ConsoleState::RECLAIMED
    g_console_panicking = true            // today
    console_tx_flush_sync()               // today; no-op when the ring is disarmed
```

**`kickos_isr_fault` must funnel through `kpanic_enter` (B2, REQUIRED).** Today
`kickos_isr_fault` (console.cc:208-225) does its own `flush_sync` + polled banner and halts,
but it NEVER calls `kpanic_enter`. In `USER_OWNED` that means the terminal MPU-fault path
prints to a UART the kernel no longer owns -- and the single most likely post-handover
faulter IS the userspace console driver itself -- so the fault report is dropped on a dead
chip path and the system halts silently. Fix: `kickos_isr_fault` calls `kpanic_enter` at its
TOP. `kpanic_enter` is idempotent and subsumes the flush, so its existing terminal behavior
is preserved; the difference is that the terminal MPU fault now reclaims the UART (D6) and
polled-prints the banner. **Dependency to record for M2 kill-and-resume fault policy:** only
a TERMINAL fault exit may reclaim. When a resume/kill-one-thread fault path lands, it must
NOT reclaim -- the driver keeps the device, and a dark report on the resume path is then the
CORRECT behavior. Gate reclaim on "this fault terminates the system," not on "a fault
happened."

Reclaim runs BEFORE the flush (the flush is a no-op post-handover anyway -- the ring is
disarmed). It must be **idempotent and re-entrant from any partial state** (nit): a nested
fault mid-reclaim re-enters `kpanic_enter` with `g_console_state` still `USER_OWNED` and must
re-run reclaim cleanly. Write reclaim as straight-line register STORES with no
read-modify-write on driver-touched registers -- absolute writes are safe to repeat; an RMW
on a garbled register is not.

`arch_console_reclaim` must assume **every register inside the driver's granted window is
garbage** (userspace mis-config: wrong baud, DMA armed, FIFO thresholds moved, mode changed,
clock gated) and re-establish a known polled-ready channel. It MAY trust everything that
stayed OUTSIDE the window -- pin mux, SCU/SIM clock gates, clock tree -- because the MMIO
grant is exact-window and those live in privileged-only peripherals per the grant-window
discipline; the driver could never reach them. Reclaim ADDS nothing to kernel access: the
chip backend code never left the kernel image, and an MMIO grant never removes the kernel's
privileged background access, so the polled writer can always reach the registers.

**Single-core race argument.** Once `arch_irq_save` masks, no user thread -- the driver
included -- ever runs again on this core, so nothing can concurrently poke the UART during
or after reclaim. **M4/SMP caveat (record now, solve later):** under AMP/SMP another core's
driver must be stopped or fenced before reclaim, or it races the polled panic writer. Out
of scope for single-core M3; flagged for the multicore design.

**Reclaim depth = rewrite EVERY writable register inside the window, NOT "re-run init"
(S1).** `*_init` relies on RESET DEFAULTS for the registers it never writes; a hostile or
buggy driver touches exactly those untouched registers, so replaying init leaves them
garbled. Reclaim must therefore drive every writable in-window register to a known
polled-ready value, whether or not init originally wrote it. The registers below that init
does NOT touch are the true silent-loss sources -- they are called out explicitly.

**Reclaim depth, XMC4800 (USIC0 CH0, ASC), the primary silicon target.** In-window channel
registers (U0C0 base), IN THIS ORDER:
- **`kernel_clock_enable(U0C0)` with its RM read-back FIRST.** The driver can clear
  `KSCFG.MODEN` (window offset 0x00C, `usic.h:40`), which GATES the UART kernel clock; with
  the module clock off, EVERY later reclaim write is silently ignored and the banner is lost.
  Re-enabling the module clock (and read-back-confirming per the RM) before any other write
  is mandatory and is a register init never re-touches at reclaim time.
- `CCR = 0` -- disable the channel / drop the protocol so any driver DMA or mode stops.
- reset the FIFO: `TBCTR = 0`, `RBCTR = 0` (driver may have enabled TX/RX FIFO).
- `set_baud(U0C0, BAUD_115200_72MHZ)` -- rewrite `FDR` + `BRG` (baud may have been changed).
- `SCTR = SCTR_WLE_8 | SCTR_FLE_8 | SCTR_TRM_ACTIVE | SCTR_PDL`, `TCSR = TCSR_TDEN_TDV | TCSR_TDSSM`
  (clears any DMA-trigger / interrupt-enable bits the driver set), `PCR = PCR_ASC_SP | PCR_ASC_SMD | PCR_ASC_TSTEN`.
- optionally clear `FMR.MTDV` -- a stale Transmit-Data-Valid word left by the driver emits
  one garbage byte on the first polled write; clearing it suppresses that byte.
- `PSCR = 0xFFFFFFFF` -- clear stale protocol status flags.
- `CCR = CCR_MODE_ASC` -- re-enable.
- Do NOT touch `SCU_CGATCLR0`/`PRCLR0` (system clock gate) or `P1_IOCR4` (pin mux): outside
  the window, privileged, intact. TBIEN stays clear (panic is polled).

**Reclaim depth, K64F (UART0), the secondary functional target.** UART0 registers:
- `C2 = 0` -- disable TX/RX/TIE so the driver stops.
- **`MODEM = 0`** -- clears `TXCTSE` (hardware CTS flow control). If the driver set it, the
  bounded polled writer waits forever on an absent CTS and drops EVERY byte -- the exact
  silent-loss signature. Init never writes MODEM, so only a full-window reclaim clears it.
- **`C3 = 0`** -- clears `TXINV` (TX-line inversion, which corrupts every framed byte).
- **`S2 = 0`** -- clears any driver-set status/config bits (e.g. inverted polarity).
- **`IR = 0`** -- clears `IREN` (infrared modulation on TX).
- **`C7816 = 0`** -- clears ISO-7816 smartcard framing.
- flush + disable FIFO if the driver enabled it: `CFIFO |= (TXFLUSH | RXFLUSH)`, `PFIFO = 0`.
- disable DMA: `C5 = 0`.
- re-derive + rewrite baud from `SystemCoreClock` into `BDH`/`BDL`/`C4` (as `uart0_init`).
- `C1 = 0` (8N1).
- `C2 = C2_TE` (TX only is enough for the panic banner; RE optional).
- Do NOT touch `SIM_SCGC4/5` (clock gates) or `PORTB_PCR16/17` (pin mux): separate
  peripherals, outside the UART0 window, privileged, intact.

Pin-mux and the clock tree outside the window are intact (separate privileged peripherals) --
keep that assumption. No DMA-abort of external controllers is needed: the DMA controllers sit
OUTSIDE the granted window, so the driver could never arm a descriptor against them; disabling
the peripheral's own DMA-request enable (`C5` / `TCSR`) inside the window is sufficient.

A wrong reclaim looks EXACTLY like silent panic loss -- the worst failure this system has --
so reclaim depth is a per-chip HW-confirm item, tested by D-test 3 (scramble-then-panic).
That test MUST include the two writes that reproduce TRUE silent loss: XMC `KSCFG.MODEN = 0`
and K64F `MODEM.TXCTSE = 1`.

### D7 -- the userspace console driver (new deliverable)

An unprivileged thread spawned with (a) the UART MMIO window via
`kos_thread_params.mmio_base/size` (implied R|W|DEV) and (b) a console endpoint cap
carrying `CAP_WAIT` (recv). It does not need to clock or pin the UART: the kernel's
`kickos_xmc_usic_init` already ran at boot and `console_tx_deinit` did NOT ungate the
clock or un-mux the pins -- the driver inherits a live, ASC-mode, pinned channel and only
drives TX. It MAY re-config baud/mode within its window; it CANNOT reach SCU/IOCR.

**Polled TX for stage ii-a (recommended).** Simplest thing that proves end-to-end; no IRQ
line to grant, no ack dance:

```
void console_driver_entry(void* arg)
{
    int ep = 1;                       // delegated recv cap lands at child index 1
    char buf[KOS_EP_MSG_MAX];
    for (;;)
    {
        uint32_t badge = 0;
        int n = kos_recv(ep, buf, sizeof(buf), &badge);
        if (n < 0)                    // bad cap -> unrecoverable; exit and let root respawn
        {
            break;
        }
        for (int i = 0; i < n; i++)
        {
            while (usic_tx_slot_free() == 0) { }   // poll TDV (bounded in a real impl)
            usic_tx_put((uint8_t)buf[i]);          // write TBUF0
        }
    }
    kos_exit(0);
}
```

Tier-1 IRQ-as-event TX (recv a batch, prime the TX-empty IRQ, `kos_irq_wait`, drain, ack)
is the ii-b/later refinement -- lower CPU at high baud, but it adds an IRQ grant + the
level-reassert care the k64dspi driver documents. First light does not need it.

**The driver MUST NOT use libc stdio (B3, hard rule).** `printf`/`puts`/`fprintf(stdout,..)`
route through `_write` (D5) -> `kos_send(0, ..)` -> the very endpoint the driver is serving.
That is a SELF-SEND: the driver blocks in `send` on its own endpoint waiting for a receiver
that is itself. It cannot even fail fast with EPIPE, because the driver holds the `CAP_WAIT`
receiver cap, so `recv_holders >= 1` and the dead-endpoint gate never fires -- it deadlocks
outright. The driver emits diagnostics only via `kos_kconsole_write` (RTT / kernel chip path)
or by writing its own granted UART window directly. This rule extends to any library the
driver links.

**Isolation reality by target.** XMC4800 (ARM PMSA) is the genuine per-thread grant: only
the driver's region set maps the U0C0 window; another unprivileged thread faults on it, and
SCU/IOCR stay privileged. That is why XMC is the first target -- the security boundary is
real and enforced at the window edge. K64F (SYSMPU + AIPS) works FUNCTIONALLY but peripheral
isolation is coarse: to let the unprivileged driver reach UART0 a privileged bring-up shim
must open the AIPS PACR slot GLOBALLY (as `user/driver/k64dspi` opens the DSPI slot), after
which any unprivileged thread can poke UART0. "Driver owns the device" on K64F is convention
plus the kernel/user split, not per-thread enforcement -- document it, do not pretend it.

**Build/lib location.** Mirror `user/driver/` and the k64dspi precedent: a new
`user/driver/xmcuart/` exporting `kickos_xmcuart` (STATIC, `kickos_apply_freestanding`,
`target_link_libraries(... kickos_user)`, `install(EXPORT KickOSTargets)`), guarded in
`user/driver/CMakeLists.txt` by `KICKOS_CHIP STREQUAL "xmc4800"`. Its `.data`/`.bss` land
in `.appdata` (not in the kernel closed set), user-reachable, exactly as k64dspi does. The
driver ships an optional privileged bring-up helper (no-op on XMC; opens the AIPS slot +
spawns the thread on the later K64F port).

### D8 -- driver-death policy

No kernel auto-adoption. When the driver faults/exits, its `cap_teardown` closes its recv
cap; the LANDED endpoint close protocol drops `recv_holders` to 0 and EPIPE-wakes every
parked sender with `-1`. Clients get `-1`, `_write` falls back to `kconsole_write` (chip
path dark, RTT + diag LED remain), and the console goes dark on the wire. Restart is root's
job: respawn the driver and re-publish (D3). This deliberately keeps the reclaim + re-arm +
re-attach machinery OFF the non-panic path and avoids a kernel-vs-respawned-driver ownership
race.

**Root MUST drop its own WAIT-bearing cap immediately after spawning the driver (S4, hard
rule).** EPIPE-on-driver-death fires ONLY when `recv_holders` reaches 0. Root creates E with
full rights and delegates a `CAP_WAIT` copy to the driver, so at spawn `recv_holders == 2`
(root + driver). If root keeps its WAIT-bearing cap, the driver's death drops the count to 1,
NOT 0 -- no EPIPE, senders stay parked forever, clients hang. That is strictly worse than
dark. So root closes its own WAIT-bearing cap on E right after the driver is spawned,
leaving the driver as the sole receiver. (Root keeps no receiver; re-publish after a death
uses a FRESH endpoint E', per below.)

**Publish + driver-spawn are ONE atomic root-side act (S6, root discipline).** Apps are
spawned only AFTER both publish and a confirmed driver spawn. If the driver spawn FAILS after
`kos_console_publish` returned, `g_console_state` is already `USER_OWNED`, `g_stdout_target`
names an endpoint with no receiver, and every app spawned afterward parks on its first
`printf` probe forever (the send blocks with no receiver; no EPIPE because root's ref may
still hold, and the fallback only helps once the send actually returns `-1`).
The kernel cannot cheaply detect "published but no live driver," so root MUST treat
publish+spawn as inseparable and must not spawn console-dependent apps if the driver spawn
did not succeed.

**Re-publish semantics, confirmed.** Root creates a FRESH endpoint E' for the new driver
and calls `kos_console_publish(E')`. Because `g_console_state` is already `USER_OWNED`,
`console_tx_deinit` is skipped; the call just re-points `g_stdout_target` to E' and moves
the kernel ref. Consequence to accept: apps spawned before the respawn still hold index 0 =
the DEAD E, so their sends return `-1` and they stay on the `kconsole_write` fallback (dark)
for their lifetime; only apps spawned AFTER the re-publish get E' at index 0 and print. This
is the honest cost of not mutating live tables (D4). Full recovery of old clients would need
a client-side re-probe or a table rewrite -- neither is in scope; see open questions.

### D9 -- priority

Driver priority >= its clients, a stated convention, NOT a spawn-time check. Rendezvous
lends no urgency (no PI on endpoints -- there is no owner to boost), so a client that
out-ranks the driver blocks on `send` until the driver is scheduled. Root's spawn discipline
owns this; the kernel does not police it.

The hazard when the convention is violated is UNBOUNDED PRIORITY INVERSION, not deadlock: a
middle-priority CPU-bound thread can starve the driver indefinitely while a high-priority
client waits on `send` for it -- classic inversion with no PI to break it. Separately, the
ii-a POLLED driver is itself an inversion risk even when correctly ranked: at top priority it
busy-polls the TX slot for the whole message -- roughly 22 ms per 256-byte message at 115200
baud (8N1 ~ 87 us/byte) -- and starves every client and lower thread for that entire span.
That CPU cost is the concrete argument for the ii-b IRQ-as-event TX tier (D7) at high baud.

## 3. Lifecycle (end to end)

```
 root (privileged)                 kernel                              driver / apps
 -----------------                 ------                              -------------
 1. E = kos_endpoint_create()
 2. kos_console_publish(E)  -----> IrqLock:
                                   console_tx_deinit() (D2 a-d)
                                   ref++, g_stdout_target = E-slot
                                   g_console_state = USER_OWNED (LAST)
 2b. drain g_chip_writers to 0 (lock released) before publish returns  (B1)
 3. spawn driver:
    mmio = UART window,
    caps = { E | CAP_WAIT }   --------------------------------------> recv cap at index 1
 3b. root CLOSES its own WAIT cap on E (S4): recv_holders now == 1 (driver only)
 4. spawn apps                --> cap_install_defaults seats          driver: recv-loop,
    (after publish)               { E-slot, CAP_SIGNAL } at index 0   poll-writes TX
                                                                      apps: printf -> _write
                                                                        -> kos_send(0, ..)
                                                                        -> rendezvous -> driver
 -- steady state: kernel kprintf chip path DROPS (RTT/dark); apps print via the driver --
 5. driver dies              --> close protocol: recv_holders->0,     parked senders get -1
                                 EPIPE-wake parked senders            apps fall to kconsole_write
 6. root respawns + re-publish(E') (D8): new target; new apps print, old apps stay dark
 -- panic at any time in USER_OWNED --
    kpanic_enter: arch_console_reclaim(); state=RECLAIMED; flush; banner polled to the UART
```

Between step 2 and the driver's first `recv`, a client `send` simply parks on
`send_waiters` -- rendezvous absorbs the handover gap with no lost output and no "handover
in progress" state to size. That is a genuine argument for rendezvous over a buffered
channel here.

## 4. Selftest / HW-test plan

Emulator-testable (QEMU/sim, in-tree `ctest`):
- **U-test 1 (routing).** Drive `g_console_state` through the three values behind a test
  hook and assert `console_emit` routes buffered / drops / polled respectively; assert RTT
  fan-out is unaffected in `USER_OWNED`.
- **U-test 2 (deinit ordering).** Assert `console_tx_deinit` leaves `armed == false`, the
  line detached, and the state flip happening strictly after (a fault-injection point
  between steps observes `KERNEL_OWNED`).
- **U-test 3 (default seating + refs).** After a simulated publish, assert
  `cap_install_defaults` seats `{CAP_SIGNAL}` at index 0, bumps `endpoint_refs` but NOT
  `recv_holders`, and that child teardown drops the ref.
- **U-test 4 (EPIPE on driver death).** A parked sender + last recv-holder close -> sender
  woken `-1`; `_write` falls back to the REMAINDER only (S5). With the per-invocation re-probe
  a second `_write` DOES re-attempt `kos_send(0)` (no sticky state), so the assertion is that
  the fallback covers the remainder of each failing write, not that later writes stop probing.
  This exercises the LANDED close protocol through the fallback path.
- **U-test 5 (index-0 reservation, B3 -- re-validates landed cap.cc).** Assert a task's own
  `sem`/`endpoint`/`mutex` create never returns handle index 0 (scan starts at 1); assert
  `cap_install_at(_, 0, _)` from `cap_install_defaults` is the only writer of slot 0; run the
  existing cap selftests to confirm nothing assumed a first-create-at-0. On a default-8 board
  assert `MAX_HANDLES - 1 == 7` own-caps still install and the 8th fails cleanly.
- **U-test 6 (in-flight chip-writer drain, B1).** With a fault/preemption hook, park a writer
  that has sampled `KERNEL_OWNED` and incremented `g_chip_writers`, then run
  `kos_console_publish`; assert publish does NOT return until `g_chip_writers` drains to 0,
  and that a `console_tx_write` not-armed fall-through re-reads the state and DROPS the chip
  write when it is no longer `KERNEL_OWNED`.

Silicon-only (XMC4800 first; the reclaim/UART behavior cannot be faithfully emulated):
- **D-test 1 (end-to-end handover).** Root creates E, publishes, spawns the `xmcuart` driver
  (MMIO + recv cap), spawns an app that `printf`s; assert the output appears on the UART
  driven by the userspace driver (kernel chip path silent).
- **D-test 2 (driver death -> dark + EPIPE).** Kill the driver; assert clients get `-1`,
  the console goes dark on the wire, RTT/diag LED still work, and a re-published fresh driver
  restores output for newly spawned apps. **Add the root-still-holds-WAIT sub-case (S4):**
  spawn the driver but have root deliberately NOT close its own WAIT-bearing cap, kill the
  driver, and assert senders HANG (recv_holders stays 1, no EPIPE) -- this is the failure the
  S4 rule forbids; it documents why the rule is load-bearing. The pass case is the same test
  with root correctly dropping its WAIT cap and senders getting `-1`.
- **D-test 3 (scramble-then-panic reclaim -- the critical one).** The driver deliberately
  garbles its in-window UART registers, then root forces a panic; assert the panic banner
  still arrives, polled, intact. This is the ONLY test that validates reclaim depth (D6); a
  wrong reclaim is indistinguishable from silent panic loss, so this gates the feature. The
  scramble MUST include the two writes that reproduce TRUE silent loss -- XMC `KSCFG.MODEN = 0`
  (gated module clock) and K64F `MODEM.TXCTSE = 1` (CTS flow control stall) -- in addition to
  baud, mode, FIFO. Also exercise the B2 path: trigger the panic via an MPU FAULT in the
  driver (the likely real-world faulter) and assert `kickos_isr_fault` funnels through
  `kpanic_enter`, reclaims, and prints. Re-run functionally on K64F once that port lands.

Staging split (recommended):
- **ii-a: kernel mechanism + minimal polled XMC driver.** D1-D5, D7 (polled), D8, D9, minus
  reclaim. Includes the B1 writer-drain, the B3 index-0 reservation change to `cap_install`
  (which touches LANDED stage-(i) cap.cc and so must be RE-VALIDATED on this silicon run,
  U-test 5), and the S3/S4/S5/S6 rules. Proves the whole client->driver->UART path on silicon
  (D-test 1/2) with the panic path still on the pre-handover assumption. Note B2 (funnel
  `kickos_isr_fault` through `kpanic_enter`) can land in ii-a cheaply, but its reclaim payoff
  only appears once D6 exists in ii-b -- until then it is behavior-preserving.
- **ii-b: `arch_console_reclaim` + panic wiring + D-test 3.** D6 on XMC, then K64F functional.
  Split out because reclaim is the highest-risk, most chip-specific part and deserves its own
  focused review + the deliberate scramble-then-panic HW test, rather than riding in on the
  mechanism change. ii-a can land and be exercised before ii-b exists (panic simply behaves
  as today until reclaim is wired).

## 5. Rulings (fable review, resolved)

All eight review questions are decided. They are recorded here as settled decisions; the
design sections above already reflect them.

1. **Reclaim depth per chip = full in-window rewrite (Q1 -> S1).** Not "re-run init." Reclaim
   drives EVERY writable register inside the granted window to a known polled-ready value,
   including the ones init leaves at reset default (the true silent-loss sources: XMC
   `KSCFG.MODEN`, K64F `MODEM.TXCTSE`/`C3`/`IR`/`C7816`). No external-DMA abort needed --
   controllers are outside the window. See D6; D-test 3 guards it.
2. **Kernel dark output = accept RTT-or-dark for M3 (Q2).** Steady-state kernel kprintf goes
   to RTT or nowhere on the wire post-handover. A kernel-owned second UART is a later
   per-board option, not M3 scope.
3. **Old-client recovery = accept old-apps-dark for stage ii (Q3).** A client re-probe cannot
   recover old apps: their index-0 cap names the DEAD endpoint permanently, so re-probing
   index 0 still hits a dead object. With the per-invocation re-probe (S5) such a client does
   spend one wasted syscall per write (there is no sticky state), accepted as the cost of
   correct per-thread routing. The real future fix is a distinct stdout CAP TYPE resolved
   against `g_stdout_target` at send time (so a re-publish transparently redirects live
   clients) -- recorded for later, NOT built now.
4. **`g_stdout_target` kernel ref = KEEP it (Q4 -> S3).** The publish-held refcount closes
   the publish-to-first-spawn zero-ref window that relying on `recv_holders` + client send
   refs would leave open. It is held via `obj_ref_inc` / `endpoint_ref_drop` on the global
   gen-encoded handle, never raw `endpoint_refs[]` arithmetic. See D3.
5. **Privileged-only publish = confirmed (Q5).** Syscall 29 is gated on `current->privileged`.
   A distinct delegable capability is not warranted for M3.
6. **Driver TX tier = ii-a polled confirmed (Q6).** Land polled TX first (D7); IRQ-as-event
   TX is the ii-b/later refinement justified by the high-baud CPU cost quantified in D9.
7. **M4/SMP reclaim fence = record only (Q7).** No hook reserved now. The single-core reclaim
   argument (D6) stands; the multicore stop/fence-the-other-core requirement is flagged for
   the SMP design and will reshape reclaim then.
8. **`recv` writable-pointer bound-check = already in place (Q8).** Confirmed: the landed
   `recv` validates the caller's buffer + badge-out pointer at `syscall.cc:310-319`, so the
   driver (a privileged server) does not open a write oracle. No new work.
