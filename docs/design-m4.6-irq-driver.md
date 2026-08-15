<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# M4.6 design -- the IRQ-driven driver pattern, and the buffered userspace UART on top

> **Status: LANDED** -- the M4.6 step-0 design gate. See
> `design/README.md` for the marker taxonomy.

This document is the M4.6 step-0 gate: the general
mechanism by which an **unprivileged userspace driver owns an interrupt line** -- claimed by
root at bring-up, handed to the driver at spawn, and **reclaimed when the driver dies**.
UART is the first and forcing consumer; M4.7 (ADC / RTC / watchdog / entropy / PWM / DAC)
must reuse this substrate **unchanged**.

First written against `master` `64410b7`, in `file:line` form.
**Recovered 2026-07-30 from an unpushed worktree.** Its citations have since been re-verified
against the tree at `58a0c6b` and converted to the `file (symbol)` form
`reference/invariants.md` mandates, because a `file:start-end` range fails silently the moment
anything above it moves: roughly seven citations in ten had drifted, several onto unrelated
code. The DESIGN survives the M4.5.x unprivileged-root arc unchanged. What the arc did change
is the posture its gates lean on: root is now unconditionally unprivileged, `idle` is the only
privileged thread left in the system, the separate device and clock authority bits are gone,
and the authority set is the six bits in `reference/invariants.md`. That is why the mint gate
in section 2.4 is stated as an `AUTH_IRQ` check and not as a privilege check.

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

1. **Ungated.** The tier-1 register/wait/ack arms (syscalls 14/15/16, the first of which this
   document renames to `KOS_SYS_IRQ_CLAIM`) had **no authority check and no capability**
   (`kernel/syscall/syscall.cc` (the `KOS_SYS_IRQ_CLAIM` / `_WAIT` / `_ACK`
   case arms)). Any unprivileged thread could claim ANY line in `[0, KICKOS_MAX_IRQ)`,
   first-come-first-served. Compare `KOS_SYS_IRQ_ATTACH = 11`
   (`kernel/syscall/syscall.cc` (`case KOS_SYS_IRQ_ATTACH`)), which requires **`AUTH_IRQ`**
   AND a `CAP_SEM` bearing `CAP_SIGNAL`.
2. **Nothing is released on death.** `sched::exit_current` (`kernel/sched/sched.cc`
   (`exit_current`)) tears down capabilities only. An `IrqBinding` is not a cap type
   (`kernel/include/kickos/cap.h` (`enum class CapType`) has
   `CAP_EMPTY/SEM/MUTEX/ENDPOINT/REPLY/AUTHORITY`), bindings are **bump-allocated with no free
   list** (`kernel/irq/irq.cc` (`irq_register`, "no unregister/free path yet")), and
   `irq_detach` is never called on exit. A dead driver leaves the line **armed**, its ISR
   masking and `sem_post`ing into a semaphore nobody waits on, its binding slot burned
   (`KICKOS_MAX_IRQ_HANDLES` defaults to 8 and is **4** on bluepill-c8 / f302nucleo /
   microbit), and the line unclaimable forever (`-KOS_EBUSY`).
3. **Edge-only.** The binding contract is latch-and-coalesce
   (`arch/include/kickos/arch/arch.h` (the latch-and-coalesce contract stated above
   `arch_irq_mask`)). Level sources -- RX72M `GROUPBL0`, and every "one status register, N
   sub-sources" UART -- need a per-binding trigger type so the rearm does `clear_pending` then
   `unmask` (`TODO.md` ("[M4] level-trigger tier-1 bindings")).

All three are closed here. Hole 1 and hole 2 are closed by the **same** decision.

---

## 1. What is general mechanism, what is UART policy

The reviewer's first question. Explicitly:

| Layer | Item | Status |
|---|---|---|
| **General kernel mechanism** | the IRQ capability object type + pool + refcount (sec.2) | M4.6, reused verbatim by M4.7 |
| | `kos_irq_claim` (`AUTH_IRQ` mint) / `wait` / `ack` / `notify` (sec.2) | M4.6 |
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

**RULED 2026-07-31, and the three records that read as disagreeing are made to agree with
this.** They were answering two different questions as though they were one. The two axes:

- **Mint** -- turning a bare line number into an owned binding -- is gated on the existing
  **`AUTH_IRQ`** bit, exactly as `STATE.md` and `TODO.md` scope it, and exactly as sec.2.4
  states. No new authority bit.
- **Use** -- `wait` / `ack` / `notify` on a line already claimed -- is gated on **possession**
  of the line cap plus the matching right bit, at `cap_resolve_e` (sec.2.3). Neither `STATE.md`
  nor `TODO.md` addressed this, which is silence, not a competing answer.

So "gate `irq_register` on `AUTH_IRQ`" was never wrong; it was **incomplete**, and reading it
as the whole fix is what produced the appearance of a third answer. It closes hole 1 alone.
Hole 2 needs the object: `cap_teardown` is the walk that already runs on every death path
(sec.4), and no authority bit can release anything. The capability is also the only shape that
can fund the WAIT/SIGNAL split the two-thread driver requires -- sec.3.5 kills spawn-time mint
on precisely that ground.

A **narrower per-line authority is REFUSED**, so `TODO.md`'s "needs a decision rather than
work" is now decided rather than carried. The reason is sec.3.6's and it is checkable rather
than aesthetic: while `KOS_SYS_IRQ_ATTACH` stays reachable by any `AUTH_IRQ` holder naming a
bare line number, tier 2 is a namespace-wide door and a per-line grant on tier-1 mint buys
nothing. The holder populations of a would-be seventh bit and of `AUTH_IRQ` are identical
today -- `AUTH_IRQ` is declared in exactly one place in the tree
(`user/apps/common/selftest/main.cc` (`KICKOS_APP_AUTHORITY`)), and all four tier-1 drivers run
at authority zero -- so the bit would be the pre-M4.5.4 authority-inflation mistake mirrored.
Sec.3.6 records the falsifier to check instead of re-arguing.

One consequence to land with the gate: `AUTH_IRQ`'s own comment
(`kernel/include/kickos/cap.h` (`enum CapAuthority`)) enumerates its uses as "irq_attach,
irq_unmask" and must gain `irq_claim`, or the bit's documented meaning drifts from what it
gates.

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
  say "additive: each new pool gains one arm" (`kernel/syscall/cap.cc` (`obj_ref_inc`,
  `obj_ref_drop`, `obj_close_protocol`); `kernel/include/kickos/cap.h` (the `obj_ref_inc`
  declaration)). The Book already carries the recipe
  (`docs/book/adding-a-kernel-object-type-the-additive-recipe.md`).
- It needs a second, parallel teardown walk in `exit_current`, with its own ordering
  relationship to `cap_teardown` and `domain_release` -- a new invariant to get wrong. The
  capability route reuses the one walk that already exists.
- It leaves hole 1 (ungating) unsolved: a registry still needs a bespoke authority story.
  A capability's *possession* IS the authorisation for `wait`/`ack`/`notify`, checked at the
  one chokepoint (`kernel/syscall/cap.cc` (`cap_resolve_e`)). Minting is a separate question,
  answered by `AUTH_IRQ` in sec.2.4.

Cost of the capability route: 1 `CapType` enumerator, 1 pool + 1 `uint8_t` refcount array in
`Kernel`, 3 switch arms, 1 `cap_resolve_e` case, and the `IrqBinding` moves from a bump array
to a `SlotPool`. That is the entire delta. It is smaller than the registry.

Concretely, the three switches over `CapType` (`kernel/include/kickos/cap.h`
(`enum class CapType`)) each gain one arm. The wrong precedent to generalise from is the
**poolless** type -- the reply cap, whose entire content is a packed thread handle in `obj`,
deliberately absent from `cap_resolve_e` because that would chase `obj` in a pool that does not
exist; it resolves through `cap_lookup` plus an explicit type test instead. Axis-3 authority is
not even that: it is a byte on the TCB and no `CapType` member at all, which is the standing
reminder that a table entry is for naming a thing whose liveness the holder could get wrong.
The IRQ cap is the opposite shape from both. It names a pooled, refcounted `IrqBinding`, so it
DOES take the `cap_resolve_e` arm and the `obj_ref_inc` / `obj_ref_drop` arms, exactly as
`CAP_SEM` does. The types share a switch and nothing else.

### 2.2 The object

`kernel/include/kickos/cap.h` gains exactly one enumerator:

```c++
enum class CapType : uint8_t
{
    // CAP_EMPTY / CAP_SEM / CAP_MUTEX / CAP_ENDPOINT / CAP_REPLY unchanged
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
semaphores. The IRQ block of `Kernel` (`kernel/include/kickos/instance.h` (`struct Kernel`, the
interrupt-dispatch group)) becomes:

```c++
IrqEntry irq_table[KICKOS_MAX_IRQ];                    // line -> handler; ISR reads by index
SlotPool<IrqBinding, KICKOS_MAX_IRQ_HANDLES> irq_bindings;
uint8_t irq_refs[KICKOS_MAX_IRQ_HANDLES];              // parallel refcount, as sem_refs
```

`irq_binding_count` is deleted with the bump allocator.

**The latency invariant survives.** `irq_event_isr` is still handed `&binding` as its `arg`
(`kernel/irq/irq.cc` (`irq_event_isr`)) -- a `SlotPool` slot has a stable address for its whole
life (`slots_[]` is a fixed array), so the ISR still does zero lookups. This is the same
property `irq_wait`'s "the binding is stable, so parking on its sem outside the lock is safe"
(`kernel/irq/irq.cc` (`irq_wait`)) already relies on; it now rests on the pool rather than on
"never freed".

### 2.3 Rights -- every bit has a real check

| Right | Meaning on an IRQ cap | Checked at |
|---|---|---|
| `CAP_WAIT` | may `kos_irq_wait` / `kos_irq_ack` (consume an event, rearm the line) | `cap_resolve_e`, IRQ type + `CAP_WAIT` |
| `CAP_SIGNAL` | may `kos_irq_notify` (software-post the binding's notification) | `cap_resolve_e`, IRQ type + `CAP_SIGNAL` |
| `CAP_TRANSFER` | may be delegated into a child table at spawn | the delegate site (unchanged) |

No dead field -- the house rule stated at `kernel/include/kickos/cap.h` (`enum CapRights`).

`CAP_SIGNAL` is **not** "may raise the line at the controller". Raising at the controller
stays `KOS_SYS_IRQ_INJECT = 9`, test-only scaffolding compiled out of the production ABI and
deliberately ungated (it simulates a device firing, not an arm of the controller), untouched.
`kos_irq_notify` is a pure `sem_post` on the binding's notification -- see sec.2.6 for why that
distinction is load-bearing rather than fussy.

### 2.4 ABI

The ABI is unstable until M6, so syscall 14 is **renamed in place** (it was the tier-1
register call), not deprecated alongside a replacement:

```c
KOS_SYS_IRQ_CLAIM  = 14, // (line, flags) -> CAP_IRQ cap handle, or -KOS_E*
                         //   EPERM (lacks AUTH_IRQ), EINVAL (line/flags), EBUSY (line owned),
                         //   ENOMEM (binding pool or cap table full)
