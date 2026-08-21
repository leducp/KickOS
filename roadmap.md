<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS roadmap

The milestone-level plan: the general idea to tackle per milestone. **No granular items** --
those live in `TODO.md` (the actionable checklist); the design behind them lives in
`docs/reference/architecture.md`; validated end-state lives in `docs/archive/M1_state.md`.

**Milestones are keyed to THEME, not sequence.** A milestone names a *capability the kernel
gains*, not a date. Work that merely follows M1 is not "M2" unless it needs the MPU; orthogonal
work (perf, a real-peripheral-IRQ demux, a userspace driver) is **anytime coherence** and lands
whenever it is ready, tagged as such in `TODO.md`.

## Done

- **M0 -- x86 sim.** The real kernel + an unprivileged userspace app in one Linux process:
  tickless scheduler, semaphores, syscalls across the SVC boundary, `mprotect`-emulated MPU,
  IRQ-as-event, OS-agnostic `main`, the pooled-object pattern. Runs in CI. (Detail in git.)
- **M1 + M1.x -- the MCU fleet.** First silicon, then breadth: **10 boards across 5 ISAs**
  (armv7m, armv6m, RXv3, RV32IMAC, Xtensa LX6) up on hardware, privilege + SVC (no HW MPU yet),
  each with a console, tickless timer, fault dump, and inject-driven IRQ path; plus telemetry,
  the buffered console, and per-chip clock bring-up. Full record in `docs/archive/M1_state.md`.
- **M2 -- hardware MPU enforcement.** A cross-domain access faults on real silicon.
- **M3 -- capabilities & object model**, and user clock-select.
- **M4 -- the driver era.** M3 made real fleet-wide. The sub-milestone ledger below is the only
  place a number is assigned.
- **M5 -- the driver era completed, and everything for SMP that is not SMP.** Ten PRs, `M5.1.1`
  through `M5.1.10`, master `a41856d6`.

The `###` sections that follow carry the detail for each of the above. They stay because the
reasoning in them is the reference for how those subsystems work, not because anything is pending.

### M2 -- hardware MPU enforcement
Make per-task isolation real on silicon. **Status:** the enforcement mechanism has landed on
silicon across the reference set -- K64F SYSMPU, XMC PMSA, RX72M MPU, ESP32-C6 PMP -- each with
selftest under enforcement plus a cross-domain `mpu_fault` trap; the arch-independent floor
(memory domains, per-thread private stacks, pow2 region placement, confused-deputy out-pointer
copy-in) is in. Remaining tail (C6 peripheral APM open, the deferred syscall-buffer bounds) is tracked in `TODO.md` / `docs/m2-readiness.md`.
Two halves:
- **Mechanism, per chip** -- `arch_mpu_apply()` backends wired into the task-switch hook, one
  distinct mechanism class at a time (the discipline: prove `{base,size,attr}` is sufficient,
  never leak a per-arch field). Reference pair first: **RISC-V PMP/NAPOT** (traps in CI without
  silicon) + **XMC v7-M PMSA**; then **K64F SYSMPU** (byte-granular), **RX72M** (non-ARM), then
  the same-mechanism tail.
- **Security model, arch-independent** -- the memory-domain object (a shared region set) with
  **per-thread private stacks**, power-of-two region placement, and **syscall-argument /
  user-pointer validation** (the soundness floor). Design in `docs/reference/architecture.md`;
  readiness matrix in `docs/m2-readiness.md`.

### M3 -- capabilities & object model (and user clock-select)
The object/credential model on top of M2's enforcement:
- **Per-task typed handle table** (Zircon/seL4 shape) replacing global object ids -- rights bits,
  refcounted, destroy-on-last-close; **authenticated grant ownership** as its memory-side twin.
  **LANDED -- the first M3 capability:** the semaphore syscall ABI migrated from global object ids
  to a per-task `CapEntry` table with a single `cap_resolve` chokepoint, silicon-validated under
  enforcement on all four M2 mechanism classes, plus authenticated-grant spawn delegation
  (subset-only rights narrowing, deterministic B1 placement). Each item below carries its own
  status; two of the four have since landed with a design record each.
- **One blocking primitive**, not an object zoo -- a cap-named wait/wake object; richer sync
  built in userspace; the sole justified typed object is a priority-inheritance mutex.
- **Console *device* handover** -- a userspace UART driver takes the peripheral as a capability;
  the kernel relinquishes it and the panic path reclaims + re-inits it.
  **LANDED**, silicon-proven on XMC: `docs/design-m3-console-handover-stageii.md`. What remains is
  fleet coverage, not the mechanism; the chips that carry a per-chip `arch_console_reclaim` body
  are whatever `grep -rln '^void arch_console_reclaim(void)' arch/*/chip/` lists, now most of them.
- **Low-barrier hard constraint** -- a plain app never writes a capability manifest; the runtime
  wires a sane default cap set (never resurrect CapDL-to-boot friction).
- **User-selectable CPU clock / low-power mode.** **LANDED, both sides**: the read is
  `kos_cpu_clock_hz()` (`KOS_SYS_CPU_CLOCK_HZ`) and the write is `KOS_SYS_CPU_CLOCK_SET` over the
  `arch_cpu_clock_set` seam with its coherence tail -- `docs/design-m3-clock-select.md`. A governor,
  DVFS and any idle heuristic are deliberately NOT in it: the seam is mechanism, not policy.

