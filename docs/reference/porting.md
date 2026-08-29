<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS porting guide

This is the practical companion to `architecture.md`: how a new target implements
the `arch.h` seam. It also records the **M1 ARMv7-M spike** -- the design and
feasibility conclusion for the one mechanism the roadmap flagged as make-or-break.

The porting seam is `arch/include/kickos/arch/arch.h` (authoritative). A target
provides two halves:

- an **arch** backend (`arch/<arch>/`, e.g. `arch/arm/armv7m/`) -- the ISA-generic
  machinery: context switch, syscall trap, critical section, core timer/clock, NVIC;
- a **chip** backend (`arch/arm/chip/<chip>/`) -- the hardware edges: reset/startup
  + vector table, clock tree, UART console, `arch_init`/`arch_shutdown`, the
  linker script (which defines the user-RAM region `__kickos_ram_start/_end`), and
  `SystemCoreClock` (defined in the chip C, not the linker script). Optionally a
  chip may override `arch_diag_led_init`/`arch_diag_led_set` (the kernel
  diagnostic LED, `kdiag_led_*`); both have no-op fallback TUs
  (`arch/common/arch_diag_led_{init,set}_default.cc`), so a board with no known LED
  just leaves them out.

No KickOS seam is a weak symbol. An optional seam's fallback body lives ALONE in a
translation unit named `<symbol>_default.cc` that defines EXACTLY ONE global symbol, and a
backend's own definition must sit in an always-anchored archive member -- the rule is stated
in `arch/CMakeLists.txt` (lines 11-71) and detailed under *Privileged register write* below.

### Fault-reporter contract (panic must survive console handover)

A chip's fault/exception reporter MUST, before ANY console output: call
`kpanic_enter()` first, and emit only via `kprintf`/`console_emit` -- NEVER
`arch_console_write_sync` directly. Both are load-bearing once a board enables
console *device handover* (a userspace driver takes the UART): `kpanic_enter`'s
reclaim branch re-seizes + re-inits the relinquished UART (`arch_console_reclaim`),
and `console_emit` honors `ConsoleState` (drops the chip path while USER_OWNED,
routes to the polled writer once RECLAIMED). A reporter that prints before
`kpanic_enter`, or pokes the sync writer directly, would emit to a relinquished
(possibly dead) UART and SILENTLY lose the panic banner -- the worst failure mode
in the system. All six current fault reporters satisfy this; a new arch port must
too. Additionally, before a board turns on handover it must supply a real
`arch_console_reclaim` body (the generic fallback,
`arch/common/arch_console_reclaim_default.cc`, is a no-op -- a silent
reclaim failure otherwise). Every chip in the tree has one except `mps2`, `nrf51`,
`sam3x8e`, `stm32f103`, `stm32f302` and `virt`; the current set is whatever
`grep -rn "^void arch_console_reclaim(void)" arch/` reports, minus the declaration in
`arch/include/kickos/arch/arch.h`.

**Reclaim depth is a full in-window REWRITE, not a re-run of `*_init`.** A body must drive
EVERY writable register inside the driver's granted window to a known polled-ready value,
including the ones `*_init` never writes because it relies on their reset defaults -- which is
exactly the set a buggy or hostile driver reaches, and therefore the true silent-loss source.
On `mk64f` those are `MODEM.TXCTSE` (a bounded polled writer waits forever on an absent CTS
and drops every byte), `C3.TXINV`, `C5` (the peripheral DMA request enables), `S2`, `IR`,
`C7816` and the FIFO pair; on `xmc4800` it is `KSCFG.MODEN` (a gated channel clock). Write the
body as straight-line ABSOLUTE stores with no read-modify-write: the reclaim is re-entrant from
a nested fault, absolute writes are safe to repeat and an RMW on a garbled value is not
(`arch/arm/chip/mk64f/chip_mk64f.cc`, `arch/arm/chip/xmc4800/usic_uart.cc`). A body MAY trust
everything OUTSIDE the window -- pin mux, SCU/SIM clock gates, the clock tree -- because the
MMIO grant is exact-window and those live in privileged-only peripherals the driver could never
reach. The idempotence is load-bearing rather than incidental: the panic gate is
`state != RECLAIMED`, so the body runs on devices no driver ever touched
(`invariants.md`, `panic-console-probe-independent`).

**Decode BOTH fault banks, and print the address.** An ungranted device access does not
reliably surface as the MPU's own fault: on some Cortex-M a peripheral-bridge error response
arrives as a **BusFault** rather than MemManage, and on SYSMPU it arrives as an imprecise bus
error with the MPU's own registers carrying the real story. A port's reporter must therefore
decode both and name the faulting address. The shared ARMv7-M reporter is the worked example
(`arch/arm/armv7m/arch_armv7m.cc`, `kickos_armv7m_fault_report`): a set MMFSR byte
(`CFSR[7:0]`) selects the `MPU FAULT` label, `MMFAR` is printed only when `MMARVALID`
(`CFSR` bit 7) is set and `BFAR` only when `BFARVALID` (bit 15) is set -- both hold stale
contents otherwise -- `BFSR.IMPRECISERR` (bit 10) is called out so a reader does not read the
stacked PC as the culprit, and then the `arch_fault_report_extra()` chip hook runs. That hook
is where a bus-side unit reports: `mk64f` reads SYSMPU `CESR`, decodes the per-slave-port
`SPERR` nibble, and says so explicitly when NO protection error is latched -- which is the tell
for a peripheral-bridge fault rather than an MPU one.

### Fault-isolation contract (a faulting thread takes its task and nothing beyond)

**The rule.** A fault taken in unprivileged thread context, in a thread that is not already
dying, kills that thread and its TASK; every other fault panics exactly as before. The scope
is the task because siblings share the address space the faulting thread was writing, and a
plain spawn is a thread of the caller's task. A backend implements no part of that scoping: it
decides only WHETHER the fault is a thread's own, and `sched::exit_current` draws the rest. The
core half is `kernel/init/fault.cc`: a backend's fault handler calls
`kickos_fault_kill_thread(frame)` BEFORE it starts its dump and simply RETURNS when that
answers true, and the exception return then lands in `kickos_thread_fault_exit`. The
reasoning is in `../design-m4.7.9-fault-isolation.md` (sections 3 and 4).

**The two seams**, both declared in `arch/include/kickos/arch/arch.h`, both optional. Their
fallback bodies (`arch/common/arch_fault_is_user_thread_default.cc`,
`arch/common/arch_fault_redirect_to_exit_default.cc`) decline and do nothing, so a backend
that has not been ported keeps today's panic path with no edit at all.

- `bool arch_fault_is_user_thread(void* frame)` answers whether the CPU was **unprivileged**
  and in **thread context** at fault time. The thread's identity is NOT the answer, and
  neither is `ctx.resting_npriv`: syscall dispatch runs PRIVILEGED on a stack the calling
  thread itself owns -- its own kernel block where `KICKOS_KERNEL_STACKS` is 1, its own
  thread stack where it is 0 (`invariants.md`, `syscall-in-priv-thread-context`) -- so a
  fault there is a kernel bug in code the thread merely called and it must still panic.
  armv7m and armv6m read
  `CONTROL.nPRIV` plus the stacked `xPSR` IPSR field (exception entry does not modify
  `CONTROL`, so the read gives the privilege at fault time); rv32imac reads `mstatus.MPP`,
  the privilege before the trap, out of a CSR; the sim has no ring and reads its own
  `raised` / `isr_depth` pair, which carries the same fact.
- `void arch_fault_redirect_to_exit(void* frame)` rewrites the resume context so the
  exception return lands in `kickos_thread_fault_exit`, privileged, in thread mode, on the
  faulting thread's own stack, and hands the fault facts to `kickos_fault_record`. It is
  called ONLY after the predicate returned true. The thread is dying, so its register values
  need not be preserved and the stub takes no arguments. Concretely: ARM rewrites the stacked
  PC, keeps the T bit and CLEARS the IT/ICI bits (a fault inside an IT block would otherwise
  resume with stale condition state and conditionally skip the stub's first instructions),
  then clears `CONTROL.nPRIV`, which exception return does not restore; RISC-V writes `mepc`
  and sets `mstatus.MPP` to M. A backend whose fault-status registers are sticky and
  write-1-to-clear must also clear them here: unlike the panic path this fault is not the
  last, so a bit left set mislabels the NEXT thread's fault with this one's status.

**The handler MUST NOT PRINT.** Printing from a fault handler forces `kpanic_enter`, which
masks interrupts and reclaims the console permanently (the contract above, and
`invariants.md`, `panic-console-probe-independent`), and a system that is meant to survive
this fault cannot pay that. The facts go to `kickos_fault_record` and are printed later by
`kickos_thread_fault_exit`, in thread context, through the ordinary `kprintf` path. That is
why the kill test sits ahead of the first output AND ahead of `kpanic_enter` in
`kickos_armv7m_fault_report` and `kickos_rv_fault_report`.

**Proving the frame is trustworthy.** A port must know which of its fault facts it may
believe before it reads them, and this is the part that is easy to get wrong.

- A fact read from a REGISTER (privilege, cause, fault-status, fault-address) is always
  valid: nothing the thread did to its stack can forge it.
- A frame that lives in MEMORY on the faulting thread's stack must be gated by
  `kickos_fault_frame_trusted(frame, bytes)` BEFORE a single word of it is read. A wild SP
  hands the handler a frame the thread never legitimately produced; the test is that the
  whole frame lies inside the running thread's own recorded stack, and it fails closed on no
  current thread, on idle, and on a thread with no recorded stack.
- On a STACK OVERFLOW the abort happens during HARDWARE STACKING: the stacking decrements SP
  before the writes, so on an abort the frame pointer names memory that was never written.
  armv7m therefore ALSO declines on the CFSR stacking-error bits MSTKERR/MUNSTKERR (bits 4/3)
  and STKERR/UNSTKERR (bits 12/11), mask `0x1818`, read out of the register before the frame
  is touched. `MLSPERR` and `LSPERR` are deliberately EXCLUDED: lazy FP preservation leaves
  the integer frame valid, and including them would panic a legitimate user fault.
- The two tests do NOT subsume each other and a port needs both wherever both apply. The
  CFSR bits catch a stacking abort whose SP was still in range; the bounds test catches a
  frame the hardware wrote in full at an SP the thread had no business holding, which sets no
  CFSR bit at all. A core with no fault-status register (v6-M) has only the bounds test.
  rv32imac has no stacking abort to detect (its trap prologue is software and runs M-mode,
  which bypasses the unlocked PMP entries), so on that backend an overflowed thread's frame
  is written SUCCESSFULLY below its own stack and nothing on the fault path would say so.
- **THE FAULT-PATH TESTS ARE THE SECOND LINE, NEVER THE FIRST, AND A PORT THAT STOPS HERE
  SHIPS A PRIVILEGE ESCALATION.** Everything above runs AFTER a write has landed, and only
  when the trap was a fault. A software prologue also stores on the ecall and interrupt
  paths, where nothing faults and none of these tests run. So a port whose prologue is
  software owes an ENTRY-path check, before its first store, and it owes three things the
  fault-path tests do not:
    1. the interrupted stack pointer must be in the running thread's stack AND aligned;
    2. the check must be an EXTENT in the direction the frame grows, not a pointer test.
       `sp == stack_lo` passes a range test and then puts the whole frame outside the grant,
       and a bump allocator with no padding puts a NEIGHBOUR thread's granted region there;
    3. the extent must cover the kernel's own descent and not just the frame, because
       `syscall_dispatch` runs on the caller's continuation (`arch.h`, the `arch_syscall`
       contract). That figure is measured per build, never written by hand: see
       `tests/static/check_trap_redzone.sh` and the per-arch trap-stack headers.
  Order the arithmetic so it cannot wrap, which means taking the subtraction only after the
  lower bound is proven; `kickos_fault_frame_trusted` in `kernel/init/fault.cc` is the shape
  to copy. A core that stacks in HARDWARE at the pre-exception privilege gets leg 1 free for
  the hardware frame only: any registers the port pushes itself go BELOW the checked range
  and need the same treatment. See `docs/book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md`.
- **A core whose exception CANCELS the faulting instruction has NO moved SP to read, and needs a
  third test.** Both tests above rest on an SP that moved: armv7m's on a stacking abort, rv32imac's
  on a software prologue that wrote the frame at one. RXv3 restores the architectural state of the
  cancelled instruction (RXv3 ISA UM sec.5.3.1), so after an overflow the USP reads exactly as it did
  before the denied push and the bounds test passes with nothing left below it. Its frame is on the
  kernel ISP, which no thread can invalidate, so that test says nothing either. `bool
  kickos_fault_below_stack(uintptr_t addr)` is the third one: it answers whether the faulting ADDRESS
  landed beneath the running thread's stack, which a downward-growing stack makes an EXACT test for
  the overflow class, and it fails closed when there is no recorded stack. rxv3 applies it to `MPDEA`
  on an operand-access MPU error. **The converse is not exact and the cost is measured, not argued:**
  a cross-domain access to a LOWER address escalates to the panic dump instead of dying alone
  (`mpu_fault` on `rx72m`, `0x13200`, below `domainA`'s stack), while one to a higher address dies
  alone (`rxdrv`, `0x8c068`). A port on any instruction-cancelling ISA inherits this and should read
  `../design-m4.7.9-fault-isolation.md` section 4.2 before reaching for a distance threshold instead:
  a threshold fails in the UNSAFE direction, because a frame larger than the threshold puts privileged
  code back on an exhausted stack.
- Worth one line of history: an earlier implementation read the stacked IPSR field straight
  out of that stale RAM and declined only BY ACCIDENT, because those bytes happened to be
  non-zero. Had they held zero, the kernel would have rewritten and resumed a fabricated
  context, privileged.

**The classification a new port must state** (top-level `CMakeLists.txt`).

- `KICKOS_HAVE_PRIV_RING` answers whether the core has a privilege ring to read at all.
  Every discriminator above is one, so a core without one can never tell a thread's own fault
  from a kernel bug in code it called and MUST keep panicking. It is per BOARD and not per
  arch, because the ARMv6-M Unprivileged/Privileged Extension is OPTIONAL and separate from
  the MPU extension: Cortex-M0 omits it, Cortex-M0+ implements it, and both are
  `KICKOS_ARCH=armv6m` with no predefined macro between them. armv6m is therefore ENUMERATED
  per `KICKOS_BOARD` with a `FATAL_ERROR` default, so a new armv6m board must declare its
  class instead of silently inheriting one.
- `KICKOS_FAULT_ISOLATION` is the derived flag, and it does two jobs from one fact. It
  selects the backend seam TUs (`arch/CMakeLists.txt` drops the two declining fallbacks from
  the shared list, and adds `arch/arm/armv6m/arch_armv6m_fault.cc` only where it holds), and
  spelled as `KICKOS_FAULT_OUTCOME` (`panic` or `thread-kill`) it is the token the fault
  gates parse. The link and the test therefore assert the same fact, and no gate re-derives
  the posture from an arch name of its own. `tests/integration/check_faultsurvive.sh` is the witness.
- It is NOT gated on `KICKOS_HAVE_MPU`. Privilege comes from `arch_context_init` and `kmain`
  spawns root unprivileged in every posture, so a FLAT board kills a faulting thread exactly
  as an enforcing one does. What differs on a flat board is only which accesses fault at all:
  a cross-domain write completes there instead of trapping.

**Why lx6 declines, and what rxv3's opt-in costs.**

- lx6 CANNOT. `PS.UM` is 1 for kernel and thread alike and `arch_context_init` discards
  `privileged`, so it has no privilege ring and `KICKOS_HAVE_PRIV_RING` refuses it. This is a
  HARDWARE fact, not an unported feature: there is no unprivileged thread to kill, so the rule has
  nothing to discriminate and no amount of backend code would give it something.
- **rxv3 opted in during M4.8.3 and is witnessed on SILICON ONLY.** It has an exact discriminator
  (`PSW.PM`), its exception frame sits on the kernel ISP, and the redirect is the syscall trap's own
  PSW rewrite: clear `PM`, set `U`, so the `rte` lands supervisor-on-USP exactly where
  `svc_trampoline` runs. It declined until M4.8.3 because that `rte` had never executed -- there is
  STILL no RXv3 emulator anywhere in this tree and still no CI gate -- and what changed is that
  `rx72m` was on the bench. Treat every rxv3 claim here as unfalsifiable off that board.
- rxv3 also needs a cause filter that the register-only backends do not. The fixed-vector offset is
  the only thing that names the exception, no register carries it, so the backend's `frame` is a
  small struct pairing the saved PC/PSW with that offset. Only the five instruction-cancelling
  causes (`0x50` privileged, `0x54` access, `0x5C` undefined, `0x60` address, `0x64` FP) may kill a
  thread; the `_rx_trap` catch-all is cause `0`, and it carries the NMI, which is accepted at an
  instruction boundary with `PSW.PM` still set. Without the filter a chip-level NMI would kill
  whichever thread happened to be running.
- The sim carries an extra term (an x86_64 host) because its seam rewrites the host
  `ucontext` register file directly. On a host layout it does not know
  `arch_fault_is_user_thread` declines, and the gates would then assert an outcome the
  runtime cannot produce.

### Thread pointer and kernel stacks (what a new arch owes)

Two mechanisms are ARCH facts before they are board ones, declared as capability symbols
in `arch/Kconfig`. **That help text is the authority**; what follows says only which
question each symbol answers and who answers it.

- **`ARCH_HAS_TLS`** -- the arch can hand a thread its own thread pointer from
  UNPRIVILEGED code, with no syscall and no kernel read: a register the kernel writes at
  restore, or a leaf the kernel provides. `arch/Kconfig` names which arch answers how, and
  names the one that CANNOT -- `ARCH_SIM` does not select it, its threads being `ucontext`
  coroutines in one host thread sharing one `%fs`. So the arch with the largest test suite
  cannot witness `KICKOS_TLS` at all, and a new arch must state which of the two shapes it
  has before writing any of it.
- **`ARCH_HAS_KERNEL_STACKS`** -- the arch's trap entry can transfer to a per-thread kernel
  stack, and its `arch_context` carries the `kernel_sp` that entry loads. NECESSARY AND NOT
  SUFFICIENT: whether a given board actually carves the blocks is `KICKOS_KERNEL_STACKS`,
  which asks this AND the chip's `HAS_MPU`.
- **`ARCH_KERNEL_STACKS_MANDATORY`** -- stronger, and deliberately NOT implied by the one
  above: the arch's `switch.S` compiles the transfer with no red-zone path beside it, and
  its backend `static_assert`s the knob non-zero. An arch that writes only the transfer
  selects it; one that writes both paths does not, and then the red zone is what its
  unconverted boards depend on. armv7m is the one arch carrying both.

**The escape a too-small part takes is `KICKOS_TLS=n`.** With TLS on every arena block is
strided by a power of two, so `KICKOS_USER_STACK_SIZE` and `KICKOS_ROOT_STACK_SIZE` must
BE powers of two and must be the SAME one -- the root `CMakeLists.txt` refuses each failure
by name at configure, and its message names the two ways out. On a part where that rounding
costs a thread, turning the knob off costs a `thread_local` and keeps the thread;
`f302nucleo` and `bluepill-c8` both make that trade. There is no equivalent escape from a
mandatory kernel stack, which is why an arch selects that symbol only once its entry has no
other path to compile.

**`errno` is neither of these.** newlib reaches its reentrant state through `_impure_ptr`
and calls `__errno` nowhere in the pinned toolchains, so a per-thread `errno` follows
The per-thread reentrant state (one `struct _reent` per slot in the app-data window) and
never `KICKOS_TLS`. A port wanting both turns on both.

### Adding a board/chip (the five edit points)

1. `boards/<board>/board.cmake` -- the board descriptor: one file setting
   `KICKOS_ARCH` and `KICKOS_CHIP` (empty for the sim), plus a CPU flag
   (`KICKOS_MCPU` / `KICKOS_MFLOAT_ABI`) only where the board genuinely differs
   from its chip's baseline (a float ABI, or an emulated core on the mps2 QEMU
   boards). The build's board resolver (`cmake/kickos.cmake`,
   `kickos_load_board_descriptor`) and the ARM cross toolchain
   (`cmake/toolchain-arm-none-eabi.cmake`) both include this file pre-`project()`,
   so arch/chip/CPU can never disagree between the two.
