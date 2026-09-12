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

**THE ABI-FREEZE MILESTONE CARRIES NO NUMBER, AND THAT IS THE RULING RATHER THAN AN OMISSION.** It
fires when the ABI is ready to freeze, which is a state and not a position in a sequence: a number
would assert a readiness nobody has yet. It is the last milestone on this roadmap and it is named,
never numbered. No other document may assign it one -- four design documents and `TODO.md` had
settled on "M8, the last one", which by 2026-08-31 was wrong twice over, M8 being IPC/IRQ
optimisation and the list running to M11 then, and to M12 now.

**M4.7.4 exists because compatibility work keeps appearing on its own.** KickOS is not released and
will not be before that milestone, so **there is no legacy to manage** and every mechanism that
manages some is
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
boards; the tree is at 23 (2026-08-29, `ls boards/`). That verdict was recorded only in a
developer's local notes, which is why this entry exists at all.

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
terms are target properties -- the widest app `KICKOS_CAP_PEAK` and the service list's `RETAINED_CAPS`
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
expected, so this milestone measures the delta with the fleet image-gate sweep, which takes every
visible configure preset, and states it,
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
  are spent at the three residues `../STATE.md` and `design-m7-smp.md` name.
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
thread ceiling or takes continuation-style blocking. **A THIRD OPTION IS WHAT ACTUALLY SHIPPED, so
read this pair as superseded**: armv7m carries BOTH entry designs under `KICKOS_KERNEL_STACKS`,
gated on the chip capability `HAS_MPU`, and the boards that cannot afford blocks keep their red
zone and gain usable stack rather than losing threads. **Never fall back to privileged execution on
user stacks.** microbit is already at the arena cliff (`_ebss` IS `__kickos_ram_start`), and
`bluepill-c8-st`/`f302nucleo-st` sit near 3 percent flash slack while trusted entry adds text.

### M6 -- the MMU: a unicore A53 on QEMU `virt`
The memory model this milestone STARTED from is **one physical address space + per-thread MPU
regions**, which is still what every region board does. A real **MMU
(VMSA / page tables)** adds virtual address spaces: foundational, not a port. The **Domain seam** is
shaped to absorb it (a domain becomes a page-table root instead of an MPU region set).

**And it ships TWO backends, not one, because that is the only way the seam can be shown not to be
one backend's shape with a header around it.** `arch_mpu_region` is frozen today because five
backends and the RX72M litmus earned it that status; an `arch_aspace_*` family designed against a
single A-profile core would carry A64 assumptions that nothing in the tree could detect. The second
backend is **x86_64 on QEMU**, chosen because it is maximally different where it matters -- one root
register instead of two, so activating an address space replaces the kernel's mapping as well;
feature-gated 12-bit address-space identifiers instead of 16-bit ones always present; and a syscall
entry that does NOT switch stacks, where A64 hands the kernel a separate stack pointer in hardware.
It costs no silicon.

**THREE backends, and the second is RV64 rather than x86_64.** A64 and x86_64 agree on 64-bit
addresses, a hardware table walker, a 4 KiB granule, a generous high half and byte order, differing on
only two axes, so an empty signature diff across that pair reads as proof of SIMILARITY. The litmus is
**RV64 with Sv39**: its physical address space is wider than its virtual one, which breaks a
full-direct-map assumption on a mainstream part; its paging mode and level count are selectable on one
chip; the identifier length may be hardwired to zero; and it is buyable, the C906 in the Allwinner D1
being a unicore Sv39 part, which is the RX72M property that made that litmus credible. x86_64 stays,
falsifying the ENTRY AND BOOT paths instead -- a syscall entry that loads no stack pointer, and
adopting a translation regime firmware already turned on.

**They go in order and never concurrently.** The reason the MMU precedes multicore is that two firsts
in one bite makes the switch path undebuggable; three at once is worse. Sub-milestones, and each is a PR:
**M6.1** the A53 port on a 1:1 map, proving boot, trap, switch and UART with no allocator work --
**merged, PR 37**, master `3681624f`;
**M6.2** translation, processes and enforcement; **M6.3** the RV64 Sv39 backend and the aspace-seam
verdict; **M6.4** the x86_64 backend and the entry-path verdict; **M6.5** frame-level capabilities,
designed against three backends instead of one.

Unicore FIRST, and the SMP seams are cut here while they still compile to nothing at one core:
EL1/EL0 and exception vectors, identity map and then an aspace ACTIVATE, GICv2/v3 for the UART and
the timer PPI with the table keyed `(line, kind)` rather than a flat NVIC index, the Generic Timer as
the tickless one-shot, `arch_ipi_send`/`arch_ipi_wait` as empty macros, a per-CPU struct reached
through `TPIDR_EL1` (**freed at M6.2's T6a: seating the kernel stack pointer before the initial
frame is built made `SP_EL1` trustworthy on entry, so the EL0 entry spends no scratch register and
the collision with this line is gone**), and an `arch_dcache_flush`/`invalidate` seam for DMA. **The aspace family names
concepts and not mechanisms**, which is the seam's standing doctrine: no architecture's registers and
no architecture's maintenance instruction appear in it. `docs/design-m6-mmu.md` carries the frozen
family -- create, destroy, map, unmap, activate, a
page-window acquire/release pair and a granule query, with map and unmap coherence-complete so no
flush call exists to schedule and no address-space identifier appears above the seam at all. Two decisions this milestone must FREEZE rather than
defer: **high-half versus fully separate address spaces** (it drives `kaccess_from_user` and whether
a kernel pointer survives `arch_aspace_activate`), and that **one lock spans capability
resolve-to-use**. Do not let the identity map become the allocator: prove boot, trap, switch and
UART on a 1:1 map, then cut `arch_aspace_*` before any second core.

### M7 -- multicore: SMP on the A53 port, and AMP where the cores are heterogeneous
The AMP-versus-shared-kernel question closes **per class**, not as one kernel-wide verdict. A
homogeneous A53 cluster shares one kernel image; heterogeneous companions (i.MX8MP's Cortex-M7, the
ESP32-C6 LP core) stay AMP over a ring, and the homogeneous MCU dual-core parts that fail per-line
interrupt targeting -- the RP pair -- are an AMP candidate rather than a shared-kernel one. The
dual-core LX6 is a homogeneous MCU part too and is NOT in that group: it passes targeting, and
`docs/design-multicore.md` makes it a gated shared-kernel candidate, which is a third class.

`IrqLock` ("interrupts off => exclusive") is single-core-only, so the rework is a **Big Kernel Lock**
first (local-IRQ-off + one spinlock, byte-identical on single-core builds), then per-core run queues
and finer locks only where real atomics exist. Cross-core object reclamation and TLB shootdown use a
blocking IPI rendezvous over the same transport, so the doorbell cannot be fire-and-forget only.
**Do not judge this milestone by its speedup**: on today's measured 53 percent `IrqLock` hold, Amdahl
caps two cores at 1.31x and four at 1.55x, and the hold-shortening that moves those numbers is M8.
Re-derive both after M8 rather than freezing a verdict here. Spikes: `docs/design-m7-smp.md`
(candidate ranking, cross-core IPC invariants), `docs/design-m7-state-inventory.md` (per-core versus
global), `docs/design-capability-table.md` section 8.

**The contract is `docs/design-multicore.md`, and THIS FILE OWNS THE MAPPING BETWEEN IT AND THIS
MILESTONE.** That document names no milestone on purpose: a design describes a mechanism, and a
mechanism does not move when the schedule does. The inversion is not hypothetical here, the
2026-08-21 resequencing having cost two file renames and three separate correction passes over
documents that described something which had not changed.

Two of its rulings decide the shape of this milestone rather than merely informing it. **The
deliverable is a second core and not the lock**, a big lock compiling to nothing at one core, so a
correct one and an absent one are the same image. And **a shared kernel is gated on a hardware
PREDICATE rather than an architecture family**, of which the MMU is not a member and per-line
interrupt targeting is; the parts that fail it get AMP, which is what closes the AMP-candidate
question this section leaves open above for the MCU dual-core parts. Its step plan is S0 through S7,
identifiers local to that file, and this milestone adopts every one of them:

| step | sub-milestone | what it lands |
| --- | --- | --- |
| S0 | M7.0 | the contract, the SMP seam differ, the configure-time predicate refusal |
| S1 | M7.1 | per-core state, landed at one core |
| S2 | M7.1 | the second core boots and idles |
| S3 | M7.2 | the lock, the doorbell, threads on every core |
| S4 | M7.2 | translation across cores |
| S6 | M7.3 | the GICv3 posture |
| S5 | M7.4 | the RV64 backend and the SMP-seam verdict |
| -- | M7.5 | the cleanup the train owes: the death point on every board, and the skip set retriaged |
| S6b | M7.6 | `imx8mp-evk` as a chip port, and the predicate declared per part |
| S7 | M7.7 | AMP |
| -- | M7.8 | the shared window reached through an endpoint: N7's remaining half |
| -- | M7.9 | thread placement: the task's scheduling grant, the affinity mask, core isolation |
| -- | M7.10 | the LX6 shared kernel, and the silicon measurement that made it declarable |
| -- | M7.11 | AMP as two images: the partition's port capabilities, one artefact, the ping-pong |
| -- | M7.12 | the RP pair as two images, and the partition generalised past two nodes |

**M7.8 THROUGH M7.11 TAKE `--` IN THE STEP COLUMN, as M7.5 does, because none is a step of the
contract.** M7.8 completes freeze N7 rather than implementing a numbered step; M7.9 is a
scheduling contract the contract itself does not carry; M7.10 is a backend; M7.11 is the
two-image posture N6b decided plus the port capabilities N6g freezes; and M7.12 is a port plus a
width. The contract's own step
plan ends at S7 and none of them reopens it.

**THE LAST THREE ROWS WERE REDRAWN AFTER THE WORK, WHICH IS THE WRONG ORDER AND IS RECORDED AS
SUCH.** M7.9 was once "SMP/AMP witness: AMP on the RP pair, SMP on the LX6", one row for two
models on two parts. Three things pulled it apart. AMP-by-convention was ruled out for the LX6
and the A53 -- a partition nothing enforces is not isolation, and the LX6 passes the predicate,
so it gets a shared kernel. The RP pair moved to two images per node, which is most of a
milestone rather than a tail. And thread placement turned out to be the thing that makes an LX6
shared kernel worth having at all, since without a way to reserve a core the second core buys a
control loop nothing.
**An external audit read the ledger, read the branch, and refused the mismatch**, which is the
ledger working: this file is the sole place a milestone number is assigned, so a branch whose
content contradicts it is unmergeable by construction. The rows are now what the branches are.
The lesson is the ordering, not the renumber -- a scope decision taken in conversation is not
taken until this table says so.

