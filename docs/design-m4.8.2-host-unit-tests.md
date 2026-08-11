<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# M4.8.2: the host unit-test layer has two seams, not one

> **Status: EXPLORATORY** -- a spike. Nothing here licenses a change outside `tests/drvbringup/`,
> which is the one thing in this document that is implemented, runs, and is mutation-proved.
> See `design/README.md` for the marker taxonomy.
>
> This continues an earlier spike that lives on a branch, not in this tree: the M4.9 host
> unit-test record and its `pidonation` proof of concept, written when the layer was still
> numbered M4.9. It is cited below as **the prior spike**, without an in-repo path, because
> writing one would be a claim about a tree that does not contain it. Where this document
> differs from it, the difference is named.
>
> Contract references: `reference/invariants.md`, `reference/porting.md`. The bring-up contract
> itself is `user/include/kickos/sys/driver_service.h`.

## 0. The one-paragraph ruling

The layer is **not** a new harness, **not** a mode of the sim, and **not** one thing. It is two
families of plain host executables that share one mechanism -- link-time substitution over a
seam the OS already declares -- and differ in which seam they cut:

| | cuts | subject | existing instance | state to reset |
|---|---|---|---|---|
| **U-seam** | the syscall boundary, `extern "C" kos_*` | user- and system-side code: bring-up choreography, service transports, class contracts | `tests/uartclass/`, and now `tests/drvbringup/` | none: there is no kernel |
| **K-seam** | the arch boundary, `extern "C" arch_*` | kernel internals: scheduler, sync, caps, time | `tests/ktime/`, and the prior spike's `pidonation` | the whole `Kernel` instance |

The prior spike found the K-seam and measured it at 16 functions. It did not look for the U-seam,
because its customer did not need one. **The U-seam is 11 functions, it is the cheaper half, and it
should land first.** Measured, on `7bdf1067`: `nm --undefined-only` over one host TU that takes the
address of `kickos::driver::bring_up` reports exactly eleven symbols, every one an `extern "C"` free
function -- `kos_print`, `kos_sleep_ns`, `kos_ram_alloc`, `kos_mem_self_grant`,
`kos_endpoint_create`, `kos_console_publish`, `kos_irq_claim`, `kos_thread_spawn`,
`kos_thread_kill`, `kos_handle_close`, `kos_send_timed`. Add `kickos::driver::edge_relay_thread` and
it is fourteen (`kos_irq_wait`, `kos_irq_notify`, `exit`).

## 1. What the prior spike already ruled, and it stands

Restated so this document can be read alone, not to re-argue any of it.

1. **The layer is worth building, and it costs a stub file, not an abstraction.** The sim board
   already compiles the kernel for the host, so no new port and no new seam is needed.
2. **A mock may stand in for a peripheral only where a silicon arm covers the same contract.** This
   was bought by a real defect: a mock in a target image satisfied five public class names from the
   executable, the real backend's archive member was then never extracted, no duplicate was
   reported, and two boards ran with their console driver silently redirected into the mock. The
   remedy in the tree is `tests/uartclass/` plus the `class_backend` ctest gate.
3. **The layer FREES silicon coverage, it does not replace it.** Silicon stays the sole authority
   for whether an MPU/PMP descriptor DENIES a store, for context-switch assembly and `EXC_RETURN`,
   for a real interrupt arriving mid-critical-section, for the fault path, for peripheral register
   semantics, for link-time and layout properties, and for timing.
4. **A `Kernel` fixture is practical today; `KICKOS_MULTI_INSTANCE` is not a prerequisite.**
   `kernel() = Kernel{}` plus `sched::init()` is the entire reset, because every member of the
   struct carries an initialiser.
5. **No framework yet.** doctest is the eventual choice on the house rules (real include guard,
   zero non-ASCII, C++11, one file, MIT) and Boost.UT is out on sight (`#pragma once`, non-ASCII).
   Adopt when the arm count makes filtering worth the repo weight, and not before.
6. **gmock is rejected on mechanism, not weight.** Every KickOS seam is an `extern "C"` free
   function in code compiled `-fno-exceptions -fno-rtti`; gmock cannot mock a free function and
   offers "introduce an interface" as the remedy. The linker already redirects for free.
