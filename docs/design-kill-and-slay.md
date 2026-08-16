<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Kill and slay: a death the scheduler grants, not a stranger's teardown

> **Status: LANDED** -- S1 through S4 shipped in M4.8.4 and the ABI is silicon-witnessed
> (TAG `m484sl`, `../STATE.md`). Read section 14 before section 3: it carries the ruling on
> each open question and the three claims that did not survive the tree.

Design gate. Written against `M4.8.4-tail`, reading only.

**S1 through S4 have since LANDED.** Sections 1 to 13 are kept as written, because the
reasoning is what the implementation was judged against and an edited premise cannot be
checked. Section 14 is the record of what landed, where the design was WRONG, what was
ruled on each open question, and what is still open. **Read section 14 before acting on
anything above it**: three claims in sections 3.2, 3.3 and 3.7 did not survive contact with
the tree, and one of them would have been a bug had it been implemented as written.

The subject is class B of the `TODO.md` M4.8.x triage, two of whose three accepted costs converge on
one question:

> Do we need a reaper and let a stranger thread run `cap_teardown`, or do we have a thread
> cap/authority that lets the scheduler know this thread should be killed when it next lists it?

The second. This document designs it, records the reaper as the rejected alternative, and reports
three places where the reasoning that got here does not survive contact with the tree.

## 1. Three premises that changed, stated first

Everything downstream depends on these, so they lead rather than hide in a section.

**1.1 `arch_fault_redirect_to_exit` cannot be reused. It is not relocatable to a saved context on
any backend.** Read as five bodies rather than as a name, the seam is a rewrite of *live* machine
state:

| backend | what it rewrites | why a saved context is out of reach |
| --- | --- | --- |
| `armv6m` (`arch/arm/armv6m/arch_armv6m_fault.cc:38`) | hardware exception frame `f[6]`/`f[7]`, plus a live `msr CONTROL` | privilege for a non-running thread lives in `arch_context::npriv`, which this never writes; the stub would land unprivileged |
| `armv7m` (`arch/arm/armv7m/arch_armv7m.cc:255`) | same, plus it READS and write-1-CLEARS CFSR/HFSR | applied off a fault it would attribute the CPU's current fault status to that thread and destroy it for the real reporter |
| `rv32imac` (`arch/riscv/rv32imac/arch_rv32imac.cc:515`) | live CSRs only; the body is literally `(void)frame;` | nothing at all lands in the thread's frame; the saved `F_MEPC`/`F_MSTATUS` words are never touched |
| `rxv3` (`arch/rx/rxv3/arch_rxv3.cc:380`) | the PC/PSW pair on the ISP, plus `MPU_MPECLR` | that ISP frame is dead after the `rte`; a READY thread has no ISP frame, and the MPU latch is global |
| `sim` (`arch/sim/sim.cc:1102`) | the host `ucontext_t` the kernel materialised for THIS signal | that context exists only until sigreturn, and the body hard-assumes the faulter is `current` |

So a scheduler-driven redirect is not a reuse of the witnessed seam. It needs its own seam. Which
turns out to be cheap for a reason section 3.3 gives, but the reuse claim is false and the design
must not rest on it.

**1.2 The termination argument is not the hardware's, because the clock is TICKLESS by default.**
`ktime_rearm` (`kernel/time/time.cc:70-108`) takes the minimum of the sleep-queue head and
`sched::next_timed_event()`, and when that minimum is `UINT64_MAX` it calls `arch_timer_disarm()`.
`policy_next_timed_event` (`kernel/sched/policy_fifo_rr.cc:150-158`) reports `UINT64_MAX` for a FIFO
current thread. `KICKOS_SCHED_PERIODIC_TICK` is opt-in. So on an image of FIFO threads with no
sleeper there is no periodic interrupt at all, and "the RR slice timer catches the compute loop" is
not available. A design that arms a timer to reach a spinning thread would be adding a clock the
system deliberately does not run.

The design below does not need one. Section 3.1 shows why, and it is a stronger argument than the
timer would have been.

**1.3 The rxv3 below-stack item is SEPARABLE, and separating it is the win.** Both the filing
(`TODO.md:5599`) and design 4.2 name the fix as "a stub that never runs on the dying thread's
stack". Neither a reaper nor a scheduler redirect is required to get that. Resetting the stub's
stack pointer to the top of the dying thread's own stack gets it, in the fault handler, with no new
concept. Section 6 works it out, and it can land before anything else here.

## 2. The ABI

Syscall numbers are append-only; the current high-water mark is `KOS_SYS_TASK_KILL = 52`
(`user/include/kickos/sys/abi.h:169`). The ABI is unstable until M6, so no versioning and no
migration.

### 2.1 What is appended

```c
KOS_SYS_THREAD_SLAY = 53, // (kos_thread_t, timeout_us) -> 0 (the target is EXITED and its
                          //   capability table is swept), -KOS_ETIMEDOUT (the redirect is
                          //   armed and irrevocable, the teardown has not finished),
                          //   -KOS_EBADF (bad/stale/exited/inactive handle),
                          //   -KOS_EPERM (caller did not spawn it),
                          //   -KOS_EINVAL (self, idle, or a privileged target).
KOS_SYS_TASK_SLAY = 54,   // (kos_task_t, timeout_us) -> 0 (the group is empty and its slot
                          //   released), -KOS_ETIMEDOUT, -KOS_EBADF, -KOS_EPERM, -KOS_EINVAL.
```

`timeout_us` matches `KOS_SYS_THREAD_JOIN`'s existing convention (`abi.h:153`), including
`KOS_TIMEOUT_NONE` for the arm-and-return form.

Userspace wrappers, mirroring the existing pair:

```c
int kos_thread_slay(kos_thread_t thread, uint64_t timeout_us);
int kos_task_slay(kos_task_t task, uint64_t timeout_us);
```

### 2.2 Accepted versus gone, exactly

Three guarantee levels, and the middle one is new.

| call | 0 means | the residual |
| --- | --- | --- |
| `kos_thread_kill` / `kos_task_kill` | the request was ACCEPTED and the target's park is broken | a target that never re-enters the kernel never dies |
| `kos_thread_slay` returning `-KOS_ETIMEDOUT` | the target will never execute another unprivileged instruction; its resume is CLAIMED | its capability table is not yet swept, so a name it holds is not yet released |
| `kos_thread_slay` returning 0 | GONE: `state == EXITED`, `cap_teardown` ran to its totality asserts, the domain reference is dropped | the pool slot itself is reclaimed lazily at the next spawn, as today (`ThreadPool::alloc`) |

**Three LEVELS, not three returns.** Every row above is a statement about the TARGET, and there is
a fourth return that is not: `-KOS_ECANCELED` says the CALLER was cancelled while parked and says
nothing at all about the victim, which stays condemned. It is listed in 14.2 with the reason it is
absent here. A caller that reads any non-zero return as "the slay did not take" is wrong.

The kill semantics are preserved bit for bit and must be: the death point at the next syscall ENTRY
and not exit is what gives a driver returning from a cancelled `irq_wait` one window to quiet its
device over memory it already holds (`kernel/syscall/syscall.cc:201-219`). Slay's whole content is
that it denies that window. Two calls, because one call cannot mean both.

`-KOS_ETIMEDOUT` deserves its own line in the header, because it is the only return in the ABI that
is weaker than "gone" and strictly stronger than "accepted". It is also what makes the starvation
hazard of section 3.6 visible in the ABI instead of hidden in a blocking call.

### 2.3 The exit code

Reuse `KOS_EXIT_CANCELLED` (130). A slain thread was cancelled; what differs is who chose the
moment. A slain-specific exit code would be a second truth about one event, and `KOS_EXIT_FAULT` is already
documented as not join-visible (M4.7.9 section 9.2), so nothing reads the distinction. Open question
5 records this as a ruling worth taking explicitly rather than by omission.

## 3. The mechanism

Three steps, of which two already exist.

```
  kos_thread_slay(h)
        |
        |  1. MARK          t->cancel_kind = CANCEL_SLAY          (new: one byte, no new byte)
        |  2. UNPARK        thread_cancel(t)                      (exists: park.cc:144, total over WaitKind)
        |  3. WAIT          park the caller on t's death          (exists: WAIT_JOIN + exit_current's pool scan)
        v
  ... the scheduler eventually decides t runs ...
        |
        |  4. REBUILD       in switch_to, before arch_switch:     (new: one test, one arch seam)
        |                     if next->cancel_kind == CANCEL_SLAY and not next->dying:
        |                        arch_ctx_redirect(&next->ctx, kickos_thread_slay_exit,
        |                                          next->stack_base, next->stack_size)
        v
  t resumes at kickos_thread_slay_exit, privileged, thread mode, at the TOP of its own stack
        |
        v
  sched::exit_current(KOS_EXIT_CANCELLED)   -- the victim runs its OWN teardown, as today
```

