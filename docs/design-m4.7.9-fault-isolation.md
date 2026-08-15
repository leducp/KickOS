<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# M4.7.9 fault isolation: a thread dies, the system does not

> **Status: LANDED**, in four commits ending at `23b9abb`.

This file was written as a spike and
is kept as the record of the reasoning; the sections below say where the shape held and where it
did not.

| section | state |
| --- | --- |
| 1 What happened before | historical. It describes the tree BEFORE this milestone; every row still names a real entry point |
| 2 The domain question | held. No new domain, teardown or ownership code was written |
| 3 The rule | implemented as stated, all three clauses |
| 4.1 Redirect, do not run | implemented. `arch_fault_is_user_thread` + `arch_fault_redirect_to_exit`, both declining by default |
| 4.2 The case it does not survive | implemented, and CORRECTED against the first draft. The guard is frame validity, not clause 3.3 |
| 4.3 The reaper alternative | not chosen, and still the recorded fallback |
| 5 What the survivor learns | held, no new mechanism added |
| 5.1 The latency hazard | MEASURED and DECIDED. The priority deflate was rejected. See below |
| 6 Gates | implemented. Four existing gates reworked, `faultsurvive` added |
| 7 `exit()` at kernel level | implemented, landed first as proposed |
| 8 Proposed order | followed, except that step 6's `lx6` is a decline, not a port. Step 5's `rxv3` half landed in M4.8.3, not here |
| 9 Questions | 1, 2 and 3 are answered, and three rulings taken since are recorded there, 9.5 being the published-console record route (M4.8.3). 4 and 5 remain open |

**Backends that opted in:** armv7m (and every M33 board through it), armv6m on a core that has the
privilege extension, rv32imac, sim on x86_64, and **rxv3, added in M4.8.3**. **Backends that
declined:** lx6, which cannot (no privilege ring); armv6m on a Cortex-M0, for the same reason as
lx6. `docs/reference/porting.md` carries the porter's trail and `docs/reference/invariants.md` the
contract.

**The rxv3 decline is OVERTURNED, and its reasoning is kept because the reasoning was right.** The
decline said rxv3 "could but would ship an `rte` no emulator in this tree can execute", and that
remains true to the letter: there is still no RXv3 emulator anywhere in this tree, and there is
still no CI gate for it. What changed is not the tree but the bench: `rx72m` was on it, so the
`rte` was executed on silicon and the port is witnessed there and only there. A future session must
read this as "witnessed on one board, unfalsifiable off the bench", not as "gated".

Porting it also found the one case section 4.2 does not cover on this ISA, which is recorded in 4.2
rather than here.

The goal, in the words that set it: a motor-control thread and a communication thread share a
system. The com thread crashes (MPU fault, divide by zero, undefined behaviour, a wild jump). The
motor thread MUST keep running, because it is the one responsible for driving the motor to a safe
state. Before this milestone it did not: every fault on every backend ended the whole system. It
does now, on every backend that has a privilege ring to read.

Contract references: `reference/invariants.md`, `reference/porting.md`. Section numbers are cited
from source, so no heading here is renumbered.

## 1. What happened before this milestone

Every fault entry point on every backend was terminal. Two shapes:

| backend | entry | terminal call |
| --- | --- | --- |
| armv7m (also every M33 board) | `HardFault_Handler`, `arch/arm/armv7m/arch_armv7m.cc:277` | `kfault_terminate()` |
| armv6m | `HardFault_Handler`, `arch/arm/armv6m/arch_armv6m.cc:191` | `kfault_terminate()` |
| rv32imac | `.Lfault`, `arch/riscv/rv32imac/switch.S:360` | `kickos_isr_fault()` on a user PMP fault, else `kfault_terminate()` |
| rxv3 | fixed vectors, `arch/rx/chip/rx72m/startup.S:107` | `kickos_isr_fault()` on a user MPU fault, else `kfault_terminate()` |
| lx6 | `_kickos_lx6_fault`, `arch/xtensa/chip/esp32/startup.S:136` | `kfault_terminate()` |
| sim | `on_sigsegv`, `arch/sim/sim.cc:551` | `kickos_isr_fault()` then `arch_shutdown(2)` |

`kfault_terminate` blinks forever or shuts the host process down with status 132
(`arch/common/kfault_terminate_default.cc:53`). `kickos_isr_fault` is the only path that names the
faulting task, and it too ends the system, via `kickos_terminate(0)`
(`kernel/init/console.cc:318`). No fault handler on any backend can return: each one tail-calls a
`[[noreturn]]`, and none of them rewrites `mepc` / the stacked PC / `EXC_RETURN` to resume anything.