**M7.11 IS WHERE THE PARTITION BECOMES AN ORIGIN AND NOT ONLY A GEOMETRY, which is why its row
gained a clause after the redraw above rather than being resequenced again.** N6b left one image
per node owing a placement contract and a doorbell map, which M7.11's vehicle pays; what it then
found is that a node had no way to be HANDED anything. A capability cannot cross (N6d) and the
privileged mint has no reachable caller (contract section 7), so a node's endpoints came from
probe scaffolding and nowhere else. N6g answers it with the shape the memory partition already
uses: one list, per-node derivation, nothing stated twice.

**AND IT FOUND A DEFECT OLDER THAN ITSELF, which is recorded here because the sequencing looks
odd otherwise: a step about AMP carries a fix to the map editor's bookkeeping.** The range list
was not a total record of a space's mappings, an unprivileged thread's stack being installed
behind its back, so a frame capability could be mapped over a live stack and the release path,
which re-derived a run's identity by reading the page tables back, then dropped nothing. AMP
only moved the layout onto it: the branch never touched that code path, and a stack size alone
reproduces and un-reproduces the collision with the instance keying untouched. The stack and
its guard are one recorded range now, which makes the list total, and the run's slot is stored
on the range rather than asked of the hardware.

**WHAT THAT COST, MEASURED RATHER THAN ARGUED, because the shape of the cost decided the fix.**
The record grew from sixteen bytes to twenty-four: there is no padding in it to steal, so the
field costs eight bytes to alignment whatever width it is given, and squeezing it beside the
page count would have bought a saving nobody needs while narrowing what a caller may reserve.
Only translating boards keep a range list, so no region board pays. The other half of the cost
is the budget: a thread's stack takes a slot now, so the figure scales with the thread count,
and it is a real configuration knob rather than a hardcoded fallback for that reason.

**S7 IS LANDED AND WHAT IT PORTED IS NARROWER THAN "AMP", which is worth stating because the
column stays a ruling.** `qemu-arm64` ships a fourth posture where the image DRIVES four cores
and ONE kernel schedules on one of them: the count and the model diverge, which is the whole
point of separating them. The instance index comes from the core identity there, the third
keying and the one a chip owes, so every core reaches its own kernel with no adoption call. The
peers are nodes for the shared window and reached over the same doorbell the shared kernel uses;
each validates what it is sent and answers, and none of them runs a scheduler.

**THAT LAST CLAUSE IS THE BOUNDARY AND IT IS DELIBERATE.** The arena is one linker region with a
link-time assert modelling its exact allocation order, so a per-node arena IS the partition
layout the contract leaves open, and building one inside this step would have answered that
question by accident. So the node whose kernel believes itself alone is core 0's, and what the
peers demonstrate is the crossing rather than a second scheduler.

**THE SECURITY WORK LANDED AS VALIDATION AND IT VALIDATES BOTH WAYS.** Every index and length
read out of the window is another node's writing: a head that names more outstanding slots than
the ring holds, a length past a slot and a port outside the receiver's own mint are each refused
by name, and the SEND side refuses a malformed tail for the same reason, a producer believing the
consumer's index overwriting slots the consumer is reading. No index read from the window is
ever used as an index. One ring per ORDERED PAIR of nodes is forced by the no-RMW rule rather
than chosen: one inbox per receiver would have several producers on one head. What one service
call will do is bounded per sender and across the call, an unbounded drain being a peer's hold
on a masked handler rather than a correctness question; and a refused far depth no longer owns
its ring for the life of the image.

**AND IT IS PROTOCOL SCAFFOLDING, NOT AN ISOLATION BOUNDARY BETWEEN KERNELS, which is stated
in the window's own header so that no later reader has to infer it.** Every node maps the same
writable kernel RAM. The mint sits outside the shared window and the counters sit beside it,
which puts them out of a far side's reach through the PROTOCOL and not out of a compromised
peer KERNEL's reach at all: that peer rewrites the mint, the counters and every other node's
kernel data. So what validation buys is defence against a MALFORMED peer, and the boundary
against a hostile one is per-node memory partitioning, which is the partition layout this
milestone deliberately leaves open.

**WHAT IT DOES NOT REACH, both recorded rather than left for rediscovery.** The window is not
yet reached through an endpoint, so nothing above the syscall can tell the two localities apart
and no second API surface exists; the resolve-to-ring branch and the reply path's local thread
pointer are a later step's. And the heterogeneous case has NO VEHICLE: the emulated i.MX8MP
models the A53 cluster alone, ships no Cortex-M7 companion, and cannot release a second core of
the cluster either.

**S5 AND S6 ARE RESEQUENCED, SO THE STEP COLUMN NO LONGER RUNS IN ORDER AND THE MILESTONE
COLUMN DOES.** The step identifiers are the contract's while the milestone numbers are schedule
positions, and these two steps depend on neither each other nor a shared file, so the GICv3
posture takes the earlier slot because the controller-neutral rename it carries is what the RV64
branch's gates are resolved against.


**M7.5 IS NOT A STEP OF THE CONTRACT, and it existed because two items were ruled separate rather
than done. Both landed, and a full fleet sweep witnessed the pair.** The one the contract does not
reach at all was the death point: read outside the lock at ONE core with the fix written and
switched OFF there, so a cancel landing between `syscall_dispatch`'s entry read and the park was
owed to nobody and the target parked forever on every single-core board. It is now asked on every
board, and as a PREDICATE in each caller's own under-lock prologue rather than as the noreturn
action it was inside the shared park helpers: a caller may reach its park with a transaction
already committed, and exiting from inside the park would abandon it. TEN callers carry it. The
register fastpath is exempt on the ruling `docs/design-multicore.md` section 4 gap 1 already made,
and says so where the exemption lives.

**THE CLASS IS EVERY SITE THAT WRITES `ThreadState::BLOCKED`, NOT THE TWO PARK FUNNELS, and framing
it the other way is what made this take two passes.** Enumerating from the funnels being refactored
found `wq_block` and `park_queueless` and armed their callers; it could not find the third writer,
`ktime_sleep_until`, which reaches neither. A sleep carries a deadline and so self-wakes, which
makes the ordinary miss a cancelled thread sleeping out its remaining delay rather than a thread
lost, a latency bug. **The unbounded case is real though:** `ktime_sleep_ns` saturates to
`UINT64_MAX` on overflow, and a saturated sleep parks on a deadline no clock reaches, which is
exactly the defect this item exists to close. The same shape as the skip-set misdiagnosis below:
the question was framed around the mechanism being touched rather than around the claim being
made.

**WHAT GATED IT WAS COST RATHER THAN DOUBT, AND THE SWEEP PRICED THAT COST AT ZERO.** Those funnels
also serve the register fastpath on four arches, and they carry the depths `trap_redzone` measures,
so the change was held back to land with a full sweep and not beside other work. On
`qemu-riscv`/rv32imac the eight class depths came out byte-for-byte what they were before it, which
does not generalise on its own: armv7m carries a hardware frame in the zone that rv32imac does not,
and that is exactly why the rest stayed unknown until measured. The sweep measured them.
`trap_redzone` appears in all 57 preset logs and all 57 passed, so no preset moved past its red zone
on any of the five classes rooted at `syscall_dispatch`.

**THE OTHER ITEM WAS RETRIAGED RATHER THAN COMPLETED, BECAUSE THE LABEL IT INHERITED WAS WRONG.**
The arms skipped above one kernel core were filed as asserting a single-core ORDERING. For at least
four of the five tier-1 IRQ arms reworked here the progress order was only how the precondition got
manufactured, and the real blocker is an ABSENCE CLAIM: a service, a redelivery or a wake that must
not happen raises no event for a later read to be ordered after. Six arms came off the skip set, 28
to 22. The question for the remaining 22 is not whether an arm assumes an order but whether it
asserts that something did NOT happen.

**It sat before the board port and AMP rather than after them because it carried a live defect and
depended on neither.** A thread cancelled in the window parked forever on every single-core board,
so the slot was chosen by what the fix was worth rather than by what was left over. It shared no
file with S5 or S6 either, so the three ran at once.

**S4 JOINED S3 IN ONE SUB-MILESTONE RATHER THAN FOLLOWING IT, and the reason is what S4 turned out to
be.** Its gap 5 was not groundwork for a later step: a core that switched to a thread holding no
space kept a dead translation root installed while the release path rerooted only itself and freed
the tables under it. Closing that alongside the step that put threads on every core is what makes the
pair coherent, so the two land together and every later row moves up. S4 is complete, its
instruction-side poke included.

**S5 IS COMPLETE AND ITS VERDICT IS THE STEP'S FINDING, WHICH IS WHAT THE CONTRACT ASKED FOR.** The
differ exited 0: not one member of the seam frozen before either backend moved, over a real corpus
of 25 signature records with every group above its floor and a 47-record known-answer control
answering as expected. That is a recorded result and not a re-takeable one, the differ having since
been removed from the tree. A second backend went in whose identity is a published index rather
than a register read, whose doorbell is a CLINT word lowered through a machine-mode trampoline
rather than a GIC software interrupt, and whose lock is LR/SC; the seam absorbed all three
unchanged. **That is a positive result about the seam rather than an absence of news.**

**WHAT S5 ADDED TO THE PLAN'S OWN ACCOUNT, because the RV64 lowering is not the A64 one.** The
ruling that a peer's machine software interrupt cannot be delegated is measured here rather than
argued, and the measurement made the trampoline SMALLER than the ruling bought: a supervisor store
to a peer's CLINT word is permitted on this machine, so only the RECEIVE side runs in machine mode
and `arch_ipi_send` has no machine-mode leg at all. Against that, this backend is the one that can
discover what the contract says A64 cannot, and it did twice. The kernel lock had exactly one
release site across a swap in the whole tree, in the armv8a switch, so the first RV64 core to reach
its idle thread carried the lock into `wfi`; and the one cause every raise arrives on carries three
sources here rather than one, so a poll that cleared it while servicing only the doorbell lost a
device line. Both are in the record, and both were found by running rather than by any gate.

