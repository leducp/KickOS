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
**Eclipse ThreadX** (preemption-threshold, MPU-isolated loadable Modules), and **RTEMS**
(pluggable schedulers incl. EDF/rate-monotonic, SMP, newlib).

## Design pillars

- **Clear userspace/kernel separation** — privileged kernel, unprivileged threads, syscalls
  across an SVC boundary. Microkernel, not monolithic.
- **MPU as a first-class citizen** — per-task memory regions live in the TCB and are
  reprogrammed on every context switch.
- **Proper scheduling** — event-driven FIFO scheduling that can switch on *any* event (yield,
  block, semaphore post, device IRQ), **not** only a periodic tick. Round-robin is available;
  the tick is optional/forced, never the sole trigger → a **tickless** core.
- **First-class host/x86 "sim"** — kernel + userspace as one Linux process for hardware-free,
  CI-friendly testing (the thing NuttX makes painful; done cleanly here).
- **C++ first-class** — kernel in **freestanding C++** (`-fno-exceptions -fno-rtti`); userspace
  gets **full C++ as a per-app opt-in** (exceptions/RTTI allowed there).

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
7. **Instance-scoped state, no hard singletons.** Kernel + sim state hang off an instance
   handle, so multiple `kernel+userspace` instances can run in one host process (the KickCAT
   sim end-goal). Honored from the first sim code. (Target builds may collapse to a single
   static instance for size.)
8. **Dependency inversion — the app consumes the kernel.** The application owns the top-level
   build; KickOS is a prebuilt package (libraries + headers + startup + board linker script +
   flags) via a `kickos_add_application()` CMake helper. The app defines a known entry
   (`kickos_app_main()`) that the kernel's boot path calls after init.
9. **Conventions.** Traditional include guards `KICKOS_<PATH>_H` (no `#pragma once`); no ternary
   operators; comments only for hidden constraints/invariants. **Allman brace style**, enforced by
   the checked-in `.clang-format`, matched to the sibling projects `../KickCAT` / `../kickmsg`
   (4-space indent, indented namespaces + case labels, left-aligned pointers, leading-comma ctor
   init, preserved one-liners, `ColumnLimit: 0`); run `clang-format -i` on changed sources.

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

Small `extern "C"` interface every target implements:

- `arch_context_init(tcb, entry, arg, stack_top)` — build an initial exception/return frame so
  the first switch-in "returns" into `entry`. ARM: fabricated register frame on the task PSP,
  `CONTROL.nPRIV=1` for user threads. Sim: a `ucontext_t` via `makecontext`.
- `arch_switch_request()` — request a **deferred** context switch. ARM: set PendSV pending
  (switch runs in the PendSV handler at lowest priority, after all IRQs). Sim: flag + act on
  signal return / at a safe point.
- `arch_irq_save()/restore()` — critical section. **v7-m: BASEPRI + `ldrex/strex`** (near
  lock-free, IRQs stay on); **v6-m: PRIMASK** (no exclusive monitors on ARMv6-M). Sim:
  `sigprocmask`. Wrapped by RAII `IrqLock`.
- `arch_timer_arm(deadline)` / `arch_timer_disarm()` + `arch_clock_now()` — monotonic clock +
  one-shot next-event timer. ARM: free-running TIM/DWT + compare (or SysTick reload). Sim:
  `clock_gettime(MONOTONIC)` + `timer_create`/`SIGALRM`.
- `arch_mpu_apply(regions, n)` — load per-task MPU regions on switch-in; per **chip**: K64F
  **SYSMPU**, RP2040 ARMv6-M PMSA, F411 ARMv7-M PMSA. Sim: `mprotect` over the mmap'd RAM arena.
  F103: no-op.
- `arch_syscall_return(...)` — deliver the syscall result to the caller frame.

The SVC/trap **entry** is arch code that reads the syscall number + args and calls the
arch-agnostic `syscall_dispatch()`.

