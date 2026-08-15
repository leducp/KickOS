<!-- SPDX-License-Identifier: CECILL-C -->
# Design note: M3 #4 stage (ii) -- console device handover + panic-path reclaim

> **Status: LANDED** -- console device handover shipped and is silicon-proven on XMC: an app
> `printf` reaches the wire through IPC and an unprivileged userspace driver, under enforcement,
> and the panic path reclaims a driver-garbled UART so the banner survives. Two console drivers
> exist (`system/driver/xmc4800/xmcuart`, `system/driver/mk64f/k64uart`); fleet-wide rollout is
> M4 work (`design-driver-era-scope.md` G1).
>
> **The contract now lives in the Reference**: `reference/console.md` (the routing guard, the
> handover mechanism, the reclaim, and "The publisher's obligations" for the root-side rules) plus
> `reference/invariants.md` (the three `console-*` invariants). This note is the decision record
> behind them, and three of its decisions have been SUPERSEDED by what shipped: the reclaim gate
> (D6), the racing-writer drain (D3), and the index-0 reservation (D4). Each says so in place.
> See `design/README.md` for the marker taxonomy.

Stage (i) (the endpoint object plus `send`/`recv`/`create`, root-only caps, the `recv_holders`
dead-gate and EPIPE) landed first; its how/why is
`book/endpoints-synchronous-ipc-by-rendezvous.md`. This note designs what sits ON TOP:
relinquishing the UART to a userspace driver, routing kernel output around the relinquished
device, reclaiming it in a panic, and the driver itself. It builds only on the landed endpoint
contract and does not reopen it.

---

## 1. Concept: two axes, not one flag more

The console decided buffered-vs-sync from three flags at one choke point (`console_tx_armed()`,
`arch_in_isr()`, `g_console_panicking`). Handover asks an ORTHOGONAL question those flags cannot
answer: who owns the UART TX register.

**DECIDED: a three-value ownership axis, consulted BEFORE the buffered-vs-sync sub-decision**,
because in the middle value the kernel must touch the device on NO path at all. REJECTED: a fourth
flag on the same axis. `g_console_panicking` keeps its exact meaning and is load-bearing only in
`KERNEL_OWNED`; `RECLAIMED` subsumes it for the handed-over case.

RTT is untouched throughout: `kconsole_write` fans out to RTT under its own IrqLock before it ever
reaches `console_emit`, so RTT carries the bytes in every state. Only the chip path is gated.

## 2. Decisions

### D1 -- the routing choke point

**DECIDED: a distinct `USER_OWNED` state that DROPS.** REJECTED: the tempting minimal handover,
"clear `g_tx.armed` and let the existing guard fall through". With `armed == false` the
`KERNEL_OWNED` branch takes its `else` and POLL-WRITES the TX data register, which is the exact
register the userspace driver now owns: the two-drivers-one-device hazard, reintroduced silently
on the steady-state `kprintf` path. The kernel must stop TOUCHING the device, not merely stop
BUFFERING, so only a state that returns without any `arch_console_write*` call is sufficient.
`armed == false` stays NECESSARY (a stray `KERNEL_OWNED` producer must not enqueue).

**DECIDED (B1): the read-then-flip race needs two mechanisms, both load-bearing.** A writer that
sampled `KERNEL_OWNED`, or is already mid polled write, can still poke the UART AFTER the flip,
and the state check alone cannot close that window. (a) The buffered producer's not-armed
fall-through re-reads the state and DROPS the chip write unless it is still `KERNEL_OWNED`. (b) An
in-flight chip-writer count, incremented under the same read that decided to write and decremented
after the poke, which publish drains to zero before it returns. Root cannot spawn the driver until
publish returns, so the stale writer is off the device before any userspace driver touches it.

### D2 -- `console_tx_deinit`: the relinquish sequence