### M4 -- the driver era (make M3 real, fleet-wide)
M3 proved the mechanisms (endpoints/IPC, console handover, panic reclaim, clock-select) but each
on ONE or TWO chips. The driver era reuses them and makes them REAL ACROSS THE FLEET, then grows
a driver framework on top. Single-core throughout. Full gap list + sequencing in
`docs/design-driver-era-scope.md`.
- **The objective: driver support is the VEHICLE that validates the service APIs against real
  hardware variation** -- prove the console/UART, gpio, pinmux, clock/power, and bus (SPI/I2C)
  service APIs are genuinely vendor-neutral, not accidentally shaped around one vendor. Full
  driver support is the means; a hardware-hardened, vendor-neutral service API is the end.
- **The API-neutrality matrix -- FOUR easy-to-flash boards, diverse across BOTH vendor AND
  arch/MPU family:** c6 (ESP32-C6, rv32imac / PMP / Espressif), xmc (XMC4800, armv7m / PMSAv7 /
  Infineon), k64f (FRDM-K64F, armv7m / SYSMPU / NXP), rx72m (RXv3 / RX-MPU / Renesas). Four
  vendors x four arch/MPU families is a far stronger neutrality test than adding another ARM
  board -- a bias baked around one vendor cannot survive all four.
- **Scope guard -- the ASPIRATION is FULL per-board peripheral coverage, PRIORITIZED not
  restricted.** Supporting *all* of a board's peripherals is how we discover hardware that needs
  a dedicated / NEW service API -- that discovery IS the point -- so coverage is NOT bounded
  a-priori to the service-defined classes (console/UART, gpio, pinmux, clock/power, SPI/I2C).
  The bound is a COMPLEXITY-vs-GAIN weighting per candidate (api-discovery value + real
  usefulness + cross-vendor coverage, against bring-up effort + spec depth + DMA/IRQ plumbing +
  the no-probe penalty on c6/rx72m), NOT a class restriction. That weighting -- the per-board
  backlog + the cross-board neutrality shortlist in `docs/design-m4-driver-matrix.md` -- is what
  keeps scope from ballooning (analog-in, PWM, the event fabric, the on-chip EtherCAT SC are
  exactly the high-gain non-class peripherals it surfaces). Still prefer MAPPING different
  drivers to different boards over duplicating one driver on all four.
- **Scope guard -- the no-probe constraint is a design INPUT:** rx72m and c6 are easy-flash but
  have NO usable debug probe, so bring-up there is print-debug only. The services must therefore
  be console-observable, or two of the four matrix boards cannot be brought up at all.
- **Fleet userspace UART / console drivers + per-chip `arch_console_reclaim` + handover
  validation** -- the userspace console drivers that exist are whatever
  `grep -rln KOS_SVC_CONSOLE system/driver/` lists: UART on xmc4800, mk64f, esp32c6, esp32, rx72m and
  stm32f411 (the polled pair plus the `*uartirq` set), and USB CDC on imxrt1062 and rp2xxx
  (the `select`-only lists `system/init/{picopi,pizero2350,teensy41}/service_list_usbcdc.cc`, which
  reach an image only under `-DKICKOS_SERVICE_LIST`). Every other board is kernel-owned, and so are
  those three in their default posture. The chips shipping a reclaim body are
  `grep -rln '^void arch_console_reclaim(void)' arch/*/chip/`, now most of the fleet (the fault-funnel
  porting invariant: no real reclaim => a driver-garbled UART silently eats the panic banner). The panic path now reclaims from ANY
  state rather than only after a handover, which widened that invariant rather than retiring it:
  every chip body must be idempotent absolute stores. A board can still lose its dump for some
  other reason, and **no emulated gate can catch that class, because every fault-dump gate in the
  fleet runs on an unbuffered console.** One driver per
  chip family, silicon-available first; isolation is real only where the MPU gates peripherals.
- **Clock-select fleet-wide** -- extend `arch_cpu_clock_set` per opt-in chip, or keep the declining
  fallback explicitly.
- **The enabling services** -- **init** (separate init from the app; spawn drivers-with-caps in
  dependency order; settle the entry-point rename EARLY), **clock-tree / power-manager**, **pinmux**
  (one-shot init-time config), **gpio** (a pin allocator that mints per-pin caps -- cold IPC to
  allocate, direct MMIO to toggle). Deep-dive prose under "## Later" below.
- **The driver framework** -- a call/reply (reply-cap) IPC layer on CAP_ENDPOINT for synchronous
  SPI/I2C drivers; the driver-lib / demo split; multi-instance = thread-per-instance. The
  class/service duality (driver-lib class as the primitive, service composed on top; the consumer
  picks the coupling and pays only for what it uses) is designed in `docs/design-m4-driver-model.md`.

#### The sub-milestone ledger -- THIS PARAGRAPH IS THE ONLY PLACE A NUMBER IS ASSIGNED

**No other file assigns a sub-milestone number.** Any document may cite one, but a document that
*defines* one is a bug: the numbers moved once already and the correction had to chase fourteen
references across four files. A design record should say "deferred", "fleet work" or "the wave after
this one" and let this ledger say which number that is. `../STATE.md` carries the locked ORDER of
what is next; this carries the numbering.