7. **Death cases need no fork framework.** One executable per case plus a ctest
   `PASS_REGULAR_EXPRESSION`, with a sentinel the program prints only when the invariant did NOT
   fire, because `PASS_REGULAR_EXPRESSION` makes ctest ignore the exit status.
8. **`arch_switch` returns, and everything downstream of it is out of scope.** The sharpest
   consequence: a blocking primitive's park STATE is assertable, its RETURN VALUE is a fiction,
   because no waker ever wrote `wait_result`.

All eight stand. Nothing below contradicts them.

## 2. Where this document differs

### 2.1 The seam is plural, and the cheap half was missed

The prior spike derived one number, 16, for the K-seam, and read it as "the kernel's stateful core
is one stub file away from the host". True, and the reading generalises further than it claimed:
**the same sentence is true of the user side, at 11 functions, with no state to reset at all.**

That is not a refinement, it changes the landing order. A K-seam gate has to reassign the singleton,
and its arms are therefore order-coupled through one global. A U-seam gate has no kernel: reset is a
`memset` of a static arena and three counters, arms are independent, and there is no
`arch_switch`-returns trap to document because there is no scheduler to switch. The U-seam half of
the layer is strictly less to get wrong, and `tests/uartclass/` shows it was already being built
without being named.

### 2.2 The proving ground is a failure path, not a state machine

The prior spike chose priority-donation arithmetic, because the arithmetic is unreadable on target.
That is a good subject and its eleven-mutant table is the strongest evidence in the document.

But it makes the layer look like an OBSERVATION instrument, and half of what the layer is for is
INJECTION. `bring_up`'s `unwind` is the better first customer for exactly that reason: it needs a
syscall to fail on demand, and no target arm and no sim posture can arrange that. Section 5 is that
gate.

### 2.3 One of the prior spike's open questions has an answer now

> *"`check_class_backend.sh` is on an unmerged branch. Should the layer depend on it landing, or
> carry the gate itself?"*

It landed. `tests/check_class_backend.sh` is in this tree and runs as ctest `class_backend`. The
question that replaces it is section 6.3, and it is sharper.

## 3. The seam, and what each choice cannot test

### 3.1 Not a mode of the sim, and the reason is not cost

The sim runs a whole real image: the kernel, the arch backend over host pages, `mprotect`
enforcement, and a real host thread per KickOS thread. That is its value and it is also why it
cannot host these gates.

- **Its clock is real.** `arch_clock_now` reads the host clock, so the sim has no virtual time. That
  is what makes a batch `ctest` an invalid instrument for it (`STATE.md`), and it is what makes a
  one-second readiness timeout cost one second.
- **It cannot make a syscall fail.** The syscall path is the real kernel. To make `kos_irq_claim`
  refuse, a sim-hosted test would have to arrange the kernel state that refuses -- which is a
  different test, of a different subject, and for several refusals is not arrangeable at all.
- **It runs one image per configuration.** The interesting descriptor shapes for `bring_up` are a
  space, not a board.

So the ruling is: **the sim is unchanged and keeps every test it has.** The unit layer links objects
directly, and the two families differ in how much of the OS comes along.

### 3.2 What a U-seam gate cannot test

This list is the price of the choice and it is not short.

- **Whether a refusal was the RIGHT refusal.** `kos_irq_claim` returning `-KOS_EBUSY` in a U-seam
  gate is scripted. Whether the kernel actually refuses a second claim of a held line is a kernel
  question, and stays one.
- **Whether cooperative cancellation happened.** `kos_thread_kill` is honoured only inside
  `kos_irq_wait`. No thread runs in a U-seam gate, so "the peer actually died" belongs to
  `tests/check_sim_drvdeath.sh` and to silicon.
- **Whether a close reclaims the console.** `kos_handle_close(ep)` taking `recv_holders` to 0, the
  console being noted dead, and the next `kos::print` reaching the wire are three kernel effects.
  A U-seam gate proves the ORDER of the calls that would cause them. That is the half with no other
  witness; it is not the whole invariant.
- **Anything about the link.** By construction: the gate's own fake is the shadowing hazard that
  `class_backend` exists to catch (section 6.3).
- **Anything a real thread would race for.** There is no concurrency in a U-seam gate at all.

### 3.3 What a K-seam gate cannot test

The prior spike's section on this is correct and is item 3 and item 8 of section 1 above. One
addition it did not make explicit: a K-seam gate cannot test a SYSCALL, only the kernel function
behind it. The argument marshalling, the authority check and the fault-safe copy live in the syscall
dispatcher, and a gate that calls `sched::wake` directly has skipped all three.