Nobody runs a stranger's `cap_teardown`. The victim does, in its own context, through
`exit_current`, which is the funnel every other death already uses (`sched.cc:195`, three existing
callers plus the fault stub).

### 3.1 The state partition, and why the union is total

The two halves must cover every state a thread can be in, or the defect returns in a corner.
`ThreadState` has exactly five values (`kernel/include/kickos/thread.h:24-31`).

| state at slay time | claimed by | how |
| --- | --- | --- |
| `EXITED` | refused | `-KOS_EBADF`, as `thread_kill` (`syscall_thread.cc:557`) |
| `INACTIVE` | refused | `-KOS_EBADF`, as `thread_kill` |
| `RUNNING` | **structurally impossible** | see below |
| `BLOCKED` | steps 2 then 4 | `thread_cancel` aborts the park and makes the thread READY; the rebuild then claims its resume, so unlike kill it never returns to userspace at all |
| `READY` | step 4 | the rebuild claims its resume |

**`RUNNING` is impossible for a target that is not the caller, on one core, and this is the whole
termination argument.** `switch_to` writes `ThreadState::RUNNING` for exactly one thread
(`sched.cc:30`) and demotes the outgoing one to `READY` in the same block (`sched.cc:27`);
`sched::start` (`sched.cc:81`) is the only other writer and runs once. Slay is reached only through
`syscall_dispatch` on behalf of a thread, and a thread executing a syscall is the RUNNING one.
Therefore a target distinct from the caller is `READY` or `BLOCKED` or refused, and both live states
are claimed by a mechanism that acts at the resume rather than at the request.

The consequence worth naming: **a pure compute loop is reachable with no timer, no tick and no
preemption.** For a peer to have called slay at all, the victim must already have yielded the CPU
(by its own syscall, or by an interrupt-driven preemption that happened for its own reasons). The
victim being off-CPU is a precondition of the request existing, not something the design has to
arrange. This is why premise 1.2 costs nothing.

Two preconditions this rests on, both stated rather than assumed:

- **Single core.** On SMP a peer can be genuinely RUNNING and the argument fails outright. M5 owns
  SMP; open question 1 records the shape a slay would then need.
- **No in-kernel slay caller.** There is none today and the design forbids one: from ISR context
  `arch_in_isr()` is true, `kernel().current` names the interruptee, and the `RUNNING` arm becomes
  real. A kernel-internal slay would have to use the fault path's shape (a live frame), which is a
  different mechanism. Refuse it at the seam rather than leave it available.

### 3.2 The redirect point under a deferred switch

This is the interaction the design must get right, because it is live: `switch_to` publishes
`kernel().current = next` at `sched.cc:29`, nine lines before `arch_switch` at `sched.cc:38`, and on
ARM, RISC-V and RX that call only PENDS (`arch/include/kickos/arch/arch.h:56-60`; ARM
`arch_arm_common.cc:55`, RISC-V `arch_rv32imac.cc:159`, RX `switch.S:62`). Another agent is
repairing the class A consequence of exactly this (`TODO.md:5514`) in `kernel/sched/sched.cc` right
now.

**The requirement, stated so it can be coordinated rather than patched around: the rebuild may touch
only the INCOMING thread's context, only inside `switch_to`, only before `arch_switch`.** That rule
is exact on every backend and it is exact *because* of the deferral, not in spite of it:

- The deferred switcher (`PendSV_Handler`, `.Lswitch`, `_kickos_rx_pendsw`,
  `_kickos_int_level1`) SAVES the outgoing thread's live registers into `prev->ctx` and RESTORES
  `next->ctx`. A rebuild of `next->ctx` placed before `arch_switch` is therefore what the switcher
  restores. A rebuild of `prev->ctx` there would be overwritten by the save and silently lost.
- On the immediate backends (`lx6` in thread context, `sim` in thread context) `arch_switch` reads
  `next->ctx` at the call, so the same placement holds with no special case.
- The sim collapses several logical switches inside one ISR into a single physical swap
  (`sim.cc:415-422`). A hook in `switch_to` fires per decision and so cannot miss an intermediate
  target; a hook in the arch layer would.

**Two consequences of the deferred window that must be written down rather than discovered.**

First, `switch_to` is file-local (anonymous namespace, `sched.cc:19-22`) and `reschedule` early-returns
when `next == current` (`sched.cc:92-95`). A thread that is already current and stays current never
passes the hook. That is correct here, because that thread is the caller and self-slay is refused.

Second, and this is the one that needs the maintainer's eye: **the self-slay refusal is only exact
once class A's repair lands.** In the window between `sched.cc:29` and the physical swap, the thread
physically executing is `prev` while `kernel().current` names `next`. If `prev` issues
`slay(prev_handle)` there, the `t == sched::current()` test does not match and the refusal is
defeated -- the same way `sched::wake`'s guard is defeated, for the same reason. **The failure is
benign**: `prev` is marked, is saved normally by the switcher, and is rebuilt into the stub the next
time the scheduler picks it. It dies, which is what it asked for; it gets that instead of
`-KOS_EINVAL`. No isolation escape and no lost teardown. But it is a hole in a stated refusal, and
it closes for free when class A closes. Slay should not ship its own second answer to "who is
current".

### 3.3 The seam, and why it is nearly free

The rebuild needs one thing per backend: rebuild `ctx` so the thread resumes at a given entry,
privileged, in thread mode, at the top of a given stack, discarding every frame it had.

**That function already exists on all six backends. It is `arch_context_init`**
(`arch/include/kickos/arch/arch.h:51`), called on every spawn (`kernel/thread/thread.cc:208`) and on
idle and root at boot (`kmain.cc`), and exercised by every thread on every board in the fleet. It
fabricates a first frame at the top of the supplied stack, with privilege expressed in each
backend's own idiom -- `armv6m/armv7m` write `arch_context::npriv`, `rv32imac` writes the frame's
`F_MSTATUS` with `MPP = M`, `rxv3` writes the frame's PSW, `lx6` discards the argument because
`PS.UM` is 1 either way.

So the seam is a thin wrapper rather than five new bodies:

```c
// Rebuild `ctx` so the thread resumes at `entry`, privileged, in thread mode, at the top
// of [stack_base, stack_base + stack_size). Every frame the thread had is discarded.
// Returns false where the backend cannot express it.
bool arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size);
```

Its shared body is `arch_context_init(ctx, entry, nullptr, stack_base, stack_size, /*privileged=*/1)`
plus `return true`. Three backends need one extra line and the reasons are worth recording, because
each is a place a reader would expect the shared body to suffice:

- **`sim`**: privilege there is not in the context at all. It is the guard-page posture -- the
  per-thread `mprotect` set plus the mid-syscall `raised` state -- and `arch_context_init` explicitly
  discards its `privileged` argument (`sim.cc:899-901`). `arch_switch` calls `guard_apply_current()`,
  which programs the thread's RESTING grant, so a stub resumed through the shared body would
  `SIGSEGV` on its first read of kernel state. The sim's wrapper must raise the arena for the
  rebuilt context, which is what `arch_fault_redirect_to_exit` does at `sim.cc:1114-1118`.
- **`sim`, second line**: `arch_context_init` substitutes a malloc'd 64 KiB host stack when the
  caller's is under `SIM_HOST_MIN_STACK` and never frees it (`sim.cc:864-871`). A rebuild on a
  small-stack thread therefore leaks 64 KiB per slay on the host. The default spawn path hands over
  a pool stack well above the floor, so this is a host-only leak on a non-default shape -- but it is
  a leak, and a `taskdeath`-style unit suite that slays in a loop would find it. Either the wrapper
  reuses the existing `SimContext` stack or the leak is documented at the seam.
- **`lx6`**: `arch_context_init` builds the COOP resume format, and the switcher branches on
  `ctx->resume_kind` (`arch/xtensa/chip/esp32/startup.S:294-296`). A rebuild must set
  `resume_kind` to COOP so the switcher takes the `retw` path and not the interrupt-frame path. This
  is one field, but a rebuild that forgets it resumes into a windowed-register unwind that does not
  match the frame.

**What the seam does not do, deliberately.** It reads no status register, clears no latch, and
records no fault. That is the half of `arch_fault_redirect_to_exit` which premise 1.1 shows is not
relocatable, and slay has no use for it: a slain thread has no fault facts.

### 3.4 The stub