**DECIDED: flush to shift-idle, disable the TX-empty IRQ at the peripheral, `irq_detach` plus
NVIC-mask the line, disarm the ring, all under ONE IrqLock**, and idempotent (S2: an early return on
an unarmed ring also covers the polled-only chips that never arm). The flush must wait for a
drained SHIFTER, not a free TX slot; deinit while the last byte is still shifting emits one
garbled character.

**DECIDED: the caller flips the ownership state LAST, after this returns.** The four steps are
atomic against every buffered producer and the drain ISR, but a SYNCHRONOUS fault is not maskable
and can land between any two of them. Flipping last is what makes every interior point safe: the
UART is still kernel-inited and reachable, the state is still `KERNEL_OWNED`, so the fault path's
existing flush plus polled write works verbatim. Even after the ring is disarmed but before the
flip, `KERNEL_OWNED` routes to the polled writer poking a UART that is still the kernel's.

### D3 -- `kos_console_publish`: the syscall (29)

**DECIDED: one gated syscall does the whole hand-off** -- relinquish the kernel path (D2, skipped
on a re-publish because the ring is already disarmed), take a KERNEL reference on the stdout
endpoint, re-point `g_stdout_target` (dropping any previous target), flip the state LAST, then
drain the racing writer. The kernel reference is what closes the publish-to-first-spawn zero-ref
window (ruling 4), and it goes through the `obj_ref_inc` / `endpoint_ref_drop` helpers, NEVER raw
`endpoint_refs[]` arithmetic, because raw bumps bypass the free-at-zero teardown and the
receiver-waiter guard. `g_stdout_target` holds the GLOBAL gen-encoded handle, not a pool index.
The call is deliberately re-callable, which is what D8's re-publish relies on.

**SUPERSEDED, the drain mechanism.** This note wrote it as a bare relax-spin,
`while (g_chip_writers != 0)`. What SHIPPED instead: the publisher drops itself to
`KICKOS_PRIO_MIN` and YIELDS each pass, under a bounded guard that panics rather than hanging
silently. The bare spin LIVELOCKS, because the scheduler is strict-priority and an in-flight
writer preempted mid polled write (that loop runs without IrqLock) can only finish once
rescheduled, which never happens while a higher-priority publisher spins. Draining to zero is
still safe: after the flip no path increments the count, and a polled writer never blocks between
its increment and decrement, so a non-zero count always means a RUNNABLE writer exists.

**Also beyond this note:** publish seats the PUBLISHER's own index 0 as well. Root was created
before any publish, so the default-seating path never seated its slot; without this the publishing
task's own `printf` would fail its probe and fall back to the now-dark kernel path.

**The gate MOVED.** Written here as `current->privileged`; the gate that stands is the
`AUTH_CONSOLE` authority bit (ruling 5). Current contract: `reference/console.md` plus
`reference/architecture.md`.

### D4 -- capability index 0 is the stdout slot

**DECIDED: the kernel seats index 0 of every child spawned after a publish, send-only.**
`CAP_SIGNAL` alone: no `CAP_WAIT`, so a client cannot recv-steal console traffic AND its reference
does not bump `recv_holders` (a client is not a receiver and must not hold the dead-endpoint gate
open), and no `CAP_TRANSFER`, so it cannot be delegated onward. The reference is dropped by the
child's own teardown like any other seated cap. Before a publish, defaults seat nothing, so the
selftest and bring-up world that never publishes is untouched.

**DECIDED: apps spawned BEFORE a publish keep an empty index 0 and stay on the fallback.**
REJECTED: late-seating index 0 into already-live tables. Mutating another task's capability table
from a syscall breaks the "your table changes at spawn or by your own hand" model. Root must
therefore publish before spawning any app that should print through the driver, which is the
root-task rule D8 carries.