| number | subject | state |
| --- | --- | --- |
| M4.1 | the init + service seam | landed |
| M4.2 | the first driver proof (K64F console) | landed |
| M4.3 | foundational services: clock oracle, pinmux | landed |
| M4.4 | per-chip console drivers | landed |
| M4.5.x | unprivileged root, region encoding, the gates that fail, the comment purge | landed |
| M4.6.1 | the IRQ substrate and the buffered userspace UART | landed |
| M4.6.2 | the USB CDC console, partially witnessed on `pizero2350` | superseded by M4.9.1 |
| M4.7.1 | the capability-table rework: codec, storage, errno, sizing (`docs/design-capability-table.md`) | landed |
| M4.7.2 | the review findings against M4.7.1 | landed |
| M4.7.3 | per-task table width, and a per-task cap on inbound replies: the chunk directory earns its keep | landed |
| M4.7.4 | delete the legacy management: nothing is released before the ABI-freeze milestone, so there is none to carry | landed |
| M4.7.5 | Kconfig owns configuration; CMake keeps the build graph | landed |
| M4.7.6 | the language level moves to C++20, and the tree uses what it buys | landed |
| M4.7.7 | root is a pool thread: a kill tag of its own, a nameable root, call/reply from an app's own `main` | landed |
| M4.7.8 | the timed wait: an abortable/timed call, thread join, wait-until-last | landed |
| M4.7.9 | fault isolation: a thread that faults dies alone, `exit()` reaches the kernel on every port, diagnostics carry a short column | landed |
| M4.8.1 | the class layer the driver-model ruling requires, plus the one generic service over (class x chip) that replaced twelve bring-ups | merged, PR 19 |
| M4.8.2 | the host unit-test layer, and the `sched::wake()` dying-guard repair it is the tool to prove | merged, PR 20 |
| M4.8.3 | the task layer: a set of threads that is one unit, plus the fault record a published console swallowed | merged, PR 21 |
| M4.8.4 | close the 4.8.x tail: the wake-guard premise, the release ordering the narrowing left, rxv3's measured below-stack cost, and the three instruments that let them through | merged, PR 22 |
| M4.9.1 | the USB CDC console, continuing M4.6.2 | merged, PR 23 |
| M4.9.2 | the user substrate says what it means, and it grew past that line: a relaxed atomic wherever `volatile` stood in for one plus the house wrapper that carries the ordering as a type parameter, one definition per non-template body, the four gates `style.md` already claimed, the per-chip `arch_console_reclaim` and `arch_console_flush_sync` bodies on every chip that publishes (which closes G2), the i.MX RT1062 USB CDC backend (stage S6, root-caused to the AIPSTZ bridge rather than the MPU), `reclaimwit` as the board-agnostic reclaim and drain witness, a fault report that no longer dies silently during driver bring-up, and two armv7m fault-path repairs | merged, PR 24 |
| M4.9.3 | the instruments the M4.9.2 witness pass exposed: the `ctest -LE host` image-gate sweep that no tool ever ran, the pool-arena assert binding every board, a gate refusing an atomic read-modify-write, and `reclaimwit` registered so the reclaim seams are automated rather than hand-run | merged, PR 25 |

M4.9.3 closes the M4 wave. What was the unnumbered `M4.9.x..N` tail is now **M5**, a milestone of
its own rather than a sub-milestone, and it carries the rest of the driver era plus the single-core
groundwork the SMP milestone needs. **M5 has no ledger table here yet**, so its sub-milestone
numbers currently live only in the merge history, which is exactly the situation the paragraph
above calls a bug. Writing that table is owed.

**M4.7.x is kernel-core work carrying an M4 number on purpose.** The banner and package versions are
`0.<milestone>.<submilestone>` and must stay monotonic, so a capability rework cannot be numbered
back into M3 even though capabilities are M3's theme. It lands BEFORE the rest of the driver era
because the capability table is the heart of userspace, and it gates SMP: three assumptions in that
subsystem are single-core.

**The three M4.7 numbers are one arc, and M4.7.3 is what the other two were for.** M4.7.1 fixes
the handle codec, the errno and the storage, and sizes the table at configure from declared demand
-- but under **one fleet-wide width**, which is the assumption M4.7.3 removed: root keeps the
summed width, and every spawned child gets `KICKOS_CAP_CHILD_WIDTH`. At one width the chunk
directory M4.7.1 introduces was provably inert: `CapChunkList::take` is all-or-nothing, so the
chunk free list was isomorphic to a free list of whole runs and segmentation could never succeed
where a contiguous run would have failed. It landed anyway, deliberately, because per-task width is
the only thing that makes it load-bearing, and a narrow child run is now what it carves. The
alternative -- flat runs then, chunks again here -- would have churned the layout and burnt a bench
pass to save forty lines. The cost of carrying it meanwhile was 440 to 936 bytes on the mid-range
boards and nothing at all on the 16 KiB parts, which take the flat path at
`KICKOS_MAX_HANDLES <= KCAP_CHUNK_TARGET` and where every run is still the full width.

**Inbound replies got a per-task CAP, and not the reserved sub-range this entry planned.** A client
mints a reply capability into the SERVER's table through plain `cap_install`, so inbound reply caps
and the server's own creates draw on one free list: the configure-time sum is therefore not a bound
on when a task's own mint can fail, and the coupling runs between PEERS, since one client's
`kos_call` can be refused because three others are mid-call. M4.7.2 answered the provisioning half
only, with a fourth declared term whose default is 0 -- the three supply-7 boards sit at demand ==
floor == supply, so a nonzero fleet default would stop them configuring at all. Partitioning the run
was the other half, and its price is what ruled it out: a second free-list head, which `cap.h`
records as unavailable ("Thread has no spare bytes for a second field" is why the list is circular),
or an O(width) scan on the `kos_call` fastpath. What shipped instead is `KICKOS_CAP_REPLY_MAX`, a
bound on LIVE inbound reply caps per task, sized from the number M4.7.2's term supplies and probed
before the fastpath pops a receiver (`cap_can_take_reply`: a free dynamic slot AND below the bound).
A count is all it needs, so the scan it does pay is the cheap one: `cap_reply_live` walks the run on
the flat path, bounded by `KCAP_CHUNK_TARGET` and not by the codec's ceiling, and reads a stored
counter on the segmented one, where the chunk directory's tail padding gives the field away free.
The bound is ONE-WAY because the reply term is charged to `KICKOS_CAP_CHILD_WIDTH` as well as to
root's summed width: reply traffic can never crowd out a task's own creates, and
`cmake/cap_table.cmake` refuses a configure where a default-width child would keep no slot of its
own once the bound is spent.

