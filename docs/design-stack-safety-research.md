<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Stack safety, kernel-stack footprint, and lessons from other kernels

> **Status: EXPLORATORY.** Sections 1 to 7 are research recorded on 2026-09-17 following
> the M8.9-p4 audit. Section 8 is the M9.0 investigation those sections asked for, run
> 2026-09-22 against the frozen M8.12 baseline, and its verdict is to KEEP the per-thread
> kernel continuation. Recommendations and experiments, not an approved implementation plan
> or a change to the architecture contract. No silicon measurements were taken for either.

## 1. Recommendation and project fit

Keep the static stack-budget check while improving runtime boundary tests. Evaluate
shared per-CPU kernel stacks in M9 as a separate architectural experiment after
freezing the M8.12 performance baseline. The strongest reason for that experiment is RAM saved and
potentially simpler syscall completion, not removal of one test.

KickOS's [design pillars](reference/architecture.md#design-pillars) require a small,
MPU-first microkernel, predictable event-driven scheduling, ordinary application
entry through `main`, a portable arch/chip seam, and a host simulator running the real
kernel. The [roadmap](../roadmap.md) explicitly identifies kernel concurrency as
research and permits a measured conclusion that the existing design should remain.

This is separate from the M8.9 fixes. M9's existing reference-kernel survey is the
natural place to evaluate it alongside big-kernel-lock (BKL) partitioning. Neither
experiment is a prerequisite for adopting the other. Establish the effect of stack
sharing with the lock policy held constant, and the effect of lock partitioning
with the stack model held constant, before evaluating their interaction. This
record assigns no new sub-milestone and commits to no implementation outcome.

The extended survey also supplies a counterexample: Fiasco retains per-thread
kernel stacks and uses their lifetime in its IPC timeout design. A microkernel does
not have to share kernel stacks. This makes the choice an explicit tradeoff between
RAM and saved-operation complexity, not a prerequisite for a capability architecture.

seL4's shared-stack execution model fits several of these goals. Importing its whole
object model does not: KickOS explicitly excludes untyped/Retype, hierarchical
CSpace, and a capability derivation tree. Sharing kernel stacks does not require any
of those mechanisms, nor a new application-facing programming model.

| Goal | Shared per-CPU kernel-stack assessment |
|---|---|
| Small RAM footprint | Strong potential benefit: stack storage scales with CPUs instead of thread slots. |
| Isolation | Keeps privileged execution on trusted memory; sharing alone adds no overflow protection. |
| Predictable IRQ latency | Must be measured. Stack sharing and the interrupt-masking policy are separate choices. |
| IPC performance | Possible reduction in context preservation; no demonstrated speedup. |
| Simple applications | Blocking user APIs can remain unchanged; pending work becomes explicit kernel state. |
| Portability and host sim | Requires a completion contract that works across synchronous and deferred context switches. |
| DRY and maintainability | A common completion path could replace divergent fast/slow paths; permanent duplicate execution models would work against that benefit. |

## 2. What KickOS currently checks

The term "trap red zone" here means headroom reserved for trap processing, not the
x86-64 ABI red zone. Which stack needs that headroom depends on the architecture,
entry class, and configuration. A syscall may transfer to a per-thread kernel stack;
an interrupt may use a per-core trap stack; some configurations retain execution on
the interrupted thread stack. Panic reporting can switch to its own stack.

[`check_trap_redzone.sh`](../tests/static/check_trap_redzone.sh) builds a scratch
configuration with GCC `-fcallgraph-info=su,da`. The analysis combines compiler frame
sizes and call edges into a longest weighted path from declared roots, then compares
the result with architecture budgets and applicable stack capacities. The
[`roots declaration`](../tests/static/trap_redzone_roots.txt) supplies assembly-entry
roots and the information the compiler graph cannot supply on its own.

This is useful regression detection: a deeper call chain can fail the gate even when
ordinary tests run with ample stack space. It is not a universal proof. Its scope
depends on the selected compiler/configuration, complete roots and indirect-call
modelling, correct assembly-frame accounting, and interrupt-nesting assumptions.
The script documents no capacity-fit clause for `stack=trap`; that is a specific
coverage gap worth assessing, not proof that those stacks currently overflow.

The existing runtime evidence answers narrower questions:

- [`trapnest`](../user/apps/common/trapnest/main.cc) parks a low RV32 user SP and
  injects a nested trap. Its counters check that nested handling selects the intended
  interrupt stack. It does not exercise every maximum-depth kernel path.
- [`check_trapnest.sh`](../tests/integration/check_trapnest.sh) checks survival and
  the counters. A passing result is evidence about that entry/nesting scenario.
- [`thread.cc`](../kernel/thread/thread.cc) reserves kernel stack blocks, writes a
  boundary canary, and supports fill-pattern high-water measurements. A damaged
  canary detects an overflow that has already happened. An untouched pattern does
  not prove SP stayed above it: a large allocation can skip the intervening memory.

Three claims must remain separate: a tested workload fits, crossing a boundary is
contained, and every permitted execution fits. They need different evidence.

## 3. What the reference projects actually do

This is a bounded source survey, not an audit of every port or optional execution
mode. In particular, an ordinary RTOS thread stack must not be counted as an extra
privileged stack: sandbox/module/process configurations may add a second stack.
The following source paths are evidence of the inspected mechanism, not a ranking
of projects. No implementation code was copied into KickOS.

| Reference | Observed execution/storage model | Useful stack-safety mechanism or test |
|---|---|---|
| NuttX | Task contexts plus separately configured interrupt stacks on the inspected ARM paths | Function-entry checks or hardware stack limits; software hook excludes interrupt mode. |
| Zephyr | Thread and privileged-stack configurations, plus interrupt stacks | Hardware guard tests deliberately fault on protected boundary writes. |
| FreeRTOS | Per-task saved stack/context in the inspected generic checker | Saved-SP and fill-pattern checks; explicit detection limitations. |
| RIOT | Per-thread saved SP; Cortex-M exception stack handled separately | Scheduler sentinel, optional MPU guards or SPLIM, recursive overflow tests. |
| ChibiOS | Retained per-thread context; sandbox mode adds a privileged stack | Switch-time headroom check or a configurable MPU guard. |
| RTEMS | Per-thread initial stack, plus per-CPU interrupt-stack accounting | Switch extension checks bounds/patterns; tests deliberately corrupt stack ends. |
| RT-Thread | Per-thread stack; Smart mode also tracks a kernel SP | Software SP/sentinel checks, optional MPU guards, sentinel-corruption unit test. |
| ThreadX | Per-thread context; user-mode Modules allocate a kernel stack per thread | Boundary/high-water checks and a corruption/notification regression test. |
| Fiasco | Per-thread kernel stack and saved kernel SP | Optional stack-fill depth diagnostics; IPC timeout ownership depends on the stack. |
| seL4 | Reusable per-CPU kernel stack; blocked work recorded in objects | Simplifies storage and execution states; stack sizing remains necessary. |

### NuttX: instrumentation and hardware limits

The inspected ARMv7-M implementation uses `-finstrument-functions` and `-ffixed-r10`.
A function-entry hook compares SP, with exception-context headroom subtracted,
against the limit in `r10`. This is useful diagnostic coverage for transient depth
that periodic checks can miss. It costs execution time, code space, and a register;
instrumentation also changes the build being measured. It is not automatically a
pre-write memory-containment guarantee.