**SUPERSEDED, the reservation mechanism (B3).** For index 0 to be dependable, an own-create
(`sem`/`endpoint`/`mutex`) must never occupy it. This note proposed starting `cap_install`'s
free-slot scan at index 1. What SHIPPED is a reserved RANGE
`[0 .. KICKOS_CAP_FIRST_DYNAMIC)`, today `[0..2)`: the placement scan starts at
`KICKOS_CAP_FIRST_DYNAMIC` and `cap_install_at` refuses index 0 outright, so the range moves only
by a deliberate fleet-wide ABI change and never by something drifting into it. The sizing
consequence landed differently too: this note
was written against a `KICKOS_MAX_HANDLES` default of 8 with a tiny-board floor of 6, whereas the
values in force are 10 with the four polled-only tiny boards at 7. Current contract:
`reference/invariants.md` (`own-create-skips-reserved-cap-index`) plus
`reference/architecture.md`.

### D5 -- `_write` migration (libc-side, kernel stays simple)

**DECIDED: no persistent state; each write re-classifies against the CALLING thread's own index 0**
and, only on failure, falls back to the kernel writer for THAT write. Index 0 is per-thread and
fixed at spawn, so the classification is stable within a thread without caching it, and a
pre-publish thread's failure never poisons a post-publish thread whose slot IS seated. The cost is
one wasted syscall per write for a permanently-dark client, accepted as the price of correct
per-thread routing.

**DECIDED (S5): the fallback resends the REMAINDER only**, never the whole buffer, or the chunks
already delivered to the driver are duplicated on RTT when a mid-stream failure hits.

The policy lives in THREE writers kept deliberately in step (`user/include/kickos/sys/emit.h`
names them): `kickos::emit()` for freestanding apps, `tests/tap/tap.cc`'s `emit()` for the suite,
and `_write` in `user/src/newlib_stubs.cc`. So `printf` and `std::cout` DO reach a published
driver -- MEASURED on the frdmk64f full-service-list run (2026-07-30), where the whole TAP suite
arrives that way. What DROPS is `kos_print` / `kos_kconsole_write`, the kernel bring-up path, and
even that is still carried by RTT. Two real losses remain and only these two: an app that calls
`kos_print` instead of the publish-aware writer loses its output, and the dark window between the
publish flip and the driver actually serving index 0 (`reference/console.md`).

### D6 -- `arch_console_reclaim()`: the arch seam and the panic path

**DECIDED: a panic forces the UART back to a polled-ready channel through a new arch seam**, with
a lone-TU no-op fallback (`arch/common/arch_console_reclaim_default.cc`) for boards that never hand
over. Reclaim runs BEFORE the flush, which is a no-op post-handover anyway.

**DECIDED (B2): the fault reporter funnels through `kpanic_enter`.** It previously did its own
flush and polled banner and never called it, so in a handed-over state the terminal fault path
printed to a UART the kernel no longer owned. The single most likely post-handover faulter IS the
console driver itself, so the report was dropped on a dead chip path and the system halted
silently. **M2 dependency:** only a TERMINAL fault exit may reclaim. A kill-and-resume fault path
must NOT, because the driver keeps the device and a dark report there is CORRECT. Gate reclaim on
"this fault terminates the system", never on "a fault happened".

**SUPERSEDED, the reclaim gate.** This note gated reclaim on the state being `USER_OWNED`. What
SHIPPED reclaims from ANY state that is not already `RECLAIMED`, and stores `RECLAIMED` BEFORE
calling the body. Two reasons the narrower gate was wrong: the ownership axis records a PUBLISH
and never whether the device is GARBLED, and a thread merely GRANTED the console window can wreck
the channel with no publish at all (still `KERNEL_OWNED`); and storing the state first is what
makes reclaim unconditional-once, so a synchronous fault inside the body re-enters and stops
instead of recursing with the old state, and a body that truncates the byte in the shift register
cannot cut the banner it just printed. WITNESSED on silicon: a `KERNEL_OWNED` channel gated at
`KSCFG.BPMODEN=1, MODEN=0` still delivered its fault banner (`user/apps/xmc4800-relax/conreclaim`,
`c5d9b0d`). The widening rests entirely on every chip body being idempotent absolute stores.
Current contract: `reference/invariants.md` (`panic-console-probe-independent`) plus
`reference/console.md`.