There is no armv8m backend. M33 parts build as armv7m and share that fault handler.

## 2. The domain question is already answered by the code

The ruling given: the thread that did the wrong thing is the one that dies; a thread sharing that
domain and behaving correctly keeps it; a domain nobody holds is reclaimed by the kernel.

That is exactly what `Domain::refcount` already implements.

- `Domain` carries `refcount` and `immortal` and no owner pointer
  (`kernel/include/kickos/domain.h:29`).
- `domain_ref` runs once, at thread create (`kernel/thread/thread.cc:87`).
- `domain_release` runs once, in `sched::exit_current` (`kernel/sched/sched.cc:202`), and only
  decrements (`kernel/domain/domain.cc:259`).
- A slot with `refcount == 0 and not immortal` is free, and the next `domain_for` overwrites it
  wholesale (`kernel/domain/domain.cc:34`, `:216`).
- The regions a thread actually runs on are its own copy, taken at create
  (`kernel/thread/thread.cc:123`), so releasing the domain reference does not disturb a co-tenant
  and does not disturb the dying thread either.
- An MMIO grant is never shared: `has_mmio` skips the dedup scan (`kernel/domain/domain.cc:194`),
  so a DEV window has exactly one holder and comes back automatically when that holder dies.

**Consequence, and the central simplification of this milestone: fault-death needs no new domain
code, no new teardown code and no new ownership rule. It needs to reach the teardown that already
exists.** `sched::exit_current` (`kernel/sched/sched.cc:186`) is already total: it releases the
domain, runs `cap_teardown` (mutex force-unlock, endpoint EPIPE to parked senders, IRQ line detach
and mask, reply-cap EPIPE), wakes joiners and the wait-until-last waiter, and ends the process only
when it was the last live thread. M4.7.9 is therefore a routing problem, not a teardown problem.

## 3. The rule

> **A fault taken in unprivileged thread context, in a thread that is not already dying, kills that
> thread and nothing else. Every other fault panics exactly as it does today.**

Three clauses, each load-bearing.

### 3.1 Unprivileged

The discriminator is the privilege the CPU was in when it faulted, not the thread's identity and
not which stack the frame came from.

It cannot be "was this a pool thread", because KickOS runs syscall dispatch in **privileged thread
mode on the thread's own stack** (`arch/arm/armv7m/switch.S:6`, `svc_trampoline` at `:157`). A fault
there is a kernel bug in code the thread merely called, and it must still panic. Privilege
separates the two cleanly: user code runs with `CONTROL.nPRIV = 1`, kernel code on its behalf runs
with `nPRIV = 0`.

It cannot be `ctx.resting_npriv` either. That field says the thread is a user thread, not that the
fault happened in user code, so it would misclassify a fault inside syscall dispatch as the
thread's own fault and quietly kill a thread over a kernel defect.

Per backend, the bit already exists:

| backend | discriminator | present today |
| --- | --- | --- |
| armv7m / armv6m | `CONTROL.nPRIV == 1`. Exception entry does not modify `CONTROL`, so reading it in the handler gives the thread-mode privilege at fault time. `arch_context` already carries `npriv` and `resting_npriv` (`arch/arm/armv7m/arch_armv7m.cc:28`) | **no**: the handler prints an MSP/PSP label and consults nothing (`arch_armv7m.cc:232`) |
| rv32imac | `(mstatus and MSTATUS_MPP_M) == 0`, MPP being the privilege before the trap | **yes**, `arch/riscv/rv32imac/arch_rv32imac.cc:507` |
| rxv3 | `PSW.PM != 0`, PM being the previous processor mode | **yes**, `arch/rx/rxv3/arch_rxv3.cc:325` |
| lx6 | `PS.UM` | **no**, `PS` is dumped but not tested (`arch/xtensa/lx6/arch_xtensa.cc:399`) |
| sim | no real privilege; host signal only | n/a, see 6.3 |

Two backends therefore already compute the exact bit this milestone needs, and already reach a
handler that names the faulting task. They just terminate instead of killing the thread.