## 4. Observation, injection, and the two things that must not be built

### 4.1 Observation without an ABI: there is no boundary, so there is no ABI

The question as posed -- how does a test observe scheduler state without an ABI for it -- dissolves
once the seam is chosen. **A K-seam gate IS the kernel.** `kernel()` resolves to
`detail::g_instance` in the test's own address space, so `kernel().current`, a thread's `state`, the
ready bitmap and a half-swept capability table are ordinary struct reads. There is no user/kernel
boundary in the process, so there is nothing for an ABI to cross.

**REJECTED: a `KICKOS_ENABLE_SELFTEST`-gated observation syscall.** Two reasons, and the second is
the one that matters:

1. It is a permanent ABI cost for a test-only need, and this tree already carries one such
   concession (`kos_guard_addr`) and should not grow the habit.
2. **It would be a WEAKER instrument.** A syscall can report only what its return and out-struct
   can carry, and only at a moment the test can reach. A host gate reads the whole struct, at any
   point, and can CONSTRUCT a state that no run reaches.

That second point is the whole argument for the `sched::wake()` customer, so state it in its terms.
`TODO.md` records the repair and its blocker: the guard is blanket, `reschedule()` already performs
the priority test, the minimal repair is a priority comparison, and the **open proof obligation is
to enumerate every peer that can observe a dying thread's half-swept capability table.** 1568
measured deaths are evidence and not proof, because the woken peers only churned their own
resources.

A K-seam gate is the instrument for that enumeration, and it is worth being precise about what it
does and does not deliver:

- It **can** seat a dying thread, drive `cap_teardown` to a chosen chunk boundary, and let a peer of
  a chosen priority read the table there -- once per observation point, deterministically, with no
  race to win and no soak to run.
- It **can** show that the guard is the only thing suppressing the switch, by asserting the ready
  structure and `current` on both sides of one `wake` call. On target the only observable is run
  order.
- It **cannot** turn the enumeration into a green run. The enumeration is a list of observation
  points a human writes; the gate makes each point CHECKABLE and each future addition cheap. A
  green suite is not the proof. It is what makes the proof auditable.

### 4.2 Injection: an Nth-call counter at the seam, and the timeout is free

Mechanism, and it is the whole mechanism: each faked symbol keeps a call ordinal and compares it to
a 1-based `_fail_at` knob, where 0 means "never". An ordinal beyond the number of calls a run makes
is silently no failure, so a stale knob reads as a happy path and never as a spurious red.

`KOS_DRV_READY_WAIT_MAX * KOS_DRV_READY_WAIT_NS` is about one second, and the naive-timeout worry it
raises is real for any test that runs on a clock. **A U-seam gate does not run on a clock:
`kos_sleep_ns` is a no-op that increments a counter.** Measured: the 13-arm gate in section 5,
including one arm that drives the readiness poll to its full 1000-iteration budget, runs in **0.66
ms per process** (100 processes in 66 ms wall).

That yields the rule worth writing down, because the tempting alternative is worse:

> **A host gate accelerates time by faking the clock syscall, never by shrinking a shipped
> constant.** A gate that lowered `KOS_DRV_READY_WAIT_MAX` for its own benefit would test a geometry
> no board ships, and would stop failing the day the shipped value changed.

One presentation detail that is load-bearing rather than cosmetic: the trace oracle collapses
consecutive sleeps into a single `sleep*1000` token. Uncollapsed, that one arm would overrun the
trace buffer, and the sharpest arm in the gate would become a truncation.

### 4.3 What it must NOT become

- **No fetched dependency, ever.** The project vendors nothing, has no submodule and no
  `ExternalProject`, and nothing is fetched at build time. That constraint is not up for
  renegotiation for a test layer. Agreed with the prior spike, including its "doctest, and not yet".
  Sharpened by section 5: after writing 13 arms I wanted exactly one thing a framework would have
  given me, an expected-versus-got diff printer, and it is eight lines. Filtering is the other real
  benefit, and `ctest -R` already provides it at executable granularity.