**M4.7.4 exists because compatibility work keeps appearing on its own.** KickOS is not released and
will not be before the ABI-FREEZE MILESTONE, the last one on this roadmap, so **there is no legacy
to manage** and every mechanism that manages some is
pure cost: it has to be kept in sync, it is read as a supported path, and it makes a deleted thing
look alive. The class, with the instances found so far:

- **tombstones for deleted knobs.** A guard whose only job is to refuse something that no longer
  exists. `KICKOS_MAX_HANDLES` had one, and M4.7.2 caught itself UPDATING its message text to match a
  new term, which is the whole failure in one line. Deleted; the sweep looks for the rest.
- **fallbacks that only fire when the build is already wrong.** `kernel/include/kickos/config/system.h`
  defaults `KICKOS_MAX_HANDLES` and then asserts against its own default, because two CMake probes
  read the header before the value exists. That is a workaround for configuration living in two
  places, and the generated header in M4.7.3 removes the need for it rather than tuning it.
- **inert grants and parameters kept "for signature parity"** with a path that no longer needs them.
- **doc and comment text that documents a removed mechanism as if a reader might still meet it.**

The rule for the pass: if the only reason a thing exists is that something else USED to exist, delete
it. Keep a guard only when it catches a mistake somebody can still make today.

**Why per-task width is not YAGNI.** One fleet-wide width means every thread is provisioned for the
fattest thread in the image. At the ceiling the codec is cut for -- one task holding 60000
capabilities against 64 threads -- that is ~30 MiB where the real demand is ~530 KiB, a factor of
~58. An operating system is the ceiling its applications work under, not the application: the width
law is the one part of M4.7.1 that does not survive contact with the top of its own declared range.

**The driver wave has been renumbered TWICE, and frozen records keep the number of their decision
date.** Documents written before 2026-08-03 use `M4.7` for it; documents written between then and
2026-08-07 use `M4.8.x`; it is now `M4.9.x`. Both moves happened for the same reason, and the
repetition is the signal rather than the chore: kernel-core work keeps turning out to be the
prerequisite for the driver work, so it takes the nearer numbers and the drivers move out. A third
move should be read as evidence about the sequencing, not as bookkeeping.

The roadmap is a DIRECTION, not a contract: renumbering to match what the work turned out to be
beats forcing work into a number it does not fit. Only the banner and package versions may never
go backward.

**A `..N` row is a count nobody knows yet, and it is always the LAST row of a wave.** A witness pass
OPENS items as well as closing them, so the tail keeps the suffix until the pass has run; the numbers
it earns are then assigned here, and the tail row moves up to the next free number.

### M4.7.5 -- configuration mechanism: Kconfig owns configuration, CMake keeps the build graph
**NUMBER ASSIGNED 2026-08-05, and the design questions below are now answered rather than open.** The
spike is deliberately outside master history, so this entry is the tracked record of what was decided.

**What is settled.** Kconfig is the single declaration language for every knob and emits a generated
header; CMake keeps the build graph; logic too complex for CMake moves to Python, which the tree
already requires. **One kernel configure/build, then link N apps** -- Zephyr's per-app kernel was
measured and rejected at 12 to 19 times the rebuild cost to buy 640 B on a 128 KiB part and nothing
at all on the 16 KiB one. Declarations are prefix-free and the emitted prefix is `CONFIG_`, as in
every Kconfig project. A disabled boolean is **absent**, tested with `#ifdef`, following Linux:
the 0 case is not handled, because the knobs come from Kconfig and the setting side is validated
there. Python emits a small CMake fragment that CMake `include()`s, rather than CMake parsing
`.config` itself: NuttX and Zephyr parse, esp-idf generates, and generating keeps string work out of
the language that reads worst. **Devicetree stays refused, and for its own reason** -- it is hardware
description, and that is the only question board count was ever the right criterion for.

**The acceptance test the milestone is judged against:** adding a knob, a board, a service or a
driver must be a DECLARATION, never a mechanism change. The corollary that makes it checkable is
that a CMake function may know its arguments and the shape of Kconfig's output, but never which
boards, chips or knobs exist -- not by naming them, not by globbing for them, and not by
reimplementing a tool that already knows. Nine of today's 26 CMake functions pass, eleven are
deleted outright, four are ported, and two fail for a different reason and survive anyway.

**Two orderings bind.** The dead `kos_service_cfg` fields must be deleted in M4.7.4, before a
generator exists that would emit them; and the per-board ladders must move into Kconfig before the
driver wave, not after, or the wave writes roughly 564 hand-maintained artifacts that the scheme then
deletes. `kconfiglib` is ISC, one pure-Python file, build-time only, and never in a shipped artefact.

**This revisits a decision that had no home in this repo.** A pre-M4 spike settled on "a
consolidated per-board descriptor, NOT devicetree/Kconfig", judged as overkill below roughly 30 to 40
boards; the tree is at 20. That verdict was recorded only in a developer's local notes, which is why
this entry exists at all.

**It also bundled two questions and judged both on board count.** Board count is the right criterion
for devicetree, which is hardware description. It is close to irrelevant for Kconfig, which is knob
management, validation and dependency expression. The pressure was not board count: configuration
was split across C headers and CMake with leakage in both directions. That cost two `cc -E -P`
probes reading headers back into CMake (`cmake/cap_table.cmake`, `cmake/boot_arena.cmake`), a
`file(STRINGS)` scrape of a `static constexpr` that no preprocessor can hand over, and a
hand-rolled C++ function-body parser in CMake regex (`_kickos_seam_int_in_file` in
`cmake/boot_arena.cmake`), which also reimplements the linker's archive-member selection rule.
M4.7.3's generated header removed two more of the same class: the directory-tree walk that carried
the width to subdirectories, and the `KICKOS_MAX_HANDLES` fallback that existed only so a
misconfigured build still preprocessed.