**THE DOORBELL COMES BEFORE THE SECOND INTERRUPT CONTROLLER, AND THAT ORDER WAS ARGUED RATHER THAN
INHERITED.** GICv2 issues a software-generated interrupt through `GICD_SGIR` and GICv3 through
`ICC_SGI1R_EL1`, so a reader will ask why GICv3 does not come first and save writing the send twice.
Because the seam does not change: a core-index bitmask is expressible on both controllers without
loss, so `arch_ipi_send` is written once and only the lowering is per controller. What DOES change is
what a failure teaches. S3 is where the rendezvous semantics are discovered, and discovering them
against one lowering is cheaper than against two at once. The same reasoning puts S5 before S7: the
doorbell earns a second backend's witness before an AMP node is built on it.

And S6 answers a question S3 leaves open rather than duplicating one: `arch_irq_mask`, `arch_irq_unmask`
and `arch_irq_clear_pending` write banked registers and act on whichever core calls them. S3 settles
that, so the second controller implements a settled semantics instead of carrying the same open
question twice.

**S6 IS LANDED: `qemu-arm64` SHIPS TWO INTERRUPT POSTURES, the way the RV64 board ships Sv39 and
Sv48.** A Kconfig choice under `ARCH_ARMV8A`, an unprompted derived version reaching CMake as well
as C, a `gicv3` board-config variant and a `qemu-arm64-gicv3` preset; `arch/CMakeLists.txt` links
one backend on that version and refuses any other value at configure time. The GICv3 backend sits
BESIDE the GICv2 one behind a controller-neutral `arch/arm64/common/gic.h`, and the rename that
created that interface landed as its own commit with every allocated section byte-identical but the
build stamp, so a bisect separates the rename from the backend. Measured on a quiet box with the
host unit layer on: seven presets green -- `qemu-arm64` 46, `qemu-arm64-smp` 50, `qemu-arm64-gicv3`
50, `sim` 405, `qemu-riscv64` and `qemu-riscv64-sv48` 55 each, `qemu` 64. Under GICv3 the doorbell
arm reports four cores answering over 64 rounds, with each of the three secondaries acknowledging
INTID 0 thirty-two times and core zero absent because the initiator services its own bit inline.
Of the emulator's raise count only the round's 64 is fixed; the boot total varies in the low
eighties because a running kernel raises the doorbell too.

**A POST-COMMIT AUDIT FOUND THE DISABLE PATHS RETURNING BEFORE THE DISABLE TOOK EFFECT**, and it
is recorded here because a green fleet could not have found it. GICv3 applies a cleared enable
asynchronously and RWP reports the completion, `GICD_CTLR.RWP` for the distributor and
`GICR_CTLR.RWP` for each redistributor; the first pass read the distributor's at its three
control writes and the redistributor's nowhere, leaving all five `ICENABLER` writes returning
early. `arch_irq_mask` is the operational one, so driver teardown returned with delivery still
possible -- section 4's gap 2, a mask that is not exclusion, in a new place. QEMU completes these
writes synchronously, so no arm here can witness either the defect or the fix.

**THE GROUP MISMATCH IS SILENT IN HARDWARE AND LOUD IN THIS FLEET, and the second half is bought by
the timer rather than by any instrument.** `ICC_SGI1R_EL1` generates Group 1 alone, so every INTID
this backend configures is Group 1 and the acknowledge and end-of-interrupt registers are Group 1's
to match. The controller drops a mismatched interrupt and reports nothing -- no fault, no log, no
INTID -- exactly as `docs/design-multicore.md`'s S3 facts say. What makes it loud here is that the
timer PPI rides the same group decision: leaving it in Group 0 while the SGI is Group 1 fails six
gates. A backend with the SGI right and the timer wrong would boot, answer doorbells and hang.

Emulator-grade, per section 7 of the contract, and one degree further than that section states:
QEMU's GICv3 model is not a GIC-500, so this posture matching the i.MX8MP's controller is a claim
about the ARCHITECTURE version and not about the part's implementation of it.

**S6b IS LANDED AND ITS SECOND HALF WAS NOT REACHABLE.** `qemu-system-aarch64 -M imx8mp-evk`
exposes no `gic-version` option at all, unlike `virt`: the machine's GIC-500 is wired, so that board
does not boot without the GICv3 posture. Its value is that GIC version, routing and topology are
properties of a PART while `smp.cmake` declared them per ARCH, and that audit item is discharged:
the six properties now split by owner in separate namespaces, an arch declaring the three an ISA
hands every part it defines and a part declaring the three its die decides, with an arch that sets a
part-owned property refused.

**What it did NOT deliver is the four cores its expected result named, and the reason is the
emulator rather than the port.** The machine models no secondary release whatever: no PSCI conduit,
the other three cores held powered off, and the reset controller that would release them present
only as an unimplemented device. The board therefore boots ONE core of four and refuses more at
compile time. The silicon sequence is recorded in `docs/reference/boards.md` and was deliberately
not written, an unrunnable release path carrying a correctness claim being judged worse than a
recorded mechanism; that is a decision open to reversal rather than a ruling.

**The step also settled something its own description had folded together: the LAUNCH is not one of
the six properties.** Two parts of one arch can meet every one of them and still release a secondary
by different mechanisms, which is what `virt` and this die do. A predicate that absorbed the launch
would have declared this part predicate-failing, which is false.

What S6b cannot carry, and neither can S7: the heterogeneous AMP case. The machine models the A53
cluster alone with no Cortex-M7 companion and decodes the companion's tightly-coupled memory as
unimplemented, so the part that is both SMP and AMP at once has no vehicle on this bench at all.

**M7.8 IS THE HALF OF N7 THAT S7 DID NOT REACH, and the freeze is what says it is owed rather
than optional.** S7 built the window and its validation; nothing above the syscall can yet tell a
local receiver from a remote one, because no user-facing cross-node call exists. N7 freezes ONE
IPC mechanism with locality resolved below the seam, so a second API surface is not the
alternative -- the alternative is that the freeze is unmet. The hard half is the reply path: it
rests on a raw local thread pointer with a one-shot generation guard riding the minted
capability, and a remote caller has no such thread in this kernel. Priority donation across nodes
has no meaning at all, which is a thing to state rather than to solve.

**M7.9 IS THREAD PLACEMENT, AND THE MILESTONE THAT ONCE HELD THIS NUMBER IS NOW M7.10 AND
M7.11.** What it lands is a scheduling grant carried by the task -- a priority ceiling and a core
set, given at creation and narrowing only -- one affinity mask per thread, one syscall that
intersects a request against the grant, and core isolation as a Kconfig knob that cannot take the
boot core. `pin` and `unpin` are userspace wrappers over the one call, a zero mask being the ask
for the task's default set rather than a malformed one.

**IT IS NOT ONLY A MULTICORE FEATURE, AND A RECORD THAT FILES IT AS ONE HIDES WHAT IT CLOSES.**
The placement half folds to nothing at one core, proven byte-identical. The priority CEILING does
not: an unbounded priority let any unprivileged thread take the top of the run queue and starve
the system, on every board in the fleet, single-core included. That hole predates multicore and
this is what shuts it.

**AND IT IS WHAT MAKES A SECOND CORE WORTH HAVING ON A CONTROL SYSTEM.** Without a way to reserve
a core, a shared kernel hands a motor loop a scheduler that may migrate it mid-cycle and a comms
stack that competes with it. Isolation plus pinning is what turns two cores into two jobs.
Priority does not cross a node boundary and the two scales are incomparable, so tuning across
nodes is an integration act no run checks -- which is a reason to place work deliberately rather
than a reason to distrust the mechanism.

**M7.10 IS THE LX6 SHARED KERNEL**, and the measurement that made its predicate declarable at
all: `S32C1I` excluding between PRO_CPU and APP_CPU over internal SRAM, and two processor
identities that differ, both taken on silicon because the ISA defines each as a configurable
option no document on this bench resolves. **M7.11 IS THE TWO-IMAGE MECHANISM ON AN EMULATED PART**, with
the cross-node ping-pong that is also the first witness of a far reply reaching a receiving
thread in another kernel rather than a service body. It does NOT deliver the RP pair: the only
own-image posture it lands is `qemu-arm64`'s, and the row said otherwise until 2026-09-04.

**M7.12 IS THE RP PAIR AND THE WIDTH, and it exists because M7.11's row was redrawn to describe
what landed.** Two halves, neither of which the other needs. The PORT is the named part M7's own
header calls an AMP candidate for failing per-line interrupt targeting: two images on the RP2350,
which also forces the merged artefact into that part's flash container, since an ELF is what an
emulator boots and not what a board takes. The WIDTH is the partition past two nodes: the kernel
layer is already N-general and witnessed at four in the host fixture, so what is two-node is the
scaffolding around it -- a pair-shaped two-ELF comparator, three per-node demo sources where one
node-agnostic app belongs, and N6h stating the check's obligation in two-node words. **A
three-node partition is witnessable in emulation with no new hardware**, which is what keeps the
width honest while the port waits on a board.

**WHY THE ORIGINAL ROW DID NOT SURVIVE CONTACT.** It paired two models on two parts as one
milestone. AMP-by-convention was then ruled out wherever nothing enforces a partition, which
sends the LX6 to the shared kernel it already qualifies for; the RP pair moved to one image per
node, which is most of a milestone rather than a tail; and placement turned out to be the
prerequisite that makes the LX6 backend worth building. Three rows where there was one.

**TWO REAL PARTS ARE NAMED FOR THE MULTICORE ERA, AND THE SECOND IS POST-M8.** The i.MX8MP
Verdin at 4 GiB is the shared-kernel target and the part S6b's chip port is written against. The
**Milk-V Duo (CV1800B) at 64 MB is a candidate for after M8**, and it is an AMP part by the
predicate rather than by judgement: the datasheet's own words make its two C906 cores asymmetric,
so it fails requirement 5 outright.

**It also carries a port shape nothing in the tree has.** Its main C906 has an MMU and its
companion has none, so the first real AMP silicon is MIXED-MODEL AMP, a translating node beside a
region-model node. M7.7's AMP node is translating-model on both sides, so that is a different
port and not an increment on this one. Two further facts belong here rather than being
rediscovered: no coherency unit is documented anywhere in that part's datasheet, and `mhartid` is
hardwired to zero in the openc906 release with the integrator customising it per instance -- so
an AMP node taking its instance index from its core identity, which is N6's rule, has no register
to stand on there.