2. `arch/arm/chip/<chip>/cpu.cmake` -- the chip's own CPU baseline (`KICKOS_MCPU`,
   `KICKOS_MFLOAT_ABI`): the core and its FPU are a **chip** fact, not a board
   one, so this is where `-mcpu`/`-mfpu`/`-mfloat-abi` actually live. The
   toolchain file includes it right after the board descriptor and only fills
   in what the board left unset (`if(NOT DEFINED ...)`); a bare-metal board that
   resolves neither value here nor in its own `board.cmake` is refused at
   configure. A new chip must ship one.
3. `CMakePresets.json` -- add a configure + build preset (only for boards that
   actually build/link today).
4. `arch/arm/chip/<chip>/` -- the chip sources (`*.cc`, `*.S`, auto-globbed), a
   linker script named exactly `<chip>.ld`, which must `#include <sections.ld.h>`
   and invoke `KICKOS_CODE_DEBRIS_SECTIONS`, `KICKOS_NONALLOC_SECTIONS`,
   `KICKOS_STATIC_RELOC_ASSERT` and `KICKOS_TLS_TEMPLATE` alongside the two arena
   asserts -- every cross link passes `-Wl,--orphan-handling=error`, so an input
   section no rule names is a LINK failure printing a section name and no hint of
   what is owed, and configure refuses a script missing any of the four by name
   instead (`arch/CMakeLists.txt`),
   `arch/arm/chip/<chip>/include/kickos/chip_limits.h` with the chip's own constants
   (configure REFUSES a chip that ships none), and
   `arch/arm/chip/<chip>/include/kickos/chip_mmap.h` with its peripheral base
   addresses. CMake derives the dir from the chip name, puts it on the include path,
   and installs it -- **no root-CMake edit needed** (this used to be a
   silently-failing step).
5. `boards/<board>/configs/<variant>/defconfig` -- the board's configuration, one
   complete statement per variant. At least a `base`; configure REFUSES a board that
   ships none, and names the variants it does ship.

`boards/<board>/` is also where a `<chip>.ld` linker override lives for a shared chip,
and `boards/<board>/Kconfig` where its own options go -- proven on the `stm32f411` pair
(f411disco + blackpill), which share a chip and differ in five wiring facts.

**Two headers, because there are two kinds of fact**, and both are pure-`#define` so
`startup.S` can include them. `kernel/include/kickos/config/{board,system}.h` pull in
both.

`chip_limits.h` holds the chip's CONSTANTS, defined **unconditionally** on the model of
`kernel/include/kickos/config/limits.h`: `KICKOS_MAX_IRQ` (the NVIC or ICU line count,
which the `startup.S` vector table derives its `.rept` from, so the vector table and
the kernel IRQ table are **one fact** rather than the same number in two files) and, on
the RX, `KICKOS_RX_INTB_ENTRIES`. Nothing configures these and no option's availability
depends on them, which is exactly why they are not knobs: a smaller value would shrink
the kernel table and the vector table together and strand the lines above it, with
nothing to catch it. A configuration cannot set them, and a chip that ships no
`chip_limits.h` is refused at configure rather than falling back on the sim's 32.

`board_config.h` holds the provisioning KNOBS: `KICKOS_MAX_THREADS`, the idle/root/user
stack sizes sized to the chip's SRAM (too big and the link fails on the linker-script
RAM `ASSERT`), and the pool sizes. **There is no such file in the source tree**: it is
GENERATED into the build tree from the board's defconfig, and it is what the whole fleet
compiles against. That is the other reason the chip's constants sit in their own headers
with their own include guards -- a generated header on the front of the include path
would otherwise shadow them.

A knob therefore has exactly ONE place to be set, its defconfig, and a `-DKICKOS_...=`
on the CMake line becomes a REQUEST against the declarations rather than a define behind
their back. There is no per-board-versus-per-chip precedence to learn and no partial
override to get wrong: earlier this was an if/else between two headers sharing one
include guard, so a board that restated one knob silently lost every other value its
chip had set.

### Configuration from Kconfig

A board's configuration is `boards/<board>/configs/<variant>/defconfig`.
`tools/kconfig/genconfig.py` resolves the declarations and writes three things into the
build tree's `generated/` directory: `.config` (the audit surface), a `board_config.h`
under `include/kickos/`, and a `kickos_config.cmake` fragment that the root
`CMakeLists.txt` includes once. The generated
include directory precedes the board's, and both headers carry the same include guard,
and there is no source `board_config.h` for it to shadow.

Nothing about a board's provisioning is stated in CMake. The knobs are declared in
`Kconfig` with a `range` and a `default`, the arch and chip facts in `arch/Kconfig`, the
board stanzas in `boards/Kconfig`, and the board's own values in its defconfig, which
states only what differs from the declared defaults.

**A preset selects a board and a variant and nothing else**, in NuttX's
`<board>:<variant>` spirit, and the memory-protection posture is part of what a variant
states. So a board that can enforce says so in the variants that do
(`CONFIG_MEMORY_MODEL_MPU=y`) and carries a `flat` variant where the non-enforcing build
is wanted; the enforcing posture is not something a `-D` flips in an existing build
directory, and the stale-cache trap that made a fresh directory mandatory per posture
cannot be expressed. A variant defconfig is a COMPLETE statement rather than a delta on
`base`, which is what `savedefconfig` writes back and what makes any one of them
readable on its own.

**A value the declarations do not permit is REFUSED at configure, not defaulted.**
kconfiglib warns on an out-of-range integer and falls back on the symbol's own default,
which is the fleet value rather than the one asked for, so a board could otherwise be
handed a LARGER pool than its defconfig specified. The generator reads every requested
value back after resolution and exits non-zero on a mismatch, naming the symbol, the
range or the unmet dependency, and what it actually resolved to. The same applies to a
`-D` on the CMake line: on a crossed board it becomes a request the generator can refuse,
including one that names no symbol at all.

`kconfiglib` is then required. It is build-time only, one pure-Python file, ISC, and
never reaches a shipped artefact. Put it in a venv of its own and name the interpreter
absolutely through `KICKOS_KCONFIG_PY`; do not put that venv's `bin/` on `PATH`, where
its `python3` shadows the interpreter the ESP flash tools run on. `cmake/kconfig.cmake`
prints the exact recipe when it cannot import it. A packaged `kconfig-mconf` parses this
tree unmodified and can drive `menuconfig` interactively, but it cannot replace the
generator: the read-back refusal above is the generator's, and the C implementation has
the same silent out-of-range fallback.

**An `AT` clause in `<chip>.ld` is only valid if the LOADER honours LMA.** This is
LOADER-dependent, not arch-dependent, and both sides of the decision look identical in the
linker script.

Broken case, fixed in M4.5.6: `arch/riscv/chip/esp32c6/esp32c6.ld` linked `.data`
`> RAM AT > RAM` with an explicit VMA at `ORIGIN + 128K` while the load counter kept
counting from the end of `.text`, so `_sidata` landed OUTSIDE every loaded segment. esptool
`elf2image` builds the bootable image from VMAs, so those bytes were never loaded, and
`Reset_Handler` then copied uninitialised SRAM over the `.data` the ROM had already placed
correctly. Measured on silicon: `sidata=40806810 sdata=40820000 ... src+1c=ee95242c
dst+1c=00000000`. Because the corrupting bytes are uninitialised SRAM, the symptom varies
by die and power-on history -- it presented as a board that reached `entry 0x40800000` and
then went silent, and bisecting found nothing. The fix was to drop the `AT` and add
`ASSERT(_sidata == _sdata)` so it cannot regress. `arch/xtensa/chip/esp32/esp32.ld`
carried the same latent construct and took the same assert (image-neutral there).

CORRECT case, and the trap: `arch/riscv/chip/virt_rv32/virt_rv32.ld` carries the SAME
`> RAM AT > RAM` construct and the SAME divergence (`_sidata = 0x80010ce8` against
`_sdata = 0x80020000`), and there it is RIGHT -- QEMU's ELF loader places each segment at
its PhysAddr, so `.data`'s bytes really are at the LMA and the `Reset_Handler` copy does
real work. The `qemu-riscv` `+MPU` 15/15 gate is the proof. **Do NOT add the
`_sidata == _sdata` assert to `virt.ld`**; a comment in that file already says so.

So: decide which side your loader is on BEFORE writing an `AT` clause, and pin the decision
with an `ASSERT` either way. `docs/reference/boards.md`, *M4.5.6*, holds the wire evidence.

**A `*pattern*` in an input-section spec matches the archive MEMBER name, never the archive
path.** GNU `ld` tests such a pattern against each input's own filename: for an object named on
the link line that is its path, but for an archive member it is the MEMBER name, and the
enclosing archive's name is not part of the candidate string. That is the mechanism behind the
selector inversion
every enforcement `<chip>.ld` now carries: `*user*(.data)` matched member basenames only, so it
silently missed `libkickos_user.a(newlib_stubs.o)` -- and with it the heap arena and the whole
toolchain runtime, none of whose members happen to be spelled "user". Naming an archive needs
the `archive:member` COLON form (`*libkickos_kernel.a:*(.data .data.*)`), and this binutils does
not match archive members inside `EXCLUDE_FILE` at all, so a bare `*libkickos_kernel.a` there
matches nothing (verified with `nm`). Both facts are why the enforcement layout is written
INVERTED: capture the CLOSED KickOS-owned set first, by colon selector, then let the app
catch-alls take everything else, so an unmatched or newly added archive lands app-side
(reachable) rather than kernel-side (faults). A path-substring selector is also
build-path-sensitive -- a checkout under `/home/user/` makes foreign object paths candidates --
which the colon form removes.

A chip whose flash boot needs a checksummed second stage (RP2040 boot2) adds a
fifth point: `cmake/<chip>_checksum.py` plus a `boot2.S`/`boot2.ld` in the chip
dir; the build wires the multi-stage boot2 image automatically (keyed on
`${KICKOS_CHIP}`). A chip on a non-ARM ISA additionally needs a new arch backend +
toolchain file -- see `arch/xtensa/` (ESP32) or `arch/rx/` (RX72M) for worked examples.

Status: eight arch backends (**armv7m** Cortex-M3/M4/M4F/M7/M33, **armv6m** Cortex-M0/M0+,
**armv8a** Cortex-A53, **rxv3** Renesas RX72M, **lx6** Xtensa/ESP32, **rv32imac** RISC-V,
**rv64imac** RISC-V, **x86_64** Intel/AMD 64-bit) across the chips below, plus the host `sim`.
The count is the directory set under `arch/` that carries a backend: `arch/arm/armv6m`,
`arch/arm/armv7m`, `arch/arm64/armv8a`, `arch/riscv/rv32imac`, `arch/riscv/rv64imac`,
`arch/rx/rxv3`, `arch/xtensa/lx6` and `arch/x86/x86_64`.
"MPU" is whether the chip ships an enforcement backend: `arch/<family>/chip/<chip>/mpu.cmake` on a
region chip, `aspace.cmake` on a translating one. Where it does, cross-domain trapping is
silicon-proven unless the row says otherwise:

| Chip | Board | Core | MPU | Validation |
|------|-------|------|-----|------------|
| `mps2` | qemu / qemu-m33 / qemu-m7 / qemu-m3 | M4F / M33 / M7 / M3 | PMSAv7 + PMSAv8 | QEMU (four runnable CI gates, **plus runtime enforcement gates** on both PMSA revisions) |
| `nrf51` | microbit | M0 | -- | QEMU (runnable CI gate) |
| `virt` | qemu-riscv | RV32IMAC | PMP | QEMU (runnable CI gate, **plus a runtime enforcement gate**) |
| `virt_rv64` | qemu-riscv64 / qemu-riscv64-sv48 | RV64IMAC | **Sv39 + Sv48 MMU** | QEMU, **NOT IN CI**: `.github/workflows/ci.yml` carries no rv64 job, so both postures are LOCAL `ctest` only (`--preset qemu-riscv64` and `--preset qemu-riscv64-sv48`), 52 arms each (re-derived 2026-08-29 by `ctest -N`), **plus runtime translation-enforcement gates**: an UNPRIVILEGED read of an unmapped page (`qemu_riscv64_aspace_ufault`, which replaced a kernel-side `aspace_fault` arm on 2026-08-29 that faulted before it reached the unmap), a stack guard, a kernel-half denial and a surviving fault. The gap is a decision nobody has taken, not a toolchain gap (`../reference/boards.md`, *CI coverage*) |
| `virt_arm64` | qemu-arm64 | Cortex-A53 | **VMSAv8 MMU** | QEMU (runnable CI gate, the `qemu-arm64` job, 42 ctest arms re-derived 2026-08-29 by `ctest -N`, **plus runtime translation-enforcement gates**) |
| `q35` | qemu-x86_64 | x86_64 | -- | QEMU, **NOT IN CI**: `.github/workflows/ci.yml` carries no x86_64 job, so the board is LOCAL `ctest` only (`--preset qemu-x86_64`), booted as a PE32+ UEFI application under OVMF. The chip selects no memory family, so the map is flat and there is no enforcement gate to run (`../reference/boards.md`, *CI coverage*) |
| `xmc4800` | xmc4800-relax | M4F | PMSAv7 | **hardware** (LED + USIC VCOM console over the buffered ring; enforcement + the canonical per-thread peripheral-isolation proof) |
| `stm32f411` | f411disco / blackpill | M4F | PMSAv7 | **hardware** (LED + UART + ping-pong; enforcement selftest + `mpu_fault` MemManage denial + an unprivileged root, all on `f411disco` 2026-07-29). Witnessed on one of the two boards; `blackpill` shares this backend and was not re-run |
| `stm32f302` | f302nucleo | M4 | -- | **hardware** (LED PB13 + console; the full suite at the `f302nucleo-st` provisioning -- 63 ok / 0 not ok / 5 skipped on 16 KiB SRAM, measured at `124b68c`). Not an enforcement target: the F302R8 line has no MPU, so `arch_mpu_min_region()` returns 0 (`arch/arm/chip/stm32f302/chip_stm32f302.cc:321`) |
| `stm32f103` | bluepill-c8 | M3 | -- | **hardware** (F103 port HW-proven on the now-retired 10 K clone, 2026-07-14; RAM-limited selftest; c8 build-only). No MPU: the degraded privilege-only build |
| `rp2040` | picopi | M0+ | PMSAv6-M | **hardware** (selftest over UART0/GP0; v6-M cross-domain fault silicon-proven 2026-07-19) |
| `rp2350` | pizero2350 | M33 | **PMSAv8** | **hardware** (enforcement selftest + `mpu_fault` MemManage denial + bench/soak). Reuses the `armv7m` backend verbatim; only the MPU descriptor shape differs |
| `mk64f` | frdmk64f | M4F | SYSMPU | **hardware** (revalidated 2026-07-15: full selftest + buffered console ring; **unprivileged root on the FULL service list**, 2026-07-29). SYSMPU enforces SRAM/domains but is bus-slave-side, so it cannot gate peripherals; the `AIPS0` PACR half of that is `arch_periph_enable`'s, below |
| `imxrt1062` | teensy41 | **M7** | PMSAv7 + fixed | **hardware** (enforcement selftest + soak). The only speculating core: needs the fixed-region wrap (`../design-teensy-mpu-hang.md`) |
| `rx72m` | rx72m | RXv3 | RX MPU | **hardware** (selftest + SCI6 console; DPFPU switch; enforcement + a granted peripheral window). **No CI gate** -- see below |
| `esp32` | esp32-wroom | Xtensa LX6 | -- | **hardware** (selftest + console, 240 MHz). No per-domain MPU and no privilege split |
| `esp32c6` | esp32c6-wroom | RV32IMAC | PMP | **hardware** (selftest + buffered ring console; first real peripheral IRQ; enforcement + peripheral isolation). Peripheral access also needs the one-time bus-side APM open, which `arch_init` programs at boot |
| `sam3x8e` | due | M3 | -- | port proven on silicon (2026-07-09); test unit retired (peripheral-I/O fault) |

Build-only chips are verified by construction (register review + image
inspection); flash to a board to confirm. `apps/blink` is a no-UART LED smoke
test available on every board with a known LED.

**What CI does and does not re-check.** Not every row above is defended by a green CI run, and a
porter needs to know which. Enforcement is gated at RUNTIME on the sim (`mprotect`), `virt` (PMP)
and the four `mps2` images (full TAP suite as unprivileged threads plus a real MemManage denial;
PMSAv7 on the M4/M7/M3, PMSAv8 on the M33). The silicon boards get an enforcement **build** sweep
instead, and it is worth having on its own -- it compiles their enforcement-only link surface,
including `arch_reserved_blocks`, which has **no fallback TU on purpose**, so an enforcing port
that forgets to declare its reserved set fails to LINK rather than leaving a silent open hole --
but their chip-specific trapping (SYSMPU, the M7 anti-speculation wrap, PMSAv6) stays
silicon-proven. Xtensa is build-only (no upstream ESP32 machine model), and
**Renesas RX has no CI gate at all**: RX72M needs `-misa=v3`/`-mdfpu`, which exist only in the
registration-gated Renesas GNURX build. So a change to the arch seam is unverified for RX until
you build it yourself. Per-ISA detail: `boards.md` ("CI coverage & cross toolchains").

### Cross toolchains (before you configure anything)

A cross build resolves its compiler through a per-family hint variable, seeded from the
environment, overridable with `-D`, falling back to `PATH` when empty:
`KICKOS_ARM_TOOLCHAIN_BIN`, `KICKOS_RISCV_TOOLCHAIN_BIN`, `KICKOS_RX_TOOLCHAIN_BIN`,
`KICKOS_XTENSA_BIN`. No toolchain file carries a default compiler path, so **keep local pins in
`.session/env.sh` (gitignored) and `source` it first**; CI sets the same variables and puts the
pinned tarball's bin on `PATH`. Both routes matter: CMake's `try_compile` re-reads the toolchain
file with a fresh cache, so it inherits the environment and `PATH` but never a `-D` cache entry --
which is why each toolchain file re-exports the resolved value into the environment.

**The hint is convenience; the capability check is the safety net.** `find_program` HINTS fall
through to `PATH` when the hinted directory is absent, so a fresh clone on another host can
silently resolve a distro cross-gcc -- and Debian's `arm-none-eabi-g++` is C-only picolibc with no
`libstdc++`/`libsupc++` for any multilib, which used to surface ~40 build steps later as
`fatal error: exception: No such file or directory`. The ARM and RISC-V toolchain files therefore
probe the compiler they actually resolved (`cmake/toolchain-cxx-runtime-check.cmake`) and refuse
it at configure time, naming the compiler, the multilib, what was missing, the override variable
and the official tarball URL. The probes carry the board's own `-mcpu`/`-march`, because a
toolchain can ship `libstdc++` for one multilib and not another and the default multilib would
hide that; picolibc is tested *positively* (via `__PICOLIBC__`) because Debian's build also
defines `__NEWLIB__`, so inferring it from newlib's absence would pass it. **A new ARM or RISC-V
port inherits this for free.** RX and Xtensa deliberately skip it -- neither has a same-name
C-only twin on `PATH` to fall through to, so `find_program(... REQUIRED)` is already loud enough.

### Pin-function config (`arch_pinmux_set`)