```c
extern "C" void kickos_thread_slay_exit(void* arg)
{
    (void)arg;
    ::kickos::sched::exit_current(KOS_EXIT_CANCELLED);
}
```

It prints nothing. `kickos_thread_fault_exit` prints because a fault is an event a supervisor cannot
otherwise learn about; a slay was requested by a thread that already knows. Adding a banner would
also put the slay path through `kprintf_fault`'s published-console delivery
(M4.7.9 section 9.5), whose ordering guarantee rests on a console driver provisioned at or above
every stdout client -- a coupling worth nothing here.

### 3.5 No second truth: `cancelled` becomes `cancel_kind`

Slay needs to distinguish "may return to userspace for its cleanup window" (kill) from "may not"
(slay). That distinction is real and irreducible: it is the entire difference between the two calls.

It must not become a second flag beside `cancelled`. **Replace `bool cancelled`
(`thread.h:142`) with `uint8_t cancel_kind`**:

```c
enum : uint8_t
{
    CANCEL_NONE = 0,
    CANCEL_KILL = 1, // cooperative: dies at its next syscall entry, keeps its window
    CANCEL_SLAY = 2  // its resume is claimed: it executes no further unprivileged instruction
};
```

One byte where one byte was, at the same offset, so `sizeof(Thread)` is unchanged by construction
rather than by measurement. One authority answers "has this thread been asked to die", and it
carries how. The two existing readers become value tests: the death point at `syscall.cc:215` and
the refusal to re-block an already-cancelled thread at `kernel/irq/irq.cc:209`.

**Idempotence and restart use `dying`, not a fourth state.** The rebuild is idempotent in its values
(an absolute entry and an absolute stack top), so applying it twice before the stub runs is
harmless. What must not happen is a rebuild after the stub has made progress: `cap_teardown` releases
`IrqLock` between chunks (`cap.cc:838`), so a half-swept thread is preemptible, and re-entering the
stub from the top would restart a sweep over a table that is already partly empty. The guard is the
flag that exists for exactly this window: `exit_current` sets `dying` at its top and never clears it,
and `thread.h:115-121` documents that `state` cannot serve because a switch back rewrites it to
`RUNNING`. So the hook is:

```c
if (next->cancel_kind == CANCEL_SLAY and not next->dying)
{
    arch_ctx_redirect(...);
}
```

The residual window is between the resume and `exit_current`'s first `IrqLock` -- a handful of
instructions, with interrupts on. A preemption there causes one idempotent restart of work that has
not happened. Naming it is the point; it needs no machinery.

### 3.6 The gate: parenthood, unchanged

**Slay keeps `thread_kill`'s gate exactly: spawn parenthood** (`caller_spawned`,
`syscall_thread.cc:568`), and `kos_task_slay` keeps `kos_task_kill`'s CREATORSHIP
(`task_created_by`, `syscall_thread.cc:617`). Against the seam and grant tiebreaker:

- **Grant the narrowest unit.** Slay reaches exactly the set kill already reaches. It adds no edge to
  the authority graph; it changes what happens along an edge the caller already has. An authority bit
  or a capability would be a second gate on one edge -- the second-truth failure -- and `authority`
  is read without `IrqLock` (`thread.h:123-127`), so widening its meaning buys a concurrency
  obligation for nothing. The tree's own note on the kill gate says why parenthood is the right
  currency: it "grants the caller nothing it did not already have and cannot be delegated"
  (`STATE.md`, the console-reclaim blocker).
- **Refuse rather than silently mask.** Slay refuses a privileged target and refuses idle, mirroring
  the core rule `kickos_fault_kill_thread` states for itself (`kernel/init/fault.cc:94`). Rebuilding
  a privileged thread's context is not a privilege escalation -- it is already privileged -- but a
  privileged thread may be inside kernel work holding kernel invariants, and discarding its frames
  discards them mid-flight. `-KOS_EINVAL`, loudly.
- **Pay only what is needed.** No new gate, no new state, no new thread, no new queue.

One thing the gate does NOT protect against and should not pretend to: a parent slaying a driver
thread denies it the window in which it would have quieted its device. That is the meaning of the
call, and the caller chose it over `kill`. The header should say so, because the failure is a live
peripheral with no owner, not a kernel fault.

### 3.7 Waiting for gone

Step 3 reuses `WAIT_JOIN` wholesale. `thread_join` already parks queue-lessly on a target thread
(`wait_obj` = the target, `WaitKind` `WAIT_JOIN`), and `exit_current` already sweeps the thread pool
waking every `WAIT_JOIN` waiter whose `wait_join_target()` is the dying thread
(`sched.cc:253-272`). Slay's wait is a join with a slay in front of it, so it should be exactly that:
no new `WaitKind`, no new list, no `.bss`.

`kos_task_slay`'s wait is the one that does not already exist, because "the group is empty" has no
waiter today. It needs a `WAIT_TASK_EMPTY` value in the existing `WaitKind` enum (an existing byte,
`thread.h:56-68`) with `wait_obj` holding the `Task*`, plus one arm in the pool scan `exit_current`
already runs. Zero `.bss`, one arm, and it belongs in the last sub-milestone rather than the first.

**The starvation hazard, named.** The caller blocks; the victim must be scheduled to run its own
teardown. If a third thread at higher priority spins, the victim never runs and the caller times
out. That is a general property of a priority scheduler and not specific to slay, and the timeout
argument is what keeps it visible instead of turning it into an unbounded park. A "death boost" that
raises the victim's priority for the duration would remove the hazard and is REFUSED here: it puts a
policy decision in the core, and the core owns mechanism while the policy owns which thread runs
(`sched.h:17-19`). Open question 2 puts it to the maintainer anyway, because it is a defensible
alternative and not obviously wrong.

## 4. What slay guarantees per board, and where it does not hold

The guarantee is "the target executes no further unprivileged instruction". It rests on the target
being off-CPU when the request is made (section 3.1), which rests on the target being unable to hold
the CPU indefinitely.

**On a board with a privilege ring, that holds absolutely.** An unprivileged thread cannot mask
interrupts, so it cannot prevent the preemption that must already have happened for the caller to be
running.

**On `esp32-wroom` (lx6) and `microbit` (armv6m, Cortex-M0), `KICKOS_HAVE_PRIV_RING` is 0** and the
statement weakens in a way that must be written down rather than glossed. On lx6 `PS.UM` is 1 for
kernel and thread alike, which is a hardware fact (`CMakeLists.txt:452-454`), and the Cortex-M0 has
no privilege extension. A thread there can mask interrupts and spin, and then it is RUNNING and
never leaves -- but so is every other mechanism defeated, including the syscall-entry death point,
and no thread other than that one ever runs again. **The honest statement: on a board with no
privilege ring, slay's guarantee is conditional on the target not having masked interrupts, and a
target that has is unreachable by any mechanism because the system has already stopped
scheduling.** That is a pre-existing property of those two boards, not a defect slay introduces, and
it must not be recorded as the same kind of gap as an unported backend.

Note the asymmetry with fault isolation, which is genuinely refused on lx6: slay does not need to
DISCRIMINATE privilege at fault time, it needs to control a resume, and the scheduler controls that
on every backend. So slay is implementable on lx6 where `arch_fault_redirect_to_exit` is not. That
is a real widening and it is worth having.

**On `rx72m` (rxv3) every claim is unfalsifiable off that one board.** There is no RXv3 emulator and
no CI gate (`STATE.md`, the rxv3 section). Section 9 treats that as a gating fact rather than a
footnote.

## 5. The reaper, and why it is rejected

Design 4.3 (`docs/design-m4.7.9-fault-isolation.md`) posits it precisely: mark the TCB, hand it to a
reaper thread, switch directly to the next runnable thread without ever resuming the faulter. It
prices itself honestly -- generalising `exit_current` from "the current thread" to "this thread",
"plus a thread, a stack, a queue, and a priority decision for it" -- and records itself as the
fallback if 4.1 proves unsound.

4.1 has not proved unsound. It is silicon-witnessed on five boards and four enforcement classes.
Four reasons to reject the reaper here, in weight order.

**5.1 It is a new authority, and one concrete defect shows the authority is not free.**
`cap_teardown` is stranger-safe in its own body -- no `sched::current()` anywhere in `cap.cc`,
everything threaded through `Thread* c` into `obj_close_protocol(closer, ...)`, and every
self-priority and self-reschedule action gated on `not teardown` (`cap.cc:266-275`, `cap.cc:344-347`).
That is genuinely good news for a reaper and it is not sufficient. The endpoint and reply arms call
`sched::wake` (`cap.cc:288`, `cap.cc:350`), and `sched::wake`'s two switch-deferral clauses read
`kernel().current`: `state == EXITED` at `sched.cc:156` and `dying and t->prio <= c->prio` at
`sched.cc:162`. Under a reaper those clauses consult the REAPER's flags, not the victim's. The
deferral logic M4.8.2 shipped as its headline repair -- and which class A is repairing again right
now -- would be reasoning about the wrong thread. That is not a bug to fix in passing; it is the
signature of an authority that was designed for one shape being handed a second.