**DECIDED (S1): reclaim depth = rewrite EVERY writable register inside the window**, not "re-run
init". Init relies on RESET DEFAULTS for the registers it never writes, and a hostile or buggy
driver touches exactly those, so replaying init leaves them garbled. Write it as straight-line
absolute STORES with no read-modify-write on driver-touched registers: absolute writes are safe to
repeat, an RMW on a garbled register is not, and reclaim must be re-entrant from any partial
state.

Three depth decisions worth keeping past the per-chip register lists:

- **The module clock goes FIRST, with its read-back.** The driver can gate the UART's own kernel
  clock (XMC `KSCFG.MODEN`), after which EVERY later reclaim write is silently ignored and the
  banner is lost. This is a register init never re-touches at reclaim time.
- **Everything OUTSIDE the window is TRUSTED**: pin mux, SCU/SIM clock gates, the clock tree. The
  MMIO grant is exact-window and those live in privileged-only peripherals, so the driver could
  never reach them. Reclaim ADDS nothing to kernel access either, since the chip backend never
  left the kernel image and an MMIO grant never removes the kernel's privileged background access.
  No external-DMA abort is needed for the same reason: the DMA controllers sit outside the window,
  so disabling the peripheral's own DMA-request enable inside it is sufficient.
- **The two registers that reproduce TRUE silent loss are named, and D-test 3 MUST include them**:
  XMC `KSCFG.MODEN = 0` (gated module clock) and K64F `MODEM.TXCTSE = 1`, where the bounded polled
  writer waits forever on an absent CTS and drops EVERY byte. Init writes neither, so only a
  full-window reclaim clears them. A wrong reclaim looks EXACTLY like silent panic loss, the worst
  failure this system has, which is why depth is a per-chip HW-confirm item.

### D7 -- the userspace console driver

**DECIDED: an unprivileged thread holding (a) the UART MMIO window and (b) an endpoint cap
carrying `CAP_WAIT`.** It needs no clock or pin authority: the kernel's init already ran and the
relinquish deliberately does NOT ungate the clock or un-mux the pins, so the driver inherits a
live, pinned, ASC-mode channel and only drives TX. It MAY reconfigure baud or mode within its
window; it CANNOT reach SCU/IOCR.

**DECIDED: polled TX first.** Simplest thing that proves end-to-end, with no IRQ line to grant and
no ack dance. Tier-1 IRQ-as-event TX is the later refinement, justified by D9's CPU cost, and it
adds an IRQ grant plus the level-reassert care the DSPI driver documents.

**DECIDED (B3, hard rule): the driver MUST NOT use libc stdio.** `printf` and friends route through
`_write` (D5) to a send on the very endpoint the driver is serving: a SELF-SEND, on which it
blocks waiting for a receiver that is itself. It cannot even fail fast with EPIPE, because the
driver holds the `CAP_WAIT` cap, so `recv_holders >= 1` and the dead-endpoint gate never fires. It
DEADLOCKS outright. The driver diagnoses only via `kos_kconsole_write` or by writing its own
granted window directly, and the rule extends to any library it links.

**Isolation reality by target.** XMC4800 (ARM PMSA) is the genuine per-thread grant: only the
driver's region set maps the channel window, another unprivileged thread faults on it, and
SCU/IOCR stay privileged. That is why XMC is the first target, because the security boundary is
real and enforced at the window edge. K64F (SYSMPU plus AIPS) works FUNCTIONALLY, but peripheral
isolation is coarse: letting the unprivileged driver reach UART0 needs a privileged bring-up shim
to open the AIPS PACR slot GLOBALLY, after which any unprivileged thread can poke UART0. "Driver
owns the device" on K64F is convention plus the kernel/user split, not per-thread enforcement.
Document that, do not pretend otherwise.