One-shot init-time pin muxing is an arch/chip-seam entry
(`arch/include/kickos/arch/arch.h`): `int arch_pinmux_set(uint32_t port, uint32_t
pin, uint32_t func)`, reached from userspace as syscall `KOS_SYS_PINMUX_SET` (33),
gated on `AUTH_PINMUX`. `func` is a **chip-opaque** function code (the PORT/PCR/IOCR
encoding), so the ABI `{port, pin, func}` stays vendor-neutral while each backend
owns its own encoding. Returns 0, `-KOS_EINVAL` (out of range), or `-KOS_EBUSY` (a
kernel-owned pin the backend refuses). The **declining fallback**
(`arch/common/arch_pinmux_set_default.cc`) **returns `-KOS_ENOSYS`**, so
a non-empty board pin-map fails LOUD on a chip with no backend rather than silently
mis-muxing.

Backends exist for `mk64f`, `xmc4800`, `rp2040`, `rp2350`, `esp32c6`, `rx72m`,
`stm32f411`, `stm32f103`, `stm32f302`, `sam3x8e`, `imxrt1062`, `esp32`. `nrf51`,
`mps2`, and `virt` keep the declining fallback (no central mux block -- per-peripheral
PSEL, emulated, or virtual). Per-backend caveats: `stm32f103` covers default-mapped
peripherals only (AFIO_MAPR remap out of scope); `imxrt1062` keys `port`=GPIO-bank /
`pin`=bit against a PARTIAL pad table (a hole returns `-KOS_EINVAL`); `esp32c6` packs
BOTH of that family's mux stages into `func` -- the IO_MUX pad word in bits `[15:0]`,
the GPIO-matrix out-sel signal index in `[31:24]` behind an arm bit `[23]` -- so the
kernel-owned-pin refusal covers the matrix and not just the pad (`esp32` still muxes
the pad only); `rx72m` likewise packs both RX stages -- the MPC `PmnPFS` byte
(`PSEL[5:0]` | `ISEL` | `ASEL`) in `[7:0]`, the `PORTm.PMR` bit value in `[8]`, an arm
bit for the `PFS` write in `[9]` -- and does the MPC `PWPR` unlock plus the mandated
"clear `PMR`, write `PSEL`, restore `PMR`" order itself, since a `PFS` write outside
that bracket is silently dropped. `port` there is the DENSE register index 0..0x17
(PORT0..PORTQ; `PORTG` is 0x10), and package pin holes are not modelled -- a pin absent
from the package is accepted and writes a reserved bit. The board
supplies the routing as a `kos_board_pinmap` table the init service walks before the
service list; the init DAG is pinmux -> service list -> app.

`stm32f411` encodes MODER verbatim in `[1:0]`, the AF number in `[7:4]`, and an
output-preset-high arm in `[8]` (`PINMUX_OUT_HIGH`, chip-local at
`arch/arm/chip/stm32f411/chip_stm32f411.cc:389`): the preset writes `BSRR` **before**
`MODER`, so a pin never drives the `ODR` reset level for even one cycle on its way to
its idle level -- which is what an active-low chip select needs. The bit is refused
`-KOS_EINVAL` on any non-output mode. `OSPEEDR` and `PUPDR` stay unreachable through
this seam. `stm32f302` shares the first two fields and has no third, which is the point
of `func` being chip-opaque: a new field is a per-chip encoding change, not an ABI one.

### Peripheral enable (`arch_periph_enable`)

Owning a peripheral window is not enough to reach it: the block must be clocked, and
the bus must stop classifying the access as supervisor-only. Both live in chip-global
registers that no per-thread MPU grant can ever cover, so they are a seam of their own
-- `int arch_periph_enable(uintptr_t base)` (`arch/include/kickos/arch/arch.h:105`),
reached from userspace as syscall `KOS_SYS_PERIPH_ENABLE` (39) via
`kos_periph_enable`. It ungates the clock and drops the bus-side supervisor-protect for
the register block at `base`, and it is **idempotent**, so a driver calls it as its
first act without knowing whether the board already did.

`base` is the register-**BLOCK** base and must match a per-chip table **EXACTLY**; a
backend never range-matches. Both the register and the bit are derived from `base`, so
the ABI carries no register address and no bit number, and a caller cannot name a
shared block's register or bit. The seam returns 0, `-KOS_EINVAL` (no entry for that
base) or `-KOS_ENOSYS`. The **declining fallback returns `-KOS_ENOSYS`**
(`arch/common/arch_periph_enable_default.cc`) rather than 0, so a driver whose block really is
gated fails LOUD on a chip with no backend instead of reading registers that BusFault.

**The gate is possession, not authority.** The syscall refuses `-KOS_EPERM` unless the
caller holds a live region whose `attr` carries `ARCH_MPU_DEV` and whose `base` equals
the requested base exactly -- containment does not count, so a window covering the
block is not a licence to enable a different one
(`kernel/syscall/syscall_mem.cc` (`caller_holds_mmio_block`), dispatch at
`kernel/syscall/syscall.cc` (the `KOS_SYS_PERIPH_ENABLE` arm)). No
capability bit grants this; **holding the window IS the credential**, which is why an
unprivileged driver can enable its own block while root cannot enable one it does not
hold. A **privileged** caller short-circuits the check and is always allowed, but the
only privileged thread in a running image is `idle`, which calls no syscalls -- so every
caller that can reach this seam, root included, goes through the possession check.

**The containment rule decides whether a base gets an entry at all.** A base is tabled
only where the bus gate's granularity is *contained by* the block the window covers.
Where the coarsest available gate would also open kernel-reserved registers, the base
is **refused** instead -- an absent entry is a decision, not an omission, and a porter
adding one must check this before the datasheet. The K64F PIT is the worked case:
`AIPS0` classifies per 4 KiB slot, so clearing SP for a granted channel-2 window
(`0x40037120`) would also expose the chained ch0+ch1 pair `arch_clock_now` runs on --
which `arch_reserved_blocks` protects by address. So the PIT gets no entry and
`pit_clock_init` gates it at boot instead (`arch/arm/chip/mk64f/chip_mk64f.cc:498`).

Backends exist for **four** chips; every other chip keeps the fallback, deliberately
including `esp32c6` (its one-time bus-side APM open is programmed by `arch_init`, not
per block):

| Chip | Block | Clock gate | Bus protect |
|------|-------|------------|-------------|
| `mk64f` | UART0 `0x4006A000` | `SIM_SCGC4` bit 10 | `AIPS0` slot 106, PACR `0x40000064` bit 22 |
| `mk64f` | DSPI0 `0x4002C000` | `SIM_SCGC6` bit 12 | `AIPS0` slot 44, PACR `0x40000044` bit 14 |
| `stm32f411` | SPI1 `0x40013000` | `RCC_APB2ENR` bit 12 | -- none exists for this bus |
| `rx72m` | RIIC0 `0x00088300` | `MSTPCRB` bit 21 | -- none exists for this bus |
| `rx72m` | RIIC1 `0x00088320` | `MSTPCRB` bit 20 | -- none exists for this bus |
| `rx72m` | RIIC2 `0x00088340` | `MSTPCRC` bit 17 | -- none exists for this bus |
| `imxrt1062` | USB1 `0x402E0000` | already done by `usb_clock_init` | `AIPSTZ3` OPACR, the USB1 slot's SP bit |

The `mk64f` PACR register and bit are **computed** from `base` (`slot_of` / `pacr_of` /
`pacr_sp_bit`, `arch/arm/chip/mk64f/regs/aips.h`) rather than tabled, and the clock is
ungated before the protect is dropped. `stm32f411` and `rx72m` are clock-only: no
privilege-classification register exists for those buses in this tree. `imxrt1062` is the
mirror image, protect-only: `usb_clock_init` has already done USB1's clock half, so the
entry clears the USB1 slot's Supervisor-Protect bit in the AIPSTZ bridge's OPACR and
nothing else. The bridge's unit is 16 KiB, so that necessarily opens OTG2 and USBNC too. That is a
legitimate backend shape -- the seam does not promise both halves, only that whatever
the chip has for that block is done.

A porter's in-env check is `periph_enable_unheld` (`selftest`, unguarded so it runs on
every board): it spawns an unprivileged worker holding no DEV window and requires
`-KOS_EPERM`, then self-grants a non-DEV region at the exact base and requires
`-KOS_EPERM` again, pinning the `ARCH_MPU_DEV` filter. The **positive** arm has no
in-env carrier at all: the host sim defines no `arch_periph_enable` (it keeps the declining
fallback), and the ONE window its `arch_mpu_region_encodable` admits
(`arch/sim/sim.cc`) is the fake register block the write seam below needs -- every other
address, and every other shape of that one, still fails closed. That arm is silicon-only,
and `c6blink` and `rxdrv` carry probes for it.

### Which core am I (`arch_cpu_id`)

**A port does nothing here today, and that is the point.** `KICKOS_NUM_CORES` defaults to 1 on
every board, and at 1 `arch_cpu_id()` is a MACRO expanding to the literal `0u`
(`arch/include/kickos/arch/arch.h`), so no symbol exists, no call is emitted, and a single-core
image is byte-identical to one with no such seam at all.

**It is a preprocessor fold and not an inline on purpose.** An `inline` returning 0 would rely on
the optimiser, and this tree has already measured GCC out-lining an `always_inline` candidate at
`-Os` (`system/include/kickos/sys/atomic.h` records it). The `cpu_id_fold` gate pins the property,
so softening the macro fails the build rather than silently costing the byte-identity.

**A multi-core port raises `KICKOS_NUM_CORES` in its defconfig and DEFINES the function.** There is
deliberately no `arch/common/` fallback member on that arm, following `arch_reserved_blocks`: a
port that raises the knob and ships no definition gets a LINK ERROR, never a kernel that quietly
believes every core is core 0. The knob is an ordinary Kconfig int, so it reaches C through the
generated `kickos/board_config.h` like every other provisioning integer and needs no CMake edit.

Splitting per-core kernel state is NOT part of this seam and is not done yet;
`../design-m7-state-inventory.md` classifies what would have to move. Where an arch already
needs a per-core block it declares its OWN, on the same two-arm shape: armv8a's
`struct armv8a_percpu` (`arch/arm64/armv8a/include/kickos/arch/percpu.h`) is reached by an
accessor that folds to the array's first element at one core and reads TPIDR_EL1 above one.
That block is the arch's and not this seam's, the register it is reached through being A64's.

### The cross-core doorbell (`arch_ipi_send`, `arch_ipi_wait`)

**A port does nothing here today either, and for the same reason:** at
`KICKOS_NUM_CORES == 1` both are EMPTY MACROS that consume their argument, so no symbol exists
and no image carries the seam. The argument is a bitmask of core indices; 0 names nobody.

**Two calls and not one, from the first line.** A doorbell that can only be fire-and-forget
cannot express a rendezvous, and a rendezvous is what a TLB shootdown is on an architecture with
no broadcast invalidate. So the send is separate from the wait, and an initiator pokes every
target once and then waits once.

**A broadcast architecture's `arch_aspace_map`/`arch_aspace_unmap` do NOT go through this pair.**
A64 invalidates and waits with two instructions and no far-side code at all, so the maintenance
inside those two stays a local sequence; routing it through a doorbell would invent a deadlock
that architecture cannot have. Where a port DOES need the far side to execute, its handler takes
no kernel lock and the lock's own acquire loop services a pending doorbell, or an initiator
holding the lock waits on a core spinning to take it.

### Data-cache maintenance (`arch_dcache_flush`, `arch_dcache_invalidate`)

Make this core's writes over a range visible to an observer that does not snoop, and such an
observer's writes visible to this core. A DMA engine on a non-snooping bus is the ordinary case;
a companion core across a window that is not coherent is the other.

**Concepts, so no line size crosses the seam.** The backend reads its own line size and rounds
the range out to it, which is what keeps a caller from carrying a figure that has to stay in step:
armv8a reads `CTR_EL0` rather than trusting the A53's 64 bytes, because a smaller line anywhere in
the hierarchy would leave lines untouched at 64.

**The invalidate may not discard what sits beside the buffer.** A range whose ends fall inside a
line shares those lines with its neighbours, so the backend CLEANS as it invalidates
(`dc civac` on A64, not `dc ivac`). That costs nothing a caller can measure and removes an
alignment rule no caller would honour.

`addr` is a KERNEL-usable pointer, which is what `arch_aspace_acquire` answers. A range named by a
user virtual address is not one operation on a backend whose physical space is discontiguous, so
the page splitting belongs above this seam.

**There is no `arch/common/` fallback and nothing in the tree calls it yet.** Who calls it is open
(`../design-m6-mmu.md` section 7), so a port defines it when its first caller arrives and fails the
LINK until then: a no-op default would report maintenance it never did, which is the one answer
worse than a link error.

### The map editor's acquire pair (`arch_aspace_acquire`, `arch_aspace_release`)

Reach the frame backing one page of a space through a kernel-usable pointer, and release it. A
backend that maps all of physical memory in its high half inlines the acquire to an addition; one
whose physical space is wider than its virtual, or that reaches frames through a fixed set of
windows, does real work and can RUN OUT.

**`ARCH_ASPACE_ACQUIRE_MIN` (`arch/include/kickos/arch/arch.h`) is how many are holdable at once
per core, and a windowed backend sizes its pool for that figure and `static_assert`s against it.**
The figure counts OUTSTANDING CALLS and not distinct pages: two acquires of one page are two holds
unless the backend counts references. `arch/riscv/rv64imac/aspace_rv64imac.cc` is the worked
windowed backend: `ACQUIRE_CAPACITY`, a per-core slot table, and the `static_assert` against this
figure. `arch/arm64/armv8a/aspace_armv8a.cc` carries the assert in the shape an UNBOUNDED port
fills in, its acquire being an addition and its release a no-op.

**It is measured from the tree rather than chosen.** The deepest holder is the page-split access
scenario behind `KOS_ASPACE_OP_SPLIT_ACCESS` (`kernel/syscall/syscall_aspace.cc`): four pages held
across two spaces while `ep_copy` acquires one end in each, six at the peak. The endpoint copy
alone holds two, which is the floor a caller doing nothing else meets. **A caller of the seam owes
the other half:** a walk over many pages releases each before taking the next, as
`KOS_ASPACE_OP_SPAN` does, or it puts the whole tree over the figure for a reason that is the
walk's and not the seam's.

### Naming the frame behind a page (`arch_aspace_frame_at`)

`arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)` answers the
granule-aligned PHYSICAL address the page holding `va` is mapped onto, and 0 where that page is not
mapped. It spends no acquire hold and reads no frame's contents; `va` need not be granule-aligned
and the byte offset inside the granule is not carried, an answer being a frame and not a pointer to
a byte.

**It exists because naming a frame by ARITHMETIC on acquire pointers is only correct where acquire
is an addition, and it fails silently where it is not.** Subtracting two acquired pointers and
dividing by the granule is a stable identity on an offset-map backend; on one windowing a handful
of slots the same small number comes back for every frame in the system, so code comparing frame
identity reports "the same frame" for frames that differ and SUCCEEDS. That is the shape to reach
for whenever a caller wants to know WHICH frame rather than to read one
(`../design-m6-mmu.md` F8).

### Privileged register write (`arch_periph_reg_write`)

The other member of the same seam family, and the one for a bus that classifies the
WRITE side **per register** rather than per block. There the window is readable and
mostly writable, but a few registers discard an unprivileged store **silently** -- no
fault, read-back unchanged. `int arch_periph_reg_write(uintptr_t base, uintptr_t offset,
uint32_t value)` (`arch/include/kickos/arch/arch.h`) performs that one store privileged,
reached from userspace as syscall `KOS_SYS_PERIPH_REG_WRITE` (42) via
`kos_periph_reg_write`.

**The `U` / `PV` notation this page uses is the XMC reference manual's, and it is ADDITIVE.**
XMC4700/XMC4800 RM V1.3 front matter, Table 2: `U` = unprivileged mode permitted, `PV` =
privileged mode permitted, and a cell lists every mode that is permitted. So `U,PV` on a
register's `Write` column means an unprivileged store lands, and `Write = PV` with no `U` means
it does not. This is reference-manual knowledge citable from nothing in this tree, which is why
it is stated here: a porter cannot read the seam table below, or `boards.md`'s USIC records,
without it. On the XMC USIC channel, RM Table 18-20 marks **exactly three** registers
`Write = PV`: `FDR`, `BRG` and `CCR`. Every other channel register the drivers touch --
`KSCFG`, `SCTR`, `TCSR`, `PCR`, `PSCR`, `DX0CR`, `INPR`, `CCFG`, `TBCTR`, `TRBSR` and the `INx`
push aperture -- is `U,PV`, so an in-window holder reaches it with a plain store. `INPR` in
particular is `U,PV`: an older transcription listed it as a fourth `PV` register and that was a
slip, never measured.

**The two seams differ in what the ABI carries, and that follows from the two reasons a register
lands in the kernel at all.** For `arch_periph_enable` (class 1) the register is OUTSIDE any
grantable window -- SCU, SIM, RCC, AIPS, APM carry authority over peripherals the caller was
never granted -- so the caller names only the BLOCK and the register plus bit are DERIVED from
it: naming them would be naming something the caller has no claim to. For
`arch_periph_reg_write` (class 2) the register is INSIDE the caller's own window and it already
reads that address freely, so withholding the name buys nothing while making the seam a
per-register function on every chip; the caller therefore names `(base, offset)`. What replaces
the derivation as the bound is the **per-chip allowlist of exact register addresses**, so the
reachable command space is not "the block I hold" but "the specific registers this chip's porter
enumerated inside the block I hold" -- strictly narrower than what a class-1 entry already
grants. That is the rule a new chip's entry is judged against, and the premise it rests on
("already reads that address freely") is only true because the possession predicate CHECKS it;
see the containment paragraph below.

Two backends exist in-tree. `xmc4800` is the real one. The **sim** is the second, and its
purpose is a CI gate (`arch/sim/sim.cc`: `SIM_PVREG_BASES`, `SIM_PVREG_WINDOW`,
`SIM_PRIV_WRITE_REGS`). It models a write-PV-only block over real host pages: a **64 KiB**
window (`0x10000`) taken from the FIRST of FIVE candidate bases that `MAP_FIXED_NOREPLACE`
accepts -- `0x40000000` (1 GiB), `0x100000000` (4 GiB), `0x400000000` (16 GiB),
`0x10000000000` (1 TiB), `0x100000000000` (16 TiB) -- and PUBLISHED at init, so nothing
assumes a fixed host address. All five are valid addresses, decades apart, so no host
mapping or ASLR layout takes the whole set; `selftest`'s `periph_reg_write_mask` walks the
same list in the same order, and a drift between the two shows up as every candidate
refused, never as a pass. The allowlist has NO base column -- there is only ever one block
-- and one of its two entries sits deliberately BEYOND the grantable window, reproducing
the containment hazard on the host. 64 KiB is the window because every host page size in
practice (4, 16 and 64 KiB) divides it, so "`mprotect` describes this exactly" holds by
construction rather than by the host happening to use 4 KiB pages.

Porting a chip means writing the table; the mechanism is already gated there, and six
mutations each came out red on a distinct check: mask widened, silent trim, refuse-and-act,
containment dropped, alignment dropped, wrap dropped. What the host still cannot establish
is three things, and each stays a per-chip MEASUREMENT: the bus PV classification (that an
unprivileged store is silently DISCARDED -- `pvprobe` only), that a tabled block is
CLOCKED, and any chip's actual mask column.