**This is NOT scoped by MPU posture, and an earlier draft of this section claimed it was.** The
claim was that a flat board has no unprivileged threads, so every fault on it would panic
unchanged. That is false, measured on `qemu-flat`: `kernel/init/kmain.cc` sets
`root_attr.privileged = false` with no posture gate, and privilege comes from
`arch_context_init`, not from `KICKOS_HAVE_MPU`. A flat board runs unprivileged threads and they
die thread-scoped under this rule exactly as they do on an enforcing one.

What differs on a flat board is only which accesses fault at all: a cross-domain write completes
instead of trapping, so the mpu_fault and rootfault apps never reach a fault there. That is a
property of the app, not of the rule.

Consequence for section 6: **the gate rework must split on PRIVILEGE posture, not on MPU
posture.**

### 3.2 Thread context

A fault escalated from inside an exception handler is a kernel bug and panics. On ARM this is the
stacked `xPSR` IPSR field being non-zero; on RISC-V and RX the privilege test above already implies
it, since a trap handler runs at M-mode / supervisor mode.

### 3.3 Not already dying

`Thread::dying` (`kernel/include/kickos/thread.h:120`) is set at the top of `exit_current` and never
cleared. A fault taken while it is set escalates to the old panic path.

This clause is what bounds the whole design. It is also the answer to the one case section 4 cannot
otherwise survive: a stack overflow. See 4.2.

## 4. How the thread dies

`exit_current` is an ordinary kernel function. It takes `IrqLock`, calls `cap_teardown`, which
**releases `IrqLock` between chunks** (`kernel/syscall/cap.cc:838`) so interrupts run and other
threads are scheduled during the sweep, and it ends in `reschedule()` plus an `arch_idle_wait()`
loop. None of that can run from a fault exception handler: on ARM a HardFault runs at priority -1,
where `IrqLock` gates nothing it needs to gate and a pending PendSV can never be taken, so
`reschedule()` would not switch and the sweep would run with the system wedged behind it.

### 4.1 The proposal: redirect, do not run

The fault handler runs no teardown. It **rewrites the faulting thread's resume context** so that
returning from the exception lands in a privileged thread-mode stub, and returns.

```
kickos_thread_fault_exit()   // privileged, thread mode, the thread's own stack
{
    sched::exit_current(KOS_EXIT_FAULT);
}
```

Per backend that is: stacked PC := the stub, privilege := privileged, everything else left alone
(ARM: keep the T bit, clear IT/ICI); `mepc` := the stub with `mstatus.MPP` := M (RISC-V); the
saved PC/PSW pair on the ISP (RX). The thread's register values are not preserved, because the
thread is dying and the stub takes no arguments.

The posture the stub runs in is **the same posture syscall dispatch already runs in**: privileged,
thread mode, on the thread's own stack. That is not a new hazard being introduced, it is the
existing model, which is what makes this cheap.

Two new arch hooks, both total, both defaulting so that a backend which has not opted in keeps
today's behaviour exactly:

- `bool arch_fault_is_user_thread(...)` returns `false` by default.
- `void arch_fault_redirect_to_exit(Thread*)`, only ever called when the first returned `true`.

### 4.2 The case this does not survive on its own

If the fault WAS a stack overflow, the stub would run on the overflowed stack and fault again.

**An earlier draft said clause 3.3 catches this, and that is wrong. Measured, the overflow never
reaches a first redirect at all.** The abort happens during HARDWARE STACKING, before any handler
code runs: the stacking pseudocode decrements SP before the writes, so on an abort `frame` points
at memory the frame was never written to. The observed dump was `CFSR=0x92` (MSTKERR set) with
`PC=0x0` and an `xPSR` carrying no Thumb bit, which is not a frame that thread ever produced.

Worse, the rule as first implemented then read `f[7] & 0x1FF` out of that stale RAM and declined
BY ACCIDENT, because those bytes happened to be non-zero. Had they held zero in bits 0..8, the
kernel would have rewritten and resumed a fabricated context, privileged.

So the real guard is FRAME VALIDITY, and it must be read from the status register before the frame
is trusted at all: `arch_fault_is_user_thread` declines when CFSR carries MSTKERR/MUNSTKERR
(bits 4/3) or STKERR/UNSTKERR (bits 12/11), mask `0x1818`. `MLSPERR` and `LSPERR` are deliberately
excluded: lazy FP preservation leaves the integer frame valid, and including them would panic a
legitimate user fault.

Clause 3.3 is still the guard, but for a narrower case than claimed: a stub that re-faults with SP
still valid. The overflow construction does not reach it.

