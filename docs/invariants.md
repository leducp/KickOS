<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS invariants (Book)

> Book chapter (tier 1). The cross-cutting properties a change must not break,
> extracted by the M1.x close-out review across the arch seam, kernel, per-arch
> backends, and docs. Each is a *checklist item* for future milestones (M2 above
> all): re-audit the whole arch/chip/board matrix against these, hunting both
> drift (one arch violates what the others uphold) and absence (a backend that
> forgot one). `source` cites where it lives in the code.

## Context switch & FP save/restore

- **`switch-frame-matches-init`** — The frame fabricated by arch_context_init must be byte-identical to what the context-switch save path pushes/pops (same order, same slots), and the arch_context field offsets the asm hard-codes (sp@0, npriv@4, resting_npriv@8, trace_tid@12 on armv7m; sp@0/trace_tid@4 on rx; sp/ps/pc/resume_kind/trace_tid on xtensa) must stay pinned by static_assert. A silent struct reorder or frame-layout drift corrupts the saved SP/privilege on the first switch-in.
  - *applies:* context switch; armv7m, armv6m, rx, xtensa
  - *source:* arch/arm/armv7m/arch_armv7m.cc:24-31; arch/arm/armv7m/switch.S:11-18,134-151; arch/rx/rxv3/arch_rxv3.cc:34-38,144-204; arch/xtensa/lx6/arch_xtensa.cc:31-39,187-213

- **`npriv-banked-on-switch`** — CONTROL.nPRIV is saved and restored per switch as if it were a register (ctx.npriv), NOT reset to the thread's resting posture: a thread blocked mid-syscall runs privileged (the SVC trampoline raised it) and must resume privileged. arch_context_init seeds ctx.npriv AND ctx.resting_npriv from the thread's resting privilege.
  - *applies:* context switch + privilege; armv7m, armv6m (rx uses banked PSW.PM the same way)
  - *source:* arch/arm/armv7m/include/kickos/arch/context.h:24-37; arch/arm/armv7m/switch.S:59-77; arch/arm/armv7m/arch_armv7m.cc:152-161

- **`fp-armv7m-lazy-frame`** — On an FPU-equipped ARMv7-M part the extended FP state is saved/restored conditionally on EXC_RETURN bit4: PendSV must vstmdb {s16-s31} BEFORE stmdb {r4-r11,lr} (higher addresses) on save and vldmia AFTER the callee pop on restore, so a thread with no live FP context carries no FP frame. Breaking the bit4 test or the push/pop ordering corrupts FP state across a switch.
  - *applies:* context switch, FP save/restore; armv7m with __ARM_FP
  - *source:* arch/arm/armv7m/switch.S:52-72

- **`fp-rx-full-context`** — The RX SWINT switcher saves the full context each switch: R1-R15 (single-precision FP lives in the GPR file), FPSW, both accumulators, and — under -mdfpu — the DPFPU file (DR0-DR15 + DPSW/DCMR/DECNT via DPUSHM/DPOPM); arch_context_init fabricates exactly those slots with the DPFPU reset posture (DPSW=0x100, DECNT=1). Dropping -mdfpu must leave the slots carrying zeroes, not desync the frame.
  - *applies:* context switch, FP save/restore; rxv3 (rx72m)
  - *source:* arch/rx/rxv3/arch_rxv3.cc:20-38,158-201

- **`fp-xtensa-caller-saved`** — On LX6 CPENABLE is global (CP0 enabled for all threads), so FP data registers are NOT banked by that enable. The cooperative (thread-context) switch relies on the compiler spilling caller-saved FP regs; only the preemptive level-1 interrupt frame saves f0-f15+FCR+FSR. A change that assumes the cooperative path saves FP registers is wrong.
  - *applies:* context switch, FP save/restore; xtensa lx6 (esp32)
  - *source:* arch/xtensa/lx6/arch_xtensa.cc:443-454

## Critical section & interrupt-priority bands

