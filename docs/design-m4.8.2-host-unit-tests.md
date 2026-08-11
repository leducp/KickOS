<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# M4.8.2: the host unit-test layer has two seams, not one

> **Status: ACTIVE.** Sections 0 to 6 were written as a spike and are kept as the reasoning;
> section 7's items 1 to 4 have LANDED and section 8 is the record of what landing them found,
> including two corrections to this document and one to `TODO.md`. Items 5 to 7 are still owed.
> Section 9 is the framework decision, which is GoogleTest via Conan and which SUPERSEDES item 5
> of section 1 and two of section 4.3's three bullets. See `design/README.md` for the marker
> taxonomy.
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
| **U-seam** | the syscall boundary, `extern "C" kos_*` | user- and system-side code: bring-up choreography, service transports, class contracts | `tests/unit/uartclass/`, and now `tests/unit/drvbringup/` | none: there is no kernel |
| **K-seam** | the arch boundary, `extern "C" arch_*` | kernel internals: scheduler, sync, caps, time | `tests/unit/ktime/`, and the prior spike's `pidonation` | the whole `Kernel` instance |

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
   remedy in the tree is `tests/unit/uartclass/` plus the `class_backend` ctest gate.
3. **The layer FREES silicon coverage, it does not replace it.** Silicon stays the sole authority
   for whether an MPU/PMP descriptor DENIES a store, for context-switch assembly and `EXC_RETURN`,
   for a real interrupt arriving mid-critical-section, for the fault path, for peripheral register
   semantics, for link-time and layout properties, and for timing.
4. **A `Kernel` fixture is practical today; `KICKOS_MULTI_INSTANCE` is not a prerequisite.**
   `kernel() = Kernel{}` plus `sched::init()` is the entire reset, because every member of the
   struct carries an initialiser.
5. **No framework yet.** SUPERSEDED by section 9: the layer is GoogleTest via Conan. doctest was
   the eventual choice on the house rules (real include guard, zero non-ASCII, C++11, one file,
   MIT) and Boost.UT was out on sight (`#pragma once`, non-ASCII); the argument that lost is that
   filtering was priced at executable granularity and death cases at one executable each.
6. **gmock is rejected on mechanism, not weight.** Every KickOS seam is an `extern "C"` free
   function in code compiled `-fno-exceptions -fno-rtti`; gmock cannot mock a free function and
   offers "introduce an interface" as the remedy. The linker already redirects for free.
7. **Death cases need no fork framework.** One executable per case plus a ctest
   `PASS_REGULAR_EXPRESSION`, with a sentinel the program prints only when the invariant did NOT
   fire, because `PASS_REGULAR_EXPRESSION` makes ctest ignore the exit status.
8. **`arch_switch` returns, and everything downstream of it is out of scope.** The sharpest
   consequence: a blocking primitive's park STATE is assertable, its RETURN VALUE is a fiction,
   because no waker ever wrote `wait_result`.

Seven stand. **Item 4's second half does not**: every member of `Kernel` does carry an initialiser,
but `cap.cc` keeps three data outside that struct, so the assignment is not the entire reset once the
real capability layer is in the gate. Section 8.1 has the correction.

## 2. Where this document differs

### 2.1 The seam is plural, and the cheap half was missed

The prior spike derived one number, 16, for the K-seam, and read it as "the kernel's stateful core
is one stub file away from the host". True, and the reading generalises further than it claimed:
**the same sentence is true of the user side, at 11 functions, with no state to reset at all.**

That is not a refinement, it changes the landing order. A K-seam gate has to reassign the singleton,
and its arms are therefore order-coupled through one global. A U-seam gate has no kernel: reset is a
`memset` of a static arena and three counters, arms are independent, and there is no
`arch_switch`-returns trap to document because there is no scheduler to switch. The U-seam half of
the layer is strictly less to get wrong, and `tests/unit/uartclass/` shows it was already being built
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