**The gate is a STRICTER possession predicate** than `arch_periph_enable`'s.
`caller_holds_mmio_reg(base, offset)` (`kernel/syscall/syscall_mem.cc`) first matches a
live `ARCH_MPU_DEV | ARCH_MPU_R | ARCH_MPU_W` region whose base equals `base` EXACTLY,
then requires that region to **CONTAIN** `[base + offset, +4)`. No authority bit;
privileged callers short-circuit. The containment half is not a refinement, it is what
makes the possession check bound anything: matching the base alone lets a 32-byte window
at a block's start reach every register the chip table names anywhere in that block
(MEASURED before the fix -- a holder of 32 bytes at `0x40000000` passed the gate at
offsets `0x40`, `0x1000` and `0x0FFFFFFF`, and at an offset that wrapped 2^32). On the
XMC4800 that was a live confused deputy: a 32-byte window at `0x40030200` covers
`FDR`/`BRG` but not `CCR` at `+0x040`, so a holder that could neither read nor write
`CCR` could have the kernel write it privileged.

**Malformed requests are refused ahead of possession.** The offset must be 4-ALIGNED and
`base + offset` must not wrap; both answer `-KOS_EINVAL` whatever the caller holds,
because they are malformed rather than unauthorised. `arch_periph_enable`'s helper and
semantics are untouched by this -- it takes a block base and has no offset to bound.

What is deliberately NOT true is that a holder gains the block: the chip carries an
**ALLOWLIST of exact `(base, offset)` pairs** and refuses `-KOS_EINVAL` for anything
else. A per-block entry would hand back exactly what the bus classification withholds,
so the granularity is the register, never a range.

**"One register" is not "only the authority the driver needs", so each entry ALSO carries a
per-entry VALUE MASK.** `(value & ~mask) != 0` answers `-KOS_EINVAL` **before the store**
(`arch/arm/chip/xmc4800/chip_xmc4800.cc`, `arch_periph_reg_write` against
`PRIV_WRITE_REGS`): REFUSED WHOLE, never trimmed and never read-modify-written, because a
silently dropped configuration bit is the failure class the read-back exists to catch. On
`xmc4800`, `CCR` grants `MODE[3:0]`, `RIEN` and `AIEN` -- 6 bits of 32, with `RIEN`/`AIEN`
deliberate because `xmcssc` arms them last -- and withholds `TBIEN`, `HPCEN`, `PM`,
`RSIEN`, `DLIEN`, `TSIEN` and `BRGIEN`. `FDR` grants `STEP` and `DM`. `BRG` grants every
writable field, where the mask withholds only read-only and reserved bits and so buys
little; it is there so no entry carries a blanket word. A `static_assert` pins each
composed grant against the word it must equal, so a field-mask edit in the chip's register
header cannot widen a grant silently.

Silicon witness (`.session/m456-silicon/b2-pvprobe.log`, `xmc4800-relax` at commit
`270b6fa`):

```
[pvprobe] mask refusal: CCR|TBIEN rc=-22 (want -22), pre=0xc001 post=0xc001 unchanged
```

`pre == post` is the load-bearing half: the off-mask bit was not trimmed away and stored
without it, the whole word was refused.

**A tabled block must also be CLOCKED.** The contract REQUIRES it -- quoted from
`arch/include/kickos/arch/arch.h`: *"An entry's block MUST be CLOCKED whenever the syscall
can reach it"* -- though no backend CHECKS it. A store into a clock-gated block faults
INSIDE the privileged store, in the kernel's own frame, and that reaches
`kfault_terminate`: whole-system death from one syscall, i.e. an unprivileged caller's
one-syscall system-kill primitive. Not live today only because `kickos_xmc_usic_init()`
runs unconditionally from `arch_init` so USIC0 is never gated; a `U1C0`/`U2C0` entry
(gated at reset behind `CGATCLR1`) would arm it. A porter adding an entry for a block that
can be gated must either ungate it at `arch_init` or have the backend check.

Returns 0, `-KOS_EPERM` (the possession gate above, decided in the syscall layer),
`-KOS_EINVAL` (misaligned, wrapping, or not on the allowlist), or `-KOS_ENOSYS` (no
backend on this chip). `-KOS_ENOSYS`, not 0: a caller reaching for this seam has a
register the bus refuses it, so a success answer would report a store that never
happened. Every failure is a checked errno; nothing here faults.

**The default is NOT a weak symbol** -- and neither is any other seam fallback in the tree.
It is a one-symbol archive member, `arch/common/arch_periph_reg_write_default.cc`, compiled
into every `kickos_arch_*` library. The rule is stated canonically in `arch/CMakeLists.txt`
(lines 11-71), and it is LINKER behaviour that every Unix-like linker and MSVC `.lib` share,
not a language guarantee. A second symbol in that TU would drag the member in
unconditionally and collide on every chip with a backend.

**The load-bearing invariant is on the BACKEND side: a chip's definition must live in an
always-ANCHORED archive member** -- one the link pulls for some other reason
(`chip_<chip>.cc`, force-loaded via `-u g_isr_vector` -> `startup.S` -> `Reset_Handler`;
`arch/arm/chip/xmc4800/usic_uart.cc`, pulled for `arch_console_tx_backend`). Scan order
(`kickos_arch_*` after `kickos_chip_*` in the rescan group) is only a BACKSTOP: MEASURED
with the group reversed, the link still resolves correctly from the anchored chip member.
**A chip that puts its definition in a dedicated TU nothing else references gets NEITHER
protection -- the fallback resolves the reference first and the board SILENTLY DECLINES at
runtime.** Proved by mutation: the group reversed plus `arch_idle_wait` moved into an
unreferenced TU linked with ZERO diagnostics, `objdump` showing the fallback's `wfi`
instead of the chip's `nop`.

`tests/static/check_seam_defaults.sh` (ctest `seam_defaults`, run on EVERY board) gates it in four
legs, all four mutation-proved: the one-symbol rule, plus no fallback in `kickos_kernel`; for
a seam a backend defines, the fallback member ABSENT from the link map with the backend's
member present -- the anchoring leg; for a seam no backend defines, the fallback member
present in the map's inclusion list for that exact symbol, and a board that resolves no seam
from a fallback at all fails too, so the gate cannot go vacuous; and zero weak symbols
outside `tests/static/weak_allowlist.txt`.

| Chip | Block | Register | Per-entry value mask | Why it needs the seam |
|------|-------|----------|----------------------|-----------------------|
| `xmc4800` | USIC0 CH1 `0x40030200` | `FDR` `0x010` | `0x0000C3FF` (`STEP[9:0]`, `DM[15:14]`) | RM Table 18-20 marks it `Write = PV` with no `U`; the divider is the whole setting the driver needs |
| `xmc4800` | USIC0 CH1 `0x40030200` | `BRG` `0x014` | `0xF3FF7FDB` (every writable field) | same table row; only read-only and reserved bits are withheld |
| `xmc4800` | USIC0 CH1 `0x40030200` | `CCR` `0x040` | `0x0000C00F` (`MODE[3:0]`, `RIEN`, `AIEN`) | same table row; the mask is what keeps the channel's other interrupt enables out of the grant |

`xmc4800`'s U0C0 (`0x40030000`) has ONE entry and only one, its `CCR` at mask `0x0000E00F`
(`MODE[3:0]`, `TBIEN`, `RIEN`, `AIEN`), which is the CH1 mask plus `TBIEN`
(`chip_xmc4800.cc`, `CCR_CONSOLE_GRANT`, `static_assert`ed against that literal). The extra
bit is what lets a userspace driver arm the console channel's transmit-buffer interrupt. Its
baud and enable stay the kernel's (`usic_uart.cc`), so `FDR`, `BRG` and every other U0C0
register have no entry, and an absent entry is a refusal rather than an omission, the same
rule as the K64F PIT above.

A porter's in-env check is `periph_reg_write_unheld` (`selftest`, unguarded), THREE arms:

1. A caller holding no DEV window requires `-KOS_EPERM`.
2. A holder of a real DEV window at a base the chip does not table requires a REFUSAL
   that is neither `-KOS_EPERM` (the gate passed) nor 0 (nothing may accept an untabled
   address) -- so on every board with no backend it pins the default's `-KOS_ENOSYS` at
   runtime. It finds the window **by attempting the spawn over candidate bases**, NOT
   with `kos_grant_probe`: the probe syscall needs `KICKOS_ENABLE_SELFTEST`, and using it
   would silently drop this arm on a production-ABI build.
3. The OFFSET BOUND: the same holder asks for an offset that is 4-aligned and that the
   chip table COULD name, but that lies outside the window it holds, and requires
   `-KOS_EPERM`. Arm 3 is the one the earlier shape could not express -- `PRW_OFFSET` was
   `0x2`, deliberately unaligned and unnameable, so it never tested containment. It is now
   `0x4` and the unnameability guarantee moved from the offset to the BASE.

All three verdicts collapse into one `volatile unsigned char` bitmask, so the added arms
cost ZERO net static RAM -- which is not cosmetic: see the `bluepill-c8-st` zero-slack note
under *Program-memory floors*, where any growth in a shared test breaks a link no gate
covers.

It reports PARTIAL where no FREE DEV window can be minted -- on the host sim, because the
one window the sim admits is the fake register block that `periph_reg_write_mask` holds.
A PARTIAL is `tap::partial`, so the arm reports `ok N - <name> # PARTIAL <reason>` and the
harness prints a `# partial: N` summary. The gate permits partials BY NAME, per board
(`EXPECT_PARTIALS`, threaded from `user/apps/common/selftest/CMakeLists.txt` and checked in
`tests/integration/check_tap_stream.sh`), so a DEV-window-encodability regression on ARM cannot make
every board take the PARTIAL early return and lose the seam's refusal contract fleet-wide
behind a green CI: the boards where it is not permitted go red. Measured: the held arms DO
run on `qemu`/`m3`/`m7`/`m33`/`riscv-mpu`/`microbit`; only `sim` degrades, and there
`periph_reg_write_mask` covers the same ground with the window it holds.

That second case, `periph_reg_write_mask` (sim only), is what puts the allowlist match, the
mask edge, refuse-not-trim and the containment refusal under a gate: it drives the SHARED
`KOS_SYS_PERIPH_REG_WRITE` dispatch arm and the same `caller_holds_mmio_reg`, with only the
table and the store target sim-side. Its containment arm requires `-KOS_EPERM` and not
`-KOS_EINVAL`, which is what proves the refusal came from the kernel's span check rather
than from the chip layer. It does NOT skip when it finds no window: a refusal there is a
regression in the grant path or a drift between its base list and the sim's, so it FAILS.

What no host gate answers is whether the bus DISCARDS an unprivileged direct store, which
is the whole premise of the seam. `user/apps/xmc4800-relax/pvprobe` is the discriminating
probe, running a direct store and a seam store to the same register from the same thread.
**That half ships gate-free** -- no QEMU model classifies a register's write side -- so the
XMC captures are its only evidence.

**That silicon arm is TAKEN** (`xmc4800-relax`, 2026-07-30, one run, one thread, one held
window): the three seam writes read back `exact`, while the same thread's direct stores to
the same `FDR`/`BRG`/`CCR` reported `DROPPED (post == pre)`. Both controls came out right
in the same capture -- `SCTR` (a `U,PV` register in the window) `LANDED` under a direct
unprivileged store, and an ungranted SCU poke faulted (`CFSR=0x82`,
`MMFAR=0x50004648`). Both refusals were taken on hardware too: off-allowlist `-22`
(`-KOS_EINVAL`) and unheld window `-1` (`-KOS_EPERM`). So the seam is not a no-op and not
a widening of the window.

---

## Minimum hardware requirement

KickOS has **two** floors and they are far apart. Conflating them is the trap this
section exists to prevent. Both are `-Os` figures, and the second one's SRAM half is a
**provisioning** statement, not a part size:

- the **run** floor -- kernel, console and a small app with a couple of threads:
  **32 KiB program memory + 16 KiB SRAM, at `-Os`**;
- the **suite** floor -- additionally host the fleet-uniform validation image
  (`selftest`, the TAP suite every board is witnessed with): **64 KiB program memory at
  `-Os`**, plus **16 KiB SRAM with a static-allocation profile** -- heap 0 and 1 KiB user
  stacks -- which buys a clean run of 63 `ok` / 0 `not ok`, 5 of those 63 being named
  skips. A **zero-skip** run needs a 32 KiB part.

`f302nucleo` (STM32F302R8, 64 KiB flash / 16 KiB SRAM) is the worked case for both, and
it is what makes the SRAM half a provisioning statement. At the chip defaults it refuses
every spawn. At the `f302nucleo-st` provisioning the same 16 KiB part runs the suite on
silicon at **63 ok / 0 not ok / 5 skipped**, plan `1..63` (measured at `124b68c`,
`.session/m456-silicon/b5-nuc-selftest-after.log`). That provisioning is one file, the `st` variant's
defconfig (`../../boards/f302nucleo/configs/st/defconfig`): self-test on,
`KICKOS_USER_HEAP_SIZE 0`, `KICKOS_MAX_SEMAPHORES 6`, `KICKOS_MAX_THREADS 3`, and the
stack sizes (`KICKOS_USER_STACK_SIZE 1024`, `KICKOS_ROOT_STACK_SIZE 1536`,
`KICKOS_IDLE_STACK_SIZE 512`). It used to be split between the preset and the board's
defconfig, and a porter had to read both. So "KickOS runs here", "KickOS is validated here with named skips" and "KickOS is
validated here with none" are three different claims about one board.