- **`tests/tap/` is NOT the substrate, and the reason is mechanical, not stylistic.** `tap.cc`
  routes every line through the stdout endpoint cap with a kernel-console fallback -- it is written
  in terms of `kos_send` and `kos_kconsole_write`, which is to say in terms of the very syscalls a
  U-seam gate replaces. Linking it into a U-seam gate would make the harness a client of the fake it
  is reporting through, and a K-seam gate has the same problem one level down. `tests/tap/` is the
  ON-TARGET harness, it is good at that, and it stays there.
- **The right substrate already exists and must not be abstracted.** Four host gates use a
  `g_failures` counter, `printf("not ok - ")`, and a `main` that lists its cases by name. That is
  about twenty lines per gate, it is readable without documentation, and every copy is free to grow
  the one helper its own subject needs. Do not factor it into `tests/lib/`: the day it becomes a
  library is the day a gate author has to read it.

## 5. The proof of concept: `bring_up`'s unwind, exercised

`tests/drvbringup/`, one ctest case `drv_bringup`, registered on the sim build beside the other
five host gates.

```
cmake --preset sim && cmake --build build/sim && ctest --test-dir build/sim -R '^drv_bringup$'
```

### 5.1 Why this subject

`user/include/kickos/sys/driver_service.h` is the single bring-up path for every driver in the tree.
Its failure branches -- a refused `kos_irq_claim`, a readiness timeout, a refused
`kos_thread_spawn`, a refused `block_init`, a refused publish -- had **zero** coverage of any kind
before this gate, and three separate reasons they could not get any:

- `tests/check_sim_drvdeath.sh` case 2 covers the handover tail's EPIPE arm, which is DOWNSTREAM of
  a bring-up that already succeeded, not `unwind` itself.
- The sim's two other console postures do not route through `bring_up` at all, deliberately: the
  host may refuse any given candidate window base, so those postures discover the base BY SPAWNING,
  which a descriptor's single `cfg->mmio_base` cannot express. The comment at
  `system/init/sim/service_list.cc:151-154` is that reason.
- No target image can make a syscall fail on demand.

### 5.2 The shape

| file | lines | what |
|---|---|---|
| `tests/drvbringup/kos_seam.h` | 65 | the control block and the trace accessors |
| `tests/drvbringup/kos_seam.cc` | 243 | the eleven faked syscalls, recording |
| `tests/drvbringup/bringup_unwind.cc` | 384 | 13 arms over 2 synthetic descriptors |
| `tests/drvbringup/CMakeLists.txt` | 25 | one executable |

Three decisions inside that are worth stating because the obvious alternative is worse.

**The oracle is the full ordered trace, rendered as one string, never a set of counters.** Every
claim `unwind` makes is about ORDER: close the claimed lines, then the endpoint, then cancel the
peers, and all of it before the diagnostic print. A counter oracle cannot fail on a reordering, and
two of the seven mutants in section 5.4 are pure reorderings. A complete bring-up renders as

```
alloc grant ep10 pub10 claim11 claim12 spawn50 spawn51 close11 close12 close10 probe
```

Caps come from one monotonic range and thread ids from a disjoint one, so a token names WHICH
resource it is talking about and not merely which call produced it.

**The descriptors are synthetic, not any chip's.** The subject is the choreography. A real
descriptor would drag a REGDIR-private vector number and a class block layout into the gate without
making one failure branch more reachable. Both synthetic descriptors are still put through
`static_assert(drv::valid(...))`, so the gate cannot drift into testing a shape the validator
forbids. The three-thread one exists for one reason: two live peers at the moment of failure is the
fewest that tells reverse cancellation from forward.

**The readiness latch is set by the spawn fake, not by `block_init`.** A spawned thread does not run
here, so its one observable effect on the parent -- reaching its loop and setting the latch -- is
modelled at the spawn. `block_init` publishes the latch ADDRESS, which is what a class substrate
does by laying the block out. Clearing `latch_on_spawn` is then exactly "the thread never got
there", and it is the readiness-timeout arm.

**One bug the review of this gate found, worth carrying forward as a rule for the harness.** The
first version advanced its trace cursor by `snprintf`'s return, which is the length it WOULD have
written. On the first truncation the cursor walks past the buffer, the next `cap - len` underflows,
and the following `snprintf` gets a past-the-end destination and a huge size: memory corruption, not
truncation. Dormant at 512 bytes, live for the next longer arm. Proved both ways with the buffers
shrunk to 24 and 12 bytes under `-fsanitize=address,undefined`: the old bookkeeping reports
`index 30 out of bounds for type 'char [24]'`, the clamped version is silent while every arm fails
on the truncated oracle, which is the correct failure. **A recording seam is program code and gets
the same scrutiny as the subject.** It is not test scaffolding exempt from review.

