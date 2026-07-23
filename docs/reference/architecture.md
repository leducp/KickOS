<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS -- Architecture

KickOS is a small **microkernel** RTOS with a clear userspace/kernel separation, MPU-first
per-task isolation, an event-driven **tickless** scheduler, and a **first-class x86 host
"sim"** that runs the real kernel + userspace as one Linux process.

It draws design ideas (studied, never copied -- see *Licensing*) from
**NuttX** (structure/porting/build-modes), **Argon RTOS** (C++ substrate),
**RIOT-OS** (tickless scheduler + native/host port), **ChibiOS** (tickless time-delta timers,
HAL, MPU sandbox), **microC/OS-III** (RR-within-priority + per-task quantum, task-local signaling,
introspection), **RT-Thread** (scalable footprint, device framework, POSIX/CMSIS-RTOS2 compat),
**Eclipse ThreadX** (preemption-threshold, MPU-isolated loadable Modules), **RTEMS**
(pluggable schedulers incl. EDF/rate-monotonic, SMP, newlib), and -- for the microkernel
paradigm itself -- **seL4** (capabilities, a minimal privileged kernel, IPC-centric design,
badged endpoints, static capability distribution) and **Zircon** (typed handles, channels,
`wait_many`).

## Design pillars

- **Clear userspace/kernel separation** -- privileged kernel, unprivileged threads, syscalls
  across an SVC boundary. Microkernel, not monolithic.
- **MPU as a first-class citizen** -- isolation is per **memory domain** (see below): threads
  share memory within a domain, domains are MPU-isolated from each other, and the MPU is
  reprogrammed on every context switch-in.
- **Proper scheduling** -- event-driven FIFO scheduling that can switch on *any* event (yield,
  block, semaphore post, device IRQ), **not** only a periodic tick. Round-robin is available;
  the tick is optional/forced, never the sole trigger -> a **tickless** core.
- **First-class host/x86 "sim"** -- kernel + userspace as one Linux process for hardware-free,
  CI-friendly testing: the real kernel runs under CTest on the host, no board or emulator needed.
- **C++ first-class** -- kernel in **freestanding C++** (`-fno-exceptions -fno-rtti`); userspace
  gets **full C++ as a per-app opt-in** (exceptions/RTTI allowed there).
