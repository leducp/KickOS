<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS -- Architecture

KickOS is a small **microkernel** RTOS with a clear userspace/kernel separation, MPU-first
per-domain isolation, an event-driven **tickless** scheduler, and a **first-class x86 host
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
incidental: **capabilities, not fds or global ids** for object access (a per-thread typed handle
table -- ambient authority contradicts the isolation pillar; see "Object model, capabilities &
IPC" below); a **deliberately minimal
syscall surface** (`read`/`open`/`socket` are userspace stubs over IPC to servers, never kernel
calls -- the debug console `write` is the sole sanctioned exception, cf. `seL4_DebugPutChar`);
and **services published by static capability distribution** (seL4/CapDL-style -- the root task
grants each client exactly the endpoint caps its manifest allows), with **badged endpoints** to
authenticate callers. This fits the static-allocation, deterministic-RTOS ethos far better than a
runtime name server.

**MPU-first, but not MPU-only, and the MMU half is no longer aspirational.** Most of the fleet
isolates on an MPU (PMSA, no address translation), and the **Domain / address-space seam is kept
backend-agnostic** so that a **VMSA (page-table / MMU)** backend slots in behind the same
abstraction. **Two such backends now ship**: `armv8a` on `virt_arm64` (VMSAv8, EL0/EL1, a fixed
granule and level count) and `rv64imac` on `virt_rv64` (Sv39 or Sv48, selected per config variant).
A chip declares which family it is by shipping `arch/<family>/chip/<chip>/mpu.cmake` for region
descriptors or `aspace.cmake` for translation, never both, and the two are mutually exclusive at
configure time. What has NOT been built is a translating backend on real application-class silicon:
both translating boards are emulated, so the discipline the seam was designed for is proven under
QEMU and not on a part. The design claim the seam makes is unchanged and now measured rather than
promised -- keep MPU/PMSA and VMSA specifics in the **arch/chip layer**, never leaked into the core
or the syscall ABI (the same arch-neutrality the non-ARM **RX72M** target exists to prove). "One
address-space abstraction, MPU *or* MMU behind it" is the shape the tree has; see
`../design-m6-mmu.md` for what each backend actually does and `boards.md` for what is witnessed
where.

### Non-goals -- seL4 machinery deliberately NOT adopted

"In the seL4 tradition" is a statement about the *shape* of the system, not a commitment to its
mechanisms. Four are refused, and each is refused on **arithmetic**, not on taste -- which matters,
because "we are seL4-like" otherwise reads as a promise that these are coming.

- **No untyped memory and no `Retype`.** The kernel owns the arena and bump-allocates it, by
  design. Accountability for who may obtain memory comes from Rule 7 grant admission plus the two
  ways a region reaches a thread -- a spawn grant, and self-grant -- both gated on `AUTH_MEMORY`.
  Untyped/Retype answers "which memory may this task obtain and from whose budget", and a
  single-owner static arena answers it already, without a derivation object per allocation.
- **No CNodes and no hierarchical CSpace.** A capability handle is one flat unsigned 32-bit word
  (`kos_cap_t`) packing a **16-bit** table index against a **16-bit** generation
  (`kernel/include/kickos/cap.h`). Both widths are FIXED fleet-wide and neither is derived from
  `KICKOS_MAX_HANDLES`, so the same logical capability prints the same value on every board. The
  word is spendable in full because the handle no longer travels in an errno-carrying return: a
  minting syscall returns a status and writes the handle to an out-parameter. One index value is
  reserved -- the all-ones index is never a slot, which caps a table at `2^16 - 1` entries and is
  what makes `KOS_CAP_AUTHORITY` and `KOS_CAP_NONE` unmintable. **No board states a width at all**:
  a board states only its `KICKOS_CAP_TABLE_SUPPLY`, and the width is summed at configure from four
  declarations -- the kernel's reserved range, the chosen service list's retention, the widest app
  peak in the tree, and the widest declared peak of concurrent INBOUND reply capabilities (**0 by
  default**, declared by nothing in tree) -- then RAISED to the grant-list floor
  `KICKOS_MAX_SPAWN_GRANTS + 1` whenever the sum falls below it, and checked against that supply
  (`cmake/cap_table.cmake`). So the width is a property of the IMAGE, not of the board: the same
  board configures a different one for a different service list or a different widest app. The fleet
  configures **three** widths -- **7**, **10** and **11** -- and widening a table no longer
  renumbers anything.
  Hierarchical CSpace exists to address a
  space too large to index directly; at that size the guard/radix machinery would cost more than
  the space it organises. What actually keeps tables small is not the field width but the
  **static** allocation: one slab holds a run per possible thread. A run is NOT one width -- root
  gets the summed `KICKOS_MAX_HANDLES`, every spawned thread gets `KICKOS_CAP_CHILD_WIDTH` -- so the
  slab is `((KICKOS_MAX_THREADS + 2) x child chunks + root's extra chunks) x 8 x 8` bytes of
  `.bss`. Re-derive it from the `KickOS: cap table =` line at configure rather than from this
  formula.
- **No derivation tree and no recursive revoke.** Refcounted last-close plus cap-generation
  staling already gives the property that matters (a revoked capability stops working, and a stale
  handle cannot be resurrected). A derivation tree buys *transitive* revoke, which needs
  per-capability parentage, and `CapEntry` is a fully-spent 8 bytes with nowhere to put a parent
  link: the object handle, type, rights and cap-gen, plus a `CAP_REPLY`'s 8-bit call sequence split
  into the spare bits beside the type and the rights -- and a DEAD entry's `obj` word carries the
  run's free-list links, so even an empty slot has nothing free. A problem a bigger table makes
  worse rather than better.
- **No per-instance (per-pin, per-clock, per-line) capabilities.** Roughly **100 muxable pins** on
  the larger parts, and the cost is not addressing -- the index field would reach them -- it is
  that a run is **statically reserved for every possible thread**, in whole chunks: 100 slots rounds
  to 13 chunks = 104 reserved slots per run. Per-thread width does not rescue this -- the pins would
  be held by a driver thread, and a thread that holds them needs the width -- so at
  `(KICKOS_MAX_THREADS + 2) x 104 x 8` bytes the table is **14,976 bytes** of
  `.bss` at the fleet's `KICKOS_MAX_THREADS` of 16, and still 3,328 bytes on the two-thread tiny
  boards, against a measured boot arena of 6,560 bytes on `bluepill-c8` -- which those boards would
  never reach anyway, since their supply is 7 and configure refuses the sum outright. Worse, the
  requirement is per *thread*: a bring-up body
  muxing thirty pins needs thirty live slots in its own table, so the cost scales with
  **concurrency** and not with the pin count -- which is what makes it unanswerable at any table
  size. So authority is granted per *class* -- the six bits `AUTH_MEMORY`, `AUTH_PINMUX`,
  `AUTH_PSTATE`, `AUTH_IRQ`, `AUTH_SYSTEM`, `AUTH_CONSOLE` -- rather than per instance. The
  consequence is honest and worth stating: a holder of `AUTH_PINMUX` may mux **any** pin.

The common thread is that a run is statically reserved for every possible thread -- the TCB holds only
the chunk *directory*, and the entries live in one slab carved at boot -- which is what holds the
fleet's three configured ROOT widths at 7, 10 and 11 slots regardless of what the handle codec
could address. A spawned thread is narrower still: it gets `KICKOS_CAP_CHILD_WIDTH`. If that ever changes, two
of the four deserve revisiting -- the CNode and per-instance bullets. The untyped-memory bullet
never depended on table size at all, and the derivation-tree bullet gets *stronger* as tables
grow. But the
static-allocation ethos is not going anywhere, so the honest expectation is that these stay refused.

## Targets

KickOS runs on a host `sim` (x86-64) plus a fleet spanning five MCU ISAs. Each target earns its
place by proving a distinct point about the arch/chip seam:

- **FRDM-K64F** -- Cortex-M4F, NXP **SYSMPU** (byte-granular bus-master protection, `__MPU_PRESENT=0`
  -- not the ARM core MPU). It is also the chip that proved a protection unit on a *bus-slave*
  port cannot gate peripherals at all (see *Memory domains*).
- **XMC4800** -- Cortex-M4F, ARMv7-M PMSA: the M2 reference pair's ARM half, and the canonical
  per-thread peripheral-isolation proof (a granted MMIO window works, an ungranted poke faults).
- **RP2040/Pico** -- Cortex-M0+, ARMv6-M PMSA core MPU (8 regions).
- **RP2350/pizero2350** -- Cortex-M33: ARMv8-M **PMSAv8**, a different descriptor shape
  (`base`+`limit` with MAIR indirection, not pow2 sizes) reached through the same seam.
- **STM32F411** -- Cortex-M4F, ARMv7-M PMSA core MPU.
- **i.MX RT1062/Teensy 4.1** -- Cortex-M7: the one **speculating** core in the fleet, which is why
  a chip may declare thread-invariant *fixed* MPU regions (`kickos_arm_mpu_fixed`) to wrap
  unbacked external apertures as Device before the caches come up.
- **STM32F103** -- Cortex-M3, *no MPU*: the degraded "privilege-only" build proving the arch
  layer degrades cleanly.
- **Renesas RX72M** -- RXv3, *non-ARM*, has an MPU: the architecture-honesty check that `arch.h`
  carries no ARM-isms, and a second, independent MPU backend for M2.
- **ESP32 (Xtensa LX6)** and **ESP32-C6 (RV32IMAC)** -- two more non-ARM ISAs. The C6 has RISC-V
  **PMP** (an M2 backend, shared with QEMU `virt`); the Xtensa part has neither a per-thread MPU nor
  a privilege split, which forces the isolation model to treat both as *optional* per-arch
  capabilities (best-effort where absent).

**MPU hardware programming is target-specific (chip or arch), not one shared routine.** The
switch-in `arch_mpu_apply()` only **stashes** the incoming region set (shared, and not
overridable); `kickos_arch_mpu_commit()` **programs the hardware** from the context-switch
epilogue, after the physical swap, and is the per-target definition -- K64F SYSMPU, ARM
PMSAv7/v6-M (XMC4800, F411, i.MX RT1062, RP2040; the shared fallback TU
`arch/arm/common/kickos_arch_mpu_commit_default.cc`), RP2350 PMSAv8, C6/virt RISC-V PMP,
RX72M MPU.
(See `design-mpu-commit-deferred.md`.) The set of enforcement-capable chips is not a list to
maintain by hand: a chip opts in by shipping `arch/<family>/chip/<chip>/mpu.cmake`, and
the same chips select `HAS_MPU` in `arch/Kconfig`, which is what makes the enforcing
posture selectable at all. A configuration asking for it on a chip that declares neither is
refused -- by Kconfig on the unmet dependency, or by a configure error if the two
declarations ever disagree -- rather than becoming a silent no-op.

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
   placement so a system can run with the heap disabled.
6. **Dual API in userspace.** A plain C syscall layer with ergonomic C++ RAII wrappers on top.
7. **Instance-scoped state, no hard singletons.** Kernel + sim state hangs off an instance
   handle, so multiple `kernel+userspace` instances can run in one host process (the KickCAT
   sim end-goal). Kernel **objects** (TCBs, semaphores) live in generational pools inside the instance, and the **runtime core**
   (scheduler/time/syscall pools/sim arch) is aggregated into a `Kernel`/`Instance` struct reached
   via a compile-time-selectable `kernel()` accessor (a static singleton on MCU / for size;
   thread-local per instance for the multi-slave sim). Full multi-instance in the sim additionally
   needs per-instance event delivery.
8. **Dependency inversion -- the app consumes the kernel.** The application owns the top-level
   build; KickOS is a prebuilt package (libraries + headers + startup + board linker script +
   flags) consumed as a plain `add_executable` linked against the exported `kickos` target -- or
   `kickos_cxx` for a full-C++ (exceptions/STL/RTTI) app (with `kickos_add_application()` as
   optional sugar). The kernel's root thread calls one init seam `kickos_init_entry(argc, argv)`
   (`<kickos/sys/init.h>`) after kernel init; the CMake cache var `KICKOS_INIT_PROVIDER` selects
   the target that supplies it (default `kickos_default_init`, a thin passthrough
   `kickos_init_entry -> kickos_default_init_run -> kickos_app_main`), so a plain app still writes
   only `int main` and no manifest. App/libstdc++ global ctors run in the root thread BEFORE the
   seam; RETURNING from the seam is a single-shot shutdown with that status -- through the
   `kos_shutdown` syscall, so it needs `AUTH_SYSTEM` and panics `"root: shutdown refused"` if root
   narrowed that bit away -- and a persistent init never returns (parks/loops). `kickos_default_init_run`
   narrows root's authority to `kickos_app_authority()` before the app main, so a custom provider that
   delegates to it inherits the confinement. A bad/missing provider is a build-time FATAL_ERROR, never
   a silent fallback (CMake target selection, not a link-time fallback).
9. **Conventions.** `style.md` is the contract: Allman everywhere, 4-space indent, traditional
   include guards, no ternary, spelled logical operators, fixed-width types, and comments only for
   hidden constraints. Shared in shape with the sibling projects `../KickCAT` / `../kickmsg`.
   There is no formatter; the rules are held by review.

### How KickOS differs from its inspirations

None of them combine MPU-first per-domain isolation + a microkernel SVC boundary + a first-class
x86 sim. Most (RTEMS, microC/OS, RT-Thread core) are flat-memory. **ThreadX Modules** and
**ChibiOS/SB** are the closest prior art and the references we lean on for the isolation model.

Key borrowed ideas, attributed:
- **NuttX** -- `FLAT`/`PROTECTED`/`KERNEL` build-mode taxonomy (KickOS is a **PROTECTED build** on
  every board: one kernel image, one privilege crossing, and no separately-loaded user binary. The
  region boards are PROTECTED in NuttX's own sense, MPU plus a trap; the translating boards keep the
  same build mode while drawing the boundary in page tables, which is not NuttX's `KERNEL` mode
  because there is still no loader and no per-process image); the `make export`
  dependency-inversion packaging;
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

### Toolchain-libc lesson from NuttX (and the cross-libc C++/sim pain it documents)

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
  CMakePresets.json + cmake/presets/*.json   # per-arch/board presets (arm, arm64, host, riscv,
                                    #   rx, x86, xtensa)
  Kconfig                          # top-level symbol tree: capability facts (HAS_MPU, ...),
                                    #   sourced boards/Kconfig + arch/Kconfig + per-board Kconfig
  tools/kconfig/genconfig.py       # resolves a board's defconfig into <build>/generated/:
                                    #   .config, include/kickos/board_config.h, kickos_config.cmake
  cmake/
    toolchain-{arm-none-eabi,aarch64-none-elf,riscv-none-elf,rx-elf,xtensa-esp32-elf,x86_64-uefi,host}.cmake
    toolchain-cxx-runtime-check.cmake  # refuses a resolved cross compiler that lacks
                                    #   newlib + libstdc++ for THIS board's multilib
    kickos.cmake                    # board -> arch/chip resolution + image (.bin/.uf2/.hex) helpers
    cap_geometry.cmake              # the table's structural constants, emitted to C
    cap_table.cmake                 # the configure-time capability-width sum + supply check
    boot_arena.cmake                # the boot-arena footprint model
    build_stamp.cmake               # the build identity stamped into the image
  arch/
    Kconfig                        # arch/chip capability declarations: pure `select`, no knobs,
                                    #   no chip CONSTANT (those live in chip_limits.h)
    include/kickos/arch/arch.h      # THE porting interface (extern "C" seam)
    sim/                            # host x86-64 backend
    arm/
      common/                       # shared Cortex-M glue (arch_arm_common.cc), PMSA + the
                                    #   fixed-region seam (kickos_arm_mpu_fixed)
      armv6m/                       # M0+: PRIMASK crit, ctx-switch asm
      armv7m/                       # M3/M4/M4F/M7/M33: BASEPRI crit, CLZ, ctx-switch asm, cache
      chip/{mps2,nrf51,mk64f,rp2040,rp2350,imxrt1062,stm32f411,stm32f103,stm32f302,sam3x8e,xmc4800}/
    arm64/ armv8a/ chip/virt_arm64/  # AArch64 (EL0/EL1, VMSAv8 page tables, GICv2, PL011)
    rx/    rxv3/  chip/rx72m/        # Renesas RXv3 (SWINT switch, INT syscall)
    xtensa/ lx6/  chip/esp32/        # Xtensa LX6 (windowed ABI, no privilege split)
    riscv/ rv32imac/ chip/{virt_rv32,esp32c6}/  # RV32IMAC (machine mode, mtvec demux,
                                    #   CLINT/PLIC, PMP)
           rv64imac/ chip/virt_rv64/ # RV64IMAC (supervisor mode, stvec, Sv39/Sv48 page tables)
    x86/   x86_64/ chip/q35/         # x86_64 UEFI application (PE32+): the syscall entry is
                                    #   SYSCALL/SYSRET, arch_init ADOPTS the translation regime
                                    #   firmware already has live, and pe_image.ld places no
                                    #   memory map (the linker's own default layout plus three
                                    #   section wildcards)
  kernel/
    include/kickos/                 # public kernel + syscall-number headers
    sched/  thread/  task/  sync/  time/  irq/  syscall/  init/  ktrace/  bench/  domain/  grant/
  lib/
    libc/                           # freestanding: mem/str (string.cc) + a small vsnprintf (fmt.cc)
    include/kickos/                 # libc/{fmt,string}.h, console_tx.h, rtt.h
    rtt.cc                          # SEGGER RTT backend (console ch0 + telemetry ch1)
  system/                           # kickos_system: fleet-wide system layer
    include/kickos/sys/             # errno.h (KOS_E* taxonomy), cap_index.h (the well-known cap
                                    #   indices; renumberable downward only), init.h (the init
                                    #   seam), pinmap.h + service.h (the board-provider seams)
    init/                           # kickos_default_init (the passthrough provider) + the
                                    #   per-board pinmap and service-list providers
    cxx/                            # verbose-terminate handler
    driver/<chip>/<driver>/         # the driver LIBS, per chip: esp32/lx6uart, esp32c6/c6uart,
                                    #   imxrt1062/rt1062usb, mk64f/{k64dspi,k64uart,k64uartirq},
                                    #   rp2xxx/rpusb, rx72m/rxsci, stm32f411/f4uartirq,
                                    #   xmc4800/{xmcssc,xmcuart,xmcuartirq}.
                                    #   Unprivileged, linked by an app.
  user/
    include/                        # userspace API (kos.h, sys.h, app.h) + driver/ client headers
    src/                            # syscall stubs + newlib stubs
    lib/spi_proxy/                  # vendor-neutral proxy backend of the SPI class
    apps/common/                    # fleet-wide apps; a sample, not the list: hello, selftest
                                    #   (TAP gate), stress, sched_exit, mpu_fault, fault,
                                    #   fp_switch, blink, bench, cxxtest, tele_*
    apps/<board>/                   # that board's own demos (xmcspi, k64drv, rxdrv, c6blink, ...)
  boards/
    Kconfig                        # one stanza per board: the board choice + `select` of its chip
    <board>/                       # board.cmake (arch/chip, + a CPU flag only where the board
                                    #   differs from its chip) + configs/<variant>/defconfig
                                    #   (Kconfig-resolved into the generated board_config.h)
                                    #   + optional <chip>.ld override / board-local Kconfig
  docs/                             # README.md (map); book/ (how & why); reference/ (code-synced)
  README.md  roadmap.md  TODO.md  STATE.md
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
  incoming region set on switch-in (shared, not overridable); `kickos_arch_mpu_commit()` **programs the
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

**TCB:** saved SP/context ptr, `state` (INACTIVE/READY/RUNNING/BLOCKED/EXITED, where BLOCKED covers a wait queue, the timer delta list and a queue-less park alike -- `wait_kind` is what says which), `prio`
(+ `base_prio`, the assignment anchor PI raises `prio` above), `policy` (FIFO|RR), `quantum_ns` +
`slice_deadline_ns` (an ABSOLUTE deadline: the RR quantum is wall-clock, see
`invariants.md` `rr-quantum-is-wall-clock`), intrusive
links (ready/wait/timer lists), stack bounds, **MPU region descriptors**, privilege flag.

**Ready queue:** array of per-priority FIFO lists + a priority bitmap. Highest = find-first-set,
written **once and arch-neutrally** as `31 - __builtin_clz(bm)` in `highest_prio()`
(`kernel/sched/policy_fifo_rr.cc`). There is no per-arch variant: on ARMv7-M the builtin is a `CLZ`
instruction, and on ARMv6-M -- which has none -- the compiler lowers it to a libgcc helper.

**Pluggable policy interface (RTEMS-style).** The core owns *mechanism* (run state, context
switch, ready structure); the *policy* (which thread runs next) sits behind a small interface
(`SchedPolicy`, `kernel/include/kickos/sched.h`): `pick_next()`, `on_ready(t)`, `on_remove(t)`,
`on_yield(t)`, `on_slice_expire(t)`, plus the **tickless timed-event seam** -- `on_switch_in(t)`
arms the incoming thread and `next_timed_event()` reports the earliest policy deadline
(`UINT64_MAX` = none), so the core owns the clock and the policy owns the deadline.
**FIFO + RR ship first**
(priority bitmap + per-priority FIFO, optional per-thread quantum); **EDF / rate-monotonic** drop
in later without touching `reschedule()`, IPC, or the arch layer. Runqueues are kept **SMP-ready**
(per-core) for RP2040 core1 later.

**`sched::reschedule()`** -- the single decision point: ask the active policy for `pick_next()`;
if != current, call `arch_switch(from, to)` (which may defer). No caller is privileged over another -- **the tick
is not special.**

**Triggers (all equal):**
1. `thread_yield()` -- voluntary.
2. Block on empty `sem_wait`/`mutex_lock`/endpoint recv -> wait queue -> reschedule.
3. Wake from **thread** ctx: `sem_post` readies a waiter; if higher prio, reschedule now.
4. Wake from **IRQ** ctx: an ISR posts -> mark reschedule-needed -> PendSV switches on IRQ exit.
   **The headline "not the tick" path -- a device interrupt drives scheduling directly.**
5. Timer expiry: a sleeping/timed-wait thread's deadline passes -> readied -> reschedule.
6. RR slice expiry (RR only) -> reschedule among equal priority; each RR task has its own
   configurable **time quantum** + a yield-remaining-quantum call.

**Task-switch hook.** The single switch-in path reprograms per-thread MPU regions and updates
introspection counters in one place, so MPU + stats can't drift from the actual switch. ISR
posts may run direct (scheduler-locked) or, later, deferred to a handler task.

**Tickless.** Monotonic clock from a free-running counter; a **delta list** of absolute
deadlines (ChibiOS `TIMEDELTA` model). Arm the one-shot timer for
`min(nearest deadline, running-RR slice expiry)` with a **minimum-delta guard** (never program a
compare that may already be in the past). Pure-FIFO with nothing time-pending => timer disarmed,
zero timer interrupts. `CONFIG_SCHED_PERIODIC_TICK` (opt-in) forces a classic periodic tick.
Idle thread at lowest prio: ARM `WFI`; sim `sigsuspend`.

**Thread lifecycle past the exit -- cancel, join, wait-until-last.** `KOS_SYS_THREAD_KILL` is a
COOPERATIVE cancel: it marks the target and wakes it out of WHATEVER park it is in -- the abort is
TOTAL over `WaitKind` (`kernel/thread/park.cc`, `thread_abort_park`) and reports `-KOS_ECANCELED`
where the primitive has a channel to report one; the target then runs its own `sched::exit_current`.
`KOS_SYS_THREAD_JOIN` OBSERVES the death, bounded by an optional `timeout_us`
(`KOS_TIMEOUT_NONE` = none). The joiner parks **queue-less** tagged `WAIT_JOIN` with `wait_obj`
naming the target TCB, and `sched::exit_current` sweeps the thread pool for that tag at every
thread exit -- a scan at an exit and on no other path, which is what buys join **zero per-TCB
state and no waiter list**, a byte budget the 16 KiB boards care about. Kill and join take the
SAME gate, spawn parenthood (`ThreadAttr::spawner_tag` against `ThreadPool::kill_tag_of`), which
is non-transferable: there is no table entry for a `cap_grant` to copy, and it hands the caller
nothing it did not already have. They decide ONE state oppositely, and that difference is the
point -- see `invariants.md` `join-accepts-the-unreclaimed-exit`.
`KOS_SYS_WAIT_LAST` is the AGGREGATE: it returns once the caller is the last live thread
(`sched::live_count()` reaching 1), parking queue-less tagged `WAIT_LIVE_LAST` and woken by that
same exit sweep. It takes **no deadline** -- it is the shutdown condition itself rather than a
wait for an event, so no caller could know a bound -- and it is **root-only**, `-KOS_EPERM` to
anyone else: it reaches outside the caller's own spawn subtree, and it is single-seat, so an
ordinary thread parking there first would deny root its shutdown condition for as long as it
waits. That refusal is also what makes a second-waiter case unreachable, so there is none to
refuse. It is the only way to await a thread
the caller cannot NAME: a spawn hands back a handle to the child alone, so a `main`'s
grandchildren are unnameable. Its condition is GLOBAL, so it never returns in an image whose
service list holds a driver thread that does not exit.

**Group death is the TASK layer's, not the thread's.** `KOS_SYS_TASK_CREATE` makes an EMPTY group
holding a domain built from its own grant and `kos_thread_params::task` seats a member;
`KOS_SYS_TASK_KILL` ends the whole group from a supervisor, and a member's own death ends it too.
The gate there is CREATORSHIP rather than possession, the address space stays on `Domain`, and 0
means the request was ACCEPTED and never that the thread is gone -- a thread that never re-enters
the kernel is unreachable without preemption. `docs/design-task-layer.md` is the record.

---

## User/kernel separation

- Kernel privileged on **MSP**; threads on **PSP**. User threads unprivileged
  (`CONTROL.nPRIV=1`); kernel threads privileged.
- **Syscalls via `SVC`**: handler reads number + args (r0-r3), dispatches through an
  arch-independent **syscall table**, returns in r0. Sim: a trampoline flips an emulated-
  privilege flag (+ `mprotect` toggles kernel-mem accessibility) and calls `syscall_dispatch()`.
- **Syscall return ABI (`system/include/kickos/sys/errno.h`).** A syscall that can fail returns its
  error as the **negated** code `-KOS_E*`; a success -- a count, a byte-count -- is
  **non-negative**, so `rc < 0` is unambiguously an error and never aliases a valid count (counts
  and byte-counts stay small). **A handle is never a return value**, and that is what buys the
  handle word its full width: every minting syscall -- `KOS_SYS_SEM_CREATE`, `KOS_SYS_MUTEX_CREATE`,
  `KOS_SYS_ENDPOINT_CREATE`, `KOS_SYS_IRQ_CLAIM`, `KOS_SYS_THREAD_CREATE`
  (`user/include/kickos/sys/abi.h`) -- returns a status and writes the handle through an
  out-parameter. So every handle class spends all 32 bits and a live handle may have bit 31 set:
  `cap.h`'s `KCAP_INDEX_BITS` / `KCAP_GEN_BITS`, the "NO SIGN TEST" notes on `free()` and
  `resolve()` in `kernel/include/kickos/slotpool.h`, and `ThreadPool::INDEX_BITS`. The code set
  mirrors POSIX magnitudes -- `EPERM` `ESRCH` `EIO` `EBADF` `ENOMEM` `EFAULT` `EBUSY` `EINVAL`
  `EMFILE` `EPIPE` `EDEADLK` `ENOSYS` `EOVERFLOW` `ENOTSUP` `ETIMEDOUT` `ECANCELED` -- plus
  **`EOWNERDEAD`**, the robust-mutex case: a mutex *acquired*
  while its prior owner died holding it, still returned negative (`-KOS_EOWNERDEAD`) for the
  caller to special-case as HELD. Six of those carry a kernel-specific meaning that has to be
  stated, because the POSIX name does not give it:
  **`ESRCH`** is a one-shot reply cap whose parked caller is gone (aborted or reused);
  **`ENOSYS`** is an arch backend that does not implement the call on this chip -- the
  declining fallback TU (e.g. `arch/common/arch_pinmux_set_default.cc`), so an unported syscall
  is a clean refusal rather than a silent no-op; **`EMFILE`** is ONE thread's capability table
  refusing a mint, and it is deliberately not `ENOMEM` because the fix is a wider declaration
  rather than more RAM (from `KOS_SYS_CALL` it names the SERVER's table, since the reply cap is
  minted into the receiver). TWO conditions produce it, and the second has free slots: the run's
  free list is empty, or the thread already holds `KICKOS_CAP_REPLY_MAX` live `CAP_REPLY` entries;
  **`EOVERFLOW`** is a bounded counter already at its ceiling, refused rather than wrapped --
  `sem_post` with no waiter at `KOS_SEM_COUNT_MAX` and the object-refcount ceiling behind
  `KOS_SYS_CONSOLE_PUBLISH` and a spawn's delegation batch (`kernel/syscall/syscall.cc`,
  `kernel/syscall/syscall_thread.cc`; documented per call in `user/include/kickos/sys.h`);
  **`ETIMEDOUT`** is a caller-supplied deadline that passed before the operation could happen,
  and it promises that NOTHING happened for the caller: `KOS_SYS_SEND_TIMED`, `KOS_SYS_RECV_TIMED`,
  `KOS_SYS_CALL_TIMED` and `KOS_SYS_THREAD_JOIN` given a `timeout_us` other than
  `KOS_TIMEOUT_NONE` expire with no peer, move no bytes, return no reply, and leave the joined
  thread running. `kernel/time/time.cc` decides only THAT a deadline
  expired and delegates each endpoint park's unwind to `endpoint_wait_abort`
  (`kernel/thread/park.cc`), which unlinks the right queue and reverts the right
  donation; the deadline is cancelled in `sched::wake_no_resched`, the one unpark funnel, so a
  rendezvous that beats it can never also report it. ONE caveat, and it is the reason the promise is
  worded per-caller: a `KOS_SYS_CALL_TIMED` whose request a server had already taken leaves
  that server holding its reply capability, whose eventual `KOS_SYS_REPLY` answers `ESRCH`;
  and
  **`ECANCELED`** is *this* thread having been cancelled by `KOS_SYS_THREAD_KILL` -- the wait it was
  in, or was about to enter, is abandoned and the thread is expected to exit itself
  (`kernel/thread/park.cc`'s `thread_cancel`/`thread_abort_park` sets `wait_result`, the death point
  is the next syscall ENTRY in `kernel/syscall/syscall.cc`, and `kernel/irq/irq.cc` refuses to
  re-block an already-cancelled thread). Five syscalls
  stay OUT of this scheme by return type: `ram_alloc` returns a pointer (every failure is NULL -- a
  negated errno cast to a pointer would be non-NULL); `cpu_clock_hz`/`cpu_clock_set` and
  `KOS_SYS_PERIPH_CLOCK_HZ` return a u32 Hz whose 0 already means unknown / no-silicon-clock; and
  the selftest-only `KOS_SYS_GUARD_ADDR` returns a raw address.
- **MPU per domain, first-class** (see *Memory domains* below): the running thread's domain
  region set is reloaded on every switch-in (`arch_mpu_apply` stashes it; `kickos_arch_mpu_commit`
  programs the hardware after the physical swap). A thread touching a domain
  region not granted to it faults -> kernel reports. Granted **data** regions were isolated from
  M0; **per-thread stack regions** came with the M2 `Domain` object, which moved a thread's stack
  out of the kernel pool and into the arena. Under a region backend that set is per-thread, so a
  sibling faults on another's stack -- a BACKEND STRENGTHENING, not the portable promise, since a
  translating backend maps a task's stacks task-wide (`design-m6-mmu.md` F9).
- **Sim isolation**: back "physical RAM" with one `mmap` arena; on every switch-in the running
  thread's regions are `mprotect`-ed (grant its domain data region, everything else no-access) so
  a cross-domain pointer raises `SIGSEGV`, translated into the same fault path. **Per-domain data
  isolation has been enforced in the sim since M0.** Grant *geometry* is validated (page-aligned
  arena sub-range), and grant *ownership* is no longer taken on the spawner's word: it is
  authenticated (M3), and Rule 7 (`grant-refuses-kernel-reserved-blocks`) mechanically refuses an
  inadmissible region for **every** granter, privileged ones included. What the sim still cannot
  reproduce is the user<->kernel boundary itself -- a Linux process has no CPU privilege level --
  so a reserved arena page stands in for it, and that one boundary is hardware-only.

---

## Memory domains (the isolation unit)

The unit of memory isolation is a **memory domain** -- a lightweight "process" in the
memory-boundary sense only: no `fork`, no `exec`, no loader and no per-process image. What backs the
boundary depends on the part. Where the chip carries region descriptors the domain carries a region
set and every domain lives in ONE physical address space; where the chip translates, the domain
carries a page-table root and the boundary IS a virtual address space. Nothing above the arch layer
distinguishes the two. Model borrowed from
**Zephyr `k_mem_domain`** / **ARINC 653** spatial partitions (temporal partitioning is *not*
implied); ThreadX Modules and ChibiOS/SB are the loadable-code cousins.

- **A domain owns a region set** -- code (RX), data/heap (RW-NX), and any granted MMIO -- plus a
  **privilege posture**.
- **Threads belong to a TASK, which owns the domain they share** (cooperative within a domain), **and
  each thread's set carries its own stack region**. So a domain switch reloads the domain regions and
  every switch-in also loads the incoming thread's stack region; the MPU is therefore reprogrammed on
  **every** context switch (there is no "same-domain skip": the stack region differs per thread, and
  the privilege posture can differ moment-to-moment across the syscall boundary). **That per-thread
  set is what denies a sibling another's stack, and it is a region-backend strengthening rather than
  the portable contract**: the floor is that a thread-scoped grant guarantees access to its HOLDER,
  and a translating backend maps a task's stacks task-wide, where a sibling reaches them
  (`design-m6-mmu.md` F9).
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
domain's RAM is one region: power-of-two-aligned on a pow2-mode backend, granule-aligned on a
base+limit one such as the K64F's SYSMPU (`arch_mpu_region_pow2`).

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
| RX72M -- RXv3 MPU (CPU-side) | yes | none (PRCR is an unrelated write-latch, not a privilege gate) | **yes** | silicon-proven: SRAM/domain enforcement (selftest + mpu_fault cross-domain trap, 2026-07-17) AND a real granted peripheral window -- `rxdrv` blinks LED6 through a granted 16-byte PORT8 PODR window while an ungranted PORT8.PDR poke faults ("MPU FAULT: task 'rxdrv'") |
| K64F -- SYSMPU (bus-slave-side) | **no** | **AIPS PACR** (by privilege+master, per 4 KB slot, NOT per-thread) | **no** | silicon-proven: an unprivileged PIT access faults via AIPS while SYSMPU latches no error; clearing the slot's PACR SP bit then admits ALL user code |

**The K64F AIPS `PACR` bit is DERIVED from the block base, not tabled** (`slot_of` / `pacr_of` /
`pacr_sp_bit`, `arch/arm/chip/mk64f/regs/aips.h`, each pinned by `static_assert`), because a table
of 128 slots would be a table of transcription errors. The derivation, from K64 RM 20.2.2-20.2.3:
a slot is one 4 KiB step of the bridge aperture, so `slot = (base - AIPS0_BASE) / 0x1000`; eight
slots share one 32-bit `PACR`, `PACRA..PACRD` at `+0x20` covering slots 0-31 and `PACRE..PACRP`
at `+0x40` covering 32-127; each slot owns a NIBBLE, field 0 at bits `[31:28]` down to field 7 at
`[3:0]`, laid out `reserved[3] / SP[2] / WP[1] / TP[0]`. So the supervisor-protect bit is
`1 << (30 - 4 * (slot % 8))`. Worked: DSPI0 at `0x4002_C000` is slot **44** -> `AIPS0_PACRF`
`0x4000_0044`, field 4 = bits `[15:12]`, `SP` = bit **14**. UART0 at `0x4006_A000` is slot 106 ->
`PACRN` `0x4000_0064` bit 22; the PIT at `0x4003_7000` is slot 55 -> `PACRG` `0x4000_0048` bit 2.
Every `PACR` resets to `0x4444_4444` on this part -- every nibble `SP = 1` -- so **every**
peripheral slot is supervisor-only until something clears it, and the coarse granularity is the
whole 4 KiB slot: `arch_periph_enable` therefore has no PIT entry, because clearing slot 55's `SP`
for a granted channel-2 window would equally expose the chained ch0+ch1 pair the timebase runs on.
The full DSPI0 register map is in `bus-service.md`.

**The ESP32-C6 has TWO units in series, and only the CPU-side one faults.** An HP-CPU access to an
HP peripheral in user (REE) mode passes PMP first and APM second, and APM is consulted only if PMP
passed (TRM 16.1, Table 16.1-1) -- over the whole HP peripheral aperture
`0x6000_0000 .. 0x600A_FFFF` (Table 16.1-1 note 2), and TEE is always R/W/X while every drop to
user mode is REE (16.3.1). PMP is the per-thread line the MMIO grant programs; **APM
(Access Permission Management, TRM chapter 16) is per SECURITY MODE** -- TEE / REE0 / REE1 / REE2 --
never per thread, and its default posture DENIES every REE mode access to every peripheral
(16.3.2 Note). So without a one-time APM open a U-mode driver reaches nothing even with a correct
PMP grant. `apm_open_ree0()` runs in `arch_init` (`arch/riscv/chip/esp32c6/chip_esp32c6.cc`) on
every C6 board, so this is boot-time state on all of them, not a per-app step. Registers
(`arch/riscv/chip/esp32c6/regs/apm.h`; TRM Reg 16.1-16.5 and 16.53):

| Register | Address / offset | Reset | Contents |
|---|---|---|---|
| `HP_TEE_M0_MODE_CTRL_REG` | `0x6009_8000` (`+0x00 + 0x4*n`) | 0 | per-master security mode; the HP CPU is master **M0**, and reset 0 already maps its U-mode to **REE0**, so KickOS writes nothing here |
| `HP_APM_REGION_FILTER_EN_REG` | `HP_APM` `+0x0000` | `0x01` | bit n enables region n; region 0 on at reset |
| `HP_APM_REGIONn_ADDR_START_REG` | `+0x0004 + 0xC*n` | 0 | region start |
| `HP_APM_REGIONn_ADDR_END_REG` | `+0x0008 + 0xC*n` | `0xFFFFFFFF` | region end |
| `HP_APM_REGIONn_ATTR_REG` | `+0x000C + 0xC*n` | 0 | `R0_X` b0, `R0_W` b1, `R0_R` b2, then `R1_X` b3 .. `R2_R` b8 |
| `HP_APM_FUNC_CTRL_REG` | `+0x00C4` | `0xF` | `M0..M3_FUNC_EN`; all four enforcing at reset, so KickOS writes nothing here either |

`HP_APM` is at `0x6009_9000`. Region 0 is LEFT at its reset values -- start 0, end `0xFFFFFFFF`,
attr 0 -- which is the catch-all that denies every REE mode everywhere; overlaps are a permit
UNION (16.3.2.3), so a later region granting `R0_R | R0_W` beats it on the overlap. `arch_init`
programs regions **1..3** to the complement of the HP-bus Rule 7 reserved blocks -- INTMTX, and
the contiguous PCR..HP_APM span -- so those stay APM-closed to REE on top of the grant path's
refusal, and everything else is REE0 read/write. The APM registers are writable only in TEE mode
(= M-mode), so a REE thread cannot reprogram them even if it somehow held a PMP window over
`0x6009_9000`; the grant path refuses that window anyway (Rule 7 lists HP_APM and HP_TEE).

**An APM denial does NOT trap.** Per TRM 16.5 a blocked read returns 0, a blocked write is
DROPPED, and a separate per-master APM interrupt fires -- there is no `mcause` 5/7. So per-thread
isolation on this chip is proven on the **PMP** fault and never on APM, and a "no fault" result
from an APM-scope test is not evidence that enforcement is broken. An APM-only denial is
observable solely through the TRM's per-master exception-info registers, and KickOS defines
**neither** the interrupt nor those registers: the header above carries only what `arch_init`
writes, so reading them is a bench step against the TRM, not a call into the tree.

**The C6 GPIO window's granularity is a REGISTER limit, not a PMP limit, and it is a real
boundary caveat.** The GPIO matrix block is at `0x6009_1000` and the granted window (`c6blink`) is
its first **64 bytes** -- pow2 and 64-aligned, so one PMP NAPOT entry -- covering the pin BANK:
the output latch `GPIO_OUT` (`+0x0004`) with its atomic `W1TS` (`+0x0008`) / `W1TC` (`+0x000C`),
the direction `GPIO_ENABLE` (`+0x0020`) with `ENABLE_W1TS` (`+0x0024`) / `ENABLE_W1TC` (`+0x0028`),
and `GPIO_IN` (`+0x003C`).
The matrix out-sel `GPIO_FUNCn_OUT_SEL_CFG` (`+0x0554 + 0x4*n`) and the per-pin config and
interrupt registers (`+0x0074` up) stay OUTSIDE it, which is what makes it a capability rather
than the block: the holder drives and reads its bank but cannot ROUTE a peripheral signal onto a
pad. Re-muxing is out of reach on the other stage too -- IO MUX (`0x6009_0000`) and PCR
(`0x6009_6000`) are never granted, and the pad plus out-sel are reached only through the mediated
`arch_pinmux_set` seam, which refuses a kernel-owned pin on BOTH stages.

The caveat is what the window's own registers are: `W1TS`/`W1TC` and `ENABLE`/`ENABLE_W1TS` are
WHOLE-PORT -- bit p addresses pin p -- so a thread holding the bank can toggle the output latch,
and set the direction, of ANY pin in the port, not only its own. PMP draws the per-thread line at
the register level and cannot draw it per pin; single-pin isolation would need per-pin ETM or
dedicated-GPIO features, which is out of scope. What bounds it is that only pins something muxed
onto the matrix actually drive a pad, and that no other peripheral is reachable from the window at
all. State this plainly when reasoning about the C6 boundary: the per-thread line is genuine and
PMP-enforced, but its unit is a register bank, not a pin.

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
descriptor (no rounding) + not a bit-band alias, or for a **RAM** grant require
`arch_ram_region_admissible` AND confinement to the user arena **for every caller** (no
privileged waiver) -- power-of-two size plus natural alignment on a pow2-mode backend
(PMSAv7, PMP NAPOT), a granule multiple on a base+limit one (PMSAv8, SYSMPU, RX). `domain_for` (`kernel/domain`) runs it at the **region-commit chokepoint** on the
prospective committed geometry before it allocates a domain slot; the caller-owned-stack path in
`thread_create_call` runs the same predicate on the stack region, and `thread_create` carries a backstop
assert. Each enforcing chip declares its owns-for-life set via `arch_reserved_blocks` (`arch.h`) --
there is **no fallback TU on purpose**, so an enforcing port that forgets one fails to *link* (affirmative
fail-closed); the set is owns-for-life only (a neutralize-then-grant watchdog is excluded unless
its tick feeds the timebase) and includes every access-permission controller, bus-side ones
included -- the K64F AIPS bridge PACR pages, the ESP32-C6 HP_APM/HP_TEE. On a **bit-band core** (`arch_bitband_present()` != 0) the overlap
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

Status: **per-domain isolation is enforced everywhere it can be.** In the sim it has held since
M0 -- `arch_mpu_apply` `mprotect`s the user-RAM arena to the running thread's granted region set
on every switch-in, and a cross-domain write faults (CI-covered by the `selftest` domain stage).
M2 completed the picture: the full `Domain` object (a shared region set several threads
reference, plus per-thread private stacks) and the per-**chip** hardware backends are in, so the
same cross-domain write is silicon-proven to fault on SYSMPU, PMSAv6-M/v7/v8, RISC-V PMP and the
RX MPU. A single per-thread granted region (`ThreadAttr.mem_base` / `kos_thread_params.mem_base`,
backed by `arch_ram_alloc`) remains the simple case a domain is built from.

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
  no-execute); (2) **IRQ-as-event** -- a `CAP_IRQ` capability on the line, minted by a
  bring-up path holding `AUTH_IRQ` (`irq_claim`) and DELEGATED to the driver at spawn, which
  then loops `irq_wait(cap)` / service.
  The kernel's generic ISR stub masks the line, posts the driver's notification
  (a semaphore), flags reschedule -> PendSV switches to the now-ready driver task; the next
  `irq_wait` auto-re-arms the consumed line, so no explicit ack is needed (`irq_ack(h)` stays an
  OPTIONAL early re-arm that reacts sooner to a latched raise; the latch-and-coalesce contract
  keeps the event either way). The
  driver never runs in handler mode.

**API sketch (arch-neutral):** `irq_attach/detach` (in-kernel);
`irq_claim/wait/ack/notify/discard/unmask`
(userspace, CAPABILITY-based -- the C++ `kos::Irq` owns the cap and closes it on destruction, so
root writes `auto irq = kos::Irq::claim(line);` and the driver `auto irq = kos::Irq::adopt(cap);`,
backed by the existing Semaphore as the notification; `notify` posts it WITHOUT touching the
controller, the doorbell a service thread rings for an IRQ thread that owns the registers, and
`discard` (`KOS_SYS_IRQ_DISCARD`) drops whatever the controller has latched for the line while
masking and unmasking neither);
backed by an interrupt-controller abstraction in the arch/chip layer (NVIC on ARM;
sim = signal-driven injection). Userspace never *injects* -- reacting is `register`/`wait`, and
raw in-handler-mode callbacks are the privileged `irq_attach` (TCB, not defended). `irq_inject`
is only the sim's fake-a-device-firing mechanism (test scaffolding, gated/privileged),
never a userspace primitive.

### Driver packaging: class versus service

**The ruling: the class is the primitive, the service is a thin thread composed on top of it,
never the reverse.** A driver is packaged as an in-process CLASS, as a shared SERVICE, or as BOTH
-- and when it is both, the service is composed over the class.

- **Class (driver-lib).** A hardware-agnostic object linked directly into the consumer thread. No
  IPC, no thread of its own, no endpoint; the transaction runs inline, in the caller's thread. A
  tight-coupling consumer -- a KickCAT ESC in its cyclic fieldbus loop -- uses this and pays
  nothing beyond the register work.
- **Service.** A thread that owns exactly ONE class instance plus the peripheral capability, and
  multiplexes clients over a `CAP_ENDPOINT` (call/reply, reply-cap). Sharing and arbitration live
  here and ONLY here. The badge that would let it authenticate MUTUALLY-UNTRUSTING clients is not
  in effect yet (`badge` is `KOS_BADGE_NONE`, see *Service publication* below), so a shipped
  service today fronts one client's several devices, not several clients.

The class is written first and DEFINES the API; the service is a transport over that same API and
is not allowed to invent its own.

**The 1:1 rule is what stops it rotting.** The service request protocol MUST be a 1:1
serialisation of the class methods -- the endpoint is literally "the class API, over the wire". If
the two drift, two APIs must be maintained and the hardware-agnostic contract erodes. A new
capability on the class is a new message on the service, mechanically. The wire form of that rule
for SPI/I2C is `bus-service.md`.

The class API must therefore assume none of the things a service has: it does not own a thread (it
runs in the caller's thread inline, or in the service's thread when shared, and must work either
way), it does not own an endpoint (it is a synchronous transaction primitive, and any blocking is
the caller's wait rather than an IPC it initiates), and it assumes no exclusivity beyond the
peripheral capability it was handed.

**Two capability shapes, both first-class.** In the class model the cap IS the hardware -- the
MMIO region granted at spawn, "you hold the device", and holding the region is the authority, so
single-writer is free. In the service model the cap is an endpoint cap -- "you may ASK the holder"
-- and the client never touches the MMIO, holding only the right to send requests. Neither is the
"real" one, and the same driver may be reached both ways in different images.

**This is the microkernel dividend, stated concretely.** Because drivers live in userspace, the
CONSUMER chooses the coupling and pays only for what it uses: link the class and call it inline
for lowest latency and zero kernel tax, or talk to the service and pay the round-trip deliberately
in exchange for arbitration and isolation from the device. The kernel levies no driver tax at all
-- it only routes capabilities -- so the same peripheral is a private inline class in one image
and a shared service in another with NO kernel change. A kernel-resident driver cannot offer that
choice.

**Bus versus device (the SPI shape).** A shared bus needs a second split INSIDE the class layer: a
**bus class** owns the peripheral, its cap and the transaction engine, and a **device handle**
carries chip-select plus per-device config and is issued against a bus. A sole user of a bus holds
its own bus instance fully inline with no arbitration; when unrelated clients share it, a bus
SERVICE owns the bus class and arbitrates device-handle traffic. The one-block-many-modes parts
(XMC USIC, RX SCI = UART/SPI/I2C by mode) resolve the mode-select seam IN THE CLASS, not the
service. Multi-instance is thread-per-instance: one service thread per bus or device it fronts.

A **watchdog is a class by default**, and the exception proves the rule: it is single-owner
liveness proof, so it is instantiated in the thread that must prove it is alive and the kick
authority is the cap on its MMIO region. Routing the kick through a service would INVERT its
purpose -- if the service thread wedged, every client would fail to kick, which is the exact
failure a watchdog exists to catch. The one service case is a software-watchdog SUPERVISOR: N
threads check in, the supervisor owns the watchdog class instance and kicks the hardware only if
all checked in. Still built on the class. (Design: `../design-m4-driver-model.md`.)

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
  -fno-threadsafe-statics -fno-use-cxa-atexit`. Bring-up: run `.init_array` ctors in startup.
  Of the usual ABI stubs only `__dso_handle` exists (`user/src/newlib_stubs.cc`, and it is in
  `tests/static/weak_allowlist.txt`): `-fno-use-cxa-atexit` removes the need for `__cxa_atexit`, and
  `__cxa_pure_virtual` is never emitted because nothing declares a pure virtual. **`operator
  new/delete` is not provided at all** -- the kernel links `-nostdlib++`, so a stray `operator new`
  is a LINK ERROR rather than a silent heap allocation (`CMakeLists.txt`, the `-nostdlib++` link options). No implicitly
  heap-allocating STL. `extern "C"` at asm/startup/syscall seams.
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
- **Per-thread reentrant state**: on every board but the sim, whose libc is the host's, gives every
  thread slot its own `struct _reent` and points libc at the running thread's copy from
  `switch_book` and `sched::start` (`kernel/sched/sched.cc`), through the one word libc resolves
  its state by (`&_impure_ptr`, or `__getreent`'s on Xtensa). `errno` is that struct's first
  member, so this -- and **not `KICKOS_TLS`** -- is what makes `errno` follow the thread; the two
  are independent mechanisms and a program wanting both turns both on. `_REENT_INIT_PTR` aims
  every copy's `_stdin`/`_stdout`/`_stderr` at the one global `__sf[3]`, so **stdio buffering
  stays process-wide** however many copies exist.

---

## Build system

- **CMake + Ninja.** Toolchain files `toolchain-arm-none-eabi.cmake` / `toolchain-host.cmake`;
  presets in `CMakePresets.json` + `cmake/presets/*.json`.
- Board select `-DKICKOS_BOARD=<board>` plus `-DKICKOS_CONFIG_VARIANT=<variant>` (or a preset,
  which is exactly that pair and nothing else); the board descriptor pins chip, arch,
  memory map, console driver, clock config, linker script, and the variant's defconfig
  states the configuration, the memory-protection posture included.
- Static libs: `kickos_kernel` (TCB/scheduler), `kickos_arch_<arch>`, `kickos_lib`, `kickos_user`
  (linked as one RESCAN link group, since arch<->kernel reference each other). A clean split
  separates `kickos_kernel` (TCB/scheduler) from **`kickos_system`** -- the fleet-wide system
  layer: an INTERFACE header home for the syscall error taxonomy (`errno.h`), the
  capability-index convention (`cap_index.h`, renumberable downward only), the init seam
  (`init.h`) and the two board-provider seams (`pinmap.h`, `service.h`); and the home of the
  class/service driver layer plus the per-board bring-up descriptor, both **populated**.
  `system/driver/<chip>/<name>/` carries the driver libs -- `esp32/lx6uart`, `esp32c6/c6uart`,
  `imxrt1062/rt1062usb`, `mk64f/{k64dspi,k64uart,k64uartirq}`, `rp2xxx/rpusb`, `rx72m/rxsci`,
  `stm32f411/f4uartirq`, `xmc4800/{xmcssc,xmcuart,xmcuartirq}` -- and `system/init/` carries the
  bring-up descriptors:
  `common/` (the passthrough init provider, the empty pinmap and the empty service list) plus a
  per-board directory of `pinmap.cc` and `service_list*.cc` providers for `f302nucleo`,
  `f411disco`, `frdmk64f`, `picopi`, `pizero2350`, `teensy41`, `xmc4800-relax`, `rx72m`,
  `esp32-wroom`, `esp32c6-wroom` and `sim`. `kickos_system` carries no archive, so it links separately (never in a RESCAN
  group) and is propagated to every app via `kickos_core`.
- **Init provider (the entry seam target).** The target supplying `kickos_init_entry` is a
  separate library selected by the `KICKOS_INIT_PROVIDER` cache var (default `kickos_default_init`,
  a `kickos_system` service that passes through to the app's `main`); it is spliced into the link
  group right after `kickos_kernel` and resolved across the RESCAN boundary. A power user names
  their own target; a missing/unknown provider is a CMake FATAL_ERROR -- never a silent fallback,
  never a link-time fallback TU -- and the installed package refuses a consumer override of the frozen
  provider.
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
  Those two lines are the whole supported path **on MCU targets too**, not just the sim: the
  bare-metal link recipe, the chip linker script and -- via `INTERFACE_LINK_DEPENDS` -- a real
  build-system dependency on that script all ride the exported target, so an edited `.ld` relinks
  instead of leaving a stale image to flash. Bare metal adds exactly one optional line,
  `kickos_emit_image(<target>)`, because turning the ELF into `.bin`/`.hex`/`.uf2` is a `POST_BUILD`
  action and no usage requirement can carry an action. `kickos_add_application(<name> SOURCES...
  BOARD...)` remains **optional sugar** with no powers the plain path lacks; the in-tree fleet uses
  it, downstream projects need not. Switching sim<->MCU is a one-word `BOARD`/toolchain change.
  First-class acceptance criterion, gated both ways (`tests/integration/check_oot_export{,_mcu}.sh`).
  KickOS's own warning flags are **never** part of that interface -- they are this project's
  hygiene policy, applied `PRIVATE` to targets we own, and a consumer's diagnostics stay theirs.
- **Declaring a driver / QEMU test / board provider.** Three macros in `cmake/kickos.cmake` give
  each its single shape: `kickos_add_driver(<name> [SOURCES] [CLASS] [REGDIR])` -- a freestanding,
  exported driver-lib linking `kickos_user`, its `.data`/`.bss` landing app-side; `kickos_add_qemu_test(NAME
  TARGET BOARD SCRIPT ...)` -- a QEMU boot gate (exit 77 = SKIP) that keeps the per-board QEMU env
  prefix in exactly one place; and `kickos_add_board_provider(<name> SOURCE [LINK])` -- a pinmap or
  service-list descriptor lib that folds its `install(EXPORT)` in so adding a provider cannot drift
  from a hand-maintained install list. The `KICKOS_BOARD` cache-var help and the MPU-enforcement
  board list are **derived from globs** over `boards/*/board.cmake`, so neither goes stale against
  the fleet.
- **CTest** runs the sim ELF natively in CI.

---

## Object model, capabilities & IPC

The object/credential model layered on the MPU enforcement. Enforcement is only meaningful once
hardware constrains unprivileged userspace, so this model is designed against *all* object types
that exist (semaphore, mutex, IRQ handle, memory grant), not over-fit to one. **Status: every
`CapType` is live.** The SEMAPHORE, PI-MUTEX (`CAP_MUTEX`) and ENDPOINT/IPC (`CAP_ENDPOINT`) paths
landed at M3, silicon-validated under enforcement; `CAP_IRQ` (a tier-1 interrupt-line binding,
minted by `irq_claim` -- see *Drivers & interrupts* above) and `CAP_REPLY` (the one-shot reply cap,
minted by `cap_install_reply`) are equally live. `CAP_FRAME` (a RUN of physical frames) and
`CAP_ASPACE` (an address space, named by a generational domain handle) landed at M6.5 C1 and are
built only where the board translates; the two of them take the last values the type field holds,
and `CAP_KIND_MAX` is what a third kind would fail against. Each object pool was added additively
via the recipe in Book ch.8.2, and `CAP_ASPACE` adds none: a `Domain` is already refcounted, so its
hold is `domain_ref` and its ceiling is refused at `obj_ref_inc`. The contract below is code-synced to `kernel/include/kickos/cap.h`,
`kernel/syscall/cap.cc`, `kernel/syscall/syscall.cc`.

- **Per-thread typed handle table, not global ids or fds.** A global object id every thread can name
  is ambient authority -- the opposite of the isolation pillar. Each `Thread` holds a `CapRun`,
  a directory of fixed-size `CapEntry` chunks reserved from one static slab AT SPAWN -- every
  chunk or the spawn fails, and `cap_install` never allocates, which is what keeps a client
  driving a server's reply mint inside the server's own run. A table that fits one chunk
  (`KCAP_CHUNK_TARGET`, 8) compiles a FLAT path with no directory, no shift and no mask, and its
  run is exactly the declared width; wider tables reserve a ceiling count of chunks, so the last
  chunk's tail is paid for and unaddressable. **The width is per TASK, not per image**: root gets
  the sum below, every spawned thread gets `KICKOS_CAP_CHILD_WIDTH`, and the slab is carved from both
  classes rather than from the widest. Root's width is a configure-time SUM of
  four declarations -- the kernel's reserved range, the chosen service list's `RETAINED_CAPS`, the
  app's declared `CAPABILITIES` peak, and the peak concurrent INBOUND reply capabilities a thread's
  table must hold (`INBOUND_REPLY_CAPS` on the service list, `CAPABILITIES_INBOUND_REPLY` on the
  app, combined as the widest, and **0 by default** -- nothing in tree declares it). A client mints
  into the SERVER's table, so without that fourth term the sum is not a bound on when a thread's own
  mint can fail. Beneath the sum sits a floor that is nobody's declaration, the grant-list floor
  `KICKOS_MAX_SPAWN_GRANTS + 1`: it RAISES a width that falls below it rather than refusing it, so
  no app is asked to declare capabilities it does not hold, and it refuses only when the floor
  itself exceeds supply. The total is checked against the board's `KICKOS_CAP_TABLE_SUPPLY`, which
  is the only capability figure a board states. The width itself is
  **computed, and no board declares one**; three values come out across the fleet. **7** on the
  two supply-7 boards (`bluepill-c8`, `f302nucleo`), whose supply clamps the
  selftest's optional peak away, so the arms that wanted it reclaim and skip -- one flat chunk,
  224 B of `.bss`. **10** on a supply-16 board whose list retains nothing -- every preset default
  except `frdmk64f`'s and `xmc4800-relax`'s, `microbit` included, and equally under any
  `*_uartirq` list: root takes two chunks of 8 with a 6-slot unaddressable tail while every
  spawned thread takes one, 1216 B where `KICKOS_MAX_THREADS` is 16, 704 B where it is 8 and
  448 B where it is 4. **Do not recompute these -- read the `KickOS: cap table =` configure
  line, which is the authority.** Those are the widths ROOT is
  configured at; a spawned thread gets
  `KICKOS_CAP_CHILD_WIDTH` whatever the row says. **11** only when a
  RETAINING service list is selected -- exactly three declare `RETAINED_CAPS 1`
  (`system/CMakeLists.txt`): the two SPI lists `services_frdmk64f` and `services_xmc4800relax`, and
  the sim's UART list `services_simuart`. A `CapEntry` is
  8 bytes and **fully spent**: the global object handle, `CapType`, the rights bits, the cap-gen,
  and -- packed into the spare bits beside the type and the rights -- a `CAP_REPLY`'s 8-bit call
  sequence (`KCAP_REPLY_SEQ_LO_BITS` / `KCAP_REPLY_SEQ_HI_BITS`, seated by `cap_reply_seq_seat`).
  Even an empty slot's spare word is spoken for: a DEAD entry's `obj` holds the run's free-list
  links. That is why there is nowhere to put a parent link. Handles are **opaque** to
  userspace (never assume an array index). The table is a pure per-thread naming+rights layer that
  WRAPs the unchanged global object pools (`slotpool.h`), it does not replace them: object
  liveness is a global property (the pool + its refcount), capability possession is per-thread. Cost,
  stated honestly: per-thread table RAM + a resolve indirection per syscall + refcount traffic -- an
  isolation trade, not a free win.
- **One resolve chokepoint (`cap_resolve`).** It validates index / liveness / type / rights
  (`(rights & need) == need`), then WRAPs to the object pool, which re-checks the object-gen. Its
  precondition is the resolve contract: **the caller holds `IrqLock`, and the resolved pointer is
  used under the SAME continuous lock** -- so a concurrent `handle_close` cannot free the object
  between resolve and use. Two independent generations guard it: per-thread **cap-gen**
  (use-after-close) plus global **object-gen** (use-after-destroy), both `uint16_t`.
- **Rights are three bits, each enforced at a real site (no dead field):** `CAP_WAIT` (`sem_wait`),
  `CAP_SIGNAL` (`sem_post`), `CAP_TRANSFER` (may be delegated). Memory R/W/X is NOT a cap right --
  it lives in the MPU region descriptor and is enforced by hardware; duplicating it in the cap
  would be the forbidden checked-twice field.
- **Lifecycle -- refcounted destroy-on-last-close.** `sem_create` allocates the object (refs = 1)
  and returns a full-rights (`WAIT|SIGNAL|TRANSFER`) cap handle into the creator's table.
  `KOS_SYS_HANDLE_CLOSE(cap)` (type-agnostic, renamed from the old `sem_destroy`) drops MY handle:
  bump the slot cap-gen, empty the entry, `refs--`; the object frees only at the LAST close
  (refs -> 0). Thread exit closes every held cap (`cap_teardown`, called from `exit_current`) so
  no thread leaks references. A teardown-close that would strand a parked waiter LEAKS (floors
  refs at 1), never strands -- unreachable today (every parked waiter pins its own cap).
- **Well-known reserved cap indices (`system/include/kickos/sys/cap_index.h`).** Indices
  `[0 .. KICKOS_CAP_FIRST_DYNAMIC)` (today `[0..2)`) are reserved well-known slots: index 0 =
  `KOS_CAP_STDOUT` (the send-only console endpoint) and index 1 = `KOS_CAP_CLOCK`, held for a
  board's clock/time-service cap. Axis-3 authority is deliberately **not** one of them: the
  authority word names no pool object, holds no refcount and bumps no generation, so it lives in
  the TCB as `Thread::authority` (8 bits in existing padding) and costs no index on any board.
  `KOS_CAP_AUTHORITY` survives as a **pseudo-handle** -- `0x7FFFFFFF`, whose index field is the
  all-ones value the codec's capacity rule keeps out of every table, pinned by a `cap.h`
  static_assert -- so `kos_cap_narrow` still has a name for the word without the word owning a
  slot. **The authority width is 8 bits**, bounded by
  `Thread::authority` and `kos_thread_params::authority` alike: six are defined, so a seventh and
  eighth cost nothing and a ninth widens both fields. An
  **own-create** (`sem`/`mutex`/`endpoint` create) takes the head of the run's free list in O(1)
  (`cap_install` -> `cap_run_peek_free`) and **allocates nothing**, refusing `-KOS_EMFILE` when the
  table is full. It can **never alias a reserved slot**, and not because of a scan floor: the
  reserved plane is simply never THREADED ONTO the list -- `cap_run_free_build` starts at
  `KICKOS_CAP_FIRST_DYNAMIC`, and `cap_run_free_release` / `cap_run_free_unlink` no-op below it --
  so no pop can hand back a well-known index. A release goes to the list **tail**, never the head,
  so with `F` free slots each slot's cap-gen advances once per `F` mints instead of one counter
  taking every mint. A reserved slot is seated
  ONLY by the kernel (`cap_install_defaults` seats stdout, and is the sole writer of index 0) or
  by explicit spawn delegation, whose `i+1` packing lands delegated cap 0 on the reserved clock
  index. Userspace only *names* a reserved slot by these constants -- it never chooses the
  index. The range is not frozen, but either direction is an ABI break: renumber **downward** only
  for a slot nothing seats, keeping the usable dynamic count constant -- the width follows on its
  own, since the reserved range is one of the terms it is summed from; **append** by raising the last reserved
  index and `KICKOS_CAP_FIRST_DYNAMIC` together, which costs one slot on every table in the fleet
  -- so weigh it first against putting the state in the TCB, as the authority word does. The
  `cap.h` static_assert floors the dynamic count at >=1 either way.
- **B1 wire contract (every in-tree app that builds a spawn grant list depends on it):** a fresh
  child table has cap-gen 0 in every slot, so
  on a fresh table `handle == index`; delegation places delegated cap `i` at child index `i + 1`
  (so delegated cap 0 lands on the reserved clock index and every further one in the dynamic
  range), and `cap_install_defaults` seats the stdout
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
output). The syscall `kos_console_publish`, gated on `AUTH_CONSOLE`, performs the handover -- it
**relinquishes** the buffered path via `console_tx_deinit` (flush, disable the TX interrupt,
detach/NVIC-mask, disarm), takes a kernel ref on the userspace driver's stdout endpoint, then
flips the state to USER_OWNED last; a stale chip writer that raced the flip is drained (via the
`g_chip_writers` count, with the publisher yielding at lowered priority so a lower-priority
writer can finish) before publish returns. The **field panic path reclaims** the UART:
`kpanic_enter` calls `arch_console_reclaim` and flips to RECLAIMED, and `kickos_isr_fault`
funnels through `kpanic_enter` so a terminal fault in the driver still reclaims and polled-prints;
the diag LED stays the always-present 1-bit last resort. A chip `arch_console_reclaim` body
force-retakes the peripheral and rewrites every in-window register to a known baud/state, since
userspace config is untrusted; every chip has one except `mps2`, `nrf51`, `sam3x8e`,
`stm32f103`, `stm32f302` and `virt` (the live set is
`grep -rn "^void arch_console_reclaim(void)" arch/` minus the declaration in
`arch/include/kickos/arch/arch.h`; the `arch/arm/chip/xmc4800/usic_uart.cc` one is
silicon-witnessed), and those six keep the no-op fallback TU, so on them the reclaim is wiring
with nothing behind it (see [console.md](console.md)). Routing userspace output through a kernel syscall to "share" the device is
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

**Silicon.** Both halves are done and the second is what M2 closed:

- **Privilege boundary** -- flash; UART output matches the sim. GDB confirms the MSP/PSP split,
  unprivileged `CONTROL`, and SVC syscalls crossing the boundary.
- **Enforcement** -- per-thread MPU regions are loaded on every switch-in, and a wild
  cross-domain user write **traps**: MemManage on the ARM PMSA parts (XMC4800, i.MX RT1062,
  RP2040 v6-M, RP2350 v8-M), a SYSMPU-reported bus fault on K64F, the access exception via
  MPESTS/MPDEA on RX72M, and `mcause=7` under PMP on ESP32-C6. Proving the *negative* is the
  point: `mpu_fault` is a dedicated binary whose deliberate cross-domain store must NOT
  complete, because "the MPU is on" is not "the MPU protects" (Book ch.7.4).

Per-chip evidence, dates and the remaining non-gating tails are in `../m2-readiness.md`;
which of this CI re-checks on each push -- and which stays a bench step -- is in
`boards.md` ("CI coverage & cross toolchains").

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