**Both `cc -E -P` probes and the `constexpr` scrape are now GONE, and the rule they leave behind
is about direction.** The provisioning integers reach CMake from the generated fragment, which is
the same resolution the compile reads. The structural constants the width is summed from went the
other way: they are declared in `cmake/cap_geometry.cmake` and emitted to C through the generated
`config/cap_width.h`, because a value the BUILD must read cannot be owned by C without a probe to
read it back. They are not configuration and get no Kconfig symbol -- nothing selects one, and no
defconfig can state one. What survives is the function-body parser, which reads a C++ RETURN
LITERAL rather than a macro and is a different problem.

**Kconfig would be additive, not a replacement**: it owns the knobs and emits a generated header,
while CMake keeps the build graph.

**The deciding question was never Kconfig, and the answer is neither option the spike started with.**
The capability-table width was a maximum over APP TARGET PROPERTIES plus the service list's. Those are
build-graph facts, and Kconfig is one-pass and static with no way to say "the widest declaration among
the app targets in this build". The two candidates were one-app-per-build, which removes the problem
but breaks whole-fleet-in-one-configure, and a hybrid that keeps the summing in CMake. **The design's
answer is that the summing is DELETED rather than relocated**: with one kernel build and N apps, the
maximum has nothing to range over, because the apps do not exist when the kernel is configured. The
width becomes an ordinary provisioning integer -- an `int` with a `range`, stated in the defconfig
like any pool size -- and what the summing used to guarantee is replaced by an app-side
`static_assert` against the installed generated header plus the runtime refusal that already exists.

**That deletion has NOT happened, and what landed keeps the summing in CMake deliberately.** Its
terms are target properties -- the widest app `CAPABILITIES` and the service list's `RETAINED_CAPS`
-- so the sum is build-graph arithmetic over numbers CMake already holds, which is what CMake is
for. What made it a hazard was never the arithmetic: it was that its INPUTS were read back out of C
through a preprocessor probe, and that is what is gone. Deleting the sum outright is a separate
change with its own consequences -- every board would state a width it cannot compute, and the
optional-demand grant would go with it -- and it is not required to close the backflow.

**Half of it has already landed.** The generated header was worth doing inside M4.7.3 on its own
merits, since per-task width adds a width and a class id per task and every workaround above would
otherwise have been ported onto a wider set of computed outputs. It shipped there, taking the
directory-tree walk and the fallback with it, so Kconfig now lands on a clean seam and is largely
deletion. The build-time `kconfiglib` host dependency is NOT the project's first: `arch/CMakeLists.txt`
already requires `Python3` for the RP2040/RP2350 second-stage checksum, in CI as well as locally.

### M4.7.6 -- C++20, and the features that pay for it

The tree pins `cxx_std_17`, which is why twelve aggregate initialisers carry `/*field=*/`
comment labels: a label can sit beside the wrong field and still compile, and the language
had no way to say it. Designated initializers are C99 in C and C++20 in C++, so a C
consumer of `kos_service_cfg` can already write `.name =` today while the in-tree C++ that
defines those same structures cannot.

**No compiler in the fleet blocks it.** RX GNURX 14.2 is the oldest, then ARM 15, the host
15, RISC-V and Xtensa 16.1; C++20 was feature-complete well before 14. RX is nonetheless
the one to build FIRST: it is a vendor fork and the only family with no CI, so a break
there surfaces on the bench rather than in a pull request.

**What earns the bump, in order of what it buys this kernel:**
- **`constinit`** asserts an object is constant-initialised: no runtime static initialiser,
  no guard variable. The tree already gates on this at link time (`kernel_ctor_placement`
  proves no kernel ctor leaked into the app-ctor window); `constinit` makes it a per-object
  compile-time property instead of an archaeology check.
- **designated initializers** delete twelve files of comment labels that can lie.
- **`<bit>`**, and it pays far less than it looks. Measured on all five toolchains:
  `countl_zero` lowers to a count-leading-zeros instruction on ARM and Xtensa only, while
  rv32imac (no Zbb) and RX emit an out-of-line libgcc call, so it is a pessimization there.
  The textbook site, the `bit_ceil` loop in `arch_ram_region_size`, sits in an INSTALLED
  header and is blocked by the C++17 interface rule. What survives measurement is two
  sites: `countr_zero` in the PMSAv7 RASR encoder, which is byte-identical, drops a cast
  and is defined at zero where `__builtin_ctz(0)` is undefined, and `has_single_bit` in
  the PMP NAPOT gate, which saves four bytes. Four other candidates were REFUSED because
  the hand-rolled form lets the compiler fuse the pow2 test with the alignment test that
  follows it.
- **`[[no_unique_address]]`** shrinks a struct only where a member's type is EMPTY, and a
  sweep of every class in `kernel/`, `arch/`, `lib/`, `system/` and `user/` found none: the
  tree has no stateless policy, comparator or tag idiom, and every type carries a register
  window, a shared-memory pointer or real bookkeeping. Nothing to apply it to.

**What it costs, and how it is measured.** Images move: header and inlining differences are
expected, so this milestone measures the delta with the 50-preset instrument and states it,
rather than claiming equivalence. `char8_t` is a real breaking change for any `u8""`
literal, and rewritten comparisons can shift overload resolution where a class defines
`operator==`/`!=` by hand; both are greppable before the flag is flipped.