- **`irqlock-nesting-safe`** — IrqLock must record the prior interrupt-mask state and restore exactly that on scope exit (never unconditionally unmask), so nested critical sections compose. Every arch's arch_irq_save/restore returns/consumes the full prior state (BASEPRI word, PSW.IPL field, PS, or the sim's SIGALRM/SIGUSR1 unblocked flags).
  - *applies:* critical section / IrqLock; all arches
  - *source:* kernel/include/kickos/irqlock.h:14-32; arch/arm/armv7m/arch_armv7m.cc:164-181; arch/rx/rxv3/arch_rxv3.cc:207-228; arch/xtensa/lx6/arch_xtensa.cc:233-245; arch/sim/sim.cc:526-559

- **`basepri-write-needs-barrier`** — On ARMv7-M, raising BASEPRI is not self-synchronizing: arch_irq_save must follow the msr with DSB+ISB, else an interrupt can still be taken under the old mask on the next instruction and preempt a critical section. (RX MVTIPL and Xtensa RSIL are self-synchronizing and correctly omit the dance.)
  - *applies:* critical section; armv7m
  - *source:* arch/arm/armv7m/arch_armv7m.cc:164-176; arch/rx/rxv3/arch_rxv3.cc:207-216

- **`device-irq-in-maskable-band`** — A device IRQ line's priority MUST be programmed into the IrqLock-maskable band BEFORE the line is enabled, or an IRQ can preempt an IrqLock-held section and corrupt kernel state. Bands: armv7m PRIO_DEVICE=0x30 >= lock 0x20; rx IPL_DEVICE=4 < IPL_LOCK=12; xtensa masks levels 1-3. NVIC IPR / ICU IPR reset to 0 (highest), so the write is mandatory, not optional.
  - *applies:* ISR priority bands; armv7m, rx, xtensa
  - *source:* arch/arm/armv7m/arch_armv7m.cc:203-216; arch/arm/armv7m/regs.h:41-50; arch/rx/rxv3/arch_rxv3.cc:353-366; arch/rx/rxv3/regs.h:37-40; arch/xtensa/lx6/arch_xtensa.cc:56-58

- **`deferred-switch-lowest-band`** — The deferred-switch mechanism (PendSV / RX SWINT / xtensa L1-exit) must sit at the lowest active priority AND below the IrqLock threshold, so a switch requested from an ISR fires only after every other exception tail-chains and only once the requesting critical section releases. armv7m PRIO_PENDSV=0xF0; rx IPL_PENDSW=1 (< IPL_LOCK=12).
  - *applies:* deferred-switch model, ISR bands; armv7m, armv6m, rx, xtensa
  - *source:* arch/arm/armv7m/switch.S:26-29; arch/rx/rxv3/arch_rxv3.cc:429-436; arch/xtensa/lx6/arch_xtensa.cc:216-230

## Deferred switch & blocking model

- **`arch-switch-may-defer`** — arch_switch MAY not complete synchronously (ARM/RX pend an exception, xtensa flags g_arch_switch_pending when in ISR); the scheduler must not assume the register swap has happened when arch_switch returns. The switch target crosses only through g_arch_next/g_arch_current — PendSV saves g_arch_current and ignores the `from` argument.
  - *applies:* deferred-switch model; all arches
  - *source:* arch/include/kickos/arch/arch.h:44-48; arch/arm/common/arch_arm_common.cc:61-68; arch/xtensa/lx6/arch_xtensa.cc:216-230; docs/architecture.md:220-222

- **`no-block-from-isr`** — Blocking (sem_wait, sleep, any detach_current/block_current path) is legal ONLY from thread context: it relies on arch_switch completing synchronously. From ISR context the switch would defer and the 'blocked' thread would keep running, so detach_current panics when arch_in_isr() is true.
  - *applies:* deferred-switch model, sync primitives; kernel + all arches
  - *source:* kernel/sched/sched.cc:105-127; kernel/sync/sync.cc:37-48