**DECIDED: the driver lib lives under `system/`, not `user/`**, keyed by chip, because a chip
driver lib is board support a consumer links on top of the OS rather than an app, even though it
builds unprivileged. Its `.data`/`.bss` land in `.appdata`, user-reachable, like the DSPI
precedent.

### D8 -- driver-death policy

**DECIDED: no kernel auto-adoption.** The driver's teardown closes its recv cap, the landed close
protocol drops `recv_holders` to zero and EPIPE-wakes every parked sender, clients fall back to
the kernel writer (chip path dark, RTT and the diag LED remain), and the console goes dark on the
wire. Restart is root's job: respawn and re-publish. This deliberately keeps the reclaim, re-arm
and re-attach machinery OFF the non-panic path and avoids a kernel-vs-respawned-driver ownership
race.

**S4 (root drops its own WAIT-bearing cap right after the spawn) and S6 (publish and driver-spawn
are one atomic act)** are now stated with their full holder-count and dark-window reasoning in
`reference/console.md`, "The publisher's obligations". Both remain hard rules, and source comments
cite them by these labels.

**DECIDED: a re-publish uses a FRESH endpoint**, and the cost is accepted: apps spawned before the
respawn still hold the DEAD endpoint at index 0, so they stay on the fallback (dark) for their
lifetime, and only apps spawned AFTER get the new target. That is the honest price of not mutating
live tables (D4). Full recovery of old clients is ruling 3.

### D9 -- priority

**DECIDED: driver priority >= its clients is a stated convention, NOT a spawn-time check.**
Rendezvous lends no urgency (no PI on an endpoint, there is no owner to boost), so a client that
out-ranks the driver blocks on its send until the driver is scheduled. Root's spawn discipline owns
this; the kernel does not police it.

The hazard when the convention is violated is UNBOUNDED PRIORITY INVERSION, not deadlock: a
middle-priority CPU-bound thread can starve the driver indefinitely while a high-priority client
waits on its send, with no PI to break it. Separately, the polled driver is itself an inversion
risk even when correctly ranked: at top priority it busy-polls the TX slot for the whole message,
roughly 22 ms per 256-byte message at 115200 8N1 (~87 us/byte), starving every client and lower
thread for that entire span. That measured cost is the concrete argument for the IRQ-as-event tier
(D7).

## 3. Lifecycle (end to end)

The step order and the publisher's obligations are `reference/console.md`. The design point worth
keeping here: between the publish and the driver's first receive, a client send simply parks on
the endpoint's send-waiter queue. Rendezvous absorbs the handover gap with no lost output and no
"handover in progress" state to size, which is a genuine argument for rendezvous over a buffered
channel at this seam.

## 4. Selftest / HW-test plan

Emulator-testable (in-tree `ctest`):
- **U-test 1 (routing).** Drive the ownership state through all three values and assert the choke
  point routes buffered / drops / polled, with RTT fan-out unaffected in `USER_OWNED`.
- **U-test 2 (deinit ordering).** Assert the relinquish leaves the ring disarmed and the line
  detached, and that a fault-injection point between steps still observes `KERNEL_OWNED`.
- **U-test 3 (default seating and refs).** After a simulated publish, assert index 0 is seated
  `CAP_SIGNAL`, that the endpoint reference count moves but `recv_holders` does NOT, and that
  child teardown drops it.
- **U-test 4 (EPIPE on driver death).** A parked sender plus the last recv-holder closing wakes
  the sender with EPIPE; assert the fallback covers the REMAINDER of each failing write (S5), and
  NOT that later writes stop probing (there is no sticky state).
- **U-test 5 (index-0 reservation, B3).** Assert an own-create never lands in the reserved range,
  that the default-seating path is the only writer of slot 0, and that the existing cap selftests
  do not assume a first create at index 0.
- **U-test 6 (in-flight writer drain, B1).** Park a writer that sampled `KERNEL_OWNED`, then
  publish; assert publish does not return until the count drains, and that the not-armed
  fall-through drops
  its chip write once the state has moved.