#### On RXv3 neither test can see the overflow, and that is an ISA property (M4.8.3)

Both guards above read a moved SP: armv7m's because the stacking abort left one, rv32imac's because
its software prologue wrote the frame at one. **RXv3 has neither, because every one of these
exceptions is INSTRUCTION-CANCELING** (RXv3 ISA UM sec.5.3.1, Table 5.1): the CPU restores the
architectural state of the cancelled instruction, so the denied `push` reads as though it never
moved the USP. The frame is on the kernel ISP, where nothing the thread did can invalidate it, and
the USP passes a containment test with nothing left below it.

**Measured on `rx72m` before this was understood** (`.session/logs/m483rxovf-*`, and
`m483rxovft-*` with the `KICKOS_RX_MPU_TRACE` localizer on): the kill was ACCEPTED, the stub ran
privileged on the exhausted stack, supervisor bypasses the RX MPU so nothing trapped the damage, and
it smashed its way to `=== RX EXCEPTION (trap) === PC=0x0 PSW=0x0`.

The only evidence left is the faulting ADDRESS, so the port added `kickos_fault_below_stack(addr)`
and rxv3 refuses an operand-access MPU error beneath the running thread's stack base. A stack grows
down, so an overflow's first denied access is beneath the base by construction and the test is EXACT
for that case. It is not exact for the converse, and the cost is measured rather than argued: on
`rx72m` a cross-domain write to a LOWER address escalates to the panic dump instead of dying alone,
which `user/apps/common/mpu_fault` demonstrates (`0x13200`, below `domainA`'s stack), while `rxdrv`'s
peripheral poke (`0x8c068`, above it) still dies alone. Nothing regresses: before this milestone
every RX fault panicked.

**4.3's reaper is what removes this guard rather than tuning it.** A stub that never runs on the
dying thread's stack needs no test at all, which is the property 4.3 already claimed and is now
measured on an ISA that needs it. Filed in `TODO.md`.

Under enforcement an unprivileged thread's stack is an MPU region, so an overflow faults on the
guard rather than walking into a peer. `faultoverflow` is the witness, and it asserts MSTKERR in
the dump rather than merely asserting that a panic happened, so it cannot pass by the same accident
again.

### 4.3 The alternative, recorded and not chosen

Give the kernel a reaper thread, have the fault handler mark the TCB and hand it to the reaper, and
switch directly to the next runnable thread without ever resuming the faulter. That removes the
dying thread's stack from the picture entirely and would survive 4.2.

It costs a generalisation of `exit_current` from "the current thread" to "this thread" (the parts
that are already `Thread*`-taking are `cap_teardown` and `domain_release`; the `current`-specific
parts are `reschedule()` and the trailing idle loop), plus a thread, a stack, a queue, and a
priority decision for it. It is the fallback if 4.1 proves unsound in practice, and it is also the
shape M4.7.8 already recorded as wanted for other reasons (`TODO.md`, reaper classification).

## 5. What the surviving thread learns, and when

The mechanisms already exist and this milestone should add none.

- A peer parked sending on the dead thread's endpoint is woken with `-KOS_EPIPE` when the last
  WAIT-bearing cap goes (`kernel/syscall/cap.cc:283`).
- A peer holding a mutex the dead thread owned gets `MUTEX_OWNER_DIED` (`kernel/sync/sync.cc:457`).
- A peer that joined it with a deadline is woken with 0 (`kernel/sched/sched.cc:228`).

That last one is the watchdog the brief describes, and M4.7.8 is what made it a watchdog: a join
that can time out. The motor thread joins the com thread with a bounded deadline; a return of 0
means the com thread died, `-KOS_ETIMEDOUT` means it is still alive but silent. Both are actionable
without a new primitive.

### 5.1 The latency hazard, which is the actual safety question

`cap_teardown` runs **in the dying thread's own context, at its own priority**, and the dying
thread's priority is deliberately not deflated during the sweep (`kernel/sync/sync.cc:459`,
`kernel/syscall/cap.cc:260`). If the com thread is higher priority than the motor thread, the motor
thread is delayed by the whole sweep of a thread that is already dead.

For the stated use case that is the thing that matters, so it needed a number and a decision, not a
paragraph. Both are now in hand.

#### The measurement