- **`exit-parks-for-deferred-switch`** — exit_current must not run off the end after committing the switch-away: on a deferring arch the PendSV/SWINT can only fire once the IrqLock releases, so the EXITED thread (off the ready set, never rescheduled) must park in an arch_idle_wait loop until the pended switch lands. Running past with the switch merely pending is a known-fixed bug class.
  - *applies:* deferred-switch model, thread exit; armv7m, armv6m, rx
  - *source:* kernel/sched/sched.cc:141-171

- **`arch-in-isr-truth`** — arch_in_isr() must read true in ISR/handler context and false throughout syscall_dispatch and ordinary thread context. RX has no IPSR: its g_in_isr counter is bumped ONLY by the device-IRQ/timer first-level dispatchers, never by the syscall INT path, so dispatch reads false as the contract requires.
  - *applies:* deferred-switch model, syscall contract; all arches (rx software counter)
  - *source:* arch/include/kickos/arch/arch.h:62-63,123-142; arch/rx/rxv3/arch_rxv3.cc:59-62,230-233,400-417; arch/arm/common/arch_arm_common.cc:70-77

## Syscall, privilege & the user/kernel boundary

- **`syscall-in-priv-thread-context`** — The arch MUST run syscall_dispatch() in privileged THREAD context on the calling thread's own continuation, never in handler/ISR context. A blocking syscall blocks by an ordinary synchronous context switch that freezes the mid-dispatch continuation on the caller's stack and resumes it inline when rescheduled. On ARM the SVC handler redirects to svc_trampoline (privileged thread mode); it does not dispatch inside the handler.
  - *applies:* syscall + privilege + resume-inline; armv7m, armv6m, rx (xtensa: plain call, trivially satisfied)
  - *source:* arch/include/kickos/arch/arch.h:123-142; arch/arm/armv7m/switch.S:111-159; arch/xtensa/lx6/arch_xtensa.cc:432-440; docs/porting.md:88-101

- **`syscall-restores-resting-priv`** — On syscall return the SVC trampoline must restore the caller's resting privilege (ctx.resting_npriv), NOT hard-code nPRIV=1: a privileged thread issuing a syscall must drop back to exactly its entry posture, never be demoted to unprivileged.
  - *applies:* syscall + privilege; armv7m, armv6m
  - *source:* arch/arm/armv7m/switch.S:142-159; arch/arm/armv6m/switch.S:117-127; arch/arm/armv7m/include/kickos/arch/context.h:33-37

- **`user-thread-return-via-syscall`** — An unprivileged thread whose entry returns must route through kickos_user_thread_return (which traps out via the exit syscall); it must NOT run the kernel's kickos_thread_return directly, because exit_current with nPRIV=1 makes IrqLock/BASEPRI a no-op and the switch-path SCS write faults. arch_context_init selects the return address by privilege.
  - *applies:* syscall + privilege, thread exit; armv7m, armv6m, rx
  - *source:* arch/arm/armv7m/arch_armv7m.cc:46-53,124-131; arch/rx/rxv3/arch_rxv3.cc:64-70,164-172; kernel/thread/thread.cc:85-90

- **`privilege-escalation-gated`** — Privilege-crossing syscalls must be gated on the caller being privileged: spawning a privileged thread, irq_attach, and ram_alloc all reject (-1) an unprivileged caller. There is no path by which an unprivileged thread obtains privilege or the whole-arena grant.
  - *applies:* user/kernel boundary, syscall; kernel (all arches)
  - *source:* kernel/syscall/syscall.cc:128-133,310-317,343-356

- **`user-args-validated-at-boundary`** — Every user-supplied scalar must be range-checked or clamped at the syscall boundary before use, and an invalid one returns -1 (never panics): priority is bounds-checked before it indexes ready lists / shifts a 1u<<prio bitmap; console len is clamped to 4096; injected/attached IRQ lines are checked against [0,KICKOS_MAX_IRQ); an unknown syscall number returns -1. There is no MPU to contain an OOB access at M1.
  - *applies:* user/kernel boundary; kernel (all arches)
  - *source:* kernel/syscall/syscall.cc:120-127,218-229,293-297,318-323,381-386