### M8 -- IPC and IRQ optimisation, on measured evidence
**The rebaseline is the first thing the optimisation phase does, and the phase is not first**:
per-thread kernel stacks and the MMU both change trap and continuation costs, so no percentage
measured before them is a planning input, and by the same argument no percentage measured before
this milestone's own fixes and de-duplication is one either. The ordering ruling is below; what
follows here is the phase's content, in order of structural value rather than micro-cost: an
end-to-end instrument (physical IRQ assertion through first userspace MMIO, plus
the outermost lock-hold distribution, reporting p50/p99/max and not minima); `kos_reply_recv` fusing
reply and receive under one kernel entry; donation to the already-blocked receiver on the rendezvous
fastpath; a sticky IRQ notification with overflow accounting instead of a saturating counting
semaphore; binding an IRQ notification to the endpoint receive wait so ONE driver thread waits on
both; and edge-only persistent arming under a kernel-held storm budget.

**THE MILESTONE OPENS ON FIXES, PASSES THROUGH DRY, AND ONLY THEN OPTIMISES, AND THAT ORDER IS THE
RULING RATHER THAN A CONVENIENCE.** Optimising a hot path that is about to be rewritten for
correctness spends the work twice, and a de-duplication pass moves the very code an optimisation
would have been measured against. So the three phases are strictly ordered, and each one's exit
is the next one's baseline.

| sub-milestone | what it lands |
| --- | --- |
| M8.1 | task lifecycle and address-space teardown: root's death policy, the dying sibling's space, the forcible slay |
| M8.1.1 | the build files: the parent selects, an app declares, a test names its subject, and a seam gate stops hand-rolling |
| M8.2 | the AMP window and far IPC: the slot snapshot, reply-ring admission, the refusal shape |
| M8.3 | isolation and syscall robustness: region-board grant ownership, the copy that may fail, per-task object budgets |
| M8.4 | the gates, the CI matrix, and the instrument's own arithmetic |
| M8.5 | DRY in the kernel and the arch backends |
| M8.5.1 | what M8.5 measured and left: the budget made to bind per kind, the window priced and declined, the reporter narrowed, and the package read from outside on every arch |
| M8.6 | DRY in the build, the gate library and userspace |
| M8.6.1 | the console's unit of atomicity becomes a LINE, and every chip gets a ring |
| M8.6.2 | the static gate corpus audited: the suspicion that it was mostly dead weight, measured and answered |
| M8.7 | P0: the rebaseline campaign, and the end-to-end instrument |
| M8.8 | the per-switch and per-wake plumbing |
| M8.9 | the IPC structure: the reserved reply slot, and the fused reply-receive |
| M8.10 | translating boards and SMP: the reent seat, ASIDs |
| M8.11 | what only a measurement can justify |
| M8.12 | the M8 exit measurement, frozen |

**THE FIX PHASE IS CUT BY SEAM AND NOT BY SEVERITY**, so that each sub-milestone is one review gate
over one surface whose invariants can be stated together. Cutting it by severity instead would have
put the root-death policy, an AMP ring protocol change and a region-allocator ownership rule in one
pass with nothing in common but their rank, and would have left each of the three surfaces reopened
by a later pass. The consequence to accept is that M8.1 through M8.3 each carry Low items that a
severity cut would have deferred: finishing a surface is worth more than ranking its defects.

**THE BY-RULE DECISION IS CARRIED IN BOTH DIRECTIONS, AND THAT IS WHAT COLLAPSES SIX LISTS TO ONE.**
M8.4 already rules that every `KICKOS_*` symbol reaches CMake by rule rather than through an
allowlist that is a second authority. Measured: SIX hand-maintained lists decide membership today,
88 entries between them, with no cross-check between the two directions -- three in the root file
turning a `-D` into a request, three in the generator turning a resolved `.config` into CMake, and a
seventh hard-coding five booleans inside the gate that checks the sixth. **The gate covers the
opposite direction from the live bug**: it asserts every prompted symbol CAN be requested, and
nothing asserts a resolved symbol REACHES CMake, which is why two symbols are invisible to CMake
today while a defconfig appears to set them. So the outbound half emits every symbol into the
fragment by the rule the generated header already uses, which retires the three generator lists and
those two defects as a CLASS rather than one at a time; the inbound half derives the request syntax
from the prompted set and the symbol type, which kconfiglib exposes and the forwarding gate already
computes, retiring the three root lists and the gate itself. What survives is ONE list, the two
bespoke console and telemetry inversions that project a value back onto a choice.

**AND THE END OF THAT ROAD IS THAT THE BUILD DECIDES NO KNOB AT ALL, WHICH IS A DECISION M8 SHOULD
TAKE RATHER THAN DRIFT INTO.** The precedents all do it: one generates a make include so every
symbol is a make variable, another imports every `CONFIG_*` line into CMake variables in one loop
and only ever tests them. What CMake irreducibly keeps is the board-and-variant selector, the
toolchain pick that must happen before `project()`, the build-graph switches that are not
configuration, and the mapping from values to sources -- reading, never deciding. What goes is the
`-D` route entirely: a knob is then set by a defconfig, by `menuconfig` or by `setconfig`, and the
root lists, the forwarding gate, the `option()` calls for Kconfig knobs and the CMP0077 dependence
go with it. **Two questions it forces, and neither is answered here.** Nine presets pass a knob on
the command line, seven for the AMP node id and two for the appdata size, so either fragments merge
over a defconfig -- which bends the rule that a variant is a complete statement -- or those become
per-node defconfigs. And arch and chip are stated in BOTH `board.cmake` and Kconfig only because the
toolchain reads the board before `project()`; running Kconfig before `project()` retires that
duplication and the four agree checks with it, and it is an optional second step the by-rule import
does not need. **One behaviour is kept deliberately against the precedent**: the generator's
read-back refusal, where a value the declarations do not permit fails the configure instead of
falling back quietly.

**M8.5 ALSO CARRIES TWO ITEMS THAT HAD NO HOME, ASSIGNED HERE RATHER THAN LEFT OWED.** Neither is
de-duplication and the row's subject is not widened by accident: they are assigned because a
milestone is where an item gets decided, and these two had been recorded as owed with a direction
and no number. The first is the per-task object budget bounding CREATES rather than HOLDS, so a task
at its ceiling can delegate its objects and die and one live task ends up holding a whole pool; the
direction is charging on delegation or counting holds, and the second wants a per-(task, slot)
record rather than one owner byte. The second is the panic reporter's tail, which is most of what a
red-zone figure measures on rv32imac and rxv3, where eight figures then sat at exactly their reserve
so the next assert on any dispatch chain would fail a gate on a board nobody named; the direction
was pricing a reporter that runs on a stack of its own. Its sibling is the `esp32c6-wroom-st` kernel
`.bss` ending 68 bytes short of the `.appdata` boundary, where a crossing costs 32 KiB of arena and
fails nothing, and **that one cannot simply be asserted**: `esp32c6-wroom-bench` has already
crossed, so the assert lands with whatever fixes that preset.

**M8.5.1 EXISTS BECAUSE M8.5's MEASUREMENTS OUTRAN ITS SCOPE, AND EVERY ITEM IN IT IS A FIGURE
RATHER THAN AN INTENTION.** It is not a spillover row: each item below was measured while doing
M8.5's own work, recorded with its evidence, and deliberately not acted on because acting would
have widened a de-duplication milestone into a design one.

**THE OBJECT BUDGET NEVER BOUND, SO THE RESERVE WAS THE WHOLE BOUND WEARING THE BUDGET'S NAME.
LANDED.** The one object budget defaulted to 255 and no board overrode it, against pools
of four to sixteen slots, so `min(budget, slots - reserve)` was `slots - reserve` everywhere and
the term named in the grant never decided. There is one ceiling now, and the board states it:
four per-pool budgets, each asserted at BUILD time to sit strictly below its own pool's width,
which is what carries "no task takes a pool's last slot" without a runtime clamp. **THE ASSERTS
BOUND WHAT A TASK MAY TAKE AND NOT WHAT IT MAY HOLD**, and the AMP port seat is the one charged
install that adds a hold no admission ever saw: `amp_ports_seat` panics rather than refusing, so
a partition naming more crossings than the endpoint budget would boot root already past its own
ceiling and the four asserts would pass. What holds root there is a second build-time relation,
in `CMakeLists.txt` beside the pool one, so the shape is `ports <= budget < pool` and the
ceiling is true from root's first instruction.
`TASK_OBJECT_RESERVE` is gone, and so is `Task::object_budget`, which stored a compile-time
constant per task and was the same shape one layer down. **FOUR FIGURES AND NOT ONE**: a single
budget forced down to the narrowest charged pool would cost the other three kinds `.bss` for
slots a task never asked for (`STATE.md` carries the bisection that measured it). **The
supervisor-respawn question is dissolved rather than answered**: the denial is now a sizing
property of the pool.

**THE APP WINDOW IS QUANTISED TOO COARSELY ON THE ONE PART THAT CANNOT AFFORD IT, AND 16 KiB IS
THE ONLY VALUE THE PART ADMITS. MEASURED AND DECLINED.** `f411disco` has 128 KiB of SRAM, a 16
KiB `_appdata_size` and 17,888 bytes of kernel `.data`/`.bss`, so it declares a 32 KiB reserve
of which 14,880 bytes are dead, 11.4 percent of the part. M8.5 recorded 17,488 over by 1,104 for
15,280; the drift since is `g_panic_stack`, 432 bytes this tree did not have then, less 40 the
kernel singleton has shed. `blackpill` shares the script and measures identically at every
symbol, so the figure stays a chip figure and no board file gains one.

**THE DIRECTION M8.5 NAMED DOES NOT PAY, AND THE LINK IS WHAT SAYS SO.** App statics already
fill 7,864 of the 16,384, so a 4 KiB window fails the link on the window-overflow ASSERT, and an
8 KiB window LINKS GREEN, but only with `KICKOS_KERNEL_DATA_RESERVE` moved to 24K alongside
`KICKOS_APPDATA_SIZE`: the reserve's own over-generous-window ASSERT refuses the 32K default
once the window shrinks to 8K. That gives a pad of 328 bytes on `selftest` and at most 2,744 on
any image in the preset. The pad is the newlib heap and every thread shares it: newlib seats one
`struct _reent` per thread slot -- 9 here, 4,608 bytes of the window -- and each slot's first
stdio use mallocs a 1,024-byte buffer, so the slot count this board advertises wants about 9 KiB
of pad. `gpioblink` is the only image on the board that links an allocator and it has 9,816
bytes today. A 32 KiB window is worse in the other direction, the arena losing 16,384. So the
rung below does not run and the rung above costs more than the waste.