- **Low barrier -- seL4's paradigm, not its ceremony.** The UX benchmark is the sibling
  **KickCAT**: easy to use, easy to tweak, no big machinery -- *you write a `main`, and that's it*;
  the provided userspace libc/runtime already does most of the job for a basic app (this is what
  M0.3's OS-agnostic `main` entry delivers). Two axes: **app authors** -- a plain app never writes
  a capability manifest to print "hello"; the runtime/root task wires a **sane default capability
  set**, and cap customization is opt-in for advanced users (easy things easy, hard things
  possible). **Porters** -- adding a CPU means implementing the small **arch/chip seam** (`arch.h`
  + mpu/irq/timer/context backends), not restructuring the kernel; tractable for anyone who's done
  a NuttX port. This is a hard constraint on the capability model: capabilities must hide
  behind defaults, never resurrect the "CapDL manifest just to boot hello world" friction that
  makes seL4 painful to start with -- and hard to bring up on a new core.

## North star (long-term direction)

KickOS aims to be a **minimal microkernel RTOS in the seL4 tradition**: the smallest useful
privileged kernel -- threads, protection domains, IPC, capabilities, IRQ routing -- with
*everything else* (filesystems, networking, console, device drivers) as **unprivileged userspace
servers reached by IPC**. The design choices already made are downstream of this goal, not
incidental: **capabilities, not fds or global ids** for object access (a per-task typed handle
table -- ambient authority contradicts the isolation pillar; see "Object model, capabilities &
IPC" below); a **deliberately minimal
syscall surface** (`read`/`open`/`socket` are userspace stubs over IPC to servers, never kernel
calls -- the debug console `write` is the sole sanctioned exception, cf. `seL4_DebugPutChar`);
and **services published by static capability distribution** (seL4/CapDL-style -- the root task
grants each client exactly the endpoint caps its manifest allows), with **badged endpoints** to
authenticate callers. This fits the static-allocation, deterministic-RTOS ethos far better than a
runtime name server.

**MPU-first, but not MPU-only.** Isolation ships on the MPU (PMSA, no address translation), but
the **Domain / address-space seam is kept backend-agnostic** so a **VMSA (page-table / MMU)**
backend can slot in one day for application-class cores -- e.g. **Cortex-A72 / Raspberry Pi 4B**
(GICv2, EL0/EL1, generic timer). This is *aspirational, not roadmapped*; its only claim on
present design is a discipline we already hold -- keep MPU/PMSA specifics in the **arch/chip
layer**, never leaked into the core or the syscall ABI (the same arch-neutrality the non-ARM
**RX72M** target exists to prove). "MPU-first per-task isolation" is the M0-M2 reality; "one
address-space abstraction, MPU *or* MMU behind it" is the horizon.

## Targets

KickOS runs on a host `sim` (x86-64) plus a fleet spanning five MCU ISAs. Each target earns its
place by proving a distinct point about the arch/chip seam:

- **FRDM-K64F** -- Cortex-M4F, NXP **SYSMPU** (byte-granular bus-master protection, `__MPU_PRESENT=0`
  -- not the ARM core MPU).
- **RP2040/Pico** -- Cortex-M0+, ARMv6-M PMSA core MPU.
- **STM32F411** -- Cortex-M4F, ARMv7-M PMSA core MPU.
- **STM32F103** -- Cortex-M3, *no MPU*: the degraded "privilege-only" build proving the arch
  layer degrades cleanly.
- **Renesas RX72M** -- RXv3, *non-ARM*, has an MPU: the architecture-honesty check that `arch.h`
  carries no ARM-isms, and a second, independent MPU backend for M2.
- **ESP32 (Xtensa LX6)** and **ESP32-C6 (RV32IMAC)** -- two more non-ARM ISAs. The C6 has RISC-V
  **PMP** (an M2 backend, shared with QEMU `virt`); the Xtensa part has neither a per-task MPU nor
  a privilege split, which forces the isolation model to treat both as *optional* per-arch
  capabilities (best-effort where absent).

**MPU hardware programming is target-specific (chip or arch), not one shared routine.** The
switch-in `arch_mpu_apply()` only **stashes** the incoming region set (shared, weak);
`kickos_arch_mpu_commit()` **programs the hardware** from the context-switch epilogue, after the
physical swap, and is the per-target override -- K64F SYSMPU, ARM PMSAv7 (RP2040/F411, the shared
weak default), RP2350 PMSAv8, C6/virt RISC-V PMP, RX72M MPU. (See `design-mpu-commit-deferred.md`.)

---

## Licensing & clean-room discipline (hard constraint)

- **License: CeCILL-C** (French, GNU LGPL-compatible, file-level copyleft). `LICENSE` + SPDX
  `SPDX-License-Identifier: CECILL-C` headers on every source file.
- **Inspired, never copy-pasted.** The inspirations carry assorted licenses (NuttX/microC-OS-III/
  RT-Thread = Apache-2.0, RIOT = LGPL-2.1, ChibiOS = GPL-3/Apache, ThreadX = MIT, RTEMS =
  BSD-style). We study their designs/algorithms/APIs and write our own code -- no lifting source,
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
4. **Identical userspace across arches (on target).** On an MCU, a plain userspace app links the
   **KickOS freestanding libc** (the zero-overhead default); an app that opts into full C++ instead
   links the **toolchain's** libc + libstdc++ (see the layered libc model in the toolchain-libc
   lesson below). The **sim is deliberately exempt**: it is a hosted ELF, so its arch backend and
   any full-C++ app ride **host libc/libstdc++** -- forcing "KickOS libc only" there buys nothing
   and would block the sim's whole purpose (running real userspace, e.g. a KickCAT slave). The
   uniformity that matters is the **arch-neutral syscall/porting seam**, not one libc binary.
5. **Static allocation first, heap optional.** Kernel objects support link-time-static
   placement (e.g. `ThreadWithStack<2048>`) so a system can run with the heap disabled.
6. **Dual API in userspace.** A plain C syscall layer with ergonomic C++ RAII wrappers on top.
7. **Instance-scoped state, no hard singletons.** Kernel + sim state hangs off an instance
   handle, so multiple `kernel+userspace` instances can run in one host process (the KickCAT
   sim end-goal). Kernel **objects** (TCBs, semaphores) are caller-owned, and the **runtime core**
   (scheduler/time/syscall pools/sim arch) is aggregated into a `Kernel`/`Instance` struct reached
   via a compile-time-selectable `kernel()` accessor (a static singleton on MCU / for size;
   thread-local per instance for the multi-slave sim). Full multi-instance in the sim additionally
   needs per-instance event delivery.
8. **Dependency inversion -- the app consumes the kernel.** The application owns the top-level
   build; KickOS is a prebuilt package (libraries + headers + startup + board linker script +
   flags) consumed as a plain `add_executable` linked against the exported `kickos` target -- or
   `kickos_cxx` for a full-C++ (exceptions/STL/RTTI) app (with `kickos_add_application()` as
   optional sugar). The app defines a known entry
   (`kickos_app_main()`) that the kernel's boot path calls after init.
9. **Conventions.** Traditional include guards `KICKOS_<PATH>_H` (no `#pragma once`); no ternary
   operators; comments only for hidden constraints/invariants. **Allman brace style**, enforced by
   the checked-in `.clang-format`, matched to the sibling projects `../KickCAT` / `../kickmsg`
   (4-space indent, indented namespaces + case labels, left-aligned pointers, leading-comma ctor
   init, east-const/west-volatile, `ColumnLimit: 0`) -- Allman *everywhere*, no one-liners; run
   `clang-format -i` on changed sources.

### How KickOS differs from its inspirations

None of them combine MPU-first per-task isolation + a microkernel SVC boundary + a first-class
x86 sim. Most (RTEMS, microC/OS, RT-Thread core) are flat-memory. **ThreadX Modules** and
**ChibiOS/SB** are the closest prior art and the references we lean on for the isolation model.

Key borrowed ideas, attributed:
- **NuttX** -- `FLAT`/`PROTECTED`/`KERNEL` build-mode taxonomy (KickOS is a **PROTECTED build**:
  MPU + SVC, single physical space, no MMU); the `make export` dependency-inversion packaging;
  the toolchain-libc lesson (below).
- **Argon** -- dual C/C++ API; static object allocation; the critical-section split
  (v7-m BASEPRI + `ldrex/strex`, v6-m PRIMASK).
- **RIOT** -- "preemptive, tickless, priorities, optional RR within priority" (our exact model);
  the `native` host port (`ucontext` + signals) as the sim reference; `thread_flags`.
- **ChibiOS** -- tickless `TIMEDELTA` timer; HAL/driver model; ChibiOS/SB MPU sandboxes.
- **microC/OS-III** -- RR per-task quantum + yield-quantum; task-local signaling; built-in
  introspection; ISR deferred-post; application hooks.
- **RTEMS** -- pluggable scheduler-policy interface (FIFO/RR now; EDF/rate-monotonic later);
  SMP-aware runqueues; newlib.
- **ThreadX** -- preemption-threshold; Modules -> loadable MPU-isolated user modules.
- **RT-Thread** -- scalable/config-gated footprint; optional POSIX/CMSIS-RTOS2 compat.

### Toolchain-libc lesson from NuttX (fix for its C++/sim pain)

NuttX ships its **own** libc (`libs/libc/`) whose headers are **deliberately not
newlib-compatible** -- its own docs warn that mixing them with another libc's headers "is bound to
cause you problems." NuttX still supports linking the toolchain's own C++ runtime
(`CONFIG_LIBCXXTOOLCHAIN` + `CONFIG_LIBSUPCXX_TOOLCHAIN`, alongside `CONFIG_LIBCXX`/`CONFIG_UCLIBCXX`),
but because that runtime was built against the *toolchain's* libc, hosting it over NuttX's own
libc yields a steady stream of **header/type/namespace ABI mismatches** -- conflicting
`div_t`/`ldiv_t` between `<cstdlib>` and NuttX's `<stdlib.h>`, `<cmath>` failing because toolchain
headers `using std::abs;` names NuttX never put in `std`, and having to *add* locale support so
libstdc++ can sit on top. A secondary tax is the runtime porting layer libsupc++/libgcc assume
(`_impure_ptr` from `vterminate.o`, unwinder locks).

**KickOS sidesteps the mismatch class by never putting toolchain C++ on our own libc.** The libc
strategy is layered: a plain app links KickOS's **freestanding** libc (no libstdc++); an app that
opts into full C++ links `libstdc++`/`libsupc++` over the **libc they were built against**. The
fleet is on **pinned vendor toolchains that are all newlib** -- Arm GNU Toolchain (ARM),
RISCStar (RISC-V), GNURX (RX) -- so the full-C++ opt-in links libstdc++/libsupc++ over their
native **newlib** on every arch, no per-toolchain libc special-casing; our `newlib_stubs.cc`
fits directly. **Design rule: keep KickOS's own libc newlib-*family*-compatible** so the *sim*
can ride host `libstdc++` and so freestanding/newlib interop stays clean. What remains on target
is only the well-trodden **syscall-stub** porting layer -- uniformly the newlib bottom edge
(`_sbrk`, `_write/_read/_close/_fstat/_isatty/_exit`, `_impure_ptr`/reent, `__malloc_lock` when
threaded, C++ guard/lock hooks) routed to KickOS syscalls. (Honest caveat: that stub tax is real
-- we sidestep the header/ABI class, not the bottom-edge class.)

---

## Repo layout

```
KickOS/
  CMakeLists.txt
  CMakePresets.json + cmake/presets/*.json   # per-arch/board presets (arm, host, riscv, rx, xtensa)
  cmake/
    toolchain-arm-none-eabi.cmake
    toolchain-host.cmake
    kickos.cmake                    # board -> arch/chip resolution + image (.bin/.uf2/.hex) helpers
  arch/
    include/kickos/arch/arch.h      # THE porting interface (extern "C" seam)
    sim/                            # host x86-64 backend
    arm/
      common/                       # shared Cortex-M glue (arch_arm_common.cc)
      armv6m/                       # M0+: PRIMASK crit, sw-clz, ctx-switch asm
      armv7m/                       # M3/M4/M4F: BASEPRI crit, CLZ, ctx-switch asm
      chip/{mps2,nrf51,mk64f,rp2040,stm32f411,stm32f103,stm32f302,sam3x8e,xmc4800}/
    rx/    rxv3/  chip/rx72m/        # Renesas RXv3 (SWINT switch, INT syscall)
    xtensa/ lx6/  chip/esp32/        # Xtensa LX6 (windowed ABI, no privilege split)
    riscv/ rv32imac/ chip/{virt,esp32c6}/  # RV32IMAC (mtvec demux, CLINT/PLIC, PMP)
  kernel/
    include/kickos/                 # public kernel + syscall-number headers
    sched/  thread/  sync/  time/  irq/  syscall/  init/  ktrace/  bench/
  lib/
    libc/                           # freestanding: mem/str, small vsnprintf, heap, assert
    libcxx/                         # __cxa_* stubs, guards, operator new/delete
    rtt.cc                          # SEGGER RTT backend (console ch0 + telemetry ch1)
  user/
    include/                        # userspace API + syscall stubs
    apps/                           # hello, selftest (TAP gate), stress, sched_exit, mpu_fault,
                                    #   fault, fp_switch, blink, bench, tele_*
  boards/<board>/                   # per-board descriptor: board.cmake (arch/chip/-mcpu)
                                    #   + optional board_config.h / <chip>.ld overrides
  docs/                             # README.md (map); book/ (how & why); reference/ (code-synced)
  README.md  roadmap.md  TODO.md  M1_state.md
```

---

## The porting layer -- `arch/include/kickos/arch/arch.h`

Small `extern "C"` interface every target implements (authoritative source:
`arch/include/kickos/arch/arch.h` -- this is a summary). `struct arch_context` is opaque, sized
per-arch in `arch/<arch>/include/kickos/arch/context.h`.

- `arch_context_init(ctx, entry, arg, stack_base, stack_size, privileged)` -- build an initial
  frame so the first switch-in "returns" into `entry(arg)`. ARM: fabricated register frame on
  the task PSP, `CONTROL.nPRIV=1` for unprivileged threads. Sim: a `ucontext_t` via
  `makecontext`. When `entry` returns the arch routes into `kickos_thread_return()`.
- `arch_switch(from, to)` -- switch the running context. **May be deferred**: ARM pends PendSV
  and the register swap happens on exception return; sim swaps now, or at signal-exit when
  called from ISR context. Callers must not assume the switch completed on return.
- `arch_start(boot, first)` -- enter the first thread from the boot context.
- `arch_irq_save()/restore()` + `arch_in_isr()` -- critical section (RAII `IrqLock`). **v7-m:
  BASEPRI**; **v6-m: PRIMASK**. Sim: `sigprocmask`.
- `arch_timer_arm(deadline)` / `arch_timer_disarm()` + `arch_clock_now()` -- monotonic clock +
  one-shot next-event timer. ARM: free-running TIM/DWT + compare (or SysTick). Sim:
  `clock_gettime(MONOTONIC)` + `timer_create`/`SIGALRM`.
- `arch_mpu_apply(regions, n)` + `arch_mpu_probe_addr()` -- `arch_mpu_apply` **stashes** the
  incoming region set on switch-in (shared/weak); `kickos_arch_mpu_commit()` **programs the
  hardware** from the switch epilogue, per **chip/arch**: K64F **SYSMPU**, ARM **PMSA** (v6-M/
  v7-M), RISC-V **PMP**, RX **MPU**. Sim: `arch_mpu_apply` `mprotect`s the arena directly
  (synchronous switch, no deferred commit). F103: no-op.
- `arch_syscall(nr, a0..a3)` -- the user->kernel trap; runs `syscall_dispatch()` in privileged
  **thread** context so a blocking syscall is an ordinary synchronous switch (see the contract
  in `arch.h`). 64-bit args/results are split into `uintptr_t` halves (`sys/abi.h`), so no
  separate result-delivery seam is needed. ARM: SVC. Sim: privilege-flipping trampoline.
- `arch_irq_inject(irq)` -- raise an emulated device line (sim: signal; ARM: pend NVIC).
- `arch_console_write`, `arch_idle_wait`, `arch_init`, `arch_shutdown` -- console bottom edge,
  idle (WFI / `sigsuspend`), bring-up, halt.
- Kernel-provided callbacks the arch invokes: `kickos_isr_timer()`, `kickos_isr_irq(irq)`,
  `kickos_isr_fault(addr, is_write)`, `kickos_thread_return()`, `syscall_dispatch(...)`.

**ISA-neutral by design.** The interface names *concepts* (switch, syscall-trap, crit-section,
timer, mpu), never *mechanisms* -- PendSV/SVC/BASEPRI live inside `arch/arm`, not in `arch.h`.
Litmus test: a non-ARM port (Renesas RX72M -- software-interrupt context switch, `INT` syscall,
RX MPU) must fit the same seam with no signature changes.

---

## Scheduler (the core constraint)

**TCB:** saved SP/context ptr, `state` (READY/RUNNING/BLOCKED/SLEEPING/EXITED), `prio`
(+ `base_prio` for later prio-inheritance), `policy` (FIFO|RR), `slice_remaining`, intrusive
links (ready/wait/timer lists), stack bounds, **MPU region descriptors**, privilege flag.

**Ready queue:** array of per-priority FIFO lists + a priority bitmap. Highest = find-first-set.
ARMv7-m uses `CLZ`; **ARMv6-M (RP2040) has no CLZ** -> software ffs/De-Bruijn fallback in
`armv6m/`.

**Pluggable policy interface (RTEMS-style).** The core owns *mechanism* (run state, context
switch, ready structure); the *policy* (which thread runs next) sits behind a small interface:
`pick_next()`, `on_ready(t)`, `on_block(t)`, `on_tick/quantum(t)`. **FIFO + RR ship first**
(priority bitmap + per-priority FIFO, optional per-task quantum); **EDF / rate-monotonic** drop
in later without touching `reschedule()`, IPC, or the arch layer. Optional per-thread
**preemption-threshold** (ThreadX) is a policy attribute. Runqueues are kept **SMP-ready**
(per-core) for RP2040 core1 later.

**`sched::reschedule()`** -- the single decision point: ask the active policy for `pick_next()`;
if != current, call `arch_switch(from, to)` (which may defer). No caller is privileged over another -- **the tick
is not special.**

**Triggers (all equal):**
1. `thread_yield()` -- voluntary.
2. Block on empty `sem_wait`/`mutex_lock`/`msg_recv` -> wait queue -> reschedule.
3. Wake from **thread** ctx: `sem_post` readies a waiter; if higher prio, reschedule now.
4. Wake from **IRQ** ctx: an ISR posts -> mark reschedule-needed -> PendSV switches on IRQ exit.
   **The headline "not the tick" path -- a device interrupt drives scheduling directly.**
5. Timer expiry: a sleeping/timed-wait thread's deadline passes -> readied -> reschedule.
6. RR slice expiry (RR only) -> reschedule among equal priority; each RR task has its own
   configurable **time quantum** + a yield-remaining-quantum call.

**Task-switch hook.** The single switch-in path reprograms per-task MPU regions and updates
introspection counters in one place, so MPU + stats can't drift from the actual switch. ISR
posts may run direct (scheduler-locked) or, later, deferred to a handler task.

**Tickless.** Monotonic clock from a free-running counter; a **delta list** of absolute
deadlines (ChibiOS `TIMEDELTA` model). Arm the one-shot timer for
`min(nearest deadline, running-RR slice expiry)` with a **minimum-delta guard** (never program a
compare that may already be in the past). Pure-FIFO with nothing time-pending => timer disarmed,
zero timer interrupts. `CONFIG_SCHED_PERIODIC_TICK` (opt-in) forces a classic periodic tick.
Idle thread at lowest prio: ARM `WFI`; sim `sigsuspend`.

---

## User/kernel separation

- Kernel privileged on **MSP**; threads on **PSP**. User threads unprivileged
  (`CONTROL.nPRIV=1`); kernel threads privileged.
- **Syscalls via `SVC`**: handler reads number + args (r0-r3), dispatches through an
  arch-independent **syscall table**, returns in r0. Sim: a trampoline flips an emulated-
  privilege flag (+ `mprotect` toggles kernel-mem accessibility) and calls `syscall_dispatch()`.
- **Syscall return ABI (`user/include/kickos/sys/errno.h`).** A syscall that can fail returns its
  error as the **negated** code `-KOS_Exxx`; a success -- a handle, a count, a byte-count -- is
  **non-negative**, so `rc < 0` is unambiguously an error and never aliases a valid handle/count
  (handles are bounded well under `INT_MAX`, counts stay small). The code set mirrors POSIX
  magnitudes -- `EPERM` `EBADF` `EINVAL` `EFAULT` `ENOMEM` `EPIPE` `EDEADLK` `EBUSY` -- plus
  **`EOWNERDEAD`**, the robust-mutex case: a mutex *acquired* while its prior owner died holding it,
  still returned negative (`-KOS_EOWNERDEAD`) for the caller to special-case as HELD. Two syscalls
  stay OUT of this scheme by return type: `ram_alloc` returns a pointer (every failure is NULL -- a
  negated errno cast to a pointer would be non-NULL) and `cpu_clock_hz`/`cpu_clock_set` return a
  u32 Hz whose 0 already means unknown / no-silicon-clock.
- **MPU per domain, first-class** (see *Memory domains* below): the running thread's domain
  region set is reloaded on every switch-in (`arch_mpu_apply` stashes it; `kickos_arch_mpu_commit`
  programs the hardware after the physical swap). A thread touching a domain
  region not granted to it faults -> kernel reports. (M0 isolates granted **data** regions;
  per-thread private *stacks* -- a sibling can't scribble another's stack -- arrive with the M2
  `Domain` object, since M0 stacks still live in the kernel pool, not the arena.)
- **Sim isolation**: back "physical RAM" with one `mmap` arena; on every switch-in the running
  thread's regions are `mprotect`-ed (grant its domain data region, everything else no-access) so
  a cross-domain pointer raises `SIGSEGV`, translated into the same fault path. **Per-domain data
  isolation is enforced in the sim from M0**; grant *geometry* is validated (page-aligned arena
  sub-range) but grant *ownership* is spawner-asserted until M2 (a domain trusting a bad grant is
  the M2 credential model). The user<->kernel boundary can't be fully trapped in a Linux process
  (no CPU privilege), so a reserved arena page stands in for it; real user<->kernel enforcement is
  hardware (M2).

---

## Memory domains (the isolation unit)

The unit of memory isolation is a **memory domain** -- a lightweight "process" in the
memory-boundary sense only (no MMU, no virtual memory, no `fork`/`exec`). Model borrowed from
**Zephyr `k_mem_domain`** / **ARINC 653** spatial partitions (temporal partitioning is *not*
implied); ThreadX Modules and ChibiOS/SB are the loadable-code cousins.

- **A domain owns a region set** -- code (RX), data/heap (RW-NX), and any granted MMIO -- plus a
  **privilege posture** (privilege is a property of the *domain*, not of an individual thread).
- **Threads belong to a domain and share its memory** (cooperative within a domain), **but each
  thread keeps its own private stack region** -- a sibling cannot scribble another thread's stack.
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
(higher-numbered region wins) -- so KickOS forbids overlap and treats `arch_mpu_region.attr` as
the **unprivileged** access rights (supervisor access comes from the background region / SYSMPU
RGD0, not from these descriptors). With this, the same region set programs identically on SYSMPU
and PMSA. The seam signature does not change (RX72M litmus preserved).

**Region budget** (only **8** regions on ARMv6-M/v7-M; ~12 on K64F SYSMPU): kernel needs ~0
explicit regions (background map); a domain is ~3 (code, data/heap, optional MMIO) + 1 per-thread
stack + the fault guard -- comfortably within 8. Data/heap placement uses linker sections so a
domain's RAM is one power-of-two-aligned PMSA region.

**Peripheral (MMIO) isolation is hardware-bounded.** Per-thread *memory* (SRAM) isolation is
uniform across the fleet, but per-thread *peripheral* isolation is only as strong as the silicon
allows, and the fleet splits on one fact: **does the chip's per-thread protection unit sit on the
CPU access path (so it sees MMIO addresses) or on a bus-slave port (so it does not)?** A CPU-side
unit (ARM PMSA, RISC-V PMP, RX MPU) checks every load/store address including the peripheral
aperture, so a granted peripheral window is a genuine per-thread capability and an ungranted
peripheral access faults. A bus-slave-side unit (K64F SYSMPU) never sees the peripheral bridge, so
it cannot gate peripherals at all -- a separate, coarser authority does, and that authority is not
per-thread. (Narrative + the worked K64F bring-up: `../book/peripheral-isolation-and-the-hardware-ceiling.md`.)

| Chip / unit | MPU covers MMIO? | Separate peripheral gate | Per-thread MMIO isolation | Evidence |
|---|---|---|---|---|
| XMC4800 -- ARM v7-M PMSA (CPU-side) | yes | none per-thread (some peripheral registers are PV-write-only at the bus -- a kernel/user split, not a per-master gate) | **yes** | silicon-proven: a granted USIC-SSC DEV window works + an ungranted peripheral poke faults MemManage (xmcspi loopback, 2026-07-17) |
| RISC-V PMP (qemu-riscv; ESP32-C6) | yes | ESP32-C6 APM/PMS (per security-mode, default-deny user; needs a one-time global open) | **yes** (PMP discriminates per thread; APM opened once) | PMP path proven on qemu-riscv; **C6 SRAM enforcement + per-thread peripheral isolation PROVEN on silicon** (18/18 + mpu_fault; `c6blink` drives the APM open + an 8 B PMP window, ungranted poke PMP-faults) |
| RX72M -- RXv3 MPU (CPU-side) | yes | none (PRCR is an unrelated write-latch, not a privilege gate) | **yes** | SRAM/domain enforcement silicon-proven (selftest 20/20 + mpu_fault cross-domain trap, 2026-07-17); a real granted peripheral window not yet run on silicon |
| K64F -- SYSMPU (bus-slave-side) | **no** | **AIPS PACR** (by privilege+master, per 4 KB slot, NOT per-thread) | **no** | silicon-proven: an unprivileged PIT access faults via AIPS while SYSMPU latches no error; clearing the slot's PACR SP bit then admits ALL user code |

Consequences: on a CPU-side-MPU chip an unprivileged userspace driver can be granted only its own
peripheral window (the MMIO-grant model, `design-task9-mmio-driver.md`). On **K64F** that model is
unavailable: the kernel can open a peripheral slot to user mode (clear the AIPS `PACR` `SP` bit) or
keep it supervisor-only, but once open it is reachable by *every* unprivileged thread -- there is
no per-domain peripheral boundary. K64F peripheral drivers therefore either accept coarse
(kernel-vs-user, per-slot) isolation or mediate through a kernel syscall; SRAM/domain isolation is
unaffected (SYSMPU still enforces it, 17/17). `ARCH_MPU_DEV` is meaningful only where the unit
carries a memory-type field (ARM PMSA: device + XN); it is a silent no-op on PMP and RX (R/W/X only).

**Rule 7 -- the grant path refuses kernel-reserved blocks.** Single-ownership of a peripheral is
only real if the kernel can *refuse* a grant overlapping a block it owns for life (timebase, IRQ
controller, MPU, clock/reset gates). The refusal is **mechanical and binds every granter,
privileged ones included** -- it is not "trust the granter". `grant_region_admissible(base, size,
attr, caller_privileged)` (`kernel/grant`) is the single-region policy: refuse size-0/wrap, refuse
ANY reserved-block overlap, then for a **device** grant require privileged + exactly one MPU
descriptor (no rounding) + not a bit-band alias, or for a **RAM** grant require natural
power-of-two alignment AND confinement to the user arena **for every caller** (no privileged
waiver). `domain_for` (`kernel/domain`) runs it at the **region-commit chokepoint** on the
prospective committed geometry before it allocates a domain slot; the caller-owned-stack path in
`thread_spawn` runs the same predicate on the stack region, and `thread_create` carries a backstop
assert. Each enforcing chip declares its owns-for-life set via `arch_reserved_blocks` (`arch.h`) --
there is **no weak default**, so an enforcing port that forgets one fails to *link* (affirmative
fail-closed); the set is owns-for-life only (a neutralize-then-grant watchdog is excluded unless
its tick feeds the timebase). On a **bit-band core** (`arch_bitband_present()` != 0) the overlap
test also covers each reserved block's word-per-bit alias image, and a device grant reaching
either alias window is refused. `grant_reserved_validate` asserts once at boot (`kmain`) that the
arena + app extents are reserved-disjoint. Under no enforcement (`KICKOS_HAVE_MPU=0`) the whole
module is inline no-op stubs, so the call sites pay zero flash. (Design + worked K64F PIT case:
`../design-m4-driver-model.md` sec.7.)

**Domains vs kernel instances (complementary, not competing).** The KickCAT whole-bus sim runs
many slaves, and **each slave is its own MCU -> its own KickOS kernel instance**; several
instances co-reside in one host process (invariant #7, instance-scoped state -- the KickCAT
`EmulatedNetwork`/`LoopbackSocket` path). **Memory domains isolate threads/apps *within* one
kernel (one MCU).** The two compose: N simulated MCUs (kernel instances), each internally
partitioned into domains. A slave is an *instance*, not a domain.

Status: **per-domain isolation is enforced in the sim as of M0** -- `arch_mpu_apply` `mprotect`s
the user-RAM arena to the running thread's granted region set on every switch-in, and a
cross-domain write faults (CI-covered by the `selftest` domain stage). The current shape is a
per-thread granted region (`ThreadAttr.mem_base` / `kos_thread_params.mem_base`, backed by
`arch_ram_alloc`); the full `Domain` object (a shared region set several threads reference, +
per-thread private stacks) and per-**chip** hardware backends land in **M2**.

---

## Drivers & interrupts

**Core idea: an interrupt is an event that wakes a thread** -- the same scheduler path as trigger
#4. The kernel owns the vector table and the real ISR; drivers attach through a kernel interrupt
API. Two flavors:

- **In-kernel drivers (privileged)** -- bootstrap/core only: system timer, interrupt controller,
  MPU, and a **minimal debug console** (write-only, polling, unbuffered `putchar` for
  panic/early-boot/fault reporting -- the standard microkernel exception, cf. seL4). Direct
  handler: `irq_attach(irq, handler, arg)` runs a privileged callback in handler mode. The
  **full UART driver** (IRQ-driven, buffered, RX/TX, multi-client) is a *userspace* driver, not
  this.
- **Userspace drivers (unprivileged -- the goal)** -- a user task granted, at creation:
  (1) **MMIO** -- the device register block added to its MPU regions (device attrs, RW,
  no-execute); (2) **IRQ-as-event** -- `irq_register(irq)` then loop `irq_wait(h)` / service.
  The kernel's generic ISR stub masks the line, posts the driver's notification
  (sem/thread-flag), flags reschedule -> PendSV switches to the now-ready driver task; the next
  `irq_wait` auto-re-arms the consumed line, so no explicit ack is needed (`irq_ack(h)` stays an
  OPTIONAL early re-arm that reacts sooner to a latched raise; the latch-and-coalesce contract
  keeps the event either way). The
  driver never runs in handler mode.

**API sketch (arch-neutral):** `irq_attach/detach` (in-kernel); `irq_register/wait/ack/unmask`
(userspace, handle-based -- the C++ `kos::Irq` wraps the handle so a driver writes
`auto irq = kos::Irq::request(line); irq.wait();`, backed by the existing Semaphore as the
notification); backed by an interrupt-controller abstraction in the arch/chip layer (NVIC on ARM;
sim = signal-driven injection). Userspace never *injects* -- reacting is `register`/`wait`, and
raw in-handler-mode callbacks are the privileged `irq_attach` (TCB, not defended). `irq_inject`
is only the sim's fake-a-device-firing mechanism (test scaffolding, gated/privileged),
never a userspace primitive.

**Class-driver leaf (shared register logic across the trust boundary).** Where the kernel and a
userspace driver run the same device register sequence, that logic is factored into a
**freestanding class-driver leaf** (`kickos_class_<chip>`, e.g. `xmc4800/class/usic_class.h`)
written to the kernel bar: POD state + free functions taking the instance context by **explicit
base** (`op(uintptr_t base, ...)`, never an internal instance index), **no** constructor/
destructor, **no** mutable static, no exceptions/STL -- so it links **unchanged from BOTH the
privileged TCB and an unprivileged userspace driver**. RAII-owning handles and STL ergonomics live
ONLY in the userspace service/inline wrapper above it; the class/service boundary IS the
constructor-freedom boundary. This is orthogonal to Rule 7: sharing the register *code* is a
link-time decision, refusing the resource *grant* is a runtime one -- the kernel-owned timer
instance is never granted even though its register code is shared. (Design:
`../design-m4-driver-model.md` sec.3-4, 6.)

**Consistency payoff:** identical driver code in the **sim** (IRQ = injected event) and on
**hardware** (real NVIC line). This is exactly the KickCAT path: the ESC SYNC0/PDI IRQ (real) or
an EmulatedESC event (sim) wakes a userspace EtherCAT driver task that services the ESC and
feeds the slave app.

---

## C++ decisions

- **Kernel**: freestanding C++ -- `-ffreestanding -fno-exceptions -fno-rtti
  -fno-threadsafe-statics -fno-use-cxa-atexit`. Bring-up: run `.init_array` ctors in startup;
  provide `__cxa_pure_virtual`, `__dso_handle`/`__cxa_atexit` stub; `operator new/delete` mapped
  to the kernel heap or `=delete`d. No implicitly heap-allocating STL. `extern "C"` at
  asm/startup/syscall seams.
- **Userspace**: default freestanding subset; **each app may opt into full C++**
  (`-fexceptions -frtti`) by linking the toolchain's `libstdc++`/`libsupc++` over the **toolchain's
  own libc** -- newlib on every arch (Arm GNU / RISCStar / GNURX are all newlib) -- so the C++
  runtime rides its native libc, no cross-libc header/ABI collision (see the NuttX lesson above).
  KickOS's own freestanding libc backs the default apps that never link libstdc++.
- **libc**: one freestanding KickOS libc for the kernel + freestanding userspace, identical on sim
  and target. Its ABI is kept **newlib-family-compatible** so the *sim* rides host `libstdc++` and
  newlib interop stays clean. The bottom edge is a syscall-stub porting layer
  (`_sbrk`, `_write/_read/_close/_fstat/_isatty/_exit/_kill/_getpid`, `_impure_ptr`/reent,
  `__malloc_lock` when threaded, and C++ guard/lock hooks) routed to KickOS
  syscalls: the same seam under both the sim's host `libstdc++` and a target full-C++ app's
  toolchain newlib -- one newlib seam fleet-wide. (The full seam detail: `docs/design-kickcat-k64f.md`.)

---

## Build system

- **CMake + Ninja.** Toolchain files `toolchain-arm-none-eabi.cmake` / `toolchain-host.cmake`;
  presets in `CMakePresets.json` + `cmake/presets/*.json`.
- Board select `-DKICKOS_BOARD=<board>` (or a preset); the board descriptor pins chip, arch,
  memory map, console driver, clock config, linker script.
- Static libs: `kickos_kernel`, `kickos_arch_<arch>`, `kickos_lib`, `kickos_user`
  (linked as one `--start-group`/`--end-group` set, since arch<->kernel reference
  each other).
- **Dependency-inversion packaging (the DX goal).** KickOS installs/exports a CMake package
  (config + libs + startup object + board linker script + flags). Consumption modes: **in-tree**
  (`add_subdirectory`) and **out-of-tree** (`find_package(KickOS)` / FetchContent / export
  tarball, a la NuttX `make export`).
- **Ergonomic bar: match NuttX's CMake export.** NuttX bakes the whole link recipe (flags,
  linker script, `_start` entry, `--start-group` of its libs) into the exported *toolchain*, so
  the app is a plain `add_executable`. KickOS offers the same feel two ways:
  ```cmake
  find_package(KickOS REQUIRED)              # or FetchContent
  add_executable(my_slave main.cc)
  target_link_libraries(my_slave PRIVATE kickos)   # `kickos` = the whole OS as usage reqs
  ```
  The exported `kickos` INTERFACE target carries the component link group + flags (sim: host libc
  threads); a full-C++ app links `kickos_cxx` instead (both sit over a posture-neutral `kickos_core`).
  `kickos_add_application(<name> SOURCES... BOARD...)` remains as **optional sugar** and is
  where per-board image emission (`.bin`/`.hex`/`.uf2`) hangs on MCU targets. On the MCU side the
  linker script / `crt0` / entry live in the exported ARM toolchain (mirroring NuttX), so the
  same two lines yield a flashable image; switching sim<->MCU is a one-word `BOARD`/toolchain
  change. First-class acceptance criterion.