- **`handle-not-pointer-across-boundary`** — No kernel pointer crosses the user/kernel boundary: kernel objects (semaphores, threads, IRQ bindings) are named by small integer handles resolved through a single validate-and-resolve chokepoint. Semaphore handles pack index+generation; sem_destroy bumps the slot generation so a stale handle fails to resolve rather than aliasing a recycled slot (ABA). The kernel must never strlen a user pointer (kconsole_write takes explicit buf,len).
  - *applies:* user/kernel boundary; kernel (all arches)
  - *source:* kernel/syscall/syscall.cc:1-9,34-94,213-229; kernel/include/kickos/instance.h:57-68

- **`privilege-only-boundary-m1`** — At M1 the user/kernel boundary is privilege-only (CONTROL.nPRIV / PSW.PM + SVC trap); there is NO hardware MPU enforcement, so arch_mpu_apply is a no-op on every MCU backend and an unprivileged thread can still read/write kernel memory. Code must not assume memory isolation between domains until M2.
  - *applies:* user/kernel boundary, MPU; armv7m, armv6m, rx, xtensa (sim DOES enforce via mprotect)
  - *source:* arch/arm/common/arch_arm_common.cc:139-144; arch/rx/rxv3/arch_rxv3.cc:304-311; arch/xtensa/lx6/arch_xtensa.cc:347-352; arch/sim/sim.cc:606-621

- **`mpu-apply-on-every-switch-in`** — The running thread's MPU region set must be (re)loaded on every switch-in and at first start, replacing the whole active set; region granting must be fail-closed (a region not a page-aligned sub-range of the arena is skipped, never applied to host memory). A privileged thread gets the whole arena; an unprivileged thread gets only its domain region.
  - *applies:* context switch, MPU, user/kernel boundary; kernel + sim (MCU no-op today)
  - *source:* kernel/sched/sched.cc:31-38,80-85; kernel/thread/thread.cc:54-72; arch/sim/sim.cc:187-227,606-621

## Tickless timer & clock

- **`timer-min-delta-guard`** — The tickless one-shot timer must never be armed for a compare that may already be in the past: ktime_rearm floors the next deadline at now+KICKOS_TIMER_MIN_DELTA_NS. A sub-min-delta RR quantum (including a hostile user value) collapses to the min slice rather than re-arming every tick (interrupt storm).
  - *applies:* tickless timer; kernel + all arches
  - *source:* kernel/time/time.cc:94-101; kernel/sched/policy_fifo_rr.cc:62-81

- **`rearm-before-switch`** — ktime_rearm must program the next-event timer for the INCOMING thread before arch_switch, because the outgoing thread will not return to switch_to until it is itself resumed — so the incoming thread's policy deadline (RR slice) must be armed now, not after the switch.
  - *applies:* tickless timer, context switch; kernel
  - *source:* kernel/sched/sched.cc:31-37,75-85

- **`timer-delta-clamp-before-convert`** — The ns->cycle conversion for the one-shot timer must clamp the delta to the counter's one-shot range BEFORE the ns*freq multiply (else a far-future or UINT64_MAX deadline overflows), and sleep deadlines must saturate to UINT64_MAX on now+ns overflow. A clamped deadline fires early and the kernel re-arms the remainder (harmless extra wake).
  - *applies:* tickless timer, ns<->tick; armv7m, armv6m, rx, xtensa
  - *source:* arch/arm/common/arch_arm_common.cc:89-123; kernel/time/time.cc:126-133; arch/rx/rxv3/arch_rxv3.cc:258-296; arch/xtensa/lx6/arch_xtensa.cc:305-338