**THE TWO ROLES CAN BE SEPARATED ON PMSAv7 AND IT STILL DOES NOT PAY.** `MPU_RASR[15:8]` is the
subregion-disable field, one bit per eighth of a region of 256 bytes or more, and this tree
writes it as zero everywhere. A 64 KiB enclosing region at the RAM base with 8 KiB subregions
grants the window as subregions 3 and 4 and returns 8,192 of the 14,880. The price is
`arch_mpu_encode_default.cc` and `arch_mpu_region_encodable_default.cc`, the shared PMSAv7 path
for armv7m AND armv6m, whose pow2 rule `cmake/boot_arena.cmake` scrapes out of
`arch_mpu_region_pow2()`, plus a subregion mask the portable `struct arch_mpu_region` has no
field for and that PMP NAPOT and the K64F SysMPU cannot express.

**AND THE 14,880 BYTES COST NO ADVERTISED CAPABILITY.** The arena is 73,728 and the tree's own
`KICKOS_POOL_ARENA_ASSERT` demand -- idle 512, root 4,096, eight pool stacks of 4,096 with their
pow2 run-ups -- tops out at `0x20016000`, leaving 32,768 spare. That is the criterion M8.5
itself applied to the panic stack: what buys nothing owed is headroom, not loss.

**RECORDED AND NOT TAKEN: PLACING THE WINDOW AT THE RAM BASE INSTEAD OF ABOVE KERNEL DATA**, two
`MEMORY` regions rather than one, which removes the quantum entirely -- the base is aligned for
free, kernel `.bss` growth costs its own size and nothing more -- and returns 14,336 bytes at no
cost in heap or in MPU descriptors. It moves every RAM address on the board, `arch_mpu_probe_addr`'s
`guard_word` in kernel `.bss` included, so the PMSAv7 silicon witness recorded in
`docs/reference/boards.md` stops matching what the board would print; and it makes one chip
diverge from the nine-script reserve class M8.5 has just landed. `rx72m` and `esp32c6-wroom` are
untouched. The granularity rule is not wrong, it is exact, and the f411 measurement says this
knob has nowhere to go rather than that the rule has.

**THE FAULT REPORTER LOOKED LIKE THE PANIC REPORTER'S SHAPE AND WAS NOT.**
`kickos_thread_fault_exit -> kprintf_fault -> kvprintf_route -> the console` set rv32imac and rxv3
EXITK, with armv7m and armv6m carrying the same chain, so it read as four arches of the same
problem. What made moving the PANIC reporter pay was that ONE console tail was charged to four
classes at once, every `KICKOS_ASSERT` on a dispatch chain reaching it; the fault reporter is on
exactly one class per arch, so that argument does not transfer. The measurement then said EXITK
sizes no allocation on any of the 54 declared presets -- SYS, SYSK or SVCK binds every kernel
block, 188 to 332 bytes above it -- so a per-core array would have spent 576 to 704 bytes of
kernel `.bss` to return none, and its claim has no refusal arm that keeps the second faulter's
announcement. Refused on those two grounds, and what was actually costing the figures was a stack
ARRAY rather than the console: the reporter now formats into `KDIAG_FAULT_LINE_MAX` bytes instead
of the 256 every kernel diagnostic gets, taking EXITK to 576 on rv32imac, 448 on rxv3 and 448 on
armv6m and EXIT to 448 on armv7m. The one place the reporter did size something is the armv7m
spawn floor, which it no longer holds and which `TODO.md` carries as its own decision.

**AND AN APP IS BUILT OUT OF TREE FOR EVERY ARCH, WHICH RETIRES A COVERAGE HOLE RATHER THAN
DOCUMENTING IT.** `oot_export` registers on the host preset and its MCU sibling on one armv7m
board, so the only thing in the tree that reads the installed package from outside covers two of
seventy-one presets. Two Xtensa assembler sources carrying a `.h` extension have failed that
compile for as long as they have existed and nothing reports them. Building the package per arch
family makes both fall out as build failures and needs neither a rename nor a content-sniffing
skip.

**THE REST ARE RECORDED IN `TODO.md` WITH THEIR EVIDENCE AND BELONG HERE BY SIZE RATHER THAN BY
THEME**: the banner gate that asks only whether a listed file yields a banner and not whether the
set is whole; the kernel-data-reserve class closed across nine scripts with no control in `tests/`
keeping it closed; `appdata_no_kernel` not registering on the three arches that gained
archive-selected `.bss`; the hold walk's masked time, estimated statically and never measured; the
intra-archive resolution rule stated five times in `arch/CMakeLists.txt` and wrong in both
directions; a divisor computed from a frequency literal, which no arm in the tree can catch; the
fourth cross-node `volatile` word outside DRY-5's chip-side sweep; `qemu-x86_64` green over two
page-table helpers no arm of it distinguishes from broken; the doorbell send whose dropped
request-cell store leaves the cross-core gate announcing that every core answered; and the ninety
lines the RP2350 doorbell still duplicates.

**M8.6.1 TAKES A PIECE OF M9's GROUND DELIBERATELY, AND THE RATE IS THE ARGUMENT.** M9 owns
console locking, and the tear was left to it with the rate recorded as the reason to take it
early. It is taken here because it costs a CI job about a quarter of its runs under load and
because the fix turned out to be smaller than the item assumed, not because the phase order
bends.
**WHAT THE DESIGN DISCUSSION SETTLED, and three of the four turns were corrections to a claim
this file would otherwise have carried.** The unit of atomicity the console owes is a LINE, not
a byte and not a report: a reader parses lines, so whole lines in any order stay legible while
two producers inside one line destroy it. That is why the fix is an indivisible insert and not a
lock across the transmission, which at 115200 would mask for about 22 ms.
Three claims made on the way were WRONG and are recorded because each one nearly became the
design. **"The fault path bypasses the ring by design"**: it does not, and nothing hardcodes it.
`console_emit` routed faults to the synchronous writer through `arch_in_isr()`, a blanket rule
standing for "this context may not wait" -- true of `console_tx_write`, which chunks and waits
UNMASKED between chunks, and false of buffering as such. A non-waiting insert is safe from any
context, which is what let the fault path keep the ring. **"A ring needs a TX interrupt"**: it
does not. A producer can drain its own queue outside the lock under a single-drainer flag, and
the interrupt is one way to trigger a drain rather than a precondition for having a queue.
**"Panic and fault both need synchrony"**: only panic does. The system stops after a panic so a
queued line is one nobody reads, while a thread fault leaves the system running and the drain
carries it -- and because the ring is FIFO, deferring the fault report also ORDERS it, landing
after the queued lines instead of inside one.
**THE RING SIZE WAS NEVER A CHOICE AND WAS ELEVEN COPIES OF ONE NUMBER.** 512 is forced: the
ring reserves one slot, so a 256-byte `kprintf` line does not fit 256, and CRLF expansion raises
the floor again. It is now one knob with a build-time assert tying it to the line bound, because
a too-small ring FAILS SAFE -- the insert refuses, the locked writer takes over, everything
works and the short masked window is silently gone.
**AND THE COOK BUFFER WAS THE LARGEST TERM OF THREE RED-ZONE CLASSES, which nothing said.**
`diag.h` claimed the 96-byte fault line array was the largest term of the EXITK descent; it was,
at 256, until M8.5.1 shrank it past `console.cc`'s 128-byte CRLF buffer and left the claim
behind. Measured by moving each: the fault array is worth 160 bytes of EXITK, the cook buffer
128 of EXITK, FAULT and PANIC alike. Expanding CRLF during the ring copy deletes that buffer, so
the milestone buys stack on three classes before it buys the flake.
**WHAT IT CANNOT FIX, stated so a green run is not read as one.** `qemu-arm64-amp2` tears on the
same mechanism and a kernel lock cannot reach it: two nodes, two kernels, two images, one UART
with no arbitration. rp2350 is the fleet's only answer to that shape, a claim serialised by
SPINLOCK31 with a hold bound priced on wire time, and the two AMP backends therefore disagree
about whether a shared console is owned. That is a partition-wide ownership contract and stays
M9's.

**M8.6 HAS A MEASURED LEDGER RATHER THAN AN AMBITION, AND THE TREE ALREADY OWNS THE IDIOM THAT
REMOVES MOST OF IT.** The build corpus is 11925 lines over 272 files and the root file alone is 16
percent of it. No function in the shared module is dead and the heavy helpers are correctly placed:
the weight is concentrated in three places, each with an existing in-tree pattern to move into --
the per-chip opt-in fragments that already work, and the `foreach ... include()` loop the
integration directory already uses. The seven toolchain files share 253 lines drawn from one set of
47; one nine-branch arch ladder is the same five commands per branch with a different source list;
and the root holds a 310-line AMP arithmetic block whose job is identical to two files that are
already modules. Net about 870 lines, the root from 1875 to roughly 1300, and the first four moves
are independent of each other and carry no semantic risk. One ordering constraint is real: the AMP
block must keep running before the capability-table sizing, whose width depends on the port count.

**LANDED, AND THE LEDGER'S ARITHMETIC WAS WRONG IN BOTH DIRECTIONS ON FOUR SEPARATE ITEMS. THE
PATTERN IS THE DURABLE PART, NOT THE TOTAL.** Measured: the build corpus went 12741 lines over 284
files to 12470 over 296, the root file 2039 to 1551, `main.cc` 14209 to 7417, `abi.h` 1157 to 644.
Not the predicted 870. The toolchain extraction BEAT its estimate at -398 on the six real files.
The arch ladder MISSED at net +27 against -135: only 99 of the nine branches' 286 lines were
repeated dispatch, the other 187 being source lists that MOVE rather than fold, and nine new files
cost nine gated SPDX headers. The gate library came to -104 against -145, its corpus builder -3
where -55 was predicted, while `LC_ALL=C` -- flagged and not counted -- was -117 because 39 files
each carried a paragraph explaining the line. And the integration corpus, said to want its own
measurement, wants no refactor at all: both pairs quoted for it collapsed from 0.88 and 0.85 to
0.41 and 0.61 once the gate library landed, and 348 of its 570 nominally foldable line occurrences
are shell syntax, script prologue, or calls INTO the library, which is what an extraction leaves
behind.
**THE COMMON CAUSE IS THAT A CONTAINMENT OR SHINGLE METRIC COUNTS LINES THAT LOOK ALIKE.** A source
list resembles another source list, a per-board wrapper resembles another per-board wrapper, and a
descriptor literal resembles another, without any of them being duplication. **So every remaining
shingle-derived estimate in this roadmap is an upper bound on the fold and not a promise**, and an
item's own figures are to be re-measured before it is briefed: `abi.h` was 1157 lines and not 1001
with 44 percent probe surface and not 30, the selftest TU 14209 and not 12566, the driver adapters
92 to 126 lines and not 46 to 108, and two items -- DRY-10a and DRY-10b -- were already closed on
master before the milestone opened.