- **CTest** runs the sim ELF natively in CI.

---

## Object model, capabilities & IPC

The object/credential model layered on the MPU enforcement. Enforcement is only meaningful once
hardware constrains unprivileged userspace, so this model is designed against *all* object types
that exist (semaphore, mutex, IRQ handle, memory grant), not over-fit to one. **Status: the
SEMAPHORE, PI-MUTEX (`CAP_MUTEX`), and ENDPOINT/IPC (`CAP_ENDPOINT`) capability paths are LANDED
(M3), silicon-validated under enforcement; each object pool was added additively via the recipe in
Book ch.8.2.** The contract below is code-synced to `kernel/include/kickos/cap.h`,
`kernel/syscall/cap.cc`, `kernel/syscall/syscall.cc`.

- **Per-task typed handle table, not global ids or fds.** A global object id every task can name
  is ambient authority -- the opposite of the isolation pillar. Each `Thread` embeds a fixed
  `CapEntry handles[KICKOS_MAX_HANDLES]` (default 8, floor 6 on the tiny boards); a `CapEntry` is
  8 bytes = (global object handle, `CapType`, rights, cap-gen). Handles are **opaque** to
  userspace (never assume an array index). The table is a pure per-task naming+rights layer that
  WRAPs the unchanged global object pools (`slotpool.h`), it does not replace them: object
  liveness is a global property (the pool + its refcount), capability possession is per-task. Cost,
  stated honestly: per-task table RAM + a resolve indirection per syscall + refcount traffic -- an
  isolation trade, not a free win.