**5.2 The footprint answers to a board with zero slack.** A reaper wants a thread and a stack and a
queue. On `microbit`, `__kickos_ram_start` IS `_ebss` and the arena granule is 32 bytes, so ANY
non-zero `.bss` addition moves the arena base a full granule and costs a `kos_ram_alloc` grain.
That is measured, not argued: the `Task` pool of 9.3 cost 32 bytes there and turned
`mem_self_grant` into a declared skip (`docs/design-task-layer.md` section 8.2, fourth finding), and
`Task` was REPACKED rather than grown for 9.4 to avoid flipping a second arm. A reaper TCB is 256
bytes plus a stack on that board. The scheduler redirect costs zero bytes (section 7).

**5.3 Its one genuine advantage evaporates.** The reaper's real merit -- and it is real -- is that it
removes the dying thread's stack from the picture, which is the property 4.2 says "would survive"
the overflow case and which the rxv3 filing points at. But that property is obtainable for the price
of a stack-pointer reset (section 6), at zero footprint, on the path that already exists. Once the
reset is available the reaper is paying a thread and a stack and a new authority for something a
register write already bought.

**5.4 It does not reach the case slay exists for.** A reaper reaps a thread that has already been
routed to it. Routing a spinning, syscall-free thread still requires claiming its resume, which is
the scheduler redirect. The reaper is downstream of the hard part, not a substitute for it.

**Where the reaper stays on the table.** Section 4.3's own reason -- an SMP core where a peer is
genuinely RUNNING on another CPU and cannot be redirected at a resume that is not coming. Open
question 1.

## 6. The rxv3 below-stack heuristic

### 6.1 What it is, and the trap in narrowing it

`kickos_fault_below_stack` (`kernel/init/fault.cc:74-82`) is read by exactly one caller: rxv3's
`arch_fault_is_user_thread`, for cause `0x54` with `MPESTS.DMPER` set (`arch_rxv3.cc:365-372`). When
the denied address is below the running thread's `stack_base`, the predicate declines and the fault
escalates to the panic dump instead of a thread-scoped death.

It exists because RXv3 CANCELS the faulting instruction and restores SP (ISA UM sec.5.3.1), so the
denied push reads as though it never moved the USP and no SP-based test can see an overflow. The
faulting ADDRESS is the only evidence left. Measured before the test existed: the kill was accepted,
the stub ran privileged on the exhausted stack, supervisor bypasses the RX MPU so nothing trapped
the damage, and it smashed its way to `PC=0x0` (`.session/logs/m483rxovf-*`).

The measured cost is a false positive: `mpu_fault`'s cross-domain write to `0x13200`, below
`domainA`'s stack, used to die thread-scoped and now escalates, while `rxdrv`'s `0x8c068` above the
stack still dies alone.

The filing forbids the obvious narrowing and is right to: "Do NOT replace the stack base with a
distance threshold -- a frame larger than the threshold puts privileged code back on an exhausted
stack, which is the UNSAFE direction."

### 6.2 What a stack reset changes, and it is the unsafe direction that disappears

**The answer to "what does a scheduler-driven redirect let the rxv3 fault path do that it cannot do
today" is: nothing directly.** The slay redirect acts on a saved context; a fault acts on a live
one. They do not share code. Manufacturing a dependency because the two items were filed together
would be the wrong answer, so this section gives the real one.

What DOES change the rxv3 path is one property the slay seam needs anyway, and which is therefore
cheap to argue for once slay is on the table: **the stub must run at the TOP of the dying thread's
own stack, not at whatever depth the thread had reached.**

For slay this is free -- `arch_context_init` fabricates its frame at the stack top by construction,
so `arch_ctx_redirect` has the property without asking. For the FAULT path it is per-backend work in
each backend's own idiom, because a live frame cannot be rebuilt:

- `rxv3`: `mvtc` the USP to `stack_base + stack_size` before the `rte`. This is the backend that
  needs it and the cheapest place to get it.
- `armv6m`/`armv7m`: write a synthetic 8-word frame at the stack top and `msr psp`. Two hazards:
  the frame must be 8-byte aligned, and on a part with lazy FP stacking the `EXC_RETURN` in the
  handler's LR decides whether the CPU expects an extended frame -- a relocated frame must match
  what `EXC_RETURN` says, or the pop reads 26 words out of 8 words of memory.
- `rv32imac`: the stub already runs with `sp` on the trap frame (`switch.S:361-362`); the shim would
  set `sp` before the `mret`.
- `sim`: `uc_mcontext.gregs[REG_RSP]`, respecting the host ABI's alignment and return-slot
  convention.

**With the reset in place, the argument for the below-stack test collapses to one thing, and it is
not safety.** The damage the test prevents was "the stub ran on the exhausted stack". A stub at the
stack top cannot. What the test still buys is ATTRIBUTION: labelling an overflow as a panic-worthy
provisioning bug rather than as one thread misbehaving. That is a diagnostics preference, and it is
being paid for with a measured false positive on a real app.

It should also be said plainly that the reset makes the fault path strictly safer than today even
where the test is kept: on a non-overflowed fault the current redirect lands the stub at whatever
depth the thread had reached, and `exit_current` plus `cap_teardown` plus `kprintf_fault` need
headroom there. A reset gives them the whole stack.

### 6.3 Where the heuristic ends up: narrowed to a band, not deleted

Deleting it outright is tempting and I do not recommend it, for a reason that only shows up when the
fleet is read together. On `armv7m` the equivalent escalation comes from the CFSR `0x1818`
stacking-abort mask (`arch_armv7m.cc:235`), which is a FRAME-VALIDITY test, not a stack-safety test:
when hardware stacking aborted there is no frame to rewrite, and 4.2 records that the observed
`frame` pointed at RAM the frame was never written to. A stack reset does not fix that -- there is
nothing to relocate -- so armv7m keeps escalating an overflow no matter what rxv3 does. Deleting
rxv3's test would therefore make an overflow die thread-scoped on one arch and panic on three,
diverge from the four witnessed `faultsurvive_ovf` captures, and break
`check_faultsurvive.sh`'s corroboration table -- which is already a class C defect that MISJUDGES
one class.

So: **narrow it to a guard band immediately below the thread's own stack base**, and accept that a
wild write inside that band is still misattributed as an overflow. The unsafe direction the filing
warned about is exactly what the reset removes, so a band becomes admissible where a bare threshold
was not. The band's width must be MEASURED against the two captures that bracket it --
`faultsurvive_ovf` at `MPDEA=0x121fc` (an overflow, must escalate) and `mpu_fault` at `0x13200`
(a cross-domain write, must die alone) -- and one MPU region granule is the natural first candidate
because it is the unit the hardware itself denies in.

**I could not settle the width, and it is not settleable off the bench.** Those two addresses are
about 4 KiB apart but belong to different threads' stacks, so whether one granule separates them
depends on where `domainA`'s stack base sits in that image, which no capture in the tree records and
no host gate can produce. It is open question 3, and rx72m silicon is the only instrument.

**This whole section is independent of slay and should land first.** It touches the fault path only,
it has an existing witness set to regress against, and its own gate (`faultsurvive_ovf` must still
escalate, `mpu_fault` must return to dying alone) is a two-row table on one board.

## 7. Footprint

The question can veto a design here, so it is answered before the scope estimate.

| item | `.bss` | `sizeof(Thread)` | note |
| --- | --- | --- | --- |
| `bool cancelled` -> `uint8_t cancel_kind` | 0 | unchanged | same width at the same offset; not a measurement, a construction |
| the `switch_to` hook | 0 | unchanged | two tests on an existing field |
| `arch_ctx_redirect` | 0 | unchanged | `.text` only; the shared body is one call |
| `kickos_thread_slay_exit` | 0 | unchanged | `.text` only, and it prints nothing |
| slay's wait | 0 | unchanged | reuses `WAIT_JOIN` and the pool scan `exit_current` already runs |
| `WAIT_TASK_EMPTY` (task_slay only) | 0 | unchanged | a value in an existing enum in an existing byte; one arm in an existing scan |
| **total** | **0** | **unchanged** | |