**WHAT M8.1.1 DID NOT FINISH IS M8.6's, AND IT IS THE SAME SHAPE M8.1.1 REMOVED FROM THE APPS.**
Three of the four arrows turned. What kept the old pattern is the root file itself -- 22 flat copies
of one four-condition guard around 34 unit-test descents, of which ordering explains only 12 -- plus
70 `EXISTS` guards on paths that are all in `git ls-files`, which is exactly the "guard against a
declaration that cannot decline" class the apps were cleaned of. And the 36 new gate files now
repeat what the app files used to: 49 raw test registrations across 22 of them, 18 sharing an
identical tail, 12 hand-written board ladders over the same fleet subsets while two board-set
helpers have no caller, and an inclusion list of 36 names that silently does not pick up a new gate
file. Four apps still decline for themselves on a value the parent already holds.

**THE GATES COME BEFORE THE DE-DUPLICATION, AND THE BENCH IS REPAIRED IN M8.4 RATHER THAN M8.7.**
Both follow from what the two later phases rest on. A DRY pass is exactly the change class a static
gate is supposed to catch, and seventeen `tests/static` gates have no positive control -- no
planted violation the detector must fire on every run -- so they cannot be shown to fire at all.
Those controls are worth more before the refactor than after it, and the seventeen are named in
`TODO.md` rather than counted here, a denominator moving every time a gate lands.
**THAT FIGURE WAS OVERTAKEN AND M8.6.2 RE-DERIVED IT: sixty of 68 gates carry a control, so eight
lack one and not seventeen.** The number moved because M8.4 added controls and because a gate
that HAS one is not the same as a gate whose control can fire, which only a mutation shows. And P0 is a
measurement campaign rather than a repair: the rv32 bracket excludes the save and restore that the
switch column claims to contain, and the phase table's composite correction is arithmetic on the
instrument rather than on the kernel. Repairing the instrument inside the campaign would make the
campaign's own numbers the first thing it invalidates.

**`MIN` STOPS BEING THE STATISTIC, AND THE REASON IT WAS ONE HAS TO BE ANSWERED PER BOARD.**
`kernel/bench/bench.cc` reads the minimum because the XMC4800's DWT is documented unreliable on
that silicon, and a glitched read can only inflate a delta. That argument is sound for that part and
is not a fleet-wide licence: a minimum hides the tail this milestone exists to shorten, which is why
the instrument above is specified at p50/p99/max. The board whose counter is untrustworthy keeps a
recorded exception; every other board reports a distribution.

**NINE DECISIONS WERE TAKEN BEFORE THE CUT, AND THEY ARE WHAT MAKE THE SUB-MILESTONES SIZEABLE.**
Root may die, and root's slot is retired rather than freed: `ThreadPool::alloc` never reclaims
`ROOT_INDEX`, so `is_root` stays the slot compare and no successor can ever answer it. A far call
refused after its slot was taken publishes an empty `PORT_REPLY`, which is the wire's only refusal
shape and what the contract already promised. Reply-ring space becomes the admission test for
TAKING a call, which is a consumer-side change and leaves the producer's arithmetic intact. Grant
ownership on region boards is recorded per allocated block, so `abi.h`'s unconditional promise
becomes true as written instead of being narrowed to the weaker of two backends. The far arm
validates the full 16-bit sequence. Per-task object budgets land at the creators rather than being
recorded as an accepted assumption. Every `KICKOS_*` symbol reaches CMake by rule instead of
through an allowlist that is a second authority. The uncovered presets get a CI job each:
`qemu-arm64-gicv3` under the emulator whose toolchain CI already fetches, and the shared-image
`pizero2350-amp`, which is silicon and so earns a build rather than a run; the rv64 job goes to
M8.1 by the rule below rather than to M8.4. And the fastpath is refused by name above one kernel
core, today held only by the incidental absence of an `smp.cmake` on the four arches that have it.

**M8.1.1 EXISTS BECAUSE THE APP-TO-TEST DEPENDENCY POINTS THE WRONG WAY, AND EVERY OTHER SYMPTOM IN
THE BUILD FILES FOLLOWS FROM IT.** An application is not a test, yet 36 of 67 app `CMakeLists.txt`
register tests, 161 registrations between them, and the two biggest apps carry gates that are not
theirs at all: `hello` is the smallest image that boots a chip, so barrier, doorbell and entry-order
gates ride it; `selftest` is the richest, so every gate that inspects a linked ELF rides that. Above
them the parent descends unconditionally into 45 directories and selects nothing, so 47 of the 67
children bail out themselves and 35 then re-check `NOT TARGET` in case the declaration they just
asked for declined. Two wrappers declare an app (`kickos_add_diagnostic_app` 47 uses,
`kickos_add_application` 29), and the keyword DSL one of them parses, with the validation its own
keywords make necessary, serves FIVE apps.

**The target shape is `hello_c`, and it is reachable rather than aspirational**: 62 of the 67 apps
need nothing but a declaration, a link and an image, which is what
`examples/oot-mcu-app/CMakeLists.txt` already calls the supported path and the reference shape. So
in-tree apps currently diverge from the surface the package documents for its own consumers. Four
arrows get turned around, and they are one change: **the parent selects** on the posture it already
holds, **an app declares** itself and stops, **a test names the subject it rides** from the test
side, and **an authority is total** rather than fatal-or-silent with a sentinel at every call site.

**THE UNIT FILES CARRY THE MIRROR IMAGE OF THE SAME FAULT: THREE HELPERS FOR THE EASY CASE AND NONE
FOR THE RECURRING HARD ONE.** `kickos_add_unit_test`, `kickos_add_kseam_gate` and
`kickos_discover_unit_tests` are called 18, 10 and 8 times, and yet seven gates still hand-roll a
`$<FILTER:>` source genex and four stand up their own object library, the largest at 93 lines.
What they are all re-implementing is one thing: COMPILE THESE KERNEL SOURCES AT A CHOSEN POSTURE
AGAINST A SEAM. That is the abstraction the directory needs and does not have, and the filtering the
gates hand-roll is the symptom of its absence rather than the defect itself. So the same pass that
turns the arrows around gives that pattern one name and collapses the three helpers that cover the
case which was never hard.

**IT IS SEPARATED FROM M8.1 AND NOT FOLDED INTO IT**, because M8.1 merged before this was
understood and a merged milestone does not reopen. What M8.1 did contribute is three new app files
that reproduced the pattern while the audit was flagging it in the old ones.

**M8 OPTIMISES THE COARSE-LOCK DESIGN AND DOES NOT BREAK IT, WHICH IS WHY PER-CORE READY QUEUES
ARE NOT IN M8.10.** Under one kernel lock they replace a filtered scan with a pop and permit no
concurrent scheduling at all, while their real shape needs a thread home, a remote-wake protocol, a
migration rule, a priority and donation behaviour, and a rule forbidding two core-queue locks. Those
are the lock-partition design, so landing the queues first is building them twice. They move to M9.
**If M8.7's A53 numbers show the filtered scan is itself material, M8 may land a
behaviour-preserving preparatory layout under the lock and nothing more**, labelled for M9.

**THE MILESTONE HAS TWO FROZEN MEASUREMENTS AND THEY ARE NOT INTERCHANGEABLE.** M8.7 is the
PRE-OPTIMISATION baseline that decides what M8.8 through M8.11 are worth attempting; M8.12 is the
M8-EXIT measurement taken after them. **M9 is judged against M8.12 and never against M8.7**, and no
figure predating M8.7 is a planning input for either: the whole phase table predates trusted
stacks, the MMU, the big kernel lock and word-wise `memcpy`.

**A SUB-MILESTONE IS THE JOB PLUS ITS TESTS, SO IT CARRIES THE CI THAT RUNS THEM.** Where a fix's
witness needs infrastructure the tree lacks -- a CI job, a preset, an emulator arm -- building it
belongs to the sub-milestone that needs the witness, never to M8.4 because M8.4 is where the word
CI appears. SM-2's arm wants a 4-hart `qemu-riscv64` run and there is no rv64 job in
`.github/workflows/ci.yml` at all, so that job is M8.1's; M8.4 keeps only the coverage no
sub-milestone needs for a witness of its own. **A fix witnessed locally with a CI gap recorded
beside it is not finished work**, and if a claim genuinely cannot be witnessed in CI then that is a
refusal to state with its reason rather than a debt to file.

**WHAT M8 IS NOT, BECAUSE `TODO.md` SAID OTHERWISE.** Three items there carry an `M8` tag from the
era when four design documents and `TODO.md` had settled on "M8, the last one", the reading this
file rules against above. One of them, the RISC-V context-switch cost, belongs here on its merits
and is M8.11. The other two are a confinement ladder and share nothing with this milestone but the
word MPU: "Option B", which drops each backend's permissive privileged background so a kernel wild
pointer faults instead of riding it, and the ARMv8-M TrustZone backend layered on top of it. Both
are hardening and debuggability rather than performance, Option B forks the "privileged is the
background" contract every board rests on, and neither is a bug fix anywhere, the M7 speculation
stall being closed already by Option A. They stay in `Later`. The footprint chase that `TODO.md`
points at "the M8 footprint work" has no row here either, and gets its home when one is assigned.

**Protection is not assumed cheap here, and the figure that said so has moved by 4.7x.** This
section quoted `MPU_APPLY` at 443 cycles a switch, 886 of a 3651-cycle locked round trip, from the
phase table of `docs/design-m5-ipc-fastpath.md` section 3.0.4. Section 8.5 measures the same board
after the PMP precompute at **19 for `MPU_APPLY` and 75 for `MPU_COMMIT`, 94 a switch and 188 a
round trip**: the 443 was the pre-split phase carrying the commit work, and the name alone does not
say so. What this invalidates is not a detail but the milestone's own headline: 3.0.4 priced the
removal of protection at `f = 0.457` and **1.37x** on a term that is now a quarter of the size, so
that multiplier is superseded and no replacement is stated here. P0 owns it, which is what makes
the rebaseline a precondition rather than a formality -- and the whole phase table shares the
defect, every figure in it predating trusted stacks, the MMU, the big kernel lock and word-wise
`memcpy`, with the A53, rv64 and x86 carrying no cycle figure at all.
Bulk transfer is a SEPARATE object from the endpoint, and its wire format is
**`{region-cap, offset, len}`, never a raw address** -- the M4 review's finding 10, which stays
one paragraph only until the first large-transfer path lands.