A **named** reduced suite is a supported posture, not a degraded one. `microbit` (nRF51,
**32 KiB** SRAM -- the board deliberately takes the 32 KiB nRF51822 variant rather than the
BBC micro:bit v1's 16, `arch/arm/chip/nrf51/nrf51.ld`) declares the cases it cannot host as
an expected-skip list checked by name in CI (`EXPECT_SKIPS` in
`../../user/apps/common/selftest/CMakeLists.txt`, rationale in `boards.md` under
*Per-board caveats*): a skip not on that list fails the gate. That list is down to ONE name,
`uart_service` on the `p2` image, and even that one is a PIN rather than an arena outcome --
`KICKOS_SELFTEST_NO_UART_SERVICE` holds it out because its 1 KiB ring and the arena probes
want the same region. The `p1` image expects no skip at all.
`f302nucleo` has no gate of any kind, so its 5 skips are enumerated below instead.

Every figure below names its board, its app and its optimisation level. Flash figures are
reused from `../archive/M4.5_footprint_meas.md` section 3; RAM figures are read out of ELFs
linked at this branch tip.

### The optimisation level is part of every floor

Every byte count in this section is `-Os`. The fleet gets that from `CMAKE_BUILD_TYPE`
`MinSizeRel` in `cmake/presets/arm.json:10`, which is **in-tree only**: an out-of-tree
consumer reaching KickOS through `find_package(KickOS)` picks its own `CMAKE_BUILD_TYPE`
and never sees the presets. Both the empty default and `Debug` are `-O0`.

`f302nucleo` `hello`, same tree, same tip, flash = `.text` + `.data` + `.init_array` +
`.ARM.exidx`:

| Build type | flash |
| --- | --- |
| `MinSizeRel` (`-Os -DNDEBUG`) | 19,088 |
| `Debug` (`-O0 -g`) | 36,852 |

A floor quoted bare is therefore off by 1.9x: "fits in 32 KiB" is true at `-Os` and false
at `-O0`. Quote the optimisation level with the number. The installed package does
**not** override a consumer's build type -- that is the consumer's decision -- so the
condition travels as documentation and nothing else.

**`Debug` is not a supported configuration on a 64 KiB part.** The suite floor is an
`-Os` floor and the `-O0` image does not fit. Measured at `c5d9b0d`, and in both cases
`selftest` is the ONLY image that fails to link -- every other app on the board fits even
at `-O0`:

```
ld: user/apps/common/selftest/selftest section `.text' will not fit in region `FLASH'
ld: region `FLASH' overflowed by 7920 bytes      # f302nucleo-st
ld: region `FLASH' overflowed by 7692 bytes      # bluepill-c8-st
```

The same `f302nucleo-st` `selftest` at `-Os` uses 54,944 B of its 64 KiB and keeps
**10,592 B (10.34 KiB) free**. **All five of those figures were measured at `c5d9b0d` and
are NOT current** -- the tree is at `124b68c` and two further commits have grown the suite,
so treat them as the shape of the result, never as today's numbers. They move with every
milestone that grows the suite: they read 5,120 / 4,884 / 12.9 KiB one milestone earlier,
before two syscalls and three selftest arms landed. Re-measure rather than trusting any
quoted figure, this one included. This costs no
debuggability: `MinSizeRel` carries `-g`, so the symbols are all there and only `-O0` is
unavailable. No gate builds these boards in `Debug`, deliberately -- the link failure
already names the overflow in bytes.

**The tightest link in the fleet is the POOL term, not the boot term, and it is
`f302nucleo-st` at 1,248 B.** Configure prints only the needed side:

```
-- KickOS: boot stacks idle=512->512/16 root=1536->1536/16 (mpu granule 0 pow2=1)
-- KickOS: arena model idle=512/16 root=1536/16 pool=2x1024/16
```

Worst-image margin against `KICKOS_POOL_ARENA_ASSERT`, tightest first (re-measured
2026-08-23): `f302nucleo-st` **1,248 B**, `f302nucleo` 1,280 B, `bluepill-c8` 1,344 B,
`bluepill-c8-st` 1,632 B, `microbit` 6,144 B, `frdmk64f{,-st} +MPU` 24,576 B. Nothing in the
fleet is at or below zero on either assert, and the tightest BOOT margin is `f302nucleo`'s
3,328 B.

**A margin is not a spare-slot count.** Dividing it by the stride overestimates, because each
added slot ALSO raises `__kickos_ram_start`: on `f302nucleo` the 1,280 B margin against a
1,024 B stride suggests a third thread and there is none, the per-slot `.bss` run-up
measuring +416 B there. Only where a large root alignment absorbs the whole run-up do the two
agree. On both `frdmk64f` variants every image has the same `__kickos_ram_start`, the
SYSMPU putting app `.bss` in the fixed `.appdata` window, so "worst image" is a tie rather
than a pick; on the four flat ARM boards it is always `selftest_p3`. **`bluepill-c8-st` used to hold this title
at exactly zero boot slack** (2,560 needed against 2,560 available); surrendering its 8 KiB
heap carve in M4.9.3 took it to 8,192 B and moved the fleet's fragile edge to the 16 KiB part.

Both asserts are `<=`, so an exact fit passes and the next byte of static RAM in a SHARED
test breaks the link. There is no available-vs-needed pair anywhere in the build output and
neither `ASSERT` message carries numbers, so the failure is loud but mute: to get the margin
you re-link and subtract the symbols yourself. `bluepill-c8` in particular has **no ctest
gate** (`ctest -N` in its build dir lists only the universal `kickos_build` fixture) and **no
physical unit**, so neither CI nor a bench run can catch it -- only a full-fleet build.

### The program-memory floor

`hello` -- kernel, arch, chip, console, UART driver, libgcc, minimal app -- across the
fourteen real boards (`../archive/M4.5_footprint_meas.md` s.3):

| | Board | `hello` flash, `-Os` |
| --- | --- | --- |
| minimum | `microbit` (nRF51, armv6m) | 18,708 |
| ARM maximum | `teensy41` (imxrt1062, M7) | 20,692 |
| RX | `rx72m` | 21,732 |
| Xtensa | `esp32-wroom` | 22,996 |

So the kernel plus a small app costs **18.7 to 23.0 KiB at `-Os`**, of which the kernel core is
a flat ~12.5 KiB across boards (`../archive/M4.5_footprint_meas.md` s.4). A 32 KiB part holds
that with room for an app, but **no chip in the tree declares one**: the smallest FLASH
regions are 64K, on `stm32f302` (`arch/arm/chip/stm32f302/stm32f302.ld:13`) and
`stm32f103` (`arch/arm/chip/stm32f103/stm32f103.ld:17`). The 32 KiB run floor is derived
from the measured `hello` sizes, not witnessed on a part.

The two non-ARM ISAs cost 1.0 to 2.3 KiB more than the ARM maximum. `esp32c6-wroom`
looks far larger -- 49,112 bytes in `../archive/M4.5_footprint_meas.md` s.3 -- but that is
**region accounting, not code**: its linker script carves no flash region, so code and
data share the 512 KiB RAM and the figure is whole-RAM occupancy. Measured at tip, its
`hello` code is ordinary: `.text` 22,840 + `.data` 48 + `.init_array` 8 = 22,896, and
the rest of the 49,072-byte total is `.bss` 9,792 plus a 16,384-byte `.userheap`
(`KICKOS_USER_HEAP_SIZE` at this chip's declared default). Per-ISA code density accounts for the
remaining ARM/non-ARM gap; that attribution is **inferred, not measured**.

The suite spans 46,932 to 57,568 bytes at `-Os` across the fleet and both
`KICKOS_ENABLE_SELFTEST` settings (`../archive/M4.5_footprint_meas.md` s.3), so it needs a
**64 KiB** part; the tightest shipped configuration keeps 13,820 bytes free.

### The SRAM model

SRAM divides into four spans and a porter must budget all four. Every boundary is a
linker-script symbol, so the split is readable out of any linked ELF:

    SRAM = static + .userheap + arena + _kernel_stack_size

- **static** -- `.data` plus real `.bss`, dominated by `kickos::detail::g_instance`,
  the kernel singleton holding every object pool
  (`kernel/include/kickos/instance.h:65-99`), and -- wherever `KICKOS_KERNEL_STACKS` is 1
  -- by the per-thread kernel-stack array beside it, `KICKOS_THREAD_SLOTS` blocks of
  `KICKOS_KERNEL_STACK_SIZE` (`kernel/thread/thread.cc`, `KStackBlock`). That array is a
  FIRST-ORDER term on a small part, and it sits BELOW the arena, so raising the slot count
  shrinks the arena while raising the demand on it. `microbit` provisions four threads and
  not eight for exactly that reason -- `KICKOS_THREAD_SLOTS` is `KICKOS_MAX_THREADS + 1`, so
  that is five blocks of the armv6m 896 and not nine -- and its defconfig
  carries the derivation (`../../boards/microbit/configs/base/defconfig`).
- **`.userheap`** -- `KICKOS_USER_HEAP_SIZE`, carved *below* `__kickos_ram_start` by
  `stm32f302.ld`, so it trades against the arena 1:1. A knob like the stacks: its
  per-chip default is declared in `Kconfig` and a variant states its own, which reaches
  the linker script as a `-D` (`arch/CMakeLists.txt`).
- **arena** -- `[__kickos_ram_start, __kickos_ram_end)`
  (`arch/arm/chip/stm32f302/stm32f302.ld:110-111`), the MPU-governed user-RAM pool.
  **Every thread stack comes from here**, as does every `kos_ram_alloc`.
- **`_kernel_stack_size`** -- the MSP/boot stack, taken off the top
  (`arch/arm/chip/stm32f302/stm32f302.ld:18`, 2K there; `__kickos_ram_end = _estack -
  _kernel_stack_size`).

`arm-none-eabi-size` reports `.userheap` inside its `bss` column (the section is
`ALLOC`/`NOBITS`), so that column is **not** the static footprint.

The arena must hold, allocated in this order (`kernel/init/kmain.cc:222`, `:224`):

    arena >= align(KICKOS_IDLE_STACK_SIZE)
           + align(KICKOS_ROOT_STACK_SIZE)
           + N * align(KICKOS_USER_STACK_SIZE)
           + whatever the app kos_ram_alloc's

for N **concurrently live** spawned threads taking a kernel-default stack
(`kernel/syscall/syscall_thread.cc:354`). `align()` is `arch_ram_region_size` /
`arch_ram_region_align` (`arch/include/kickos/arch/arch.h`). The **MPU geometry** has three
modes, keyed on `arch_mpu_min_region()` and `arch_mpu_region_pow2()`:

| `min` | `pow2()` | size | geometry align | backends |
|---|---|---|---|---|
| 0 | n/a | 16-byte granular | 16 | the three no-MPU chips (`nrf51/chip_nrf51.cc:110`, `stm32f103/chip_stm32f103.cc:383`, `stm32f302/chip_stm32f302.cc:321`) and LX6 |
| != 0 | 1 | power of two, >= `min` | the size | ARM PMSAv7 (32), RISC-V PMP NAPOT (8) |
| != 0 | 0 | multiple of `min` | `min` | ARM PMSAv8 (32), NXP SYSMPU (32), RX (16) |

**`KICKOS_TLS` is a FOURTH leg and it overrides all three.** With the knob on, the alignment
is `pow2_ceil(want)` wherever that exceeds the geometry above, whatever the descriptor asks
for: the ARM thread pointer is SP masked down to the thread's own block, so the blocks must
be STRIDED by a power of two or a mask lands in a neighbour's. So the row-1 figures are the
geometry and not the answer -- `microbit` is a no-MPU chip on that row and still strides by
**2048**, its 2048-byte user and root stacks being their own `pow2_ceil`
(`../../boards/microbit/configs/base/defconfig`). `f302nucleo` and `bluepill-c8` are the
boards that set `KICKOS_TLS=n` rather than pay the rounding. `cmake/boot_arena.cmake`'s
`kickos_region_align()` mirrors this leg, reading the knob out of the resolved
configuration, so the link-time assert models the geometry the allocator actually produces.

The pow2 mode and the TLS leg are what pay a natural-alignment run-up, and there it can cost
as much again as the request, so compute it rather than assuming the sum of the sizes. A
base+limit backend with `KICKOS_TLS` off pays at most one granule per block.

On a v8-M chip the mode is **posture-dependent**: `arch_arm_pmsav8.cc` (which defines
`pow2() == 0`) enters the link only at `KICKOS_HAVE_MPU=1`, so a non-enforcement build
of the same chip shapes regions with the v7-M pow2 fallback.

The arena is a pure bump allocator (`arch/common/arch_ram_common.cc:42`) and **never
takes a block back**. An exited thread's default stack returns to a single-size-class
free list on the thread pool instead (`kernel/include/kickos/thread.h:203-207`), which
is why N is a peak-concurrency figure while every `kos_ram_alloc` is permanent.

**The idle + root terms AND the thread-stack pool are checked at link time on every
board.** `KICKOS_BOOT_ARENA_ASSERT` (`arch/common/boot_arena.ld.h`, fed by
`cmake/boot_arena.cmake`) replays those two allocations including alignment padding.
`KICKOS_POOL_ARENA_ASSERT` (same header) carries the same replay one step further, over
`KICKOS_MAX_THREADS` blocks of `align(KICKOS_USER_STACK_SIZE)`. Neither may be opted out
of by omission: a linker script carrying no invocation is a configure `FATAL_ERROR`, and
that check reads the script this board actually links, so a BOARD-LOCAL override
(`boards/qemu-m33/mps2.ld` is the one in tree) is covered like any chip script.
Configure prints the demand terms the asserts replay, and only the demand side:

```
-- KickOS: arena model idle=<size>/<align> root=<size>/<align> pool=<count>x<size>/<align>
```

Without that assert, an image whose arena cannot back every advertised slot's stack links
clean and fails per-spawn at runtime with `-KOS_ENOMEM` -- the same code as a full slot
table, so the shortfall is indistinguishable from a legitimate limit. That asymmetry is
why the two floors are indistinguishable from the build system and obvious on silicon, and
why the honest place to answer the question is the link.

**Headroom is a LINK-TIME quantity, and whether it is also PER-IMAGE depends on the
board.** Where `__kickos_ram_start` follows `.bss`, each app's static footprint moves the
arena base and the FATTEST image is what caps `KICKOS_MAX_THREADS`: on `bluepill-c8-st`
the images span 3,040 B of base, and the split `selftest_p3` is the binding one. Where an
alignment window pins the base instead, every image on the board reports the SAME headroom
-- that is `frdmk64f` at `KICKOS_HAVE_MPU=1`, where all images sit at `0x20012940`. Check
which shape a board has before quoting a number, and never quote a board-wide figure for
the first shape. Only the linker knows the arena base, which is exactly why the assert
lives in the linker script and not in CMake.

**Before trimming the demand, price the sections carved between `.bss` and the arena
base.** Each has a different owner and the demand side may be blameless:

- `.userheap`, sized by `KICKOS_USER_HEAP_SIZE`, sits immediately below
  `__kickos_ram_start`, so every byte of heap is a byte of arena. This was the WHOLE
  deficit on `bluepill-c8`: the `CHIP_STM32F103` default was 8192, inherited from its
  128 KiB `stm32f411` sibling, on a part with 20 KiB of SRAM. Its thread provisioning
  (`KICKOS_MAX_THREADS 2` x 2048) was never the problem.
- the `.appdata` enforcement window on an enforcing chip. `frdmk64f` WITHOUT the MPU has
  **+83,328 B** of headroom at `KICKOS_MAX_THREADS 16`; `KICKOS_HAVE_MPU=1` moves
  `__kickos_ram_start` to `0x20012940` and the window eats ~110 KiB of arena. There the
  demand side genuinely had to come down, which is why the enforcing variants provision
  **8** at 8192-byte stacks (`../../boards/frdmk64f/configs/base/defconfig`, `.../st/`)
  while `flat` states no `KICKOS_MAX_THREADS` at all and keeps the default 16. The 8192 is
  a power of two because `KICKOS_TLS` strides every arena block by one, NOT because the
  SYSMPU granule asks for it -- that granule is 32 and this backend is the base+limit row
  of the table above.

The model is validated against the real linker rather than by inspection, on both arena
shapes, and re-swept 2026-08-23. `f302nucleo-st` at `KICKOS_MAX_THREADS=3` measures
**+1,248 B** and links, and at `-DKICKOS_MAX_THREADS=4` FAILS. `frdmk64f-st +MPU` is
provisioned at 8 with **+24,576 B**; it links at 9, 10 and 11, and FAILS at
`-DKICKOS_MAX_THREADS=12` -- at 11 `KICKOS_POOL_TOP` computes to exactly
`__kickos_ram_end`, margin zero, which the `<=` accepts. `bluepill-c8-st` at heap 0 measures
**+1,632 B** on the pool assert and +5,728 B on the boot one, and at
`-DKICKOS_USER_HEAP_SIZE=8192` it FAILS the BOOT assert first. The sign flip lands where the
model says it does in all three.

### The heap is a per-board profile, not a requirement

`KICKOS_USER_HEAP_SIZE 0` is a supported profile, not a broken one. Newlib falls back to
unbuffered stdio when the stream-buffer `malloc` fails, so `printf` and `std::cout` still
emit with no heap. The in-tree precedent is
`nrf51`, whose chip default is heap 0 (`Kconfig`, `KICKOS_USER_HEAP_SIZE`) and whose
`microbit_selftest` QEMU gate is green
(`../../user/apps/common/selftest/CMakeLists.txt:96`).

A heapless profile costs stdio **buffering**, and `malloc` for an app that wants it. It
does not cost the standard-API surface. The project's rule that user-facing apps are
written against `printf`/`std::cout` rather than `kos_*` requires those APIs to work, not
a heap.

So the SRAM floor is **16 KiB with a static-allocation profile**, and the carve is a
decision a variant can state, over a chip default declared in `Kconfig`: 0 on `nrf51`,
2048 on `stm32f302`, 8192 on `stm32f103` and `stm32f411`, 16384 everywhere else.

A part at the floor **plus MPU enforcement** is tighter still, because enforcement costs
RAM of its own: region descriptors, per-domain data in `g_instance`
(`kernel/include/kickos/instance.h:95`), and a fixed `.appdata` window carved for app
globals as one aligned region. The smallest window in the tree is 16 KiB
(`arch/arm/chip/stm32f411/stm32f411.ld:31`) -- the entire SRAM of a part at the floor. No
board in the tree is both small and enforcing: the smallest-SRAM MPU/PMP chips are
`stm32f411` and `xmc4800` at **128 KiB**, and every other one is larger.

### Worked arithmetic

Measured at tip, `-Os`. The four spans sum to the part's SRAM exactly; that identity is
the first check a porter should run.

| Board | SRAM | App | static | `.userheap` | arena | boot stack |
| --- | --- | --- | --- | --- | --- | --- |
| `f302nucleo` | 16,384 | `hello` | 3,776 | 2,048 | **8,512** | 2,048 |
| `f302nucleo` | 16,384 | `selftest`, chip defaults | 7,760 | 2,064 | **4,512** | 2,048 |
| `f302nucleo` | 16,384 | `selftest`, `f302nucleo-st` | 8,672 | 0 | **5,664** | 2,048 |
| `bluepill-c8` | 20,480 | `hello` | 3,456 | 2,048 | **12,928** | 2,048 |
| `bluepill-c8` | 20,480 | `selftest`, chip defaults | 5,096 | 2,072 | **11,264** | 2,048 |
| `bluepill-c8` | 20,480 | `selftest`, `bluepill-c8-st` | 5,096 | 24 | **13,312** | 2,048 |
| `microbit` | 32,768 | `hello` | -- | -- | -- | -- |
| `microbit` | 32,768 | `selftest` | -- | -- | -- | -- |
| `f411disco` | 131,072 | `hello` | 6,872 | 8,200 | **107,808** | 8,192 |
| `f411disco` | 131,072 | `selftest` | 10,760 | 8,216 | **103,904** | 8,192 |

Thread capacity that follows, each board with its own stack sizes:

| Board / app | idle | root | user | arena less boot stacks | N by arena | `MAX_THREADS` | N |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `f302nucleo` `hello` | 512 | 2,048 | 2,048 | 5,952 | 2 | 2 | **2** |
| `f302nucleo` `selftest`, chip defaults | 512 | 2,048 | 2,048 | 1,952 | **0** | 2 | **0** |
| `f302nucleo` `selftest`, `f302nucleo-st` (pre-`124b68c`) | 512 | 2,048 | 1,024 | 3,104 | 3 | 4 | **3** |
| `bluepill-c8` `hello` | 512 | 2,048 | 2,048 | 10,368 | 5 | 2 | **2** |
| `bluepill-c8` `selftest`, chip defaults | 512 | 2,048 | 2,048 | 8,704 | 4 | 2 | **2** |
| `bluepill-c8` `selftest`, `bluepill-c8-st` | 512 | 2,048 | 2,048 | 10,752 | 5 | 2 | **2** |
| `microbit` `hello` | 512 | 2,048 | 2,048 | -- | -- | 4 | -- |
| `f411disco` `hello` | 2,048 | 4,096 | 4,096 | 101,664 | 24 | 8 | **8** |

**The `f302nucleo` and `f411disco` rows predate the `124b68c` right-size** (which
took `KICKOS_ROOT_STACK_SIZE` to 1,536 and `KICKOS_MAX_THREADS` to 3; N stayed **3**, so
those readings hold while their byte columns do not). **The `microbit` rows are worse than
stale and their byte columns are struck out rather than carried**: that board moved to the
32 KiB nRF51822 at four thread slots, and it also acquired `KICKOS_THREAD_SLOTS` kernel-stack
blocks in `.bss` below the arena, so both the SRAM constant and every term derived from it
changed. Only the part constant and the stated stack sizes are kept; re-link the board and
read the rest. The `bluepill-c8` rows were re-linked at the M4.9.3 heap right-size and are
current. Either way, re-link and re-read rather than
quoting (step 5 of the checklist below is that measurement) -- a stale byte column here is
what made this board look like a thread-provisioning problem for a whole milestone.

Four readings, and they are the point of the section:

- **`f302nucleo` `hello` fits with nothing spare.** It needs exactly two threads
  (`../../user/apps/common/hello/main.cc:74-75`) and gets exactly two, from both
  constraints at once. Silicon-witnessed.
- **`f302nucleo` `selftest` is provisioning-bound, not part-bound.** At the chip defaults
  it gets zero threads, and on silicon the suite failed every spawning case (17 ok /
  42 not ok / 10 skipped) on `w >= 0` / `drv >= 0` assertions -- a negative spawn return,
  which is what N=0 predicts. Raising `KICKOS_MAX_THREADS` to 4 alone moved the tally not
  at all: the arena bound, not the pool. Surrendering the 2K heap carve and halving
  `KICKOS_USER_STACK_SIZE` to 1,024 takes the arena to 5,664 and N to 3, and the same
  part then runs the suite at 63 ok / 0 not ok / 5 skipped. Silicon-witnessed both ways,
  measured at `124b68c`. That reading predates `9da898e`, which split this board's suite; it is
  THREE images today, so the board emits no single `1..63` plan; read the plan sizes off the
  configure line (see `boards.md`, *Three boards run the selftest as THREE images*).
- **SRAM size is not the ranking.** `bluepill-c8` has 4 KiB *more* SRAM than `f302nucleo`
  and used to host *fewer* threads, missing `hello`'s second stack by 96 bytes, purely
  because its heap carve was 8K against f302's 2K. That 8K was the `CHIP_STM32F103`
  default inherited from its 128 KiB `stm32f411` sibling; at 2K (and 0 on the `st`
  variant, which carries the fleet's heaviest static image) the same part seats both of
  `hello`'s threads with room over. `microbit` carves heap 0 -- the `CHIP_NRF51` default,
  stated in `Kconfig` and not in the board -- and has the roomiest small-board arena in the
  fleet; it is no longer the same part class, that board having moved to a 32 KiB
  nRF51822. **The heap carve, not the part's
  SRAM, is usually what binds.** (`bluepill-c8` is build-only, so every N here is a model
  prediction, not a witness.)
- On a comfortable board the constraint **inverts**: `f411disco`'s arena would hold 24
  user stacks and `KICKOS_MAX_THREADS 8` is what caps it
  (`../../boards/f411disco/configs/base/defconfig`). Above roughly 32 KiB of
  SRAM, provisioning is the knob and the part is not the limit.

### What binds beyond memory

The arena is one of two independent constraints on a spawn; the other is object-pool
capacity, all of it inside `g_instance` and all `-D`-overridable. **Only the thread pool is
checked at link time, on every board** -- `KICKOS_POOL_ARENA_ASSERT`, see
*The SRAM model*. Every other pool below is unchecked at build time. The suite's own
requirement, for a zero-skip run:

| Knob | Default | Pool | Suite needs | Tight-board value |
| --- | --- | --- | --- | --- |
| `KICKOS_MAX_THREADS` | 16 (`config/system.h`) | `thread.h` (`ThreadPool`) | **>= 4** | 2 |
| `KICKOS_MAX_SEMAPHORES` | 16 (`system.h`) | `instance.h` (`sems`) | **>= 6** | 4 (f302) |
| `KICKOS_CAP_TABLE_SUPPLY` | 16 (`system.h`) | per-thread cap table | **>= 10** | 7 |
| `KICKOS_MAX_MUTEXES` | 8 (`system.h`) | `instance.h` (`mutexes`) | >= 2 | 4 (c8) |
| `KICKOS_MAX_ENDPOINTS` | 4 (`system.h`) | `instance.h` (`endpoints`) | >= 1 | 4 |
| `KICKOS_MAX_IRQ_HANDLES` | 8 (`system.h`) | `instance.h` (`irq_bindings`) | >= 1 | 4 (f302) |
| `KICKOS_MAX_DOMAINS` | `MAX_THREADS + 2` (`system.h`) | `instance.h` (`domains`) | derived | 4 |
| `KICKOS_MAX_TASKS` | `THREAD_SLOTS + 1` (`system.h`) | `instance.h` (`tasks`) | derived | one per live thread under the implicit default, so the floor is `KICKOS_THREAD_SLOTS + 1`; a `static_assert` in `kernel/task/task.cc` refuses less. An EXPLICIT task (`kos_task_create`) that holds no thread is a slot this floor does NOT budget, so an app that creates groups and never populates them makes a spawn answer -KOS_ENOMEM one earlier. A group with N members repays N-1 |

The `Tight-board value` column is the chip default. The `f302nucleo` `st` variant
overrides semaphores to 6 and threads to **3** -- not 4: it was
cut at `124b68c` once the pool assert made the overcommit a link error, and any text citing
`arm.json` for `KICKOS_MAX_THREADS=4` on this board is stale. The stack sizes are the
board's own, in its base defconfig
(`KICKOS_USER_STACK_SIZE` 2048 -> **1024**, `KICKOS_ROOT_STACK_SIZE` 2048 -> **1536**),
chosen against MEASURED paint-and-scan watermarks:
deepest pool worker 592 B, root 1,048 B, idle 76 B -- a 488 B (31.8%) margin on the 1,536 B
root stack. That is the pattern to copy on a tight part: measure the watermark, then
provision, rather than provisioning for comfort.

The cap table is the one row that is NOT a board knob. `KICKOS_MAX_HANDLES` is summed at
configure from four declarations (`cmake/cap_table.cmake`) -- the kernel's reserved range,
the chosen service list's `RETAINED_CAPS`, the app's `CAPABILITIES`, and the peak
concurrent INBOUND reply capabilities a task's table must hold, declared by whoever owns
the protocol's fan-in: `INBOUND_REPLY_CAPS` on `kickos_add_board_provider`,
`CAPABILITIES_INBOUND_REPLY` on `kickos_add_application`, combined as the widest, and
**0 by default** -- nothing in tree declares it. The total is checked against the board's
`KICKOS_CAP_TABLE_SUPPLY`, which is all a board states. A demand that exceeds supply is a
configure FATAL naming every term; a board too small for an app's OPTIONAL peak still
configures, and the arms that wanted those slots reclaim and skip. Beneath the sum the
grant-list floor `KICKOS_MAX_SPAWN_GRANTS + 1` RAISES a width that falls below it instead
of refusing it, and refuses only when the floor itself exceeds the board's supply.
`cap.h` keeps both asserts (`KICKOS_MAX_HANDLES > KICKOS_CAP_FIRST_DYNAMIC`, and
`KICKOS_MAX_SPAWN_GRANTS < KICKOS_MAX_HANDLES`) because `tests/unit/captable` substitutes a
width the sum never produces. A build that misses the generated `kickos/config/cap_width.h`
does not reach them: it fails on the missing include. The suite's own floor is measured off the suite's own call sites. **Two** of the 63 cases need a 4th concurrent
worker: `call_infoless_revert`, four mutually-dependent workers spawned before any join
(`../../user/apps/common/selftest/main.cc:2212-2215`), and `mutex_chain_boost`, a four-link
boost chain (`main.cc:1010-1013`). **Both** ask first, with `pool_can_host(4)`, and since
that probe spawns four real threads it tests the arena as much as the pool. Asking first is
not only about the message: each of the two now creates staging semaphores before its
spawns, and a board too small to host the workers is also too small to supply those, so a
probe is what keeps the outcome a skip rather than a create failure. Detecting the shortfall
from a negative spawn return instead would not say which limit was hit, because
`kos_thread_create` answers `-KOS_ENOMEM` for a full slot table and for a missing stack block
alike. The other 61 cases
need no 4th worker, so a small part loses one chained-priority-inheritance case and
one call/reply case, not the suite. The 6-semaphore peak is `mutex_deadlock`: two permanent
plus four live (`main.cc:1126-1129`) -- but on a 7-handle board the **cap table** binds
first, since `KICKOS_CAP_FIRST_DYNAMIC 2`
(`../../system/include/kickos/sys/cap_index.h`) plus the suite's two permanent caps
leaves 3 free against the 6 that case wants live (rationale `main.cc:1133-1136`).

`f302nucleo` walked that ladder one knob at a time: at `KICKOS_MAX_THREADS=4` plus
`KICKOS_USER_STACK_SIZE=1024` and the 2K heap still carved, which bought one more case
(18 ok / 41 not ok / 11 skipped), `sem_destroy` then failed on a semaphore **handle**
against `KICKOS_MAX_SEMAPHORES 4` (`main.cc:685`); adding `KICKOS_USER_HEAP_SIZE=0` and
`KICKOS_MAX_SEMAPHORES=6` is what took the run to 0 `not ok`. **A spawn failure is not
evidence that the thread pool is the limit; check the arena first, and expect the next pool
behind it.** The runtime cannot tell you which one you hit -- `-KOS_ENOMEM` covers both --
which is why the honest place to answer it is the link, and why `KICKOS_POOL_ARENA_ASSERT`
exists. Eight of `f302nucleo`'s nine pre-`124b68c` skips were arena starvation labelled
"pool too small"; see *The 5 skips on a 16 KiB part*.

`KICKOS_MAX_THREADS` counts **spawnable** threads only. The pool is one slot wider than the
knob (`KICKOS_THREAD_SLOTS`, `kernel/include/kickos/config/system.h`): root holds one for the
life of the system and never reaches `EXITED`, so a spawn still draws the full stated count.
That is why `f302nucleo` runs a two-thread app at `KICKOS_MAX_THREADS 2`. Idle is the one
thread the pool does not seat; it runs on `Kernel::idle_tcb`
(`kernel/include/kickos/instance.h`), handed to `thread_create` by pointer. The knob dominates static RAM: on `f302nucleo` `selftest` at
`KICKOS_MAX_HANDLES=9` and heap 0, `g_instance` measures 2,448 bytes at 2 threads,
3,296 at 4, 4,152 at 6 and 5,000 at 8 -- **about 424 bytes per slot**, because each slot
buys a `Domain` too (`system.h:67`). **Those four figures hold one slot fewer than the same
knob value gives today**: they were measured with root's TCB outside `g_instance`, so the
slope carries and the base does not. **Do not correct them with the 424-byte slope**: that
slope includes a `Domain` per slot, and seating root added a pool slot WITHOUT one, so the
slope overshoots the real growth by roughly 165 bytes. They were also taken at a 9-slot cap table,
a width no board in the fleet configures: `KICKOS_MAX_HANDLES` is not a per-chip knob, it
is the summed total described above, and part of the per-thread cost is the cap run itself,
so neither the base nor the slope carries over to a table of another width unmeasured.

Seating root in the pool itself cost almost nothing, measured on four boards. Static RAM is
**flat to the byte** on `bluepill-c8-st`, `microbit` and `f302nucleo`: the deleted
`g_root_tcb` pays for the extra pool slot exactly. It is **+8 bytes on `xmc4800-relax`**,
where `Thread` is 264 bytes with the FPU context and `ThreadPool` grows 272 rather than 266,
because the extra `gen[]` entry pushes the trailing pointer past an 8-byte boundary. Text grew
184 to 236 bytes depending on the ISA, the most on `microbit`'s Thumb-1.

The suite also allocates from the arena and never returns it: one 4 KiB page
(`main.cc:504`) and three 256-byte domain regions (`:1505`, `:2901-2902`), 4,864 bytes in
all, plus the two self-grant probe ladders (`:3221`, `:3283`) and one spare stack for
`caller_stack` (`:1443`). Each has a real `tap::skip` or a `tap::partial` when
the arena cannot spare it, so these cost coverage rather than a failure.

### Deriving the suite's SRAM figures

Measured, not assumed -- but measured BEFORE the `124b68c` right-size, which took
`KICKOS_ROOT_STACK_SIZE` to 1,536 and `KICKOS_MAX_THREADS` to 3. Every byte column below is
therefore the older link; the METHOD is what to reuse, and step 5 of the checklist is how.
`f302nucleo` `selftest` at the then-shipped `f302nucleo-st` provisioning
(then in `cmake/presets/arm.json`; the cap table was 9 slots wide, a width no
board configures today -- `KICKOS_MAX_HANDLES` is summed at configure and no chip header
states it), `-Os`, no-MPU 16-byte granule. `g_instance` measures 3,336. **That figure and the
`.data`/`.bss` breakdown below it hold one pool slot fewer than the same knob value gives
today**, for the reason stated above, and the 424-byte slope must NOT be used to correct
them: it prices a `Domain` per slot, where seating root added a slot without one, so it
overshoots by roughly 165 bytes.

    static  (.data 380 + .bss 8,260 + 32 alignment)   8,672
    .userheap  (KICKOS_USER_HEAP_SIZE=0)                  0
    arena   [__kickos_ram_start, __kickos_ram_end)    5,664
    _kernel_stack_size                                2,048
    ------------------------------------------------------
    SRAM                                             16,384

That arena pays `align(512) + align(2,048) = 2,560` for the boot stacks and then holds
`floor((5,664 - 2,560) / 1,024) = 3` user stacks with 32 bytes to spare, so
N = min(3, 4) = **3**. That is the whole result and the right-size did not move it: **the
suite passes on 16 KiB of SRAM**, 63 ok / 0 not ok / 5 skipped (measured at `124b68c`,
before `9da898e` split this board's suite into two images), because the cases that do
not need a 4th worker all run at N = 3. This arithmetic is exactly what
`KICKOS_POOL_ARENA_ASSERT` now replays at link time for this chip, with the run-ups paid
against the real arena base instead of a quoted one.

A **zero-skip** run is what needs a bigger part. On top of the 16,384 above it wants a 4th
user stack (1,024) and the permanent arena allocations the remaining skips decline -- the
4 KiB MMIO page and the `kos_ram_alloc(1)` ladder -- plus, at the earlier provisioning, the
shared domain region and the two cross-domain buffers those four un-skipped cases now do
take. That is **a 32 KiB part**, and an enforcing chip pays an `.appdata` window
besides, plus power-of-two natural alignment on every block where
`arch_mpu_region_pow2()` is 1 (a base+limit backend pays only one granule per block). One of
the five is not a RAM question at all: `mutex_deadlock` wants the suite's 3 OPTIONAL
capabilities, so it needs a board whose `KICKOS_CAP_TABLE_SUPPLY` covers the full summed
demand of 10 -- 2 reserved (`KICKOS_CAP_FIRST_DYNAMIC`) + the suite's 5 mandatory peak + 3
optional. Only `bluepill-c8` and `f302nucleo` still supply 7
(`grep -rn KICKOS_CAP_TABLE_SUPPLY boards/`); `microbit` dropped its own and takes the default 16,
so on that part the cap table is no longer what binds.
`f411disco` is the zero-skip
witness -- 128 KiB SRAM, 0 skips -- and it provisions `KICKOS_MAX_THREADS 8`
(`../../boards/f411disco/configs/base/defconfig`), above the measured 4-thread
peak, so 4 is a floor and not a fitted value.

The 16 KiB result is not slack to spend elsewhere: raising only the thread pool on this
part fails the **link** at `KICKOS_MAX_THREADS=12`, with `KICKOS_BOOT_ARENA_ASSERT`
reporting that the arena can no longer hold idle + root.

### The 5 skips on a 16 KiB part

`f302nucleo` was re-provisioned at `124b68c` and the measured skip count on silicon went
**9 -> 5**. Silicon, `.session/m456-silicon/b5-nuc-selftest-after.log`: plan `1..63`,
`# skipped: 5`, `# all tests passed (5 skipped)`. The BEFORE run (`b2-nuc-selftest.log`)
was `# skipped: 9`, and it also carried a real failure -- `not ok 46 -
periph_enable_unheld`.

Every skip is a **named** resource decline the case emits about itself, not a silent pass.
Four are the concurrent-worker ceiling or the arena; one is the cap table:

| Case | TAP skip reason | What actually binds | Site |
| --- | --- | --- | --- |
| `mutex_chain_boost` | pool too small (4 interdependent workers) | 4th worker against N = 3 | `main.cc` (`t_mutex_chain`) |
| `call_infoless_revert` | pool too small (4 interdependent workers) | 4th worker against N = 3 | `main.cc` (`t_call_infoless_revert`) |
| `mutex_deadlock` | pool too small | 6 live caps against the 3 free the 7-slot supply leaves; the suite's 3 OPTIONAL slots are not granted | `main.cc` (`t_mutex_deadlock`) |
| `irq_as_event` | 4 KiB MMIO-page alloc failed -- board too small | one 4,096 B block | `main.cc` (`t_irqdrv`) |
| `mem_self_grant` | arena too small to reach the region ceiling | the `kos_ram_alloc(1)` ladder | `main.cc` (`t_selfgrant`) |

The FOUR that un-skipped are `endpoint_crossdomain`, `mem_self_grant_nonpow2`,
`region_mode` and `domain_share` -- all four wanted arena, and arena is what the
re-provisioning bought. `caller_stack` still reports PARTIAL (`t_caller_stack`:
`accept half not run (arena cannot spare a stack)`) and counts as a pass, not a skip;
`confused_deputy` carries the same construct and did not trip it on this part.

**8 of the 9 former skips were arena STARVATION wearing a "pool too small" label.** That
label is not sloppiness in the cases: `kos_thread_create` returns `-KOS_ENOMEM` for BOTH
"slot table full" and "no stack block", so the two are INDISTINGUISHABLE at runtime and a
case that only sees a negative spawn return cannot tell which limit it hit. This is the
same trap *What binds beyond memory* states from the other end -- a spawn failure is not
evidence that the thread pool is the limit -- and it is why the pool term is now checked at
LINK time on every board (see *The SRAM model*). At the current
`KICKOS_MAX_THREADS 3` against an arena that backs exactly three stacks, the two remaining
4th-worker skips are honest under either reading.

**`mutex_deadlock` is mislabelled differently, and no arena work will ever un-skip it**:
its guard is cap-table and semaphore exhaustion, not memory at all. The two halves of a
declared demand fail in different places, and this arm is the second kind. A **peak** that
the board's supply cannot seat is a configure FATAL naming every term, so it never reaches
a run at all. An **OPTIONAL** peak that does not fit is simply not granted: the width stays
at the mandatory sum, configure announces the dropped slots and the app that declared them,
and the arm that wanted them reclaims what it did get and skips itself
(`cmake/cap_table.cmake`). Granting them here means raising `KICKOS_CAP_TABLE_SUPPLY`, and
the board's defconfig records why that is not affordable on this part.

The genuine 16 KiB limits that remain are small and specific: `irq_as_event` needs one
4,096 B block, and `caller_stack`'s accept half needs 2,064 B.

### Porter's checklist

Given a new part's flash and SRAM, in order:

1. **Program memory, and the optimisation level with it.** Below ~24 KiB at `-Os`, stop:
   the kernel plus a minimal app does not fit -- and the same image is 1.9x larger at
   `-O0`, so budget roughly double if the consumer does not set `CMAKE_BUILD_TYPE`.
   Below 64 KiB the part runs KickOS but cannot host the validation suite -- plan to
   witness it with `apps/blink` and `hello`, or with a named expected-skip list as
   `microbit` does.
2. **Write the board's stack and pool knobs explicitly** in its defconfig. There is
   no useful default on a small part: the `config/system.h` fallbacks are 64 KiB stacks
   and 16 threads, sized for the host sim. If the board ships its own header it must
   restate EVERY knob it needs -- `boards/<board>/include` replaces the chip's header
   outright and does not layer over it.
3. **Decide the heap carve first, not last.** `KICKOS_USER_HEAP_SIZE` comes out of the
   arena 1:1, and on a 16-20 KiB part it is routinely the difference between one thread
   and four. Zero is a supported profile: it costs stdio buffering and `malloc`, not
   `printf`/`std::cout` -- see *The heap is a per-board profile* above.
4. **Read `arch_mpu_min_region()` AND `arch_mpu_region_pow2()` for your chip** before
   computing anything. Only a nonzero granule with `pow2() == 1` makes every arena block
   power-of-two and naturally aligned, which can double the cost of a stack; a base+limit
   backend rounds to the granule instead. See the table above.
5. **Link `hello` and read the four spans** out of the ELF -- `_estack`,
   `_kernel_stack_size`, `_kickos_heap_start`, `__kickos_ram_start`,
   `__kickos_ram_end`. They must sum to the part's SRAM. This is the measurement, and it
   costs one build.
6. **Compute N** = `floor((arena - align(idle) - align(root)) / align(user))`, then
   `N = min(N, KICKOS_MAX_THREADS)`. That is how many threads the board will host.
   Subtract any `kos_ram_alloc` the app makes first.
7. **If N is short**, in decreasing order of yield: cut `KICKOS_USER_HEAP_SIZE`, cut
   `KICKOS_USER_STACK_SIZE`, cut `KICKOS_MAX_THREADS` (~424 bytes of static RAM each),
   then the other pools. Do **not** raise `KICKOS_MAX_THREADS` to fix a spawn failure
   without checking the arena: on a tight part the arena is usually the binding term,
   and the raise spends static RAM to make the arena smaller.
8. **A clean link proves that the boot stacks fit AND that the thread-stack pool fits**,
   on every board: both `KICKOS_BOOT_ARENA_ASSERT` and `KICKOS_POOL_ARENA_ASSERT` are
   mandatory, and a linker script missing either is a configure `FATAL_ERROR`. Configure
   refuses the same script for a second reason: it must also invoke the FOUR
   shared-vocabulary macros `KICKOS_CODE_DEBRIS_SECTIONS`, `KICKOS_NONALLOC_SECTIONS`,
   `KICKOS_STATIC_RELOC_ASSERT` and `KICKOS_TLS_TEMPLATE` (`arch/CMakeLists.txt`; their
   bodies are in `arch/common/sections.ld.h`, which carries a fifth,
   `KICKOS_TLS_ALIGN_ASSERT`, that configure does not demand). That check exists because
   every cross link passes `-Wl,--orphan-handling=error` (`CMakeLists.txt` for the fleet,
   `arch/CMakeLists.txt` for the rp2040 boot2 stage), where an input section no rule names
   is a link failure naming the section and nothing else. Nothing checks the other pools at
   build time, so an object create returning a negative handle is still a hardware-first
   sign.
9. **If your `<chip>.ld` writes an `AT` clause, decide whether the LOADER honours LMA**
   before writing it, and pin the decision with an `ASSERT` either way -- see *An `AT`
   clause in `<chip>.ld` is only valid if the LOADER honours LMA* above. An unhonoured LMA
   is a silent `.data` corruption whose symptom varies by die and does not bisect.
10. **Put every seam your chip defines in `chip_<chip>.cc`** -- or in another member the link
    always pulls for some other reason -- and NEVER in a dedicated TU nothing else references.
    A seam's fallback is a one-symbol archive member, not a weak symbol, so an unanchored
    definition is never extracted: the fallback answers first and the board silently DECLINES
    at runtime. Gated by ctest `seam_defaults`; see *Privileged register write* above.

---

## The ARMv7-M syscall spike (the M1 de-risk)

### The problem

`arch.h` requires (portability-critical contract, quoted from the header):

> the arch MUST run `syscall_dispatch()` in privileged **THREAD** context on the
> calling thread's own continuation -- NOT in ISR/handler context. A blocking
> syscall blocks by an ordinary synchronous context switch ... `arch_in_isr()` must
> read false during dispatch.

On Cortex-M, `SVC` traps into **handler mode**. If `syscall_dispatch()` ran in the
SVC handler, then during a blocking call:

- `arch_in_isr()` (IPSR != 0) would read **true** -- contract violation;
- a blocking `arch_switch()` pends **PendSV**, but PendSV is lower priority than
  the active SVC handler, so it **cannot preempt** it. The switch would be
  deferred until SVC returns -- i.e. the syscall would return to the user instead
  of blocking. The kernel's blocking primitives would break.

The roadmap note: *validate this in M1 week one; if infeasible the fallback
(deferred syscall completion) is a core restructure.* **It is feasible.** Design:

### The design (implemented in `arch/arm/armv7m/switch.S`)

Dispatch runs in **privileged thread mode** via an exception-return trampoline.
Two observations make it small:

1. The hardware exception frame that `SVC` stacks **already carries the syscall
   arguments** (`r0`=nr, `r1..r3`=a0..a2, `r12`=a3 -- the user stub loads a3 into
   r12 before `svc`) and, in the stacked `LR`, the return address of
   `arch_syscall`'s caller. So the SVC handler only has to rewrite the stacked
   `PC`.
2. **Lowering** privilege in thread mode needs no trap -- a plain `MSR CONTROL`
   with `nPRIV=1` suffices. Only *raising* privilege needs the trap.

Flow:

```
user (unprivileged, PSP)          arch_syscall:  ldr r12,[sp]  ; a3 -> stacked r12
                                                 svc #0
      |  hardware stacks {r0..r3,r12,lr,pc,xPSR} on PSP, enters handler mode
      v
SVC_Handler (handler mode)        rewrite stacked PC := svc_trampoline
                                  CONTROL.nPRIV = 0            ; trampoline privileged
                                  bx lr (EXC_RETURN thread/PSP)
      |  hardware unstacks -> thread mode, r0..r3/r12 = args, LR = caller-return
      v
svc_trampoline (PRIVILEGED THREAD mode, on the thread's own PSP stack)
                                  push {r12,lr}               ; a3 at [sp], save caller-return
                                  bl syscall_dispatch         ; <-- runs in thread mode
                                  ... (may block: see below) ...
                                  CONTROL.nPRIV = 1           ; drop to unprivileged
                                  bx lr                       ; -> arch_syscall's caller, r0=result
```

**Why blocking now works.** `syscall_dispatch` runs in *thread* mode, so
`arch_in_isr()` reads false. A blocking call reaches `arch_switch`, which pends
PendSV. When the kernel's `IrqLock` (BASEPRI) is released, **PendSV -- a real
exception, higher priority than thread mode -- preempts the trampoline** and
performs the switch. PendSV freezes the entire mid-dispatch continuation on
*this thread's PSP stack* and resumes it inline when the thread is next
scheduled. This is precisely the sim's synchronous-`swapcontext` semantics,
achieved with the native exception mechanism. No deferred-completion restructure
is needed. **Feasibility: confirmed.**

### Registers and privilege across a blocking switch

- The trampoline clobbers only `r0-r3,r12,lr`; the user's `r4-r11` (live since
  `svc`, untouched by exception entry/return and by the SVC handler) pass through
  to the caller intact. `syscall_dispatch` preserves `r4-r11` per AAPCS.