**ISA-neutral by design.** The interface names *concepts* (switch-request, syscall-entry,
crit-section, timer, mpu), never *mechanisms* — PendSV/SVC/BASEPRI live inside `arch/arm`, not
in `arch.h`. Litmus test: a non-ARM port (Renesas RX72M — software-interrupt context switch,
`INT` syscall, RX MPU) must fit the same seam with no signature changes.

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
if ≠ current, call `arch_switch_request()`. No caller is privileged over another — **the tick
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
- **MPU per task, first-class**: kernel keeps a few fixed regions (kernel code RX, kernel data
  RW-priv); background region off for unprivileged. Each TCB carries its own regions (task code
  RX, task stack/data RW-NX); `arch_mpu_apply()` reloads them on switch-in. A user thread
  touching kernel or another task's region faults → kernel reports.
- **Sim isolation**: back "physical RAM" with one `mmap` arena; user regions are `mprotect`-ed
  so a wild user pointer raises `SIGSEGV`, translated into the same fault path.

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
(userspace); backed by an interrupt-controller abstraction in the arch/chip layer (NVIC on ARM;
sim = signal-driven injection).

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
- Object libs: `kickos_kernel`, `kickos_arch_<arch>`, `kickos_lib`, `kickos_user`.
- **Dependency-inversion packaging (the DX goal).** KickOS installs/exports a CMake package
  (config + libs + startup object + board linker script + flags) plus a
  `kickos_add_application(<name> SOURCES… BOARD…)` helper that performs the final link and emits
  the image (host ELF for `sim`; `.bin`/`.hex` for STM32/K64F; `.uf2` for Pico). Consumption
  modes: **in-tree** (`add_subdirectory`) and **out-of-tree** (`find_package(KickOS)` /
  FetchContent / export tarball, à la NuttX `make export`).
- **Ergonomic bar: match NuttX's CMake export.** The consumer project should be ~3 lines:
  ```cmake
  find_package(KickOS REQUIRED)                       # or FetchContent
  kickos_add_application(my_slave SOURCES main.cc BOARD frdmk64f)
  # → flashable image; swap BOARD sim → host ELF, same sources
  ```
  Everything (arch, MPU/privilege posture, libc, linker script, image format) is carried by the
  package + `BOARD` — switching sim↔MCU is a one-word change. This is a first-class acceptance
  criterion.
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

### Milestone 1 — first MCU + remaining targets (privilege + SVC; no HW MPU yet)

9. **Toolchain**: install `arm-none-eabi-*` (+ `pyOCD`/`openocd`, later `picotool`);
   `toolchain-arm-none-eabi.cmake`, MCU presets.
10. **FRDM-K64F bring-up**: startup, MCG clocks, UART console, linker script, PendSV/SVC/SysTick
    + fault handlers; privilege + SVC only; flash via OpenSDA or pyOCD/openocd; run the same M0
    demo (UART output matches the sim, minus the enforced-MPU-fault case).
11. **RP2040/Pico** (boot2/XIP, ARMv6-M sw-clz), then **F411**, then **F103** — all
    privilege+SVC only — + docs.

### Milestone 2 — hardware MPU enforcement (per chip)

12. `arch_mpu_apply()` chip backends wired into the task-switch hook: **K64F SYSMPU** first,
    then **RP2040 ARMv6-M PMSA**, then **F411 ARMv7-M PMSA**. Fault handler reports offending
    task/address; the wild-write demo now traps on hardware, matching the sim.

### Later

Full MPU region policy & shared-memory IPC; message-passing IPC + userspace drivers (à la
ChibiOS/SB para-virtualized HAL + virtual IRQ); runloops + channels / multi-object waiting;
`thread_flags` / task-local signaling; built-in introspection (config-gated) + ISR deferred-post
handler task; a ChibiOS-style HAL/driver model; priority inheritance + preemption-threshold;
pluggable EDF / rate-monotonic policies; loadable MPU-isolated user modules (ThreadX-style);
optional POSIX / CMSIS-RTOS2 compat APIs; TLSF heap; validate a real `<vector>`/exception
userspace app against the toolchain `libstdc++`; RP2040 SMP (core1); Renode CI targets; and a
**Renesas RX72M** port (non-ARM `arch/rx`) as the arch-neutrality proof.

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
  (`find_package` + `kickos_add_application()`) and runs.

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
