<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Stack safety, kernel-stack footprint, and lessons from other kernels

> **Status: EXPLORATORY.** Research recorded on 2026-09-17 following the M8.9-p4
> audit. Recommendations and experiments, not an approved implementation plan or a
> change to the architecture contract. Investigation belongs in M9, after the
> frozen M8.12 baseline, alongside the kernel-concurrency research. No new silicon
> measurements were taken.

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
comments explicitly warn that overflow detection is not guaranteed. MPU ports have separate handling and may rely on
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