- Kernel syscall work runs on a stack **the calling thread alone owns**, and
  `KICKOS_KERNEL_STACKS` says which of its two. At 1 the trampoline relocates
  `sp` onto that thread's `ctx.kernel_sp` -- its slot in the
  `KICKOS_THREAD_SLOTS` x `KICKOS_KERNEL_STACK_SIZE` array in kernel `.bss`
  (`kernel/thread/thread.cc`, `KStackBlock`) -- and calls `syscall_dispatch`
  there, below a continuation header laid at the block's top; the only bytes it
  spends on the thread's own stack are the eight the transfer pushes inside the
  hardware frame the exception return just popped
  (`arch/arm/armv7m/switch.S`, `svc_trampoline`). At 0 -- the sim, and the
  no-MPU armv7m boards that keep a red zone instead -- dispatch stays on the
  caller's own thread stack and user stacks must be sized for kernel call
  depth. That was the M0 model on every backend; the separate per-thread kernel
  stack is the refinement, and it landed.
- **Privilege is a saved/restored register, not the resting privilege.** A thread
  blocked mid-syscall is *privileged* (the trampoline raised it). PendSV therefore
  **saves the outgoing thread's current `CONTROL.nPRIV`** into its context and
  restores the incoming thread's saved value -- so a mid-syscall thread resumes
  privileged, then the trampoline lowers privilege on the way back to the user.
  This is the ARM twin of the sim's `SimContext::raised` re-raise-on-switch-in.