- **One resolve chokepoint (`cap_resolve`).** It validates index / liveness / type / rights
  (`(rights & need) == need`), then WRAPs to the object pool, which re-checks the object-gen. Its
  precondition is the resolve contract: **the caller holds `IrqLock`, and the resolved pointer is
  used under the SAME continuous lock** -- so a concurrent `handle_close` cannot free the object
  between resolve and use. Two independent generations guard it: per-task **cap-gen**
  (use-after-close) plus global **object-gen** (use-after-destroy), both `uint16_t`.
- **Rights are three bits, each enforced at a real site (no dead field):** `CAP_WAIT` (`sem_wait`),
  `CAP_SIGNAL` (`sem_post`), `CAP_TRANSFER` (may be delegated). Memory R/W/X is NOT a cap right --
  it lives in the MPU region descriptor and is enforced by hardware; duplicating it in the cap
  would be the forbidden checked-twice field.
- **Lifecycle -- refcounted destroy-on-last-close.** `sem_create` allocates the object (refs = 1)
  and returns a full-rights (`WAIT|SIGNAL|TRANSFER`) cap handle into the creator's table.
  `KOS_SYS_handle_close(cap)` (type-agnostic, renamed from the old `sem_destroy`) drops MY handle:
  bump the slot cap-gen, empty the entry, `refs--`; the object frees only at the LAST close
  (refs -> 0). Thread exit closes every held cap (`cap_teardown`, called from `exit_current`) so
  no thread leaks references. A teardown-close that would strand a parked waiter LEAKS (floors
  refs at 1), never strands -- unreachable today (every parked waiter pins its own cap).