Silicon-only (the reclaim and UART behaviour cannot be faithfully emulated):
- **D-test 1 (end-to-end handover).** Publish, spawn the driver with the window plus recv cap,
  spawn an app that prints; assert the output appears on the UART driven by the userspace driver
  with the kernel chip path silent.
- **D-test 2 (driver death: dark plus EPIPE).** Kill the driver; assert clients fail, the wire
  goes dark, RTT and the diag LED still work, and a re-published fresh driver restores output for
  newly spawned apps. Includes the FALSIFIER sub-case for S4: have root deliberately KEEP its own
  WAIT-bearing cap, kill the driver, and assert senders HANG rather than getting EPIPE. That is
  the failure the rule forbids, and running it is what shows the rule is load-bearing.
- **D-test 3 (scramble-then-panic reclaim), the gating test.** An unprivileged holder of the
  console window garbles its in-window registers and faults; assert the banner still arrives,
  polled, intact. This is the ONLY test that validates reclaim depth (D6), and a wrong reclaim is
  indistinguishable from silent panic loss, so it gates the feature. The scramble MUST include the
  two true-silent-loss writes named in D6 on top of baud, mode and FIFO, and must also trigger via
  an MPU FAULT in the driver (the likely real-world faulter) to exercise the B2 funnel. On XMC it
  is the standalone `conreclaim` app and NOT a build option on the demo app: the channel admits
  exactly ONE holder, so the scrambler and the driver cannot both be it in one image
  (`design-driver-era-scope.md` G3). The property under test does not depend on WHO garbled the
  UART, so nothing is lost to the split.

## 5. Rulings (fable review, resolved)

1. **Reclaim depth per chip = full in-window rewrite (Q1 -> S1).** Not "re-run init". See D6;
   D-test 3 guards it.
2. **Kernel dark output = accept RTT-or-dark for M3 (Q2).** Steady-state kernel output goes to RTT
   or nowhere on the wire post-handover. A kernel-owned second UART is a later per-board option,
   not M3 scope.
3. **Old-client recovery = accept old-apps-dark for stage ii (Q3), and the future fix is
   recorded, NOT built.** A client-side re-probe cannot recover an old app: its index-0 cap names
   the DEAD endpoint permanently, so re-probing index 0 still hits a dead object. With the
   per-invocation re-probe (D5) such a client spends one wasted syscall per write, accepted as the
   cost of correct per-thread routing. The real fix is a DISTINCT stdout capability TYPE resolved
   against `g_stdout_target` at SEND time, so a re-publish transparently redirects live clients.
   That is the one genuinely open design direction left in this note.
4. **The kernel stdout reference = KEEP it (Q4 -> S3).** It closes the publish-to-first-spawn
   zero-ref window that relying on `recv_holders` plus client send refs would leave open, and it
   is held through the ref helpers, never raw array arithmetic. See D3.
5. **Gated publish = confirmed (Q5).** A distinct delegable capability was not warranted for M3.
   The ruling was taken as `current->privileged`; the gate is now the `AUTH_CONSOLE` authority
   bit, which is what carries the ruling now that root is unprivileged everywhere and a privilege
   check would gate nothing.
6. **Driver TX tier = polled confirmed (Q6).** Land polled TX first (D7); IRQ-as-event TX is the
   later refinement justified by the CPU cost quantified in D9.
7. **SMP reclaim fence = record only (Q7).** No hook reserved now. The single-core argument holds:
   once the panic masks, no user thread including the driver ever runs again on this core, so
   nothing can concurrently poke the UART during or after reclaim. Under AMP/SMP another core's
   driver must be stopped or fenced first, or it races the polled panic writer. Flagged for the
   SMP design, which will reshape reclaim.
8. **`recv` writable-pointer bound-check = already in place (Q8).** Confirmed: the landed receive
   path validates the caller's buffer and badge-out pointer, so the driver does not open a write
   oracle. No new work.
