<!-- SPDX-License-Identifier: CECILL-C -->
# Protection follows the CPU, not the scheduler's intent

> Where the per-thread MPU region set must be committed to hardware, and why that
> point is the *physical* context switch and never the scheduler's logical decision
> to switch. This chapter builds on Chapter 3.5 (the deferred switch -- a switch that
> is requested now but completes later) and Chapter 7 (per-thread memory protection);
> it is the timing rule that ties them together. It points into
> `../reference/invariants.md` (`mpu-apply-on-every-switch-in`, `arch-switch-may-defer`,
> `deferred-switch-lowest-band`) for the exact per-arch contract -- this chapter
> explains why the rule reads the way it does.

## The resource that is keyed to "the current context"

A thread's context is not only its registers and stack (Chapter 3.5). It is also the
set of addresses it is *allowed* to touch: its code, its data, its own stack, and any
region deliberately shared with it -- the MPU region set the hardware faults everything
outside of (Chapter 7). That region set is a single hardware resource: the MPU holds
*one* active configuration at a time, and it means "these are the bounds of whatever is
running right now."

So there are two facets of "the current thread" that the hardware tracks, and they
must agree. The registers-and-stack facet is swapped by the context switch. The
memory-bounds facet is swapped by reprogramming the MPU. If the two ever disagree --
if the MPU says "the bounds are thread B's" while the CPU is still physically executing
thread A's instructions off thread A's stack -- then the running code is being judged
against the wrong rules. The whole point of this chapter is *when* the second swap is
allowed to happen, and the answer is: at the same instant as the first, never before.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (protection hardware) and
ch.2 (the dispatcher) -- here the two meet.*

## Why: the logical switch and the physical switch are not the same instant

On a naive mental model the context switch is atomic: the scheduler decides thread B
should run, and thread B runs. On real silicon that is often false, and Chapter 3.5
already named why. A switch requested from an interrupt -- or from a thread while a
critical section is held -- must not take effect until the interrupt and the lock have
finished. So the architectures provide a *deferred* switch: a lowest-priority
"do it on the way out" trap (ARM's PendSV, RX's SWINT, a RISC-V software interrupt) that
the switcher *pends* and lets fire later. (Contract: `arch-switch-may-defer` and
`deferred-switch-lowest-band` in `../reference/invariants.md`.)

This splits one conceptual event into two instants:

- **The logical switch.** `switch_to(next)` runs, on the outgoing thread. It updates
  the scheduler's bookkeeping -- `kernel().current = next` -- picks the incoming thread,
  arms its timer slice, and *requests* the physical swap. Then it returns, and the
  outgoing thread keeps executing.
- **The physical switch.** Later -- once the critical section releases and every
  higher exception has tail-chained -- the pended trap fires, saves the outgoing
  registers, loads the incoming ones, swaps the stack pointer, and returns into thread
  B. Only now is thread B actually on the CPU.

Between those two instants there is a window. `kernel().current` already says B, the
scheduler believes B is running -- but the CPU is still running A, on A's stack, with
A's return addresses and A's spilled temporaries live in memory. Everything in that
window is thread A's execution wearing thread B's name tag.

## The hazard: applying protection at the decision point

Now put the MPU reprogram in the wrong place. The obvious, tempting spot is right where
the scheduler decides -- inside `switch_to`, next to `kernel().current = next`:

```
// switch_to(next), the EAGER (wrong) shape:
kernel().current = next;
next->state = ThreadState::RUNNING;
arch_mpu_apply(next->regions, next->region_count);  // MPU now describes B ...
ktime_rearm();
arch_switch(&prev->ctx, &next->ctx);                // ... but this only PENDS the swap
```

Read it against the two instants. `arch_mpu_apply` reprograms the hardware
*immediately* -- the MPU now enforces thread B's bounds. But `arch_switch` only *pends*
the physical swap; it returns, and thread A keeps executing. For the whole width of the
window, thread A runs under thread B's region set.

What happens depends on whether protection is actually enforced:

- **With the MPU on**, thread A's very next access to its *own* stack -- a spill, a
  local, a return address -- is an address that thread B's region set does not map.
  The MPU faults. A thread takes a hard fault touching its own stack, which reads as
  impossible until you see the window: `current` and the MPU both say B, while A is the
  one physically executing.
- **With protection off** (a privilege-only build, or a board with no MPU), there is no
  fault. Thread A simply runs -- silently -- under thread B's isolation. Nothing traps,
  nothing is logged, and A can now read or write whatever B was permitted and A was not.
  That is a confidentiality and integrity breach, and it is the *worse* outcome
  precisely because it is quiet.

A concrete shape makes the window vivid. Take a chain of threads contending on a mutex
(Chapter 2.3): a low-priority holder, and higher-priority threads spinning up behind it.
Under that churn the scheduler is constantly deciding to switch while a critical section
is held, so the switch is constantly deferred -- the window is open on almost every
transition. The observable symptom is a hard fault whose fault address is a *stack*
address, taken while the kernel's `current` pointer and the live MPU configuration name
one thread but a different thread is physically mid-instruction. Same peripheral, same
code; the only variable is which thread the silicon is actually running versus which one
the scheduler has already crowned.

## The deeper lesson: a passing test does not prove a race-free mechanism

Here is the part worth internalising, because it generalises far beyond the MPU.

The window is a *structural* defect -- it exists in the shape of the code regardless of
whether any run ever lands in it. And whether a run lands in it is almost entirely a
function of how *wide* the window is, which is a property of the silicon, not of the
logic:

- On a fast core, the gap between the eager `arch_mpu_apply` and the pended swap is a
  handful of instructions. The odds that an access to the outgoing stack falls inside
  those few instructions are tiny. The board runs, the enforcement tests pass, and you
  get a green wall of confidence.
- On a slow core the same gap is *wide*. A Cortex-M0+ has no hardware divide, so the
  tickless time math -- the ns/cycle conversions the switch path does on the way through
  (Chapter 2.1) -- is a long software routine. That stretches the window from a few
  instructions to many, and under scheduling churn the outgoing thread reliably touches
  its stack while the window is open. The slow board fails every time; the fast boards
  never did.

So the enforcement tests passing on the fast fleet did *not* prove the enforcement
mechanism was race-free. It proved only that the fast fleet's window was too narrow to
hit. A structural race can hide, unobserved, behind a green test suite on every fast
board and surface only when a slow core or heavier churn widens the window enough to be
caught. The discipline the reader should take away: reason about a hardware-timing
window *structurally* -- "is there any instant where the two facets disagree?" -- and do
not let a passing test stand in for that argument. Tests sample the window; they do not
close it.

## The virtue hiding in the failure: enforcement surfaces its own bugs

There is a redeeming turn. The reason this bug was *catchable* at all is that the MPU
was on. With enforcement enabled, thread A running on the wrong region set trapped the
moment it touched its own stack -- a loud, immediate, localisable hard fault with a
telltale signature (fault address in a stack, `current`/MPU disagreeing with the
physical thread). With enforcement off, the identical defect produces silent
cross-thread memory access that might corrupt state now and manifest as a mystery
elsewhere, hours later, or never visibly at all.

That is a general property worth stating plainly:

> Hardware memory protection earns its keep twice. Once by *containing* a bug -- a wild
> access faults instead of corrupting a neighbour. And again by *surfacing* latent
> ones -- a timing defect that would be invisible silent corruption becomes an
> immediate, diagnosable trap. Turning the MPU on does not only make the system safer;
> it makes its own bugs observable.

## The fix: commit protection in the switch epilogue

The principle falls straight out of the diagnosis. The MPU region set is the
memory-bounds facet of the current context, so it must change *with* the context -- at
the physical switch point, never at the logical decision. Concretely:

1. **At the scheduling decision, stash the requested region set.** `arch_mpu_apply`
   does not touch the hardware; it copies the incoming thread's regions into a private
   pending buffer (a copy, not a pointer -- so a later change to the thread's descriptor
   cannot be chased by the commit). The scheduler's decision is recorded, not enacted.
2. **In the switch epilogue, commit it to hardware.** After the pended trap has saved
   the outgoing registers and loaded the incoming ones -- *after* the physical swap, when
   the CPU is genuinely now running the incoming thread -- the trap handler calls the
   commit routine that programs the stashed set into the MPU. The two facets change
   together, atomically as far as any running thread can observe.

The commit itself brackets the disable/reprogram/re-enable so that a device interrupt
firing above the switch trap's priority cannot observe a half-programmed or disabled
MPU -- the same lowest-band reasoning that makes the deferred switch safe in the first
place (`deferred-switch-lowest-band`). An architecture whose switch is *not* deferred,
or a board with no MPU, needs no stash: the eager apply is already at the physical
switch point, or there is nothing to commit. The stash-then-commit shape is exactly the
correction for the arches where the two instants diverge.

This is the same discipline Chapter 3.5's telemetry rule already follows from the other
direction: the switch record is emitted from the tids read out of the two contexts that
*physically* swapped, never by re-reading the scheduler's decision, because a preempting
ISR can rewrite the decision between the logical `switch_to` and the physical swap
(`switch-record-from-physical-contexts`). Naming, protection, and any other
context-keyed fact must all be read or written at the true switch point, not the
intended one.

## The general rule, and the forward tie

State it once, for every resource the hardware keys to "the current context":

> Any hardware-enforced *current-context* resource -- the MPU region set today, and an
> MMU page-table root, banked FPU/lazy-stacking state, or an address-space id tomorrow
> -- must be switched in lockstep with the physical register-and-stack swap, at the true
> switch point. Committing it at the scheduler's logical decision opens a window in
> which the running code is governed by the next thread's context while it is still the
> previous thread executing.

The forward tie is direct. When an MMU arrives (the roadmap's MPU-*and*-MMU horizon),
the page-table root is exactly such a resource: swap it a few instructions too early and
the outgoing thread runs against the incoming thread's address space, with faults or
silent cross-space access of precisely the shape above. The rule established here for
the MPU region set is the rule the page-table-root switch will inherit -- protection
follows the CPU, not the scheduler's intent.

## Where to go next

- The deferred switch this chapter rests on -- why a switch requested now completes
  later, and the lowest-priority trap that carries it: Chapter 3.5, *Context switching
  and the silicon contract* (the deferred-switch axis).
- What the region set *is* and how it is built per thread: Chapter 7, *Memory
  protection*, and Chapter 3.7, *Peripheral isolation and the hardware ceiling* (what a
  region can and cannot gate).
- The exact, binding contract -- when `arch_mpu_apply` runs, that the switch may defer,
  and the priority band the commit sits in: `../reference/invariants.md`
  (`mpu-apply-on-every-switch-in`, `arch-switch-may-defer`, `deferred-switch-lowest-band`).