- **Well-known reserved cap indices (`user/include/kickos/sys/cap_index.h`, FROZEN).** Indices
  `[0 .. KICKOS_CAP_FIRST_DYNAMIC)` (today `[0..4)`) are reserved well-known slots: index 0 =
  `KOS_CAP_STDOUT` (the send-only console endpoint), 1..3 held for a future clock/service cap.
  An **own-create** (`sem`/`mutex`/`endpoint` create) scans placement from
  `KICKOS_CAP_FIRST_DYNAMIC`, so it can **never alias a reserved slot**; a reserved slot is seated
  ONLY by the kernel (`cap_install_defaults` seats stdout, and is the sole writer of index 0) or
  by explicit spawn delegation (indices 1..3). Userspace only *names* a reserved slot by these
  constants -- it never chooses the index. Frozen = never renumber; append by raising the last
  reserved index and `KICKOS_CAP_FIRST_DYNAMIC` together, keeping the range small (each reserved
  slot is one fewer dynamic slot, floored to >=1 by the `cap.h` static_assert).
- **B1 wire contract (8 apps depend on it):** a fresh child table has cap-gen 0 in every slot, so
  on a fresh table `handle == index`; delegation places delegated cap `i` at child index `i + 1`
  (so delegated caps fill the reserved range 1..3), and `cap_install_defaults` seats the stdout
  cap at index 0 only once the console is published (pre-publish it seats nothing -- a plain app
  needs no manifest and falls back to `kconsole_write`). Delegation rides
  `kos_thread_params.caps` (each entry `(source_cap, rights_mask)`),
  requires the source cap carry `CAP_TRANSFER`, and NARROWS rights subset-only (`child.rights =
  parent.rights & mask`; a mask adding a bit the parent lacks is rejected, never widened); the
  WHOLE list is validated before the child slot is claimed (no half-populated child, no dangling
  ref bumps).