`sizeof(Thread)` is asserted computationally rather than as a literal: `thread_scalar_bytes()`
(`thread.h:300-315`) is a hand-maintained constant, `KICKOS_THREAD_EXPECTED_SIZE` sums it with the
region array and `CapRun`, `thread.h:321` asserts equality, and `thread.h:327` additionally asserts
NO TAIL PADDING -- so on this struct a new member is never free and the width-preserving swap is the
only zero-cost shape available. The documented figures are 256 `microbit`, 264 `picopi`, 2480 `sim`
(`docs/reference/invariants.md:126`), and the literal `2480` appears nowhere in the tree.

**The gate for the whole milestone is therefore stated up front: `microbit`'s `.bss` is byte-identical
and its declared skip set does not grow.** That board is where 9.3 lost `mem_self_grant`'s last
allocation grain, and it is the only board whose skip set the tree states. A design that costs it
one more arm is a design that failed here, not a design with a cost.

The one footprint risk that is real and is on the host: the sim's malloc-per-rebuild noted in
section 3.3. It costs no target byte and it is a leak.

## 8. How this composes with what 9.5 already built

Nothing in 9.5 changes. Every piece of it is load-bearing for slay and is reused rather than
extended.

- **The cancel is TOTAL over `WaitKind`.** `thread_abort_park` (`park.cc:88-142`) is what slay's step
  2 calls, through `thread_cancel`. Slay does not need its own unpark and must not have one: the
  arms are not wakes but unwinds -- a mutex waiter does not acquire the mutex and the owner's
  inherited priority is reverted, a semaphore's count is untouched, an endpoint park goes through the
  same `endpoint_wait_abort` a deadline expiry uses, a reply park bumps `call_seq`. All of that is
  bookkeeping the OTHER side of each park needs, and it must happen before the victim's context is
  discarded. A slain thread's `wait_result` is then written and never read, which is correct and
  worth a comment at the seam so a reader does not "fix" it.
- **A member's death ends its group.** `task_cancel_group` (`park.cc:168-188`) marks every peer with
  the same `cancel_kind` the dying member carries. Under slay that must propagate the KIND: a member
  slain should slay its peers, or the group dies by two different rules. This is the one place 9.5's
  code needs a parameter, and it is one argument on a function that deliberately refused an `except`
  argument for good reasons (mutation M2). Adding a `kind` argument is not the same shape as `except`
  -- `except` was a redundant authority beside two existing guards, and `kind` is information the
  callee cannot derive.
- **The gate is CREATORSHIP, not possession.** Unchanged for `kos_task_slay` (section 3.6).
- **A task handle is a plain word**, generation over a BIASED index, all-zero meaning "no task".
  Untouched. Slay adds no handle.
- **The cap codec is untouched.** Slay adds no capability and no table entry. This matters for the
  gate argument: there is no cap to grant, so parenthood remains the only currency, exactly as
  `syscall_thread.cc:547-549` says for kill.
- **The ORDER inside `exit_current` is unchanged**, and slay must not perturb it: `dying`, then
  `task_cancel_group`, then `task_release`, then `cap_teardown` outside any lock, then `EXITED`, then
  the console-death note, then `on_remove`, then the join sweep, then `reschedule`. The
  `task_release`-before-sweep order exists for the DEV-window respawn race and is untested
  (M22/M25, filed). Slay reaches `exit_current` at its top, so it inherits all of it. A slay that
  short-circuited any of it would be a second death path, which is the whole thing this design
  avoids.

One place where slay makes an existing filing STRICTER rather than looser, worth noting because it
is free: a slain driver thread never returns to userspace, so the `dev_window_free` skip on a
`dying` holder (`thread.cc:52-53`) is reached sooner relative to the thread's last user
instruction. That does not fix M22 -- the missing choreography is a gate, not a mechanism -- but it
does not worsen it either.

## 9. What can be gated in-env, and what only silicon can say

Being concrete here matters more than usual, because three of the four instruments in class C are
themselves defective.

### 9.1 What the host unit layer CAN see

The K-seam substitutes `arch_*` with a whole-`Kernel` reset (M4.8.2), and `arch_switch` is an
`arch_*` symbol. So a K-seam suite CAN see:

- the state partition of section 3.1: slay a `READY` thread, slay a `BLOCKED` thread, assert the
  refusals for `EXITED`, `INACTIVE`, self, idle, privileged;
- **that the hook fires on the incoming thread and only there**, by substituting `arch_switch` with
  a stub that records `(from, to)` and asserting the rebuild happened against `to`;
- the `dying` guard: drive a real sweep (which `tests/unit/schedwake/wake_dying.cc` already does),
  preempt mid-chunk, and assert the hook declines;
- `cancel_kind` propagation through `task_cancel_group`;
- ordering as an ordered TRACE rather than a counter, which is what
  `tests/unit/taskdeath/group_kill.cc` already does and for the stated reason: a counter oracle
  cannot fail on a reordering.

A returning `arch_switch` stub is a clean unit seam precisely because the test becomes the CPU.

### 9.2 What the host unit layer STRUCTURALLY CANNOT see

- **The death point itself.** The fixture never resumes a real context, so it cannot witness that the
  rebuilt context actually lands on the stub. It can witness that the seam was CALLED with the right
  arguments and nothing more. That gap is the same one `TODO.md` already records for the cooperative
  death point ("Nothing witnesses the DEATH POINT on silicon").
- **The SIM posture only.** The K-seam fixture compiles one posture, so `KCAP_RUN_CHUNKS > 1` -- which
  `frdmk64f` runs -- never reaches a K-seam arm. Any arm whose subject is a segmented cap table is
  unreachable there, and `cap_teardown`'s bound is `thread_cap_capacity(c)`, which is exactly the
  segmented quantity. A slay-during-sweep arm on the host is therefore testing the flat path only,
  and must say so.
- **Deferral.** The sim is immediate in thread context and collapses ISR-context switches. A rebuild
  that is correct on the sim says nothing about the pended backends, which is where the placement
  rule of section 3.2 earns its keep. This is the M21/M23 lesson from the 9.4/9.5 run restated: a
  survival on the sim can mean unreachable rather than vacuous.

### 9.3 What emulation adds

`qemu` (armv7m) and `qemu-riscv` (rv32imac) exercise real pended switches and real MPU/PMP
enforcement, so they can witness the rebuild landing on the stub for real. Those are the two arches
where a slay arm should be a registered ctest and not a fleet-pass item. M21 and M23 both needed
`qemu` and `qemu-riscv` respectively before their KILLED verdicts stood; the same will be true here.

### 9.4 What is silicon-only, and one board that is worse than that

- **`rxv3`, entirely.** No emulator, no CI gate. Every rxv3 claim in section 6 is unfalsifiable off
  `rx72m`, and section 6's whole subject is rxv3. The band width of open question 3 is a bench
  measurement or it is nothing.
- **`lx6`.** The immediate-switch backend with two resume formats and no privilege ring. `esp32-wroom`
  is the only instrument, and note that `faultsurvive` is not even a target there, so the existing
  fault-path witness set has no lx6 row to compare against.
- **`armv6m`.** `picopi` is the only enforcement unit for that class, and `microbit` is the footprint
  authority. `microbit`'s `.bss` claim is a build-time fact and is gateable in-env; `picopi`'s
  enforcement run is not.
- **A slain driver whose IRQ line is armed.** The interesting case is the wake racing the device, and
  it wants an enforcing board with an IRQ UART service list. `TODO.md` already carries this for the
  cooperative death point; slay does not make it cheaper.

**One instrument must be repaired before any of this is witnessed**, and the tail's own locked order
already says class C first: `check_faultsurvive.sh`'s corroboration table misses two enforcement
classes and MISJUDGES one. Section 6 changes what that script's subject does on one board. A gate
that misjudges cannot witness a fix to anything else.

## 10. Mutation plan

The discipline is the gate, because a gate is an artifact that can be deleted. For each behaviour
introduced, the mutation that must kill it. A mutant that survives is a missing arm, not a passing
design.