- **`cycle64-wrap-extend-atomic`** — The software 32->64-bit extension of the free-running cycle counter (catch-wrap on each read) must run under the critical section so the {read counter, compare to last, bump high} sequence is atomic against a concurrent reader from thread and ISR context. LIMITATION: a wrap not observed within one 2^32-cycle period is missed (M1).
  - *applies:* tickless clock; armv7m (DWT), rx (CMTW1), xtensa (CCOUNT)
  - *source:* arch/arm/armv7m/arch_armv7m.cc:38-44,62-76; arch/rx/rxv3/arch_rxv3.cc:48-53,90-104; arch/xtensa/lx6/arch_xtensa.cc:70-74,141-155

- **`conversions-track-live-clock`** — All ns/cycle conversions and the SysTick/baud programming must track the LIVE SystemCoreClock (or kickos_rx_timer_hz), not a compile-time constant; the cached reciprocal-multiply / max-delta must be recomputed when the clock value changes. A PLL bring-up that updates SystemCoreClock must therefore keep the timer and clock correct.
  - *applies:* tickless timer, ns<->tick, SystemCoreClock; armv7m, armv6m, rx, xtensa
  - *source:* arch/arm/armv7m/arch_armv7m.cc:54-56,78-107; arch/arm/common/arch_arm_common.cc:39-47,94-119

- **`timer-disarm-clears-pend`** — 'Disarm' must mean no callback will fire: disarming (and reprogramming) the one-shot timer must clear any interrupt request already latched while the line was masked (ARM ICSR PENDSTCLR; RX clears the IR flag), or a stray timer callback fires once on the next IrqLock release.
  - *applies:* tickless timer; armv7m, armv6m, rx
  - *source:* arch/arm/common/arch_arm_common.cc:124-137; arch/rx/rxv3/arch_rxv3.cc:298-302

## IRQ dispatch

- **`irq-table-no-null-slots`** — Every IRQ line is seeded (by irq_init, before any driver attaches) with the null-object default handler that masks the line and bumps the spurious counter, so the ISR dispatch path indexes the table with NO null check and an unhandled enabled line cannot storm or be silently dropped.
  - *applies:* IRQ dispatch; kernel + all arches
  - *source:* kernel/irq/irq.cc:42-74,173-189; kernel/init/kmain.cc:99

- **`one-driver-per-line`** — A line may be claimed (irq_attach or irq_register) only while it still holds the null-object default; a second claim fails (-1/false) rather than overwriting an existing driver and orphaning its irq_wait forever. No stealing.
  - *applies:* IRQ dispatch; kernel
  - *source:* kernel/irq/irq.cc:81-98,110-138

- **`isr-mask-then-wake-ack-unmask`** — The first-level ISR must mask the line (so it cannot re-fire while its driver services it in thread context) then post the bound notification; the driver re-enables the line via irq_ack once serviced. A raise of a masked line is dropped (level-coalesced), not latched. The ISR handler runs by index with its arg being the pre-bound binding — no table search on the hot path (latency invariant).
  - *applies:* IRQ dispatch, ISR wakes pre-bound target; kernel + all arches
  - *source:* kernel/irq/irq.cc:31-40,157-167,173-189; arch/include/kickos/arch/arch.h:144-160; arch/sim/sim.cc:715-725

## Console

- **`console-single-producer`** — The buffered console ring is a strict single-producer/single-consumer: the buffered path (arch_console_write) is entered ONLY from thread context with the ring armed and not panicking; ISR context, fault context, panic, and pre-arm boot all fall back to the bounded polled writer (arch_console_write_sync). console_emit is the single routing chokepoint; any new path that logs from a raw ISR must not reach the buffered producer.
  - *applies:* console single-producer ring; kernel + buffered-console chips
  - *source:* kernel/init/console.cc:50-60; lib/include/kickos/console_tx.h:53-62; docs/console.md:75-101,175-182