### M9 -- kernel concurrency, and what the big kernel lock actually costs
**KickOS is a research project, and the lock is the research; drivers are a final step.** That is
why this milestone takes the slot ahead of the driver era rather than following it, and the position
is a maintainer's judgement rather than a consequence of anything else in this file. **The axis is
solved against open**: the driver model is already proven in this tree by working implementations
against the expected model, so what the driver era holds is BREADTH of a settled pattern and can be scheduled
whenever; how a kernel this size should be locked above one core has no answer here yet.

**THE MILESTONE IS NAMED FOR A QUESTION AND ITS ANSWER MAY BE THAT THE LOCK STAYS.** What it owes is
per-core scheduler ownership and ready queues, remote-work inboxes instead of a core reaching into a
peer's queue, and only then an evidence-gated escape from the global lock. Capability
resolve-to-use stays globally protected until a lifetime replacement is designed, because the
partition that makes scheduling concurrent does not make object teardown safe. **A measured verdict
that the coarse lock survives is a successful outcome of this milestone, not a failure of it.**

**NO SPEEDUP FIGURE IS STATED HERE, AND THAT IS DELIBERATE.** The scenario arithmetic that sizes
this work rests on a DERIVED locked fraction rather than an SMP measurement, and this file has just
finished repairing the last figure of exactly that kind: `MPU_APPLY` was quoted fleet-wide at 443
cycles a switch, and section 8.5 of `docs/design-m5-ipc-fastpath.md` measures the same board at 94
once the PMP precompute landed, which left every derived multiplier resting on a term a quarter of
its assumed size. So the envelope belongs to the audit that computed it. **M8.12 is what this
milestone is sized from, and every value is recomputed there before M9 is approved.**

**IT CARRIES A STOP CONDITION, WHICH IS THE POINT OF THE ENTRY METRICS.** The entry metrics are the
M8.12 locked fraction, the lock-wait cycles under contention, and p50/p99/max for both IRQ-to-user
and the IPC round trip. The stop condition is that a partition whose measured overhead exceeds its
measured benefit is REFUSED and recorded as refused. Two shapes are already known to lose: a
single-flow IPC call is a serial dependency chain no second core shortens, so same-core handoff
stays the latency posture and a regression budget guards it; and extra atomics can cost more than a
bounced global line saves. `docs/design-multicore.md` section 9 owns the open questions and this
file owns only the number.

| sub-milestone | what it lands |
| --- | --- |
| M9.0 | the reference-kernel survey, and the lock-domain questions it is allowed to answer |
| M9.1 | the lock's own bound: fair arbitration per backend, the doorbell poll kept |
| M9.2 | ownership under the lock: a home derived from the mask, per-core ready queues, the wait-edge rule |
| M9.3 | the per-pair rings: the kinds, the depth that cannot fill, the drain budget |
| M9.4 | the local scheduler leaves the lock, under the stop condition |
| M9.5 | same-owner IPC leaves the lock, blocked on the lifetime question |
| M9.6 | the write-up, the contract changes, and the M9 exit measurement |

**EVERY ROW AFTER M9.0 IS ASSIGNED AND NOT YET APPROVED, and the distinction is the point of
assigning them.** A number here fixes what a stage IS, so that the evidence gate can refuse the
stage without the argument moving to a different number afterwards. M9.0 can start whenever, being
read-only. **AND THE LADDER IS NOT A COMMITMENT TO ITS OWN SHAPE.** These seven rows are the plan as
it reads today, and this file has renumbered and re-cut worse than this when the work found
something: a discovery moves the roadmap rather than the roadmap constraining the discovery, so a
stage that turns out to be two, or to be already answered by the one before it, is re-cut on the
spot and no argument is owed to its number.

**THE STAGES THAT BUILD OWNERSHIP LAND UNDER THE LOCK IN EVERY OUTCOME, AND ONLY THE ESCAPE IS
EVIDENCE-GATED.** The opposite staging was proposed and is refused: bound the lock first, then build
ownership only if the bound fails. It is refused because this milestone OWES per-core scheduler
ownership, ready queues and remote-work inboxes whatever the measurement says -- they are what makes
"local" a measurable thing at all -- and because a protocol built after the verdict is a protocol
tested under the conditions that produced the verdict. So M9.2 and M9.3 land with one lock still
held, which is also the cheapest way to test a publication protocol: without concurrency there is no
race to chase while the shape is still moving. M9.4 and M9.5 are the ones the stop condition can
refuse.

**TWO OUTCOMES ARE VALID AND BOTH ARE STATED BEFORE THE WORK, so that neither reads as a
disappointment.** The first is that the lock stays and now has a bound: fair arbitration per backend
plus M8.8's one acquisition per syscall entry and M8.8 through M8.11's shorter hold, with a worst
wait DERIVED per backend or the backend recording that it cannot be. The second is an owner-local
hot plane behind the same control lock: home-owned scheduler state and ready queues, wakes and asks
as per-pair publications, the local scheduler and then same-owner IPC leaving the lock, while
capability topology, lifetime and migration stay behind the fair global lock. **The first outcome is
reached by the stop condition firing, and it is a success.** What it may NOT rest on is the register
fastpath: that path exists on armv6m, armv7m, rv32imac and rxv3, holds no lock, and none of the
three shared-kernel arches has it. Porting it above one core is a separate item nobody has costed,
and `TODO.md`'s G-06 -- decided, not yet
landed -- is the configure refusal that makes that unreachability stated rather than incidental.
Today it is unreachable only because no fastpath arch has an SMP build file.

**M9.1 COMES FIRST BECAUSE BOTH OUTCOMES NEED IT: the control plane keeps a global lock either
way.** Today's lock is a bare test-and-set retry loop on every shared-kernel backend and has no
fairness bound at all, and `klock.h` takes it AFTER the interrupt mask, so a core spinning for it is
interrupt-masked and the lock wait is inside every core's interrupt-latency bound. The per-backend
shape follows the ISA and is not one design: rv64 has a fetch-and-add and a ticket there retries
never; ARMv8.2 with the large-system extensions has the same property, which is what an RK3588 would
show; the A53 that the emulator models is ARMv8.0 with no such extension, so a ticket counter is
ITSELF a load-linked retry loop and its ceiling is a measured axiom rather than a derivation; and
the LX6 has a compare-and-swap alone, so it records that no bound is derivable there. Opening the
interrupt mask between spin attempts, so that the wait leaves the masked window, is evaluated here
and not assumed.

**THE INBOX IS THE AMP RING WITH A POINTER WHERE THE MESSAGE WAS, AND IT IS ONE RING PER ORDERED
PAIR RATHER THAN ONE INBOX PER CORE.** `docs/design-multicore.md` already forces that shape and
already states the ordering: a shared head would need a read-modify-write and N9 forbids one above
the seam, publication precedes the raise, the barrier is a full one on both sides and not an
acquire/release pair, and the ring is the authority while a raise is only a hint. The cost is
storage quadratic in the core count, sized in the stage. **AND A WAKE OR A REPLY IS NEVER REFUSED ON
FULL**, which is a ruling and not a sizing preference: the tree's own cross-core wake is an
idempotent single-writer publication with no full case, and a refused reply is a loss nobody can
retry. So the wake and reply kinds are sized so they cannot fill -- a parked thread has exactly one
waker, so a depth at least the threads homed at the target is enough -- and only control-plane
kinds, which carry no deadline, may refuse. A core never waits on a peer's scheduler lock while
holding local state; it publishes.

**A HOME IS THE OWNER OF A THREAD'S SCHEDULER STATE AND IS DERIVED FROM THE MASK, NEVER A FIELD
BESIDE IT.** `docs/design-multicore.md` section 8 already froze one mask with no flag, one pick rule
and migration as an ask rather than a yank, and M9.2 adds ownership without adding a placement field
a caller writes: a single-bit mask fixes the home, a wider one lets it move, and a move is ownership
transfer over the ask that already exists. The wait-edge rule is the write discipline that makes it
sound: a READY or RUNNING control block is written by its home core, a PARKED one by its waker, and
the wait edge is what confers the write. That is today's waker-cleared discipline promoted to a
rule.

**AN ENDPOINT'S OWNER IS THE HOME OF ITS FIRST RECEIVER, STICKY, AND CLEARED WHEN NO HOLDER IS
LEFT.** The alternative was an owner named at the mint, and it is refused because it would put a
placement argument on a creation call and make locality an ABI fact. A request that lands at a stale
owner COMPLETES under the global lock and is never refused. A hard-real-time composition gets static
owners by pinning its servers, which the grant already supports, and the default profile stays
usable with the default init and no composition at all. N7 is untouched at both ends: locality
reaches neither the mint nor the call.

**CROSS-CORE PRIORITY INHERITANCE IS A RING KIND, AND A CEILING IS AN ADMISSION RULE RATHER THAN A
MECHANISM.** Same-core donation is unchanged. Across cores today's boost already reaches a running
peer only at its next scheduling pass, so the change is from loose to stated: the recompute becomes
a published request with the delivery delay priced, and the sealed profile adds a ceiling as an
admission rule so that a server's response time is bounded by provisioning. Helping by migration is
refused: it moves a thread to break an inversion, which is a yank. **The server's own response time
on its own core is the largest term in every cross-owner bound and is written down as such** rather
than left out, which is how the first version of this arithmetic came to look better than it was.

**A LINE FOLLOWS ITS CLAIMER AND A WAITER FOLLOWS ITS LINE, and this lands whole with M9.2 rather
than as an interim pin in M8.9.** Two rules. A CLAIM says "I serve this line from here" and routes
the line to the claimer's core; a WAIT re-homes the waiter to the line's core. Admission does not
change: a task whose grant cannot reach that core is REFUSED and never clamped. The claimer is the
server by contract, so a provider that wants a driver on a line delegates the authority and lets the
driver claim rather than claiming on its behalf. **The kernel never rewrites a thread's own mask**,
which is what today's pin-to-the-line's-core does and what goes away: placement becomes an internal
home, and that is precisely why the rule cannot land as a pin first. The long work may be PUSHED
from the home core to an idle core inside the mask, at a wake the home is too busy to take or at a
preemption, consumed by the target through a ring -- never a pull, so no core steals and the
single-writer rule holds. A single-bit mask never pays for it and a wide mask pays at most one
transfer per wake. Every controller touch executes on the line's core, by re-home or by the routed
touch that already exists. The consequence is that interrupt-to-userspace is core-local in both
profiles with no declaration from anybody: the kernel provides locality and balance stays userspace
policy, exactly as section 8's anti-work-conserving placement already puts it. M8.9's sticky
notification and its bind-to-the-receive-wait are designed against this now, so that the wait surface
moves once.