#### Probing the ring: four design facts that are traps

All four are ARMv7-M architecture, not KickOS policy (ARM DDI 0403E.e). The prober that
pins them is `user/apps/common/ringpriv`.

- **A FAULT is the WRONG expectation for an unprivileged `MSR` to a privileged-only
  special register.** B5.2.3 gates the whole `MSR` group on `if CurrentModeIsPrivileged()`
  with no else clause, and its Exceptions clause reads "None". So an unprivileged write to
  `CONTROL.nPRIV` is **IGNORED, not trapped** -- a test asserting a fault would FAIL on
  correct hardware. The assertion has to be a READ-BACK.
- **Only `CONTROL` can carry that read-back.** B5.2.2's `MRS` pseudocode returns ZERO for
  `PRIMASK`/`BASEPRI`/`FAULTMASK` when unprivileged
  (`R[d]<7:0> = if CurrentModeIsPrivileged() then BASEPRI<7:0> else '00000000'`), so
  "wrote a mask, read back zero" is VACUOUS: it reads zero whether or not the write
  landed. `MRS CONTROL` carries no privilege guard in that same pseudocode. The obvious
  prober is the wrong one.
- **The read, the write, the `isb` and the read-back must be ONE asm block.** The syscall
  return path restores `CONTROL.nPRIV` from `ctx.resting_npriv`
  (`arch/arm/armv7m/switch.S`), so a single `print` between the `msr` and the read-back
  ERASES a real promotion and turns the gate GREEN on a broken system. Measured while
  building the prober: `0x2` before, `0x3` after a landed write, and back to `0x2` after
  one print. Capture first, print after -- this is the `npriv-banked-on-switch` invariant
  (`invariants.md`) seen from the userspace side.
- **Unprivileged PPB access is a DIFFERENT mechanism, and it is MPU-independent.** B3.1.1
  makes the PPB (`0xE0000000`-`0xE0100000`, which contains the System Control Space)
  privileged-access-only, so an unprivileged read of `SCB->CPUID` BusFaults with the MPU
  OFF. Confirmed on `f302nucleo` silicon by debugger attach: `CFSR=0x00008200` (BFSR
  `0x82` = `PRECISERR | BFARVALID`), `BFAR=0xe000ed00` (the exact probed address), and
  `HFSR=0x40000000` (`FORCED`, because `BUSFAULTENA` is never set in-tree). Assert `BFAR`,
  not a banner.