The capture, its instrument and the limits of both are `docs/archive/M4.7.9_teardown_latency_meas.md`,
which is the authority for every figure below. Measured on `23b9abb`, `bench` variant, 296 deaths per
board, run-to-run spread below instrument resolution. All five `cap_teardown` arms were exercised in
every death: mutex force-unlock returning `EOWNERDEAD`, endpoint EPIPE to two parked senders, an
outstanding reply cap, a claimed IRQ line, and the thread's own semaphores.

| phase | xmc4800-relax | frdmk64f |
| --- | --- | --- |
| fault to stub | 2.3us | 2.6us |
| THREAD FAULT dump (PREEMPTIBLE, runs before `dying` is set) | 105.3us | 132.1us |
| `cap_teardown` sweep | 50.6us | 72.7us |
| **FROZEN, nothing of any priority runs** | **55.3us** | **78.2us** |
| total, fault to off the ready set | 162.9us | 212.9us |

The sweep scales at about 2.3us per slot (xmc4800-relax) and 3.2us per slot (frdmk64f), so a 64-slot
table extrapolates to roughly 150us / 210us of sweep.

Two facts fall out of that table and they are the whole answer:

- **The fault path costs a higher-priority survivor nothing extra over an ordinary exit.**
  `kos_exit`'s frozen window is the same to within 2us. The frozen term is `cap_teardown`, and
  `cap_teardown` is not something fault-death added.
- **The whole extra cost of a fault death is the preemptible print**, which by construction does not
  hold the survivor off: it runs before `dying` is set, at the dying thread's priority, and a
  higher-priority peer preempts it normally.

#### The decision: the priority deflate is REJECTED

Measured, not argued. Deflating the dying thread to priority 1 before the sweep buys nothing and
costs about 1.9us (xmc4800-relax) / 3.1us (frdmk64f).

The reason it buys nothing is not about donation invariants at all. With the dying thread deflated
to priority 1, a priority-20 peer woken mid-sweep still waited 34.6us, and the sweep was still never
preempted, because `sched::wake()` returns early on `kernel().current->dying` for ANY woken thread
regardless of priority. There is no reschedule left for a deflate to act on. **A priority deflate is
inert while that blanket guard stands**, so the deflate is not a one-line change that happens to be
unnecessary; it is a change to the wrong layer.

The guard itself is the real finding and it is scheduler core-path work that does not ride this
milestone. It is recorded in `TODO.md` with its numbers and with the proof obligation it still owes.

**RETRACTED IN PART, M4.8.2.** The blanket guard is gone: `sched::wake` now admits a strictly
higher-priority peer, so a deflate is no longer inert and the paragraph above no longer supports the
rejection. The MEASUREMENTS stand and the conclusion is now the other way round from the one the
rejection assumed: deflating the dying thread would lower the very quantity the new guard compares
against, so it would ADD preemptions rather than buy nothing. The two changes multiply and are not
independent. What is still true, and is the reason to leave the deflate rejected, is that it is a
change to the wrong layer. See `design-m4.8.2-host-unit-tests.md` section 8.2, which also records
that the timer already performs exactly this deflate, mid-sweep and with no `dying` test.

#### The budget this rests on, stated so it can be checked

Against the motivating workload, a 10 kHz control loop on a 100us period tripping its safety at ten
silent cycles, a frozen window of 55us (xmc4800-relax) / 78us (frdmk64f) is at most ONE missed cycle
out of the ten the supervisor tolerates. That is inside budget with margin, and it is the assumption
the rejection rests on.

**A workload that does not fit this assumption invalidates the decision, not the measurement.** A
period under about 160us, or a supervisor that trips on fewer than two silent cycles, would put the
sweep back in the critical path, and at that point the repair is the `wake()` guard rather than the
deflate.

## 6. Gates

### 6.1 The four existing fault gates change meaning

Four, not three: `check_qemu_ringppb.sh` was missed in the first pass. `ringpriv`'s second arm has
unprivileged root read `SCB->CPUID`, and that BusFault is now a thread kill. Its sibling
`qemu_ringpriv` is unaffected.


`tests/integration/check_fault_dump.sh`, `tests/integration/check_mpu_fault.sh` and `tests/integration/check_rootfault.sh` all assert
today that a deliberate fault ends the system. Under this rule that stays true on a flat board and
becomes false on an enforcing one. Each gate must therefore become posture-aware, and the enforcing
arm must assert the opposite of what it asserts now.