KOS_SYS_IRQ_WAIT   = 15, // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT)
KOS_SYS_IRQ_ACK    = 16, // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks WAIT)
KOS_SYS_IRQ_NOTIFY = 43, // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EPERM (cap lacks SIGNAL)
```

43 is the next free number: the highest assigned today is
`KOS_SYS_PERIPH_REG_WRITE = 42` (`user/include/kickos/sys/abi.h` (`enum kos_syscall_nr`)).

Claim flags (`user/include/kickos/sys/abi.h`, new):

```c
enum kos_irq_claim_flags
{
    KOS_IRQ_EDGE  = 0,      // default; latch-and-coalesce rearm (bare unmask)
    KOS_IRQ_LEVEL = 1 << 0  // rearm does clear_pending then unmask (sec.5)
};
```

Userspace C decls (`user/include/kickos/sys.h`, replacing the `kos_irq_register` /
`kos_irq_wait` / `kos_irq_ack` tier-1 block):

```c
int kos_irq_claim(int line, unsigned int flags); // needs AUTH_IRQ; -> CAP_IRQ cap handle
int kos_irq_wait(int irq_cap);
int kos_irq_ack(int irq_cap);
int kos_irq_notify(int irq_cap);
```

`kos::Irq` (`user/include/kickos/kos.h` (`class Irq`)) gains the release path its own comment
says it lacks: the destructor calls `kos_handle_close(cap_)`, and it becomes move-only.

### 2.5 Kernel-side shape

`irq_register(int line)` becomes:

```c++
// AUTH_IRQ mint. Claims `line` (one owner, no stealing), allocates a binding from the
// pool, and installs a full-rights CAP_IRQ into the caller's table. The line is left
// MASKED with needs_rearm set: the FIRST irq_wait arms it (sec.3.2) -- so no window exists
// in which the line is armed while its eventual owner is not yet running.
int irq_claim(Thread* c, int line, unsigned int flags);
```

Body, with step 1 outside the lock (`cap_check_authority` is the one cap.h entry point that
does not want `IrqLock`) and steps 2 onward under one `IrqLock`:

1. `cap_check_authority(sched::current(), AUTH_IRQ)`, else `-KOS_EPERM`. Not a privilege
   check: see the paragraph below.
2. Range-check `line`, reject unknown `flags` bits (`-KOS_EINVAL`).
3. `-KOS_EBUSY` if `irq_table[line].handler != irq_default_handler` (the existing one-owner
   test, `kernel/irq/irq.cc` (`irq_register`)).
4. `irq_bindings.alloc()`, then `cap_install`, and release the binding when the install
   refuses: a full cap table must not leak a binding slot. `-KOS_ENOMEM` on either.
   (Shipped as install-then-unwind rather than the probe-before-allocate the reply cap uses,
   because the claimer drives this on its OWN table -- see `kernel/irq/irq.cc` (`irq_claim`).)
5. `sem_init(&b->sem, 0)`; `b->line = line`; `b->needs_rearm = true`; `b->trigger` set to
   `IRQ_LEVEL` when the claim carries the level flag and to `IRQ_EDGE` otherwise (written as
   an `if`/`else`, not a ternary).
6. `irq_refs[idx] = 1`.
7. `irq_attach(line, irq_event_isr, b)`.
8. `arch_irq_clear_pending(line)` -- drop pre-claim garbage, as today
   (`kernel/irq/irq.cc` (`irq_register`, the clear-before-arm comment)). **Do not
   `arch_irq_unmask`.** This is the change that kills the handover race (sec.3.2).
9. `cap_install` the binding handle `irq_bindings.handle_for(idx)` into `c`'s table with the
   IRQ cap type and rights `CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER`.

**Why the mint gate is `AUTH_IRQ` and not a privilege check.** After the M4.5.x arc the system
has exactly ONE privileged thread, `idle`, and `idle` issues no syscalls at all. A
privileged-only `irq_claim` would therefore be a syscall no thread in the system can reach: a
gate that reads as strict and is in fact a deletion. `AUTH_IRQ` is the bit that already gates
the neighbouring line-binding syscalls, `KOS_SYS_IRQ_ATTACH` and `KOS_SYS_IRQ_UNMASK`
(`kernel/syscall/syscall.cc` (`case KOS_SYS_IRQ_ATTACH`)), and the claim arm copies that arm
verbatim, right down to the error: `cap_check_authority(sched::current(), AUTH_IRQ)`, else
`-KOS_EPERM`. Root holds `AUTH_IRQ` today and seats no child with it, so the population that
can mint is exactly the population that can already seize a line through tier 2. That is the
security statement sec.3.2 rests on, and it is a statement about the BIT.

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
| `kos_irq_inject(line)` -- raise at the controller | **REJECTED.** `arch_irq_inject` on RX **refuses** any real vector: "A real peripheral line cannot be pended from software on RX" (`arch/rx/rxv3/arch_rxv3.cc` (`arch_irq_inject`)). The forcing consumer is RX72M SCI6, vector 87. Dead on arrival. |
| A PI mutex around the ring, producer primes the peripheral itself | **REJECTED.** Makes the IRQ-servicing thread take a lock a lower-priority producer holds -- a priority-inversion surface on the drain path, bounded only by PI. And it makes the ring's `tail` two-writer, losing SPSC. |
| `kos_irq_notify(irq_cap)` -- `sem_post` the binding's notification | **CHOSEN.** Pure kernel-object operation, zero arch surface, works identically on all 13 chips and on `sim`. Keeps `tail` single-writer (the producer never touches the peripheral or `tail`). |

The consequence, which must be written into the contract: **a wake from `irq_wait` is
advisory, not one-per-hardware-event.** A driver's IRQ loop MUST re-read the device status
on every wake and be idempotent about finding nothing. That is already how every existing
precedent is written (`user/apps/frdmk64f/k64drv/main.cc` (`pit_driver`) etc.), and the
phantom-wake defense (`needs_rearm` set on wait-RETURN, never in the ISR,
`kernel/include/kickos/irq.h` (`struct IrqBinding`, the `needs_rearm` comment)) exists
precisely because spurious wakes were always possible. `kos_irq_notify` makes an existing
tolerated condition into a documented one.

One real interaction to state: a notify-driven `irq_wait` return sets `needs_rearm = true`,
so the next `irq_wait` unmasks a line that may never have been masked. `arch_irq_unmask` on
an already-unmasked line is idempotent on all five backends (NVIC `ISER` write-1-to-set;
software-bitmap clear; RX `IER` bit-set). Safe. On a LEVEL binding the rearm additionally
calls `arch_irq_clear_pending` -- also safe, since a genuinely asserted level source
re-latches immediately (sec.5).

---

## 3. Decision 2 -- handover at spawn, with no unowned-armed window

### 3.1 The race as it stands

Handing the kernel console TX line to a userspace driver is today a **three-step handover with
an unowned interval in the middle**:

```
root:               kos_console_publish(ep)
                      -> console_tx_deinit()   [kernel/init/console_tx.cc (console_tx_deinit)]
                           flush_sync; irq_disable; irq_detach(line); armed = false
                    kos_spawn(driver, ...)