### 5.3 The arms

Thirteen, of which one is a positive control and twelve are failure paths.

| arm | what it pins |
|---|---|
| a complete bring-up touches no unwind | **the positive control.** Without it, a fake that refused everything would make every arm below pass while proving nothing |
| a refused cfg has no effect (x3: wrong kind, foreign base, posture mismatch) | the refusal happens BEFORE anything is allocated. The rc alone would pass on a guard that refused after allocating |
| `block_init` refuses the cfg | the block is allocated and granted, and there is no endpoint yet to close. A close here would close `KOS_CAP_NONE` |
| the publish fails | the endpoint that could not be published is still closed |
| the FIRST `kos_irq_claim` fails | `claimed` is 0, so nothing but the endpoint may be closed |
| the SECOND `kos_irq_claim` fails | exactly the one line that WAS claimed is closed. This is the only arm that separates `claimed` from `line_count` |
| the first spawn fails | both lines close, nobody is cancelled |
| a later spawn fails | one live peer: the endpoint close BEFORE the cancel becomes observable |
| peers are cancelled in reverse spawn order | two live peers, on the three-thread descriptor |
| a thread never reaches its loop | the readiness poll spends its full shipped budget, then unwinds one live peer |
| the barrier sits between the spawns | the poll runs AFTER the first spawn, never before it |
| the handover probe reports a dead driver | `-KOS_EPIPE` cancels every peer in reverse order and returns the code unchanged |
| a timed-out handover probe cancels nothing | `-KOS_ETIMEDOUT` leaves the service thread alive, so it cancels nobody and prints nothing |

The last two are a pair, and they are the sharpest thing in the gate after the reorderings: the
header says `-KOS_EPIPE` and every other refusal must be handled differently, and **the difference
is entirely in the side effects.** Both return a negative code. An rc assertion cannot tell them
apart.

### 5.4 The mutation evidence

Seven mutants in `user/include/kickos/sys/driver_service.h`, each applied alone, rebuilt, run,
reverted. The header is byte-identical to `7bdf1067` afterwards (`git diff` on it is empty).
**All seven KILLED.** Baseline and post-restore runs both green.

| # | mutant | arms that failed |
|---|---|---|
| M1 | `unwind(line, claimed, ...)` becomes `unwind(line, 0, ...)` on the claim failure: the claimed line leaks | 1 -- and it is the second-claim arm, the only one that can see it |
| M2 | `unwind` cancels its peers BEFORE closing the endpoint | 3 |
| M3 | `cancel_all` sweeps in spawn order instead of reverse | 2 |
| M4 | the readiness barrier moves ahead of the first spawn | 15 |
| M5 | the handover tail probes BEFORE closing its own receive cap | 3 |
| M6 | a failed spawn unwinds without the peers it already spawned | 2 |
| M7 | `wait_ready` reports success on a latch that never fired | 3 |

M1 is the one that justifies the trace oracle. It changes no return code, closes no wrong handle and
prints the same diagnostic: it simply leaks an IRQ line back into a pool that will not have it. Its
only witness is "and exactly line 11 was closed".

M2 and M5 are the same shape and are the reason a subset oracle was rejected. Both leave every
return code, every handle and every diagnostic identical, and reorder two adjacent calls whose order
the header's own comments say is load-bearing in both places.

### 5.5 A free result, verified

The `sim-ubsan` CI job reconfigures the same preset under
`-fsanitize=undefined -fno-sanitize-recover=all` and runs the registered suite. Verified on this
box: `drv_bringup` builds and passes there. That puts the real `bring_up`, `unwind`, `wait_ready`,
`spawn_one` and `console_handover_finish` under UBSan, which no target build does.

## 6. ctest, and one thing that should change

### 6.1 The sim's batching constraint is not inherited

`STATE.md` records that a batch `ctest` across suites is not a valid instrument for `sim` and
`qemu`: they have no silicon clock, they fail under the load of a back-to-back run, and they pass
standalone. That constraint belongs to tests that MEASURE through `arch_clock_now`.