`user/apps/common/rootfault` needs particular care: root is unprivileged
(`kernel/init/kmain.cc:247`), so under this rule root itself dies thread-scoped. That is coherent
(if root was the last live thread, `exit_current` reaches `live == 0` and ends the process with the
code), but it is a real semantic change to a gate that currently expects a panic, and it is worth
stating in the milestone rather than discovering in a capture.

### 6.2 The new witness

A `faultsurvive` app: two threads, one faults, the other keeps working and reports. The gate must
assert ordering, not just presence, because the whole claim is that the survivor ran **after** the
fault. Mutation proof: break the discriminator and confirm the gate fails.

### 6.3 Where it cannot be witnessed

- **sim** has no real privilege, so 3.1 has no bit to read. Either leave it panicking (honest) or
  give it a synthetic posture. Leaving it panicking costs the fastest test loop, which is a real
  cost; this is an open question.
- **f302nucleo** was believed to have an open defect where `udf` never entered `HardFault_Handler`.
  It was the FLASH COMMAND: `--connect-under-reset --reset` armed `DEMCR.VC_HARDERR` and the core
  halted at the handler's first instruction. `tools/flash-stlink.sh` no longer pairs the two, the
  `udf` escalates normally, and the board can carry the witness. It has no MPU, so the MPU-fault
  arms stay out of reach there; `udf` fault isolation needs none of it.

## 7. `exit()` at kernel level

Separate from the fault work and much smaller than it looks.

`kos_exit` already exists and works (`KOS_SYS_EXIT = 8`, `user/include/kickos/sys/abi.h:57`,
dispatch at `kernel/syscall/syscall.cc:526`), and `_exit` already reaches it
(`user/src/newlib_stubs.cc:126`), which is why `abort()` and a failed `assert` already terminate
correctly.

What does not work is standard `exit()`. It does not **link**: newlib's `exit` pulls
`__libc_fini_array`, which needs `_fini`, and no linker script in the tree defines one
(`user/apps/common/sched_exit/main.cc:30` states this).

Every linker script already partitions the app `.fini_array` into its own section and then asserts
it empty (`boards/qemu-m33/mps2.ld:74`, `:253`, and the same pair in every other chip script). So
the array `__libc_fini_array` would walk is guaranteed empty, and an empty `_fini` is not a stub
that hides something: it is the truth the linker already enforces. Defining it makes `exit()` link
and behave exactly like `_exit`, with the assert left in place as the thing that keeps that true.

This is the consumer-API item the brief names, it is orthogonal to the fault work, and it should
land first because it is small and unblocks M4.8.1's "call `exit()` from the services".

## 8. Proposed order

1. `_fini`, so `exit()` links. Small, orthogonal, independently gated (section 7).
2. The two arch hooks, defaulting to today's behaviour, plus `kickos_thread_fault_exit` and the
   clause 3.3 escalation. No backend opted in yet, so nothing changes.
3. armv7m opts in. Most boards, and the one with no discriminator today, so it is the honest first
   target rather than the easy one.
4. The `faultsurvive` app and gate, plus the posture split in the three existing gates (6.1).
5. rv32imac and rxv3 opt in. Both already have the bit and already reach a task-naming handler;
   this is wiring, not design.
   **rv32imac landed here; rxv3 landed in M4.8.3, and "wiring, not design" was wrong about it.** The
   redirect itself is wiring -- it is the syscall trap's own PSW rewrite -- but the frame-validity
   half is not, because this ISA cancels the faulting instruction and restores SP (see 4.2).
6. armv6m, lx6.
7. Measure the teardown window and decide the priority deflate (5.1).

## 9. Questions, answered and still open

### 9.1 Answered

1. **sim**: it opted in, on x86_64 only. Its seam rewrites the host `ucontext` register file
   directly, so on a host whose layout it does not know `arch_fault_is_user_thread` declines and the
   gates fall back to asserting a panic. The fast loop is kept without claiming a privilege the host
   does not have.
2. **The exit code**: `KOS_EXIT_FAULT`. It is NOT distinguishable by a joiner, and that is a ruling,
   not an oversight. See 9.2.
3. **Does the fault dump still print?** Yes, from `kickos_thread_fault_exit` in thread context, and
   the handler prints NOTHING. `kickos_fault_record` carries the facts across the redirect. The
   arch reporters were not reused, precisely because each one calls `kpanic_enter` first thing and
   that reclaims the console permanently. It was not the largest refactor in the milestone.

### 9.2 `KOS_EXIT_FAULT` is not join-visible, and that is deliberate