driver:             kos_irq_register(line)              [line is unowned in between]
```

Between `irq_detach` and the driver's `irq_register` the line is **unowned**. It is left
masked by `irq_detach` (`kernel/irq/irq.cc` (`irq_detach`)), so no interrupt is lost or
stormed -- but the line is claimable by *any* thread in that window, because
the mint was ungated (hole 1). A second thread that claims line 31 first wins,
and the real console driver gets `-KOS_EBUSY` and cannot start.

### 3.2 The fix: claim-masked, arm-on-first-wait

Two changes, both already stated above:

- `kos_irq_claim` requires **`AUTH_IRQ`** (sec.2.4). A thread without that bit cannot claim
  any line, ever. Hole 1 closed against every principal that is not already trusted with the
  line namespace. There is no longer a race for an ordinary thread to win.
- `kos_irq_claim` mints the binding **masked**, with `needs_rearm = true`. The line is armed
  by the **first `irq_wait`**, which by construction runs in the thread that will consume the
  event.

That gives the two properties the gate asks for, as a pair of invariants:

> **INVARIANT H1 (no armed-and-unowned).** From `arch_irq_unmask` to `irq_detach` a line has
> exactly one live `IrqBinding`. `irq_claim` does not unmask; the only unmask is
> `rearm_locked`, reachable only through `irq_wait`/`irq_ack` on a resolved IRQ cap; the
> only mask-and-detach is `irq_ref_drop` at refs -> 0. A line therefore transitions
> unowned-masked -> owned-masked -> owned-armed -> owned-masked -> unowned-masked. It is
> never armed-and-unowned. Nothing in this chain depends on who may mint: H1 is a property of
> the unmask/detach call graph alone, so the `AUTH_IRQ` gate neither strengthens nor weakens
> it.

> **INVARIANT H2 (no running-driver-with-kernel-attached-line).** `console_tx_deinit` runs
> `irq_detach` under one `IrqLock` (`kernel/init/console_tx.cc` (`console_tx_deinit`)), and
> `kos_console_publish` calls it BEFORE flipping to `USER_OWNED`
> (`kernel/syscall/syscall.cc` (`case KOS_SYS_CONSOLE_PUBLISH`)). `irq_claim` then fails
> `-KOS_EBUSY` if the line is still kernel-attached. So the ordering is *enforced by the EBUSY
> check*, not by convention: nobody can claim the console TX line until the kernel has
> genuinely let go.

And a companion rule, because `AUTH_IRQ` is a **class-wide** bit over the whole line namespace
and not a per-line right. Two holders of the bit are indistinguishable to `irq_claim`, so the
publish-to-claim window of sec.3.1 is closed against threads that do NOT hold the bit and
against nothing else.

> **RULE H3 (claim before you seat).** Root completes `irq_claim` of every line it intends to
> hand over BEFORE seating `AUTH_IRQ` on any child. Vacuous today: no child holds `AUTH_IRQ`,
> root is the sole seater of authority (`kos_thread_params::authority` narrows and never
> widens, and only a spawning parent writes a child's authority slot), and a thread that has
> dropped a bit can never regain it. The rule exists so that the day a child does get the bit,
> the ordering that keeps the sec.3.1 window uncontested is written down rather than
> rediscovered.

State the security claim precisely, because the syscall is the wrong place to look for it. The
enforceable statement is about the BIT: **`AUTH_IRQ` means "may bind or mint any free
controller line."** It cannot mean less. `irq_attach` seizes lines out of the same `irq_table`
under the same bit, so a principal that could mint but not attach, or the reverse, would gain
nothing it did not already have. `irq_claim` adds no reach to an `AUTH_IRQ` holder; it adds a
*release path* and a *delegation path* to reach the holder already had.

The corrected bring-up sequence, all of it in root's `start()`:

```
1. kos_console_publish(ep)     // console_tx_deinit: flush, irq_disable, irq_detach, disarm
2. tx_cap = kos_irq_claim(TX_LINE, KOS_IRQ_EDGE)    // EBUSY-guarded against step 1
3. rx_cap = kos_irq_claim(RX_LINE, ...)             // if the chip has a separate RX line
4. spawn_driver(...)  delegating { ep(WAIT), tx_cap(WAIT|SIGNAL), rx_cap(WAIT), ... }
```

Steps 2-3 are the *only* new syscalls in the sequence. Step 1 is unchanged. No new kernel
handover primitive is needed -- which is the payoff of Decision 1.

### 3.3 Delegation: extend the spawn helper, do not invent

> Superseded by M4.8.1: the per-thread cap list argued for here is now descriptor data and the
> helper is `user/include/kickos/sys/driver_service.h`'s `spawn_one`. The shape below is what
> landed; only its spelling changed.

The helper of the day granted exactly `{ep, KOS_CAP_WAIT}` plus one MMIO window. It needs two extensions, both plumbing over
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

The one-cap helper stayed beside it so the two landed SPI services
(`system/init/frdmk64f/service_list.cc`, `service_list_xmc4800relax.cc`) did not churn; M4.8.1
retired it, and both services became descriptors instead.

**Corrected at implementation time: the two threads CANNOT both hold the window.** An earlier
draft here claimed they could -- that a spawn carrying MMIO takes a fresh domain slot
(`kernel/domain/domain.cc` (`domain_for`), "An MMIO grant is a capability -- never shared"), so
two threads given the same `win_base` would each get a domain covering both regions. They do
not: the M4.5.2 one-holder-per-window check runs first and refuses an equal window a live
thread already holds with `-KOS_EBUSY` (`kernel/domain/domain.cc` (`domain_for`, its
`dev_window_free` arm)). The second spawn simply fails.

That is not a problem, because it is the rule sec.7.2 already states from the other direction:
**every peripheral register belongs to the IRQ thread alone.** So the grants are asymmetric --
the IRQ thread takes `{win, shared}` and the service thread takes `{shared}` only, which is
exactly the ownership split the SPSC rings need. Only DEV windows are exclusive; a RAM region
is not, and the service thread's data-only domain reaches the same block. Still no kernel
domain change, and the cost is 2 slots against a `KICKOS_MAX_DOMAINS` default of
`KICKOS_MAX_THREADS + 2 = 18` (`kernel/include/kickos/config/system.h`
(`KICKOS_MAX_DOMAINS`)), so there is headroom. **A driver that tried to touch the peripheral
from its service thread would therefore fail at SPAWN, not at the register write** -- the
isolation rule is enforced by the domain model rather than by convention.

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

### 3.5 Rejected alternative: spawn-time mint

The fourth shape, recorded because it is the one a reviewer reaches for on seeing H3 and
wanting the unowned interval gone by construction: put an `irq_line` field in
`kos_thread_params` next to `mmio_base`/`mmio_size`, and have `thread_spawn` mint the binding
and install the IRQ cap directly into the child. No parent ever holds the cap, so there is no
window at all. It dies on sec.2.1's own argument. The two-thread driver needs ONE binding
shared into TWO children with DIFFERENT rights: WAIT for the IRQ thread, SIGNAL for the service
thread that rings the doorbell. A per-spawn mint can only produce either two bindings for one
line, which the one-owner rule forbids and which would give the two threads two unrelated
notifications, or one binding plus a second spawn that fails `-KOS_EBUSY`. Root-claims-then-
delegates is the only shape that can fund the WAIT/SIGNAL split, and once the parent must hold
the cap at all, the interval H3 governs exists.

### 3.6 Per-line granularity: REFUSED, and what would falsify `AUTH_IRQ`

Ruled with sec.2.1. If per-line granularity is ever genuinely wanted, it does not come from a
finer authority bit and it does not come from possession of the cap. It comes from **retiring
userspace tier-2**.
While `KOS_SYS_IRQ_ATTACH` exists as an unprivileged-reachable syscall, any bit that gates
minting must also gate attaching, and attaching is inherently namespace-wide: the caller names
a bare line number. Delete that syscall and the whole line namespace has exactly one entry
point, `irq_claim`, at which point a per-line grant becomes expressible. A second, mint-only
authority bit added before then buys nothing, because tier 2 remains the wider door.

The falsifier for the `AUTH_IRQ` decision, stated so it can be checked rather than argued:
**it becomes wrong the day a long-lived unprivileged principal must hold `irq_attach` /
`irq_unmask` but be denied minting.** Today the two holder populations are identical (root,
and nobody else), which is exactly why one bit suffices. The moment they diverge, a seventh
authority bit earns its keep, and there is room for it: `CapAuthority` uses bits 0..5 of a
`uint8_t`, so two are spare.

---

## 4. Decision 3 -- teardown on driver death

Three death paths must converge on one state: **line masked and unowned, binding slot back
in the pool, no resurrectable waiter, console still able to print.**

| Path | Route to teardown |
|---|---|
| `kos_exit` / return from entry | `sched::exit_current` -> `cap_teardown` (`kernel/sched/sched.cc` (`exit_current`)) |
| fault-kill (MPU / bus / illegal instruction) | the fault handler kills the **current** thread, i.e. the same `exit_current` |
| `kos_handle_close(irq_cap)` (voluntary release) | `handle_close` -> `obj_close_protocol` + `obj_ref_drop` (`kernel/syscall/cap.cc` (`handle_close`)) |

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
  `set_default(irq); arch_irq_mask(irq);` under `IrqLock` (`kernel/irq/irq.cc` (`irq_detach`)).
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
  `endpoint_ref_drop` verbatim (`kernel/syscall/cap.cc` (`endpoint_ref_drop`)): floor `refs` at
  1 rather than free a binding with a parked waiter. Unreachable today (a parked waiter pins
  its own cap, so `refs >= 1`, and a parked thread cannot self-exit) -- kept as
  defence-in-depth and as the correct behaviour once a kill primitive exists.

### 4.2 Why the IRQ arm of `obj_close_protocol` is empty

The endpoint arm EPIPEs parked senders when the last receiver goes
(`kernel/syscall/cap.cc` (`obj_close_protocol`, the `CAP_ENDPOINT` arm)); the reply arm EPIPEs
the parked caller (the `CAP_REPLY` arm of the same switch). Should the IRQ arm wake a parked
waiter with `-KOS_EPIPE`?

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
`domain_release` in the same `exit_current` (`kernel/sched/sched.cc` (`exit_current`)). The
kernel does not know the ring's layout (it is userspace policy, deliberately -- sec.1), and
reading a dying domain's memory to poke a peripheral the kernel no longer owns inverts the
whole isolation argument. Draining is not merely hard, it is wrong.

Why not hand back to the kernel ring: covered in sec.4.4.

Accounting: the driver's `kos_uart_stats` (sec.7.6) lives in the shared ring block, which is
arena memory allocated by root at bring-up -- so it OUTLIVES the driver thread and a
supervisor can read the final `tx_dropped` after a restart. That is the difference between
dropping and hiding.

### 4.4 The console ownership state machine on driver death

`kernel/init/console.cc` (`enum class ConsoleState`) has
`KERNEL_OWNED -> USER_OWNED -> RECLAIMED`, and `RECLAIMED` is currently reached only by
`kernel/init/console.cc` (`kpanic_enter`), via `arch_console_reclaim`.

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
   `recv_holders` reaches 0 (`kernel/syscall/cap.cc` (`obj_close_protocol`, the `CAP_ENDPOINT`
   arm)), additionally call `console_note_driver_death()` if `e.obj` is the currently published
   console target. `cap_console_publish` already stores that handle (`kernel/syscall/cap.cc`
   (`cap_console_publish` / `g_stdout_target`)), so no new identity tracking is needed.
   `recv_holders` counts WAIT-bearing caps, so on a two-thread driver it reaches 0 when the
   SERVICE thread dies, while the registers belong to the IRQ thread, which is not counted here
   at all. **DISPROVED 2026-08-02**: keying the reclaim on the note alone reprograms the UART
   under a live IRQ thread that still owns the register window, silencing its source. The fix
   asks the DEVICE instead: `console_on_driver_death` defers while any live domain still holds
   `arch_console_reclaim_window()`, the note stays SET across a refusal, and every
   `exit_current` and voluntary close re-runs the check, so the LAST holder's own exit reclaims.
2. `sched::exit_current` calls `console_on_driver_death()` **after** `cap_teardown` and
   before `domain_release`. `console_on_driver_death` checks the note and, if the device window
   is free, runs `arch_console_reclaim(); g_console_state = RECLAIMED;`.

**Ordering is the whole point.** Doing the reclaim inside a cap arm would run it at an
arbitrary index in `cap_teardown`'s loop (`kernel/syscall/cap.cc` (`cap_teardown`)) -- possibly
while the driver's IRQ cap is still live and the TX line still armed, so
`arch_console_reclaim` would re-init a UART whose interrupt can still fire into
`irq_event_isr` and post a dying thread's semaphore. Running it after the whole loop
guarantees: every IRQ cap dropped -> every line masked and detached -> *then* the device is
re-initialised. Deterministic.

`arch_console_reclaim` falls back to a no-op (`arch/common/arch_console_reclaim_default.cc`)
and is defined today by exactly four chips: **mk64f**
(`arch/arm/chip/mk64f/chip_mk64f.cc` (`arch_console_reclaim`)), **xmc4800**
(`arch/arm/chip/xmc4800/usic_uart.cc` (`arch_console_reclaim`)), **esp32c6**
(`arch/riscv/chip/esp32c6/chip_esp32c6.cc` (`arch_console_reclaim`)) and **esp32**
(`arch/xtensa/chip/esp32/chip_esp32.cc` (`arch_console_reclaim`)).

**Three providers ship a console service list, not two**: `system/init/sim/service_list.cc`
registers `simcon` with `kind = KOS_SVC_CONSOLE` and publishes exactly as the silicon services
do. **The sim needs no `arch_console_reclaim` body, and there is nothing for one to do**: the sim's
"device" is host fd 1, which has no register state a dead driver can garble, and the polled writer
(`arch/sim/sim.cc` (`arch_console_write_sync`)) writes to that same fd with no dependence on
the ring or the emulated TX line. The no-op fallback is therefore CORRECT on the sim, and a
body added there would have been pure ceremony that reads as coverage.

What was actually dark on the sim is the **ownership state**, not the device: driver death
never flipped `USER_OWNED`, so `console_emit` kept dropping. That half is the mechanism above,
and the sim IS the only hardware-free vehicle for it -- `tests/integration/check_sim_drvdeath.sh` turns
"driver dies, console still prints" into a CI assertion instead of a bench errand. So the
coverage claim reads: the **state machine** is gated on the host; a **per-chip reclaim body**
is effective on mk64f and xmc4800 and silicon-gated there. Every other chip degrades to
"console goes dark after driver death", which is honest and unchanged from today. Adding the
per-chip definition is M4.7 fleet work, listed in sec.8.

Rejected alternative: **re-arm the kernel TX ring (back to `KERNEL_OWNED`).** Tempting --
the line is free after teardown, and `console_buffer_init`'s body would mostly work. Rejected
because the dead driver may have left the UART in an arbitrary state (baud reprogrammed, `TE`
cleared, FIFO thresholds changed, in the C6 case `UART_INT_ENA` bits set). `arch_console_reclaim`
exists *precisely* to re-establish a known polled state and is already written per chip;
re-arming the ring would additionally need a per-chip "restore the ring's assumptions" step
that does not exist. `RECLAIMED`/polled is the smaller, already-implemented, honest answer.
Ring re-arm is a legitimate M4.7+ improvement once `arch_console_reclaim` exists fleet-wide.

Two comment fixes owed elsewhere, both stale claims this design makes visible:

- `kernel/init/console.cc` (`enum class ConsoleState`) calls `RECLAIMED` "panic forcibly took
  the UART back". It becomes "the kernel forcibly took the UART back (panic, or driver death)".
- `arch/include/kickos/arch/arch.h` (`arch_irq_inject`) still calls raising "privilege-gated",
  which `kernel/syscall/syscall.cc` (`case KOS_SYS_IRQ_INJECT`) contradicts in as many words:
  that arm is "Deliberately NOT privilege-gated", because injection simulates a DEVICE firing
  and the tier-1 model has unprivileged drivers receive IRQs. The seam comment is the one that
  is wrong. Not fixed here (it is another file's change), recorded so it is not lost.

---

## 5. Decision 4 -- level-triggered bindings

`TODO.md` ("[M4] level-trigger tier-1 bindings") already specifies this; M4.6 is its first
consumer, so it lands here.

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
the M4 level-trigger rearm path" (`arch/include/kickos/arch/arch.h` (`arch_irq_clear_pending`)).
This is that path. No new arch seam.

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
  reserved block, `arch/rx/chip/rx72m/chip_rx72m.cc` (`arch_reserved_blocks`)). Clean.
- **RX72M GROUPBL0 sources** -- `GENBL0.ENj`, kernel-owned, per-source. Clean, **once the ICU
  reserved block is extended** (sec.6.4).
- **ESP32-C6** -- the natural per-source mask is `UART_INT_ENA`, which lives inside the
  **granted UART block**. L1 forbids the kernel touching it. So the C6 mask must be the
  **PLIC `MXINT_ENABLE` bit for the device CPU int** -- kernel-owned, but coarse: it masks
  every source routed to that CPU int. Recorded as an accepted coarseness in sec.8, with the
  consequence that the C6 UART is a single grouped line (sec.6.2), not three.

### 5.2 The bulk-rearm hazard: **not triggered**

`TODO.md` ("[M4, lands with bulk-rearm] identity-free coalesced redelivery on the software
backends") and `arch/include/kickos/arch/arch.h` (the SINGLE-DOORBELL CONTRACT above
`arch_irq_mask`) warn that the software backends carry a coalesced redelivery through ONE
shared cell plus ONE doorbell, so **at most one unmask-with-pending may occur per `IrqLock`
region**.

This design does not violate it, and the reason is worth stating so the reviewer does not
have to reconstruct it:

- `rearm_locked` unmasks **exactly one** line, and its only callers are `irq_wait` and
  `irq_ack`, each of which holds its own `IrqLock` for one binding. Unchanged from today.
- The grouped-line demux (sec.6) calls `kickos_isr_irq` in a **loop**, so several
  `irq_event_isr` invocations may run in one ISR -- but each of those **masks**, and masking
  does not use the doorbell. The hazard is on unmask only.
- `kos_irq_notify` does not touch the controller at all.

So that TODO item stays correctly deferred; M4.6 does not force the identity-free dispatcher.
If a future design adds a genuine bulk rearm, that item is its prerequisite.

### 5.3 The gap the EDGE rule leaves, and `kos_irq_discard`

The EDGE rule above is right and stays: a raise latched while the line was masked MUST
redeliver, or the driver drops real events, and `irq_mask_coalesce` pins exactly that. But
read it together with rule L1 and a hole appears. The kernel is the only principal that can
reach the controller -- the ICU/NVIC sits in `arch_reserved_blocks`, so no grant can name it --
and after the first arm `rearm_locked` never clears an EDGE line again. An EDGE driver that
KNOWS its pending is stale therefore has nowhere to go. That is not hypothetical: it was the
amplifier of the `rx72m` storm, where an edge latched before the driver owned the device
re-fired with nothing able to retire it.

The fix is a new verb, not a change to the rearm policy, because the rearm policy has no
information the driver has: only the driver has read the device and knows the latch
corresponds to nothing.

> **`kos_irq_discard(irq_cap)`** -- drop whatever the controller has latched for the line.
> Needs `CAP_WAIT`, the same right and the same `cap_resolve_e` chokepoint as wait and ack.
> It touches the controller's pending state and NOTHING else: it does not mask, does not
> unmask, and does not move `needs_rearm`, so it cannot arm a line and cannot open one. The
> intended shape is `wait; read the device; discard; ack`, where the ISR has already left the
> line masked and the window is closed by construction.

Changing `rearm_locked` to clear on every EDGE rearm was the rejected alternative: it would
buy the same reach at the cost of the coalesce contract, which is a strictly larger loss --
dropping an event the driver never saw is worse than keeping one it can now retire by name.

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
- The existing substrate already has the shape `docs/design-driver-era-scope.md` (section 6,
  the GPIO decision's shared-port-IRQ-demux note) demands for a shared-line notification --
  **sticky-pending** (the binding's counting semaphore coalesces), **ack before delivery**
  (`irq_event_isr` masks *then* posts, `kernel/irq/irq.cc` (`irq_event_isr`)), and **never a
  parked rendezvous send** (`sem_post` cannot block). No new notification object is needed.

### 6.2 The chip dispatch hook, unified across arches

RISC-V already has the right shape: `kickos_rv_ext_dispatch_dev(void)`, a chip-provided void
function that decides what to post. It now follows the lone-TU pattern, so it is three files
rather than one: the fallback body in
`arch/riscv/rv32imac/kickos_rv_ext_dispatch_dev_default.cc` (`kickos_rv_ext_dispatch_dev`),
the chip override in `arch/riscv/chip/esp32c6/chip_esp32c6.cc`
(`kickos_rv_ext_dispatch_dev`), and the call site in `arch/riscv/rv32imac/switch.S` (the
`.Lextdev` external-doorbell arm). RX has the **wrong** shape:
`int kickos_rx_dev_pending_line(void)` returns ONE line
(`arch/rx/rxv3/arch_rxv3.cc` (the `kickos_rx_dev_pending_line` declaration)) and therefore
**cannot express a group vector with several `ISj` bits set at once** -- which the TRM says
happens.

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

`kickos_rx_default_irq` (`arch/rx/rxv3/arch_rxv3.cc` (`kickos_rx_default_irq`)) collapses to
`g_in_isr++; kickos_rx_dev_dispatch(); g_in_isr--;`.

**Corrected at implementation time: the edge-style `IR` clear does not MOVE into the chip hook,
it DISAPPEARS.** This section used to say it moves, which is wrong twice over. The ICU clears a
dedicated edge vector's `IR` flag **on acceptance** (HW manual Rev.1.20 15.2.1, p.479-480), so
there is nothing left to clear; and a software write after the handler would DROP a request
latched while the line was masked, which on `RXI6` is a lost received byte. For a level group
source the manual says outright "do not write to the IR flag" (same section). So the generic
placement was not merely mis-located, it was harmful on both routing classes -- which is the
sharper form of the bug the prior spike found (`c296feb` sec.1c/3.2).

A second consequence follows and it shapes the dispatch: because an accepted edge vector leaves
no flag behind, **a chip dispatch cannot DISCOVER an edge source by polling `IR`**. `RXI6`
therefore takes a dedicated first-level ISR on its own INTB slot rather than being demuxed, and
only the genuinely grouped, level sources go through the status-word loop.

This makes the fleet-wide rule uniform: **every arch's real-device first-level entry is a
chip-provided dispatch function that may post 0..N logical lines and owns its own clear
discipline.**

The ESP32-C6 needs the *same* generalization for the *same* reason, and this is a finding
worth flagging: **all UART0 sub-interrupts on the C6 share ONE interrupt-matrix source and
ONE `UART_INT_ST` register**, so `kickos_rv_ext_dispatch_dev` -- which today hard-codes
`kickos_isr_irq(UART0_TX_LINE)` (`arch/riscv/chip/esp32c6/chip_esp32c6.cc`
(`kickos_rv_ext_dispatch_dev`)) -- must become a real demux
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

`arch/rx/chip/rx72m/irq.h` (`enum vector`) currently has `SCI6_TEI = -1, SCI6_ERI = -1` and
states plainly they are "NOT implemented or referenced anywhere in the tree yet". M4.6
implements them.

**Line-number allocation.** RX's `arch_irq_*` splits at `SOFT_IRQ_LINES = 32`
(`arch/rx/rxv3/arch_rxv3.cc` (`SOFT_IRQ_LINES`)): `< 32` = software-inject logical lines,
`>= 32` = real ICU vectors, `IER`-gated via `icu_ier_set` (vector-indexed) and `IPR` via
`vector_to_ipr`. `rx72m` sets `KICKOS_MAX_IRQ = 256`
(`boards/rx72m/configs/base/defconfig` (`KICKOS_MAX_IRQ`)), so:

- `RXI6 = 86` and `TXI6 = 87` fit the existing `>= 32` real-vector path with **no seam
  change** -- `vector_to_ipr` is identity for both.
- `TEI6` / `ERI6` do **not**. Their "line" is a group SOURCE, not the group VECTOR 110;
  masking is `GENBL0.ENj`, not `IER[src>>3]`. **Decision: allocate group sources a dedicated
  logical-line range above the vector space**, based at 256:

  ```c++
  // arch/rx/chip/rx72m. Group sources are logical lines above the ICU vector space.
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