| # | mutant | must be killed by |
| --- | --- | --- |
| S1 | the `switch_to` hook never fires (delete the test) | a slain READY thread runs its next instruction; a `READY`-victim arm on qemu and qemu-riscv |
| S2 | the hook fires on `prev` instead of `next` | the rebuild is lost on every pended backend; a substituted-`arch_switch` K-seam arm asserting the argument, and a real qemu arm |
| S3 | the hook ignores `dying` | a mid-sweep preempted victim restarts `cap_teardown`; a slay-during-sweep arm driving a real sweep, asserting the totality asserts still hold |
| S4 | `cancel_kind` collapsed back to a bool (slay treated as kill) | a slain thread returns to userspace once; an arm that asserts a slain thread executes NO further user instruction, which needs a user-visible side effect the victim would have produced |
| S5 | kill treated as slay (the converse) | a cancelled `irq_wait` returner loses its cleanup window; the existing cooperative-window arm must FAIL |
| S6 | `arch_ctx_redirect` builds an unprivileged context | the stub faults on its first kernel access; qemu and qemu-riscv under enforcement, and it must not merely panic-with-a-message but be attributed |
| S7 | `arch_ctx_redirect` keeps the thread's current SP instead of the stack top | a deep-recursion victim's stub overflows; needs a deliberately deep victim, and on rxv3 this is the `m483rxovf` shape |
| S8 | the lx6 rebuild omits `resume_kind` | the switcher takes the interrupt-frame path on a COOP frame; `esp32-wroom` silicon only |
| S9 | the sim wrapper omits the arena raise | the stub SIGSEGVs on its first kernel read; a host arm, and it should be the FIRST arm written since it fails loudest |
| S10 | the self-slay refusal deleted | benign today (section 3.2), so this mutant may SURVIVE -- and if it does, that is the finding, not a pass. Re-run after class A lands |
| S11 | the privileged-target refusal deleted | a privileged victim's frames are discarded mid-kernel-work; needs a privileged victim, which only the kernel creates, so this may only be reachable as a K-seam arm |
| S12 | `task_cancel_group` drops the kind and marks peers as KILL | a slain group's peers keep their windows; a group arm asserting every member's death is windowless |
| S13 | slay returns 0 without waiting | a caller observes a live target after 0; an arm that reads the target's state at the return |
| S14 | the timeout is ignored (slay parks forever) | a starved-victim arm with a higher-priority spinner must return `-KOS_ETIMEDOUT` and not hang |
| R1 | the rxv3 band widened to the old stack-base test | `mpu_fault` escalates again; rx72m silicon, two rows |
| R2 | the rxv3 band deleted entirely | `faultsurvive_ovf` dies thread-scoped instead of escalating; rx72m silicon |
| R3 | the fault-path SP reset removed | `faultsurvive_ovf` reaches `PC=0x0` again, which is the original measured failure; rx72m silicon |

Three method notes carried forward from the 9.4/9.5 run, because each would silently invalidate this
one:

- **Restore a baseline file with an explicit mtime bump.** `shutil.copy2` preserves mtime, ninja then
  skips the rebuild, and every mutant after the sixth runs against the previous mutant's binary and
  reports an identical, meaningless kill set.
- **A survival on the sim can mean unreachable rather than vacuous.** S2, S6 and S7 are all in that
  class by construction: the sim is immediate, has no privilege ring in the context, and detects no
  guest stack overflow at all (`sim.cc:1075-1078`).
- **Re-check the baseline between mutants**, not only at the start.

## 11. Open questions needing a ruling

Only the ones I cannot settle from the tree.

**1. Does slay's termination argument have to survive SMP now, or is a redirect-at-resume acceptable
as a single-core design with a recorded SMP successor?** Section 3.1's proof uses "exactly one thread
is RUNNING". On SMP a peer is genuinely RUNNING and there is no resume to claim, which is the one
place section 4.3's reaper is not replaceable -- or an IPI plus a per-CPU fault-shaped redirect on a
live frame is. **Recommendation: rule it single-core, and record the SMP shape as belonging to M5
rather than designing for it now.** The redirect is not wasted work under SMP: it remains the correct
mechanism for a victim that is not currently on a CPU, which is most of them. Designing for a core
count the tree does not have would buy a reaper's cost today for M5's benefit.

**2. The starvation hazard: timeout, or a death boost?** A blocked caller depends on the victim being
scheduled. Section 3.7 chooses an explicit `timeout_us` and refuses to raise the victim's priority,
on the grounds that the core owns mechanism and the policy owns which thread runs.
**Recommendation: the timeout.** The counter-argument is real -- a boost bounds the wait where a
timeout only reports it, and PI already boosts for a structurally similar reason -- so this is a
ruling and not a settled point. If a boost is wanted it should be a policy hook and not a `set_prio`
call in the syscall.

**3. The rxv3 band width.** Section 6.3 recommends narrowing rather than deleting, and one MPU region
granule as the first candidate. **I could not settle the width and it is not settleable off the
bench**: it turns on where `domainA`'s stack base sits relative to `0x13200` in that image, which no
capture in the tree records. **Recommendation: land the SP reset and the narrowing as one
sub-milestone, on the bench, with the two-row regression table as its gate** -- and if the two
addresses turn out to be inseparable by any band, KEEP the stack-base test and record the false
positive as still-accepted rather than shipping a band that does not separate them.

**4. Does a stack overflow escalate, or die thread-scoped?** This is the question under question 3 and
it deserves its own answer, because the band is only a proxy for it. Today: overflow escalates on
every backend that can see it. **Recommendation: keep escalating.** An overflow is a provisioning
bug in the image, not a thread misbehaving, and the four witnessed `faultsurvive_ovf` captures
encode that reading. If the ruling goes the other way, section 6.3's band disappears and rxv3's
test is deleted outright -- which is simpler, and would also mean `armv7m`'s `0x1818` mask becomes a
fleet divergence that needs its own answer, since a stacking abort leaves no frame to redirect.

**5. `KOS_EXIT_CANCELLED` for a slain thread, or a new code?** Section 2.3 reuses it.
**Recommendation: reuse.** Nothing reads the distinction, `KOS_EXIT_FAULT` is already not
join-visible, and a second code for one event is a second truth. Raised only because it is an ABI
byte and appending is cheap enough that someone will suggest it.

**6. Does `kos_task_slay` block until the group is EMPTY, or is the thread form enough for this
milestone?** The task form needs `WAIT_TASK_EMPTY` (zero `.bss`, one arm in an existing scan) to make
its 0 mean gone. **Recommendation: ship the thread form first and the task form in the last
sub-milestone**, because every in-tree group kill goes through `drv::bring_up`'s one call site, which
today calls `kos_task_kill` and has no caller that needs "gone" synchronously. A `kos_task_slay` that
returned only `-KOS_ETIMEDOUT` would be a call whose success case is unreachable, which is worse than
not having it.

## 12. Scope, honestly

This is larger than the M4.8.4 tail it came out of. It is a new ABI pair, a new arch seam on six
backends, a hook in the file another agent is repairing, and a change to the one path whose only
instrument is a board with no emulator and no CI. Four sub-milestones, in dependency order. They
cannot be reordered.

**S1. The fault-path stack reset, and the rxv3 narrowing.** Independent of slay entirely (section
1.3). Touches `arch_fault_redirect_to_exit` on four backends plus `kickos_fault_below_stack`'s one
caller. Its gate is a two-row regression on `rx72m` plus the four existing `faultsurvive_ovf`
captures unchanged, and it needs `check_faultsurvive.sh` repaired first because that script
misjudges one class. **This is the item that closes class B item 1, and it closes it without slay.**
Landing it first also means the slay seam inherits a stack-top posture that has already been
witnessed on silicon.

**S2. `cancelled` -> `cancel_kind`, provably inert.** One byte becomes one byte at the same offset;
two readers become value tests; `CANCEL_SLAY` is defined and never set. No behaviour change, no ABI
change, no call-site change outside the kernel. The gate is exactly that: `sizeof(Thread)` unchanged
on `microbit`/`picopi`/`sim`, `microbit`'s `.bss` byte-identical, its declared skip set not grown,
every suite unchanged. This is a refactor that can be proven inert, which 9.3 established as the
right first step for core-path work -- and 9.3's own lesson applies: prove it on the board with no
slack, not on the host.

**S3. `arch_ctx_redirect`, the `switch_to` hook, and the thread-slay syscall.** The milestone. The
seam on six backends (three of which need one extra line, section 3.3), the two-test hook, the stub,
the syscall, the parenthood gate, the `WAIT_JOIN` reuse. **Gated on class A landing**, for the reason
in section 3.2: slay must not ship a second answer to "who is current", and the self-slay refusal is
inexact until class A closes. Witness set: qemu and qemu-riscv as registered arms, then `picopi`
(armv6m enforcement), `frdmk64f` (segmented cap table, which no host arm can reach), `rx72m` (the
only rxv3 instrument), `esp32-wroom` (immediate switch, two resume formats, no privilege ring).

**S4. The task-slay syscall, the `kind` argument on `task_cancel_group`, and a task-empty wait kind.** The
group form, subject to open question 6. Small on its own and it depends on S3 for every part of its
mechanism.