`sched::exit_current` hardcodes `wait_result = 0` for every JOIN wake, so a joiner cannot tell a
fault death from a clean `kos_exit`. **Ruled a non-issue and closed, not deferred.** Two reasons:

- Reusing an exit code as an error code is a caller bug. A joiner that branches on the value is
  asking a liveness primitive a question about causes.
- A supervisor should watch **liveness by activity**, not by join: "no new targets for ten cycles,
  go to safety" is the check that also catches a thread that is alive and wedged, which is the
  failure a join result cannot report at all. Section 5 already gives that supervisor everything it
  needs, and M4.7.8's timed join is what makes the distinction between silent and dead.

The join ABI is not changing for this.

### 9.3 The fault record can be taken by a later fault, and the print says so

`g_fault` is one record, because `Thread` carries no tail padding on any target, so a per-thread
field would grow every TCB (256 bytes where `KCAP_RUN_CHUNKS` is 1, 264 where it is 2). The window between the redirect and the stub is PREEMPTIBLE, since `dying` is
not set until `exit_current` runs at the end of `kickos_thread_fault_exit`, so a second unrelated
fault can overwrite the record before the first thread's stub reads it.

**Accepted, with the misattribution made impossible rather than merely unlikely.** The record
carries the `Thread*` it was captured for, and a stub that does not own the record prints
`PC unavailable, a later fault took the record` instead of another thread's PC, status and address,
and leaves the record valid for the thread it does belong to. Both threads still die correctly, both
still announce themselves by name, and the residual cost is that the first thread's PC is lost in a
double-fault race. That is strictly better than a plausible wrong answer, and it costs one pointer
in `.bss` and one comparison.

### 9.4 Still open

4. **Resource leak under a crash loop.** `kos_ram_alloc` never gives memory back (there is no
   `arch_ram_free` anywhere, `kernel/include/kickos/cap.h:198`), so a thread that is respawned and
   crashes repeatedly exhausts the arena. Access rights are not leaked (the TCB regions are wiped at
   slot reuse, `kernel/thread/thread.cc:37`), only memory. Out of scope here; worth a `TODO.md` line.
5. **The peripheral is left live.** `cap_teardown` masks and detaches the IRQ line, but the device
   itself keeps whatever the dead driver programmed into it (TE/RE still set, a DMA channel still
   armed). Harmless while the line is masked. Whether a respawned driver must be able to assume a
   quiesced device is a driver-model question, not a kernel one, and belongs with M4.8.1.

### 9.5 A published console swallowed the record, and the fix is a route, not a reclaim (M4.8.3)

The record printed through plain `kickos::kprintf`, and `console_emit` DROPS every kernel chip write
while a userspace driver owns the console. So on the two boards whose default service list carries a
console driver -- `frdmk64f` and `xmc4800-relax` -- a survivable fault's record was lost, while a
PANIC got through, because the panic path funnels through `kpanic_enter` and that reclaims. Proven
both ways at one tree on `frdmk64f` (`.session/logs/m483-fs-frdmk64f-faultsurvive.log` against
`m483-fsk-frdmk64f-faultsurvive.log`) and reproducible on the host with `kickos_services_sim`.
Isolation itself was never affected: the faulter died alone and root survived in every capture. What
was lost is the one line naming the dead thread, on every realistic image.

**The answer is forced once the question is asked properly.** While the console is published, the
ONLY agent permitted to put a byte on that device is the driver. So either the record travels to the
driver, or the kernel takes the device back. There is no third transport.

Taking it back was rejected on register facts. Every one of the four `arch_console_reclaim` bodies
clears a UART TX interrupt enable and reprograms baud, so a reclaim is not reversible from the kernel
side and a SCOPED one -- reclaim, print, hand back -- would restore the state variable while leaving
a live IRQ-driven driver's TX source silenced and its service thread parked forever. That is the
hazard `console_on_driver_death` already defers around. A PERMANENT reclaim additionally violates
`fault-kill-path-never-enters-panic` and kills a healthy console driver because one unrelated thread
faulted, on a system whose whole premise is that it keeps running.

Splitting `kpanic_enter` and taking only its forced-writer half fixes nothing: `console_emit`'s
`USER_OWNED` arm returns before the writer choice is ever reached. Poking the device polled without
reclaiming is the cheapest option of all and is rejected on the isolation principle -- it interleaves
bytes into a frame the driver is shifting out, and on a chip where the driver holds the window as a
granted capability it is the kernel writing a device it has handed away.

