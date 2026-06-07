<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS — Architecture

KickOS is a small **microkernel** RTOS with a clear userspace/kernel separation, MPU-first
per-task isolation, an event-driven **tickless** scheduler, and a **first-class x86 host
"sim"** that runs the real kernel + userspace as one Linux process.

It draws design ideas (studied, never copied — see *Licensing*) from
**NuttX** (structure/porting/build-modes), **Argon RTOS** (C++ substrate),
**RIOT-OS** (tickless scheduler + native/host port), **ChibiOS** (tickless time-delta timers,
HAL, MPU sandbox), **µC/OS-III** (RR-within-priority + per-task quantum, task-local signaling,
introspection), **RT-Thread** (scalable footprint, device framework, POSIX/CMSIS-RTOS2 compat),
**Eclipse ThreadX** (preemption-threshold, MPU-isolated loadable Modules), **RTEMS**
(pluggable schedulers incl. EDF/rate-monotonic, SMP, newlib), and — for the microkernel
paradigm itself — **seL4** (capabilities, a minimal privileged kernel, IPC-centric design,
badged endpoints, static capability distribution) and **Zircon** (typed handles, channels,
`wait_many`).

## Design pillars

- **Clear userspace/kernel separation** — privileged kernel, unprivileged threads, syscalls
  across an SVC boundary. Microkernel, not monolithic.
- **MPU as a first-class citizen** — isolation is per **memory domain** (see below): threads
  share memory within a domain, domains are MPU-isolated from each other, and the MPU is
  reprogrammed on every context switch-in.
- **Proper scheduling** — event-driven FIFO scheduling that can switch on *any* event (yield,
  block, semaphore post, device IRQ), **not** only a periodic tick. Round-robin is available;
  the tick is optional/forced, never the sole trigger → a **tickless** core.
- **First-class host/x86 "sim"** — kernel + userspace as one Linux process for hardware-free,
  CI-friendly testing: the real kernel runs under CTest on the host.
- **C++ first-class** — kernel in **freestanding C++** (`-fno-exceptions -fno-rtti`); userspace
  gets **full C++ as a per-app opt-in** (exceptions/RTTI allowed there).