S1 and S2 are each a day's work with a real gate. S3 is the milestone and its cost is dominated by
the fleet pass, not by the code: six backends of one-line-plus-a-wrapper, against a bench that is
SERIAL -- one board, one reader, one capture -- and a tree that three other tracks are also writing.
S4 is small.

**What would make me stop and re-plan**: if `arch_context_init` turns out not to be reusable on one
of the six backends for a reason section 3.3 missed. That would turn a wrapper into a fifth and sixth
hand-written body, and the honest response is to re-price S3 rather than to write them.

## 13. What I could not settle

Named rather than smoothed over.

1. **The rxv3 band width** (open question 3). Not settleable off `rx72m`. It needs one image's
   `domainA` stack base and the two existing MPDEA values, and no capture records the first.
2. **Whether the `cancel_kind` swap is truly free on every preset.** It is free by construction --
   same width, same offset -- but `thread_scalar_bytes()` is a hand-maintained constant and I did not
   build. The claim is a construction argument, not a measurement, and S2's gate exists to convert
   it.
3. **Whether `arch_context_init` is safe to re-run on a live thread's context on `lx6` and `sim`.** I
   found two concrete hazards by reading (the sim's malloc-and-never-free, lx6's `resume_kind`) and
   have no basis for asserting there is not a third. The sim's signal-mask commentary
   (`sim.cc:876-889`) describes a delivery window between `swapcontext` installing the target mask and
   loading its `rsp` that I do not fully understand the interaction of with a rebuilt context. Read
   that comment before writing the sim wrapper.
4. **RESOLVED BY THE LANDED CODE, and not the way this question expected.** The hazard was posed
   for a RELOCATED exception frame; `arch_ctx_redirect` never relocates one. `arch_context_init`
   fabricates the frame with `EXC_RETURN = 0xFFFFFFFD` (thread mode, PSP, NON-FP), so a rebuild
   RESETS the frame format rather than moving a frame whose format it would have to match: a
   thread that had an extended FP frame stacked resumes on a plain 8-word one, which is exactly
   what the new `EXC_RETURN` says. The 26-word-pop-off-8-words failure needs a frame format the
   rebuild cannot produce. The fault path is the one that must not relocate, and it does not.
5. **Whether `check_faultsurvive.sh`'s corroboration table can express the narrowed rxv3 arm at all.**
   It is filed as MISJUDGING one class, and I did not read it. S1 is gated on that repair and I
   cannot say how large the repair is.

## 14. As landed

S1, S2, S3 and S4. What follows is the delta against sections 1
to 13, not a restatement of them.

### 14.1 Three claims above that are WRONG

**14.1.1 Section 3.3's `lx6` bullet is INVERTED, and following it would have been a bug.**
It says a rebuild "must set `resume_kind` to COOP so the switcher takes the `retw` path".
The opposite is true. `arch_context_init` sets `KICKOS_RESUME_IRQ`
(`arch/xtensa/lx6/arch_xtensa.cc`), and the comment above it explains why at length: a
fabricated frame is an INTERRUPT frame, and a `retw` start leaves the trampoline a phantom
windowed frame with no valid base-save-area linkage, which corrupts the return PC and
branches into data RAM. So the shared body is not merely sufficient on `lx6`, its
`resume_kind` write is LOAD-BEARING -- a thread that blocked cooperatively is saved as
`KICKOS_RESUME_COOP`, and a rebuild that left the field alone would send the switcher down
the `retw` path onto an interrupt frame. `lx6` needs zero extra lines.

**14.1.2 Section 3.2's self-slay hole is not constructible, and mutation says so.** The
design expects mutant S10 to SURVIVE until class A lands. It does not survive: deleting the
self refusal reds the `thread_slay_gate` selftest arm. The window the section describes --
`kernel().current` naming `next` while `prev` still executes -- is entirely INSIDE the
kernel, under the `IrqLock` that `switch_to` was called beneath. `prev` cannot issue a
syscall there, because a syscall comes from userspace and the pended switch fires at the
mask boundary before `prev` reaches userspace again. So at every syscall ENTRY,
`kernel().current` IS the caller, and `t == sched::current()` is exact. This matters
because class A closed by RULING the residue an accepted cost that is undetectable in C
(`TODO.md`, class B), so a design gated on class A "closing" that window would have been
gated on something that never happens.

**14.1.3 Section 3.7 under-counts `WAIT_TASK_EMPTY` by three arms.** It prices the value at
"one arm in the existing scan". A new `WaitKind` costs FOUR: the exit sweep's wake, the
`thread_abort_park` unwind in `kernel/thread/park.cc` (whose `default` is
`KICKOS_UNREACHABLE`), the `ktime_on_timer` expiry arm in `kernel/time/time.cc` (whose
`default` is a `kpanic`), and the enum value. Both defaults are fail-closed, so the two
missing arms would have been a panic rather than a silent wrong answer -- but they are work,
and the estimate did not have them. The `.bss` cost is still zero.

### 14.2 Where the implementation diverges deliberately

- **`arch_ctx_redirect` returns `void`, not `bool`.** No backend can fail: the seam is a
  wrapper over `arch_context_init`, which is total and which every thread on every board
  already goes through. A `false` return would have exactly one caller behaviour available
  to it -- resume the thread normally -- and that silently downgrades a slay to a
  cooperative kill with nothing in the ABI able to report it. A backend that genuinely
  cannot express a privileged thread-mode resume at a given stack top cannot host a thread
  either, and its absence is a link error, which is louder.
- **The kernel-protecting refusals come BEFORE the parenthood gate.** `thread_slay` refuses
  a self, idle or privileged target and only then asks `caller_spawned`. Those two guard a
  KERNEL invariant (a privileged thread may hold kernel invariants mid-flight, and
  discarding its frames discards them) rather than the caller's rights, and a guard sitting
  behind an authority check is one that an authority change can bypass. The cost is that a
  stranger could learn a target is privileged; nothing in the tree makes that inferable
  today because no privileged thread is resolvable by handle at all (see 14.5).
- **`task_release` REPORTS the emptying** rather than the caller re-deriving it. "The group
  is empty" is a transition that happens exactly once and only its cause can see it: an
  implicit task's slot is freed inside that call, and a refcount already at zero cannot say
  whose departure took it there.
- **The caller parks BEFORE the victim's park is broken, and samples its resume epoch before
  that too.** Both are mutation-proved (E1, E2 in 14.4) and neither is in the design.
  `thread_cancel_kind` can switch, and on a backend that swaps inline -- `sim` and `lx6` --
  the victim can run to `EXITED` from inside that call. Parking afterwards means the exit
  sweep, which is the ONLY thing that wakes a `WAIT_JOIN` waiter, finds nobody parked;
  sampling the epoch afterwards means it already carries the resume it is meant to wait for
  and `wq_confirm_resume` spins to `KICKOS_POLL_SPIN_MAX` and panics.
- **A FOURTH return exists that section 2.1 does not list: `-KOS_ECANCELED`**, on both calls.
  It is CALLER-SIDE and says nothing about the target: the caller was itself cancelled while
  parked on its `WAIT_JOIN` / `WAIT_TASK_EMPTY` edge, and the victim stays condemned. Thread A
  slays B, thread C then kills or slays A while A waits, and A wakes with that in
  `wait_result` -- so a caller must not read a non-zero return as "the slay did not take".
  It is not a divergence anybody chose: it falls out of the caller parking at all, which is
  the entry above, and the parked-caller cancellation path predates this work. `abi.h` and
  `sys.h` document it on both calls; section 2.1's table was written before the park existed
  and is the thing that is incomplete.
- **`timeout_us == 0` is the arm-and-return form** and is not special-cased. Section 2.1
  spells it `KOS_TIMEOUT_NONE`, which cannot be right: that value means "no bound" for
  `KOS_SYS_THREAD_JOIN` and slay keeps that convention bit for bit. A zero deadline lands
  behind the min-delta floor, so the timer releases the park at the first opportunity.

### 14.3 The rulings on section 11

**1. SMP: RULED SINGLE-CORE**, per the recommendation, and section 14.6 records what the
mechanism owes an SMP future rather than leaving it as a sentence.

**2. Starvation: RULED the timeout**, per the recommendation. No death boost. The
`thread_slay_timeout` selftest arm constructs the hazard on purpose -- a higher-priority
compute thread holds the CPU across the deadline -- so the ABI's three levels are exercised
rather than merely documented.

**3. The rxv3 band width**: settled by S1 on silicon at one RXv3 MPU region page, 16 bytes.
Not this milestone's to reopen.

**4. Overflow escalates**: unchanged, and S1 landed on that reading.