A U-seam gate makes no clock call at all. A K-seam gate makes only faked ones. So these gates are
load-independent and batchable, and together with the four existing host gates they are the only
tests in the sim preset that are. They still register under `KICKOS_ARCH STREQUAL "sim"`, because
that is where the tree puts host targets, so today they INHERIT the standalone-run habit without
needing it. Cost of that: `drv_bringup` adds 0.01 s to a 23.1 s sim ctest, so nothing, and the
verification below was run standalone anyway.

### 6.2 Proposed, not landed: a `host` ctest label

The distinction above is currently a habit recorded in prose. It should be mechanical:

- put `LABELS host` on every host gate (`slotpool`, `ktime`, `captable_*`, `uart_class`,
  `capreply_packing`, `drv_bringup`, and the layer's future gates)
- `ctest -L host` then IS a valid batchable instrument, and `ctest -LE host` is exactly the set that
  must be run standalone

That is a one-line change per gate and it makes the constraint checkable instead of remembered. It
is cheap groundwork M4.8.2 should carry.

### 6.3 The open question that replaces the prior spike's

`tests/drvbringup/kos_seam.cc` defines **eleven public `kos_*` names**. That is section 2's disease
one level up from a driver class, and the gate that exists for it does not cover it:
`check_class_backend.sh` derives its symbol set from `user/include/kickos/driver/*.h` only, so no
syscall name is in the set.

**Investigated, and the syscall seam turns out to be accidentally safe -- for a reason that could
evaporate.** Every syscall stub in the tree lives in ONE archive member,
`user/src/syscall_stubs.cc`. A target image uses far more than eleven syscalls, so the linker must
extract that member for the rest, and the eleven then collide. Witnessed on this box: a scratch link
that defines `kos_print` in the executable and references `kos_recv` fails with
`multiple definition of 'kos_print'`. Loud, not silent.

So the protection is TU granularity, not a gate. Two things follow, and the second is the finding:

- `tests/drvbringup/` is safe today, and so would any future U-seam gate be.
- **The day `syscall_stubs.cc` is split per subsystem -- which is an ordinary refactor nobody would
  flag -- the protection is gone and the failure is silent.** Either widen `class_backend`'s symbol
  set to the syscall headers, or write down that the file is deliberately monolithic. Preference:
  widen the gate, because a comment does not survive a refactor.

## 7. What M4.8.2 still owes, in order

This groundwork delivers the U-seam half's mechanism, one real gate on it, and the rulings above.
What remains:

1. **The `host` ctest label** (6.2). Smallest item, unblocks honest batching, no risk.
2. **The K-seam fixture, landed.** The prior spike's stub file and `Kernel` fixture, brought into
   this tree against the current `kernel/` and re-measured. Its 16-function count was taken on a
   different tree and must be re-derived, not assumed. Deliverable: a fixture header plus a stub TU,
   with the `arch_switch`-returns limits stated in the header where a fixture author will read them.
3. **The `sched::wake()` observer enumeration** (4.1). The K-seam fixture's first customer, and the
   thing the milestone is named for. Note the order: the enumeration is a list a human writes and
   the gate makes each entry checkable. Do not expect this step to end in "green, therefore safe".
4. **The dying-guard repair itself**, once 3 has enumerated its observation points: the priority
   comparison, not the deletion. `TODO.md` also records a coupling that runs the wrong way --
   narrowing the guard puts MORE traffic through the preemptible window between a fault redirect
   and its stub, so `fault-record-is-printed-only-by-its-owner` must still hold afterwards. That is
   a silicon obligation and the host layer does not discharge it.
5. **`class_backend` widened to the syscall symbol set** (6.3), or the monolith written down.
6. **The blocking-call trap needs a mechanism, not a comment.** The prior spike left this open and
   it is still open: under a returning `arch_switch`, an arm that asserts on a blocking primitive's
   RETURN VALUE is asserting on a fiction, and nothing would catch that. It becomes urgent at step
   3, not before.
7. **Migration, and only then.** The prior spike classified the on-target selftest arms and put a
   real number on the win. That work is worth doing and it is worth doing LAST: a migration that
   starts before steps 2 and 6 will move arms onto a fixture whose traps are not yet gated.

One thing M4.8.2 should NOT owe: nothing here asks for `KICKOS_MULTI_INSTANCE`, and the prior
spike's reasoning for that stands. Its real customers are the KickCAT multi-slave sim and AMP.