The gates are `tests/integration/check_app_arms.sh` and `tests/integration/check_qemu_ringppb.sh`, and neither
is conditioned on enforcement: `cmake --preset qemu-flat` IS the ring-only posture (the
board's `flat` variant, its base one enforcing), which is what makes the ring gateable in
CI rather than only capturable on no-MPU silicon. Both run permanently on the MPS2 M3/M4/M7/M33
arms. `microbit` (Cortex-M0: no Unprivileged/Privileged Extension, so `msr CONTROL` is
discarded) asserts the OPPOSITE outcome with one arm rather than skipping, which
machine-checks the armv6m classification instead of leaving it as prose; it does not build
`ringppb`, because on a no-ring core the PPB read legitimately succeeds and a confinement
gate there would assert the reverse. **A porting obligation follows for armv6m:** the core's
class is per BOARD, not per arch (M0+ implements the extension, M0 does not, and no
predefined macro separates them), so a new armv6m board must state its class in
`user/apps/common/ringpriv/CMakeLists.txt`. An unclassified one is a configure
`FATAL_ERROR` rather than a silently inherited default and a vacuous pass.

---

## Context switch (PendSV) & first-thread start

`arch_switch(from, to)` never switches inline: it records `to` in `g_arch_next`
and pends PendSV (the outgoing thread is always `g_arch_current`). PendSV:

- saves `{r4-r11, EXC_RETURN}` (and `{s16-s31}` iff the FP frame is active,
  keyed on `EXC_RETURN` bit 4) to the outgoing thread's PSP, stores its SP;
- loads the incoming thread's SP, restores the callee/FP registers, sets
  `CONTROL.nPRIV`, and exception-returns onto the incoming thread's PSP.

**That PSP is either of the thread's two stacks, and PendSV must refuse neither.** A thread
preempted mid-dispatch is running on its own kernel block, so where `KICKOS_KERNEL_STACKS`
is 1 the handler first tests the PSP against THIS thread's `ctx.kernel_sp` window -- that
one and never the whole array -- and the block leg takes no room test, every byte below a
kernel PSP being written by privileged code through a pointer the kernel seated. Only a PSP
outside that window falls through to the user-stack bounds guard, which is also what an
unseated block (`kernel_sp` reads 0) and idle take.

The saved-frame layout (low->high address on PSP):

```
[r4 r5 r6 r7 r8 r9 r10 r11 EXC_RETURN]   <- PendSV-saved   (ctx.sp points here)
[r0 r1 r2 r3 r12 LR PC xPSR]             <- hardware frame
```

`arch_context_init` fabricates exactly this: `r0`=arg, `PC`=entry, hardware-frame
`LR`=`kickos_thread_return` (a returning entry lands there), `EXC_RETURN`
=`0xFFFFFFFD` (thread/PSP, non-FP).

`arch_start` reuses PendSV: it sets `g_arch_current = 0` (the "nothing to save"
sentinel), points `g_arch_next` at the first thread, and pends PendSV. The switch
fires when `sched::start()` releases its `IrqLock`; the boot MSP frame is
abandoned (the system never returns to boot).

---

## Critical section, timer, clock, NVIC

- **Critical section** = `BASEPRI`. `arch_irq_save` raises BASEPRI to
  `PRIO_LOCK_BASEPRI` (0x20); PendSV/SysTick/SVCall sit at 0xE0-0xF0 and device
  IRQs must be configured >= 0x30, so the lock masks all of them while leaving a
  future 0x00/0x10 zero-latency band unmaskable. (v6-M/RP2040 will use PRIMASK.)
- **Monotonic clock** = the **DWT cycle counter**, extended to 64-bit in software.
  *Limitation:* a 32-bit wrap (~35 s at 120 MHz) not observed within one period
  is missed; a DWT/timer overflow interrupt is the refinement.
- **One-shot timer** = **SysTick** in a disarm-on-fire (tickless) mode. Deadlines
  beyond the 24-bit range fire early and the kernel re-arms the remainder (a
  harmless extra wake). A dedicated chip compare timer is the refinement.
- **NVIC** backs `arch_irq_mask/unmask/inject`. `inject` latches a raise on a
  masked (disabled) line (ISPR holds pending independent of ISER): it coalesces
  one-deep and fires at the next `unmask`. `arch_irq_clear_pending` (ICPR) is the
  explicit discard, used at first-arm to drop pre-registration garbage.
- **MPU** -- the shared ARMv7-M **PMSA** backend (`arch_arm_common.cc`) provides per-domain
  enforcement at M2. `arch_mpu_apply` only **stashes** the incoming region set and is a PLAIN,
  non-overridable definition shared by every ARM backend; `kickos_arch_mpu_commit` (fallback TU
  `arch/arm/common/kickos_arch_mpu_commit_default.cc`) / `kickos_arm_mpu_program` **program the
  hardware** from the PendSV switch epilogue, after the physical swap (the deferred-commit seam,
  `design-mpu-commit-deferred.md`). A chip with a non-PMSAv7 MPU defines the commit, never
  `arch_mpu_apply` (K64F SYSMPU, RP2350 PMSAv8). `arch_mpu_region_encodable` bounds a grant to
  what the backend can describe.
- **Rule 7 reserved blocks (M4)** -- an enforcing chip MUST define `arch_reserved_blocks`
  (its owns-for-life peripherals: timebase, IRQ controller, every access-permission controller
  -- the MPU/PMP twin AND any bus-side gate, e.g. the K64F AIPS PACR pages or the ESP32-C6
  HP_APM/HP_TEE -- and the clock/reset gates); there
  is no fallback TU, so a missing one is a link error. A new **ARMv7-M chip with the
  Cortex-M bit-band alias (any M3/M4)** must also define `arch_bitband_present()` to return
  **1** -- the fallback answers 0, which is fail-OPEN for the bit-band alias refusal (a device
  grant into a reserved block's `0x42000000`/`0x22000000` alias image would be admitted).
  M7 (no bit-band) and non-ARM archs keep 0.
  On F411 it is **build + enforcement-link validated; silicon proof pending** (the
  canonical PMSA per-thread MMIO proof is `design-spi-driver-stm32f411.md`); PMSA
  enforcement is proven on silicon on XMC4800. See `m2-readiness.md`.

---

## MPU descriptor encodings, per backend

The seam is the same on every chip (`{base, size, attr}`, `attr` = unprivileged rights); the
register encoding is not. These are the two encodings the Reference otherwise names only by
symbol, and both are the fastest path to a correct backend for the next part in their family.

### ARMv8-M PMSAv8 (Cortex-M33: `rp2350`, and the next nRF5340 / STM32U5 / STM32H5)

`MPU_TYPE` (`0xE000ED90`), `MPU_CTRL` (`0xE000ED94`) and `MPU_RNR` (`0xE000ED98`) are
LAYOUT-COMPATIBLE with v7-M, so `MPU_CTRL = ENABLE | PRIVDEFENA` still turns the unit on and the
kernel stays the degenerate privileged domain on the background region. **The region descriptors
are not compatible, and they REUSE the same two addresses**, which is why pointing the v7-M
backend at an ARMv8-M MPU fails closed rather than under-programming:

| Address | v7-M | ARMv8-M |
|---|---|---|
| `0xE000ED9C` | `MPU_RBAR` = base[31:5], low 5 bits reserved | `MPU_RBAR` = base[31:5] + `SH[4:3]` + `AP[2:1]` + `XN[0]` |
| `0xE000EDA0` | `MPU_RASR` = ENABLE[0] + SIZE[5:1] + AP[26:24] + TEX/S/C/B + XN[28] | **`MPU_RLAR`** = limit[31:5] + `AttrIndx[3:1]` + `EN[0]` |
| `0xE000EDC0` / `0xE000EDC4` | -- | `MPU_MAIR0` / `MPU_MAIR1`, 8 attribute bytes selected by `AttrIndx` |

The v7-M path writes `base & ~0x1F`, which on ARMv8-M clears `AP` to `0b00` (privileged-only) and
so denies a thread its OWN stack on the first unprivileged access; and it writes its RASR word to
what is now `RLAR`, where bit 0 happens to be `EN` (the region enables) while limit[31:5] comes
out of the AP/TEX/S/C/B bits, i.e. a limit unrelated to base+size, with `AttrIndx` pointing into
an unprogrammed MAIR (reset MAIR = 0 = Device-nGnRnE everywhere).

Encoding actually used (`arch/arm/common/arch_arm_pmsav8.cc`, field defs in
`arch/arm/common/regs_v8m.h`):

- **base** -> `RBAR.BASE[31:5]` (`base & ~0x1F`); **base+size** -> `RLAR.LIMIT[31:5]` as an
  INCLUSIVE top, `(base + size - 1) & ~0x1F`. Because the limit is stated directly there is no
  power-of-two size and no natural-alignment gap: `arch_mpu_region_pow2()` is 0 and
  `arch_mpu_region_encodable` is the byte-granular form at a 32-byte granule
  (`size >= 32 and (base & 31) == 0 and ((base + size) & 31) == 0`).
- **`AP[2:1]`**, ARMv8-M ARM encoding: `00` = RW privileged-only, `01` = RW any, `10` = RO
  privileged-only, `11` = RO any ("any" = unprivileged permitted; the kernel reaches everything
  through PRIVDEFENA regardless). The encoder keys off X and W exactly as `mpu_rasr` does on
  v7-M: `ARCH_MPU_X` (code) gives `AP = 11` with `XN` CLEAR; everything else sets `XN` and takes
  `AP = 01` when `ARCH_MPU_W` is present, `AP = 11` when it is not. So a data or stack region is
  `01` + `XN`, read-only data is `11` + `XN`, and an MMIO grant (`ARCH_MPU_DEV`, issued `R|W`) is
  `01` + `XN` with the Device MAIR slot instead of the Normal one. `SH` is left 0 throughout.
- **MAIR is ONE-TIME, not per region**: two slots programmed once (index 0 = Normal
  write-back/write-allocate `0xFF`, index 1 = Device-nGnRE `0x04`) with `RLAR.AttrIndx`
  selecting between them. It consumes no region slots. Under SMP it must run once PER CORE
  (`0xE000EDxx` is core-local and the MPU register file is banked), not once globally.
- **Region count** comes from `MPU_TYPE.DREGION` -- read it, do not hard-code; the M33 on
  RP2350 implements 8, the same budget as the rest of the fleet.
- Violations raise **MemManage** with `MMFSR`/`MMFAR` exactly as v7-M, so the shared ARMv7-M
  reporter needs no change.

**A whole separate `armv8m` arch directory was rejected**, and the reason generalises to the next
ARMv8-M part: Mainline is a superset of v7-M for everything the arch layer touches -- BASEPRI
critical section, DWT, SysTick, NVIC, PendSV/SVCall -- so a new arch would duplicate a large,
identical backend to swap one file. The chip reuses `armv7m` verbatim and opts into PMSAv8
through its own `mpu.cmake` (`KICKOS_ARM_PMSAV8_SOURCE`), which pulls the PMSAv8 TU into the chip
library so the v7-M fallback TUs are never extracted. A v7-M chip's `mpu.cmake` never adds it and
its fallback stands. Selection is by presence-in-link, not by an `#ifdef` fork inside one
function -- and the overridden symbol is `kickos_arch_mpu_commit` (the deferred-commit seam),
never `arch_mpu_apply`, which stays the shared stash. The posture matters: the PMSAv8 TU enters
the link only at `KICKOS_HAVE_MPU=1`, so a non-enforcement build of the SAME chip still takes the
v7-M pow2 fallback (`arch/arm/common/arch_mpu_region_pow2_default.cc`).

### RXv3 RX MPU (`rx72m`)

Eight access-control regions plus one background region, base `0x0008_6400` (HW UM section 17),
page granularity **16 bytes** -- the page number is address[31:4]:

| Register | Address | Contents |
|---|---|---|
| `RSPAGEn` | `0x00086400 + n*8` | `RSPN[27:0]` = START page = `base & 0xFFFFFFF0` |
| `REPAGEn` | `0x00086404 + n*8` | `REPN[27:0]` = END page, **INCLUSIVE**, plus `UAC[2:0]` and `V[0]` |
| `MPEN` | `0x00086500` | bit 0 global enable; checking begins on the `RTE`/`RTFI` that next shifts to user mode (UM 17.2.3) |
| `MPBAC` | `0x00086504` | background (whole 4 GiB) user access in `UBAC[2:0]`, same field layout as `UAC` |
| `MPOPI` | `0x00086526` (16-bit) | writing 1 to bit 0 clears `V` on every region (UM 17.2.10) |

`UAC` bit order is **R = bit 3, W = bit 2, X = bit 1** -- read is the HIGH bit and execute the
low, NOT `r/w/x` LSB-first, and `V` is bit 0 of the same `REPAGEn` word. Write `RSPAGEn` before
`REPAGEn` and put `V` in the `REPAGEn` write, so a slot is never briefly valid with a stale
end/attr.

**`MPBAC` must stay 0.** Overlapping regions OR their permission bits with the background
(UM 17.1.4), so any nonzero `MPBAC` silently grants every user thread everywhere -- it is the one
value that turns enforcement into a no-op with no other symptom. The RX MPU also checks USER mode
only, so there is no supervisor-field hazard and no privileged region to spend. Its registers are
supervisor-only and are NOT `PRCR`-gated (UM Table 13.1 omits them), so no unlock bracket.

**`UM section 17.4.3` requires a READBACK barrier**: read any MPU register after programming so
the writes are in effect before the scheduler's `RTE` drops into user mode. It is the RX analog
of ARM's `DSB`/`ISB`, and the read must be consumed (`arch/rx/rxv3/arch_rxv3.cc` feeds it to an
empty `asm` with a `"memory"` clobber) or the volatile load can be dropped or reordered past the
`RTE`. A region the 16-byte pages cannot represent EXACTLY is fail-closed (slot left `V = 0`),
never masked wider, matching the ARM PMSA and PMP skip: rounding to page bounds would grant up to
15 bytes past the region on each side.

## Clock retune: timer-clock domains and the wait-state ordering

**Widen flash wait-states and raise voltage BEFORE the frequency rises; relax them AFTER it
falls.** A raw core-clock increase past the current wait-state's access window makes an
instruction fetch return before the data is valid -- a fetch FAULT, not merely wrong timing --
and on the way down the old, higher wait-state count is safe across the whole descent, so
relaxing early is the only unsafe half. Four chips set flash wait-states on this rule at boot
(`xmc4800` `FCON.WSPFLASH`, `stm32f103` and `stm32f411` `FLASH_ACR.LATENCY`, `sam3x8e`
`EEFCn_FMR.FWS`), and `xmc4800`'s runtime retune is the only backend that walks BOTH directions:
widen then staircase `K2DIV` down on a rise, staircase up then relax on a fall. `mk64f` applies
the identical discipline to the divider it has instead -- `SIM_CLKDIV1` widened before the MCG
walks `FEI -> FBE -> PBE -> PEE`, dropped after `PEE -> FEI`. Over-provisioning is always the
safe side: `sam3x8e` sets `FWS = 4` unconditionally because it also covers the RC and
crystal-less fallbacks. The voltage half has no in-tree implementation yet -- the RT1062's DCDC
higher-OPP step is part of its deferred 600 MHz tree -- so a port that raises voltage owes the
same ordering with nothing to copy from.

**Whether the monotonic clock survives a retune is a per-chip fact, and the console is a SECOND,
independent fact.** `arch_clock_now` is only immune where its counter sits in a clock domain the
retune does not move; where it is not immune the backend must RE-ANCHOR at the rate edge, inside
the chip code at the exact instruction that changes the divider, because every raw tick that
elapses between the `SystemCoreClock` update and the anchor is priced wrong and frozen into the
epoch permanently. Without the anchor, applying the new reciprocal to the whole accumulated tick
count reprices ALL of history: at 144 -> 24 MHz the multiplier is 6x larger and `now` jumps
forward by roughly `elapsed_since_boot * (ratio - 1)`.

| Chip | Monotonic counter | Its clock domain | Immune to a retune? |
|---|---|---|---|
| `xmc4800` | CCU40 (64-bit, linked slices) | `fCCU = fSYS = SystemCoreClock` | **no** -- re-anchors at the rate edge |
| `mk64f` | PIT ch0+ch1 chained | bus = `SystemCoreClock / BUS_DIV` (`BUS_DIV` = 2) | **no** -- re-anchors at the rate edge |
| `stm32f411` | TIM2 (32-bit + software high word) | timer kernel clock = HCLK = `SystemCoreClock` | **no**, but LATENT -- see below |
| `rp2040` | 64-bit TIMER via the watchdog tick | `clk_ref` (stays on the 12 MHz XOSC) | yes for the CLOCK; **no for the CONSOLE** |
| `rp2350` | 64-bit TIMER0 via the TICKS generator | `clk_ref` | yes for the CLOCK, same console caveat |
| `nrf51` | semihosting `SYS_CLOCK` | host, not core-derived | yes |
| `sim` | host `CLOCK_MONOTONIC` | host | yes |

`xmc4800` and `mk64f` are also the only two chips that ship an `arch_cpu_clock_set` at all, so
today the not-immune set and the re-anchoring set coincide.

**`stm32f411` is the latent hazard, and a porter adding a retune there must not read its silence
as immunity.** TIM2 is same-domain -- with `HPRE = /1` and `PPRE1` in `{/1, /2}` the APB
timer-clock doubler makes the timer kernel clock equal HCLK, which is why the anchor is seeded
from `SystemCoreClock` and not from `PCLK1` (`PCLK1` is 42 MHz against an 84 MHz HCLK). So its
`now` WOULD jump under a retune. It does not today only because F411 implements no
`arch_cpu_clock_set` and takes the fallback: it carries the sole-writer cleanup (`mult` written
only by the boot anchor, never lazily recomputed inside `arch_clock_now`) but never re-anchors,
because there is no rate edge to re-anchor at. Adding a retune to this chip means adding the
re-anchor in the same commit. Note also that retuning `PPRE1` to `/4` or worse breaks the
doubler identity the anchor assumes.

**The RP2040/RP2350 console is the other half of the same trap.** Their TIMER is on `clk_ref` and
is genuinely immune, but `clk_peri` TRACKS `clk_sys`, so a `clk_sys` retune moves the UART baud
divisor and the console garbles unless the coherence tail re-derives it -- exactly like the three
not-immune chips. They are the reference model for the TIMER only, never for the console. The
general rule for the baud half: re-derive only after the TX shift register is SHIFT-IDLE
(transmission complete), not merely after the TX buffer is empty, or the byte still clocking out
is garbled; and a target rate whose clock cannot produce the console baud within tolerance is
REFUSED at the seam rather than landed with a silently lowered baud.

---

## The QEMU verification target (`boards`: `qemu`, chip `mps2`)

A runnable armv7m target validates the arch layer on real Cortex-M4:
`qemu-system-arm -M mps2-an386`. The `mps2` chip backend
(`arch/arm/chip/mps2/`) uses **semihosting** for the console (`SYS_WRITEC`) and
exit (`SYS_EXIT_EXTENDED`), so it needs no UART; its linker script maps code at
0x0 and SRAM at 0x20000000.

Two toolchain/QEMU gotchas the chip layer resolves (both documented at their fix
site):
- **Default linker script.** The pinned Arm GNU toolchain (newlib) injects no
  default linker script, so the app's board script applies cleanly. The app link
  passes its script with a **driver-level `-T`** (not `-Wl,-T`) regardless -- it
  is correct either way, and it is what the earlier Debian arm-none-eabi toolchain
  (picolibc) *required*, since picolibc's spec injected a default `-T picolibc.ld`
  unless it saw a driver-level `-T` (a `-Wl,-T` was invisible to that check and the
  two scripts collided at address 0). Keep the driver-level form. (See the `kickos`
  interface `target_link_options`.)
- **QEMU's DWT cycle counter is frozen.** `arch_clock_now` has NO armv7m fallback at
  all -- it is a required per-chip definition (`arch_armv7m.cc`, "Monotonic clock: NO
  armv7m default"), so a board that forgets it fails to LINK; `mps2` supplies it from
  the semihosting `SYS_CLOCK` (the monotonic clock source is legitimately
  chip-specific). The DWT `CYCCNT` read is a *different* seam, `arch_trace_now`, whose
  fallback TU (`arch/arm/armv7m/arch_trace_now_default.cc`) real silicon keeps; `mps2`
  displaces that too, deriving microseconds from `SYS_CLOCK`. Caveat: on QEMU the clock
  (host wall-time, 10 ms granularity) and the one-shot timer (SysTick counting
  virtual cycles) are **two uncorrelated timebases**, so sub-10 ms deadlines land
  up to 10 ms late and can cause a bounded re-arm churn until the coarse clock
  advances. This makes the QEMU gate a *functional* check, not a timing-accurate
  one; real silicon runs the clock and the compare off the same source.

## The RP2040 chip (`board`: `picopi`, chip `rp2040`, armv6m) -- hardware-validated

The Raspberry Pi Pico (RP2040, Cortex-M0+) is the first KickOS target **confirmed
running on real silicon** (`apps/blink` blinks the onboard LED on GP25). Three
things make it the most involved chip bring-up so far:

- **Flash second stage (boot2) + XIP.** The RP2040 executes in place from external
  QSPI flash, but only after a 256-byte second stage configures the Synopsys SSI.
  The bootrom copies those 256 bytes to SRAM, checks a **CRC-32/MPEG-2** over bytes
  0..251 (little-endian at 0xFC), and jumps in. `boot2.S` does the minimum
  (datasheet section 4.10.3): disable the SSI, program the "03h serial read per access"
  XIP mode (`CTRLR0=0x001f0300`, `SPI_CTRLR0=0x03000218`, BAUDR=4), re-enable, then
  set VTOR and hand off to the app vector table at 0x1000_0100. It is
  position-independent (runs from a bootrom-chosen SRAM address). The build wires
  the multi-stage image: assemble+link `boot2.S` (own `boot2.ld`, <=252 bytes) ->
  `objcopy -O binary` -> `cmake/rp2040_checksum.py` (appends the CRC, emits a
  `.boot2` data blob) -> into the chip archive, force-linked with `-Wl,-u`.
  - **Driver-level `-T`, again.** The boot2 *sub-link* passes `boot2.ld` with a
    driver-level `-T` (not `-Wl,-T`) for the same reason as the app link -- required
    under the old picolibc apt toolchain, retained (harmless) under newlib.
- **No PLL -- one 12 MHz crystal drives everything.** For a first bring-up the clock
  tree is deliberately minimal (no PLL sequencing): enable the XOSC, switch
  `clk_ref` to it (so `clk_sys` follows to 12 MHz -- precise SysTick,
  `SystemCoreClock=12e6`), and point `clk_peri` at it (precise UART baud). A
  1 MHz watchdog tick feeds the 64-bit system TIMER, which is `arch_clock_now`
  (the RAW halves, hi/lo/hi re-read -- no DWT on v6-M, and core-safe if core 1 is
  ever launched). Every poll is **bounded**: a dead crystal degrades to the ROSC
  default instead of hanging.
- **Reset-release ordering is load-bearing.** A peripheral's `RESET_DONE` only
  asserts once it has a running clock. IO_BANK0/PADS_BANK0/TIMER (clk_sys/clk_ref,
  live at reset) are released first; **UART0 is clocked by `clk_peri` and must be
  released *after* `clocks_init`** -- release it first and its `RESET_DONE` never
  asserts, hanging the boot with no sign of life. (This exact bug bit the first
  bring-up; the LED-bisection diagnostic localized it to the reset poll.)

No RP2040 model ships in mainline QEMU, so there is no CI gate; the image is
build-verified (boot2 CRC recomputed, `.boot2` at 0x1000_0000, vectors at
0x1000_0100) and confirmed by flashing a Pico (BOOTSEL + `picotool load -x`).
The board is always BOOTSEL-recoverable, so a wrong boot2/clock config cannot
permanently brick it.

## The RISC-V RV32IMAC arch (boards `qemu-riscv`->chip `virt`, `esp32c6-wroom`->chip `esp32c6`)

The first RISC-V ISA (ESP32-C6 + the QEMU `virt` run target), sharing one arch
(`arch/riscv/rv32imac/`) across two chips. Closest to the RX72M model: a single
save-frame, deferred switch.

- **Trap model** = a VECTORED `mtvec` table, all 32 of whose slots enter the SAME
  handler (`trap_entry`, switch.S; `kickos_rv32_init` sets the mode bit, the ESP32-C6
  core implementing vectored mode only). It saves the FULL interrupted context
  (28 GPRs + `mepc` + `mstatus` + the interrupted `sp`, 128 B), then demuxes on
  `mcause`: ecall (8/11), machine software / msip (the switcher), machine timer /
  mtip. **Which stack the frame lands on is the entry's decision, and there are
  three destinations**: a U-mode entry builds it on the interrupted thread's own
  KERNEL block (`ctx.kernel_sp`, after bounds-testing the interrupted `sp`; a U-mode
  thread with no block is refused as wild), the two M-mode causes whose frame the
  switcher stores as `outgoing->sp` (msip, ecall-from-M) keep the `sp` they
  interrupted, and every other M-mode trap uses the trusted per-hart trap stack.
  `gp` and `tp` stay OUT of the frame, both being U-mode writable, and are written
  from the kernel's own knowledge on every resume instead: `gp` from the link-time
  `__global_pointer$`, but **`tp` is NOT a link-time constant** -- it is the running
  context's `stack_lo` masked down to `KICKOS_TLS_STRIDE`, which is that thread's
  own stack block and so its TLS base. ONE frame format for a voluntary block and a
  preemptive wake -- the RX/PendSV property.
- **Context switch** = deferred via the **CLINT machine software interrupt
  (`msip`)**. `arch_switch` records `g_arch_next` + pends msip; the physical swap
  ALWAYS happens in the msip trap (`.Lswitch`). Held off while an IrqLock masks
  `mstatus.MIE`. The CLINT base is chip-provided (`g_clint_msip`).
- **Syscall** = **`ecall`** -> `svc_trampoline` running **M-mode (privileged) with `sp`
  at the saved frame** (mret with `MPP=M`): the calling thread's own KERNEL block for a
  U-mode caller, the `sp` the trap interrupted for an M-mode one. Either way the
  continuation is per-thread (the arch.h contract) and the msip switcher freezes a
  blocking dispatch on that same stack, `sp` being callee-saved across it. This arch
  selects `ARCH_KERNEL_STACKS_MANDATORY` (`arch/Kconfig`), so `KICKOS_KERNEL_STACKS`
  resolves to 1 on every board and the entry compiles no red-zone path beside the
  transfer. The frame keeps the caller's `mstatus` (`MPP`=caller priv) for the return;
  `mepc`+4 skips the (4-byte) `ecall`.
- **Critical section** = `mstatus.MIE` (clear via `csrrci`, restore via `csrs`);
  `arch_in_isr` reads `g_isr_depth` (bumped only by the timer/external paths).
  `arch_idle_wait` = `wfi`.
- **Trace clock** = the `rdcycle` CSR (32-bit raw; `mcounteren` lets U-mode read
  it) **where the core implements Zicntr, which is a per-CHIP fact**: legal on
  `virt`, an illegal instruction on the ESP32-C6 HP core in every mode, so
  `esp32c6/caps.cmake` declares no trace clock and telemetry there is refused at
  configure. **Clock/one-shot timer** are chip-provided (virt: CLINT
  `mtime`/`mtimecmp`; C6: SYSTIMER -- TODO(HW)).
- **PMP** -- a **permissive bootstrap entry** (pmpaddr0 NAPOT-all, pmpcfg0 = RWX,
  U-accessible) is set in `kickos_rv32_init`. RISC-V is fail-CLOSED: once PMP is
  implemented, a U-mode access with no matching entry FAULTS (unlike ARM, where
  unprivileged is unrestricted until the MPU clamps). So without this, an
  unprivileged thread can't fetch its first instruction. Per-task PMP enforcement
  landed at **M2**: `arch_mpu_apply` stashes the incoming set on switch-in and
  `kickos_arch_mpu_commit` programs the NAPOT PMP entries from the `.Lswitch` epilogue after the
  physical swap (the deferred-commit seam) (+ `arch_mpu_region_encodable` for the grant check) -- **enforced on qemu-riscv**; the
  ESP32-C6 image specifics (all-SRAM image, gp-relative small-data, code-from-RAM,
  and a separate APM/PMS bus permission unit) are still **blocked**, see
  `m2-readiness.md`. No F/D extension -> soft-float, so the switch banks no FP.
- **`gp` anchor for full-C++ under MPU** -- RISC-V small-data addresses globals as
  `gp + imm` from one `__global_pointer$`. For a full-C++ app under per-domain
  enforcement the anchor MUST sit **inside the app's granted data region**: the
  runtime's small globals (`eh_globals`, `_impure_ptr`, the FDE registry heads) and a
  `-fexceptions` TU's `gp`-relative EH references (`DW.ref.*`, LSDA datarel) all live
  in that window, so an out-of-region anchor faults an unprivileged throw. Contract:
  link `PROVIDE(__global_pointer$ = ...)` within the app-data block, and compile the
  KickOS libs `-msmall-data-limit=0` so they emit no small-data and vacate the window
  (else granting it would hand a U-thread the kernel's own scheduler small-data). App
  TUs stay compiled *with* small-data so unwinding works. Folds into the app-data grant
  at +0 regions. `switch.S`'s restore epilogue reloads `gp` from `__global_pointer$` under
  `.option norelax` on every dispatch: the anchor sits in a window U-mode can write, so a
  thread-set `gp` would otherwise corrupt the next thread's small-data addressing. ARM and RX
  have no small-data model and skip this.
- **`arch_irq_inject`** (fake-a-device-firing test/bench scaffolding) uses the
  **supervisor software interrupt** (`mip.SSIP`, `mcause`=1) as a private channel --
  the RISC-V analog of the host sim's `raise(SIGUSR1)`. The **PLIC has no
  software-generated interrupt** (unlike the Cortex-A GIC's SGIs; QEMU faithfully
  rejects a software pending-write), so a real device IRQ cannot be faked through
  it. Masking is a software bitmask (the sim's `irq_masked` twin); a raise on a
  masked line latches one-deep (redelivered at the next unmask). The bench's
  IRQ-entry-latency sample and the IRQ self-tests (`irq_thread_ctx` /
  `irq_as_event` / `irq_mask_coalesce`) run on `virt` this way.
  SSIP needs S-mode (present on the QEMU virt CPU); the C6 is M/U-only, so its
  inject routes to an interrupt-matrix "from-CPU" line at HW bring-up. A real
  device-interrupt *receive* path (a PLIC over `meip`) is a driver-era concern.

### The QEMU verification target (`board`: `qemu-riscv`, chip `virt`)

A runnable rv32imac target validates the arch on real emulated RISC-V:
`qemu-system-riscv32 -M virt -bios none -nographic -semihosting`. `-bios none`
runs our image directly in **machine mode** (no OpenSBI). The `virt` chip
(`arch/riscv/chip/virt_rv32/`) uses the standard CLINT (`mtime`/`mtimecmp` @ 10 MHz,
`msip`) + RISC-V **semihosting** for the console (`SYS_WRITEC`) and exit
(`SYS_EXIT_EXTENDED`) -- the mps2 model -- so it needs no UART; the image links to
run from DRAM at `0x8000_0000`. `ctest --preset qemu-riscv` boots `hello` and
asserts the ping-pong (reset -> scheduler(msip) -> ecall syscalls -> CLINT timer ->
semaphore reschedule). The C6 board (`esp32c6-wroom`, chip `esp32c6`, build-only) is flash-to-validate:
esptool image + real UART/SYSTIMER/watchdog/CLINT register values are the HW pass.

---

## Verification status

The arch backend **cross-compiles clean** for Cortex-M4/M3, and the **spike is
empirically validated on QEMU Cortex-M4**: `ctest --preset qemu` boots `hello`
and asserts the two userspace threads ping-pong -- exercising reset -> C-runtime ->
scheduler start (PendSV first switch) -> **SVC-trampoline syscalls from both a
privileged and an unprivileged thread** -> SysTick one-shot driving `sleep` ->
semaphore block/wake reschedule, all on a real Cortex-M4 core. The #1 M1 risk is
retired.

Since then the chip layer has been brought up on real hardware across the fleet -- `mk64f` (M1
baseline + SYSMPU enforcement + the first unprivileged MMIO drivers), **`rp2040` running on a
real Raspberry Pi Pico** (see the RP2040 section), and the rest of the table above. The
STM32 chips reuse the `mps2`/`mk64f`/`rp2040` patterns.

Hardware MPU enforcement is **done** (M2): the cross-domain trap is silicon-proven on SYSMPU,
PMSAv6-M/v7/v8, RISC-V PMP and the RX MPU. For a new port that means enforcement is part of the
seam you implement, not a later milestone -- `arch_mpu_apply` (stash at the switch decision),
`kickos_arch_mpu_commit` (program from the switch epilogue, after the physical swap),
`arch_mpu_region_encodable`, and `arch_reserved_blocks`, which has no fallback TU so omitting it
is a link error. See `architecture.md` (Memory domains) and `invariants.md`
(`mpu-apply-on-every-switch-in`, `grant-refuses-kernel-reserved-blocks`).