**5. `KOS_EXIT_CANCELLED` for a slain thread: RULED reuse**, per the recommendation. A
slain thread was cancelled; what differs is who chose the moment, and nothing reads the
distinction.

**6. `kos_task_slay` blocks until EMPTY: RULED yes, and it shipped in S4** rather than
being deferred. The success case is reachable because `WAIT_TASK_EMPTY` makes it so, which
is the condition the recommendation put on it.

### 14.4 The mutation record

Every mutant below was applied to the tree, built, and run; the restore is a byte-for-byte
copy of the original rather than an inverse substitution, because a deletion has no
invertible search text and a failed inverse leaves the mutant in place for every later run.

| # | mutant | killed by |
| --- | --- | --- |
| S1 | the hook never fires | 4 `SlayHook` arms, `selftest`, `qemu_selftest` |
| S2 | the hook rebuilds `prev` | 3 `SlayHook` arms, `selftest`, `qemu_selftest` |
| S3 | the hook ignores `dying` | `SlayHook.a_dying_victim_is_not_rebuilt` ONLY |
| S4 | the kind collapses to a bool | `SlayHook.a_killed_thread_is_not_rebuilt`, `selftest` |
| S5 | slay marks `CANCEL_KILL` | `selftest` (`thread_slay_window`) |
| S6 | the rebuild is UNPRIVILEGED | `qemu_selftest` and `qemu_riscv_selftest`, only after the gate was repaired -- see below |
| S7 | the rebuild keeps the thread's depth | `SlayHook.the_rebuild_targets_the_slay_stub_at_the_top_of_the_victims_stack` ONLY |
| S10 | the self refusal deleted | `selftest` (`thread_slay_gate`). The design expected this to SURVIVE; see 14.1.2 |
| S11 | the idle/privileged refusal deleted | **SURVIVES**, see 14.5 |
| S13 | slay returns 0 without waiting | `selftest` |
| S14 | the timeout is ignored | `selftest` |
| E1 | the resume epoch sampled after the cancel | `selftest` |
| E2 | the caller parks after the cancel | `selftest` |
| G1 | the group cancel drops the kind | `CancelWiring.a_slain_members_exit_slays_its_peers` |
| G2 | a later kill demotes a slay | `CancelWiring.a_kill_does_not_demote_a_slay` |
| T1 | the group wake keyed on the death, not the emptying | `TaskDeath.a_death_that_leaves_peers_wakes_no_group_waiter` |
| T2 | the group wake drops the pointer compare | `TaskDeath.an_emptying_wakes_only_the_waiter_on_that_group` |
| T3 | the group wake deleted | `TaskDeath.the_last_member_out_wakes_a_group_waiter`, `selftest` |
| T4 | the creator hold dropped before the wait | **SURVIVES**, see 14.5 |
| T5 | the group is cancelled cooperatively | `selftest` (`task_slay_group`) |
| T6 | the caller-is-a-member refusal deleted | **SURVIVES**, see 14.5 |

**S6 SURVIVED A GREEN RUN, and repairing the gate is the finding.** An unprivileged rebuild
faults the stub on its first kernel access, `kickos_fault_kill_thread` catches it, and the
thread dies thread-scoped -- so the victim is still gone, the window is still denied, and
every plan, case and directive check in `tests/integration/check_tap_stream.sh` reconciled.
The only trace was a `=== THREAD FAULT ===` line the gate did not read. That script now
refuses one: the selftest never faults by design, so any such record in its stream is an arm
whose thread died the wrong way, whatever produced it.

**Three of the six group mutants were first REFUSED BY THE COMPILER**, not by a gate:
`-Werror=unused-but-set-variable` rejects T1, T2 and T3 as written, because each drops the
last reader of a local. They were re-spelled with `(void)` casts so a real red could be
shown. Worth recording as a property of this tree rather than as a curiosity: a
whole class of "delete the condition" mutants cannot compile here.

### 14.5 Open holes, named

**1. `S11` SURVIVES: the idle and privileged refusals are unreachable from userspace.** Not
a defect in them -- a defect in what can reach them. `idle` is `g_idle_tcb`, a static TCB
OUTSIDE the `ThreadPool`, so `thread_resolve` can never answer it; and root is deliberately
unprivileged (`kernel/init/kmain.cc`), so no privileged thread is resolvable by handle at
all. The guard is kept because it is fail-closed against exactly the future the driver era
and M5 open, and because `kickos_fault_kill_thread` states the identical rule and carries
the identical reachability status. **It becomes reachable, and the mutant becomes killable,
the moment a privileged thread can occupy a pool slot.**

**2. `T4` SURVIVES: dropping the creator's hold before the wait is currently harmless.** The
sweep compares the waiter's `wait_obj` against the Task POINTER, and a freed slot is still
that pointer, so the wake lands either way. It becomes a real defect when a third thread
`task_create`s into the freed slot during the victim's `cap_teardown` chunk gap: the waiter
is then told "your group is empty" about a group that is not its own. The landed code cannot
reach that state because the hold keeps the slot reserved. Unreachable in-tree today (root
is the only creator and it is the parked thread), so this is hardening with no arm.

**3. `T6` SURVIVES: a member cannot be its own group's creator**, so the `c->task == t`
refusal is unreachable. Reaching it needs a thread seated into a task it created, and
`kos_thread_params::task` only ever seats a CHILD. Kept fail-closed because the failure it
prevents is a hang rather than an error.

**4. The K-seam sees the arguments, never a resumed context.** `arch_ctx_redirect` there
RECORDS; nothing rebuilds and nothing resumes. That the rebuilt frame lands on the stub,
privileged, at the stack top is `qemu`, `qemu-riscv` and the fleet's to witness.

**5. `KCAP_RUN_CHUNKS > 1` still reaches no K-seam arm**, so the `dying` guard of S3 is
gated on the FLAT capability path only. `frdmk64f` is the segmented instrument and it is a
silicon run.

**6. No silicon.** Everything below is emulated or host. `rx72m` (rxv3, no emulator and no
CI gate anywhere), `esp32-wroom` (`lx6`, the immediate-switch backend with two resume
formats and no privilege ring, and the one backend whose seam claim in 14.1.1 is a READING
of the code rather than a run) and `picopi` (the only armv6m enforcement unit) are owed
captures. `microbit` covers armv6m as a QEMU boot, which is not enforcement.

**7. `S3` and `S7` are each killed by exactly ONE arm, and both are K-seam arms.** A
mid-sweep restart and a stub at the wrong stack depth have no userspace-visible signature
that any in-tree app produces. `S7`'s real shape on silicon is the `m483rxovf` deep-victim
case, which is an rx72m run.

### 14.6 What slay owes SMP

Ruled single-core (open question 1), so this is the debt rather than a design.

**The premise that breaks is section 3.1's**, and only that one: "a target distinct from the
caller is `READY` or `BLOCKED` or refused". On a second core a peer is genuinely `RUNNING`,
there is no resume to claim, and the redirect reaches nothing. Everything else in the
mechanism survives -- the seam, the stub, the kind, the parenthood gate, the `WAIT_JOIN`
reuse and the group-empty wait are all core-count-neutral.

What an SMP port must add, in the order it would be needed:

1. **A fifth state arm.** `RUNNING`-on-another-CPU becomes a real case in the partition, and
   it must be handled rather than refused: refusing it would make slay's guarantee depend on
   which core the victim happened to be on.
2. **An IPI plus a live-frame redirect.** That case needs the FAULT path's shape -- a
   rewrite of live machine state on the target CPU -- which premise 1.1 shows is a different
   mechanism from `arch_ctx_redirect` and is not relocatable between them. This is the one
   place section 5's reaper stays on the table.
3. **`cancel_kind` becomes a shared word.** Today it is a plain `uint8_t` written and read
   under `IrqLock`, which is a whole-machine barrier on one core and nothing at all on two.
   The escalation rule (`kind <= t->cancel_kind` then a write) is a read-modify-write and
   would need to be atomic, or the kill that must not demote a slay can do exactly that.
4. **The `switch_to` hook becomes per-CPU.** It reads `next->cancel_kind` and `next->dying`
   with no lock beyond the caller's; on SMP the victim's own CPU may be writing `dying` at
   the same instant, which is the S3 guard racing itself.
5. **`kickos_fault_stack_top` and the whole `sched::current()` family become per-CPU**, but
   that is the scheduler's SMP debt and not slay's; slay merely inherits it.

The redirect is not wasted under SMP: it remains the correct and cheapest mechanism for a
victim that is not currently on a CPU, which is most of them. What SMP adds is a second
mechanism for the minority case, not a replacement.