So: **`kprintf_fault`, and only the four fault-record sites use it.** It is `kprintf` plus one thing.
While the state is `USER_OWNED` it also hands the formatted line to the published endpoint's
ALREADY-PARKED receiver through `cap_console_deliver`, which is `endpoint_send`'s parked-receiver arm
and lives beside it so the two cannot drift. It reaches the endpoint through the KERNEL's own
identity reference (`cap_console_publish`), not the dying thread's cap table, which a thread spawned
before the publish never had seated.

Three properties are load-bearing:

- **It never parks.** A fault record must not be able to wedge a dying thread on a driver that is
  mid-write or already gone. This is also why `endpoint_send` could not simply be called with a zero
  timeout: `park_deadline_arm` floors any deadline other than `KOS_TIMEOUT_NONE` to
  `KICKOS_TIMER_MIN_DELTA_NS`, so a zero timeout still parks, still switches away, and answers
  `-KOS_ETIMEDOUT` a tick later.
- **It is narrow to this one caller.** Routing all of `kconsole_write` would relight the kernel debug
  console after a handover and retire `check_sim_published.sh`'s negative assertion, which is the
  only thing proving a handover happened at all.
- **Order survives, and not by luck.** The delivery pops the driver out of `recv`, so a later user
  `kos_send` finds no parked receiver and parks in `send_waiters` instead. The driver therefore emits
  the record, returns to `recv`, and only then takes that line. `check_faultsurvive.sh`'s
  `survived > killed` assertion holds across a handover, which `check_sim_faultsurvive_pub.sh` pins.

**A record is up to THREE lines from five call sites emitting four kinds of line, and only the first is unconditional.** The
banner always prints; then exactly one of `PC lost to a later fault` / `PC=` / `PC= STAT=`; then
`ADDR=` only when the fault latched an address. Each is its own delivery, and the first one POPS the
driver out of `recv`, so the second finds an empty `recv_waiters` unless the driver has run and
re-parked in between. That it does is not luck either,
and it is not new machinery: `cfg->prio` for a console service must already be `>=` every stdout
client, because a rendezvous carries no priority inheritance (`k64uart.cc`, D9), so the woken driver
strictly preempts a faulting client and re-parks before the next line. `frdmk64f` provisions its
console at 12 against a `faulter` at 10, and all four `m484` boards printed the banner AND its
`PC=`/status line. **The dependency is worth knowing because it is invisible at the call site**: a
console driver provisioned BELOW one of its clients -- already forbidden, for an unrelated reason --
would truncate every fault record to its first line. Making the record one message instead of four
would remove the coupling and is the natural follow-up; the line the defect was filed over is the
first one.

Two things it still loses, and both are accepted rather than unnoticed:

1. If NO receiver is parked at that instant, the line is dropped with no retry -- the faulter WAS the
   driver's last receiver, or the driver is mid-write. A bounded park would close this and is refused
   above.
2. A line handed over but never drained, because the system shut down before the driver was
   scheduled. `kickos_terminate` flushes the KERNEL ring only.

Case 1 covers the console driver faulting in its OWN service thread: that thread is running rather
than parked, so no receiver is found, and the reclaim `console_on_driver_death` then performs arrives
after the record is already gone. There is no zero-`.bss` repair for that ordering.

One consequence is a widening and is accepted rather than overlooked: a published console driver now
SEES the faulting thread's name, PC and fault status, where before those bytes were dropped inside
the kernel. That driver already receives every byte of every thread's stdout, so the record is
strictly less than what it holds; a posture where it must not is a posture with no published console.

A deferred record -- park it in `.bss` and flush it at the next `kpanic_enter` or driver-death
reclaim -- was weighed and rejected twice over: `microbit` has zero arena slack, so any `.bss` byte
costs a selftest arm, and in the case the line exists for (a system that keeps running) the record
would never arrive at all. Making the record a queryable fact for a userspace supervisor is the right
long-term shape and is where this invariant's name points, but a default image that does not poll it
still shows nothing, which is the complaint. Filed in `TODO.md`, not built.

Cost measured: `.bss` byte-identical on every image, `.text` +256 to +272 B.

Witnessed on silicon at TAG `m484` on four boards and four enforcement classes, and MUTATION-PROVEN
there on two of them -- including one whose console driver is IRQ-driven and buffered, which is
exactly the case a scoped reclaim would have wedged. The capture table is in `STATE.md`.