- **Low barrier — seL4's paradigm, not its ceremony.** The UX benchmark is the sibling
  **KickCAT**: easy to use, easy to tweak, no big machinery — *you write a `main`, and that's it*;
  the provided userspace libc/runtime already does most of the job for a basic app (this is what
  M0.3's OS-agnostic `main` entry delivers). Two axes: **app authors** — a plain app never writes
  a capability manifest to print "hello"; the runtime/root task wires a **sane default capability
  set**, and cap customization is opt-in for advanced users (easy things easy, hard things
  possible). **Porters** — adding a CPU means implementing the small **arch/chip seam** (`arch.h`
  + mpu/irq/timer/context backends), not restructuring the kernel; tractable for anyone who's done
  a NuttX port. This is a hard constraint on the capability model (12b): capabilities must hide
  behind defaults, never resurrect the "CapDL manifest just to boot hello world" friction that
  makes seL4 painful to start with — and hard to bring up on a new core.

## North star (long-term direction)

KickOS aims to be a **minimal microkernel RTOS in the seL4 tradition**: the smallest useful
privileged kernel — threads, protection domains, IPC, capabilities, IRQ routing — with
*everything else* (filesystems, networking, console, device drivers) as **unprivileged userspace
servers reached by IPC**. The design choices already made are downstream of this goal, not
incidental: **capabilities, not fds or global ids** for object access (a per-task typed handle
table — ambient authority contradicts the isolation pillar; see M2 12b); a **deliberately minimal
syscall surface** (`read`/`open`/`socket` are userspace stubs over IPC to servers, never kernel
calls — the debug console `write` is the sole sanctioned exception, cf. `seL4_DebugPutChar`);
and **services published by static capability distribution** (seL4/CapDL-style — the root task
grants each client exactly the endpoint caps its manifest allows), with **badged endpoints** to
authenticate callers. This fits the static-allocation, deterministic-RTOS ethos far better than a
runtime name server.

**MPU-first, but not MPU-only.** Isolation ships on the MPU (PMSA, no address translation), but
the **Domain / address-space seam is kept backend-agnostic** so a **VMSA (page-table / MMU)**
backend can slot in one day for application-class cores — e.g. **Cortex-A72 / Raspberry Pi 4B**
(GICv2, EL0/EL1, generic timer). This is *aspirational, not roadmapped*; its only claim on
present design is a discipline we already hold — keep MPU/PMSA specifics in the **arch/chip
layer**, never leaked into the core or the syscall ABI (the same arch-neutrality the non-ARM
**RX72M** target exists to prove). "MPU-first per-task isolation" is the M0–M2 reality; "one
address-space abstraction, MPU *or* MMU behind it" is the horizon.

## Targets

`sim` (host x86-64) is the first deliverable. MCU priority order:

1. **FRDM-K64F** — Cortex-M4F, 256 KB RAM, NXP **SYSMPU**.
2. **RP2040/Pico** — Cortex-M0+, ARMv6-M core MPU.
3. **STM32F411** — Cortex-M4F, ARMv7-M core MPU.

**STM32F103** (Cortex-M3, *no MPU*) is an optional degraded "privilege-only" build proving the
arch layer degrades cleanly. **Renesas RX72M** (RXv3, *non-ARM*, has an MPU) is a
zero-priority, keep-in-mind target whose value is an architecture-honesty check that `arch.h`
carries no ARM-isms.

**MPU is chip-specific, not arch-specific.** K64F = NXP **SYSMPU** (crossbar/bus-master region
protection, `__MPU_PRESENT=0` — not the ARM core MPU); RP2040 = ARMv6-M PMSA core MPU; F411 =
ARMv7-M PMSA core MPU. `arch_mpu_apply()` is implemented at the **chip** layer. (K64F SYSMPU
details to be confirmed against the K64 Reference Manual.)

---

## Licensing & clean-room discipline (hard constraint)

- **License: CeCILL-C** (French, GNU LGPL-compatible, file-level copyleft). `LICENSE` + SPDX
  `SPDX-License-Identifier: CECILL-C` headers on every source file.
- **Inspired, never copy-pasted.** The inspirations carry assorted licenses (NuttX/µC-OS-III/
  RT-Thread = Apache-2.0, RIOT = LGPL-2.1, ChibiOS = GPL-3/Apache, ThreadX = MIT, RTEMS =
  BSD-style). We study their designs/algorithms/APIs and write our own code — no lifting source,
  comments, or file structure verbatim. Ideas from a specific project are attributed in a design
  comment/doc, not by copying its implementation.
- KickCAT (`../KickCAT`) is a separate project; it is *used*, not vendored.

---

## Guiding invariants

1. **One porting layer.** All arch-specific behavior sits behind a small `arch::` interface.
   Adding a target = implementing that interface; kernel/lib/user code is arch-agnostic.
2. **Scheduling funnels through one function.** `sched::reschedule()` is the *only* place a
   switch is decided; every trigger just calls it. The tick is one optional caller among many.
3. **Tickless by default.** No mandatory periodic interrupt; a single "next-event" timer is
   armed for the earliest deadline. Pure-FIFO idle arms nothing.
4. **Identical userspace across arches.** Userspace links **KickOS libc only** — never host
   glibc, even on sim. Host libc is used *only* by the sim kernel's arch backend.
5. **Static allocation first, heap optional.** Kernel objects support link-time-static
   placement (e.g. `ThreadWithStack<2048>`) so a system can run with the heap disabled.
6. **Dual API in userspace.** A plain C syscall layer with ergonomic C++ RAII wrappers on top.
7. **Instance-scoped state, no hard singletons.** Kernel + sim state should hang off an instance
   handle, so multiple `kernel+userspace` instances can run in one host process (the KickCAT
   sim end-goal). *Status:* kernel **objects** (TCBs, semaphores) are already caller-owned, but
   the **runtime core** (scheduler/time/syscall pools/sim arch) is still file-static in M0 — a
   single-instance shortcut. **M0.1** aggregates that state into a `Kernel` struct reached via a
   compile-time-selectable `kernel()` accessor (static singleton on MCU / for size; thread-local
   per instance for the multi-slave sim), landing before M1 so MCU code is born instance-ready.
   Actual multi-instance in the sim additionally needs per-instance event delivery (see below).
8. **Dependency inversion — the app consumes the kernel.** The application owns the top-level
   build; KickOS is a prebuilt package (libraries + headers + startup + board linker script +
   flags) consumed as a plain `add_executable` linked against the exported `kickos` target (with
   `kickos_add_application()` as optional sugar). The app defines a known entry
   (`kickos_app_main()`) that the kernel's boot path calls after init.
9. **Conventions.** Traditional include guards `KICKOS_<PATH>_H` (no `#pragma once`); no ternary
   operators; comments only for hidden constraints/invariants. **Allman brace style**, enforced by
   the checked-in `.clang-format`, matched to the sibling projects `../KickCAT` / `../kickmsg`
   (4-space indent, indented namespaces + case labels, left-aligned pointers, leading-comma ctor
   init, east-const/west-volatile, `ColumnLimit: 0`) — Allman *everywhere*, no one-liners; run
   `clang-format -i` on changed sources.

### How KickOS differs from its inspirations

None of them combine MPU-first per-task isolation + a microkernel SVC boundary + a first-class
x86 sim. Most (RTEMS, µC/OS, RT-Thread core) are flat-memory. **ThreadX Modules** and
**ChibiOS/SB** are the closest prior art and the references we lean on for the isolation model.

Key borrowed ideas, attributed:
- **NuttX** — `FLAT`/`PROTECTED`/`KERNEL` build-mode taxonomy (KickOS is a **PROTECTED build**:
  MPU + SVC, single physical space, no MMU); the `make export` dependency-inversion packaging;
  the toolchain-libc lesson (below).
- **Argon** — dual C/C++ API; static object allocation; the critical-section split
  (v7-m BASEPRI + `ldrex/strex`, v6-m PRIMASK).
- **RIOT** — "preemptive, tickless, priorities, optional RR within priority" (our exact model);
  the `native` host port (`ucontext` + signals) as the sim reference; `thread_flags`.
- **ChibiOS** — tickless `TIMEDELTA` timer; HAL/driver model; ChibiOS/SB MPU sandboxes.
- **µC/OS-III** — RR per-task quantum + yield-quantum; task-local signaling; built-in
  introspection; ISR deferred-post; application hooks.
- **RTEMS** — pluggable scheduler-policy interface (FIFO/RR now; EDF/rate-monotonic later);
  SMP-aware runqueues; newlib.
- **ThreadX** — preemption-threshold; Modules → loadable MPU-isolated user modules.
- **RT-Thread** — scalable/config-gated footprint; optional POSIX/CMSIS-RTOS2 compat.

### Toolchain-libc lesson from NuttX (fix for its C++/sim pain)

NuttX offers `CONFIG_LIBCXXTOOLCHAIN` (+ `LIBSUPCXX_TOOLCHAIN`) to link the toolchain's own
`libstdc++`/`libsupc++`. That was painful because those libs (and the libgcc unwinder) expect a
**newlib-style OS porting layer** below them. **Design rule: make the userspace C ABI
newlib-compatible.** Then "full C++" in userspace is an opt-in that links the toolchain's
`libstdc++` — no custom STL — and the same porting layer lets the sim ride host `libstdc++`
while routing `sbrk`/`write`/locks through KickOS.

---

## Repo layout

```
KickOS/
  CMakeLists.txt
  CMakePresets.json                 # sim / frdmk64f / picopi / f411disco / bluepill
  cmake/
    toolchain-arm-none-eabi.cmake
    toolchain-host.cmake
    kickos.cmake                    # object-lib + image (.bin/.uf2/.hex) helpers
  arch/
    include/kickos/arch/arch.h      # THE porting interface (extern "C" seam)
    sim/                            # host x86-64 backend
    arm/
      common/                       # NVIC/SCB, SVC dispatch, PendSV glue, SysTick
      armv6m/                       # M0+: PRIMASK crit, sw-clz, ARMv6 PMSA MPU, ctx-switch asm
      armv7m/                       # M3/M4: BASEPRI crit, CLZ, ARMv7 PMSA MPU, ctx-switch asm
      chip/{mk64f,rp2040,stm32f411,stm32f103}/  # startup, clocks, UART, linker, MPU, boot2(rp2040)
  kernel/
    include/kickos/                 # public kernel + syscall-number headers
    sched/  thread/  sync/  time/  mm/  syscall/  init/
  lib/
    libc/                           # freestanding: mem/str, small vsnprintf, heap, assert
    libcxx/                         # __cxa_* stubs, guards, operator new/delete
  user/
    include/                        # userspace API + syscall stubs
    apps/hello/                     # the sim/M1 demo (C++)
  boards/{sim,frdmk64f,picopi,f411disco,bluepill}/  # chip + arch + memmap + console + clocks
  docs/{architecture.md,porting.md}
  README.md
```

---

## The porting layer — `arch/include/kickos/arch/arch.h`

Small `extern "C"` interface every target implements (authoritative source:
`arch/include/kickos/arch/arch.h` — this is a summary). `struct arch_context` is opaque, sized
per-arch in `arch/<arch>/include/kickos/arch/context.h`.

- `arch_context_init(ctx, entry, arg, stack_base, stack_size, privileged)` — build an initial
  frame so the first switch-in "returns" into `entry(arg)`. ARM: fabricated register frame on
  the task PSP, `CONTROL.nPRIV=1` for unprivileged threads. Sim: a `ucontext_t` via
  `makecontext`. When `entry` returns the arch routes into `kickos_thread_return()`.
- `arch_switch(from, to)` — switch the running context. **May be deferred**: ARM pends PendSV
  and the register swap happens on exception return; sim swaps now, or at signal-exit when
  called from ISR context. Callers must not assume the switch completed on return.
- `arch_start(boot, first)` — enter the first thread from the boot context.
- `arch_irq_save()/restore()` + `arch_in_isr()` — critical section (RAII `IrqLock`). **v7-m:
  BASEPRI**; **v6-m: PRIMASK**. Sim: `sigprocmask`.
- `arch_timer_arm(deadline)` / `arch_timer_disarm()` + `arch_clock_now()` — monotonic clock +
  one-shot next-event timer. ARM: free-running TIM/DWT + compare (or SysTick). Sim:
  `clock_gettime(MONOTONIC)` + `timer_create`/`SIGALRM`.
- `arch_mpu_apply(regions, n)` + `arch_mpu_probe_addr()` — load per-task MPU regions on
  switch-in; per **chip**: K64F **SYSMPU**, RP2040 ARMv6-M PMSA, F411 ARMv7-M PMSA. Sim:
  `mprotect` over an mmap'd guard page. F103: no-op.
- `arch_syscall(nr, a0..a3)` — the user→kernel trap; runs `syscall_dispatch()` in privileged
  **thread** context so a blocking syscall is an ordinary synchronous switch (see the contract
  in `arch.h`). 64-bit args/results are split into `uintptr_t` halves (`sys/abi.h`), so no
  separate result-delivery seam is needed. ARM: SVC. Sim: privilege-flipping trampoline.
- `arch_irq_inject(irq)` — raise an emulated device line (sim: signal; ARM: pend NVIC).
- `arch_console_write`, `arch_idle_wait`, `arch_init`, `arch_shutdown` — console bottom edge,
  idle (WFI / `sigsuspend`), bring-up, halt.
- Kernel-provided callbacks the arch invokes: `kickos_isr_timer()`, `kickos_isr_irq(irq)`,
  `kickos_isr_fault(addr, is_write)`, `kickos_thread_return()`, `syscall_dispatch(...)`.

**ISA-neutral by design.** The interface names *concepts* (switch, syscall-trap, crit-section,
timer, mpu), never *mechanisms* — PendSV/SVC/BASEPRI live inside `arch/arm`, not in `arch.h`.
Litmus test: a non-ARM port (Renesas RX72M — software-interrupt context switch, `INT` syscall,
RX MPU) must fit the same seam with no signature changes.

---

## Scheduler (the core constraint)

**TCB:** saved SP/context ptr, `state` (READY/RUNNING/BLOCKED/SLEEPING/EXITED), `prio`
(+ `base_prio` for later prio-inheritance), `policy` (FIFO|RR), `slice_remaining`, intrusive
links (ready/wait/timer lists), stack bounds, **MPU region descriptors**, privilege flag.

**Ready queue:** array of per-priority FIFO lists + a priority bitmap. Highest = find-first-set.
ARMv7-m uses `CLZ`; **ARMv6-M (RP2040) has no CLZ** → software ffs/De-Bruijn fallback in
`armv6m/`.

**Pluggable policy interface (RTEMS-style).** The core owns *mechanism* (run state, context
switch, ready structure); the *policy* (which thread runs next) sits behind a small interface:
`pick_next()`, `on_ready(t)`, `on_block(t)`, `on_tick/quantum(t)`. **FIFO + RR ship first**
(priority bitmap + per-priority FIFO, optional per-task quantum); **EDF / rate-monotonic** drop
in later without touching `reschedule()`, IPC, or the arch layer. Optional per-thread
**preemption-threshold** (ThreadX) is a policy attribute. Runqueues are kept **SMP-ready**
(per-core) for RP2040 core1 later.

**`sched::reschedule()`** — the single decision point: ask the active policy for `pick_next()`;
if ≠ current, call `arch_switch(from, to)` (which may defer). No caller is privileged over another — **the tick
is not special.**

**Triggers (all equal):**
1. `thread_yield()` — voluntary.
2. Block on empty `sem_wait`/`mutex_lock`/`msg_recv` → wait queue → reschedule.
3. Wake from **thread** ctx: `sem_post` readies a waiter; if higher prio, reschedule now.
4. Wake from **IRQ** ctx: an ISR posts → mark reschedule-needed → PendSV switches on IRQ exit.
   **The headline "not the tick" path — a device interrupt drives scheduling directly.**
5. Timer expiry: a sleeping/timed-wait thread's deadline passes → readied → reschedule.
6. RR slice expiry (RR only) → reschedule among equal priority; each RR task has its own
   configurable **time quantum** + a yield-remaining-quantum call.

**Task-switch hook.** The single switch-in path reprograms per-task MPU regions and updates
introspection counters in one place, so MPU + stats can't drift from the actual switch. ISR
posts may run direct (scheduler-locked) or, later, deferred to a handler task.

**Tickless.** Monotonic clock from a free-running counter; a **delta list** of absolute
deadlines (ChibiOS `TIMEDELTA` model). Arm the one-shot timer for
`min(nearest deadline, running-RR slice expiry)` with a **minimum-delta guard** (never program a
compare that may already be in the past). Pure-FIFO with nothing time-pending ⇒ timer disarmed,
zero timer interrupts. `CONFIG_SCHED_PERIODIC_TICK` (opt-in) forces a classic periodic tick.
Idle thread at lowest prio: ARM `WFI`; sim `sigsuspend`.

---

## User/kernel separation

- Kernel privileged on **MSP**; threads on **PSP**. User threads unprivileged
  (`CONTROL.nPRIV=1`); kernel threads privileged.
- **Syscalls via `SVC`**: handler reads number + args (r0–r3), dispatches through an
  arch-independent **syscall table**, returns in r0. Sim: a trampoline flips an emulated-
  privilege flag (+ `mprotect` toggles kernel-mem accessibility) and calls `syscall_dispatch()`.
- **MPU per domain, first-class** (see *Memory domains* below): the running thread's domain
  region set is loaded by `arch_mpu_apply()` on every switch-in. A thread touching a domain
  region not granted to it faults → kernel reports. (M0 isolates granted **data** regions;
  per-thread private *stacks* — a sibling can't scribble another's stack — arrive with the M2
  `Domain` object, since M0 stacks still live in the kernel pool, not the arena.)
- **Sim isolation**: back "physical RAM" with one `mmap` arena; on every switch-in the running
  thread's regions are `mprotect`-ed (grant its domain data region, everything else no-access) so
  a cross-domain pointer raises `SIGSEGV`, translated into the same fault path. **Per-domain data
  isolation is enforced in the sim from M0**; grant *geometry* is validated (page-aligned arena
  sub-range) but grant *ownership* is spawner-asserted until M2 (a domain trusting a bad grant is
  the M2 credential model). The user↔kernel boundary can't be fully trapped in a Linux process
  (no CPU privilege), so a reserved arena page stands in for it; real user↔kernel enforcement is
  hardware (M2).

---

## Memory domains (the isolation unit)

The unit of memory isolation is a **memory domain** — a lightweight "process" in the
memory-boundary sense only (no MMU, no virtual memory, no `fork`/`exec`). Model borrowed from
**Zephyr `k_mem_domain`** / **ARINC 653** spatial partitions (temporal partitioning is *not*
implied); ThreadX Modules and ChibiOS/SB are the loadable-code cousins.

- **A domain owns a region set** — code (RX), data/heap (RW-NX), and any granted MMIO — plus a
  **privilege posture** (privilege is a property of the *domain*, not of an individual thread).
- **Threads belong to a domain and share its memory** (cooperative within a domain), **but each
  thread keeps its own private stack region** — a sibling cannot scribble another thread's stack.
  So a domain switch reloads the domain regions and every switch-in also loads the incoming
  thread's stack region; the MPU is therefore reprogrammed on **every** context switch (there is
  no "same-domain skip": the stack region differs per thread, and the privilege posture can
  differ moment-to-moment across the syscall boundary).
