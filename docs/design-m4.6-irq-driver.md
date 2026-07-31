<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# M4.6 design -- the IRQ-driven driver pattern, and the buffered userspace UART on top

**DESIGN GATE. No code written yet.** This document is the M4.6 step-0 gate: the general
mechanism by which an **unprivileged userspace driver owns an interrupt line** -- claimed by
privileged bring-up, handed to the driver at spawn, and **reclaimed when the driver dies**.
UART is the first and forcing consumer; M4.7 (ADC / RTC / watchdog / entropy / PWM / DAC)
must reuse this substrate **unchanged**.

Verified against `master` `64410b7`. Every file:line below was read at that commit.
**Recovered 2026-07-30 from an unpushed worktree, unchanged.** It predates the whole M4.5.x
unprivileged-root arc, so its citations need re-verifying at the top of M4.6.1: root is now
unconditionally unprivileged, the separate device and clock authority bits were deleted, and the
authority set is the six `KOS_AUTH_*` bits in `reference/invariants.md`. The DESIGN is
unaffected -- nothing here assumed the deleted posture.

Prior art folded in and superseded: `git show c296feb:docs/design-m4-rx-irq-demux.md` (the
277-line RX72M peripheral-IRQ demux spike, never landed on master). Its routing-class
taxonomy, group-register table, level-vs-edge semantics and the `arch_reserved_blocks`
finding are carried forward here and **cited, not redone**. Section 6 is its successor.

---

## 0. The one-sentence problem, and the three holes

Today the fleet's userspace console drivers are polled-TX-only, no RX, and spin up to
`KICKOS_POLL_SPIN_MAX` (1,000,000) iterations per byte before dropping it. The kernel
already solved this for ITSELF -- `lib/include/kickos/console_tx.h` +
`kernel/init/console_tx.cc` are a working buffered IRQ-drained ring on 13 chips. M4.6 moves
that pattern across the privilege boundary.

The tier-1 IRQ substrate that must carry it (`kernel/irq/irq.cc`) has three holes:

1. **Ungated.** `KOS_SYS_IRQ_REGISTER/WAIT/ACK` (14/15/16) have **no privilege check and no
   capability** (`kernel/syscall/syscall.cc:593-604`). Any unprivileged thread may claim ANY
   line in `[0, KICKOS_MAX_IRQ)`, first-come-first-served. Compare `KOS_SYS_IRQ_ATTACH = 11`
   (`:476-524`) which IS privileged AND requires a `CAP_SEM` bearing `CAP_SIGNAL`.
2. **Nothing is released on death.** `sched::exit_current` (`kernel/sched/sched.cc:185-222`)
   tears down capabilities only. An `IrqBinding` is not a cap type (`kernel/include/kickos/cap.h:46-53`
   has `CAP_EMPTY/SEM/MUTEX/ENDPOINT/REPLY`), bindings are **bump-allocated with no free
   list** (`kernel/irq/irq.cc:128-134`, "no unregister/free path yet"), and `irq_detach` is
   never called on exit. A dead driver leaves the line **armed**, its ISR masking and
   `sem_post`ing into a semaphore nobody waits on, its binding slot burned
   (`KICKOS_MAX_IRQ_HANDLES` defaults to 8 and is **4** on bluepill-c8 / f302nucleo /
   microbit), and the line unclaimable forever (`-KOS_EBUSY`).
3. **Edge-only.** The binding contract is latch-and-coalesce (`arch/include/kickos/arch/arch.h:340-350`).
   Level sources -- RX72M `GROUPBL0`, and every "one status register, N sub-sources" UART --
   need a per-binding trigger type so the rearm does `clear_pending` then `unmask`
   (`TODO.md:292-301`).

All three are closed here. Hole 1 and hole 2 are closed by the **same** decision.

---

## 1. What is general mechanism, what is UART policy

The reviewer's first question. Explicitly:

| Layer | Item | Status |
|---|---|---|
| **General kernel mechanism** | the IRQ capability object type + pool + refcount (sec.2) | M4.6, reused verbatim by M4.7 |
| | `kos_irq_claim` (privileged mint) / `wait` / `ack` / `notify` (sec.2) | M4.6 |
| | Delegation of an IRQ cap at spawn (sec.3) | M4.6 |
| | Teardown-on-death via `cap_teardown` (sec.4) | M4.6 |
| | Per-binding EDGE/LEVEL trigger type (sec.5) | M4.6 |
| | Chip **device-dispatch hook** that may post 0..N logical lines per physical vector (sec.6) | M4.6 |
| | Console death-reclaim hook in `exit_current` (sec.4.4) | M4.6 |
| **General userspace substrate** | `kos_byte_ring` SPSC byte ring utility (sec.7.1) | M4.6, reused by M4.7 |
| | `spawn_driver` (N delegated caps + shared data region) (sec.3.3) | M4.6 |
| | Two-thread driver shape: one `irq_wait` thread + one `recv` thread (sec.7.2) | M4.6, the M4.7 template |
| **UART-specific policy** | the `Uart` class concept (sec.7.3) | M4.6 |
| | `kos_uart_req/rsp` wire ABI (sec.7.4) | M4.6 |
| | TX drain / prime policy, RX overrun policy, drop accounting (sec.7.5-7.6) | M4.6 |
| | one-line-serves-RX-and-TX demux inside the driver (sec.7.7) | M4.6 |
| | RX72M `GROUPBL0` `ISj -> line` map, SCI6 register poking (sec.6.3) | M4.6 |

The dividing line: **the kernel never learns what a UART is.** It routes lines and
capabilities. Everything in "UART-specific policy" lives in `system/driver/` or
`user/include/kickos/sys/`.

---

## 2. Decision 1 -- the IRQ line IS a capability

### 2.1 The decision

**An interrupt line becomes a first-class kernel object named by a new `CapType` member for
IRQ lines (sec.2.2), allocated from a generational `SlotPool`, refcounted, delegated at spawn
like any other cap, and released by the existing `cap_teardown` on thread exit.**

Rejected alternative: **a separate IRQ registry with its own ownership + teardown hook**
(keep the integer handle, add an `irq_release_all(Thread*)` called from `exit_current`).
Rejected for four concrete reasons:

- It cannot express **two threads sharing one line with different rights**, which the UART
  driver requires (sec.7.2: the `irq_wait` thread needs WAIT, the service thread needs
  SIGNAL to ring the TX doorbell). A refcounted capability gives this for free; an
  owner-thread-pointer registry cannot express it at all without inventing a second
  ownership concept.
- It duplicates machinery that already exists and is already documented as **additive**:
  `obj_ref_inc` / `obj_ref_drop` / `obj_close_protocol` are three switches whose comments
  say "additive: each new pool gains one arm" (`kernel/syscall/cap.cc:300`, `:160`, `:195`;
  `kernel/include/kickos/cap.h:141`). The Book already carries the recipe
  (`docs/book/adding-a-kernel-object-type-the-additive-recipe.md`).
- It needs a second, parallel teardown walk in `exit_current`, with its own ordering
  relationship to `cap_teardown` and `domain_release` -- a new invariant to get wrong. The
  capability route reuses the one walk that already exists.
- It leaves hole 1 (ungating) unsolved: a registry still needs a bespoke privilege story.
  A capability's *possession* IS the authorisation, checked at the one chokepoint
  (`cap_resolve_e`, `kernel/syscall/cap.cc:372`).

Cost of the capability route: 1 `CapType` enumerator, 1 pool + 1 `uint8_t` refcount array in
`Kernel`, 3 switch arms, 1 `cap_resolve_e` case, and the `IrqBinding` moves from a bump array
to a `SlotPool`. That is the entire delta. It is smaller than the registry.

### 2.2 The object

`kernel/include/kickos/cap.h` gains exactly one enumerator:

```c++
enum class CapType : uint8_t
{
    // CAP_EMPTY / CAP_SEM / CAP_MUTEX / CAP_ENDPOINT / CAP_REPLY / CAP_AUTHORITY unchanged
    CAP_IRQ // a tier-1 IRQ line binding; `obj` names a slot in the binding pool
};
```

`kernel/include/kickos/irq.h`:

```c++
// Trigger type of a tier-1 binding. EDGE (the M0.2..M4.5 contract) rearms by bare
// unmask -- a raise latched while masked redelivers, so no pulse is lost. LEVEL
// discards the latch first: after the driver clears the device, a still-asserted
// source re-latches on its own, and a deasserted one must stay quiet.
enum IrqTrigger : uint8_t
{
    IRQ_EDGE = 0,
    IRQ_LEVEL = 1
};

struct IrqBinding
{
    Semaphore sem;
    int line = -1;
    // Set only when an irq_wait RETURNS ... (existing comment, unchanged)
    bool needs_rearm = false;
    uint8_t trigger = IRQ_EDGE;
};
```

`used` is **deleted**: liveness moves to the pool's own `used_[]` bit, exactly as for
semaphores. `kernel/include/kickos/instance.h:98-100` becomes:

```c++
IrqEntry irq_table[KICKOS_MAX_IRQ];                    // line -> handler; ISR reads by index
SlotPool<IrqBinding, KICKOS_MAX_IRQ_HANDLES> irq_bindings;
uint8_t irq_refs[KICKOS_MAX_IRQ_HANDLES];              // parallel refcount, as sem_refs
```

`irq_binding_count` is deleted with the bump allocator.

**The latency invariant survives.** `irq_event_isr` is still handed `&binding` as its `arg`
(`kernel/irq/irq.cc:37-42`) -- a `SlotPool` slot has a stable address for its whole life
(`slots_[]` is a fixed array), so the ISR still does zero lookups. This is the same property
`irq_wait`'s "the binding is stable, so parking on its sem outside the lock is safe"
(`:175-177`) already relies on; it now rests on the pool rather than on "never freed".

### 2.3 Rights -- every bit has a real check