**THE ROUTED MASK TOUCH IS NOT PART OF THIS AND WAS WITHDRAWN AS A DECISION.** `irq_route.cc`
publishes a mask, unmask or clear of a line owned by another core to that core and waits for
completion; it is the tree's one blocking cross-core wait. It is reachable on the LX6 alone, the two
shared-kernel backends answering that no line has an owning core and performing the touch locally,
so on both of M9's targets the ask cannot be reached. It is a control-plane touch of a controller
mask, not an interrupt handover, and no thread moves. It stays as it is, and it becomes reachable
everywhere once routing is kernel state on every backend. **What it must never become is a
precedent**: on a deadline path the shape is a publication plus a completion cell, never a wait, and
a blocking cross-core wait spends the asker's core interrupt-masked on a peer that may be spinning
for the very lock the asker holds.

**CAPABILITY LIFETIME IS THE NAMED PRECONDITION OF M9.5 AND IS NOT DECIDED HERE.** Resolve-to-use is
one continuous lock today and generations are a detector rather than a guarantee, and this file
already rules that the protection stays until a lifetime replacement is DESIGNED. Three candidates
stand: keep the lock on the resolve, in which case M9.5 buys hold length alone; freeze the topology,
which is a mechanism M10 owns; or timestamp quiescence, which is the answer that survives
translation. **An epoch or a quiescence flag beside the existing release point is refused as a
second answer**, per section 4 of the multicore contract. The plan assumes the lock is kept on the
resolve, quiescence runs as a spike, and the question is reopened before M9.5 with M8.12's split of
the locked span into resolve, handoff and scheduling in hand.

**EIGHT CORES IS A SCENARIO AND NOT A TARGET.** No preset configures more than four, no shared
kernel has ever run on silicon, and an emulator's timing model cannot witness cost at any width, so
an eight-core run answers questions about ring storage, drain work and arrays that assumed four --
never about latency, and never the choice between the two outcomes. **So M9's verdict is provisional
on emulation by construction**, and the silicon re-check is the RK3588-class part, which belongs to
the driver era rather than to this milestone. Its feasibility spike may run at any time; the
requirement-5 ruling for a part whose clusters share an ISA but not a performance class, and the
per-cluster constants, belong to that port.

**WHAT MAY NOT BE USED AS A BOUND, stated because each has been offered as one.** An observed
maximum is a sample and establishes nothing: the distribution validates an analytical bound and does
not replace it. A retry loop above the ISA's own primitives is refused; the load-linked and
compare-and-swap loops inside a lock primitive are the floor and are stated as measured axioms.
Every doorbell service drains a fixed budget and the remainder stays published and retriggers, so no
drain is unbounded. And the paths that are unbounded TODAY are an inventory this milestone owes a
capping mechanism for, each one already named in `TODO.md`: the lock acquire itself, the
effective-priority recompute over an unbounded donor set, the linear ready pick, the linear waiter
pop, and the untimed wait that no cycle detection covers.

**M8.7'S INSTRUMENT IS SIZED FOR THIS MILESTONE AND NOT ONLY FOR M8.** The bench refuses more than
one kernel core outright today and reports a minimum, an average and a maximum with no percentile
and no lock metric at all, so M9's bounds would have no inputs. What M8.7 owes beyond its own
purpose is one accumulator per core aggregated at report time, the lock HOLD and lock WAIT
distributions rather than the hold alone, the doorbell round trip, the longest interrupt-masked
window, and, for the interrupt-to-userspace span, whether the wake was local or cross-core so that
M8.12 can price what a line following its claimer buys. **The argument for specifying it there is
cost and not urgency**: it instruments spans M8.7 is already opening, so it is nearly free in that
pass and a separate campaign later. Nothing here is unrecoverable if it is missed -- a measurement
can be re-run, and re-running one is a cost rather than a wall.

**M9.0 READS KERNELS THIS PROJECT ADMIRES, AND IT MAY NOT COPY THEM.** seL4, Fiasco.OC, NuttX,
RIOT, Zephyr, ThreadX, RTEMS, RT-Thread, ChibiOS and FreeRTOS are each remarkable in their own way,
and clean revision-pinned checkouts of them sit on the development box (`CONTEXT.local.md` names
them and their revisions; it is gitignored, so no personal path ships here). What the survey
extracts is DESIGN: lock domains, acquisition order, remote wake, migration, whether the lock is
released inside the switch, and what benchmark evidence each project publishes. **Cite by path and
never copy**, because kernel lock code read under one licence and then written into this tree is
the licence and clean-room angle of the review rather than a stylistic worry. The output enumerates
the design space and says where KickOS sits in it; it does not rank, and no shipped document in this
tree grades another project. A majority among them is not an argument, and a single-core kernel is
not an SMP precedent. **Its per-project row carries a licence**, because the clean-room rule is only
checkable against one. **And the survey's own scope grew**: the tree's AMP window is a row, so is
its routed mask touch, and so are a lock-free capability kernel and a production remote-wake inbox
that both sit on the box already; a multikernel with a published crossover between shared memory
with locks and message passing, a capability kernel binding every context to a core, a scheduling
context and budget design inside a project already surveyed, and a message-passing system with
inheritance across the message path are named as references that are NOT on the box, so that their
absence is a known gap rather than an implied verdict.

### M10 -- static composition, and an init provider that stays
**THE DRIVER ERA MOVES DOWN ONE PLACE AND THIS TAKES THE SLOT.** The reason is the same one that put
M9 ahead of the drivers: the driver model is a settled pattern whose remaining work is breadth,
while how a system of this shape is COMPOSED has no answer in this tree at all. What exists today is
a default root that creates a graph and returns, and a seam that already permits an init which never
does. What does not exist is a way to state a whole system outside the code that builds it, check
that statement before the target ever boots, and refuse a configuration the machine cannot meet.

**THE DELIVERABLE IS A STATIC SYSTEM DATABASE, AN OFFLINE ADMISSION PASS, AND A RESIDENT PROVIDER
THAT CONSUMES BOTH.** The database declares tasks, entry points, homes, priorities, stacks, MMIO
windows, capabilities, endpoints, interrupt lines, capacities and a restart policy, and optionally a
period, a deadline and a worst-case execution time. Admission runs on the HOST: it cross-checks the
declared graph against the configured pools, the linker's memory, the capability-table supply, the
ring sizes and the temporal equations, and emits target tables. **No parser runs on the target**, and
the provider walks the emitted tables in dependency order, delegates capabilities, binds lines,
gates the start, and stays resident to serve health. A provisioning library carries the validation,
the dependency walk and the syscall adapters.

**WHERE THE DATABASE LIVES IS OPEN AND IT MAY NOT BECOME A SECOND TRUTH.** This file already routed
configuration to Kconfig, so the database is either a generated VIEW of that or a new authority that
Kconfig then derives from -- and one of the two has to be chosen before anything is written, because
two hand-maintained descriptions of one system is the failure this milestone exists to remove rather
than to add.

**THE KERNEL'S SHARE IS SMALL AND MOSTLY ALREADY OWED.** A per-task object budget at the three
creators and at the line claim is M8.3's, and once it lands as decided a SEAL is very nearly "the
budget goes to zero": what a seal still needs is an authority holder, an errno, a scope and a rule
about whether it can be undone. A deferred start works as a userspace gate today; a create-suspended
posture is cleaner and is an alpha-ABI change made once. Release timers, an execution budget and an
overrun action are the temporal half, and **the scheduling-context design inside a project the M9.0
survey already reads is required reading before any of it is proposed** -- that residue is that
design, and proposing another before reading it would be inventing a solved thing.

**TWO THINGS ARE CONDITIONAL AND THE TABLE SAYS SO PER ROW.** A seal buys nothing for concurrency
while the lock is kept on the capability resolve, so its concurrency value is M9's second outcome
alone; and a bounded restart mutates capability topology that another core may be resolving, which
is incompatible with a lock-free resolve unless the restarted domain's objects are reachable only
from that domain and its supervisor. That is open, and it is the same lifetime question M9.5 is
blocked on rather than a second one.

**AND THE AUTHORITY QUESTIONS ARE NOT SETTLED EITHER.** Root is unprivileged on every board by
construction and the multicore contract puts configuration-time authority at configuration, while
placing a thread of another task, minting dynamically and starting a core are all left open there.
So this milestone either names the authority object per operation or records that it cannot yet; what
it may not do is assume root.

### M11 -- back to the driver era
Remaining drivers and breadth, plus the SPI class work of `deferred-after-pr-train.md` -- the
validation hoist and the nine divergences. This sits after the foundation on purpose: it improves
support on a base that is no longer moving under it. **The RK3588-class port is a row here**, and it
carries M9's silicon re-check with it: a part with eight cores, the large-system atomic extensions
and a GICv3 is the first thing that could answer on silicon what M9 answers on an emulator, and the
requirement-5 ruling for clusters that share an ISA without sharing a performance class is this
port's to make.

### M12 -- KickCAT as the reality check
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
- **x86_64 as a PC target, beyond the M6.4 backend** -- M6.4 brings up x86_64 as the entry-path
  falsifier, on QEMU with UEFI firmware and under a hypervisor as a second firmware, rather than as a
  product. What stays captured here is the rest of
  being an actual OS on a PC: bare metal rather than QEMU, a disk bootloader instead of `-kernel`, and
  the device breadth that implies. This is where `__KickOS__` earns its name. The MMU spike aimed its
  unicore stepping stone at x86_64 FIRST; M6 aims that at QEMU `virt` A53 instead, because that is
  the machine multicore then runs on, and uses x86_64 as the falsifier behind it.
- **i.MX8MP -- heterogeneous AMP across profiles** -- an **MMU KickOS on the Cortex-A53(s)** (VMSA)
  beside an **MPU KickOS on the Cortex-M7**, one per core cluster, over cross-core IPC. This is M7's
  AMP contract carried from homogeneous to heterogeneous cores, and it is the case that needs the
  cache-maintenance seam: the A53-to-M7 window is not coherent.