- **The kernel is a degenerate, static, privileged domain.** Its real protection is the ARM
  **background region** (`PRIVDEFENA`) / SYSMPU supervisor default, so it spends almost no
  explicit regions; MPU regions are spent describing what *unprivileged* code may reach.
- **Cross-domain sharing = a region deliberately mapped into two domains.** This is the basis for
  shared-memory IPC (roadmap). Absent an explicit shared region, domains cannot see each other.

**Region-set contract (portability keystone).** Region grants must be **non-overlapping**, and
`arch_mpu_apply(regions, n)` **replaces the whole active set** for the running thread. This is
the one contract that spans the hardware split: **K64F SYSMPU** grants are a *union* (any region
descriptor granting access wins), while **ARM PMSA** resolves overlaps by *region priority*
(higher-numbered region wins) — so KickOS forbids overlap and treats `arch_mpu_region.attr` as
the **unprivileged** access rights (supervisor access comes from the background region / SYSMPU
RGD0, not from these descriptors). With this, the same region set programs identically on SYSMPU
and PMSA. The seam signature does not change (RX72M litmus preserved).

**Region budget** (only **8** regions on ARMv6-M/v7-M; ~12 on K64F SYSMPU): kernel needs ~0
explicit regions (background map); a domain is ~3 (code, data/heap, optional MMIO) + 1 per-thread
stack + the fault guard — comfortably within 8. Data/heap placement uses linker sections so a
domain's RAM is one power-of-two-aligned PMSA region.

**Domains vs kernel instances (complementary, not competing).** The KickCAT whole-bus sim runs
many slaves, and **each slave is its own MCU → its own KickOS kernel instance**; several
instances co-reside in one host process (invariant #7, instance-scoped state — the KickCAT
`EmulatedNetwork`/`LoopbackSocket` path). **Memory domains isolate threads/apps *within* one
kernel (one MCU).** The two compose: N simulated MCUs (kernel instances), each internally
partitioned into domains. A slave is an *instance*, not a domain.