Verified at HEAD: `arch/rx/chip/rx72m/chip_rx72m.cc` (`arch_reserved_blocks`)
reserves `{mmap::ICU, 0x400}` = `0x87000..0x873FF`. That covers `IR`/`IER`/`IPR` but **NOT**
the group registers the demux will use (`GRPBL0 0x87630`, `GENBL0 0x87670`,
`GRPAL0 0x87830`, `GENAL0 0x87870`).

**Required before the demux touches them:** extend the ICU reserved block to span
`0x87000..0x8787F` (size `0x880`), so the Rule-7 grant predicate mechanically refuses to hand
any of it to a driver and the SYSMPU/MPU union stays correct. Cheap; lands in the same commit
as the group demux. This is a **latent correctness hole today**, not merely a future
requirement: an over-broad grant covering `0x8763x` from any `AUTH_MEMORY` holder would
currently succeed.

---

## 7. The UART layer -- policy on top of the mechanism

### 7.1 A userspace SPSC byte ring, factored out

There is **no generic ring anywhere in the tree**: only the kernel's file-static
`ConsoleTxRing` (`kernel/init/console_tx.cc` (`struct ConsoleTxRing`)) and the RTT rings. M4.6
factors one out for userspace, because it needs two instances (TX and RX) and M4.7 will need
more.

A new `byte_ring.h`, in the public `sys` include directory next to `bus.h`:

```c
// Publication barrier for head/tail: KICKOS_CONSOLE_TX_BARRIER generalised out of the
// kernel ring. Compiler-only by default, a real fence on a weakly-ordered core.
#define KOS_RING_BARRIER() /* per-arch seam */

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
`lib/include/kickos/console_tx.h` (`KICKOS_CONSOLE_TX_BARRIER`) generalised out of the kernel
ring: compiler-only by default and a real fence on a weakly-ordered core.

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
byte to the data register itself and advancing `tail`
(`kernel/init/console_tx.cc` (`console_tx_write`, the idle->busy prime)),
which it can do because it holds `IrqLock`. The service thread has no such lock, so it must
**not** touch `tail` or the peripheral. Instead, on the idle -> busy transition it calls
`kos_irq_notify(tx_cap)`; the IRQ thread wakes, sees a non-empty TX ring, enables the
peripheral TX interrupt and pushes the first byte. Identical net effect, single-writer
preserved, and it works on the transition-triggered parts (XMC `TBIEN`, RX `SCR.TIE`) where
merely enabling the interrupt on an idle channel raises nothing.

Both threads are spawned by the same service `start()` with the same `win_base` and the
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
(`docs/design-m4-fable-review.md`, finding 10) is honoured by *not* putting a raw pointer in
this struct.

**Taxonomy check.** `docs/design-driver-era-scope.md` (section 3.2, "Driver API taxonomy by I/O
model") rules UART an ASYNC byte stream served by "endpoint rendezvous + IRQ-as-event ... NO
call/reply needed". This design uses
`kos_call`/`kos_reply` anyway, and that is a deliberate departure, argued: a `WRITE` must
return *how many bytes were accepted* (the ring may be full) and a `READ` must return *bytes
plus a count*. Both are request/response with a result, which is what call/reply is for. The
ruling's substance -- that no *blocking device transaction* is involved, unlike SPI -- still
holds: the driver replies immediately from ring state and never parks the client waiting on
hardware. Using the landed call/reply substrate for a synchronous *result* is cheaper than
inventing a second reply convention. The 1:1 rule is satisfied: four ops, four class methods.

### 7.5 TX and RX policy

**TX (the `WRITE` op).** Service thread: `n = kos_byte_ring_push(...)`; then
`kos_irq_notify(tx_cap)` on EVERY accepted push; reply `{status = 0, len = n}`.

**Corrected at implementation time, and it was a real bug.** An earlier version rang the doorbell
only on an observed idle -> busy transition, which reads as an obvious optimisation and is a LOST
WAKEUP: between testing "the ring is empty" and pushing, the IRQ thread can drain the ring,
disarm
the peripheral TX source and park, leaving the pushed bytes with nothing armed to send them and
no
notify coming -- so they leave only when some later write happens to observe an empty ring, i.e.
on
a quiet console, never. The two sides cannot order it away, because the split that makes the
rings
lock-free is exactly what stops either thread testing the other's index and acting on it
atomically. Notifying unconditionally REMOVES the window rather than narrowing it: there is no
test left to race. The cost is self-limiting, since the notification is a counting semaphore, so
a
post landing mid-drain merely makes the next wait return at once, find nothing, and count itself
in `irq_spurious`. Noted in sec.9.4 as un-gateable: with the conditional gone there is no
interleaving left for a test to force. If `n < len`
the client sees a short write and must push the remainder itself.

**This is WEAKER than the kernel ring, not stronger.** The kernel ring's overflow branch
(`kernel/init/console_tx.cc` (`console_tx_write`, the overflow branch calling `drain_sync`)) never
refuses a byte *while the channel makes progress*: under `IrqLock` it drains the ring to the
peripheral, then poll-pushes the burst straight to the peripheral behind it, so nothing is queued
back and no caller has to do anything. What it spends is an IRQ-masked stall -- latency, not data.
A userspace driver cannot buy that, because it must never mask interrupts, so a short accept is
the only option left to it.

The kernel ring is not unconditionally lossless, and the difference should not be overstated. Both
of its poll loops are bounded by `DRAIN_POLL_CAP`, and a channel that never frees a slot -- a dead
wire, or flow control asserted off -- makes `drain_sync` reset the ring (`console_tx.cc:77`,
`g_tx.tail = head`) and makes the burst loop `return` mid-buffer (`:179`). Both drop silently and
neither increments a counter. So the honest comparison is narrower than "lossless versus lossy":
the kernel trades latency for data whenever the wire is alive, and only degrades to a silent drop
in the case where a userspace driver has nothing to deliver either. The cost of that option is that **correctness moves to the caller**:
whoever writes must loop until the whole buffer is accepted, and a caller that does not silently
drops the tail. That is not hypothetical -- it shipped in every driver here and was found on
silicon, which is why `console_write_all` (`user/include/kickos/sys/uart_service.h`) exists and why
no driver may call `tx_write` directly for a console write. `tx_dropped` counts bytes lost only on
the death path (sec.4.3) and on a client that gives up.

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

`kos_uart_stats` lives in the shared ring block (arena memory owned by root's bring-up),
so it survives the driver thread and is readable after a restart. The `STATS` op exposes it
over the wire; a selftest asserts `rx_dropped == 0` on a paced test and non-zero on a
deliberate flood, so the counter is proven live rather than merely present.

### 7.7 The one-line-serves-RX-and-TX demux

Confirmed from the fleet survey: on **most** chips the console UART's RX and TX share one
interrupt line -- mk64f UART0 = **31** ("UART0 status sources (RX/TX combined)",
`arch/arm/chip/mk64f/irq.h` (`UART0_RXTX_IRQ`)), **though on that chip the RX ERROR sources
(OR/NF/FE/PF) sit on a SEPARATE vector, IRQ 32** (RM Rev.4 3.2.2.3, Table 3-5), which the
"combined" wording covers only for RX and TX. A driver that binds 31 alone therefore recovers
from an overrun on its next TX or RX wake rather than promptly; the fix, if a bench run shows a
wedged receiver, is a second IRQ thread on 32 calling the same `service_irq()` -- the split-line
case below, at a cost of 2 of 8 binding handles, stm32f411 USART2 = 38, stm32f103 USART1 = 37,
stm32f302 USART2 = 38, sam3x8e UART = 8, rp2040 PL011 = 20, rp2350 PL011 = 34,
imxrt1062 LPUART6 = 25, esp32c6 UART0 (one matrix source), esp32 UART0 (one matrix source).

Two chips are different, in opposite directions:

- **xmc4800** -- USIC service-request routing is **programmable**: the TX buffer interrupt is
  aimed at `SR0` (NVIC 84) via `INPR.TBINP` (`arch/arm/chip/xmc4800/usic.cc` (`tx_irq_route`),
  called from `arch/arm/chip/xmc4800/usic_uart.cc` (`kickos_xmc_usic_init`)). RX can be aimed
  at a **different** `SRn`. So XMC can have genuinely separate RX and TX NVIC lines -- with the
  caveat that `SR1` (NVIC 85) is already taken by `xmcssc`
  (`system/driver/xmc4800/xmcssc/xmcssc.cc` (`USIC0_SR1_IRQ`)), so the console RX must pick a
  free node.
- **rx72m** -- fully split, and then some: `RXI6` = 86, `TXI6` = 87 dedicated, `TEI6`/`ERI6`
  grouped. Four sources, three lines needed for a complete driver.

**Decision: the driver handles the shared-line case, and it is the default shape.**
`service_irq()` reads the status register once and services every asserted condition in one
pass. That way the shared case -- 11 of 13 chips -- needs no extra thread.

**Corrected at implementation time: the split-line case CANNOT be "one IRQ thread per line, each
with its own IRQ cap".** That was this section's original answer and the RX72M implementation
proved it unbuildable. Two rules collide. A DEV window has exactly ONE holder
(`kernel/domain/domain.cc` (`domain_for`), "one grant == one domain == one thread"), so only ONE
thread can clear the peripheral's flags; and RULE L1 means a LEVEL line may only be awaited by a
thread that can clear its source, because rearming without clearing storms. So a second IRQ
thread on a second line of the SAME peripheral would need registers it cannot be granted.

What the RX72M driver does instead, and it is the pattern for any chip whose sources span
routing classes: it waits on the TX line, and a register-less **RELAY thread** converts the RX
line into a `kos_irq_notify` on that same binding. That is sound only because `RXI6` is EDGE --
an edge source needs no peripheral clear before rearm, so a thread holding no window may
legitimately consume it. The two grouped LEVEL sources (`TEI6`/`ERI6`) are deliberately NOT
claimed at all; their flags are cleared and counted inside every `service_irq()` pass by the one
thread that owns the registers. The primitive that would make the clean shape expressible is
wait-on-either-of-two-sources, the same M5 object sec.7.5 defers the blocking read behind.

**Binding-pool budget, and a real ceiling.** `KICKOS_MAX_IRQ_HANDLES` is 8 by default but
**4** on bluepill-c8, f302nucleo and microbit (the `KICKOS_MAX_IRQ_HANDLES` override in each of
`boards/bluepill-c8/configs/base/defconfig`,
`boards/f302nucleo/configs/base/defconfig`,
`boards/microbit/configs/base/defconfig`). One shared-line UART needs 1 handle,
so those boards are fine. The RX72M three-line UART needs 3 of 8. This is now a *recoverable*
budget rather than a permanent burn, because Decision 1 gives the pool a free path -- which is
the second concrete payoff of making the line a capability.

---

## 8. Per-arch feasibility

Effort scale: **S** = a few hours, in-tree, verifiable by construction. **M** = a day-plus, new
register work. **L** = new subsystem. "HW" = validation is silicon-gated.

| Arch / chip | Tier-1 real line today | What M4.6 needs | Effort | HW-gated |
|---|---|---|---|---|
| **ARM v7-M** (mk64f, xmc4800, stm32f411/f103/f302, sam3x8e, imxrt1062, mps2) | **YES, proven on silicon.** Per-line NVIC `ICER`/`ISER`/`ICPR`/`ISPR` (`arch/arm/common/arch_arm_common.cc` (`arch_irq_mask`, `arch_irq_inject`); `arch/arm/armv7m/arch_armv7m.cc` (`arch_irq_unmask`, `arch_irq_clear_pending`)). Working tier-1 drivers: xmcssc on NVIC 85, f411spi on 35, k64drv on 50 | nothing at the arch layer | **S** | no |
| **ARM v6-M** (rp2040, nrf51) | **YES** (`arch/arm/armv6m/arch_armv6m.cc` (`arch_irq_unmask`, `arch_irq_clear_pending`)) | nothing at the arch layer | **S** | no |
| **ARM v8-M** (rp2350 M33) | **YES** (reuses the v7-M path, `arch/arm/chip/rp2350/startup.S` (`g_isr_vector`, the external-interrupt `.rept`)) | nothing at the arch layer | **S** | no |
| **RXv3 / rx72m** | **PARTIAL.** Real ICU mask/unmask for `line >= 32` is correct (`arch/rx/rxv3/arch_rxv3.cc` (`arch_irq_mask` / `arch_irq_unmask`, their `icu_ier_set` arms)), **ARCH SIDE NOW LANDED**: the single-line hook was replaced by `kickos_rx_dev_dispatch` (`arch/rx/rxv3/arch_rxv3.cc`, `arch/rx/chip/rx72m/chip_rx72m.cc`), which is what `rxsci` dispatches through. `kickos_rx_dev_pending_line` never shipped and no longer names anything | (a) `kickos_rx_dev_dispatch` replacing the single-line hook, with the IR-clear moved into the chip (sec.6.2); (b) `RXI6 = 86` route -- a small delta on the proven TXI6 path; (c) **GROUPBL0 demux**: the group first-level ISR, the `ISj -> line` table, the `line >= 256` branch in `arch_irq_mask/unmask/clear_pending` toggling `GENBLn.ENj`, group register constants; (d) **extend the ICU reserved block to 0x880** (sec.6.4); (e) `SCI6_TEI/ERI` constants | **M** | **yes** -- and this is the milestone's largest single piece of arch work |
| **RISC-V / esp32c6** | **NOW LANDED for a userspace driver.** The bug shape below was a HALF-DEFINED PAIR; the missing half is defined (`arch/riscv/chip/esp32c6/chip_esp32c6.cc` (`arch_rv_hw_mask`)) and `c6uart` runs the whole suite on silicon. Kept for the shape: `arch_rv_hw_unmask` was defined by the chip (`arch/riscv/chip/esp32c6/chip_esp32c6.cc` (`arch_rv_hw_unmask`)); its twin `arch_rv_hw_mask` is not, so it links the lone-TU fallback (`arch/riscv/rv32imac/arch_rv_hw_mask_default.cc`) and `irq_event_isr`'s mask sets only a software bit while the PLIC enable, `mie` and `UART_INT_ENA` stay live. The seam is present and one half of it is wired; unmask arms the line that mask cannot disarm. Compounding it, `kickos_rv_ext_dispatch_dev` calls `kickos_isr_irq` **unconditionally**, hard-coded to one line (`arch/riscv/chip/esp32c6/chip_esp32c6.cc` (`kickos_rv_ext_dispatch_dev`)). On a level source that is an interrupt storm | (a) **define the missing half**: an `arch_rv_hw_mask` override clearing the PLIC `MXINT_ENABLE` bit for the device CPU int that `arch_rv_hw_unmask` sets -- coarse but kernel-owned, per RULE L1 (sec.5.1); (b) turn `kickos_rv_ext_dispatch_dev` into a real `UART_INT_ST` demux posting per sub-source; (c) a per-line route table so more than one real line is expressible | **M** | **yes** |
| **RISC-V / qemu virt** | no real device routed at all (semihosting console, `chip_virt.cc`) | nothing -- stays the inject-only substrate target. Useful for testing the *cap* mechanism in CI without hardware | **S** | no |
| **Xtensa LX6 / esp32** | **WAS the worst case: the mask pair was absent entirely, not half-defined. ARCH SIDE NOW LANDED** -- `kickos_lx6_hw_mask` / `kickos_lx6_hw_unmask` clear and set the INTENABLE bit of the CPU interrupt a line routes to, a 4-entry route table replaced the single hard-coded console line (`arch/xtensa/lx6/arch_xtensa.cc` (`kickos_lx6_bind_dev_int`)), and the L1 dispatcher delegates to a chip demux that posts only an ASSERTED sub-source, 0 posts being a valid outcome (`arch/xtensa/chip/esp32/chip_esp32.cc` (`kickos_lx6_dispatch_dev`)). Two corrections to the original reading: the predicted storm was LATENT, not live, because bring-up enables only TX-empty; and the kernel's own storm guard (`kernel/irq/irq.cc` (`irq_default_handler`)) was already written but INERT on this chip, since `arch_irq_mask` reached no controller -- so the hook activates a guard that existed. `INTSET` still latches only software ints 7/29, so `arch_irq_inject` cannot fake a real line here, which is another reason `kos_irq_notify` is the doorbell (sec.2.6) | the UART driver is BLOCKED on documentation, not on code: see sec.8.1 | **M** | **yes** |
| **sim** | dispatch bypasses the mask: `console_tx_service()` calls `kickos_isr_irq(TX_LINE)` without consulting `sim().irq_masked` (`arch/sim/sim.cc` (`console_tx_service`)). It also ships a console service list (`system/init/sim/service_list.cc`), so it is the one hardware-free target that can lose a console driver | (a) make the TX-line delivery respect `irq_masked`, so the sim faithfully models mask-until-ack. **NOT** an `arch_console_reclaim` body: fd 1 has no state to restore, so the no-op fallback is correct (sec.4.4). With (a) the whole cap / teardown / level / reclaim-on-death design is testable in CI with no hardware, which no other target in the fleet offers | **S** | no |
| **rp2350 Hazard3** | **does not exist in tree** (no `arch/riscv/chip/rp2350*`; design-only, `docs/design-rp2350.md` ("DEFERRED (b): Hazard3 RV32IMAC(B) target")) | out of scope | -- | -- |
| **mps2, nrf51, virt** | semihosting console, no `arch_console_tx_backend` | no UART driver to build; they validate the *general* mechanism via injected lines | **S** | no |

### 8.1 The esp32 blocker was a MISSING MANUAL, and it is now gone

Kept because the shape recurs. For one working day the classic-ESP32 TRM was not in the local
reference set -- only the WROOM-32 MODULE datasheet, which carries pinout and electricals and no
register map -- and the C6 TRM that WAS present is a different chip. The arch mask and demux were
built from facts already transcribed in-tree, the driver was DECLINED rather than half-built, and
the unknowns were listed rather than guessed. The manual arrived on 2026-08-01 and the driver
followed the same day.

**What the verification pass found is the argument for having declined.** Every register VALUE in
the tree was correct. But two citations pointed at the wrong chapters entirely (the peripheral
base
was cited as "TRM 1.5.3" when chapter 1 is the ULP coprocessor and the table is 3.3-6; the Timer
Group was cited as chapter 18, which is the RNG), and one SEMANTIC claim was wrong in the
direction
that costs a hang: `regs/uart.h` said TX-empty is "a LEVEL source, so `INT_ENA` and not `INT_CLR`
is what gates it". The condition is level on occupancy, but the RAW bit is a LATCH -- so a driver
author trusting that sentence would have skipped the per-push clear and stalled every burst that
filled the FIFO. The kernel's own polled ring never noticed because it gates on `INT_ENA`.

**The highest-value fact only the manual could give**, and it shapes the RX path on this chip:
`RXFIFO_FULL_INT_CLR` may be written ONLY while the FIFO holds less than `RXFIFO_FULL_THRHD`, and
`RXFIFO_TOUT_INT_CLR` only while the counts are zero (TRM v5.8 Reg 19.5). Written BEFORE the
drain
those clears are a silent no-op and the source keeps re-asserting. So the driver clears RX-full
only AFTER draining, and does not enable the timeout source at all, its clear being gated on a
condition the driver cannot guarantee at clear time.

Also recorded, unexploited: TRM 8.3.3 says writing an INTERNAL interrupt number into a source's
map
register disables that source for the CPU -- a per-SOURCE kernel-owned mask, finer than the
`INTENABLE` bit this port uses. Not adopted (INTENABLE is core-local and single-cycle, the map
write is an APB round-trip in an ISR), but it is the escape hatch if a second peripheral ever has
to share CPU interrupt 13.

Two hard numeric ceilings to design against, both confirmed:

- **Software-controller backends cap at 32 lines**
  (`arch/riscv/rv32imac/arch_rv32imac.cc` (`arch_irq_mask`) returns early on `line >= 32`;
  `SOFT_IRQ_LINES = 32`, `arch/rx/rxv3/arch_rxv3.cc` (`SOFT_IRQ_LINES`); `& 31u` on Xtensa;
  `SIM_IRQ_LINES = 32`), and the C6 / esp32 / sim boards set `KICKOS_MAX_IRQ = 32`. A UART
  needing RX + TX + error lines must fit inside `0..31` alongside the bench and selftest
  lines (esp32 already reserves 5/6/7/9/11 and 20,
  `arch/xtensa/chip/esp32/irq.h` (`enum kernel_line`, and the reserved-line note above it)).
  This is a second, independent argument for the C6/LX6 UART being **one grouped line** rather
  than three.
- **`arch_console_reclaim` exists on only four chips** (mk64f, xmc4800, esp32c6, esp32) while eleven
  `system/init/` service lists now reference `KOS_SVC_CONSOLE`. The sim needs no body (sec.4.4:
  fd 1 holds no state), so what it uniquely closes is the ownership half, and it does -- that is
  `sim_driver_death`. The remaining gap is per-chip bodies on the rest of the fleet, M4.7 work,
  where a driver death still leaves the console dark.

---

## 9. Validation plan

Two things must be proven separately, and conflating them is how a green suite hides a broken
mechanism (the M4.5.8 lesson): that the **cap machinery** behaves -- gate, rights, teardown,
level rearm -- and that a **real device** driven through it moves bytes. The first is
hardware-free and belongs in CI. The second is silicon by construction, because no emulated
target in the fleet carries a real device UART with a real level source (sec.8).

### 9.1 New selftests

Registered in `user/apps/common/selftest/main.cc` next to the eight existing `irq_*` cases
(`irq_thread_ctx`, `irq_as_event`, `irq_mask_coalesce`, `irq_autorearm`, `irq_phantom_wake`,
`irq_ownership`, `irq_spurious`, `irq_stale_register`), which are the style and the
injected-line pattern to follow. The raise primitive these lean on
(`KOS_SYS_IRQ_INJECT`) is compiled only under `KICKOS_ENABLE_SELFTEST`
(`kernel/syscall/syscall.cc` (the `KICKOS_ENABLE_SELFTEST` block)), so none of this reaches a
production image.

**The case NAMES below are as designed, not as landed**, and the suite consolidated them: what
exists today is `irq_claim_gate`, `irq_reclaim`, `irq_discard`, `irq_thread_ctx`, `irq_as_event`,
`irq_mask_coalesce`, `irq_autorearm`, `irq_phantom_wake`, `irq_ownership`, `irq_spurious`,
`irq_stale_register` and `byte_ring`. Every ASSERTION in this table is covered by one of those;
none of the eleven names below is a `tap::add` site. Read the rows for what they pin, and grep the
suite for the arm that pins it.

| Case (as designed) | What it asserts | Closes |
|---|---|---|
| `irq_claim_refused` | a worker at authority 0 gets `-KOS_EPERM` from the mint | hole 1 |
| `irq_claim_owner` | root's claim succeeds; a second claim of the same line is `-KOS_EBUSY`; a tier-2 attach on a tier-1-claimed line is refused too | sec.3.2 |
| `irq_claim_masked` | INVARIANT H1: between claim and the first wait an injected raise reaches no handler; the first wait arms the line and then delivers it | sec.3.2 |
| `irq_cap_bad` | wait / ack / notify on a non-IRQ cap is `-KOS_EBADF`; on a cap whose slot was freed and recycled, still `-KOS_EBADF` (the generation bump) | sec.4.1 |
| `irq_rights_split` | one binding delegated into two children, WAIT to one and SIGNAL to the other: the WAIT child's notify is `-KOS_EPERM`, the SIGNAL child's wait is `-KOS_EPERM` | sec.2.3 |
| `irq_reclaim_line` | a worker claims a line and exits; a second worker claims the SAME line and succeeds | hole 2 |
| `irq_reclaim_pool` | claim-all-then-exit, repeated more times than the pool has slots | hole 2 |
| `irq_close_releases` | `kos_handle_close` on the line cap frees the line with no thread exit | sec.4 |
| `irq_notify_wakes` | notify returns a wait with nothing asserted and without touching the controller; an inject on the same line goes through the controller and sets the rearm flag, notify does not | sec.2.6 |
| `irq_level_rearm` | on a LEVEL-flagged claim the ack discards the latch before unmasking, so a raise latched before the driver's clear does not phantom-wake the next wait | sec.5 |
| `byte_ring_spsc` (landed as `byte_ring`) | wrap, full-versus-empty disambiguation, usable capacity `size-1`, short push and pop counts | sec.7.1 |

`irq_rights_split` is the load-bearing one, and it is worth saying why: the rejected registry
alternative (sec.2.1) could not express two threads sharing one line with different rights **at
all**, so this case is the regression test for the decision itself rather than for its code. It
is also the case that forces two delegated caps into one spawn -- see sec.11, question 3.

Three coverage limits, stated rather than discovered:

- **Every refusal arm needs a worker, not root.** The suite declares five of the six authority
  bits including `AUTH_IRQ` (`user/apps/common/selftest/main.cc` (`KICKOS_APP_AUTHORITY`)), so a
  root-side mint check asserts the *grant* and can never witness the *refusal*. The shape to
  copy is `t_cpu_clock_set`, which already moves its arm into a worker for the same reason.
- **The UART wire ABI (sec.7.4) gets no emulated case.** Configure / write / read / stats round
  trips, the short write on a full ring, and the flood that must move `rx_dropped` off zero all
  need a device; they are board cases, listed per board in sec.9.2.
- **`rx72m` has no CTest gate of any kind** (board matrix, `STATE.md`), so every claim in
  sec.6.3 and sec.9.3 is bench-only from the start.

**Two ctest gates landed for the UART layer**, and what each cannot see is the point.
`sim_uart_loopback` (`tests/integration/check_sim_uartloop.sh`) runs the REAL two-thread driver over a
loopback device: nothing raises that line, so the only thing that can move a byte is the service
thread's doorbell waking the IRQ thread -- remove the notify and the read returns nothing, which
is how the doorbell is mutation-proved. It also checks CONTENT, not counts, so a ring mask or
wrap bug surfaces as a mismatch rather than a plausible length. What it cannot witness, and what
therefore stays owed to `xmc4800`: a hardware TX-empty interrupt driving the drain, asynchronous
RX from a real line, and the transition-triggered half of RULE T1 -- a host write cannot fail to
raise, so nothing here would notice a driver that waited for an interrupt to send its first byte.
The `uart_service` selftest case covers the request/reply surface with no driver at all.

**One new console ctest gate, and it is the milestone's most valuable one.** `sim_driver_death`
(`tests/integration/check_sim_drvdeath.sh`, **LANDED**): the sim publishes a console, its driver exits, and
the kernel console must come back. It is the **only** vehicle in the fleet that turns
reclaim-on-death from a bench errand into a CI assertion, because every other console gate
either keeps the driver alive or ends the system. The existing `sim_published_console` and
`sim_published_panic` gates are the pattern (`tests/integration/check_sim_published.sh`,
`tests/integration/check_sim_pubpanic.sh`).

Two implementation facts worth carrying, both discovered by building it. **The driver has to die
on its own schedule**: no kernel path wakes a receiver parked in `kos_recv` when the last
`SIGNAL` holder goes -- only the mirror case exists (`recv_holders` -> 0 EPIPEs parked SENDERS)
-- so closing the sender caps cannot kill it, and `simcon`'s own `n < 0` break is unreachable
defence rather than a working death story. That is the `kos_cap_narrow` endpoint-rights residual
(`TODO.md`) seen from the other side. The gate therefore bounds the driver to N served messages
(`KICKOS_SIMCON_EXIT_AFTER`, a knob on the ONE sim console driver -- a divergent second provider
would stop being the reference the silicon services mirror). **And the assertion is a PAIR of
results from one `kos_print` call site**: absent before the death (which proves the handover
happened), present after (which proves the reclaim). Either half alone is passable by a
regression. Per M4.5.8's rule it is proved RED by mutation, and both halves of the mechanism
were mutated independently -- dropping the `exit_current` call, and dropping the cap-layer note.

### 9.2 Board-by-board silicon order

Ordered so that each board is the cheapest remaining witness for something no earlier board
could show, and so nothing silicon runs before the hardware-free half is green.

| # | Target | Why here | Bench |
|---|---|---|---|
| 0 | sim, then the five `qemu` run gates | the whole cap / teardown / rights / level design on injected lines, plus `sim_driver_death`. A failure here is cheaper by orders of magnitude than the same failure found on a board | none needed |
| 1 | `xmc4800-relax` | the flagship: enforcing PMSAv7, an `arch_console_reclaim` body, a landed tier-1 driver on NVIC 85, and programmable USIC service-request routing so it is the ONE board that can witness both the shared-line default and the split-line special case (sec.7.7) | present |
| 2 | `frdmk64f` | the other chip with a reclaim body, and the opposite trigger class -- level-asserted `TDRE` against the XMC's transition-triggered `TBIEN`, which is the pair sec.9.3's assumption turns on. Coarse-AIPS, so it witnesses functional handover, not peripheral isolation | present |
| 3 | `rx72m` | the largest single piece of arch work in the milestone and the only group-demux witness in the fleet. Its own subsection below | present |
| 4 | `esp32c6` | the half-defined mask pair. **Must not be attempted before the `arch_rv_hw_mask` half exists** (sec.8): claiming a level source whose mask is a software bit is an interrupt storm, not a failed test | present |
| 5 | `esp32-wroom` (LX6) | the absent pair, worst case, one physical device line in total. Last of the real-device targets because it needs everything the C6 needed plus the hook that does not exist | present |
| 6 | `f302nucleo` | not a UART target: it is the binding-pool ceiling witness, `KICKOS_MAX_IRQ_HANDLES = 4`, and the ring-only arm | present |

`f411disco` is deliberately absent: it is not on the bench, and its outstanding `f411spi`
witness is M4.6.3..N debt that this milestone must not wait on.

### 9.3 The RX72M chicken-and-egg, and the `SCR.TIE` assumption

**The assumption.** `console_tx_write` primes the pump on the idle -> busy transition: it enables
the TX interrupt and then pushes the first byte directly, because on a transition-triggered part
enabling the interrupt on an idle channel raises nothing
(`kernel/init/console_tx.cc` (`console_tx_write`, the idle->busy prime)). That comment names the
RX SCI `TXI` as transition-triggered **"pending HW confirmation"**, and the confirmation was
never done. The chip side matches it: the console leaves `TE` on with `TIE` off and a comment
saying the ring primes it (`arch/rx/chip/rx72m/chip_rx72m.cc` (`sci6_console_init`), with
`rx_tx_irq_enable` / `rx_tx_slot_free` as the backend arms).

**Why the kernel gets away with it.** The prime is written to be correct either way: load-bearing
on a transition-triggered part, and a harmless immediate send on a level-asserted one. So the
RX72M console has worked on silicon for the whole project without the question ever being
answered. That is exactly the shape that makes an assumption dangerous to inherit -- silicon
evidence of a working console is **not** evidence for the trigger class.

**What M4.6 changes, and the trap.** The userspace driver cannot prime the way the kernel does:
the service thread owns neither `tail` nor any peripheral register (sec.7.2), so it rings the
doorbell instead and the IRQ thread performs both steps. The good news is structural -- the IRQ
thread's `service_irq()` does the identical enable-then-push pair the kernel does, so **the
design as written is independent of the trigger class too.** The trap is the tempting
simplification: an implementation that enables the TX interrupt and then *waits for an interrupt*
to push the first byte deadlocks on a transition-triggered part, with an empty channel that will
never raise. State it as a rule for the driver author rather than leaving it to be rediscovered:

> **RULE T1.** The first byte of a burst is pushed by the same pass that enables the TX
> interrupt, never by a later interrupt. Holds on both trigger classes; required on one.

**T1 is CONFIRMED required on the XMC, and the reason settles the RX case by CONTRAST rather
than by analogy -- do not let the XMC result travel.** The USIC transmit-buffer interrupt is
**edge-per-word**: the event occurs when a word is loaded from `TBUF` into the shift register,
"with the transmit clock edge that shifts out the first bit of a new data word" (RM V1.3
18.2.2.4, p.18-18; ASC-specific at p.18-64, "PSR.TBIF is set after the start of first data bit
of a data word"). No word moving, no event -- so enabling `TBIEN` on an idle channel raises
nothing and a driver that then waits for an interrupt to send byte one hangs. Two consequences
beyond T1. There is **nothing to gate**, so an empty ring cannot storm and the driver never
needs to disable `TBIEN`: it writes `CCR` exactly once, at bring-up. And `PSR.TBIF` is not the
interrupt source at all ("the actual status of the event indication flag has no influence on
the interrupt generation ... does not need to be cleared to generate further interrupts", RM
18.2.2.3, p.18-17), so `PSCR.CTBIF` is cosmetic here and there is no level-clear protocol to
write. The RX72M SCI is the OPPOSITE shape of question and stays open below.

That is an inference from the event definition rather than a quotable negative sentence, so it
is on the silicon list in sec.9.4 -- and it is the same reading the kernel's own ring already
relies on (`kernel/init/console_tx.cc` (`console_tx_write`, the idle->busy prime)).

So the `SCR.TIE` question does not gate the design, and it should still be **answered on the
bench while the board is up**, because sec.7.5's drain loop and the tail-loss accounting both
read better with it settled: with `TE` set, `TDR` empty and nothing ever transmitted, set `TIE`
and observe whether `TXI6` fires. One reading, and it also settles the same open claim for the
PL011 by analogy only, not by evidence -- do not let it travel.

**The second chicken-and-egg, and this one is real.** A group source is armed by `GENBL0.ENj`,
but nothing reaches the CPU unless the **group vector itself** is enabled -- vector 110 via
`IER0D.IEN6` and its `IPR` (sec.6.3). Vector 110 is not a claimable logical line: no cap names
it, and it must not become one, because a thread owning "the group" could starve every other
source in it. So arming `TEI6` cannot be expressed by the line's own mask alone, and the
generic `arch_irq_unmask` has nobody to ask.

**Decision: the group vector is armed lazily by the chip, refcounted per group.** The
`line >= 256` branch keeps a small per-group count; the first source armed in a group enables
that group's vector `IER` bit and `IPR`, and the last one disarmed clears it. It stays entirely
inside the chip layer, it stays kernel-owned as RULE L1 requires, and it needs no pseudo-line
and no new seam.

Rejected alternative: **arm every group vector unconditionally at chip init.** Simpler, and
wrong in a way that would be hard to find later -- with no source enabled, a spurious or
still-latched group assertion reaches a first-level ISR that finds `GRPBL0` all zero and
returns, which is a silent storm on a level input rather than a diagnosable fault. The
refcount also makes "no source claimed" mean "vector masked", which is the state the reclaim
path in sec.4 already restores per source.

### 9.3.2 The same question answered per chip, because the answers DIFFER

RULE T1 is a rule precisely because the trigger class is per-chip. What is now known:

| Chip | TX source | T1's prime | Evidence |
|---|---|---|---|
| **xmc4800** U0C0 `TBIEN` | **edge per word** | **LOAD-BEARING** -- enable-then-wait deadlocks on an idle channel | RM V1.3 18.2.2.4 p.18-18, ASC p.18-64; inference from the event definition, so still on the silicon list |
| **mk64f** UART0 `C2.TIE` | **level while empty** | harmless immediate send | RM Rev.4 52.3.5: S1 resets to `0xC0` with `TDRE` already set and nothing transmitted, and the flag "reasserts until the watermark has been exceeded" -- a positive reading, not an inference |
| **esp32** UART0 TX-empty | level on occupancy | harmless immediate send | **settled** from TRM v5.8 Reg 19.10: the raw interrupt is produced while the TX FIFO holds LESS than its threshold |
| **rx72m** SCI6 `SCR.TIE` | **OPEN** | unknown | `kernel/init/console_tx.cc` marks it "pending HW confirmation"; sec.9.3 above |

**Both ESP parts carry a second, orthogonal trap that T1 says nothing about**, and it is the one
that actually bites: the interrupt RAW bit is a LATCH, not a level follower. `ST == RAW & ENA`,
and
RAW is dropped only by writing 1 to the matching `INT_CLR` bit. So the driver must clear it after
EVERY push -- occupancy changed, the latch did not -- and must NOT clear it when the drain loop
stopped on a FULL FIFO, because clearing there with occupancy above the threshold leaves nothing
to
re-raise and the burst stalls. An in-tree comment asserting the opposite ("a LEVEL source, so
INT_ENA and not INT_CLR is what gates it") was wrong in exactly the direction that costs a hang.

Two things follow. The XMC answer must NOT travel: it settles that chip by contrast with the
others, not by analogy, and the RX72M SCI is a different question that stays open. And a driver
must obey T1 on every chip regardless, because the `Uart` class contract is shared across chips
where it IS load-bearing -- obeying it costs one reordering and buys portability.

**A same-IP contradiction worth carrying**, because it is the trap for whoever adds RX to the XMC
console. TX (`TBIF`) is edge-per-word and needs no flag clear at all. But `xmcssc`, on the SAME
USIC IP and silicon-validated, found the RECEIVE flags DO need a `PSCR` W1C before re-arm or "an
un-cleared level re-asserts SR1 and storms it"
(`system/driver/xmc4800/xmcssc/xmcssc.cc` (the `PSCR_CLEAR_RX` write)). So the clean
no-clear reading holds for TX only, per SOURCE and not per channel. That, plus the unsettled
question of whether an ASC single-byte frame raises `RIF` or `AIF` (`xmcssc` enables both), is
why
the first XMC console driver ships TX-only.

### 9.3.1 The seam widening the XMC console driver needs, and why it is safe

The driver runs unprivileged with only the U0C0 window, and `CCR` is one of the three
`Write = PV` registers (`docs/reference/porting.md`), so its single interrupt-enable write must
go through `arch_periph_reg_write`. The allowlist carried **U0C1 rows only**, and its `CCR` mask
deliberately withheld `TBIEN`. **Ruled: add exactly one U0C0 row**, with a mask of its own
(`CCR_GRANT | CCR_TBIEN`) so a console-only bit cannot reach `xmcssc`'s U0C1 entry -- the table's
mask is per-entry, which is what makes that the narrowest available shape. No `FDR` or `BRG` row
for U0C0: baud stays kernel-owned, which `cpu_clock_set`'s veto assumes.

**`BRGIEN` stays withheld, and not for symmetry.** It is clocked by the baud generator, so it
free-runs with the driver doing nothing; the bounded-CPU-tax argument that makes `TBIEN` safe
(the driver must keep feeding words for events to keep coming) does not cover it.

Why the widening is safe, stated so it can be checked rather than trusted. A DEV window has
exactly ONE holder, so U0C0 belongs solely to this driver, and it can already write every
unprotected register in that block -- it can garble its own channel completely whatever the mask
says. `PV` is a bus-level guard against stray writes, not this project's isolation boundary, so a
mask earns its keep only where a bit's effect ESCAPES the block or takes something from the
kernel. And a dead driver's stuck `TBIEN` reaches nobody: `irq_event_isr` masks on delivery,
`cap_teardown` -> `irq_ref_drop` -> `irq_detach` nulls the handler and masks the line, and
`arch_console_reclaim` writes `CCR = 0` before re-initialising
(`arch/arm/chip/xmc4800/usic_uart.cc` (`arch_console_reclaim`)). With the edge-per-word finding
above, a dead driver pushing no words raises at most one further event and then goes quiet.

**What the mask does NOT buy, recorded because it is easy to overrate.** Interrupt enables route
to `SR[5:0]`, which RM 18.7 p.18-153 says are **module-scope, shared between both channels**, so
an enable bit's effect does reach an NVIC line another driver may own. That escape is already
wide open by two unprotected paths and one is already witnessed on silicon: `INPR` is `U,PV`
(the `inprstorm` capture re-points SR0 from the U0C1 window), and **`FMR.SIOx` lets any holder
pulse any SR node with a single unprivileged store**, needing no enable bit at all. So the
per-entry mask here is mostly blast-radius documentation. It stays anyway, because two constants
with two asserts means an edit to one cannot silently widen the other.

### 9.4 What no gate in this plan covers

Recorded so the next pass does not read the tables above as completeness:

- **The buffered-ring panic flush on a userspace driver.** The kernel-ring case is already
  witnessed on `pizero2350` and provably dead on the host (`STATE.md`, *Open blockers*); the
  userspace analogue inherits the same in-env hole and the same "behaviour not in doubt" status.
- **A wrong `arch_mpu_region_pow2()` literal** in any backend the shared ring block passes
  through -- nothing in-tree can catch that class (`STATE.md`), and `rx72m` silicon remains the
  only check for the RX MPU.
- **`pinmux_set` and `periph_clock_hz` are vacuous on every CI target**, so a UART that
  re-derives its baud from the oracle is only meaningfully exercised on silicon.

---

## 10. Staging

Each stage builds and passes the whole suite on its own. The ordering constraint that mattered:
**S1 could not land alone as originally drawn.** Renaming the tier-1 ABI and gating the mint
breaks all four in-tree tier-1 drivers, so their migration is in the same commit; and the three
additive switch arms cannot be written without the ref-drop, so the line half of teardown came
with it too. What S1 actually absorbed, and why, is in its row. S2 is now only the CONSOLE half.

| Stage | Content | Sub-milestone |
|---|---|---|
| **S1** | **LANDED.** the kernel object: one `CapType` enumerator, the binding pool, the parallel refcount, the three additive switch arms, the `cap_resolve_e` arm; the mint with its `AUTH_IRQ` gate; tier-1 wait/ack take a cap; the `AUTH_IRQ` comment in `kernel/include/kickos/cap.h` (`enum CapAuthority`) gains the mint. **In the same commit**: the four tier-1 drivers migrate to root-claims-then-delegates, no compat shim. **The ref-drop came with it**, ahead of its S2 slot: the switch arms cannot be written without it, and a `default:` that asserts is not an option | M4.6.1 |
| **S2** | **LANDED.** the console half of teardown: the cap-layer note at `recv_holders` -> 0 on the published endpoint, the death hook after `cap_teardown` in `exit_current`, and the `sim_driver_death` gate. No sim `arch_console_reclaim` body -- there is nothing for one to do (sec.4.4) | M4.6.1 |
| **S3** | **LANDED with S1**: the level trigger (the claim flag, the rearm helper, the per-binding trigger field) came in the same commit, because the rearm helper is the one place the first-arm discard lives and splitting it would have shipped a knowingly wrong rearm | M4.6.1 |
| **S4** | **LANDED with S1**: the notify syscall and the `kos::Irq` destructor plus move-only, both of which the migrated drivers and the suite already need | M4.6.1 |
| **S5** | **LANDED.** spawn-side delegation: the driver-spawn helper carrying N caps plus the shared data region, with the one-cap wrapper kept so the two landed SPI services do not churn | M4.6.1 |
| **S6** | **LANDED.** console visibility and handover ordering. Root closes its own WAIT cap, then PROVES the driver is serving with a zero-length rendezvous on cap 0 before any client runs; a dead driver EPIPEs that probe, and the death has already reclaimed the console, so the failure is REPORTABLE and boot fails loudly. `handle_close` acts on a pending reclaim note too, so a failed spawn also gives the console back. Second case in `sim_driver_death`. **NOT** reordered to publish-after-spawn: on a chip where the driver takes the UART the kernel was using, the kernel must let go FIRST, so a window in which kernel-console writes are dropped is inherent to a single-owner device -- what the probe removes is any CLIENT running inside it | M4.6.1 |
| **S7** | **LANDED.** the userspace byte ring (`user/include/kickos/sys/byte_ring.h`), with the `KOS_RING_BARRIER` seam generalised out of the kernel ring -- and NOT the same guarantee: the kernel publishes its head under `IrqLock`, so there the macro only pins compiler order, while here producer and consumer are two THREADS and a weakly-ordered core needs a real release fence. A non-power-of-two size is REFUSED rather than masked wrong. Gated by the `byte_ring` case | M4.6.1 second half |
| **S8** | **LANDED except the silicon consumer.**: the wire ABI (`sys/uart.h`, size-asserted like `bus.h`) and the two-thread choreography (`sys/uart_service.h`), gated by the `uart_service` case which drives `serve_one` with NO device -- the peripheral belongs to the IRQ thread, so the whole request/reply surface is host-testable. `KOS_SVC_UART` added as a service kind. The first CONSUMER is the sim LOOPBACK port (`system/init/sim/service_list_uart.cc`, gated by `tests/integration/check_sim_uartloop.sh`), so the two-thread split, the doorbell and the wire ABI are all CI-gated against a real driver. **The silicon consumer LANDED too**: `xmcuartirq` took `U0C0` as the console service (sec.11 q.7's recommended route), witnessed on the wire in `m461h-xmc-uartirq.log`, and four sibling drivers followed on `frdmk64f`, `esp32c6-wroom`, `esp32-wroom` and `rx72m` | M4.6.1 second half |
| **S9** | **LANDED.** per-chip rollout in sec.9.2's order: `frdmk64f`, then `rx72m` (the dispatch-hook replacement, the group demux, the group-vector refcount, the reserved-block extension), then `esp32c6` (the missing mask half first), then `esp32-wroom` | M4.6.1 second half |

The RX72M reserved-block extension (sec.6.4) is the one item that may land **earlier than its
stage**, and should: it is a live grant-admissibility hole today, independent of the demux that
found it, and it is already filed on its own in `TODO.md`.

---

## 11. Open questions the reviewer must rule on

1. **The RX line-space cost.** Sec.6.3 raises `KICKOS_MAX_IRQ` on `rx72m` from 256 to 416 to give
   group sources their own range, costing +1280 B of `.bss` on a 1 MB-RAM part. Accept, or trade
   for a sparse `line -> (group, bit)` map that keeps the table small and adds a lookup to the
   mask path? The recommendation is accept: the part has the RAM, and a sparse map puts a search
   in `arch_irq_mask`, which the latency invariant (sec.2.2) has so far kept lookup-free.
2. ~~**The UART layer has no sub-milestone.**~~ **RULED 2026-07-31: the UART layer IS M4.6.1's
   second half.** Sections 7 and 8 belong to M4.6.1, not to a new number, so M4.6.2 stays USB CDC
   and M4.6.3..N stays the witness pass. M4.6.1 is therefore a large sub-milestone in two halves:
   the substrate (sections 2-6, landed) and the buffered UART on top (sections 7-8, S6-S9). What
   "M4.6.1 is done" means is now: a userspace UART driver serving RX and TX over the line
   capability, with the handover ordering fixed first.
3. **Delegation packing, now that a forcing consumer exists.** The service thread needs TWO
   delegated caps -- the endpoint and the line cap with SIGNAL -- and delegated cap `i` lands at
   child index `i + 1` (`kernel/syscall/syscall_thread.cc` (the delegation loop)), so the first
   lands on `KOS_CAP_CLOCK`, inside the reserved range
   (`system/include/kickos/sys/cap_index.h`), and the second falls past it into the dynamic
   range. It **works today** because nothing seats the clock index, so the delegated cap simply
   occupies a slot held for a service that does not exist yet. (When this was written the
   reserved range was twice as wide and the second cap collided with the authority seat too;
   moving the authority word to the TCB narrowed the range and removed that half of the
   problem, along with the spawn refusal that had been guarding it.) So M4.6 needs no spawn-ABI
   work -- and it entrenches the debt one turn further, on the clock index, for a child that
   merely happens not to need a clock cap. The packing fix was deferred "for want of a forcing
   consumer"; this is one. Fix it here, or record explicitly that the two-thread driver squats
   that index?
   **RULED, and this question is now ANSWERED by cap-table Stage 4.** `i + 1` is only the DEFAULT
   packing. `kos_thread_params::cap_dest` (`user/include/kickos/sys/abi.h`) is an optional
   per-grant array of destination indices, so a spawn that cares places each cap explicitly and a
   0 entry means "default". Nothing squats `KOS_CAP_CLOCK` by construction any more; a driver that
   would have can name its own indices instead. Destinations are bounded against the run the child
   actually receives, and two grants naming one slot is `-KOS_EINVAL` rather than a silent
   overwrite. Gated by selftest `cap_dest`.
4. **Rename in place, or append?** Sec.2.4 renames the mint at its existing syscall number
   because the ABI is unstable until M6. A reviewer who prefers append-only numbering even under
   an unstable ABI should say so now: it is one enumerator either way, but it changes every
   citation of the tier-1 block.
5. **Retire the tier-2 syscall?** Sec.3.6 makes per-line granularity conditional on there being
   exactly one entry point to the line namespace, and the tier-2 userspace door has **one caller
   in the whole tree** -- the suite (`user/apps/common/selftest/main.cc`, the attach and unmask
   cases). The in-kernel attach used by the console ring is a different thing and stays. So
   retiring the *syscall* is cheap, and it converts a REFUSED question (sec.3.6) into an
   expressible one. Out of scope as written; worth an explicit yes or no rather than silence.
7. ~~**Which port carries the first UART consumer, and can it be gated before the bench?**~~
   **RULED 2026-07-31: sim loopback first, LANDED.** `system/init/sim/service_list_uart.cc`
   carries a `KOS_SVC_UART` port whose "device" is host fd 1 plus an internal loopback -- the
   trick `xmcspi` uses on silicon to test a bus with no second party wired -- driven by the real
   two-thread driver and gated by `tests/integration/check_sim_uartloop.sh`. Its own service-list provider,
   not a second entry in `kickos_services_sim`, because one list links per image and the existing
   console gates must keep testing the console posture unchanged. **`xmc4800` is now CLOSED too**, by the recommended
   route: both its USIC channels were taken (`U0C0` the kernel console, `U0C1` `xmcssc`'s), so
   rather than pinning a third channel the driver BECAME the console service on `U0C0`, taking it
   at publish -- which is also where the polled-TX CPU burn goes away. That is `xmcuartirq`,
   witnessed in `m461h-xmc-uartirq.log`.
8. ~~**Is the sim reclaim body in M4.6.1 or M4.7?**~~ **MOOT: neither.** The
   sim needs no reclaim body at all: fd 1 holds no state a dead driver could garble, so the
   no-op fallback is correct and a body would be ceremony that reads as coverage. What
   the sim uniquely witnesses is the ownership state machine, and that landed in S2 as
   `sim_driver_death`.

---

## 12. Deliberately out of scope

Each of these is deferred by an argument somewhere above, not by omission. A future pass that
wants one should read the section named, because most of them are waiting on a specific missing
thing rather than on priority.

- **Blocking RX read, and the primitive it needs.** Sec.7.5: receive-from-either-of-two-sources,
  equivalently a notification bound to a thread. An M5 kernel object. The two non-blocking
  options in that section cover M4.6's real cases, and this must not be smuggled in.
- **Runtime mint-and-delegate to an already-running thread.** Sec.3.4. Spawn-time delegation
  covers every M4.6 and M4.7 case because drivers are spawned with their resources in hand.
- **Per-line IRQ authority.** Sec.2.1 and sec.3.6, REFUSED with a recorded falsifier. Revisit
  when the falsifier fires or when question 5 above is answered yes, and not before.
- **The wake arm of the close protocol for a parked waiter.** Sec.4.2: unreachable without a kill
  primitive, so it lands *with* that primitive. The leak-never-strand guard asserts if the
  unreachable case ever becomes reachable.
- **Re-arming the kernel TX ring after driver death.** Sec.4.4: the reclaim path leaves the
  console polled, which is honest and already implemented. Ring re-arm needs a per-chip
  "restore the ring's assumptions" step that does not exist. M4.7+.
- **`arch_console_reclaim` fleet-wide.** Sec.4.4 and sec.8: four chips have a body, the sim
  needs none (fd 1 holds no state -- S2 and q8 both say so), and every other board degrades to
  "console dark after driver death" -- unchanged from today. M4.7.
- **Several subscribers on one line.** Sec.6.1: two consumers of one source is a service with two
  clients, which the class/service duality already answers.
- **The large-transfer path in the UART wire ABI.** Sec.7.4: stays `-KOS_ENOSYS`, exactly as in
  the SPI/I2C wire contract, so no raw pointer enters the request struct.
- **DMA**, which is where a UART's per-byte interrupt cost actually goes away. Its own milestone.
- **USB CDC**, M4.6.2, which consumes this substrate rather than extending it.

Two adjacent items are **owned elsewhere and must not be re-derived here**: the endpoint-rights
narrow that would give a dying driver a real death story (`TODO.md`, M4.6.1 -- it is the endpoint
half of the same subject sec.4 handles for lines), and the clock-tree service contradiction and
its untimed notifier cascade (`TODO.md`, M4.6.1). Both are filed with their own analysis.