It landed. `tests/static/check_class_backend.sh` is in this tree and runs as ctest `class_backend`. The
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
  `tests/integration/check_sim_drvdeath.sh` and to silicon.
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
- **The right substrate already exists and must not be abstracted.** SUPERSEDED by section 9,
  which prices what this bullet does not: one ctest entry per executable. Four host gates use a
  `g_failures` counter, `printf("not ok - ")`, and a `main` that lists its cases by name. That is
  about twenty lines per gate, it is readable without documentation, and every copy is free to grow
  the one helper its own subject needs. Do not factor it into `tests/lib/`: the day it becomes a
  library is the day a gate author has to read it. The half that STANDS is the last sentence:
  nothing was factored into `tests/lib/`, and the one shared substrate is `tests/unit/kfixture/`,
  which was already shared.

## 5. The proof of concept: `bring_up`'s unwind, exercised

`tests/unit/drvbringup/`, one ctest case `drv_bringup`, registered on the sim build beside the other
five host gates.

```
cmake --preset sim && cmake --build build/sim && ctest --test-dir build/sim -R '^drv_bringup$'
```

### 5.1 Why this subject

`user/include/kickos/sys/driver_service.h` is the single bring-up path for every driver in the tree.
Its failure branches -- a refused `kos_irq_claim`, a readiness timeout, a refused
`kos_thread_spawn`, a refused `block_init`, a refused publish -- had **zero** coverage of any kind
before this gate, and three separate reasons they could not get any:

- `tests/integration/check_sim_drvdeath.sh` case 2 covers the handover tail's EPIPE arm, which is DOWNSTREAM of
  a bring-up that already succeeded, not `unwind` itself.
- The sim's two other console postures do not route through `bring_up` at all, deliberately: the
  host may refuse any given candidate window base, so those postures discover the base BY SPAWNING,
  which a descriptor's single `cfg->mmio_base` cannot express. The comment at
  `system/init/sim/service_list.cc:151-154` is that reason.
- No target image can make a syscall fail on demand.

### 5.2 The shape

| file | lines | what |
|---|---|---|
| `tests/unit/drvbringup/kos_seam.h` | 65 | the control block and the trace accessors |
| `tests/unit/drvbringup/kos_seam.cc` | 243 | the eleven faked syscalls, recording |
| `tests/unit/drvbringup/bringup_unwind.cc` | 384 | 13 arms over 2 synthetic descriptors |
| `tests/unit/drvbringup/CMakeLists.txt` | 25 | one executable |

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

`tests/unit/drvbringup/kos_seam.cc` defines **eleven public `kos_*` names**. That is section 2's disease
one level up from a driver class, and the gate that exists for it does not cover it:
`check_class_backend.sh` derived its symbol set from `user/include/kickos/driver/*.h` only, so no
syscall name was in the set.

**Investigated, and the syscall seam turns out to be accidentally safe -- for a reason that could
evaporate.** Every syscall stub in the tree lives in ONE archive member,
`user/src/syscall_stubs.cc`. A target image uses far more than eleven syscalls, so the linker must
extract that member for the rest, and the eleven then collide. Witnessed on this box: a scratch link
that defines `kos_print` in the executable and references `kos_recv` fails with
`multiple definition of 'kos_print'`. Loud, not silent.

So the protection is TU granularity, not a gate. Two things follow, and the second is the finding:

- `tests/unit/drvbringup/` is safe today, and so would any future U-seam gate be.
- **The day `syscall_stubs.cc` is split per subsystem -- which is an ordinary refactor nobody would
  flag -- the protection is gone and the failure is silent.**

**The gate is now widened, so the protection no longer rests on TU granularity.** Its second argument
takes a `;`-separated list of header directories and carries `user/include/kickos` and
`user/include/kickos/sys` alongside the driver classes: 11 declared symbols became 82. Escaped as
`\;` in `add_test`, or CMake splits it and every positional argument after it shifts.

**How to mutation-test it, because the obvious way tests the OLD protection.** Appending a syscall
definition to an app TU makes the LINK fail, so `kickos_build` fails and the gate never runs. Feed
the script an object defining `kos_clock_now` directly instead, which is the post-split world: leg 1
names it against `syscall_stubs.cc.obj`. On `rx72m` the count must come back non-zero (57 defined of
82) or the underscore-prefix leg is passing vacuously.

## 7. The order M4.8.2 took, and what it still owes

This groundwork delivers the U-seam half's mechanism, one real gate on it, and the rulings above.
What remains:

1. **The `host` ctest label** (6.2). LANDED, and wider than proposed: the label means "executes no
   KickOS image", which covers the source-tree checks and the static ELF checks as well as the host
   programs, and is what makes the second half of 6.2's claim literally true. Root `CMakeLists.txt`
   defines it once. `oot_export` is the one gate that reads as host and does not qualify, because it
   RUNS the app it built.
2. **The K-seam fixture, landed.** `tests/unit/kfixture/`: `kfixture.h` + `kfixture.cc` + `karch_seam.cc`,
   plus `kickos_add_kseam_gate()`. Re-derived, and the number held: **sixteen**, the same set the
   prior spike stubbed. See section 8.1 for what that measurement also showed.
3. **The `sched::wake()` observer enumeration** (4.1). Done; it is section 8.2, and it changed the
   repair.
4. **The dying-guard repair itself.** LANDED as `kernel/sched/sched.cc`'s three-clause guard, five
   mutants killed (8.3). The silicon half of the coupling is still owed: narrowing the guard puts
   MORE traffic through the preemptible window between a fault redirect and its stub, so
   `fault-record-is-printed-only-by-its-owner` must still hold afterwards, and the host layer does
   not discharge that.
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

## 8. What landing steps 1 to 4 found

### 8.1 The K-seam is sixteen, and the REAL capability sweep rides it for free

Re-derived on this tree rather than assumed, with `nm --undefined-only` over the compiled objects
minus their own definitions. Three source sets, one answer:

| source set | seam width |
|---|---|
| `sched.cc` + `policy_fifo_rr.cc` + `instance.cc` | 16 |
| the above + `sync.cc` (the prior spike's set) | 16 |
| the above + `syscall/cap.cc` | 16 |

The prior spike's number was right, and two things about it were not visible from where it stood.
Note what the sixteen is and is not: only seven are `arch_*`, so "the arch boundary" names where the
seam is CUT, and the count is a property of the chosen SOURCE SET rather than of the boundary. The
set is also not element-for-element the prior spike's, even at equal cardinality; see the trade
below.
**Adding `sync.cc` widens the seam by nothing**: the scheduler core already names all sixteen and
the synchronisation layer's needs are a subset. **Adding `syscall/cap.cc` also widens it by nothing**,
because it TRADES: `cap_teardown` and `cap_teardown_active` stop being stubs and
`console_note_driver_death` and `irq_ref_drop` take their place. That decides the fixture's source
set on its own. A fixture whose `cap_teardown` is empty cannot host this milestone's subject at all,
and there was no width to pay for the real one.

**Correction to section 1, item 4.** "`kernel() = Kernel{}` plus `sched::init()` is the entire
reset" is FALSE once the real `cap.cc` is in the gate. `cap.cc` keeps **three** data in a TU-local
`constinit`, outside the `Kernel` struct that assignment reaches:

| datum | reachable from outside? | what the fixture does |
|---|---|---|
| the chunk free list | yes, `cap_slab_init()` | `reset()` calls it |
| `teardown_depth` | no | `reset()` REFUSES if `cap_teardown_active()` |
| `g_stdout_target` | no | documented as a "no arm may" in `kfixture.h` note 4 |

`nm` over the linked gate reports one further TU-local outside `Kernel`, `policy_fifo_rr.cc`'s
`g_fifo_rr`. It lands in a writable section only because of its function-pointer relocations, is
`const`-qualified and is never written, so it needs no reset. Named here so the next reader who runs
`nm` and sees it does not conclude this table is stale.

The refusal turns "an arm abandoned a sweep" from a suite that silently passes into a suite that
stops. `g_stdout_target` is the sharper one and it has teeth: `reset()` zeroes the endpoint pool, so
gen-encoded handles REPEAT across arms, and an arm that published a console would leave a stale
global handle that a later arm's unrelated endpoint close can match, noting a console death in a
DIFFERENT arm's counter. A comment is the honest stopgap and not a fix, which is why `TODO.md`
carries it: the fix is a reset entry point beside `cap_slab_init()` on the day an arm needs to
publish.

`cap_slab_init()` was deliberately not widened to zero the depth: no arm needs it, and widening
shipped code for a fixture's convenience is the wrong direction.

### 8.2 The enumeration: three wake sites, and only ONE of them is a new preemption

`cap_teardown`'s chunk loop closes each slot in the order *close protocol, copy, empty the entry,
release to the free list, drop the object reference*, so every wake below fires while slot `i` is
still LIVE. Exactly three sites, all inside `obj_close_protocol(..., teardown=true)`:

| site | woken thread | can it outrank the dying thread? |
|---|---|---|
| `mutex_force_unlock` (`kernel/sync/sync.cc`, from the `CAP_MUTEX` arm) | the highest mutex waiter | **Yes.** See below |
| the endpoint `recv_holders`-hits-zero EPIPE drain (`kernel/syscall/cap.cc`, `CAP_WAIT` arm) | every parked sender | **Yes**, and this one needs no argument: a **plain** sender (`CALL_NONE`) boosts nothing when it parks |
| the `CAP_REPLY` arm's EPIPE of the parked caller | that caller | **Yes.** See below |

`CAP_SEM` and `CAP_IRQ` wake nobody, and `irq_ref_drop` masks and detaches without waking.

**All three sites can preempt, and the first draft of this section said two of them could not.** That
claim was that the mutex and reply sites are bounded "by construction", because the peer donated its
priority to the dying thread when it parked and the teardown arms deliberately skip the recompute.
**The bound is a SNAPSHOT taken at park time, and nothing refreshes it**: `sched::set_prio` on a
BLOCKED thread writes `prio` and walks nothing, and the PI chain walk terminates at any non-mutex
park by design (`Thread::wait_mutex` answers null there, which is what stops the walk). So

- a mutex waiter parked on a mutex the dying thread owns can be boosted afterwards, by the D2
  donation in `endpoint_call`'s slow path handing a caller's priority straight to that waiter as a
  server, and the dying owner never sees it;
- a caller parked in `CALL_REPLY_WAIT` can be chain-boosted by a third thread wanting a mutex IT
  owns, and the walk stops at the caller rather than continuing to the dying server.

Neither route needs the timer deflate below, and neither is exotic. The consequence for this
milestone is that the two sites the first draft dismissed are exactly the ones that had no arm; the
gate now has one for the mutex site (`tests/unit/schedwake/wake_dying.cc`).

**What this does to the latency claim.** `TODO.md`'s 4x-to-9x figure was measured on the sweep rather
than on the reachable wake set, and the first correction here replaced it with a count of call
SITES, which is no better: the endpoint drain is a loop over EVERY parked sender while the other two
wake at most one thread each, so by woken-thread count the "narrow" site may well dominate. Nothing
has been measured either way. What is now established is only the direction: the win is available at
all three sites, not one.

**The interaction that is not independent.** `endpoint_wait_timeout` (`kernel/syscall/syscall_ipc.cc`,
the `WAIT_EP_SEND` and `WAIT_EP_REPLY` arms) calls `sched::set_prio(server, thread_effective_prio(server))`
with no `dying` test, from the timer, in the interrupt window between chunks, where `server` may BE the
dying thread and `thread_effective_prio` walks its half-swept `held_list`, `reply_waiters` and
`served_head`. That is a live mid-sweep DEFLATE of the very quantity the narrowed guard compares
against, so it can turn either bounded site into a preemption after all. The deflate and the
narrowing multiply; they are not two independent changes, which is the sharper form of `TODO.md`'s
coupling note.

**What was already live before this milestone.** `sched::tick_rr` reschedules with no `dying` test at
all, and a suppressed `wake` has ALREADY put its peer on the ready structure before the early return,
so higher-priority preemption of a half-swept table was reachable in the shipped tree. Two bounds the
`TODO.md` entry did not state: it requires the dying thread to be **RR with a non-zero quantum**, so a
FIFO dying thread was never preempted mid-sweep and now can be; and `tick_rr` reaches the switch from
an ISR, which the chunk loop's own `IrqLock` defers to the chunk boundary on every port, whereas a
wake from thread context is not deferred at all and so switches MID-chunk with slot `i` live wherever
`arch_switch` is immediate. **That is two of the five backends, and one of them is silicon**: the sim
swaps with `swapcontext`, and Xtensa/LX6 calls `xtensa_switch` directly whenever it is not in an ISR,
which is `esp32-wroom`. ARM (PendSV), RISC-V (msip) and RX (SWINT) all pend. Both are new exposure,
and both are covered by the same construction that covers the old: every protection a woken peer runs into is independent of this
guard. The reply mint into another thread's table has its own `dying` test, `cap_reply_caller` needs a
`BLOCKED` thread in `CALL_REPLY_WAIT`, `ThreadPool::alloc` reclaims only an `EXITED` slot, and each
object reference is pinned by the dying thread's own not-yet-swept cap with a leak-rather-than-strand
floor beneath it.

### 8.3 The repair is THREE clauses, not one, and the second is why

`exit_current` wakes its join and wait-until-last waiters AFTER its own `on_remove`, and its comment
names the blanket guard as what keeps that loop from switching. **A guard narrowed on priority alone
strands those waiters.** The dying thread is off the ready structure by then, so `pick_next` cannot
return it, `switch_to` preserves `EXITED`, and it is never scheduled again: the waiters the loop has
not reached yet are never woken and `kickos_terminate` never runs. On ARM, RISC-V and RX the switch is
deferred to the `IrqLock` release and the loop survives by luck of the port; on the sim and on
Xtensa/LX6 the switch is taken there and then, so a priority-only guard would have stranded joiners
on `esp32-wroom` hardware and not merely in a host gate. This is NOT in the already-live set, because
that whole block holds one `IrqLock` and `tick_rr` cannot reach it.

The discriminator needs no new field: `state == EXITED` IS "past the point of no return", and it is
already set. So the guard reads, in order: a null `current` (the pre-start asymmetry `tick_rr` already
guards and this funnel did not), an `EXITED` current (never picked again, so its own final reschedule
is the switch), and then `dying and t->prio <= c->prio`.

Twelve mutants, each applied alone to `kernel/sched/sched.cc` or `kernel/syscall/cap.cc`, rebuilt,
run, reverted. **All twelve killed**, and the ones that matter most are those a return-code oracle
cannot see. Seven of the twelve were proposed by the ten-angle review AFTER the first five, and four
of those seven SURVIVED the gate as first written, which is what the extra arms exist for:

| # | mutant | how it dies |
|---|---|---|
| M1 | the old blanket `if (c->dying)` | 3 arms: the higher-priority peer does not preempt |
| M2 | the priority clause alone, `EXITED` clause deleted | 2 arms, and only on the TRACE: `exit_current` requests **two** switches instead of one, the first of them mid-loop |
| M3 | no `dying` clause at all | 3 arms, and only via the arm that rotates the dying thread off its ready-list head. The other two dying arms pass without any guard, because `pick_next` declines an equal-priority peer while the dying thread is still the head |
| M4 | the null-`current` test deleted | SIGSEGV |
| M5 | the deadline cancel moved after the run-state test | 1 arm: a wake racing the timer leaves a stale deadline |
| N1 | the guard compares `base_prio` instead of `prio` | 3 arms. SURVIVED at first: every arm had `prio == base_prio`, so a dying thread's inheritance boost was never seated |
| N2 | the `CAP_WAIT` right test dropped from the endpoint arm | 2 arms. SURVIVED at first: the only sweep arm's cap carried that right |
| N3 | `recv_holders == 0` becomes unconditional | 2 arms. SURVIVED at first: one holder means 1 to 0 is the only transition seen |
| N4 | the EPIPE drain loop becomes a single `if` | 2 arms. SURVIVED at first: one sender was parked |
| N5 | the exit scan matches ANY waiter, not its own joiners | 2 arms |
| N6 | the joiner is woken with no `wait_result` | 2 arms, and only because `park_join` POISONS that field: a zeroed TCB already reads 0 |
| N7 | the mutex force-unlock skipped on teardown | 7 arms |

M3 is the one worth carrying forward as a rule, and N1 is the same lesson a second time. Both
SURVIVED at first, because the guard's effect on a peer it declines is a `pick_next` call avoided
rather than a switch avoided, and only a ready list whose head is NOT the dying thread makes the
decision observable. **A guard that is usually redundant needs an arm that seats the case where it is
not**, or the gate proves the scheduler and not the guard. N2 to N4 are the same disease one layer
down: an arm with one holder, one sender and one right cannot fail on a predicate about holders,
senders or rights.

### 8.4 What this does not discharge

The enumeration is a list a human wrote and the gate makes each entry checkable; a green
`sched_wake` is not a proof that the interleaving is safe, exactly as 4.1 said it would not be. Two
obligations are explicitly still open. The **fault-redirect coupling** is silicon work: more traffic
now flows through the preemptible window between a fault redirect and its stub. And the
**concurrent-teardown path** (`TODO.md`) still has zero in-tree hits; the K-seam fixture is now the
instrument that could seat two sweeps at once, and no arm does yet.

## 9. GoogleTest, and the three rulings it supersedes

The framework question in section 1's item 5 and section 4.3 is **CLOSED, and not the way either
of them left it.** The deferral ("doctest, and adopt when the arm count makes filtering worth the
repo weight") was rejected: the sibling project `/home/leduc/projets/KickCAT` already consumes
GoogleTest through Conan, and KickOS carrying a second framework is a cost with no argument behind
it. So the layer is **GoogleTest, supplied by Conan**, and three earlier rulings go with it.

**"No framework yet" (1.5) is superseded.** doctest was chosen on the house rules and never on
capability, and the capability that decided it in the end was neither filtering nor a diff printer:
it is `EXPECT_DEATH`. Section 1's item 7 ruled that death cases need no fork framework, one
executable per case plus a ctest `PASS_REGULAR_EXPRESSION` being enough. That ruling was true and it
is now obsolete, because a forking death case sits in the SAME binary as the ordinary cases. What it
retires is not one target but a whole belt-and-braces arrangement: `PASS_REGULAR_EXPRESSION` makes
ctest ignore the return code, which is why `kickos_add_kseam_gate()` also carried
`FAIL_REGULAR_EXPRESSION "not ok"` on every gate, panic or not, and why a gate could never print
"not ok" on a passing run. Both properties are gone. The mechanism that replaces them is
`KICKOS_EXPECT_PANIC` in `tests/unit/kfixture/kseam_test.h`, and it needs one thing said out loud:
gtest matches a death test against the FORKED CHILD's stderr, `kpanic` writes stdout, and the fold
therefore happens inside the child, `dup2(STDERR_FILENO, STDOUT_FILENO)`. Folding it the other way
round replaces gtest's own capture pipe and every death case then reports an empty child message,
which reads exactly like a panic that never fired.

**"The right substrate already exists and must not be abstracted" (4.3) is superseded**, and its
reasoning was sound: four gates times twenty lines of `g_failures` + `printf("not ok - ")` is
readable without documentation. What it did not price is that ONE ctest entry per executable makes a
kernel's independent invariants indistinguishable to `ctest -R`. The layer now registers PER CASE
through `gtest_discover_tests` (`kickos_add_unit_test` in `cmake/kickos.cmake`), a deliberate
divergence from KickCAT's single `add_test`: KickCAT is a library with one job.

**"No fetched dependency, ever" (4.3) STANDS, unchanged.** Nothing is fetched at build time, there
is no submodule and no `ExternalProject`. The committed CMake calls `find_package(GTest CONFIG)`
and nothing else, so Conan, vcpkg and a system install all satisfy it; `conan/conanfile.py` is one
way to produce it, not the way the tree depends on. There is no committed Conan profile: `conan
profile detect` covers the host, and the deps are host-only, so a checked-in template would be a
second description of the toolchain that nothing renders and nothing checks.

Three knobs carry the split. `KICKOS_BUILD_TESTS` stays the umbrella and the `tests/static/`
checks stay under it alone, because they need no dependency and they are what catches drift.
`KICKOS_BUILD_UNIT_TESTS` defaults to whether `find_package(GTest QUIET CONFIG)` succeeded and
FATALs if it is forced ON without it. `KICKOS_BUILD_INTEGRATION_TESTS` declines the gates that RUN
an image. **A board build reaches no `find_package(GTest)` at all**: the probe is inside
`if(KICKOS_ARCH STREQUAL "sim")`, so a cross target cannot acquire a dependency-manager
requirement even by accident.

One thing the migration found that section 6.2's `host` label made possible. The label already
means "executes no KickOS image", which is exactly the question
`KICKOS_BUILD_INTEGRATION_TESTS` has to answer, so the knob is keyed on the label rather than on a
second list of gate names. It cannot be keyed on it at `add_test` time, because the label is set by
the line AFTER; a `cmake_language(DEFER CALL ...)` to the end of the registering directory is where
both facts are finally in scope.