- **`console-publish-prime-atomic`** — The producer's 'copy burst, publish head, enable TX IRQ' must publish head and enable the IRQ together under IrqLock, atomic against the ISR's 'drain to empty, disable IRQ', so there is no lost wakeup; the TX IRQ stays enabled whenever the ring is non-empty and the ISR disables it only on empty. The TX ISR therefore MUST sit in the IrqLock-maskable device band and must never sem_post/switch/block.
  - *applies:* console single-producer ring, ISR band; kernel + buffered-console chips (mk64f, xmc4800)
  - *source:* kernel/init/console_tx.cc:70-127; lib/include/kickos/console_tx.h:36-62; docs/console.md:181-185

- **`panic-console-probe-independent`** — Panic/fault output must not depend on the buffered ring or on an external debug probe: kpanic sets g_console_panicking, flushes queued bytes in order via console_tx_flush_sync, then prints through the polled synchronous writer and halts. The synchronous writer must remain a real transport (weak default aliases arch_console_write), never RTT-only.
  - *applies:* console panic-sync; kernel + all chips
  - *source:* kernel/init/console.cc:36-41,50-60,123-164; docs/console.md:185-197

## Telemetry

- **`telemetry-emit-atomic-one-lock`** — Every telemetry emit must assign the sequence number, sample arch_trace_now(), encode, and write to the sink under ONE IrqLock; splitting them lets a preempting context interleave an out-of-order seq/stamp and silently corrupt the host's loss/latency accounting. records_attempted/dropped are RMW under that same lock.
  - *applies:* telemetry record framing; kernel (all arches)
  - *source:* kernel/include/kickos/ktrace.h:44-134; docs/telemetry.md:131-137,346

- **`telemetry-record-atomic-drop`** — The telemetry sink is whole-record-or-drop: the ch1 ring checks free space for all n bytes up front and drops the entire record if it won't fit (a half-written binary record would desync the decoder permanently). Records are fixed-length, little-endian, self-delimiting; type tags are nonzero so a zeroed buffer never decodes as valid. One slot is reserved so head==tail is unambiguously empty.
  - *applies:* telemetry record framing, sink; lib/rtt + record.h (all arches)
  - *source:* lib/rtt.cc:92-124; include/kickos/trace/record.h:15-18,35-43,76-85; docs/telemetry.md:128-131,169-170

- **`switch-record-from-physical-contexts`** — The SWITCH {from_tid,to_tid} record must be emitted from the tids read out of the two contexts that PHYSICALLY swapped, never by re-reading g_arch_next or shared scheduler state (a preempting ISR can rewrite the decision between switch_to and the physical swap). trace_tid is stamped once at thread_create and read at the pinned ctx offset; from_tid==0xFFFF on the first switch; multiple wakes in one ISR collapse to exactly one SWITCH record.
  - *applies:* telemetry, context switch; armv7m, armv6m, rx, xtensa, sim
  - *source:* arch/include/kickos/arch/arch.h:80-88,205-210; arch/arm/armv7m/switch.S:43-95; arch/sim/sim.cc:106-131,235-251,481-523; kernel/thread/thread.cc:75-79; docs/telemetry.md:189-202

- **`telemetry-emit-fp-and-fault-free`** — The context-switch telemetry emit runs in the PendSV/SWINT/L1 tail with the incoming thread's FP state live in registers, so its whole call path is built -mgeneral-regs-only and must touch NO FP register. Fault/NMI handlers must never call ktrace_* (they preempt the BASEPRI/PRIMASK lock and would issue records out of the atomic emit region).
  - *applies:* telemetry FP-register-free ISR paths; armv7m, rx, xtensa
  - *source:* kernel/ktrace/ktrace.cc:1-8,51-55; docs/telemetry.md:228-232

- **`idle-is-trace-id-zero`** — Idle must be the first thread created so it receives trace id 0 (the decoder keys CPU% off tid 0 == idle); kmain asserts g_idle_tcb.id==0. The per-Kernel tid counter starts at 0 and, on wrap, skips both 0 (idle-only) and 0xFFFF (the no-thread sentinel).
  - *applies:* telemetry framing, thread identity; kernel
  - *source:* kernel/thread/thread.cc:14-29; kernel/init/kmain.cc:103-112; kernel/include/kickos/thread.h:48-51