Status: **per-domain isolation is enforced in the sim as of M0** — `arch_mpu_apply` `mprotect`s
the user-RAM arena to the running thread's granted region set on every switch-in, and a
cross-domain write faults (CI-covered by the `selftest` domain stage). The current shape is a
per-thread granted region (`ThreadAttr.mem_base` / `kos_thread_params.mem_base`, backed by
`arch_ram_alloc`); the full `Domain` object (a shared region set several threads reference, +
per-thread private stacks) and per-**chip** hardware backends land in **M2**.

---

## Drivers & interrupts

**Core idea: an interrupt is an event that wakes a thread** — the same scheduler path as trigger
#4. The kernel owns the vector table and the real ISR; drivers attach through a kernel interrupt
API. Two flavors:

- **In-kernel drivers (privileged)** — bootstrap/core only: system timer, interrupt controller,
  MPU, and a **minimal debug console** (write-only, polling, unbuffered `putchar` for
  panic/early-boot/fault reporting — the standard microkernel exception, cf. seL4). Direct
  handler: `irq_attach(irq, handler, arg)` runs a privileged callback in handler mode. The
  **full UART driver** (IRQ-driven, buffered, RX/TX, multi-client) is a *userspace* driver, not
  this.
- **Userspace drivers (unprivileged — the goal)** — a user task granted, at creation:
  (1) **MMIO** — the device register block added to its MPU regions (device attrs, RW,
  no-execute); (2) **IRQ-as-event** — `irq_register(irq)` then loop `irq_wait(h)` / service /
  `irq_ack(h)`. The kernel's generic ISR stub masks the line, posts the driver's notification
  (sem/thread-flag), flags reschedule → PendSV switches to the now-ready driver task. The driver
  never runs in handler mode.

**API sketch (arch-neutral):** `irq_attach/detach` (in-kernel); `irq_register/wait/ack/unmask`
(userspace, handle-based — the C++ `kos::Irq` wraps the handle so a driver writes
`auto irq = kos::Irq::request(line); irq.wait();`, backed by the existing Semaphore as the
notification); backed by an interrupt-controller abstraction in the arch/chip layer (NVIC on ARM;
sim = signal-driven injection). Userspace never *injects* — reacting is `register`/`wait`, and
raw in-handler-mode callbacks are the privileged `irq_attach` (TCB, not defended). `irq_inject`
is only the sim's fake-a-device-firing mechanism (test scaffolding, gated/privileged; see 11a),
never a userspace primitive.

**Consistency payoff:** identical driver code in the **sim** (IRQ = injected event) and on
**hardware** (real NVIC line). This is exactly the KickCAT path: the ESC SYNC0/PDI IRQ (real) or
an EmulatedESC event (sim) wakes a userspace EtherCAT driver task that services the ESC and
feeds the slave app.

---

## C++ decisions

- **Kernel**: freestanding C++ — `-ffreestanding -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit`. Bring-up: run `.init_array` ctors in startup;
  provide `__cxa_pure_virtual`, `__dso_handle`/`__cxa_atexit` stub; `operator new/delete` mapped
  to the kernel heap or `=delete`d. No implicitly heap-allocating STL. `extern "C"` at
  asm/startup/syscall seams.
- **Userspace**: default freestanding subset; **each app may opt into full C++**
  (`-fexceptions -frtti`) by linking the toolchain's own `libstdc++`/`libsupc++` — works because
  the userspace C ABI is newlib-compatible.
- **libc**: one freestanding libc for kernel + userspace, identical on sim and target. Userspace
  additionally exposes a **newlib-compatible porting layer** (`_sbrk/_write/_read/_close/_fstat/
  _isatty/_exit/_kill/_getpid`, `__errno`, C++ guard/thread hooks) backed by KickOS syscall
  stubs — the seam that lets both toolchain and host `libstdc++` sit on top with their bottom
  edge routed through KickOS.

---

## Build system

- **CMake + Ninja.** Toolchain files `toolchain-arm-none-eabi.cmake` / `toolchain-host.cmake`;
  `CMakePresets.json` per board.
- Board select `-DKICKOS_BOARD=sim|frdmk64f|picopi|f411disco|bluepill`; the board file pins chip,
  arch, memory map, console driver, clock config, linker script.
- Static libs: `kickos_kernel`, `kickos_arch_<arch>`, `kickos_lib`, `kickos_user`
  (linked as one `--start-group`/`--end-group` set, since arch↔kernel reference
  each other).
- **Dependency-inversion packaging (the DX goal).** KickOS installs/exports a CMake package
  (config + libs + startup object + board linker script + flags). Consumption modes: **in-tree**
  (`add_subdirectory`) and **out-of-tree** (`find_package(KickOS)` / FetchContent / export
  tarball, à la NuttX `make export`).
- **Ergonomic bar: match NuttX's CMake export.** NuttX bakes the whole link recipe (flags,
  linker script, `_start` entry, `--start-group` of its libs) into the exported *toolchain*, so
  the app is a plain `add_executable`. KickOS offers the same feel two ways:
  ```cmake
  find_package(KickOS REQUIRED)              # or FetchContent
  add_executable(my_slave main.cc)
  target_link_libraries(my_slave PRIVATE kickos)   # `kickos` = the whole OS as usage reqs
  ```
  The exported `kickos` INTERFACE target carries the component link group + flags (sim: host libc
  threads). `kickos_add_application(<name> SOURCES… BOARD…)` remains as **optional sugar** and is
  where per-board image emission (`.bin`/`.hex`/`.uf2`) hangs on MCU targets. On the MCU side the
  linker script / `crt0` / entry live in the exported ARM toolchain (mirroring NuttX), so the
  same two lines yield a flashable image; switching sim↔MCU is a one-word `BOARD`/toolchain
  change. First-class acceptance criterion.
- **CTest** runs the sim ELF natively in CI.

---

## Roadmap

Bring-up order optimizes the inner loop: **x86 sim first**, then MCUs in priority order
(K64F → Pico → F411), then F103 (degraded). The sim runs the *same* kernel + userspace as the
MCUs — only the `arch/sim` backend differs.

**Hardware MPU is deferred to M2.** MCUs first come up with **privilege + SVC separation only**
(no per-task hardware MPU) to reach first-boot fast. The MPU *design* stays first-class
throughout (TCB regions, task-switch hook, enforced in the sim from M0); only per-chip hardware
enforcement lands in M2.

### Milestone 0 — x86 sim process (first deliverable)

A single native Linux ELF that boots the real kernel, schedules threads, and runs an
unprivileged user app across the SVC boundary — no hardware, runnable in CI.

0. **Git + license first**: `git init`, `LICENSE` (CeCILL-C), `.gitignore`, README skeleton,
   SPDX header template; initial commit. Commit at each step boundary (bisectable history).
1. **Skeleton + build**: repo tree, CMake multi-arch, `toolchain-host.cmake`, `sim` preset,
   `kickos_add_application()` helper + exportable CMake package.
2. **`arch.h`** porting interface.
3. **Sim backend** (study RIOT `native`): `ucontext` switch, signal-based timer IRQ +
   emulated device IRQs, `mprotect` MPU emulation over an mmap arena, syscall trampoline.
4. **Kernel core**: TCB, ready queue, `sched::reschedule()`, thread create/exit, idle, `kmain`,
   RAII `IrqLock`.
5. **Sync**: counting semaphore + mutex; block/wake driving reschedule incl. the IRQ-ctx post.
6. **Time**: monotonic clock, timer/delta queue, `sleep`, tickless next-event arming
   (min-delta guard), `CONFIG_SCHED_PERIODIC_TICK` opt-in, RR slice accounting.
7. **Syscall layer**: numbers header, table, `syscall_dispatch()`, newlib-compatible user stubs,
   SVC/trap wiring.
8. **Userspace demo `apps/hello`** (C++, unprivileged): `write` syscall; semaphore posted by a
   thread and by an IRQ handler (event-driven switch, no tick); RR round-robin; a wild write
   faulting via `mprotect`. Wired into CTest — the CI gate.

### Milestone 0.1 — instance-scope the runtime (before M0.2–0.4 / M1)