**C++23 and C++26 are deliberately NOT this milestone.** GCC 14's C++23 is incomplete, and
the RX fork is where that would be felt. Of C++23 only `[[assume]]` and `std::unreachable`
would be used here and both are already expressible; `std::expected` cannot cross a C ABI
syscall boundary. C++26's contracts, `std::inplace_vector` and above all reflection are the
ones that would change how this kernel is written -- reflection is what "generate the DATA,
not the TU" wants to be -- but none is in a released cross compiler. Revisit when the RX
toolchain moves.

### M5 -- the driver era completed, and everything for SMP that is not SMP
Two halves that share one milestone because neither is worth a bench pass alone. The first
finishes what M4 started: the remaining drivers, and **KickCAT as the reality check** -- it has
been deferred through the whole driver era, and porting it to the current driver APIs is what
judges them. If it asks for an API change, that is this milestone's most valuable output; the
answer is to change the API, not to bend KickCAT. `KICKOS_MULTI_INSTANCE` belongs here and is a
REQUIREMENT rather than cleanup: the sim instantiating tens of KickOS+KickCAT slaves in one
process, against a KickCAT emulator, is how the driver APIs get reality-checked at scale, which
makes it a prerequisite for the KickCAT work rather than a sibling of it.

The second half is single-core work that pays off under any M6 outcome and needs none of M6's
decisions:
- **Bound the IPC critical section** -- a seL4-style WORK fastpath beside today's RENDEZVOUS one.
  A wall of refusals leaving a capability lookup, a queue pop and register moves, with no memory
  copy. A register-payload call shape is the KickOS equivalent, since with one physical address
  space and MPU isolation the copy IS the protection-boundary crossing and registers are the only
  free channel; it is an ABI ADDITION, so it is free until the freeze. Correctness lives in the
  slowpath and the fastpath REFUSES anything it cannot do: a fastpath handling a case the slowpath
  does not is two paths with two maintenance costs.
- **`arch_cpu_id()` folding to a literal 0 by PREPROCESSOR**, not a runtime branch and not an
  inline the optimiser must prove away. It is what delivers the byte-identical single-core build
  M6 demands, and both reference kernels do it this way.
- **The atomics get their real ORDER.** `Order` has one enumerator; ACQUIRE and RELEASE join it and
  are spent at the three residues `../STATE.md` and `design-m6-smp.md` name.
- **`struct Kernel` annotated per-core versus genuinely global**, on paper. Cheap, because the
  struct is the complete inventory and `kernel()` the single accessor; `cap_slab` sits outside it.
- **The latent uniprocessor bugs from `docs/design-capability-table.md` section 8** that are worth
  fixing on their own merits: the claim-then-commit window, the non-atomic `uint8_t` refcounts, and
  the probe/install TOCTOU whose assert becomes a hang in release.

Explicitly NOT here: a second core, cross-core anything, or MMU work. The
AMP-versus-shared-kernel question was left open here and closes in M7, per class.

## Next

> **Sequencing, decided 2026-08-21: FINISH THE KERNEL BEFORE WIDENING SUPPORT.** The order is
> escalation/TLS, then the MMU, then multicore, then IPC/IRQ optimisation, and the driver era LAST.
> The reasoning is that the driver era improves *support* while everything above it changes the
> *foundation*, and a foundation reworked under shipped drivers costs the drivers twice.
>
> Two consequences worth stating, because both reverse an earlier plan. **The MMU now precedes
> multicore**: a first A-profile port already brings exception levels, VMSA, a GIC and a new boot
> path, and a first SMP port brings secondary bring-up, IPIs, per-CPU data and TLB shootdown.
> Doing both at once is how the switch path stays undebuggable, and both reference kernels come up
> unicore on A-class first. **Optimisation now follows multicore** rather than preceding it,
> because a fastpath tuned before the exclusion contract exists is a fastpath shaped for one core
> and then reshaped for the lock.

### M5.2.1 -- trusted execution context, and the TLS that depends on it
**One milestone, one review**: this absorbs what was scheduled as M5.2.2 and M5.2.3, because the
second half deletes machinery the first half would otherwise have shipped and had reviewed.

Privileged code must never run on memory a thread can choose. Per-thread kernel stacks from
kernel-owned RAM, trusted entry and dispatch on every trapping ISA, blocking that keeps its
continuation on the kernel stack, and then **DELETE** the measured red zones and the panic-tail
exclusions the current scheme needs. The trap stack is indexed per core from the start, folding to
`[0]` at one core, so multicore is a substitution rather than a four-backend change. On top of that
foundation: per-thread newlib `_reent` and `errno`, C++ exception globals, thread identity, a
recursive malloc owner lock, and a kernel-mediated heap break, which is what finally closes
`heap_bump`.

**Two decision gates, or the sizing is a guess.** Measure kernel-stack high-water on the deepest
syscall path per arch BEFORE committing sizes, using the same `-fcallgraph-info` instrument that
sizes a kernel array instead of policing a user one. Then decide PER BOARD: 16 threads times a
kernel stack does not fit microbit's 16 KiB or bluepill-c8's 20 KiB, so that board lowers its
thread ceiling or takes continuation-style blocking. **Never fall back to privileged execution on
user stacks.** microbit is already at the arena cliff (`_ebss` IS `__kickos_ram_start`), and
`bluepill-c8-st`/`f302nucleo-st` sit near 3 percent flash slack while trusted entry adds text.

### M6 -- the MMU: a unicore A53 on QEMU `virt`
The memory model today is **one physical address space + per-thread MPU regions**. A real **MMU
(VMSA / page tables)** adds virtual address spaces: foundational, not a port. The **Domain seam** is
shaped to absorb it (a domain becomes a page-table root instead of an MPU region set).