Crucially, the hook explicitly skips interrupt mode because `r10` is not switched
for that stack. Copying the idea unchanged would not cover KickOS's nested traps.
See [the implementation](https://github.com/apache/nuttx/blob/f0c94bb64acff1471d4ba5b23b1e0e5c8d57019c/arch/arm/src/armv7-m/arm_stackcheck.c)
and the [NuttX guide](https://nuttx.apache.org/docs/latest/guides/armv7m_runtimestackcheck.html).

On supported ARMv8-M configurations NuttX instead uses stack-limit registers. Task
limits are saved in the context, and initialization sets the separate interrupt
stack's limit. This offers a hardware enforcement route, with availability depending
on the CPU and security state. See
[`arm_initialstate.c`](https://github.com/apache/nuttx/blob/f0c94bb64acff1471d4ba5b23b1e0e5c8d57019c/arch/arm/src/armv8-m/arm_initialstate.c).

### Zephyr: test the protection mechanism directly

The local Cortex-M code programs PSPLIM for supported privileged-stack configurations.
The particularly useful testing example is its RISC-V PMP guard test: deliberately
write to the protected thread or interrupt-stack boundary, then require a fatal
fault. This checks enforcement without manufacturing the deepest call chain.
See [the test](https://github.com/zephyrproject-rtos/zephyr/blob/681173c8f627a549a497a3ca4339db1a9fd1c8a2/tests/arch/riscv/pmp/isr-stack-guard/src/main.c).

For KickOS, a guard must be shown to constrain the privilege mode that actually
executes kernel code. An MPU/PMP region that confines user code is not, by itself,
evidence that privileged stack writes are confined. A finite guard region can also
be skipped by a sufficiently large SP adjustment. Boundary-write tests and actual
overflow tests cover different failure modes. Zephyr's
[fatal-error documentation](https://docs.zephyrproject.org/latest/kernel/services/other/fatal.html)
also distinguishes hardware protection from delayed sentinel detection.

### FreeRTOS: inexpensive checks with explicit limits

The inspected `../nuttx/FreeRTOS-LTS/FreeRTOS/FreeRTOS-Kernel/include/stack_macros.h`
checks the saved SP and, in the stronger software mode, boundary fill words. Its
comments explicitly warn that overflow detection is not guaranteed. MPU ports have separate
handling and may rely on
hardware protection. These checks are useful low-cost diagnostics, but are not a
stronger replacement for KickOS's compiled-path budget analysis.

That path is relative to the KickOS repository root. This distribution has no Git
metadata; the source fingerprint is recorded below.

### RIOT: the tailored-stack test already has a close precedent

RIOT stores a saved SP per thread. With `SCHED_TEST_STACK`, the scheduler checks a
sentinel when unscheduling a thread; stack filling supports usage measurement.
Its Cortex-M MPU option programs a 32-byte read-only boundary region for the next
thread and can guard the exception stack. Supported stack-limit configurations set
MSPLIM for exceptions and PSPLIM when restoring a thread.

The most directly applicable test is
[`../../nuttx/RIOT/tests/cpu/mpu_stack_guard/main.c`](../../nuttx/RIOT/tests/cpu/mpu_stack_guard/main.c):
a dedicated thread receives a known stack adjacent to a canary and deliberately
recurses. Two recursive calls prevent simple tail-call elimination. The automated
runner expects intact canary observations followed by a memory-management fault.
The sibling `cortexm_stack_limit` test exercises hardware limits with the same basic
stack/canary arrangement. These are containment experiments, not measurements of
the deepest legitimate application path; the MPU test also includes printing,
which affects its depth.

Other evidence: [`core/thread.c`](../../nuttx/RIOT/core/thread.c),
[`core/sched.c`](../../nuttx/RIOT/core/sched.c),
[`thread_arch.c`](../../nuttx/RIOT/cpu/cortexm_common/thread_arch.c), and
[`vectors_cortexm.c`](../../nuttx/RIOT/cpu/cortexm_common/vectors_cortexm.c).
For KickOS this supports the proposed dedicated-stack/canary test, provided it
constrains the actual kernel or IRQ stack under investigation.

### ChibiOS: price guards explicitly and preserve switch headroom

The inspected ARMv7-M `port_switch` checks PSP minus the upcoming saved-context
size against the outgoing thread's working-area base when software stack checking
is selected. With `PORT_ENABLE_GUARD_PAGES`, the port instead updates an MPU guard
for the incoming thread. Its documentation explicitly accounts for 32 bytes per
thread and consumption of a configurable MPU region. Context-switch assembly saves
and restores the thread's stack; blocking does not discard the call chain.

ChibiOS sandbox mode is a separate comparison: `sbStart` accepts a privileged-stack
area, and dynamic sandbox creation allocates one. Ordinary ChibiOS thread-stack
costs therefore cannot stand in for isolated application costs.

Sources: [`chcore.h`](../../nuttx/ChibiOS/os/common/ports/ARMv7-M/chcore.h),
[`chcoreasm.S`](../../nuttx/ChibiOS/os/common/ports/ARMv7-M/compilers/GCC/chcoreasm.S),
[`chschd.c`](../../nuttx/ChibiOS/os/rt/src/chschd.c), and
[`sbhost.c`](../../nuttx/ChibiOS/os/sb/host/sbhost.c).
The useful lesson is to include the imminent context save in a boundary check and
to expose guard RAM/MPU costs, not assume that a guard is free.

### RTEMS: check task and interrupt stacks, and test the checker

RTEMS's stack-checker extension checks a frame address against stack bounds and
checks a boundary sanity pattern at context switches, with distinct SMP handling.
It also checks the current CPU's interrupt-stack pattern when that stack is
registered, and supports stack-usage reporting. This is broader stack inventory
coverage than a task-only sentinel.

Its `stackchk/blow.c` test deliberately overwrites words at both ends of the current
stack. This verifies the detector without relying on compiler-generated recursive
depth. Such a destructive test needs a dedicated image and a reliable observer.
The stack-checker README explicitly discusses untouched stack holes and the risk of
faulting while printing an overflow diagnostic. Current implementation and tests
take precedence over old enablement instructions in that README.

Sources: [`check.c`](../../nuttx/rtems/cpukit/libmisc/stackchk/check.c),
[`README.md`](../../nuttx/rtems/cpukit/libmisc/stackchk/README.md), and
[`blow.c`](../../nuttx/rtems/testsuites/libtests/stackchk/blow.c).
For KickOS, test canary detection separately from hardware containment and make
trap/IRQ stacks first-class entries in the coverage inventory.

### RT-Thread: separate software diagnostics from hardware guard modes

`rt_scheduler_stack_check()` checks a boundary byte and saved-SP range in its
software mode, offers an overflow callback, and warns near the stack boundary.
The software check is conditional when hardware guards are selected; Smart mode
also has execution-mode-specific handling. The inspected Cortex-M4 guard initializer
reserves no-access regions at both ends, then reduces the usable stack range.
This consumes both space and protection resources.

The unit test deliberately corrupts a thread's sentinel and invokes the checker,
checking the callback result. It is a detector test, not proof that a guard prevents
the corruption. RT-Thread Smart also stores a per-thread kernel SP, so it should
not be generalized as a shared-kernel-stack design.

Sources: [`scheduler_comm.c`](../../nuttx/rt-thread/src/scheduler_comm.c),
[`cpuport.c`](../../nuttx/rt-thread/libcpu/arm/cortex-m4/cpuport.c),
[`thread_overflow_tc.c`](../../nuttx/rt-thread/src/utest/thread_overflow_tc.c), and
[`lwp.c`](../../nuttx/rt-thread/components/lwp/lwp.c).
The useful transfer is a small deterministic checker test plus explicit guard-mode
coverage, without treating a callback as evidence of safe recovery.

### ThreadX: per-thread kernel stacks are also an isolation tradeoff

With stack checking enabled, the generic macro tracks the lowest saved SP, checks
boundary fill words, and invokes an error handler or a deeper stack-pattern
analysis. Ports can override the generic checker. A regression test overwrites a
suspended thread's fill pattern and resumes it to exercise the error notification.

The Modules thread-create path allocates a separate kernel stack when the module
thread runs in user mode. This is a concrete precedent for paying per-thread
privileged-stack RAM to keep execution contexts, rather than sharing one stack
per CPU. It does not establish that the generic task checker covers every module
kernel-stack failure; that would require a separate port/Modules audit.

Sources: [`tx_thread.h`](../../nuttx/threadx/common/inc/tx_thread.h),
[`stack-check regression`](../../nuttx/threadx/test/tx/regression/threadx_thread_stack_checking_test.c),
and [`module thread creation`](../../nuttx/threadx/common_modules/module_manager/src/txm_module_manager_thread_create.c).

### Fiasco: a microkernel with retained kernel continuations

Fiasco's `Context` carries `_kernel_sp`. The inspected RISC-V switch saves the
outgoing kernel SP and resumes the incoming one, including a saved return point.
The context's register frame is placed at the end of its context allocation.
This is materially different from seL4's reusable per-CPU syscall stack.

The IPC timeout code supplies the most useful design evidence: it derives the
owner from the timeout object's address because that object lives on the owner's
kernel stack. Removing retained stacks therefore changes not just SP bookkeeping
but the representation and lifetime of pending operations. KickOS's timeout,
donation, cancellation, and teardown analysis must account for the same category
of dependency, without assuming the implementation is identical.

Optional `Config::Stack_depth` fills unused stack space and exposes depth through
the debugger. This is usage telemetry. The inspected paths do not establish a
universal overflow-containment mechanism or prove the absence of guards elsewhere.

Sources: [`context.cpp`](../../nuttx/fiasco/src/kern/context.cpp),
[`context-riscv.cpp`](../../nuttx/fiasco/src/kern/riscv/context-riscv.cpp),
[`ipc_timeout.cpp`](../../nuttx/fiasco/src/kern/ipc_timeout.cpp),
[`thread.cpp`](../../nuttx/fiasco/src/kern/thread.cpp), and
[`jdb_thread_list.cpp`](../../nuttx/fiasco/src/jdb/jdb_thread_list.cpp).

### seL4: simplify the execution model

The inspected seL4 allocates one kernel stack per CPU, with a default size of 4 KiB.
Its RISC-V trap entry saves the user context and selects the kernel stack before C
dispatch. Blocking IPC records state in the TCB and queues the thread; it does not
retain a suspended C call chain for that thread. Explicit preemption points can
return `EXCEPTION_PREEMPTED`, allowing the call chain to unwind.

Sources at the inspected revision:
[allocation](https://github.com/seL4/seL4/blob/28b8f4c40d4a48a206bedc7875c6695f6106f34b/src/kernel/stack.c),
[RISC-V entry](https://github.com/seL4/seL4/blob/28b8f4c40d4a48a206bedc7875c6695f6106f34b/src/arch/riscv/traps.S),
[IPC blocking](https://github.com/seL4/seL4/blob/28b8f4c40d4a48a206bedc7875c6695f6106f34b/src/object/endpoint.c),
[preemption](https://github.com/seL4/seL4/blob/28b8f4c40d4a48a206bedc7875c6695f6106f34b/src/model/preemption.c).

This reduces the number of stacks and the live execution states to reason about.
It does not establish a universally guarded kernel stack: `KernelStackBits` in
[`config.cmake`](https://github.com/seL4/seL4/blob/28b8f4c40d4a48a206bedc7875c6695f6106f34b/config.cmake)
warns that an undersized stack can corrupt memory. Architecture-specific mapping
code must still be examined; a guard on one port cannot be generalized to all ports.
Nor does adopting this architecture transfer seL4's proofs to KickOS. Proof coverage
depends on the actual verified configuration and assumptions, as stated in the
[seL4 verification overview](https://sel4.systems/Verification/proofs.html).

Linux's [VMAP_STACK documentation](https://docs.kernel.org/mm/vmalloced-kernel-stacks.html)
provides another relevant MMU-side example: guard pages and dedicated overflow
tests, with architecture-specific fault handling. It was documentation research,
not a local Linux source audit, and is not an MCU-wide solution.

## 4. Improvements that do not require a kernel rewrite

1. **Make stack coverage explicit.** Inventory thread, kernel, trap/IRQ, and panic
   stacks per configuration. For each, record capacity, entry-frame cost, nesting
   allowance, applicable guard, and evidence. Extend existing declarations where
   possible instead of maintaining a second list of roots or capacities.
2. **Add tailored-stack positive tests.** Exercise real receive, reply, fused
   reply-receive, error, timeout, and wake paths on the stack that actually hosts
   kernel execution. Lowering only user SP does not constrain a separate kernel
   stack. Include a valid minimum-headroom case and a below-threshold rejection
   case where the entry contract defines such a threshold.
3. **Add negative containment tests.** Deliberately access a guard and separately
   induce stack exhaustion. Require the expected fault reason, correct offender
   attribution, and intact adjacent memory where recoverable inspection is possible.
   Kernel-stack exhaustion may require system fail-stop rather than thread recovery.
   Report from a known-safe stack; a fault handler relying on the exhausted stack
   cannot be the sole observer.
4. **Inject nesting deterministically.** Trigger the supported interrupt sequence
   at a deliberately deep point. Check each active stack separately, including
   hardware exception frames and floating-point state where applicable. A periodic
   tick under load does not guarantee the intended interleaving occurred.
5. **Evaluate hardware limits per port.** Consider ARM stack-limit registers and
   MPU/MMU/PMP guards only after checking privileged enforcement, region cost,
   context-switch updates, and fault-entry behaviour. Do not advertise protection
   on ports where the hardware cannot provide it.
6. **Keep instrumentation diagnostic.** A NuttX-style SP-check build can find
   unexpected depth, but needs explicit interrupt coverage and an assembly/early
   entry story. Retain production-build budget checks and measurements because the
   instrumented binary has different costs.
7. **Test the existing detector independently.** Use a dedicated kernel-stack block
   or fixture to corrupt the canary and verify detection and reporting, following
   the test shape found in RTEMS, RT-Thread, and ThreadX. This small test validates
   the observer. It must not be presented as either a capacity or containment test.

RIOT provides the closest inspected precedent for the requested tailored-stack
overflow test. Fiasco and ThreadX Modules provide reasons to retain kernel
continuations when their lifetime simplifies blocking and isolation. These findings
strengthen the case for comparing both designs, rather than assuming seL4 is the
target architecture before the experiment begins.

An additional static-analysis improvement is to check capacity for declared
`stack=trap` classes and report exactly which configuration and call path set the
bound. Missing roots, unresolved edges, and unsupported dynamic-stack cases should
remain visible rather than silently becoming zero cost. Inspect the current
analyzer before proposing new checks: this is a review target, not a claim that all
of those omissions exist today.

## 5. Shared-stack experiment: what would make it worthwhile?

Current kernel stack storage is approximately `T * K`, where T is reserved thread
slots and K is the per-thread block size. A shared design would cost approximately
`C * S + T * D + E`, where C is CPU count, S is the independently sized shared stack,
D is added saved-operation state per slot, and E covers additional support storage.
IRQ/panic stacks, alignment, and unchanged user stacks must be counted consistently
in both totals. Stack sharing does not remove user stacks.

The inspected [Kconfig](../Kconfig) defaults include 1,008-byte ordinary ARMv7-M
kernel blocks, 1,184-byte RV32 blocks, and 4,096-byte blocks on several 64-bit ports.
These are configuration values, not newly measured maxima. The saving cannot be
claimed as `(T - C) * K` until S, D, and E are established.

KickOS already has a limited example of blocking without a retained kernel call
chain: [`Thread::call_frame_parked`](../kernel/include/kickos/thread.h) marks IPC
fastpath callers whose results must be written into their saved frame. This is a
starting point for investigation, not evidence that every wait can already use the
same completion mechanism.

The experiment should answer these questions:

- Can blocking operations return an explicit outcome to a common dispatch epilogue,
  with wakeup completing the saved user context exactly once?
- What must survive a block: output pointers, copy progress, deadlines, reply
  authority, notification results, donation state, and cancellation ownership?
- Can timeout, reply, cancellation, and thread death race without double completion,
  lost wakeups, use-after-free, or retained authority?
- What changes for native kernel threads, forced teardown, and synchronous host/LX6
  switching compared with deferred-switch backends?
- Can IRQ nesting and per-CPU stack ownership remain bounded while cores contend
  for the kernel lock or a runnable thread migrates?
- Does unifying completion remove more code and state than explicit continuations
  add, including all error paths and simulator support?

Start with an isolated implementation of one blocking operation on a representative
constrained port plus the host simulator. That stage can establish correctness but
cannot claim the full RAM saving while other operations still need per-thread
kernel stacks. Test timeout/cancellation/death and scheduling donation before
generalizing. Assess a deferred-switch backend and SMP before proposing fleet-wide
adoption. Keep user APIs stable so comparisons exercise the same workload.

Freeze compiler, configuration, workload, and measurement boundaries. Compare total
RAM and flash, usable thread count, stack headroom, IPC round-trip p50/p99/max,
IRQ-to-user p50/p99/max, maximum IRQ-disabled intervals, and lock-wait costs under
contention. Set acceptable regression budgets before collecting results; observed
maxima are not a formal worst-case bound.

Adopt only if the measured footprint or structural simplification pays for its
latency, portability, and maintenance costs. A result that retains per-thread stacks
is valid. Do not make a permanent second kernel execution model merely to preserve
an inconclusive prototype.

### 5.1. Independent implementation and the worker handoff

Use a separate implementation worker for the M9 prototype. The researcher may
inspect reference kernels; the worker receives a reviewed behavioural specification
and KickOS's own source and tests, without access to the reference implementations.
This separation strengthens the evidence of independent implementation. It does
not make a specification derived by translating someone else's code acceptable.

The process is:

1. **Research and specification.** Record the desired behaviour, observable results,
   invariants, resource constraints, and acceptance tests in terms of KickOS's own
   contracts. For the candidate above, specify exactly-once completion across reply,
   timeout, cancellation and death; when a CPU stack becomes reusable; and the
   expected outcomes of boundary tests. Describe the properties to implement, not
   the reference kernel's function bodies or internal control flow.
2. **Review the handoff.** Remove reference-source excerpts, translated code,
   implementation-shaped pseudocode, copied comments, and copied test bodies. Keep
   the source survey and its provenance with the researcher. Do not give the worker
   this entire report or the source-inspection transcript as its implementation
   brief. Provide a separate, self-contained requirements document. Attribution of
   an architectural idea is appropriate; it is not permission to copy its code.
3. **Implement in a fresh context.** An agent worker must start without inherited
   source excerpts, conversation history, or summaries that reconstruct reference
   implementations. It may inspect KickOS, including `call_frame_parked`, but must
   not open the reference repositories, their linked source pages, or retrieve their
   implementation through search or another agent. If reference-source exposure
   occurs, record it and stop claiming that worker is source-blind; use a fresh,
   unexposed worker if continuing under this separation process.
4. **Review and preserve provenance.** Record the reviewed specification revision,
   KickOS baseline, worker context restrictions, implementation commits, and test
   results. Review against the stated contracts and check for unexplained similarity
   to reference code. Feedback to the worker should express requirements or defects
   in its implementation, not supply a reference implementation to imitate.

The current research session has inspected third-party source, so it cannot be the
source-blind implementation context. A fresh agent context is a practical process
boundary; it is not proof that a model has never encountered public code during
training, nor an automatic legal guarantee.

This follows KickOS's
[licensing and clean-room discipline](reference/architecture.md#licensing--clean-room-discipline-hard-constraint):
study designs and write original code under CeCILL-C, with attributed inspiration
and no lifted implementation. EU Directive 2009/24/EC, Article 1(2), distinguishes
protected program expression from underlying ideas and principles; it supports the
distinction between independent implementation of a mechanism and reproduction of
its expression. [Directive text](https://eur-lex.europa.eu/legal-content/EN/ALL/?uri=CELEX%3A32009L0024)
This is the intended development process, not legal clearance of a future patch.
Any proposed reuse of third-party code would need a separate licence review and
reconsideration of the project's no-copying policy.

## 6. M8.9 audit findings that must not be lost

These are historical findings against committed M8.9-p4
`eefa886bf6d52897b7aff93dbdb8bc7b5adf2e32`, compared with M8.8
`208c6998fa58c4d496a1ae58625705fa82125d25`. Later uncommitted branch edits were visible
when this report was written; their fix status was not re-audited. Original line
numbers below refer to the audited commit, obtainable with `git show <commit>:<path>`.

| Priority | Finding and reproducer | Improvement |
|---|---|---|
| P1 | `kernel/syscall/syscall_ipc.cc:67`: deferred wake retains only the highest-priority thread. A fused reply to a core-0 client plus receive from a lower-priority core-1 sender makes both READY but can omit core 1's reschedule request. A lower-priority FIFO thread can keep that core indefinitely. | Preserve announcements for every readied thread's eligible cores while deferring local scheduling; test multiple wakes with disjoint affinities. |
| P2 | `kernel/syscall/cap.cc:374`: closing a SIGNAL-only IRQ alias clears an existing notification binding despite a live WAIT alias. Rebinding through that WAIT handle restores delivery. | Define binding authority ownership; handle multiple WAIT aliases as well as SIGNAL-only closure, and preserve teardown cleanup. |
| P2 | `kernel/syscall/syscall_ipc.cc:1031`: reply-half EBADF/EFAULT returns before writing the consumed notification mask. A valid input mask of 1 with no pending events remains 1 and can be mistaken for a delivered event. | Define output validity on every error path; write zero consumed events where validated writable opts permit it, and align consumers with the contract. |
| P2 | `kernel/syscall/syscall_ipc.cc:915`: standalone reply timing ends before the deferred-wake destructor schedules. The earlier baseline included that work. | Restore comparable span boundaries or explicitly redefine the metric before claiming improvement. This was source-order analysis, not a measured cycle saving. |

Three defects were reproduced with four scratch probes using the real IPC,
capability, IRQ, and two-core scheduler code. Address validation/copy seams were
stubbed for owned host buffers, so this was not a hardware isolation test. Controls
confirmed the missing wake announcement, recoverable binding, and zero-mask case.

The recorded stock validation passed all 672 registered host/tree tests across the
initial run and configuration-environment rerun, plus Cortex-M selftest, AArch64 SMP
selftest, SMP arrival, and SMP threads gates. Passing existing tests did not refute
the targeted reproductions. No new privilege escalation or cross-task disclosure
was established, and no silicon performance campaign was run.

Two smaller DRY opportunities were duplicated reply-capability admission/minting and
service loops manually initializing options despite an available initializer.
Prefer shared mechanics without reintroducing a removed lookup on a hot path.

The scratch audit report, probe source, build script, and output were in
`/tmp/m89-audit/`. They are session artifacts, not durable repository test fixtures;
this record preserves the scenarios but does not claim a portable replay package.
Future fixes should encode the scenarios in permanent tests. These audit defects
are independent of whether shared kernel stacks are adopted.

## 7. Evidence provenance and limits

| Tree | Inspected revision or identity | Inspected code licence |
|---|---|---|
| KickOS M8.9 audit | `eefa886bf6d52897b7aff93dbdb8bc7b5adf2e32` | CeCILL-C |
| KickOS main checkout where this report was added | `208c6998fa58c4d496a1ae58625705fa82125d25` | CeCILL-C |
| `../nuttx/nuttx` | `f0c94bb64acff1471d4ba5b23b1e0e5c8d57019c` | BSD-3-Clause ARMv7-M hook; Apache-2.0 ARMv8-M initializer |
| `../nuttx/zephyr` | `681173c8f627a549a497a3ca4339db1a9fd1c8a2` | Apache-2.0 |
| `../nuttx/seL4` | `28b8f4c40d4a48a206bedc7875c6695f6106f34b` | GPL-2.0-only kernel files |
| FreeRTOS local distribution | `task.h` reports `V11.3.0`; no Git metadata | MIT |
| `../nuttx/RIOT` | `92715ebf9791bcb7e726c067d1506fedc92c4a22` | LGPL-2.1-only core/test headers |
| `../nuttx/ChibiOS` | Local SVN tree; `ch.h` reports `8.0.0`; file hashes below | GPL-3.0 ARMv7-M port header |
| `../nuttx/rtems` | `f35edbdb95db2cce2d273ebb5ae994e31c338969` | BSD-2-Clause stack checker |
| `../nuttx/rt-thread` | `7fa651bd298ddd6cc99d60043f6472f196608825` | Apache-2.0 scheduler |
| `../nuttx/threadx` | `44d7c95c582d415c4ad84527180b29c93c3bf664` | MIT |
| `../nuttx/fiasco` | `6437d44e5fe2cfeef5209b554ff65720cb255776` | MIT core package per `LICENSE.spdx`; dependencies differ |

The reference Git checkouts had no tracked modifications when recorded. ChibiOS
was read without touching its SVN metadata; the `svn` client was unavailable, so
no revision or cleanliness assertion is made for that tree. Licence entries identify
the consulted code, not a claim that every file in a distribution has one licence.
The survey extracts design and test strategies, not implementation text.

FreeRTOS `../nuttx/FreeRTOS-LTS/FreeRTOS/FreeRTOS-Kernel/include/stack_macros.h`
SHA-256 is `7fe59a59cddf9ed4af9da3904385125636c76adff9897631c4b55139d71f87e0`.

ChibiOS source SHA-256 fingerprints:

| Path within ChibiOS | SHA-256 |
|---|---|
| `os/common/ports/ARMv7-M/chcore.h` | `fa133dfae9ef5916aece7896b07f2a611a654a735207350b1a56816d6a5fc973` |
| `os/common/ports/ARMv7-M/compilers/GCC/chcoreasm.S` | `9f0c7c6f677ed8c9dbf88f934d865d8eac70dc5a98eb95c39687ce8629d5bf2d` |
| `os/rt/src/chschd.c` | `3295a1d001a227a013f9ef19d3f1d662242dd6280ae408dac265072a16a840f8` |
| `os/sb/host/sbhost.c` | `0c42d24cf944fa3ce49c5a2755a6d6896499a19fdaf973fbafef60c958b4447a` |

External documentation links describe mechanisms,
not a certification of the local build. Relative KickOS links resolve in the reader's
checkout; use the recorded revisions when investigating a historical finding.

No reference kernel was built or benchmarked for this comparison. No common workload
comparison, shared-stack prototype, new hardware guard, or new stack test was
implemented during this research. Proposed gains and tests remain hypotheses until
the experiments above supply evidence.

## 8. The M9.0 investigation: shared per-CPU stacks against per-thread continuations

Run 2026-09-22 as M9.0's second deliverable, against this record and read-only throughout:
no build, no test, no bench and no target. Sections 1 through 7 are the 2026-09-17 input and
are unchanged. **This is an investigation and not approval to rewrite the kernel**; keeping
the current design is a valid outcome and is the one the evidence gathered here supports.
### Verdict

Keep the per-thread kernel continuation. The RAM the experiment is run for is real but small and
lands on one board that already pays it deliberately, while the change would replace a completion
mechanism that four in-tree pieces depend on -- the resume barrier, the timeout deadline, the
cancellation edge and the death-path stack relocation -- with one the tree has built exactly once,
for a single untimed operation, on four of nine backends. Nothing measured here says the shared
model is worse; what the tree says is that its price is paid in the seam contract and its return is
bounded by arithmetic that can be done without building anything. This document assigns no
sub-milestone and proposes no implementation.

### Method: the lock policy is held constant throughout

Everything below is stated with `KICKOS_KERNEL_CORES`, the big kernel lock and the current
exclusion discipline UNCHANGED. Where a statement would change under lock partitioning it is marked
and deferred to the last section, which is the only place the two questions are combined. The
reason is attribution: a stack-model change and a lock-partition change both move IPC latency, and
a campaign that lands them together can attribute neither.

Two facts make the separation clean in this tree. First, the per-thread block is a property of the
trap ENTRY and not of the scheduler: `KICKOS_KERNEL_STACKS` is resolved from the chip capability
(`Kconfig:764-800`), and nothing in `kernel/sched/sched.cc` reads it. Second, the lock is taken
inside the dispatch, after the entry has already chosen a stack. So the stack model determines what
memory the dispatch stands on and the lock policy determines who may run it; the two are composable
and are treated that way here.

### What the tree does today

#### The block, and who carves it

The per-thread kernel stacks are one array in kernel `.bss`, `KICKOS_THREAD_SLOTS` blocks of
`KICKOS_KERNEL_STACK_SIZE` (`kernel/thread/thread.cc:56-62`), with `KICKOS_THREAD_SLOTS` being
`KICKOS_MAX_THREADS + 1` (`kernel/include/kickos/config/system.h:42`). The lowest word of each block
is an overflow canary and the rest is laid with a fill pattern at init
(`kernel/thread/thread.cc:38-51`); `thread_create` seats `ctx.kernel_sp` at the block top before
`arch_context_init` runs, because a backend may build the thread's privileged return state there
(`kernel/thread/thread.cc:306-329`). The banner prints slot size, slot count and their product
(`kernel/init/kmain.cc:146-149`).

Whether a board carves them at all is `KICKOS_KERNEL_STACKS` (`Kconfig:764-800`), which resolves
from the ARCH capability `ARCH_HAS_KERNEL_STACKS` AND the chip's `HAS_MPU`, with
`ARCH_KERNEL_STACKS_MANDATORY` overriding both (`arch/Kconfig:13-43`).

#### The entry classes, by arch

| arch | selects | syscall from unprivileged | interrupt / nested | death path | panic |
| --- | --- | --- | --- | --- | --- |
| rv32imac | HAS + MANDATORY (`arch/Kconfig:110-115`) | SYS, on the thread's kernel block | TRAP on the block; every other M-mode cause is NESTED on the per-hart trap stack | EXITK on the block, RET on the thread stack | own array |
| rxv3 | HAS + MANDATORY (`arch/Kconfig:103-108`) | SYSK on the block; SYS/SYS_FAST unmarked | PENDSW unmarked | EXITK on the block | own array |
| armv6m | HAS + MANDATORY (`arch/Kconfig:82-87`) | SVCK on the block | PENDSV in handler mode on the MSP, depth 0 | EXITK on the block | own array |
| armv7m | HAS only (`arch/Kconfig:89-93`) | SVCK on the block at `KICKOS_KERNEL_STACKS=1`, SVC on the caller's own PSP at 0 | PENDSV in handler mode on the MSP, depth 0 | EXITK on the block, EXIT on the thread stack at 0 | own array |
| armv8a, rv64imac, x86_64 | HAS + MANDATORY (`arch/Kconfig:97-101`, `:120-123`, `:132-136`) | hardware gives the kernel a stack pointer; block mandatory | rv64 has a per-hart trap stack (`arch/riscv/rv64imac/include/kickos/arch/rv64_frame.h:77-90`) | -- | provisional, unbacked |
| lx6 | neither | one privilege level: the dispatch is a plain call on the caller's own stack (`arch/xtensa/lx6/arch_xtensa.cc:926-940`) | PREEMPT class | own array |
| sim | neither | a direct call on the caller's host stack (`arch/sim/sim.cc:1517-1545`) | -- | `KICKOS_PANIC_STACK_SIZE` is 0 here alone |

The class declarations are `tests/static/trap_redzone_roots.txt:114-122` (rv32imac), `:227-236`
(rxv3), `:327-356` (armv7m), `:402-415` (armv6m), `:473-474` (lx6). The armv7m section
(`:271-285`) states the one arch that compiles both entry designs and why. Three FAMILIES resolve 0 and
rest a blocking syscall's continuation on the USER stack, by two different routes. Six armv7m
presets -- `f302nucleo`, `f302nucleo-st`, `due`, `due-st`, `bluepill-c8`, `bluepill-c8-st` --
because their chips select no `HAS_MPU` while armv7m does not make the blocks mandatory
(`tests/static/trap_redzone_roots.txt:343-346`). Every LX6 preset, because `ARCH_LX6` selects
neither capability. And every `sim` preset, which selects `HAS_MPU` and still resolves 0 because
`ARCH_SIM` does not select `ARCH_HAS_KERNEL_STACKS` -- so "the chip has no MPU" is one of two
routes to that answer rather than the rule.

So there are already THREE stack classes in the fleet, not two: the per-thread block, a per-core
trap or handler stack (the rv32 per-hart `g_rv_trap_stack` at
`arch/riscv/rv32imac/arch_rv32imac.cc:167`, the rv64 per-hart row, the ARM MSP), and a per-core
panic array. The per-CPU idea is not new to the tree; what is per-thread is the SYSCALL dispatch
alone.

#### What a blocked operation costs today

The seam requires the dispatch to run in privileged THREAD context on the caller's own continuation,
with blocking calls resuming inline after a context switch (`arch/include/kickos/arch/arch.h:660-668`).
`docs/reference/porting.md:1925-2000` derives the armv7m realisation: SVC rewrites the stacked PC to
a trampoline, exception-returns to privileged thread mode, relocates `sp` onto `ctx.kernel_sp` and
calls `syscall_dispatch` there; PendSV then freezes the entire mid-dispatch continuation on that
block and resumes it inline. `arch/arm/armv7m/switch.S:5-8` says the same at the code.

A block is therefore: `wq_block` sets state, detaches, links and reschedules
(`kernel/sync/sync.cc:76-93`); `arch_switch` either swaps immediately or pends
(`arch/include/kickos/arch/arch.h:197-200`, ARM always pends at
`arch/arm/common/arch_arm_common.cc:97-104`); the waker writes `wait_result` and the resumed thread
reads it after `wq_confirm_resume` (`kernel/include/kickos/sync.h:74-84`,
`kernel/sync/sync.cc:58-72`). The pending operation's OUTPUT state is already explicit -- `ipc.buf`,
`ipc.len`, `badge_out`, `call_state`, `wait_result`, `wait_kind`, `wait_obj`, `deadline_ns`. What is
NOT explicit is the operation's remaining CONTROL FLOW, which is the C frames on the block.

The tree has exactly one operation that already blocks with no kernel continuation: the register IPC
fastpath. It parks the caller with `call_frame_parked` set, points `ipc.buf` at the caller's saved
register frame, and lets the switch that resumes the thread write the result into the frame slot the
restore pops (`kernel/syscall/syscall_ipc_fast.cc:160-185`, completion at
`kernel/sched/sched.cc:133-141`, seam call `arch_ctx_set_syscall_result` at
`arch/include/kickos/arch/arch.h:692-695`). That one operation needed a TCB flag
(`kernel/include/kickos/thread.h:252`), a second reader to reinterpret `ipc.buf` as kernel storage
(`kernel/syscall/syscall_mem.cc:370-378`), an explicit exclusion from the multicore contract
(`kernel/syscall/syscall_ipc_fast.cc:180-184`), and it carries NO deadline at all
(`kernel/syscall/syscall_ipc_fast.cc:186`). It exists on four backends: armv7m, armv6m, rv32imac,
rxv3 (`arch/*/ipc_fastpath.cmake`). The sim and lx6 decline it for the same stated reason -- a
caller's continuation there is a host return address on its own stack, not a saved register frame a
reply can land in (`arch/sim/sim.cc:1517-1521`, `arch/xtensa/lx6/arch_xtensa.cc:926-935`).

That is the honest scale marker for the whole proposal: one untimed operation, on four of nine
backends, is what the explicit-saved-state style has cost so far.

### Per-axis assessment

#### RAM and flash footprint

MEASURED: nothing new. No build was run and none may be. The per-slot figures below are Kconfig
constants (`Kconfig:807-823`) resolved by the generated board config, not link output.

DERIVED: the arithmetic in the next section. The current cost is `T * K`; the shared cost is
`C * S + T * D + E` with C the kernel core count, S the shared stack, D the added saved-operation
state per slot and E support storage. On every arch whose block exists, S cannot be smaller than the
deepest transferred dispatch, because the same dispatch has to fit: on armv6m the block is 896 and
the enforced requirement is `NEST_SVCK` 84 plus `KERNEL_DEPTH_SVCK` 808, which is 892 plus the
canary word (`arch/arm/armv6m/include/kickos/arch/armv6m_trap_stack.h:87,121`). So S is
approximately K, and the saving is bounded above by `(T - C) * K` less `T * D` less E.

DERIVED, and it is the axis's real finding: on every board in the fleet that CARVES blocks and is
real silicon, `KICKOS_KERNEL_CORES` is 1. It is NOT true that the shared model runs only under
emulation -- `boards/esp32-wroom/configs/smp` sets `KICKOS_CORES_TWO` with no AMP posture, so it
takes the shared model on silicon -- but that board is LX6 and carves no blocks at all, so it never
reaches this axis. Every board that both carves blocks and runs on silicon schedules one core. So C
is 1 wherever the RAM argument is strongest, which is the best case for the shared model and is why
the arithmetic below is the whole of this axis.

UNKNOWN: D and E. Nothing in the tree bounds the per-slot saved-operation state a general
continuation-free design would need, because no such design exists here. The fastpath's D is one bit
plus a reinterpretation of fields the TCB already had, but that operation has no deadline, no
multi-stage copy, no reply authority transfer and no cancellation ownership. D for a general design
is not derivable from the fastpath and must not be estimated from it.

UNKNOWN: flash. No figure in the tree says what a common completion epilogue would add or remove in
text. `docs/design-flash-footprint.md` and the archived footprint records predate every part of this
question. A flash claim in either direction would be invented.

#### IRQ latency

MEASURED: the M8.12 baseline has no IRQ-to-user row. The frozen table is `switch`, `lock-hold`,
`rt8`, `rt256`, `e2e-local`, and on four-core presets `lock-wait`, `doorbell`, `e2e-cross`
(`docs/archive/M8.12_meas.md:31-71`). So there is no baseline to regress against on this axis.

DERIVED: the stack model is already OFF the interrupt path on the ARM arches, where PendSV and every
device ISR run in handler mode on the MSP and the PENDSV class measures depth 0
(`tests/static/trap_redzone_roots.txt:327-328,357`). On rv32imac an interrupt taken from U-mode runs
its dispatch on the interrupted thread's kernel block (TRAP, `stack=kernel`) and every other M-mode
cause goes to the per-hart trap stack (`tests/static/trap_redzone_roots.txt:77-82,114,119`). Under a
shared per-CPU stack the rv32 TRAP class would move from the block to the shared stack, which is a
change of WHICH array is touched and not of how much work the entry does. Interrupt masking policy
is a separate choice and is untouched by either model, exactly as the 2026-09-17 record states.

UNKNOWN: whether moving the rv32 TRAP class onto a shared stack changes cache behaviour on the
ESP32-C6. The board has a bench; nothing has measured it.

#### IPC latency

MEASURED: `rt8`, `rt256` and `e2e-local` at the frozen baseline (`docs/archive/M8.12_meas.md:31-71`),
plus the silicon half on `f411disco`, `esp32c6-wroom` and `xmc4800-relax`
(`docs/archive/M8.12_meas.md:75-79`).

DERIVED: the shared model's plausible saving on the IPC path is the context the switch must preserve.
Today a blocking call's continuation is frozen on the block and restored, which on armv8a is what the
4096-byte provisional block is justified by -- the uniform exception frame is 800 bytes and a blocking
syscall holds two at once (`Kconfig:812-814`). A continuation-free design would carry one. That is a
RAM statement, not a latency one: the frames are saved by hardware or by the entry either way.

DERIVED: the completion side gets a new cost the current design does not pay. Today the resumed
thread reads `wait_result` off its own restored frame. Under explicit saved state the waker must
write into the sleeper's saved register frame, which is the fastpath's mechanism and which
`kernel/sched/sched.cc:133-141` performs at seat time, in the switch path, under the lock.

UNKNOWN: whether either effect is measurable. The 2026-09-17 record's "possible reduction in context
preservation; no demonstrated speedup" still stands, and nothing found here demonstrates one.

#### Timeout and cancellation

MEASURED: nothing. This axis has no instrument.

DERIVED, and it is the strongest argument for keeping the current model. Today a timeout is a
deadline field plus a sleep-queue entry, and the timer wake delivers `-KOS_ETIMEDOUT` through
`wait_result` into a continuation that then runs the rest of the operation's own error path
(`kernel/syscall/syscall_ipc.cc:305-316`, `kernel/time/time.cc:147-176`). Cancellation is checked
under `IrqLock` ahead of every side effect and exits from the prologue, deliberately NOT at the park,
because a transaction may already be committed (`kernel/include/kickos/sync.h:66-72`). Both work
because the operation's remaining control flow still exists as frames. Remove the frames and every
partially committed operation needs an explicit "what is left to undo, and who owns the undoing"
record. The 2026-09-17 record already names this as the Fiasco lesson; the Fiasco evidence is in the
reference section below and it is stronger than the record states.

DERIVED: the one in-tree operation that already blocks without a continuation carries NO deadline
(`kernel/syscall/syscall_ipc_fast.cc:186`). The existing precedent stops exactly where this axis
starts.

UNKNOWN: whether exactly-once completion across reply, timeout, cancellation and death can be had
without a per-slot completion token. It is the question the 2026-09-17 record poses and no work since
has touched it.

#### Teardown

MEASURED: nothing new. `docs/archive/M4.7.9_teardown_latency_meas.md` exists and predates the
per-thread blocks.

DERIVED: teardown currently uses the block's existence as its landing site. `kickos_fault_stack_top`
answers with `ctx.kernel_sp` -- the block TOP, not where the dispatch left off -- so the exit stub
DISCARDS whatever dispatch frames the block still holds, which is sound because nothing resumes a
dying thread, and which is what makes the block requirement the MAX of the dispatch class and the
exit class rather than their SUM (`kernel/init/fault.cc:195-223`). That is a per-thread property. A
shared per-CPU stack has no "this thread's top" to relocate to, and a dying thread's teardown is
preemptible: `cap_teardown` drops and retakes `IrqLock` between chunks
(`kernel/sched/sched.cc:590-594`), so the teardown itself can be switched away from and must survive
on something. Under the current model that something is the dying thread's own block. Under a shared
model it is an open design question, and it is the axis where the change is least contained.

UNKNOWN: whether the max-not-sum property survives. If teardown under a shared stack had to coexist
with a dispatch rather than replace it, S would have to hold both, which would eat part of the RAM
the change was made for. Nothing in the tree bounds that.

#### The host simulator

MEASURED: nothing. The sim is a correctness instrument, not a timing one.

DERIVED: the sim runs the real kernel (`docs/reference/architecture.md:33-34`) with `ucontext`
coroutines and a synchronous `swapcontext` (`arch/sim/sim.cc:1076-1093`), a syscall that is a direct
call on the caller's own host stack with an emulated privilege raise tracked PER CONTEXT so it
survives a blocking switch (`arch/sim/sim.cc:1517-1545`), and a 64 KiB host stack substituted under
any MCU-sized figure (`arch/sim/sim.cc:1003-1011`). It carves no kernel block at all. So the sim is
the backend a shared-stack completion contract would be HARDEST to model on, not the easiest: there
is no saved register frame for a reply to land in, which is the stated reason it ships no
`ipc_fastpath.cmake` (`arch/sim/sim.cc:1517-1521`). Whatever completion contract a shared design
defines, the sim has to implement it, and the mechanism the tree has today is one the sim explicitly
cannot use.

UNKNOWN: what a sim-implementable completion mechanism looks like. Not derivable from the tree.

#### The arch/chip seam

MEASURED: nothing; this is a contract, not a number.

DERIVED: the seam would have to change, and the change is the one the roadmap once called a core
restructure. `arch.h` currently REQUIRES dispatch on the caller's continuation with blocking calls
resuming inline (`arch/include/kickos/arch/arch.h:660-668`), `porting.md` quotes that as the
portability-critical contract and records the M1 ruling that the fallback -- deferred syscall
completion -- is a core restructure, followed by "It is feasible" for the continuation design
(`docs/reference/porting.md:1925-1945`). Two capability symbols and their whole help texts are built
on the per-thread shape (`arch/Kconfig:13-43`, restated as what a new arch owes at
`docs/reference/porting.md:324-355`).

DERIVED: the seam already spans two switch disciplines and the completion contract must cover both.
`arch_switch` may switch immediately or at exception return and the scheduler must allow deferred
completion (`arch/include/kickos/arch/arch.h:197-200`). armv7m, armv6m, rv32imac and rxv3 PEND; arm64,
rv64, x86_64, lx6 and the sim switch inline inside `wq_block` (`TODO.md:4767-4771`). The resume
barrier exists only because of that split and is a no-op on the inline backends
(`kernel/include/kickos/sync.h:74-84`). A shared-stack design needs a completion rule that is correct
on both halves, which is precisely what the 2026-09-17 goal-fit table calls out and what the fastpath
never had to solve, having been built only for the pending half.

UNKNOWN: whether a single completion rule covers both disciplines without a second execution model.
The 2026-09-17 record's own warning against a permanent duplicate execution model applies directly.

### The RAM arithmetic

Every figure below is a Kconfig-resolved constant read out of `Kconfig` and the board defconfigs, not
a link measurement. `K` is `KICKOS_KERNEL_STACK_SIZE` (`Kconfig:807-823`), `T` is
`KICKOS_MAX_THREADS + 1` (`kernel/include/kickos/config/system.h:42`), `C` is `KICKOS_KERNEL_CORES`
(`Kconfig:118-126`). Under the shared model the per-CPU stack S is taken as approximately K, for the
reason given in the RAM axis above; D and E are UNKNOWN and are not filled in.

| board / variant | arch | K | T | `T * K` | C | ceiling saving `(T - C) * K` |
| --- | --- | --- | --- | --- | --- | --- |
| microbit/base | armv6m | 896 | 5 | 4,480 | 1 | 3,584 |
| picopi/base | armv6m | 896 | 17 | 15,232 | 1 | 14,336 |
| blackpill, f411disco, xmc4800-relax, frdmk64f, base AND `-st` | armv7m | 1,008 | 9 | 9,072 | 1 | 8,064 |
| qemu/base, teensy41/base, frdmk64f/flat | armv7m | 1,008 | 17 | 17,136 | 1 | 16,128 |
| qemu/telem | armv7m | 1,472 | 17 | 25,024 | 1 | 23,552 |
| pizero2350/amp2 (per node) | armv7m | 1,456 | 7 | 10,192 | 1 | 8,736 |
| esp32c6-wroom/base | rv32imac | 1,184 | 17 | 20,128 | 1 | 18,944 |
| rx72m/base | rxv3 | 1,120 | 17 | 19,040 | 1 | 17,920 |
| qemu-arm64/base, imx8mp-evk/base | armv8a | 4,096 | 17 | 69,632 | 1 | 65,536 |
| qemu-arm64/benchsmp | armv8a | 4,096 | 17 | 69,632 | 4 | 53,248 |
| f302nucleo, f302nucleo-st, due, due-st, bluepill-c8, bluepill-c8-st | armv7m | -- | -- | 0 | 1 | 0 |
| every `esp32-wroom` preset | lx6 | -- | -- | 0 | 1 or 2 | 0 |
| every `sim` preset | sim | -- | -- | 0 | 1 | 0 |

**The armv7m `-st` presets do NOT take the larger stack, and the clause says why.**
`KICKOS_KERNEL_STACK_SIZE` reaches 1456 only under `ARCH_ARMV7M && KICKOS_ENABLE_SELFTEST &&
!KICKOS_AMP_POSTURE_NONE` (`Kconfig:820`). Selftest alone does not satisfy it: an `-st` preset
declares no AMP posture, so it falls through to 1008 and its row is identical to its base row. The
one armv7m preset that DOES satisfy the clause is `pizero2350/amp2`, which carries
`KICKOS_AMP_POSTURE_OWN_IMAGE` alongside selftest -- so the larger figure belongs to the AMP node
rather than to the selftest build, which is the opposite of how the knob reads at a glance.

The headline, on the board where it actually bites: **microbit carries 4,480 bytes of per-thread
kernel stacks in `.bss` on a 32 KiB part, 13.7 percent of the 32 KiB the linker script provisions,
and the ceiling saving from
sharing is 3,584 bytes.** The board's own defconfig names the trade in its comment -- four threads
leave room for kernel stacks and libc state (`boards/microbit/configs/base/defconfig:6-10`) -- and
`roadmap.md:452-453` records that microbit is already at the arena cliff, `_ebss` being
`__kickos_ram_start`. The nRF51 RAM length is 32768 (`arch/arm/chip/nrf51/nrf51.ld:26-32`); hardware
is 16 KiB and the emulated target is provisioned at 32.

The largest ABSOLUTE figures are the 64-bit ports at 69,632 bytes, and they are the least
interesting: those `K` values are marked PROVISIONAL in Kconfig, taken from armv8a and not measured
on their own arch (`Kconfig:810-817`), and none of those boards is RAM-constrained. A saving computed
off an unmeasured constant is a saving off a placeholder.

#### The f302nucleo-st attribution in TODO.md does not hold, and this axis is where it matters

`f302nucleo-st` lost a thread slot to the priority ceiling's four bytes, and the user stack was
measured NOT to be the lever. The dominant term was attributed to the per-slot kernel stack, and
that attribution does not hold.

**That board carves NO kernel blocks.** `CHIP_STM32F302` selects `ARCH_ARMV7M` and does not select
`HAS_MPU` (`arch/Kconfig:202-204`), armv7m does not select `ARCH_KERNEL_STACKS_MANDATORY`
(`arch/Kconfig:89-93`), so no `default 1` clause matches and `KICKOS_KERNEL_STACKS` resolves 0
(`Kconfig:764-769`). The roots file states the same in prose and names that board among the six
(`tests/static/trap_redzone_roots.txt:343-346`).

The likely mechanism is the assert's own text: both arena asserts end with a remedy clause that names
the blocks conditionally -- "which where `KICKOS_KERNEL_STACKS` is 1 includes `KICKOS_THREAD_SLOTS`
blocks" (`arch/common/boot_arena.ld.h:34-37` and `:72-77`) -- and the same header notes that the
block is the one owner the pool arithmetic cannot see, being `.bss` itself rather than a section
above it (`arch/common/boot_arena.ld.h:61-70`). A reader taking the remedy clause as a diagnosis on a
board where the condition is false gets exactly the recorded attribution.

Two consequences for this investigation. First, the `KICKOS_KERNEL_STACK_SIZE` probe on that board
is VOID rather than inconclusive: the knob reaches nothing there, so an inert probe was the answer
and cannot be told from a `-D` that never arrived. Second, and this is the load-bearing one, the
board most often cited as the fleet's RAM-pressure exemplar is one shared kernel stacks would not
help at all, because it has none to share. The exemplar for this axis is microbit.

The record that carried the wrong attribution states the corrected one.

### What the change would cost that is not RAM

- **Every blocking operation becomes explicit saved state.** Today the output state is explicit and
  the control flow is not. Send, receive, fused reply-receive, call, far reply, notification wait,
  join, task-empty wait, semaphore, PI mutex and sleep each park through one of three funnels
  (`kernel/sync/sync.cc:76-99`, `kernel/time/time.cc:147-176`) and each resumes into its own
  remaining code. Under a shared stack each of those needs a record of where it was and what it still
  owes: output pointers, copy progress, deadline, reply authority, notification result, donation
  state and cancellation ownership. The fastpath shows the shape for the simplest of them, and it
  needed a TCB flag, a second reinterpretation of `ipc.buf`, a scheduler-side completion site and an
  explicit multicore exclusion.
- **Timeout and cancellation stop being a stack unwind.** `park_cancel_pending` is deliberately not
  at the park, because a transaction may already be committed, and the contract is to exit from the
  prologue (`kernel/include/kickos/sync.h:66-72`). With no frames left, "exit from the prologue" has
  no prologue to exit from and the partially committed transaction needs a named undo owner. The
  timeout path has the same shape: `-KOS_ETIMEDOUT` currently lands in a continuation that knows what
  it had already done.
- **Teardown changes.** `kickos_fault_stack_top` relocates the death-path stubs to the block top and
  discards the dispatch frames, which is what keeps the block requirement a MAX rather than a SUM
  (`kernel/init/fault.cc:195-223`); and `cap_teardown` is preemptible between chunks
  (`kernel/sched/sched.cc:588-594`), so the teardown itself needs a stack that survives a switch.
- **The host simulator runs the real kernel and must model the same completion contract.** It has no
  saved register frame to complete into, which is the stated reason it declines the one mechanism the
  tree has (`arch/sim/sim.cc:1517-1521`). Whatever replaces the continuation, the sim implements it
  or stops being the fleet's largest test surface.
- **The arch seam grows a contract it does not have.** The current contract is one sentence in
  `arch.h` (`:660-668`) that every backend satisfies by construction. A completion contract has to be
  written, has to hold across the pending and inline switch disciplines, and becomes a new thing every
  port owes -- against a pillar that says adding a CPU means implementing a small seam, not
  restructuring the kernel (`docs/reference/architecture.md:43-45`).
- **A second execution model is the failure mode.** The 2026-09-17 record warns against making one
  permanent to preserve an inconclusive prototype, and the tree has a live example of how that
  happens: `ipc_fastpath.cmake` exists on four backends and not on five, so the fastpath park and the
  continuation park are already two models and `sched.cc` carries the completion site for both.

### What the reference kernels do, stack model only

Read-only inspection of local checkouts; cited by path, nothing copied.

**seL4** (`seL4`) allocates one kernel stack per node as a flat array
(`seL4/include/kernel/stack.h:18`, `seL4/src/kernel/stack.c:10`), default 2^12 = 4096 bytes per CPU
(`config.cmake:160-167`), and that config's own help text states there is no guard below the stack
(`config.cmake:164`). RISC-V trap entry loads the stack top directly on the uniprocessor path
(`seL4/src/arch/riscv/traps.S:94`) and keeps the per-core pointer in `sscratch` under SMP
(`seL4/src/arch/riscv/traps.S:27-34`). Blocking is entirely TCB data: a 3-word thread state carrying
`blockingObject`, `blockingIPCBadge` and the grant bits (`seL4/include/object/structures.h:245`,
`seL4/include/object/structures_64.bf:319-346`), a 2-word fault (`seL4/include/object/structures.h:255-256`), a
bound notification word (`:250-253`), and on MCS a reply object (`structures_64.bf:324,332`); the
writes are at `seL4/src/object/endpoint.c:33-44` and `seL4/src/object/notification.c:206-209`. Long operations
are interrupted at explicit preemption points returning `EXCEPTION_PREEMPTED`
(`seL4/src/model/preemption.c:16-43`, `seL4/src/api/syscall.c:366-368`) and are resumed by RESTARTING the whole
syscall from the original PC (`seL4/src/kernel/thread.c:54-61`, `seL4/src/fastpath/fastpath.c:516`). There is no
mid-operation resume.

That last point is the one this investigation should carry forward loudest: seL4 does not save an
operation's progress, it makes the operation replayable. KickOS's syscalls are not written to be
idempotent up to their first mutation, and nothing in the tree claims they are. Adopting the stack
model without that property means inventing the saved-progress representation seL4 avoided.

The per-thread byte cost of seL4's saved state is NOT ESTABLISHED as a single constant: the TCB slab
is `BIT(seL4_TCBBits)` (`seL4/include/object/structures.h:78-93`, e.g. 1024 or 2048 bytes on RISC-V 64 per
`seL4/libsel4/sel4_arch_include/riscv64/sel4/sel4_arch/constants.h:26-29`) but that slab also holds CNode
entries, and no constant pins `sizeof(tcb_t)`.

**Fiasco.OC** (`fiasco`) retains a per-thread kernel stack: `Context`
carries `_kernel_sp` (`fiasco/src/kern/context.cpp:362`) and the switch is a genuine stackful save and
restore of an in-progress C call chain (`fiasco/src/kern/riscv/context-riscv.cpp:98-137`,
`fiasco/src/kern/arm/32/context-arm-32.cpp:29-79`). The stack and the C++ object are ONE allocation of
`THREAD_BLOCK_SIZE`, which is 8K, 4K or 2K and nothing else (`fiasco/src/kern/config_tcbsize.h:5-13`,
`fiasco/src/kern/context_base.cpp:9-15`, `fiasco/src/kern/thread_object.cpp:97-99`). The lifetime
is load-bearing in
three places, not one: `IPC_timeout::owner()` recovers the owning context by masking its own `this`
pointer down to the block base, because the timeout is constructed on the owner's kernel stack
(`fiasco/src/kern/ipc_timeout.cpp:37-47`, `fiasco/src/kern/context_base.cpp:48-53`);
`IPC_remote_timeout::owner()`
does the same for the cross-CPU abort path, whose handler calls `abort_drq` on the recovered owner
(`fiasco/src/kern/ipc_remote_timeout.cpp:29-59`); and `Pi_wait_timeout::waiter()` does it for
priority-inheritance wait timeouts (`fiasco/src/kern/pi_mutex.cpp:214-235`). `current()` itself is derived
from the live stack pointer (`fiasco/src/kern/context_base.cpp:83-97`). Optional `Config::Stack_depth` fills
the unused part of the block for depth inspection (`fiasco/src/kern/config.cpp:68`,
`fiasco/src/kern/thread.cpp:379-381`).

So the counterexample is stronger than the 2026-09-17 record states: it is not one timeout design but
a whole family of pending-operation objects whose identity IS their address inside the owner's stack,
covering local timeout, remote abort and PI wait. A microkernel does not have to share kernel stacks,
and Fiasco spends the retained stack on making pending-operation ownership free.

**The MCU-class fleet** is the strongest evidence on the axis the RAM argument is made for.

- **NuttX** (`nuttx`): on the MCU-class Cortex-M ports there is no separate
  privileged stack at all -- the SVC path flips the CONTROL privilege bit and returns to thread mode
  on the SAME PSP (`nuttx/arch/arm/src/armv7-m/arm_svcall.c:340-344`, and the armv8-m sibling).
  A genuine per-task kernel stack exists only under `CONFIG_ARCH_ADDRENV` on MMU archs, default
  `CONFIG_ARCH_KERNEL_STACKSIZE` 1568 (`nuttx/include/nuttx/addrenv.h:168`,
  `nuttx/arch/arm/src/armv7-a/arm_addrenv_kstack.c:120-165`). Per-CPU: the interrupt stack
  (`arch/Kconfig:1413-1421`, `nuttx/arch/arm/src/armv7-m/arm_exception.S:256-263`) and the idle stack.
  Blocking keeps the call chain (`nuttx/sched/semaphore/sem_wait.c:254`).
- **Zephyr** (`zephyr`): a genuine per-thread privileged stack for every
  user-mode thread, default `CONFIG_PRIVILEGED_STACK_SIZE` 1024 (`arch/Kconfig:402-412`), allocated in
  `setup_priv_stack` (`zephyr/arch/arm/core/cortex_m/thread.c:50-73`) and switched onto at SVC entry
  (`zephyr/arch/arm/core/cortex_m/svc.S:89,197,409,468`). Per-CPU: the ISR stack
  (`zephyr/kernel/init.c:142-143,397-398`, default 2048 at `zephyr/kernel/Kconfig:167-172`) and the
  idle threads.
  Blocking keeps the call chain (`zephyr/kernel/sched.c:522-552`).
- **FreeRTOS-LTS** (`FreeRTOS-LTS`): under the default MPU wrapper model a
  per-task system-call stack is embedded in the task's MPU settings and PSP is switched onto it for
  the duration of a system call
  (`FreeRTOS-LTS/FreeRTOS/FreeRTOS-Kernel/portable/GCC/ARM_CM4_MPU/portmacro.h:205-249`,
  `port.c:356-364,515-615,647`). Its size `configSYSTEM_CALL_STACK_SIZE` is mandatory and
  application-supplied, with no shipped default (`portmacro.h:206-209`) -- NOT ESTABLISHED as a
  number. Only the MSP is shared. Blocking keeps the call chain
  (`FreeRTOS-LTS/FreeRTOS/FreeRTOS-Kernel/queue.c:1616-1621`).

All three MCU-class kernels keep per-thread privileged execution state and put only the interrupt or
exception stack per CPU. Two of the three pay a dedicated per-task privileged stack to do it, at 1024
and 1568 bytes, which brackets KickOS's 896 to 1472. On the fleet class where the RAM argument is
supposed to be strongest, the shared-stack design has no MCU-class precedent in these three trees.

### Preserved from the 2026-09-17 record

#### The independent stack-test improvements, carried forward

These are independent of the stack-model decision and survive either outcome
(`docs/design-stack-safety-research.md:312-357`):

1. an explicit per-configuration stack inventory -- thread, kernel, trap/IRQ and panic -- each with
   capacity, entry-frame cost, nesting allowance, applicable guard and evidence, extending existing
   declarations rather than starting a second list;
2. tailored-stack POSITIVE tests on the stack that actually hosts kernel execution, covering receive,
   reply, fused reply-receive, error, timeout and wake, including a valid minimum-headroom case and a
   below-threshold rejection where the entry contract defines one;
3. negative containment tests: touch a guard, and separately induce exhaustion, requiring the expected
   fault reason, correct offender attribution and intact adjacent memory, reported from a known-safe
   stack;
4. deterministic nesting injection at a deliberately deep point, checking each active stack
   separately including hardware exception frames and FP state;
5. per-port evaluation of hardware limits and MPU/PMP guards only after checking privileged
   enforcement, region cost, context-switch updates and fault-entry behaviour;
6. instrumentation kept diagnostic, with explicit interrupt coverage and an early-entry story, never
   replacing production-build budget checks;
7. an independent detector test that corrupts a canary on a dedicated block and checks detection and
   reporting, presented as neither a capacity nor a containment test.

One of the record's coverage observations is confirmed still open on today's tree: the static gate has
no capacity-fit clause for `stack=trap`, and the skip is by NAME rather than by the figure fitting
(`tests/static/check_trap_redzone.sh:334-341`). The gate's other three classes each have a clause:
`stack=panic` against `KICKOS_PANIC_STACK_SIZE` (`:319-337`), `stack=kernel` against the block minus
the canary word (`:342-366`), and the unmarked thread classes against `KICKOS_MIN_STACK_SIZE`
(`:367-379`). The rv32 per-hart trap stack is sized by a single documented reporter chain at 560
measured against 832 enforced (`arch/riscv/rv32imac/include/kickos/arch/rv_trap_stack.h:209-233`), so
the gap is a missing GATE and not an unsized array. If the shared-stack model were ever adopted, this
clause is the one that would have to be written first, because the shared stack would inherit exactly
the `stack=trap` shape.

#### The M8.9 audit findings, carried forward with their current status

Historical findings against M8.9-p4 `eefa886b`, compared with M8.8 `208c6998`; line numbers are the
audited commit's and are read with `git show <commit>:<path>`
(`docs/design-stack-safety-research.md:463-497`). The record states their fix status was not
re-audited. Reading today's tree, not running anything:

| priority | finding | status on today's tree |
| --- | --- | --- |
| P1 | `syscall_ipc.cc:67` deferred wake retained only the highest-priority thread, so a fused reply plus a lower-priority cross-core receive could omit a core's reschedule request | ADDRESSED by reading: `DeferredWake::offer` keeps the highest and places the other across cores through `sched::place_ready` (`kernel/syscall/syscall_ipc.cc:40-86`) |
| P2 | `cap.cc:374` closing a SIGNAL-only IRQ alias cleared a live notification binding | ADDRESSED by reading: the `CAP_IRQ` close now does nothing to the binding and says why -- a waiter parks on the notification, not the line (`kernel/syscall/cap.cc:360-367`) |
| P2 | `syscall_ipc.cc:1031` reply-half EBADF/EFAULT returned before writing the consumed notification mask, so an input mask of 1 could survive as a phantom event | ADDRESSED by reading: consumed bits are written on every path after the input snapshot, errors included, skipped only when the input mask was zero (`kernel/syscall/syscall_ipc.cc:1023-1024`) |
| P2 | `syscall_ipc.cc:915` standalone reply timing ended before the deferred-wake destructor scheduled, so the span was not comparable with the earlier baseline | SUPERSEDED rather than fixed: M8.12 redefines the reply spans explicitly and excludes `REPLY_LOCKED`, `REPLY_WAKE` and `REPLY_RECV_TOTAL` from any delta against M8.7 by name (`docs/archive/M8.12_meas.md:92-110`) |

Three of the four were reproduced with scratch probes against the real IPC, capability, IRQ and
two-core scheduler code, with address validation stubbed for owned host buffers, so none was a
hardware isolation test; the scratch artifacts were session-local and are gone. "Addressed by
reading" above means the current code states the corrected behaviour at the cited lines; it is not a
test result, and nothing here re-ran the probes. The scenarios still deserve permanent tests, and that
is independent of the stack model. Also preserved: the two DRY opportunities the audit left --
duplicated reply-capability admission and minting, and service loops hand-initialising options despite
an available initializer.

### Interaction with the lock question, stated only now

Everything above holds with the lock policy fixed, and the following conclusions are INDEPENDENT of
it: the RAM arithmetic and its headline board; the arch-seam contract change; the host-simulator
completion problem; the teardown relocation property; the timeout and cancellation control-flow
argument; and the observation that the fleet's MCU-class references keep per-thread privileged state.
None of those reads a lock.

Two are NOT independent. First, per-CPU stack OWNERSHIP under contention: with one kernel lock, one
core at a time is inside the dispatch, so a shared per-CPU stack is trivially single-writer. Under
partitioned locks two cores can be in the kernel at once, which is fine for per-CPU stacks by
construction, but a thread migrating between cores while an operation is pending needs its saved
state to be core-independent -- and today part of that state is frames on a block that no core owns.
So lock partitioning makes the shared model's saved state MORE necessary, not less, and the two
changes are not independent in that direction. Second, the completion write: the fastpath's result
write happens in the seat path (`kernel/sched/sched.cc:133-141`), which is inside the lock today; who
holds what when a waker completes a remote sleeper's saved frame is a lock-partition question and
cannot be answered from here.

The sequencing consequence is one sentence: measuring the stack model first, under today's lock, is
the only order in which its cost is attributable, and the lock work does not need it.

### What would have to be measured before any implementation could be proposed

Against the frozen M8.12 baseline (`docs/archive/M8.12_meas.md`), whose instrument is M8.7's and is
checked rather than asserted (`:10-24`), whose rows are `switch`, `lock-hold`, `rt8`, `rt256`,
`e2e-local`, plus `lock-wait`, `doorbell` and `e2e-cross` on the four-core presets, and whose silicon
half is `f411disco`, `esp32c6-wroom` and `xmc4800-relax` (`:25-79`). Its exclusion list and the
XMC's min-versus-p50 headline label are binding on any comparison (`:92-142`).

1. **The footprint baseline this axis does not have.** A fleet static-footprint capture per preset --
   kernel `.bss`, `.data`, text, arena floor and seated slot count -- taken on the current tree so a
   later one is a delta. `docs/archive/M4.5_footprint_meas.md` and `M4.7.9_footprint_meas.md` both
   predate the blocks, the MMU and the lock, and `TODO.md` already records that no M8 sub-milestone
   owns a fleet footprint chase. Without this, any RAM claim about the change is arithmetic against
   arithmetic.
2. **What the microbit slot actually costs and buys.** Whether dropping `K` on armv6m, or removing
   the blocks, changes the seated thread count on a 32 KiB part -- with the knob driven through the
   generated board config and not the CMake cache, which is the specific reason the `f302nucleo-st`
   probe was inconclusive (the same item). The same method error would make this probe
   uninterpretable too.
3. **D, per slot, from a real design rather than from the fastpath.** The saved-operation record for
   ONE timed, cancellable, multi-stage operation, sized in bytes. Until D exists the saving is an
   upper bound and not a figure.
4. **IRQ-to-user, which the baseline lacks entirely.** A p50/p99/max row on at least one region board
   and one RISC-V board, added to the instrument BEFORE any stack change, or the axis stays
   unmeasurable in both directions.
5. **IPC round trip on both switch disciplines.** `rt8`, `rt256` and `e2e-local` on at least one
   pending-switch backend (armv7m or rv32imac silicon) and one inline-switch backend, since the
   completion contract differs there and a result from one says nothing about the other.
6. **Maximum interrupt-disabled interval**, unchanged instrument, because a completion write moving
   into the switch path lengthens a masked region and nothing currently reports that.
7. **Stack headroom on the shared stack under the deepest legitimate path**, which needs the
   `stack=trap` capacity-fit clause written first: a shared per-CPU kernel stack is a `stack=trap`
   class and the gate has no clause for one today.
8. **Teardown and cancellation correctness before any timing.** Exactly-once completion across reply,
   timeout, cancellation and thread death; forced teardown under a preemptible `cap_teardown`; and
   the host simulator running the same contract. These are pass or fail, not numbers, and no timing
   result means anything until they pass.

Regression budgets are set before collection, per the M8.12 record's own rules, and observed maxima
remain observations and not worst-case bounds. A result that retains per-thread kernel stacks is a
valid result, and on the evidence gathered here it is the expected one.