- **Resolution is cold-path.** A handle is bound to its target at arm time; an ISR **never**
  resolves a cap -- `irq_attach` resolves the cap ONCE (requires `CAP_SIGNAL`) and stores the
  GLOBAL object handle in the binding, which `irq_sem_post` re-resolves from the pool per fire
  (an ISR runs on a random interrupted thread's table, so `cap_resolve` from ISR context is
  meaningless). Capabilities are an arm-path concern only.
- **Authenticated grant ownership** is the memory-side twin: a domain may grant/share only a
  region it owns. Same problem as handles, applied to RAM instead of objects; designed together.
- **Low-barrier is a hard constraint.** A plain app never writes a capability manifest: the
  runtime/root task wires the default cap set (`cap_install_defaults`), which seats the stdout cap
  at reserved index 0 once the console is published and nothing before -- `write`/`printf` stays a
  direct syscall until console handover (#4) gives it a cap argument. Customization is opt-in
  delegation. Do not resurrect CapDL-manifest-to-boot friction -- the exact seL4 pain KickOS
  exists to avoid.

**Synchronization surface -- one blocking primitive, not an object zoo.** The kernel exposes the
minimum that genuinely needs scheduler involvement: a cap-named **blocking wait/wake** object (the
counting semaphore, i.e. the seL4-notification shape). Everything richer is built in userspace
(mutual exclusion = a binary wait/wake; condvar / rwlock / barrier = userspace state + the
primitive). Admission test for any *new kernel* sync primitive: does it require kernel state or a
scheduler action userspace cannot safely perform? If not, it stays in userland. The one typed
object justified beyond the primitive is a **priority-inheritance mutex** -- PI *is* a scheduler
action (boost the holder to the highest waiter's priority, restore on release), so it cannot live
in userspace; a mutex *without* PI earns nothing over a binary semaphore and must never be a
distinct kernel object. **ISR asymmetry:** the wait/wake `post` is ISR-safe (an ISR readies a
thread); the PI-mutex lock/unlock are thread-context only (an ISR owns no thread identity).

**Syscall-argument validation (the soundness floor).** Enforcement is only sound if the kernel
never trusts a user pointer: copy-in user structs via a checked read, bound-check writable
out-pointers and buffers against the caller's granted regions, and copy user strings into
fixed kernel buffers rather than aliasing (a fault reporter that `%s`-prints an aliased user
pointer is an info-leak oracle). Handles then resolve at that already-checked boundary. This
closes the confused-deputy path a whole-arena syscall raise would leave open.

**Console device handover.** The kernel owns the buffered debug console at boot. Because two
drivers cannot share one peripheral, a three-state ownership axis `g_console_state`
(KERNEL_OWNED / USER_OWNED / RECLAIMED) gates `console_emit` ahead of its buffered-vs-sync
decision: in USER_OWNED the kernel touches the device on no path (RTT still carries kernel
output). The privileged syscall `kos_console_publish` performs the handover -- it
**relinquishes** the buffered path via `console_tx_deinit` (flush, disable the TX interrupt,
detach/NVIC-mask, disarm), takes a kernel ref on the userspace driver's stdout endpoint, then
flips the state to USER_OWNED last; a stale chip writer that raced the flip is drained (via the
`g_chip_writers` count, with the publisher yielding at lowered priority so a lower-priority
writer can finish) before publish returns. The **field panic path reclaims** the UART:
`kpanic_enter` calls `arch_console_reclaim` and flips to RECLAIMED, and `kickos_isr_fault`
funnels through `kpanic_enter` so a terminal fault in the driver still reclaims and polled-prints;
the diag LED stays the always-present 1-bit last resort. Still to build: the per-chip
`arch_console_reclaim` bodies (force-retake the peripheral and rewrite every in-window register
to a known baud/state, since userspace config is untrusted -- weak no-op today) and the userspace
UART driver itself. Routing userspace output through a kernel syscall to "share" the device is
rejected -- an ambient-authority console service contradicts the microkernel split.

**Service publication.** A published userspace driver = **endpoint capability (control) +
shared-memory grant (data)**. How a client finds and may invoke a server across domains: (1)
**naming/discovery** -- static-first, the root task distributes endpoint caps per a boot manifest
(a dynamic name server is an optional later layer); (2) **capability delegation** -- IPC that
carries a capability hands the client its endpoint cap; (3) **badged endpoints** -- a distinctly
badged cap per client lets the server authenticate callers without a separate identity path (the
object-side twin of authenticated grant ownership); (4) an **interface convention** -- message
structs or a tiny IDL.

---

## Verification

**Sim (automated, CI -- `cmake --preset sim && ctest`):**
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

## Sim end-goal (forward-looking -- not built yet, but constrains the sim design now)

The eventual purpose of the sim is to run a **KickCAT EtherCAT slave** (`../KickCAT`, C++17)
against a software **`EmulatedESC`** as an in-repo example -- full-software EtherCAT testing.
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