Structural prep for invariant #7, landed before MCU work so M1/M2 code is born instance-ready
(zero runtime cost; behavior unchanged).

8a. Aggregate the file-static runtime core (scheduler `g_ready`/`g_bitmap`/`g_current`/`g_idle`/
    `g_live`/`g_boot`/`g_policy`, the time delta list, the syscall object pools, and the sim
    arch's arena/timer/signal state) into a `Kernel`/`Instance` struct, reached through a
    `kernel()` accessor. The accessor is compile-time selectable: a single `static Kernel` on
    MCU / for size; a thread-local pointer per instance for the multi-slave sim. Objects (TCBs,
    sems) stay caller-owned. No functional change; the CI gate is unchanged output.
    *(Actual multi-instance sim — per-instance event delivery — is Later, see the sim end-goal:
    timer→`timerfd`, IRQ→`eventfd`, one `epoll` loop per instance thread; `SIGSEGV` stays but
    demuxes by faulting address. That removes the shared-signal problem and the
    signal-handler/deferred-switch gymnastics.)*

8b. Extract a reusable **intrusive-list node** (embed a `Node`/`ListLink` in the TCB + a small
    typed list helper with `container_of`) to replace the ad-hoc `qnext`/`qprev`/`tnext` fields
    threaded by hand through `rq_*` (`sched.cc`), the wait-queue `wq_*` (`sync.cc`
    `wq_push_back`/`wq_unlink`), and the timer delta list. `rq_*` and `wq_*` are literally the same
    doubly-linked-list ops on the same `qnext/qprev` node written twice; only the *policy on top*
    differs (`rq_rotate` vs `wq_pop_highest`'s priority scan), so unify the list mechanics and
    leave rotate/pop-highest as thin callers. Done here, with the scheduler already open, so the
    critical section is touched **once**. Preserve the share-vs-separate split deliberately — do
    **not** over-generalize into one node per list: `qnext/qprev` stays **shared** by ready *and*
    wait (a thread is on exactly one — ready XOR blocked; this exclusivity is what makes
    `detach_current()` the forcing function and is exactly the aliasing that caused the M0
    `block_on` corruption), while `tnext` stays a **separate** link-set so a timed wait can sit on
    the timer delta list *and* a wait queue at once (`sem_timedwait` later). A typed node makes
    "on two mutually-exclusive lists at once" a structural bug rather than a silent link-clobber.
    The **timer delta list** (`g_sleepq`, `sleepq_insert`/`sleepq_remove` in `time.cc`) is the
    third consumer, but shares only the *node*, not the ops: its **sorted insert is policy** (walk
    by `deadline_ns`), not the generic FIFO `push_back`. Keep it **singly-linked** (`tnext` only) —
    today there is no arbitrary removal (`sleepq_remove` is only ever called on the head from the
    expiry loop, so effectively O(1); `sleep()` always runs to expiry). Promote `tnext` to
    doubly-linked (add `tprev`) **only when `sem_timedwait`/timed condvars land** (Later): that is
    the case where a thread sits on the sleepq *and* a wait queue and is woken early by the
    wait-queue side, making **mid-list cancellation** the hot path (O(n) walk → O(1) with a
    backlink). Backlinks do nothing for the sorted *insert*, which stays O(n) regardless.
    Also drop the redundant `inline` on `highest_prio` (anon-namespace helpers don't need it), and
    replace the TCB's `void* wait_queue` with a **forward-declared `WaitQueue*`** (`struct
    WaitQueue;` in `thread.h`, pointer to incomplete type — no `sync.h` include, so no
    `thread.h`→`sync.h`→`thread.h` cycle, which is the reason it was `void*`). Today the field is
    only assigned; it becomes dereferenced at `sem_timedwait` (a timeout-woken thread must unlink
    from the queue it's parked on — read the pointer back *as* a `WaitQueue*`), so type it now. No
    functional change.

8c. Finish the **policy/mechanism split** (RTEMS ideal): move the ready structure (`rq_push_back`/
    `rq_remove`/`rq_rotate` + the priority bitmap) *into* the FIFO/RR policy, so the core keeps
    only run-state + `g_current` + the context switch and calls **only** `pick_next`/`on_ready`/
    `on_remove`/`on_yield`/`on_slice_expire` — no direct `g_ready` access. Removes the current
    hybrid coupling (core hard-wires the bitmap *and* calls policy hooks). FIFO/RR share the whole
    structure; RR == FIFO + `on_slice_expire` rotation (never invoked for FIFO: quantum 0 ⇒
    `slice_deadline = UINT64_MAX` ⇒ `tick_rr` early-returns). Guardrail: do **not** speculatively
    widen the hook interface for a hypothetical EDF — a plugin API needs a second plugin to be
    validated; keep the 5 hooks minimal now and revisit when EDF/rate-monotonic actually lands
    (Later). CI gate is the safety net. No functional change. **File layout:** move the FIFO/RR
    policy into its own TU (e.g. `kernel/sched/policy_fifo_rr.cc`) so `sched.cc` holds only
    mechanism (run-state, switch, `reschedule`) — the physical split mirrors the logical one and
    keeps the seam honest. If more policies land, promote to a `kernel/sched/policy/` subdir.
    Side benefit: the RR **slice/quantum** (currently core state with a `UINT64_MAX` "no
    deadline" sentinel that FIFO also carries) becomes RR-policy-owned; the time subsystem then
    asks the policy for its next timed event (FIFO contributes none), removing the sentinel and
    the notion of a "FIFO slice" from the core.

8d. Add a **`KICKOS_UNREACHABLE(msg)`** idiom (`kpanic`-based, `[[noreturn]]`, fail-LOUD with a
    message) and use it for the genuinely-dead control-flow traps — the `while (true) {}` after
    `reschedule()` in `exit_current`, and the one after `exit_current()` in `kickos_thread_return`.
    An impossible state (an EXITED thread rescheduled) must **halt loudly with a diagnostic**, not
    spin silently forever (a dead hang with zero information is the worst failure mode). Distinct
    from a *defensive guard* like the `g_live > 0` clamp, which prevents a would-be functional
    consequence (unsigned underflow ⇒ a kernel that never reaches its shutdown condition) and may
    stay a guard; `KICKOS_UNREACHABLE` is for "this can never happen — prove me wrong loudly."

8e. Split `config.h` by *audience* — three buckets, don't re-conflate them:
    - **structural / fixed** (design invariants, not knobs): `KICKOS_NUM_PRIO` (coupled to the
      `uint32_t` ready bitmap — bumping it needs a hierarchical bitmap, not a bigger number) and
      the derived `KICKOS_PRIO_IDLE/MIN/MAX`; also `KICKOS_MPU_MAX_REGIONS` (hardware-fixed). These
      are internal, not user-facing.
    - **user / app** (provisioning knobs, tunable per system, CMake-`-D` overridable):
      `KICKOS_MAX_SEMAPHORES`, thread-pool size, … — the "static-allocation-first, sized by the
      app" surface. Also hoist the periodic-tick period here as **`KICKOS_TICK_PERIOD_NS`** — it is
      currently a file-local `constexpr kTickPeriodNs = 1000000ull` (`time.cc:23`, the 1 ms tick),
      a buried tuning literal that belongs in config as a named, overridable knob (only meaningful
      in the `KICKOS_SCHED_PERIODIC_TICK` build variant).
    - **board / chip** (hardware-derived): `KICKOS_MAX_IRQ` and `KICKOS_TIMER_MIN_DELTA_NS` — these
      leave `config.h` entirely for the board layer at M1 / M2 (see 10, 12a).

8f. **`sleep(0)` should yield, not block.** In `ktime_sleep_ns` (`time.cc`) a zero duration
    currently flows through `ktime_sleep_until` and the min-delta guard turns it into a real ~20µs
    block — wrong for the common `sleep(0) == yield` idiom (FreeRTOS `vTaskDelay(0)`, RTEMS
    `wake_after(0)`): the caller means "relinquish to my peers and return," not "park me." Fix:
    `if (ns == 0) { sched::yield(); return; }`. Do **not** extend this to `0 < ns < min-delta` —
    those must keep rounding *up* to min-delta, because a delay API promises the caller is off-CPU
    for the requested time, whereas `yield()` returns immediately when no peer is ready (a spin, not
    a delay). The **overflow** saturation (`now + ns` wrap ⇒ `UINT64_MAX`) is correct and stays: it
    avoids the worst case (wrap → past deadline → min-delta turns a huge request into a ~20µs
    sleep); `UINT64_MAX` = "never wakes" is effectively a caller bug, optionally a `KICKOS_ASSERT`,
    but saturating is the safe default (2⁶³ ns ≈ 292 yr is already "never").

### Milestone 0.2 — IRQ-as-event, proven on sim (mechanism before silicon)

The IRQ mechanism is arch-neutral kernel plumbing, not a hardware detail — so it belongs on the
sim *before* M1, and M1's IRQ step then collapses to "swap the sim controller for the NVIC." Two
tiers, per the driver model (see "IRQ / driver model" above): tier 2 (in-kernel privileged
`irq_attach` callback) already exists on sim via `g_table`/`kickos_isr_irq`; tier 1 (unprivileged
userspace driver, IRQ-as-event) is the net-new work here. Scope this milestone to tier 1 + a
demonstrator for tier 2.

8g. **Thin interrupt-controller abstraction** — `mask(line)` / `unmask(line)` / `raise(line)` and
    *nothing more*. Deliberately minimal: do **not** model NVIC priority grouping, pending-vs-active,
    edge-vs-level, or tail-chaining yet — those are earned per-chip at M1 when real silicon
    validates them (the policy-hook lesson: don't shape an abstraction against one implementation).
    Sim backend: `mask` suppresses `SIGUSR1` delivery / drops the pending line; `unmask` re-enables.

8h. **IRQ-as-event kernel plumbing** — `irq_register(line) -> handle`, `irq_wait(h)`, `irq_ack(h)`.
    A handle binds a line to a notification; reuse the **existing Semaphore** (no new blocking
    primitive) — `irq_wait` is `sem_wait`, the first-level stub does `sem_post`. The generic
    first-level ISR stub: **mask the line → post the bound sem → flag reschedule**; the driver then
    runs in *thread* context (never handler mode), services the device, and calls `irq_ack` to
    unmask. C++ sugar: `auto irq = kos::Irq::request(line); irq.wait();`.
    **Latency invariant (protect it forever):** the object is bound to the line at register time, so
    the ISR wakes a **pre-bound target directly** — *never* a handle/table lookup in ISR context
    (this survives the M2 capability model, 12b: capabilities are cold arm-path only). The `Semaphore`
    is the M0.2 notification for simplicity; a single-waiter line (the common case) can later use a
    lighter **direct-to-thread notification** (per-thread pending bit + wake, coalescing — cf.
    FreeRTOS task notifications) instead of a full counting sem. Multi-object `wait_any` (Later) is
    a cold-path layer only threads that use it pay for; plain single-object `irq_wait` never does.

8i. **Fake sim device + userspace-driver selftest** — fabricate a sim "device": a word of
    "MMIO" (granted to the driver task via the existing `mem_base` region grant, RW) plus a line
    that `raise`s on a timer. The selftest then proves the whole tier-1 contract on the host,
    end to end: ISR → sem post → reschedule → **unprivileged** driver wakes in thread mode → reads
    its MMIO register → `ack` → line unmasks. This is the M1 de-risk: the contract is exercised
    before any NVIC exists.

### Milestone 0.3 — OS-agnostic app entry (NuttX-style; completes the dependency-inversion pillar)

Today the app defines `extern "C" void kickos_app_main(void)` — it names a KickOS symbol, so it
still *knows* it runs under KickOS. NuttX's model: the OS owns the true entry, the app writes a
plain `int main(int argc, char** argv)`, and the build renames it so the app source is
OS-agnostic (same source builds on KickOS, Linux, NuttX). This is the natural completion of item 8
(dependency inversion) — the kernel becomes invisible to the app.

8j. **Kernel owns the true entry, app owns `main`.** Sim: the libc-called `main()` does
    `arch_init → kmain → sched::start`. MCU (M1): the reset vector `__start` (linker
    `ENTRY(__start)`) does the same. The app writes a standard `int main(int, char**)`; the
    exported `kickos` usage target / `kickos_add_application()` injects **`-Dmain=kickos_app_main`**
    for the app TUs (NuttX's actual trick), so the app's `main` compiles to the symbol `root_entry`
    calls — no collision with the kernel's real `main` on the hosted sim. Kernel code is unchanged
    (still calls `kickos_app_main`); only the app-build flag and the entry signature move.

8k. **Sharp edges to settle here** (don't hand-wave):
    - **Linkage of the renamed symbol.** `main` is special-cased to an *unmangled* symbol; once
      `-Dmain=kickos_app_main` makes it an ordinary name, a **C++** app entry would mangle while a
      C app's would not. Pin it: kernel declares `extern "C" int kickos_app_main(int, char**)` and
      the contract requires the app entry reach that symbol. This changes the ABI seam from today's
      `extern "C" void(void)` → `extern "C" int(int, char**)`.
    - **argc/argv**: forward the sim's real host `argv` (bonus: sim apps take CLI args); pass
      `argc=0` / `argv=nullptr` on MCU.
    - **return value**: `main`'s `int` becomes the app exit status → `arch_shutdown(status)` on the
      sim (CI-friendly for single-shot apps), ignored for daemon-style apps that never return.
      Convert `hello`/`selftest` to plain `int main`; the CI checkers key off the exit status.

8l. **App console API — kill the imagined `stdout`.** Today apps write `kos_write(1, s, strlen(s))`,
    inventing a POSIX fd (`1 == stdout`) that has no backing — KickOS has no fd namespace, no
    stdout; there is only the debug console. There are **two distinct surfaces**, and they must not
    be conflated (as an earlier draft did):
    - **Ordinary output = libc stdio** (`printf`/`putc`/`fwrite`) → **stdout** → a **userspace
      console driver** (IPC). This is the "write a `main`, `printf`, done" low-barrier path. The
      bootstrap→migrate story lives *here*: today stdio's `_write` backend has no driver so it falls
      back to the debug console; when the driver lands the backend targets the **stdout stream
      handle** instead — app code *and* libc unchanged. At 12b `stdout`/`stderr` are
      **default-granted stream capability handles** (every app gets them → `printf` just works), and
      a Later POSIX shim can map `write(1,…)` onto them — real handles, not imagined integers.
    - **Dedicated debug output = `kos_print(s)` (C ABI) + `kos::print(...)` (C++ wrapper)**
      (mirrors `kos_sem_create`/`kos::Semaphore`), pointed **directly and permanently at the kernel
      debug console** — the developer escape hatch: *fast to reach* (immediate, zero setup, works in
      boot/panic, driver-independent), *slow as I/O* (unbuffered, polling). It does **not** migrate
      to the driver — being always-available is the whole point.
    Mechanics: `kos_print` does `strlen` in the *userspace stub* and calls a syscall with explicit
    `(buf, len)` — the kernel must **never `strlen` a user pointer** (unbounded, possibly
    cross-domain read); so the syscall is `kos_console_write(buf, len)` (no `fd`; `buf` bound-checked
    per M2 12). **Long-term the debug console is capability-gated** (12b): a general app cannot spam
    it freely; the cap is granted only to dev builds, system tasks, and the **console driver itself**
    (which needs it for its own diagnostics and as a pre-hardware fallback). **M0 demos:** `hello`/
    `selftest` are bring-up/debug programs with no console server, so they legitimately use
    `kos::print` *today* (openly debug output, not fake stdout); when 12b lands they get the cap
    granted or move to stdio.

### Milestone 0.4 — object lifecycle: settle the pooled-object pattern (before M1, on sim)

Not a feature — completes the **static-allocation pattern** that every pooled kernel object copies
(mutexes, thread pool, M0.2 IRQ handles, M2 memory domains). Today `sem_create` (syscall.cc:37) is
a monotonic **bump allocator** (`g_sem_count++`): no free, so the 16-slot pool is a one-way leak,
and `kos::Semaphore` (kos.h:59) is a *lying RAII type* — its ctor allocates a slot, its dtor frees
nothing. Settle the pattern once, on sim, before M1 replicates object pools across targets.

8m. **Freelist allocator + `sem_destroy`.** Replace the bump counter with a free-bitmap/freelist
    over `g_sems[]`. `sem_destroy(id)`: validate the id, then **reject with -1 if the semaphore has
    waiters** — *quiescent-only destroy*. This is deliberate: waking waiters with an error would
    need a failure-return channel on `sem_wait` (currently `void`), which is exactly the
    `wait_result` channel that timed wait introduces — keep that coupling in *one* place (timed
    wait, Later), and let M0.4 stay bounded. **New hazard the freelist introduces:** slot ids get
    reused, so a stale handle (id of a destroyed sem) can alias a fresh one — an ABA/use-after-free
    the bump allocator never had (ids were never recycled). Fail-loud fix: pack a small
    **generation counter** into the handle (id + gen); a stale handle fails validation loudly rather
    than silently poking a reused slot. **Migration-safe for the M2 capability model (12b):** keep
    the returned id **opaque** (userspace must not assume it's a global array index), and route every
    object lookup through **one validate-and-resolve chokepoint**, so swapping "global id → object"
    for "per-task handle → object" later touches a single place. The gen-counter here is a stopgap
    the handle table subsumes.

8n. **Settle `kos::Semaphore`'s ownership contract.** With destroy available, make it a *real*
    owning RAII type — ctor creates, dtor destroys, **non-copyable, movable** (so a moved-from
    handle doesn't double-free) — or, if we keep it a non-owning handle, say so explicitly and drop
    the RAII pretense. Recommend owning: it's the idiomatic C++ and kills the current
    scope-exit-leaks-a-slot trap. Whatever the pool's home is after 8a (Kernel struct), 8m/8n build
    on it.

### Milestone 1 — first MCU + remaining targets (privilege + SVC; no HW MPU yet)

9. **Toolchain**: install `arm-none-eabi-*` (+ `pyOCD`/`openocd`, later `picotool`);
   `toolchain-arm-none-eabi.cmake`, MCU presets.
10. **FRDM-K64F bring-up**: startup, MCG clocks, UART console, linker script, PendSV/SVC/SysTick
    + fault handlers; privilege + SVC only; flash via OpenSDA or pyOCD/openocd; run the same M0
    demo (UART output matches the sim, minus the enforced-MPU-fault case). The real NVIC is wired
    up here by implementing the **thin controller abstraction** (`mask`/`unmask`/`raise`, 8g)
    against it — the IRQ-as-event contract (8h/8i) is already proven on sim, so this step is
    "swap the sim controller for the NVIC," not a new mechanism. This is also where any NVIC
    reality the sim didn't model (priority grouping, pending-vs-active, edge-vs-level) is earned.
    And move **`KICKOS_MAX_IRQ`** (the in-kernel IRQ-table size) out of the global config into the
    **board/chip layer**, sized to that chip's NVIC line count (RP2040 ≤32 vs. K64F/F411 ~86) —
    the M0 global 32 is a sim placeholder, right-sized per chip here.
11. **RP2040/Pico** (boot2/XIP, ARMv6-M sw-clz), then **F411**, then **F103** — all
    privilege+SVC only — + docs.

11a. **IRQ-path error policy** (the real interrupt controller lands here). Today `kickos_isr_irq`
     silently drops both an out-of-range `irq` and an in-range one with no handler. Fix:
     (a) validate the user-supplied `irq` at the `KOS_SYS_irq_inject` *syscall boundary* (reject
     with -1) — the number is unprivileged-user-reachable, so it must be validated, **not**
     `KICKOS_UNREACHABLE`d (don't let a user halt the kernel); the in-kernel range check then
     stays a trusted-path guard. (b) Gate/privilege `KOS_SYS_irq_inject` itself — injecting a
     hardware IRQ from userspace is test scaffolding (real IRQs come from devices), so treat it
     like `guard_addr` (`KICKOS_ENABLE_SELFTEST` / privileged-only). (c) Define the
     **spurious / unhandled-IRQ** policy: an enabled line with no handler must be **reported and
     masked** at the controller (else it re-asserts → storm), not silently ignored. Implement via
     a **default handler (null-object)**: every table slot is initialized to a cheap default;
     `irq_attach`/`irq_detach` swap the slot to/from the real handler. This drops the null check
     (always a valid callback — not a perf win: the common path is one indirect call either way,
     the branch is predictable) and makes the default the single home for the spurious policy. The
     default runs in ISR context, so it must be **cheap/async-safe: bump a spurious counter + mask
     the line**, never console I/O (surface the counter later via introspection).

### Milestone 2 — hardware MPU enforcement (per chip)

12. `arch_mpu_apply()` chip backends wired into the task-switch hook: **K64F SYSMPU** first,
    then **RP2040 ARMv6-M PMSA**, then **F411 ARMv7-M PMSA**. Fault handler reports the offending
    thread/address; the wild-write demo traps on hardware, matching the sim. Complete the
    **memory domain** model (sim already enforces per-domain *data* isolation — M0 step 9):
    - TCB `regions[]` → `Thread → Domain*` (a shared region set) **+ per-thread private stacks
      carved from user RAM** (so a sibling can't scribble another thread's stack — not enforced in
      M0, where stacks live in the kernel pool);
    - **authenticated grant ownership** — a domain may only grant/share a region it owns (M0
      trusts the spawner's `mem_base`); a credential/handle model instead of raw pointers;
    - **syscall-argument (user-pointer) validation** — the kernel must bounds-check user pointers
      (`write` buf, `clock_now` out-ptr, spawn params) against the caller's regions, closing the
      confused-deputy path that M0's whole-arena syscall raise leaves open;
    - non-overlapping grants, `attr` = unprivileged rights, supervisor via background/RGD0, and
      **PMSA power-of-two size/alignment** for region placement (M0's byte-granular `arch_ram_alloc`
      suits SYSMPU/RX; v7-M needs pow2 regions — via linker sections or aligned allocation).

12a. Move **`KICKOS_TIMER_MIN_DELTA_NS`** out of the global config into the **board/arch layer**,
     tuned per chip (sub-µs on a hardware compare vs. the sim's ~20 µs signal-delivery floor),
     ideally derived from measured arm+dispatch latency rather than a hand-picked default. Note
     it also floors the finest RR quantum.

12b. **Capability handles — the object-side twin of the memory-grant hardening above.** Decision:
     KickOS moves off the current *global object id* (`sem_create` returns an id every task can name
     — ambient authority, the opposite of MPU-first isolation) to a **per-task typed handle table**
     à la Zircon `zx_handle_t` / seL4 capabilities: refcounted objects, rights bits, "destroy on
     last close." Memory grant : address range :: capability handle : kernel object — the same
     authenticated-per-domain-ownership problem (12's third bullet) applied to objects instead of
     RAM, so design them as twins here. **Why M2, not M0.4:** capabilities are an *enforcement*
     feature and enforcement is only real once the HW MPU constrains unprivileged userspace (M2);
     and by here the handle/rights ABI is designed against **all** object types that exist (sem +
     mutex from M0.1, irq-handle from M0.2, plus any M1/M2 additions), not over-fit to semaphores
     on the sim (the 8g abstraction-shaping lesson). Refcounting also dissolves the 8m/8n lifecycle
     hazards: "destroy on last close" replaces "reject destroy if waiters," and the handle table
     subsumes 8m's generation-tagged-id ABA workaround. **Cost, stated honestly:** per-task table
     RAM + a resolve indirection per syscall + refcount inc/dec — a deliberate isolation-vs-static-
     determinism trade, not a free win. Gates the Later **multi-object wait** (`wait_any` needs
     handles). Invariant to preserve from 8h: capability resolution is a *cold-path* (arm-time)
     concern — **never resolve a handle in an ISR**; the ISR wakes a pre-bound target directly.
     **Hard UX constraint (low-barrier pillar):** the runtime/root task must wire a **sane default
     capability set** for a plain app so a "hello world" *never* needs a hand-written manifest —
     cap customization is opt-in. Do not resurrect the CapDL-manifest-to-boot friction; that is the
     exact seL4 pain KickOS exists to avoid. **Concrete grounding example (the debug console, 8l):**
     the default set grants **stdout** (→ console driver) to *every* app so `printf` just works, but
     the **debug-console cap** (`kos::print` / `kos_console_write`) only to dev builds, system tasks,
     and the console driver — a general app can't spam the shared debug console. This is the least-
     abstract case for capabilities: it removes an ambient-authority output channel without adding
     any ceremony to the common `printf` path.

### Milestone 2.1 — docs housekeeping (after M2)

13. Split this document once the roadmap section has churned enough to read half-changelog: a
    timeless **`ARCHITECTURE.md`** (pillars, invariants, porting seam, scheduler/MPU/domain
    design, C++/build decisions) and a separate **`ROADMAP.md`** (milestones + status). Useful
    once external contributors/consumers appear — they want the design, not milestone
    bookkeeping. Not before: a premature split just adds cross-referencing overhead.

14. Final **comment cleanup pass**: purge time-bound / roadmap-flavored comments from the code
    ("M0 exercises…", "lands later", "M2 will…", "the userspace irq API lands later") — status
    belongs in `ROADMAP.md`, not scattered in source where it silently goes stale the moment a
    milestone ships. Rewrite each to state *timeless behavior/constraint* (per the comment rule:
    hidden constraints and invariants only, no circumstance narration).

### Later

Multi-domain isolation (the *Memory domains* model) + cross-domain shared-memory IPC;
message-passing IPC + userspace drivers (à la
ChibiOS/SB para-virtualized HAL + virtual IRQ); runloops + channels / multi-object waiting;
`thread_flags` / task-local signaling; built-in introspection (config-gated) + ISR deferred-post
handler task; a ChibiOS-style HAL/driver model; priority inheritance + preemption-threshold;
pluggable EDF / rate-monotonic policies; loadable MPU-isolated user modules (ThreadX-style);
optional POSIX / CMSIS-RTOS2 compat APIs; TLSF heap; validate a real `<vector>`/exception
userspace app against the toolchain `libstdc++`; RP2040 SMP (core1); Renode CI targets; and a
**Renesas RX72M** port (non-ARM `arch/rx`) as the arch-neutrality proof.

**Service publication (expose a userspace driver's interface across domains).** Transport (IPC) and
the bulk data path (cross-domain shared-memory grant) are listed above; the missing piece is how a
client in one domain *finds and is allowed to invoke* a server in another. Four parts: (1) **naming
/ discovery** — static-first (the root task distributes endpoint caps per a boot manifest, no
runtime name server; a dynamic name server à la QNX `name_attach` stays an optional later layer if
hotplug is ever needed); (2) **capability delegation** — IPC that *carries a capability* (12b cap
transfer), the mechanism that hands the client its endpoint cap; (3) **badged endpoints** — the
server gets a distinctly badged cap per client so it can authenticate/distinguish callers without a
separate identity path (the object-side twin of 12's authenticated grant ownership); (4) an
**interface convention** — hand-rolled message structs or a tiny IDL (cf. MIG/FIDL). A published
driver = **endpoint cap (control) + shared-memory grant (data)**.

**A-profile / MMU (VMSA) support alongside MPU (PMSA).** The north-star horizon: a page-table
address-space backend so KickOS runs on application-class cores (Cortex-A72 / RPi 4B — GICv2,
EL0/EL1, generic timer, BCM2711 board support). Aspirational; the near-term discipline is only
"keep PMSA specifics in the arch/chip layer so the Domain seam admits a VMSA backend without core
churn." A whole new arch family (`arch/aarch64`), well beyond the M0–M2 MCU roadmap.

**Timed wait (`sem_timedwait` / timed `mutex_lock` / condvar timeout).** Deferred deliberately: pure
kernel mechanism with *no* hardware payoff (runs identically on sim and MCU), so unlike M0.2 it
earns no "prove-on-sim-before-silicon" slot, and bring-up never needs it. It keeps surfacing only
because the TCB was *designed* with hooks for it — that recurrence is a sign the design is coherent,
not that it's urgent. When built, it lands as **one unit**, not piecemeal:
- promote `tnext` to **doubly-linked** (add `tprev`) — the sem-post-wins path removes the thread
  from the *middle* of the sleepq (cancel the pending timeout); O(n) walk → O(1) (see 8b);
- **dereference `wait_queue`** (now a typed `WaitQueue*`, 8b) on the timeout-wins path to unlink the
  thread from the queue it's parked on;
- deliver **timeout-vs-success via `wait_result`** — the same `sem_wait` failure-return channel that
  quiescent-only `sem_destroy` (8m) deliberately avoids needing early;
- get the **cancel-the-loser race** right under `IrqLock`: whichever of {post, timeout} fires first
  wakes the thread and must cancel the other source before it also fires.
- **Structural move:** don't bolt a timeout onto the semaphore. `sem_timedwait` is really "block on a
  queue *and* a timer, wake on the first, cancel the other" — a generalization of today's separate
  `block_current` (queue-only) and `ktime_sleep_until` (timer-only). Build it by **unifying the wait
  primitive**, and sleep/block/timed-wait all fall out of one mechanism.

---

## Verification

**Sim (automated, CI — `cmake --preset sim && ctest`):**
- Two-thread FIFO ordering; higher-priority thread preempts on ready.
- Semaphore post (thread-ctx *and* IRQ-ctx) triggers a switch **with the tick disabled**.
- RR round-robins equal-priority threads only when enabled.
- `sleep`/timed-wait ordering via the tickless timer queue.
- User-thread SVC roundtrip returns correct results.
- MPU violation caught and reported (via `mprotect`/`SIGSEGV`).
- **Dependency inversion**: an out-of-tree app builds against the exported KickOS sim package
  (`find_package` + plain `add_executable` linked to the `kickos` target) and runs.

**K64F / Pico / F411 (hardware):**
- **M1**: flash; UART output matches the sim (minus enforced-MPU-fault). GDB confirms MSP/PSP
  split, unprivileged `CONTROL`, SVC syscalls crossing the boundary.
- **M2**: per-task MPU regions loaded on switch-in; wild user write traps (SYSMPU bus fault on
  K64F; MemManage on ARM PMSA parts), matching the sim.

---

## Sim end-goal (forward-looking — not built yet, but constrains the sim design now)

The eventual purpose of the sim is to run a **KickCAT EtherCAT slave** (`../KickCAT`, C++17)
against a software **`EmulatedESC`** as an in-repo example — full-software EtherCAT testing.
KickCAT already ships a `freedom-k64f` slave example (NuttX + LAN9252 SPI), the same K64F that
is our #1 MCU, so the north star is one slave app running on KickOS/K64F (real SPI ESC) *and*
KickOS/sim (`EmulatedESC`).

What KickCAT needs from the OS is modest: a single-threaded event loop (`slave::Slave::routine()`
in `while(true)`), an optional timer/condvar, and the two-method `AbstractESC::read/write`.
`EmulatedESC` is pure software, so an in-sim slave needs no hardware bridge. KickCAT already has
an OS-abstraction layer (Linux/Windows/PikeOS/NuttX); the out-of-scope integration is adding a
KickOS backend.

Hosting shapes (both with KickCAT precedent): preferred = multiple `kernel+userspace` instances
in one process (KickCAT `LoopbackSocket` + `EmulatedNetwork`); fallback = multiprocess with IPC
(KickCAT `network_simulator` + TAP-over-shared-memory). This is why invariant #7
(instance-scoped state) and pluggable external event sources are honored from day one.