- **`telemetry-off-zero-cost`** — With telemetry compiled out, every ktrace_* entry point is an empty inline (no symbols, hot paths byte-unchanged) and the ctx.trace_tid field and arch_trace_stamp_id seam are elided, so the telemetry-OFF struct layout and switch paths are byte-identical to a build that never had telemetry.
  - *applies:* telemetry; kernel + all arches
  - *source:* kernel/include/kickos/ktrace.h:142-154; arch/arm/armv7m/include/kickos/arch/context.h:39-45; arch/include/kickos/arch/arch.h:80-88

- **`trace-clock-u32-anchored`** — arch_trace_now() is a raw monotonic u32 counter that wraps (armv7m DWT CYCCNT, rx/xtensa cycle/CMTW counters, sim ns/1000 us); absolute time is reconstructed by the decoder from the two SESSION clock anchors (near at init, far at shutdown). A target with no such source must not define KICKOS_HAVE_TRACE_CLOCK and cannot enable telemetry. The closing SESSION + ch1 drain at shutdown must run with IRQs masked so no record lands after the records_attempted snapshot.
  - *applies:* telemetry trace clock, record framing; all arches
  - *source:* arch/include/kickos/arch/arch.h:70-88; arch/arm/armv7m/arch_armv7m.cc:192-198; arch/sim/sim.cc:366-419,576-581; kernel/include/kickos/ktrace.h:116-134

## Scheduler & instance state

- **`instance-scoped-state-no-arch-crossing`** — All kernel runtime bookkeeping (ready lists, current/idle, sleepq, object pools, IRQ table, telemetry counters) lives in the single Kernel instance reached only via kernel(); the sim arch backend keeps its own parallel SimInstance and never crosses the arch seam. Multiple instances co-reside in one host process, so no cross-cutting global may leak scheduler/telemetry state across emulated MCUs.
  - *applies:* instance scoping, arch seam; kernel + sim
  - *source:* kernel/include/kickos/instance.h:1-9,27-92; arch/sim/sim.cc:49-104

- **`ready-bitmap-tracks-lists`** — The priority ready bitmap bit p must be set iff ready[p] is non-empty: every push sets the bit, every remove that empties the list clears it. highest_prio() trusts the bitmap (31-clz) to pick the run list, and priority indexes both the ready[] array and a 1u<<prio shift — so a stale bit or an out-of-range prio is an OOB read/UB.
  - *applies:* scheduler ready structure; kernel (FIFO/RR policy)
  - *source:* kernel/sched/policy_fifo_rr.cc:23-59,83-91; kernel/syscall/syscall.cc:120-127

- **`tcb-link-node-shared-ready-xor-wait`** — A thread's intrusive `link` node is shared between the ready list and a wait queue (mutually exclusive membership), while timer-list membership uses the SEPARATE tnext link. A wait-queue block must detach_current from the ready list BEFORE pushing onto the wait queue (else it clobbers the links the ready removal needs); a timed wait may be on the timer list and a wait queue simultaneously only because they use different links.
  - *applies:* scheduler / sync queue membership; kernel
  - *source:* kernel/sync/sync.cc:37-48; kernel/sched/sched.cc:118-127; kernel/time/time.cc:104-113; kernel/include/kickos/thread.h:38-45

- **`sem-post-direct-handoff`** — Semaphore wakeups are a direct token hand-off: sem_post wakes the highest-priority waiter (FIFO among equals) and does NOT increment count; the woken waiter returns from sem_wait without decrementing. Count is incremented only when there is no waiter. Breaking this double-counts or loses a token.
  - *applies:* synchronization primitives; kernel
  - *source:* kernel/sync/sync.cc:13-34,58-91