Unicore FIRST, and the SMP seams are cut here while they still compile to nothing at one core:
EL1/EL0 and exception vectors, identity map and then `arch_aspace_activate` (TTBR0 + ASID), GICv2/v3
for the UART and the timer PPI with the table keyed `(line, kind)` rather than a flat NVIC index,
the Generic Timer as the tickless one-shot, `arch_ipi_send`/`arch_ipi_wait` as empty macros, a
per-CPU struct reached through `TPIDR_EL1`, an `arch_aspace_flush` that is a local TLBI, and an
`arch_dcache_clean`/`invalidate` seam for DMA. Two decisions this milestone must FREEZE rather than
defer: **high-half versus fully separate address spaces** (it drives `kaccess_from_user` and whether
a kernel pointer survives `arch_aspace_activate`), and that **one lock spans capability
resolve-to-use**. Do not let the identity map become the allocator: prove boot, trap, switch and
UART on a 1:1 map, then cut `arch_aspace_*` before any second core.

### M7 -- multicore: SMP on the A53 port, and AMP where the cores are heterogeneous
The AMP-versus-shared-kernel question closes **per class**, not as one kernel-wide verdict. A
homogeneous A53 cluster shares one kernel image; heterogeneous companions (i.MX8MP's Cortex-M7, the
ESP32-C6 LP core) stay AMP over a ring, and homogeneous MCU dual-core parts are an AMP candidate
rather than a shared-kernel one.

`IrqLock` ("interrupts off => exclusive") is single-core-only, so the rework is a **Big Kernel Lock**
first (local-IRQ-off + one spinlock, byte-identical on single-core builds), then per-core run queues
and finer locks only where real atomics exist. Cross-core object reclamation and TLB shootdown use a
blocking IPI rendezvous over the same transport, so the doorbell cannot be fire-and-forget only.
**Do not judge this milestone by its speedup**: on today's measured 53 percent `IrqLock` hold, Amdahl
caps two cores at 1.31x and four at 1.55x, and the hold-shortening that moves those numbers is M8.
Re-derive both after M8 rather than freezing a verdict here. Spikes: `docs/design-m6-smp.md`
(candidate ranking, cross-core IPC invariants), `docs/design-m6-state-inventory.md` (per-core versus
global), `docs/design-capability-table.md` section 8.

### M8 -- IPC and IRQ optimisation, on measured evidence
Rebaseline FIRST: per-thread kernel stacks and the MMU both change trap and continuation costs, so
no percentage measured before them is a planning input. Then, in order of structural value rather
than micro-cost: an end-to-end instrument (physical IRQ assertion through first userspace MMIO, plus
the outermost lock-hold distribution, reporting p50/p99/max and not minima); `kos_reply_recv` fusing
reply and receive under one kernel entry; donation to the already-blocked receiver on the rendezvous
fastpath; a sticky IRQ notification with overflow accounting instead of a saturating counting
semaphore; binding an IRQ notification to the endpoint receive wait so ONE driver thread waits on
both; and edge-only persistent arming under a kernel-held storm budget.

**Protection is not assumed cheap here.** `MPU_APPLY` measures 443 cycles per switch on
`esp32c6-wroom` and both switches are inside the lock, which is 886 of a 3651-cycle locked round
trip; `docs/design-m5-ipc-fastpath.md` section 3.0.4 prices its removal at `f = 0.457`, **1.37x**.
Bulk transfer is a SEPARATE object from the endpoint, and its wire format is
**`{region-cap, offset, len}`, never a raw address** -- the M4 review's finding 10, which stays
one paragraph only until the first large-transfer path lands.

### M9 -- back to the driver era
Remaining drivers and breadth, plus the SPI class work of `deferred-after-pr-train.md` -- the
validation hoist and the nine divergences. This sits after the foundation on purpose: it improves
support on a base that is no longer moving under it.

### M10 -- KickCAT as the reality check
**After** the driver era, not inside it. KickCAT has been deferred through the whole driver era, and
porting it to the driver APIs as they then stand is what judges them: if it asks for an API change,
that is the most valuable output, and the answer is to change the API rather than bend KickCAT.
It runs last because it judges a finished surface; judging one still being widened tests nothing.

## Later
Multi-domain isolation + cross-domain shared-memory IPC; message-passing IPC + userspace drivers;
**service publication** (naming/discovery, capability delegation, badged endpoints, an interface
convention); runloops + multi-object waiting; timed wait (`sem_timedwait`) as one unified wait
primitive; introspection; a HAL/driver model; pluggable EDF / rate-monotonic policies; loadable
MPU-isolated user modules; POSIX / CMSIS-RTOS2 compat; TLSF heap; RP2040 AMP; Renode CI; and
**the Book** as the durable how-&-why reference (see `docs/book/`).