| Right | Meaning on an IRQ cap | Checked at |
|---|---|---|
| `CAP_WAIT` | may `kos_irq_wait` / `kos_irq_ack` (consume an event, rearm the line) | `cap_resolve_e`, IRQ type + `CAP_WAIT` |
| `CAP_SIGNAL` | may `kos_irq_notify` (software-post the binding's notification) | `cap_resolve_e`, IRQ type + `CAP_SIGNAL` |
| `CAP_TRANSFER` | may be delegated into a child table at spawn | the delegate site (unchanged) |

No dead field -- the house rule at `kernel/include/kickos/cap.h:59-63`.

`CAP_SIGNAL` is **not** "may raise the line at the controller". Raising at the controller
stays `KOS_SYS_IRQ_INJECT = 9`, privileged test scaffolding, untouched. `kos_irq_notify` is a
pure `sem_post` on the binding's notification -- see sec.2.6 for why that distinction is
load-bearing rather than fussy.

### 2.4 ABI

The ABI is unstable until M6, so `KOS_SYS_IRQ_REGISTER = 14` is **renamed in place**, not
deprecated alongside a replacement:

```c
KOS_SYS_IRQ_CLAIM  = 14, // (line, flags) -> CAP_IRQ cap handle, or -KOS_E*
                         //   EPERM (not privileged), EINVAL (line/flags), EBUSY (line owned),
                         //   ENOMEM (binding pool or cap table full)
KOS_SYS_IRQ_WAIT   = 15, // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT)
KOS_SYS_IRQ_ACK    = 16, // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT)
KOS_SYS_IRQ_NOTIFY = 36, // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks SIGNAL)
```

36 is the next free number (highest today is `KOS_SYS_REPLY = 35`,
`user/include/kickos/sys/abi.h:60`).

Claim flags (`user/include/kickos/sys/abi.h`, new):

```c
enum kos_irq_claim_flags
{
    KOS_IRQ_EDGE  = 0,      // default; latch-and-coalesce rearm (bare unmask)
    KOS_IRQ_LEVEL = 1 << 0  // rearm does clear_pending then unmask (sec.5)
};
```

Userspace C decls (`user/include/kickos/sys.h`, replacing `:149-154`):

```c
int kos_irq_claim(int line, unsigned int flags); // privileged-only; -> CAP_IRQ cap handle
int kos_irq_wait(int irq_cap);
int kos_irq_ack(int irq_cap);
int kos_irq_notify(int irq_cap);
```

`kos::Irq` (`user/include/kickos/kos.h:200-229`) gains the release path its own comment says
it lacks: the destructor calls `kos_handle_close(cap_)`, and it becomes move-only.

### 2.5 Kernel-side shape

`irq_register(int line)` becomes:

```c++
// Privileged mint. Claims `line` (one owner, no stealing), allocates a binding from the
// pool, and installs a full-rights CAP_IRQ into the caller's table. The line is left
// MASKED with needs_rearm set: the FIRST irq_wait arms it (sec.3.2) -- so no window exists
// in which the line is armed while its eventual owner is not yet running.
int irq_claim(Thread* c, int line, unsigned int flags);
```

Body, under one `IrqLock`:

1. Range-check `line`, reject unknown `flags` bits (`-KOS_EINVAL`).
2. `-KOS_EBUSY` if `irq_table[line].handler != irq_default_handler` (the existing one-owner
   test, `kernel/irq/irq.cc:124`).
3. `cap_has_free_slot(c)` probe FIRST, then `irq_bindings.alloc()`. Probe-before-allocate so
   a full cap table does not leak a binding slot -- the same discipline as the reply cap
   (`kernel/include/kickos/cap.h:146-149`). `-KOS_ENOMEM` on either.
4. `sem_init(&b->sem, 0)`; `b->line = line`; `b->needs_rearm = true`; `b->trigger` set to
   `IRQ_LEVEL` when the claim carries the level flag and to `IRQ_EDGE` otherwise (written as
   an `if`/`else`, not a ternary).
5. `irq_refs[idx] = 1`.
6. `irq_attach(line, irq_event_isr, b)`.
7. `arch_irq_clear_pending(line)` -- drop pre-claim garbage, as today (`:152`). **Do not
   `arch_irq_unmask`.** This is the change that kills the handover race (sec.3.2).
8. `cap_install` the binding handle `irq_bindings.handle_for(idx)` into `c`'s table with the
   IRQ cap type and rights `CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER`.

`irq_wait` / `irq_ack` take a cap handle and resolve it:

```c++
int irq_wait(Thread* c, int irq_cap)
{
    IrqBinding* b = nullptr;
    {
        IrqLock lock;
        int err = 0;
        b = static_cast<IrqBinding*>(
            cap_resolve_e(c, irq_cap, CapType::CAP_IRQ, CAP_WAIT, &err));
        if (b == nullptr)
        {
            return -err;
        }
        rearm_locked(b);   // sec.5
    }
    sem_wait(&b->sem);
    {
        IrqLock lock;
        b->needs_rearm = true;
    }
    return 0;
}
```

The three cap-machinery arms:

- `obj_ref_inc`, IRQ arm: `irq_refs[irq_index_of(obj)]++`. Ignores `rights` (there is no
  recv-holder analogue).
- `obj_close_protocol`, IRQ arm: **returns 0**, no protocol. It never refuses a voluntary
  close (unlike the mutex R2 rule) and it wakes nobody -- see sec.4.2 for why that is right.
- `obj_ref_drop` -> `irq_ref_drop(obj, teardown)`: sec.4.1.
- `cap_resolve_e`: one more `else if` arm, resolving the IRQ type through
  `kernel().irq_bindings.resolve(e->obj)`.

`cap_resolve_e`'s existing two-level guard now covers IRQ bindings for free: the per-task
cap-gen, then the pool's object-gen. A stale IRQ cap naming a recycled binding slot fails
to resolve rather than aliasing a live line -- the ABA guard the bump allocator could never
have.

### 2.6 Why `kos_irq_notify` and not `kos_irq_inject`

The two-thread UART driver (sec.7.2) needs the service thread to wake the `irq_wait` thread
on the TX ring's idle -> busy transition. Three candidates:

| Candidate | Verdict |
|---|---|
| `kos_irq_inject(line)` -- raise at the controller | **REJECTED.** `arch_irq_inject` on RX **refuses** any real vector: "A real peripheral line cannot be pended from software on RX" (`arch/rx/rxv3/arch_rxv3.cc:760-765`). The forcing consumer is RX72M SCI6, vector 87. Dead on arrival. |
| A PI mutex around the ring, producer primes the peripheral itself | **REJECTED.** Makes the IRQ-servicing thread take a lock a lower-priority producer holds -- a priority-inversion surface on the drain path, bounded only by PI. And it makes the ring's `tail` two-writer, losing SPSC. |
| `kos_irq_notify(irq_cap)` -- `sem_post` the binding's notification | **CHOSEN.** Pure kernel-object operation, zero arch surface, works identically on all 13 chips and on `sim`. Keeps `tail` single-writer (the producer never touches the peripheral or `tail`). |

The consequence, which must be written into the contract: **a wake from `irq_wait` is
advisory, not one-per-hardware-event.** A driver's IRQ loop MUST re-read the device status
on every wake and be idempotent about finding nothing. That is already how every existing
precedent is written (`user/apps/frdmk64f/k64drv/main.cc:71-90` etc.), and the
phantom-wake defense (`needs_rearm` set on wait-RETURN, never in the ISR,
`kernel/include/kickos/irq.h:37-42`) exists precisely because spurious wakes were always
possible. `kos_irq_notify` makes an existing tolerated condition into a documented one.

One real interaction to state: a notify-driven `irq_wait` return sets `needs_rearm = true`,
so the next `irq_wait` unmasks a line that may never have been masked. `arch_irq_unmask` on
an already-unmasked line is idempotent on all five backends (NVIC `ISER` write-1-to-set;
software-bitmap clear; RX `IER` bit-set). Safe. On a LEVEL binding the rearm additionally
calls `arch_irq_clear_pending` -- also safe, since a genuinely asserted level source
re-latches immediately (sec.5).

---

## 3. Decision 2 -- handover at spawn, with no unowned-armed window

### 3.1 The race as it stands

Handing the kernel console TX line to a userspace driver is today a **three-step across a
privilege boundary**:

```
root (privileged):  kos_console_publish(ep)
                      -> console_tx_deinit()            [kernel/init/console_tx.cc:249-258]
                           flush_sync; irq_disable; irq_detach(line); armed = false
                    kos_spawn(driver, ...)
driver (unpriv):    kos_irq_register(line)              [line is unowned in between]
```

Between `irq_detach` and the driver's `irq_register` the line is **unowned**. It is left
masked by `irq_detach` (`kernel/irq/irq.cc:110`), so no interrupt is lost or stormed -- but
the line is claimable by *any* unprivileged thread in that window, because
`KOS_SYS_IRQ_REGISTER` is ungated (hole 1). A second unprivileged thread that claims line 31
first wins, and the real console driver gets `-KOS_EBUSY` and cannot start.

### 3.2 The fix: claim-masked, arm-on-first-wait

Two changes, both already stated above:

- `kos_irq_claim` is **privileged-only**. An unprivileged thread cannot claim any line, ever.
  Hole 1 closed. There is no longer a race to lose.
- `kos_irq_claim` mints the binding **masked**, with `needs_rearm = true`. The line is armed
  by the **first `irq_wait`**, which by construction runs in the thread that will consume the
  event.

That gives the two properties the gate asks for, as a pair of invariants:

> **INVARIANT H1 (no armed-and-unowned).** From `arch_irq_unmask` to `irq_detach` a line has
> exactly one live `IrqBinding`. `irq_claim` does not unmask; the only unmask is
> `rearm_locked`, reachable only through `irq_wait`/`irq_ack` on a resolved IRQ cap; the
> only mask-and-detach is `irq_ref_drop` at refs -> 0. A line therefore transitions
> unowned-masked -> owned-masked -> owned-armed -> owned-masked -> unowned-masked. It is
> never armed-and-unowned.

> **INVARIANT H2 (no running-driver-with-kernel-attached-line).** `console_tx_deinit` runs
> `irq_detach` under one `IrqLock` (`kernel/init/console_tx.cc:249-258`), and
> `kos_console_publish` calls it BEFORE flipping to `USER_OWNED`
> (`kernel/syscall/syscall.cc:273-285`). `irq_claim` then fails `-KOS_EBUSY` if the line is
> still kernel-attached. So the ordering is *enforced by the EBUSY check*, not by
> convention: root cannot claim the console TX line until the kernel has genuinely let go.

The corrected bring-up sequence, all of it in privileged `start()`:

```
1. kos_console_publish(ep)     // console_tx_deinit: flush, irq_disable, irq_detach, disarm
2. tx_cap = kos_irq_claim(TX_LINE, KOS_IRQ_EDGE)    // EBUSY-guarded against step 1
3. rx_cap = kos_irq_claim(RX_LINE, ...)             // if the chip has a separate RX line
4. spawn_driver(...)  delegating { ep(WAIT), tx_cap(WAIT|SIGNAL), rx_cap(WAIT), ... }
```

Steps 2-3 are the *only* new syscalls in the sequence. Step 1 is unchanged. No new kernel
handover primitive is needed -- which is the payoff of Decision 1.

### 3.3 Delegation: extend `spawn_unprivileged`, do not invent

`user/include/kickos/sys/driver_bringup.h`'s `spawn_unprivileged` grants exactly
`{ep, KOS_CAP_WAIT}` plus one MMIO window. It needs two extensions, both plumbing over
machinery that already exists (spawn already delegates a cap list into child indices from
`KOS_SPAWN_DELEGATED_CAP0 = 1`):

```c++
struct kos_driver_cap
{
    int cap;         // parent's cap handle
    unsigned rights; // subset of the parent's rights to delegate
};

struct kos_driver_spawn
{
    void (*entry)(void*);
    void* arg;
    char const* name;
    uint8_t prio;
    uintptr_t win_base;      // MMIO window (exact, encodable, never rounded)
    size_t win_size;
    void* shared_base;       // ring block, shared by the driver's threads (sec.7.1)
    size_t shared_size;
    kos_driver_cap const* caps;  // delegated into child indices 1..n
    size_t cap_count;
    char const* fail_tag;
};

int spawn_driver(struct kos_driver_spawn const* p);
```

`spawn_unprivileged` stays as the one-cap convenience wrapper so the two landed SPI services
(`system/init/service_list_frdmk64f.cc`, `service_list_xmc4800relax.cc`) do not churn.

**Two domain-model facts checked, and one budget consequence.** A spawn carrying an MMIO
window always takes a **fresh** domain slot -- `domain_for` skips the share loop when
`has_mmio` (`kernel/domain/domain.cc:144-147`, "An MMIO grant is a capability -- never
shared"). That dedup rule is about *slot reuse*, not about access: two threads spawned with
the same `win_base` and the same `shared_base` each get their own `Domain` whose region set
contains both regions, so **both threads can reach the shared ring block and the peripheral.
No kernel domain change is required for the two-thread driver.** The cost is 2 domain slots
instead of 1; `KICKOS_MAX_DOMAINS` defaults to `KICKOS_MAX_THREADS + 2 = 18`
(`kernel/include/kickos/config/system.h:60-61`), so there is headroom.

The shared ring block must satisfy the RAM arm of `grant_region_admissible`: naturally
aligned and confined to the user arena, **for every caller including privileged ones**
(`docs/design-m4-driver-model.md` rule 7). So it is one `kos_ram_alloc`'d, power-of-two,
naturally-aligned block holding both rings plus the `Uart` state (sec.7.1).

### 3.4 Rejected alternative: a kernel-side "transfer" syscall

`kos_irq_transfer(irq_cap, thread_handle)` -- move a binding's ownership to an
already-running thread. Rejected: it needs a *second* delegation mechanism alongside the
spawn-time cap list, it needs the target thread's cap table to have a free slot at an
arbitrary moment (a failure mode with no good recovery), and it is the runtime
mint-and-delegate feature that `docs/design-driver-era-scope.md` already **deferred for want
of a forcing consumer**. Spawn-time delegation covers every M4.6 and M4.7 case, because
drivers are spawned by the service list with their resources already in hand.

---

## 4. Decision 3 -- teardown on driver death

Three death paths must converge on one state: **line masked and unowned, binding slot back
in the pool, no resurrectable waiter, console still able to print.**

| Path | Route to teardown |
|---|---|
| `kos_exit` / return from entry | `sched::exit_current` -> `cap_teardown` (`kernel/sched/sched.cc:193`) |
| fault-kill (MPU / bus / illegal instruction) | the fault handler kills the **current** thread, i.e. the same `exit_current` |
| `kos_handle_close(irq_cap)` (voluntary release) | `handle_close` -> `obj_close_protocol` + `obj_ref_drop` (`kernel/syscall/cap.cc:506-527`) |

Note what is **not** a path: a thread parked in `irq_wait` cannot be killed by a third party.
KickOS faults kill the running thread, and there is no `kos_kill`. So "the binding is freed
while its waiter is parked" is not reachable in M4.6 -- but it is guarded anyway (sec.4.2),
because it becomes reachable the moment a kill primitive lands.

### 4.1 `irq_ref_drop`

```c++
// Drop one reference to IRQ binding `obj_handle`; release at refs -> 0. Same accounting
// shape as sem_ref_drop, same leak-don't-strand guard. DETACH BEFORE FREE is load-bearing:
// irq_event_isr holds the binding's ADDRESS as its pre-bound arg, so the slot must stop
// being reachable from the dispatch table before it returns to the pool.
void irq_ref_drop(int obj_handle, bool teardown)
{
    int const idx = irq_index_of(obj_handle);
    if (idx < 0)
    {
        return;
    }
    uint8_t& r = kernel().irq_refs[idx];
    if (r > 0)
    {
        r--;
    }
    if (r == 0)
    {
        IrqBinding* b = kernel().irq_bindings.resolve(obj_handle);
        if (b != nullptr and not b->sem.waiters.empty())
        {
            KICKOS_ASSERT(teardown); // refs->0 with a waiter parked is unreachable via close
            r = 1;                   // leak, never strand
            return;
        }
        if (b != nullptr)
        {
            irq_detach(b->line); // restores the null-object default AND masks the line
        }
        kernel().irq_bindings.free(obj_handle);
    }
}
```

Each requirement, discharged:

- **Line ends masked and unowned, re-registrable.** `irq_detach` already does exactly this:
  `set_default(irq); arch_irq_mask(irq);` under `IrqLock` (`kernel/irq/irq.cc:102-111`).
  Restoring the null-object default is what makes a later `irq_claim` pass its `-KOS_EBUSY`
  test.
- **Binding slot returns to the pool.** `SlotPool::free` bumps the slot generation, so every
  outstanding IRQ cap naming it stops resolving. A stale cap in another thread's table is
  now inert rather than dangerous -- the property the bump allocator lacked.
- **Detach strictly before free.** `irq_detach` takes `IrqLock`; an ISR cannot be in flight
  once it returns (the ISR runs to completion and cannot be preempted by the lock holder).
  After detach, no dispatch-table entry references the slot, so freeing it cannot hand a
  live ISR a recycled binding.
- **A parked `irq_wait` must not resurrect.** The leak-never-strand guard mirrors
  `endpoint_ref_drop` verbatim (`kernel/syscall/cap.cc:131-154`): floor `refs` at 1 rather
  than free a binding with a parked waiter. Unreachable today (a parked waiter pins its own
  cap, so `refs >= 1`, and a parked thread cannot self-exit) -- kept as defence-in-depth and
  as the correct behaviour once a kill primitive exists.

### 4.2 Why the IRQ arm of `obj_close_protocol` is empty

The endpoint arm EPIPEs parked senders when the last receiver goes (`:246-260`); the reply
arm EPIPEs the parked caller (`:264-288`). Should the IRQ arm wake a parked waiter with
`-KOS_EPIPE`?

**No.** A parked `irq_wait` waiter is, by the argument above, always the holder of a
reference -- so the only way to reach refs -> 0 with a waiter parked is a *kill*, which does
not exist. Adding a wake arm now would be a code path with no reachable trigger and no test,
i.e. exactly the "raise a limit to hide a hole" smell inverted. The leak-never-strand guard
in `irq_ref_drop` is the honest placeholder: it asserts in debug if the unreachable case ever
becomes reachable, which is the signal to add the arm **together with** the kill primitive
that made it reachable.

### 4.3 In-flight TX on driver death: **DROP, with accounting**

Decision: **the driver's TX ring contents are LOST when the driver dies, and the loss is
counted, not hidden.**

Why not drain: the ring lives in the driver's own domain, whose backing region is released by
`domain_release` in the same `exit_current` (`kernel/sched/sched.cc:194`). The kernel does not
know the ring's layout (it is userspace policy, deliberately -- sec.1), and reading a dying
domain's memory to poke a peripheral the kernel no longer owns inverts the whole isolation
argument. Draining is not merely hard, it is wrong.

Why not hand back to the kernel ring: covered in sec.4.4.

Accounting: the driver's `kos_uart_stats` (sec.7.6) lives in the shared ring block, which is
arena memory allocated by privileged bring-up -- so it OUTLIVES the driver thread and a
supervisor can read the final `tx_dropped` after a restart. That is the difference between
dropping and hiding.

### 4.4 The console ownership state machine on driver death

`kernel/init/console.cc:47-53` has `KERNEL_OWNED -> USER_OWNED -> RECLAIMED`, and `RECLAIMED`
is currently reached only by `kpanic_enter` (`:215-223`, via `arch_console_reclaim`).

**Decision: driver death lands the console in `RECLAIMED` (polled-only), via a new
`exit_current` hook that runs AFTER `cap_teardown`.**

```c++
// kernel/init/console.cc
// The published console endpoint has lost its last WAIT-bearing cap: no userspace driver
// can ever receive again. Take the UART back to a known polled state so panic -- and
// ordinary kprintf -- still reach the wire. Idempotent.
void console_on_driver_death(void);
```

Mechanism, in two pieces:

1. In `obj_close_protocol`'s existing `CAP_ENDPOINT` arm, at the point where
   `recv_holders` reaches 0 (`kernel/syscall/cap.cc:249`), additionally set a kernel flag if
   `e.obj` is the currently published console target. `cap_console_publish` already stores
   that handle (`:589-593`), so no new identity tracking is needed -- and keying on
   *recv_holders reaching 0* rather than on a thread identity means a **multi-threaded driver
   reclaims only when its LAST receiver dies**, which is exactly right for the two-thread
   shape.
2. `sched::exit_current` calls `console_on_driver_death()` **after** `cap_teardown` and
   before `domain_release`. `console_on_driver_death` checks the flag and, if set, runs
   `arch_console_reclaim(); g_console_state = RECLAIMED;`.

**Ordering is the whole point.** Doing the reclaim inside a cap arm would run it at an
arbitrary index in `cap_teardown`'s loop (`:531-546`) -- possibly while the driver's IRQ cap
is still live and the TX line still armed, so `arch_console_reclaim` would re-init a UART
whose interrupt can still fire into `irq_event_isr` and post a dying thread's semaphore.
Running it after the whole loop guarantees: every IRQ cap dropped -> every line masked and
detached -> *then* the device is re-initialised. Deterministic.

`arch_console_reclaim` falls back to a no-op (`arch/common/arch_console_reclaim_default.cc`)
and is defined today only by **mk64f** (`chip_mk64f.cc:739`) and **xmc4800**
(`usic_uart.cc:160`). Those are exactly the two boards that ship a console service list, so
M4.6's targets are covered; every other chip degrades to "console goes dark after driver
death", which is honest and unchanged from today. Adding the definition per chip is M4.7 fleet
work, listed in sec.8.

Rejected alternative: **re-arm the kernel TX ring (back to `KERNEL_OWNED`).** Tempting --
the line is free after teardown, and `console_buffer_init`'s body would mostly work. Rejected
because the dead driver may have left the UART in an arbitrary state (baud reprogrammed, `TE`
cleared, FIFO thresholds changed, in the C6 case `UART_INT_ENA` bits set). `arch_console_reclaim`
exists *precisely* to re-establish a known polled state and is already written per chip;
re-arming the ring would additionally need a per-chip "restore the ring's assumptions" step
that does not exist. `RECLAIMED`/polled is the smaller, already-implemented, honest answer.
Ring re-arm is a legitimate M4.7+ improvement once `arch_console_reclaim` exists fleet-wide.

One doc fix: `console.cc:51`'s comment calls `RECLAIMED` "panic forcibly took the UART back".
It becomes "the kernel forcibly took the UART back (panic, or driver death)".

---

## 5. Decision 4 -- level-triggered bindings

`TODO.md:292-301` already specifies this; M4.6 is its first consumer, so it lands here.

The whole change is one helper, called from both `irq_wait` (on entry) and `irq_ack`:

```c++
// Rearm the line a previous wait consumed. Caller holds IrqLock.
// EDGE: bare unmask -- a raise latched while masked redelivers (no pulse lost).
// LEVEL: discard the stale latch FIRST. The driver has already cleared the device by the
// time it waits/acks, so a latch surviving from before that clear would phantom-wake the
// next wait; a genuinely still-asserted source re-latches on its own after the clear.
void rearm_locked(IrqBinding* b)
{
    if (not b->needs_rearm)
    {
        return;
    }
    b->needs_rearm = false;
    if (b->trigger == IRQ_LEVEL)
    {
        arch_irq_clear_pending(b->line);
    }
    arch_irq_unmask(b->line);
}
```

`arch_irq_clear_pending` was added with the coalesce fix and is documented as "reserved for
the M4 level-trigger rearm path" (`arch/include/kickos/arch/arch.h:371-375`). This is that
path. No new arch seam.

### 5.1 Who clears the source: **always the driver, never the kernel**

The kernel masks at the **controller**; the driver clears at the **peripheral**. This is not
a style preference, it is the trust boundary: the peripheral block is *granted* to the
driver, so a kernel write into it would split ownership across the privilege boundary and
break the single-owner whole-block rule (`docs/design-m4-driver-model.md` rule 1). The prior
RX spike reached the same conclusion (`c296feb` sec.5, "Level-clear ownership") and it is
adopted here as a general rule:

> **RULE L1.** The kernel's mask for a tier-1 line targets a register the KERNEL owns. The
> driver's clear targets a register the DRIVER owns. If a chip offers no kernel-owned mask
> for a source, that source cannot be a tier-1 binding on that chip (sec.8 records where).

Per-arch consequences of L1, which sec.8 tabulates:

- **ARM NVIC** -- `ICER` is in the PPB, kernel-owned, per-line. Clean.
- **RX72M dedicated vectors** -- `ICU.IER` bit, kernel-owned (and the ICU is already a
  reserved block, `chip_rx72m.cc:294-312`). Clean.
- **RX72M GROUPBL0 sources** -- `GENBL0.ENj`, kernel-owned, per-source. Clean, **once the ICU
  reserved block is extended** (sec.6.4).
- **ESP32-C6** -- the natural per-source mask is `UART_INT_ENA`, which lives inside the
  **granted UART block**. L1 forbids the kernel touching it. So the C6 mask must be the
  **PLIC `MXINT_ENABLE` bit for the device CPU int** -- kernel-owned, but coarse: it masks
  every source routed to that CPU int. Recorded as an accepted coarseness in sec.8, with the
  consequence that the C6 UART is a single grouped line (sec.6.2), not three.

### 5.2 The bulk-rearm hazard: **not triggered**

`TODO.md:302-310` and `arch.h:356-362` warn that the software backends carry a coalesced
redelivery through ONE shared cell plus ONE doorbell, so **at most one unmask-with-pending
may occur per `IrqLock` region**.

This design does not violate it, and the reason is worth stating so the reviewer does not
have to reconstruct it:

- `rearm_locked` unmasks **exactly one** line, and its only callers are `irq_wait` and
  `irq_ack`, each of which holds its own `IrqLock` for one binding. Unchanged from today.
- The grouped-line demux (sec.6) calls `kickos_isr_irq` in a **loop**, so several
  `irq_event_isr` invocations may run in one ISR -- but each of those **masks**, and masking
  does not use the doorbell. The hazard is on unmask only.
- `kos_irq_notify` does not touch the controller at all.

So `TODO.md:302-310` stays correctly deferred; M4.6 does not force the identity-free
dispatcher. If a future design adds a genuine bulk rearm, that TODO is its prerequisite.

---

## 6. Decision 5 -- shared and grouped lines

### 6.1 The model: one logical line per logical SOURCE

**A tier-1 line is a logical SOURCE, not a physical vector. The chip owns the
vector -> source(s) demux, in a first-level ISR that LOOPS.**

Two consequences:

- One line still has exactly one binding and one owner (`-KOS_EBUSY` unchanged). **Several
  subscribers on one line is not supported, deliberately.** If two drivers need two sources
  of one physical vector, the chip demuxes them into two logical lines. If two consumers
  genuinely need the *same* source, that is a service with two clients, not two bindings --
  the class/service duality (`docs/design-m4-driver-model.md`) already answers it.
- The existing substrate already has the shape `docs/design-driver-era-scope.md:645-648`
  demands for a shared-line notification -- **sticky-pending** (the binding's counting
  semaphore coalesces), **ack before delivery** (`irq_event_isr` masks *then* posts,
  `kernel/irq/irq.cc:37-42`), and **never a parked rendezvous send** (`sem_post` cannot
  block). No new notification object is needed.

### 6.2 The chip dispatch hook, unified across arches

RISC-V already has the right shape: `kickos_rv_ext_dispatch_dev(void)`, a chip-provided void
function that decides what to post (`arch/riscv/rv32imac/arch_rv32imac.cc:521`, overridden at
`chip_esp32c6.cc:438`). RX has the **wrong** shape:
`int kickos_rx_dev_pending_line(void)` returns ONE line
(`arch/rx/rxv3/arch_rxv3.cc:842`) and therefore **cannot express a group vector with several
`ISj` bits set at once** -- which the TRM says happens.

**Decision: replace the RX single-line hook with the void-dispatch shape, matching RISC-V.**

```c++
// arch/rx/rxv3 -- chip device-dispatch hook. Called from the shared first-level ISR for
// every INTB device slot. The chip reads its own status registers and calls
// kickos_isr_irq() ONCE PER ASSERTED SOURCE (a group vector may assert several at once).
// It also owns the per-source clear discipline: edge sources clear ICU.IR[vector]; level
// group sources must NOT (writing IR is futile) -- see RULE L1.
// The fallback TU does nothing (build-only posture: no real device routed).
void kickos_rx_dev_dispatch(void);
```

`kickos_rx_default_irq` (`arch_rxv3.cc:854-867`) collapses to `g_in_isr++;
kickos_rx_dev_dispatch(); g_in_isr--;`. The **edge-style `reg8(ICU_IR_BASE + line) = 0` at
`:863` moves into the chip hook**, where it belongs: it is correct for a dedicated edge
vector and **wrong for a level group source** (the level source re-asserts it immediately),
which is the bug the prior spike found (`c296feb` sec.1c/3.2) and which the current generic
placement bakes in for every future line.

This makes the fleet-wide rule uniform: **every arch's real-device first-level entry is a
chip-provided dispatch function that may post 0..N logical lines and owns its own clear
discipline.**

The ESP32-C6 needs the *same* generalization for the *same* reason, and this is a finding
worth flagging: **all UART0 sub-interrupts on the C6 share ONE interrupt-matrix source and
ONE `UART_INT_ST` register**, so `kickos_rv_ext_dispatch_dev` -- which today hard-codes
`kickos_isr_irq(UART0_TX_LINE)` (`chip_esp32c6.cc:438-443`) -- must become a real demux
reading `UART_INT_ST` and posting per sub-source. **The C6 UART and the RX72M GROUPBL0 are
the same shape**: one physical vector, N logical sources, level semantics, a status word to
loop over. Designing for one gets the other.

### 6.3 RX72M GROUPBL0 concretely

Carried forward from the prior spike (`c296feb` sec.1c and sec.2), re-verified against the
tree, TRM citations to be confirmed by the review checklist in sec.9:

| Group | Vector | IER | Status | Enable |
|---|---|---|---|---|
| GROUPBL0 | 110 | `IER0D.IEN6` | `GRPBL0 = 0x00087630` | `GENBL0 = 0x00087670` |
| GROUPBL1 | 111 | `IER0D.IEN7` | `GRPBL1 = 0x00087634` | `GENBL1 = 0x00087674` |
| GROUPBL2 | 107 | -- | `GRPBL2 = 0x00087638` | `GENBL2 = 0x00087678` |
| GROUPAL0 | 112 | `IER0E.IEN0` | `GRPAL0 = 0x00087830` | `GENAL0 = 0x00087870` |
| GROUPAL1 | 113 | -- | `GRPAL1 = 0x00087834` | `GENAL1 = 0x00087874` |

SCI6's four sources span **two routing classes at once**:

| Source | Meaning | Routing | Address | Trigger |
|---|---|---|---|---|
| `RXI6` | receive-data-full | dedicated vector | **86**, `IER0A.IEN6`, `IPR086` | edge |
| `TXI6` | transmit-data-empty | dedicated vector | **87**, `IER0A.IEN7`, `IPR087` | edge |
| `TEI6` | transmission-end | GROUPBL0 | `GRPBL0.IS12` / `GENBL0.EN12` | **level** |
| `ERI6` | receive error (ORER/FER/PER) | GROUPBL0 | `GRPBL0.IS13` / `GENBL0.EN13` | **level** |

`arch/rx/chip/rx72m/irq.h:29-33` currently has `SCI6_TEI = -1, SCI6_ERI = -1` and states
plainly they are "NOT implemented or referenced anywhere in the tree yet". M4.6 implements
them.

**Line-number allocation.** RX's `arch_irq_*` splits at `SOFT_IRQ_LINES = 32`
(`arch_rxv3.cc:96`): `< 32` = software-inject logical lines, `>= 32` = real ICU vectors,
`IER`-gated via `icu_ier_set` (vector-indexed) and `IPR` via `vector_to_ipr`. `rx72m` sets
`KICKOS_MAX_IRQ = 256` (`boards/rx72m/include/kickos/board_config.h:16`), so:

- `RXI6 = 86` and `TXI6 = 87` fit the existing `>= 32` real-vector path with **no seam
  change** -- `vector_to_ipr` is identity for both.
- `TEI6` / `ERI6` do **not**. Their "line" is a group SOURCE, not the group VECTOR 110;
  masking is `GENBL0.ENj`, not `IER[src>>3]`. **Decision: allocate group sources a dedicated
  logical-line range above the vector space**, based at 256:

  ```c++
  // arch/rx/chip/rx72m -- group sources are logical lines above the ICU vector space.
  enum { KICKOS_RX_GROUP_LINE_BASE = 256 };
  // line = KICKOS_RX_GROUP_LINE_BASE + group_index * 32 + bit
  ```

  So `TEI6 = 256 + 0*32 + 12 = 268` and `ERI6 = 269`. `arch_irq_mask/unmask/clear_pending`
  gain a third branch for `line >= 256` that toggles `GENBLn.ENj` and no-ops `clear_pending`
  (level: clearing is the driver's peripheral write, RULE L1). `KICKOS_MAX_IRQ` on rx72m
  rises to 416 (256 + 5*32).

  Rejected alternative: **reuse low logical lines `< 32` for group sources.** Rejected
  because `< 32` is the software-inject space, and overlapping the two would make
  `arch_irq_inject` able to fake a real level source -- exactly the kind of aliasing that
  makes a test pass while the hardware path is broken.

  Cost check: `irq_table[KICKOS_MAX_IRQ]` is `IrqEntry{handler, arg}` = 8 bytes on RX, so 416
  lines = 3328 B, up from 2048 B. +1280 B of `.bss` on a 4 MB-flash / 1 MB-RAM part. Acceptable;
  flagged in sec.11 as a decision the reviewer may want to trade against a sparse map.

### 6.4 The `arch_reserved_blocks` finding (carried forward, still true)

Verified at HEAD: `arch_reserved_blocks` in `arch/rx/chip/rx72m/chip_rx72m.cc:294-312`
reserves `{mmap::ICU, 0x400}` = `0x87000..0x873FF`. That covers `IR`/`IER`/`IPR` but **NOT**
the group registers the demux will use (`GRPBL0 0x87630`, `GENBL0 0x87670`,
`GRPAL0 0x87830`, `GENAL0 0x87870`).

**Required before the demux touches them:** extend the ICU reserved block to span
`0x87000..0x8787F` (size `0x880`), so the Rule-7 grant predicate mechanically refuses to hand
any of it to a driver and the SYSMPU/MPU union stays correct. Cheap; lands in the same commit
as the group demux. This is a **latent correctness hole today**, not merely a future
requirement: a privileged over-broad grant covering `0x8763x` would currently succeed.

---

## 7. The UART layer -- policy on top of the mechanism

### 7.1 A userspace SPSC byte ring, factored out

There is **no generic ring anywhere in the tree**: only the kernel's file-static
`ConsoleTxRing` (`kernel/init/console_tx.cc:29-45`) and the RTT rings. M4.6 factors one out
for userspace, because it needs two instances (TX and RX) and M4.7 will need more.

A new `byte_ring.h`, in the public `sys` include directory next to `bus.h`:

```c
// Publication barrier for head/tail: KICKOS_CONSOLE_TX_BARRIER generalised out of the
// kernel ring. Compiler-only by default, a real fence on a weakly-ordered core.
#define KOS_RING_BARRIER() /* per-arch seam, exactly as console_tx.h:26-28 */

// Single-producer / single-consumer byte ring. `size` MUST be a power of two; usable
// capacity is size-1 (one slot reserved so head==tail is unambiguously empty).
// head is written ONLY by the producer, tail ONLY by the consumer -- that is what makes
// this lock-free. A second writer of either index breaks it; see the two-thread driver
// contract (sec.7.2) for which thread owns which index.
struct kos_byte_ring
{
    unsigned char* buf;
    uint32_t size;
    uint32_t mask;
    volatile uint32_t head;
    volatile uint32_t tail;
};

void   kos_byte_ring_init(struct kos_byte_ring* r, unsigned char* buf, uint32_t size);
uint32_t kos_byte_ring_used(struct kos_byte_ring const* r);
uint32_t kos_byte_ring_space(struct kos_byte_ring const* r);
// Producer side. Returns bytes accepted (< n on a full ring -- the caller decides policy).
uint32_t kos_byte_ring_push(struct kos_byte_ring* r, unsigned char const* src, uint32_t n);
// Consumer side. Returns bytes removed.
uint32_t kos_byte_ring_pop(struct kos_byte_ring* r, unsigned char* dst, uint32_t n);
// Consumer side, one byte, for the drain loop that must check slot_free per byte.
int    kos_byte_ring_pop_one(struct kos_byte_ring* r, unsigned char* out);
```

POD plus free functions, not a template -- there is exactly one element type, and the
template-discipline rule reserves templates for owning multi-type containers. It publishes
`head`/`tail` per operation through the ring barrier declared above, the same seam as
`console_tx.h:26-28` generalised out of the kernel ring: compiler-only by default and a real
fence on a weakly-ordered core.

**Placement.** Both rings plus the `Uart` state live in ONE power-of-two, naturally-aligned
block from `kos_ram_alloc`, granted as the driver threads' shared data region (sec.3.3).
Sizing: TX 512 B (matching the RX72M `CONSOLE_TX_SIZE = 512`, itself chosen `>` kprintf's
256 B buffer), RX 256 B, state ~64 B -> one 1 KiB block.

### 7.2 The two-thread driver, and why one thread cannot work

**An IRQ-driven UART service needs two threads.** One thread cannot block on both
`kos_recv` (client requests) and `kos_irq_wait` (device events) -- KickOS has no
receive-from-either primitive. This is the single most important structural fact in the UART
design and the reviewer should test it first.

```
        client ---kos_call--->  [ uart_service_thread ]  parks in kos_recv(ep)
                                   |  push TX ring (owns head)
                                   |  pop  RX ring (owns tail)
                                   |  kos_irq_notify(tx_cap)   <-- the doorbell
                                   v
                              [ shared ring block ]
                                   ^
                                   |  pop  TX ring (owns tail)
                                   |  push RX ring (owns head)
        device --IRQ-->        [ uart_irq_thread ]      parks in kos_irq_wait(irq_cap)
                                   |  owns EVERY peripheral register
```

The ownership split is what keeps both rings strict SPSC with no lock:

| Resource | Sole writer |
|---|---|
| TX ring `head` | service thread |
| TX ring `tail` | IRQ thread |
| RX ring `head` | IRQ thread |
| RX ring `tail` | service thread |
| every peripheral register | **IRQ thread only** |

That last row is the one that differs from the kernel's ring, and it is why
`kos_irq_notify` exists. The kernel's `console_tx_write` primes the pump by writing the first
byte to the data register itself and advancing `tail` (`kernel/init/console_tx.cc:150-158`),
which it can do because it holds `IrqLock`. The service thread has no such lock, so it must
**not** touch `tail` or the peripheral. Instead, on the idle -> busy transition it calls
`kos_irq_notify(tx_cap)`; the IRQ thread wakes, sees a non-empty TX ring, enables the
peripheral TX interrupt and pushes the first byte. Identical net effect, single-writer
preserved, and it works on the transition-triggered parts (XMC `TBIEN`, RX `SCR.TIE`) where
merely enabling the interrupt on an idle channel raises nothing.

Both threads are spawned by the same privileged `start()` with the same `win_base` and the
same `shared_base` (sec.3.3), so they see the same peripheral and the same rings. Priorities:
the IRQ thread strictly **above** the service thread (device drain must preempt request
serving), and both above their clients.

### 7.3 The `Uart` class concept

Mirroring the implicit `Bus` concept in `user/include/kickos/sys/spi_service.h`, and obeying
the 1:1 rule from `docs/design-m4-driver-model.md` ("the service request protocol MUST be a
1:1 serialization of the class methods"):

```c++
// The Uart concept. A class-driver instance owns one UART peripheral + its two rings.
// It owns NO thread and NO endpoint (design-m4-driver-model.md "What the class API must
// NOT assume") -- the two service threads call into it from opposite sides.
struct Uart
{
    // --- called from the service thread ---
    uint32_t configure(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);
    uint32_t write(unsigned char const* buf, uint32_t len);  // -> bytes queued; may be < len
    uint32_t read(unsigned char* buf, uint32_t len);         // -> bytes dequeued; may be 0
    void     stats(struct kos_uart_stats* out) const;
    bool     tx_idle() const;

    // --- called from the IRQ thread ---
    void     service_irq();   // one pass: demux status, drain TX, fill RX, clear the device
};
```

`write` and `read` are non-blocking by construction: they move bytes between the caller and
a ring and return a count. Every blocking decision is the *service*'s (sec.7.5), never the
class's -- which is what lets the same class be linked inline by a consumer that wants no
IPC at all.

`service_irq()` is the userspace twin of `console_tx_isr` and the one method that touches the
device. Its body, per chip, is: read the status register once, then for each asserted
condition do the peripheral work and clear that condition. It must be **idempotent about
finding nothing asserted**, because `kos_irq_notify` wakes it without a hardware event
(sec.2.6).

### 7.4 The wire ABI

A new `uart.h`, in the public `sys` include directory alongside `bus.h`:

```c
enum kos_uart_op
{
    KOS_UART_CONFIGURE = 0,
    KOS_UART_WRITE = 1,
    KOS_UART_READ = 2,
    KOS_UART_STATS = 3
};

struct kos_uart_req
{
    uint8_t  op;
    uint8_t  flags;      // KOS_UART_F_* (see sec.7.5)
    uint16_t len;        // WRITE: payload bytes that follow; READ: bytes requested
    uint32_t baud;       // CONFIGURE only
    uint8_t  data_bits;  // CONFIGURE only
    uint8_t  parity;
    uint8_t  stop_bits;
    uint8_t  reserved;
};

struct kos_uart_rsp
{
    int32_t  status;     // 0 or -KOS_E*
    uint16_t len;        // bytes written / bytes returned
    uint16_t reserved;
};

struct kos_uart_stats
{
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t tx_dropped;   // ring full at write time
    uint32_t rx_dropped;   // ring full at IRQ time (software overrun)
    uint32_t rx_overrun;   // hardware overrun flag seen (ORER / OR / RXFIFO_OVF)
    uint32_t rx_framing;   // FER / FE / FRM_ERR
    uint32_t rx_parity;    // PER / PF / PARITY_ERR
    uint32_t irq_wakes;    // total irq_wait returns (hardware + notify)
    uint32_t irq_spurious; // wakes that found nothing asserted
};
```

Payload rides in the same `KOS_EP_MSG_MAX = 256` buffer as `bus.h`, so `WRITE` carries up to
`256 - sizeof(kos_uart_req)` bytes per call. The `region_cap`-based large-transfer path stays
reserved and `-KOS_ENOSYS`, exactly as in `bus.h` -- the offset-based discipline
(`docs/design-driver-era-scope.md`, finding 10) is honoured by *not* putting a raw pointer in
this struct.

**Taxonomy check.** `docs/design-driver-era-scope.md:305-323` rules UART an ASYNC byte stream
served by "endpoint rendezvous + IRQ-as-event ... NO call/reply needed". This design uses
`kos_call`/`kos_reply` anyway, and that is a deliberate departure, argued: a `WRITE` must
return *how many bytes were accepted* (the ring may be full) and a `READ` must return *bytes
plus a count*. Both are request/response with a result, which is what call/reply is for. The
ruling's substance -- that no *blocking device transaction* is involved, unlike SPI -- still
holds: the driver replies immediately from ring state and never parks the client waiting on
hardware. Using the landed call/reply substrate for a synchronous *result* is cheaper than
inventing a second reply convention. The 1:1 rule is satisfied: four ops, four class methods.

### 7.5 TX and RX policy

**TX (the `WRITE` op).** Service thread: `n = kos_byte_ring_push(...)`; if the ring was
empty before the push, `kos_irq_notify(tx_cap)`; reply `{status = 0, len = n}`. If `n < len`
the client sees a short write and retries -- **no drop, backpressure surfaced to the caller**,
which is strictly better than the kernel ring's overflow behaviour (`console_tx_write` drains
synchronously under `IrqLock` on overflow, `console_tx.cc:163-180`, accepting an IRQ-masked
stall). A userspace driver must never mask interrupts, so short-write-plus-retry is the only
honest option. `tx_dropped` counts bytes lost only on the death path (sec.4.3) and on a
client that gives up.

IRQ thread: exactly `console_tx_isr`'s loop -- pop while the peripheral has a free slot;
disable the TX interrupt when the ring empties.

**RX.** IRQ thread: while the peripheral has a received byte, read it and
`kos_byte_ring_push` one byte into the RX ring. On a full RX ring, **drop the newest byte and
count `rx_dropped`** -- the alternative (stop reading the peripheral) turns a software
overflow into a hardware overrun plus a stuck level interrupt, i.e. it converts a counted
loss into a storm. Error flags (`ORER`/`FER`/`PER` and friends) are cleared and counted, and
the offending byte is discarded.

**RX read, and the honest scope limit.** The `READ` op is **non-blocking in M4.6**: it
returns `0..len` bytes, possibly 0. A client that wants to wait has two options in M4.6:

1. Poll with `kos_sleep_ns` between calls. Adequate for a console REPL; wasteful otherwise.
2. Pass the driver a `CAP_SEM` bearing `CAP_SIGNAL` (delegated at spawn), which the driver
   posts on the RX ring's empty -> non-empty transition. The client does
   `kos_sem_wait` then a `READ`. Zero new kernel mechanism.

A **blocking `READ` that parks the client inside the driver is DEFERRED**, and the reason is
a genuine missing primitive rather than a scheduling choice: the service thread would have to
stash the one-shot reply cap and later be woken by the IRQ thread -- but the service thread is
parked in `kos_recv`, and only a rendezvous `kos_send` from the IRQ thread could wake it,
which would **block the IRQ thread** while the service thread serves a request, risking RX
FIFO overrun. The missing primitive is **receive-from-either-of-two-sources** (equivalently,
a notification bound to a thread, the `seL4_TCB_BindNotification` shape). Naming it precisely
is the point: it is an M5 kernel object, it is what `docs/design-driver-era-scope.md`'s
"timed / abortable IPC -> EARLY-M4" item is really asking for, and it must not be smuggled in
under M4.6's scope. Option 2 above covers the real M4.6 use cases without it.

### 7.6 Accounting

`kos_uart_stats` lives in the shared ring block (arena memory owned by privileged bring-up),
so it survives the driver thread and is readable after a restart. The `STATS` op exposes it
over the wire; a selftest asserts `rx_dropped == 0` on a paced test and non-zero on a
deliberate flood, so the counter is proven live rather than merely present.

### 7.7 The one-line-serves-RX-and-TX demux

Confirmed from the fleet survey: on **most** chips the console UART's RX and TX share one
interrupt line -- mk64f UART0 = **31** ("UART0 status sources (RX/TX combined)",
`arch/arm/chip/mk64f/irq.h:14`), stm32f411 USART2 = 38, stm32f103 USART1 = 37,
stm32f302 USART2 = 38, sam3x8e UART = 8, rp2040 PL011 = 20, rp2350 PL011 = 34,
imxrt1062 LPUART6 = 25, esp32c6 UART0 (one matrix source), esp32 UART0 (one matrix source).

Two chips are different, in opposite directions:

- **xmc4800** -- USIC service-request routing is **programmable**: the TX buffer interrupt is
  aimed at `SR0` (NVIC 84) via `INPR.TBINP` (`arch/arm/chip/xmc4800/usic.cc:70-76`, called
  from `usic_uart.cc:132`). RX can be aimed at a **different** `SRn`. So XMC can have genuinely
  separate RX and TX NVIC lines -- with the caveat that `SR1` (NVIC 85) is already taken by
  `xmcssc` (`system/driver/xmc4800/xmcssc/xmcssc.cc:241`), so the console RX must pick a free
  node.
- **rx72m** -- fully split, and then some: `RXI6` = 86, `TXI6` = 87 dedicated, `TEI6`/`ERI6`
  grouped. Four sources, three lines needed for a complete driver.

**Decision: the driver handles the shared-line case, and it is the default shape.**
`service_irq()` reads the status register once and services every asserted condition in one
pass. The IRQ thread is written against ONE IRQ cap; a chip with separate lines is a
*special case* served by spawning one IRQ thread per line, each with its own IRQ cap, all
calling the same `service_irq()` (idempotent by contract). That way the shared case -- 11 of
13 chips -- needs no extra thread, and the split case does not need a different class.

**Binding-pool budget, and a real ceiling.** `KICKOS_MAX_IRQ_HANDLES` is 8 by default but
**4** on bluepill-c8, f302nucleo and microbit
(`arch/arm/chip/stm32f103/include/kickos/board_config.h:34`,
`arch/arm/chip/stm32f302/include/kickos/board_config.h:24`,
`arch/arm/chip/nrf51/include/kickos/board_config.h:24`). One shared-line UART needs 1 handle,
so those boards are fine. The RX72M three-line UART needs 3 of 8. This is now a *recoverable*
budget rather than a permanent burn, because Decision 1 gives the pool a free path -- which is
the second concrete payoff of making the line a capability.

---

## 8. Per-arch feasibility

Effort scale: **S** = a few hours, in-tree, verifiable by construction. **M** = a day-plus, new
register work. **L** = new subsystem. "HW" = validation is silicon-gated.

| Arch / chip | Tier-1 real line today | What M4.6 needs | Effort | HW-gated |
|---|---|---|---|---|
| **ARM v7-M** (mk64f, xmc4800, stm32f411/f103/f302, sam3x8e, imxrt1062, mps2) | **YES, proven on silicon.** Per-line NVIC `ICER`/`ISER`/`ICPR`/`ISPR` (`arch/arm/common/arch_arm_common.cc:389,399`; `armv7m/arch_armv7m.cc:170,191`). Working tier-1 drivers: xmcssc on NVIC 85, f411spi on 35, k64drv on 50 | nothing at the arch layer | **S** | no |
| **ARM v6-M** (rp2040, nrf51) | **YES** (`armv6m/arch_armv6m.cc:177,192`) | nothing at the arch layer | **S** | no |
| **ARM v8-M** (rp2350 M33) | **YES** (reuses the v7-M path, `rp2350/startup.S:38-40`) | nothing at the arch layer | **S** | no |
| **RXv3 / rx72m** | **PARTIAL.** Real ICU mask/unmask for `line >= 32` is correct (`arch_rxv3.cc:703,735-736`), but `kickos_rx_dev_pending_line` is **unimplemented** so no device other than the two hard-vectored slots (30 CMWI0, 87 TXI6) dispatches anywhere | (a) `kickos_rx_dev_dispatch` replacing the single-line hook, with the IR-clear moved into the chip (sec.6.2); (b) `RXI6 = 86` route -- a small delta on the proven TXI6 path; (c) **GROUPBL0 demux**: the group first-level ISR, the `ISj -> line` table, the `line >= 256` branch in `arch_irq_mask/unmask/clear_pending` toggling `GENBLn.ENj`, group register constants; (d) **extend the ICU reserved block to 0x880** (sec.6.4); (e) `SCI6_TEI/ERI` constants | **M** | **yes** -- and this is the milestone's largest single piece of arch work |
| **RISC-V / esp32c6** | **BROKEN for a userspace driver.** `arch_rv_hw_mask` is a no-op fallback (`arch/riscv/rv32imac/arch_rv_hw_mask_default.cc`) that **no chip defines**, so `irq_event_isr`'s mask sets only a software bit while the PLIC enable, `mie` and `UART_INT_ENA` stay live -- and `kickos_rv_ext_dispatch_dev` calls `kickos_isr_irq` **unconditionally**, hard-coded to one line (`chip_esp32c6.cc:438-443`). On a level source that is an interrupt storm | (a) **`arch_rv_hw_mask` override** clearing the PLIC `MXINT_ENABLE` bit for the device CPU int -- coarse but kernel-owned, per RULE L1 (sec.5.1); (b) turn `kickos_rv_ext_dispatch_dev` into a real `UART_INT_ST` demux posting per sub-source; (c) a per-line route table so more than one real line is expressible | **M** | **yes** |
| **RISC-V / qemu virt** | no real device routed at all (semihosting console, `chip_virt.cc`) | nothing -- stays the inject-only substrate target. Useful for testing the *cap* mechanism in CI without hardware | **S** | no |
| **Xtensa LX6 / esp32** | **BROKEN, worse than C6.** `arch_irq_mask/unmask` contain **no HW hook whatsoever** (`arch_xtensa.cc:550,561`), and `kickos_lx6_dispatch_l1:380-384` dispatches `g_console_line` unconditionally. One physical device line exists in total | (a) a new `kickos_lx6_hw_mask/unmask` hook clearing the console CPU int's `INTENABLE` bit; (b) an `INT_ST` demux in the L1 dispatcher; (c) a per-line table. Note `INTSET` latches only software ints 7/29 (`arch_xtensa.cc:544-548`), so `arch_irq_inject` cannot fake a real line here -- which is another reason `kos_irq_notify` is the doorbell (sec.2.6) | **M** | **yes** |
| **sim** | dispatch bypasses the mask: `console_tx_service()` calls `kickos_isr_irq(TX_LINE)` without consulting `sim().irq_masked` (`arch/sim/sim.cc:447-451`) | make the TX-line delivery respect `irq_masked`, so the sim faithfully models mask-until-ack. Then the whole cap/teardown/level design is testable in CI with no hardware | **S** | no |
| **rp2350 Hazard3** | **does not exist in tree** (no `arch/riscv/chip/rp2350*`; design-only, `docs/design-rp2350.md:180-202`) | out of scope | -- | -- |
| **mps2, nrf51, virt** | semihosting console, no `arch_console_tx_backend` | no UART driver to build; they validate the *general* mechanism via injected lines | **S** | no |

Two hard numeric ceilings to design against, both confirmed:

- **Software-controller backends cap at 32 lines** (`arch_rv32imac.cc:406` returns early on
  `line >= 32`; `SOFT_IRQ_LINES = 32`, `arch_rxv3.cc:96`; `& 31u` on Xtensa;
  `SIM_IRQ_LINES = 32`), and the C6 / esp32 / sim boards set `KICKOS_MAX_IRQ = 32`. A UART
  needing RX + TX + error lines must fit inside `0..31` alongside the bench and selftest
  lines (esp32 already reserves 5/6/7/9/11 and 20, `arch/xtensa/chip/esp32/irq.h:39-40`).
  This is a second, independent argument for the C6/LX6 UART being **one grouped line** rather
  than three.
- **`arch_console_reclaim` exists on only two chips** (mk64f, xmc4800). Those are exactly the
  two boards with a console service list today, so M4.6's targets are covered; the fleet
  rollout is M4.7 work.

---

## 9. Validation plan

### 9.1 New selftests

*(names and registration style to be finalised against `user/apps/common/selftest/main.cc`;
the existing `irq_*` tests are the pattern)*

### 9.2 Board-by-board silicon order

*(see below)*

### 9.3 The RX72M chicken-and-egg, and the `SCR.TIE` assumption

*(see below)*

---

## 10. Staging

*(see below)*

---

## 11. Open questions the reviewer must rule on

*(see below)*

---

## 12. Deliberately out of scope

*(see below)*