### RISC-V context-switch cost -- optimization (post-MMU, not scheduled)
The rv32 trap-based switch software-saves the full integer file (~60 stack words/switch vs
armv7m's ~18 with hardware exception stacking) -- a ~3.5x per-handoff cost, general to RISC-V
(not C6-specific; Hazard3 shares it). Two levers, both fable-gated: (a) a **cooperative
fast-path** (voluntary switches save only callee-saved regs, ~2x, portable to every rv32 target
incl. the C6), and (b) an **optional Zcmp `cm.push`/`cm.pop`** compile-gated path (Hazard3-only;
compresses instruction count, not memory traffic; single-digit % on top of (a), mostly code
size). Full analysis, the compile-gate design, and the prerequisite bench-bracket fix in
`docs/design-riscv-switch-cost.md`.

### ARMv8-M TrustZone kernel-confinement backend (post-MMU, opt-in, per-chip)
The armv8-M-with-Security-Extension MECHANISM for kernel confinement: kernel/TCB in Secure state,
apps in Non-secure. Not a per-task isolation mechanism and NOT an MPU replacement -- NS tasks are
still isolated from each other by MPU_NS at the same per-switch cost. The framing that makes it fit:
kernel confinement is already arch-dependent (PMSAv7 background-drop, PMP locked entries, RX-MPU,
SYSMPU RGD0); uniformity lives in the GOAL (confined TCB + per-task isolation + capability
authority), not the register mechanism. So TrustZone is simply the strongest armv8-M realization of
the parked "confine the kernel / drop PRIVDEFENA" goal (Option B), layered on top of Option B rather
than replacing it; chips without the extension use Option B alone. Buys a hardware TCB boundary
(NS-privileged still cannot touch Secure memory) + a PSA-style secure-services partition that fits
the capability-gated-services model. A security/assurance play, not a performance one. Post-MMU
(needs the driver-era service model and SMP settled, since the MPUs and the SAU are banked per core);
per-chip capability (M23/M33/M55/M85 MAY have it, detect + fall back); RP2350's M33 is a concrete
target. Detail in `TODO.md` under the post-MMU optimizations.

### Userspace init service (driver-era; not hardware-gated -- anytime-coherence)
Today the user's `main` doubles as pid-1: it IS the init entry, holds full userspace
rights, and spawns every task -- so (1) the app's `main` is really the SYSTEM init wearing
the app's name, and (2) that init pattern (create endpoint, publish console, spawn the
driver with caps, close the parent cap, spawn apps -- exactly the M3 handover choreography)
gets re-hand-rolled by every root task. Idea: rename the entry (`kos_init_entry` /
`kos_init_userspace`) to separate init from the app, and ship a DEFAULT init service that
does configurable bring-up then calls the real user `main` with a configurable capability
set. A power user links their OWN init service instead. Constraints: keep the LOW-BARRIER
zero-config default (a plain app still writes no manifest -- the default init wires the sane
cap set; never reintroduce CapDL-to-boot friction), and the entry RENAME is a consumer-facing
breaking change -- settle the entry-point seam EARLY (a cheap-now-vs-break-later quick-win)
rather than after consumers bake in `main`. Formalizes the implicit root task. Its NATURAL
home is the **driver era** -- spawning drivers-with-caps + a proper driver API is what turns
"KickOS runs on one board" into "any app builds on KickOS," and the init service is a gating
enabler for that. It is when real user apps can actually land: today KickCAT is the only
consumer and it is a POC (one board, a driver more demo than proper API), not evidence the
real-app story exists yet. Not gated by any hardware capability; its home is the
driver-era workstream (now **M4**), not a later hardware-gated milestone.

### Userspace power-manager service (driver-era; mechanism/policy split)
The M3 clock-select syscall (`arch_cpu_clock_set`) is deliberately a MECHANISM seam: change
the CPU/bus clock COHERENTLY (re-anchor the monotonic clock, re-derive baud, re-arm timers)
and return the landed Hz. POLICY -- which P-state when, DVFS, idle/low-power governors --
belongs in userspace, in a dedicated **power-manager driver/service**, exactly as the console
DEVICE moved to a userspace UART driver. Like the console, the privileged steps a userspace
driver cannot safely touch (flash wait states, voltage/regulator scaling, PLL relock) stay
kernel-side behind the seam; the power manager drives policy through it. Pairs with the driver
era + the init service; the M3 seam is the stepping stone, not the final home.

**The fuller vision -- a clock-tree service.** The service is really the OWNER of the whole
clock TREE: the PLL, dividers/muxes, and the tree-level clock gates (which live in the shared
SCU/RCC/SIM block, NOT per-peripheral windows, so they are refcounted CENTRALLY -- a branch
feeding two peripherals gates off only when both are idle; per-driver gating covers only a
peripheral's LOCAL enable). Because peripheral clocks are DERIVED from the shared PLL, a rate
change CASCADES: every derived-clock consumer must re-derive (a UART re-derives baud, an SPI
its prescaler) -- a rate-change-notifier fan-out (Linux Common-Clock-Framework shape). The
kernel is itself such a consumer (its monotonic clock + timer), so it can never fully leave:
the irreducible KERNEL RESIDUE is (a) re-anchor its own clock atomically on a rate change, and
(b) gate the safety-critical privileged steps (flash wait-states, voltage/regulator, PLL
relock) so a service BUG is wrong policy (restartable), not a flash-controller hard-fault.
Authority is a delegatable clock-control CAPABILITY (the service holds it like a driver holds
an MMIO grant), not full privilege. This is the console-handover pattern applied to the clock:
machinery -> userspace service, kernel keeps only the re-anchor + privileged-step residue.

### Platform targets past the A53 (captured, not scheduled)
The MMU itself is **M6** and multicore is **M7**; what stays here is the hardware these two unlock,
each wanting a feasibility spike rather than a slot:
- **x86_64 PC target** -- KickOS as an actual OS on a PC (QEMU first, then bare metal): paging plus
  a different boot, privilege and interrupt model (long mode, ring0/3, APIC). This is where
  `__KickOS__` earns its name. The MMU spike originally aimed its unicore stepping stone here; M6
  aims it at QEMU `virt` A53 instead, because that is the machine multicore then runs on.
- **i.MX8MP -- heterogeneous AMP across profiles** -- an **MMU KickOS on the Cortex-A53(s)** (VMSA)
  beside an **MPU KickOS on the Cortex-M7**, one per core cluster, over cross-core IPC. This is M7's
  AMP contract carried from homogeneous to heterogeneous cores, and it is the case that needs the
  cache-maintenance seam: the A53-to-M7 window is not coherent.
